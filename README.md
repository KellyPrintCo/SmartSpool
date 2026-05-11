# SmartSpool

> **Automatic RFID filament identification for the Bambu Lab AMS 2 Pro.**
> Built and maintained by [Kelly Print Co](https://www.etsy.com/shop/KellyPrintCo).

Load a tagged spool into AMS Slot 1 and your printer instantly knows exactly
what filament is loaded — material, brand, color, and temperatures. No screen
tapping, no manual entry.

This repo contains the open-source firmware, browser extension, and 3D-printable
files. Want the convenience of a pre-built kit? Get one on [Etsy](https://www.etsy.com/shop/KellyPrintCo).

## What's in this repo

| Folder | What it is |
|---|---|
| [`firmware/`](firmware/) | ESP32 firmware — the brain of SmartSpool |
| [`extension/`](extension/) | Print Wizard Chrome extension — adds a guided print button to MakerWorld |
| [`hardware/`](hardware/) | 3D-printable mount STL/3MF files |
| [`version.json`](version.json) | Update manifest — devices fetch this to check for new firmware |
| [`docs-OTA-process.md`](docs-OTA-process.md) | How firmware updates work and how to publish them |

## Quick start

**If you bought a kit:** unbox it, follow the setup card. You're done.

**If you're building it yourself from these files:**

1. **Print the mount** (PETG recommended) from `hardware/stl/`
2. **Wire it up** — ESP32 + MFRC522, see `firmware/README.md` for the wiring diagram
3. **Flash the firmware** — open `firmware/SSOLite.ino` in Arduino IDE 2.x and click Upload
4. **Install the extension** — load `extension/` unpacked in Chrome
5. **Configure** — connect to the `SmartSpool-Setup` Wi-Fi, open `192.168.4.1`, enter your printer info

Full instructions in [`firmware/README.md`](firmware/README.md).

## Features

- **Automatic RFID slot 1 assignment** — tap a tagged spool, AMS updates instantly
- **Custom filament profiles** — unlimited material/brand/color combinations via web UI
- **Auto tag rewrite** *(optional)* — if you change filament info on the printer touchscreen, SmartSpool can update the RFID tag to match
- **Over-the-air updates** — firmware updates push automatically through the Print Wizard extension or device web UI
- **MakerWorld integration** — Print Wizard adds a guided "Print" button that walks beginners through filament check, build plate selection, and bed prep

## Requirements

- Bambu Lab **AMS 2 Pro**
- Bambu Lab **X1, P1, or A1 series** printer
- **LAN Only Mode** and **Developer Mode** enabled on the printer
- 2.4GHz WiFi network
- Components per [`firmware/README.md`](firmware/README.md) parts list

## Compatibility

| Bambu hardware | Status |
|---|---|
| AMS 2 Pro | ✅ Fully supported |
| AMS Lite | ❌ Different mount geometry |
| Original AMS | ❌ Different mount geometry |
| X1C / X1 Carbon | ✅ |
| P1S / P1P | ✅ |
| A1 / A1 mini | ✅ |
| H2D | Not yet tested |

## Releases

Firmware binaries for every release are attached to GitHub releases. The
current stable version is shown in [`version.json`](version.json).

Devices running v1.3.0 or later receive future updates automatically over WiFi.

## License

MIT License — see [LICENSE](LICENSE). You're free to fork, modify, sell, and
distribute. Attribution appreciated but not required.

This project is **not affiliated with Bambu Lab**. SmartSpool uses Bambu's
documented LAN-mode MQTT API and does not modify or interfere with the printer
firmware in any way.

## Contributing

Pull requests and issues welcome. See [`.github/ISSUE_TEMPLATE/`](.github/ISSUE_TEMPLATE/)
for the bug report and feature request templates.

## Support

- **Found a bug?** Open an [issue](https://github.com/KellyPrintCo/SmartSpool/issues/new/choose)
- **Need help with your kit?** Email Kelly Print Co via your Etsy order
- **Want to chat?** Discussions tab on this repo

## Acknowledgments

- Bambu Lab — for building the printer this works with
- The OrcaSlicer team — for documenting the MQTT protocol
- The 3D printing community on r/BambuLab — for being the most helpful corner of the internet
