#pragma once
#include <LovyanGFX.hpp>

void displayInit();
LGFX_Sprite& displayCanvas();   // 400x300 1-bpp landscape
void displayCommit();           // blit canvas to panel
