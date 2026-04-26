# Waveshare ESP32-S3-RLCD-4.2 — LCD driver & LovyanGFX panel config

Date: 2026-04-26
Author: Task 2 of `docs/superpowers/plans/2026-04-26-esp32-ai-usage-display.md`
Hardware surveyed: Waveshare ESP32-S3-RLCD-4.2 (MCU `ESP32-S3-WROOM-1-N16R8`,
4.2" reflective LCD, 300×400, no touch, no backlight).

This note de-risks spec §11.3 ("can LovyanGFX drive this panel out of the
box"). The downstream consumer is `firmware/src/display.cpp` (Task 13).

**Headline finding (read this first):** the panel driver chip is **Sitronix
ST7305**, a 1-bit-per-pixel monochrome reflective driver with a non-standard
4-bits-per-byte tiled framebuffer layout. **LovyanGFX has no upstream
`Panel_ST7305`**, and the closest mono panel class (`Panel_SharpLCD`) uses a
different protocol (line-prefix, no register addressing) so it is **not** a
drop-in. Task 13 will therefore have two viable paths — see §6 / §7.

---

## 1. Source

| Fact | Source | Confidence |
|------|--------|-----------|
| Board overview, components | <https://docs.waveshare.net/ESP32-S3-RLCD-4.2/> | high |
| Resource index (demo zip, schematic, datasheets) | <https://docs.waveshare.net/ESP32-S3-RLCD-4.2/Resources-And-Documents> | high |
| Driver chip = ST7305 | Waveshare resources page lists `ST_7305_V0_2.pdf` as *the* LCD datasheet for this board; init sequence in demo code matches ST7305/ST7306 register set | high |
| ST7305 datasheet | <https://www.waveshare.net/w/upload/5/5d/ST_7305_V0_2.pdf> | high |
| Schematic PDF (`/tmp/rlcd-schematic.pdf`) | <https://www.waveshare.net/w/upload/e/e6/ESP32-S3-RLCD-4.2-schematic.pdf> | high |
| Demo zip (extracted to `/tmp/rlcd-demo/ESP32-S3-RLCD-4.2-Demo/`, ~169 MB, **not committed**) | <https://files.waveshare.net/wiki/ESP32-S3-RLCD-4.2/ESP32-S3-RLCD-4.2-Demo.zip> | high |
| GitHub mirror | <https://github.com/waveshareteam/ESP32-S3-RLCD-4.2> | high |
| Zephyr port (independent confirmation; calls it `sitronix,st7306`) | <https://docs.zephyrproject.org/latest/boards/waveshare/esp32s3_rlcd_4_2/doc/index.html> | medium (different chip name — see §2) |
| LovyanGFX panel inventory (no ST7305 driver exists) | <https://github.com/lovyan03/LovyanGFX/tree/master/src/lgfx/v1/panel> | high |

The most authoritative single file is the Arduino LVGL_V9 demo:
`/tmp/rlcd-demo/ESP32-S3-RLCD-4.2-Demo/01_Arduino/examples/09_LVGL_V9_Test/`
— it contains `display_bsp.cpp` (init sequence + SPI bring-up) and
`09_LVGL_V9_Test.ino` (pin numbers passed in by hand). The ESP-IDF
`10_FactoryProgram` example has the same code plus a `main/user_config.h`
that names every pin as a `#define` — that file is the cleanest reference
for the pin map.

---

## 2. Panel chip identification

**Driver chip: Sitronix ST7305** (datasheet on Waveshare's resource index).

Confidence: **high** that *the demo treats it as ST7305*. Note one wart:
Zephyr's mainline board file calls it `sitronix,st7306` — ST7305 and ST7306
are sibling parts in the same family with overlapping register sets. The
Waveshare-shipped init sequence matches the register names in the **ST7305**
datasheet (`0xD6` NVM Load, `0xD1` Booster, `0xC0` Gate Voltage, `0xB3/0xB4`
HPM/LPM gate waveforms, `0xB9/0xB8` Source/Panel settings, `0x36` Memory
Data Access Control, `0x3A` Data Format Select). For our purposes treat it
as **ST7305**, but if a register write misbehaves, double-check against the
ST7306 datasheet too.

Panel class: **monochrome (1 bpp), reflective, memory-in-pixel-style** but
*driven by a normal SPI register interface* (not the Sharp-LCD line-prefix
protocol). Has a TE (tearing effect) output pin.

Resolution exposed by the chip: 312×400 internally, **300×400 visible**
(the demo uses `start-column = 204` / column window `0x12..0x2A`, page
window `0x00..0xC7` → 200 bytes wide × 200 rows of "fat pixels"; the chip
packs **4 horizontal × 2 vertical = 8 pixels per byte**, so 300×400 visible
pixels → 75 byte-columns × 200 byte-rows = 15000 bytes of framebuffer).

---

## 3. SPI bus / pin map

Authoritative source: `02_ESP-IDF/10_FactoryProgram/main/user_config.h` in
the demo zip, cross-checked against the schematic LCD1 connector net labels.

```
LCD signal      ESP32-S3 GPIO    Notes
-----------     -------------    ---------------------------------------
SCK  (LCD_SCL)  GPIO11           SPI clock, up to 10 MHz in demo
MOSI (LCD_SDA)  GPIO12           4-wire SPI, MOSI only
MISO            -- (none)        ST7305 is write-only on SPI; tie to -1
DC   (LCD_RS)   GPIO5            data/command select
CS   (LCD_CS)   GPIO40
RST  (LCD_RESET)GPIO41           active-low; demo holds idle high w/ pull-up
TE              GPIO6            tearing-effect output from ST7305
                                 (demo doesn't actually use it; safe to
                                 leave as input or ignore)
BL  (backlight) -- (none)        reflective panel; no backlight rail. The
                                 LCD1 FPC has no backlight pin — confirmed
                                 from schematic.
```

SPI host: demo uses `SPI3_HOST` (a.k.a. HSPI / SPI peripheral instance 3 on
ESP32-S3; LovyanGFX exposes this as `SPI3_HOST` in
`Bus_SPI::config_t::spi_host`). Pixel clock 10 MHz, SPI mode 0,
`lcd_cmd_bits = 8`, `lcd_param_bits = 8`.

Reset timing observed in `display_bsp.cpp::RLCD_Reset`:
`high 50 ms → low 20 ms → high 50 ms`. LovyanGFX's default
(`5 ms low / 50 ms wait`) is too short for this part — see §5.

Schematic also shows the LCD1 FPC carries unconnected touchpanel pads
(`TP_VCC`, `TP_RESET`, `TP_INT`, `TP_SDA`, `TP_SCL`) — no touch chip is
populated. We can ignore these.

---

## 4. Color and orientation

- **Color depth**: 1 bpp (monochrome). The chip has a 4-grayscale mode but
  the demo configures 2-level B/W (`0x3A 0x11`). For a glanceable usage
  display this is fine; we keep B/W.
- **Color order**: not applicable in the RGB565 sense. Per Waveshare's
  enum: `ColorBlack = 0x00`, `ColorWhite = 0xFF` in the packed framebuffer.
- **Pixel packing**: non-standard. Each byte holds **4 pixels horizontally
  × 2 pixels vertically** with this bit order (from
  `RLCD_SetPortraitPixel`):
  `bit = 7 - (local_x * 2 + local_y)` where
  `index = (y/2) * (width/4) + (x/4)`. Translation: a LovyanGFX
  `getColor()` callback that returns RGB565 must be thresholded to 1 bpp
  and then scattered into this layout. **This is the single biggest
  reason `Panel_SharpLCD`'s code does not work as-is** — Sharp memory
  LCDs are 1 bpp linear, ST7305 is 1 bpp tiled.
- **Default chip rotation**: portrait, 300W × 400H. The board's mechanical
  orientation (per Waveshare product photos) is also portrait.
- **Our target rotation**: **landscape, 400W × 300H** (per spec §6/§13 —
  "B1 layout (400×300)"). Demo achieves this by passing `width=400,
  height=300` to `DisplayPort` and using `RLCD_SetLandscapePixel`, which
  remaps `(x, y) → (x, height-1-y)` and writes through the same
  4×2-tile encoder. The MADCTL write `0x36 0x48` in the demo selects a
  scan direction; when we wrap this in LovyanGFX we'll pass
  `lgfx::Panel_Device::config_t::offset_rotation = 0..3` and let our
  custom panel class handle the bit-scatter.

---

## 5. Init sequence quirks

Lifted verbatim from
`01_Arduino/examples/09_LVGL_V9_Test/display_bsp.cpp::RLCD_Init`. None of
these are part of any LovyanGFX `Panel_XXX::init_impl()` — every byte
below has to be sent manually, by either keeping Waveshare's
`DisplayPort` class or writing a `Panel_ST7305::init_impl()` that emits
this list.

```
RST high 50 ms → low 20 ms → high 50 ms       (custom — LovyanGFX default
                                                is too short)

0xD6  17 02         NVM Load Control
0xD1  01            Booster Enable
0xC0  11 04         Gate Voltage
0xC1  69 69 69 69   VSHP
0xC2  19 19 19 19   VSLP
0xC4  4B 4B 4B 4B   VSHN
0xC5  19 19 19 19   VSLN
0xD8  80 E9         (osc / gate timing)
0xB2  02            framerate
0xB3  E5 F6 05 46 77 77 77 77 76 45    HPM gate waveform
0xB4  05 46 77 77 77 77 76 45          LPM gate waveform
0xB7  13            (gate ctl)
0xB0  64            multiplex ratio (mux = 0x64 = 100)
0x11                Sleep Out
                    DELAY 200 ms ← REQUIRED, ST7305 boot
0xC9  00            (source ctl)
0x36  48            MADCTL — memory-data-access; demo uses 0x48 in this
                    pin orientation
0x3A  11            COLMOD — 1bpp data format
0xB9  20            source voltage = 0x00 (per DTS) — demo writes 0x20,
                    Zephyr DTS says 0x00. **Open question (§7).**
0xB8  29            panel settings = 0x29
0x21                Display Inversion ON (panel has `inversion-on` flag in
                    Zephyr DTS)
0x2A  12 2A         CASET = column 0x12 .. 0x2A
0x2B  00 C7         RASET = row   0x00 .. 0xC7
0x35  00            TE on, mode 0
0xD0  FF            (auto-power)
0x38                Idle Mode OFF
0x29                Display ON
```

After this, every framebuffer flush is:
```
0x2A  12 2A         CASET (same as init)
0x2B  00 C7         RASET (same as init)
0x2C  <15000 bytes> RAMWR — full-frame blit
```

Refresh rate: with SPI at 10 MHz and ~15000 bytes per frame, theoretical
peak ≈ 80 fps, but the panel itself updates at the framerate set by
`0xB2 02` (~32 Hz HPM mode). For this project we'll push at most a few Hz.

---

## 6. Proposed LovyanGFX panel config (skeleton)

There are two viable shapes for Task 13. **Option A is recommended** for
v1 because it's the smallest amount of new code; Option B is the "proper"
LovyanGFX way and worth doing later if we want LovyanGFX's font / sprite
machinery to render directly into the panel.

### Option A — keep Waveshare's `DisplayPort`, draw into a 1bpp scratch buffer with LovyanGFX `LGFX_Sprite`

```cpp
// firmware/src/display.cpp  (skeleton)
//
// We don't subclass LGFX_Device. Instead we use LovyanGFX purely as a
// software 1bpp canvas via LGFX_Sprite, then push to the panel using
// Waveshare's DisplayPort each frame.
//
// Pros: zero new C++ for the panel; init sequence is already debugged
//       upstream. Cons: no hardware-accelerated partial updates.

#include <LovyanGFX.hpp>
#include "display_bsp.h"   // copied verbatim from Waveshare demo

static DisplayPort g_panel(
  /*mosi=*/12, /*sclk=*/11, /*dc=*/5,
  /*cs  =*/40, /*rst =*/41,
  /*width=*/400, /*height=*/300);     // landscape

static LGFX_Sprite g_canvas;          // 1bpp framebuffer

void display_init() {
  g_panel.RLCD_Init();
  g_canvas.setColorDepth(lgfx::palette_1bit);
  g_canvas.createSprite(400, 300);
  g_canvas.fillSprite(TFT_WHITE);
}

void display_flush() {
  // walk the LGFX_Sprite, threshold, and call RLCD_SetPixel
  for (int y = 0; y < 300; ++y) {
    for (int x = 0; x < 400; ++x) {
      uint8_t bit = g_canvas.readPixel(x, y) ? ColorWhite : ColorBlack;
      g_panel.RLCD_SetPixel(x, y, bit);
    }
  }
  g_panel.RLCD_Display();
}
```

Note: with `AlgorithmOptimization == 3` (the default in the demo) the
`RLCD_SetPixel` call is a single LUT lookup — 400×300 = 120 000 calls
per full refresh, ~5 ms on ESP32-S3 @ 240 MHz. We refresh at most every
30 s (per spec §6), so this is fine.

### Option B — write a real `Panel_ST7305 : public lgfx::Panel_HasBuffer`

Skeleton — fill in `init_impl()` from §5 and `display()` from
`RLCD_Display`. Only worth doing if/when Option A's perf hurts.

```cpp
class LGFX_RLCD : public lgfx::LGFX_Device {
  lgfx::Bus_SPI    _bus;
  lgfx::Panel_ST7305 _panel;     // <-- new class to be written

public:
  LGFX_RLCD() {
    {
      auto cfg = _bus.config();
      cfg.spi_host   = SPI3_HOST;
      cfg.spi_mode   = 0;
      cfg.freq_write = 10'000'000;
      cfg.freq_read  =  4'000'000;   // unused; ST7305 has no MISO
      cfg.use_lock   = true;
      cfg.dma_channel= SPI_DMA_CH_AUTO;
      cfg.pin_sclk   = 11;
      cfg.pin_mosi   = 12;
      cfg.pin_miso   = -1;
      cfg.pin_dc     = 5;
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }
    {
      auto cfg = _panel.config();
      cfg.pin_cs           = 40;
      cfg.pin_rst          = 41;
      cfg.pin_busy         = -1;
      cfg.panel_width      = 300;     // chip-native (portrait)
      cfg.panel_height     = 400;
      cfg.offset_x         = 0x12 * 4;  // CASET start * 4 (4 px/byte)
      cfg.offset_y         = 0;
      cfg.offset_rotation  = 1;       // 1 = 90° CW → 400 × 300 landscape
      cfg.dummy_read_pixel = 0;
      cfg.dummy_read_bits  = 0;
      cfg.readable         = false;
      cfg.invert           = true;    // 0x21 inversion-on
      cfg.rgb_order        = false;   // mono — irrelevant
      cfg.dlen_16bit       = false;
      cfg.bus_shared       = false;
      _panel.config(cfg);
    }
    setPanel(&_panel);
  }
};
```

Then `Panel_ST7305` itself would inherit `Panel_HasBuffer`, allocate a
15 KB byte buffer in PSRAM (the demo does
`heap_caps_malloc(DisplayLen, MALLOC_CAP_SPIRAM)`), and override:

- `init_impl()` — emit the §5 sequence
- `setPixel`/`writeFillRectPreclipped` — write into the tiled byte
  layout from `RLCD_SetLandscapePixel`
- `display()` (whole-screen) and `displayPart()` — emit the
  `0x2A/0x2B/0x2C` window + RAMWR

This is ~250 lines of new C++. Defer to a follow-up unless Task 13
hits a wall.

---

## 7. Open questions

1. **`0xB9` (source voltage) — `0x20` vs `0x00`.** Waveshare's Arduino
   demo writes `0x20`; Zephyr's DTS says `<0x00>`. Both produce a working
   image on someone's bench but they differ. Stick with **`0x20` (demo
   value)** since that's what's been validated on this exact board.
2. **`0x36` MADCTL byte for our chosen rotation.** Demo writes `0x48` and
   then does software rotation in `RLCD_SetLandscapePixel`. We could try
   `0x68` / `0x28` to let the chip do the flip, but it's unverified —
   keep software rotation for v1.
3. **TE pin (GPIO6) usage.** The demo doesn't read it. If we later see
   tearing on partial updates, route TE → GPIO interrupt and gate
   `RLCD_Display` on it. Not needed for our 1 Hz tick (spec §13).
4. **Does `Panel_HasBuffer` cope with non-linear pixel packing?** Looking
   at Sharp's class, it assumes linear-bytes-per-row. If we go Option B
   we may need to override more of the base class than usual. Re-evaluate
   when implementing.
5. **Can the 8 MB PSRAM be assumed available at `display_init`?** Spec
   pins flash to N16R8 so yes, but if PSRAM init ever fails the
   `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)` call will return NULL and
   the board will assert. Should add a `if (!buf) ESP_LOGE(...)` guard.

---

## TL;DR for Task 13

- Driver chip is **ST7305** (mono, reflective, 1 bpp, weird 4×2-tiled
  framebuffer). LovyanGFX has no upstream driver for it.
- **Pins**: SCK=11, MOSI=12, DC=5, CS=40, RST=41, TE=6 (unused).
  No MISO, no backlight. SPI3_HOST, mode 0, 10 MHz.
- **Take Option A**: vendor in Waveshare's `display_bsp.{h,cpp}` (BSD-3
  per Waveshare wiki — verify license header before commit) and use
  `LGFX_Sprite` with `palette_1bit` as the canvas. Threshold and blit at
  whatever rate the renderer asks for.
- Reset is `50 ms H / 20 ms L / 50 ms H`. Sleep-Out (`0x11`) needs a
  **200 ms** wait before the next command.
- Init sequence in §5 is the canonical list — copy it verbatim, do not
  hand-edit.
