/************************************************************
 * SEENGREAT 64x64 HUB75 (P3) + Raspberry Pi Pico 2W
 * - Adafruit_Protomatter (Pinout gemäß SEENGREAT-Dokument)
 * - DHCP (kein mDNS), zeigt beim Start die zugewiesene IP auf dem Panel
 * - Weboberfläche: Text, Farbe (#RRGGBB), Speed, Helligkeit, Y-Position
 * Tested with: Earle Philhower Raspberry Pi Pico core
 ************************************************************/
#include <Arduino.h>
#include <WiFi.h>        // Pico W WiFi (Earle core)
#include <WebServer.h>   // Generic WebServer (mit dem Core mitgeliefert)
#include <Adafruit_Protomatter.h>
#include <Adafruit_GFX.h>

// ====== Panel / Pins laut SEENGREAT-Dokument (Pico 2W) ======
/* R1,G1,B1 -> GP2,GP3,GP4 | R2,G2,B2 -> GP5,GP8,GP9
   A,B,C,D,E -> GP10,GP16,GP18,GP20,GP22
   CLK->GP11, LAT->GP12, OE->GP13  */
uint8_t rgbPins[6]  = { 2, 3, 4, 5, 8, 9 };
uint8_t addrPins[5] = { 10, 16, 18, 20, 22 };
uint8_t clockPin = 11, latchPin = 12; int8_t oePin = 13;
// width=64, bitDepth=4 (gute Balance), doubleBuffer=true
Adafruit_Protomatter matrix(64, 6, 1, rgbPins, 5, addrPins,
                            clockPin, latchPin, oePin, true, 4);

// ====== WLAN (nur STA/DHCP) ======
// Trage hier dein WLAN ein:
const char* WIFI_SSID = "SSID";
const char* WIFI_PASS = "14D12k82";

// ====== Webserver ======
WebServer server(80);

// ====== Anzeige-Config ======
struct Config {
  String text = "HELLO P3 64x64 // PICO 2W";
  uint8_t r = 255, g = 200, b = 0;  // Default Gelb-Orange
  uint8_t brightness = 160;         // 0..255 (wird auf 5-6 Bit gemappt)
  uint8_t y = 36;                   // Cursor-Y (mittig für 8px Font)
  uint8_t speed = 2;                // 1..8 (Pixel/Step)
} cfg;

static inline uint8_t scale8(uint8_t v, uint8_t s){
  return (uint16_t(v) * s + 127) / 255;
}
static inline uint16_t C(uint8_t r, uint8_t g, uint8_t b){
  return matrix.color565(scale8(r, cfg.brightness),
                         scale8(g, cfg.brightness),
                         scale8(b, cfg.brightness));
}

// ====== Scroll-State ======
int scrollX = 64;
unsigned long lastStepMs = 0;
const uint16_t STEP_INTERVAL_MS = 16; // Grundtakt, tatsächl. Pixelrate via cfg.speed

// ====== Helpers ======
String ipToString(IPAddress ip) {
  return String(ip[0]) + "." + ip[1] + "." + ip[2] + "." + ip[3];
}

bool parseHexColor(const String &s, uint8_t &r, uint8_t &g, uint8_t &b){
  // erwartet "#RRGGBB" oder "RRGGBB"
  String t = s;
  if (t.length() == 7 && t[0] == '#') t = t.substring(1);
  if (t.length() != 6) return false;
  char *endptr = nullptr;
  uint32_t val = strtoul(t.c_str(), &endptr, 16);
  if (endptr == t.c_str() || *endptr != '\0') return false;
  r = (val >> 16) & 0xFF;
  g = (val >> 8)  & 0xFF;
  b = (val)       & 0xFF;
  return true;
}

void drawScroll(){
  matrix.fillScreen(0);
  matrix.setCursor(scrollX, cfg.y);
  matrix.setTextColor(C(cfg.r, cfg.g, cfg.b));
  matrix.setTextWrap(false);
  matrix.setTextSize(1);
  matrix.print(cfg.text);
  matrix.show();
}

// ====== HTML UI ======
const char* HTML_PAGE =
"<!DOCTYPE html><html><head><meta charset='utf-8'/>"
"<meta name='viewport' content='width=device-width,initial-scale=1'/>"
"<title>SEENGREAT 64x64</title>"
"<style>body{font-family:sans-serif;max-width:680px;margin:24px auto;padding:0 12px}"
"label{display:block;margin:10px 0 4px}input,button{font-size:16px;padding:8px;width:100%}"
".row{display:grid;grid-template-columns:1fr 1fr;gap:12px}"
".ip{margin:12px 0;padding:8px;background:#eee;border-radius:8px}"
"</style></head><body>"
"<h1>SEENGREAT 64×64 – Laufschrift</h1>"
"<div class='ip'>IP: %IP%</div>"
"<form method='POST' action='/set'>"
"<label>Text</label><input name='text' value='%TEXT%' maxlength='200'>"
"<div class='row'>"
"<div><label>Farbe (#RRGGBB)</label><input name='color' value='%COLOR%'></div>"
"<div><label>Helligkeit (0–255)</label><input name='brightness' type='number' min='0' max='255' value='%BRI%'></div>"
"</div>"
"<div class='row'>"
"<div><label>Speed (1–8)</label><input name='speed' type='number' min='1' max='8' value='%SPD%'></div>"
"<div><label>Y-Position (0–63)</label><input name='y' type='number' min='0' max='63' value='%Y%'></div>"
"</div>"
"<button type='submit'>Übernehmen</button>"
"</form>"
"<p><a href='/clear'>Text löschen</a> &nbsp;|&nbsp; <a href='/reboot'>Neustart</a></p>"
"</body></html>";

String renderIndex(){
  String s(HTML_PAGE);
  s.replace("%IP%", ipToString(WiFi.localIP()));
  s.replace("%TEXT%", cfg.text);
  char col[8]; snprintf(col, sizeof(col), "#%02X%02X%02X", cfg.r, cfg.g, cfg.b);
  s.replace("%COLOR%", String(col));
  s.replace("%BRI%", String(cfg.brightness));
  s.replace("%SPD%", String(cfg.speed));
  s.replace("%Y%",   String(cfg.y));
  return s;
}

void handleRoot(){
  server.send(200, "text/html; charset=utf-8", renderIndex());
}

void handleSet(){
  bool ok = true;
  if (server.hasArg("text")){
    String t = server.arg("text");
    t.trim();
    if (t.length() == 0) t = " "; // nie leerer String
    cfg.text = t;
    // Cursor zurücksetzen, damit es sofort sichtbar wird
    scrollX = 64;
  }
  if (server.hasArg("color")){
    uint8_t r,g,b;
    if (parseHexColor(server.arg("color"), r, g, b)){
      cfg.r = r; cfg.g = g; cfg.b = b;
    } else ok = false;
  }
  if (server.hasArg("brightness")){
    int v = server.arg("brightness").toInt();
    v = constrain(v, 0, 255);
    cfg.brightness = (uint8_t)v;
  }
  if (server.hasArg("speed")){
    int v = server.arg("speed").toInt();
    v = constrain(v, 1, 8);
    cfg.speed = (uint8_t)v;
  }
  if (server.hasArg("y")){
    int v = server.arg("y").toInt();
    v = constrain(v, 0, 63);
    cfg.y = (uint8_t)v;
  }
  server.sendHeader("Location", "/", true);
  server.send(303);
}

void handleClear(){
  cfg.text = " ";
  scrollX = 64;
  server.sendHeader("Location", "/", true);
  server.send(303);
}

void handleReboot(){
  server.send(200, "text/plain", "Rebooting...");
  delay(300);
  NVIC_SystemReset();
}

void handleNotFound(){
  server.send(404, "text/plain", "Not found");
}

// ====== Setup ======
void setup(){
  if (matrix.begin() != PROTOMATTER_OK) {
    for(;;); // hard error: Panel init
  }
  matrix.setTextWrap(false);
  matrix.setTextSize(1);

  // WLAN verbinden (DHCP)
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  // Wartefenster mit animierter Punkteanzeige, IP nach Erfolg auf das Panel
  unsigned long t0 = millis();
  String ipShown = "";
  for (;;){
    wl_status_t st = WiFi.status();
    if (st == WL_CONNECTED) {
      ipShown = ipToString(WiFi.localIP());
      break;
    }
    // kleiner Warte-Spinner auf dem Panel
    matrix.fillScreen(0);
    matrix.setCursor(2, 18);
    matrix.setTextColor(C(160,160,160));
    matrix.print("Verbinde WLAN...");
    int dots = ((millis()/400)%4);
    matrix.setCursor(2, 36);
    matrix.print(String("SSID: ") + WIFI_SSID);
    matrix.setCursor(2, 50);
    matrix.print(String("...") + String('.', dots));
    matrix.show();
    delay(80);
    if (millis() - t0 > 20000) { // 20s Timeout -> weiter versuchen, aber starten
      break;
    }
  }

  // IP kurz anzeigen (falls vorhanden)
  if (WiFi.status() == WL_CONNECTED) {
    String ip = ipToString(WiFi.localIP());
    matrix.fillScreen(0);
    matrix.setCursor(2, 26);
    matrix.setTextColor(C(0,255,120));
    matrix.print("IP:");
    matrix.setCursor(2, 42);
    matrix.setTextColor(C(255,255,255));
    matrix.print(ip);
    matrix.show();
    delay(2000);
  }

  // Webserver Routen
  server.on("/", HTTP_GET, handleRoot);
  server.on("/set", HTTP_POST, handleSet);
  server.on("/clear", HTTP_GET, handleClear);
  server.on("/reboot", HTTP_GET, handleReboot);
  server.onNotFound(handleNotFound);
  server.begin();

  // Start Laufschrift
  scrollX = 64;
}

// ====== Loop ======
void loop(){
  server.handleClient();

  // nicht blockierend scrollen
  unsigned long now = millis();
  if (now - lastStepMs >= (STEP_INTERVAL_MS)) {
    lastStepMs = now;

    drawScroll();
    // mehrere Pixel pro Tick für Speed
    for (uint8_t i = 0; i < cfg.speed; i++){
      scrollX -= 1;
    }

    // Breite grob schätzen: 6px pro Zeichen bei TextSize=1 (5+1), min 64
    int16_t textW = max(64, (int16_t)cfg.text.length() * 6);
    if (scrollX < -textW) scrollX = 64;
  }
}
