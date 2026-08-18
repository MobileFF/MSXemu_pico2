"""
msx_menu.py — ROM selection menu for MSX1 emulator
Renders directly into msx.get_framebuf() using MicroPython's framebuf module.

Color note: msx.get_framebuf() holds RGB565 in big-endian byte order (high
byte at lower address) to match direct SPI DMA output.  MicroPython's
framebuf.RGB565 stores colors as native uint16_t, which on little-endian ARM
means low byte first in memory — the opposite convention.  Therefore colors
must be byte-swapped before passing to framebuf methods.

Use the rgb() helper below for all colors.
"""

import uos
import framebuf

try:
    import msx as _msx
except ImportError:
    _msx = None


# ---------------------------------------------------------------------------
# Color helpers
# ---------------------------------------------------------------------------

def rgb(r, g, b):
    """RGB888 → 565be value suitable for our framebuf / MicroPython framebuf."""
    v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
    # byte-swap so memory layout is [high_byte, low_byte] for DMA → ILI9341
    return ((v >> 8) | (v << 8)) & 0xFFFF


C_BLACK  = rgb(0,   0,   0)
C_WHITE  = rgb(255, 255, 255)
C_GREEN  = rgb(0,   220, 0)
C_YELLOW = rgb(255, 255, 0)
C_GRAY   = rgb(128, 128, 128)
C_CYAN   = rgb(0,   220, 220)
C_RED    = rgb(220, 0,   0)


# ---------------------------------------------------------------------------
# HDMI bridge state (hdmi_bridge/README.md), mirrored here from main.py's
# globals so MenuCanvas.flush() can also update the HDMI output — otherwise
# every screen in this file (runtime menu, Audio/HDMI Settings, ROM
# selector, boot error screen) would only ever repaint the LCD, leaving
# HDMI frozen on the last gameplay frame while a menu is open. main.py
# calls set_display_state() once at boot and again whenever the HDMI
# Settings menu changes these values.
# ---------------------------------------------------------------------------

_display_mode = 'both'
_hdmi_enabled = False


def set_display_state(display_mode, hdmi_enabled):
    global _display_mode, _hdmi_enabled
    _display_mode = display_mode
    _hdmi_enabled = hdmi_enabled


def hdmi_suspend():
    """Temporarily disable HDMI output (leaving _display_mode untouched)
    around an SD-heavy operation (SD listdir()/read()/write()) — HDMI's
    SPI mode 3 conflicts with the mode 0 that LCD/SD both need, and
    switching between them right around SD access is unreliable on real
    hardware ("frequent same-peripheral SPI mode switching on RP2350
    remains an open problem" — see msx_core.c). Deliberately does NOT
    force display_mode to 'lcd': a display=hdmi setup (no LCD in use at
    all) must keep working without an LCD ever being required — while
    HDMI is suspended, MenuCanvas.flush()'s own existing safety fallback
    (use_lcd = ... or not use_hdmi) harmlessly no-ops the LCD render calls
    if no LCD is actually wired, and HDMI output resumes automatically as
    soon as hdmi_resume() is called. Returns the previous _hdmi_enabled
    value; pass it to hdmi_resume() when the SD-heavy operation is done.
    """
    global _hdmi_enabled
    prev = _hdmi_enabled
    _hdmi_enabled = False
    return prev


def hdmi_resume(prev_enabled):
    """Restore the _hdmi_enabled value hdmi_suspend() returned."""
    global _hdmi_enabled
    _hdmi_enabled = prev_enabled


# ---------------------------------------------------------------------------
# Low-level draw helpers (write directly into the MSX framebuf)
# ---------------------------------------------------------------------------

class MenuCanvas:
    """Thin wrapper: exposes framebuf.FrameBuffer on the MSX C framebuf."""

    W = 256
    H = 192

    def __init__(self, msx_module):
        self._msx = msx_module
        self._buf = msx_module.get_framebuf()   # bytearray view of C memory
        self._fb  = framebuf.FrameBuffer(self._buf, self.W, self.H,
                                          framebuf.RGB565)

    def clear(self, color=C_BLACK):
        self._fb.fill(color)

    def text(self, s, x, y, color=C_WHITE):
        self._fb.text(s, x, y, color)

    def hline(self, x, y, w, color=C_GRAY):
        self._fb.hline(x, y, w, color)

    def rect(self, x, y, w, h, color, fill=False):
        if fill:
            self._fb.fill_rect(x, y, w, h, color)
        else:
            self._fb.rect(x, y, w, h, color)

    def flush(self):
        # 1:1 native — matches main.py's gameplay render path exactly
        # (same centered 256x192 window). Using the 1.5x scaled
        # render_to_display() here left stale menu pixels in the border
        # area once gameplay started, since main.py only ever repaints
        # the smaller centered window.
        #
        # Mirrors main.py's main-loop display-mode logic (see
        # set_display_state() above) so menus/error screens show up on
        # whichever output(s) are actually active, same as gameplay.
        # Safety fallback: always use the LCD if HDMI isn't actually usable
        # right now (e.g. HDMI just toggled Off from this very menu while
        # display was still 'hdmi') — otherwise flush() would do nothing at
        # all and the screen would freeze with no way to see what's
        # happening, even though the emulator keeps running (found on real
        # hardware navigating this exact menu).
        use_hdmi = _hdmi_enabled and _display_mode in ('both', 'hdmi')
        use_lcd  = _display_mode in ('both', 'lcd') or not use_hdmi
        if use_lcd:
            self._msx.render_to_display_1to1()
            self._msx.wait_display()
        if use_hdmi:
            # raw332, not render_to_hdmi(): menus draw arbitrary UI colors
            # (borders, highlights) that aren't limited to the MSX's 16
            # game-palette colors, so the 4-bit palette-lookup path would
            # render any non-matching color as black (found on real
            # hardware — see hdmi_bridge/README.md's Phase 4 section).
            self._msx.render_to_hdmi_raw332()


# ---------------------------------------------------------------------------
# File browser
# ---------------------------------------------------------------------------

def _list_roms(directory, exclude_names=()):
    """Return sorted list of .ROM / .rom files in directory, excluding exclude_names."""
    try:
        entries = uos.listdir(directory)
    except OSError:
        return []
    roms = [e for e in entries
            if e.lower().endswith('.rom') and e.lower() not in exclude_names]
    roms.sort()
    return roms


def _draw_file_list(canvas, title, items, selected, scroll):
    """Render a scrollable file list menu."""
    canvas.clear(C_BLACK)

    # Title bar
    canvas.rect(0, 0, canvas.W, 12, C_GREEN, fill=True)
    canvas.text(title[:31], 2, 2, C_BLACK)

    # File list (max 15 items visible, 8px font + 2px gap)
    LINE_H   = 10
    LIST_Y   = 14
    MAX_ROWS = (canvas.H - LIST_Y - 12) // LINE_H  # leave room for footer

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

    # Footer
    canvas.hline(0, canvas.H - 11, canvas.W, C_GRAY)
    if items:
        status = f"{selected + 1}/{len(items)}"
        canvas.text("ENTER:select  ESC:skip", 2, canvas.H - 10, C_GRAY)
        canvas.text(status, canvas.W - len(status) * 8 - 2, canvas.H - 10, C_CYAN)
    else:
        canvas.text("No .ROM files found", 2, canvas.H - 10, C_RED)

    canvas.flush()


def _draw_message(canvas, title, line1, line2="", color=C_WHITE):
    canvas.clear(C_BLACK)
    canvas.rect(0, 0, canvas.W, 12, C_CYAN, fill=True)
    canvas.text(title[:31], 2, 2, C_BLACK)
    canvas.text(line1[:31], 2, 20, color)
    if line2:
        canvas.text(line2[:31], 2, 32, C_GRAY)
    canvas.flush()


# ---------------------------------------------------------------------------
# Keyboard integration
# ---------------------------------------------------------------------------

# HID keycodes used for navigation
HID_UP    = 0x52
HID_DOWN  = 0x51
HID_LEFT  = 0x50
HID_RIGHT = 0x4F
HID_ENTER = 0x28
HID_ESC   = 0x29

def _get_key(usb_host_mod):
    """Poll USB HID and return a single keycode (or 0 if none / no host)."""
    if usb_host_mod is None:
        return 0
    try:
        report = usb_host_mod.get_hid_report()
    except Exception:
        return 0
    if report and len(report) >= 8:
        for i in range(2, 8):
            k = report[i]
            if k != 0:
                return k
    return 0


def _wait_key_release(usb_host_mod):
    """Spin until all keys are released (debounce)."""
    import time
    time.sleep_ms(80)
    while _get_key(usb_host_mod) != 0:
        time.sleep_ms(20)


# ---------------------------------------------------------------------------
# Save state (shared by main.py's F5/F8 hotkeys and the runtime menu)
# ---------------------------------------------------------------------------

SAVE_HEADER_SIZE = 64  # must match msx_core.h's MSX_SAVE_HDR_SZ


def save_state_to(msx_module, path):
    """
    Write CPU/VDP/mapper header + RAM + VRAM to `path`.

    Deliberately does NOT build one big (~80KB) in-memory blob: the header
    (64 bytes, msx.get_state_header()) and the live RAM/VRAM
    (msx.get_ram_view() / get_vram_view(), zero-copy views of memory the
    emulator already has) are written as separate pieces, so this never
    needs a large contiguous allocation from MicroPython's GC heap — which
    reliably fails with MemoryError under normal gameplay/menu use even
    with 100+KB nominally free, since that heap doesn't compact.

    VRAM (pattern/name/color tables, sprites) is included so the screen is
    correct immediately on load instead of looking corrupted until the
    game's own code next redraws it.
    """
    header = msx_module.get_state_header()
    if not header:
        raise OSError("get_state_header() failed")
    with open(path, 'wb') as f:
        f.write(header)
        # Chunked (write_chunked(), not one f.write(64KB-view)) — a single
        # large multi-block SD write was observed, on the read side of this
        # exact pattern, to be unreliable on real hardware (see
        # readinto_chunked()'s comment above); write the same way for
        # symmetry/safety even though this side hasn't failed yet.
        write_chunked(f, msx_module.get_ram_view())
        write_chunked(f, msx_module.get_vram_view())


def load_state_from(msx_module, path):
    """
    Read CPU/VDP/mapper header + RAM + VRAM from `path` (written by
    save_state_to()). Returns True on success. Same zero-large-allocation
    rationale as save_state_to(): the header is tiny, RAM/VRAM are read
    directly into the emulator's own live memory via readinto().
    """
    with open(path, 'rb') as f:
        header = f.read(SAVE_HEADER_SIZE)
        if not msx_module.set_state_header(header):
            return False
        # Chunked, not one big readinto() — see readinto_chunked()'s
        # comment above (same real-hardware SD reliability issue that hit
        # the BIOS/cart loads).
        ram_view = msx_module.get_ram_view()
        vram_view = msx_module.get_vram_view()
        n_ram = readinto_chunked(f, ram_view, len(ram_view))
        n_vram = readinto_chunked(f, vram_view, len(vram_view))
    return n_ram == msx_module.RAM_SIZE and n_vram == msx_module.VRAM_SIZE


# ---------------------------------------------------------------------------
# Cartridge loading (small ROMs in-RAM, Mega ROMs SD-backed/paged)
# ---------------------------------------------------------------------------

# ROMs at or below this size use the simple in-RAM path (msx.load_cart());
# larger ones use msx.load_cart_paged() so a 128KB-1MB Mega ROM never needs
# to fit in RAM — see msx_core.h's Mega ROM comment block. This matches
# detect_mapper()'s own boundary (a ROM this small can't be bank-switched
# anyway, so there's nothing paging would buy).
_CART_INRAM_MAX = 0x8000  # 32KB

# BIOS loading (main.py's load_bios_file()) and in-RAM cart loading
# (load_cart_smart() below) both read straight into a zero-copy view of
# already-resident C-side memory (msx.get_bios_view() / msx.cart_alloc())
# now, needing no Python-owned scratch buffer at all — this board's GC
# heap is tight enough that even a *shared, lazily-allocated* ~32KB
# scratch bytearray was observed to fail allocation on real hardware right
# after gc.collect(), and even right at module-import time before that
# (compiling this module's own bytecode already uses a meaningful chunk of
# the GC heap, so an eager 32KB allocation immediately after failed
# outright: "MemoryError ... allocating 32768 bytes" at "msx_menu.py, line
# NNN, in <module>"). The only remaining use for a scratch buffer here is
# the small (<=8KB) Mega ROM mapper-detection prefix read below, which
# doesn't have a natural C-side destination to read directly into (it's
# just a peek at the ROM header before deciding how to load it) — kept
# lazy (allocated on first actual use, not at import time) for the same
# reason, but sized to just 8KB now that nothing here needs 32KB anymore.
_PREFIX_BUF_SIZE = 8192
_prefix_buf = None


def get_rom_load_buf():
    """Return the shared Mega-ROM-mapper-detection scratch buffer (8KB),
    allocating it on first call. See the comment above for why this is
    lazy rather than a module-level `= bytearray(...)`."""
    global _prefix_buf
    if _prefix_buf is None:
        _prefix_buf = bytearray(_PREFIX_BUF_SIZE)
    return _prefix_buf


# A single f.readinto(buf) call for a large read was observed on real
# hardware to raise OSError from sdcard.py's readblocks() (a multi-block
# SD read, CMD18, spanning dozens of 512-byte blocks in one continuous SPI
# transaction) — and, more insidiously, may also silently read *corrupted*
# data on borderline transfers that don't hard-fail. _copy_to_flash_cache()
# below already reads in small 4KB chunks and has always been reliable —
# match that here for every large read instead of one big readinto().
_READ_CHUNK = 4096


def readinto_chunked(f, buf, size):
    """Read exactly `size` bytes from `f` into `buf` (from offset 0), a
    few KB at a time. Returns the number of bytes actually read (may be
    less than `size` at EOF)."""
    mv = memoryview(buf)
    pos = 0
    while pos < size:
        n = f.readinto(mv[pos: pos + min(_READ_CHUNK, size - pos)])
        if not n:
            break
        pos += n
    return pos


def write_chunked(f, buf):
    """Write all of `buf` to `f` a few KB at a time — same one-big-SD-
    transaction concern as readinto_chunked() above, on the write side
    (sdcard.py's writeblocks(), CMD25)."""
    mv = memoryview(buf)
    size = len(mv)
    pos = 0
    while pos < size:
        end = min(pos + _READ_CHUNK, size)
        f.write(mv[pos:end])
        pos = end

# Mega ROMs are paged from a copy on the Pico's own onboard flash
# filesystem ('/'), not directly from SD. Measured on real hardware:
# reading bank-switch pages straight from SD gave ~6 FPS on a
# bank-switch-heavy game (SD's per-read protocol overhead, several ms
# each, dominates frame time) and also forced disabling the pipelined
# render/compute overlap (SD shares SPI1 with the LCD — a fetch could
# collide with an in-flight display DMA). Onboard flash is a separate
# QSPI peripheral (no SPI1 contention) with far lower per-read latency.
# The one-time SD->flash copy costs a few seconds at cart-select time
# (already covered by the menu's "Loading…" message); after that,
# gameplay reads never touch SD again.
_FLASH_CACHE_PATH = '/megarom_cache.rom'
_FLASH_CACHE_META = '/megarom_cache_src.txt'
_FLASH_COPY_CHUNK = 4096


def _flash_cache_valid(src_path, src_size):
    """True if _FLASH_CACHE_PATH already holds a copy of src_path/src_size."""
    try:
        with open(_FLASH_CACHE_META, 'r') as f:
            saved_path = f.readline().strip()
            saved_size = int(f.readline().strip())
    except (OSError, ValueError):
        return False
    if saved_path != src_path or saved_size != src_size:
        return False
    try:
        return uos.stat(_FLASH_CACHE_PATH)[6] == src_size
    except OSError:
        return False


def _copy_to_flash_cache(src_path, size):
    """Stream src_path (on SD) into _FLASH_CACHE_PATH (onboard flash) in
    small chunks — never holds more than one chunk in RAM at a time."""
    # Invalidate the meta file BEFORE writing any data: if the copy below
    # is interrupted (power loss, reset), the stale/incomplete .rom file
    # must not appear valid on the next boot's _flash_cache_valid() check
    # (which only inspects path+size, not actual content — a half-written
    # file with the right final size would otherwise look valid).
    try:
        uos.remove(_FLASH_CACHE_META)
    except OSError:
        pass

    with open(src_path, 'rb') as src, open(_FLASH_CACHE_PATH, 'wb') as dst:
        remaining = size
        while remaining > 0:
            buf = src.read(min(_FLASH_COPY_CHUNK, remaining))
            if not buf:
                break
            dst.write(buf)
            remaining -= len(buf)
    with open(_FLASH_CACHE_META, 'w') as f:
        f.write(f"{src_path}\n{size}\n")


def load_cart_smart(msx_module, slot, path):
    """
    Load a cartridge into `slot`, automatically choosing between the
    simple in-RAM path (small ROMs) and the onboard-flash-backed paged
    path (Mega ROM, >32KB — see _FLASH_CACHE_PATH above). For paged
    carts the flash-cache file is opened here and handed to
    msx.load_cart_paged(), which keeps its own reference to it for the
    life of the cart — msx.eject_cart() closes it; callers must not.
    Returns True on success.
    """
    size = uos.stat(path)[6]

    if size <= _CART_INRAM_MAX:
        # Zero-copy: msx.cart_alloc() mallocs msx->cart[slot] (C heap) and
        # hands back a view of it directly — read the ROM straight into
        # that instead of a separate Python-owned scratch buffer first.
        # Same reasoning as main.py's load_bios_file(): even a shared,
        # lazily-allocated ~32KB scratch bytearray was observed to fail
        # allocation on this board's GC heap right after gc.collect().
        # Chunked (readinto_chunked(), not one big readinto()) also avoids
        # one giant multi-block SD read in a single go, which separately
        # was observed to be unreliable on real hardware.
        view = msx_module.cart_alloc(slot, size)
        if view is None:
            return False
        with open(path, 'rb') as f:
            n = readinto_chunked(f, view, size)
        if n != size:
            return False
        return msx_module.cart_finalize(slot)

    if not _flash_cache_valid(path, size):
        _copy_to_flash_cache(path, size)

    f = open(_FLASH_CACHE_PATH, 'rb')
    try:
        # Mapper-select init code is always in the ROM's fixed first
        # page (bank 0), so a short prefix is enough for detect_mapper()
        # to find it — keep this small: a large one-shot read here can
        # fail with MemoryError even with 70+KB nominally free, since
        # MicroPython's GC heap doesn't compact (same lesson as the
        # save-state redesign — see msx_core.h's Mega ROM comment).
        # Reuse the shared scratch buffer via readinto_chunked() rather
        # than allocating a fresh prefix-sized bytes object or issuing one
        # large readinto() in a single SD transaction, for the same
        # reasons as the in-RAM cart path above — this file position isn't
        # relied on afterward (cart_fetch_from_pyfile() in modmsx.c always
        # seeks explicitly before reading a page).
        buf = get_rom_load_buf()
        prefix_len = min(size, 8192)
        n = readinto_chunked(f, buf, prefix_len)
        mapper = msx_module.detect_mapper(memoryview(buf)[:n])
        if mapper == msx_module.MAPPER_PLAIN:
            # Heuristic found no bank-switch pattern despite the ROM
            # being large — fall back to KONAMI (the most common Mega
            # ROM mapper) rather than refusing to load at all; a game
            # that never actually switches banks still runs fine paged.
            mapper = msx_module.MAPPER_KONAMI
        ok = msx_module.load_cart_paged(slot, f, size, mapper)
    except Exception:
        f.close()
        raise
    if not ok:
        f.close()
    return ok


# ---------------------------------------------------------------------------
# Public API
# ---------------------------------------------------------------------------

def select_rom(msx_module, directory, title="Select ROM",
               usb_host_mod=None, auto_if_one=True, timeout_ms=5000,
               exclude_names=()):
    """
    Interactive ROM file selector.

    Parameters
    ----------
    msx_module   : the `msx` C module (already initialized with display).
    directory    : SD card path to scan for .ROM files (e.g. "/sd/msx").
    title        : menu title string.
    usb_host_mod : optional `usb_host` module for keyboard input.
    auto_if_one  : if True, auto-select when only one file is found.
    timeout_ms   : ms to wait for keyboard input before auto-selecting (0=infinite).

    Returns
    -------
    str  — full path of selected file, or None if user skipped (ESC).
    """
    import time

    # HDMI suspended only around the actual SD directory listing — the
    # interactive browsing loop below just redraws the already-fetched
    # `items` list from RAM (no further SD access per keypress), so HDMI
    # stays on throughout browsing. See hdmi_suspend()'s comment.
    _prev_hdmi = hdmi_suspend()
    items = _list_roms(directory, exclude_names=exclude_names)
    hdmi_resume(_prev_hdmi)
    canvas = MenuCanvas(msx_module)

    if not items:
        _draw_message(canvas, title, "No .ROM files found in", directory, C_RED)
        time.sleep_ms(2000)
        return None

    if auto_if_one and len(items) == 1:
        _draw_message(canvas, title, f"Auto: {items[0]}", "Loading…", C_GREEN)
        time.sleep_ms(800)
        return directory + "/" + items[0]

    # Without a keyboard, auto-select the first file after a brief display
    if usb_host_mod is None:
        _draw_message(canvas, title,
                      items[0] if items else "(none)",
                      "No keyboard — auto-selecting", C_YELLOW)
        time.sleep_ms(1500)
        return (directory + "/" + items[0]) if items else None

    selected = 0
    scroll   = 0
    MAX_ROWS = (MenuCanvas.H - 26) // 10

    _draw_file_list(canvas, title, items, selected, scroll)
    _wait_key_release(usb_host_mod)

    last_key  = 0
    deadline  = (time.ticks_ms() + timeout_ms) if timeout_ms > 0 else None

    while True:
        time.sleep_ms(30)
        key = _get_key(usb_host_mod)

        # Auto-select first item when timeout expires with no key activity
        if deadline is not None and key == 0:
            if time.ticks_diff(deadline, time.ticks_ms()) <= 0:
                _draw_message(canvas, title, f"Auto: {items[selected]}",
                              "No input — loading…", C_YELLOW)
                time.sleep_ms(600)
                return directory + "/" + items[selected]

        if key == last_key:
            continue   # still held — ignore repeat for now
        last_key = key

        if key == 0:
            continue

        # Any key press resets the timeout
        deadline = (time.ticks_ms() + timeout_ms) if timeout_ms > 0 else None

        if key == HID_UP:
            if selected > 0:
                selected -= 1
                if selected < scroll:
                    scroll = selected
        elif key == HID_DOWN:
            if selected < len(items) - 1:
                selected += 1
                if selected >= scroll + MAX_ROWS:
                    scroll = selected - MAX_ROWS + 1
        elif key == HID_ENTER:
            _wait_key_release(usb_host_mod)
            return directory + "/" + items[selected]
        elif key == HID_ESC:
            _wait_key_release(usb_host_mod)
            return None

        _draw_file_list(canvas, title, items, selected, scroll)


# ---------------------------------------------------------------------------
# Runtime emulator menu (GUI+F7, analogous to the PB-1000 emulator's
# emulator_menu.py). Called from main.py's poll_keyboard() while
# gameplay is paused; returns once the user resumes.
# ---------------------------------------------------------------------------

_RUNTIME_ITEMS = ["Swap Cartridge", "Save State", "Load State",
                  "Audio Settings", "HDMI Settings", "Reset MSX", "Resume"]

# Volume steps in 16-unit increments (0-256; 256 = original full-scale
# default, ~75% PWM duty — see modmsx.c). Filter steps 0-8 (0=off; higher
# = heavier low-pass smoothing, trades clarity for less buzzer harshness).
_VOLUME_STEP = 16
_VOLUME_MAX  = 256
_FILTER_MAX  = 8

# HDMI bridge output settings (hdmi_bridge/README.md) — see main.py's
# HDMI_CS_PIN/HDMI_BAUD. 'display' selects which output(s) get rendered
# each frame; 'frame_skip' throttles how often the (blocking, ~40ms at
# 10MHz) HDMI send runs.
_DISPLAY_MODES = ["both", "lcd", "hdmi"]
_FRAME_SKIP_MAX = 8


def _draw_runtime_menu(canvas, cursor, msg=""):
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
    """
    Live-adjustable Volume/Filter screen. Changes take effect immediately
    (msx_module.set_audio_volume/set_audio_filter) so you hear the result
    while adjusting. ENTER persists both values into config.txt via
    save_config(); ESC returns without writing the file (the live-adjusted
    values remain in effect for the rest of this session either way).
    """
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
                    msg = "Saved to config.txt"
                except Exception as e:
                    msg = f"Save failed: {e}"
            _draw_audio_settings(canvas, cursor, volume, filt, msg)
            time.sleep_ms(800)
            return
        elif key == HID_ESC:
            _wait_key_release(usb_host_mod)
            return

        _draw_audio_settings(canvas, cursor, volume, filt, msg)


def _draw_hdmi_settings(canvas, cursor, state, msg=""):
    canvas.clear(C_BLACK)
    canvas.rect(0, 0, canvas.W, 12, C_YELLOW, fill=True)
    canvas.text("HDMI SETTINGS", 2, 2, C_BLACK)

    rows = [
        f"HDMI: {'On' if state['enabled'] else 'Off'}",
        f"Display: {state['display'].upper()}",
        f"Frame Skip: {state['frame_skip']}",
    ]
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


def _show_hdmi_settings_menu(msx_module, usb_host_mod, config_path, hdmi_state,
                              init_hdmi_output):
    """
    Live-adjustable HDMI/Display/Frame Skip screen (hdmi_bridge/README.md).
    Changes take effect immediately, same philosophy as _show_audio_settings_
    menu(): adjusting here affects the running session right away (the
    caller's main loop reads these values every frame), and ENTER additionally
    persists them into config.txt. ESC returns without writing the file, but
    the live-adjusted values remain in effect for the rest of this session.

    hdmi_state: dict with keys 'enabled' (bool), 'display' ('both'/'lcd'/
    'hdmi'), 'frame_skip' (int). Returned (possibly modified) so the caller
    can update its own module-level globals.

    init_hdmi_output: callback taking no args, called the moment HDMI is
    turned on from Off — mirrors what main.py's boot-time
    msx.init_hdmi_output() call does, since turning HDMI on for the first
    time here (if config.txt had hdmi=0 at boot) needs that same one-time
    GPIO/SPI setup. Safe/idempotent to call more than once.
    """
    import time

    canvas = MenuCanvas(msx_module)
    cursor = 0
    state = dict(hdmi_state)
    msg = ""

    _draw_hdmi_settings(canvas, cursor, state, msg)
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
            cursor = (cursor - 1) % 3 if key == HID_UP else (cursor + 1) % 3
        elif key == HID_LEFT or key == HID_RIGHT:
            sign = -1 if key == HID_LEFT else 1
            if cursor == 0:
                was_enabled = state['enabled']
                state['enabled'] = not state['enabled']
                if state['enabled'] and not was_enabled:
                    init_hdmi_output()
                set_display_state(state['display'], state['enabled'])
            elif cursor == 1:
                idx = _DISPLAY_MODES.index(state['display'])
                idx = (idx + sign) % len(_DISPLAY_MODES)
                state['display'] = _DISPLAY_MODES[idx]
                set_display_state(state['display'], state['enabled'])
            else:
                state['frame_skip'] = max(1, min(_FRAME_SKIP_MAX,
                                                  state['frame_skip'] + sign))
        elif key == HID_ENTER:
            _wait_key_release(usb_host_mod)
            if config_path is None:
                msg = "No config path — not saved"
            else:
                try:
                    save_config(config_path, {
                        'hdmi': '1' if state['enabled'] else '0',
                        'display': state['display'],
                        'hdmi_frame_skip': str(state['frame_skip']),
                    })
                    msg = "Saved to config.txt"
                except Exception as e:
                    msg = f"Save failed: {e}"
            _draw_hdmi_settings(canvas, cursor, state, msg)
            time.sleep_ms(800)
            return state
        elif key == HID_ESC:
            _wait_key_release(usb_host_mod)
            return state

        _draw_hdmi_settings(canvas, cursor, state, msg)


def show_emulator_menu(msx_module, usb_host_mod, rom_dir, exclude_names,
                       save_path, config_path=None, hdmi_state=None,
                       init_hdmi_output=None):
    """
    Pause gameplay and show the runtime emulator menu (GUI+F7).
    All actions (cart swap, save/load, reset) are performed directly here;
    the caller just needs to resume its main loop once this returns.

    hdmi_state/init_hdmi_output: see _show_hdmi_settings_menu(). Pass the
    caller's current HDMI/display/frame_skip state in (a dict with keys
    'enabled'/'display'/'frame_skip' — the caller should always pass one,
    defaulting to disabled if the HDMI bridge addon was never configured
    in config.txt, so it can still be turned on live from this menu); the
    (possibly updated) state is returned so the caller can update its own
    globals.
    """
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
            return hdmi_state
        elif key == HID_ENTER:
            _wait_key_release(usb_host_mod)
            label = _RUNTIME_ITEMS[cursor]

            if label == "Resume":
                return hdmi_state

            elif label == "Swap Cartridge":
                # select_rom() suspends HDMI internally around its own SD
                # directory listing and resumes it for interactive
                # browsing (no SD access happens per-keypress there), so
                # HDMI can stay on for this call.
                selected = select_rom(msx_module, rom_dir,
                                      title="Select Cartridge ROM",
                                      usb_host_mod=usb_host_mod,
                                      auto_if_one=False,
                                      timeout_ms=0,
                                      exclude_names=exclude_names)
                if selected:
                    # HDMI suspended from the moment a file is picked until
                    # the load finishes (success or not) — the "Loading…"
                    # draw right below and the eject/load further down are
                    # exactly the SD-heavy-access-right-after-an-HDMI-mode
                    # -draw pattern that's unreliable on real hardware (see
                    # hdmi_suspend()'s comment); does NOT force LCD mode,
                    # so a display=hdmi (no LCD) setup keeps working the
                    # same way, just without a picture during this window.
                    _prev_hdmi = hdmi_suspend()
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
                            ok = load_cart_smart(msx_module, 0, selected)
                            if ok:
                                msx_module.reset()
                                msg = f"Loaded {selected.rsplit('/',1)[-1]}"
                            else:
                                msg = "load_cart() failed"
                        except Exception as e:
                            # Broad catch: OSError (file missing) and
                            # MemoryError (GC heap too fragmented for the
                            # cart-sized read) are both real possibilities.
                            msg = f"Load failed: {e}"
                    finally:
                        hdmi_resume(_prev_hdmi)
                else:
                    msg = ""

            elif label == "Save State":
                # Writing ~64KB to SD takes a couple of seconds — show
                # feedback so this isn't mistaken for a hang. HDMI
                # suspended for the duration (see hdmi_suspend()'s
                # comment — a sustained ~64KB SD write is exactly the
                # kind of SD access that's unreliable soon after an
                # HDMI-mode draw); does not force LCD mode.
                _prev_hdmi = hdmi_suspend()
                _draw_runtime_menu(canvas, cursor, "Saving…")
                try:
                    save_state_to(msx_module, save_path)
                    msg = "State saved"
                except Exception as e:
                    msg = f"Save failed: {e}"
                finally:
                    hdmi_resume(_prev_hdmi)

            elif label == "Load State":
                # Reading ~64KB from SD takes a couple of seconds — same
                # as above, show feedback instead of an apparent freeze,
                # and same HDMI-suspend-during-SD-access reasoning.
                _prev_hdmi = hdmi_suspend()
                _draw_runtime_menu(canvas, cursor, "Loading…")
                try:
                    ok = load_state_from(msx_module, save_path)
                    msg = "State loaded" if ok else "Invalid save file"
                except Exception as e:
                    msg = f"Load failed: {e}"
                finally:
                    hdmi_resume(_prev_hdmi)

            elif label == "Audio Settings":
                _show_audio_settings_menu(msx_module, usb_host_mod, config_path)
                msg = ""

            elif label == "HDMI Settings":
                hdmi_state = _show_hdmi_settings_menu(
                    msx_module, usb_host_mod, config_path, hdmi_state,
                    init_hdmi_output)
                msg = ""

            elif label == "Reset MSX":
                msx_module.reset()
                msg = "MSX reset"

        if redraw:
            _draw_runtime_menu(canvas, cursor, msg)


def load_config(config_path):
    """
    Read a simple key=value config file from SD.
    Returns a dict with keys like 'bios', 'cart'.

    Example /sd/msx/config.txt:
        bios=/sd/msx/MSX.ROM
        cart=/sd/msx/MySoftware.ROM
    """
    cfg = {}
    try:
        with open(config_path, 'r') as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith('#'):
                    continue
                if '=' in line:
                    k, _, v = line.partition('=')
                    cfg[k.strip().lower()] = v.strip()
    except OSError:
        pass
    return cfg


def save_config(config_path, updates):
    """
    Persist key=value pairs into the config file, preserving existing
    lines (including comments and unrelated keys). Matching keys are
    updated in place; new keys are appended.
    """
    try:
        with open(config_path, 'r') as f:
            lines = f.readlines()
    except OSError:
        lines = []

    remaining = dict(updates)
    for i, line in enumerate(lines):
        stripped = line.strip()
        if not stripped or stripped.startswith('#') or '=' not in stripped:
            continue
        k = stripped.partition('=')[0].strip().lower()
        if k in remaining:
            lines[i] = f"{k}={remaining.pop(k)}\n"

    for k, v in remaining.items():
        lines.append(f"{k}={v}\n")

    # MicroPython's file object has no writelines(); build one string and
    # write it in a single call instead (also avoids truncating the file
    # via 'w' and then failing partway through a multi-write sequence).
    data = "".join(lines)
    with open(config_path, 'w') as f:
        f.write(data)
