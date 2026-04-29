3D Case comming soon!!!!

# Onkyo TX-SR 806 — ESP32 RS232 Bridge

ESP32-basierte RS232-Bridge für den Onkyo TX-SR 806 AV-Receiver. Verbindet einen AVR ohne Netzwerkanschluss über RS232 mit dem Heimnetzwerk — steuerbar per WebGUI und OpenHAB.

---

## Features

- **WebGUI** (kein Login, kein Passwort)
  - Power ON / OFF
  - Eingangsquelle wählen (CD, BD/DVD, CBL/SAT, Tuner, …)
  - Lautstärke-Slider (Wert 0–80, identisch mit AVR-Display)
  - Status-Anzeige mit IP-Adresse
- **WiFi-Setup** direkt im Browser — kein Flashen für WLAN-Wechsel
- **OpenHAB-Integration** via eISCP-Bridge (TCP Port 60128)  
  - Bidirektional: Statusänderungen am Gerät kommen zurück zu OpenHAB
  - Kompatibel mit dem offiziellen [Onkyo Binding](https://www.openhab.org/addons/bindings/onkyo/)
- **mDNS**: erreichbar unter `http://onkyo-bridge.local`
- Keine externen Libraries nötig

---

## Hardware

| Bauteil | Bemerkung |
|---------|-----------|
| ESP32 DevKit (ESP32-WROOM-32) | Jedes Standard-Board |
| MAX3232 Modul | RS232 ↔ 3.3V TTL Pegelwandler |
| DB9-Kabel (female) | Zum RS232C-Port des AVR |
| Jumperkabel | ESP32 ↔ MAX3232 |

### Verdrahtung

```
ESP32 GPIO17 (TX) ──→ MAX3232 T1IN → T1OUT ──→ DB9 Pin 2 (AVR RXD)
ESP32 GPIO16 (RX) ←── MAX3232 R1OUT ← R1IN ←── DB9 Pin 3 (AVR TXD)
ESP32 GND         ──→ MAX3232 GND  ←────────── DB9 Pin 5 (GND)
ESP32 3.3V        ──→ MAX3232 VCC
```

> **Wichtig:** MAX3232 mit **3.3V** betreiben, nicht 5V. Keine Hardware-Handshake-Leitungen nötig.

RS232-Parameter: 9600 Baud, 8N1, kein Handshake.

---

## Installation

### 1. Arduino IDE vorbereiten

- Arduino IDE 2.x installieren
- ESP32 Board-Support hinzufügen:  
  `https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json`
- Board: **ESP32 Dev Module**

### 2. Flashen

`firmware/onkyo_bridge/onkyo_bridge.ino` öffnen und auf den ESP32 flashen.  
Keine externen Libraries erforderlich.

### 3. WiFi einrichten

1. ESP32 startet als offener Hotspot: **`Onkyo-Bridge`**
2. Mit Handy oder PC verbinden
3. Browser: `http://192.168.4.1`
4. Netzwerk scannen → SSID und Passwort eingeben → Verbinden
5. ESP32 startet neu und ist danach unter seiner IP erreichbar

### 4. WebGUI verwenden

```
http://onkyo-bridge.local
```
oder direkt per IP-Adresse (z.B. `http://192.168.1.50`).

---

## OpenHAB-Integration

1. **Onkyo Binding** installieren (Add-on Store)
2. Thing anlegen (Main UI oder Datei):

```
Thing onkyo:onkyoAVR:avr "Onkyo TX-SR 806" [
    ipAddress = "192.168.1.50",
    port      = 60128
]
```

3. Items verlinken (Power, Volume, Mute, Input)

Der ESP32 erscheint für OpenHAB wie ein normaler Netzwerk-AVR.  
Statusänderungen am Gerät (Fernbedienung, Frontplatte) werden automatisch zurückgemeldet.

---

## Projektstruktur

```
Onkyo_ESP32_Bridge/
├── firmware/
│   └── onkyo_bridge/
│       └── onkyo_bridge.ino
├── openhab/
│   ├── onkyo.things
│   └── onkyo.items
├── HARDWARE.md
├── SETUP.md
└── README.md
```

---

## ISCP-Protokoll

Der ESP32 übersetzt zwischen zwei Protokollvarianten:

| | RS232 (ISCP) | Ethernet (eISCP) |
|---|---|---|
| Format | `!1CMD\r` | 16-Byte Header + ISCP |
| Port | UART 9600 Baud | TCP 60128 |
| Richtung | ESP32 ↔ AVR | OpenHAB ↔ ESP32 |

---

## Lizenz

MIT
