// rex::string::to_utf8 as skylanders-portal's USB portal uses it (Windows error texts and device strings), without
// the rexglue SDK. UTF-16 to UTF-8, unpaired surrogates as U+FFFD.
#pragma once

#include <string>
#include <string_view>

namespace rex::string {

inline std::string to_utf8(std::u16string_view in) {
  std::string out;
  out.reserve(in.size());
  for (size_t i = 0; i < in.size(); ++i) {
    char32_t c = in[i];
    if (c >= 0xD800 && c <= 0xDBFF && i + 1 < in.size() && in[i + 1] >= 0xDC00 && in[i + 1] <= 0xDFFF) {
      c = 0x10000 + ((c - 0xD800) << 10) + (in[i + 1] - 0xDC00);
      ++i;
    } else if (c >= 0xD800 && c <= 0xDFFF) {
      c = 0xFFFD;
    }
    if (c < 0x80) {
      out += static_cast<char>(c);
    } else if (c < 0x800) {
      out += static_cast<char>(0xC0 | (c >> 6));
      out += static_cast<char>(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
      out += static_cast<char>(0xE0 | (c >> 12));
      out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (c & 0x3F));
    } else {
      out += static_cast<char>(0xF0 | (c >> 18));
      out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
      out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (c & 0x3F));
    }
  }
  return out;
}

}  // namespace rex::string
