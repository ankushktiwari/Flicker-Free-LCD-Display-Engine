// Flicker-Free LCD Display Engine (ESP32, Wokwi)
// - Never calls lcd.clear(): a shadow buffer remembers what is on screen and
//   only characters that changed are written.
// - 4 pages rotate on a millis() timer; a critical fault overrides them at once.
// - No delay() anywhere.
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <stdarg.h>

LiquidCrystal_I2C lcd(0x27, 16, 2);

// ---------- Pins ----------
#define VBATT_PIN 34     // pot: battery voltage 9.0 - 12.6 V
#define CURR_PIN 35      // pot: load current 0 - 10 A
#define TEMP_PIN 32      // pot: temperature 20 - 80 C
#define RED_LED 2
#define GREEN_LED 4
#define YELLOW_LED 5
#define BUZZER 18
#define RELAY 19         // ACTIVE LOW: LOW = load connected
#define BTN_NEXT 25      // jump to next page
#define BTN_ACK 26       // acknowledge / clear a fault
#define BTN_TEST 27      // raise a manual test fault

// ---------- Display settings ----------
const uint8_t COLS = 16, ROWS = 2, CELLS = 32;
const uint32_t REFRESH_MS = 250;   // how often the screen content is recomposed
const uint32_t PAGE_MS = 3000;     // time each page stays up
const uint32_t SAMPLE_MS = 50;     // sensor + fault check (fault latency)
const uint32_t STAT_MS = 5000;
const uint8_t  CHAR_BUDGET = 6;    // max characters written per loop pass
const uint8_t  MERGE_GAP = 1;      // merge two changes if only 1 char apart
const uint8_t  NPAGES = 4;

// ---------- Limits ----------
const float V_MIN = 9.0, V_MAX = 12.6;
const float UV_TRIP = 9.3, UV_CLR = 9.6;      // under-voltage (with hysteresis)
const float OC_TRIP = 8.0, OC_CLR = 7.0;      // over-current
const float OT_TRIP = 60.0, OT_CLR = 55.0;    // over-temperature
const float WARN_SOC = 20.0, WARN_I = 6.0, WARN_T = 50.0;

// ---------- Data ----------
float volts = 12, amps = 0, tempC = 25;        // filtered values
float shownV = 12, shownI = 0, shownT = 25;    // values after display dead-band
bool haveData = false;
bool cUV = false, cOC = false, cOT = false;    // cause currently present
bool fUV = false, fOC = false, fOT = false, fTest = false;   // latched faults
bool faultMode = false, relayOn = true, forceRender = false;
uint8_t faultCode = 0, page = 0;
uint32_t tPage = 0, ackDeniedUntil = 0;
const char* FAULT_NAME[] = {"", "OVCURR", "OVTEMP", "UNDERV", "TEST"};

// ---------- Display engine state ----------
char frame[ROWS][COLS + 1];     // what we WANT on screen (off-screen buffer)
char shadow[ROWS][COLS + 1];    // what is ON screen now
uint8_t scanPos = 0;
uint32_t charsWritten = 0, cursorMoves = 0, renders = 0, naiveChars = 0, bigFlushes = 0;
uint32_t maxLoopUs = 0, stalls = 0;

// ---------- Event log ----------
void logEvent(const char* tag, const char* fmt, ...) {
  char msg[100];
  va_list ap; va_start(ap, fmt); vsnprintf(msg, sizeof msg, fmt, ap); va_end(ap);
  uint32_t t = millis();
  Serial.printf("[%5lu.%03lu] %-6s %s\n", (unsigned long)(t / 1000), (unsigned long)(t % 1000), tag, msg);
}

// ---------- Frame buffer helpers ----------
void fbClear() {
  for (uint8_t r = 0; r < ROWS; r++) { for (uint8_t c = 0; c < COLS; c++) frame[r][c] = ' '; frame[r][COLS] = 0; }
}
void fbPrintf(uint8_t row, uint8_t col, const char* fmt, ...) {
  char tmp[COLS + 1];
  va_list ap; va_start(ap, fmt); vsnprintf(tmp, sizeof tmp, fmt, ap); va_end(ap);
  for (uint8_t i = 0; tmp[i] && col + i < COLS; i++) frame[row][col + i] = tmp[i];
}

bool diffAhead(uint8_t r, uint8_t c) {      // another change within MERGE_GAP chars?
  for (uint8_t k = 1; k <= MERGE_GAP; k++)
    if (c + k < COLS && frame[r][c + k] != shadow[r][c + k]) return true;
  return false;
}

// Write only changed characters, at most `budget` per call, then return.
// The scan position is remembered, so the rest is written on the next pass.
uint8_t flush(uint8_t budget) {
  uint8_t n = 0, visited = 0;
  while (visited < CELLS && n < budget) {
    uint8_t r = scanPos / COLS, c = scanPos % COLS;
    if (frame[r][c] == shadow[r][c]) { scanPos = (scanPos + 1) % CELLS; visited++; continue; }
    lcd.setCursor(c, r); cursorMoves++;               // start of a run of changes
    while (c < COLS && n < budget) {
      if (frame[r][c] == shadow[r][c] && !diffAhead(r, c)) break;   // end of run
      lcd.write((uint8_t)frame[r][c]);
      shadow[r][c] = frame[r][c]; n++; charsWritten++; c++; visited++;
    }
    scanPos = (r * COLS + c) % CELLS;
  }
  return n;
}

// ---------- Page content ----------
float band(float shown, float v, float b) { return fabsf(v - shown) >= b ? v : shown; }

void composeBattery() {
  int pct = constrain((int)((shownV - V_MIN) / (V_MAX - V_MIN) * 100.0 + 0.5), 0, 100);
  fbPrintf(0, 0, "Batt %4.1fV %3d%%", shownV, pct);
  int fill = (pct * 13 + 50) / 100;
  frame[1][0] = '['; frame[1][14] = ']';
  for (int i = 0; i < 13; i++) frame[1][1 + i] = i < fill ? '#' : '-';
}

bool isWarning() {
  float soc = (volts - V_MIN) / (V_MAX - V_MIN) * 100.0;
  return soc < WARN_SOC || amps > WARN_I || tempC > WARN_T;
}

void composeSystem() {
  int nf = fUV + fOC + fOT + fTest;
  fbPrintf(0, 0, "State:%s", isWarning() ? "WARNING" : "NORMAL");
  fbPrintf(1, 0, "Relay:%-3s Flt:%d", relayOn ? "ON" : "OFF", nf);
}

void composeTelemetry() {
  uint32_t s = millis() / 1000;
  fbPrintf(0, 0, "I:%4.1fA T:%2.0f%cC", shownI, shownT, 223);
  fbPrintf(1, 0, "Up %02lu:%02lu:%02lu", (unsigned long)(s / 3600), (unsigned long)((s / 60) % 60), (unsigned long)(s % 60));
}

void composeStats() {
  float saved = naiveChars ? 100.0 * (1.0 - (float)charsWritten / (float)naiveChars) : 0;
  fbPrintf(0, 0, "Wr:%lu Mv:%lu", (unsigned long)charsWritten, (unsigned long)cursorMoves);
  fbPrintf(1, 0, "Saved %.0f%%", saved);
}

uint8_t activeFault() {                         // highest priority latched fault
  if (fOC) return 1;
  if (fOT) return 2;
  if (fUV) return 3;
  if (fTest) return 4;
  return 0;
}

void composeFault() {
  uint32_t now = millis();
  bool b = (now / 500) % 2;                     // only the "!!" blink
  char l0[24], l1[24];
  snprintf(l0, sizeof l0, "%sFAULT%s %-6s", b ? "!!" : "  ", b ? "!!" : "  ", FAULT_NAME[faultCode]);
  bool causeOn = (faultCode == 1 && cOC) || (faultCode == 2 && cOT) || (faultCode == 3 && cUV);
  if ((int32_t)(ackDeniedUntil - now) > 0) snprintf(l1, sizeof l1, "Cause STILL ON!");
  else if ((now / 2000) % 2 == 0) {
    if (faultCode == 1) snprintf(l1, sizeof l1, "I=%.1fA >%.1fA", amps, OC_TRIP);
    else if (faultCode == 2) snprintf(l1, sizeof l1, "T=%.0f%cC >%.0f%cC", tempC, 223, OT_TRIP, 223);
    else if (faultCode == 3) snprintf(l1, sizeof l1, "V=%.1fV <%.1fV", volts, UV_TRIP);
    else snprintf(l1, sizeof l1, "Manual test");
  } else snprintf(l1, sizeof l1, causeOn ? "Fix cause, ACK" : "Cause gone: ACK");
  fbPrintf(0, 0, "%s", l0); fbPrintf(1, 0, "%s", l1);
}

void renderFrame() {
  fbClear();
  if (faultMode) composeFault();
  else {
    if (page == 0) composeBattery();
    else if (page == 1) composeSystem();
    else if (page == 2) composeTelemetry();
    else composeStats();
    fbPrintf(1, 15, "%d", page + 1);              // page number
  }
  renders++; naiveChars += CELLS;                 // what a clear+redraw would have cost
}

// ---------- Relay ----------
void setRelay(bool on) {
  if (on == relayOn) return;
  relayOn = on; digitalWrite(RELAY, on ? LOW : HIGH);
  logEvent("RELAY", on ? "CLOSED" : "OPEN (fault)");
}

// ---------- Fault handling: override / restore the display ----------
void updateFaultMode() {
  uint8_t code = activeFault();
  if (code && (!faultMode || code != faultCode)) {
    faultMode = true; faultCode = code;
    setRelay(false);
    logEvent("DISPLAY", "FAULT SCREEN: %s", FAULT_NAME[code]);
    renderFrame(); flush(CELLS); bigFlushes++;    // immediate, not rate-limited
  } else if (!code && faultMode) {
    faultMode = false; faultCode = 0; page = 0; tPage = millis();
    setRelay(true);
    logEvent("DISPLAY", "fault cleared, normal pages resume");
    renderFrame(); flush(CELLS); bigFlushes++;
  }
}

void ackPressed() {
  if (!(fUV || fOC || fOT || fTest)) { logEvent("ACK", "no fault latched"); return; }
  bool denied = false;
  if (fOC) { if (cOC) denied = true; else fOC = false; }
  if (fOT) { if (cOT) denied = true; else fOT = false; }
  if (fUV) { if (cUV) denied = true; else fUV = false; }
  fTest = false;
  if (denied) { ackDeniedUntil = millis() + 2000; forceRender = true; logEvent("ACK", "refused, cause still active"); }
  else logEvent("ACK", "cleared by operator");
  updateFaultMode();
}

// ---------- Sensors (50 ms task) ----------
void sampleTask() {
  float v = V_MIN + analogRead(VBATT_PIN) / 4095.0 * (V_MAX - V_MIN);
  float i = analogRead(CURR_PIN) / 4095.0 * 10.0;
  float t = 20.0 + analogRead(TEMP_PIN) / 4095.0 * 60.0;

  // Faults use the raw readings (fast); the display uses smoothed values.
  if (v < UV_TRIP) cUV = true; else if (v > UV_CLR) cUV = false;
  if (i > OC_TRIP) cOC = true; else if (i < OC_CLR) cOC = false;
  if (t > OT_TRIP) cOT = true; else if (t < OT_CLR) cOT = false;
  if (cUV && !fUV) { fUV = true; logEvent("FAULT", "UNDER-VOLTAGE %.2fV", v); }
  if (cOC && !fOC) { fOC = true; logEvent("FAULT", "OVER-CURRENT %.2fA", i); }
  if (cOT && !fOT) { fOT = true; logEvent("FAULT", "OVER-TEMP %.1fC", t); }

  if (!haveData) { volts = shownV = v; amps = shownI = i; tempC = shownT = t; haveData = true; }
  else { volts += 0.3 * (v - volts); amps += 0.3 * (i - amps); tempC += 0.3 * (t - tempC); }
  shownV = band(shownV, volts, 0.07);             // dead-band: no digit flutter
  shownI = band(shownI, amps, 0.1);
  shownT = band(shownT, tempC, 0.5);
  updateFaultMode();
}

// ---------- Buttons (polled, debounced with millis) ----------
const uint8_t BTN_PIN[3] = {BTN_NEXT, BTN_ACK, BTN_TEST};
bool bStable[3] = {HIGH, HIGH, HIGH}, bLast[3] = {HIGH, HIGH, HIGH};
uint32_t bT[3] = {0, 0, 0};
bool btnPressed(uint8_t k) {
  bool r = digitalRead(BTN_PIN[k]); uint32_t now = millis();
  if (r != bLast[k]) { bLast[k] = r; bT[k] = now; }
  if (r != bStable[k] && now - bT[k] >= 30) { bStable[k] = r; return r == LOW; }
  return false;
}
void buttonsTask() {
  if (btnPressed(0) && !faultMode) { page = (page + 1) % NPAGES; tPage = millis(); forceRender = true; }
  if (btnPressed(1)) ackPressed();
  if (btnPressed(2)) { if (!fTest) { fTest = true; logEvent("FAULT", "manual test fault"); updateFaultMode(); } }
}

// ---------- Display task: page timer + refresh timer + flush ----------
void displayTask() {
  static uint32_t tRender = 0;
  uint32_t now = millis();
  if (!faultMode && now - tPage >= PAGE_MS) {
    page = (page + 1) % NPAGES; tPage = now; forceRender = true;
    logEvent("PAGE", "-> %d", page + 1);
  }
  if (forceRender || now - tRender >= REFRESH_MS) {
    tRender = now; forceRender = false; renderFrame();
  }
  flush(CHAR_BUDGET);
}

// ---------- LEDs and buzzer ----------
void outputsTask() {
  uint32_t now = millis();
  bool r = faultMode, w = !faultMode && isWarning(), g = !faultMode && !w;
  digitalWrite(RED_LED, r); digitalWrite(YELLOW_LED, w); digitalWrite(GREEN_LED, g);
  digitalWrite(BUZZER, faultMode && (now % 1000) < 150);
}

void setup() {
  Serial.begin(115200);
  pinMode(RED_LED, OUTPUT); pinMode(GREEN_LED, OUTPUT); pinMode(YELLOW_LED, OUTPUT);
  pinMode(BUZZER, OUTPUT); pinMode(RELAY, OUTPUT);
  pinMode(BTN_NEXT, INPUT_PULLUP); pinMode(BTN_ACK, INPUT_PULLUP); pinMode(BTN_TEST, INPUT_PULLUP);
  lcd.init(); lcd.backlight();                    // init clears once; never again
  digitalWrite(RELAY, LOW);
  for (uint8_t r = 0; r < ROWS; r++) { for (uint8_t c = 0; c < COLS; c++) shadow[r][c] = ' '; shadow[r][COLS] = 0; }
  sampleTask();
  renderFrame(); flush(CELLS);
  logEvent("BOOT", "=== FLICKER-FREE LCD ENGINE STARTED ===");
}

void loop() {
  static uint32_t tSample = 0, tStat = 0, lastUs = 0;
  uint32_t now = millis(), us = micros();
  if (lastUs) {
    uint32_t dt = us - lastUs;
    if (dt > maxLoopUs) maxLoopUs = dt;
    if (dt > 50000) stalls++;
  }
  lastUs = us;

  if (now - tSample >= SAMPLE_MS) {
    tSample += SAMPLE_MS;
    if (now - tSample > 5 * SAMPLE_MS) tSample = now;
    sampleTask();
  }
  buttonsTask();
  displayTask();
  outputsTask();

  if (now - tStat >= STAT_MS) {
    tStat = now;
    float saved = naiveChars ? 100.0 * (1.0 - (float)charsWritten / (float)naiveChars) : 0;
    logEvent("STAT", "page=%d fault=%d renders=%lu written=%lu naive=%lu moves=%lu saved=%.1f%% maxLoop=%luus stalls=%lu",
             page + 1, faultMode, (unsigned long)renders, (unsigned long)charsWritten, (unsigned long)naiveChars,
             (unsigned long)cursorMoves, saved, (unsigned long)maxLoopUs, (unsigned long)stalls);
  }
}
