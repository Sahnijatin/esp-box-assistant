#pragma once
#include <stdbool.h>
// Downloads and installs firmware from url, then reboots.
// Returns false only if the update failed (the box keeps running the current version).
bool ota_run(const char *url, const char *version);
