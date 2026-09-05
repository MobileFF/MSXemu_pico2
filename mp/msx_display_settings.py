"""
msx_display_settings.py — unified "Display Settings" runtime menu screen
(display routing / frame skip / LCD panel / rotation / HDMI baud / manual
reinit), split out of msx_menu.py and imported lazily (only when the user
actually opens this menu item) so its compile cost isn't paid at every
boot — see show_emulator_menu()'s "Display Settings" branch in
msx_menu.py.

2026-09-06: this used to be two separate screens ("HDMI Settings":
display/frame_skip, live; "Display Settings": lcd/rotate/hdmi_baud,
restart-only) split back when 'display' had a third 'both' value and a
separate hdmi=0/1 enable flag existed alongside it. Now that LCD/HDMI are
strictly mutually exclusive (see msx_menu.set_display_state()) there's
barely anything left to justify two menus, so they're merged into one.
"""

from msx_menu import (MenuCanvas, C_BLACK, C_YELLOW, C_GREEN, C_WHITE,
                      C_CYAN, C_GRAY, _echo_msg, _wait_key_release, _get_key,
                      HID_UP, HID_DOWN, HID_LEFT, HID_RIGHT, HID_ENTER, HID_ESC,
                      save_config, set_display_state)

_LCD_MODELS = ["ST7796", "ILI9341"]
_HDMI_BAUD_OPTIONS = [5_000_000, 8_000_000, 10_000_000]
_DISPLAY_MODES = ["lcd", "hdmi"]  # mutually exclusive — see set_display_state()
_FRAME_SKIP_MAX = 8

_N_ROWS = 6
_ROW_REINIT = 5  # last row is an action, not a value — see show()'s ENTER handling


def _draw(canvas, cursor, state, msg=""):
    _echo_msg(msg)
    canvas.clear(C_BLACK)
    canvas.rect(0, 0, canvas.W, 12, C_YELLOW, fill=True)
    canvas.text("DISPLAY SETTINGS", 2, 2, C_BLACK)

    rows = [
        f"Display: {state['display'].upper()}",
        f"Frame Skip: {state['frame_skip']}",
        f"LCD Panel: {state['lcd']}",
        f"Rotate: {'180' if state['rotate'] else '0'}",
        f"HDMI Baud: {state['hdmi_baud'] // 1_000_000}MHz",
        f"Reinit {state['display'].upper()} now",
    ]
    y = 20
    for i, label in enumerate(rows):
        if i == cursor:
            canvas.rect(0, y, canvas.W, 10, C_GREEN, fill=True)
            canvas.text(label, 2, y + 1, C_BLACK)
        else:
            canvas.text(label, 2, y + 1, C_WHITE)
        y += 12

    canvas.text("Panel/Rotate/Baud: restart needed", 2, y + 2, C_GRAY)
    if msg:
        canvas.text(msg[:31], 2, y + 13, C_CYAN)

    canvas.hline(0, canvas.H - 21, canvas.W, C_GRAY)
    canvas.text("LEFT/RIGHT:adjust  UP/DOWN:field", 2, canvas.H - 20, C_GRAY)
    canvas.text("ENTER:save/act  ESC:back(no save)", 2, canvas.H - 10, C_GRAY)
    canvas.flush()


def show(msx_module, usb_host_mod, config_path, display_state,
         init_hdmi_output=None, init_lcd_output=None):
    """Unified Display/Frame Skip/LCD Panel/Rotate/HDMI Baud/Reinit editor.

    display_state: dict with 'display' ('lcd'/'hdmi', mutually exclusive
    — see set_display_state()), 'frame_skip' (int), 'lcd' (str, one of
    _LCD_MODELS), 'rotate' (bool), 'hdmi_baud' (int, Hz). Returned
    (possibly modified) so the caller can update its own globals and this
    menu shows the last-picked values if reopened.

    'Display' and 'Frame Skip' take effect immediately (same philosophy
    as _show_audio_settings_menu() in msx_menu.py) and are only persisted
    to msx.ini when ENTER is pressed; 'LCD Panel'/'Rotate'/'HDMI Baud' are
    restart-only (read once at boot).

    init_hdmi_output/init_lcd_output: callbacks taking no args, called
    the moment 'Display' switches to that side — since a boot that
    started on the *other* side never initialized this one's hardware at
    all (see main.py's exclusive boot logic), switching here needs that
    same one-time GPIO/SPI setup. Both are safe/idempotent to call more
    than once (harmless if that side was already initialized, e.g. at
    boot) — also reused directly by the last row ("Reinit ... now"),
    added after real-hardware reports of the HDMI picture going black
    and staying that way during long play sessions even though the menu
    itself (which uses a different, palette-independent send path) still
    draws fine — symptoms consistent with the receiver's color palette
    table (sent once, at init — see msx_send_hdmi_palette()) getting
    desynced somehow over a long session. Re-running the same init
    resends it, a plausible real fix and not just a cosmetic no-op;
    re-running the LCD's init similarly re-issues its full panel reset
    sequence, a real recovery attempt for a wedged panel/peripheral.
    """
    import time

    canvas = MenuCanvas(msx_module)
    cursor = 0
    state = dict(display_state)
    msg = ""

    _draw(canvas, cursor, state, msg)
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
            cursor = (cursor - 1) % _N_ROWS if key == HID_UP else (cursor + 1) % _N_ROWS
        elif key == HID_LEFT or key == HID_RIGHT:
            sign = -1 if key == HID_LEFT else 1
            if cursor == 0:
                idx = _DISPLAY_MODES.index(state['display'])
                idx = (idx + sign) % len(_DISPLAY_MODES)
                state['display'] = _DISPLAY_MODES[idx]
                set_display_state(state['display'])
                if state['display'] == 'hdmi':
                    if init_hdmi_output:
                        init_hdmi_output()
                elif init_lcd_output:
                    init_lcd_output()
            elif cursor == 1:
                state['frame_skip'] = max(1, min(_FRAME_SKIP_MAX,
                                                  state['frame_skip'] + sign))
            elif cursor == 2:
                idx = _LCD_MODELS.index(state['lcd']) if state['lcd'] in _LCD_MODELS else 0
                idx = (idx + sign) % len(_LCD_MODELS)
                state['lcd'] = _LCD_MODELS[idx]
            elif cursor == 3:
                state['rotate'] = not state['rotate']
            elif cursor == 4:
                idx = (_HDMI_BAUD_OPTIONS.index(state['hdmi_baud'])
                       if state['hdmi_baud'] in _HDMI_BAUD_OPTIONS else 0)
                idx = (idx + sign) % len(_HDMI_BAUD_OPTIONS)
                state['hdmi_baud'] = _HDMI_BAUD_OPTIONS[idx]
            # cursor == _ROW_REINIT: an action, not a value — LEFT/RIGHT
            # do nothing there, see the ENTER handling below.
        elif key == HID_ENTER:
            if cursor == _ROW_REINIT:
                _wait_key_release(usb_host_mod)
                if state['display'] == 'hdmi':
                    if init_hdmi_output:
                        init_hdmi_output()
                elif init_lcd_output:
                    init_lcd_output()
                msg = f"{state['display'].upper()} reinitialized"
                # Deliberately does NOT return — stays on this screen so
                # the user can see whether the picture actually came back
                # before backing out with ESC.
            else:
                _wait_key_release(usb_host_mod)
                if config_path is None:
                    msg = "No config path — not saved"
                else:
                    try:
                        save_config(config_path, {
                            'display': state['display'],
                            'hdmi_frame_skip': str(state['frame_skip']),
                            'lcd': state['lcd'],
                            'rotate': '180' if state['rotate'] else '0',
                            'hdmi_baud': str(state['hdmi_baud']),
                        })
                        msg = "Saved to msx.ini"
                    except Exception as e:
                        msg = f"Save failed: {e}"
                _draw(canvas, cursor, state, msg)
                time.sleep_ms(800)
                return state
        elif key == HID_ESC:
            _wait_key_release(usb_host_mod)
            return state

        _draw(canvas, cursor, state, msg)
