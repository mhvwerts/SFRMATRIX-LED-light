/*
  LED PWM driver with serial command interface and momentary switch
  Board:  Arduino Uno / Nano (ATmega328P)
  Output: pin 9 (Timer1, OC1A) -> 1k -> base of 2N3904
  Input:  pin 2 <- momentary switch (hardware-debounced, see below)
  PWM:    ~31.4 kHz (Timer1, prescaler 1, 8-bit phase-correct)
  Serial: 115200 baud, line-based, terminated by \n or \r\n

  Commands (case-insensitive):
    SET <0..255>   set duty cycle, e.g. "SET 128"
    <0..255>       bare number, same as SET (handy in Serial Monitor)
    PCT <0..100>   set duty in percent, e.g. "PCT 20"
    GET            report current level (also "?")
    HELP           print command list

  Replies (one line each):
    OK <level>     command accepted, <level> is the value now active
    ERR <reason>   command rejected, level unchanged

  Momentary switch (works without any host application; the serial port
  stays silent when the switch is used, a host can simply ask with GET):
    Tap  (< HOLD_MS)       toggle: LED on -> off, LED off -> back to the
                           remembered level
    Hold (>= HOLD_MS)      after the threshold the level ramps along the
                           brightness curve for as long as the switch is
                           held, and stays where it was on release.
                           - LED on:  alternately ramps up, then down,
                                      on successive holds (down goes all
                                      the way to 0 = off)
                           - LED off: ramps up, starting at level 1
                           The ramp clamps at 255 (or 0) and never wraps.
                           If the level is already at an end, the ramp
                           goes the other way.
    A level set over serial is also the level that a tap restores.
    A serial command that arrives while the switch is held cancels the
    button action until the switch is released.

  Switch circuit (debounced in hardware, so no software debounce here):
    +5V -- R1 10k -- node -- D2
    node -- C1 1uF -- GND                  (tantalum: + towards node)
    node -- R2 220 -- switch -- GND
  The RC filter delays the release edge by roughly 9 ms. Irrelevant for
  the time scales used here.

  Note: pin 10 shares Timer1 and will also run at this frequency.
        Do not combine with the Servo library or tone().
*/

const uint8_t  PWM_PIN       = 9;
const uint8_t  BUTTON_PIN    = 2;          // external pull-up: pressed = LOW
const uint8_t  DEFAULT_LEVEL = 20;         // ~8 % (20/255 = 7.8 %)
const uint32_t BAUD          = 115200;

// Button timing
const uint32_t HOLD_MS      = 1000;        // press longer than this = hold
const uint32_t RAMP_MS      = 8000;        // full ramp 0..255 (or back)
const uint32_t RAMP_STEP_MS = 20;          // how often the ramp updates

uint8_t level  = DEFAULT_LEVEL;            // level currently output
uint8_t lastOn = DEFAULT_LEVEL;            // level restored by a tap when off

char    lineBuf[24];
uint8_t lineLen  = 0;
bool    overflow = false;

// ---------------------------------------------------------------------------
// Brightness curve
//
// The ramp moves a position x in 0..1 at constant speed, and the curve maps
// x to a PWM level. Perceived brightness is roughly logarithmic, so an
// exponential curve makes the light seem to brighten at a steady rate
// (with RAMP_MS = 8000 about one doubling per second at the bright end).
//
//   level = (255^x - 1) * 255/254
//
// The offset makes x = 0 give exactly level 0 and x = 1 give exactly 255.
// To use another curve, replace this function pair (they must be inverses
// of each other). For a linear ramp:
//   levelFromX: return (uint8_t)(x * 255.0f + 0.5f);
//   xFromLevel: return lv / 255.0f;
// ---------------------------------------------------------------------------
const float LN_MAX = 5.5412635f;           // ln(255)

uint8_t levelFromX(float x) {
  float f = (expf(x * LN_MAX) - 1.0f) * (255.0f / 254.0f);
  if (f < 0.0f)   f = 0.0f;
  if (f > 255.0f) f = 255.0f;
  return (uint8_t)(f + 0.5f);
}

float xFromLevel(uint8_t lv) {
  return logf(1.0f + lv * (254.0f / 255.0f)) / LN_MAX;
}

// ---------------------------------------------------------------------------
// Level handling
// ---------------------------------------------------------------------------
enum BtnState : uint8_t {
  BTN_IDLE,          // switch released, nothing going on
  BTN_PRESSED,       // pressed, waiting to see whether it's a tap or a hold
  BTN_RAMPING,       // held past HOLD_MS, level follows the ramp
  BTN_WAIT_RELEASE   // button action cancelled, ignore until released
};
BtnState btn = BTN_IDLE;

// Write the level to the output. Used directly by the ramp, which must not
// touch lastOn on every step.
void applyLevel(uint8_t v) {
  level = v;
  analogWrite(PWM_PIN, level);
}

// Set a level on purpose (serial command or tap). A non-zero level becomes
// the one a later tap restores, and a button action in progress is cancelled.
void setLevel(uint8_t v) {
  applyLevel(v);
  if (v > 0) lastOn = v;
  if (btn != BTN_IDLE) btn = BTN_WAIT_RELEASE;
}

void replyOk() {
  Serial.print(F("OK "));
  Serial.println(level);
}

void replyErr(const __FlashStringHelper *msg) {
  Serial.print(F("ERR "));
  Serial.println(msg);
}

// Parse an integer that must span the whole string (trailing spaces allowed).
bool parseInt(const char *s, long &out) {
  if (!*s) return false;
  char *end;
  out = strtol(s, &end, 10);
  if (end == s) return false;
  while (*end == ' ') end++;
  return *end == '\0';
}

void printHelp() {
  Serial.println(F("Commands: SET <0..255> | <0..255> | PCT <0..100> | GET | HELP"));
}

void handleLine(char *s) {
  while (*s == ' ') s++;               // skip leading spaces
  if (!*s) return;                     // ignore empty lines
  for (char *p = s; *p; p++) *p = toupper(*p);

  long v;

  if (!strcmp(s, "GET") || !strcmp(s, "?")) {
    replyOk();
  } else if (!strcmp(s, "HELP")) {
    printHelp();
  } else if (!strncmp(s, "PCT ", 4)) {
    if (!parseInt(s + 4, v))            replyErr(F("bad number"));
    else if (v < 0 || v > 100)          replyErr(F("range 0..100"));
    else { setLevel((uint8_t)((v * 255 + 50) / 100)); replyOk(); }
  } else {
    const char *num = !strncmp(s, "SET ", 4) ? s + 4 : s;
    if (num == s && !(isdigit(*s) || *s == '-' || *s == '+')) {
      replyErr(F("unknown command"));
    } else if (!parseInt(num, v)) {
      replyErr(F("bad number"));
    } else if (v < 0 || v > 255) {
      replyErr(F("range 0..255"));
    } else {
      setLevel((uint8_t)v);
      replyOk();
    }
  }
}

// ---------------------------------------------------------------------------
// Momentary switch
// ---------------------------------------------------------------------------
uint32_t pressMs     = 0;      // when the switch went down
uint32_t rampStartMs = 0;      // when the current ramp began
uint32_t lastStepMs  = 0;      // last ramp update
float    rampX0      = 0.0f;   // ramp position at rampStartMs
bool     rampUp      = true;   // direction of the current/next ramp

void onTap() {
  if (level > 0) {
    lastOn = level;            // remember what to come back to
    applyLevel(0);
  } else {
    setLevel(lastOn);
  }
}

void startRamp(uint32_t now) {
  if (level == 0)        { rampUp = true;  applyLevel(1); }   // start at 1
  else if (level == 255) { rampUp = false; }                  // already at top
  rampX0      = xFromLevel(level);
  rampStartMs = lastStepMs = now;
  btn = BTN_RAMPING;
}

void stepRamp(uint32_t now) {
  float x = rampX0 + (rampUp ? 1.0f : -1.0f) * (float)(now - rampStartMs) / RAMP_MS;
  if (x < 0.0f) x = 0.0f;
  if (x > 1.0f) x = 1.0f;
  applyLevel(levelFromX(x));
  lastStepMs = now;
}

void endRamp() {
  // Keep the reached level for a later tap, unless the ramp ended at 0:
  // then a tap brings back the level from before the ramp.
  if (level > 0) lastOn = level;
  rampUp = !rampUp;            // the next hold goes the other way
  btn = BTN_IDLE;
}

// Call on every pass of loop(). Never blocks.
void buttonUpdate() {
  bool     down = (digitalRead(BUTTON_PIN) == LOW);
  uint32_t now  = millis();

  switch (btn) {
    case BTN_IDLE:
      if (down) { btn = BTN_PRESSED; pressMs = now; }
      break;

    case BTN_PRESSED:
      if (!down) {                              // released early: a tap
        btn = BTN_IDLE;
        onTap();
      } else if (now - pressMs >= HOLD_MS) {
        startRamp(now);
      }
      break;

    case BTN_RAMPING:
      if (!down)                               endRamp();
      else if (now - lastStepMs >= RAMP_STEP_MS) stepRamp(now);
      break;

    case BTN_WAIT_RELEASE:
      if (!down) btn = BTN_IDLE;
      break;
  }
}

// ---------------------------------------------------------------------------
void setup() {
  pinMode(PWM_PIN, OUTPUT);
  pinMode(BUTTON_PIN, INPUT);                      // external pull-up (R1)
  applyLevel(DEFAULT_LEVEL);                       // starts Timer1 PWM at the default level
  TCCR1B = (TCCR1B & 0b11111000) | 0x01;           // prescaler 1 -> ~31.4 kHz

  Serial.begin(BAUD);
  Serial.println(F("READY LED-PWM 31.4kHz"));
  printHelp();
  replyOk();
}

void loop() {
  buttonUpdate();

  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (overflow) {
        replyErr(F("line too long"));
      } else if (lineLen > 0) {
        lineBuf[lineLen] = '\0';
        handleLine(lineBuf);
      }
      lineLen  = 0;
      overflow = false;
    } else if (lineLen < sizeof(lineBuf) - 1) {
      lineBuf[lineLen++] = c;
    } else {
      overflow = true;                             // discard until end of line
    }
  }
}
