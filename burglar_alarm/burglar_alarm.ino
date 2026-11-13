/*
  ==========================================================================
  ESP32 DevKit V1 Burglar Alarm
  ==========================================================================
  Board  : "DOIT ESP32 DEVKIT V1" (or "ESP32 Dev Module") in Arduino IDE
  Library: "Keypad" by Mark Stanley / Alexander Brevig (Library Manager)
  Core   : esp32 by Espressif, 2.0.x or 3.x (uses tone()/noTone())

  --------------------------------------------------------------------------
  OPERATION
  --------------------------------------------------------------------------
  DISARMED    : Enter the code then '#'  ->  EXIT_DELAY.   ("9713*" then "#")
  EXIT_DELAY  : Piezo beeps for 20 s (faster in the last 5 s). Entering the
                code cancels arming. At the end BOTH inputs must be LOW
                (door shut, no motion), otherwise -> ALARM, else -> ARMED.
  ARMED       : Reed opens  -> ENTRY_DELAY (30 s, configurable).
                PIR active  -> ALARM immediately (configurable).
                Entering the code -> DISARMED.
  ENTRY_DELAY : Piezo beeps fast. Enter the code before it ends or -> ALARM.
  ALARM       : Relay HIGH (external sounder on) for 10 minutes.
                Entering the code silences it -> DISARMED.
                After 10 minutes the relay drops and the system re-arms
                (configurable). Any sensor still active at that moment is
                ignored until it goes LOW again, so an open door can't
                re-trigger the siren forever.

  Code entry: the code ends in '*', so '*' can't be used as "clear".
  When disarmed, '#' is the "enter/arm" key. When armed, the code is matched
  as soon as the last digit is typed (no '#' needed); '#' clears the buffer.
  The key buffer also clears after 10 s with no key presses.

  --------------------------------------------------------------------------
  PIN MAP (all chosen to avoid strapping pins 0/2/5/12/15 for inputs,
  flash pins 6-11, and input-only pins 34-39 that have no internal pull-up)
  --------------------------------------------------------------------------
    Keypad ROW1..ROW4 : GPIO 32, 33, 25, 26   (Keypad lib enables pull-ups)
    Keypad COL1..COL3 : GPIO 27, 14, 13
    Reed switch       : GPIO 18  (INPUT_PULLUP, closed = LOW = secure)
    PIR output        : GPIO 19  (active HIGH)
    Relay driver      : GPIO 23  -> NPN base resistor
    Piezo             : GPIO 22
    Status LED        : GPIO 2   (on-board blue LED; output only, safe)

  --------------------------------------------------------------------------
  WIRING NOTES
  --------------------------------------------------------------------------
  Keypad (typical 7-pin 3x4 membrane, pins left to right as printed):
    Many are COL2, ROW1, COL1, ROW4, COL3, ROW3, ROW2 -- others are
    R1 R2 R3 R4 C1 C2 C3. Check with a multimeter (press a key, find which
    two pins short) and adjust ROW_PINS / COL_PINS below.

  Relay via NPN (2N2222 / BC337 / BC547):
    GPIO23 --[1 kΩ]--+-- Base
                     |
                  [10 kΩ] to GND  (holds the transistor OFF while the
                                   ESP32 boots and the pin floats)
    Emitter -> GND
    Collector -> relay coil (-)
    Relay coil (+) -> 5 V (VIN pin when USB powered, or a separate 5 V)
    Flyback diode (1N4148 / 1N4007) ACROSS THE COIL: cathode (band) to +5 V,
    anode to collector. Without it the coil's back-EMF kills the transistor.
    1 kΩ gives ~2.6 mA base current -> fine for a 5 V coil up to ~150 mA.
    Use a 5 V relay MODULE instead? Most already have the transistor+diode;
    check whether it is active-HIGH or active-LOW and set RELAY_ON_LEVEL.
    Keep all grounds common.

  Reed switch (normally closed when the magnet is next to it):
    GPIO18 -> reed -> GND. Internal pull-up is ~45 kΩ, which is weak for a
    long cable run: add an external 10 kΩ pull-up to 3.3 V and a 100 nF cap
    to GND at the ESP32 end. Fail-safe: a cut wire reads HIGH = "open" and
    will trigger the alarm.

  PIR (HC-SR501 or AM312):
    The ESP32 GPIO is NOT 5 V tolerant -- the PIR output must be 3.3 V.
    AM312: runs natively at 3.3 V, power from the 3V3 pin.
    HC-SR501: its output is already 3.3 V (on-board regulator), so you can
      power it from 5 V/VIN safely; or run it at 3.3 V by feeding the "H"
      jumper pin / regulator output (bypassing the regulator).
    HC-SR501 settings: jumper on "H" (repeat trigger), time pot fully
    anticlockwise (~3 s hold) so it has reset before the exit delay ends.
    PIRs need ~30-60 s to settle after power-up; see PIR_WARMUP_MS.

  Piezo:
    Passive piezo: GPIO22 --[100 Ω]-- piezo -- GND (PIEZO_IS_PASSIVE = true).
    Active buzzer (has its own oscillator): set PIEZO_IS_PASSIVE = false.
    For more volume, drive it through a second NPN like the relay.
  ==========================================================================
*/

#include <Arduino.h>
#include <Keypad.h>
#include <string.h>

// ==========================================================================
// USER SETTINGS -- change these
// ==========================================================================
const char ALARM_CODE[] = "9713*";           // up to 16 chars, may include * (not #)

const uint32_t EXIT_DELAY_MS      = 20UL * 1000;        // spec: 20 s
const uint32_t ENTRY_DELAY_MS     = 30UL * 1000;        // time to disarm after door opens
const uint32_t ALARM_DURATION_MS  = 10UL * 60 * 1000;   // spec: 10 minutes
const uint32_t KEY_BUFFER_TIMEOUT = 10UL * 1000;        // clear half-typed codes
const uint32_t PIR_WARMUP_MS      = 30UL * 1000;        // PIR ignored after power-up

const bool ENTRY_DELAY_ENABLED     = true;   // false: reed opening while ARMED = instant alarm
const bool PIR_TRIGGERS_WHEN_ARMED = true;   // false: PIR only checked at end of exit delay
const bool PIR_USES_ENTRY_DELAY    = false;  // true: PIR starts the entry delay instead of instant alarm
const bool CODE_SILENCES_ALARM     = true;   // false: siren runs the full 10 min regardless
const bool REARM_AFTER_ALARM       = true;   // false: go to DISARMED after the 10 min
const bool PIEZO_DURING_ALARM      = true;   // piezo warbles alongside the relay siren

const bool     PIEZO_IS_PASSIVE = true;      // false for an active (self-oscillating) buzzer
const uint16_t PIEZO_FREQ_HZ    = 2700;      // passive piezos are loudest near resonance
const uint16_t PIEZO_FREQ_LOW   = 1800;      // second tone for the alarm warble / error beep

const uint8_t RELAY_ON_LEVEL = HIGH;         // spec: relay HIGH = alarm sounder on

// ==========================================================================
// PINS
// ==========================================================================
const uint8_t PIN_REED  = 18;
const uint8_t PIN_PIR   = 19;
const uint8_t PIN_RELAY = 23;
const uint8_t PIN_PIEZO = 22;
const uint8_t PIN_LED   = 2;

const byte KP_ROWS = 4;
const byte KP_COLS = 3;
byte ROW_PINS[KP_ROWS] = {32, 33, 25, 26};
byte COL_PINS[KP_COLS] = {27, 14, 13};
char KEYMAP[KP_ROWS][KP_COLS] = {
  {'1', '2', '3'},
  {'4', '5', '6'},
  {'7', '8', '9'},
  {'*', '0', '#'}
};
Keypad keypad = Keypad(makeKeymap(KEYMAP), ROW_PINS, COL_PINS, KP_ROWS, KP_COLS);

// ==========================================================================
// DEBOUNCED INPUT
// ==========================================================================
struct DebouncedInput {
  uint8_t  pin;
  uint32_t debounceMs;
  bool     stable;
  bool     lastRaw;
  uint32_t lastChange;

  DebouncedInput(uint8_t p, uint32_t db) : pin(p), debounceMs(db), stable(false), lastRaw(false), lastChange(0) {}

  void begin(uint8_t mode) {
    pinMode(pin, mode);
    lastRaw = digitalRead(pin) == HIGH;
    stable = lastRaw;
    lastChange = millis();
  }
  void update(uint32_t now) {
    bool raw = digitalRead(pin) == HIGH;
    if (raw != lastRaw) {
      lastRaw = raw;
      lastChange = now;
    } else if (raw != stable && (now - lastChange) >= debounceMs) {
      stable = raw;
    }
  }
  bool high() const { return stable; }   // HIGH = door open / motion = "not secure"
};

DebouncedInput reed(PIN_REED, 50);
DebouncedInput pir(PIN_PIR, 100);

// ==========================================================================
// STATE MACHINE
// ==========================================================================
enum AlarmState : uint8_t { DISARMED, EXIT_DELAY, ARMED, ENTRY_DELAY, ALARM };
const char* const STATE_NAMES[] = {"DISARMED", "EXIT_DELAY", "ARMED", "ENTRY_DELAY", "ALARM"};

AlarmState state = DISARMED;
uint32_t   stateEnteredAt = 0;

// After an alarm times out and re-arms, a sensor that is still active is
// bypassed until it restores (goes LOW), then it is live again.
bool reedBypassed = false;
bool pirBypassed  = false;

// ==========================================================================
// KEY BUFFER
// ==========================================================================
const uint8_t KEY_BUF_SIZE = 16;
char     keyBuf[KEY_BUF_SIZE + 1];
uint8_t  keyLen = 0;
uint32_t lastKeyAt = 0;

void clearKeys() { keyLen = 0; keyBuf[0] = '\0'; }

void pushKey(char k) {
  if (keyLen == KEY_BUF_SIZE) {               // keep the newest keys
    memmove(keyBuf, keyBuf + 1, KEY_BUF_SIZE - 1);
    keyLen--;
  }
  keyBuf[keyLen++] = k;
  keyBuf[keyLen] = '\0';
}

bool bufferEndsWithCode() {
  size_t n = strlen(ALARM_CODE);
  return keyLen >= n && memcmp(keyBuf + keyLen - n, ALARM_CODE, n) == 0;
}

// ==========================================================================
// PIEZO (non-blocking): priority = feedback pattern > key click > state pattern
// ==========================================================================
struct Feedback {
  uint8_t  remaining = 0;   // beeps left
  uint16_t onMs = 0, offMs = 0, freq = 0;
  uint32_t stepAt = 0;
  bool     on = false;
} fb;

uint32_t clickUntil = 0;
bool     piezoIsOn = false;
uint16_t piezoFreq = 0;

void startFeedback(uint8_t count, uint16_t onMs, uint16_t offMs, uint16_t freq) {
  fb.remaining = count; fb.onMs = onMs; fb.offMs = offMs; fb.freq = freq;
  fb.stepAt = millis(); fb.on = true;
}
void beepOK()    { startFeedback(2, 80, 80, PIEZO_FREQ_HZ); }
void beepError() { startFeedback(1, 600, 0, PIEZO_FREQ_LOW); }
void beepDisarm(){ startFeedback(3, 60, 60, PIEZO_FREQ_HZ); }

// Returns true and sets freq if the feedback pattern wants sound now.
bool feedbackSound(uint32_t now, uint16_t& freq) {
  if (fb.remaining == 0) return false;
  uint32_t dur = fb.on ? fb.onMs : fb.offMs;
  if (now - fb.stepAt >= dur) {
    fb.stepAt = now;
    if (fb.on) {
      fb.on = false;
      if (--fb.remaining == 0) return false;
    } else {
      fb.on = true;
    }
  }
  freq = fb.freq;
  return fb.on;
}

void setPiezo(bool on, uint16_t freq) {
  if (on == piezoIsOn && (!on || freq == piezoFreq)) return;   // avoid re-configuring every loop
  if (PIEZO_IS_PASSIVE) {
    if (on) tone(PIN_PIEZO, freq); else noTone(PIN_PIEZO);
  } else {
    digitalWrite(PIN_PIEZO, on ? HIGH : LOW);
  }
  piezoIsOn = on;
  piezoFreq = freq;
}

// Simple on/off pattern helper based on time since the state started.
bool pattern(uint32_t elapsed, uint16_t onMs, uint16_t periodMs) {
  return (elapsed % periodMs) < onMs;
}

void updatePiezo(uint32_t now) {
  uint16_t freq = PIEZO_FREQ_HZ;
  bool on = false;
  uint32_t elapsed = now - stateEnteredAt;

  if (feedbackSound(now, freq)) {
    on = true;
  } else if ((int32_t)(clickUntil - now) > 0) {
    on = true;
    freq = PIEZO_FREQ_HZ;
  } else {
    switch (state) {
      case EXIT_DELAY:
        // slow beep, then fast for the last 5 s as a warning
        on = (EXIT_DELAY_MS - elapsed > 5000) ? pattern(elapsed, 100, 1000)
                                              : pattern(elapsed, 100, 250);
        break;
      case ENTRY_DELAY:
        on = pattern(elapsed, 150, 500);
        break;
      case ALARM:
        if (PIEZO_DURING_ALARM) {
          on = true;
          freq = ((elapsed / 250) & 1) ? PIEZO_FREQ_LOW : PIEZO_FREQ_HZ;   // warble
        }
        break;
      default:
        break;
    }
  }
  setPiezo(on, freq);
}

void updateLed(uint32_t now) {
  uint32_t elapsed = now - stateEnteredAt;
  bool on = false;
  switch (state) {
    case DISARMED:    on = false; break;
    case EXIT_DELAY:  on = pattern(elapsed, 250, 500); break;
    case ARMED:       on = pattern(elapsed, 100, 2000); break;   // short blip = armed
    case ENTRY_DELAY: on = pattern(elapsed, 125, 250); break;
    case ALARM:       on = true; break;
  }
  digitalWrite(PIN_LED, on ? HIGH : LOW);
}

// ==========================================================================
// TRANSITIONS
// ==========================================================================
void setRelay(bool alarmOn) {
  digitalWrite(PIN_RELAY, alarmOn ? RELAY_ON_LEVEL : !RELAY_ON_LEVEL);
}

void enterState(AlarmState s) {
  Serial.printf("[%10lu] %s -> %s\n", (unsigned long)millis(), STATE_NAMES[state], STATE_NAMES[s]);
  state = s;
  stateEnteredAt = millis();
  clearKeys();
  setRelay(s == ALARM);
}

bool pirReady(uint32_t now) { return now >= PIR_WARMUP_MS; }
bool reedOpen()             { return reed.high(); }
bool pirMotion(uint32_t now){ return pirReady(now) && pir.high(); }

// ==========================================================================
// KEY HANDLING
// ==========================================================================
void handleKey(char k, uint32_t now) {
  clickUntil = now + 40;     // key click
  lastKeyAt = now;

  if (state == DISARMED) {
    if (k == '#') {
      if (bufferEndsWithCode()) {
        Serial.println("Code OK - arming");
        beepOK();
        enterState(EXIT_DELAY);
      } else {
        Serial.println("Wrong code");
        beepError();
        clearKeys();
      }
    } else {
      pushKey(k);
    }
    return;
  }

  // Any other state: '#' just clears, the code is matched as soon as it is typed.
  if (k == '#') {
    clearKeys();
    return;
  }
  pushKey(k);
  if (!bufferEndsWithCode()) return;

  if (state == ALARM && !CODE_SILENCES_ALARM) {
    Serial.println("Code entered but CODE_SILENCES_ALARM is false");
    beepError();
    clearKeys();
    return;
  }
  Serial.println("Code OK - disarmed");
  beepDisarm();
  reedBypassed = pirBypassed = false;
  enterState(DISARMED);
}

// ==========================================================================
// STATE LOGIC
// ==========================================================================
void updateStateMachine(uint32_t now) {
  uint32_t elapsed = now - stateEnteredAt;

  switch (state) {
    case DISARMED:
      break;

    case EXIT_DELAY:
      if (elapsed >= EXIT_DELAY_MS) {
        bool r = reedOpen();
        bool p = pirMotion(now);
        if (r || p) {
          Serial.printf("Exit delay ended with input HIGH (reed=%d pir=%d)\n", r, p);
          enterState(ALARM);
        } else {
          reedBypassed = pirBypassed = false;
          enterState(ARMED);
        }
      }
      break;

    case ARMED: {
      // Clear bypasses once the sensor restores.
      if (reedBypassed && !reedOpen())     { reedBypassed = false; Serial.println("Reed restored - live"); }
      if (pirBypassed  && !pirMotion(now)) { pirBypassed  = false; Serial.println("PIR restored - live"); }

      bool r = !reedBypassed && reedOpen();
      bool p = PIR_TRIGGERS_WHEN_ARMED && !pirBypassed && pirMotion(now);

      if (r) {
        Serial.println("Reed opened");
        enterState(ENTRY_DELAY_ENABLED ? ENTRY_DELAY : ALARM);
      } else if (p) {
        Serial.println("PIR motion");
        enterState(PIR_USES_ENTRY_DELAY && ENTRY_DELAY_ENABLED ? ENTRY_DELAY : ALARM);
      }
      break;
    }

    case ENTRY_DELAY:
      // Sensors are ignored here: the occupant is walking in. Only the timer matters.
      if (elapsed >= ENTRY_DELAY_MS) {
        Serial.println("Entry delay expired");
        enterState(ALARM);
      }
      break;

    case ALARM:
      if (elapsed >= ALARM_DURATION_MS) {
        Serial.println("Alarm duration reached");
        if (REARM_AFTER_ALARM) {
          reedBypassed = reedOpen();
          pirBypassed  = pirMotion(now);
          if (reedBypassed) Serial.println("Reed still open - bypassed until it closes");
          if (pirBypassed)  Serial.println("PIR still active - bypassed until it clears");
          enterState(ARMED);   // enterState() clears only the key buffer, bypass flags persist
        } else {
          enterState(DISARMED);
        }
      }
      break;
  }
}

// ==========================================================================
// SETUP / LOOP
// ==========================================================================
void setup() {
  // Drive outputs to a safe state first.
  pinMode(PIN_RELAY, OUTPUT);
  setRelay(false);
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);
  pinMode(PIN_PIEZO, OUTPUT);
  digitalWrite(PIN_PIEZO, LOW);

  Serial.begin(115200);
  delay(50);   // one-off at boot only, lets the serial monitor attach

  reed.begin(INPUT_PULLUP);
  pir.begin(INPUT);          // PIR drives the line; add INPUT_PULLDOWN if yours floats when idle
  keypad.setDebounceTime(20);

  clearKeys();
  stateEnteredAt = millis();
  Serial.println();
  Serial.println("ESP32 burglar alarm ready - DISARMED");
  Serial.printf("PIR warm-up: %lu s\n", (unsigned long)(PIR_WARMUP_MS / 1000));
  beepOK();
}

void loop() {
  uint32_t now = millis();

  reed.update(now);
  pir.update(now);

  char k = keypad.getKey();
  if (k) handleKey(k, now);

  if (keyLen && (now - lastKeyAt) >= KEY_BUFFER_TIMEOUT) clearKeys();

  updateStateMachine(now);
  // Fresh timestamp: a transition above may have set stateEnteredAt after 'now'.
  uint32_t outNow = millis();
  updatePiezo(outNow);
  updateLed(outNow);
}
