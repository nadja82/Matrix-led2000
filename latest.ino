/****************************************************
 * 16x16 WS2812B – Dub Visualizer v6 (Safe Ripples)
 * - Effects: Plasma, Matrix, CRT, Ring, Ripples(SAFE), EKG
 * - Themes, Auto-Rotate (whitelist), Crossfade
 * - Flare Mode (+ 5s safety), Sound reactive (A0)
 * - Persistent settings (LittleFS /config.ini)
 * - Robust WiFi: reconnect + mDNS reinit, /ping, /black, /factory
 * - Boot LED self-test (rainbow wipe)
 * - mDNS: http://led-16x16.local/
 ****************************************************/

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <DNSServer.h>
#include <FastLED.h>
#include <FS.h>
#include <LittleFS.h>
#include <WiFiManager.h>

// ========= WLAN =========
#define PREFERRED_SSID "Wilma2001_Ext"
#define PREFERRED_PASS "14D12k82"

// ===== LED/Matrix =====
#define DATA_PIN    D5            // ESP8266 D5 = GPIO14
#define LED_TYPE    WS2812B
#define COLOR_ORDER GRB
#define MATRIX_W    16
#define MATRIX_H    16
#define NUM_LEDS    (MATRIX_W * MATRIX_H)

CRGB leds[NUM_LEDS];
CRGB ledsNext[NUM_LEDS];

// ===== Web/Misc =====
ESP8266WebServer server(80);
const char* MDNS_NAME = "led-16x16";

// ===== Effekte/Settings =====
enum Effect : uint8_t { E_PLASMA=0, E_MATRIX=1, E_CRT=2, E_RING=3, E_RIPPLES=4, E_EKG=5, EFFECT_COUNT=6 };
enum ColorTheme : uint8_t { T_DUB=0, T_PURPLE=1, T_OCEAN=2, T_FIRE=3, T_RAINBOW=4, T_MONO=5, THEME_COUNT=6 };

struct Settings {
  uint8_t effect        = E_PLASMA;
  uint8_t brightness    = 160;  // failsafe: not 0
  uint8_t speed         = 110;
  uint8_t density       = 120;
  uint8_t hueShift      = 0;
  bool    soundEnabled  = false;
  uint8_t soundSensitivity = 140;
  uint8_t soundMode     = 0;    // 0=Peaks, 1=Reactive
  uint8_t colorTheme    = T_DUB;
  uint16_t autoCycleMin = 0;
  uint8_t enabledMask   = 0x3F; // all 6 enabled
  bool     flareEnabled = false;
  uint16_t flareHoldMs  = 120;
} S;

uint32_t lastFrameMs = 0;
uint32_t lastModeChangeMs = 0;

// Crossfade
bool     transitioning    = false;
uint8_t  nextEffect       = E_PLASMA;
uint32_t transStartMs     = 0;
uint16_t transDurationMs  = 1100;

// Flare
uint32_t flareUntilMs     = 0;
uint32_t lastSoundSeenMs  = 0;   // for flare safety

// WiFi Events
WiFiEventHandler hGotIP, hDiscon;
bool needMDNSReinit = false;

// ===== XY-Mapping (serpentine) =====
inline uint16_t XY(uint8_t x, uint8_t y) {
  if (x >= MATRIX_W || y >= MATRIX_H) return 0;
  return (y & 1) ? (y * MATRIX_W + (MATRIX_W - 1 - x)) : (y * MATRIX_W + x);
}

// ===== Palettes / Themes =====
CRGBPalette16 PAL_DUB(
  CHSV(128,180, 20), CHSV(140,180,160), CHSV(120,200,200), CHSV(100,120,255),
  CHSV(96,200,140),  CHSV(85,220,110),  CHSV(160,80,60),   CHSV(128,120,20),
  CRGB::Black,CRGB::Black,CRGB::Black,CRGB::Black,
  CHSV(128,180,20), CHSV(140,180,160), CHSV(96,200,140), CRGB::Black
);
CRGBPalette16 PAL_PURPLE(
  CHSV(200,200,30), CHSV(205,200,120), CHSV(210,180,200), CHSV(220,140,255),
  CHSV(195,220,160), CHSV(208,220,110), CHSV(188,110,80), CHSV(200,140,30),
  CRGB::Black,CRGB::Black,CRGB::Black,CRGB::Black,
  CHSV(210,200,80), CHSV(215,180,180), CHSV(205,200,140), CRGB::Black
);
CRGBPalette16 PAL_OCEAN = OceanColors_p;
CRGBPalette16 PAL_FIRE  = HeatColors_p;
CRGBPalette16 PAL_RAINBOW = RainbowColors_p;
CRGBPalette16 PAL_MONO(
  CHSV(0,0,5), CHSV(0,0,30), CHSV(0,0,80), CHSV(0,0,120),
  CHSV(0,0,160), CHSV(0,0,200), CHSV(0,0,240), CHSV(0,0,255),
  CHSV(0,0,5), CHSV(0,0,30), CHSV(0,0,80), CHSV(0,0,120),
  CHSV(0,0,160), CHSV(0,0,200), CHSV(0,0,240), CHSV(0,0,255)
);

inline const CRGBPalette16& activePalette() {
  switch (S.colorTheme) {
    case T_PURPLE:  return PAL_PURPLE;
    case T_OCEAN:   return PAL_OCEAN;
    case T_FIRE:    return PAL_FIRE;
    case T_RAINBOW: return PAL_RAINBOW;
    case T_MONO:    return PAL_MONO;
    default:        return PAL_DUB;
  }
}
inline uint8_t baseHueForTheme() {
  switch (S.colorTheme) {
    case T_PURPLE:  return 200;
    case T_OCEAN:   return 140;
    case T_FIRE:    return 0;
    case T_RAINBOW: return 0;
    case T_MONO:    return 0;
    default:        return 120; // Dub
  }
}

// ===== Sound (A0) =====
uint16_t soundBaseline = 512;
uint16_t soundAmp = 0;
bool     soundPeak = false;

uint16_t readSound() {
  const uint8_t N = 6;
  uint32_t sum = 0;
  for (uint8_t i=0; i<N; i++) sum += analogRead(A0);
  uint16_t raw = sum / N;

  uint8_t emaAlpha = map(S.soundSensitivity, 0,255, 1, 12);
  soundBaseline = ((uint32_t)(255-emaAlpha) * soundBaseline + (uint32_t)emaAlpha * raw) / 255;

  uint16_t amp = (raw > soundBaseline) ? (raw - soundBaseline) : (soundBaseline - raw);
  if (amp > 1023) amp = 1023;

  uint16_t thr = map(255 - S.soundSensitivity, 0,255, 20, 300);
  bool isPeak = (amp > thr);

  static uint32_t lastPeak = 0;
  uint32_t now = millis();
  soundPeak = false;
  if (S.soundMode == 0) {
    if (isPeak && (now - lastPeak) > 120) { soundPeak = true; lastPeak = now; }
  }
  soundAmp = amp;

  // remember for flare safety
  if (S.soundMode==0 ? soundPeak : (soundAmp>thr)) lastSoundSeenMs = now;

  return amp;
}

// ===== Pixel helpers =====
inline void PSET(CRGB* tgt, uint8_t x, uint8_t y, const CRGB& c){ tgt[XY(x,y)] = c; }
inline void PADD(CRGB* tgt, uint8_t x, uint8_t y, const CRGB& c){ tgt[XY(x,y)] += c; }
inline void PFDB(CRGB* tgt, uint8_t amount){ for (uint16_t i=0;i<NUM_LEDS;i++) tgt[i].fadeToBlackBy(amount); }
inline void PCLEAR(CRGB* tgt){ for (uint16_t i=0;i<NUM_LEDS;i++) tgt[i]=CRGB::Black; }

// ===== Effects =====
// -- Plasma --
void drawPlasmaTo(CRGB* tgt, uint32_t t) {
  const CRGBPalette16& pal = activePalette();
  float soundFactor = 0.0f;
  if (S.soundEnabled) {
    if (S.soundMode == 1) soundFactor = min(1.0f, soundAmp / 400.0f);
    else if (soundPeak)   S.hueShift += 8;
  }
  float time = t * (0.001f + (S.speed + soundFactor*60.0f) * 0.00001f);
  const float cx = (MATRIX_W - 1) / 2.0f;
  const float cy = (MATRIX_H - 1) / 2.0f;

  for (uint8_t y=0; y<MATRIX_H; y++) {
    for (uint8_t x=0; x<MATRIX_W; x++) {
      float dx = (x - cx), dy = (y - cy);
      float dist = sqrtf(dx*dx + dy*dy);
      float angle = atan2f(dy, dx);
      float v = sinf(dist * 0.9f - time) + sinf(angle*3.0f + time*1.2f);
      uint8_t idx = S.hueShift + (uint8_t)((v+1.0f)*96.0f);
      PSET(tgt, x, y, ColorFromPalette(pal, idx, 200));
    }
  }
}

// -- Matrix --
struct Drop { int16_t y; uint8_t len; uint8_t speed; bool alive; };
Drop drops[MATRIX_W];
void spawnDrop(uint8_t col, uint8_t extraLen=0) {
  drops[col].y = - (int8_t) random8(2, 6);
  drops[col].len = max<uint8_t>(2, (S.density >> 5) + random8(1,4) + extraLen);
  drops[col].speed = max<uint8_t>(1, (S.speed >> 6));
  drops[col].alive = true;
}
void initMatrix() { for (uint8_t x=0; x<MATRIX_W; x++) drops[x].alive = false; }
void drawMatrixTo(CRGB* tgt, uint32_t t) {
  for (uint16_t i=0; i<NUM_LEDS; i++) tgt[i].nscale8_video(180);
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
  uint8_t baseHue = baseHueForTheme();
  for (uint8_t x=0; x<MATRIX_W; x++) {
    if (!drops[x].alive) continue;
    drops[x].y += drops[x].speed;
    for (int8_t k=0; k<drops[x].len; k++) {
      int16_t y = drops[x].y - k;
      if (y >= 0 && y < MATRIX_H) {
        uint8_t v = 220 - k*24;
        PSET(tgt, x, y, CHSV(baseHue, 220, v < 20 ? 20 : v));
        if (y+1 < MATRIX_H) PADD(tgt, x, y+1, CHSV(baseHue, 240, 40));
      }
    }
    if (drops[x].y - drops[x].len > MATRIX_H) drops[x].alive = false;
  }
}

// -- CRT --
uint8_t vignette(uint8_t x, uint8_t y) {
  uint8_t vx = min(x, (uint8_t)(MATRIX_W-1 - x));
  uint8_t vy = min(y, (uint8_t)(MATRIX_H-1 - y));
  uint8_t v = min(vx, vy); return 200 + v*6;
}
void drawCRTTo(CRGB* tgt, uint32_t t) {
  float rollPos = fmodf((t * (0.02f + S.speed * 0.0004f)), (float)MATRIX_H);
  uint8_t extra = 0;
  if (S.soundEnabled) {
    if (S.soundMode == 0 && soundPeak) extra = 60;
    else if (S.soundMode == 1)          extra = map(min<uint16_t>(soundAmp,400), 0,400, 0,40);
  }
  uint8_t baseHue = baseHueForTheme();
  for (uint8_t y=0; y<MATRIX_H; y++) {
    for (uint8_t x=0; x<MATRIX_W; x++) {
      uint8_t base = 40;
      uint8_t sl = (y & 1) ? 20 : 0;
      float d = fabsf((float)y - rollPos);
      uint8_t roll = (d < 1.0f) ? 150 : (d < 2.0f ? 60 : 0);
      uint8_t v = base + roll + extra + sl;
      v = qadd8(v, vignette(x,y) - 200);
      PSET(tgt, x, y, CHSV(baseHue, 40, v));
    }
  }
}

// -- Ring --
void drawRingTo(CRGB* tgt, uint32_t t) {
  static uint16_t z = 0; z += 2;
  const CRGBPalette16& pal = activePalette();
  for (uint8_t y=0; y<MATRIX_H; y++)
    for (uint8_t x=0; x<MATRIX_W; x++)
      PSET(tgt, x, y, ColorFromPalette(pal, inoise8(x*20,y*20,z), 70));

  float cx = (MATRIX_W - 1) * 0.5f, cy = (MATRIX_H - 1) * 0.5f;
  float baseR = 3.2f + 3.0f * (S.soundEnabled ? (S.soundMode==1 ? min(1.0f, soundAmp/400.0f) : (soundPeak?0.5f:0.0f)) : 0.2f);
  float angSpd = 0.02f + S.speed * 0.00007f;
  static float ang = 0.0f; ang += angSpd; if (ang > 6.283f) ang -= 6.283f;
  uint8_t glow = 100 + (S.soundEnabled ? min<uint16_t>(soundAmp, 155) : 40);
  for (int i=0; i<72; i++) {
    float a = ang + (i * (2*PI/36.0f));
    int xi = (int)roundf(cx + baseR * cosf(a));
    int yi = (int)roundf(cy + baseR * sinf(a));
    if (xi>=0 && xi<MATRIX_W && yi>=0 && yi<MATRIX_H) {
      PADD(tgt, xi, yi, ColorFromPalette(pal, 110 + i, 140 + (glow/2)));
      if (xi+1<MATRIX_W) PADD(tgt, xi+1, yi, ColorFromPalette(pal, 120, 40));
      if (xi>0)          PADD(tgt, xi-1, yi, ColorFromPalette(pal, 120, 40));
      if (yi+1<MATRIX_H) PADD(tgt, xi, yi+1, ColorFromPalette(pal, 120, 40));
      if (yi>0)          PADD(tgt, xi, yi-1, ColorFromPalette(pal, 120, 40));
    }
  }
}

// -- EKG --
void drawEKGTo(CRGB* tgt, uint32_t t) {
  for (uint16_t i=0; i<NUM_LEDS; i++) tgt[i].nscale8_video(210);
  uint8_t huePurple = (S.colorTheme==T_PURPLE) ? 205 : 200;
  if (S.colorTheme==T_DUB) huePurple = 190;
  if (S.colorTheme==T_OCEAN) huePurple = 160;
  if (S.colorTheme==T_FIRE)  huePurple = 0;
  if (S.colorTheme==T_MONO)  huePurple = 0;
  const uint8_t sat = (S.colorTheme==T_MONO) ? 0 : 200;
  const uint8_t baseBright = 180;

  static float sweepX = 0.0f;
  float sweepSpeed = 0.20f + (S.speed * 0.004f);
  sweepX += sweepSpeed; if (sweepX >= MATRIX_W) sweepX -= MATRIX_W;

  float amp = 0.6f;
  if (S.soundEnabled) amp += (S.soundMode==1) ? min(1.8f, (float)soundAmp/220.0f) : (soundPeak?1.6f:0.4f);

  float y1 = 5.0f  + 0.8f * sinf(t * 0.0045f);
  float y2 = 10.0f - 0.8f * cosf(t * 0.0040f);

  static int  spikeTimer1 = 0, spikeTimer2 = 0;
  static int  spikeX1 = 8,  spikeX2 = 8;
  if (S.soundEnabled && ((S.soundMode==0 && soundPeak) || (S.soundMode==1 && soundAmp>240))) {
    spikeTimer1 = 6; spikeTimer2 = 6;
    spikeX1 = (int)roundf(sweepX); spikeX2 = (spikeX1 + 6) % MATRIX_W;
  }

  auto drawLine = [&](float baseY, int spikeX, int& spikeTimer) {
    for (int x=0; x<MATRIX_W; x++) {
      float yy = baseY + amp * sinf((x * 0.55f) + (t * 0.0065f));
      if (spikeTimer > 0) { int dx = abs(x - spikeX);
        if (dx <= 1) yy -= 4.0f; else if (dx == 2) yy += 2.0f; }
      int y = (int)roundf(yy);
      if (y>=0 && y<MATRIX_H) PSET(tgt, x, y, CHSV(huePurple, sat, baseBright));
      if (y+1>=0 && y+1<MATRIX_H) PADD(tgt, x, y+1, CHSV(huePurple, sat, baseBright/2));
    }
    if (spikeTimer > 0) spikeTimer--;
  };
  drawLine(y1, spikeX1, spikeTimer1);
  drawLine(y2, spikeX2, spikeTimer2);
  PFDB(tgt, 10);
}

// ======= SAFE DUB RIPPLES =======
#define RIPPLE_MAX 3
struct RippleS { uint16_t r; uint8_t b; bool active; };   // r in Q8.8, b 0..255
RippleS ripS[RIPPLE_MAX];
uint16_t distMap[NUM_LEDS];     // Q8.8 distance to center per pixel
uint16_t maxDistQ = 0;          // max distance (Q8.8)
const uint16_t RIPPLE_WIDTH_Q = 80;   // ~0.31px band (Q8.8)
const uint16_t RIPPLE_VEL_Q   = 60;   // ~0.23px/frame (Q8.8)
uint32_t lastRippleSpawnMs = 0;

void initRipplesSafe() {
  float cx = (MATRIX_W - 1) * 0.5f, cy = (MATRIX_H - 1) * 0.5f;
  maxDistQ = 0;
  for (uint8_t y=0; y<MATRIX_H; y++) {
    for (uint8_t x=0; x<MATRIX_W; x++) {
      float dx = x - cx, dy = y - cy;
      float d  = sqrtf(dx*dx + dy*dy);
      uint16_t q = (uint16_t)(d * 256.0f); // Q8.8
      distMap[XY(x,y)] = q;
      if (q > maxDistQ) maxDistQ = q;
    }
  }
  for (uint8_t i=0; i<RIPPLE_MAX; i++) ripS[i] = {0, 0, false};
}

inline void spawnRippleSafe(uint8_t baseB) {
  for (uint8_t i=0; i<RIPPLE_MAX; i++) {
    if (!ripS[i].active) {
      ripS[i] = {0, baseB, true};
      lastRippleSpawnMs = millis();
      return;
    }
  }
  // if all used: replace first (simple and stable)
  ripS[0] = {0, baseB, true};
  lastRippleSpawnMs = millis();
}

void drawRipplesTo(CRGB* tgt, uint32_t t) {
  static uint16_t z = 0; z += 2;
  const CRGBPalette16& pal = activePalette();

  // Subtle background noise
  for (uint8_t y=0; y<MATRIX_H; y++) {
    for (uint8_t x=0; x<MATRIX_W; x++) {
      uint8_t n = inoise8(x*22, y*22, z);
      PSET(tgt, x, y, ColorFromPalette(pal, n + 30, 70));
    }
    yield(); // keep WDT happy
  }

  // Triggers
  if (S.soundEnabled) {
    uint16_t thr = map(255 - S.soundSensitivity, 0, 255, 20, 300);
    bool trig = (S.soundMode == 0) ? soundPeak : (soundAmp > thr);
    if (trig && (millis() - lastRippleSpawnMs > 90)) {
      uint8_t b = (uint8_t)constrain(100 + (soundAmp >> 2), 80, 220);
      spawnRippleSafe(b);
    }
  } else if (millis() - lastRippleSpawnMs > 900) {
    spawnRippleSafe(120);
  }

  // Render ripples (integer only)
  for (uint8_t i=0; i<RIPPLE_MAX; i++) if (ripS[i].active) {
    uint16_t r = ripS[i].r;
    uint8_t  b = ripS[i].b;

    for (uint16_t idx = 0; idx < NUM_LEDS; idx++) {
      uint16_t d = distMap[idx];
      uint16_t band = (d > r) ? (d - r) : (r - d);
      if (band < RIPPLE_WIDTH_Q) {
        uint8_t v = (uint8_t)((uint32_t)(RIPPLE_WIDTH_Q - band) * b / RIPPLE_WIDTH_Q);
        leds[idx] += ColorFromPalette(pal, 100 + i*6, v);
      }
    }

    ripS[i].r += RIPPLE_VEL_Q;
    ripS[i].b  = (ripS[i].b > 3) ? (ripS[i].b - 3) : 0;

    if (ripS[i].r > (uint16_t)(maxDistQ + 2*RIPPLE_WIDTH_Q) || ripS[i].b == 0) {
      ripS[i].active = false;
    }
  }

  PFDB(tgt, 8);
}

// ===== Dispatcher =====
void drawEffectTo(CRGB* tgt, uint8_t effect, uint32_t t) {
  switch (effect) {
    case E_PLASMA:  drawPlasmaTo(tgt, t);  break;
    case E_MATRIX:  drawMatrixTo(tgt, t);  break;
    case E_CRT:     drawCRTTo(tgt, t);     break;
    case E_RING:    drawRingTo(tgt, t);    break;
    case E_RIPPLES: drawRipplesTo(tgt, t); break;  // SAFE
    case E_EKG:     drawEKGTo(tgt, t);     break;
  }
}

// ===== Crossfade =====
void startTransition(uint8_t newEff) {
  if (newEff == S.effect) return;
  nextEffect = newEff;
  transStartMs = millis();
  transitioning = true;
  for (uint16_t i=0;i<NUM_LEDS;i++) ledsNext[i]=CRGB::Black;
  drawEffectTo(ledsNext, nextEffect, millis());
}
void updateTransition() {
  if (!transitioning) return;
  uint32_t elapsed = millis() - transStartMs;
  if (elapsed >= transDurationMs) {
    for (uint16_t i=0;i<NUM_LEDS;i++) leds[i] = ledsNext[i];
    S.effect = nextEffect;
    transitioning = false;
    return;
  }
  uint8_t amt = map(elapsed, 0, transDurationMs, 0, 255);
  for (uint16_t i=0;i<NUM_LEDS;i++) leds[i] = blend(leds[i], ledsNext[i], amt);
}

// ===== Auto-Cycle =====
bool isEffectEnabled(uint8_t eff) { return (eff<EFFECT_COUNT) && ((S.enabledMask>>eff)&1); }
uint8_t nextEnabledEffect(uint8_t from) {
  for (uint8_t k=1; k<=EFFECT_COUNT; k++) { uint8_t c=(from+k)%EFFECT_COUNT; if (isEffectEnabled(c)) return c; }
  return from;
}

// ===== HTML (unchanged UI) =====
const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html><html><head><meta charset="utf-8"/>
<meta name="viewport" content="width=device-width, initial-scale=1"/>
<title>16×16 LED Controller</title>
<style>
*{box-sizing:border-box}body{font-family:system-ui,-apple-system,Segoe UI,Roboto,Ubuntu,Arial,sans-serif;background:#f3f5f7;margin:0;padding:16px}
.card{max-width:860px;margin:0 auto;background:#fff;border-radius:16px;padding:18px 18px 20px;box-shadow:0 6px 26px rgba(0,0,0,.07)}
h1{font-size:20px;margin:0 0 8px}
label{display:block;margin-top:12px;font-size:14px;color:#333}
select,input[type=range],input[type=number]{width:100%}
.row{display:flex;gap:12px;flex-wrap:wrap}.row>div{flex:1 1 240px}
.val{font-variant-numeric:tabular-nums}
.kv{font-size:12px;color:#666;margin-top:8px}
button{margin-top:12px;padding:10px 14px;border-radius:10px;border:0;background:#111;color:#fff;font-weight:600;cursor:pointer}
.buttons{display:flex;gap:8px;flex-wrap:wrap}
small{color:#666}
.toggle{display:flex;align-items:center;gap:8px;margin-top:12px}
fieldset{border:1px solid #e6e8eb;border-radius:12px;padding:10px;margin-top:12px}
legend{padding:0 6px;color:#666;font-size:12px}
</style></head><body>
<div class="card">
  <h1>16×16 LED Controller</h1>
  <div class="kv">mDNS: <b id="mdns"></b> — IP: <b id="ip"></b> — Host: <b id="host"></b></div>

  <div class="row">
    <div><label>Active Effect</label>
      <select id="effect">
        <option value="0">Plasma Tunnel</option>
        <option value="1">Matrix Rain</option>
        <option value="2">CRT / Scanline</option>
        <option value="3">Rotating Ring (Dub)</option>
        <option value="4">Dub Ripples (SAFE)</option>
        <option value="5">EKG Lines (Dub)</option>
      </select>
    </div>
    <div><label>Color Theme</label>
      <select id="colorTheme">
        <option value="0">Dub (Teal/Green)</option>
        <option value="1">Purple</option>
        <option value="2">Ocean</option>
        <option value="3">Fire</option>
        <option value="4">Rainbow</option>
        <option value="5">Mono</option>
      </select>
    </div>
  </div>

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

  <div class="row">
    <div class="toggle"><input type="checkbox" id="soundEnabled"/><label for="soundEnabled">Sound reactive (A0)</label></div>
    <div><label>Sensitivity: <span id="ssv" class="val"></span></label><input type="range" id="soundSensitivity" min="0" max="255" step="1"/></div>
    <div><label>Mode</label><select id="soundMode"><option value="0">Peaks</option><option value="1">Reactive</option></select></div>
  </div>

  <div class="row">
    <div><label>Auto switch (minutes, 0=off)</label><input type="number" id="autoCycleMin" min="0" max="120" step="1"/></div>
    <div><label>Transition (ms)</label><input type="number" id="transDurationMs" min="200" max="5000" step="100"/></div>
  </div>

  <fieldset>
    <legend>Which effects may run?</legend>
    <div class="row">
      <div><label><input type="checkbox" id="en0"/> Plasma</label></div>
      <div><label><input type="checkbox" id="en1"/> Matrix</label></div>
      <div><label><input type="checkbox" id="en2"/> CRT</label></div>
      <div><label><input type="checkbox" id="en3"/> Ring</label></div>
      <div><label><input type="checkbox" id="en4"/> Ripples</label></div>
      <div><label><input type="checkbox" id="en5"/> EKG</label></div>
    </div>
  </fieldset>

  <fieldset>
    <legend>Flare mode</legend>
    <div class="row">
      <div class="toggle"><input type="checkbox" id="flareEnabled"/><label for="flareEnabled">Show only on audio level</label></div>
      <div><label>Flare hold (ms)</label><input type="number" id="flareHoldMs" min="5" max="3000" step="5"/></div>
    </div>
    <div class="kv"><small>Safety: if no peaks for 5 s, image shows anyway.</small></div>
  </fieldset>

  <div class="buttons">
    <button id="apply">Save</button>
    <button onclick="fetch('/ping').then(r=>r.text()).then(t=>alert(t))">Ping</button>
    <button onclick="if(confirm('Delete config & reboot?')) fetch('/factory').then(()=>location.reload())">Factory Reset</button>
    <button onclick="fetch('/black')">Black</button>
  </div>

  <div class="kv"><small>Settings save immediately (LittleFS) and survive resets.</small></div>
</div>

<script>
const el = s => document.querySelector(s);
function refreshState(){
  fetch('/state').then(r=>r.json()).then(j=>{
    el('#effect').value = j.effect;
    el('#brightness').value = j.brightness; el('#bv').textContent = j.brightness;
    el('#speed').value = j.speed; el('#sv').textContent = j.speed;
    el('#density').value = j.density; el('#dv').textContent = j.density;
    el('#hueShift').value = j.hueShift; el('#hv').textContent = j.hueShift;
    el('#soundEnabled').checked = !!j.soundEnabled;
    el('#soundSensitivity').value = j.soundSensitivity; el('#ssv').textContent = j.soundSensitivity;
    el('#soundMode').value = j.soundMode;
    el('#colorTheme').value = j.colorTheme;
    el('#autoCycleMin').value = j.autoCycleMin;
    el('#transDurationMs').value = j.transDurationMs;
    for(let i=0;i<6;i++){ el('#en'+i).checked = !!(j.enabledMask & (1<<i)); }
    el('#flareEnabled').checked = !!j.flareEnabled;
    el('#flareHoldMs').value = j.flareHoldMs;
    el('#mdns').textContent = j.mdns; el('#ip').textContent = j.ip; el('#host').textContent = j.hostname;
  });
}
function apply(){
  const p = new URLSearchParams();
  p.set('effect', el('#effect').value);
  p.set('brightness', el('#brightness').value);
  p.set('speed', el('#speed').value);
  p.set('density', el('#density').value);
  p.set('hueShift', el('#hueShift').value);
  p.set('soundEnabled', el('#soundEnabled').checked ? 1 : 0);
  p.set('soundSensitivity', el('#soundSensitivity').value);
  p.set('soundMode', el('#soundMode').value);
  p.set('colorTheme', el('#colorTheme').value);
  p.set('autoCycleMin', el('#autoCycleMin').value);
  p.set('transDurationMs', el('#transDurationMs').value);
  let mask = 0; for(let i=0;i<6;i++){ if(el('#en'+i).checked) mask |= (1<<i); }
  p.set('enabledMask', mask);
  p.set('flareEnabled', el('#flareEnabled').checked ? 1 : 0);
  p.set('flareHoldMs', el('#flareHoldMs').value);
  fetch('/set', {method:'POST', body:p}).then(()=>refreshState());
}
document.querySelectorAll('input[type=range]').forEach(r=>{
  r.addEventListener('input', e=>{
    const id = e.target.id, v = e.target.value;
    if(id==='brightness') el('#bv').textContent = v;
    if(id==='speed') el('#sv').textContent = v;
    if(id==='density') el('#dv').textContent = v;
    if(id==='hueShift') el('#hv').textContent = v;
    if(id==='soundSensitivity') el('#ssv').textContent = v;
  });
});
el('#apply').addEventListener('click', apply);
refreshState();
</script>
</body></html>
)HTML";

// ===== Persistenz =====
const char* CFG_PATH = "/config.ini";
void saveSettings() {
  File f = LittleFS.open(CFG_PATH, "w"); if (!f) return;
  f.printf("effect=%u\nbrightness=%u\nspeed=%u\ndensity=%u\nhueShift=%u\n", S.effect,S.brightness,S.speed,S.density,S.hueShift);
  f.printf("soundEnabled=%u\nsoundSensitivity=%u\nsoundMode=%u\n", S.soundEnabled?1:0,S.soundSensitivity,S.soundMode);
  f.printf("colorTheme=%u\nautoCycleMin=%u\nenabledMask=%u\ntransDurationMs=%u\n", S.colorTheme,S.autoCycleMin,S.enabledMask,transDurationMs);
  f.printf("flareEnabled=%u\nflareHoldMs=%u\n", S.flareEnabled?1:0,S.flareHoldMs);
  f.close();
}
void parseKV(char* line, String& k, int& v) { char* eq=strchr(line,'='); if(!eq){k="";v=0;return;} *eq=0; k=String(line); v=atoi(eq+1); }
void loadSettings() {
  if (!LittleFS.exists(CFG_PATH)) { saveSettings(); return; }
  File f = LittleFS.open(CFG_PATH, "r"); if (!f) return;
  char buf[64]; while (f.available()) {
    size_t n=f.readBytesUntil('\n',buf,sizeof(buf)-1); buf[n]=0;
    String key; int val; parseKV(buf,key,val);
    if(key=="effect")S.effect=constrain(val,0,EFFECT_COUNT-1);
    else if(key=="brightness")S.brightness=constrain(val,5,255);
    else if(key=="speed")S.speed=constrain(val,0,255);
    else if(key=="density")S.density=constrain(val,0,255);
    else if(key=="hueShift")S.hueShift=constrain(val,0,255);
    else if(key=="soundEnabled")S.soundEnabled=(val!=0);
    else if(key=="soundSensitivity")S.soundSensitivity=constrain(val,0,255);
    else if(key=="soundMode")S.soundMode=constrain(val,0,1);
    else if(key=="colorTheme")S.colorTheme=constrain(val,0,THEME_COUNT-1);
    else if(key=="autoCycleMin")S.autoCycleMin=constrain(val,0,120);
    else if(key=="enabledMask")S.enabledMask=(uint8_t)val;
    else if(key=="transDurationMs")transDurationMs=constrain(val,200,5000);
    else if(key=="flareEnabled")S.flareEnabled=(val!=0);
    else if(key=="flareHoldMs")S.flareHoldMs=constrain(val,5,3000);
  } f.close();
}

// ===== JSON =====
String jsonState() {
  IPAddress ip = WiFi.isConnected() ? WiFi.localIP() : WiFi.softAPIP();
  String ipStr = ip.toString();
  char buf[540];
  snprintf(buf, sizeof(buf),
    "{\"effect\":%u,\"brightness\":%u,\"speed\":%u,\"density\":%u,\"hueShift\":%u,"
    "\"soundEnabled\":%u,\"soundSensitivity\":%u,\"soundMode\":%u,"
    "\"colorTheme\":%u,\"autoCycleMin\":%u,\"enabledMask\":%u,"
    "\"transDurationMs\":%u,\"flareEnabled\":%u,\"flareHoldMs\":%u,"
    "\"ip\":\"%s\",\"hostname\":\"%s\",\"mdns\":\"%s.local\"}",
    S.effect,S.brightness,S.speed,S.density,S.hueShift,
    (unsigned)S.soundEnabled,S.soundSensitivity,S.soundMode,
    S.colorTheme,S.autoCycleMin,S.enabledMask,
    transDurationMs,(unsigned)S.flareEnabled,S.flareHoldMs,
    ipStr.c_str(), WiFi.hostname().c_str(), MDNS_NAME
  );
  return String(buf);
}

// ===== HTTP =====
void handleRoot()   { server.send_P(200, "text/html; charset=utf-8", INDEX_HTML); }
void handleState()  { server.send(200, "application/json", jsonState()); }
void handleSet() {
  uint8_t newEffect = S.effect;
  if (server.hasArg("effect"))          newEffect        = (uint8_t) constrain(server.arg("effect").toInt(), 0, EFFECT_COUNT-1);
  if (server.hasArg("brightness"))      S.brightness     = (uint8_t) constrain(server.arg("brightness").toInt(), 5, 255);
  if (server.hasArg("speed"))           S.speed          = (uint8_t) constrain(server.arg("speed").toInt(), 0, 255);
  if (server.hasArg("density"))         S.density        = (uint8_t) constrain(server.arg("density").toInt(), 0, 255);
  if (server.hasArg("hueShift"))        S.hueShift       = (uint8_t) constrain(server.arg("hueShift").toInt(), 0, 255);
  if (server.hasArg("soundEnabled"))    S.soundEnabled   = (server.arg("soundEnabled").toInt() == 1);
  if (server.hasArg("soundSensitivity"))S.soundSensitivity= (uint8_t) constrain(server.arg("soundSensitivity").toInt(), 0, 255);
  if (server.hasArg("soundMode"))       S.soundMode      = (uint8_t) constrain(server.arg("soundMode").toInt(), 0, 1);
  if (server.hasArg("colorTheme"))      S.colorTheme     = (uint8_t) constrain(server.arg("colorTheme").toInt(), 0, THEME_COUNT-1);
  if (server.hasArg("autoCycleMin"))    S.autoCycleMin   = (uint16_t) constrain(server.arg("autoCycleMin").toInt(), 0, 120);
  if (server.hasArg("enabledMask"))     S.enabledMask    = (uint8_t) server.arg("enabledMask").toInt();
  if (server.hasArg("transDurationMs")) transDurationMs  = (uint16_t) constrain(server.arg("transDurationMs").toInt(), 200, 5000);
  if (server.hasArg("flareEnabled"))    S.flareEnabled   = (server.arg("flareEnabled").toInt() == 1);
  if (server.hasArg("flareHoldMs"))     S.flareHoldMs    = (uint16_t) constrain(server.arg("flareHoldMs").toInt(), 5, 3000);

  if (newEffect == E_MATRIX) initMatrix();
  if (newEffect != S.effect) startTransition(newEffect);

  FastLED.setBrightness(S.brightness);
  lastModeChangeMs = millis();
  saveSettings();
  server.send(200, "application/json", jsonState());
}
void handlePing()    { server.send(200, "text/plain", "pong"); }
void handleBlack()   { FastLED.clear(true); server.send(200, "text/plain", "black"); }
void handleFactory() {
  LittleFS.remove(CFG_PATH);
  server.send(200, "text/plain", "factory-reset");
  delay(200);
  ESP.restart();
}

// ===== WiFi =====
void setupWiFi() {
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);          // modem-sleep off
  WiFi.setAutoReconnect(true);
  WiFi.hostname(MDNS_NAME);

  hGotIP = WiFi.onStationModeGotIP([](const WiFiEventStationModeGotIP& e){
    Serial.printf("[WiFi] Got IP: %s\n", e.ip.toString().c_str());
    needMDNSReinit = true;
  });
  hDiscon = WiFi.onStationModeDisconnected([](const WiFiEventStationModeDisconnected& e){
    Serial.printf("[WiFi] Disconnected (%d). Reconnecting...\n", e.reason);
    WiFi.disconnect(false);
    WiFi.begin(PREFERRED_SSID, PREFERRED_PASS);
  });

  WiFi.begin(PREFERRED_SSID, PREFERRED_PASS);
  Serial.println(F("[WiFi] Connecting..."));
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) { delay(250); Serial.print('.'); }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(F("\n[WiFi] Preferred failed -> WiFiManager/AP..."));
    WiFiManager wm; wm.setHostname(MDNS_NAME);
    if (!wm.autoConnect("LED-16x16_Setup")) {
      WiFi.mode(WIFI_AP);
      WiFi.softAP("LED-16x16", "12345678");
      Serial.print(F("[WiFi] AP IP: ")); Serial.println(WiFi.softAPIP());
    }
  } else {
    Serial.printf("\n[WiFi] IP: %s\n", WiFi.localIP().toString().c_str());
  }

  if (MDNS.begin(MDNS_NAME)) { MDNS.addService("http","tcp",80); }
  else Serial.println(F("[mDNS] FAILED"));
}

// ===== Boot LED self-test =====
void bootTest() {
  for (uint8_t y=0; y<MATRIX_H; y++) {
    for (uint8_t x=0; x<MATRIX_W; x++) {
      leds[XY(x,y)] = CHSV((x*16+y*8), 255, 180);
    }
    FastLED.show(); delay(30);
  }
  delay(200);
  FastLED.clear(true);
}

// ===== Setup / Loop =====
void setup() {
  Serial.begin(115200);
  LittleFS.begin();
  loadSettings(); saveSettings();

  FastLED.addLeds<LED_TYPE, DATA_PIN, COLOR_ORDER>(leds, NUM_LEDS).setCorrection(TypicalLEDStrip);
  FastLED.setBrightness(S.brightness);
  FastLED.clear(true);
  bootTest();

  setupWiFi();

  server.on("/", handleRoot);
  server.on("/state", handleState);
  server.on("/set", handleSet);
  server.on("/ping", handlePing);
  server.on("/black", handleBlack);
  server.on("/factory", handleFactory);
  server.begin();

  random16_add_entropy(analogRead(A0));
  initMatrix();
  initRipplesSafe();        // <<< important for SAFE ripples
  lastModeChangeMs = millis();
  lastSoundSeenMs  = millis();

  drawEffectTo(leds, S.effect, millis());
  FastLED.show();
}

void loop() {
  server.handleClient();
  MDNS.update();

  // WiFi/mDNS watchdog
  static uint32_t lastWifiCheck = 0;
  if (millis() - lastWifiCheck > 3000) {
    lastWifiCheck = millis();
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("[WiFi] Lost. Retry...");
      WiFi.disconnect(false);
      WiFi.begin(PREFERRED_SSID, PREFERRED_PASS);
    }
    if (needMDNSReinit && WiFi.status()==WL_CONNECTED) {
      needMDNSReinit=false; MDNS.close();
      if (MDNS.begin(MDNS_NAME)) { MDNS.addService("http","tcp",80); Serial.printf("[mDNS] Reinit ok: http://%s.local\n", MDNS_NAME); }
      else Serial.println("[mDNS] Reinit failed");
    }
  }

  uint32_t nowMs = millis();
  if (S.soundEnabled) readSound();

  // Auto-Cycle
  if (S.autoCycleMin > 0 && !transitioning) {
    uint32_t intervalMs = (uint32_t)S.autoCycleMin * 60000UL;
    if (nowMs - lastModeChangeMs >= intervalMs) {
      uint8_t nxt = nextEnabledEffect(S.effect);
      if (nxt != S.effect) startTransition(nxt);
      lastModeChangeMs = nowMs;
      saveSettings();
    }
  }

  // Frame timing
  uint16_t frameInterval = 15 + (255 - S.speed) / 6;
  if (S.soundEnabled && S.soundMode == 1) {
    uint16_t boost = soundAmp / 30;
    frameInterval = (frameInterval > boost) ? (frameInterval - boost) : 2;
  }

  if (nowMs - lastFrameMs >= frameInterval) {
    lastFrameMs = nowMs;

    // Flare logic + safety
    bool showNow = true;
    if (S.flareEnabled) {
      showNow = (nowMs <= flareUntilMs);
      if (S.soundEnabled && (nowMs - lastSoundSeenMs > 5000)) {
        showNow = true; // safety: show if no peaks for 5s
      }
      uint16_t thr = map(255 - S.soundSensitivity, 0,255, 20, 300);
      bool triggered = S.soundEnabled && ((S.soundMode==0) ? soundPeak : (soundAmp > thr));
      if (triggered) flareUntilMs = nowMs + (uint32_t)S.flareHoldMs;
    }

    if (showNow) {
      if (!transitioning) {
        drawEffectTo(leds, S.effect, nowMs);
      } else {
        drawEffectTo(ledsNext, nextEffect, nowMs);
        updateTransition();
      }
    } else {
      FastLED.clear();
    }

    uint8_t dynBright = max<uint8_t>(S.brightness, 5);
    if (S.soundEnabled && S.soundMode == 0 && soundPeak) dynBright = qadd8(dynBright, 30);
    FastLED.setBrightness(dynBright);
    FastLED.show();
  }
}
