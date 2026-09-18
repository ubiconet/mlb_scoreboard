# Releases

Firmware binaries, produced by:

```powershell
& 'C:\Users\Steve\.platformio\penv\Scripts\platformio.exe' run `
    --environment esp32-s3-devkitc-1 --target deploy
```

Each deploy writes two files:

- `mlb_scoreboard_latest.bin` — always the most recent build (overwritten).
- `mlb_scoreboard_<version>.bin` — permanent archive named after
  `FIRMWARE_VERSION` in `src/config.h` (e.g. `mlb_scoreboard_v2.22.bin`),
  matching the version string shown on the device's boot splash.

To flash a binary over USB:

```powershell
& 'C:\Users\Steve\.platformio\penv\Scripts\platformio.exe' run `
    --target upload --environment esp32-s3-devkitc-1 --upload-port COM13
```

(The regular `--target upload` rebuilds from source; to flash a specific
archived binary use esptool directly, or temporarily copy it over the
build output.)
