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


def _list_dir_entries(directory, exclude_names=()):
    """[(name, is_dir), ...] for `directory`: dirs first, then .ROM files,
    each sorted. exclude_names filters files only, never dirs."""
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
    roms = sorted(e[0] for e in entries
                  if not (e[1] & 0x4000) and e[0].lower().endswith('.rom')
                  and e[0].lower() not in exclude_names)
    return [(d, True) for d in dirs] + [(r, False) for r in roms]


def _find_first_rom_recursive(directory, exclude_names=(), max_depth=6):
    """Depth-first (files before subfolders) search for the first .ROM
    under `directory` — used when there's no keyboard to browse with.
    max_depth bounds runaway recursion. Returns a path or None."""
    if max_depth <= 0:
        return None
    entries = _list_dir_entries(directory, exclude_names=exclude_names)
    for name, is_dir in entries:
        if not is_dir:
            return directory + "/" + name
    for name, is_dir in entries:
        if is_dir:
            found = _find_first_rom_recursive(
                directory + "/" + name, exclude_names, max_depth - 1)
            if found:
                return found
    return None


def _draw_file_list(canvas, title, items, selected, scroll):
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
        canvas.text("No .ROM files found", 2, canvas.H - 10, C_RED)

    canvas.flush()


def select(msx_module, directory, title="Select ROM",
          usb_host_mod=None, auto_if_one=True, timeout_ms=5000,
          exclude_names=(), start_dir=None):
    """Interactive ROM file selector with folder navigation. `directory`
    is the floor of navigation (ENTER on ".."/ESC there cancels) — always
    the caller's real ROM root. `start_dir`, if given, is just where
    browsing initially opens (e.g. the currently-loaded cart's own
    folder) — the user can still navigate up past it to `directory`.
    auto_if_one auto-selects when the initial listing has exactly one
    ROM and no subfolders. Returns the selected path, or None if
    cancelled/nothing found."""
    import time

    root_dir = directory
    canvas = MenuCanvas(msx_module)

    def _listing(d):
        # Suspended only around the actual SD listing; the browsing loop
        # itself just redraws `entries` from RAM (no per-keypress SD access).
        _prev_hdmi = hdmi_suspend()
        _prev_lcd  = lcd_suspend()
        try:
            return _list_dir_entries(d, exclude_names=exclude_names)
        finally:
            lcd_resume(_prev_lcd)
            hdmi_resume(_prev_hdmi)

    # No keyboard: can't navigate, so search recursively instead of just
    # the top level.
    if usb_host_mod is None:
        found = _find_first_rom_recursive(directory, exclude_names=exclude_names)
        if found:
            _draw_message(canvas, title, found[len(directory) + 1:],
                          "No keyboard — auto-selecting", C_YELLOW)
            time.sleep_ms(1500)
            return found
        _draw_message(canvas, title, "No .ROM files found under", directory, C_RED)
        time.sleep_ms(2000)
        return None

    cur_dir = start_dir if start_dir else directory
    entries = _listing(cur_dir)

    if auto_if_one and len(entries) == 1 and not entries[0][1]:
        name = entries[0][0]
        _draw_message(canvas, title, f"Auto: {name}", "Loading…", C_GREEN)
        time.sleep_ms(800)
        return cur_dir + "/" + name

    if not entries and cur_dir == root_dir:
        _draw_message(canvas, title, "No .ROM files found in", cur_dir, C_RED)
        time.sleep_ms(2000)
        return None

    def _display_items():
        items = [".."] if cur_dir != root_dir else []
        items += [(n + "/") if is_dir else n for n, is_dir in entries]
        return items

    selected = 0
    scroll   = 0
    MAX_ROWS = (MenuCanvas.H - 26) // 10

    _draw_file_list(canvas, title, _display_items(), selected, scroll)
    _wait_key_release(usb_host_mod)

    last_key  = 0
    deadline  = (time.ticks_ms() + timeout_ms) if timeout_ms > 0 else None

    while True:
        time.sleep_ms(30)
        key = _get_key(usb_host_mod)
        has_updir = (cur_dir != root_dir)

        # Timeout, no key activity: load current item if it's a ROM, else
        # fall back to a recursive search from the root.
        if deadline is not None and key == 0:
            if time.ticks_diff(deadline, time.ticks_ms()) <= 0:
                idx = selected - (1 if has_updir else 0)
                if idx >= 0 and idx < len(entries) and not entries[idx][1]:
                    path = cur_dir + "/" + entries[idx][0]
                    _draw_message(canvas, title, f"Auto: {entries[idx][0]}",
                                  "No input — loading…", C_YELLOW)
                    time.sleep_ms(600)
                    return path
                found = _find_first_rom_recursive(root_dir, exclude_names=exclude_names)
                if found:
                    _draw_message(canvas, title, found[len(root_dir) + 1:],
                                  "No input — loading…", C_YELLOW)
                    time.sleep_ms(600)
                    return found
                deadline = None  # nothing loadable anywhere — keep waiting

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

        _draw_file_list(canvas, title, _display_items(), selected, scroll)
