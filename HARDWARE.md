# Hardware — ESP32 RS232 Bridge für Onkyo TX-SR 806

## Benötigte Teile

| Bauteil | Bemerkung |
|---------|-----------|
| ESP32 DevKit (z.B. ESP32-WROOM-32) | Jedes Standard-Board |
| MAX3232 Modul | RS232 ↔ 3.3V TTL Pegelwandler |
| DB9-Buchse (female, Kabel) | Zum Onkyo RS232-Port |
| Jumperkabel | Verbindung ESP32 ↔ MAX3232 |

## Verdrahtung

```
ESP32               MAX3232           Onkyo TX-SR 806
─────               ───────           ───────────────
GPIO17 (TX)  ──→    T1IN     T1OUT ──→  DB9 Pin 2 (RXD)
GPIO16 (RX)  ←──    R1OUT    R1IN  ←──  DB9 Pin 3 (TXD)
GND          ──→    GND              DB9 Pin 5 (GND)
3.3V         ──→    VCC

DB9 Pin 5 (GND) ←── GND (gemeinsame Masse!)
```

### DB9-Pinbelegung (Buchse, female, seitens ESP32-Kabel)

```
      ┌──────────────┐
      │  1   2   3   │
      │    4   5     │
      │  6   7   8   │
      │      9       │
      └──────────────┘
Pin 2 = RXD  (Eingang AVR, kommt von ESP32 TX über MAX3232)
Pin 3 = TXD  (Ausgang AVR, geht zu ESP32 RX über MAX3232)
Pin 5 = GND
```

## RS232-Parameter Onkyo TX-SR 806

| Parameter | Wert |
|-----------|------|
| Baudrate  | 9600 |
| Datenbits | 8    |
| Parität   | None |
| Stoppbits | 1    |
| Handshake | None |

## Firmware-Pins (anpassbar in onkyo_bridge.ino)

```cpp
#define RS232_RX_PIN  16   // ESP32 empfängt von AVR
#define RS232_TX_PIN  17   // ESP32 sendet an AVR
```

## Wichtig

- MAX3232 braucht **3.3V**, NICHT 5V (sonst ESP32-Schaden)
- Masse-Verbindung zwischen ESP32, MAX3232 und Onkyo GND ist Pflicht
- Der TX-SR 806 hat einen normalen DB9-Stecker auf der Rückseite (RS232C)
- Der AVR sendet beim Einschalten einen langen Binary-Burst — das ist normal und wird von der Firmware automatisch verworfen
- Kein Hardware-Handshake (RTS/CTS) nötig — nur 3 Leitungen: TX, RX, GND
