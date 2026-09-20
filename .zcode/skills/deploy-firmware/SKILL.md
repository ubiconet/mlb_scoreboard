---
name: deploy-firmware
description: Build the MLBScoreboard firmware and publish a release to GitHub (releases/ binaries + manifest.json), then get it onto the device. Use when the user asks to build, deploy, publish, release, or push a firmware update, or to update the scoreboard device.
---

# Deploy a firmware release

## Preconditions

1. **Bump `FIRMWARE_VERSION`** in `src/config.h` (minor bump, e.g. `v2.41` →
   `v2.42`). The deploy target refuses to run when the version still equals
   the one in `releases/manifest.json` — devices only flash strictly newer
   versions, so an unbumped deploy is a silent no-op.
2. Build must compile cleanly:
   ```powershell
   & 'C:\Users\Steve\.platformio\penv\Scripts\platformio.exe' run --environment esp32-s3-devkitc-1
   ```

## Publish

3. Commit the source changes first (the deploy step only commits `releases/`):
   ```powershell
   git add -A
   git commit -m "<describe the change>; v2.42"
   ```
4. Run the deploy — this builds, writes
   `releases/mlb_scoreboard_latest.bin` (overwritten),
   `releases/mlb_scoreboard_<version>.bin` (permanent archive), and
   `releases/manifest.json` (`{"version","file","url"}`), then commits
   `releases/` and pushes to GitHub:
   ```powershell
   & 'C:\Users\Steve\.platformio\penv\Scripts\platformio.exe' run --environment esp32-s3-devkitc-1 --target deploy
   ```
   The push is what publishes the update. If the guard aborts, bump the
   version (step 1) and rerun. If `git push` fails, the release is
   committed locally only — fix the error and `git push` manually.
5. GitHub's raw CDN caches the manifest ~5 minutes. Verify it flipped:
   ```powershell
   curl.exe -s https://raw.githubusercontent.com/ubiconet/mlb_scoreboard/main/releases/manifest.json
   ```

## Getting the update onto the device

The device checks the manifest automatically after boot (6 attempts in the
first ~100 s, then every 10 min) and flashes itself. On this install's
network, TLS (port 443) to GitHub is frequently blocked, so when the user
wants the device updated NOW, use the portal upload instead — it serves
plain HTTP on the LAN and always works:

6. Find the device IP from the serial log (`[NET] Online: ip=...`) or the
   TFT, then upload the new binary:
   ```powershell
   curl.exe -m 120 -F "update=@releases/mlb_scoreboard_latest.bin" http://<device-ip>/update
   ```
   (Browser equivalent: `http://<device-ip>/update`, pick the .bin,
   "Upload & Flash". The device reboots into the new firmware.)
7. Verify over serial (`FW=<version>` in the boot banner, news/schedule
   fetch lines):
   ```powershell
   & 'C:\Users\Steve\.platformio\penv\Scripts\python.exe' tools/capture_serial.py COM13 90 serial_log.txt
   ```
   (Substitute the current COM port; `pio device list` finds it.)

## Notes

- The scoreboard's USB port also works: `pio run -t upload
  --upload-port COM13` — use when the device is plugged in and the portal
  is unreachable.
- Never edit `releases/manifest.json` by hand; the deploy target owns it.
- Full background: `AGENTS.md` §2 "Releases + firmware self-update" and
  `releases/README.md`.
