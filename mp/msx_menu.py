"""
msx_menu.py — the lazy-import entry points for the ROM browser/runtime
menu; everything else used to live here directly.

2026-10-16: this module's foundation (MenuCanvas/color/HID constants/
display-suspend state/keyboard polling/message echo/wrap in phase 1;
save-state I/O + the shared chunked-file-I/O helpers in phase 2; msx.ini
config I/O + cartridge loading in phase 3) was ported to C (src/
modmsx_ui.c, module `msx_ui`) to remove its compile-time GC-heap cost —
msx_menu.py is NOT lazily imported like msx_rom_browser.py/
msx_runtime_menu.py/msx_save_slots.py/msx_display_settings.py are
(main.py imports it eagerly at every boot), so its own compile cost used
to be paid unconditionally every single boot (see doc/memory_usage.md's
§3.1/§3.2/§4.1 for the real-hardware MemoryError history this caused).
This file now re-exports every one of those names from msx_ui below so
every existing `from msx_menu import ...` call site elsewhere in this
project keeps working completely unchanged — only this file's own
import line changed, not any caller. See modmsx_ui.c's own header
comment for the full rationale, including why msx.load_cart_paged()
itself is still reached via a method call on the passed-in msx_module
object rather than reimplemented.

Only select_rom()/show_emulator_menu() (thin lazy-import wrappers around
msx_rom_browser.py/msx_runtime_menu.py, both still Python) and log_mem()
remain defined directly in this file.

Subroutine (CALL/RST) hooks are unaffected by any of this — they stay a
separate, deliberately-Python-only mechanism (msx.set_call_hook(),
mp/msx_ext.py/msx_fdd.py).
"""

from msx_ui import (
    MenuCanvas,
    C_BLACK, C_WHITE, C_GREEN, C_YELLOW, C_GRAY, C_CYAN, C_RED, rgb,
    HID_UP, HID_DOWN, HID_LEFT, HID_RIGHT, HID_ENTER, HID_ESC,
    set_display_state, hdmi_suspend, hdmi_resume, lcd_suspend, lcd_resume,
    get_key as _get_key, wait_key_release as _wait_key_release,
    echo_msg as _echo_msg, wrap_msg as _wrap_msg, draw_message as _draw_message,
    SAVE_HEADER_SIZE, MAX_SAVE_SLOTS,
    readinto_chunked, write_chunked,
    save_state_to, load_state_from,
    save_slot_path, save_base_for_cart, rotate_and_save_state, list_save_slots,
    load_config, save_config,
    get_rom_load_buf, load_cart_smart,
)


# ---------------------------------------------------------------------------
# Heap diagnostics — 2026-09-08, added after chasing several real-hardware
# MemoryErrors this week (main.py itself, this module, mid-gameplay lazy
# imports, a Mega ROM scratch buffer) mostly by trial and error. Cheap,
# always-on visibility into gc.mem_free() at the key points where a lazy
# .py file is about to be compiled (the actual moment these failures hit)
# beats reactively guessing after the next real-hardware report. Printed
# to the console only — no SD/file access, no extra heap cost beyond the
# f-string itself.
# ---------------------------------------------------------------------------

def log_mem(tag):
    import gc
    print(f"MEM {tag}: {gc.mem_free()} bytes free")


# ---------------------------------------------------------------------------
# File browser — see msx_rom_browser.py (lazily imported by select_rom()
# below and by show_emulator_menu()'s "Swap Cartridge" branch).
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# Save state / config I/O / cartridge loading — all moved to C
# (src/modmsx_ui.c, phases 2-3, see that file's own header comment) and
# imported from msx_ui above: save_state_to()/load_state_from()/
# save_slot_path()/save_base_for_cart()/rotate_and_save_state()/
# list_save_slots() + SAVE_HEADER_SIZE/MAX_SAVE_SLOTS (phase 2);
# load_config()/save_config()/get_rom_load_buf()/load_cart_smart() +
# the Mega-ROM flash-cache logic behind it (phase 3). Kept here only as
# a section marker since every call site in this project still refers to
# them as msx_menu.* / imports them from here.
# ---------------------------------------------------------------------------


# ---------------------------------------------------------------------------
# Public API
# ---------------------------------------------------------------------------

def select_rom(msx_module, directory, title="Select ROM",
               usb_host_mod=None, timeout_ms=5000,
               exclude_names=(), start_dir=None, ext='.rom'):
    """Thin lazy-import wrapper — see msx_rom_browser.select() for the
    actual implementation (kept out of this module's eager compile path;
    only loaded the first time a ROM actually needs picking). `ext`
    selects which file extension is browsable ('.rom' cartridges by
    default, '.dsk' for the virtual FDD's disk-image browser).

    2026-10-01: only collects garbage if actually about to compile this
    module — see show_emulator_menu()'s identical fix/comment below
    (gc.collect() alone can starve pizero's DVI output; main.py's
    _prewarm_menu_modules() may already have imported this at boot)."""
    import sys
    if 'msx_rom_browser' not in sys.modules:
        import gc
        gc.collect()
    import msx_rom_browser
    log_mem("after msx_rom_browser import")
    return msx_rom_browser.select(msx_module, directory, title=title,
                                  usb_host_mod=usb_host_mod,
                                  timeout_ms=timeout_ms,
                                  exclude_names=exclude_names,
                                  start_dir=start_dir, ext=ext)


# ---------------------------------------------------------------------------
# Runtime emulator menu (GUI+F7, analogous to the PB-1000 emulator's
# emulator_menu.py). Called from main.py's poll_keyboard() while
# gameplay is paused; returns once the user resumes. Lives in
# msx_runtime_menu.py, imported lazily below only the first time GUI+F7
# is actually pressed — see that module's docstring.
# ---------------------------------------------------------------------------

def show_emulator_menu(msx_module, usb_host_mod, rom_dir, exclude_names,
                       save_path, config_path=None,
                       init_hdmi_output=None, init_lcd_output=None,
                       display_state=None, cart_path=None,
                       fdd_mode=False, disk_path=None, diskrom_path=None):
    # Thin lazy-import wrapper — see msx_runtime_menu.show() for the
    # actual implementation (kept out of this module's eager compile
    # path; only loaded the first time GUI+F7 is actually pressed).
    # fdd_mode/disk_path: mode=disk (see mp/msx_fdd.py) — "Swap Cartridge"
    # becomes "Swap Disk" and browses .DSK files instead of .ROM.
    # diskrom_path: the Disk ROM to reuse for "Switch to Disk Mode" (see
    # msx_runtime_menu.show()'s docstring) — pass main.py's remembered
    # msx.ini diskrom= even while currently in cart mode. Returns
    # (display_state, cart_path, disk_path, fdd_mode, diskrom_path) —
    # fdd_mode/diskrom_path can now change here too (mode-switch menu
    # items), not just cart_path/disk_path.
    # 2026-10-01 real-hardware finding: gc.collect() here used to run
    # unconditionally on every single GUI+F7 press, even once main.py's
    # _prewarm_menu_modules() (pizero only) had already imported
    # msx_runtime_menu at boot, making the collect's own stated purpose
    # ("defragment before compiling") a no-op — there is no compile left
    # to defragment for. Worse: disp_dvi.c's own comment documents
    # gc.collect() ALONE as sufficient to starve the DVI TMDS buffer queue
    # ("late_scanline_ctr... never recovers once positive"), and this
    # project chased exactly that ("No Signal" a few Win+F7 presses in,
    # not necessarily the first) down two dead ends (freezing the lazy
    # modules into the firmware, then pre-warming them at boot) before
    # noticing THIS call was still unconditionally running every time
    # regardless of either fix. Only collect when actually about to
    # compile something (i.e. the module isn't cached yet — pico2, or a
    # pizero build without the pre-warm step).
    import sys
    if 'msx_runtime_menu' not in sys.modules:
        import gc
        gc.collect()
    import msx_runtime_menu
    log_mem("after msx_runtime_menu import")
    return msx_runtime_menu.show(msx_module, usb_host_mod, rom_dir,
                                 exclude_names, save_path,
                                 config_path=config_path,
                                 init_hdmi_output=init_hdmi_output,
                                 init_lcd_output=init_lcd_output,
                                 display_state=display_state,
                                 cart_path=cart_path,
                                 fdd_mode=fdd_mode, disk_path=disk_path,
                                 diskrom_path=diskrom_path)
