# rawdata_converter.py
# This script prints RGB value for each pixel.

# Usage
# $ python ./rawdata_converter.py image.png > image.h

import sys
from PIL import Image

darken = True

filename = 'src.png'
name = 'src'
args = sys.argv
if 2 <= len(args):
  filename = args[1]
  tmp = args[1].split('.')
  name = tmp[0]

org_img = Image.open(filename)
org_img = org_img.convert('RGB')
org_w, org_h = org_img.size

print("#include <stdint.h>")
print("constexpr uint8_t " + name + "[" + str(org_h) + "][" + str(3*org_w) + "] = {")
for y in range(org_h):
  print("  {", end="")
  for x in range(org_w):
    r, g, b = org_img.getpixel((x, y))
    
    if darken:
      r = r // 2
      g = g // 2
      b = b // 2

    print(" 0x%02x, 0x%02x, 0x%02x" % (r, g, b), end="")
    if x != org_w - 1:
      print(",", end="")

  if y == org_h - 1:
    print("  }")
  else:
    print("  },")

print("};")
