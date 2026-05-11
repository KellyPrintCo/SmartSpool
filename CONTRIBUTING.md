# Contributing to SmartSpool

Thanks for thinking about contributing.

## What kinds of contributions are welcome

- **Bug fixes** — always welcome. Open a PR with a clear description of what was broken.
- **New filament profile presets** — if you've tuned settings for a specific brand, share them.
- **Documentation improvements** — better wording, clearer instructions, troubleshooting tips.
- **Mount variants** — alternate STLs for different mounting preferences (rear mount, slot 2, etc.).
- **New features** — discuss in an issue first before writing code.

## What's out of scope

- Forking and rebranding without attribution (legally fine, just not a contribution).
- Closing/proprietary changes to firmware (this is and stays MIT-licensed).
- Changes that break compatibility with shipped customer units without a clear migration path.

## How to contribute code

1. Fork the repository
2. Create a branch: `git checkout -b feature/your-thing`
3. Make your changes
4. Test thoroughly — both the firmware on a real ESP32 and the extension in Chrome
5. Commit with clear messages
6. Open a pull request describing what changed and why

## Code style

**Firmware (C++):**
- Match the existing style (Allman braces, 2-space indents, snake_case for variables)
- Keep functions short and focused
- Add comments for non-obvious logic
- Use `Serial.printf` with the `[TAG]` prefix convention for logging

**Extension (JavaScript):**
- Match the existing style (2-space indents, camelCase, double quotes)
- Avoid frameworks — vanilla JS keeps the extension small and fast
- Use the existing message-passing patterns instead of new ones
- No build tools — files load directly into Chrome

## Testing your changes

Before opening a PR:

1. Flash the firmware to a real ESP32 with an MFRC522 and verify the basic flow works
2. Load the extension unpacked in Chrome and walk through the full Print Wizard flow
3. Run through the test checklist in `docs/SmartSpool_Test_Checklist.docx` if your change touches anything tested there

## Reporting security issues

Don't open public issues for security problems. Email Kelly Print Co through
the Etsy shop or open a private security advisory on this repo.

## License

By contributing, you agree your contributions will be MIT-licensed (same as
the rest of the project).
