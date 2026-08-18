# MSX1 Emulator Architecture

## Purpose

This document lays out the overall structure of the MSX1 emulator running on a Raspberry Pi Pico 2 (RP2350) + MicroPython, clarifying each module's responsibilities and dependencies.

## Design Principles

- **Performance-critical parts** (CPU/VDP/PSG emulation, memory management, display DMA) are implemented **in C**, exposed to MicroPython as the `msx` C user module.
- **Boot flow, configuration, UI, and input handling** are implemented **in Python (MicroPython)**, taking advantage of fast iteration and easy file handling (SD card, config.txt, etc.).
- The C side represents "the MSX system itself" and is written to be host-agnostic (real Pico 2 hardware vs. a native dev build), isolating Pico-SDK-specific code behind `#ifdef __arm__`. This lets the core logic be built and verified with native gcc without needing real hardware.

---

## Layers

```text
┌─────────────────────────────────────────────┐
│  mp/main.py   boot flow, main loop        │
│  mp/msx_menu.py   ROM select / runtime menu / save-state │
│  mp/msx_keymap.py USB HID -> MSX key-matrix mapping      │
└───────────────────┬───────────────────────────┘
                     │ import msx  (C user module)
┌───────────────────▼───────────────────────────┐
│  src/msx/modmsx.c     MicroPython <-> C bridge layer          │
│    - holds the global msx_state_t instance                    │
│    - converts between Python objects and C structs            │
│    - Pico-SDK-dependent bits: PWM audio, file I/O (Mega ROM fetch) │
└───────────────────┬───────────────────────────┘
                     │
┌───────────────────▼───────────────────────────┐
│  src/msx/msx_core.c/h   the MSX system itself (pure C99)      │
│    - slot/memory management, I/O ports                        │
│    - the frame loop (run_frame)                                │
│    - save state                                                │
│    - LCD init and DMA transfer                                 │
├─────────────────────────────────────────────┤
│  src/msx/z80/          Z80 CPU core (superzazu/z80)            │
│  src/msx/tms9918/      VDP core (vrEmuTms9918)                 │
│  src/msx/emu2149/      PSG core (emu2149)                      │
└─────────────────────────────────────────────┘
```

---

## C Core (`src/msx/msx_core.c` / `.h`)

### Emulated Hardware

| Component | Implementation | Notes |
| :--- | :--- | :--- |
| CPU | Z80 @ 3.579545MHz | Uses [superzazu/z80](https://github.com/superzazu/z80) (MIT) |
| VDP | TMS9918A | Uses [vrEmuTms9918](https://github.com/visrealm/vrEmuTms9918) (MIT). Renders 256×192, 16-color palette |
| PSG | AY-3-8910 equivalent | Uses [emu2149](https://github.com/digital-sound-antiques/emu2149) (MIT). Clocked at half the Z80 clock (1.789772MHz), matching real MSX hardware |
| PPI | Simplified i8255 | Slot select, keyboard matrix readback |
| Memory | 4 slots × 4 pages (16KB each) | Slot 0 = BIOS, 1/2 = cartridge, 3 = RAM |

### Memory Management and Cartridge Mappers

`msx_state_t` (`msx_core.h`) is the single struct holding all emulation state. Cartridges can be loaded two ways:

1. **Full in-RAM load** (`msx_load_cart()`) — the whole ROM is `malloc()`'d and copied into the heap. For non-bank-switched cartridges up to 32KB.
2. **Paged mode** (`msx_load_cart_paged()`) — for large bank-switched cartridges (a.k.a. Mega ROM, 128KB-1MB). The whole ROM is never loaded into RAM; only **the 4 currently bank-switched-in 8KB windows (a fixed 32KB)** are cached. A bank-switch write triggers a fetch of the needed 8KB page via a caller-registered callback (`msx_cart_fetch_fn`). The C core itself is agnostic to where the data actually comes from (SD card, onboard flash, etc.).

Supported mappers: `MSX_MAPPER_PLAIN` (no banking), `MSX_MAPPER_ASCII8`, `MSX_MAPPER_ASCII16`, `MSX_MAPPER_KONAMI` (no SCC). A simple heuristic (`detect_mapper()`) scans the ROM for known bank-select write patterns to auto-detect the mapper.

See `memory_map_en.md` for the full address map.

### Frame Loop

`msx_run_frame()` runs one full frame's worth (59659 T-states, 60fps) of Z80 execution, VDP scanline rendering, and PSG sample generation in one call. The framebuffer is double-buffered (`framebuf[2]`), so a display DMA transfer can run concurrently with the next frame's emulation (see the pipelining discussion below).

### Display Output

`msx_render_to_display_1to1()` DMA-transfers MSX's native 256×192 resolution centered on the panel, unscaled — this is the production render path. A 1.5x-scaled path (`msx_render_to_display()`) still exists in the code but is currently unused, since the extra transfer volume roughly doubled frame time and made gameplay feel like slow motion.

---

## MicroPython Bridge Layer (`src/msx/modmsx.c`)

Exposes `msx_core.c`'s functionality to Python as a C user module named `msx`. Its main responsibilities:

- Holding the single global `static msx_state_t msx_state` instance (one MSX system per Pico)
- Marshalling between Python `bytes`/`bytearray`/file objects and C-side buffers (zero-copy is the default policy — see below)
- PWM audio output (`hardware/pwm.h`; audio samples generated per-frame into a ring buffer are consumed by a 22050Hz timer interrupt that updates the PWM duty cycle)
- The Mega ROM paging fetch callback (`cart_fetch_from_pyfile()`) — a Python-opened file object is held as a MicroPython GC root pointer (`MP_REGISTER_ROOT_POINTER`) and read directly via `mp_stream_seek()`/`mp_stream_read_exactly()`
- A set of debug APIs (`debug_step`/`debug_cpu`/`debug_peek`/`debug_psg`, etc.)

See `extension_api_en.md` for the complete function list.

### Committing to Zero-Copy

MicroPython's GC heap is **non-compacting** — even when the *total* free space is plenty, a single large contiguous allocation can fail (`MemoryError`) with high probability once the heap has any scattered live objects. Learning this the hard way drove a strict zero-copy policy (referencing existing C-side memory directly, e.g. via `bytearray_by_ref`, instead of allocating and copying):

- `msx.get_ram_view()` — a direct view of `msx_state.ram` (64KB)
- `msx.get_vram_view()` — a direct view of the VDP core's internal VRAM (16KB)
- `msx.get_framebuf()` — a direct view of the display framebuffer
- Save states — a tiny 64-byte header (CPU/VDP registers, etc.) is read/written separately from RAM/VRAM, so no 80KB+ temporary buffer is ever allocated

---

## Python Layer (`mp/`)

### `main.py` — Boot Flow and Main Loop

Responsibilities:

- Hardware init (clock tuning, UART, SD mount, LCD init, USB host, PWM audio)
- Reading and applying `config.txt` (BIOS/cart paths, LCD panel model, rotation, volume/filter)
- Loading the BIOS and cartridge (delegated to `msx_menu.load_cart_smart()`)
- Polling the USB keyboard and joystick
- The main loop (below)

The main loop uses **pipelining**: frame N's display DMA transfer runs concurrently with frame N+1's Z80/VDP emulation (safe thanks to the double-buffered framebuffer). This measured roughly a 1.5x FPS improvement over naive sequential execution.

> **Note**: if Mega ROM bank-switch fetches were to read from the SD card (which shares the SPI1 bus with the LCD), they would collide with this pipelining and cause `I/O error`s during an in-flight display DMA transfer. The current implementation avoids this entirely by only ever fetching Mega ROM pages from the Pico 2's onboard flash (a separate QSPI bus, independent of SPI1), keeping pipelining safe. See the Mega ROM section in `usage_guide_en.md` for details.

### `msx_menu.py` — ROM Selection, Runtime Menu, Cart Loading

- `select_rom()` — a UI for picking a `.ROM` file from the SD card (keyboard-driven, with a timeout-based auto-select)
- `load_cart_smart()` — automatically chooses full in-RAM loading vs. Mega ROM paging based on file size. For Mega ROMs, the file is copied once from SD to the Pico's onboard flash (`/megarom_cache.rom`) and read from there thereafter
- `save_state_to()` / `load_state_from()` — the zero-copy save-state implementation
- `show_emulator_menu()` — the GUI+F7 runtime menu (swap cartridge, save/load state, adjust volume/filter, reset)
- `load_config()` / `save_config()` — reading and writing `config.txt`

### `msx_keymap.py` — Keyboard Mapping

Converts USB HID keycodes (a modifier byte plus up to 6 simultaneous keys) into MSX's keyboard matrix (11 rows × 8 bits, active-low). As on real hardware, which character a given key actually produces depends on the BIOS ROM's regional layout (Japanese vs. English).

---

## Dependency Direction

```text
main.py
  -> msx (C module, import msx)
  -> msx_keymap.py
  -> msx_menu.py

msx_menu.py
  -> msx (C module)
  -> uos / framebuf (MicroPython standard library)

msx (modmsx.c)
  -> msx_core.c/h
  -> hardware/pwm.h, hardware/spi.h, etc. (Pico SDK, guarded by #ifdef __arm__)

msx_core.c
  -> z80/z80.c   (Z80 core)
  -> tms9918/vrEmuTms9918.c  (VDP core)
  -> emu2149/emu2149.c       (PSG core)
```

---

## Boot Sequence (Runtime Flow)

1. `main.py`'s `run()` is invoked (typically `import main; main.run()` from `boot.py` during development, or auto-started as `main.py` in production)
2. `msx.init()` — zero-initializes the C-side state, registers the fetch callback
3. `msx.boost_peri_clock()` — makes `clk_peri` track `clk_sys`, lifting the SPI/UART baud-rate ceiling
4. USB host init (`usb_host.init()` — this resets `clk_sys` to 240MHz, so `boost_peri_clock()` must be called again afterward)
5. Mount the SD card
6. Read `config.txt`
7. Initialize the LCD (`msx.init_display_hardware()`, applying panel model/rotation settings)
8. Load the BIOS ROM (`msx.load_bios()`)
9. Load the cartridge (`config.txt`'s `cart=` if set → otherwise an interactive selector menu → otherwise boot straight to MSX BASIC)
10. Set up PWM audio and apply volume/filter settings
11. `msx.reset()` resets the Z80, and the main loop begins
12. Main loop: start display DMA → compute the next frame (pipelined) → poll keyboard/joystick → wait for DMA to finish — repeat forever

---

## Future Extension Points

- Auto-boot deployment as `main.py` (currently run manually via `import main; main.run()` for testing)
- Real-hardware verification of multi-slot cartridges (cart slot 2) and the ASCII-8/ASCII-16 mappers (only the KONAMI mapper has been verified on real hardware so far)
- Support for KONAMI SCC (the extra sound chip found in some Konami cartridges) — currently only the SCC-less KONAMI4-equivalent mapper is implemented
