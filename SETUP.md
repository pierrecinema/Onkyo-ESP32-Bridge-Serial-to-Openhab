# Setup-Anleitung — Onkyo ESP32 Bridge

## 1. Arduino IDE vorbereiten

1. Arduino IDE installieren (2.x empfohlen)
2. **ESP32 Board-Support** hinzufügen:
   - Preferences → Additional Board Manager URLs:
     `https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json`
   - Tools → Board Manager → "esp32" installieren
3. Board wählen: **ESP32 Dev Module**
4. Keine externen Libraries nötig (alles im ESP32-Core enthalten)

## 2. Firmware flashen

1. `firmware/onkyo_bridge/onkyo_bridge.ino` öffnen
2. Board und Port auswählen
3. Upload

## 3. WiFi einrichten (Erststart)

1. ESP32 startet als WLAN-Hotspot: **`Onkyo-Bridge`** (kein Passwort)
2. Mit Handy/PC verbinden
3. Browser: **http://192.168.4.1**
4. „Netzwerke scannen" → SSID auswählen → Passwort eingeben → Verbinden
5. Nach Verbindung: IP-Adresse notieren (z.B. `192.168.1.50`)
6. ESP32 ist jetzt erreichbar unter:
   - `http://192.168.1.50` (IP)
   - `http://onkyo-bridge.local` (mDNS, falls dein Router/OS das unterstützt)

## 4. OpenHAB Onkyo Binding konfigurieren

1. **Binding installieren:**
   - OpenHAB Main UI → Add-on Store → „Onkyo Binding" installieren

2. **Things-Datei anpassen:**
   - `openhab/onkyo.things` öffnen
   - IP-Adresse des ESP32 eintragen:
     ```
     ipAddress = "192.168.1.50"
     ```
   - Datei nach `/etc/openhab/things/onkyo.things` kopieren (oder via UI erstellen)

3. **Items und Sitemap:**
   - `openhab/onkyo.items` → `/etc/openhab/items/onkyo.items`
   - `openhab/onkyo.sitemap` → `/etc/openhab/sitemaps/onkyo.sitemap`

4. **OpenHAB neu starten** oder Konfiguration neu laden

## 5. Testen

### WebGUI ESP32
- http://onkyo-bridge.local aufrufen (oder IP direkt)
- ON / OFF Buttons testen
- Eingang wählen
- Lautstärke-Slider bewegen (Wert 0–80, identisch mit AVR-Display)
- Status-Zeile zeigt "Verbunden · IP" wenn alles läuft

### OpenHAB
- Main UI → Things → Onkyo TX-SR 806 → Status sollte ONLINE sein
- Items testen: Power, Volume, Mute
- OpenHAB kommuniziert direkt über eISCP (Port 60128) — bidirektional

### Serielle Debug-Ausgabe (optional)
- Arduino IDE Serial Monitor (115200 baud)
- `[RS232 →]` = ESP32 sendet an AVR
- `[RS232 ←]` = ESP32 empfängt von AVR

## 6. Lautstärke

Slider zeigt den AVR-Wert direkt (0–80), identisch mit dem Display am Gerät.
Maximum anpassen falls nötig:
```cpp
#define VOL_MAX_HEX  80   // in onkyo_bridge.ino
```

## 7. WiFi erneut ändern

- http://onkyo-bridge.local/wifi aufrufen
- oder ESP32 neu flashen (NVS bleibt erhalten — WiFi-Daten überleben Flash-Vorgänge)

## Projektstruktur

```
Onkyo_ESP32_Bridge/
├── firmware/
│   └── onkyo_bridge/
│       └── onkyo_bridge.ino   ← ESP32 Firmware
├── openhab/
│   ├── onkyo.things           ← OpenHAB Thing (IP anpassen!)
│   └── onkyo.items            ← OpenHAB Items
├── HARDWARE.md                ← Verdrahtung MAX3232
└── SETUP.md                   ← Diese Datei
```
