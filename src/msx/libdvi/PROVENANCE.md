# libdvi provenance

Vendored 2026-09-20 (phase 3 of the RP2350-PiZero port, see
`調査用/RP2350-PiZero_HDMI出力適用調査.md` §7) from:

- **Upstream**: [Wren6991/PicoDVI](https://github.com/Wren6991/PicoDVI)
  (Luke Wren, BSD-3-Clause) — the canonical PIO-bitbanged DVI library for
  RP2040/RP2350.
- **Actual source copied**: [SvenMb/PicoDVI_rp2350](https://github.com/SvenMb/PicoDVI_rp2350),
  a fork of the above "adapted for the Waveshare RP2350 PiZero" — carries
  the same BSD-3-Clause license (see `LICENSE` in this directory) plus the
  RP2350-specific GPIO>31 support (`PICO_PIO_USE_GPIO_BASE`,
  `pio_sm_set_pins_with_mask64()`/`pio_sm_set_pindirs_with_mask64()` in
  `dvi_serialiser.c`) that this board's DVI pins (GPIO32-39) need and that
  upstream had not yet merged as of this snapshot.

Files copied verbatim (unmodified) from `software/libdvi/`: `dvi.c/h`,
`dvi_config_defs.h`, `dvi_serialiser.c/h/.pio`, `dvi_timing.c/h`,
`tmds_encode.c/h/.S`, `tmds_table.h`, `tmds_table_fullres.h`,
`util_queue_u32_inline.h`.

Not copied: the fork's own `common_dvi_pin_configs.h` (this project
defines its own `waveshare_rp2350_pizero_dvi_cfg` directly in
`src/msx/display/disp_dvi.c` instead of depending on upstream's macro-
selection mechanism), example apps, and the fork's own CMakeLists.txt
(this project's `src/msx/micropython_msx.cmake` integrates the sources
directly, matching how `msx_lib` already vendors z80/vrEmuTms9918/
emu2149 as an INTERFACE library rather than a nested subdirectory build).

Confirmed working reference (GPL-3.0, NOT used as a code source — see
below): [ugufru/xroar-waveshare-rp2350-pizero](https://github.com/ugufru/xroar-waveshare-rp2350-pizero)
independently validates this exact library + pin config on this exact
board at a locked 640x480p60. Its GPL-3.0 license is incompatible with
this project's MIT license, so **no code or close derivation from that
repo was copied** — it was used only to confirm publicly-verifiable
hardware facts (GPIO pin numbers, memory-budget order of magnitude) that
are not themselves copyrightable expression. `disp_dvi.c`'s actual driver
logic (the core1 launch, scanline_callback-as-producer design) was
written independently against libdvi's own public `dvi.h` API and the
tiny BSD-3-Clause `apps/hello_dvi/main.c` example in the forks above.
