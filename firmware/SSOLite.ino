/*
 * ============================================================================
 *  SmartSpool OS Lite (SSO-Lite)  v1.0
 *  ESP32 + MFRC522 -> Bambu Lab AMS Slot 1 auto-assignment over MQTT/TLS
 * ----------------------------------------------------------------------------
 *  Target board:  ESP32 DevKit V1 / WROOM-32
 *  Toolchain:     Arduino IDE 2.x  with esp32 core 2.0.x or 3.0.x
 *  Required libs:
 *    - MFRC522         by Miguel Balboa     (>= 1.4.10)
 *    - PubSubClient    by Nick O'Leary       (>= 2.8)
 *    - ArduinoJson     by Benoit Blanchon    (>= 6.21, NOT v7)
 *  All other includes ship with the ESP32 Arduino core.
 * ============================================================================
 */

#include <Arduino.h>
#include <SPI.h>
#include <MFRC522.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Update.h>
#include <Preferences.h>
#include <vector>

#include "web_pages.h"

/* =========================================================================
 *  PIN MAP   (avoid GPIO 0/2/12/15 strap pins; 6-11 are flash; 34-39 input-only)
 * ========================================================================= */
#define PIN_RFID_SS    5     // MFRC522 SDA / SS
#define PIN_RFID_RST   22    // MFRC522 RST
// SPI default on ESP32: SCK=18, MISO=19, MOSI=23
#define PIN_LED        2     // on-board LED (status)

/* =========================================================================
 *  CONSTANTS
 * ========================================================================= */
#define FW_VERSION       "1.3.0"
#define MAGIC_HEADER     "SSO1"          // 4 bytes - identifies our tags
/* Storage spans 4 blocks across 2 sectors = 64 bytes total.
 * Sector trailers (blocks 7, 11) are NEVER touched. */
#define DATA_BLOCK_A     4                // sector 1 block 0
#define DATA_BLOCK_B     5                // sector 1 block 1
#define DATA_BLOCK_C     6                // sector 1 block 2
#define DATA_BLOCK_D     8                // sector 2 block 0
#define TAG_BLOB_SIZE    64
#define TAG_PAYLOAD_MAX  57               // 64 - 4(magic) - 1(ver) - 1(len) - 1(csum)
#define MAX_PROFILES     32
#define MAX_UID_MAPS     32
#define MAX_HISTORY      5
#define DEBOUNCE_MS      3000             // same tag twice within this window = ignored
#define MQTT_RETRY_MS    5000
#define WIFI_RETRY_MS    10000
#define ASSIGN_TIMEOUT_MS 4000

/* =========================================================================
 *  TYPES
 * ========================================================================= */
enum SystemState : uint8_t {
  STATE_BOOT, STATE_WIFI_CONNECTING, STATE_MQTT_CONNECTING,
  STATE_IDLE, STATE_READING, STATE_WRITING, STATE_ASSIGNING, STATE_ERROR
};

struct SystemConfig {
  char     wifi_ssid[33];
  char     wifi_pass[65];
  char     printer_ip[16];
  char     printer_serial[20];
  char     lan_code[17];
  bool     auto_assign;
  bool     auto_rewrite;     // v1.2: rewrite tag when touchscreen changes slot 1 info
  uint8_t  ams_id;          // pinned to 0
  uint8_t  tray_id;         // pinned to 0  (Slot 1)
};

struct FilamentProfile {
  char     name[24];        // user-friendly label
  char     material[8];     // PLA / PETG / ABS / TPU / PA / PC / PVA
  char     code[8];         // tray_info_idx, e.g. GFL99
  char     color[7];        // RRGGBB (no #)
  char     brand[20];
  uint16_t nozzle_min;
  uint16_t nozzle_max;
  uint16_t bed_temp;
  uint16_t flow_x1000;      // 1000 = 1.00x
  bool     in_use;
};

struct UidMap {                     // UID-only fallback if data blocks corrupt
  uint8_t  uid[10];
  uint8_t  uid_len;
  char     code[8];
  bool     in_use;
};

struct LastScan {
  uint8_t  uid[10];
  uint8_t  uid_len;
  FilamentProfile profile;
  uint32_t timestamp;
  bool     valid;
  bool     wrote_to_printer;
  char     source[12];      // "tag" / "uid_map" / "manual"
};

struct PendingAssign {
  FilamentProfile profile;
  uint32_t next_attempt;
  uint8_t  attempts;
  bool     pending;
};

/* v1.2: when auto_rewrite is enabled and the printer reports a slot 1 change
 * that didn't come from us, we queue a rewrite of the tag so the next scan
 * persists the user's touchscreen change to the RFID tag itself. */
struct PendingRewrite {
  uint8_t  uid[10];
  uint8_t  uid_len;
  FilamentProfile profile;
  bool     pending;
  uint32_t queued_at;
};

/* v1.3: OTA update state — tracks an in-progress firmware upload from the
 * extension.  The upload handler populates this; the completion handler
 * reads it and either reboots or reports the error. */
struct OtaState {
  bool     active;
  bool     error;
  uint32_t bytes_written;
  uint32_t total_size;
  char     error_msg[64];
};

/* =========================================================================
 *  GLOBALS
 * ========================================================================= */
MFRC522            rfid(PIN_RFID_SS, PIN_RFID_RST);
MFRC522::MIFARE_Key defaultKey;          // FFFFFFFFFFFF
WiFiClientSecure   tlsClient;
PubSubClient       mqtt(tlsClient);
WebServer          httpd(80);
Preferences        prefs;

SystemConfig       g_cfg;
SystemState        g_state = STATE_BOOT;
LastScan           g_lastScan = {};
PendingAssign      g_pending  = {};
PendingRewrite     g_pendingRewrite = {};
OtaState           g_ota           = {};
FilamentProfile    g_profiles[MAX_PROFILES];
UidMap             g_uidMaps[MAX_UID_MAPS];
LastScan           g_history[MAX_HISTORY];
uint8_t            g_historyCount = 0;
FilamentProfile    g_currentSlot1 = {};   // last reported AMS slot 1 state
bool               g_officialTagDetected = false;
uint32_t           g_lastWifiAttempt = 0;
uint32_t           g_lastMqttAttempt = 0;
uint32_t           g_lastSelfTest    = 0;

/* Pending write request, queued by web UI, executed when a tag is presented */
struct PendingWrite {
  FilamentProfile profile;
  bool     pending;
  uint32_t expires;
};
PendingWrite g_pendingWrite = {};

/* Forward declarations */
void   loadConfig();
void   saveConfig();
void   loadProfiles();
void   saveProfiles();
void   loadUidMaps();
void   saveUidMaps();
void   seedDefaultProfiles();
void   initRFID();
void   initWiFiSTA();
void   initMQTT();
void   initWebServer();
void   rfidPoll();
void   handleTag(MFRC522::Uid &uid);
bool   readTagData(FilamentProfile &out);
bool   writeTagData(const FilamentProfile &p);
bool   authBlock(uint8_t block);
String encodeProfile(const FilamentProfile &p);
bool   decodeProfile(const String &raw, FilamentProfile &out);
FilamentProfile* findProfileByCode(const char *code);
FilamentProfile* findProfileByName(const char *name);
UidMap* findUidMap(const uint8_t *uid, uint8_t len);
void   pushHistory(const LastScan &s);
void   sendAmsAssign(const FilamentProfile &p);
void   mqttCallback(char *topic, byte *payload, unsigned int len);
void   processReport(const JsonDocument &doc);
void   setLED(bool on);
void   blinkLED(uint8_t times, uint16_t ms);
String uidToHex(const uint8_t *uid, uint8_t len);
uint8_t xorChecksum(const uint8_t *data, size_t len);
/* v1.3 OTA */
void   handleOtaUpload();
void   handleOtaComplete();

/* =========================================================================
 *  SETUP / LOOP
 * ========================================================================= */
void setup() {
  Serial.begin(115200);
  delay(150);
  Serial.println();
  Serial.println(F("====================================================="));
  Serial.println(F("  SmartSpool OS Lite  v" FW_VERSION));
  Serial.println(F("====================================================="));

  pinMode(PIN_LED, OUTPUT);
  setLED(false);

  /* Persistent storage */
  prefs.begin("ssolite", false);
  loadConfig();
  loadProfiles();
  loadUidMaps();
  if (!prefs.getBool("seeded", false)) {
    seedDefaultProfiles();
    prefs.putBool("seeded", true);
    saveProfiles();
  }

  /* Hardware */
  initRFID();

  /* Network */
  initWiFiSTA();

  /* MQTT */
  tlsClient.setInsecure();        // Bambu uses a self-signed cert
  mqtt.setBufferSize(4096);       // reports can be big
  mqtt.setKeepAlive(30);
  mqtt.setSocketTimeout(10);
  initMQTT();

  /* Web UI */
  initWebServer();

  g_state = STATE_IDLE;
  Serial.println(F("[BOOT] Ready."));
  blinkLED(3, 80);
}

void loop() {
  /* Web first - keep UI responsive even when offline */
  httpd.handleClient();

  /* WiFi watchdog */
  if (WiFi.status() != WL_CONNECTED) {
    if (millis() - g_lastWifiAttempt > WIFI_RETRY_MS) {
      Serial.println(F("[WiFi] reconnecting..."));
      WiFi.reconnect();
      g_lastWifiAttempt = millis();
    }
  } else {
    /* MQTT loop only when WiFi is up */
    if (!mqtt.connected()) {
      if (millis() - g_lastMqttAttempt > MQTT_RETRY_MS) {
        initMQTT();
        g_lastMqttAttempt = millis();
      }
    } else {
      mqtt.loop();
    }
  }

  /* Process pending AMS assignment retries */
  if (g_pending.pending && millis() >= g_pending.next_attempt) {
    if (mqtt.connected()) {
      Serial.printf("[ASSIGN] retry attempt %u\n", g_pending.attempts + 1);
      sendAmsAssign(g_pending.profile);
    } else {
      g_pending.next_attempt = millis() + MQTT_RETRY_MS;
    }
  }

  /* RFID polling */
  rfidPoll();

  /* Pending write expiration */
  if (g_pendingWrite.pending && millis() > g_pendingWrite.expires) {
    Serial.println(F("[WRITE] queued write expired, cancelling."));
    g_pendingWrite.pending = false;
  }

  delay(15);
}

/* =========================================================================
 *  CONFIG PERSISTENCE
 * ========================================================================= */
void loadConfig() {
  prefs.getString("wifi_ssid", g_cfg.wifi_ssid, sizeof(g_cfg.wifi_ssid));
  prefs.getString("wifi_pass", g_cfg.wifi_pass, sizeof(g_cfg.wifi_pass));
  prefs.getString("printer_ip", g_cfg.printer_ip, sizeof(g_cfg.printer_ip));
  prefs.getString("printer_sn", g_cfg.printer_serial, sizeof(g_cfg.printer_serial));
  prefs.getString("lan_code",  g_cfg.lan_code, sizeof(g_cfg.lan_code));
  g_cfg.auto_assign  = prefs.getBool("auto_assign", true);
  g_cfg.auto_rewrite = prefs.getBool("auto_rewrite", false);   // off by default
  g_cfg.ams_id  = 0;
  g_cfg.tray_id = 0;
  Serial.printf("[CFG] ssid=%s ip=%s sn=%s auto=%d rewrite=%d\n",
                g_cfg.wifi_ssid, g_cfg.printer_ip, g_cfg.printer_serial,
                g_cfg.auto_assign, g_cfg.auto_rewrite);
}

void saveConfig() {
  prefs.putString("wifi_ssid", g_cfg.wifi_ssid);
  prefs.putString("wifi_pass", g_cfg.wifi_pass);
  prefs.putString("printer_ip", g_cfg.printer_ip);
  prefs.putString("printer_sn", g_cfg.printer_serial);
  prefs.putString("lan_code",  g_cfg.lan_code);
  prefs.putBool  ("auto_assign",  g_cfg.auto_assign);
  prefs.putBool  ("auto_rewrite", g_cfg.auto_rewrite);
}

/* =========================================================================
 *  PROFILES
 * ========================================================================= */
void seedDefaultProfiles() {
  memset(g_profiles, 0, sizeof(g_profiles));
  struct Seed { const char *name, *mat, *code, *color, *brand; uint16_t nmin,nmax,bed; };
  const Seed defaults[] = {
    {"Generic PLA Black",   "PLA",  "GFL99", "000000", "Generic",   190,230, 60},
    {"Generic PLA White",   "PLA",  "GFL99", "FFFFFF", "Generic",   190,230, 60},
    {"Generic PLA Red",     "PLA",  "GFL99", "C8161D", "Generic",   190,230, 60},
    {"Generic PLA Blue",    "PLA",  "GFL99", "0066CC", "Generic",   190,230, 60},
    {"Overture PLA Matte",  "PLA",  "GFL99", "1C1C1C", "Overture",  200,220, 60},
    {"Generic PETG",        "PETG", "GFG99", "0099A8", "Generic",   230,255, 75},
    {"Generic PETG Black",  "PETG", "GFG99", "000000", "Generic",   230,255, 75},
    {"Generic ABS",         "ABS",  "GFB99", "F2F2F2", "Generic",   240,270, 95},
    {"Generic TPU 95A",     "TPU",  "GFU99", "FFFFFF", "Generic",   220,240, 50},
    {"Generic PA (Nylon)",  "PA",   "GFN99", "303030", "Generic",   260,290, 80},
    {"Generic PC",          "PC",   "GFC99", "F0F0F0", "Generic",   270,290, 100},
    {"Generic PVA Support", "PVA",  "GFS99", "F4E2B8", "Generic",   200,210, 60},
  };
  size_t n = sizeof(defaults)/sizeof(defaults[0]);
  for (size_t i = 0; i < n && i < MAX_PROFILES; i++) {
    auto &p = g_profiles[i];
    strncpy(p.name,     defaults[i].name,  sizeof(p.name)-1);
    strncpy(p.material, defaults[i].mat,   sizeof(p.material)-1);
    strncpy(p.code,     defaults[i].code,  sizeof(p.code)-1);
    strncpy(p.color,    defaults[i].color, sizeof(p.color)-1);
    strncpy(p.brand,    defaults[i].brand, sizeof(p.brand)-1);
    p.nozzle_min = defaults[i].nmin;
    p.nozzle_max = defaults[i].nmax;
    p.bed_temp   = defaults[i].bed;
    p.flow_x1000 = 1000;
    p.in_use     = true;
  }
  Serial.printf("[PROFILES] seeded %u defaults\n", (unsigned)n);
}

void loadProfiles() {
  size_t got = prefs.getBytes("profiles", g_profiles, sizeof(g_profiles));
  if (got != sizeof(g_profiles)) {
    memset(g_profiles, 0, sizeof(g_profiles));
  }
}

void saveProfiles() {
  prefs.putBytes("profiles", g_profiles, sizeof(g_profiles));
}

void loadUidMaps() {
  size_t got = prefs.getBytes("uidmaps", g_uidMaps, sizeof(g_uidMaps));
  if (got != sizeof(g_uidMaps)) memset(g_uidMaps, 0, sizeof(g_uidMaps));
}

void saveUidMaps() {
  prefs.putBytes("uidmaps", g_uidMaps, sizeof(g_uidMaps));
}

FilamentProfile* findProfileByCode(const char *code) {
  for (auto &p : g_profiles) if (p.in_use && strcmp(p.code, code) == 0) return &p;
  return nullptr;
}
FilamentProfile* findProfileByName(const char *name) {
  for (auto &p : g_profiles) if (p.in_use && strcmp(p.name, name) == 0) return &p;
  return nullptr;
}
UidMap* findUidMap(const uint8_t *uid, uint8_t len) {
  for (auto &m : g_uidMaps) {
    if (!m.in_use) continue;
    if (m.uid_len != len) continue;
    if (memcmp(m.uid, uid, len) == 0) return &m;
  }
  return nullptr;
}

/* =========================================================================
 *  RFID
 * ========================================================================= */
void initRFID() {
  SPI.begin();                          // SCK=18 MISO=19 MOSI=23
  rfid.PCD_Init();
  delay(50);
  for (uint8_t i = 0; i < 6; i++) defaultKey.keyByte[i] = 0xFF;
  byte v = rfid.PCD_ReadRegister(MFRC522::VersionReg);
  Serial.printf("[RFID] MFRC522 firmware version: 0x%02X\n", v);
  if (v == 0x00 || v == 0xFF) {
    Serial.println(F("[RFID] WARNING - reader not responding. Check wiring/power."));
  }
}

bool authBlock(uint8_t block) {
  return rfid.PCD_Authenticate(MFRC522::PICC_CMD_MF_AUTH_KEY_A, block,
                               &defaultKey, &(rfid.uid)) == MFRC522::STATUS_OK;
}

void rfidPoll() {
  /* PICC_IsNewCardPresent has to be called repeatedly; it does not block */
  if (!rfid.PICC_IsNewCardPresent()) return;
  if (!rfid.PICC_ReadCardSerial())   return;

  /* Detect Bambu official tag (heuristic: tag is NTAG/ULTRALIGHT, NOT MIFARE Classic).
   * Real Bambu spools use NTAG216 in some revisions; we are not allowed to spoof,
   * so when we see a non-Classic tag we *disable* automation for safety. */
  MFRC522::PICC_Type t = rfid.PICC_GetType(rfid.uid.sak);
  if (t != MFRC522::PICC_TYPE_MIFARE_1K &&
      t != MFRC522::PICC_TYPE_MIFARE_4K &&
      t != MFRC522::PICC_TYPE_MIFARE_MINI) {
    g_officialTagDetected = true;
    Serial.println(F("[RFID] Non-Classic tag (likely Bambu/NTAG). Auto-assign disabled."));
    rfid.PICC_HaltA();
    rfid.PCD_StopCrypto1();
    return;
  }
  g_officialTagDetected = false;

  handleTag(rfid.uid);

  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
}

void handleTag(MFRC522::Uid &uid) {
  /* Debounce identical UID */
  bool sameUid = (uid.size == g_lastScan.uid_len) &&
                 (memcmp(uid.uidByte, g_lastScan.uid, uid.size) == 0);
  uint32_t now = millis();
  if (sameUid && g_lastScan.valid && (now - g_lastScan.timestamp < DEBOUNCE_MS)) {
    return;   // ignore bounces
  }

  Serial.printf("[RFID] tag UID=%s\n", uidToHex(uid.uidByte, uid.size).c_str());
  setLED(true);

  /* If web UI queued a write request, fulfil it now */
  if (g_pendingWrite.pending) {
    Serial.println(F("[WRITE] writing pending profile to tag..."));
    bool ok = writeTagData(g_pendingWrite.profile);
    Serial.printf("[WRITE] result: %s\n", ok ? "OK" : "FAIL");
    g_pendingWrite.pending = false;
    blinkLED(ok ? 2 : 5, 80);
    setLED(false);
    return;
  }

  /* v1.2: auto-rewrite — if there's a queued tag rewrite for THIS UID
   * (printer reported a slot 1 change since this tag was last scanned),
   * write the new profile to the tag before doing the normal read flow. */
  if (g_cfg.auto_rewrite && g_pendingRewrite.pending &&
      g_pendingRewrite.uid_len == uid.size &&
      memcmp(g_pendingRewrite.uid, uid.uidByte, uid.size) == 0) {
    Serial.println(F("[REWRITE] applying queued touchscreen change to tag..."));
    bool ok = writeTagData(g_pendingRewrite.profile);
    Serial.printf("[REWRITE] result: %s\n", ok ? "OK" : "FAIL");
    g_pendingRewrite.pending = false;
    /* Fall through into the normal read path so we still update the AMS
     * with the new profile and refresh history. The tag now contains the
     * new data, so readTagData() below will read the freshly written
     * payload. */
    if (!ok) {
      blinkLED(5, 80);
      setLED(false);
      return;
    }
  }

  /* Read tag data */
  FilamentProfile p = {};
  bool readOk = readTagData(p);
  const char *src = "tag";

  if (!readOk) {
    /* Fallback to UID map */
    UidMap *m = findUidMap(uid.uidByte, uid.size);
    if (m) {
      FilamentProfile *fp = findProfileByCode(m->code);
      if (fp) { p = *fp; readOk = true; src = "uid_map"; }
    }
  }

  if (!readOk) {
    Serial.println(F("[RFID] tag unreadable and no UID mapping."));
    blinkLED(4, 60);
    setLED(false);
    return;
  }

  /* Resolve to local profile (canonicalises temps) */
  FilamentProfile *known = findProfileByCode(p.code);
  if (known) p = *known;

  /* Update last-scan and history */
  memcpy(g_lastScan.uid, uid.uidByte, uid.size);
  g_lastScan.uid_len = uid.size;
  g_lastScan.profile = p;
  g_lastScan.timestamp = now;
  g_lastScan.valid = true;
  g_lastScan.wrote_to_printer = false;
  strncpy(g_lastScan.source, src, sizeof(g_lastScan.source)-1);
  pushHistory(g_lastScan);

  /* Auto-assign */
  if (g_cfg.auto_assign && !g_officialTagDetected) {
    sendAmsAssign(p);
  }

  blinkLED(2, 60);
  setLED(false);
}

void pushHistory(const LastScan &s) {
  /* prepend */
  for (int i = MAX_HISTORY-1; i > 0; i--) g_history[i] = g_history[i-1];
  g_history[0] = s;
  if (g_historyCount < MAX_HISTORY) g_historyCount++;
}

/* ---------------- Tag data layout ----------------
 * 4 blocks across 2 sectors = 64 bytes total.
 *   Sector 1 block 4  -> bytes  0..15
 *   Sector 1 block 5  -> bytes 16..31
 *   Sector 1 block 6  -> bytes 32..47
 *   Sector 2 block 8  -> bytes 48..63
 *   (Block 7 = sector 1 trailer, block 11 = sector 2 trailer; never written.)
 *
 *   bytes 0..3  : magic "SSO1"
 *   byte  4     : version (0x01)
 *   byte  5     : payload length N (<= 57)
 *   bytes 6..6+N-1 : ASCII payload, e.g.
 *                "M=PLA;C=GFL99;X=000000;B=Overture;N=210;D=60"
 *   bytes 6+N..62  : zero pad
 *   byte  63    : XOR checksum of bytes 0..62
 * --------------------------------------------------*/
String encodeProfile(const FilamentProfile &p) {
  /* Use mid-temperature of nozzle range as N (printer recomputes anyway). */
  uint16_t ntemp = (p.nozzle_min + p.nozzle_max) / 2;
  char buf[64];
  snprintf(buf, sizeof(buf),
           "M=%s;C=%s;X=%s;B=%s;N=%u;D=%u",
           p.material, p.code, p.color, p.brand, ntemp, p.bed_temp);
  return String(buf);
}

bool decodeProfile(const String &raw, FilamentProfile &out) {
  memset(&out, 0, sizeof(out));
  int start = 0;
  while (start < (int)raw.length()) {
    int end = raw.indexOf(';', start);
    if (end < 0) end = raw.length();
    String tok = raw.substring(start, end);
    int eq = tok.indexOf('=');
    if (eq > 0) {
      String k = tok.substring(0, eq);
      String v = tok.substring(eq+1);
      if      (k == "M") strncpy(out.material, v.c_str(), sizeof(out.material)-1);
      else if (k == "C") strncpy(out.code,     v.c_str(), sizeof(out.code)-1);
      else if (k == "X") strncpy(out.color,    v.c_str(), sizeof(out.color)-1);
      else if (k == "B") strncpy(out.brand,    v.c_str(), sizeof(out.brand)-1);
      else if (k == "N") { uint16_t t = v.toInt(); out.nozzle_min = t-20; out.nozzle_max = t+20; }
      else if (k == "D") out.bed_temp = v.toInt();
    }
    start = end + 1;
  }
  out.flow_x1000 = 1000;
  out.in_use = true;
  if (strlen(out.code) == 0 || strlen(out.material) == 0) return false;
  if (strlen(out.brand) == 0) strncpy(out.brand, "Generic", sizeof(out.brand)-1);
  if (strlen(out.color) == 0) strncpy(out.color, "808080", sizeof(out.color)-1);
  snprintf(out.name, sizeof(out.name), "%s %s", out.brand, out.material);
  return true;
}

uint8_t xorChecksum(const uint8_t *data, size_t len) {
  uint8_t c = 0; for (size_t i = 0; i < len; i++) c ^= data[i]; return c;
}

bool readTagData(FilamentProfile &out) {
  uint8_t blob[TAG_BLOB_SIZE];
  uint8_t buf[18]; byte sz;
  uint8_t blocks[4] = {DATA_BLOCK_A, DATA_BLOCK_B, DATA_BLOCK_C, DATA_BLOCK_D};
  for (int i = 0; i < 4; i++) {
    if (!authBlock(blocks[i])) {
      Serial.printf("[RFID] auth fail block %u\n", blocks[i]);
      return false;
    }
    sz = sizeof(buf);
    if (rfid.MIFARE_Read(blocks[i], buf, &sz) != MFRC522::STATUS_OK) {
      Serial.printf("[RFID] read fail block %u\n", blocks[i]);
      return false;
    }
    memcpy(&blob[i*16], buf, 16);
  }
  if (memcmp(blob, MAGIC_HEADER, 4) != 0) {
    Serial.println(F("[RFID] no SSO1 magic on tag."));
    return false;
  }
  uint8_t ver = blob[4];
  uint8_t plen = blob[5];
  uint8_t cs  = blob[TAG_BLOB_SIZE - 1];
  if (plen > TAG_PAYLOAD_MAX) plen = TAG_PAYLOAD_MAX;
  uint8_t calc = xorChecksum(blob, TAG_BLOB_SIZE - 1);
  if (calc != cs) {
    Serial.printf("[RFID] checksum mismatch (calc=%02X stored=%02X). Trying recovery...\n", calc, cs);
    /* Attempt recovery: parse anyway, decoder is forgiving */
  }
  if (ver != 0x01) Serial.printf("[RFID] unknown version %u, attempting parse anyway.\n", ver);

  char payload[TAG_PAYLOAD_MAX + 1] = {0};
  memcpy(payload, &blob[6], plen);
  payload[plen] = 0;
  String raw = String(payload);
  Serial.printf("[RFID] payload: %s\n", raw.c_str());
  return decodeProfile(raw, out);
}

bool writeTagData(const FilamentProfile &p) {
  String raw = encodeProfile(p);
  if (raw.length() > TAG_PAYLOAD_MAX) {
    Serial.printf("[WRITE] payload %u too long (max %u).\n",
                  raw.length(), TAG_PAYLOAD_MAX);
    return false;
  }
  uint8_t blob[TAG_BLOB_SIZE] = {0};
  memcpy(blob, MAGIC_HEADER, 4);
  blob[4] = 0x01;                       // version
  blob[5] = (uint8_t)raw.length();
  memcpy(&blob[6], raw.c_str(), raw.length());
  blob[TAG_BLOB_SIZE - 1] = xorChecksum(blob, TAG_BLOB_SIZE - 1);

  uint8_t blocks[4] = {DATA_BLOCK_A, DATA_BLOCK_B, DATA_BLOCK_C, DATA_BLOCK_D};
  for (int i = 0; i < 4; i++) {
    /* authBlock() crosses sectors transparently - the library re-keys when
     * the requested block is in a different sector than the last auth. */
    if (!authBlock(blocks[i])) {
      Serial.printf("[WRITE] auth fail block %u\n", blocks[i]);
      return false;
    }
    auto st = rfid.MIFARE_Write(blocks[i], &blob[i*16], 16);
    if (st != MFRC522::STATUS_OK) {
      Serial.printf("[WRITE] write fail block %u status=%d\n", blocks[i], st);
      return false;
    }
  }

  /* Update / add UID fallback mapping */
  UidMap *m = findUidMap(rfid.uid.uidByte, rfid.uid.size);
  if (!m) for (auto &x : g_uidMaps) if (!x.in_use) { m = &x; break; }
  if (m) {
    memcpy(m->uid, rfid.uid.uidByte, rfid.uid.size);
    m->uid_len = rfid.uid.size;
    strncpy(m->code, p.code, sizeof(m->code)-1);
    m->in_use = true;
    saveUidMaps();
  }
  return true;
}

/* =========================================================================
 *  WIFI
 * ========================================================================= */
void initWiFiSTA() {
  if (strlen(g_cfg.wifi_ssid) == 0) {
    /* Fallback: open AP for setup */
    Serial.println(F("[WiFi] No SSID configured. Starting AP 'SSOLite-Setup'..."));
    WiFi.mode(WIFI_AP);
    WiFi.softAP("SSOLite-Setup", "spool1234");
    Serial.print(F("[WiFi] AP IP: "));
    Serial.println(WiFi.softAPIP());
    return;
  }
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(g_cfg.wifi_ssid, g_cfg.wifi_pass);
  Serial.printf("[WiFi] connecting to %s ", g_cfg.wifi_ssid);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) {
    delay(250); Serial.print('.');
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print(F("[WiFi] connected. IP="));
    Serial.println(WiFi.localIP());
  } else {
    Serial.println(F("[WiFi] FAILED. Falling back to AP."));
    WiFi.mode(WIFI_AP);
    WiFi.softAP("SSOLite-Setup", "spool1234");
    Serial.print(F("[WiFi] AP IP: "));
    Serial.println(WiFi.softAPIP());
  }

  /* mDNS: makes the device reachable as ssolite.local on the LAN,
   * which the Chrome extension uses for zero-config discovery. */
  if (MDNS.begin("ssolite")) {
    MDNS.addService("http", "tcp", 80);
    MDNS.addServiceTxt("http", "tcp", "fw", FW_VERSION);
    MDNS.addServiceTxt("http", "tcp", "name", "SmartSpool OS Lite");
    Serial.println(F("[mDNS] http://ssolite.local/"));
  } else {
    Serial.println(F("[mDNS] failed to start"));
  }
}

/* =========================================================================
 *  MQTT (Bambu LAN)
 * ========================================================================= */
void initMQTT() {
  if (WiFi.status() != WL_CONNECTED)        return;
  if (strlen(g_cfg.printer_ip) == 0)        return;
  if (strlen(g_cfg.lan_code)  == 0)         return;
  if (strlen(g_cfg.printer_serial) == 0)    return;

  Serial.printf("[MQTT] connecting to %s:8883 sn=%s\n",
                g_cfg.printer_ip, g_cfg.printer_serial);
  mqtt.setServer(g_cfg.printer_ip, 8883);
  mqtt.setCallback(mqttCallback);

  String clientId = String("SSOLite-") + String((uint32_t)ESP.getEfuseMac(), HEX);
  if (mqtt.connect(clientId.c_str(), "bblp", g_cfg.lan_code)) {
    Serial.println(F("[MQTT] connected."));
    String topic = String("device/") + g_cfg.printer_serial + "/report";
    mqtt.subscribe(topic.c_str());
    Serial.printf("[MQTT] subscribed: %s\n", topic.c_str());

    /* Ask for full status push */
    String reqTopic = String("device/") + g_cfg.printer_serial + "/request";
    const char *pushAll = "{\"pushing\":{\"sequence_id\":\"0\",\"command\":\"pushall\"}}";
    mqtt.publish(reqTopic.c_str(), pushAll);
  } else {
    Serial.printf("[MQTT] failed, rc=%d\n", mqtt.state());
  }
}

void sendAmsAssign(const FilamentProfile &p) {
  if (!mqtt.connected()) {
    Serial.println(F("[ASSIGN] MQTT not connected, queueing."));
    g_pending.profile = p;
    g_pending.pending = true;
    g_pending.attempts = 0;
    g_pending.next_attempt = millis() + MQTT_RETRY_MS;
    return;
  }

  /* tray_color is RRGGBBAA */
  char colorAA[10];
  snprintf(colorAA, sizeof(colorAA), "%sFF", p.color);

  StaticJsonDocument<512> doc;
  JsonObject prn = doc.createNestedObject("print");
  prn["sequence_id"]      = String(millis());
  prn["command"]          = "ams_filament_setting";
  prn["ams_id"]           = g_cfg.ams_id;
  prn["tray_id"]          = g_cfg.tray_id;
  prn["tray_info_idx"]    = p.code;
  prn["tray_color"]       = colorAA;
  prn["nozzle_temp_min"]  = p.nozzle_min;
  prn["nozzle_temp_max"]  = p.nozzle_max;
  prn["tray_type"]        = p.material;

  char payload[512];
  size_t n = serializeJson(doc, payload, sizeof(payload));
  String topic = String("device/") + g_cfg.printer_serial + "/request";
  bool ok = mqtt.publish(topic.c_str(), (const uint8_t*)payload, n, false);
  Serial.printf("[ASSIGN] publish %s: %s\n", topic.c_str(), ok ? "OK" : "FAIL");
  Serial.printf("         payload=%s\n", payload);

  if (ok) {
    g_pending.pending = false;
    g_pending.attempts = 0;
    g_lastScan.wrote_to_printer = true;
    g_currentSlot1 = p;          // optimistic; report will confirm
  } else {
    g_pending.profile = p;
    g_pending.pending = true;
    g_pending.attempts++;
    /* exponential backoff capped */
    uint32_t delayMs = MQTT_RETRY_MS * (1 << min<uint8_t>(g_pending.attempts, 4));
    g_pending.next_attempt = millis() + delayMs;
  }
}

void mqttCallback(char *topic, byte *payload, unsigned int len) {
  /* Bambu reports are large; we only inspect AMS slot 1 fields. */
  StaticJsonDocument<256> filter;
  filter["print"]["ams"]["ams"][0]["tray"][0]["tray_info_idx"] = true;
  filter["print"]["ams"]["ams"][0]["tray"][0]["tray_color"]    = true;
  filter["print"]["ams"]["ams"][0]["tray"][0]["tray_type"]     = true;

  DynamicJsonDocument doc(2048);
  DeserializationError err = deserializeJson(doc, payload, len, DeserializationOption::Filter(filter));
  if (err) return;
  processReport(doc);
}

void processReport(const JsonDocument &doc) {
  JsonVariantConst tray = doc["print"]["ams"]["ams"][0]["tray"][0];
  if (tray.isNull()) return;
  const char *idx = tray["tray_info_idx"] | "";
  const char *col = tray["tray_color"]    | "";
  const char *typ = tray["tray_type"]     | "";

  if (strlen(idx)) strncpy(g_currentSlot1.code,     idx, sizeof(g_currentSlot1.code)-1);
  if (strlen(typ)) strncpy(g_currentSlot1.material, typ, sizeof(g_currentSlot1.material)-1);
  if (strlen(col) >= 6) {
    strncpy(g_currentSlot1.color, col, 6);
    g_currentSlot1.color[6] = 0;
  }

  /* Detect change to slot 1 from a non-SSO source (touchscreen, slicer, etc).
   *
   * Two modes of response:
   *
   *   auto_rewrite ON (off by default):
   *     The user changed filament info via the touchscreen and wants the tag
   *     to learn the new info.  Queue a rewrite for the last-scanned UID
   *     using the printer's reported values.  The rewrite happens on the
   *     next tag scan, so the tag persists the user's change.  We do NOT
   *     reapply the old profile — the user's intent is to change it.
   *
   *   auto_rewrite OFF:
   *     Existing behavior — if auto_assign is on, reapply our last scanned
   *     profile to undo whatever drift happened (max once per 30s).
   *
   * In both modes we ignore reports that match what we just sent (echoes).
   */
  static uint32_t lastReapply = 0;
  bool drifted = (g_lastScan.valid &&
                  strlen(g_lastScan.profile.code) > 0 &&
                  strcmp(g_lastScan.profile.code, idx) != 0 &&
                  strlen(idx) > 0 &&
                  !g_officialTagDetected);

  if (drifted && g_cfg.auto_rewrite) {
    /* Build a profile from the report. Prefer a known local profile by
     * code, then override color from the report. If the code is unknown,
     * synthesize a minimal profile. */
    FilamentProfile rewrite = {};
    FilamentProfile *known = findProfileByCode(idx);
    if (known) {
      rewrite = *known;
    } else {
      strncpy(rewrite.code, idx, sizeof(rewrite.code)-1);
      strncpy(rewrite.material, typ, sizeof(rewrite.material)-1);
      strncpy(rewrite.brand, "Generic", sizeof(rewrite.brand)-1);
      snprintf(rewrite.name, sizeof(rewrite.name), "Touchscreen %s", typ);
      rewrite.nozzle_min = 200;
      rewrite.nozzle_max = 230;
      rewrite.bed_temp   = 60;
      rewrite.flow_x1000 = 1000;
      rewrite.in_use     = true;
    }
    if (strlen(col) >= 6) {
      strncpy(rewrite.color, col, 6);
      rewrite.color[6] = 0;
    }

    memcpy(g_pendingRewrite.uid, g_lastScan.uid, g_lastScan.uid_len);
    g_pendingRewrite.uid_len  = g_lastScan.uid_len;
    g_pendingRewrite.profile  = rewrite;
    g_pendingRewrite.pending  = true;
    g_pendingRewrite.queued_at = millis();
    Serial.printf("[REWRITE] queued: UID=%s code=%s color=%s — will apply on next scan\n",
                  uidToHex(g_pendingRewrite.uid, g_pendingRewrite.uid_len).c_str(),
                  rewrite.code, rewrite.color);
  } else if (drifted && g_cfg.auto_assign &&
             millis() - lastReapply > 30000) {
    Serial.println(F("[ASSIGN] detected drift; reapplying."));
    sendAmsAssign(g_lastScan.profile);
    lastReapply = millis();
  }
}

/* =========================================================================
 *  WEB SERVER
 * ========================================================================= */
String jsonEscape(const char *s) {
  String o = "\"";
  while (*s) {
    char c = *s++;
    if (c == '"' || c == '\\') o += '\\';
    o += c;
  }
  o += '"';
  return o;
}

void apiStatus() {
  StaticJsonDocument<1024> d;
  d["fw"]        = FW_VERSION;
  d["state"]     = (int)g_state;
  d["wifi_ip"]   = WiFi.localIP().toString();
  d["wifi_rssi"] = WiFi.RSSI();
  d["wifi_ok"]   = WiFi.status() == WL_CONNECTED;
  d["mqtt_ok"]   = mqtt.connected();
  d["auto"]      = g_cfg.auto_assign;
  d["auto_rewrite"] = g_cfg.auto_rewrite;
  d["official_tag_lock"] = g_officialTagDetected;

  /* Report any pending tag-rewrite so the web UI can display "tap your tag
   * to apply the latest touchscreen change". */
  JsonObject pr = d.createNestedObject("pending_rewrite");
  pr["pending"] = g_pendingRewrite.pending;
  if (g_pendingRewrite.pending) {
    pr["uid"]      = uidToHex(g_pendingRewrite.uid, g_pendingRewrite.uid_len);
    pr["code"]     = g_pendingRewrite.profile.code;
    pr["material"] = g_pendingRewrite.profile.material;
    pr["color"]    = g_pendingRewrite.profile.color;
    pr["age_ms"]   = (uint32_t)(millis() - g_pendingRewrite.queued_at);
  }

  JsonObject sl = d.createNestedObject("slot1");
  sl["code"]     = g_currentSlot1.code;
  sl["material"] = g_currentSlot1.material;
  sl["color"]    = g_currentSlot1.color;

  JsonObject ls = d.createNestedObject("last_scan");
  if (g_lastScan.valid) {
    ls["valid"]    = true;
    ls["uid"]      = uidToHex(g_lastScan.uid, g_lastScan.uid_len);
    ls["code"]     = g_lastScan.profile.code;
    ls["material"] = g_lastScan.profile.material;
    ls["color"]    = g_lastScan.profile.color;
    ls["brand"]    = g_lastScan.profile.brand;
    ls["name"]     = g_lastScan.profile.name;
    ls["source"]   = g_lastScan.source;
    ls["sent"]     = g_lastScan.wrote_to_printer;
    ls["age_ms"]   = (uint32_t)(millis() - g_lastScan.timestamp);
  } else {
    ls["valid"] = false;
  }

  JsonArray hist = d.createNestedArray("history");
  for (uint8_t i = 0; i < g_historyCount; i++) {
    JsonObject e = hist.createNestedObject();
    e["uid"]   = uidToHex(g_history[i].uid, g_history[i].uid_len);
    e["code"]  = g_history[i].profile.code;
    e["color"] = g_history[i].profile.color;
    e["name"]  = g_history[i].profile.name;
    e["t"]     = (uint32_t)g_history[i].timestamp;
  }
  String out; serializeJson(d, out);
  httpd.send(200, "application/json", out);
}

void apiProfiles() {
  DynamicJsonDocument d(8192);
  JsonArray a = d.to<JsonArray>();
  for (auto &p : g_profiles) {
    if (!p.in_use) continue;
    JsonObject o = a.createNestedObject();
    o["name"]        = p.name;
    o["material"]    = p.material;
    o["code"]        = p.code;
    o["color"]       = p.color;
    o["brand"]       = p.brand;
    o["nozzle_min"]  = p.nozzle_min;
    o["nozzle_max"]  = p.nozzle_max;
    o["bed"]         = p.bed_temp;
    o["flow_x1000"]  = p.flow_x1000;
  }
  String out; serializeJson(d, out);
  httpd.send(200, "application/json", out);
}

void apiSaveProfile() {
  if (!httpd.hasArg("plain")) { httpd.send(400, "text/plain", "no body"); return; }
  StaticJsonDocument<512> d;
  if (deserializeJson(d, httpd.arg("plain"))) { httpd.send(400, "text/plain", "bad json"); return; }
  const char *name = d["name"] | "";
  if (!name[0]) { httpd.send(400, "text/plain", "name required"); return; }

  FilamentProfile *slot = findProfileByName(name);
  if (!slot) for (auto &p : g_profiles) if (!p.in_use) { slot = &p; break; }
  if (!slot)  { httpd.send(507, "text/plain", "no profile slots free"); return; }

  memset(slot, 0, sizeof(*slot));
  strncpy(slot->name,     name,                                sizeof(slot->name)-1);
  strncpy(slot->material, d["material"] | "PLA",               sizeof(slot->material)-1);
  strncpy(slot->code,     d["code"]     | "GFL99",             sizeof(slot->code)-1);
  strncpy(slot->color,    d["color"]    | "808080",            sizeof(slot->color)-1);
  strncpy(slot->brand,    d["brand"]    | "Generic",           sizeof(slot->brand)-1);
  slot->nozzle_min = d["nozzle_min"] | 200;
  slot->nozzle_max = d["nozzle_max"] | 230;
  slot->bed_temp   = d["bed"]        | 60;
  slot->flow_x1000 = d["flow_x1000"] | 1000;
  slot->in_use     = true;
  saveProfiles();
  httpd.send(200, "text/plain", "ok");
}

void apiDeleteProfile() {
  if (!httpd.hasArg("name")) { httpd.send(400, "text/plain", "no name"); return; }
  FilamentProfile *p = findProfileByName(httpd.arg("name").c_str());
  if (!p) { httpd.send(404, "text/plain", "not found"); return; }
  p->in_use = false;
  saveProfiles();
  httpd.send(200, "text/plain", "ok");
}

void apiQueueWrite() {
  if (!httpd.hasArg("plain")) { httpd.send(400, "text/plain", "no body"); return; }
  StaticJsonDocument<256> d;
  if (deserializeJson(d, httpd.arg("plain"))) { httpd.send(400, "text/plain", "bad json"); return; }
  const char *name = d["name"] | "";
  FilamentProfile *p = findProfileByName(name);
  if (!p) { httpd.send(404, "text/plain", "profile not found"); return; }
  g_pendingWrite.profile = *p;
  g_pendingWrite.pending = true;
  g_pendingWrite.expires = millis() + 60000;     // 60s window to present a tag
  httpd.send(200, "text/plain", "queued; present tag within 60s");
}

void apiCancelWrite() {
  g_pendingWrite.pending = false;
  httpd.send(200, "text/plain", "cancelled");
}

void apiForceAssign() {
  if (!httpd.hasArg("plain")) { httpd.send(400, "text/plain", "no body"); return; }
  StaticJsonDocument<256> d;
  if (deserializeJson(d, httpd.arg("plain"))) { httpd.send(400, "text/plain", "bad json"); return; }
  const char *name = d["name"] | "";
  FilamentProfile *p = findProfileByName(name);
  if (!p) { httpd.send(404, "text/plain", "profile not found"); return; }
  sendAmsAssign(*p);
  httpd.send(200, "text/plain", "sent");
}

void apiGetConfig() {
  StaticJsonDocument<512> d;
  d["wifi_ssid"]      = g_cfg.wifi_ssid;
  d["printer_ip"]     = g_cfg.printer_ip;
  d["printer_serial"] = g_cfg.printer_serial;
  d["lan_code_set"]   = strlen(g_cfg.lan_code) > 0;
  d["auto_assign"]    = g_cfg.auto_assign;
  d["auto_rewrite"]   = g_cfg.auto_rewrite;
  String out; serializeJson(d, out);
  httpd.send(200, "application/json", out);
}

void apiSaveConfig() {
  if (!httpd.hasArg("plain")) { httpd.send(400, "text/plain", "no body"); return; }
  StaticJsonDocument<512> d;
  if (deserializeJson(d, httpd.arg("plain"))) { httpd.send(400, "text/plain", "bad json"); return; }
  if (d.containsKey("wifi_ssid"))      strncpy(g_cfg.wifi_ssid,      d["wifi_ssid"],      sizeof(g_cfg.wifi_ssid)-1);
  if (d.containsKey("wifi_pass"))      strncpy(g_cfg.wifi_pass,      d["wifi_pass"],      sizeof(g_cfg.wifi_pass)-1);
  if (d.containsKey("printer_ip"))     strncpy(g_cfg.printer_ip,     d["printer_ip"],     sizeof(g_cfg.printer_ip)-1);
  if (d.containsKey("printer_serial")) strncpy(g_cfg.printer_serial, d["printer_serial"], sizeof(g_cfg.printer_serial)-1);
  if (d.containsKey("lan_code"))       strncpy(g_cfg.lan_code,       d["lan_code"],       sizeof(g_cfg.lan_code)-1);
  if (d.containsKey("auto_assign"))    g_cfg.auto_assign  = d["auto_assign"];
  if (d.containsKey("auto_rewrite"))   g_cfg.auto_rewrite = d["auto_rewrite"];
  saveConfig();
  httpd.send(200, "text/plain", "saved; reboot to apply network changes");
}

void apiReboot() {
  httpd.send(200, "text/plain", "rebooting");
  delay(200);
  ESP.restart();
}

void apiToggleAuto() {
  g_cfg.auto_assign = !g_cfg.auto_assign;
  saveConfig();
  httpd.send(200, "text/plain", g_cfg.auto_assign ? "on" : "off");
}

/* v1.2: toggle the auto-rewrite-on-touchscreen-change feature */
void apiToggleRewrite() {
  g_cfg.auto_rewrite = !g_cfg.auto_rewrite;
  saveConfig();
  /* Clearing the queue when the feature is turned off avoids surprising
   * the user later with a stale rewrite they don't expect. */
  if (!g_cfg.auto_rewrite) g_pendingRewrite.pending = false;
  httpd.send(200, "text/plain", g_cfg.auto_rewrite ? "on" : "off");
}

/* v1.2: cancel a queued tag rewrite (web UI button) */
void apiCancelRewrite() {
  g_pendingRewrite.pending = false;
  httpd.send(200, "text/plain", "cancelled");
}

/* =========================================================================
 *  OTA FIRMWARE UPDATE  (v1.3)
 *
 *  The Chrome extension (Print Wizard) downloads firmware.bin from GitHub
 *  and POSTs it here as multipart/form-data with field name "firmware".
 *  The ESP32 Update library writes it to the OTA partition and we reboot.
 *
 *  Endpoint: POST /api/update
 *  Endpoint: GET  /api/update/status   (poll during / after upload)
 * ========================================================================= */

void handleOtaUpload() {
  HTTPUpload &up = httpd.upload();

  if (up.status == UPLOAD_FILE_START) {
    /* Reset state */
    memset(&g_ota, 0, sizeof(g_ota));
    g_ota.active = true;
    Serial.printf("[OTA] upload start — file: %s\n", up.filename.c_str());

    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
      g_ota.error = true;
      String e; Update.printError(e);
      strncpy(g_ota.error_msg, e.c_str(), sizeof(g_ota.error_msg)-1);
      Serial.printf("[OTA] begin error: %s\n", g_ota.error_msg);
    }

  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (!g_ota.error) {
      size_t written = Update.write(up.buf, up.currentSize);
      if (written != up.currentSize) {
        g_ota.error = true;
        snprintf(g_ota.error_msg, sizeof(g_ota.error_msg),
                 "write mismatch %u/%u", written, up.currentSize);
        Serial.printf("[OTA] %s\n", g_ota.error_msg);
      } else {
        g_ota.bytes_written += written;
        g_ota.total_size     = up.totalSize;
        /* Progress every 64 KB */
        if (g_ota.bytes_written % 65536 < (size_t)up.currentSize)
          Serial.printf("[OTA] %u bytes written\n", g_ota.bytes_written);
      }
    }

  } else if (up.status == UPLOAD_FILE_END) {
    g_ota.active = false;
    if (!g_ota.error) {
      if (Update.end(true)) {
        Serial.printf("[OTA] upload OK — %u bytes total\n", g_ota.bytes_written);
      } else {
        g_ota.error = true;
        String e; Update.printError(e);
        strncpy(g_ota.error_msg, e.c_str(), sizeof(g_ota.error_msg)-1);
        Serial.printf("[OTA] end error: %s\n", g_ota.error_msg);
      }
    }
  }
}

void handleOtaComplete() {
  sendCors();
  if (g_ota.error || Update.hasError()) {
    StaticJsonDocument<128> d;
    d["ok"]    = false;
    d["error"] = g_ota.error_msg;
    String out; serializeJson(d, out);
    httpd.send(500, "application/json", out);
    Serial.printf("[OTA] FAILED: %s\n", g_ota.error_msg);
    return;
  }

  /* Success — acknowledge before rebooting so the extension knows we're done */
  StaticJsonDocument<64> d;
  d["ok"]  = true;
  d["msg"] = "rebooting";
  String out; serializeJson(d, out);
  httpd.send(200, "application/json", out);
  Serial.println(F("[OTA] success — rebooting in 1s"));
  delay(1000);
  ESP.restart();
}

/* Polled by the extension to track upload progress (optional convenience).
 * Also used after reboot to confirm the new version is running. */
void apiOtaStatus() {
  sendCors();
  StaticJsonDocument<256> d;
  d["active"]        = g_ota.active;
  d["bytes_written"] = g_ota.bytes_written;
  d["total_size"]    = g_ota.total_size;
  d["error"]         = g_ota.error;
  d["error_msg"]     = g_ota.error_msg;
  d["fw"]            = FW_VERSION;   /* included so extension can confirm new version after reboot */
  String out; serializeJson(d, out);
  httpd.send(200, "application/json", out);
}


void sendCors() {
  httpd.sendHeader("Access-Control-Allow-Origin",  "*");
  httpd.sendHeader("Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS");
  httpd.sendHeader("Access-Control-Allow-Headers", "Content-Type");
  httpd.sendHeader("Access-Control-Max-Age",       "600");
}

void apiHealth() {
  sendCors();
  StaticJsonDocument<256> d;
  d["ok"]   = true;
  d["fw"]   = FW_VERSION;
  d["name"] = "SmartSpool OS Lite";
  String out; serializeJson(d, out);
  httpd.send(200, "application/json", out);
}

void initWebServer() {
  httpd.on("/",                HTTP_GET,  [](){ httpd.send_P(200, "text/html", PAGE_INDEX); });
  httpd.on("/style.css",       HTTP_GET,  [](){ httpd.send_P(200, "text/css",  PAGE_CSS); });
  httpd.on("/app.js",          HTTP_GET,  [](){ httpd.send_P(200, "application/javascript", PAGE_JS); });

  httpd.on("/api/health",      HTTP_GET,  apiHealth);
  httpd.on("/api/status",      HTTP_GET,  [](){ sendCors(); apiStatus(); });
  httpd.on("/api/profiles",    HTTP_GET,  [](){ sendCors(); apiProfiles(); });
  httpd.on("/api/profiles",    HTTP_POST, [](){ sendCors(); apiSaveProfile(); });
  httpd.on("/api/profiles",    HTTP_DELETE, [](){ sendCors(); apiDeleteProfile(); });
  httpd.on("/api/write",       HTTP_POST, [](){ sendCors(); apiQueueWrite(); });
  httpd.on("/api/write/cancel",HTTP_POST, [](){ sendCors(); apiCancelWrite(); });
  httpd.on("/api/assign",      HTTP_POST, [](){ sendCors(); apiForceAssign(); });
  httpd.on("/api/config",      HTTP_GET,  [](){ sendCors(); apiGetConfig(); });
  httpd.on("/api/config",      HTTP_POST, [](){ sendCors(); apiSaveConfig(); });
  httpd.on("/api/reboot",      HTTP_POST, [](){ sendCors(); apiReboot(); });
  httpd.on("/api/auto",        HTTP_POST, [](){ sendCors(); apiToggleAuto(); });
  httpd.on("/api/rewrite",     HTTP_POST, [](){ sendCors(); apiToggleRewrite(); });
  httpd.on("/api/rewrite/cancel", HTTP_POST, [](){ sendCors(); apiCancelRewrite(); });

  /* v1.3 OTA — the upload handler is the second callback argument */
  httpd.on("/api/update", HTTP_POST, handleOtaComplete, handleOtaUpload);
  httpd.on("/api/update/status", HTTP_GET, apiOtaStatus);

  /* CORS preflight: respond 204 to any OPTIONS request on /api/* */
  httpd.onNotFound([](){
    if (httpd.method() == HTTP_OPTIONS) {
      sendCors();
      httpd.send(204);
      return;
    }
    httpd.send(404, "text/plain", "not found");
  });
  httpd.begin();
  Serial.println(F("[WEB] HTTP server on :80"));
}

/* =========================================================================
 *  HELPERS
 * ========================================================================= */
String uidToHex(const uint8_t *uid, uint8_t len) {
  char buf[32]; size_t off = 0;
  for (uint8_t i = 0; i < len && off < sizeof(buf)-3; i++) {
    off += snprintf(buf+off, sizeof(buf)-off, "%02X", uid[i]);
  }
  return String(buf);
}
void setLED(bool on)              { digitalWrite(PIN_LED, on ? HIGH : LOW); }
void blinkLED(uint8_t n, uint16_t ms) {
  for (uint8_t i = 0; i < n; i++) {
    setLED(true); delay(ms); setLED(false); delay(ms);
  }
}
