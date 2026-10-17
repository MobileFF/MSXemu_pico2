# src/msx/manifest_freeze.py
#
# Freezes a hand-picked subset of mp/*.py into the firmware image (as
# compiled bytecode in flash), removing their GC-heap compile cost
# entirely. Complements modmsx_ui.c's C-port approach (doc/memory_usage.md
# §8) for files that are stable/low-change-frequency enough that losing
# the "edit .py + mpremote cp, no firmware rebuild" iteration speed is an
# acceptable trade — see doc/memory_usage.md §9 for the full rationale and
# per-file list.
#
# Wired in from src/msx/micropython_msx.cmake, which:
#   - sets $(MSX_MP_DIR) (via MICROPY_MANIFEST_MSX_MP_DIR) to this
#     project's mp/ directory, copied alongside src/ by bldfrm_msx.sh
#     specifically for this step (same "avoid the Google-Drive-mounted
#     path during the actual build" reasoning already applied to src/
#     itself — see that script's own header comment)
#   - only points MICROPY_FROZEN_MANIFEST at this file for the pico2
#     target (see that .cmake file's own comment for why pico2-only, for
#     now, and src/msx/boards/WAVESHARE_RP2350_PIZERO/mpconfigboard.cmake
#     for the prior pizero-only attempt this is deliberately NOT repeating
#     unmodified)
#
# $(PORT_DIR)/$(BOARD_DIR)/$(MPY_DIR)/$(MPY_LIB_DIR) are MicroPython's own
# built-in manifest substitutions (always available, regardless of what
# this file does).

# Keep the stock rp2-port defaults (asyncio, onewire, ds18x20, dht,
# neopixel, port-level modules/) -- this project doesn't currently use
# any of them (confirmed via grep across mp/), but there's no reason to
# drop them just because we're also freezing our own files.
include("$(PORT_DIR)/boards/manifest.py")

# ---------------------------------------------------------------------
# 2026-10-17: Stage 1 trial — board_config.py only, on pico2 then
# pico2_piousb — passed real-hardware verification on both (confirmed via
# `mpremote ls` showing board_config.py absent from the device filesystem
# while the emulator still ran correctly, i.e. the frozen copy was
# genuinely what loaded; no USB/mpremote connection instability on either
# board after repeated reconnect/run cycles — see doc/memory_usage.md
# §9.4/§9.5 for the full writeup).
#
# 2026-10-17: Stage 2 — expanded to every other mp/*.py file EXCEPT
# main.py/boot.py (which can't be frozen the same way — main.py is the
# auto-run entry point developers actively edit every session, and
# boot.py runs before board_config.py's own board-type detection is even
# possible — see boot.py's own comment). This is, bytecode-wise, close to
# a superset of what the 2026-09-30 pizero attempt froze (that attempt's
# own comment says "lazily-imported menu modules" — presumably
# msx_rom_browser.py/msx_runtime_menu.py/msx_save_slots.py/
# msx_display_settings.py/msx_mode_switch.py, now included below), plus
# several eager-imported files that attempt never touched
# (board_config.py/msx_keymap.py/msx_ext.py/msx_menu.py) and one more
# lazy one (msx_fdd.py) and the pure-Python SD fallback (sdcard.py).
# Still pico2/pico2_piousb only, NOT pizero — see this file's own
# micropython_msx.cmake wiring and doc/memory_usage.md §9.2's writeup on
# why pizero stays excluded (its hand-tuned-tight RAM/flash layout is the
# leading suspect for that attempt's regression, independent of which
# REPL transport it used).
#
# None of these files have any module-level side effects (verified by
# grepping for top-level statements beyond constant/def/class — only
# constants and function/class definitions execute at import time), and
# none use __file__ or similar introspection that frozen modules can't
# provide (grepped project-wide; only main.py uses
# `if __name__ == '__main__':`, which isn't frozen).
#
# DO NOT extend this list to pizero without a fresh real-hardware
# confirmation on that board specifically — see doc/memory_usage.md §9
# before changing that.
freeze(
    "$(MSX_MP_DIR)",
    (
        "board_config.py",
        "msx_keymap.py",
        "msx_ext.py",
        "msx_menu.py",
        "sdcard.py",
        "msx_mode_switch.py",
        "msx_fdd.py",
        "msx_rom_browser.py",
        "msx_runtime_menu.py",
        "msx_save_slots.py",
        "msx_display_settings.py",
        # 2026-10-17: main.py's own ~1300-line body, split out verbatim
        # into msx_app.py specifically so it could be added here — see
        # msx_app.py's own header comment and doc/memory_usage.md §9 for
        # the full rationale. main.py itself (now a ~20-line launcher
        # that does `from msx_app import run`) is NOT frozen and can't
        # be: it's the script MicroPython's boot sequence auto-execs,
        # which only ever reaches a real filesystem file, never a frozen
        # module.
        "msx_app.py",
    ),
)
