# Uçak Radar – ESP32-C3 + GC9A01

Ayarladığın konum ve yarıçaptaki uçakları yuvarlak ekranda radar gibi gösterir.
Veri: [airplanes.live](https://airplanes.live) (yedek: adsb.lol) – ücretsiz, anahtar gerekmez.

## 1) Bağlantı (ESP32-C3 SuperMini)

| GC9A01 | ESP32-C3 |
|--------|----------|
| SCL    | GPIO4    |
| SDA    | GPIO6    |
| CS     | GPIO7    |
| DC     | GPIO1    |
| RST    | GPIO0    |
| VCC/GND| 3V3/GND  |

Farklı pin istersen `platformio.ini` içindeki `-DTFT_*` değerlerini değiştir.

## 2) `.bin` dosyasını elde etme

**A) GitHub ile (kurulum gerektirmez):** Bu klasörü yeni bir GitHub deposuna yükle →
**Actions** sekmesi → son çalışma → **Artifacts** → `ucak-radar-firmware` → içinden `firmware_merged_esp32c3_vspi.bin`.

**B) Kendi bilgisayarında:**
```
pip install platformio
pio run
```
Çıktı proje klasöründe: `firmware_merged_esp32c3_vspi.bin` ve `firmware_merged_esp32c3_hspi.bin`
(önce `vspi` olanı dene; çöküyorsa ya da ekran boşsa `hspi` olanı dene).

## 3) ESP32-C3'e yükleme (tek dosya, 0x0 adresine)

* **Tarayıcıdan:** Chrome/Edge ile <https://web.esphome.io> → cihazı USB ile bağla → *Connect* → *Install* → `firmware_merged_esp32c3_vspi.bin` seç.
* **Komut satırı:** `esptool.py --chip esp32c3 --port COM5 write_flash 0x0 firmware_merged_esp32c3_vspi.bin`

Port görünmezse BOOT tuşuna basılı tutarak USB'yi tak.

## 4) Kullanım

1. Cihaza güç ver → **UcakRadar** WiFi ağı yayınlanır (şifre: `radar1234`).
2. Bilgisayardan bu ağa bağlan → ayar sayfası kendiliğinden açılır (açılmazsa `http://192.168.4.1`).
3. Ev WiFi adı/şifresi, enlem, boylam ve yarıçap (km) gir → **Kaydet**.
4. Cihaz yeniden başlar, ev ağına bağlanır, uçakları gösterir. Ayarı değiştirmek için
   yine **UcakRadar** ağına bağlan (AP her zaman açık) ya da ev ağından `http://ucakradar.local`.
5. BOOT tuşuna 5 sn basılı tutmak tüm ayarları siler.

## Notlar

* Ekran 2 Hz yenilenir; veri 5 sn'de bir çekilir, aradaki karelerde uçaklar hız ve yöne göre ilerletilir.
* Yerdeki uçaklar gösterilmez. En fazla 60 uçak çizilir.
* Renkler ters/yanlışsa `platformio.ini` içindeki `TFT_INVERSION_ON` veya `TFT_RGB_ORDER` satırını aç.
* HTTPS sertifikası doğrulanmaz (`setInsecure`); sadece herkese açık uçuş verisi çekilir, şifre gönderilmez.
