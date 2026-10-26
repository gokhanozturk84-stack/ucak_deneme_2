#pragma once
// Irtifa (feet) -> RGB565 renk. Arduino'dan bagimsiz, bilgisayarda test edilebilir.
//  alcak = koyu, yuksek = acik (tek tonda kehribar -> krem)
#include <stdint.h>

#define ALT_UNKNOWN (-99999)
#define ALT_MAX_FT 40000

static inline uint16_t altRgb565(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static inline uint16_t altColor(int ft) {
  if (ft == ALT_UNKNOWN) return altRgb565(150, 150, 150);   // bilinmiyor: gri
  struct Stop { int ft; uint8_t r, g, b; };
  static const Stop S[] = {
    {     0, 140,  60,   0},   // cok alcak: koyu kehribar
    {  5000, 190, 100,  10},
    { 10000, 235, 160,  40},
    { 20000, 250, 210, 110},
    { 30000, 255, 235, 170},
    { 40000, 255, 255, 225},   // cok yuksek: acik krem
  };
  const int N = sizeof(S) / sizeof(S[0]);
  if (ft <= S[0].ft) return altRgb565(S[0].r, S[0].g, S[0].b);
  if (ft >= S[N - 1].ft) return altRgb565(S[N - 1].r, S[N - 1].g, S[N - 1].b);
  for (int i = 1; i < N; i++) {
    if (ft <= S[i].ft) {
      int span = S[i].ft - S[i - 1].ft;
      int t = ft - S[i - 1].ft;   // 0..span
      uint8_t r = (uint8_t)(S[i - 1].r + (S[i].r - S[i - 1].r) * t / span);
      uint8_t g = (uint8_t)(S[i - 1].g + (S[i].g - S[i - 1].g) * t / span);
      uint8_t b = (uint8_t)(S[i - 1].b + (S[i].b - S[i - 1].b) * t / span);
      return altRgb565(r, g, b);
    }
  }
  return altRgb565(255, 255, 255);
}
