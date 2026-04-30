# KEY button pin verification (Waveshare ESP32-S3-RLCD-4.2)

**Date**: 2026-04-30
**Status**: deferred — physical KEY button NOT present on the user's board

## TL;DR (post-e2e update)

The user's actual board has only BOOT (no second user button), even though the
Waveshare reference docs and demo source describe a third "KEY" button on
GPIO 18. This is likely a board-revision difference. The KEY-dismiss feature
is therefore **disabled** in v1.0; clearance happens via SessionEnd hook or
the 15-min timeout. The research below stays in the repo for future use:
if the user solders a tactile button to GPIO 18 (or a different pin), the
software side can be re-enabled by reverting the deletion of `firmware/src/key.{h,cpp}`
and the corresponding wires in `main.cpp` (see commit history).

## Original research (kept for future reference)

## Method

1. Read board overview from the official Waveshare wiki:
   - <https://docs.waveshare.net/ESP32-S3-RLCD-4.2/>
   - Confirms three buttons: BOOT (download-mode), PWR (power), KEY ("自定义功能按键" — customizable function button). No GPIO numbers on that page.
2. Attempted to parse the official schematic PDF:
   - <https://www.waveshare.net/w/upload/e/e6/ESP32-S3-RLCD-4.2-schematic.pdf>
   - WebFetch cannot decode compressed Altium PDF binary; schematic content was not readable as text.
3. Found authoritative GPIO assignment in the official Waveshare GitHub demo repo:
   - `button_bsp.c`:
     <https://github.com/waveshareteam/ESP32-S3-RLCD-4.2/blob/main/02_Example/Arduino/07_Audio_Test/src/ExternLib/button/button_bsp.c>
     explicitly defines `BOOT_KEY_PIN 0` and `GP18_KEY_PIN 18`, both active-low.
   - `07_Audio_Test.ino` (same repo):
     <https://github.com/waveshareteam/ESP32-S3-RLCD-4.2/blob/main/02_Example/Arduino/07_Audio_Test/07_Audio_Test.ino>
     names the FreeRTOS task that monitors `GP18ButtonGroups` as **`KEY_LoopTask`** —
     directly linking the board silkscreen label "KEY" to GPIO 18.
4. Cross-checked GPIO 18 against all known pin assignments for this board —
   no conflict found:
   - LCD: GPIO 5 (DC), 11 (SCK), 12 (MOSI), 40 (CS), 41 (RST), 6 (TE)
   - I2C: GPIO 13 (SDA), 14 (SCL)
   - SD card: GPIO 21 (CMD), 38 (CLK), 39 (D0)
   - GPIO 18 is used by no other peripheral.
5. The factory program `user_config.h` (`02_Example/ESP-IDF/10_FactoryProgram/main/user_config.h`)
   lists only LCD and I2C pins — no button pins — consistent with buttons being
   configured at runtime in the BSP layer rather than as compile-time constants.

## Result

| Button | GPIO | Notes |
|--------|------|-------|
| BOOT   | 0    | Reserved — do not use as user input (strapping pin; enters download mode when held LOW at power-on) |
| PWR    | —    | Connected to power-management logic; not mapped to an ESP32 GPIO user-input path |
| **KEY**| **18** | User-dismiss button — used by `key.cpp` |

**KEY = GPIO 18, active-low** (external pull-up; the BSP sets `INPUT_PULLUP` logic equivalent via `active level = 0` in `multi_button`).

## Configuration (for Task 9)

```cpp
// firmware/src/key.cpp
constexpr int kKeyPin = 18;

void keyInit() {
    pinMode(kKeyPin, INPUT_PULLUP);
    // ...
}
```

- `pinMode(18, INPUT_PULLUP)` — internal pull-up; button shorts to GND when pressed
- Active-low: pressed = digital LOW
- 30 ms software debounce (matches the 5 ms polling period used in the Waveshare demo's `clock_task_callback`)

## Probe sketch (for hardware confirmation before Task 9)

The GPIO 18 assignment is **strongly indicated** by official Waveshare demo source code.
However, the schematic PDF was not text-readable, so the user may wish to run the
probe sketch below to confirm on actual hardware before wiring up `key.cpp`.

**To run on actual hardware:**

1. Save the current `firmware/src/main.cpp` content elsewhere:
   ```bash
   cp firmware/src/main.cpp firmware/src/main.cpp.bak
   ```
2. Replace `firmware/src/main.cpp` with the probe content below.
3. Flash:
   ```bash
   cd firmware && /Users/rock/.platformio/penv/bin/pio run -e rlcd42 -t upload
   ```
4. Monitor:
   ```bash
   /Users/rock/.platformio/penv/bin/pio device monitor
   ```
5. Press BOOT, PWR, KEY in turn — note which GPIO toggles.
   - BOOT should toggle GPIO 0.
   - KEY should toggle GPIO 18 (per the demo code finding above).
6. Restore main.cpp:
   ```bash
   cp firmware/src/main.cpp.bak firmware/src/main.cpp
   ```

```cpp
#include <Arduino.h>

// Probe: configure all plausible button GPIOs as INPUT_PULLUP and report
// state on every change. Press each labeled button to identify pin.
static const int kPins[] = {0, 1, 2, 3, 14, 15, 16, 17, 18, 21, 35, 36, 37, 38, 39, 40};
static int kLast[sizeof(kPins)/sizeof(kPins[0])];

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("[probe] press buttons; expect a HIGH->LOW edge on the key's GPIO");
  for (size_t i = 0; i < sizeof(kPins)/sizeof(kPins[0]); ++i) {
    pinMode(kPins[i], INPUT_PULLUP);
    kLast[i] = digitalRead(kPins[i]);
  }
}

void loop() {
  for (size_t i = 0; i < sizeof(kPins)/sizeof(kPins[0]); ++i) {
    int v = digitalRead(kPins[i]);
    if (v != kLast[i]) {
      Serial.printf("GPIO%2d: %d -> %d\n", kPins[i], kLast[i], v);
      kLast[i] = v;
    }
  }
  delay(5);
}
```

Expected output when pressing KEY: `GPIO18:  1 -> 0` (press) then `GPIO18:  0 -> 1` (release).
