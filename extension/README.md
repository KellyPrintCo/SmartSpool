# MakerWorld Print Wizard for SSO-Lite

A Chrome extension that adds a "Print Wizard" button to MakerWorld model pages
and walks beginners through the whole printing process — checking what
filament is loaded, picking the right one, tagging the spool, choosing the
build plate, and handing the model off to Bambu Studio.

It pairs with **SmartSpool OS Lite (SSO-Lite)** running on your ESP32 + MFRC522
reader, talks to it over your LAN, and uses Studio's existing deeplink to send
the model to your printer. No cloud, no server, no extra software.

## How it works

```
You browse a model on MakerWorld
        │
        │  ① extension injects "Print Wizard" button
        ▼
  Wizard opens as an overlay
        │
        │  ② calls SSO-Lite over LAN (HTTP) to read AMS Slot 1
        ▼
  Wizard checks if the loaded filament matches what the model needs
        │
        │  ③ if not, you pick a profile and tap a tag — SSO-Lite writes the
        │     tag and pushes the new filament to AMS Slot 1 over MQTT
        ▼
  Wizard confirms the printer is ready (plate, bed prep)
        │
        │  ④ clicks MakerWorld's own "Open in Studio" link
        ▼
  Bambu Studio opens with the model — you click Print
```

The extension never talks to the printer directly. All printer communication
goes through the SSO-Lite firmware (which you already have running for AMS
auto-assignment). Studio handles the actual print job.

## Install

This is an **unpacked extension** — Chrome's developer load mode. Five steps:

1. **Update SSO-Lite firmware to v1.1.0** (see `SSOLite.ino` in the SSO-Lite
   project folder). The new version adds CORS headers (so the browser can call
   it) and an mDNS responder (so the extension can find it as `ssolite.local`).
   Re-flash from Arduino IDE; everything keeps working as before.

2. Download or clone this folder somewhere stable on your Mac/PC. Don't put
   it in Downloads or it'll go away when you clean up.

3. Open Chrome and go to `chrome://extensions`.

4. Toggle **Developer mode** on (top-right corner of that page).

5. Click **Load unpacked** and pick the `mw-print-wizard/` folder.

The extension's icon (a green spool) should appear in your toolbar. If
you don't see it, click the puzzle-piece icon and pin it.

## First use

1. Click the toolbar icon. The popup shows whether it can reach SSO-Lite.
2. If it says "unreachable", click **Settings**:
   - Most users: leave address as `ssolite.local`. This works on macOS and
     iOS out of the box, on Windows 10+ it usually works, on Linux it works
     if you have Avahi/`avahi-daemon` installed.
   - If `ssolite.local` doesn't resolve, replace it with the device's IP
     address (you can find this in your router's DHCP table or in the
     SSO-Lite serial monitor at boot).
   - Click **Test connection**. You should see "Connected (vX.X.X)."
   - Click **Save**.
3. Open any model page on `https://makerworld.com/...`. Look for the green
   **Print Wizard** button next to MakerWorld's regular Print/Open in Studio
   button. (If MakerWorld redesigns and the button can't find an anchor, it
   floats in the bottom-right corner instead.)
4. Click it. Walk through the wizard.

## What the wizard does, step by step

| Step | What happens |
|---|---|
| **Welcome** | Shows the model title, image, recommended materials and plate (scraped from the page). |
| **Connect** | Verifies SSO-Lite is reachable. Shown only on first use; skipped after the first successful connection in a wizard session. |
| **Filament check** | Asks SSO-Lite what's currently in AMS Slot 1, compares against the page's recommended materials. Big green ✓ if they match, big yellow ⚠ if they don't. |
| **Pick filament** *(only if mismatched)* | Shows your SSO-Lite profile library as a grid of color-coded cards. Profiles whose material matches the model are pinned to the top with a "recommended" badge. |
| **Load & tag** *(only if swapping)* | Tells you to load the spool and tap the tag. Live status — polls SSO-Lite every 800ms, advances when a successful read of the chosen profile is detected. |
| **Build plate** | Shows what plate the model wants, with a beginner-friendly explanation. Requires you to tick "the right plate is on my printer" before continuing. |
| **Quick prep** | Four-item checklist: clean bed, plate seated, nozzle clear, door position. All four must be ticked. |
| **Send to Studio** | Clicks MakerWorld's existing "Open in Studio" link. Studio opens with the model loaded; you press Print there. |
| **Done** | A short reminder to watch the first layer. |

## What about the "MQTT verification failed" error?

The wizard inherits SSO-Lite's solution: your printer needs to be in **LAN
Only Mode + Developer Mode**. Without it, MQTT commands (the AMS slot
assignment) get rejected. See the SSO-Lite README's troubleshooting section.

If you're keeping cloud mode (e.g. you need Bambu Handy via cloud), turn off
SSO-Lite's auto-assign and the wizard will still walk you through everything
useful — it just won't push the AMS change. You set the slot manually in
Studio.

## Files

| File | Purpose |
|---|---|
| `manifest.json` | MV3 manifest. Permissions, content-script targets, web-accessible resources. |
| `background.js` | Service worker. Owns the SSO-Lite HTTP client and the settings store. Everything else talks to it via `chrome.runtime.sendMessage`. |
| `content.js` | Injected into MakerWorld pages. Detects model pages, scrapes metadata, injects the button, opens the wizard iframe. |
| `content.css` | Button + iframe overlay styling. Namespaced with `.sso-mw-` to avoid collisions with MakerWorld's own classes. |
| `wizard.html` / `wizard.js` / `wizard.css` | The wizard itself. Runs inside an iframe that the content script injects. Communicates with the parent (content script) via `postMessage` and with SSO-Lite via the background worker. |
| `popup.html` / `popup.js` / `popup.css` | Toolbar popup — health check + quick links. |
| `options.html` / `options.js` | Settings page — SSO-Lite hostname/IP/port, test button. |
| `icons/` | 16/48/128 PNG icons. |

## Permissions explained

| Permission | Why we need it |
|---|---|
| `storage` | Remember your SSO-Lite hostname between sessions. |
| `host_permissions` for `*.makerworld.com` | Inject the content script on model pages. |
| `host_permissions` for `http://ssolite.local/*` and `http://*/*` | Talk to SSO-Lite over LAN. The wildcard `http://*/*` is there so users can use a raw IP (which we can't predict). MV3 service workers need explicit host permission to fetch cross-origin. |

The extension does **not** read or send anything to any cloud service. All
network traffic goes either to MakerWorld (you're already there) or to your
own SSO-Lite device on your LAN.

## Troubleshooting

**"Print Wizard" button never appears on MakerWorld**
- Open the page DevTools → Console. The content script logs once at load.
- MakerWorld may have changed their DOM. As a fallback the button auto-floats
  to the bottom-right corner if the script can't find the regular Print
  button anchor.
- Make sure the URL matches `/models/` — the script ignores other pages.

**Wizard says "Couldn't reach SSO-Lite"**
- Open the toolbar popup. If it's also unreachable from there:
  - Open `http://ssolite.local/` directly in a new tab. If that doesn't load,
    you have an mDNS issue — switch to the IP in Settings.
  - Make sure SSO-Lite is on the same Wi-Fi as your computer. Some "guest"
    networks block client-to-client traffic.
  - Check the SSO-Lite serial monitor — does it print `[mDNS] http://ssolite.local/` at boot?

**"Tag write" times out**
- The wizard polls for 65 seconds. If you didn't tap a tag in time, click
  back and re-pick the profile.
- Make sure the SSO-Lite reader's LED is doing its boot-blink — if it
  isn't, the ESP32 isn't powered.

**Studio doesn't open at the end**
- Make sure Bambu Studio is installed (`bambustudio://` deeplinks need it).
- If MakerWorld's own "Open in Studio" button doesn't work either, the
  problem is upstream of us.
- The wizard shows a fallback message in the "Send" step if it can't find
  the link.

## License

Same as SSO-Lite. Not affiliated with Bambu Lab or MakerWorld.
