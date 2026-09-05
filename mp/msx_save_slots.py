"""
msx_save_slots.py — Load State slot picker for the rotating save-state
history (msx_menu.MAX_SAVE_SLOTS, see rotate_and_save_state()), split out
and imported lazily like msx_rom_browser.py/msx_display_settings.py so
its compile cost isn't paid at every boot.
"""

from msx_menu import (MenuCanvas, C_BLACK, C_GREEN, C_WHITE, C_GRAY, C_RED,
                      _get_key, _wait_key_release, HID_UP, HID_DOWN,
                      HID_ENTER, HID_ESC, list_save_slots, save_slot_path,
                      hdmi_suspend, hdmi_resume, lcd_suspend, lcd_resume)


def _draw(canvas, title, slots, selected):
    canvas.clear(C_BLACK)
    canvas.rect(0, 0, canvas.W, 12, C_GREEN, fill=True)
    canvas.text(title[:31], 2, 2, C_BLACK)

    LINE_H, LIST_Y = 10, 14
    for i, slot in enumerate(slots):
        y = LIST_Y + i * LINE_H
        label = f"Slot {slot}" + (" (latest)" if slot == 0 else "")
        if i == selected:
            canvas.rect(0, y, canvas.W, LINE_H, C_GREEN, fill=True)
            canvas.text(label, 2, y + 1, C_BLACK)
        else:
            color = C_WHITE if (i % 2 == 0) else C_GRAY
            canvas.text(label, 2, y + 1, color)

    canvas.hline(0, canvas.H - 11, canvas.W, C_GRAY)
    if slots:
        canvas.text("ENTER:load  ESC:cancel", 2, canvas.H - 10, C_GRAY)
    else:
        canvas.text("No saved states", 2, canvas.H - 10, C_RED)
    canvas.flush()


def select(msx_module, base, title="Load State", usb_host_mod=None):
    """Interactive save-slot picker (at most msx_menu.MAX_SAVE_SLOTS
    entries — no scrolling needed). Returns the chosen slot's full path,
    or None if cancelled / no saves exist."""
    import time

    _prev_hdmi = hdmi_suspend()
    _prev_lcd  = lcd_suspend()
    try:
        slots = list_save_slots(base)
    finally:
        lcd_resume(_prev_lcd)
        hdmi_resume(_prev_hdmi)

    canvas = MenuCanvas(msx_module)

    if not slots:
        _draw(canvas, title, slots, 0)
        time.sleep_ms(1500)
        return None

    # No keyboard: just load the most recent slot — same fallback
    # philosophy as select_rom()'s no-keyboard path.
    if usb_host_mod is None:
        return save_slot_path(base, slots[0])

    selected = 0
    _draw(canvas, title, slots, selected)
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

        if key == HID_UP:
            selected = (selected - 1) % len(slots)
        elif key == HID_DOWN:
            selected = (selected + 1) % len(slots)
        elif key == HID_ENTER:
            _wait_key_release(usb_host_mod)
            return save_slot_path(base, slots[selected])
        elif key == HID_ESC:
            _wait_key_release(usb_host_mod)
            return None

        _draw(canvas, title, slots, selected)
