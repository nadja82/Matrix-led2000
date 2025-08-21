Matrix-led2000 — led2002.ino 
Overview 
This project is a sketch for the SEENGREAT 64×64 HUB75 (P3) LED panel driven by a Raspberry Pi Pico 2W.
It displays scrolling text on the panel and provides a simple web interface to configure: 
Text (up to 200 characters)
Color (#RRGGBB)
Brightness
Scroll speed
Y-position (vertical offset) 
The Pico connects to Wi-Fi using DHCP (no mDNS) and shows its assigned IP on the panel at startup. 
 Tested Environment 
Arduino Core: Earle Philhower Raspberry Pi Pico (RP2040)
Board: Raspberry Pi Pico 2W
Libraries: 
Adafruit_Protomatter
Adafruit_GFX
(included in core) WiFi.h, WebServer.h 
 Hardware Setup 
Panel: SEENGREAT HUB75 64×64 (P3)
Controller: Raspberry Pi Pico 2W 
Pin Mapping 
R1,G1,B1 -> GP2, GP3, GP4
R2,G2,B2 -> GP5, GP8, GP9
A,B,C,D,E -> GP10, GP16, GP18, GP20, GP22
CLK -> GP11
LAT -> GP12
OE -> GP13

Protomatter Configuration 
Adafruit_Protomatter matrix(
64, 6, 1, rgbPins,
5, addrPins,
clockPin, latchPin, oePin,
true, 4
);
 Width: 64
Color pins: 6
Address pins: 5
Double buffering: enabled
Bit depth: 4 
 Configuration 
Set your Wi-Fi credentials inside the sketch: cpp const char* WIFI_SSID = "YOUR_SSID"; const char* WIFI_PASS =
"YOUR_PASSWORD"; 
 Web InterfaceAfter boot, the panel shows a short animation, then the IP address.
Open a browser and visit http://<IP>. 
Form Fields 
Text: max 200 characters
Color: #RRGGBB
Brightness: 0–255 (scaled internally to 5–6 bit)
Speed: 1–8 (pixels per step)
Y-Position: 0–63 (text line height; default font 8 px) 
Extra Links 
/clear → clear text
/reboot → restart the device 
 API Endpoints 
GET / → Web UI (shows current values & IP)
POST /set → Apply text, color, brightness, speed, Y-position
GET /clear → Clear text and reset scroll position
GET /reboot → Restart Pico 2W
404 → Returns “Not found” 
 Display Logic 
Font: Adafruit GFX (5×7, TextSize=1; Wrap=off)
Scroll behavior: 
scrollX starts at 64 and decreases by cfg.speed pixels per tick
Tick interval: 16 ms
Text width: max(64, len(Text) * 6) → resets to 64 at end
Brightness scaling: scale8()
Color conversion: matrix.color565() via helper C(r,g,b)
Color input: accepts #RRGGBB or RRGGBB 
 Known Limitations 
Only Wi-Fi STA/DHCP, no mDNS
No persistence: settings are not stored in flash/EEPROM and reset after reboot
Default ASCII font: special characters/umlauts may not display correctly
Color value must be 6-digit hex 
 Build Instructions 
In Arduino IDE, select board Raspberry Pi Pico 2W (Earle Philhower Core).
Install libraries Adafruit_Protomatter and Adafruit_GFX via Library Manager.
Open the sketch, enter Wi-Fi credentials, compile and upload.
Connect the panel as described above and ensure stable power supply.
After startup, read the IP from the panel and open it in your browser. 
 Troubleshooting 
Panel stays black → check pin mapping; failed matrix.begin() will hard-error into endless loop
No IP → check SSID/password, AP range, DHCP enabled on router
Flickering/artifacts → lower brightness, use stable power and proper wiring
Wrong colors → ensure color code is exactly #RRGGBB (6 hex digits) 
License / Credits 
Based on Arduino + Adafruit Protomatter/GFX
Panel: SEENGREAT 64×64 HUB75 (P3)
Add license file in repository if needed
