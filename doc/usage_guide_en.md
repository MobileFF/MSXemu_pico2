# Usage Guide

## 1. Initial Setup

### What You Need

- A Raspberry Pi Pico 2 flashed with the built firmware (`firmware/firmware_msx.uf2` — see `build_guide_en.md`)
- An MSX BIOS/BASIC ROM (32KB) — not included in this project for copyright reasons; provide your own
- (Optional) cartridge ROM(s)
- A microSD card (FAT32-formatted)
- A USB keyboard + USB OTG adapter

### SD Card Directory Layout

```
/sd/msx/
  MSX_jp.rom        # BIOS ROM (any filename, specified in config.txt)
  ANTADV.ROM        # cartridge ROM(s), optional, multiple allowed
  NEMESIS.ROM        # large (Mega ROM) cartridges go in the same place
  config.txt         # config file (optional — works fine without one)
  save.bin           # save state (auto-created on first save)
```

### Writing `config.txt`

```ini
bios=/sd/msx/MSX_jp.rom
cart=/sd/msx/ANTADV.ROM
lcd=ST7796
rotate=0
volume=256
audio_filter=0
```

Every key is optional. See "Complete `config.txt` Reference" in `ext_hooks_guide_en.md` for defaults and details on each key. Omitting `cart` shows a menu at boot to pick a ROM from the SD card.

---

## 2. Boot Flow

1. Powering the Pico 2 (via USB) boots MicroPython.
2. (For development) connect with `mpremote` etc. and run `import main; main.run()`.
3. Initialization proceeds automatically in this order:
   - Clock tuning, UART console setup
   - USB host init (keyboard detection begins)
   - SD card mount
   - Read `config.txt`
   - LCD init (applying panel model/rotation settings)
   - Load the BIOS ROM
   - Load the cartridge (`config.txt`'s setting -> interactive selector -> boot straight to MSX BASIC if none chosen)
   - Audio setup
4. MSX starts, showing either the BASIC prompt or the cartridge's game.

The current FPS and audio ring-buffer level are printed to the console every 300 frames (visible over the real-hardware serial console).

---

## 3. Keyboard Operation

The USB keyboard's HID reports are mapped directly onto MSX's keyboard matrix. The physical layout is kept as close to a real MSX keyboard as possible, but **which character a given key actually produces depends on the BIOS ROM's regional layout (Japanese vs. English)**. For example, an English BIOS produces `:` with Shift+;, which is the US-layout BIOS's own convention — the key mapping itself is consistently based on physical key position.

### Special Keys and Hotkeys

| Key | Action |
| :--- | :--- |
| F5 | Quick save |
| F8 | Quick load |
| GUI (Win/Cmd) + F7 | Opens the runtime menu |

See `ext_hooks_guide_en.md` for details.

---

## 4. Joystick

Wired per `hardware_guide_en.md`, it's recognized the same way real hardware does (via the PSG). No additional software configuration is needed — unmodified real-hardware game joystick-reading routines work as-is. With no wiring, it always reads as "nothing pressed," so keyboard-only play works fine.

---

## 5. Runtime Menu (GUI+F7)

Pressing **GUI + F7** during gameplay opens a menu with:

- **Swap Cartridge** — swap in a different cartridge (Mega ROMs selectable too)
- **Save State** / **Load State** — save/restore the current state to the SD card
- **Audio Settings** — adjust volume and audio quality live
- **Reset MSX** — resets the Z80
- **Resume** — returns to gameplay

See `ext_hooks_guide_en.md` for exactly what each item does.

---

## 6. About Large Cartridges (Mega ROM)

Bank-switched cartridges of 128KB or more (some Konami titles like Gradius/Nemesis, and other ASCII-8/ASCII-16-mapper titles) can be played just like any other ROM: place them in `/sd/msx/` and select via `cart=` or the menu.

The first time you select one, it takes a few seconds to copy to the Pico 2's onboard flash (shown as "Loading…" on screen). After that, it loads quickly from that cache and plays at a speed comparable to a regular cartridge. See `ext_hooks_guide_en.md` and `architecture_en.md` for the technical details.

> Supported mappers are ASCII-8 / ASCII-16 / KONAMI (no SCC). KONAMI SCC cartridges (with the extra sound chip) are not currently supported on the audio side.

---

## 7. Save State

Using the `F5` (save) / `F8` (load) hotkeys, or the runtime menu's `Save State` / `Load State`, you can save/restore the entire CPU, VDP, RAM, and VRAM state to/from `save.bin` on the SD card. The save file is roughly 80KB. There is only one slot (no multiple save slots).

---

## 8. Troubleshooting

| Symptom | Fix |
| :--- | :--- |
| Boot screen is black | The LCD and SD card share wiring — check the physical connections first (`hardware_guide_en.md`). If the SD card also isn't recognized, it's almost certainly a wiring fault |
| Keyboard doesn't respond | Check the USB OTG adapter connection. Can also be caused by a missing `usb_host.start_bg_timer(8)` call in firmware (developer-facing — see `build_guide_en.md`) |
| Sound is distorted/harsh | Lower `volume=` in `config.txt`, or raise `audio_filter=` (also adjustable from the runtime menu's Audio Settings). For a real fix, add a hardware RC filter (`hardware_guide_en.md`) |
| Pitch sounds wrong | A real bug (an incorrect PSG clock setting) that existed earlier has been fixed — make sure you're running the latest firmware |
| A Mega ROM takes a while to load | Normal — the first selection triggers an SD-to-onboard-flash copy (a few seconds). Subsequent loads are fast |
| Save/load fails with an error | Check the SD card's free space and write permission. If it persists, suspect a physical SD card contact issue |

---

## Related Documentation

- `hardware_guide_en.md` — wiring and parts
- `build_guide_en.md` — build and flash instructions
- `ext_hooks_guide_en.md` — detailed reference for the runtime menu, hotkeys, and `config.txt`
- `architecture_en.md` / `memory_map_en.md` / `extension_api_en.md` / `dev_guide_en.md` — developer-facing technical docs
