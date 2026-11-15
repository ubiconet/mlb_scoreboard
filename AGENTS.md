# AGENTS.md — Operating instructions for AI coding assistants

This file describes how AI assistants should work on the **MLBScoreboard**
project. The goal: keep changes small, verifiable, and consistent with the
existing architecture; produce a build that an OTA-updated device can run
without surprises.

---

## 1. Project summary

- **Target hardware:** YD-ESP32-S3-family 44-pin DevKitC-1 clone — the
  same devkit as the NHLScoreboard — **N8R8 module** (8 MB flash, 8 MB
  octal PSRAM; `qio_opi` memory type, OTA app slots 3.375 MB each, plus a
  coredump partition). PSRAM sits in the main heap from boot but
  `BOARD_HAS_PSRAM` is NOT defined, so an Arduino `String` still caps at
  65,535 bytes. Octal PSRAM permanently reserves GPIO 35–37. Since v2.74
  (the board switch) a build targets this module only: **an old 4 MB
  DevKitC-1 board must NOT OTA onto a v2.74+ build** (the qio_opi image
  won't boot without octal PSRAM) — migrating a board requires a USB
  flash, which also writes the new partition table.
- **Peripherals (MLB build — v2.76 rewire pin map):**
  - 2.0" ST7789 TFT, 320×240, **software-SPI** on pins
    `SCK=8, MOSI=9, CS=12, DC=10, RESET=11`, backlight LED line on
    `GPIO 18` (driven HIGH after a clean panel init, LOW before the
    low-battery sleep), MISO on GPIO 17 wired but unused (software SPI
    writes only). See the SPI gotcha note in
    `memories/repo/hardware_architecture.md` and in
    `src/common/hal/tft_panel.h`.
  - 2× MAX7219 8×8 LED matrices (CS=21, CLK=20, DIN=14 — DIN was not on
    the rewire list; confirm it). **CLK on GPIO 20 is the ESP32-S3's
    native USB D+ line**: the devkit's native USB-C port can't enumerate
    on this wiring, so Serial and USB flashing go through the CH343 UART
    port (`-DARDUINO_USB_CDC_ON_BOOT=0` in platformio.ini).
  - 7 discrete count LEDs (balls/strikes/outs) on GPIO 42/41/40 (balls
    1–3), 39/38 (strikes 1–2), 48/47 (outs 1–2) — moved off GPIO 1–7 in
    the v2.76 rewire so an ADC1 pin is free for the battery divider
    (bench-confirmed v2.81; GPIO 46 is free/unused). OUT 1 on GPIO 48 is
    the devkit's onboard-RGB (WS2812) data line repurposed as a plain
    output — the shell's old pin-48-low RGB hack is gone (a driven line
    keeps the onboard RGB dark). While the board has no network link all
    seven flash together (network-down indicator, `setCountLedsBlink`).
  - MAX98357 I2S audio amp (score sounds, added v2.75; wired v2.76) —
    `I2S_BCLK=5, I2S_LRC=4, I2S_DIN=6` (the pins the count LEDs vacated).
    Five synthesized clips (crowd cheer/roar, bat crack, crack+cheer,
    ballpark organ) live in `sports/mlb/audio_clips.h`, regenerated from
    `tools/gen_score_sounds.py` + `tools/gen_audio_clips.py` (never edit
    the header by hand, and include it from exactly ONE TU — it's ~600 KB
    of PROGMEM arrays).
  - Power: 3.7 V LiPo → switch → 5 V boost converter. The power gate
    (`common/app/power_monitor`, battery constants in `sport_config.h`)
    checks the pack at power-on via an optional divider on
    `BATTERY_SENSE_PIN` (`BATTERY_SENSE_ENABLED`, 470k/470k + 100 nF)
    and/or detects brownout boot storms via an NVS boot journal. Low
    battery flashes the warning screen (`common/ui/low_battery_screen`)
    and deep-sleeps the board; with the divider wired it silently
    re-checks every 15 min and resumes on its own once the charger lifts
    the pack past the resume threshold. The same divider also feeds the
    RUNTIME GAUGE (v2.75: on-screen battery icon + diagnostics readout)
    — `BATTERY_SENSE_PIN` defaults to GPIO 7 (ADC1_CH6, the NHL board's
    gauge pin, free since the LED rewire); the runtime gauge only works
    on ADC1 pins (1–10) because Wi-Fi kills ADC2. `BATTERY_SENSE_ENABLED`
    stays false until the physical divider is confirmed on that pin —
    flipping it with a floating pin risks a spurious low-battery boot
    shutdown.
  - Onboard RGB LED (GPIO 48): unused; the shell pins its data line low
    at boot so the floating WS2812 can't glow.
- **Firmware:** Arduino-ESP32 framework, PlatformIO build system.
- **Build envs:**
  - `esp32-s3-devkitc-1` → USB upload via the devkit's CH343 UART port
    (the native USB-C port is unusable since the v2.76 rewire put the
    matrix CLK on GPIO 20 — the S3's USB D+ line; see sport_config.h).
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

### Runtime architecture (v2.74 framework sync with the NHL board)

- **Core 1 (Arduino `loop()`):** pure display. The generic shell handles
  boot/update/network screens (splash → connecting screen → status page /
  AP instructions), then calls `sport::tick()`; the sport renders from
  POD snapshots via `take*Snapshot()`. No HTTP on this core.
- **Wi-Fi connection sequence (v2.74, from the NHL board):** the network
  service tracks WHY each connect attempt started
  (`ConnectReason`: STARTUP / LINK_LOST / SETUP_RETRY / NEW_NETWORK) and
  exposes live progress (`getConnectProgress`) that the connecting screen
  (`common/ui/connect_screen`) renders — network name, radio step, retry
  count, time left before setup mode. A connection still in flight after
  the boot splash shows this screen (the splash no longer extends).
  `networkLinkUp()` drives the network-down indicator: all 7 count LEDs
  flash until the link is real (hardware-timer blink in `count_leds`, so
  blocking pushes can't stall it).
- **Team-setup hold (v2.74):** with every preferred-team slot set to
  "None" the board keeps the setup screen (device IP + QR) on the display
  until the portal saves a team — nothing to follow otherwise. MLB's
  factory defaults pre-seed PHI/ATL/BOS, so the hold only appears after
  an explicit all-None save or a factory reset that defaults to zero.
- **Manual mode (v2.61):** the setup portal's "Manual Mode" page
  (`/manual`, handlers + volatile state in `common/comms/network_service`,
  same pattern as the NHL scoreboard) fully overrides the display: the TFT
  shows a dedicated manual screen (inning number in the live screen's red
  LED-dot font with TOP/BOTTOM in white beneath, v2.69), the matrices show
  portal-set scores, and the count LEDs follow portal-set
  balls/strikes/outs. `mlb_app.cpp`'s tick short-circuits into
  `renderManual()` while active and repaints the previous screen on exit.
- **Game demo (v2.74, from the NHL board):** the portal's Diagnostics
  page (`/diag`) can start a scripted live game — **Blue Jays (home) vs
  Dodgers (away)** — rendered through the real live renderer (`mlb_app.cpp`'s
  `startGameDemo`/`advanceGameDemo`): plate appearances resolve every 6 s
  from a fixed outcome table, the count/builds and base runners advance,
  runs score, half-innings flip on 3 outs, and each result fires the
  full-screen at-bat card. Works with NO Wi-Fi (`portalDisplayOverrideActive()`
  lets portal overrides own the display while provisioning), which also
  makes the display self-test (`/display/test`) an offline bench check.
  Exactly one portal mode (demo / manual) owns the scoreboard at a time.
- **Score sounds (v2.75, audio amp):** the setup portal's "Score Sounds"
  picker (NVS `sound_id`, five synthesized clips) selects what plays when
  one of the user's PREFERRED teams scores a run in the followed live
  game (fallback games with no preferred team stay silent, like the NHL
  goal horn), on every run in the game demo, and on demand from the
  picker's Preview button, the diagnostics "Play Test Sound", the full
  board test, and the manual-mode sound button — all gated by the
  portal's audio-enable checkbox (NVS `audio_en`). Playback streams from
  flash over I2S via a dedicated core-0 task (`common/hal/audio.h`), so
  slow software-SPI canvas pushes can't starve the DMA ring. Sound
  triggers missed while a portal override owns the display never replay
  in a burst (baseline resets on override exit).
- **Battery gauge (v2.75):** on gauge-capable boards (ADC1 divider, see
  the power section above) a quarter-step battery icon sits top-right on
  the waiting, live, demo, and manual screens, and the diagnostics page
  shows live volts/percent; the full board test verifies the sense
  circuit against a plausible 1S LiPo window.
- **Core 0 (FreeRTOS tasks):** the network task (portal/DNS/ArduinoOTA,
  priority 2) and the MLB data task (`sports/mlb/mlb_data_task.cpp`,
  priority 1) do all API calls, JSON parsing, and snapshot publishing.
  The data task also runs `health::serviceHealthHeartbeat()` (SB_DEBUG
  heap/stack trend line) and `power::serviceBatteryMonitor()` (runtime
  battery gauge sample every 30 s).
- **Game selection (v2.67):** the setup portal's "Favorite Team
  Priorities" dropdowns (NVS `team1..3`, defaults PHI/ATL/BOS) pick up to
  three preferred teams; the board follows the highest-priority preferred
  team with a live game and falls back to the FIRST live game on today's
  slate (schedule order = start-time order) when none of them are live,
  rolling to the next live game when the followed game ends. The setup
  portal configures Wi-Fi, preferred teams, timezone, and the idle-clock
  toggle; it also links the Diagnostics page (feed status, stability/
  crash summary, display test, game demo, factory reset) and a Support
  section (email + website). Clearing the network name and saving
  disconnects the board back onto its setup AP. The ESPN news feed and
  upcoming-games carousel that used to exist were removed in v2.62 (team
  selection was restored in v2.67); waiting mode shows a standby screen
  with schedule diagnostics.

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
  This builds the firmware, writes `releases/` (`mlb_scoreboard_latest.bin`,
  permanent `mlb_scoreboard_<version>.bin`, a merged `*_factory.bin`,
  bootloader/partitions/boot_app0 components, and a flasher-tool
  `manifest.json`), then publishes BOTH copies (NHL-board flow, v2.84):
  1. **GitHub** — commits `releases/` and pushes to
     `ubiconet/mlb_scoreboard` (the archive / manual-download copy).
  2. **Website** — mirrors the release set into `firmware/mlb/` of the
     sales-site repo (`github.com/ubiconet/scoreboard-site`, clone at
     `C:\projects\scoreboard-site`, override via `MINIBOARD_SITE_ROOT`),
     rewrites that copy's manifest URLs to
     `https://sports-miniboards.com/firmware/mlb`, regenerates the
     browsable `index.html` pages, commits and pushes — that push
     publishes Netlify (the site is repo-connected; `_publish_netlify`
     falls back to a CLI deploy only if the link is missing).
  The target refuses to deploy when `FIRMWARE_VERSION` still matches the
  published manifest version (devices only flash strictly newer
  versions), so **bump `FIRMWARE_VERSION` in `src/config.h` before every
  deploy**.
- **Self-update flow** (`src/common/comms/ota_update.cpp`): **website
  manifest over TLS (v2.84, NHL flow)**. The boot check fetches
  `https://sports-miniboards.com/firmware/mlb/manifest.json` (no-cache,
  no CDN lag — GitHub raw cached ~5 min) over an insecure TLS session;
  the N8R8 board's PSRAM heap funds the handshake beside the 153.6 KB
  render canvas, so no park is needed for the check. (History: through
  v2.83 the check ran over plain HTTP from raw.githack.com because the
  old 4MB board's heap could not fund TLS beside the canvas — don't
  revive that transport unless the board loses its PSRAM.) When the
  manifest version is strictly newer, the **display park** engages
  (`OtaStage::CHECKING` → the render core stops and calls
  `tftPanel.releaseCanvas()`; the updater waits for
  `otaDisplayParked()`), the binary downloads over TLS from the site
  (manifest's `url` field), and a successful install reboots
  immediately so the canvas never needs to fit back after TLS churn.
  The render canvas is allocated by the boot splash's first canvas()
  call, on the pristine PRE-Wi-Fi heap (post-association fragmentation
  can leave the largest block far below the 153.6 KB the canvas needs —
  measured 33 KB maxAlloc after feeds start on the old board). Install
  details: one reused TLS session for the binary (this AP refuses a
  second fresh handshake moments after the first), progress screen
  drawn DIRECT to the panel (the canvas is parked), failures leave the
  current firmware running (a failed install may leave a stale screen
  until reboot — the canvas may not fit back after TLS churn).
- **Update checks are BOOT-ONLY** (adopted from the NHL port): the
  automatic check runs exactly once per power cycle — three attempts in
  the boot window, then never again mid-session. To update a running
  device: power-cycle it. Since v2.74 the portal's Firmware Update
  section matches the NHL board: installed version, live `/ota/status`
  polling, the latest-binary download link, and the manual `/update`
  upload page — the "check now" button is gone (`requestOtaCheckNow()`
  remains in `ota_update.cpp` for a future trigger).
- **The OTA manifest buffer must fit the whole manifest** (NHL lesson,
  applied in v2.74): `sManifestBuf` in `ota_update.cpp` is a 2 KB static
  buffer and `readExactBody()` refuses any body larger than it — an
  undersized buffer silently fails EVERY check the moment the manifest
  grows a field. If the manifest ever approaches 2 KB, grow the buffer
  in the same change.
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
  direct-panel draws are the AP provisioning screen
  (`network_service.cpp`, via `tftPanel.raw()`) and the OTA update
  screen (`common/ui/ota_screen.cpp`) — the update screen must render
  while the display is parked (canvas freed for the TLS download), so
  it cannot use the canvas.
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
   flash size against the 3.375 MB OTA app slots (the build prints the
   percentage; v2.74 sits ~42%).
2. Upload to the device (OTA or USB — a board moving from the pre-v2.74
   4 MB DevKitC build can ONLY be migrated over USB: the qio_opi image
   and the new partition table both need a wired flash, and an old board
   must never OTA onto a v2.74+ build).
3. Confirm via Serial Monitor (with `-DSB_DEBUG=1`) or the on-screen
   diagnostics that:
   - The boot splash shows the new version string, and the boot banner
     reports the reset reason + last crash (health).
   - With Wi-Fi unreachable, the count LEDs all flash (network-down
     indicator) and the connecting screen shows the network name and
     countdown before setup mode takes over.
   - The waiting standby screen, when no game is live, shows the
     diagnostic line `sched att:N ok:M err:ok http:200` — this confirms
     the data task is fetching the schedule (also visible on the
     portal's Diagnostics page as live feed status).
   - The MAX7219 matrices still show the live time when the device is
     in waiting mode (hour on home, minute on away).
   - Count LEDs (balls/strikes/outs) update during a live game.
   - The portal's Diagnostics page: "Run Display Test" cycles the
     matrices/LEDs/screen; "Start Game Demo" plays the Blue Jays vs
     Dodgers scripted game on every output (works with no Wi-Fi too);
     "Play Test Sound" plays the selected score sound once the amp is
     wired (silently no-ops until then).
   - The setup page's Score Sounds picker previews each clip without
     saving; the audio-enable checkbox silences every trigger.

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
