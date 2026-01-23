Boating with the Baileys
Jan 2026

ESP32 customisable display using data from SignalK. It features:
Customisable background images,
Custom icons, 
Alerting through icon colours and built in buzzer
Global or Per Screen buzzer
WebUI driven
Touch and non Touch options

ESP32-S3 2.8" IPS LCD Round Display

This folder contains files and documentation for building the ESP32-S3 based 2.8" IPS LCD round display.

Contents:
- PlatformIO project sources (refer to root `src/`)

Build and upload instructions:
1. Install PlatformIO and required toolchains.
2. Open the project root and run `pio run` then `pio run --target upload`.

See the main project root for full source code and assets.

Use and SD cards for your icons and images, Store the icons and png (ideally monochrome images) and then you can change the colours. Convert your larger background images to bin files. There is a convert script in the project that will help you in you cant do this in your image tool.

The display is 480x480, so make your background images this size

Use 70x70 for icons

I used Figma to create my images.

Save them on the sd card in a folder called /assests

Video coming soon of the whole feature set and setup.

The Baileys






How to convert PNG backgrounds
------------------------------

This project uses raw RGB565 `.bin` background files on the SD card. The workflow we used is:

- Convert `*.png` to RGB565 `.bin` using `convert_png_to_rgb565.py` (or run `batch_convert.sh` which calls it for the assets).
- Copy the produced `.bin` files to the SD card `assets/` folder on the display.

Example (from project root):

```bash
# convert a single PNG to a .bin
python3 convert_png_to_rgb565.py assets/Rev_Counter.png assets/Rev_Counter.bin

# or run the batch helper (installs Pillow if needed)
./batch_convert.sh
```
