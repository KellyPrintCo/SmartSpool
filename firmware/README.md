# SmartSpool OS Lite (SSO-Lite) — Complete Build Guide

ESP32 + MFRC522 → Bambu Lab AMS Slot 1 auto-assignment over MQTT/TLS.

This document is the full design, build, troubleshooting, and productization guide.
The firmware itself lives in `SSOLite.ino` and `web_pages.h` — open them in Arduino IDE
and flash. No other configuration is needed at compile time; everything is set through
the on-device web UI.

---

## What's New in v1.3.0

**Over-the-air firmware updates** *(new feature)*

SmartSpool can now update its own firmware over WiFi — no USB cable, no
Arduino IDE, no manual flashing. The update prompt appears automatically when
Print Wizard connects to a device running an older version, or in the device's
own web UI under Controls → Firmware Update.

How it works:
1. SmartSpool checks `version.json` on GitHub for the latest published firmware
2. If an update is available, the web UI / Print Wizard shows an update prompt
3. The new firmware binary downloads from GitHub Releases
4. The binary uploads to the SmartSpool device over LAN (~5 seconds)
5. SmartSpool's `Update` library writes it to the OTA partition
6. Device reboots and comes back on the new firmware (~10-15 seconds)
7. Web UI / Print Wizard auto-reconnects and verifies the new version

The whole process takes 25-40 seconds end to end on a typical home network.

Configuration:
- Firmware author edits `version.json` in the GitHub repo to publish updates
- New endpoint `POST /api/update` accepts firmware binaries
- New endpoint `GET /api/update/status` reports upload progress
- See `OTA_RELEASE_GUIDE.md` for the full release publishing process

**Important chicken-and-egg note:** Devices running firmware older than v1.3.0
do not have the OTA endpoint and must be flashed to v1.3.0 via USB once.
After that, all future updates can be OTA. Ship pre-flashed units with v1.3.0+
to avoid customers ever encountering this.

---

## What's New in v1.2.0

**Auto-rewrite on touchscreen change** *(new, off by default)*

When enabled, if you change slot 1 filament info on the printer's touchscreen
(color, brand, material, temps), SmartSpool catches the change via MQTT and
queues a rewrite of the RFID tag. The next time you tap that tag on the reader,
the new info is written to the tag automatically — no need to use the SmartSpool
web UI to manually update each tag.

How it works:
1. You scan tag A → SmartSpool reads it and applies its profile to slot 1.
2. You change filament info on the touchscreen (different color, brand, etc.).
3. The printer publishes the new state over MQTT.
4. SmartSpool detects this is *different* from what it just set, and queues the
   new info as a "pending rewrite" tied to tag A's UID.
5. Next time tag A is presented to the reader, SmartSpool writes the new info
   to the tag before doing the normal scan flow. Tag now persists the change.

Off by default because some users intentionally do one-off touchscreen overrides
they don't want remembered. Enable it via:
- Web UI → Setup tab → "Auto-rewrite tag on touchscreen change"
- Web UI → Controls → "Toggle Auto-Rewrite" button
- API: `POST /api/rewrite`

A pending rewrite is shown on the dashboard with a yellow card; you can cancel
it before scanning.

**Side-slit mounting** *(mechanical change)*

The recommended mount location is now the side slits on the *outside* of the
AMS 2 Pro, not the desiccant slot. This avoids any conflict with the AMS lid
opening, the drying chamber, or internal cable routing. The reader still has
range to read tags on a spool loaded into slot 1.

---

## 1. System Architecture

```
 ┌──────────────┐     SPI      ┌──────────────────┐    Wi-Fi/TLS   ┌─────────────┐
 │  RFID Tag    │◀────────────▶│      ESP32       │───────────────▶│   Bambu     │
 │ (MIFARE 1K)  │   MFRC522    │  (SSO-Lite FW)   │   MQTT 8883    │   Printer   │
 └──────────────┘              │                  │   bblp / LAN   │  (X1/P1/A1) │
                               │  • Web UI :80    │                │             │
                               │  • Profile DB    │                │  AMS Slot 1 │
                               │  • UID fallback  │◀───────────────│  (tray 0,0) │
                               └──────────────────┘   /report      └─────────────┘
```

### Data flow
1. Tag enters RF field → MFRC522 returns UID and authenticates sector 1 with default key A.
2. ESP32 reads blocks 4–6 (48 bytes), validates magic header `SSO1`, parses compact key=value payload.
3. The parsed Bambu code (e.g. `GFL99`) is resolved against the on-device profile database to canonicalise temps and color.
4. ESP32 publishes `ams_filament_setting` to `device/{serial}/request` over MQTT/TLS.
5. Printer applies the change; the next `/report` push confirms the new tray state.

### Tag lifecycle
```
   blank tag ──► [Web UI: Queue Write] ──► written tag ──► scanned ──► assigned ──► reused
                                                  │                            ▲
                                                  └───── Edit ─────────────────┘
```

### State machine
| State | Trigger | Exit |
|---|---|---|
| BOOT | power-on | config loaded → WIFI_CONNECTING |
| WIFI_CONNECTING | startup / disconnect | got IP → MQTT_CONNECTING |
| MQTT_CONNECTING | TCP up | broker ack → IDLE |
| IDLE | nothing happening | tag presented → READING; UI write queued + tag → WRITING |
| READING | card detected | parse OK → ASSIGNING; parse fail → UID fallback or ERROR |
| WRITING | pending write fulfils | success → IDLE; failure → ERROR |
| ASSIGNING | auto-assign on | publish OK → IDLE; publish fail → retry queue |
| ERROR | any failure | LED blink, log, return to IDLE |

### Failure handling
- **Wi-Fi drop**: 10 s reconnect loop; web UI stays up on STA IP, AP fallback if SSID unconfigured.
- **MQTT drop**: 5 s reconnect; pending assign request is held in `g_pending` and re-sent on reconnect with exponential backoff (5/10/20/40/80 s).
- **Tag corruption**: checksum mismatch logs a warning but the parser is forgiving — partial fields recover. If the magic header itself is gone, UID-fallback table maps `(UID → code)`.
- **AMS override / drift**: `processReport()` watches the printer's reported tray 0 code; if it differs from the last-scanned profile, SSO-Lite re-applies once per 30 s.
- **Bambu official RFID detected**: non-Classic tag types (NTAG/Ultralight) trigger `g_officialTagDetected = true`, auto-assign is suppressed, and the dashboard pill turns yellow.

---

## 2. Hardware Design

### Parts list
| Item | Qty | Notes |
|---|---|---|
| ESP32 DevKit V1 / WROOM-32 / NodeMCU-32S | 1 | Any 30/38-pin board with native USB |
| MFRC522 RFID module | 1 | Common red breakout, 3.3 V logic |
| MIFARE Classic 1K white card / sticker | n | The "blue" 13.56 MHz cards from any vendor |
| Dupont jumpers (F-F) | 7 | One per signal + VCC/GND |
| USB-C / micro-USB cable | 1 | Power + flashing |
| 3D-printed mount (Section 13) | 1 | optional but recommended |

### Wiring (ESP32 ↔ MFRC522)

| MFRC522 | ESP32 GPIO | Function | Notes |
|---|---|---|---|
| SDA / SS | **GPIO 5** | SPI chip-select | safe pin |
| SCK | **GPIO 18** | SPI clock | VSPI default |
| MOSI | **GPIO 23** | SPI MOSI | VSPI default |
| MISO | **GPIO 19** | SPI MISO | VSPI default |
| RST | **GPIO 22** | reset | safe pin |
| GND | **GND** | ground | |
| 3V3 | **3V3** | **3.3 V ONLY** | see warning below |
| IRQ | (not connected) | — | unused; we poll |

> ⚠️ **3.3 V ONLY.** Many MFRC522 boards say "3.3 V or 5 V" on the silkscreen.
> The MFRC522 IC itself is **only** 3.3 V. Powering from 5 V will work briefly and then
> kill the chip. Use the ESP32's 3V3 pin (which can sink ~500 mA — plenty for this module).

### Pins to AVOID on ESP32

| Pin | Why |
|---|---|
| GPIO 0 | Boot strap — must be HIGH at reset to boot from flash |
| GPIO 2 | Boot strap & on-board LED on most boards |
| GPIO 12 | Boot strap — wrong level locks board into 1.8 V flash mode |
| GPIO 15 | Boot strap — pulled up enables verbose serial spam |
| GPIO 6–11 | Connected to internal SPI flash — touching these bricks the boot |
| GPIO 34–39 | Input-only, no internal pull-ups |

The pins chosen (5, 18, 19, 22, 23) are all general-purpose and free of strap conflicts.

### Physical placement near AMS Slot 1

For an AMS or AMS Lite, the ideal mount point is on the **outer face of the slot 1
filament guide**, with the MFRC522 antenna oriented so that a tag stuck to the side of
the spool (or a separate tag on a hook) passes within ~25 mm of the coil during loading.
Tag read range with a Classic 1K card and a default-tuned MFRC522 is ~30–40 mm.

A 3D-printable bracket is described in Section 13.

---

## 3. RFID Tag System

### A. Storage strategy

MIFARE Classic 1K = 16 sectors × 4 blocks × 16 bytes = 1024 bytes total.

```
Sector 0  Block 0   manufacturer block (UID + data) — READ ONLY
Sector 0  Block 1   usable, but we avoid (some clones break here)
Sector 0  Block 2   usable, but avoided
Sector 0  Block 3   sector trailer (keys + ACL) — DO NOT WRITE
Sector 1  Block 4   ◄── SSO data block A
Sector 1  Block 5   ◄── SSO data block B
Sector 1  Block 6   ◄── SSO data block C
Sector 1  Block 7   sector trailer — DO NOT WRITE
Sectors 2-15        free for future extensions
```

We use **only sector 1**, blocks 4–6 (48 bytes total). This is enough for the compact
encoded format and leaves the rest of the card available for future features (usage
counters, cloning detection, etc.).

### B. Data formats

#### Compact (on-tag, what SSO-Lite reads/writes)
```
M=PLA;C=GFL99;X=000000;B=Overture;N=210;D=60
```
Fields:
- `M` — material short name (PLA / PETG / ABS / TPU / PA / PC / PVA)
- `C` — Bambu generic code (`tray_info_idx`, e.g. `GFL99`)
- `X` — color RGB hex (no `#`, no alpha)
- `B` — brand name (≤ 19 chars)
- `N` — nominal nozzle temp (printer recomputes range)
- `D` — bed temp

The parser is order-independent and tolerant of missing optional fields.

#### Full JSON (used in HTTP API + persistent storage)
```json
{
  "name": "Overture PLA Matte",
  "material": "PLA",
  "code": "GFL99",
  "color": "1C1C1C",
  "brand": "Overture",
  "nozzle_min": 200,
  "nozzle_max": 220,
  "bed": 60,
  "flow_x1000": 1000
}
```

#### Tag block layout (48 bytes)
| Bytes | Content |
|---|---|
| 0–3 | magic `S S O 1` (`0x53 0x53 0x4F 0x31`) |
| 4 | version (`0x01`) |
| 5 | payload length N (≤ 41) |
| 6 .. 5+N | ASCII payload |
| 5+N .. 46 | zero padding |
| 47 | XOR checksum of bytes 0..46 |

### C. Tag security
- **Keys:** SSO-Lite uses the default factory key A (`FF FF FF FF FF FF`) on sector 1.
  Keeping the default keeps tags re-flashable from any standard tool and avoids the
  *brick-on-typo* failure mode. If you want to lock tags, you can change Key A in the
  sector trailer — but **never touch block 7 unless you understand MIFARE access bits**;
  one bad write permanently locks the sector.
- **Overwrite protection:** The web UI requires an explicit "Queue Write" press; the
  reader will only write within a 60 s window per request. A tag presented outside that
  window is read-only.
- **UID fallback:** every successful write also stores `(UID → tray_info_idx)` in the
  device's UID map. If the data blocks ever go bad, SSO-Lite still recognises the spool
  by UID and applies the right profile.

---

## 4. Filament Profile System

Profiles live in NVS (`Preferences`) and are loaded into `g_profiles[]` at boot.
On first boot, the firmware seeds a starter set:

| Name | Material | Code | Color | Brand | Nozzle | Bed |
|---|---|---|---|---|---|---|
| Generic PLA Black  | PLA  | GFL99 | #000000 | Generic  | 190–230 | 60 |
| Generic PLA White  | PLA  | GFL99 | #FFFFFF | Generic  | 190–230 | 60 |
| Generic PLA Red    | PLA  | GFL99 | #C8161D | Generic  | 190–230 | 60 |
| Generic PLA Blue   | PLA  | GFL99 | #0066CC | Generic  | 190–230 | 60 |
| Overture PLA Matte | PLA  | GFL99 | #1C1C1C | Overture | 200–220 | 60 |
| Generic PETG       | PETG | GFG99 | #0099A8 | Generic  | 230–255 | 75 |
| Generic PETG Black | PETG | GFG99 | #000000 | Generic  | 230–255 | 75 |
| Generic ABS        | ABS  | GFB99 | #F2F2F2 | Generic  | 240–270 | 95 |
| Generic TPU 95A    | TPU  | GFU99 | #FFFFFF | Generic  | 220–240 | 50 |
| Generic PA (Nylon) | PA   | GFN99 | #303030 | Generic  | 260–290 | 80 |
| Generic PC         | PC   | GFC99 | #F0F0F0 | Generic  | 270–290 | 100 |
| Generic PVA Sup.   | PVA  | GFS99 | #F4E2B8 | Generic  | 200–210 | 60 |

**Bambu generic codes** are 5-character `tray_info_idx` values that tell the printer to
treat the slot as the corresponding generic profile (slicer-side). The `GF*99` codes
above are the widely-documented community-known values; if your printer firmware uses
different ones, edit the seed list in `seedDefaultProfiles()` or change the code in the
profile from the web UI.

**Lookup logic:**
1. Tag scanned → compact payload parsed → `code` extracted.
2. `findProfileByCode(code)` returns the canonical local profile (full temps, brand, name).
3. If no local match exists, the tag's own values are used directly.

Profiles are user-editable from the **Profiles** tab. Up to 32 profiles fit; deleted
slots are reused automatically.

---

## 5. Tag Writing System

### A. Write mode
Used for blank tags and for overwriting existing SSO tags. Implementation in
`writeTagData()`:

1. Encode profile → compact ASCII payload.
2. Build 48-byte blob: magic + version + length + payload + zero pad + checksum.
3. For each of blocks 4, 5, 6:
   - Authenticate with default Key A.
   - `MIFARE_Write(block, data, 16)`.
4. Update UID-map entry `(UID → code)` and persist.

### B. Update mode
Field-level edits are done by **reading first, merging, then writing the whole 48-byte
blob**. This is safer than partial-block writes (MIFARE writes are always 16 bytes) and
keeps the checksum valid.

```
read tag → decode profile → patch fields from web form → encode → write tag
```

### C. UI integration
1. User opens **Tag Manager** tab.
2. Picks a profile from the dropdown (or hits a Quick-Write preset).
3. Presses **Queue Write**.
4. ESP32 enters write-armed state for 60 s.
5. User taps the tag on the reader.
6. LED double-flashes on success, 5-flashes on failure.

---

## 6. MQTT Implementation (Bambu LAN mode)

### Connection parameters
| Setting | Value |
|---|---|
| Host | printer's LAN IP |
| Port | `8883` |
| Transport | TLS (self-signed cert; we use `setInsecure()`) |
| Username | `bblp` |
| Password | LAN access code (printer screen → Settings → WLAN → Access Code) |
| Client ID | `SSOLite-{efuseMacHex}` (must be unique per printer connection) |
| Keepalive | 30 s |

### Topics
| Direction | Topic |
|---|---|
| publish | `device/{serial}/request` |
| subscribe | `device/{serial}/report` |

### Assignment command
```json
{
  "print": {
    "sequence_id": "12345",
    "command": "ams_filament_setting",
    "ams_id": 0,
    "tray_id": 0,
    "tray_info_idx": "GFL99",
    "tray_color": "000000FF",
    "nozzle_temp_min": 190,
    "nozzle_temp_max": 230,
    "tray_type": "PLA"
  }
}
```

`tray_color` is **RGBA** (8 hex chars). SSO-Lite always appends `FF` for full alpha.

### Confirming the assignment
The printer pushes the new tray state in the next `/report` message. SSO-Lite filters
the JSON to just `print.ams.ams[0].tray[0]` and updates `g_currentSlot1`. The web UI
shows this as the live "AMS Slot 1" swatch.

If the reported `tray_info_idx` ever drifts away from the last scanned profile (e.g. the
user reset the AMS or the slicer overrode it), SSO-Lite reapplies the assignment once,
rate-limited to one reapply per 30 s.

### Timing constraints
- Allow up to ~2 s after publish for the printer to apply the change.
- Don't burst commands faster than one per second; the printer will accept them but the
  AMS UI updates rate-limit.
- During an active print, AMS slot edits are still accepted but won't change the
  currently-loaded filament until the next `M620` (filament change).

### Retry logic
```
publish → ok? ┬─ yes: clear pending
              └─ no:  attempts++, next_attempt = now + 5s × 2^min(attempts,4)
                      stored in g_pending, retried in main loop
```

---

## 7. ESP32 Firmware

The complete firmware is in `SSOLite.ino` + `web_pages.h`. Key modules:

| Module | Where | Responsibility |
|---|---|---|
| State + globals | top of `SSOLite.ino` | configuration, last scan, history, pending |
| Persistence | `loadConfig/saveConfig`, `loadProfiles/saveProfiles`, `loadUidMaps/saveUidMaps` | NVS via `Preferences` |
| Defaults | `seedDefaultProfiles()` | runs once on first boot |
| RFID | `initRFID`, `rfidPoll`, `handleTag`, `readTagData`, `writeTagData`, `authBlock`, `encodeProfile`, `decodeProfile` | MFRC522 driver wrapper, parser, encoder |
| Wi-Fi | `initWiFiSTA` | STA with AP fallback to `SSOLite-Setup` / `spool1234` |
| MQTT | `initMQTT`, `sendAmsAssign`, `mqttCallback`, `processReport` | TLS connect, publish, filtered subscribe |
| Web | `initWebServer` + `api*()` handlers | REST + static HTML/CSS/JS |
| Helpers | `uidToHex`, `xorChecksum`, `setLED`, `blinkLED` | utility |

All the firmware features required by the spec are implemented:
- ✅ MFRC522 read/write (3-block payload + checksum)
- ✅ Compact format parsing
- ✅ JSON parsing (HTTP + MQTT report filter)
- ✅ Profile mapping with code/name lookups
- ✅ Wi-Fi STA + AP fallback
- ✅ MQTT TLS to Bambu LAN
- ✅ Auto-assign with debounce + duplicate protection
- ✅ Retry queue with exponential backoff
- ✅ UID fallback mapping
- ✅ Embedded web UI on port 80
- ✅ Serial debug logging at 115200 baud

---

## 8. Web UI

Hosted on the ESP32 at `http://<device-ip>/`. All UI assets are PROGMEM strings in
`web_pages.h` — no external CDN, works fully offline.

### Tabs

**Dashboard**
- Live AMS Slot 1 swatch + code
- Last scanned tag (color, name, UID, source: tag / uid_map)
- Connection pills (Wi-Fi, MQTT, auto-assign, official-tag lock)
- Last 5 scans (color-coded history)

**Tag Manager**
- Select profile + "Queue Write"
- Quick-Write preset buttons (one per profile, color-bordered)
- Cancel queued write

**Profiles**
- List of all profiles with edit/delete buttons
- Add/edit form (name, brand, material, code, color picker, temps)

**Controls**
- Force assign (sends MQTT without scanning a tag)
- Toggle auto-assign
- Reboot device

**Setup**
- Wi-Fi SSID/password
- Printer IP / serial / LAN access code
- Auto-assign default

The UI polls `/api/status` every 1.5 s for live updates.

---

## 9. Auto-Assignment Logic

```
                       ┌────────────────────────────────────┐
                       │                                    │
   tag in field ──► debounce (3s on same UID) ──► read 3 blocks
                                                  │
                       (read fails)               │ (read ok)
                       │                          ▼
                       ▼                  decode compact payload
                 UID-map lookup            │
                       │                   │
                 (no map → drop)           ▼
                       │             resolve to local profile
                       │                   │
                       └───────► merge ◄───┘
                                          │
                                          ▼
                                 push to history
                                          │
                                  auto-assign on?
                                  │            │
                                 no            yes
                                  │            │
                                  └─► idle     ▼
                                          publish MQTT
                                          │       │
                                         ok       fail
                                          │       │
                                          ▼       ▼
                                    g_currentSlot1   pending+backoff
```

- **Debounce:** 3000 ms on identical UID prevents bouncing reads from firing 10 commands.
- **Duplicate protection:** if the same profile code is already loaded in slot 1 *and*
  was set by us, we still publish (the user might have clicked a quick-write preset for
  a reason); but the report watcher won't trigger drift correction in this case.
- **Reapply on AMS reset:** the report processor compares reported `tray_info_idx`
  against `g_lastScan.profile.code` and reapplies once per 30 s if they differ.

---

## 10. Edge Case Handling

| Edge case | Behavior |
|---|---|
| Tag completely unreadable | `readTagData()` returns false → `findUidMap()` lookup → if hit, use mapped profile; if miss, log + LED 4-blink |
| Tag partially corrupted | parser is forgiving: missing fields are filled with defaults, decoder still returns true if `code` and `material` present |
| Checksum mismatch | warning logged, parser still attempts decode |
| MQTT broker down | publish queued in `g_pending`, retried with exponential backoff (5/10/20/40/80 s cap) |
| Wi-Fi drops | 10 s reconnect loop; web UI remains available on AP fallback if SSID gone |
| AMS override (user changed slot manually) | `processReport()` sees drift → reapplies last scanned profile (max once / 30 s) |
| Bambu official RFID spool inserted | non-Classic tag type detected → `g_officialTagDetected=true`, auto-assign suppressed, dashboard shows yellow lock pill |
| Multiple AMS units present | hard-pinned to `ams_id=0`, `tray_id=0` per spec |
| Profile slots full | API returns 507; UI shows error; user must delete a profile |
| Write request without tag in 60 s | request expires, `g_pendingWrite.pending = false` |

---

## 11. Step-by-Step Setup Guide

(As if you were 13 and have never built one of these.)

### Things you need
- An ESP32 board (the rectangular dev-kit one with a USB port).
- An MFRC522 reader (the red one with a coil on the front).
- 7 jumper wires (female-to-female).
- 5 blank MIFARE 1K cards (the blue ones, 13.56 MHz — *not* the white 125 kHz ones).
- A computer with Arduino IDE installed.
- The Wi-Fi name & password.
- Your Bambu printer's IP address and LAN access code.

### Step 1 — Wire it up
With both modules **unpowered**, connect:

| MFRC522 pin | ESP32 pin |
|---|---|
| SDA | GPIO 5 |
| SCK | GPIO 18 |
| MOSI | GPIO 23 |
| MISO | GPIO 19 |
| RST | GPIO 22 |
| GND | GND |
| 3.3V | 3V3 |
| IRQ | (leave empty) |

Double-check 3.3V. **Never** wire it to 5V; the chip will die.

### Step 2 — Install Arduino IDE & libraries
1. Download Arduino IDE 2.x from arduino.cc.
2. **File → Preferences → Additional Board Manager URLs**, paste:
   `https://espressif.github.io/arduino-esp32/package_esp32_index.json`
3. **Tools → Board → Boards Manager**, search "esp32", install Espressif's package.
4. **Tools → Manage Libraries**, install:
   - `MFRC522` by GithubCommunity / Miguel Balboa (≥ 1.4.10)
   - `PubSubClient` by Nick O'Leary (≥ 2.8)
   - `ArduinoJson` by Benoit Blanchon — pick **version 6.21.x** (NOT v7).

### Step 3 — Flash the firmware
1. Open `SSOLite.ino` in Arduino IDE — `web_pages.h` opens automatically as a tab.
2. **Tools → Board** → "ESP32 Dev Module".
3. **Tools → Port** → the COM port that appeared when you plugged in the ESP32.
4. **Tools → Partition Scheme** → "Default 4MB with spiffs" is fine.
5. Click the **Upload** arrow. If the ESP32 won't enter download mode, hold the BOOT
   button while it says "Connecting…".
6. Open **Serial Monitor** at **115200 baud**. You should see:
   ```
   ===== SmartSpool OS Lite v1.0.0 =====
   [PROFILES] seeded 12 defaults
   [RFID] MFRC522 firmware version: 0x92
   [WiFi] No SSID configured. Starting AP 'SSOLite-Setup'...
   [WiFi] AP IP: 192.168.4.1
   [WEB] HTTP server on :80
   [BOOT] Ready.
   ```

### Step 4 — Connect to the device's setup AP
On your phone or laptop, connect to Wi-Fi:
- SSID: `SSOLite-Setup`
- Password: `spool1234`

Open `http://192.168.4.1` — the SSO-Lite web UI loads.

### Step 5 — Get your printer's info
On the Bambu printer screen:
- Settings → WLAN — note the **IP address**.
- Settings → WLAN → "Access Code" (LAN-only mode must be enabled).
- Settings → Device → **serial number** (starts with `01`, `02`, `03`, or similar).

### Step 6 — Configure SSO-Lite
In the web UI → **Setup** tab:
1. Wi-Fi SSID + password (your home Wi-Fi, the one the printer is on).
2. Printer IP, serial, LAN access code.
3. ✅ Auto-assign on scan.
4. Save.
5. The device reboots and joins your home Wi-Fi. Find its new IP from your router or
   serial monitor, and reload the UI from there.

When MQTT connects, the dashboard's MQTT pill turns green and the AMS Slot 1 swatch
populates.

### Step 7 — Write your first tag
1. Tag Manager tab → pick "Generic PLA Black".
2. Click **Queue Write**.
3. Tap a blank MIFARE 1K card on the reader within 60 s.
4. Watch the serial monitor — you should see:
   ```
   [RFID] tag UID=A1B2C3D4
   [WRITE] writing pending profile to tag...
   [WRITE] result: OK
   ```

### Step 8 — Test auto-assign
Tap the same tag on the reader without queueing a write. You should see:
```
[RFID] tag UID=A1B2C3D4
[RFID] payload: M=PLA;C=GFL99;X=000000;B=Generic;N=210;D=60
[ASSIGN] publish device/01P00.../request: OK
```
Look at the printer — AMS Slot 1's filament should switch to "Generic PLA" with black color.

You're done. 🎉

---

## 12. Troubleshooting

### MFRC522 not reading anything (`firmware version: 0x00` or `0xFF`)
- Wiring: check SDA/SS goes to GPIO 5 specifically (the firmware hard-codes it).
- Power: the module **must** be on 3.3 V. If you ran it on 5 V, the chip is probably dead.
- Cold solder joints on the breakout's pin header — reflow them.
- Long Dupont wires can drop SPI clock — keep wires under 15 cm and twist GND with each signal if you must run them longer.

### Boot loop / "Brownout detector triggered"
- USB cable is power-only or current-limited. Try another cable, ideally USB-C.
- Drawing too much from a weak USB port — plug into a 1 A+ supply.

### "Failed to connect to ESP32" at flash time
- Hold the **BOOT** button while clicking Upload, release when "Writing…" starts.
- Try a different USB port. Some hubs don't pass the auto-reset signal.

### Wi-Fi connects but MQTT fails (rc=-2 or rc=5)
- rc=-2 → cannot reach host: wrong IP, or Wi-Fi VLAN isolation is blocking.
- rc=5 → bad credentials: username MUST be `bblp` (lowercase), password is the LAN access code (not the cloud password).
- Make sure **LAN-only mode** or **LAN access code** is enabled on the printer screen. Without it, port 8883 is closed.
- Some firewalls block self-signed TLS — verify the printer is reachable: `openssl s_client -connect <ip>:8883`.

### TLS handshake fails
- The firmware uses `tlsClient.setInsecure()` which accepts the self-signed cert. If you removed that line for security, you need to embed the printer cert; see ESP32 `WiFiClientSecure::setCACert()`.

### AMS not updating
- Verify the assign command was published: serial log shows `[ASSIGN] publish ... OK`.
- Verify `ams_id=0, tray_id=0` corresponds to physical slot 1 (it does on AMS / AMS Lite).
- The slicer can override AMS settings at print start. After slicing, recheck slot 1.
- Some printer firmware versions reject the command silently if the AMS is mid-load. Wait for AMS idle and retry from the **Force Assign** button.

### Wrong filament code applied
- The Bambu generic codes (`GFL99` etc.) sometimes change between firmware versions. If your printer doesn't recognise the code, edit the profile via the Profiles tab and try alternates (community-known codes include `GFL99`, `GFL00`, `GFL98` for Generic PLA depending on firmware).
- Verify by clicking the "Force Assign" button while watching the printer screen — the change is instant if accepted.

### LED behaviour
| Pattern | Meaning |
|---|---|
| 3 short blinks at boot | system ready |
| solid on briefly | tag in field, processing |
| 2 blinks | success (read or write) |
| 4 blinks | tag unreadable + no UID map |
| 5 blinks | write failed |

### "Boot pin conflicts" / serial spam
Don't add buttons or external pull-ups on GPIO 0/2/12/15. If you previously wired RST to GPIO 0 (some ancient tutorials do), move it to GPIO 22 as in this build.

---

## 13. Productization Plan

A path from "working hack" to "kit you can ship".

### Mechanical (MakerWorld / Printables release)
- **Printer mount**: clip-on bracket for AMS or AMS Lite slot 1 outer face. ESP32 sits on
  the back, MFRC522 antenna faces outward. Print in PETG (avoid PLA — heat sensitive).
- **Tag holder**: 38 mm disc that snaps onto a spool's outer rim with a recess for a 25 mm
  MIFARE sticker. Two designs: rim-clip and adhesive.
- **Enclosure**: two-part snap-fit, 65 × 45 × 20 mm, with USB-C cutout, MFRC522 antenna
  window, and an LED light pipe.

### Kit BOM (target retail $35)
| Item | Cost |
|---|---|
| ESP32 WROOM-32 module | ~$4 |
| MFRC522 breakout | ~$2 |
| 10 × MIFARE 1K stickers | ~$3 |
| Pre-soldered ribbon harness | ~$1 |
| Printed enclosure + mount | ~$2 |
| USB-C cable | ~$2 |
| Box + insert + manual | ~$3 |
| **COGS** | **~$17** |

### UX improvements for a shipped product
- **Setup QR system**: device boots with a unique AP password printed on a sticker;
  scanning a QR autofills SSID/password and links to `http://192.168.4.1`.
- **OTA updates**: replace the manual flash step with `ArduinoOTA` once the device joins
  Wi-Fi.
- **Pre-flashed tags**: ship 5 stickers already programmed for "PLA Black/White" + 1 PETG
  + 1 ABS + 2 blank.
- **Mobile-first UI**: the current UI is responsive but could be wrapped in a Capacitor
  shell for a friendlier "app".
- **mDNS hostname**: announce as `ssolite.local` so users don't need to find the IP.

### Compliance considerations
- FCC Part 15 unintentional-radiator compliance: an ESP32 module used inside an enclosure
  with no RF amplification is generally covered by the module's own FCC-ID (e.g. ESP-WROOM-32:
  2AC7Z-ESPWROOM32) as long as the antenna isn't modified.
- CE: same logic with the module's RED CE mark.

---

## 14. Advanced Features (already implemented)

### Tag cloning prevention
Each tag's UID is stored in the UID-map alongside the code at write time. When a *different*
UID is presented for the same code, the firmware accepts it but logs a warning. A future
extension (sector 2) can store an HMAC of `(UID || payload)` keyed on a device-side secret,
making clones detectable: any tag whose HMAC doesn't match its UID is rejected.

### Multi-tag history (last 5)
`g_history[5]` rolling buffer; surfaced on the Dashboard "Recent Tags" list with color
swatches and source attribution.

### Auto color preview
The web UI mirrors the tag's color into the AMS Slot 1 swatch as soon as the report
echoes back, plus the "Last Scan" panel updates immediately on read. The Quick-Write
preset buttons are bordered in the profile color too.

### Usage tracking (estimate)
The firmware tracks the number of reads per UID via the UID-map (extend `UidMap` with
`uint32_t scan_count` and `uint32_t first_seen`). This gives a rough "how many prints
since I installed this spool" indicator without needing a load-cell. Each load-after-cold
counts as one scan; for finer estimation, watch print start/end events on the report
topic.

### Quick-write presets
Every profile becomes a one-click write button on the Tag Manager tab. Click a colored
"PLA Black" button → tap blank tag → done.

---

## File summary

| File | Purpose |
|---|---|
| `SSOLite.ino` | Main firmware: state machine, RFID, MQTT, Wi-Fi, persistence, web API. |
| `web_pages.h` | HTML / CSS / JS for the embedded web UI as `PROGMEM` strings. |
| `README.md` | This document. |

## License & disclaimer
This code talks to a Bambu printer using the documented (LAN-mode) MQTT command set.
It uses **Generic** filament profiles only — it does not spoof or replicate Bambu's
proprietary RFID tag format. Use at your own risk; not affiliated with Bambu Lab.
