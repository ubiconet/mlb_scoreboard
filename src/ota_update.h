#pragma once

#include <Arduino.h>

// Firmware self-update against the releases/manifest.json in the GitHub
// repo (written by `pio run -t deploy`). Runs entirely on the core-0 data
// task: fetch manifest, compare version with FIRMWARE_VERSION, and when
// they differ download + flash the new binary while the renderer shows the
// "updating, do not turn off" screen (see handleOtaUpdateScreen()).
void serviceOtaUpdates();

// True while an update check is between the downloading and reboot stages —
// the data task skips its normal feed fetches in that window so the TLS
// download never competes for heap or airtime.
bool otaUpdateInProgress();
