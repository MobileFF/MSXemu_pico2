# MSX1 Emulator for Raspberry Pi Pico 2

An MSX1 emulator running on Raspberry Pi Pico 2 (RP2350).

## Overview

This project emulates an MSX1 computer, equipped with a Z80 CPU, TMS9918A VDP, and AY-3-8910 PSG. It combines a high-speed C emulation core with flexible MicroPython logic for peripheral handling, providing an environment capable of running real BASIC/cartridge software as well as developing custom extensions.

## Key Features

- **High-Performance Emulation Core**: Z80 CPU ([superzazu/z80](https://github.com/superzazu/z80)), TMS9918A VDP ([vrEmuTms9918](https://github.com/visrealm/vrEmuTms9918)), and AY-3-8910 PSG ([emu2149](https://github.com/digital-sound-antiques/emu2149)) all implemented in C, with a pipelined rendering design achieving real-hardware-class frame rates (measured ~64 FPS).
- **MicroPython Framework**: Boot flow, UI, and configuration logic are written in MicroPython for easy customization.
- **Display Support**: Supports ST7796 (480×320) and ILI9341 (320×240) TFT LCDs, switchable via `config.txt`. 180° screen rotation is also supported.
- **External Keyboard**: Supports USB HID keyboards in host mode.
- **Cartridge Support**: PLAIN / ASCII-8 / ASCII-16 / KONAMI mappers (with auto-detection). Bank-switched "Mega ROM" cartridges (128KB+) are supported beyond available RAM via a cache on the Pico 2's own onboard flash.
- **Storage**: Loads the BIOS/cartridge ROM from an SD card, and supports zero-copy save/load state (including RAM and VRAM).
- **Runtime Menu**: GUI+F7 on a USB keyboard opens an on-screen menu for swapping cartridges, saving/loading state, resetting, adjusting volume/audio filter, and HDMI settings — all without stopping gameplay.
- **Joystick Support**: Supports an Atari/MSX 9-pin joystick (JOY1) — directions and trigger A/B.
- **Optional HDMI Output**: Video can also be output over HDMI using a second Pico 2 + PICO-HDMI-PLUS (see [hdmi_bridge/README.md](hdmi_bridge/README.md)).

## Limitations

- **MSX1 only.** MSX2/2+/turboR emulation is not supported.
- **Only one cartridge slot can be used at a time.** Loading two cartridges simultaneously is not supported.
- **Does not run on the original Raspberry Pi Pico (RP2040).** A Raspberry Pi Pico 2 (RP2350) is required due to performance and RAM requirements.
- **Using a Mega ROM together with HDMI output can slightly reduce performance** (due to onboard-flash cache reads). Adjust the Skip Frame setting (`hdmi_frame_skip`) in the runtime "HDMI Settings" menu if needed.
- **Joystick port 2 (JOY2) is not supported.** Only JOY1 is available.
- **Simultaneous LCD + HDMI output (`display=both`) is not recommended.** The LCD output may show occasional glitches. Using only one output (`display=lcd` or `display=hdmi`) is recommended.

## Quick Start

1.  **Hardware**: Prepare a Raspberry Pi Pico 2, an ST7796 or ILI9341 LCD, and an SD card module. See the [Hardware Guide](doc/hardware_guide_en.md) for details.
2.  **Build**: Compile the custom MicroPython firmware with `bldfrm_msx.sh`. See the [Build Guide](doc/build_guide_en.md).
3.  **Flash**: Copy the generated `firmware/firmware_msx.uf2` to your Pico 2 via BOOTSEL.
4.  **Setup**: Transfer the Python files under `mp/` to the Pico 2, and place the MSX BIOS ROM and any cartridge ROMs under `/sd/msx/` on the SD card. See the [Usage Guide](doc/usage_guide_en.md).
5.  **Run**: Deployed as `main.py`, the emulator starts automatically on power-up.

## Documentation Index

- [Build Guide](doc/build_guide_en.md) - How to set up the build environment and compile firmware.
- [Hardware Guide](doc/hardware_guide_en.md) - Wiring diagrams, BOM, and pin assignments.
- [Usage Guide](doc/usage_guide_en.md) - Initial setup, ROM preparation, and operation manual.
- [Development Guide](doc/dev_guide_en.md) - Code structure, internal APIs, and debugging tips.
- [Architecture](doc/architecture_en.md) - Overview of the emulator's internal design.
- [Extension API](doc/extension_api_en.md) - Reference for the `msx` C module's public API.
- [Extension Hooks Guide](doc/ext_hooks_guide_en.md) - Runtime menu and other extension points.
- [Memory Map](doc/memory_map_en.md) - Save-state binary layout.

*(Japanese documentation is available in files without the `_en` suffix)*

## Project Structure

```text
MSX_emu_pico2/
├── src/msx/                 # C source (Z80/VDP/PSG cores & MicroPython wrapper)
├── mp/                       # MicroPython code (boot flow, menu, keymap)
├── doc/                      # Documentation & guides
├── hdmi_bridge/               # Optional HDMI output bridge (sender side) resources
├── sample/                    # Sample BASIC programs, etc.
└── README_en.md                # This file
```

## License

MIT License. See [LICENSE](LICENSE) for details.

Bundled third-party components (`src/msx/z80/`, `src/msx/tms9918/`, `src/msx/emu2149/`) are each under their own license (all MIT) — see the LICENSE file in each component's directory.

Copyrighted files such as the MSX BIOS ROM are NOT included in this repository. Please obtain your own.

## Acknowledgments

- This project's emulation core builds on the following excellent open-source libraries. Many thanks to their authors.
  - [superzazu/z80](https://github.com/superzazu/z80) — Z80 CPU core (MIT License)
  - [visrealm/vrEmuTms9918](https://github.com/visrealm/vrEmuTms9918) — TMS9918A VDP core (MIT License)
  - [digital-sound-antiques/emu2149](https://github.com/digital-sound-antiques/emu2149) — AY-3-8910 PSG core (MIT License)
- This project builds on the same author's Casio PB-1000 emulator (the C + MicroPython two-layer architecture, USB host stack, etc.) as its design and implementation foundation.
- Thanks to everyone who has shared information about the MSX.
- The source code was primarily developed using the following AI agent tools:
  - Claude Code
  - OpenAI Codex
  - Google Antigravity
