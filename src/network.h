#pragma once

void startNetworkServices();
void startNetworkTask();
// Runs on the main loop; performs any pending display work owned by that core.
void handleNetworkDisplay();
// True once when the device is online and the setup screen may give way.
bool consumeScoreboardRelease();

bool isOnline();
// True while the device runs its own setup AP with no usable saved Wi-Fi —
// the boot UI shows the "connect to the scoreboard" page instead of the
// status page in that state.
bool isProvisioning();
// Saved Wi-Fi SSID ("" when none) and the device's current IP ("" while
// not online) — read by the boot status page.
const char* getSavedWifiSsid();
String getDeviceIp();
void getPreferredTeamIds(int outTeamIds[3]);
bool isClockDisplayEnabled();

