/************************************************************
 * ESP8266 16x16 Matrix: Matrix-Rain + Webtext + Ticker (Sound-aktiv)
 * - mDNS: http://matrix.local/
 * - STA (DHCP) -> AP-Fallback
 * - Startet sofort mit Defaults
 * - Sound: analogRead(A0) > Threshold => Effekte laufen
 * - Einheitliche Orientierung via zentralem Flip in drawPixel()
 ************************************************************/
#include <Arduino.h>
#include <FastLED.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>

// ====== LED / Matrix ======
#define WIDTH              16
#define HEIGHT             16
#define NUM_LEDS           (WIDTH*HEIGHT)
#define DATA_PIN           D5
#define LED_TYPE           WS2812B
#define COLOR_ORDER        GRB
#define BRIGHTNESS_DEFAULT 64
#define POWER_LIMIT_MW     2000
#define MATRIX_SERPENTINE  1   // 1 = serpentin verdrahtet, 0 = jede Zeile gleiche Richtung

// Globale Ausrichtungs-Flip (falls Text/Matrix gespiegelt/gedreht erscheint)
#define MATRIX_FLIP_X      0   // 1 = horizontal spiegeln
#define MATRIX_FLIP_Y      0   // 1 = vertikal spiegeln

CRGB leds[NUM_LEDS];

// ====== WLAN ======
const char* WIFI_SSID = "Wilma2001_Ext";
const char* WIFI_PASS = "14D12k82";   // ggf. anpassen

// AP-Fallback
const char* AP_SSID = "MatrixText-AP";
const char* AP_PASS = "12345678";

ESP8266WebServer server(80);

// ====== UI-State (Defaults laufen sofort) ======
String  uiText = "HELLO";
uint8_t uiMode = 0;           // 0=Rain, 1=Ticker, 2=Overlay
uint8_t uiBrightness = BRIGHTNESS_DEFAULT;
uint16_t uiSpeed = 40;        // ms/frame
bool    uiRainbow = false;
CRGB    uiColor = CRGB(0,255,60);

// ====== Sound ======
int  soundThreshold  = 200;   // 0..1023
bool soundActiveOnly = true;  // Effekte nur bei Sound?

// ====== Mapping ======
uint16_t XY(uint8_t x, uint8_t y) {
  if (x >= WIDTH || y >= HEIGHT) return 0;
  if (MATRIX_SERPENTINE) {
    if (y & 0x01) return y*WIDTH + (WIDTH-1-x);
    else          return y*WIDTH + x;
  } else {
    return y*WIDTH + x;
  }
}

inline uint8_t mapX(uint8_t x){ return MATRIX_FLIP_X ? (WIDTH  - 1 - x) : x; }
inline uint8_t mapY(uint8_t y){ return MATRIX_FLIP_Y ? (HEIGHT - 1 - y) : y; }

// ====== 5x7-Font (ASCII 32..127) ======
const uint8_t font5x7[] PROGMEM = {
  // 32..127, 5 Bytes pro Zeichen
  0x00,0x00,0x00,0x00,0x00, 0x00,0x00,0x5F,0x00,0x00, 0x00,0x07,0x00,0x07,0x00,
  0x14,0x7F,0x14,0x7F,0x14, 0x24,0x2A,0x7F,0x2A,0x12, 0x23,0x13,0x08,0x64,0x62,
  0x36,0x49,0x55,0x22,0x50, 0x00,0x05,0x03,0x00,0x00, 0x00,0x1C,0x22,0x41,0x00,
  0x00,0x41,0x22,0x1C,0x00, 0x14,0x08,0x3E,0x08,0x14, 0x08,0x08,0x3E,0x08,0x08,
  0x00,0x50,0x30,0x00,0x00, 0x08,0x08,0x08,0x08,0x08, 0x00,0x60,0x60,0x00,0x00,
  0x20,0x10,0x08,0x04,0x02, 0x3E,0x51,0x49,0x45,0x3E, 0x00,0x42,0x7F,0x40,0x00,
  0x42,0x61,0x51,0x49,0x46, 0x21,0x41,0x45,0x4B,0x31, 0x18,0x14,0x12,0x7F,0x10,
  0x27,0x45,0x45,0x45,0x39, 0x3C,0x4A,0x49,0x49,0x30, 0x01,0x71,0x09,0x05,0x03,
  0x36,0x49,0x49,0x49,0x36, 0x06,0x49,0x49,0x29,0x1E, 0x00,0x36,0x36,0x00,0x00,
  0x00,0x56,0x36,0x00,0x00, 0x08,0x14,0x22,0x41,0x00, 0x14,0x14,0x14,0x14,0x14,
  0x00,0x41,0x22,0x14,0x08, 0x02,0x01,0x51,0x09,0x06, 0x32,0x49,0x79,0x41,0x3E,
  0x7E,0x11,0x11,0x11,0x7E, 0x7F,0x49,0x49,0x49,0x36, 0x3E,0x41,0x41,0x41,0x22,
  0x7F,0x41,0x41,0x22,0x1C, 0x7F,0x49,0x49,0x49,0x41, 0x7F,0x09,0x09,0x09,0x01,
  0x3E,0x41,0x49,0x49,0x7A, 0x7F,0x08,0x08,0x08,0x7F, 0x00,0x41,0x7F,0x41,0x00,
  0x20,0x40,0x41,0x3F,0x01, 0x7F,0x08,0x14,0x22,0x41, 0x7F,0x40,0x40,0x40,0x40,
  0x7F,0x02,0x04,0x02,0x7F, 0x7F,0x04,0x08,0x10,0x7F, 0x3E,0x41,0x41,0x41,0x3E,
  0x7F,0x09,0x09,0x09,0x06, 0x3E,0x41,0x51,0x21,0x5E, 0x7F,0x09,0x19,0x29,0x46,
  0x46,0x49,0x49,0x49,0x31, 0x01,0x01,0x7F,0x01,0x01, 0x3F,0x40,0x40,0x40,0x3F,
  0x1F,0x20,0x40,0x20,0x1F, 0x7F,0x20,0x10,0x20,0x7F, 0x63,0x14,0x08,0x14,0x63,
  0x07,0x08,0x70,0x08,0x07, 0x61,0x51,0x49,0x45,0x43, 0x00,0x7F,0x41,0x41,0x00,
  0x02,0x04,0x08,0x10,0x20, 0x00,0x41,0x41,0x7F,0x00, 0x04,0x02,0x01,0x02,0x04,
  0x40,0x40,0x40,0x40,0x40, 0x00,0x01,0x02,0x00,0x00, 0x20,0x54,0x54,0x54,0x78,
  0x7F,0x48,0x44,0x44,0x38, 0x38,0x44,0x44,0x44,0x20, 0x38,0x44,0x44,0x48,0x7F,
  0x38,0x54,0x54,0x54,0x18, 0x08,0x7E,0x09,0x01,0x02, 0x0C,0x52,0x52,0x52,0x3E,
  0x7F,0x08,0x04,0x04,0x78, 0x00,0x44,0x7D,0x40,0x00, 0x20,0x40,0x44,0x3D,0x00,
  0x7F,0x10,0x28,0x44,0x00, 0x00,0x41,0x7F,0x40,0x00, 0x7C,0x04,0x18,0x04,0x78,
  0x7C,0x08,0x04,0x04,0x78, 0x38,0x44,0x44,0x44,0x38, 0x7C,0x14,0x14,0x14,0x08,
  0x08,0x14,0x14,0x18,0x7C, 0x7C,0x08,0x04,0x04,0x08, 0x48,0x54,0x54,0x54,0x20,
  0x04,0x3F,0x44,0x40,0x20, 0x3C,0x40,0x40,0x20,0x7C, 0x1C,0x20,0x40,0x20,0x1C,
  0x3C,0x40,0x30,0x40,0x3C, 0x44,0x28,0x10,0x28,0x44, 0x0C,0x50,0x50,0x50,0x3C,
  0x44,0x64,0x54,0x4C,0x44, 0x00,0x08,0x36,0x41,0x00, 0x00,0x00,0x7F,0x00,0x00,
  0x00,0x41,0x36,0x08,0x00, 0x08,0x04,0x08,0x10,0x08, 0x00,0x00,0x00,0x00,0x00
};

uint8_t glyphCol(char c, uint8_t col) {
  if (c < 32 || c > 127) c = 32;
  uint16_t idx = (c - 32) * 5 + col;
  return pgm_read_byte(&font5x7[idx]);
}
int textPixelWidth(const String& s){ return s.length() ? s.length()*6 : 0; }

void clearMatrix(){ fill_solid(leds, NUM_LEDS, CRGB::Black); }

void drawPixel(int x,int y,const CRGB& c){
  if (x<0||y<0||x>=WIDTH||y>=HEIGHT) return;
  leds[XY(mapX(x), mapY(y))] = c;  // zentrale Orientierung
}

void drawChar5x7(int x,int y,char c,const CRGB& col){
  for(uint8_t cx=0; cx<5; cx++){
    uint8_t bits = glyphCol(c, cx);
    for(uint8_t cy=0; cy<7; cy++) if (bits & (1<<cy)) drawPixel(x+cx, y+cy, col);
  }
}
void drawTextAt(int x,int y,const String& s,const CRGB& col){
  int cursor=x;
  for(uint16_t i=0;i<s.length();i++){ drawChar5x7(cursor, y, s[i], col); cursor += 6; }
}

// ====== Effekte ======
int scrollX = WIDTH;

void effectTicker(){
  clearMatrix();
  CRGB col = uiRainbow ? CHSV((millis()/16)%255,255,255) : uiColor;
  drawTextAt(scrollX, 4, uiText, col);
  FastLED.show();
  scrollX--;
  if (scrollX < -textPixelWidth(uiText)) scrollX = WIDTH;
}

void effectRainBase(bool overlayText){
  // shift down + neue Tropfen
  for (int x=0;x<WIDTH;x++){
    for (int y=HEIGHT-1;y>0;y--) leds[XY(mapX(x),mapY(y))] = leds[XY(mapX(x),mapY(y-1))];
    leds[XY(mapX(x),mapY(0))] = (random8() < 50)
      ? (uiRainbow ? CHSV((millis()/8 + x*8)%255,255,255) : uiColor)
      : CRGB::Black;
  }
  for (int i=0;i<NUM_LEDS;i++) leds[i].nscale8(200);
  if (overlayText && uiText.length()){
    CRGB col = uiRainbow ? CHSV((millis()/16)%255,255,255) : uiColor;
    int startX = (WIDTH - min(WIDTH, textPixelWidth(uiText))) / 2;
    drawTextAt(startX, 4, uiText, col);
  }
  FastLED.show();
}
void effectRain(){ effectRainBase(false); }
void effectOverlay(){ effectRainBase(true); }

// ====== Web UI ======
const char* PAGE =
"<!DOCTYPE html><html><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<style>body{font-family:sans-serif;max-width:540px;margin:24px auto;padding:0 12px}"
"label{display:block;margin-top:12px;font-weight:600}"
"input[type=text]{width:100%;padding:8px;font-size:16px}"
".row{display:flex;gap:12px;align-items:center}.row>*{flex:1}"
"button{margin-top:16px;padding:10px 14px;font-size:16px;cursor:pointer}"
"fieldset{border:1px solid #ccc;border-radius:8px;padding:12px;margin-top:16px}"
"</style></head><body>"
"<h2>16×16 Matrix – Text & Matrix</h2>"
"<form method='POST' action='/set'>"
"<label>Text</label><input name='text' type='text' maxlength='64' placeholder='Dein Text'>"
"<fieldset><legend>Modus</legend>"
"<div><label><input type='radio' name='mode' value='0'> Matrix</label></div>"
"<div><label><input type='radio' name='mode' value='1'> Ticker</label></div>"
"<div><label><input type='radio' name='mode' value='2'> Overlay</label></div>"
"</fieldset>"
"<div class='row'>"
"<div><label>Farbe</label><input name='color' type='color' value='#00ff3c'></div>"
"<div><label>Helligkeit</label><input name='bright' type='range' min='1' max='255' value='64' oninput='bval.value=this.value'><output id='bval'>64</output></div>"
"</div>"
"<div class='row'>"
"<div><label>Speed (ms/frame)</label><input name='speed' type='number' min='5' max='200' value='40'></div>"
"<div><label>Regenbogen</label><input name='rainbow' type='checkbox'></div>"
"</div>"
"<fieldset><legend>Sound Control</legend>"
"<div class='row'>"
"<div><label>Threshold</label><input name='threshold' type='range' min='0' max='1023' value='200' oninput='tval.value=this.value'><output id='tval'>200</output></div>"
"<div><label>Nur bei Sound</label><input name='soundonly' type='checkbox' checked></div>"
"</div>"
"</fieldset>"
"<button type='submit'>Übernehmen</button>"
"</form>"
"<p style='margin-top:18px;font-size:14px;color:#555'>Aufruf im LAN: <code>http://matrix.local/</code></p>"
"<p style='margin-top:6px;font-size:13px;color:#666'>AP-Fallback: <code>http://192.168.4.1/</code> (SSID: MatrixText-AP)</p>"
"<p style='margin-top:18px;font-size:14px;color:#555'>Aktuell: <span id='state'></span></p>"
"<script>"
"async function refresh(){try{const r=await fetch('/state');const j=await r.json();"
"document.getElementById('state').textContent=`Mode ${j.mode} | Bright ${j.bright} | Speed ${j.speed}ms | RGB(${j.r},${j.g},${j.b}) | Text: \\\"${j.text}\\\" | Thresh: ${j.thresh} | SoundOnly: ${j.soundonly}`;}"
"catch(e){}} setInterval(refresh,1000); refresh();"
"</script>"
"</body></html>";

uint8_t hex2byte(const String& s){ char b[3]={s[0],s[1],0}; return (uint8_t)strtoul(b,nullptr,16); }
void handleRoot(){ server.send(200,"text/html",PAGE); }
void handleState(){
  String j="{";
  j+="\"mode\":"+String(uiMode)+",";
  j+="\"bright\":"+String(uiBrightness)+",";
  j+="\"speed\":"+String(uiSpeed)+",";
  j+="\"r\":"+String(uiColor.r)+",";
  j+="\"g\":"+String(uiColor.g)+",";
  j+="\"b\":"+String(uiColor.b)+",";
  j+="\"thresh\":"+String(soundThreshold)+",";
  j+="\"soundonly\":"+(String)(soundActiveOnly?"true":"false")+",";
  String safe=uiText; safe.replace("\"","\\\"");
  j+="\"text\":\""+safe+"\"}";
  server.send(200,"application/json",j);
}
void handleSet(){
  if (server.hasArg("text")){ uiText=server.arg("text"); uiText.trim(); if(!uiText.length()) uiText="HELLO"; scrollX=WIDTH; }
  if (server.hasArg("mode")){ uiMode=(uint8_t)constrain(server.arg("mode").toInt(),0,2); }
  if (server.hasArg("bright")){ uiBrightness=(uint8_t)constrain(server.arg("bright").toInt(),1,255); FastLED.setBrightness(uiBrightness); }
  if (server.hasArg("speed")){ uiSpeed=(uint16_t)constrain(server.arg("speed").toInt(),5,200); }
  if (server.hasArg("threshold")){ soundThreshold = constrain(server.arg("threshold").toInt(),0,1023); }
  soundActiveOnly = server.hasArg("soundonly");
  uiRainbow = server.hasArg("rainbow");
  if (server.hasArg("color")){
    String hex=server.arg("color"); // #RRGGBB
    if (hex.length()==7 && hex[0]=='#'){
      uint8_t r=hex2byte(hex.substring(1,3));
      uint8_t g=hex2byte(hex.substring(3,5));
      uint8_t b=hex2byte(hex.substring(5,7));
      uiColor = CRGB(r,g,b);
    }
  }
  server.sendHeader("Location","/");
  server.send(302,"text/plain","OK");
}

// ====== WLAN ======
bool startSTA(){
  WiFi.mode(WIFI_STA);
  WiFi.hostname("matrix"); // kompatibel für ESP8266 Core
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.printf("Verbinde zu %s", WIFI_SSID);
  uint32_t t0=millis();
  while (WiFi.status()!=WL_CONNECTED && millis()-t0<15000){
    delay(300); Serial.print(".");
  }
  Serial.println();
  return WiFi.status()==WL_CONNECTED;
}

void startAP(){
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  IPAddress ip = WiFi.softAPIP();
  Serial.printf("AP aktiv: %s  IP: %s\n", AP_SSID, ip.toString().c_str());
}

// ====== Setup / Loop ======
uint32_t lastFrameTs=0;

void setup(){
  delay(50);
  FastLED.addLeds<LED_TYPE, DATA_PIN, COLOR_ORDER>(leds, NUM_LEDS);
  FastLED.setBrightness(uiBrightness);
  FastLED.setMaxPowerInMilliWatts(POWER_LIMIT_MW);
  clearMatrix(); FastLED.show();

  Serial.begin(115200);
  Serial.println("\nBooting...");

  bool sta = startSTA();
  if (!sta){
    Serial.println("STA fehlgeschlagen -> AP-Fallback");
    startAP();
  } else {
    Serial.print("Verbunden. IP: "); Serial.println(WiFi.localIP());
    if (MDNS.begin("matrix")) {              // matrix.local
      MDNS.addService("http", "tcp", 80);
      Serial.println("mDNS aktiv: http://matrix.local/");
    } else {
      Serial.println("mDNS Start fehlgeschlagen");
    }
  }

  server.on("/", HTTP_GET, handleRoot);
  server.on("/state", HTTP_GET, handleState);
  server.on("/set", HTTP_POST, handleSet);
  server.begin();

  random16_add_entropy(analogRead(A0)); // bisschen Entropie via A0
  lastFrameTs = millis(); // direkt loslegen
}

void loop(){
  server.handleClient();
  MDNS.update();

  uint32_t now=millis();
  if (now - lastFrameTs < uiSpeed) return;
  lastFrameTs = now;

  bool doEffect = true;
  if (soundActiveOnly){
    int micVal = analogRead(A0);   // 0..1023 (0..1.0V)
    doEffect = (micVal >= soundThreshold);
  }

  if (doEffect){
    switch (uiMode){
      case 0: effectRain(); break;
      case 1: effectTicker(); break;
      case 2: effectOverlay(); break;
    }
  } else {
    clearMatrix();
    FastLED.show();
  }
}
