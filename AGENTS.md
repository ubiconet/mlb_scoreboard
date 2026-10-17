# AGENTS.md — Operating instructions for AI coding assistants

This file describes how AI assistants should work on the **MLBScoreboard**
project. The goal: keep changes small, verifiable, and consistent with the
existing architecture; produce a build that an OTA-updated device can run
without surprises.

---

## 1. Project summary

- **Target hardware:** ESP32-S3 DevKitC-1 (4 MB flash).
- **Peripherals (MLB build):**
  - 2.0" ST7789 TFT, 320×240, **software-SPI** on pins
    `SCK=13, MOSI=12, CS=9, DC=10, RESET=11` (see the SPI gotcha note in
    `memories/repo/hardware_architecture.md` and in
    `src/common/hal/tft_panel.h`).
  - 2× MAX7219 8×8 LED matrices (DIN=14, CLK=8, CS=16).
  - 7 discrete count LEDs on GPIO 1–7 (balls/strikes/outs).
- **Firmware:** Arduino-ESP32 framework, PlatformIO build system.
- **Build envs:**
  - `esp32-s3-devkitc-1` → USB CDC upload.
  - `esp32-s3-devkitc-1-ota` → ArduinoOTA Wi-Fi upload.

### Template architecture (v2.59 reorg)

This repo is a **sport scoreboard template**: a generic framework plus one
sport implementation, so new sports (NHL, etc.) are started by copying the
repo and replacing the sport folder.

```
src/
├── main.cpp              # GENERIC app shell: boot sequence, OTA screen,
│                         #   boot status page, NTP sync → sport::tick()
├── config.h              # composition root: FIRMWARE_VERSION (deploy script
│                         #   reads it HERE), timezone, OTA manifest URLs
├── common/               # GENERIC framework — no sport knowledge
│   ├── config.h          # framework defaults (SB_DEBUG gate, boot/network/
│   │                     #   OTA pacing)
│   ├── app/sport_api.h   # THE contract a sport implements (namespace sport)
│   ├── hal/              # tft_panel (panel + canvas + pushes), led_matrix
│   │                     #   (MAX7219), count_leds (7 counter LEDs)
│   ├── comms/            # http_fetcher (keep-alive HTTP + buffered parse),
│   │                     #   network_service (Wi-Fi/portal/NVS, branding
│   │                     #   injected), ota_update
│   ├── data/             # snapshot_channel.h (cross-core mailbox template),
│   │                     #   time_util (NTP/ISO-8601 helpers)
│   └── ui/               # gfx helpers, QR, boot splash/status, OTA screen
└── sports/
    └── mlb/              # THE SPORT — replaced wholesale per new sport
        ├── sport_config.h  # pins, panel geometry, branding, poll cadences,
        │                   #   UI theme (COLOR_*), sport timing constants
        ├── mlb_app.cpp     # WAITING/LIVE state machine + sport:: contract
        ├── mlb_renderer_linescore.cpp / _waiting.cpp / mlb_renderer_internal.h
        ├── mlb_logos.*     # logo RAM cache + draw helpers
        ├── mlb_state.*     # snapshot channels, activeGamePk,
        │                   #   fetch diagnostics
        ├── mlb_teams.*     # ONE team table {id, abbrev}
        ├── mlb_client.*    # feed endpoints + JSON filters (uses http_fetch)
        ├── mlb_data_task.cpp # core-0 fetch/publish loop
        ├── mlb_snapshot.h    # POD snapshot structs (the core0→core1 contract)
        └── team_logos.h / boot_logo.h   # generated assets
```

**Sport selection:** each PlatformIO env sets
`build_src_filter = +<main.cpp>, +<common/>, +<sports/mlb/>` and
`-Isrc/sports/mlb`, so `#include "sport_config.h"` resolves to whichever
sport the env selects. Common code NEVER names a sport (the only
common→sport reach is the include-path-resolved `sport_config.h` seam plus
the `sport::` function contract in `common/app/sport_api.h`).

**Starting a new sport:** see the checklist in README.md. Short version —
copy `src/sports/mlb` → `src/sports/<sport>`, rewrite the sport folder,
point the env's src filter + `-I` at it, update `OTA_*` URLs in
`src/config.h` and `RAW_BASE`/`LATEST_FILE` in `tools/release_deploy.py`.

### Runtime architecture (unchanged by the reorg)

- **Core 1 (Arduino `loop()`):** pure display. The generic shell handles
  boot/update/network screens, then calls `sport::tick()`; the sport renders
  from POD snapshots via `take*Snapshot()`. No HTTP on this core.
- **Manual mode (v2.61):** the setup portal's "Manual Mode" page
  (`/manual`, handlers + volatile state in `common/comms/network_service`,
  same pattern as the NHL scoreboard) fully overrides the display: the TFT
  shows the boot splash, the matrices show portal-set scores, and the count
  LEDs follow portal-set balls/strikes/outs (inning + top/bottom are held
  in state). `mlb_app.cpp`'s tick short-circuits into
  `renderManual()` while active and repaints the previous screen on exit.
- **Core 0 (FreeRTOS tasks):** the network task (portal/DNS/ArduinoOTA,
  priority 2) and the MLB data task (`sports/mlb/mlb_data_task.cpp`,
  priority 1) do all API calls, JSON parsing, and snapshot publishing.
- **Game selection (v2.67):** the setup portal's "Favorite Team
  Priorities" dropdowns (NVS `team1..3`, defaults PHI/ATL/BOS) pick up to
  three preferred teams; the board follows the highest-priority preferred
  team with a live game and falls back to the FIRST live game on today's
  slate (schedule order = start-time order) when none of them are live,
  rolling to the next live game when the followed game ends. The setup
  portal configures Wi-Fi, preferred teams, timezone, and the idle-clock
  toggle. The ESPN news feed and upcoming-games carousel that used to
  exist were removed in v2.62 (team selection was restored in v2.67);
  waiting mode shows a standby screen with schedule diagnostics.

---

## 2. Build & deploy workflow

```powershell
# Compile (no upload) — fast iteration on code
& 'C:\Users\Steve\.platformio\penv\Scripts\platformio.exe' run `
    --environment esp32-s3-devkitc-1

# USB upload (reliable; preferred when developing)
& 'C:\Users\Steve\.platformio\penv\Scripts\platformio.exe' run `
    --target upload --environment esp32-s3-devkitc-1

# OTA upload (Wi-Fi; flaky on weak signal — see notes below)
& 'C:\Users\Steve\.platformio\penv\Scripts\platformio.exe' run `
    --target upload --environment esp32-s3-devkitc-1-ota `
    --upload-port 192.168.1.205
```

**OTA caveats** (from past runs):
- Wi-Fi to 192.168.1.205 is variable (~80–600 ms RTT, occasional 50 %
  packet loss). OTA uploads often stall at 4–19 % and may take 3–4 min
  when they do succeed.
- `--timeout=120` is set in `platformio.ini` for the OTA env.
- If OTA stalls repeatedly, **power-cycle the device** (or flash over
  USB) before retrying — leftover OTA state can wedge the bootloader.

### Releases + firmware self-update

- **One-command release:** `pio run -e esp32-s3-devkitc-1 -t deploy`.
  This builds the firmware, writes `releases/`
  (`mlb_scoreboard_latest.bin`, permanent `mlb_scoreboard_<version>.bin`,
  `manifest.json` with `{"version","file","url"}`), then **commits
  `releases/` and pushes to GitHub** — the push is what publishes the
  update. The target refuses to deploy when `FIRMWARE_VERSION` still
  matches the published manifest version (devices only flash strictly
  newer versions), so **bump `FIRMWARE_VERSION` in `src/config.h` before
  every deploy**. GitHub's raw CDN caches the manifest ~5 min after a push.
- **Self-update flow** (`src/common/comms/ota_update.cpp`; approach
  verified end-to-end on the same framework/hardware in the NHL port —
  device pulled a 1.67 MB release from GitHub raw and rebooted into it
  in ~25 s): shortly after the network comes online (before any feed
  fetch — the TLS handshake needs the pristine boot heap; on this core
  2.0.x the 2x16 KB mbedTLS buffers cannot be shrunk, `setBufferSizes`
  is a 3.x API), the core-0 data task fetches the manifest over TLS
  (the only TLS connection the updater makes; the feeds run plain
  HTTP — see the note at the top of `sports/mlb/mlb_client.cpp` and
  `common/comms/http_fetcher.h`). If the manifest version is strictly
  newer than `FIRMWARE_VERSION`, manifest and binary download over ONE
  reused TLS session (this AP refuses a second fresh TLS handshake
  moments after the first) while the renderer shows the "do not turn
  off" progress screen (`common/ui/ota_screen.cpp`), then the device
  reboots into the new image. Failures leave the current firmware
  running.
- **Update checks are BOOT-ONLY** (adopted from the NHL port): the
  automatic check runs exactly once per power cycle — two attempts in
  the boot window, then never again mid-session (the fragmented
  post-feed heap can't fund the handshake, and the AP refuses later
  fresh TLS connections). To update a running device: power-cycle it,
  or use the portal's "check now". The portal button remains available
  and works best right after a boot.
- **CRITICAL WIRING** — the sport's data task MUST call
  `serviceOtaUpdates(onlineFor)` every loop pass, hold feeds while
  `otaUpdateInProgress()`, and gate the first feed fetch behind
  `otaBootGateReached(onlineFor)` (this repo's
  `mlbDataTaskLoop` does; the NHL port dropped these calls once and
  its updater was silently dead until restored — don't repeat that).
- **Testing an OTA release**: (1) deploy the release (vN) to GitHub;
  (2) build a DEBUG firmware labeled vN-1 (`-DSB_DEBUG=1`,
  `-DARDUINO_USB_CDC_ON_BOOT=0` so logs reach the CH343 UART) and
  flash it over serial; (3) wait out the ~5 min raw-CDN manifest
  cache; (4) reset the board via the CH343 auto-reset lines (RTS high
  120 ms → release) and capture serial: expect `[OTA] manifest ->
  HTTP 200`, `update available`, `flashed N bytes OK`, then a second
  ROM banner as the device reboots into the release. The device's
  portal `/ota/status` shows `checked/ok` and the splash shows the
  new version.

---

## 3. Build-versioning rules — **MANDATORY on every code change**

`src/config.h` (the composition root) carries a single human-readable
version string:

```cpp
static const char* FIRMWARE_VERSION = "v2.60";
```

This string is drawn on the boot splash (see `renderBootSplash()` in
`src/common/ui/boot_splash.cpp`) so the user can confirm at a glance which
firmware is on the device. `tools/release_deploy.py` reads it from
`src/config.h` — keep the definition in that file.

**Every change that ships to the device must bump `FIRMWARE_VERSION`.**

### Bump policy

- Increment the **minor** number with each build (i.e. the digit after
  the dot) — the "patch level" in semver terms. Example: `v2.58` →
  `v2.59` → `v2.60` → …
- Don't bump the major (`v2` → `v3`) unless the change is a breaking
  rewrite of the user-facing behavior (e.g. switching scoreboard modes,
  removing the OLED header, swapping in a totally different display
  controller, etc.). Ask the user before doing a major bump.
- Reset the minor back to `0` when the major goes up.
- The version is purely a string — keep it short, ASCII, and visible in
  the boot splash. Do not add build dates or commit SHAs to the string;
  the firmware compile date is already available via `__DATE__`.

### When to bump

| Change | Bump? |
|---|---|
| Bug fix that doesn't change user-visible behavior | **yes**, minor++ |
| New diagnostic info on screen | yes, minor++ |
| Tweaks to internal data flow / dirty-rect / caching | yes, minor++ |
| Comment-only / formatting / refactor with no behavior change | **yes**, minor++ (still ships, still counts as a build) |
| User-facing feature change (new carousel page, etc.) | yes, minor++ |
| Breaking change to display layout, modes, or APIs | ask first; major++ |

### Example edit

```diff
- static const char* FIRMWARE_VERSION = "v2.60";
+ static const char* FIRMWARE_VERSION = "v2.60";
```

The boot splash will pick this up automatically (it reads the constant on
every `renderBootSplash()` call).

---

## 4. Coding conventions

- **No new heap allocations in `loop()`/`sport::tick()`.** The render
  canvas is lazily `new`'d once inside `TftPanel` and cached. Don't add
  `String`/JSON docs/etc. on the render path.
- **Feed/API calls live in core 0** (`sports/mlb/mlb_data_task.cpp` +
  `mlb_client.cpp`). Do not fetch from core 1. If you need new MLB data,
  add a field to a struct in `sports/mlb/mlb_snapshot.h`, publish through
  the channels in `sports/mlb/mlb_state.cpp` (or
  `common/data/snapshot_channel.h` for a new channel), and read it via a
  `take*Snapshot()` accessor.
- **Common code must stay sport-agnostic.** `src/common/` and
  `src/main.cpp` never include a sport header other than the
  include-path-resolved `sport_config.h` (constants only) and never call
  anything outside the `sport::` contract in `common/app/sport_api.h`.
  Sport logic belongs in `src/sports/<sport>/`.
- **Serial.printf is expensive** at 115200 baud (~25 ms per call). Use
  `DBG_PRINTF(fmt, ...)` (defined in `src/common/config.h`) instead — it
  compiles to a no-op when `SB_DEBUG == 0`. Production builds default
  `SB_DEBUG` to 0; leave it at 1 temporarily only while diagnosing an
  issue.
- **No `Serial.println` in hot paths** (`loop()`/`sport::tick()`,
  `rotateCarousel()`, `updateAtBatResultDisplay()`). All status prints go
  through `DBG_PRINTF` and only fire on state transitions.
- **Renderer internals stay inside the sport's renderer files.** The data
  task publishes through the public APIs (`mlb_renderer.h`,
  `mlb_state.h`). `mlb_renderer_internal.h` is renderer-private — never
  include it from outside the renderer pair.
- **All drawing goes through the shared canvas in `TftPanel`** and one
  push call (`pushFull` / `pushBand` / `pushRows`). The only sanctioned
  direct-panel draw is the AP provisioning screen
  (`network_service.cpp`, via `tftPanel.raw()`).
- **Pin assignments live in the sport's `sport_config.h`** and are passed
  to the HAL at init (`initLedMatrix(din, clk, cs)`,
  `initCountLeds(pins)`, `tftPanel.begin(...)`). `src/common/` contains
  no pin constants. Don't change pins without verifying against the
  actual hardware — the SPI pin gotcha in
  `memories/repo/hardware_architecture.md` explains why we cannot use
  hardware SPI for the ST7789 even though it would be faster.

---

## 5. Testing before claiming a fix works

After any non-trivial change:

1. `pio run --environment esp32-s3-devkitc-1` — must compile with no
   errors. Warnings about unused variables/functions are okay but should
   be addressed before bumping the firmware version. Also sanity-check
   flash size against the 1.5 MB OTA partition (the build prints the
   percentage; v2.63 sits ~90.4%).
2. Upload to the device (OTA or USB).
3. Confirm via Serial Monitor (with `-DSB_DEBUG=1`) or the on-screen
   diagnostics that:
   - The boot splash shows the new version string.
   - The waiting standby screen, when no game is live, shows the
     diagnostic line `sched att:N ok:M err:ok http:200` — this confirms
     the data task is fetching the schedule.
   - The MAX7219 matrices still show the live time when the device is
     in waiting mode (hour on home, minute on away).
   - Count LEDs (balls/strikes/outs) update during a live game.

---

## 6. Things to avoid (lessons learned)

- **Don't switch to hardware SPI for the ST7789.** (Also: never include
  `boot_logo.h` from two TUs — the 80 KB logo array is emitted once per
  including TU; pass the logo through like `renderManual()` does.) The 5-arg software
  SPI constructor is the proven-working configuration. See
  `src/common/hal/tft_panel.h` and
  `memories/repo/hardware_architecture.md` for the full story.
- **Don't block in `loop()`/`sport::tick()`.** Every API call belongs on
  core 0.
- **Don't keep `JsonDocument`s on the render core.** Since the v2.62
  carousel removal the render core carries no JSON at all — keep it that
  way.
- **Don't remove the dirty-rect tracking in `renderLinescore()`.**
  Without it the live screen flickers visibly on every 5 s linescore
  poll.
- **Don't open a second TLS connection while one is alive.** The OTA
  updater reuses one session for manifest + binary on purpose; the feed
  client closes its keep-alive session (`http_fetch::closeSession()`)
  before anything else connects.
- **Don't bump `FIRMWARE_VERSION` past `v9.x` without a discussion.**
  Two-digit minors look weird in the splash. Plan a major bump before
  then.
- **Don't add pin constants or sport names to `src/common/`.** That
  breaks the template separation the v2.59 reorg established.

---

## 7. Repo memory

Long-lived project notes live under `/memories/repo/` in the assistant's
memory. The current file `hardware_architecture.md` covers:

- Pin assignments
- MLB API integration details (TLS heap churn, fields-filter gotchas,
  standings carousel)
- The ST7789 SPI pin gotcha (DO NOT switch to hardware SPI)
- Performance notes (debug-flag, dirty-rect, sprite pre-rasterization,
  logo cache)
- The v2.59 template reorganization (folder layout, sport_api contract,
  include-path sport selection)

When you discover something durable about this project (a build quirk,
a library version pin, a hardware pitfall), **update
`hardware_architecture.md`** so the next assistant doesn't repeat the
investigation.
