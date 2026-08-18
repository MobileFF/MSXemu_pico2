# Runtime Menu, Hotkey, and Config Key Reference

> **Note**: this emulator does not have the PB-1000 version's "hook a subroutine from a BASIC `CALL`" extension mechanism. This MSX emulator's customization points are consolidated into three things: the runtime menu (GUI+F7), hotkeys, and the `config.txt` file. This document is the detailed reference for all three. For programmatic extension (adding new APIs or mapper types), see `dev_guide_en.md`.

---

## 1. Runtime Menu (GUI+F7)

Pressing **either GUI key (Windows/Command key) + F7** together on a USB keyboard pauses gameplay and opens an on-screen menu (`show_emulator_menu()` in `mp/msx_menu.py`).

Controls: **Up/Down** to move between items, **ENTER** to select, **ESC** to close the menu and resume gameplay.

| Menu item | Behavior |
| :--- | :--- |
| **Swap Cartridge** | Pick a `.ROM` file from the SD card to replace the current cartridge with (automatically calls `msx.reset()`). ROMs over 32KB are automatically loaded in Mega ROM (paged) mode |
| **Save State** | Saves the current CPU/VDP/RAM/VRAM state to `save.bin` on the SD card (takes ~2 seconds, shows a "Saving…" message) |
| **Load State** | Restores state from `save.bin` |
| **Audio Settings** | Opens a sub-screen for live-adjusting volume and the audio filter (see below) |
| **Reset MSX** | Resets the Z80 (does not change the loaded cartridge) |
| **Resume** | Closes the menu and resumes gameplay (same as ESC) |

### Audio Settings Sub-Screen

| Control | Behavior |
| :--- | :--- |
| Up/Down | Switch between the Volume / Filter fields |
| Left/Right | Adjust the selected field's value (in steps of 16, takes effect on the audio immediately) |
| ENTER | Writes the current Volume/Filter values to `config.txt` and closes the screen |
| ESC | Closes the screen without saving (the values stay changed for the current session either way) |

Volume ranges 0-256 (in steps of 16); Filter ranges 0-8 (in steps of 1).

---

## 2. Hotkeys (During Gameplay)

| Key | Action |
| :--- | :--- |
| **F5** | Quick save (same as "Save State", to `save.bin` on the SD card) |
| **F8** | Quick load (same as "Load State") |
| **GUI + F7** | Opens the runtime menu (above) |

F5/F8 are edge-triggered (fire once per physical key press), so holding them down does not repeatedly trigger the action.

---

## 3. Complete `config.txt` Reference

Written as `key=value` lines (lines starting with `#` are comments) in `/sd/msx/config.txt` on the SD card.

```ini
bios=/sd/msx/MSX_jp.rom
cart=/sd/msx/CART.ROM
lcd=ILI9341
rotate=180
volume=128
audio_filter=2
```

| Key | Default | Description |
| :--- | :--- | :--- |
| `bios` | `/sd/msx/MSX.ROM` | Path to the BIOS/BASIC ROM file |
| `cart` | (none) | Path to the cartridge ROM. If omitted, an interactive selector is shown at boot |
| `lcd` | `ST7796` | The connected LCD panel model: `ST7796` (480×320) or `ILI9341` (320×240) |
| `rotate` | `0` | Set to `180` to flip the display 180° |
| `volume` | `256` | Startup volume (0-256). Also adjustable from the runtime menu's Audio Settings |
| `audio_filter` | `0` | Startup audio low-pass filter strength (0-8) |

If `cart` is omitted, or the specified file isn't found, an interactive selector listing the SD card's `.ROM` files is shown (with no USB keyboard connected, the first — or only — file in the list is auto-selected).

Adjusting `volume`/`audio_filter` in the runtime menu's Audio Settings and pressing ENTER automatically updates these two keys (other keys and comment lines are preserved).

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
