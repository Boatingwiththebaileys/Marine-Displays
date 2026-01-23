#!/bin/bash
# Batch convert all PNG background images to RGB565 binary format

echo "Converting PNG images to RGB565 binary format..."

# Install PIL if needed
python3 -c "import PIL" 2>/dev/null || pip3 install Pillow

# Convert each background image
python3 convert_png_to_rgb565.py assets/Rev_Counter.png assets/Rev_Counter.bin
python3 convert_png_to_rgb565.py assets/Rev_Fuel.png assets/Rev_Fuel.bin
python3 convert_png_to_rgb565.py assets/Temp_Exhaust.png assets/Temp_Exhaust.bin
python3 convert_png_to_rgb565.py assets/Fuel_Temp.png assets/Fuel_Temp.bin
python3 convert_png_to_rgb565.py assets/Oil_Temp.png assets/Oil_Temp.bin

echo ""
echo "Done! Copy these .bin files to your SD card /assets/ folder"
echo "Original PNGs can be kept as backup or deleted to save space"
