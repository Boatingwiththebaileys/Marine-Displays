#!/usr/bin/env python3
"""
Convert PNG images to RGB565 binary format for ESP32-S3
Usage: python3 convert_png_to_rgb565.py input.png output.bin
"""

import sys
from PIL import Image
import struct

def rgb888_to_rgb565(r, g, b):
    """Convert RGB888 to RGB565 format"""
    r5 = (r >> 3) & 0x1F
    g6 = (g >> 2) & 0x3F
    b5 = (b >> 3) & 0x1F
    return (r5 << 11) | (g6 << 5) | b5

def convert_png_to_rgb565(input_file, output_file):
    """Convert PNG to RGB565 binary file"""
    print(f"Converting {input_file} to {output_file}...")
    
    # Open and convert image to RGB
    img = Image.open(input_file).convert('RGB')
    width, height = img.size
    
    print(f"Image size: {width}x{height}")
    
    # Create binary file
    with open(output_file, 'wb') as f:
        pixel_count = 0
        for y in range(height):
            for x in range(width):
                r, g, b = img.getpixel((x, y))
                rgb565 = rgb888_to_rgb565(r, g, b)
                # Write as little-endian 16-bit value
                f.write(struct.pack('<H', rgb565))
                pixel_count += 1
    
    file_size = pixel_count * 2
    print(f"Converted {pixel_count} pixels ({file_size:,} bytes)")
    print(f"Done! Created {output_file}")

if __name__ == '__main__':
    if len(sys.argv) != 3:
        print("Usage: python3 convert_png_to_rgb565.py input.png output.bin")
        sys.exit(1)
    
    input_file = sys.argv[1]
    output_file = sys.argv[2]
    
    try:
        convert_png_to_rgb565(input_file, output_file)
    except Exception as e:
        print(f"Error: {e}")
        sys.exit(1)
