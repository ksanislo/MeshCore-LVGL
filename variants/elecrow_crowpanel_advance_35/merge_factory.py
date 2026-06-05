# Post-build: produce <PROGNAME>-merged.bin -- a full factory image (bootloader @0x0, partition table
# @0x8000, boot_app0 @0xe000, app @0x10000), flashable at offset 0x0 by a web/serial flasher (with the
# erase option) to turn a bare device into our base firmware. From there the device can OTA. The plain
# <PROGNAME>.bin stays the app/OTA image the updater pulls. Mirrors MeshCore's "<name>-merged.bin".
import os

Import("env")

def gen_merged(source, target, env):
    build = env.subst("$BUILD_DIR")
    prog  = env.subst("$PROGNAME")
    app   = os.path.join(build, prog + ".bin")
    boot  = os.path.join(build, "bootloader.bin")
    parts = os.path.join(build, "partitions.bin")
    fw    = env.PioPlatform().get_package_dir("framework-arduinoespressif32")
    boot_app0 = os.path.join(fw or "", "tools", "partitions", "boot_app0.bin")
    esptool   = os.path.join(env.PioPlatform().get_package_dir("tool-esptoolpy") or "", "esptool.py")
    out   = os.path.join(build, prog + "-merged.bin")
    mcu   = env.BoardConfig().get("build.mcu", "esp32")
    flash_size = env.BoardConfig().get("upload.flash_size", "4MB")

    for p in (app, boot, parts, boot_app0, esptool):
        if not os.path.isfile(p):
            print("merge_factory: skipped (missing %s)" % p)
            return

    # esptool v4 syntax (tool-esptoolpy): merge_bin + --flash_* ; keep each file's own mode/freq header.
    rc = env.Execute(
        '"$PYTHONEXE" "%s" --chip %s merge_bin --flash_mode keep --flash_freq keep --flash_size %s '
        '-o "%s" 0x0 "%s" 0x8000 "%s" 0xe000 "%s" 0x10000 "%s"'
        % (esptool, mcu, flash_size, out, boot, parts, boot_app0, app)
    )
    if rc == 0:
        print("merge_factory: wrote %s" % out)
    else:
        print("merge_factory: esptool merge_bin failed (rc=%s)" % rc)

env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", gen_merged)
