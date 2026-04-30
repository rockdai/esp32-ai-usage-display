#pragma once
#include <stdint.h>

// Configure the KEY GPIO as INPUT_PULLUP. Call once from setup().
void keyInit();

// Edge-triggered, debounced. Returns true exactly once per *clean* press.
// Call every loop() iteration. Internally samples every ~5 ms; requires
// 30 ms of stable LOW after a HIGH→LOW transition before declaring a press.
bool keyPressedSinceLastCall();
