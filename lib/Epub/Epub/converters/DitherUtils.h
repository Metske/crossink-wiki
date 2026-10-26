#pragma once

#include <stdint.h>

// 4x4 Bayer matrix for ordered dithering
inline const uint8_t bayer4x4[4][4] = {
    {0, 8, 2, 10},
    {12, 4, 14, 6},
    {3, 11, 1, 9},
    {15, 7, 13, 5},
};

// Divide the full 8-bit range into four equally sized display levels.
inline uint8_t quantizeGrayTo4Level(uint8_t gray) { return gray >> 6; }

// Compensate 8-bit image luminance for the non-linear reflectance of 4-level
// e-ink grayscale (where physical levels 1 and 2 are much darker than linear
// 1/3 and 2/3 steps). Uses a gamma ~0.5 curve: out = floor(sqrt(gray * 255)).
constexpr uint8_t compensateEinkImageTone(uint8_t gray) {
  if (gray == 0) return 0;
  if (gray == 255) return 255;
  uint32_t product = static_cast<uint32_t>(gray) * 255u;
  uint32_t x = (static_cast<uint32_t>(gray) + 255u) >> 1;
  x = (x + product / x) >> 1;
  x = (x + product / x) >> 1;
  x = (x + product / x) >> 1;
  x = (x + product / x) >> 1;
  if (x * x > product) --x;
  return static_cast<uint8_t>(x > 255u ? 255u : x);
}

// Apply Bayer dithering and quantize to 4 levels (0-3)
// Stateless - works correctly with any pixel processing order
inline uint8_t applyBayerDither4Level(uint8_t gray, int x, int y) {
  constexpr uint16_t maxLevel = 3;
  constexpr uint16_t inputRange = 255;
  const uint16_t scaled = static_cast<uint16_t>(gray) * maxLevel;
  uint8_t level = static_cast<uint8_t>(scaled / inputRange);
  const uint16_t remainder = scaled % inputRange;

  // Cell-centred thresholds 8, 24, ... 248 give an even 16-step coverage.
  const uint16_t threshold = static_cast<uint16_t>(bayer4x4[y & 3][x & 3]) * 16 + 8;
  if (level < maxLevel && remainder > threshold) {
    ++level;
  }
  return level;
}
