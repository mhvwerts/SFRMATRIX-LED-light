const int BUTTON_PIN = 2;
const uint8_t QUEUE_SIZE = 8;              // events buffered between loop() runs

struct ButtonEvent {
  unsigned long pressMs;                   // millis() at press
  unsigned long durationMs;                // how long it was held
};

volatile ButtonEvent queue[QUEUE_SIZE];
volatile uint8_t head = 0;                 // written by ISR
volatile uint8_t tail = 0;                 // written by loop()
volatile uint8_t overflows = 0;

volatile bool isDown = false;
volatile unsigned long downMs = 0;

void onChange() {
  unsigned long now = millis();
  bool pressed = (digitalRead(BUTTON_PIN) == LOW);

  if (pressed && !isDown) {                // press edge
    isDown = true;
    downMs = now;
  } else if (!pressed && isDown) {         // release edge
    isDown = false;
    uint8_t next = (head + 1) % QUEUE_SIZE;
    if (next != tail) {
      queue[head].pressMs = downMs;
      queue[head].durationMs = now - downMs;
      head = next;
    } else {
      overflows++;                         // queue full, event dropped
    }
  }
  // Any other combination is a glitch (same level twice) and is ignored
}

void setup() {
  pinMode(BUTTON_PIN, INPUT);              // external pull-up (R1)
  Serial.begin(115200);
  attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), onChange, CHANGE);
  Serial.println("Ready. Press the button.");
}

void loop() {
  while (tail != head) {
    ButtonEvent e;
    noInterrupts();
    e.pressMs = queue[tail].pressMs;
    e.durationMs = queue[tail].durationMs;
    interrupts();
    tail = (tail + 1) % QUEUE_SIZE;

    Serial.print("Press at ");
    Serial.print(e.pressMs);
    Serial.print(" ms, held ");
    Serial.print(e.durationMs);
    Serial.println(" ms");
  }

  if (overflows) {
    Serial.println("Warning: event queue overflowed");
    overflows = 0;
  }
}
