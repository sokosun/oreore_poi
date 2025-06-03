import sys

print("#include <stdint.h>")
print("constexpr uint8_t red[1][720] = {")

print("  {", end="")
for x in range (0,717,3):
  print("0x20, 0x00, 0x00, ", end="")
print(  "0x20, 0x00, 0x00}")

print("};")


print("constexpr uint8_t green[1][720] = {")

print("  {", end="")
for x in range (0,717,3):
  print("0x00, 0x20, 0x00, ", end="")
print(  "0x00, 0x20, 0x00}")

print("};")


print("constexpr uint8_t blue[1][720] = {")

print("  {", end="")
for x in range (0,717,3):
  print("0x00, 0x00, 0x20, ", end="")
print(  "0x00, 0x00, 0x20}")

print("};")


