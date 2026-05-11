# Publishing Firmware Updates

SmartSpool v1.3.0+ supports over-the-air (OTA) updates through Print Wizard.
This document explains how to publish a new firmware version that customer
devices will pick up automatically.

## One-time setup

### 1. Create the GitHub repository

Create a public repo at `github.com/KellyPrintCo/SmartSpool` (or fork the
existing one). Push the SSOLite firmware source plus this `version.json`
file at the repo root.

### 2. Update extension constants if you forked

If your GitHub org isn't named `KellyPrintCo`, change two constants:

**`mw-wizard/background.js`** line ~20:
```js
const VERSION_JSON_URL =
  "https://raw.githubusercontent.com/<YOUR_ORG>/SmartSpool/main/version.json";
```

**`SSOLite/web_pages.h`** (the device's own web UI updater) — search for
`VERSION_JSON` and update the URL.

## Publishing a new firmware version

Three steps. Total time: about 5 minutes.

### Step 1 — Build the firmware binary

In Arduino IDE:

1. Open `SSOLite.ino`
2. Update the `FW_VERSION` constant — increment from `1.3.0` to `1.3.1`, `1.4.0`, etc.
3. Make your code changes
4. Save the sketch
5. **Sketch → Export Compiled Binary**
6. The IDE creates a `.bin` file in the sketch folder named something like
   `SSOLite.ino.esp32.bin` or `build/esp32.esp32.esp32/SSOLite.ino.bin`
7. Rename this file to `firmware.bin`

### Step 2 — Create a GitHub Release

1. Go to `github.com/KellyPrintCo/SmartSpool/releases/new`
2. Click **"Choose a tag"** and create a new tag matching your version: `v1.3.1`
3. Set the release title to the same: `v1.3.1`
4. In the description, write the release notes (these appear in Print Wizard
   when customers see the update prompt)
5. **Attach `firmware.bin`** by dragging it into the assets area
6. Click **"Publish release"**

The firmware binary is now publicly accessible at:
```
https://github.com/KellyPrintCo/SmartSpool/releases/download/v1.3.1/firmware.bin
```

### Step 3 — Update version.json

Edit `version.json` at the repo root and commit:

```json
{
  "version": "1.3.1",
  "firmware_url": "https://github.com/KellyPrintCo/SmartSpool/releases/download/v1.3.1/firmware.bin",
  "release_notes": "Fixed UID fallback timing bug. Improved web UI on small screens.",
  "required": false,
  "min_compatible": "1.3.0"
}
```

Commit and push to `main`. Customers who open Print Wizard or the device web
UI will see the update prompt within seconds.

## version.json fields

| Field | Purpose |
|---|---|
| `version` | The new firmware version (semver: `major.minor.patch`) |
| `firmware_url` | Direct download URL for the `.bin` file from your GitHub release |
| `release_notes` | Short description shown to customers in the update prompt |
| `required` | If `true`, customers cannot skip the update — they must install it before using Print Wizard |
| `min_compatible` | The oldest firmware version the current Print Wizard extension supports. If a customer has firmware older than this, the update is required regardless of `required` field |

## When to mark `required: true`

Mark a release required only when:

- A critical security fix is in this version
- A bug in the older firmware causes data loss or device damage
- Bambu changed their MQTT API and the old firmware no longer works

Avoid making updates required for new features or minor bug fixes. The friction
annoys users who don't want to update right now.

## What customers see

**Optional update:**
- Connect to SmartSpool in Print Wizard → "Firmware update available" step
- "Update SmartSpool →" button (primary action)
- "Skip for now" button (secondary)
- Release notes shown in a callout

**Required update:**
- Same UI but the Skip button is hidden
- Customer must install before continuing to the rest of the wizard

**During update:**
- Real-time progress bar: download → upload → flash → reboot → reconnect
- Total time: typically 25-40 seconds on a normal home network
- Wizard auto-advances to filament check after success

**If update fails:**
- Error message in red, with retry option
- Device stays on the previous firmware version — never bricked
- Customer can retry, skip (if optional), or get help

## Important: the chicken-and-egg problem

**Customers on firmware older than v1.3.0 cannot OTA update** — they don't have
the OTA endpoint yet. They need to flash v1.3.0 (or newer) over USB the first
time using Arduino IDE. All subsequent updates can be OTA.

This is unavoidable. Mention it in your Etsy listing and setup guide:

> "Firmware updates happen automatically through Print Wizard once your
> SmartSpool is on v1.3.0 or later. Units shipped from June 2024 onward come
> pre-flashed with v1.3.0+ and never need manual flashing."

If you're selling assembled units, always flash the latest version before
shipping so customers never see this issue. DIY kit buyers should be directed
to install v1.3.0+ as their first flash.

## Rolling back a bad release

If you publish a release that breaks something:

1. Revert `version.json` to the previous version (commit the change)
2. New customers will get the older firmware automatically
3. Customers already on the broken version need to either:
   - Wait for you to publish a fixed version (they'll auto-update to it)
   - Flash via USB if the broken firmware is so broken it can't OTA

This is why it's worth keeping the previous firmware build around. Don't delete
old GitHub releases — they're your rollback path.

## Testing a new release before publishing

The safest workflow:

1. Build the new firmware binary
2. Flash it to a test unit via USB (don't use OTA on the test unit yet)
3. Run through your test checklist (see `SmartSpool_Test_Checklist.docx`)
4. Create the GitHub release as a **draft** first
5. Manually test OTA from the draft URL — set `VERSION_JSON_URL` to a private gist with the draft details
6. Once OTA works end-to-end, publish the release and update `version.json` on `main`

This catches the rare case where the binary itself is fine but the OTA flash
process has an issue (corrupted upload, wrong partition scheme, etc).
