#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

// The Adafruit GFX built-in 5x7 font has no UTF-8 support, so "Tomás" or
// "Caída" would render as garbage. asciiFold() converts the Spanish UTF-8
// text in game_data.h to plain ASCII (á->a, ñ->n, ...) and truncates to n-1.
inline void asciiFold(const char* in, char* out, size_t n) {
  if (n == 0) return;
  static const char MAP_C3[] =  // second byte 0x80..0xBF after 0xC3
      "AAAAAAACEEEEIIII"
      "DNOOOOOxOUUUUYTs"
      "aaaaaaaceeeeiiii"
      "dnooooo/ouuuuyty";
  size_t o = 0;
  const uint8_t* p = reinterpret_cast<const uint8_t*>(in ? in : "");
  while (*p && o + 1 < n) {
    uint8_t c = *p++;
    if (c < 0x80) {
      out[o++] = static_cast<char>(c);
      continue;
    }
    char rep = '?';
    if (c == 0xC3 && *p >= 0x80 && *p <= 0xBF) {
      rep = MAP_C3[*p++ - 0x80];
    } else if (c == 0xC2 && *p) {
      uint8_t d = *p++;
      rep = (d == 0xA1) ? '!' : (d == 0xBA || d == 0xB0) ? 'o' : '?';
    } else {
      while ((*p & 0xC0) == 0x80) p++;  // skip rest of any other sequence (e.g. em dash)
      rep = '-';
    }
    out[o++] = rep;
  }
  out[o] = '\0';
}

// Splits ASCII text into two lines of at most `width` chars, breaking at a space.
inline void wrapTwoLines(const char* text, char* l1, char* l2, size_t width) {
  size_t len = strlen(text);
  if (len <= width) {
    memcpy(l1, text, len + 1);
    l2[0] = '\0';
    return;
  }
  size_t cut = width;
  while (cut > 0 && text[cut] != ' ') cut--;
  if (cut == 0) cut = width;  // no space: hard cut
  memcpy(l1, text, cut);
  l1[cut] = '\0';
  const char* rest = text + cut;
  while (*rest == ' ') rest++;
  size_t rlen = strlen(rest);
  if (rlen > width) rlen = width;
  memcpy(l2, rest, rlen);
  l2[rlen] = '\0';
}
