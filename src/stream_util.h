#pragma once
// Arduino'dan bagimsiz, test edilebilir akis yardimcilari.
// R tipi: `int get()` fonksiyonu olmali (bayt dondurur, bitince/zaman asiminda -1).
#include <stddef.h>
#include <string.h>

// Akista `pat` dizisini bulana kadar okur. Bulursa true.
template <class R>
bool skipTo(R& r, const char* pat) {
  const size_t m = strlen(pat);
  size_t k = 0;
  int c;
  while ((c = r.get()) >= 0) {
    if ((char)c == pat[k]) {
      if (++k == m) return true;
    } else {
      k = ((char)c == pat[0]) ? 1 : 0;
    }
  }
  return false;
}

// JSON dizisinin siradaki { ... } nesnesini buf'a okur (NUL ile biter).
//  >0 : nesne uzunlugu
//   0 : nesne cok buyuktu, atlandi
//  -1 : dizi bitti ya da akis kesildi
template <class R>
int readObject(R& r, char* buf, size_t cap) {
  int c;
  for (;;) {
    c = r.get();
    if (c < 0 || c == ']') return -1;
    if (c == '{') break;
  }
  size_t len = 0;
  bool overflow = false;
  int depth = 1;
  bool inStr = false, esc = false;
  buf[len++] = '{';
  while (depth > 0) {
    c = r.get();
    if (c < 0) return -1;
    if (!overflow) {
      if (len < cap - 1) buf[len++] = (char)c;
      else overflow = true;
    }
    if (inStr) {
      if (esc) esc = false;
      else if (c == '\\') esc = true;
      else if (c == '"') inStr = false;
    } else {
      if (c == '"') inStr = true;
      else if (c == '{') depth++;
      else if (c == '}') depth--;
    }
  }
  if (overflow) return 0;
  buf[len] = 0;
  return (int)len;
}
