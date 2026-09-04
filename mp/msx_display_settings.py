"""
msx_display_settings.py — "Display Settings" runtime menu screen
(LCD panel / rotation / HDMI baud / exclusive boot), split out of
msx_menu.py and imported lazily (only when the user actually opens this
menu item) so its compile cost isn't paid at every boot — see
show_emulator_menu()'s "Display Settings" branch in msx_menu.py.
"""

from msx_menu import (MenuCanvas, C_BLACK, C_YELLOW, C_GREEN, C_WHITE,
                      C_CYAN, C_GRAY, _echo_msg, _wait_key_release, _get_key,
                      HID_UP, HID_DOWN, HID_LEFT, HID_RIGHT, HID_ENTER, HID_ESC,
                      save_config)

_LCD_MODELS = ["ST7796", "ILI9341"]
_HDMI_BAUD_OPTIONS = [5_000_000, 8_000_000, 10_000_000]


def _draw(canvas, cursor, state, msg=""):
    _echo_msg(msg)
    canvas.clear(C_BLACK)
    canvas.rect(0, 0, canvas.W, 12, C_YELLOW, fill=True)
    canvas.text("DISPLAY SETTINGS", 2, 2, C_BLACK)

    rows = [
        f"LCD Panel: {state['lcd']}",
        f"Rotate: {'180' if state['rotate'] else '0'}",
        f"HDMI Baud: {state['hdmi_baud'] // 1_000_000}MHz",
        f"Exclusive Boot: {'On' if state['boot_exclusive'] else 'Off'}",
    ]
    y = 20
    for i, label in enumerate(rows):
        if i == cursor:
            canvas.rect(0, y, canvas.W, 10, C_GREEN, fill=True)
            canvas.text(label, 2, y + 1, C_BLACK)
        else:
            canvas.text(label, 2, y + 1, C_WHITE)
        y += 12

    canvas.text("Excl.Boot needs HDMI Settings:", 2, y + 2, C_GRAY)
    canvas.text("Display=LCD/HDMI (not Both)", 2, y + 11, C_GRAY)
    canvas.text("(restart to apply)", 2, y + 22, C_CYAN)
    if msg:
        canvas.text(msg[:31], 2, y + 34, C_CYAN)

    canvas.hline(0, canvas.H - 21, canvas.W, C_GRAY)
    canvas.text("LEFT/RIGHT:adjust  UP/DOWN:field", 2, canvas.H - 20, C_GRAY)
    canvas.text("ENTER:save  ESC:back(no save)", 2, canvas.H - 10, C_GRAY)
    canvas.flush()


def show(msx_module, usb_host_mod, config_path, display_state):
    """LCD panel/rotation/HDMI baud/exclusive-boot editor — restart-only
    (none take effect live). display_state: dict with 'lcd' (str, one of
    _LCD_MODELS), 'rotate' (bool), 'hdmi_baud' (int, Hz), 'boot_exclusive'
    (bool). Returned so re-opening this menu shows the last-picked values."""
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
            cursor = (cursor - 1) % 4 if key == HID_UP else (cursor + 1) % 4
        elif key == HID_LEFT or key == HID_RIGHT:
            sign = -1 if key == HID_LEFT else 1
            if cursor == 0:
                idx = _LCD_MODELS.index(state['lcd']) if state['lcd'] in _LCD_MODELS else 0
                idx = (idx + sign) % len(_LCD_MODELS)
                state['lcd'] = _LCD_MODELS[idx]
            elif cursor == 1:
                state['rotate'] = not state['rotate']
            elif cursor == 2:
                idx = (_HDMI_BAUD_OPTIONS.index(state['hdmi_baud'])
                       if state['hdmi_baud'] in _HDMI_BAUD_OPTIONS else 0)
                idx = (idx + sign) % len(_HDMI_BAUD_OPTIONS)
                state['hdmi_baud'] = _HDMI_BAUD_OPTIONS[idx]
            else:
                state['boot_exclusive'] = not state['boot_exclusive']
        elif key == HID_ENTER:
            _wait_key_release(usb_host_mod)
            if config_path is None:
                msg = "No config path — not saved"
            else:
                try:
                    save_config(config_path, {
                        'lcd': state['lcd'],
                        'rotate': '180' if state['rotate'] else '0',
                        'hdmi_baud': str(state['hdmi_baud']),
                        'boot_exclusive': '1' if state['boot_exclusive'] else '0',
                    })
                    msg = "Saved — restart to apply"
                except Exception as e:
                    msg = f"Save failed: {e}"
            _draw(canvas, cursor, state, msg)
            time.sleep_ms(1000)
            return state
        elif key == HID_ESC:
            _wait_key_release(usb_host_mod)
            return state

        _draw(canvas, cursor, state, msg)
