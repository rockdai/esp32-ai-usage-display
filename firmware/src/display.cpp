// Wraps Waveshare's vendored DisplayPort BSP (display_bsp.{h,cpp}) and exposes
// a 400x300 landscape 1-bpp LGFX_Sprite as the drawing canvas. Each
// displayCommit() walks the sprite, threshold pixels, and pushes them through
// DisplayPort::RLCD_SetPixel (LUT-accelerated tile packing) followed by
// RLCD_Display() which emits CASET/RASET/RAMWR over SPI.
//
// Pin map and init quirks come from
// docs/superpowers/notes/2026-04-26-lcd-driver.md (T2 research).

#include <Arduino.h>
#include "display.h"
#include "display_bsp.h"

// Pins per T2 notes: SCK=11, MOSI=12, DC=5, CS=40, RST=41 (TE=6 unused).
static constexpr int PIN_MOSI = 12;
static constexpr int PIN_SCK  = 11;
static constexpr int PIN_DC   = 5;
static constexpr int PIN_CS   = 40;
static constexpr int PIN_RST  = 41;

// Landscape geometry per spec (B1 layout 400x300).
static constexpr int CANVAS_W = 400;
static constexpr int CANVAS_H = 300;

// DisplayPort is heap-allocated so we control construction order (it talks to
// the SPI driver in its ctor, which must run after Arduino's runtime is up).
static DisplayPort* g_panel  = nullptr;
static LGFX_Sprite  g_canvas;

void displayInit() {
  Serial.println("[lcd] init begin");
  g_panel = new DisplayPort(PIN_MOSI, PIN_SCK, PIN_DC, PIN_CS, PIN_RST,
                            CANVAS_W, CANVAS_H, SPI3_HOST);
  g_panel->RLCD_Init();
  Serial.println("[lcd] panel init done");

  g_canvas.setColorDepth(1);
  g_canvas.setPaletteColor(0, 0xFFFFFFu);  // off pixel = white background
  g_canvas.setPaletteColor(1, 0x000000u);  // ink = black
  if (!g_canvas.createSprite(CANVAS_W, CANVAS_H)) {
    Serial.println("[lcd] createSprite FAILED");
    return;
  }
  g_canvas.fillScreen(0);

  // Draw "OK" before the first commit.
  g_canvas.setTextColor(1);
  g_canvas.setTextSize(3);
  g_canvas.setCursor(20, 20);
  g_canvas.print("OK");

  displayCommit();
  Serial.println("[lcd] first frame committed");
}

LGFX_Sprite& displayCanvas() { return g_canvas; }

void displayCommit() {
  if (!g_panel) return;
  // RLCD_ColorClear(ColorWhite) sets every panel byte to 0xFF (all-white) so
  // that pixels we never touch in this loop come up as background. We could
  // also leave the previous frame intact and only diff, but for correctness
  // (no ghosting from prior frames) wipe + redraw.
  g_panel->RLCD_ColorClear(ColorWhite);
  for (int cy = 0; cy < CANVAS_H; ++cy) {
    for (int cx = 0; cx < CANVAS_W; ++cx) {
      // Sprite stores 1bpp; readPixelValue returns the raw palette index 0/1.
      uint8_t v = g_canvas.readPixelValue(cx, cy);
      // Convention in display_bsp.h: ColorBlack=0, ColorWhite=0xFF. A canvas
      // "ink" (palette index 1, i.e. black on screen) maps to ColorBlack.
      uint8_t panel_color = v ? ColorBlack : ColorWhite;
      g_panel->RLCD_SetPixel(static_cast<uint16_t>(cx),
                             static_cast<uint16_t>(cy),
                             panel_color);
    }
  }
  g_panel->RLCD_Display();
}
