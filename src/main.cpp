// Ucak Radar - ESP32-C3 + GC9A01 (240x240 yuvarlak TFT)
//
//  * Acilista WiFi AP yayinlar (UcakRadar) + captive portal ayar sayfasi
//  * Ayar sayfasindan: ev WiFi adi/sifresi, enlem, boylam, yaricap (km)
//  * Ayarlar kaydedilince cihaz yeniden baslar, ev agina baglanir
//  * Ucak verisi: airplanes.live (yedek: adsb.lol) - ucretsiz, API anahtari yok
//  * Ekran saniyede 2 kez yenilenir; ucaklar hiz/yone gore ara konumlandirilir

#include <Arduino.h>
#include <esp_system.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <math.h>

#include "stream_util.h"
#include "alt_color.h"

// ======================= Ayarlanabilir sabitler =======================
static const char*    AP_SSID           = "UcakRadar";
static const char*    AP_PASS           = "radar1234";   // en az 8 karakter
static const uint32_t FETCH_INTERVAL_MS = 5000;          // veri cekme araligi
static const uint32_t FRAME_MS          = 500;           // ekran yenileme (2 Hz)
static const int      MAX_AC            = 60;            // en fazla ucak sayisi
static const int      RESET_PIN         = 9;             // BOOT dugmesi: 5 sn bas = ayarlari sil
#define FW_VERSION "v8-koyu-acik"

struct Provider {
  const char* name;
  const char* urlFmt;   // %.4f enlem, %.4f boylam, %d yaricap (deniz mili)
  const char* arrayKey; // JSON'da ucak dizisinin anahtari
};
static const Provider PROVIDERS[3] = {
  {"adsb.lol",   "https://api.adsb.lol/v2/point/%.4f/%.4f/%d",                 "\"ac\":"},
  {"adsb.fi",    "https://opendata.adsb.fi/api/v2/lat/%.4f/lon/%.4f/dist/%d",  "\"aircraft\":"},
  {"airplanes",  "https://api.airplanes.live/v2/point/%.4f/%.4f/%d",           "\"ac\":"},
};

// Ekran geometrisi
static const int CX = 120, CY = 120, RAD = 104;

// ======================= Global durum =======================
struct Config {
  String ssid, pass;
  double lat = 0, lon = 0;
  float  radiusKm = 50;
  bool   hasLoc = false;
};
static Config cfg;
static Preferences prefs;

struct Aircraft {
  char  id[9];
  float dx, dy;   // merkeze gore km (dogu, kuzey) - veri zamaninda
  float vE, vN;   // km/sn
  float hdg;      // derece
  int32_t altFt;  // irtifa (feet), bilinmiyorsa ALT_UNKNOWN
};
static Aircraft acList[MAX_AC];
static int acCount = 0;
static volatile uint32_t acStamp = 0;   // son basarili verinin millis() degeri
static SemaphoreHandle_t acMutex;

static WebServer server(80);
static DNSServer dns;
// GC9A01 ekran tanimi (pinler platformio.ini'deki -DPIN_* bayraklarindan gelir)
#ifndef LCD_INVERT
#define LCD_INVERT true
#endif

class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_GC9A01 _panel;
  lgfx::Bus_SPI _bus;

 public:
  LGFX() {
    {
      auto c = _bus.config();
      c.spi_host = SPI2_HOST;
      c.spi_mode = 0;
      c.freq_write = 40000000;
      c.freq_read = 16000000;
      c.spi_3wire = true;
      c.use_lock = true;
      c.dma_channel = SPI_DMA_CH_AUTO;
      c.pin_sclk = PIN_SCLK;
      c.pin_mosi = PIN_MOSI;
      c.pin_miso = -1;
      c.pin_dc = PIN_DC;
      _bus.config(c);
      _panel.setBus(&_bus);
    }
    {
      auto c = _panel.config();
      c.pin_cs = PIN_CS;
      c.pin_rst = PIN_RST;
      c.pin_busy = -1;
      c.panel_width = 240;
      c.panel_height = 240;
      c.offset_x = 0;
      c.offset_y = 0;
      c.readable = false;
      c.invert = LCD_INVERT;
      c.rgb_order = false;
      c.dlen_16bit = false;
      c.bus_shared = false;
      _panel.config(c);
    }
    setPanel(&_panel);
  }
};

static LGFX tft;
static LGFX_Sprite spr(&tft);

// Metni (cx, cy) merkezli cizer
static void drawCentered(const char* s, int cx, int cy, uint16_t color) {
  spr.setTextColor(color);
  int w = spr.textWidth(s);
  int h = spr.fontHeight();
  spr.drawString(s, cx - w / 2, cy - h / 2);
}
static bool sprOk = false;

static uint32_t restartAt = 0;

// Bulunan WiFi aglari (acilista bir kez taranir; AP calisirken tarama yayini bozar)
static String scanList[16];
static int scanCount = 0;
static uint32_t rescanAt = 0;

static void doScan() {
  int n = WiFi.scanNetworks(false, false);
  scanCount = 0;
  for (int i = 0; i < n && scanCount < 16; i++) {
    String ss = WiFi.SSID(i);
    if (ss.isEmpty()) continue;
    bool dup = false;
    for (int j = 0; j < scanCount; j++) {
      if (scanList[j] == ss) { dup = true; break; }
    }
    if (!dup) scanList[scanCount++] = ss;
  }
  WiFi.scanDelete();
}

// Renkler (RGB565)
static inline uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}
static uint16_t C_BG, C_RING, C_AXIS, C_PLANE, C_LABEL, C_CENTER, C_DIM, C_ERR, C_OK;

// ======================= Ayarlar (NVS) =======================
static void loadConfig() {
  prefs.begin("radar", false);
  cfg.ssid     = prefs.getString("ssid", "");
  cfg.pass     = prefs.getString("pass", "");
  cfg.hasLoc   = prefs.getBool("hasloc", false);
  cfg.lat      = prefs.getDouble("lat", 0);
  cfg.lon      = prefs.getDouble("lon", 0);
  cfg.radiusKm = prefs.getFloat("rad", 50);
  prefs.end();
  if (cfg.radiusKm < 1) cfg.radiusKm = 1;
  if (cfg.radiusKm > 400) cfg.radiusKm = 400;
}

// ======================= Ucak verisi (HTTPS akis ayristirma) =======================
struct NetSource {
  WiFiClient* c = nullptr;
  int read1() {
    uint32_t t = millis();
    for (;;) {
      int x = c->read();
      if (x >= 0) return x;
      if (!c->connected() && !c->available()) return -1;
      if (millis() - t > 6000) return -1;
      delay(2);
    }
  }
  int readBlock(uint8_t* b, int n) {
    uint32_t t = millis();
    for (;;) {
      int k = c->read(b, n);
      if (k > 0) return k;
      if (!c->connected() && !c->available()) return -1;
      if (millis() - t > 6000) return -1;
      delay(2);
    }
  }
};

static char netStatus[40] = "";   // ekranda gosterilen son veri durumu
static JsonDocument jsonFilter;
static char objBuf[1800];
static Aircraft tmpList[MAX_AC];

static void initFilter() {
  jsonFilter["flight"]       = true;
  jsonFilter["hex"]          = true;
  jsonFilter["lat"]          = true;
  jsonFilter["lon"]          = true;
  jsonFilter["gs"]           = true;
  jsonFilter["track"]        = true;
  jsonFilter["true_heading"] = true;
  jsonFilter["mag_heading"]  = true;
  jsonFilter["alt_baro"]     = true;
  jsonFilter["alt_geom"]     = true;
  jsonFilter["seen_pos"]     = true;
}

static bool fetchFrom(const Provider& pv, int nm) {
  WiFiClientSecure client;
  client.setInsecure();   // sertifika dogrulamasi yok (bellek/basitlik icin)

  char url[160];
  snprintf(url, sizeof(url), pv.urlFmt, cfg.lat, cfg.lon, nm);

  const char* shortHost = pv.name;
  Serial.printf("fetch %s heap=%u max=%u\n", url, (unsigned)ESP.getFreeHeap(),
                (unsigned)ESP.getMaxAllocHeap());

  HTTPClient http;
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  http.setReuse(false);              // "Connection: close"
  http.setConnectTimeout(6000);
  http.setTimeout(8000);
  http.setUserAgent("Mozilla/5.0 (compatible; UcakRadar-ESP32C3/1.0)");
  http.addHeader("Accept", "application/json");
  http.addHeader("Accept-Encoding", "identity");   // sikistirma isteme
  const char* wantHeaders[] = {"Transfer-Encoding"};
  http.collectHeaders(wantHeaders, 1);
  if (!http.begin(client, String(url))) {
    snprintf(netStatus, sizeof(netStatus), "%s: begin hata", shortHost);
    return false;
  }

  int code = http.GET();
  if (code != 200) {
    String why = code < 0 ? HTTPClient::errorToString(code) : String("");
    snprintf(netStatus, sizeof(netStatus), "%s: %d %.18s", shortHost, code, why.c_str());
    Serial.printf("HTTP hata: %s\n", netStatus);
    http.end();
    return false;
  }
  const uint32_t tData = millis();

  NetSource src;
  src.c = http.getStreamPtr();
  BodyReader<NetSource> rd;
  rd.s = &src;
  rd.chunked = http.header("Transfer-Encoding").indexOf("chunked") >= 0;
  if (!skipTo(rd, pv.arrayKey)) {
    snprintf(netStatus, sizeof(netStatus), "%s: veri formati", shortHost);
    Serial.println("ac alani bulunamadi");
    http.end();
    return false;
  }

  const double cosLat = cos(cfg.lat * DEG_TO_RAD);
  int n = 0;
  while (n < MAX_AC) {
    int len = readObject(rd, objBuf, sizeof(objBuf));
    if (len < 0) break;
    if (len == 0) continue;

    JsonDocument doc;
    if (deserializeJson(doc, objBuf, (size_t)len, DeserializationOption::Filter(jsonFilter))) continue;

    if (!doc["lat"].is<double>() || !doc["lon"].is<double>()) continue;
    if (doc["alt_baro"].is<const char*>()) continue;   // "ground" -> yerdeki ucak

    double lat = doc["lat"].as<double>();
    double lon = doc["lon"].as<double>();
    double dLat = lat - cfg.lat;
    double dLon = lon - cfg.lon;
    if (dLon > 180) dLon -= 360;
    if (dLon < -180) dLon += 360;
    float north = (float)(dLat * 111.195);
    float east  = (float)(dLon * 111.195 * cosLat);

    float gs  = doc["gs"] | 0.0f;                 // knot
    float hdg = -1;
    if (doc["track"].is<double>()) hdg = doc["track"].as<float>();
    else if (doc["true_heading"].is<double>()) hdg = doc["true_heading"].as<float>();
    else if (doc["mag_heading"].is<double>()) hdg = doc["mag_heading"].as<float>();
    float sp = doc["seen_pos"] | 0.0f;            // konumun yasi (sn)

    Aircraft& a = tmpList[n];
    memset(&a, 0, sizeof(a));
    const char* fl = doc["flight"] | "";
    while (*fl == ' ') fl++;
    strncpy(a.id, fl, 8);
    a.id[8] = 0;
    for (int i = (int)strlen(a.id) - 1; i >= 0 && a.id[i] == ' '; i--) a.id[i] = 0;
    if (!a.id[0]) {
      const char* hx = doc["hex"] | "?";
      strncpy(a.id, hx, 8);
      a.id[8] = 0;
    }

    if (hdg >= 0 && gs > 0) {
      float v = gs * 1.852f / 3600.0f;            // km/sn
      a.vE = v * sinf(hdg * DEG_TO_RAD);
      a.vN = v * cosf(hdg * DEG_TO_RAD);
    }
    a.hdg = hdg >= 0 ? hdg : 0;
    a.altFt = ALT_UNKNOWN;
    if (doc["alt_baro"].is<double>()) a.altFt = (int32_t)doc["alt_baro"].as<double>();
    else if (doc["alt_geom"].is<double>()) a.altFt = (int32_t)doc["alt_geom"].as<double>();
    a.dx = east + a.vE * sp;
    a.dy = north + a.vN * sp;
    n++;
  }
  http.end();

  xSemaphoreTake(acMutex, portMAX_DELAY);
  memcpy(acList, tmpList, sizeof(Aircraft) * n);
  acCount = n;
  acStamp = tData;
  xSemaphoreGive(acMutex);
  snprintf(netStatus, sizeof(netStatus), "%s: %d ucak", shortHost, n);
  Serial.printf("Veri alindi: %s\n", netStatus);
  return true;
}

static void netTask(void*) {
  uint32_t last = 0;
  bool first = true;
  int hostIdx = 0;
  for (;;) {
    if (WiFi.status() == WL_CONNECTED && cfg.hasLoc && cfg.ssid.length()) {
      if (first || millis() - last >= FETCH_INTERVAL_MS) {
        first = false;
        last = millis();
        int nm = (int)ceilf(cfg.radiusKm / 1.852f);
        if (nm < 1) nm = 1;
        if (nm > 250) nm = 250;
        if (!fetchFrom(PROVIDERS[hostIdx], nm)) {
          Serial.printf("Veri alinamadi: %s\n", PROVIDERS[hostIdx].name);
          hostIdx = (hostIdx + 1) % 3;   // siradaki kaynaga gec
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}

// ======================= Ekran =======================
static void drawScreen(const String* lines, const uint16_t* colors, int n) {
  if (!sprOk) return;
  spr.fillSprite(C_BG);
  spr.setTextFont(2);
  int y0 = CY - (n - 1) * 11;
  for (int i = 0; i < n; i++) {
    drawCentered(lines[i].c_str(), CX, y0 + i * 22, colors[i]);
  }
  spr.pushSprite(0, 0);
}

static void rot(float x, float y, float c, float s, int px, int py, int& ox, int& oy) {
  ox = px + lroundf(x * c - y * s);
  oy = py + lroundf(x * s + y * c);
}

static void drawPlane(int px, int py, float hdg, uint16_t col) {
  float th = hdg * DEG_TO_RAD, c = cosf(th), s = sinf(th);
  int ax, ay, bx, by, cx2, cy2;
  // govde (2 px kalinlik)
  rot(-0.7f, -8, c, s, px, py, ax, ay); rot(-0.7f, 7, c, s, px, py, bx, by);
  spr.drawLine(ax, ay, bx, by, col);
  rot(0.7f, -8, c, s, px, py, ax, ay);  rot(0.7f, 7, c, s, px, py, bx, by);
  spr.drawLine(ax, ay, bx, by, col);
  // kanatlar
  rot(0, -2, c, s, px, py, ax, ay); rot(-8, 3, c, s, px, py, bx, by); rot(8, 3, c, s, px, py, cx2, cy2);
  spr.fillTriangle(ax, ay, bx, by, cx2, cy2, col);
  // kuyruk
  rot(0, 4, c, s, px, py, ax, ay); rot(-3.5f, 8, c, s, px, py, bx, by); rot(3.5f, 8, c, s, px, py, cx2, cy2);
  spr.fillTriangle(ax, ay, bx, by, cx2, cy2, col);
}

static void drawRadar() {
  if (!sprOk) return;

  // Veriyi kilit altinda kopyala
  static Aircraft local[MAX_AC];
  int n;
  uint32_t stamp;
  xSemaphoreTake(acMutex, portMAX_DELAY);
  n = acCount;
  memcpy(local, acList, sizeof(Aircraft) * n);
  stamp = acStamp;
  xSemaphoreGive(acMutex);

  spr.fillSprite(C_BG);

  // Halkalar ve eksenler
  for (int i = 1; i <= 3; i++) spr.drawCircle(CX, CY, RAD * i / 3, C_RING);
  spr.drawLine(CX - RAD, CY, CX + RAD, CY, C_AXIS);
  spr.drawLine(CX, CY - RAD, CX, CY + RAD, C_AXIS);
  spr.setTextFont(1);
  drawCentered("N", CX, 10, C_DIM);
  spr.fillCircle(CX, CY, 2, C_CENTER);

  // Irtifa renk skalasi (ucaklardan once cizilir, ucaklar ustte kalir)
  for (int x = 80; x < 160; x++) {
    spr.drawFastVLine(x, 212, 4, altColor((x - 80) * ALT_MAX_FT / 79));
  }
  spr.setTextFont(1);
  spr.setTextColor(C_DIM);
  spr.drawString("0", 70, 211);
  spr.drawString("40k", 164, 211);

  // Ucaklar (hiz/yone gore ara konum)
  float dt = (millis() - stamp) / 1000.0f;
  if (dt > 60) dt = 60;
  const float R = cfg.radiusKm;
  const float k = RAD / R;
  int shown = 0;
  for (int i = 0; i < n; i++) {
    float x = local[i].dx + local[i].vE * dt;
    float y = local[i].dy + local[i].vN * dt;
    if (x * x + y * y > R * R) continue;
    int px = CX + lroundf(x * k);
    int py = CY - lroundf(y * k);
    drawPlane(px, py, local[i].hdg, altColor(local[i].altFt));

    int tw = spr.textWidth(local[i].id);
    int lx = px;
    if (lx < tw / 2 + 4) lx = tw / 2 + 4;
    if (lx > 236 - tw / 2) lx = 236 - tw / 2;
    int ly = (py < 205) ? py + 15 : py - 15;
    drawCentered(local[i].id, lx, ly, C_LABEL);
    shown++;
  }

  // Alt bilgi
  char t[40];
  bool stale = (millis() - stamp) > 20000;
  if (stale) {
    snprintf(t, sizeof(t), "baglanti sorunu");
    drawCentered(t, CX, 228, C_ERR);
  } else {
    snprintf(t, sizeof(t), "%d ucak  %d km", shown, (int)lroundf(R));
    drawCentered(t, CX, 228, C_OK);
  }

  spr.pushSprite(0, 0);
}

static void renderFrame() {
  if (!sprOk) return;
  bool wifiOk = WiFi.status() == WL_CONNECTED;
  String L[6];
  uint16_t C[6];

  if (cfg.ssid.isEmpty() || !cfg.hasLoc) {
    L[0] = "UCAK RADAR";                 C[0] = C_CENTER;
    L[1] = "Ayar icin WiFi'ye baglan:";  C[1] = C_DIM;
    L[2] = AP_SSID;                      C[2] = C_PLANE;
    L[3] = String("Sifre: ") + AP_PASS;  C[3] = C_LABEL;
    L[4] = "Sayfa: 192.168.4.1";         C[4] = C_LABEL;
    drawScreen(L, C, 5);
    return;
  }
  if (!wifiOk) {
    L[0] = "UCAK RADAR";                 C[0] = C_CENTER;
    L[1] = "WiFi baglaniyor...";         C[1] = C_PLANE;
    L[2] = cfg.ssid;                     C[2] = C_LABEL;
    L[3] = "Ayar: " + String(AP_SSID);   C[3] = C_DIM;
    L[4] = "192.168.4.1";                C[4] = C_DIM;
    drawScreen(L, C, 5);
    return;
  }
  if (acStamp == 0) {
    L[0] = "UCAK RADAR";                 C[0] = C_CENTER;
    L[1] = "Veri bekleniyor...";         C[1] = C_PLANE;
    L[2] = WiFi.localIP().toString();    C[2] = C_LABEL;
    L[3] = "ucakradar.local";            C[3] = C_DIM;
    int nl = 4;
    if (netStatus[0]) { L[4] = netStatus; C[4] = C_ERR; nl = 5; }
    L[nl] = FW_VERSION; C[nl] = C_DIM; nl++;
    drawScreen(L, C, nl);
    return;
  }
  drawRadar();
}

// ======================= WiFi yonetimi =======================
static bool wifiTrying = false;
static bool wifiEverTried = false;
static uint32_t wifiTryStart = 0, wifiLastFail = 0;
static bool mdnsOn = false;

static void wifiManage() {
  if (cfg.ssid.isEmpty()) return;
  if (WiFi.status() == WL_CONNECTED) {
    wifiTrying = false;
    if (!mdnsOn) {
      if (MDNS.begin("ucakradar")) MDNS.addService("http", "tcp", 80);
      mdnsOn = true;
    }
    return;
  }
  uint32_t now = millis();
  if (!wifiTrying) {
    if (!wifiEverTried || now - wifiLastFail > 40000) {
      wifiEverTried = true;
      wifiTrying = true;
      wifiTryStart = now;
      WiFi.setAutoReconnect(true);
      WiFi.begin(cfg.ssid.c_str(), cfg.pass.isEmpty() ? nullptr : cfg.pass.c_str());
    }
  } else if (now - wifiTryStart > 20000) {
    // Basarisiz: taramayi durdur ki AP kararli kalsin, 40 sn sonra tekrar dene
    WiFi.setAutoReconnect(false);
    WiFi.disconnect();
    wifiTrying = false;
    wifiLastFail = now;
  }
}

// ======================= Web arayuzu =======================
static String htmlEscape(const String& s) {
  String o;
  o.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '&') o += "&amp;";
    else if (c == '<') o += "&lt;";
    else if (c == '>') o += "&gt;";
    else if (c == '"') o += "&quot;";
    else if (c == '\'') o += "&#39;";
    else o += c;
  }
  return o;
}

static const char PAGE_HEAD[] PROGMEM = R"HTML(<!doctype html><html lang="tr"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Uçak Radar</title>
<style>
body{margin:0;background:#0b1410;color:#d8f3e0;font:16px system-ui,sans-serif}
main{max-width:460px;margin:0 auto;padding:20px 16px 40px}
h1{font-size:22px;margin:0 0 4px;color:#7dffa8}
.st{font-size:13px;color:#8fb9a0;margin-bottom:18px;line-height:1.5}
label{display:block;margin:14px 0 4px;font-size:14px;color:#a9d6b8}
input,select{box-sizing:border-box;width:100%;padding:11px;border-radius:8px;border:1px solid #2a4a38;background:#10201a;color:#eafff0;font-size:16px}
.row{display:flex;gap:10px}.row>div{flex:1}
button{margin-top:10px;padding:12px 16px;border:0;border-radius:8px;background:#2fbf6b;color:#04140a;font-size:16px;font-weight:600}
button.sec{background:#1d3a2b;color:#bff0d0;font-size:14px;padding:8px 12px;margin-top:6px}
button.main{width:100%;margin-top:22px}
small{color:#7aa38a;display:block;margin-top:4px;font-size:12px}
.err{background:#4a1a1a;color:#ffd0d0;padding:10px;border-radius:8px;margin-bottom:14px}
</style></head><body><main>
<h1>✈ Uçak Radar</h1>
)HTML";

static void sendPage(const String& body) {
  String h;
  h.reserve(body.length() + 1500);
  h += FPSTR(PAGE_HEAD);
  h += body;
  h += F("</main></body></html>");
  server.send(200, "text/html; charset=utf-8", h);
}

static void handleRoot() {
  String b;
  b.reserve(3500);
  b += F("<div class=st>Ev ağı: ");
  if (WiFi.status() == WL_CONNECTED) {
    b += F("bağlı (");
    b += WiFi.localIP().toString();
    b += F(") &middot; ucakradar.local");
  } else if (cfg.ssid.length()) {
    b += F("bağlı değil");
  } else {
    b += F("henüz ayarlanmadı");
  }
  b += F("<br>Görünen uçak: ");
  b += String(acCount);
  b += F("</div>");

  b += F("<form method=post action=/save>");
  b += F("<label>Bulunan WiFi ağları</label><select id=sl onchange=sec(this)><option value=''>");
  b += scanCount ? F("— listeden bir ağ seçin —") : F("— ağ bulunamadı —");
  b += F("</option>");
  for (int i = 0; i < scanCount; i++) {
    b += F("<option>");
    b += htmlEscape(scanList[i]);
    b += F("</option>");
  }
  b += F("</select>");
  b += F("<button type=button class=sec onclick=yeni()>Ağları yeniden tara</button>"
         "<small id=tm>Yeniden tarama sırasında bağlantı birkaç saniye kopabilir.</small>");
  b += F("<label>Ev WiFi ağ adı</label><input name=ssid maxlength=32 required autocomplete=off value=\"");
  b += htmlEscape(cfg.ssid);
  b += F("\">");
  b += F("<label>WiFi şifresi</label><input name=pass type=password maxlength=63 autocomplete=off placeholder=\"");
  b += cfg.ssid.length() ? F("(değiştirmek için doldurun)") : F("şifre (açık ağsa boş bırakın)");
  b += F("\">");
  b += F("<div class=row><div><label>Enlem</label><input name=lat inputmode=decimal required placeholder=\"örn. 41.0082\" value=\"");
  if (cfg.hasLoc) b += String(cfg.lat, 5);
  b += F("\"></div><div><label>Boylam</label><input name=lon inputmode=decimal required placeholder=\"örn. 28.9784\" value=\"");
  if (cfg.hasLoc) b += String(cfg.lon, 5);
  b += F("\"></div></div>");
  b += F("<small>Haritada bir noktaya sağ tıklayıp enlem/boylamı kopyalayabilirsiniz (ondalık, nokta veya virgül).</small>");
  b += F("<label>Ekranda görünecek alanın yarıçapı (km)</label><input name=rad inputmode=decimal required value=\"");
  b += String(cfg.radiusKm, 0);
  b += F("\"><small>1 – 400 km. Küçük yarıçap daha ayrıntılı gösterir.</small>");
  b += F("<button class=main type=submit>Kaydet ve yeniden başlat</button></form>");
  b += F("<script>function sec(x){if(x.value)document.getElementsByName('ssid')[0].value=x.value}"
         "function yeni(){document.getElementById('tm').textContent="
         "'Taranıyor... bağlantı kopabilir. 12 sn sonra sayfa yenilenecek; açılmazsa WiFi ağına yeniden bağlanıp sayfayı yenileyin.';"
         "fetch('/rescan').catch(function(){});setTimeout(function(){location.reload()},12000)}</script>");
  sendPage(b);
}

static void handleRescan() {
  rescanAt = millis() + 800;   // tarama ana dongude, yanit gittikten sonra yapilir
  server.send(200, "text/plain", "ok");
}

static void handleSave() {
  String ssid = server.arg("ssid");
  ssid.trim();
  String pass = server.arg("pass");
  String latS = server.arg("lat"), lonS = server.arg("lon"), radS = server.arg("rad");
  latS.trim(); lonS.trim(); radS.trim();
  latS.replace(',', '.'); lonS.replace(',', '.'); radS.replace(',', '.');
  double lat = latS.toDouble(), lon = lonS.toDouble();
  float rad = radS.toFloat();

  String err;
  if (ssid.length() < 1 || ssid.length() > 32) err = "Ağ adı 1–32 karakter olmalı.";
  else if (latS.isEmpty() || lonS.isEmpty() || lat < -90 || lat > 90 || lon < -180 || lon > 180)
    err = "Enlem (-90…90) ve boylam (-180…180) geçersiz.";
  else if (!(rad >= 1 && rad <= 400)) err = "Yarıçap 1–400 km arasında olmalı.";
  else if (pass.length() > 0 && (pass.length() < 8 || pass.length() > 63))
    err = "WiFi şifresi 8–63 karakter olmalı.";

  if (err.length()) {
    String b = "<div class=err>" + htmlEscape(err) + "</div><p><a style=color:#7dffa8 href=/>← Geri dön</a></p>";
    sendPage(b);
    return;
  }

  // Sifre bos + ayni ag adi = eski sifreyi koru
  if (pass.isEmpty() && ssid == cfg.ssid) pass = cfg.pass;

  prefs.begin("radar", false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.putDouble("lat", lat);
  prefs.putDouble("lon", lon);
  prefs.putFloat("rad", rad);
  prefs.putBool("hasloc", true);
  prefs.end();

  sendPage(F("<h1 style=font-size:20px>✔ Kaydedildi</h1><p>Cihaz yeniden başlıyor. Birkaç saniye sonra "
             "ev ağınıza bağlanıp uçakları göstermeye başlayacak.</p>"));
  restartAt = millis() + 1500;
}

static void handleNotFound() {
  // Captive portal: bilinmeyen her istegi ana sayfaya yonlendir
  String loc = String("http://") + server.client().localIP().toString() + "/";
  server.sendHeader("Location", loc, true);
  server.send(302, "text/plain", "");
}

// ======================= setup / loop =======================
static void checkResetButton() {
  static uint32_t downSince = 0;
  if (digitalRead(RESET_PIN) == LOW) {
    if (!downSince) downSince = millis();
    if (millis() - downSince > 5000) {
      prefs.begin("radar", false);
      prefs.clear();
      prefs.end();
      delay(200);
      ESP.restart();
    }
  } else {
    downSince = 0;
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(RESET_PIN, INPUT_PULLUP);

  C_BG     = rgb(0, 0, 0);
  C_RING   = rgb(0, 110, 50);
  C_AXIS   = rgb(0, 70, 35);
  C_PLANE  = rgb(255, 210, 0);
  C_LABEL  = rgb(210, 255, 220);
  C_CENTER = rgb(0, 200, 255);
  C_DIM    = rgb(120, 160, 130);
  C_ERR    = rgb(255, 70, 60);
  C_OK     = rgb(90, 220, 120);

  loadConfig();
  initFilter();
  acMutex = xSemaphoreCreateMutex();

  Serial.println("1) ekran baslatiliyor");
  tft.init();
  Serial.println("2) ekran tamam");
  tft.setRotation(0);
  tft.fillScreen(0);
  spr.setColorDepth(8);
  sprOk = spr.createSprite(240, 240) != nullptr;
  Serial.println(sprOk ? "3) sprite tamam" : "3) Sprite olusturulamadi (bellek)");

  Serial.println("4) WiFi AP baslatiliyor");
  Serial.println("Surum: " FW_VERSION);
  Serial.printf("Reset nedeni: %d (1=guc, 3=yazilim, 4=cokme, 5/6=watchdog, 9=brownout)\n",
                (int)esp_reset_reason());
  WiFi.persistent(false);
  WiFi.setHostname("ucakradar");

  // Once (AP acilmadan) cevredeki aglari tara
  {
    String L[2] = {"UCAK RADAR", "WiFi aglari taraniyor..."};
    uint16_t C[2] = {C_CENTER, C_PLANE};
    drawScreen(L, C, 2);
  }
  WiFi.mode(WIFI_STA);
  delay(100);
  doScan();
  Serial.printf("Bulunan ag sayisi: %d\n", scanCount);

  WiFi.mode(WIFI_AP_STA);
  delay(100);
  // Bazi ESP32-C3 SuperMini kartlari yuksek verici gucunde calismaz: gucu dusur
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  bool apOk = WiFi.softAP(AP_SSID, AP_PASS, 1, 0, 4);   // kanal 1, gizli degil, 4 istemci
  delay(100);
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  WiFi.setSleep(false);
  Serial.printf("AP baslatildi: %s, ssid=%s, ip=%s\n", apOk ? "evet" : "HAYIR", AP_SSID,
                WiFi.softAPIP().toString().c_str());
  dns.start(53, "*", WiFi.softAPIP());

  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/rescan", HTTP_GET, handleRescan);
  server.onNotFound(handleNotFound);
  server.begin();

  xTaskCreate(netTask, "net", 16384, nullptr, 1, nullptr);
  Serial.println("Hazir. AP: UcakRadar");
}

void loop() {
  uint32_t t0 = millis();
  wifiManage();
  if (rescanAt && millis() > rescanAt) {
    rescanAt = 0;
    doScan();
  }
  renderFrame();
  while (millis() - t0 < FRAME_MS) {
    server.handleClient();
    dns.processNextRequest();
    delay(3);
  }
  checkResetButton();
  if (restartAt && millis() > restartAt) ESP.restart();
}
