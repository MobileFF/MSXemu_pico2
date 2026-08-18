# Development Guide

## 1. System Architecture

See `architecture_en.md` for the full design. Key points:

- Performance-critical parts (Z80/VDP/PSG emulation) are implemented in C, in `msx_core.c`/`.h`, and exposed as a MicroPython C user module named `msx` (`modmsx.c`).
- The boot flow, UI, and configuration are implemented in MicroPython (`mp/*.py`).
- The C core isolates Pico-SDK-dependent code behind `#ifdef __arm__`, which means **it can be built and verified with native gcc, without hardware** (see below).

---

## 2. Directory Layout

```
MSX_emu_pico2/
├── bldfrm_msx.sh              # Build script
├── firmware/                  # firmware_msx.uf2 auto-copied here on a successful build
├── doc/                       # This documentation
├── src/msx/
│   ├── msx_core.h             # msx_state_t and the public API declarations
│   ├── msx_core.c             # The MSX system itself (Z80/VDP/PSG integration, memory management, frame loop, display DMA)
│   ├── modmsx.c                # The MicroPython C user module (`msx`)
│   ├── micropython_msx.cmake   # CMake build configuration
│   ├── z80/                    # superzazu/z80 (MIT) — Z80 CPU core
│   ├── tms9918/                 # visrealm/vrEmuTms9918 (MIT) — TMS9918A VDP core
│   └── emu2149/                 # digital-sound-antiques/emu2149 (MIT) — AY-3-8910 PSG core
└── mp/
    ├── main.py              # Boot flow and main loop
    ├── msx_menu.py              # ROM selection, runtime menu, save state, cart loading
    └── msx_keymap.py            # USB HID -> MSX key-matrix mapping
```

> This repository also contains the original PB-1000 emulator's C sources (`src/*.c`), which are independent of the MSX emulator and out of scope for this guide. Only the USB host core (`src/usb_host_core.c`, `src/modusb_host.c`) is shared between the two. PB-1000's Python sources are source-controlled in a separate project, so `mp/` in this repository now contains only MSX emulator files.

---

## 3. C Module Details

### The `msx_core` module (`src/msx/msx_core.c` / `.h`)

This is the MSX system itself, designed to avoid any Pico SDK dependency, and is compiled two ways:

- **Real-hardware build** (inside `#ifdef __arm__`): functions that actually touch GPIO/SPI/DMA peripherals — LCD init, DMA transfer, clock configuration, etc.
- **Host build** (the `#else` branch): stub implementations (no-ops) of the above, for logic-only verification.

Public API, roughly grouped:

| Category | Example functions |
| :--- | :--- |
| Init | `msx_init()`, `msx_load_bios()`, `msx_reset()`, `msx_destroy()` |
| Cartridge | `msx_load_cart()`, `msx_load_cart_paged()`, `msx_set_cart_fetch_cb()`, `msx_detect_mapper()`, `msx_eject_cart()` |
| Execution | `msx_run_frame()` |
| I/O | `msx_mem_read()`/`msx_mem_write()`, `msx_port_read()`/`msx_port_write()`, `msx_set_key_matrix()`, `msx_set_joystick()` |
| Display | `msx_init_display_hardware()`, `msx_render_to_display_1to1()`, `msx_render_to_display()`, `msx_wait_display()` |
| Clock tuning | `msx_boost_peri_clock()`, `msx_boost_dma_priority()` |
| Save state | `msx_save_state_header()`, `msx_load_state_header()`, `msx_get_vram_ptr()` |
| Debug | `msx_debug_step()`, `msx_debug_get_cpu()`, `msx_debug_peek()`, `msx_debug_get_psg()`, etc. |

### The `msx` module (`src/msx/modmsx.c`)

The bridge that makes `msx_core`'s functionality callable from Python. See `extension_api_en.md` for the complete function list and signatures.

Implementation notes:

- Holds a single global `static msx_state_t msx_state;` (the design assumes one MSX system per Pico).
- Python buffers (`bytes`/`bytearray`) are passed straight through to the C core as a pointer obtained via `mp_get_buffer_raise()` (never copied).
- Large data (RAM/VRAM/framebuffer) handed back to Python uses `mp_obj_new_bytearray_by_ref()` for a zero-copy reference view.
- For Mega ROM page fetches, a Python-opened file object is held as a MicroPython GC root pointer via `MP_REGISTER_ROOT_POINTER` and read directly from C using `mp_stream_seek()`/`mp_stream_read_exactly()` (no Python method-call overhead; errors come back as an errcode, not an exception).

---

## 4. Verifying Logic with a Host Build

Flashing and connecting to real hardware is slow, so this project's standard workflow is: **verify logic correctness with native gcc first, before ever touching hardware.**

```bash
mkdir -p /tmp/msx_hosttest && cd /tmp/msx_hosttest
cp -r <repo>/src/msx ./msx
gcc -I. -o test_boot my_test.c msx/msx_core.c msx/z80/z80.c \
    msx/tms9918/vrEmuTms9918.c msx/emu2149/emu2149.c -lm -Wall
./test_boot
```

Because `msx_core.h`/`.c` strip out Pico SDK header dependencies via `#ifdef __arm__`, this compiles as-is. `modmsx.c` depends on MicroPython headers (`py/runtime.h`, etc.) and is not part of the host build.

A typical test `.c` file drives `msx_init()` -> `msx_load_bios()` -> `msx_reset()` -> a loop of `msx_run_frame()`, then inspects `msx.cpu.pc` or VRAM contents (`vrEmuTms9918VramValue()`) directly. Bank switching and Mega ROM page-cache behavior can be verified the same way, using a fake `msx_cart_fetch_fn` callback that just reads from an in-memory array.

---

## 5. Adding a New Mapper Type

1. Add a new `MSX_MAPPER_*` constant in `msx_core.h`.
2. Add that mapper's window-to-bank-number mapping logic to `cart_page_ptr()` (the read path) in `msx_core.c`.
3. Add that mapper's bank-switch address detection to `cart_mapper_write()`. If you also want paged (Mega ROM) support, add the corresponding `cart_page_refill()` calls.
4. If useful, add a detection pattern to `detect_mapper()`'s heuristic (`scan_mapper_patterns()`).
5. Register the new mapper constant in `modmsx.c`'s `MP_QSTR_MAPPER_*` bindings.
6. Verify with a host build that reads/writes land at the correct addresses before testing on real hardware.

---

## 6. Adding a New `msx.*` Python API

1. Implement a `static mp_obj_t msx_py_xxx(...)` function in `modmsx.c`.
2. Wrap it with `MP_DEFINE_CONST_FUN_OBJ_0` through `_3` depending on argument count, or `MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN` for variable/4+ arguments (fixed-arity macros only go up to 3 arguments).
3. Add `{ MP_ROM_QSTR(MP_QSTR_xxx), MP_ROM_PTR(&msx_py_xxx_obj) }` to `msx_module_globals_table[]`.
4. Wrap Pico-SDK-dependent implementations in `#if HAVE_PICO_SDK`.
5. Document it in `extension_api_en.md`.

---

## 7. Adding a New `config.txt` Key

`mp/msx_menu.py`'s `load_config()` is a simple `key=value` parser; the meaning of each key is up to the caller (`main.py`). To add a new key:

1. In `main.py`'s `run()`, after `cfg = load_config(CONFIG_PATH)`, add `cfg.get('new_key', default_value)` where appropriate.
2. If it should also be adjustable from the runtime menu, wire it up to write back via `msx_menu.py`'s `save_config()` (which preserves existing keys while updating/appending).

> **Note**: MicroPython's file object has no `writelines()` (raises `AttributeError`). When rewriting a config file, build the full content as one string via `"".join(lines)` and write it with a single `f.write()` call — this also avoids corrupting the file via a partial write on failure.

---

## 8. Debugging Techniques

### Inspecting slot_select / memory contents on real hardware

Reading specific byte offsets (see `memory_map_en.md`) out of `msx.get_state_header()`'s 64-byte return value lets you snapshot CPU/slot-select state without stopping execution. To force a particular slot mapping, modify the relevant byte and write it back with `msx.set_state_header()`, then read through it with `msx.debug_peek()` to inspect memory under that mapping directly.

### Measuring FPS

`main.py`'s main loop prints the measured FPS and audio ring-buffer level every 300 frames. A useful real-hardware workflow: launch `main.run()` in a background thread via `mpremote exec`, wait a while, then check the output.

### Common Debugging Pitfalls

- **If a background thread already has the SD card mounted, calling `uos.mount()`/`uos.umount()` from a separate one-shot script can accidentally unmount the running thread's SD card too.** Before writing a diagnostic script, check whether `sd` already appears in `uos.listdir('/')`, and skip remounting if it does.
- **`machine.reset()` does not zero SRAM contents.** Uninitialized heap regions can retain data from the previous run. If data you expect to be freshly initialized looks stale, suspect the initialization ordering (e.g. is a callback registered *after* the data it should have gated is already fetched?).

---

## 9. Coding Conventions and Gotchas

- Avoid allocating large temporary buffers (tens of KB or more); prefer a zero-copy reference into existing memory (RAM/VRAM/framebuffer) instead. MicroPython's GC heap is non-compacting, so a large contiguous allocation can fail even with plenty of nominal free space.
- When adding a new static buffer to the C heap or `.bss`, check `gc.mem_free()` on real hardware to confirm it isn't squeezing the GC heap too hard.
- Initializing a Pico SDK subsystem (like USB host) can implicitly change clock settings (`clk_sys`/`clk_peri`, etc.). Make it a habit to verify with `msx.debug_clocks()`.
