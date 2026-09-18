#pragma once

void startNetworkServices();
void startNetworkTask();
// Runs on the main loop; performs any pending display work owned by that core.
void handleNetworkDisplay();
// True once when the device is online and the setup screen may give way.
bool consumeScoreboardRelease();

bool isOnline();
void getPreferredTeamIds(int outTeamIds[3]);
bool isClockDisplayEnabled();

