/****************************************************
 * 16x16 WS2812B – 3 Effekte + Web UI + mDNS + Sound
 * WLAN-Logik:
 *  1) Versuche bevorzugtes WLAN (PREFERRED_SSID/PASS)
 *  2) Fallback WiFiManager Captive Portal: "LED-16x16_Setup"
 *  3) Notfall-Hotspot (AP): SSID "LED-16x16", PASS "12345678"
 *
 * mDNS: http://led-16x16.local
 * Sound-Sensor (A0) zuschaltbar in der Weboberfläche
 ****************************************************/

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <DNSServer.h>
#include <ESP8266mDNS.h>
#include <FastLED.h>
#include <WiFiManager.h> // von tzapu

// ========= Benutzer: bevorzugtes WLAN eintragen =========
#define PREFERRED_SSID "SSID"
#define PREFERRED_PASS "PWD"   // <--- HIER setzen
// ========================================================

// ===== LED/Matrix =====
#define DATA_PIN    D5
#define LED_TYPE    WS2812B
#define COLOR_ORDER GRB
#define MATRIX_W    16
#define MATRIX_H    16
#define NUM_LEDS    (MATRIX_W * MATRIX_H)
CRGB leds[NUM_LEDS];

// ===== Web/Misc =====
ESP8266WebServer server(80);
const char* MDNS_NAME = "led-16x16";

// ===== Effekte/Settings =====
enum Effect : uint8_t { E_PLASMA=0, E_MATRIX=1, E_CRT=2 };

struct Settings {
  uint8_t effect     = E_PLASMA;
  uint8_t brightness = 140;  // 5..255
  uint8_t speed      = 110;  // 0..255
  uint8_t density    = 120;  // Matrix
  uint8_t hueShift   = 0;    // Plasma
  // Sound
  bool    soundEnabled   = false;
  uint8_t soundSensitivity = 140; // 0..255
  uint8_t soundMode        = 0;   // 0=Peaks, 1=Reactive
} S;

uint32_t lastFrame = 0;

// ===== XY-Mapping (serpentin) =====
inline uint16_t XY(uint8_t x, uint8_t y) {
  if (x >= MATRIX_W || y >= MATRIX_H) return 0;
  return (y & 1) ? (y * MATRIX_W + (MATRIX_W - 1 - x))
                 : (y * MATRIX_W + x);
}

// ======= Sound (A0) =======
uint16_t soundBaseline = 512;
uint16_t soundAmp = 0;
bool     soundPeak = false;
uint32_t lastSoundMs = 0;

uint16_t readSound() {
  const uint8_t N = 6;
  uint32_t sum = 0;
  for (uint8_t i=0; i<N; i++) sum += analogRead(A0);
  uint16_t raw = sum / N; // 0..1023

  uint8_t emaAlpha = map(S.soundSensitivity, 0,255, 1, 12);
  soundBaseline = ((uint32_t)(255-emaAlpha) * soundBaseline + (uint32_t)emaAlpha * raw) / 255;

  uint16_t amp = (raw > soundBaseline) ? (raw - soundBaseline) : (soundBaseline - raw);
  if (amp > 1023) amp = 1023;

  uint16_t thr = map(255 - S.soundSensitivity, 0,255, 20, 300);
  bool isPeak = (amp > thr);

  uint32_t now = millis();
  if (isPeak && now - lastSoundMs > 50) {
    soundPeak = true;
    lastSoundMs = now;
  } else {
    soundPeak = false;
  }

  soundAmp = amp;
  return amp;
}

// ====== Effekt 1: Plasma-Tunnel ======
void drawPlasma(uint32_t t) {
  float soundFactor = 0.0f;
  if (S.soundEnabled) {
    if (S.soundMode == 1) soundFactor = min(1.0f, soundAmp / 400.0f);
    else if (soundPeak)   S.hueShift += 8; // Peak-Farbsprung
  }

  float time = t * (0.001f + (S.speed + soundFactor*60.0f) * 0.00001f);
  const float cx = (MATRIX_W - 1) / 2.0f;
  const float cy = (MATRIX_H - 1) / 2.0f;

  for (uint8_t y=0; y<MATRIX_H; y++) {
    for (uint8_t x=0; x<MATRIX_W; x++) {
      float dx = (x - cx), dy = (y - cy);
      float dist = sqrtf(dx*dx + dy*dy);
      float angle = atan2f(dy, dx);

      float v = 0.0f;
      v += sinf(dist * 0.90f - time * 2.0f);
      v += sinf((angle * 3.0f) + time * 1.7f);
      v += sinf((dx * 0.8f + dy * 0.8f) * 0.7f + time);

      uint8_t val = (uint8_t) constrain((v * 42.5f) + 128.0f, 0.0f, 255.0f);
      uint8_t hue = S.hueShift + val + (uint8_t)(soundFactor * 30.0f);
      leds[XY(x,y)] = CHSV(hue, 255, 255);
    }
  }
}

// ====== Effekt 2: Matrix-Rain ======
struct Drop {
  int16_t y;
  uint8_t len;
  uint8_t speed;
  bool alive;
};
Drop drops[MATRIX_W];

void spawnDrop(uint8_t col, uint8_t extraLen=0) {
  drops[col].y = - (int8_t) random8(2, 6);
  drops[col].len = max<uint8_t>(2, (S.density >> 5) + random8(1,4) + extraLen);
  drops[col].speed = max<uint8_t>(1, (S.speed >> 6));
  drops[col].alive = true;
}
void initMatrix() { for (uint8_t x=0; x<MATRIX_W; x++) drops[x].alive = false; }

void drawMatrix(uint32_t t) {
  for (uint16_t i=0; i<NUM_LEDS; i++) leds[i].nscale8_video(180);

  uint8_t spawnBoost = 0;
  if (S.soundEnabled) {
    if (S.soundMode == 0 && soundPeak) spawnBoost = 2;
    else if (S.soundMode == 1)         spawnBoost = soundAmp > 200 ? 1 : 0;
  }

  for (uint8_t x=0; x<MATRIX_W; x++) {
    if (!drops[x].alive) {
      uint8_t chance = S.density;
      if (random8() < chance/10) spawnDrop(x, spawnBoost);
    }
  }

  static uint32_t lastStep = 0;
  uint16_t interval = 30 + (255 - S.speed);
  if (S.soundEnabled && S.soundMode == 1) interval = max<uint16_t>(10, interval - soundAmp/10);

  if (t - lastStep >= interval) {
    lastStep = t;
    for (uint8_t x=0; x<MATRIX_W; x++) {
      if (!drops[x].alive) continue;
      drops[x].y += drops[x].speed;
      for (uint8_t k=0; k<drops[x].len; k++) {
        int16_t yy = drops[x].y - k;
        if (yy >= 0 && yy < MATRIX_H) {
          uint8_t v = 255 - k * (220 / max<uint8_t>(1, drops[x].len));
          uint8_t sat = (k==0) ? 100 : 240;
          CRGB c = CHSV(100, sat, v);
          if (S.soundEnabled && S.soundMode == 1 && k==0) {
            c.fadeLightBy( map(constrain(1023 - soundAmp,0,1023), 0,1023, 10,120) );
          }
          leds[XY(x, yy)] = c;
        }
      }
      if (drops[x].y - drops[x].len > MATRIX_H) drops[x].alive = false;
    }
  }
}

// ====== Effekt 3: CRT/Scanline ======
uint8_t vignette(uint8_t x, uint8_t y) {
  uint8_t vx = min(x, (uint8_t)(MATRIX_W-1 - x));
  uint8_t vy = min(y, (uint8_t)(MATRIX_H-1 - y));
  uint8_t v = min(vx, vy); // 0..7
  return 200 + v*6;        // 200..242
}
void drawCRT(uint32_t t) {
  float rollPos = fmodf((t * (0.02f + S.speed * 0.0004f)), (float)MATRIX_H);
  uint8_t extra = 0;
  if (S.soundEnabled) {
    if (S.soundMode == 0 && soundPeak) extra = 60;
    else if (S.soundMode == 1)          extra = map(min<uint16_t>(soundAmp,400), 0,400, 0,40);
  }

  for (uint8_t y=0; y<MATRIX_H; y++) {
    for (uint8_t x=0; x<MATRIX_W; x++) {
      uint8_t base = 40;
      uint8_t sl = (y & 1) ? 20 : 0;
      float d = fabsf((float)y - rollPos);
      uint8_t roll = (d < 1.0f) ? 150 : (d < 2.0f ? 60 : 0);

      uint8_t v = base + roll + extra;
      v = (uint8_t) constrain(v - sl + random8(0, 16), 0, 255);

      CRGB c = CHSV(160, 30, v);
      uint8_t vig = vignette(x,y);
      c.r = (uint16_t)c.r * vig / 255;
      c.g = (uint16_t)c.g * vig / 255;
      c.b = (uint16_t)c.b * vig / 255;
      leds[XY(x,y)] = c;
    }
  }
}

// ========= Webserver =========
const char* INDEX_HTML PROGMEM = R"HTML(
<!doctype html><html><head>
<meta name="viewport" content="width=device-width, initial-scale=1" />
<title>16x16 LED Controller</title>
<style>
body{font-family:system-ui,-apple-system,Segoe UI,Roboto,Ubuntu,sans-serif;margin:20px}
.card{max-width:560px;margin:auto;padding:16px;border:1px solid #ddd;border-radius:16px;box-shadow:0 6px 24px rgba(0,0,0,.08)}
h1{font-size:20px;margin:0 0 12px}
label{display:block;margin-top:12px;font-size:14px;color:#333}
select,input[type=range]{width:100%}
.row{display:flex;gap:12px}.row>div{flex:1}
.val{font-variant-numeric:tabular-nums}
.kv{font-size:12px;color:#666;margin-top:8px}
button{margin-top:16px;width:100%;padding:10px 14px;border-radius:10px;border:0;background:#111;color:#fff;font-weight:600}
small{color:#666}
.toggle{display:flex;align-items:center;gap:8px;margin-top:12px}
</style></head><body>
<div class="card">
  <h1>16×16 LED Controller</h1>

  <div class="kv" id="netinfo">Loading network…</div>

  <label>Effect
    <select id="effect">
      <option value="0">Plasma Tunnel</option>
      <option value="1">Matrix Rain</option>
      <option value="2">CRT / Scanline</option>
    </select>
  </label>

  <div class="row">
    <div><label>Brightness: <span id="bv" class="val"></span></label>
      <input type="range" id="brightness" min="5" max="255" step="1"/></div>
    <div><label>Speed: <span id="sv" class="val"></span></label>
      <input type="range" id="speed" min="0" max="255" step="1"/></div>
  </div>

  <div class="row">
    <div><label>Density (Matrix): <span id="dv" class="val"></span></label>
      <input type="range" id="density" min="0" max="255" step="1"/></div>
    <div><label>Hue Shift (Plasma): <span id="hv" class="val"></span></label>
      <input type="range" id="hueShift" min="0" max="255" step="1"/></div>
  </div>

  <div class="toggle">
    <input type="checkbox" id="soundEnabled">
    <label for="soundEnabled">Sound Reaktiv (A0)</label>
  </div>
  <div class="row">
    <div><label>Sound Sensitivity: <span id="ssv" class="val"></span></label>
      <input type="range" id="soundSensitivity" min="0" max="255" step="1"/></div>
    <div><label>Sound Mode</label>
      <select id="soundMode">
        <option value="0">Peaks (Impulse)</option>
        <option value="1">Reactive (kontinuierlich)</option>
      </select>
    </div>
  </div>

  <button id="apply">Apply</button>
  <small id="state"></small>
</div>

<script>
async function loadState(){
  const r = await fetch('/state'); const s = await r.json();
  effect.value = s.effect; brightness.value = s.brightness; speed.value = s.speed;
  density.value = s.density; hueShift.value = s.hueShift;
  soundEnabled.checked = s.soundEnabled;
  soundSensitivity.value = s.soundSensitivity;
  soundMode.value = s.soundMode;
  bv.textContent=brightness.value; sv.textContent=speed.value; dv.textContent=density.value; hv.textContent=hueShift.value; ssv.textContent=soundSensitivity.value;
  state.textContent = 'Connected: ' + s.hostname + ' @ ' + s.ip + ' (mDNS: '+ s.mdns +')';
  netinfo.textContent = 'IP: ' + s.ip + ' | Hostname: ' + s.hostname + ' | mDNS: ' + s.mdns;
}
[brightness,speed,density,hueShift,soundSensitivity].forEach(el=>el.addEventListener('input',()=>{
  bv.textContent=brightness.value; sv.textContent=speed.value; dv.textContent=density.value; hv.textContent=hueShift.value; ssv.textContent=soundSensitivity.value;
}));
apply.onclick = async ()=>{
  const params = new URLSearchParams({
    effect: effect.value,
    brightness: brightness.value,
    speed: speed.value,
    density: density.value,
    hueShift: hueShift.value,
    soundEnabled: soundEnabled.checked ? 1 : 0,
    soundSensitivity: soundSensitivity.value,
    soundMode: soundMode.value
  });
  await fetch('/set?' + params.toString());
  loadState();
};
loadState();
</script>
</body></html>
)HTML";

String jsonState() {
  IPAddress ip = WiFi.isConnected() ? WiFi.localIP() : WiFi.softAPIP();
  String ipStr = ip.toString();
  char buf[300];
  snprintf(buf, sizeof(buf),
    "{\"effect\":%u,\"brightness\":%u,\"speed\":%u,\"density\":%u,\"hueShift\":%u,"
    "\"soundEnabled\":%u,\"soundSensitivity\":%u,\"soundMode\":%u,"
    "\"ip\":\"%s\",\"hostname\":\"%s\",\"mdns\":\"%s.local\"}",
    S.effect, S.brightness, S.speed, S.density, S.hueShift,
    (unsigned)S.soundEnabled, S.soundSensitivity, S.soundMode,
    ipStr.c_str(), WiFi.hostname().c_str(), MDNS_NAME
  );
  return String(buf);
}

void handleRoot()   { server.send_P(200, "text/html; charset=utf-8", INDEX_HTML); }
void handleState()  { server.send(200, "application/json", jsonState()); }

void handleSet() {
  if (server.hasArg("effect"))          S.effect          = (uint8_t) constrain(server.arg("effect").toInt(), 0, 2);
  if (server.hasArg("brightness"))      S.brightness      = (uint8_t) constrain(server.arg("brightness").toInt(), 5, 255);
  if (server.hasArg("speed"))           S.speed           = (uint8_t) constrain(server.arg("speed").toInt(), 0, 255);
  if (server.hasArg("density"))         S.density         = (uint8_t) constrain(server.arg("density").toInt(), 0, 255);
  if (server.hasArg("hueShift"))        S.hueShift        = (uint8_t) constrain(server.arg("hueShift").toInt(), 0, 255);
  if (server.hasArg("soundEnabled"))    S.soundEnabled    = (server.arg("soundEnabled").toInt() == 1);
  if (server.hasArg("soundSensitivity"))S.soundSensitivity= (uint8_t) constrain(server.arg("soundSensitivity").toInt(), 0, 255);
  if (server.hasArg("soundMode"))       S.soundMode       = (uint8_t) constrain(server.arg("soundMode").toInt(), 0, 1);

  if (S.effect == E_MATRIX) initMatrix();
  FastLED.setBrightness(S.brightness);
  server.send(200, "application/json", jsonState());
}

// ===== WiFi / mDNS =====
void setupWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.hostname(MDNS_NAME);

  // 1) Bevorzugtes WLAN versuchen (DHCP)
  WiFi.begin(PREFERRED_SSID, PREFERRED_PASS);
  Serial.println(F("[WiFi] Connecting to preferred SSID..."));
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
    delay(250);
    Serial.print('.');
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print(F("\n[WiFi] Connected to "));
    Serial.print(PREFERRED_SSID);
    Serial.print(F(" | IP: "));
    Serial.println(WiFi.localIP());
  } else {
    // 2) Fallback: WiFiManager (Captive Portal)
    Serial.println(F("\n[WiFi] Preferred failed -> starting WiFiManager..."));
    WiFiManager wm;
    wm.setHostname(MDNS_NAME);
    if (!wm.autoConnect("LED-16x16_Setup")) {
      // 3) Notfall: fixer AP
      Serial.println(F("[WiFi] WiFiManager failed -> start fallback AP"));
      WiFi.mode(WIFI_AP);
      WiFi.softAP("LED-16x16", "12345678");
      Serial.print(F("[WiFi] AP IP: "));
      Serial.println(WiFi.softAPIP());
    }
  }

  // mDNS bereitstellen (STA oder AP)
  if (MDNS.begin(MDNS_NAME)) {
    MDNS.addService("http", "tcp", 80);
    Serial.println(F("[mDNS] started: http://led-16x16.local"));
  } else {
    Serial.println(F("[mDNS] failed"));
  }
}

void setup() {
  delay(50);
  Serial.begin(115200);

  FastLED.addLeds<LED_TYPE, DATA_PIN, COLOR_ORDER>(leds, NUM_LEDS).setCorrection(TypicalLEDStrip);
  FastLED.clear(true);
  FastLED.setBrightness(S.brightness);

  setupWiFi();

  server.on("/", handleRoot);
  server.on("/state", handleState);
  server.on("/set", handleSet);
  server.begin();

  random16_add_entropy(analogRead(A0));
  initMatrix();
}

void loop() {
  server.handleClient();
  MDNS.update();

  if (S.soundEnabled) readSound();

  uint32_t now = millis();
  uint16_t frameInterval = 15 + (255 - S.speed) / 6;
  if (S.soundEnabled && S.soundMode == 1) {
    uint16_t boost = soundAmp / 30;
    frameInterval = (frameInterval > boost) ? (frameInterval - boost) : 10;
  }

  if (now - lastFrame >= frameInterval) {
    lastFrame = now;
    switch (S.effect) {
      case E_PLASMA: drawPlasma(now); break;
      case E_MATRIX: drawMatrix(now); break;
      case E_CRT:    drawCRT(now);    break;
    }
    uint8_t dynBright = S.brightness;
    if (S.soundEnabled && S.soundMode == 0 && soundPeak) dynBright = qadd8(dynBright, 30);
    FastLED.setBrightness(dynBright);
    FastLED.show();
  }
}
