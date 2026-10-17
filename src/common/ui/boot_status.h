#pragma once

// Boot status page: shown for BOOT_SETUP_PAGE_MS once the network
// connection is made (a firmware-update check and the first game data
// load behind it on core 0). Redraws only when connectivity changes.
// deviceName ("Baseball Scoreboard") comes from the sport branding.
void renderBootStatusPage(const char* deviceName);
