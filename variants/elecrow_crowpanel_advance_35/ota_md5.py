# Post-build: write firmware.bin.md5 (bare lowercase hex) next to the app image.
# The OTA updater fetches <url>.md5 and hands it to Update.setMD5() so a corrupt or
# truncated download fails verification and we never switch off the good firmware.
import hashlib

Import("env")

def gen_md5(source, target, env):
    bin_path = env.subst("$BUILD_DIR/${PROGNAME}.bin")
    try:
        h = hashlib.md5()
        with open(bin_path, "rb") as f:
            for chunk in iter(lambda: f.read(65536), b""):
                h.update(chunk)
        with open(bin_path + ".md5", "w") as f:
            f.write(h.hexdigest())
        print("ota_md5: %s.md5 = %s" % (bin_path, h.hexdigest()))
    except OSError as e:
        print("ota_md5: skipped (%s)" % e)

env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", gen_md5)
