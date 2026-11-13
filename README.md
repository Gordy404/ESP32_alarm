# ESP32_alarm

| | |
|---|---|
| **Author** | Gordon ([Gordy404](https://github.com/Gordy404)) |
| **Version** | 1.0.0 |
| **Date** | 2026-11-13 00:57 GMT (UK) |

The version is also in `FW_VERSION` / `FW_DATE` at the top of the sketch and is printed on the serial monitor at boot.

Burglar alarm firmware for the ESP32 DevKit V1 (Arduino IDE), with a 3x4 membrane keypad, a reed switch, a PIR, a relay-driven external sounder, and a piezo for local feedback.

## Build

1. In Arduino IDE, install the **esp32 by Espressif** core (2.0.x or 3.x).
2. Install the **Keypad** library by Mark Stanley / Alexander Brevig from the Library Manager.
3. Open `burglar_alarm/burglar_alarm.ino`, select **DOIT ESP32 DEVKIT V1** (or **ESP32 Dev Module**) and upload.
4. Serial monitor at 115200 baud logs every state change.

## Operation

| State | What happens | Leaves when |
|---|---|---|
| DISARMED | Idle. | Code then `#` (default `9713*#`) → EXIT_DELAY |
| EXIT_DELAY | Piezo beeps for 20 s, faster in the last 5 s. | Code → DISARMED. At 20 s: reed or PIR HIGH → ALARM, else → ARMED |
| ARMED | Watching sensors. | Reed opens → ENTRY_DELAY. PIR → ALARM. Code → DISARMED |
| ENTRY_DELAY | Piezo beeps fast for 30 s. | Code → DISARMED. Timeout → ALARM |
| ALARM | Relay HIGH (siren on) for 10 min, piezo warbles. | Code → DISARMED. After 10 min → ARMED, with any still-active sensor bypassed until it restores |

Because the code ends in `*`, `*` can't be a clear key. When disarmed, `#` is "enter". When armed, the code is matched as soon as it is typed, and `#` clears the buffer. Half-typed codes clear after 10 s.

## Pin map

| Function | GPIO | Notes |
|---|---|---|
| Keypad ROW1..ROW4 | 32, 33, 25, 26 | Library enables internal pull-ups |
| Keypad COL1..COL3 | 27, 14, 13 | |
| Reed switch | 18 | `INPUT_PULLUP`, closed = LOW = secure |
| PIR output | 19 | Active HIGH, must be 3.3 V logic |
| Relay driver | 23 | Via NPN, HIGH = siren on |
| Piezo | 22 | `tone()` for passive piezo |
| Status LED | 2 | On-board LED |

Avoided: strapping pins 0, 2 (used only as an LED output), 5, 12, 15; flash pins 6 to 11; input-only pins 34 to 39, which have no internal pull-ups.

## Wiring

**Relay via NPN (2N2222, BC337, BC547)**

```
GPIO23 --[1k]--+-- Base           Relay coil (+) -- 5V (VIN)
               |                  Relay coil (-) -- Collector
            [10k]                 Emitter        -- GND
               |                  Diode 1N4148/1N4007 across the coil,
              GND                 band (cathode) to +5V
```

- 1 kΩ gives about 2.6 mA base current, enough for a 5 V coil up to about 150 mA.
- The 10 kΩ pull-down keeps the relay off while the ESP32 boots and the pin floats.
- The flyback diode is mandatory: the coil's back-EMF will destroy the transistor without it.
- A ready-made relay module already has the transistor and diode. Check whether it is active-HIGH or active-LOW and set `RELAY_ON_LEVEL`.
- Keep all grounds common.

**Reed switch (normally closed with the magnet present)**: GPIO18 to reed to GND. For long cable runs add an external 10 kΩ pull-up to 3.3 V and 100 nF to GND at the ESP32 end. A cut wire reads HIGH, so it fails safe into an alarm.

**PIR**: ESP32 inputs are not 5 V tolerant. An AM312 runs from 3.3 V directly. An HC-SR501's output is already 3.3 V because of its on-board regulator, so it can be powered from 5 V, or run at 3.3 V by feeding the regulator output. Set its jumper to H (repeat trigger) and the time pot to minimum (about 3 s) so it has reset by the end of the exit delay. The firmware ignores the PIR for the first 30 s after power-up while it settles.

**Keypad**: 7-pin membrane pinouts vary. A common order is COL2, ROW1, COL1, ROW4, COL3, ROW3, ROW2. Confirm with a multimeter and edit `ROW_PINS` and `COL_PINS`.

**Piezo**: passive piezo through 100 Ω to GND. For an active buzzer set `PIEZO_IS_PASSIVE = false`.

## Settings (top of the sketch)

| Constant | Default | Meaning |
|---|---|---|
| `ALARM_CODE` | `"9713*"` | Up to 16 characters, may contain `*`, not `#` |
| `EXIT_DELAY_MS` | 20 s | Exit beeping before inputs are checked |
| `ENTRY_DELAY_MS` | 30 s | Time to disarm after the door opens |
| `ALARM_DURATION_MS` | 10 min | Siren on-time |
| `ENTRY_DELAY_ENABLED` | true | false makes the door an instant zone |
| `PIR_TRIGGERS_WHEN_ARMED` | true | false checks the PIR only at the end of the exit delay |
| `PIR_USES_ENTRY_DELAY` | false | true lets the PIR start the entry delay instead of instant alarm |
| `CODE_SILENCES_ALARM` | true | false runs the siren the full 10 min |
| `REARM_AFTER_ALARM` | true | false goes to DISARMED after the alarm |
| `PIEZO_DURING_ALARM` | true | Piezo warbles with the siren |
| `PIR_WARMUP_MS` | 30 s | PIR ignored after power-up |

## Change log

| Version | Date (UK) | Changes |
|---|---|---|
| 1.0.0 | 2026-11-13 00:57 GMT | First release: state machine, keypad code entry, exit/entry delays, 10 min relay alarm |
