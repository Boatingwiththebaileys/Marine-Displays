#!/usr/bin/env python3
"""
Merge a PlatformIO ESP32-S3 build into one .bin that flashes at offset 0.

The web installer (docs/) and anyone using esptool by hand can write this
single file instead of four separate images. Offsets for otadata and the
first app partition are read from the project's partitions.csv, so the
image always matches the layout the firmware expects.

Usage (from the repo root, after `pio run` in the display folder):
    python tools/merge_firmware.py ESP32-S3_Square_Display waveshare_square out/square.bin
"""
import csv
import glob
import os
import subprocess
import sys

BOOTLOADER_OFFSET = 0x0     # ESP32-S3 second-stage bootloader
PARTITIONS_OFFSET = 0x8000  # partition table


def read_partitions(path):
    """Return {name: (subtype, offset)} from a partitions.csv."""
    parts = {}
    with open(path) as f:
        rows = (line for line in f if line.strip() and not line.lstrip().startswith("#"))
        for row in csv.reader(rows):
            row = [c.strip() for c in row]
            if len(row) >= 4:
                parts[row[0]] = (row[2], int(row[3], 0))
    return parts


def find_boot_app0():
    home = os.path.expanduser("~/.platformio/packages")
    hits = sorted(glob.glob(os.path.join(home, "framework-arduinoespressif32*", "tools", "partitions", "boot_app0.bin")))
    if not hits:
        sys.exit("boot_app0.bin not found; run `pio run` first so the Arduino framework is installed")
    return hits[0]


def find_esptool():
    home = os.path.expanduser("~/.platformio/packages")
    for pattern in ("tool-esptoolpy/esptool.py", "tool-esptoolpy*/esptool.py"):
        hits = sorted(glob.glob(os.path.join(home, pattern)))
        if hits:
            return [sys.executable, hits[0]]
    return [sys.executable, "-m", "esptool"]


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    project, env, out = sys.argv[1:]
    build = os.path.join(project, ".pio", "build", env)

    parts = read_partitions(os.path.join(project, "partitions.csv"))
    otadata = next(off for sub, off in parts.values() if sub == "ota")
    app0 = next(off for sub, off in parts.values() if sub in ("ota_0", "factory"))

    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    cmd = find_esptool() + [
        "--chip", "esp32s3", "merge_bin", "-o", out,
        "--flash_mode", "dio", "--flash_freq", "80m", "--flash_size", "16MB",
        hex(BOOTLOADER_OFFSET), os.path.join(build, "bootloader.bin"),
        hex(PARTITIONS_OFFSET), os.path.join(build, "partitions.bin"),
        hex(otadata), find_boot_app0(),
        hex(app0), os.path.join(build, "firmware.bin"),
    ]
    print(" ".join(cmd))
    subprocess.check_call(cmd)


if __name__ == "__main__":
    main()
