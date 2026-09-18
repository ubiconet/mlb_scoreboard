#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Update.h>
#include <WiFiClientSecure.h>

#include "config.h"
#include "mlb_client.h"
#include "ota_update.h"
#include "scoreboard.h"

namespace {

// Download staging buffer. Static (not stack): the data task's 12 KB stack
// can't absorb a multi-KB read buffer alongside the HTTP/TLS frames.
uint8_t sOtaBuf[4096];

bool sCheckedOnce = false;
bool sLastCheckOk = false;
uint32_t sLastCheckAt = 0;

// Fetches and parses the release manifest. Returns true and fills
// version/url (both remain valid until the next call) on success.
bool fetchManifest(char* version, size_t versionLen, String& url) {
  WiFiClientSecure client;
  client.setInsecure();
  // Default is 120 s — a wedged handshake must not stall the data task.
  client.setHandshakeTimeout(10);
  HTTPClient http;
  http.begin(client, OTA_MANIFEST_URL);
  http.setTimeout(10000);
  int httpCode = http.GET();
  DBG_PRINTF("[OTA] manifest -> HTTP %d\n", httpCode);
  if (httpCode != HTTP_CODE_OK) {
    http.end();
    return false;
  }
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, http.getStream());
  http.end();
  if (error) {
    DBG_PRINTF("[OTA] manifest parse error: %s\n", error.c_str());
    return false;
  }
  strlcpy(version, doc["version"] | "", versionLen);
  const char* urlField = doc["url"] | "";
  if (urlField[0] != '\0') {
    url = urlField;
  } else {
    // Manifest only carried a file name — resolve it against the manifest
    // URL's directory.
    const char* fileField = doc["file"] | "";
    String base(OTA_MANIFEST_URL);
    int slash = base.lastIndexOf('/');
    url = base.substring(0, slash + 1) + fileField;
  }
  return version[0] != '\0' && url.length() > 0;
}

// Downloads and flashes the given firmware image, publishing progress to
// the renderer. Returns true when the new image is written and verified.
bool downloadAndFlash(const String& url, const char* targetVersion) {
  setOtaTargetVersion(targetVersion);

  WiFiClientSecure client;
  client.setInsecure();
  client.setHandshakeTimeout(10);
  HTTPClient http;
  http.begin(client, url);
  // Large binary over lossy Wi-Fi: generous per-read timeout, plus a
  // no-progress watchdog below.
  http.setTimeout(15000);
  int httpCode = http.GET();
  DBG_PRINTF("[OTA] firmware -> HTTP %d (%d bytes)\n", httpCode,
             (int)http.getSize());
  if (httpCode != HTTP_CODE_OK) {
    http.end();
    return false;
  }

  int total = http.getSize();  // -1 when chunked
  if (!Update.begin(total > 0 ? (size_t)total : UPDATE_SIZE_UNKNOWN)) {
    DBG_PRINTF("[OTA] Update.begin failed: %s\n", Update.errorString());
    http.end();
    return false;
  }

  publishOtaStage(OtaStage::DOWNLOADING, 0);
  WiFiClient* stream = http.getStreamPtr();
  size_t written = 0;
  uint32_t lastProgressAt = millis();
  uint32_t lastByteAt = millis();
  for (;;) {
    size_t avail = stream->available();
    if (avail == 0) {
      if ((total > 0 && written >= (size_t)total) || !stream->connected()) {
        break;
      }
      if (millis() - lastByteAt > OTA_DOWNLOAD_STALL_MS) {
        DBG_PRINTF("[OTA] download stalled at %u bytes, aborting\n",
                   (unsigned)written);
        Update.abort();
        http.end();
        return false;
      }
      delay(5);
      continue;
    }
    size_t chunk = sizeof(sOtaBuf);
    if (chunk > avail) chunk = avail;
    size_t got = stream->readBytes(sOtaBuf, chunk);
    if (got == 0) continue;
    lastByteAt = millis();
    if (Update.write(sOtaBuf, got) != got) {
      DBG_PRINTF("[OTA] flash write failed: %s\n", Update.errorString());
      Update.abort();
      http.end();
      return false;
    }
    written += got;
    if (total > 0) {
      int pct = (int)(written * 100 / (size_t)total);
      if (millis() - lastProgressAt >= 250) {
        lastProgressAt = millis();
        publishOtaStage(OtaStage::DOWNLOADING, pct);
      }
    }
  }
  http.end();

  if (total > 0 && written != (size_t)total) {
    DBG_PRINTF("[OTA] short download: %u of %d bytes\n", (unsigned)written,
               total);
    Update.abort();
    return false;
  }
  if (!Update.end(true)) {
    DBG_PRINTF("[OTA] Update.end failed: %s\n", Update.errorString());
    return false;
  }
  DBG_PRINTF("[OTA] flashed %u bytes OK; rebooting\n", (unsigned)written);
  return true;
}

}  // namespace

void serviceOtaUpdates() {
  uint32_t now = millis();
  if (now < OTA_CHECK_DELAY_MS) return;  // let the boot feed fetches settle
  uint32_t interval =
      sLastCheckOk ? OTA_CHECK_INTERVAL_MS : OTA_CHECK_RETRY_MS;
  if (sCheckedOnce && (now - sLastCheckAt) < interval) return;
  sCheckedOnce = true;
  sLastCheckAt = now;

  // The manifest fetch is a TLS connection: run it alone, with the statsapi
  // session dropped and no feed fetches racing it (otaUpdateInProgress()
  // gates those in the data-task loop).
  closeMlbApiSession();

  char manifestVersion[16] = {};
  String url;
  if (!fetchManifest(manifestVersion, sizeof(manifestVersion), url)) {
    sLastCheckOk = false;
    return;
  }
  sLastCheckOk = true;

  if (strcmp(manifestVersion, FIRMWARE_VERSION) == 0) {
    DBG_PRINTF("[OTA] up to date (%s)\n", FIRMWARE_VERSION);
    return;
  }
  DBG_PRINTF("[OTA] update available: %s -> %s\n", FIRMWARE_VERSION,
             manifestVersion);

  if (downloadAndFlash(url, manifestVersion)) {
    publishOtaStage(OtaStage::REBOOTING, 100);
    // Give the renderer a few seconds to show the reboot notice.
    uint32_t noticeUntil = millis() + 4000;
    while (millis() < noticeUntil) delay(100);
    ESP.restart();
  }

  // Failed update: tell the user, then fall back to normal operation. The
  // current firmware partition is untouched — Update only swaps on a fully
  // written + verified image.
  publishOtaStage(OtaStage::FAILED, 0);
  uint32_t failUntil = millis() + 5000;
  while (millis() < failUntil) delay(100);
  publishOtaStage(OtaStage::NONE, 0);
}

bool otaUpdateInProgress() {
  OtaStage stage = getOtaStage();
  return stage == OtaStage::DOWNLOADING || stage == OtaStage::REBOOTING;
}
