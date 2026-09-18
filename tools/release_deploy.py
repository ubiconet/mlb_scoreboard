# PlatformIO extra script: adds a `pio run -t deploy` target that copies the
# freshly built firmware into releases/ under two names:
#   - mlb_scoreboard_latest.bin       (always the most recent build)
#   - mlb_scoreboard_<version>.bin    (permanent archive for this version,
#                                      e.g. mlb_scoreboard_v2.22.bin)
# The version is read from FIRMWARE_VERSION in src/config.h, so the archive
# name always matches the version shown on the device's boot splash.

import json
import os
import re
import shutil

Import("env")

# Where the repo's releases/ folder is served from. The firmware's OTA
# updater fetches manifest.json from here (see OTA_MANIFEST_URL in
# src/config.h) and downloads the binary named inside it.
RAW_BASE = ("https://raw.githubusercontent.com/ubiconet/mlb_scoreboard/"
            "main/releases/")
LATEST_FILE = "mlb_scoreboard_latest.bin"


def _firmware_version():
    config_path = os.path.join(env["PROJECT_SRC_DIR"], "config.h")
    with open(config_path, "r", encoding="utf-8") as f:
        match = re.search(r'FIRMWARE_VERSION\s*=\s*"([^"]+)"', f.read())
    if not match:
        env.Exit("Could not find FIRMWARE_VERSION in src/config.h")
    return match.group(1)


def _deploy(source, target, env):
    version = _firmware_version()
    build_bin = os.path.join(env["PROJECT_BUILD_DIR"], env["PIOENV"],
                             "firmware.bin")
    releases_dir = os.path.join(env["PROJECT_DIR"], "releases")
    os.makedirs(releases_dir, exist_ok=True)

    archived = os.path.join(releases_dir, "mlb_scoreboard_%s.bin" % version)
    latest = os.path.join(releases_dir, LATEST_FILE)

    shutil.copyfile(build_bin, archived)
    shutil.copyfile(build_bin, latest)

    # The manifest the firmware polls after boot. `version` lets a device
    # decide whether it is current; `file`/`url` say what to download.
    manifest = {
        "version": version,
        "file": LATEST_FILE,
        "url": RAW_BASE + LATEST_FILE,
    }
    manifest_path = os.path.join(releases_dir, "manifest.json")
    with open(manifest_path, "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")

    size_kb = os.path.getsize(archived) / 1024.0
    print("deployed %s (%.1f KB) ->\n  %s\n  %s\n  %s (version %s)"
          % (build_bin, size_kb,
             os.path.relpath(archived, env["PROJECT_DIR"]),
             os.path.relpath(latest, env["PROJECT_DIR"]),
             os.path.relpath(manifest_path, env["PROJECT_DIR"]),
             version))


# Depends on the firmware binary (concrete path — $-variable strings are not
# re-expanded in custom-target dependencies) so `pio run -t deploy` builds
# first. The guard keeps normal builds from paying any setup cost.
if "deploy" in COMMAND_LINE_TARGETS:
    firmware_bin = os.path.join(env["PROJECT_BUILD_DIR"], env["PIOENV"],
                                "firmware.bin")
    env.AddCustomTarget("deploy", firmware_bin, _deploy)
