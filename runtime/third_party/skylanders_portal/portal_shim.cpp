// The two functions of skylanders-portal the copied USB portal files call, without the rest of that library (its
// figure files, its AES, the rexglue SDK): FindSkylander (figure_catalog.cpp; the traffic log names a figure the game
// read) and FigureCrc16 (figure_crypto.cpp; a dumped block 0's checksum). Copied from skylanders-portal (MIT, see
// LICENSE beside this file; the version in VERSION.txt), unchanged but for what they leave out.
#include <tuple>

#include "portal/figure_catalog.h"
#include "portal/figure_crypto.h"
#include "portal/skylander_catalog_data.h"

namespace skylanders::portal {

const SkylanderInfo* FindSkylander(uint16_t id, uint16_t variant, bool* exact) {
  // The plainest version of the character: the lowest variant without its game nibble, then the lowest variant; one
  // that was never released only if there is nothing else.
  auto rank = [](const SkylanderInfo& s) {
    return std::make_tuple(s.category == "Unreleased", s.variant & 0x0FFF, s.variant);
  };
  const SkylanderInfo* plainest = nullptr;
  for (const auto& sky : kSkylanderCatalog) {
    if (sky.id != id) continue;
    if (sky.variant == variant) {
      if (exact) *exact = true;
      return &sky;
    }
    if (!plainest || rank(sky) < rank(*plainest)) plainest = &sky;
  }
  if (exact) *exact = false;
  return plainest;
}

uint16_t FigureCrc16(const uint8_t* data, size_t size) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < size; ++i) {
    crc ^= static_cast<uint16_t>(data[i]) << 8;
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021) : static_cast<uint16_t>(crc << 1);
    }
  }
  return crc;
}

}  // namespace skylanders::portal
