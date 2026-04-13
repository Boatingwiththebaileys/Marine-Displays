#!/usr/bin/env python3
import os
bin_file='Default.bin'
c_file='src/ui_img_default_png.c'
var_name='ui_img_default_png'
if not os.path.exists(bin_file):
    print('Missing', bin_file)
    raise SystemExit(1)
with open(bin_file,'rb') as f:
    data=f.read()
# Set dimensions (Default.png was 480x480)
width=480
height=480
with open(c_file,'w') as f:
    f.write('// This file was generated from %s\n' % bin_file)
    f.write('#include "ui.h"\n\n')
    f.write('#ifndef LV_ATTRIBUTE_MEM_ALIGN\n')
    f.write('#define LV_ATTRIBUTE_MEM_ALIGN\n')
    f.write('#endif\n\n')
    var_upper = var_name.upper()
    f.write('#ifndef LV_ATTRIBUTE_IMG_%s\n' % var_upper)
    f.write('#define LV_ATTRIBUTE_IMG_%s\n' % var_upper)
    f.write('#endif\n\n')
    f.write('const LV_ATTRIBUTE_MEM_ALIGN LV_ATTRIBUTE_LARGE_CONST LV_ATTRIBUTE_IMG_%s uint8_t %s_map[] = {\n' % (var_upper, var_name))
    for i in range(0, len(data), 16):
        chunk = data[i:i+16]
        hex_bytes = ', '.join('0x%02x' % b for b in chunk)
        f.write('  ' + hex_bytes + ',\n')
    f.write('};\n\n')
    f.write('const lv_img_dsc_t %s = {\n' % var_name)
    f.write('  .header = {\n')
    f.write('    .always_zero = 0,\n')
    f.write('    .reserved = 0,\n')
    f.write('    .cf = LV_IMG_CF_TRUE_COLOR,\n')
    f.write('    .w = %d,\n' % width)
    f.write('    .h = %d,\n' % height)
    f.write('  },\n')
    f.write('  .data_size = sizeof(%s_map),\n' % var_name)
    f.write('  .data = %s_map,\n' % var_name)
    f.write('};\n')
print('Wrote', c_file)
