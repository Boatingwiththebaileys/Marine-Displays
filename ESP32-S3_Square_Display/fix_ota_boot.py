Import("env")
import os

# After every USB upload, write boot_app0.bin to the OTA data partition
# (0x29000) so the bootloader always boots from app0 instead of a stale
# app1 pointer left by a previous OTA update.

def after_upload(source, target, env):
    boot_app0 = os.path.join(
        env.PioPlatform().get_package_dir("framework-arduinoespressif32"),
        "tools", "partitions", "boot_app0.bin"
    )
    if not os.path.isfile(boot_app0):
        print("fix_ota_boot: boot_app0.bin not found at", boot_app0)
        return

    # Use PlatformIO's bundled esptool package so it works even when the
    # system Python doesn't have esptool installed.
    esptool_dir = env.PioPlatform().get_package_dir("tool-esptoolpy") or ""
    esptool_py  = os.path.join(esptool_dir, "esptool.py")
    if os.path.isfile(esptool_py):
        esptool_cmd = '"' + env.subst("$PYTHONEXE") + '" "' + esptool_py + '"'
    else:
        # Fallback: try the module path (works inside PlatformIO's venv)
        esptool_cmd = '"' + env.subst("$PYTHONEXE") + '" -m esptool'

    print("fix_ota_boot: writing boot_app0.bin to 0x29000 ...")
    env.Execute(
        " ".join([
            esptool_cmd,
            "--chip", "esp32s3",
            "--port", '"' + env.subst("$UPLOAD_PORT") + '"',
            "--baud", env.subst("$UPLOAD_SPEED"),
            "--before", "default_reset",
            "--after", "hard_reset",
            "write_flash",
            "-z",
            "0x29000",
            '"' + boot_app0 + '"',
        ])
    )
    print("fix_ota_boot: done")

env.AddPostAction("upload", after_upload)
