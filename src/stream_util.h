#pragma once
// Arduino'dan bagimsiz, test edilebilir akis yardimcilari.
// R tipi: `int get()` fonksiyonu olmali (bayt dondurur, bitince/zaman asiminda -1).
#include <stddef.h>
#include <stdint.h>
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

// HTTP govdesini okur; "Transfer-Encoding: chunked" ise parcalari birlestirir.
// S: `int read1()` (tek bayt, bitti/zaman asimi = -1) ve
//    `int readBlock(uint8_t* b, int n)` (>0 bayt, bitti/zaman asimi = -1) saglamali.
template <class S>
struct BodyReader {
  S* s = nullptr;
  bool chunked = false;
  long chunkLeft = 0;
  bool done = false;
  uint8_t b[512];
  int pos = 0, len = 0;

  int get() {
    if (pos < len) return b[pos++];
    if (done) return -1;
    int want = (int)sizeof(b);
    if (chunked) {
      if (chunkLeft == 0 && !nextChunk()) { done = true; return -1; }
      if (chunkLeft < want) want = (int)chunkLeft;
    }
    int n = s->readBlock(b, want);
    if (n <= 0) { done = true; return -1; }
    len = n;
    pos = 0;
    if (chunked) chunkLeft -= n;
    return b[pos++];
  }

 private:
  bool nextChunk() {
    long sz = 0;
    bool any = false;
    for (;;) {
      int c = s->read1();
      if (c < 0) return false;
      if (c == '\r') continue;
      if (c == '\n') { if (any) break; continue; }   // onceki parcanin CRLF'i
      if (c == ';') {                                 // chunk uzantisi: satir sonuna kadar atla
        while ((c = s->read1()) != '\n') if (c < 0) return false;
        if (any) break;
        continue;
      }
      int v;
      if (c >= '0' && c <= '9') v = c - '0';
      else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
      else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
      else return false;
      sz = sz * 16 + v;
      any = true;
    }
    if (sz == 0) return false;   // son parca
    chunkLeft = sz;
    return true;
  }
};
