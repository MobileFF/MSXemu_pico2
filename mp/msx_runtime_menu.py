# msx_runtime_menu.py — the GUI+F7 runtime emulator menu (cart swap,
# save/load, reset, Audio/Display Settings dispatch), split out of
# msx_menu.py and imported lazily (only the first time GUI+F7 is
# actually pressed) so its compile cost isn't paid at every boot — see
# msx_menu.py's show_emulator_menu() thin wrapper.
#
# 2026-09-06: split out after a MemoryError reappeared compiling
# msx_menu.py at boot (main.py had grown enough this session that the
# combined peak — main.py's already-compiled/retained footprint plus
# msx_menu.py's own compile-time cost — exceeded the heap again,
# independent of msx_menu.py's absolute size). This was the single
# largest remaining eagerly-compiled chunk of msx_menu.py (everything
# else left there — MenuCanvas, save-state plumbing, cart loading,
# config I/O — is needed unconditionally at boot, so it can't be
# deferred the same way). Mirrors the exact pattern already used for
# msx_display_settings.py/msx_rom_browser.py/msx_save_slots.py. (Also
# converted from a docstring to comments — a real-hardware MemoryError
# in the *next* lazy import, msx_display_settings.py, right after this
# one had already loaded, showed every byte still matters here too: a
# docstring is a retained string constant, a comment costs nothing.)

from msx_menu import (MenuCanvas, C_BLACK, C_YELLOW, C_GREEN, C_WHITE,
                      C_CYAN, C_GRAY, _echo_msg, _wrap_msg, _get_key, _wait_key_release,
                      HID_UP, HID_DOWN, HID_LEFT, HID_RIGHT, HID_ENTER, HID_ESC,
                      hdmi_suspend, hdmi_resume, lcd_suspend, lcd_resume,
                      select_rom, load_state_from,
                      save_config, save_base_for_cart, rotate_and_save_state,
                      log_mem)

_RUNTIME_ITEMS = ["Swap Cartridge", "Switch to Disk Mode", "Save State",
                  "Load State", "Audio Settings", "Display Settings",
                  "Reset MSX", "Resume"]

# Volume steps in 16-unit increments (0-256; 256 = original full-scale
# default, ~75% PWM duty — see modmsx.c). Filter steps 0-8 (0=off; higher
# = heavier low-pass smoothing, trades clarity for less buzzer harshness).
_VOLUME_STEP = 16
_VOLUME_MAX  = 256
_FILTER_MAX  = 8


def _draw_runtime_menu(canvas, cursor, msg="", items=_RUNTIME_ITEMS):
    _echo_msg(msg)
    canvas.clear(C_BLACK)
    canvas.rect(0, 0, canvas.W, 12, C_YELLOW, fill=True)
    canvas.text("EMULATOR MENU", 2, 2, C_BLACK)

    y = 20
    for i, label in enumerate(items):
        if i == cursor:
            canvas.rect(0, y, canvas.W, 10, C_GREEN, fill=True)
            canvas.text(label, 2, y + 1, C_BLACK)
        else:
            canvas.text(label, 2, y + 1, C_WHITE)
        y += 12

    for k, line in enumerate(_wrap_msg(msg) if msg else ()):
        canvas.text(line, 2, y + 6 + 10 * k, C_CYAN)

    canvas.hline(0, canvas.H - 11, canvas.W, C_GRAY)
    canvas.text("UP/DOWN  ENTER:select  ESC:resume", 2, canvas.H - 10, C_GRAY)
    canvas.flush()
    # 2026-10-05 DIAGNOSTIC (re-added) — see main.py's identical print.
    try:
        print(f"DVI DIAG: {canvas._msx.dvi_debug()}")
    except Exception:
        pass


def _draw_audio_settings(canvas, cursor, volume, filt, msg=""):
    _echo_msg(msg)
    canvas.clear(C_BLACK)
    canvas.rect(0, 0, canvas.W, 12, C_YELLOW, fill=True)
    canvas.text("AUDIO SETTINGS", 2, 2, C_BLACK)

    rows = [f"Volume: {volume}", f"Filter: {filt}"]
    y = 20
    for i, label in enumerate(rows):
        if i == cursor:
            canvas.rect(0, y, canvas.W, 10, C_GREEN, fill=True)
            canvas.text(label, 2, y + 1, C_BLACK)
        else:
            canvas.text(label, 2, y + 1, C_WHITE)
        y += 12

    for k, line in enumerate(_wrap_msg(msg) if msg else ()):
        canvas.text(line, 2, y + 6 + 10 * k, C_CYAN)

    canvas.hline(0, canvas.H - 21, canvas.W, C_GRAY)
    canvas.text("LEFT/RIGHT:adjust  UP/DOWN:field", 2, canvas.H - 20, C_GRAY)
    canvas.text("ENTER:save  ESC:back(no save)", 2, canvas.H - 10, C_GRAY)
    canvas.flush()


def _show_audio_settings_menu(msx_module, usb_host_mod, config_path):
    # Live-adjustable Volume/Filter screen. Changes take effect immediately
    # (msx_module.set_audio_volume/set_audio_filter) so you hear the result
    # while adjusting. ENTER persists both values into msx.ini via
    # save_config(); ESC returns without writing the file (the
    # live-adjusted values remain in effect for the rest of this session
    # either way).
    import time

    canvas = MenuCanvas(msx_module)
    cursor = 0
    volume = msx_module.get_audio_volume()
    filt   = msx_module.get_audio_filter()
    msg = ""

    _draw_audio_settings(canvas, cursor, volume, filt, msg)
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

        msg = ""
        if key == HID_UP or key == HID_DOWN:
            cursor = 1 - cursor
        elif key == HID_LEFT or key == HID_RIGHT:
            sign = -1 if key == HID_LEFT else 1
            if cursor == 0:
                volume = max(0, min(_VOLUME_MAX, volume + sign * _VOLUME_STEP))
                msx_module.set_audio_volume(volume)
            else:
                filt = max(0, min(_FILTER_MAX, filt + sign))
                msx_module.set_audio_filter(filt)
        elif key == HID_ENTER:
            _wait_key_release(usb_host_mod)
            if config_path is None:
                msg = "No config path — not saved"
            else:
                try:
                    save_config(config_path, {'volume': volume, 'audio_filter': filt})
                    msg = "Saved to msx.ini"
                except Exception as e:
                    msg = f"Save failed: {e}"
            _draw_audio_settings(canvas, cursor, volume, filt, msg)
            time.sleep_ms(800)
            return
        elif key == HID_ESC:
            _wait_key_release(usb_host_mod)
            return

        _draw_audio_settings(canvas, cursor, volume, filt, msg)


def show(msx_module, usb_host_mod, rom_dir, exclude_names,
        save_path, config_path=None,
        init_hdmi_output=None, init_lcd_output=None,
        display_state=None, cart_path=None,
        fdd_mode=False, disk_path=None, diskrom_path=None):
    # Pause gameplay and show the runtime emulator menu (GUI+F7).
    # All actions (cart/disk swap, save/load, reset) are performed
    # directly here; the caller just needs to resume its main loop once
    # this returns.
    #
    # display_state/init_hdmi_output/init_lcd_output: see
    # msx_display_settings.show(). Pass the caller's current display/
    # frame_skip/lcd/rotate/hdmi_baud state in (a dict, built from
    # whatever it actually initialized at boot — 'display' is
    # 'lcd'/'hdmi', mutually exclusive, see msx_menu.set_display_state()).
    #
    # save_path/cart_path: `save_path` is really a fallback save-state
    # BASE path (no cart loaded / BASIC-only session) —
    # save_base_for_cart() resolves the actual base (next to whichever
    # cart is currently loaded, `cart_path`, falling back to `save_path`
    # otherwise), and rotate_and_save_state()/msx_save_slots manage up to
    # MAX_SAVE_SLOTS numbered saves under that base. cart_path is updated
    # here when Swap Cartridge succeeds and returned so the caller can
    # keep it for its own F5/F8 hotkey saves.
    #
    # fdd_mode/disk_path: mode=disk (see mp/msx_fdd.py) — mutually
    # exclusive with cart_path (always None in this mode). "Swap
    # Cartridge" becomes "Swap Disk" (browses .DSK instead of .ROM).
    # Unlike Swap Cartridge, Swap Disk does NOT reset the machine — mp/
    # msx_fdd.py hooks DSKCHG (see its docstring) specifically so
    # MSX-DOS notices the swap on its own via the normal disk-change
    # protocol; the earlier unconditional msx.reset() here was removed
    # 2026-09-15 at the user's explicit request. Not yet verified on
    # real hardware beyond this — if MSX-DOS's own directory/FAT cache
    # doesn't pick up the new image correctly after a swap, that's the
    # first place to look. Save/Load State key off disk_path here
    # instead of cart_path, same rotating-slot-per-image reasoning.
    #
    # diskrom_path: the Disk ROM last loaded into cart slot 1 under
    # mode=disk (msx.ini's diskrom=, remembered by the caller even
    # while currently in cart mode — see main.py) — reused by "Switch
    # to Disk Mode" so it doesn't have to ask again once known. If
    # None, that action prompts a one-off .ROM picker instead (same
    # "ask once, remember from then on" pattern already used for
    # mp/msx_fdd.py's _fdc_base).
    #
    # "Switch to Disk Mode"/"Switch to Cart Mode" (mp/msx_mode_switch.py):
    # unlike Swap Cartridge/Swap Disk (which replace the ROM/image
    # currently active in the current mode via a live in-place load),
    # these change WHICH mode cart slot 1 is in. 2026-09-20, at the
    # user's request: rather than live-swapping cart slot 1 and calling
    # msx.reset(), they now write the new mode=/cart=/diskrom= keys to
    # msx.ini and, after an explicit ENTER-to-confirm prompt, reboot the
    # whole MCU (machine.reset()) — the normal boot sequence then loads
    # the new mode from a clean, unfragmented heap instead of a live
    # in-place swap on top of however fragmented mid-gameplay state
    # already existed. A successful switch therefore never returns here
    # at all (the device restarts); these two items only come back to
    # this loop on cancel or a load/config error.
    #
    # Returns (display_state, cart_path, disk_path, fdd_mode,
    # diskrom_path), all possibly updated, so the caller can update its
    # own globals.
    import time
    import sys

    items = list(_RUNTIME_ITEMS)
    if fdd_mode:
        items[0] = "Swap Disk"
        items[1] = "Switch to Cart Mode"

    canvas = MenuCanvas(msx_module)
    cursor = 0
    msg = ""

    _draw_runtime_menu(canvas, cursor, msg, items=items)
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

        redraw = True
        if key == HID_UP:
            cursor = (cursor - 1) % len(_RUNTIME_ITEMS)
            msg = ""
        elif key == HID_DOWN:
            cursor = (cursor + 1) % len(_RUNTIME_ITEMS)
            msg = ""
        elif key == HID_ESC:
            _wait_key_release(usb_host_mod)
            return display_state, cart_path, disk_path, fdd_mode, diskrom_path
        elif key == HID_ENTER:
            _wait_key_release(usb_host_mod)
            label = items[cursor]

            if label == "Resume":
                return display_state, cart_path, disk_path, fdd_mode, diskrom_path

            elif label == "Swap Disk":
                # No msx.reset() here (unlike Swap Cartridge below) — see
                # this function's docstring: mp/msx_fdd.py's DSKCHG hook
                # is relied on to let MSX-DOS notice the new image on its
                # own. select_rom()/hdmi_suspend()/lcd_suspend() usage
                # mirrors Swap Cartridge exactly, just filtered to .DSK
                # and without touching cart slot 1 (the Disk ROM stays
                # loaded — only the mounted image changes).
                listdir_failed = False
                start_dir = disk_path.rsplit('/', 1)[0] if disk_path else None
                try:
                    selected = select_rom(msx_module, rom_dir,
                                          title="Select Disk Image",
                                          usb_host_mod=usb_host_mod,
                                          timeout_ms=0,
                                          exclude_names=exclude_names,
                                          start_dir=start_dir, ext='.dsk')
                except OSError as e:
                    selected = None
                    listdir_failed = True
                    msg = f"Directory listing failed: {e}"
                if selected:
                    _prev_hdmi = hdmi_suspend()
                    _prev_lcd  = lcd_suspend()
                    try:
                        _draw_runtime_menu(canvas, cursor, "Loading…", items=items)
                        try:
                            import msx_fdd
                            msx_fdd.mount(selected)
                            disk_path = selected
                            msg = f"Mounted {selected.rsplit('/',1)[-1]}"
                        except Exception as e:
                            msg = f"Mount failed: {e}"
                    finally:
                        lcd_resume(_prev_lcd)
                        hdmi_resume(_prev_hdmi)
                elif not listdir_failed:
                    msg = ""

            elif label == "Swap Cartridge":
                import msx_mode_switch  # lazy — see its own docstring
                selected, err = msx_mode_switch.swap_cartridge(
                    msx_module, canvas, cursor, items, rom_dir, cart_path,
                    usb_host_mod, exclude_names)
                if selected:
                    cart_path = selected  # own save-state file from here on
                    msg = f"Loaded {selected.rsplit('/',1)[-1]}"
                else:
                    msg = err

            elif label == "Switch to Cart Mode":
                # Writes msx.ini + reboots the MCU on confirm — see
                # msx_mode_switch.py's docstring. Only returns here on
                # cancel/error (a real switch never comes back).
                import msx_mode_switch  # lazy — see its own docstring
                msg = msx_mode_switch.to_cart_mode(
                    msx_module, canvas, rom_dir, cart_path,
                    usb_host_mod, exclude_names, config_path)

            elif label == "Switch to Disk Mode":
                import msx_mode_switch  # lazy — see its own docstring
                msg = msx_mode_switch.to_disk_mode(
                    msx_module, canvas, rom_dir, diskrom_path,
                    usb_host_mod, exclude_names, config_path)

            elif label == "Save State":
                # Writing ~64KB to SD takes a couple of seconds — show
                # feedback so this isn't mistaken for a hang. HDMI
                # suspended for the duration (see hdmi_suspend()'s
                # comment — a sustained ~64KB SD write is exactly the
                # kind of SD access that's unreliable soon after an
                # HDMI-mode draw); does not force LCD mode. LCD suspended
                # too (lcd_suspend()) — see its docstring.
                _prev_hdmi = hdmi_suspend()
                _prev_lcd  = lcd_suspend()
                _draw_runtime_menu(canvas, cursor, "Saving…", items=items)
                try:
                    rotate_and_save_state(msx_module, save_base_for_cart(disk_path if fdd_mode else cart_path, save_path))
                    msg = "State saved"
                except Exception as e:
                    msg = f"Save failed: {e}"
                finally:
                    lcd_resume(_prev_lcd)
                    hdmi_resume(_prev_hdmi)

            elif label == "Load State":
                # Slot picker (msx_save_slots.py, lazily imported like
                # msx_rom_browser.py/msx_display_settings.py) suspends
                # HDMI/LCD internally only around its own SD stat() calls,
                # same reasoning as select_rom() — the interactive list
                # itself is redrawn from RAM, no per-keypress SD access.
                base = save_base_for_cart(disk_path if fdd_mode else cart_path, save_path)
                # 2026-10-01: only collect if actually about to compile —
                # see show_emulator_menu()'s identical fix/comment in
                # msx_menu.py (gc.collect() alone can starve pizero's DVI
                # output; main.py's _prewarm_menu_modules() already
                # imported this at boot there).
                if 'msx_save_slots' not in sys.modules:
                    import gc
                    gc.collect()
                import msx_save_slots
                log_mem("after msx_save_slots import")
                chosen = msx_save_slots.select(msx_module, base,
                                               usb_host_mod=usb_host_mod)
                if chosen:
                    _prev_hdmi = hdmi_suspend()
                    _prev_lcd  = lcd_suspend()
                    _draw_runtime_menu(canvas, cursor, "Loading…", items=items)
                    try:
                        ok = load_state_from(msx_module, chosen)
                        msg = "State loaded" if ok else "Invalid save file"
                    except Exception as e:
                        msg = f"Load failed: {e}"
                    finally:
                        lcd_resume(_prev_lcd)
                        hdmi_resume(_prev_hdmi)
                else:
                    msg = ""

            elif label == "Audio Settings":
                _show_audio_settings_menu(msx_module, usb_host_mod, config_path)
                msg = ""

            elif label == "Display Settings":
                # 2026-10-01: only collect if actually about to compile —
                # see show_emulator_menu()'s identical fix/comment in
                # msx_menu.py.
                if 'msx_display_settings' not in sys.modules:
                    import gc
                    gc.collect()
                import msx_display_settings  # lazy — see its own docstring
                log_mem("after msx_display_settings import")
                display_state = msx_display_settings.show(
                    msx_module, usb_host_mod, config_path, display_state,
                    init_hdmi_output=init_hdmi_output,
                    init_lcd_output=init_lcd_output)
                msg = ""

            elif label == "Reset MSX":
                msx_module.reset()
                msg = "MSX reset"

        if redraw:
            _draw_runtime_menu(canvas, cursor, msg, items=items)
