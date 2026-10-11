#pragma once
#include <stdbool.h>
// Tap-to-talk question and answer loop
void assistant_start(void);
void assistant_tap(void);     // called from the UI button
bool assistant_busy(void);
