# AGENTS.md — Operating instructions for AI coding assistants

This file describes how AI assistants should work on the **MLBScoreboard**
project. The goal: keep changes small, verifiable, and consistent with the
existing architecture; produce a build that an OTA-uploaded device can run
without surprises.

---

## 1. Project summary

- **Target hardware:** ESP32-S3 DevKitC-1 (4 MB flash).
- **Peripherals:**
  - 2.0" ST7789 TFT, 320×240, **software-SPI** on pins
    `SCK=13, MOSI=12, CS=9, DC=10, RESET=11` (see the SPI gotcha note in
    `memories/repo/hardware_architecture.md`).
  - 2× MAX7219 8×8 LED matrices (DIN=14, CLK=8, CS=16).
  - 7 discrete count LEDs on GPIO 1–7.
- **Firmware:** Arduino-ESP32 framework, PlatformIO build system.
- **Build envs:**
  - `esp32-s3-devkitc-1` → USB CDC upload.
  - `esp32-s3-devkitc-1-ota` → ArduinoOTA Wi-Fi upload.
- **Runtime architecture (as of the dual-core refactor):**
  - **Core 1 (Arduino `loop()`):** pure display. Consumes POD snapshots
    from core 0 via `takeLinescoreSnapshot()`, `takePlaySnapshot()`,
    `takeScheduleSnapshot()`. No HTTPS calls.
  - **Core 0 (FreeRTOS task in `src/mlb_data_task.cpp`):** all MLB/ESPN
    API calls, JSON parsing, snapshot publishing.
- **Carousel/news/news ticker:** cached for **30 minutes**
  (`MLB_NEWS_CACHE_TTL_MS` in `src/config.h`). News API is hit at most
  once per 30 min, not on every waiting-mode cycle.

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

- **Build + publish binaries:** `pio run -e esp32-s3-devkitc-1 -t deploy`.
  This writes into `releases/`:
  - `mlb_scoreboard_latest.bin` (always overwritten),
  - `mlb_scoreboard_<version>.bin` (permanent archive named from
    `FIRMWARE_VERSION`),
  - `manifest.json` (`{"version", "file", "url"}`).
  Commit + push `releases/` to
  `github.com/ubiconet/mlb_scoreboard` afterwards — the device fetches
  the manifest from `raw.githubusercontent.com/ubiconet/mlb_scoreboard/
  main/releases/manifest.json`.
- **Self-update flow** (`src/ota_update.cpp`): ~90 s after boot the
  core-0 data task fetches the manifest over TLS (the only TLS connection
  left — the feeds run plain HTTP; see the note at the top of
  `mlb_client.cpp`). If `version` differs from `FIRMWARE_VERSION`, it
  downloads the binary and flashes it while the renderer shows the
  "do not turn off" progress screen (`handleOtaUpdateScreen()`), then
  reboots. A failed check/download leaves the current firmware running
  and retries every 30 min.
- Therefore: **bump `FIRMWARE_VERSION` before every deploy** or devices
  will consider themselves current and skip the update.

---

## 3. Build-versioning rules — **MANDATORY on every code change**

`src/config.h` carries a single human-readable version string:

```cpp
static const char* FIRMWARE_VERSION = "v1.0";
```

This string is drawn on the boot splash (see `renderBootSplash()` in
`src/scoreboard.cpp`) so the user can confirm at a glance which firmware
is on the device.

**Every change that ships to the device must bump `FIRMWARE_VERSION`.**

### Bump policy

- Increment the **minor** number with each build (i.e. the digit after
  the dot) — the "patch level" in semver terms. Example: `v1.0` →
  `v1.1` → `v1.2` → …
- Don't bump the major (`v1` → `v2`) unless the change is a breaking
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
- static const char* FIRMWARE_VERSION = "v1.0";
+ static const char* FIRMWARE_VERSION = "v1.1";
```

The boot splash will pick this up automatically (it reads the constant on
every `renderBootSplash()` call).

---

## 4. Coding conventions

- **No new heap allocations in `loop()`.** `getCanvas()` and the news
  sprite are lazily `new`'d once and cached. Don't add `String`/JSON
  docs/etc. on the render path.
- **MLB API calls live in core 0** (`mlb_data_task.cpp`). Do not call
  `fetchMlb*()` from `loop()`. If you need new MLB data, add a new
  snapshot struct to `src/mlb_snapshot.h`, a new publisher in
  `src/scoreboard.cpp`'s `mlb_data::` namespace, and a `take*Snapshot()`
  accessor in `src/scoreboard.h`.
- **Serial.printf is expensive** at 115200 baud (~25 ms per call). Use
  `DBG_PRINTF(fmt, ...)` (defined in `src/config.h`) instead — it
  compiles to a no-op when `MLB_DEBUG == 0`. Production builds should
  default `MLB_DEBUG` to 0; leave it at 1 temporarily only while
  diagnosing an issue.
- **No `Serial.println` in hot paths** (`loop()`, `rotateCarousel()`,
  `updateAtBatResultDisplay()`). All status prints go through
  `DBG_PRINTF` and only fire on state transitions.
- **Renderer stays in the anonymous namespace in `scoreboard.cpp`.**
  The data task publishes into renderer-owned slots via the public
  setters in `scoreboard.h` (`publishUpcomingScheduleJson()`,
  `publishNewsStory()`, `setNewsStoryCount()`, `getNewsStoryCount()`,
  `getNewsStoryIndex()`, `getUpcomingScheduleJson()`, etc.). Don't
  reach into the anonymous namespace from outside the file.
- **MAX7219 / TFT pin assignments** are in `src/config.h`. Don't change
  them without verifying against the actual hardware — the SPI pin
  gotcha in `memories/repo/hardware_architecture.md` explains why we
  cannot use the 3-arg `Adafruit_ST7789(&SPI, ...)` constructor even
  though it would be faster.

---

## 5. Testing before claiming a fix works

After any non-trivial change:

1. `pio run --environment esp32-s3-devkitc-1` — must compile with no
   errors. Warnings about unused variables/functions are okay but
   should be addressed before bumping the firmware version.
2. Upload to the device (OTA or USB).
3. Confirm via Serial Monitor or the on-screen diagnostic that:
   - The boot splash shows the new version string.
   - The "no upcoming games" screen, if visible, now shows the
     diagnostic line `sched:NB Ndates Ngames Ns ago` instead of the
     generic "No upcoming games found" — this confirms the data task
     is publishing schedule JSON.
   - The MAX7219 matrices still show the live time when the device is
     in waiting mode (hour on home, minute on away).
   - Count LEDs (balls/strikes/outs) update during a live game.

---

## 6. Things to avoid (lessons learned)

- **Don't switch to hardware SPI for the ST7789.** The 5-arg software
  SPI constructor is the proven-working configuration. See
  `memories/repo/hardware_architecture.md` for the full story.
- **Don't block in `loop()`.** Every API call belongs on core 0.
- **Don't keep large `JsonDocument`s on core 1.** The renderer only
  carries the small `gUpcomingScheduleDoc` (a copy of the schedule
  range, used by `findNextGame()`) — keep it that way.
- **Don't remove the dirty-rect tracking in `renderLinescore()`.**
  Without it the live screen flickers visibly on every 5 s linescore
  poll.
- **Don't change `MLB_NEWS_CACHE_TTL_MS` below 5 minutes.** The ESPN
  endpoint is fine with 30 min; sub-5-min hammering is wasteful and
  may get rate-limited.
- **Don't bump `FIRMWARE_VERSION` past `v9.x` without a discussion.**
  Two-digit minors look weird in the splash. Plan a major bump before
  then.

---

## 7. Repo memory

Long-lived project notes live under `/memories/repo/` in the assistant's
memory. The current file `hardware_architecture.md` covers:

- Pin assignments
- MLB API integration details (TLS heap churn, fields-filter gotchas,
  standings/news carousel)
- The ST7789 SPI pin gotcha (DO NOT switch to hardware SPI)
- Performance notes (debug-flag, dirty-rect, sprite pre-rasterization,
  logo cache)

When you discover something durable about this project (a build quirk,
a library version pin, a hardware pitfall), **update
`hardware_architecture.md`** so the next assistant doesn't repeat the
investigation.
