# Releases

Firmware binaries + update manifest, produced and published by one command:

```powershell
& 'C:\Users\Steve\.platformio\penv\Scripts\platformio.exe' run `
    --environment esp32-s3-devkitc-1 --target deploy
```

The deploy target:

1. Builds the firmware.
2. Refuses to run if `FIRMWARE_VERSION` in `src/config.h` still matches
   the version in `manifest.json` — bump the version first, or devices
   will ignore the build.
3. Writes `mlb_scoreboard_latest.bin` (always overwritten), a permanent
   `mlb_scoreboard_<version>.bin` archive, and `manifest.json`
   (`version` / `file` / `url`).
4. Commits `releases/` and pushes to GitHub, which is what actually
   publishes the update — the scoreboard polls
   `raw.githubusercontent.com/ubiconet/mlb_scoreboard/main/releases/manifest.json`
   after boot and flashes itself when the version is newer.

Note: GitHub's raw CDN caches the manifest for ~5 minutes after a push, so
a device may see the previous manifest for a few minutes after a deploy.
