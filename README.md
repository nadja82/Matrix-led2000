FUNKTHERMO-TIMER 1.0 
Overview 
This Arduino sketch is designed for the ESP8266 (e.g., NodeMCU, Wemos D1 mini) as a wireless temperature & humidity receiver with display
and web configuration. 
It combines: - Wireless data reception (via /update endpoint) - TM1637 4-digit 7-segment display (temperature & humidity alternating) - Web
interface for settings - EEPROM persistence for configuration - Acoustic alarm (buzzer) on threshold violations - mDNS support → access via
http://envrx.local/ 
 Features 
Endpoints: 
GET / → Status page + link to settings
GET /api/last → JSON response:
json { "t": 23.5, "h": 40.2, "age_ms": 1234, "from": "192.168.1.50" } 
GET /update?token=XYZ&t=25.4&h=60.1 → accepts new measurement values (with token authentication)
GET /settings → HTML configuration form
POST /save → saves settings to EEPROM
Display: 
Shows temperature & humidity in alternation
Format: "25:50" = 25.5 °C
Configurable: always on or periodically (X seconds every N minutes)
Alarm: 
Buzzer sounds if thresholds are exceeded
Configurable cooldown & hysteresis
Alarm can be enabled/disabled via settings
Robust design: 
Handles NaN values gracefully (displays placeholder)
Clean re-initialization after saving or rebooting 
 Hardware Requirements 
ESP8266 (NodeMCU / Wemos D1 mini recommended)
TM1637 4-digit display (CLK + DIO pins configurable in code)
Buzzer for alarm output
Stable Wi-Fi network 
 Software Dependencies 
Install via Arduino IDE Library Manager: - ESP8266WiFi - ESP8266WebServer - ESP8266mDNS - TM1637Display - EEPROM 
 Configuration 
In the sketch, set your Wi-Fi credentials: cpp const char* WIFI_SSID = "YourSSID"; const char* WIFI_PASS = "YourPassword"; 
Optionally configure: - TOKEN for /update endpoint - Display mode (always on / periodic) - Alarm thresholds, cooldown, hysteresis 
 Usage 
Flash the sketch to your ESP8266.
Connect the TM1637 display and buzzer as defined in the code.On boot: 
Device connects to Wi-Fi via DHCP
mDNS available: http://envrx.local/
Status and IP are shown in the web interface
Access /settings to configure thresholds, display timing, and other options.
Use /update endpoint from a sender device (e.g., weather station) to push new values. 
 Troubleshooting 
No Wi-Fi connection → Check SSID & password, ensure DHCP is enabled.
mDNS not working → Access via IP instead of envrx.local.
Display off → Check TM1637 wiring (CLK/DIO), verify display mode settings.
Buzzer always on → Adjust alarm thresholds or hysteresis.
Settings not saved → Ensure EEPROM.commit() is present (included in code). 
 License / Credits 
Built for ESP8266 + TM1637 modules
Uses Arduino core libraries and TM1637Display
License: Add your preferred license (MIT recommended)





German:

Matrix-led2000 — led2002.ino
============================

Kurzbeschreibung
----------------
Sketch für **SEENGREAT 64×64 HUB75 (P3)** mit **Raspberry Pi Pico 2W**. 
Zeigt eine Laufschrift auf dem Panel und bietet eine einfache **Weboberfläche**
zur Lauftext‑Konfiguration (Text, Farbe `#RRGGBB`, Helligkeit, Speed, Y‑Position).
Bezieht die IP per **DHCP** (kein mDNS) und zeigt die IP beim Start auf dem Panel an.

Getestete Umgebung
------------------
- Arduino Core: **Earle Philhower Raspberry Pi Pico** (RP2040)  
- Board: **Raspberry Pi Pico 2W**
- Bibliotheken:
  - `Adafruit_Protomatter`
  - `Adafruit_GFX`
  - (im Core enthalten) `WiFi.h`, `WebServer.h`

Hardware / Pinbelegung (laut Code)
----------------------------------
Panel: SEENGREAT HUB75 64×64 (P3)  
Controller: Raspberry Pi Pico 2W

```
R1,G1,B1  -> GP2,  GP3,  GP4
R2,G2,B2  -> GP5,  GP8,  GP9
A,B,C,D,E -> GP10, GP16, GP18, GP20, GP22
CLK -> GP11
LAT -> GP12
OE  -> GP13
```

Protomatter-Setup (Auszug):
```
Breite 64, 6 Farb-Pins, 5 Adress-Pins, doubleBuffer = true, BitDepth = 4
Adafruit_Protomatter matrix(64, 6, 1, rgbPins, 5, addrPins, clockPin, latchPin, oePin, true, 4);
```

Konfiguration
-------------
Im Sketch WLAN‑Zugangsdaten setzen:
```
const char* WIFI_SSID = "DEIN_SSID";
const char* WIFI_PASS = "DEIN_PASSWORT";
```

Bedienung (Web UI)
------------------
- Nach dem Booten zeigt das Panel eine kleine Warteanimation und anschließend die **IP‑Adresse**.
- Browser öffnen und **http://<IP>** aufrufen.
- Formularfelder:
  - **Text** (max. 200 Zeichen)
  - **Farbe**: `#RRGGBB`
  - **Helligkeit**: 0–255 (intern auf 5–6 Bit skaliert)
  - **Speed**: 1–8 (Pixel pro Schritt)
  - **Y‑Position**: 0–63 (Textzeilen‑Höhe; Standardfont 8 px)
- Zusatz‑Links:
  - **/clear** – Text löschen
  - **/reboot** – Neustart

Funktionen / Endpunkte
----------------------
- `GET /` → HTML‑Konfiguration (zeigt aktuelle Werte & IP)
- `POST /set` → übernimmt Text, Farbe, Helligkeit, Speed, Y
- `GET /clear` → setzt Text auf Leerzeichen und resetet Scroll‑Position
- `GET /reboot` → Neustart des Pico 2W
- 404 → Text „Not found“

Anzeigelogik (Kurzüberblick)
----------------------------
- Font: Adafruit GFX (5×7, TextSize=1; Wrap=off)
- Scrollvariablen:
  - `scrollX` startet bei 64 und wird pro Takt `cfg.speed` Pixel dekrementiert
  - Grundtakt: `STEP_INTERVAL_MS = 16 ms`
  - Textbreite grob: `max(64, len(Text) * 6)` → bei Ende wieder auf 64 setzen
- Helligkeitsskalierung: `scale8()`
- Farbkonvertierung: `matrix.color565()` über Helfer `C(r,g,b)`
- Farbeingabe: Parser akzeptiert `#RRGGBB` oder `RRGGBB`

Bekannte Einschränkungen
------------------------
- **Nur STA/DHCP**, kein mDNS.
- **Keine Persistenz**: Einstellungen werden nicht im Flash/EEPROM gespeichert und gehen nach Neustart verloren.
- Zeichensatz: Standard‑GFX‑Font (ASCII‑fokussiert). Umlaute/Sonderzeichen erscheinen ggf. nicht korrekt.
- Der HTML‑Farbeingabewert muss 6‑stelliges Hex sein.

Build‑Hinweise
--------------
1. Arduino IDE: Board **Raspberry Pi Pico 2W** (Earle Philhower Core) auswählen.
2. Bibliotheken **Adafruit_Protomatter** und **Adafruit_GFX** über den Bibliotheksverwalter installieren.
3. Sketch öffnen, WLAN‑Daten eintragen, kompilieren und flashen.
4. Panel anschließen wie oben angegeben, Stromversorgung sicherstellen.
5. Nach dem Start IP am Panel ablesen, im Browser öffnen.

Troubleshooting
---------------
- **Panel bleibt schwarz** → Pinbelegung prüfen, Protomatter‑Initialisierung (`matrix.begin()`) gibt sonst Hard‑Error (Endlosschleife).
- **Keine IP** → SSID/Passwort korrekt? Access‑Point‑Reichweite? DHCP am Router aktiv?
- **Flackern/Artefakte** → Helligkeit verringern (`brightness`), stabile Versorgung und saubere Leitungsführung sicherstellen.
- **Farbe falsch** → `#RRGGBB` exakt 6 Hex‑Ziffern verwenden.

Lizenz / Credits
----------------
- Code basiert auf Arduino + Adafruit Protomatter/GFX. Lizenz bitte im Repository ergänzen.
- Panel: SEENGREAT 64×64 HUB75 (P3).

English (quick summary)
-----------------------
Pico 2W + SEENGREAT 64×64 HUB75 scroller with a tiny web UI to set text, color (`#RRGGBB`), brightness, speed and Y offset. 
Wi‑Fi STA via DHCP, shows the assigned IP on the panel at boot. Endpoints: `/` (UI), `/set` (POST), `/clear`, `/reboot`. 
Pins: R1,G1,B1→GP2,3,4; R2,G2,B2→GP5,8,9; A..E→GP10,16,18,20,22; CLK→11; LAT→12; OE→13. Uses Adafruit_Protomatter + GFX.
No persistence (settings reset on reboot); ASCII font.
