#include <Arduino.h>   // needed for PlatformIO
#include <Mesh.h>
#include "MyMesh.h"
#ifdef MESH_PROXY
  // Dual-core split (CrowPanel LVGL companion): the UI talks to the mesh backend
  // only through MeshProxy (snapshot + command/event queues). See MeshProxy.h.
  #include "MeshProxy.h"
  #include "driver/gpio.h"

  // The radio's DIO interrupt (RadioLib's setFlag, which does a non-atomic
  // `state |= INT_READY`) MUST fire on the same core that runs the_mesh.loop()
  // (the core-0 meshTask). Otherwise the ISR on one core races the loop's
  // `state = ...` on the other, the done-flag gets dropped, and the radio wedges
  // mid-TX (no TX/RX, BLE companion also stalls). Arduino binds every GPIO
  // interrupt to whichever core first installs the shared ISR service, so we
  // install it from core 0 before radio_init() does the first attachInterrupt.
  static volatile bool s_gpio_isr_done = false;
  static volatile int  s_gpio_isr_err  = -999;
  static void installGpioIsrOnCore0(void*) {
    s_gpio_isr_err = (int)gpio_install_isr_service((int)ARDUINO_ISR_FLAG);
    s_gpio_isr_done = true;
    vTaskDelete(NULL);
  }
#endif

// Believe it or not, this std C function is busted on some platforms!
static uint32_t _atoi(const char* sp) {
  uint32_t n = 0;
  while (*sp && *sp >= '0' && *sp <= '9') {
    n *= 10;
    n += (*sp++ - '0');
  }
  return n;
}

#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  #include <InternalFileSystem.h>
  #if defined(QSPIFLASH)
    #include <CustomLFS_QSPIFlash.h>
    DataStore store(InternalFS, QSPIFlash, rtc_clock);
  #else
  #if defined(EXTRAFS)
    #include <CustomLFS.h>
    CustomLFS ExtraFS(0xD4000, 0x19000, 128);
    DataStore store(InternalFS, ExtraFS, rtc_clock);
  #else
    DataStore store(InternalFS, rtc_clock);
  #endif
  #endif
#elif defined(RP2040_PLATFORM)
  #include <LittleFS.h>
  DataStore store(LittleFS, rtc_clock);
#elif defined(ESP32)
  #include <SPIFFS.h>
  DataStore store(SPIFFS, rtc_clock);
#endif

#ifdef ESP32
  #ifdef WIFI_SSID
    #include <helpers/esp32/SerialWifiInterface.h>
    SerialWifiInterface serial_interface;
    #ifndef TCP_PORT
      #define TCP_PORT 5000
    #endif
  #elif defined(BLE_PIN_CODE)
    #include <helpers/esp32/SerialBLEInterface.h>
    SerialBLEInterface serial_interface;
    #if defined(WITH_WIFI)
      // WiFi and BLE can't coexist on this chip, so in WiFi mode we never start the
      // BLE stack and drive the companion frame protocol over USB serial instead.
      #include <helpers/ArduinoSerialInterface.h>
      ArduinoSerialInterface usb_serial;
    #endif
  #elif defined(SERIAL_RX)
    #include <helpers/ArduinoSerialInterface.h>
    ArduinoSerialInterface serial_interface;
    HardwareSerial companion_serial(1);
  #else
    #include <helpers/ArduinoSerialInterface.h>
    ArduinoSerialInterface serial_interface;
  #endif
#elif defined(RP2040_PLATFORM)
  //#ifdef WIFI_SSID
  //  #include <helpers/rp2040/SerialWifiInterface.h>
  //  SerialWifiInterface serial_interface;
  //  #ifndef TCP_PORT
  //    #define TCP_PORT 5000
  //  #endif
  // #elif defined(BLE_PIN_CODE)
  //   #include <helpers/rp2040/SerialBLEInterface.h>
  //   SerialBLEInterface serial_interface;
  #if defined(SERIAL_RX)
    #include <helpers/ArduinoSerialInterface.h>
    ArduinoSerialInterface serial_interface;
    HardwareSerial companion_serial(1);
  #else
    #include <helpers/ArduinoSerialInterface.h>
    ArduinoSerialInterface serial_interface;
  #endif
#elif defined(NRF52_PLATFORM)
  #ifdef BLE_PIN_CODE
    #include <helpers/nrf52/SerialBLEInterface.h>
    SerialBLEInterface serial_interface;
  #else
    #include <helpers/ArduinoSerialInterface.h>
    ArduinoSerialInterface serial_interface;
  #endif
#elif defined(STM32_PLATFORM)
  #include <helpers/ArduinoSerialInterface.h>
  ArduinoSerialInterface serial_interface;
#else
  #error "need to define a serial interface"
#endif

/* GLOBAL OBJECTS */
#ifdef DISPLAY_CLASS
  #include "UITask.h"
  UITask ui_task(&board, &serial_interface);
#endif

StdRNG fast_rng;
SimpleMeshTables tables;
MyMesh the_mesh(radio_driver, fast_rng, rtc_clock, tables, store
   #ifdef DISPLAY_CLASS
      , &ui_task
   #endif
);

/* END GLOBAL OBJECTS */

void halt() {
  while (1) ;
}

#ifdef MESH_PROXY
static void meshTask(void*);   // backend loop, pinned to core 0 (defined below)
// Gate: meshTask is created early (to reserve its stack before LVGL grabs the heap)
// but must not touch the_mesh/radio/HSPI until ui_task.begin() has finished on core 1.
static volatile bool s_ui_ready = false;
#endif

void setup() {
  Serial.begin(115200);
#ifdef MESH_PROXY
  // Pin the GPIO ISR service to core 0 (where meshTask runs) BEFORE any
  // attachInterrupt, so the radio's DIO interrupt fires on core 0 -- same core as
  // the_mesh.loop(), avoiding a cross-core race on RadioLib's `state` flag.
  {
    xTaskCreatePinnedToCore(installGpioIsrOnCore0, "isr0", 2048, nullptr, 20, nullptr, 0);
    uint32_t t0 = millis();
    while (!s_gpio_isr_done && (uint32_t)(millis() - t0) < 1000) delay(2);
  }
#endif

  board.begin();

#ifdef DISPLAY_CLASS
  DisplayDriver* disp = NULL;
  if (display.begin()) {
    disp = &display;
    disp->startFrame();
  #ifdef ST7789
    disp->setTextSize(2);
  #endif
    disp->drawTextCentered(disp->width() / 2, 28, "Loading...");
    disp->endFrame();
  }
#endif

  if (!radio_init()) { halt(); }

  fast_rng.begin(radio_get_rng_seed());

#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  InternalFS.begin();
  #if defined(QSPIFLASH)
    if (!QSPIFlash.begin()) {
      // debug output might not be available at this point, might be too early. maybe should fall back to InternalFS here?
      MESH_DEBUG_PRINTLN("CustomLFS_QSPIFlash: failed to initialize");
    } else {
      MESH_DEBUG_PRINTLN("CustomLFS_QSPIFlash: initialized successfully");
    }
  #else
  #if defined(EXTRAFS)
      ExtraFS.begin();
  #endif
  #endif
  store.begin();
  the_mesh.begin(
    #ifdef DISPLAY_CLASS
        disp != NULL
    #else
        false
    #endif
  );

#ifdef BLE_PIN_CODE
  serial_interface.begin(BLE_NAME_PREFIX, the_mesh.getNodePrefs()->node_name, the_mesh.getBLEPin());
#else
  serial_interface.begin(Serial);
#endif
  the_mesh.startInterface(serial_interface);
#elif defined(RP2040_PLATFORM)
  LittleFS.begin();
  store.begin();
  the_mesh.begin(
    #ifdef DISPLAY_CLASS
        disp != NULL
    #else
        false
    #endif
  );

  //#ifdef WIFI_SSID
  //  WiFi.begin(WIFI_SSID, WIFI_PWD);
  //  serial_interface.begin(TCP_PORT);
  // #elif defined(BLE_PIN_CODE)
  //   char dev_name[32+16];
  //   sprintf(dev_name, "%s%s", BLE_NAME_PREFIX, the_mesh.getNodeName());
  //   serial_interface.begin(dev_name, the_mesh.getBLEPin());
  #if defined(SERIAL_RX)
    companion_serial.setPins(SERIAL_RX, SERIAL_TX);
    companion_serial.begin(115200);
    serial_interface.begin(companion_serial);
  #else
    serial_interface.begin(Serial);
  #endif
    the_mesh.startInterface(serial_interface);
#elif defined(ESP32)
  SPIFFS.begin(true);
  store.begin();
  the_mesh.begin(
    #ifdef DISPLAY_CLASS
        disp != NULL
    #else
        false
    #endif
  );

#if defined(WITH_WIFI)
  // WiFi and BLE can't coexist on this chip. In WiFi mode skip the BLE stack
  // entirely (frees the RAM esp_wifi_init needs) and drive the companion frame
  // protocol over USB serial instead, so _serial is a real, begun interface (the
  // mesh loop calls checkRecvFrame()/writeFrame() unconditionally). Toggling WiFi
  // needs a reboot to switch stacks.
  if (the_mesh.getNodePrefs()->wifi_enabled) {
    usb_serial.begin(Serial);
    the_mesh.startInterface(usb_serial);
  } else {
  #if defined(BLE_PIN_CODE)
    serial_interface.begin(BLE_NAME_PREFIX, the_mesh.getNodePrefs()->node_name, the_mesh.getBLEPin());
  #else
    serial_interface.begin(Serial);
  #endif
    the_mesh.startInterface(serial_interface);
  }
#elif defined(WIFI_SSID)
  board.setInhibitSleep(true);   // prevent sleep when WiFi is active
  WiFi.begin(WIFI_SSID, WIFI_PWD);
  serial_interface.begin(TCP_PORT);
  the_mesh.startInterface(serial_interface);
#elif defined(BLE_PIN_CODE)
  serial_interface.begin(BLE_NAME_PREFIX, the_mesh.getNodePrefs()->node_name, the_mesh.getBLEPin());
  the_mesh.startInterface(serial_interface);
#elif defined(SERIAL_RX)
  companion_serial.setPins(SERIAL_RX, SERIAL_TX);
  companion_serial.begin(115200);
  serial_interface.begin(companion_serial);
  the_mesh.startInterface(serial_interface);
#else
  serial_interface.begin(Serial);
  the_mesh.startInterface(serial_interface);
#endif
#else
  #error "need to define filesystem"
#endif

#if defined(ELECROW_CROWPANEL_ADVANCE_35) && (ENV_INCLUDE_GPS == 1)
  // CrowPanel has no on-board sensors, so sensors.begin() does nothing but the GPS
  // boot-detect probe (a ~1s delay()). Skip it -- and the GPS bring-up -- unless GPS
  // is actually enabled, so a unit with nothing on the rear UART plug never stalls.
  if (the_mesh.getNodePrefs()->gps_enabled) {
    sensors.begin();
    the_mesh.applyGpsPrefs();
  }
#else
  sensors.begin();
  #if ENV_INCLUDE_GPS == 1
    the_mesh.applyGpsPrefs();
  #endif
#endif

#ifdef MESH_PROXY
  if (the_mesh.getNodePrefs()->radio_off) radio_sleep();  // booted with the radio kill-switch on
  if (!mproxy::init()) Serial.println("MeshProxy: init failed (PSRAM/queue alloc)");
  mproxy::setBackend(the_mesh);         // so the UI can seed mutes in begin() (drainCommands runs later)
  mproxy::publishIfChanged(the_mesh);   // seed the first snapshot before the UI reads it
  // Create the backend task BEFORE ui_task.begin(): the LVGL draw buffers are
  // allocated greedily (sized to whatever internal RAM is free, down to a floor),
  // so if the UI starts first it eats the heap and the 16 KB task stack can no
  // longer be allocated (xTaskCreate fails -> no mesh/BLE at all). Reserving the
  // task stack first lets the draw buffers simply shrink to fit the remainder.
  xTaskCreatePinnedToCore(meshTask, "mesh", 16384, nullptr, 1, nullptr, 0);  // core 0
#endif

#ifdef DISPLAY_CLASS
  ui_task.begin(disp, &sensors, the_mesh.getNodePrefs());  // still want to pass this in as dependency, as prefs might be moved
#endif

#ifdef MESH_PROXY
  s_ui_ready = true;   // UI is up: release the core-0 mesh backend to start running
#endif
}

#ifdef MESH_PROXY
// The mesh/radio backend runs on its OWN FreeRTOS task pinned to core 0, so its
// CPU-bound crypto (Ed25519/AES) runs in parallel with LVGL on core 1 instead of
// blocking it. The UI talks to it only through MeshProxy: commands in, snapshot +
// events out. Only the HSPI bus is shared (serialized by the Phase-A mutex).
static void meshTask(void*) {
  // Wait for the UI to finish initializing before doing any mesh/radio/HSPI work, so
  // core 0 and core 1 don't race on the shared HSPI bus during startup (that race
  // intermittently wedged core 1's init -- stuck at "Loading...").
  while (!s_ui_ready) vTaskDelay(1);
  for (;;) {
    mproxy::drainCommands(the_mesh);    // execute UI-posted commands against the_mesh
    if (!the_mesh.getNodePrefs()->radio_off)   // radio kill-switch: don't transmit/receive
      the_mesh.loop();                  // process mesh; the 5 callbacks enqueue events
    mproxy::publishIfChanged(the_mesh); // republish the snapshot if anything changed
    mproxy::updateStats(the_mesh);      // refresh live node-info counters (display-only)
#if defined(WITH_WIFI) && defined(ESP32)
    the_mesh.wifiLoop();                // WiFi connect + NTP clock sync (WiFi mode)
#endif
#if defined(WITH_MQTT_BRIDGE)
    the_mesh.mqttLoop();                // MQTT bridge poll (WiFi mode)
#endif
    vTaskDelay(1);                      // yield a tick so idle/watchdog run
  }
}
#endif

void loop() {
#ifndef MESH_PROXY
  the_mesh.loop();   // single-core builds keep the mesh on the Arduino loop
#endif
  sensors.loop();
#ifdef DISPLAY_CLASS
  ui_task.loop();    // LVGL render + input + event drain, on core 1
#endif
  rtc_clock.tick();
}
