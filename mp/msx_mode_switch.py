# msx_mode_switch.py — cartridge-load logic for the runtime menu's Swap
# Cartridge item, plus Switch to Cart Mode / Switch to Disk Mode, split
# out of msx_runtime_menu.py and imported lazily (only when one of these
# is actually selected).
#
# 2026-09-20: even after deduplicating all three into a single shared
# helper *inside* msx_runtime_menu.py, that module was still too big to
# reliably compile on first GUI+F7 press (real-hardware MemoryError, two
# rounds — the first fix only shrank the failing allocation from 1982 to
# 1336 bytes, still failing). Moving the load logic out entirely — not
# just deduplicating it in place — mirrors the same lazy-split pattern
# already used for msx_display_settings.py/msx_rom_browser.py/
# msx_save_slots.py (see msx_runtime_menu.py's own header comment), just
# one level deeper: msx_runtime_menu.py is already one of those
# lazily-split modules, and apparently still needed splitting further.
#
# 2026-09-20 (second revision, at the user's request): Switch to Cart
# Mode / Switch to Disk Mode no longer swap the live cart in place
# (eject_cart()+load_cart_smart()+msx_fdd.register()). They now write
# the new mode=/cart=/diskrom= keys to msx.ini and, after an explicit
# ENTER-to-confirm prompt, trigger a full MCU reset (machine.reset()) —
# the normal boot sequence (main.py, already proven to load either mode
# correctly from a clean, unfragmented heap) does the actual loading
# from there, instead of a live in-place swap on top of however
# fragmented mid-gameplay heap state already existed. This also means
# these two functions need none of swap_cartridge()'s eject/load
# machinery — only the ROM picker and a confirm screen.

from msx_menu import (C_BLACK, C_YELLOW, C_WHITE, C_GRAY,
                      hdmi_suspend, hdmi_resume, lcd_suspend, lcd_resume,
                      select_rom, load_cart_smart, log_mem, save_config,
                      _get_key, _wait_key_release, HID_ENTER, HID_ESC)
from msx_runtime_menu import _draw_runtime_menu


def _load_rom_with_feedback(msx_module, canvas, cursor, items, rom_dir,
                            title, start_dir, usb_host_mod, exclude_names):
    # Browses for a ROM, suspends HDMI/LCD, ejects whatever cart slot 1
    # currently holds, loads the file, and resumes the displays. Returns
    # (path, None) on success or (None, message) otherwise — never raises
    # OSError/MemoryError/etc. itself, matching every other SD-touching
    # action in this menu. Only swap_cartridge() (a same-mode, live swap)
    # uses this now — see this module's docstring for why Switch to Cart/
    # Disk Mode no longer do a live swap at all.
    try:
        path = select_rom(msx_module, rom_dir, title=title,
                          usb_host_mod=usb_host_mod, timeout_ms=0,
                          exclude_names=exclude_names, start_dir=start_dir)
    except OSError as e:
        return None, f"Directory listing failed: {e}"
    if not path:
        return None, ""
    _prev_hdmi = hdmi_suspend()
    _prev_lcd  = lcd_suspend()
    try:
        _draw_runtime_menu(canvas, cursor, "Loading…", items=items)
        try:
            import gc
            msx_module.eject_cart(0)
            gc.collect()  # defragment before the cart-sized read
            log_mem(f"before {title} load_cart_smart()")
            if load_cart_smart(msx_module, 0, path):
                return path, None
            return None, "load_cart() failed"
        except Exception as e:
            return None, f"Load failed: {e}"
    finally:
        lcd_resume(_prev_lcd)
        hdmi_resume(_prev_hdmi)


def swap_cartridge(msx_module, canvas, cursor, items, rom_dir, cart_path,
                   usb_host_mod, exclude_names):
    start_dir = cart_path.rsplit('/', 1)[0] if cart_path else None
    selected, err = _load_rom_with_feedback(
        msx_module, canvas, cursor, items, rom_dir,
        "Select Cartridge ROM", start_dir, usb_host_mod, exclude_names)
    if not selected:
        return None, err
    msx_module.reset()
    return selected, None


def _confirm_reset(canvas, usb_host_mod, action_msg):
    # Full-screen Y/N prompt before rebooting the MCU. ENTER confirms;
    # ESC (or anything else) cancels. Drawn directly rather than via
    # _draw_runtime_menu (that renders the item list; this is a one-off
    # yes/no screen, not a menu).
    import time
    canvas.clear(C_BLACK)
    canvas.rect(0, 0, canvas.W, 12, C_YELLOW, fill=True)
    canvas.text("CONFIRM RESET", 2, 2, C_BLACK)
    canvas.text(action_msg[:38], 2, 24, C_WHITE)
    canvas.text("Device will restart.", 2, 36, C_WHITE)
    canvas.hline(0, canvas.H - 11, canvas.W, C_GRAY)
    canvas.text("ENTER:confirm  ESC:cancel", 2, canvas.H - 10, C_GRAY)
    canvas.flush()
    _wait_key_release(usb_host_mod)
    last_key = 0
    while True:
        time.sleep_ms(30)
        key = _get_key(usb_host_mod)
        if key == last_key:
            continue
        last_key = key
        if key == 0:
            continue
        if key == HID_ENTER:
            _wait_key_release(usb_host_mod)
            return True
        if key == HID_ESC:
            _wait_key_release(usb_host_mod)
            return False


def _reboot_mcu(canvas):
    # Draws a "Rebooting…" screen, makes sure it's actually finished
    # transferring to the display, then gives the bus/peripherals a
    # moment to settle before machine.reset() — mirrors the sibling
    # PB-1000 emulator's own _do_reboot_mcu() (mp/emulator_menu_system_
    # actions.py there), which does the same draw+flush+sleep_ms(400)
    # sequence. 2026-09-20 real-hardware finding: without this, the MCU
    # reset can fire while a display transfer (HDMI bridge in particular
    # — a separate SPI-connected chip, not just an internal peripheral)
    # is still in flight, leaving it out of sync with the next boot's own
    # from-scratch re-init (msx.hdmi_reset_pulse() etc. in main.py) —
    # symptom: the reboot and the emulator itself both come up fine
    # internally (visible in the serial log), but nothing ever appears on
    # the display.
    import time
    canvas.clear(C_BLACK)
    canvas.rect(0, 0, canvas.W, 12, C_YELLOW, fill=True)
    canvas.text("REBOOTING", 2, 2, C_BLACK)
    canvas.text("Restarting device…", 2, 24, C_WHITE)
    canvas.flush()
    time.sleep_ms(400)
    import machine
    machine.reset()


def to_cart_mode(msx_module, canvas, rom_dir, cart_path, usb_host_mod,
                 exclude_names, config_path):
    # Pick a cartridge, confirm, persist cart=/mode= to msx.ini, then
    # reboot the MCU. mode='' explicitly clears any leftover mode=disk —
    # main.py's `if _fdd_mode: ... elif 'cart' in cfg:` dispatch gives
    # mode=disk priority, so a stale value there would silently keep
    # winning over the new cart= on the next boot otherwise.
    #
    # Broad except below (not just OSError around select_rom(), like an
    # earlier version of this had): a mode switch is always attempted
    # mid-gameplay, the exact heap-fragmentation-prone situation this
    # whole project keeps hitting MemoryError in (see e.g.
    # msx_menu.py's load_cart_smart() comments) — select_rom()'s own
    # lazy msx_rom_browser.py import is a real place for that to happen.
    # Left uncaught, real hardware was observed to silently drop all the
    # way out to the MicroPython REPL instead of returning an error
    # message to this menu, since neither this function nor its caller
    # (msx_runtime_menu.py) nor msx_menu.show_emulator_menu() wrap this
    # call — only main.py's own top-level handler around
    # show_emulator_menu() does, and by then it's too late to recover
    # gracefully back into the menu loop.
    try:
        start_dir = cart_path.rsplit('/', 1)[0] if cart_path else None
        selected = select_rom(msx_module, rom_dir,
                              title="Select Cartridge ROM",
                              usb_host_mod=usb_host_mod, timeout_ms=0,
                              exclude_names=exclude_names, start_dir=start_dir)
        if not selected:
            return ""
        if not _confirm_reset(canvas, usb_host_mod,
                              f"Cart: {selected.rsplit('/',1)[-1]}"):
            return ""
        if config_path is None:
            return "No config path — can't switch"
        save_config(config_path, {'cart': selected, 'mode': ''})
    except OSError as e:
        return f"Directory listing failed: {e}"
    except Exception as e:
        return f"Switch failed: {e}"
    _reboot_mcu(canvas)


def to_disk_mode(msx_module, canvas, rom_dir, diskrom_path, usb_host_mod,
                 exclude_names, config_path):
    # Reuses diskrom_path (msx.ini's diskrom=, remembered even in cart
    # mode — see main.py) if already known, otherwise prompts a one-off
    # picker. Doesn't touch disk= itself — the disk image is picked via
    # "Swap Disk" (or msx.ini's own disk=) on the next boot, same as
    # today's normal mode=disk boot flow. Broad except below — see
    # to_cart_mode()'s comment for why.
    try:
        selected = diskrom_path
        if selected is None:
            selected = select_rom(msx_module, rom_dir,
                                  title="Select Disk ROM",
                                  usb_host_mod=usb_host_mod, timeout_ms=0,
                                  exclude_names=exclude_names)
            if not selected:
                return ""
        if not _confirm_reset(canvas, usb_host_mod,
                              f"Disk: {selected.rsplit('/',1)[-1]}"):
            return ""
        if config_path is None:
            return "No config path — can't switch"
        save_config(config_path, {'mode': 'disk', 'diskrom': selected})
    except OSError as e:
        return f"Directory listing failed: {e}"
    except Exception as e:
        return f"Switch failed: {e}"
    _reboot_mcu(canvas)
