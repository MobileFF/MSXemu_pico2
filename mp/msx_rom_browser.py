"""
msx_rom_browser.py — interactive ROM file selector with folder navigation,
split out of msx_menu.py and imported lazily (only when actually needed:
no 'cart' line in msx.ini at boot, or "Swap Cartridge" from the runtime
menu) so its compile cost isn't paid at every boot.
"""

import uos

from msx_menu import (MenuCanvas, C_BLACK, C_GREEN, C_WHITE, C_GRAY, C_RED,
                      C_YELLOW, C_CYAN, _draw_message, _get_key,
                      _wait_key_release, HID_UP, HID_DOWN, HID_ENTER, HID_ESC,
                      hdmi_suspend, hdmi_resume, lcd_suspend, lcd_resume)


def _list_dir_entries(directory, exclude_names=(), ext='.rom'):
    """[(name, is_dir), ...] for `directory`: dirs first, then matching
    files (by `ext`, default .ROM cartridges; pass '.dsk' for the virtual
    FDD's disk-image browser), each sorted. exclude_names filters files
    only, never dirs."""
    try:
        entries = list(uos.ilistdir(directory))
    except (OSError, UnicodeError):
        # UnicodeError: a filename in this directory isn't valid UTF-8
        # (e.g. left over from a different OS/filesystem) — MicroPython's
        # ilistdir() raises this while decoding it, not an OSError, so it
        # needs its own catch. Either way, treat this directory as
        # unreadable/skippable rather than crashing the whole browse —
        # real-hardware finding, 2026-09.
        return []
    # type & 0x4000 = dir (S_IFDIR)
    dirs = sorted(e[0] for e in entries if (e[1] & 0x4000))
    files = sorted(e[0] for e in entries
                   if not (e[1] & 0x4000) and e[0].lower().endswith(ext)
                   and e[0].lower() not in exclude_names)
    return [(d, True) for d in dirs] + [(f, False) for f in files]


def _draw_file_list(canvas, title, items, selected, scroll, ext='.rom'):
    """Render a scrollable file list menu."""
    canvas.clear(C_BLACK)

    canvas.rect(0, 0, canvas.W, 12, C_GREEN, fill=True)
    canvas.text(title[:31], 2, 2, C_BLACK)

    LINE_H   = 10
    LIST_Y   = 14
    MAX_ROWS = (canvas.H - LIST_Y - 12) // LINE_H

    for i in range(MAX_ROWS):
        idx = scroll + i
        if idx >= len(items):
            break
        y = LIST_Y + i * LINE_H
        if idx == selected:
            canvas.rect(0, y, canvas.W, LINE_H, C_GREEN, fill=True)
            canvas.text(items[idx][:31], 2, y + 1, C_BLACK)
        else:
            color = C_WHITE if (i % 2 == 0) else C_GRAY
            canvas.text(items[idx][:31], 2, y + 1, color)

    canvas.hline(0, canvas.H - 11, canvas.W, C_GRAY)
    if items:
        status = f"{selected + 1}/{len(items)}"
        canvas.text("ENTER:select  ESC:skip", 2, canvas.H - 10, C_GRAY)
        canvas.text(status, canvas.W - len(status) * 8 - 2, canvas.H - 10, C_CYAN)
    else:
        canvas.text(f"No {ext.upper()} files found", 2, canvas.H - 10, C_RED)

    canvas.flush()


def select(msx_module, directory, title="Select ROM",
          usb_host_mod=None, timeout_ms=5000,
          exclude_names=(), start_dir=None, ext='.rom'):
    """Interactive file selector with folder navigation. `directory`
    is the floor of navigation (ENTER on ".."/ESC there cancels) — always
    the caller's real ROM root. `start_dir`, if given, is just where
    browsing initially opens (e.g. the currently-loaded cart's own
    folder) — the user can still navigate up past it to `directory`.
    `ext` selects which file extension is browsable (default '.rom' for
    cartridges; '.dsk' for the virtual FDD's disk-image browser — see
    msx_fdd.py). Returns the selected path, or None if cancelled/no
    keyboard/nothing found/timed out (all treated as "no selection" by
    callers — see the no-keyboard and timeout comments below)."""
    import time

    root_dir = directory
    canvas = MenuCanvas(msx_module)

    def _listing(d):
        # Suspended only around the actual SD listing; the browsing loop
        # itself just redraws `entries` from RAM (no per-keypress SD access).
        # No need to also drain a pending DMA transfer here — flush()
        # (msx_menu.py) already blocks until fully idle (both LCD and
        # HDMI) before _draw_file_list() returns, menu/UI frames having no
        # FPS target to protect unlike gameplay's — see
        # msx_render_to_hdmi_raw332()'s comment in msx_core.c.
        _prev_hdmi = hdmi_suspend()
        _prev_lcd  = lcd_suspend()
        try:
            return _list_dir_entries(d, exclude_names=exclude_names, ext=ext)
        finally:
            lcd_resume(_prev_lcd)
            hdmi_resume(_prev_hdmi)

    # No keyboard: no way to navigate/cancel interactively, so there's
    # nothing to wait for — cancel immediately (same "no selection ->
    # BASIC" outcome as the keyboard-present timeout below). 2026-09-13:
    # this used to auto-select the first ROM found by a recursive search
    # (or the only ROM present, via the auto_if_one parameter this
    # replaced), on the reasoning that a headless setup can't pick
    # anything anyway — but same objection as the timeout fix below: no
    # 'cart'/'disk' configured should mean BASIC, not "whichever ROM
    # happens to sort first" (or "happens to be the only one present").
    if usb_host_mod is None:
        return None

    cur_dir = start_dir if start_dir else directory
    entries = _listing(cur_dir)

    if not entries and cur_dir == root_dir:
        _draw_message(canvas, title, f"No {ext.upper()} files found in", cur_dir, C_RED)
        time.sleep_ms(2000)
        return None

    def _display_items():
        items = [".."] if cur_dir != root_dir else []
        items += [(n + "/") if is_dir else n for n, is_dir in entries]
        return items

    selected = 0
    scroll   = 0
    MAX_ROWS = (MenuCanvas.H - 26) // 10

    _draw_file_list(canvas, title, _display_items(), selected, scroll, ext=ext)
    _wait_key_release(usb_host_mod)

    last_key  = 0
    deadline  = (time.ticks_ms() + timeout_ms) if timeout_ms > 0 else None

    while True:
        time.sleep_ms(30)
        key = _get_key(usb_host_mod)
        has_updir = (cur_dir != root_dir)

        # Timeout, no key activity: cancel (same as ESC) rather than
        # auto-loading whatever happens to be highlighted. 2026-09-13:
        # this used to auto-load the current/first ROM after timeout_ms of
        # no input — meant as a headless-boot convenience, but with a
        # keyboard present (this branch is unreachable without one — see
        # the no-keyboard recursive-search path above) it meant an
        # unattended boot with no 'cart'/'disk' configured would silently
        # load *some* ROM instead of the expected "no selection → BASIC"
        # outcome, purely because whoever happened to be near the machine
        # didn't press a key in time. Caller already treats None exactly
        # like an explicit ESC (main.py: "No cartridge — booting MSX
        # BASIC" / msx_fdd's "no disk image" message).
        if deadline is not None and key == 0:
            if time.ticks_diff(deadline, time.ticks_ms()) <= 0:
                _wait_key_release(usb_host_mod)
                return None

        if key == last_key:
            continue   # still held — ignore repeat for now
        last_key = key

        if key == 0:
            continue

        # Any key press resets the timeout
        deadline = (time.ticks_ms() + timeout_ms) if timeout_ms > 0 else None
        display_items = _display_items()

        if key == HID_UP:
            if selected > 0:
                selected -= 1
                if selected < scroll:
                    scroll = selected
        elif key == HID_DOWN:
            if selected < len(display_items) - 1:
                selected += 1
                if selected >= scroll + MAX_ROWS:
                    scroll = selected - MAX_ROWS + 1
        elif key == HID_ENTER:
            if has_updir and selected == 0:
                cur_dir = cur_dir.rsplit('/', 1)[0]
                entries = _listing(cur_dir)
                selected = 0
                scroll = 0
            else:
                idx = selected - (1 if has_updir else 0)
                name, is_dir = entries[idx]
                if is_dir:
                    cur_dir = cur_dir + "/" + name
                    entries = _listing(cur_dir)
                    selected = 0
                    scroll = 0
                else:
                    _wait_key_release(usb_host_mod)
                    return cur_dir + "/" + name
        elif key == HID_ESC:
            # Always cancels immediately, regardless of how many folder
            # levels deep browsing has gone (navigate up via ".." + ENTER
            # instead) — previously ESC walked up one level at a time when
            # start_dir put cur_dir below root_dir, so cancelling out of a
            # subfolder needed one ESC per level. Real-hardware finding,
            # 2026-09.
            _wait_key_release(usb_host_mod)
            return None

        _draw_file_list(canvas, title, _display_items(), selected, scroll, ext=ext)
