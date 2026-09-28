# pio_usb provenance

Vendored 2026-09-27 (Phase 4 of the RP2350-PiZero port, see
`調査用/RP2350-PiZero_USBキーボード対応調査.md`) from:

- **Upstream**: [sekigon-gonnoc/Pico-PIO-USB](https://github.com/sekigon-gonnoc/Pico-PIO-USB),
  MIT License (see `LICENSE` in this directory).
- **Exact version vendored**: tag `0.7.2`, commit
  `3c1eec341a5232640e4c00628b889b641af34b28`.

Files copied verbatim (unmodified) from the tag's `src/`: `pio_usb.c/h`,
`pio_usb_device.c`, `pio_usb_host.c`, `pio_usb_ll.h`,
`pio_usb_configuration.h`, `sdk_compat.h`, `usb_crc.c/h`,
`usb_definitions.h`, `usb_tx.pio(.h)`, `usb_rx.pio(.h)` — the `.pio.h`
files are upstream's own pre-generated (pioasm) output, already committed
in their repo; not regenerated here (same rationale as
`src/msx/libdvi/dvi_serialiser.pio.h` — see that file's own comment for
why regenerating PIO headers at MicroPython build time doesn't work with
this project's QSTR pre-scan step).

Not copied: `examples/`, the library's own `CMakeLists.txt`
(`src/msx/micropython_msx.cmake` integrates the sources directly, same
pattern as `libdvi` and `msx_lib`'s other vendored third-party code).
`pio_usb_device.c` is vendored (kept in this directory, matching
upstream's full checkout) but deliberately **not compiled**
(`micropython_msx.cmake` excludes it) — this project only ever uses
pio_usb as a host; nothing else here calls into it.

## Deliberate deviations from vendored-verbatim

Unlike libdvi (copied byte-for-byte), one constant was hand-edited after
vendoring, for a real-hardware reason:

- `pio_usb_configuration.h`: `PIO_USB_EP_POOL_CNT` reduced from upstream's
  default `32` to `8` — see that file's own comment. This project only
  ever has a single HID keyboard on this port; 32 endpoint slots
  (`endpoint_t pio_usb_ep_pool[32]`, ~6KB of `.bss`) was measured on real
  hardware to push the GC heap below MicroPython's required 64KB minimum
  once combined with everything else already competing for RP2350's
  512KB SRAM (onboard DVI's TMDS buffers, the Mega ROM cache, etc.) — a
  link-time `assert(GcHeap is too small)` failure, not a guess.

## Why this exists

Waveshare RP2350-PiZero's USB-host port (GPIO28/29) is wired as a PIO-USB
port, not to RP2350's native USB host controller that this project's
existing `usb_host_core.c`/`hcd_rp2040.c`-based stack (used by Pico 2)
assumes — see `board_config.py`'s `USB_HOST_DP_PIN` comment. This library
is the PIO-bitbanged USB host/device implementation that makes a
keyboard usable on that port at all.

## HCD glue layer

The TinyUSB-side glue (`hcd_init`/`hcd_edpt_*`/etc. bridging to
`pio_usb_host_*`) is **not** copied from this library's own upstream
example (`examples/host_hid_to_device_cdc.c`) nor from this project's
pre-existing `src/usb_host/hcd_pio_usb_custom.c` (a copy of TinyUSB's own
`hw/bsp/rp2040/family.c`-adjacent example glue that was tried early in
this project, never actually vendored/built against a real `pio_usb`
checkout, and abandoned in favor of the native RP2350 host controller for
Pico 2 — see `調査用/RP2350-PiZero_USBキーボード対応調査.md` §0's
2026-09-27 addendum). It was written fresh against this exact vendored
version's public API (`pio_usb.h`/`pio_usb_ll.h`), in
`src/usb_host/hcd_pio_usb_pizero.c`, and is expected to end up
structurally similar to that example simply because that's what a
correct TinyUSB↔pio_usb bridge looks like — not because it was copied.

## Board-specific facts this relies on

- `PIO_USB_DP_PIN` = `USB_HOST_DP_PIN` from `board_config.py` (28 on
  pizero; D- is always D+ + 1, i.e. GPIO29)
- Uses PIO1 exclusively (pio0 is fully committed to onboard DVI — see
  `src/msx/display/disp_dvi.c` — and this library needs a whole PIO block,
  3 of 4 state machines, to itself)
- Runs entirely on core0 alongside MicroPython, polled via a repeating
  timer (`usb_host_core_start_bg_timer()`, unchanged from the existing
  Pico 2 design) — **not** core1, which stays 100% DVI's. See the
  investigation doc §4 for why this is expected to work despite the
  library's own upstream examples conventionally dedicating core1 to it.
- `pio_usb_host_init()` computes its PIO clock dividers dynamically from
  whatever `clk_sys` already is at call time (`pio_usb_host.c`,
  `pio_calculate_clkdiv_from_float(clock_get_hz(clk_sys) / ...)`) — it
  does **not** hard-require `clk_sys` to be a clean multiple of 12MHz the
  way this project's own `usb_host_core_init()` comment (written for
  Pico 2's *native* USB controller) assumes. pizero therefore keeps
  `clk_sys` at DVI's required 252MHz and does not call
  `set_sys_clock_khz()` from USB init at all — unverified on real
  hardware until this is actually tested.
