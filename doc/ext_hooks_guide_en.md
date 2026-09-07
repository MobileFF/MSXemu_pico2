# Runtime Menu, Hotkey, and Config Key Reference

> **Note**: this emulator does not have the PB-1000 version's "hook a subroutine from a BASIC `CALL`" extension mechanism. This MSX emulator's customization points are consolidated into three things: the runtime menu (GUI+F7), hotkeys, and the `msx.ini` file. This document is the detailed reference for all three. For programmatic extension (adding new APIs or mapper types), see `dev_guide_en.md`.

---

## 1. Runtime Menu (GUI+F7)

Pressing **either GUI key (Windows/Command key) + F7** together on a USB keyboard pauses gameplay and opens an on-screen menu (`show_emulator_menu()` in `mp/msx_menu.py` — the actual implementation is `show()` in `mp/msx_runtime_menu.py`, lazily imported the first time GUI+F7 is pressed).

Controls: **Up/Down** to move between items, **ENTER** to select, **ESC** to close the menu and resume gameplay.

| Menu item | Behavior |
| :--- | :--- |
| **Swap Cartridge** | Pick a `.ROM` file from the SD card to replace the current cartridge with (opens in the current cartridge's own folder; subfolders browsable). Automatically calls `msx.reset()`. ROMs over 32KB are automatically loaded in Mega ROM (paged) mode |
| **Save State** | Saves the current CPU/VDP/RAM/VRAM state to the SD card (takes ~2 seconds, shows a "Saving…" message). Rotates per-cartridge, next to the ROM itself, keeping up to 10 generations (e.g. `ANTADV.ROM` → `ANTADV.0.sav`, `ANTADV.1.sav`, ...) |
| **Load State** | Pick a generation from the saved list and restore it |
| **Audio Settings** | Opens a sub-screen for live-adjusting volume and the audio filter (see below) |
| **Display Settings** | Opens a sub-screen for switching between LCD/HDMI output, frame skip, LCD panel model, rotation, and HDMI baud (see below) |
| **Reset MSX** | Resets the Z80 (does not change the loaded cartridge) |
| **Resume** | Closes the menu and resumes gameplay (same as ESC) |

### Audio Settings Sub-Screen

| Control | Behavior |
| :--- | :--- |
| Up/Down | Switch between the Volume / Filter fields |
| Left/Right | Adjust the selected field's value (in steps of 16, takes effect on the audio immediately) |
| ENTER | Writes the current Volume/Filter values to `msx.ini` and closes the screen |
| ESC | Closes the screen without saving (the values stay changed for the current session either way) |

Volume ranges 0-256 (in steps of 16); Filter ranges 0-8 (in steps of 1).

### Display Settings Sub-Screen

LCD and HDMI are mutually exclusive (no simultaneous output). Fields, top to bottom: `Display` (`LCD`/`HDMI`, live), `Frame Skip` (HDMI send throttling, live), `LCD Panel` (restart-only), `Rotate` (0/180, restart-only), `HDMI Baud` (restart-only), and `Reinit <LCD/HDMI> now` (reinitializes the currently active side in place — the **GUI+ESC** hotkey does the same thing without opening any menu).

| Control | Behavior |
| :--- | :--- |
| Up/Down | Switch fields |
| Left/Right | Adjust the selected field's value (`Display`/`Frame Skip` take effect immediately) |
| ENTER | With the cursor on `Reinit ... now`, performs the reinit in place (the screen stays open). On any other field, writes the current values to `msx.ini` and closes the screen |
| ESC | Closes the screen without saving |

---

## 2. Hotkeys (During Gameplay)

| Key | Action |
| :--- | :--- |
| **GUI + F7** | Opens the runtime menu (above) |
| **GUI + P** | Toggles the LCD/HDMI display live, without opening a menu |
| **GUI + ESC** | Reinitializes the currently active display (LCD/HDMI) in place, without opening a menu — recovers from "No Signal" during long `display=hdmi` sessions |

All of these are edge-triggered (fire once per physical key press), so holding them down does not repeatedly trigger the action.

Save/Load are done from the runtime menu's `Save State` / `Load State` (2026-09-08: the standalone F5/F8 hotkeys were removed — saving/loading also forwarded the F5/F8 keypress to the MSX itself, which could conflict with MSX software that uses F5/F8 for its own purposes).

---

## 3. Complete `msx.ini` Reference

Written as `key=value` lines (lines starting with `#` are comments) at the SD card's **root** (`/sd/msx.ini`).

```ini
bios=/sd/msx/MSX_jp.rom
cart=/sd/msx/CART.ROM
lcd=ILI9341
rotate=180
volume=128
audio_filter=2
display=hdmi
hdmi_frame_skip=2
hdmi_baud=8000000
```

| Key | Default | Description |
| :--- | :--- | :--- |
| `bios` | `/sd/msx/MSX.ROM` | Path to the BIOS/BASIC ROM file |
| `cart` | (none) | Path to the cartridge ROM. If omitted, an interactive selector is shown at boot |
| `lcd` | `ST7796` | The connected LCD panel model: `ST7796` (480×320) or `ILI9341` (320×240). Restart required |
| `rotate` | `0` | Set to `180` to flip the display 180° (LCD only — has no effect on HDMI output). Restart required |
| `volume` | `256` | Startup volume (0-256). Also adjustable from the runtime menu's Audio Settings |
| `audio_filter` | `0` | Startup audio low-pass filter strength (0-8) |
| `display` | `lcd` | `lcd` or `hdmi`. **Mutually exclusive** (no simultaneous output) — only the chosen side's hardware is initialized at boot. Also switchable live from the runtime menu's Display Settings (or the GUI+P hotkey) |
| `hdmi_frame_skip` | `2` | Send the HDMI bridge output every Nth frame (only matters when `display=hdmi`). `1` sends every frame |
| `hdmi_baud` | `8000000` | SPI baud rate (Hz) for the HDMI bridge output. 5MHz/8MHz confirmed clean on real hardware — 10MHz was found to corrupt the received palette. Restart required |

If `cart` is omitted, or the specified file isn't found, an interactive selector listing the SD card's `.ROM` files (subfolders included) is shown (with no USB keyboard connected, the first file in the list is auto-selected).

Adjusting `volume`/`audio_filter`/`display`/`hdmi_frame_skip` in the runtime menu and pressing ENTER automatically updates the corresponding key (other keys and comment lines are preserved). `lcd`/`rotate`/`hdmi_baud` are restart-only — pressing ENTER on Display Settings still writes them to `msx.ini`, but they only take effect on the next boot.

The formerly-existing `hdmi` (separate HDMI on/off) and `boot_exclusive` keys, and the `display=both` (simultaneous output) value, were removed when LCD/HDMI became strictly mutually exclusive. Leftover mentions of these in an existing `msx.ini` are harmlessly ignored, except `display=both`, which now falls back to `lcd`.

---

## 4. Mega ROM (Bank-Switched Cartridge) Support in Detail

Bank-switched cartridges of 128KB or larger ("Mega ROM") are **copied once to the Pico 2's onboard flash** before being played — never read directly from SD during gameplay (`load_cart_smart()` in `mp/msx_menu.py`).

- Supported mappers: ASCII-8 / ASCII-16 / KONAMI (no SCC). Auto-detected from the ROM's leading content.
- On first selection, the file is copied from SD to onboard flash (`/megarom_cache.rom`), taking a few seconds (shown as "Loading…" on screen). Re-selecting the same ROM skips the copy.
- Onboard flash has roughly 3MB free. Only one ROM is cached at a time; switching to a different Mega ROM overwrites it.
- All in-game bank switches read from onboard flash — the SD card is never touched during gameplay (avoiding contention on the SPI bus shared with the LCD).

**Technical background**: the original design read directly from the SD card on every bank switch, which caused two problems: (1) SD's per-access protocol overhead (several to over ten ms each) tanked FPS to around 6 on bank-switch-heavy games, and (2) it contended with the LCD's display DMA transfer on the shared SPI bus, causing I/O errors. Switching to a cache on the Pico 2's onboard flash (a separate QSPI bus, independent of SPI1) solved both problems, restoring FPS to roughly 52-64 — on par with regular cartridges.

---

## 5. Joystick Support

Wired per `hardware_guide_en.md`, joystick input is reflected through the same path real MSX hardware uses (the PSG's I/O ports, registers 14/15). Unmodified real-hardware game joystick-reading routines work as-is — no additional software configuration is needed. With no GPIO wiring, the joystick always reads as "nothing pressed," so keyboard-only play works fine too.

---

## Related Documentation

- `usage_guide_en.md` — the overall walkthrough from initial setup to boot
- `extension_api_en.md` — the `msx` module's programmatic API
- `dev_guide_en.md` — developer steps for adding new config keys or menu items
