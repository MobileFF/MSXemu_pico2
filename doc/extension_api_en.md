# `msx` Module Python API Reference

## Overview

`msx` is the MicroPython C user module implemented in `src/msx/modmsx.c` — the sole entry point for controlling the MSX1 emulator's core (`msx_core.c`) from Python.

```python
import msx

msx.init()
with open('/sd/msx/MSX.ROM', 'rb') as f:
    msx.load_bios(f.read())
msx.load_cart(0, cart_bytes)
msx.init_display_hardware(1, 62_500_000, 11, 10, 9, 8, 7, 22, 480, 320)
msx.reset()

while True:
    msx.render_to_display_1to1()
    msx.run_frame()
    msx.wait_display()
```

Emulator state lives as a single global instance inside the module (one MSX system per Pico). Every function operates on that implicit instance.

---

## Lifecycle

### `msx.init()`

Initializes (zeroes) the emulator state. Call once, before any other function. Safe to call again later (it automatically frees any previously allocated heap memory and closes any still-open Mega ROM file before re-initializing).

### `msx.reset()`

Resets the Z80 CPU (execution starts from PC=0). Call after loading the BIOS/cartridge.

### `msx.is_ready() -> bool`

Returns whether `msx.init()` has been called.

---

## ROM Management

### `msx.load_bios(data: bytes | bytearray) -> bool`

Loads a 32KB MSX BIOS/BASIC ROM into slot 0. `data`'s size must be within 0x4000-0x8000.

### `msx.load_cart(slot: int, data: bytes | bytearray, mapper: int = 0) -> bool`

Loads a cartridge ROM by copying the whole thing into the heap (full in-RAM mode; for small cartridges, up to roughly 32KB).

- `slot`: 0 = cartridge slot 1, 1 = cartridge slot 2
- `mapper`: `msx.MAPPER_PLAIN` (0, default — auto-detected if the ROM is over 32KB) / `msx.MAPPER_ASCII8` (1) / `msx.MAPPER_ASCII16` (2) / `msx.MAPPER_KONAMI` (3)

### `msx.load_cart_paged(slot: int, file_obj, size: int, mapper: int) -> bool`

Loads a large cartridge (Mega ROM, roughly 128KB-1MB) in paged mode. The whole ROM is never loaded into RAM; each bank-switch triggers a read of the relevant 8KB page from `file_obj`.

- `file_obj`: an already-open (`open(path, 'rb')`), seekable file object. **This module keeps its own reference to it — the caller must not close it** (`msx.eject_cart()` closes it automatically).
- `mapper`: `msx.MAPPER_PLAIN` is rejected (paging assumes bank switching). The typical pattern is to determine it beforehand with `msx.detect_mapper()`.
- In practice, use `mp/msx_menu.py`'s `load_cart_smart()` instead (it auto-selects the loading strategy by size and handles the onboard-flash cache copy).

### `msx.detect_mapper(data: bytes | bytearray) -> int`

Scans the given byte buffer for known bank-select write patterns (`LD (nn),A` instructions targeting specific address ranges) to guess the mapper type. This is the same heuristic `msx.load_cart()` uses internally for ROMs over 32KB, but it can also be called on just a prefix of a ROM (a few KB is usually enough) ahead of `msx.load_cart_paged()`. Unlike `msx.load_cart()`'s internal use, this does **not** apply the "≤32KB means PLAIN" size shortcut, regardless of how much data you pass in.

Returns: one of `msx.MAPPER_*`.

### `msx.eject_cart(slot: int)`

Ejects the cartridge in the given slot, freeing any allocated memory and file reference, in either full-RAM or paged mode.

### `msx.is_cart_paged(slot: int) -> bool`

Returns whether the cartridge in the given slot was loaded in paged (Mega ROM) mode.

### `msx.get_cart_fetch_stats() -> (bankswitch_count: int, fetch_count: int)`

Returns cumulative counters since boot. `bankswitch_count` is every write to a paged cart's bank-select register (hit or miss); `fetch_count` is the subset that missed the cache and triggered a synchronous read from onboard flash. A low-cost diagnostic for investigating Mega ROM bank-switch frequency / cache hit rate on real hardware (always counted internally; free unless read).

---

## Execution

### `msx.run_frame() -> int`

Runs one full frame's worth (~59659 T-states, 60fps) of Z80 execution, VDP scanline rendering, and PSG sample generation in a single call. Generated audio samples are automatically pushed into the internal ring buffer. Returns the number of samples produced this frame.

---

## Clock Tuning

### `msx.boost_peri_clock()`

Makes `clk_peri` (the SPI/UART baud-rate source) track `clk_sys`. This board defaults `clk_peri` to a fixed 48MHz, which severely caps the effective SPI baud rate — this call works around that. **After calling it, `machine.UART` objects must be re-created** (their baud divisor needs recomputing). `usb_host.init()` also resets `clk_peri`, so call this again afterward too.

### `msx.boost_dma_priority()`

Gives DMA the highest bus-fabric priority. Originally added as a suspected fix for display-DMA slowdown while USB host was active, but measured to have no actual effect (the real cause was `usb_host.init()` resetting the clocks). Left in place since it's harmless.

---

## Input

### `msx.set_key_matrix(row: int, col_mask: int)`

Sets one row of the keyboard matrix. `col_mask` is active-low (0=pressed, 1=released). Usually called via `msx_keymap.py`'s `apply_hid_report()`.

### `msx.clear_keys()`

Resets the entire keyboard matrix to "nothing pressed."

### `msx.set_joystick(port: int, state: int)`

Sets joystick state. `port`: 0=JOY1, 1=JOY2. `state` is an active-low bitmask: bit0=Up, bit1=Down, bit2=Left, bit3=Right, bit4=TriggerA, bit5=TriggerB (0=pressed). Meant to be called once per frame with the latest GPIO poll result.

---

## Display

### `msx.init_display_hardware(spi_id, baud, mosi, sck, cs, dc, rst, bl, lcd_w=480, lcd_h=320, rotate_180=False)`

Fully initializes the LCD (ST7796/ILI9341 — identical init sequence for both) from C: GPIO/SPI setup, reset pulse, init command sequence, black-screen clear, and backlight on.

- `spi_id`: 0=spi0, 1=spi1
- `baud`: SPI clock in Hz. 62.5MHz is the confirmed-safe upper bound.
- `mosi`/`sck`/`cs`/`dc`/`rst`/`bl`: the respective GPIO pin numbers
- `lcd_w`/`lcd_h`: panel size (defaults to 480×320 = ST7796). MSX's native 256×192 screen is rendered 1:1, centered on this size.
- `rotate_180`: `True` flips the display 180° (a MADCTL register change only — coordinate math is unaffected)

### `msx.setup_display(spi_id: int, cs_pin: int, dc_pin: int)`

A lightweight variant: use when SPI is already initialized by a Python display driver — just records the hardware handles for `render_to_display()`. Not needed if you use `init_display_hardware()`.

### `msx.render_to_display_1to1()`

Starts a (non-blocking) DMA transfer of MSX's native 256×192 resolution, centered on the panel, unscaled. This is the current production render path.

### `msx.render_to_display()`

The 1.5x-scaled variant (nearest-neighbor scaled to 384×288 before DMA transfer). Currently unused (the extra transfer volume slows things down), but kept as part of the API.

### `msx.wait_display()`

Blocks until the DMA transfer started by the last `render_to_display*()` finishes. Always call this before the next `render_to_display*()` or before touching the SD card (they share the SPI bus).

### `msx.get_framebuf() -> memoryview`

Returns a read-only, zero-copy reference view of the internal framebuffer (RGB565, 256×192×2 bytes, big-endian). Used by e.g. the runtime menu to draw directly via the `framebuf` module.

---

## HDMI Bridge Output

An optional feature that sends frames to a second Pico 2 + PICO-HDMI-PLUS (see `hdmi_bridge/README.md`) over the same SPI1 bus as the LCD/SD (a separate CS pin). Enabled via `hdmi=1` in `config.txt` (see `mp/main.py`).

### `msx.init_hdmi_output(cs_pin: int, baudrate: int)`

Configures the HDMI bridge output. Must be called **after** `init_display_hardware()` (it reuses that same SPI1 instance). `cs_pin` is a dedicated chip-select GPIO separate from the LCD/SD (GP28). Sends the current 16-color palette once as part of this call (see `send_hdmi_palette()`). All other HDMI functions are no-ops until this has been called.

### `msx.send_hdmi_palette()`

Sends the current 16-color palette (converted to RGB332) to the HDMI bridge. Already called once by `init_hdmi_output()`, so this normally doesn't need to be called manually (MSX1's palette is fixed hardware and never changes).

### `msx.render_to_hdmi()`

Converts the framebuffer to 4-bit palette indices (2 pixels/byte) and sends it to the HDMI bridge (blocking). Only call while the LCD/SD SPI1 bus is idle (after `wait_display()`, not concurrently with SD access). Only correct for the game screen, which is guaranteed to use only the 16 palette colors — use `render_to_hdmi_raw332()` for menu/UI screens.

### `msx.render_to_hdmi_raw332()`

Same as `render_to_hdmi()`, but sends full RGB332 (1 byte/pixel, no palette lookup). For menu/UI screens (`msx_menu.py`'s `MenuCanvas`), which use colors outside the MSX's 16-color hardware palette. Twice the bytes of `render_to_hdmi()`.

### `msx.clear_hdmi()`

Sends one all-black frame to the HDMI bridge, independent of the current framebuffer content. Intended to be called once right after `init_hdmi_output()`, to blank whatever frame the receiver was previously displaying — including one left over from a different emulator/session — before this emulator's own first real frame is sent.

---

## Audio

### `msx.setup_audio_pwm(pin: int) -> bool`

Sets up PWM audio output on the given GPIO pin. 10-bit PWM (wrap=1023), a fixed ~234kHz carrier (derived from `clk_sys`), and a 22050Hz repeating-timer interrupt that consumes samples and updates the PWM duty cycle.

### `msx.set_audio_volume(level: int)` / `msx.get_audio_volume() -> int`

Sets/gets the volume. `level` ranges 0-256 (256 = default, the original full-scale amplitude). Takes effect immediately.

### `msx.set_audio_filter(shift: int)` / `msx.get_audio_filter() -> int`

Sets/gets the strength of a one-pole IIR low-pass filter applied to PSG samples. `shift`: 0 (default, disabled) to 15 — higher values give heavier, more muffled smoothing (cutoff frequency ≈ `(22050 >> shift) / (2π)` Hz). Intended to reduce a passive buzzer's harsh high-frequency content. Note this cannot remove the PWM carrier itself (~234kHz) — that requires a hardware filter.

### `msx.get_audio_ring_level() -> int`

Returns the number of samples currently queued in the audio ring buffer — useful for diagnosing lag.

### `msx.get_audio_buf(n: int) -> memoryview`

Returns a zero-copy view of this frame's first `n` audio samples (int16).

---

## Save State

### `msx.get_ram_view() -> memoryview`

Returns a zero-copy, readable/writable view of MSX RAM (64KB) — a direct reference to `msx_state.ram`.

### `msx.get_vram_view() -> memoryview`

Returns a zero-copy, readable/writable view of VDP VRAM (16KB).

### `msx.get_state_header() -> bytearray` / `msx.set_state_header(data: bytes) -> bool`

Gets/sets the fixed 64-byte header bundling CPU registers, VDP registers, mapper banks, and I/O state. See `memory_map_en.md` for the exact field layout. RAM/VRAM are *not* part of this header — read/write them separately via `get_ram_view()`/`get_vram_view()`.

**Example save-state implementation** (see `save_state_to()`/`load_state_from()` in `mp/msx_menu.py`):

```python
def save_state_to(path):
    with open(path, 'wb') as f:
        f.write(msx.get_state_header())
        f.write(msx.get_ram_view())
        f.write(msx.get_vram_view())

def load_state_from(path):
    with open(path, 'rb') as f:
        msx.set_state_header(f.read(64))
        f.readinto(msx.get_ram_view())
        f.readinto(msx.get_vram_view())
```

The key design point is that this never allocates a large temporary buffer (see `dev_guide_en.md`).

---

## Debug API

None of these are needed for normal gameplay, but they're valuable for logic verification and real-hardware troubleshooting.

### `msx.get_vdp_reg(reg: int) -> int`

Reads a VDP register (R0-R7).

### `msx.debug_step(n: int) -> int`

Executes `n` raw Z80 instructions with no VDP/audio/frame timing considered at all. Returns the resulting PC.

### `msx.debug_cpu() -> tuple`

Returns `(pc, sp, a, f, cyc, halted, iff1, int_mode)`. `cyc` is the T-state count elapsed in the current frame.

### `msx.debug_peek(addr: int) -> int`

Reads one byte of the Z80 address space as currently mapped by `slot_select` (i.e. goes through slot mapping).

### `msx.debug_run_line(line: int, do_video: bool, do_audio: bool, do_int: bool)`

Runs one scanline's worth of processing, with video/audio/interrupt generation individually toggleable — useful for isolating which subsystem is causing a hang.

### `msx.debug_spi_baud() -> int`

Returns the actual SPI baud rate (Hz) achieved by the last `render_to_display*()` call.

### `msx.debug_clocks() -> (clk_sys_hz, clk_peri_hz)`

Returns the measured current `clk_sys`/`clk_peri` values — useful for detecting a clock reset caused by e.g. USB host init.

### `msx.debug_psg() -> tuple`

Returns `(quality, clk, rate, base_incr, realstep, psgtime, psgstep, freq_limit, env_ptr, env_pause, env_continue, env_face, env_freq, env_count, freq0, count0)` — the emu2149 core's internal state.

### `msx.debug_psg_calc(n: int)`

Calls `PSG_calc()` directly `n` times (for verifying audio-generation timing).

---

## Module Constants

| Constant | Value | Meaning |
| :--- | :--- | :--- |
| `msx.MAPPER_PLAIN` | 0 | No bank switching |
| `msx.MAPPER_ASCII8` | 1 | ASCII-8 (8KB pages, 4 windows) |
| `msx.MAPPER_ASCII16` | 2 | ASCII-16 (16KB pages, 2 windows) |
| `msx.MAPPER_KONAMI` | 3 | KONAMI (no SCC; 8KB pages, 3 switchable windows) |
| `msx.SCREEN_W` | 256 | MSX native screen width |
| `msx.SCREEN_H` | 192 | MSX native screen height |
| `msx.KEY_ROWS` | 11 | Number of keyboard matrix rows |
| `msx.AUDIO_RATE` | 22050 | Audio sample rate (Hz) |
| `msx.RAM_SIZE` | 65536 | RAM size (bytes) |
| `msx.VRAM_SIZE` | 16384 | VRAM size (bytes) |

---

## Related Source Files

- `src/msx/modmsx.c` — this API's implementation
- `src/msx/msx_core.h` — the corresponding C API declarations and comments
- `mp/main.py` / `mp/msx_menu.py` — real usage examples
