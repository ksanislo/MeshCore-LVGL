#pragma once

#include <MeshCore.h>
#include <helpers/ui/DisplayDriver.h>
#include <helpers/ui/UIScreen.h>
#include <helpers/SensorManager.h>
#include <helpers/BaseSerialInterface.h>
#include <Arduino.h>

#ifdef PIN_BUZZER
  #include <helpers/ui/buzzer.h>
#endif

#include "NodePrefs.h"

enum class UIEventType {
    none,
    contactMessage,
    channelMessage,
    roomMessage,
    newContactMessage,
    ack
};

class AbstractUITask {
protected:
  mesh::MainBoard* _board;
  BaseSerialInterface* _serial;
  bool _connected;

  AbstractUITask(mesh::MainBoard* board, BaseSerialInterface* serial) : _board(board), _serial(serial) {
    _connected = false;
  }

public:
  void setHasConnection(bool connected) { _connected = connected; }
  bool hasConnection() const { return _connected; }
  uint16_t getBattMilliVolts() const { return _board->getBattMilliVolts(); }
  bool isSerialEnabled() const { return _serial->isEnabled(); }
  void enableSerial() { _serial->enable(); }
  void disableSerial() { _serial->disable(); }
  virtual void msgRead(int msgcount) = 0;
  virtual void newMsg(uint8_t path_len, const char* from_name, const char* text, int msgcount) = 0;
  // Called when a plain-text message is sent (e.g. composed on a connected
  // companion client), so an on-device UI can reflect it. peer = recipient
  // contact name or channel name. Default no-op for UIs that don't track it.
  virtual void sentMsg(const char* peer, const char* text) { (void)peer; (void)text; }
  // Called when a telemetry response arrives from a contact, so an on-device UI
  // can display it. `lpp` is the raw CayenneLPP payload. Default no-op.
  virtual void telemetryResponse(const uint8_t* pubkey, const char* from_name,
                                 const uint8_t* lpp, uint8_t lpp_len) {
    (void)pubkey; (void)from_name; (void)lpp; (void)lpp_len;
  }
  // Called when an outgoing message's ACK arrives (delivery confirmed), so an
  // on-device UI can mark it delivered. `ack` matches the value from sendMessage.
  virtual void msgDelivered(uint32_t ack) { (void)ack; }
  virtual void notify(UIEventType t = UIEventType::none) = 0;
  virtual void loop() = 0;
};
