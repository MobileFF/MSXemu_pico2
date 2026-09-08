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
                      C_CYAN, C_GRAY, _echo_msg, _get_key, _wait_key_release,
                      HID_UP, HID_DOWN, HID_LEFT, HID_RIGHT, HID_ENTER, HID_ESC,
                      hdmi_suspend, hdmi_resume, lcd_suspend, lcd_resume,
                      select_rom, load_cart_smart, load_state_from,
                      save_config, save_base_for_cart, rotate_and_save_state,
                      log_mem)

_RUNTIME_ITEMS = ["Swap Cartridge", "Save State", "Load State",
                  "Audio Settings", "Display Settings",
                  "Reset MSX", "Resume"]

# Volume steps in 16-unit increments (0-256; 256 = original full-scale
# default, ~75% PWM duty — see modmsx.c). Filter steps 0-8 (0=off; higher
# = heavier low-pass smoothing, trades clarity for less buzzer harshness).
_VOLUME_STEP = 16
_VOLUME_MAX  = 256
_FILTER_MAX  = 8


def _draw_runtime_menu(canvas, cursor, msg=""):
    _echo_msg(msg)
    canvas.clear(C_BLACK)
    canvas.rect(0, 0, canvas.W, 12, C_YELLOW, fill=True)
    canvas.text("EMULATOR MENU", 2, 2, C_BLACK)

    y = 20
    for i, label in enumerate(_RUNTIME_ITEMS):
        if i == cursor:
            canvas.rect(0, y, canvas.W, 10, C_GREEN, fill=True)
            canvas.text(label, 2, y + 1, C_BLACK)
        else:
            canvas.text(label, 2, y + 1, C_WHITE)
        y += 12

    if msg:
        canvas.text(msg[:31], 2, y + 6, C_CYAN)

    canvas.hline(0, canvas.H - 11, canvas.W, C_GRAY)
    canvas.text("UP/DOWN  ENTER:select  ESC:resume", 2, canvas.H - 10, C_GRAY)
    canvas.flush()


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

    if msg:
        canvas.text(msg[:31], 2, y + 6, C_CYAN)

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
        display_state=None, cart_path=None):
    # Pause gameplay and show the runtime emulator menu (GUI+F7).
    # All actions (cart swap, save/load, reset) are performed directly
    # here; the caller just needs to resume its main loop once this
    # returns.
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
    # Returns (display_state, cart_path), both possibly updated, so the
    # caller can update its own globals.
    import time

    canvas = MenuCanvas(msx_module)
    cursor = 0
    msg = ""

    _draw_runtime_menu(canvas, cursor, msg)
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
            return display_state, cart_path
        elif key == HID_ENTER:
            _wait_key_release(usb_host_mod)
            label = _RUNTIME_ITEMS[cursor]

            if label == "Resume":
                return display_state, cart_path

            elif label == "Swap Cartridge":
                # select_rom() suspends HDMI internally around its own SD
                # directory listing and resumes it for interactive
                # browsing (no SD access happens per-keypress there), so
                # HDMI can stay on for this call.
                #
                # Unlike every other SD-touching action in this menu,
                # select_rom()'s own uos.listdir(rom_dir) used to be called
                # completely unguarded — on marginal SD hardware this can
                # raise OSError (seen in the field as [Errno 5] EIO), which
                # then propagated all the way out of show() and killed the
                # whole run() loop (game session lost) for what should be
                # a recoverable "SD hiccup, try again" case.
                listdir_failed = False
                # Open in the currently-loaded cart's own folder (not
                # always the SD root) — the user can still navigate up
                # past it via ".."/ESC, since rom_dir remains the floor.
                start_dir = cart_path.rsplit('/', 1)[0] if cart_path else None
                try:
                    selected = select_rom(msx_module, rom_dir,
                                          title="Select Cartridge ROM",
                                          usb_host_mod=usb_host_mod,
                                          auto_if_one=False,
                                          timeout_ms=0,
                                          exclude_names=exclude_names,
                                          start_dir=start_dir)
                except OSError as e:
                    selected = None
                    listdir_failed = True
                    msg = f"Directory listing failed: {e}"
                if selected:
                    # HDMI suspended from the moment a file is picked until
                    # the load finishes (success or not) — the "Loading…"
                    # draw right below and the eject/load further down are
                    # exactly the SD-heavy-access-right-after-an-HDMI-mode
                    # -draw pattern that's unreliable on real hardware (see
                    # hdmi_suspend()'s comment); does NOT force LCD mode,
                    # so a display=hdmi (no LCD) setup keeps working the
                    # same way, just without a picture during this window.
                    # LCD suspended too (lcd_suspend()) — see its docstring;
                    # both outputs go quiet for this window (no picture on
                    # either until it resumes), not just HDMI.
                    _prev_hdmi = hdmi_suspend()
                    _prev_lcd  = lcd_suspend()
                    try:
                        # SD reads of cart-sized files take a visible moment
                        # (shared bus with the LCD) — without this, the screen
                        # just freezes on the file list and looks hung.
                        _draw_runtime_menu(canvas, cursor, "Loading…")
                        try:
                            import gc
                            # Eject the previous cart FIRST: if it was a paged
                            # Mega ROM, this drops the GC root reference to its
                            # open flash-cache file object (msx.eject_cart()
                            # closes it and clears msx_cart_file0/1 in
                            # modmsx.c). Only THEN does gc.collect() actually
                            # have anything to reclaim — collecting before the
                            # eject leaves that file object alive and still
                            # fragmenting the heap, which was silently failing
                            # the ~32KB contiguous read below (16KB ROMs mostly
                            # got lucky; 32KB ones reliably didn't).
                            msx_module.eject_cart(0)
                            gc.collect()  # defragment before the cart-sized read
                            log_mem("before Swap Cartridge load_cart_smart()")
                            ok = load_cart_smart(msx_module, 0, selected)
                            if ok:
                                msx_module.reset()
                                cart_path = selected  # own save-state file from here on
                                msg = f"Loaded {selected.rsplit('/',1)[-1]}"
                            else:
                                msg = "load_cart() failed"
                        except Exception as e:
                            # Broad catch: OSError (file missing) and
                            # MemoryError (GC heap too fragmented for the
                            # cart-sized read) are both real possibilities.
                            msg = f"Load failed: {e}"
                    finally:
                        lcd_resume(_prev_lcd)
                        hdmi_resume(_prev_hdmi)
                elif not listdir_failed:
                    msg = ""

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
                _draw_runtime_menu(canvas, cursor, "Saving…")
                try:
                    rotate_and_save_state(msx_module, save_base_for_cart(cart_path, save_path))
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
                base = save_base_for_cart(cart_path, save_path)
                import gc
                gc.collect()  # defragment before compiling msx_save_slots.py
                              # — real-hardware finding: this always happens
                              # mid-gameplay, where cart/emulation state has
                              # already fragmented the heap more than at a
                              # fresh boot.
                import msx_save_slots
                log_mem("after msx_save_slots import")
                chosen = msx_save_slots.select(msx_module, base,
                                               usb_host_mod=usb_host_mod)
                if chosen:
                    _prev_hdmi = hdmi_suspend()
                    _prev_lcd  = lcd_suspend()
                    _draw_runtime_menu(canvas, cursor, "Loading…")
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
                import gc
                gc.collect()  # defragment before compiling
                              # msx_display_settings.py — real-hardware
                              # finding (MemoryError here): this always
                              # happens mid-gameplay, where cart/emulation
                              # state has already fragmented the heap more
                              # than at a fresh boot.
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
            _draw_runtime_menu(canvas, cursor, msg)
