#pragma once

#include <Arduino.h>

// Network service — Wi-Fi provisioning state machine, captive setup portal,
// NVS-backed preferences, and development (Arduino)OTA. Fully generic: the
// sport layer injects its branding and team-option list at
// startNetworkServices() time; nothing in here knows a sport. The only
// sport reach-outs are the display-test REQUEST flag (polled and executed
// by the sport itself) and the optional feed-diagnostics JSON hook wired
// in by the app shell.

struct NetworkBranding {
  const char* deviceName;  // e.g. "Baseball Scoreboard" — portal <title>/heading
  const char* apSsid;      // fallback provisioning AP network name
  const char* hostname;    // base hostname; a per-device suffix is appended
  const char* soundSettingLabel;  // portal sound-picker heading, e.g.
                                  // "Score Sounds" ("" hides the section)
};

// One selectable team in the portal's preferred-team dropdowns.
struct NetworkTeamOption {
  int id;
  const char* label;
};

// One selectable sound in the portal's sound dropdown (e.g. crowd cheers,
// bat crack). The sport supplies the ids/labels and maps ids to its own
// audio clips; an empty list hides the portal section entirely.
struct NetworkSoundOption {
  int id;
  const char* label;
};

void startNetworkServices(const NetworkBranding& branding,
                          const NetworkTeamOption* teamOptions,
                          size_t teamOptionCount,
                          const int defaultPreferredTeams[3],
                          const NetworkSoundOption* soundOptions = nullptr,
                          size_t soundOptionCount = 0,
                          int defaultSoundId = 0,
                          String (*statusExtraJson)() = nullptr);
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
// True while a connection attempt is in flight: joining the saved or newly
// entered Wi-Fi, for up to NETWORK_CONNECT_AND_PROBE_TIMEOUT_MS before the
// board falls back to setup mode. The shell shows the connecting screen.
bool isConnecting();
// True only while the board actually has its network link (online AND
// associated). False while connecting, in setup mode, and during a dropped
// link's reconnect grace — exactly what the network-down LEDs cover.
bool networkLinkUp();

// Why the current connection attempt started (the connecting screen's
// headline).
enum class ConnectReason : uint8_t {
  STARTUP,      // power-on with saved Wi-Fi
  LINK_LOST,    // the link dropped and stayed down past the grace window
  SETUP_RETRY,  // setup mode's periodic retry of the saved network
  NEW_NETWORK,  // credentials just saved in the portal
};

// Snapshot of the attempt in flight, for the connecting screen.
struct ConnectProgress {
  uint32_t attemptId;   // changes with every new attempt
  ConnectReason reason;
  char ssid[33];
  int wifiStatus;       // wl_status_t right now
  uint16_t retries;     // reconnects issued so far in this attempt
  uint32_t elapsedMs;
  uint32_t budgetMs;    // setup mode takes over when elapsed reaches it
};
// Fills `out` and returns true while connecting; false otherwise.
bool getConnectProgress(ConnectProgress& out);
// True while no favorite team is picked (all three slots "None") on a sport
// that offers teams — there's nothing to follow. Once the board has a
// network, the setup screen (device IP plus a QR to the portal) keeps the
// display until the portal saves a team; that includes reconnects, so the
// scoreboard never flashes up on a team-less board.
bool teamSetupPending();
// True while someone recently used the setup portal: background feed work
// pauses so the web server gets the core and the radio to itself.
bool portalEngaged();
// One-shot display-test trigger from the portal's "Run Display Test"
// button (POST /display/test). Returns true exactly once per request;
// the sport layer runs its display test cycle on the render core.
bool consumeDisplayTestRequest();

// Live panel-orientation picker (diagnostics page, bench tool): POST
// /display/orientation?mode=0..5 stores the request; the render core
// consumes it via this accessor and applies the mode through the panel
// layer, then repaints. -1 when no request is pending.
int consumePanelOrientationRequest();

// One-shot audio-test trigger from the portal's "Play Test Sound" button
// (POST /audio/test) or a sound-picker preview (POST /audio/test?id=N).
// Returns true exactly once per request; outId receives the requested
// sound id, or -1 when the request didn't name one (caller plays the
// saved selection). The no-id case must NOT collide with "no request".
bool consumeAudioTestRequest(int& outId);

// Selected sound id from the portal's sound picker (NVS "sound_id"; the
// injected option list's default when never saved). The sport layer maps
// the id to one of its clips.
int getSelectedSoundId();

// User preference: enable the audio output — score sounds and alerts
// (NVS "audio_en", default on).
bool isAudioEnabled();

// ---- Manual mode (portal "Manual Mode" page) ----
// Full manual override of the scoreboard outputs: every value below is
// set from the manual-control web page (POST /manual/set) and read by the
// render core. Opening /manual engages the mode; the page's exit button
// (/manual/exit) leaves it. Small ints/bools, benign races at worst.
bool isManualMode();
void setManualMode(bool on);
int  getManualHomeScore();      // 0..99
int  getManualAwayScore();      // 0..99
int  getManualInning();         // 1..99
bool getManualTopOfInning();    // true = top, false = bottom
int  getManualBalls();          // 0..3
int  getManualStrikes();        // 0..2
int  getManualOuts();           // 0..2

// Portal-triggered demo display mode (the diagnostics page's game demo).
bool isGameDemoMode();

// True while any portal-triggered display override (game demo, manual
// mode, or a pending display test) should own the screen even with no
// network connection — lets the diagnostics page's hardware checks run
// while the board is still provisioning (no Wi-Fi), so new builds can be
// bench-tested offline.
bool portalDisplayOverrideActive();

// Re-fire the last network boot screen (the AP instructions while
// provisioning, the team-setup screen while teamSetupPending()) after such
// an override releases the display.
void requestNetworkScreenRepaint();

// Saved Wi-Fi SSID ("" when none) and the device's current IP ("" while
// not online) — read by the boot status page.
const char* getSavedWifiSsid();
String getDeviceIp();
// The three preferred-team ids (NVS "team1..3"; 0 = slot unused).
void getPreferredTeamIds(int outTeamIds[3]);
// User preference: show the idle clock on the score matrices (NVS "show_clock").
bool isClockDisplayEnabled();
// Effective display timezone as a POSIX TZ string, selected in the setup
// portal (NVS "tz"); used for game times, countdowns, and the idle clock.
const char* getTzString();
