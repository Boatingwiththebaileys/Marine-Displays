Boating with the Baileys
Feb 2026

ESP32 customisable display using data from SignalK. 
------------------------------

It features:
- Customisable background images,
- Custom icons, 
- Alerting through icon colours and built in buzzer
- Global or Per Screen buzzer
- WebUI driven
- Touch and non Touch options
- Number display with different display options, Single large display, Dual screen, Quad Screen
- Guage with added number display (Wind instrument style)
- Graph display with the option for an additional data source & data retained while device is powered
- Navigation Display with heading & Dual data feilds
- Position and Time display
- AIS Radar display

ESP32-S3 Square Display

The hardware used (Supports V3 & V4 boards):
ESP32-S3 4inch Display Development Board, 480×480, 32-Bit LX7 Dual-Core Processor, Up To 240MHz Frequency, Supports WiFi & Bluetooth, With Onboard Antenna, ESP32 With Display https://www.waveshare.com/esp32-s3-touch-lcd-4.htm

This folder contains files and documentation for building the ESP32-S3 based square display.

Contents:
- PlatformIO project sources (refer to root `src/`)

Build and upload instructions:
1. Install PlatformIO and required toolchains.
2. Open the project root and run `pio run` then `pio run --target upload`.

See the main project root for full source code and assets.

Use an SD card for your icons and images. Store icons and PNGs (ideally monochrome images) so you can recolor them in the UI. Convert larger background images to `.bin` files. There is a conversion script in the project to help with this.

The display is 480x480, so make your background images this size.

Use 70x70 for icons.

I used Figma to create my images.

Save them on the SD card in a folder called `/assets`.

On first boot, the display creates an SSID called ESP32-SquareDisplay with the password: 12345678 - Browse to 192.168.4.1 to configure your display. 

Here is a video of the device running the code and the different display options - DIY ESP32 Marine MFD – Square Multifunction Boat Display using SignalK Data https://youtu.be/FAPvdz6oN7A

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
