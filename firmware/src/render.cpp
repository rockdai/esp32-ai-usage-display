// B1 landscape 400x300 layout renderer (spec section 7).
//
// Drawing is done into the 1-bpp LGFX_Sprite owned by display.cpp. Palette
// index 0 is white (background), 1 is black (ink). Use the literals BG/INK
// directly with set*Color/fillRect/etc. — TFT_BLACK/TFT_WHITE are color-mode
// constants and will not match.
//
// Layout sections (y coordinates):
//   0..44    header strip (CLAUDE CODE size 4 fake-bold left + Max 5x size 2 right,
//                          text at y=8 — 8 px breathing room above; separator at y=44)
//   62..121  5h block (label/tokens size 2 at y, bar 18px tall at y+19 (3px gap above),
//                      meta size 2 at y+45 (8px gap below bar))
//   164..223 weekly  (same shape as 5h)
//   270      footer separator (pushed to bottom)
//   280..    footer text (size 2)

#include <Arduino.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <LovyanGFX.hpp>
#include "render.h"
#include "display.h"

static LGFX_Sprite* d = nullptr;
static constexpr uint8_t INK = 1;
static constexpr uint8_t BG  = 0;

void renderInit() {
  d = &displayCanvas();
}

// ---- formatters ------------------------------------------------------------

// Tokens -> "1.0M" / "342K" / "0".
static void formatTokens(uint64_t v, char* out, size_t n) {
  if (v >= 1000000ULL) {
    // one decimal place in millions; integer math to avoid floating ambiguity.
    uint64_t whole = v / 1000000ULL;
    uint64_t frac  = (v % 1000000ULL) / 100000ULL;  // 0..9
    snprintf(out, n, "%llu.%lluM",
             (unsigned long long)whole, (unsigned long long)frac);
  } else if (v >= 1000ULL) {
    uint64_t whole = v / 1000ULL;
    snprintf(out, n, "%lluK", (unsigned long long)whole);
  } else {
    snprintf(out, n, "%llu", (unsigned long long)v);
  }
}

// Duration in seconds -> "1d 4h" / "4h 30m" / "15m".
// Drops zero-prefixed segments. <= 0 -> "0m".
static void fmtDuration(uint32_t secs, char* out, size_t n) {
  uint32_t days  = secs / 86400U;
  uint32_t hours = (secs % 86400U) / 3600U;
  uint32_t mins  = (secs % 3600U) / 60U;
  if (days > 0) {
    snprintf(out, n, "%ud %uh", (unsigned)days, (unsigned)hours);
  } else if (hours > 0) {
    snprintf(out, n, "%uh %um", (unsigned)hours, (unsigned)mins);
  } else {
    snprintf(out, n, "%um", (unsigned)mins);
  }
}

// ---- header ---------------------------------------------------------------

static void drawHeader(const UsageData& s, bool wifi_ok) {
  d->fillRect(0, 0, 400, 44, BG);
  d->setTextColor(INK);

  // "CLAUDE CODE" size-4 left-aligned, fake-bold via 1px x-offset double-print.
  // Top edge at y=8 (8 px breathing room above the text).
  d->setTextSize(4);
  d->setCursor(8, 8);
  d->print("CLAUDE CODE");
  int x_after = d->getCursorX();
  d->setCursor(9, 8);
  d->print("CLAUDE CODE");

  // "Max 5x" size-2 right-aligned, bottom-aligned with size-4 (size-4 spans
  // y=8..39; size-2 16 px tall starting at y=22 spans y=22..37 — visually
  // bottom-aligned with the title's lowercase baseline). Clamped so it
  // cannot collide with the title.
  const char* plan = (s.valid && s.plan[0] != '\0') ? s.plan : "Max 5x";
  d->setTextSize(2);
  int pw = d->textWidth(plan);
  int px = 392 - pw;
  if (px < x_after + 12) px = x_after + 12;
  d->setCursor(px, 22);
  d->print(plan);

  if (!wifi_ok) {
    // Small "WiFi?" indicator in the top-right corner above the plan text
    // (which is at y=22..37). At size 1 the marker is 8 px tall × 30 px
    // wide, fitting comfortably in the y=0..15 strip at the top right.
    d->setTextSize(1);
    d->setCursor(360, 2);
    d->print("WiFi?");
  }

  // Separator at y=44
  d->drawFastHLine(0, 44, 400, INK);
}

// ---- meta builders --------------------------------------------------------

static void buildMeta5h(const UsageData& s,
                        char* left, size_t lsz,
                        char* right, size_t rsz,
                        uint32_t now) {
  left[0] = '\0';
  size_t pos = 0;
  bool first = true;
  if (s.cost_5h_present) {
    pos += snprintf(left + pos, (lsz > pos) ? (lsz - pos) : 0,
                    "$%.2f", s.cost_5h_usd);
    first = false;
  }
  if (s.msgs_5h_present) {
    pos += snprintf(left + pos, (lsz > pos) ? (lsz - pos) : 0,
                    "%s%u msg", first ? "" : " . ", (unsigned)s.msgs_5h);
    first = false;
  }
  // burn rate intentionally omitted from the meta line: cost + msgs +
  // reset countdown together already exceed the 384 px bar width at size 2,
  // and the time-progress bar conveys "how fast" implicitly.
  (void)pos;

  right[0] = '\0';
  if (s.reset_5h > now) {
    char dur[24];
    fmtDuration(s.reset_5h - now, dur, sizeof(dur));
    snprintf(right, rsz, "resets in %s", dur);
  }
}

static void buildMetaWeekly(const UsageData& s,
                            char* left, size_t lsz,
                            char* right, size_t rsz,
                            uint32_t now) {
  left[0] = '\0';
  size_t pos = 0;
  bool first = true;
  if (s.cost_weekly_present) {
    pos += snprintf(left + pos, (lsz > pos) ? (lsz - pos) : 0,
                    "$%.2f", s.cost_weekly_usd);
    first = false;
  }
  if (s.msgs_weekly_present) {
    pos += snprintf(left + pos, (lsz > pos) ? (lsz - pos) : 0,
                    "%s%u msg", first ? "" : " . ", (unsigned)s.msgs_weekly);
    first = false;
  }
  (void)pos;

  right[0] = '\0';
  if (s.reset_weekly > now) {
    char dur[24];
    fmtDuration(s.reset_weekly - now, dur, sizeof(dur));
    snprintf(right, rsz, "resets in %s", dur);
  }
}

// ---- window (5h block / weekly) ------------------------------------------

static void drawWindow(int y, const char* label, uint64_t used,
                       uint32_t started, uint32_t resets, uint32_t now,
                       const char* meta_left, const char* meta_right) {
  // Label, size-2 (16 px tall), top-left at (8, y).
  d->setTextColor(INK);
  d->setTextSize(2);
  d->setCursor(8, y);
  d->print(label);

  // Big tokens, size-2 (matches label height), right-aligned at right edge.
  char tok[24];
  formatTokens(used, tok, sizeof(tok));
  d->setTextSize(2);
  int tw = d->textWidth(tok);
  int tx = 392 - tw;
  if (tx < 8) tx = 8;
  d->setCursor(tx, y);
  d->print(tok);

  // Bar at y+25 (9 px gap below the size-2 label at y..y+15), 23 px tall.
  int barY = y + 25;
  d->drawRect(8, barY, 384, 24, INK);

  uint32_t total = (resets > started) ? (resets - started) : 0;
  uint32_t elapsed;
  if (total == 0) {
    elapsed = 0;
  } else if (now <= started) {
    elapsed = 0;
  } else if (now >= resets) {
    elapsed = total;
  } else {
    elapsed = now - started;
  }
  // Bar drawn row-by-row with drawFastHLine + per-pixel edges. Avoids
  // LovyanGFX fillRect entirely (in case its 1-bpp path skips or
  // misaligns rows on the LGFX_Sprite). Each pixel that should be INK
  // is set explicitly via drawFastHLine or drawPixel — no bulk-fill
  // shortcuts are taken.
  const int barX = 8, barW = 384, barH = 23;
  int fillW = 0;
  if (total > 0) {
    uint64_t num = (uint64_t)elapsed * (uint64_t)barW;
    fillW = (int)(num / (uint64_t)total);
    if (fillW > barW) fillW = barW;
    if (fillW < 0)    fillW = 0;
  }
  // Outline top + bottom (full bar width).
  d->drawFastHLine(barX, barY,              barW, INK);
  d->drawFastHLine(barX, barY + barH - 1,   barW, INK);
  // For each interior row: set the left edge, the fill (if any), and the right edge.
  for (int row = barY + 1; row <= barY + barH - 2; ++row) {
    d->drawPixel(barX, row, INK);                    // left outline
    d->drawPixel(barX + barW - 1, row, INK);         // right outline
    if (fillW > 1) {
      d->drawFastHLine(barX + 1, row, fillW - 1, INK);
    }
  }

  // Meta at y+57 size-2 (16 px tall): 9 px gap below the bar (which ended
  // at y+47), matching the 9 px gap above the bar. Symmetric padding.
  d->setTextSize(2);
  if (meta_left && meta_left[0]) {
    d->setCursor(8, y + 57);
    d->print(meta_left);
  }
  if (meta_right && meta_right[0]) {
    int rw = d->textWidth(meta_right);
    int rx = 392 - rw;
    if (rx < 8) rx = 8;
    d->setCursor(rx, y + 57);
    d->print(meta_right);
  }
}

// ---- footer ---------------------------------------------------------------

static void drawFooter(const UsageData& s, bool stale, uint32_t ms_since_post) {
  // Separator at y=270 (pushed to bottom)
  d->drawFastHLine(0, 270, 400, INK);

  d->setTextColor(INK);
  d->setTextSize(2);

  if (s.today_present) {
    char tok[24];
    formatTokens(s.tok_today, tok, sizeof(tok));
    char left[64];
    // No "Today:" prefix — the footer position implies "today's totals".
    // With cost too the line gets too wide for stale + ago on the right.
    snprintf(left, sizeof(left), "%s tok . %u msg",
             tok, (unsigned)s.msgs_today);
    d->setCursor(8, 280);
    d->print(left);
  }

  // Right side: "!STALE Nm" or "Nm" (compact form so it fits with the left
  // text at any reasonable today value).
  uint32_t mins = ms_since_post / 60000UL;
  char right[48];
  if (stale) {
    snprintf(right, sizeof(right), "!STALE %um", (unsigned)mins);
  } else {
    snprintf(right, sizeof(right), "%um ago", (unsigned)mins);
  }
  int rw = d->textWidth(right);
  int rx = 392 - rw;
  if (rx < 8) rx = 8;
  d->setCursor(rx, 280);
  d->print(right);
}

// ---- waiting screen -------------------------------------------------------

static void drawWaiting() {
  d->setTextColor(INK);
  d->setTextSize(2);
  const char* msg = "Waiting for first sync...";
  int w = d->textWidth(msg);
  int x = (400 - w) / 2;
  if (x < 0) x = 0;
  d->setCursor(x, 140);
  d->print(msg);
}

// ---- top-level tick -------------------------------------------------------

// === DIAGNOSTIC MODE ===
// Set to 1 to draw 4 solid 18-tall INK blocks at known Y positions and
// nothing else. Used to determine whether the "white stripe" artifact
// follows the bars (= our render code) or stays at fixed absolute Y
// positions (= panel hardware quirk).
#define RENDER_DIAG_BARS 0

#if RENDER_DIAG_BARS
static void drawDiagBlock(int y, const char* label) {
  // 18-tall solid INK block, 384 wide, drawn pixel by pixel via
  // drawFastHLine for each row. No outline carve, no fill carve — pure
  // 18*384 = 6912 explicitly-set INK pixels.
  for (int row = 0; row < 18; ++row) {
    d->drawFastHLine(8, y + row, 384, INK);
  }
  // Tiny size-1 label to the right of the bar (so we can identify which
  // bar in the photo). Drawn AFTER the bar so it overprints; placed at
  // the right edge below the bar's top.
  d->setTextColor(INK);
  d->setTextSize(1);
  d->setCursor(2, y + 5);
  d->print(label);
}
#endif

void renderTick(const UsageData& s, bool stale, bool wifi_ok,
                uint32_t ms_since_post) {
  if (!d) return;

#if RENDER_DIAG_BARS
  d->fillScreen(BG);
  // Five blocks at varied Y positions, including the EXACT 5H and WEEKLY
  // bar positions used in the full layout (81 and 183). If only the
  // 183-block shows the artifact, the panel has a row-specific quirk
  // there; if all clean, the artifact comes from the surrounding render
  // (text overflow into bar area, layout interaction, etc).
  drawDiagBlock( 20, "A");   // top
  drawDiagBlock( 81, "B");   // exactly where 5H bar sits
  drawDiagBlock(140, "C");   // mid (gap above weekly's normal Y)
  drawDiagBlock(183, "D");   // exactly where WEEKLY bar sits
  drawDiagBlock(245, "E");   // bottom
  displayCommit();
  return;
#endif

  d->fillScreen(BG);
  drawHeader(s, wifi_ok);

  if (!s.valid) {
    drawWaiting();
    displayCommit();
    return;
  }

  uint32_t now = s.ts;  // we have no NTP; treat the payload's `ts` as "now"

  char meta_l[80], meta_r[48];
  buildMeta5h(s, meta_l, sizeof(meta_l), meta_r, sizeof(meta_r), now);
  drawWindow(62, "5H BLOCK", s.tok_5h, s.started_5h, s.reset_5h, now,
             meta_l, meta_r);

  buildMetaWeekly(s, meta_l, sizeof(meta_l), meta_r, sizeof(meta_r), now);
  drawWindow(164, "WEEKLY", s.tok_weekly, s.started_weekly, s.reset_weekly,
             now, meta_l, meta_r);

  drawFooter(s, stale, ms_since_post);
  displayCommit();
}
