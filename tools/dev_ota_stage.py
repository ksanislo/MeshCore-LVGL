# Post-build: stage "<PIOENV>.bin" (+ ".bin.md5") into .devtmp/ota/ so the local dev HTTP server can
# host several boards' firmware CONCURRENTLY under distinct filenames. Run one server on .devtmp/ota/
# and point each device's OTA Custom URL at its own <env>.bin -- no firmware.bin name collision.
import hashlib, os, shutil

Import("env")

def stage(source, target, env):
    bin_path = env.subst("$BUILD_DIR/${PROGNAME}.bin")
    name = env["PIOENV"]
    out_dir = os.path.join(env["PROJECT_DIR"], ".devtmp", "ota")
    try:
        os.makedirs(out_dir, exist_ok=True)
        dst = os.path.join(out_dir, name + ".bin")
        shutil.copyfile(bin_path, dst)
        h = hashlib.md5()
        with open(dst, "rb") as f:
            for chunk in iter(lambda: f.read(65536), b""):
                h.update(chunk)
        with open(dst + ".md5", "w") as f:
            f.write(h.hexdigest())
        print("dev_ota_stage: %s (md5 %s)" % (dst, h.hexdigest()))
    except OSError as e:
        print("dev_ota_stage: skipped (%s)" % e)

env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", stage)
