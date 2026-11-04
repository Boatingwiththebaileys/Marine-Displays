#!/usr/bin/env python3
"""
Turn a full flash image (marine-display-*.bin, written at 0x0) into the
separate parts the web installer flashes, and point its manifest at them.

Why: the full image runs continuously from 0x0 to the end of the app, so the
gap between the partition table and app0 is filled with 0xFF. Flashing it
whole wipes the NVS partition (WiFi and other saved settings) even when
"Erase device" is left unticked. Flashing only these parts leaves NVS (and
SPIFFS, coredump, app1) untouched:

    bootloader       0x0
    partition table  0x8000
    otadata          from the table (boot_app0: boot the new app0)
    app0             from the table

Offsets are read from the image's own partition table, so this works for any
display and for existing releases.

Usage:
    python tools/split_installer_image.py docs/marine-display-square.bin docs/manifest-square.json
Writes docs/marine-display-square/{bootloader,partitions,otadata,app}.bin,
rewrites the manifest's "builds", and removes the full image from docs/.
"""
import json
import os
import struct
import sys

PART_TABLE_OFFSET = 0x8000
PART_TABLE_SIZE = 0xC00


def read_partitions(img):
    parts = []
    for i in range(PART_TABLE_OFFSET, PART_TABLE_OFFSET + PART_TABLE_SIZE, 32):
        entry = img[i:i + 32]
        if entry[:2] != b"\xaa\x50":
            break
        ptype, subtype, offset, size = struct.unpack_from("<BBII", entry, 2)
        label = entry[12:28].split(b"\0")[0].decode(errors="replace")
        parts.append((label, ptype, subtype, offset, size))
    return parts


def trim_ff(data):
    """Drop trailing erased bytes (keep 4-byte alignment)."""
    end = len(data)
    while end > 0 and data[end - 1] == 0xFF:
        end -= 1
    end = (end + 3) & ~3
    return data[:end]


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    image_path, manifest_path = sys.argv[1:]
    img = open(image_path, "rb").read()
    if img[0] != 0xE9:
        sys.exit(f"{image_path}: doesn't start with a bootloader image")

    parts = read_partitions(img)
    otadata = next((p for p in parts if p[1] == 1 and p[2] == 0x00), None)
    app0 = next((p for p in parts if p[1] == 0 and p[2] in (0x00, 0x10)), None)
    if not app0:
        sys.exit(f"{image_path}: no app partition in the partition table")
    first_data = min(p[3] for p in parts)

    pieces = [
        ("bootloader.bin", 0x0, img[0x0:PART_TABLE_OFFSET]),
        ("partitions.bin", PART_TABLE_OFFSET, img[PART_TABLE_OFFSET:min(first_data, PART_TABLE_OFFSET + 0x1000)]),
    ]
    if otadata:
        pieces.append(("otadata.bin", otadata[3], img[otadata[3]:otadata[3] + otadata[4]]))
    pieces.append(("app.bin", app0[3], img[app0[3]:]))

    name = os.path.splitext(os.path.basename(image_path))[0]
    out_dir = os.path.join(os.path.dirname(image_path), name)
    os.makedirs(out_dir, exist_ok=True)

    builds_parts = []
    for fname, offset, data in pieces:
        data = trim_ff(data)
        if not data:
            continue
        if fname == "app.bin" and data[0] != 0xE9:
            sys.exit(f"{image_path}: no app image at {offset:#x}")
        with open(os.path.join(out_dir, fname), "wb") as f:
            f.write(data)
        builds_parts.append({"path": f"{name}/{fname}", "offset": offset})
        print(f"  {name}/{fname:15s} at {offset:#08x}  {len(data):>8} bytes")

    with open(manifest_path) as f:
        manifest = json.load(f)
    manifest["builds"] = [{"chipFamily": "ESP32-S3", "parts": builds_parts}]
    with open(manifest_path, "w") as f:
        json.dump(manifest, f, indent=2, ensure_ascii=False)
        f.write("\n")

    os.remove(image_path)


if __name__ == "__main__":
    main()
