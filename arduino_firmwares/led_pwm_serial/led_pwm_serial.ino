/*
  LED PWM driver with serial command interface
  Board:  Arduino Uno / Nano (ATmega328P)
  Output: pin 9 (Timer1, OC1A) -> 1k -> base of 2N3904
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

  Note: pin 10 shares Timer1 and will also run at this frequency.
        Do not combine with the Servo library or tone().
*/

const uint8_t PWM_PIN       = 9;
const uint8_t DEFAULT_LEVEL = 50;      // ~20 % (50/255 = 19.6 %)
const uint32_t BAUD         = 115200;

uint8_t level = DEFAULT_LEVEL;

char    lineBuf[24];
uint8_t lineLen  = 0;
bool    overflow = false;

void applyLevel(uint8_t v) {
  level = v;
  analogWrite(PWM_PIN, level);
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
    else { applyLevel((uint8_t)((v * 255 + 50) / 100)); replyOk(); }
  } else {
    const char *num = !strncmp(s, "SET ", 4) ? s + 4 : s;
    if (num == s && !(isdigit(*s) || *s == '-' || *s == '+')) {
      replyErr(F("unknown command"));
    } else if (!parseInt(num, v)) {
      replyErr(F("bad number"));
    } else if (v < 0 || v > 255) {
      replyErr(F("range 0..255"));
    } else {
      applyLevel((uint8_t)v);
      replyOk();
    }
  }
}

void setup() {
  pinMode(PWM_PIN, OUTPUT);
  applyLevel(DEFAULT_LEVEL);                       // starts Timer1 PWM at ~20 %
  TCCR1B = (TCCR1B & 0b11111000) | 0x01;           // prescaler 1 -> ~31.4 kHz

  Serial.begin(BAUD);
  Serial.println(F("READY LED-PWM 31.4kHz"));
  printHelp();
  replyOk();
}

void loop() {
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
