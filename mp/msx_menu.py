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
import time

try:
    import msx as _msx
except ImportError:
    _msx = None


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
# every screen in this file (runtime menu, Audio/Display Settings, ROM
# selector, boot error screen) would only ever repaint the LCD, leaving
# HDMI frozen on the last gameplay frame while a menu is open. main.py
# calls set_display_state() once at boot and again whenever the HDMI
# Settings menu changes these values.
# ---------------------------------------------------------------------------

_display_mode = 'lcd'   # 'lcd' | 'hdmi' | 'dvi' — mutually exclusive, see set_display_state()
_hdmi_suspended = False
_lcd_suspended = False


def set_display_state(display_mode):
    global _display_mode
    _display_mode = display_mode


def hdmi_suspend():
    """Temporarily disable HDMI output (leaving _display_mode untouched)
    around an SD-heavy operation (SD listdir()/read()/write()) — HDMI's
    SPI mode 3 conflicts with the mode 0 that LCD/SD both need, and
    switching between them right around SD access is unreliable on real
    hardware ("frequent same-peripheral SPI mode switching on RP2350
    remains an open problem" — see msx_core.c). Mirrors lcd_suspend()
    below exactly, just for the other side (display=lcd and display=hdmi
    are mutually exclusive — see set_display_state() — so at most one of
    _hdmi_suspended/_lcd_suspended is ever the one actually stopping a
    render). Returns the previous _hdmi_suspended value; pass it to
    hdmi_resume() when the SD-heavy operation is done.

    Every caller used to fall straight into its SD access the instant this
    returned, but suspending only stops the next HDMI frame from being
    sent; it does nothing about a frame whose blocking SPI send was still
    in flight (or had just finished) the moment this was called. The
    mode-0 switch the SD driver performs right after then lands with zero
    settling time in that case, which is exactly the same-peripheral SPI
    mode switching hazard documented in msx_core.c. A short pause here,
    before the caller ever touches the SD card, costs nothing during
    normal menu use and gives that in-flight activity (and the peripheral
    itself) a moment to settle first.
    """
    global _hdmi_suspended
    prev = _hdmi_suspended
    _hdmi_suspended = True
    if not prev:
        time.sleep_ms(20)
    return prev


def hdmi_resume(prev_suspended):
    """Restore the _hdmi_suspended value hdmi_suspend() returned."""
    global _hdmi_suspended
    _hdmi_suspended = prev_suspended


def lcd_suspend():
    """Temporarily disable LCD rendering (MenuCanvas.flush() no-ops its
    render_to_display_1to1()/wait_display() calls while this is set)
    around an SD-heavy operation, mirroring hdmi_suspend() above.

    2026-08-29: added while chasing a real-hardware OSError EIO from SD
    reads that reproduces on the very first SD touch after a stretch of
    LCD-only rendering (confirmed independent of HDMI — the same EIO
    reproduces with display=lcd, where HDMI's SPI mode 3 never gets used
    at all, so this isn't the already-known HDMI mode-switch hazard).
    MenuCanvas.flush() already calls msx.wait_display() after every LCD
    render, so an in-flight DMA transfer specifically isn't the
    mechanism — but nothing previously stopped a *fresh* LCD render from
    starting again right up until the moment SD is touched. This — plus
    the same short settling pause hdmi_suspend() uses — is a real,
    previously-untried experiment for that gap, not a confirmed fix.

    Returns the previous _lcd_suspended value; pass it to lcd_resume()
    when the SD-heavy operation is done. Doesn't touch _display_mode, so
    HDMI (if that's the active side) keeps rendering independently — only
    the LCD side goes quiet for the duration.
    """
    global _lcd_suspended
    prev = _lcd_suspended
    _lcd_suspended = True
    if not prev:
        time.sleep_ms(20)
    return prev


def lcd_resume(prev_suspended):
    """Restore the _lcd_suspended value lcd_suspend() returned."""
    global _lcd_suspended
    _lcd_suspended = prev_suspended


# ---------------------------------------------------------------------------
# Low-level draw helpers (write directly into the MSX framebuf)
# ---------------------------------------------------------------------------

class MenuCanvas:
    """Thin wrapper: exposes framebuf.FrameBuffer on the MSX C framebuf."""

    W = 256
    H = 192

    def __init__(self, msx_module):
        self._msx = msx_module
        # 2026-09-30/10-01: DVI briefly drew into the BACK buffer
        # (get_framebuf_back()) instead of the ready one, on the theory
        # that disp_dvi.c's scanline_callback (core1, reading
        # framebuf[ready_idx] continuously in the background) races a
        # menu drawing into that same live buffer — plausible in theory,
        # and the Win+F7 freeze this was meant to fix was real, but that
        # freeze's actual cause turned out to be msx_wait_display()
        # dereferencing a NULL spi_inst on DVI (see msx_core.c), unrelated
        # to this buffer choice. The back-buffer "fix" itself, once
        # isolated by reverting just this one line, was confirmed on real
        # hardware to be what caused a NEW, worse regression ("No Signal",
        # permanent, both buffers apparently ending up invalid rather than
        # the theoretical race merely glitching a frame). Reverted to the
        # plain front/ready buffer, same as LCD/HDMI — confirmed working
        # correctly on real hardware. The theoretical race is left
        # unaddressed (no corruption from it has been observed), rather
        # than re-attempt a fix whose exact failure mode was never fully
        # explained.
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
        # whichever output is actually active, same as gameplay.
        # display=lcd/hdmi are mutually exclusive (no more 'both' — see
        # set_display_state()'s comment), so exactly one of these is true
        # except during the brief SD-heavy-operation windows where
        # hdmi_suspend()/lcd_suspend() quiet the active side entirely (the
        # screen goes blank for that window rather than falling back to
        # the other side, which may not even be initialized).
        use_hdmi = (_display_mode == 'hdmi') and not _hdmi_suspended
        use_lcd  = (_display_mode == 'lcd') and not _lcd_suspended
        use_dvi  = (_display_mode == 'dvi')
        if use_lcd:
            self._msx.render_to_display_1to1()
            self._msx.wait_display()
        if use_dvi:
            # No-op (disp_dvi.c has no per-frame "compose" step; its
            # scanline_callback reads framebuf[framebuf_ready_idx] directly
            # from its own background core1 callback) — kept only as a
            # stable call site. See __init__()'s comment for the back-
            # buffer approach this used to take instead, and why it was
            # reverted.
            self._msx.render_to_display_dvi()
        if use_hdmi:
            # raw332, not render_to_hdmi(): menus draw arbitrary UI colors
            # (borders, highlights) that aren't limited to the MSX's 16
            # game-palette colors, so the 4-bit palette-lookup path would
            # render any non-matching color as black (found on real
            # hardware — see hdmi_bridge/README.md's Phase 4 section).
            self._msx.render_to_hdmi_raw332()


# ---------------------------------------------------------------------------
# File browser — see msx_rom_browser.py (lazily imported by select_rom()
# below and by show_emulator_menu()'s "Swap Cartridge" branch).
# ---------------------------------------------------------------------------

_last_printed_msg = None


def _echo_msg(msg):
    # Print `msg` to the REPL/serial console the first time it's seen (not
    # on every redraw of the same screen, since these draw functions get
    # called repeatedly while a message stays on screen). The LCD's small
    # font is small (31 chars per line), so the on-screen text is drawn
    # via _wrap_msg() (two lines max) in the menu _draw() functions.
    global _last_printed_msg
    if msg and msg != _last_printed_msg:
        print(f"MENU: {msg}")
    _last_printed_msg = msg


def _wrap_msg(msg, width=31, max_lines=2):
    # Word-wrap a status message into at most `max_lines` lines of `width`
    # characters (the menu font is 8 px wide on a 256 px canvas, so 31
    # chars fit). Words longer than `width` are hard-split. Anything past
    # `max_lines` is dropped — keep messages short enough to fit.
    lines = []
    cur = ""
    for word in msg.split(" "):
        while len(word) > width:
            if cur:
                lines.append(cur)
                cur = ""
            lines.append(word[:width])
            word = word[width:]
        if not cur:
            cur = word
        elif len(cur) + 1 + len(word) <= width:
            cur += " " + word
        else:
            lines.append(cur)
            cur = word
    if cur:
        lines.append(cur)
    return lines[:max_lines]


def _draw_message(canvas, title, line1, line2="", color=C_WHITE):
    _echo_msg(f"{line1} {line2}".strip() if line2 else line1)
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
    # 2026-09-30: on pizero, usb_host.get_hid_report() only reflects
    # whatever usb_host.task() (tuh_task()) last dequeued — main.py's
    # poll_keyboard() calls it every frame outside any menu (see its own
    # comment: pizero's 1ms hardware-alarm tick deliberately does NOT
    # drive tuh_task() itself). Once inside a menu, poll_keyboard()'s main
    # loop isn't running — this was the ONLY other place reading HID
    # reports, and it never called task() itself, so the report buffer
    # went stale the instant a menu opened. Real-hardware finding: this
    # made _wait_key_release() (below) spin forever on pizero — the GUI+F7
    # combo that opened the menu could never be observed as "released"
    # (the report just stayed frozen at "still held"), a 100%-reproducible
    # hang on every menu open. Harmless/no-op on pico2 (native USB host
    # doesn't need explicit task() polling — see usb_host_core_task()).
    try:
        usb_host_mod.task()
    except Exception:
        pass
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


MAX_SAVE_SLOTS = 10  # 2026-09-06: rotating save-state history per cart


def save_base_for_cart(cart_path, fallback_base):
    """Base path (no slot number/extension) for a cart's rotating saves —
    sits right next to the ROM file itself (e.g. "/sd/games/Foo.ROM" ->
    "/sd/games/Foo"), so each cart keeps its own save history regardless
    of where it lives on the card. Falls back to `fallback_base` (main.py's
    single shared base) when no cart is loaded (BASIC-only session)."""
    if not cart_path:
        return fallback_base
    return cart_path[:-4] if cart_path.lower().endswith('.rom') else cart_path


def save_slot_path(base, slot):
    return f"{base}.{slot}.sav"


def rotate_and_save_state(msx_module, base, max_slots=MAX_SAVE_SLOTS):
    """Shift existing save slots up by one (0->1, 1->2, ..., dropping
    whatever was in the oldest slot), then write the new state into slot 0
    (most recent). A missing slot (OSError from rename/remove) is normal —
    e.g. before max_slots saves have ever been made — and just skipped."""
    try:
        uos.remove(save_slot_path(base, max_slots - 1))
    except OSError:
        pass
    for i in range(max_slots - 2, -1, -1):
        try:
            uos.rename(save_slot_path(base, i), save_slot_path(base, i + 1))
        except OSError:
            pass
    save_state_to(msx_module, save_slot_path(base, 0))


def list_save_slots(base, max_slots=MAX_SAVE_SLOTS):
    """Existing slot numbers for `base`, most recent (0) first."""
    slots = []
    for i in range(max_slots):
        try:
            uos.stat(save_slot_path(base, i))
            slots.append(i)
        except OSError:
            pass
    return slots


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
# the small Mega ROM mapper-detection prefix read below, which doesn't
# have a natural C-side destination to read directly into (it's just a
# peek at the ROM header before deciding how to load it) — kept lazy
# (allocated on first actual use, not at import time) for the same
# reason.
#
# 2026-09-08: "first actual use" turned out to matter — real-hardware
# finding: loading a normal (in-RAM) cart first, then a Mega ROM later
# via Swap Cartridge, means this allocation's actual first attempt now
# happens well into a session (gameplay already run, msx_runtime_menu.py/
# msx_rom_browser.py already imported for the swap), on a heap
# considerably more fragmented than at a fresh boot — a gc.collect()
# right before allocating (see get_rom_load_buf()) wasn't enough to fix
# it on its own ("MemoryError ... allocating 8192 bytes" persisted).
# Shrunk 8KB -> 4KB (mapper-select bank-switch code is reliably within
# the first few KB of real Mega ROMs, and get_rom_load_buf()'s caller
# already tolerates a failed/incomplete detection via the KONAMI
# fallback — see load_cart_smart()'s comment) to make the allocation
# itself easier to satisfy, and load_cart_smart() now also tolerates
# this allocation failing outright (falls back to KONAMI instead of
# aborting the whole cart load) as a last resort.
_PREFIX_BUF_SIZE = 4096
_prefix_buf = None


# 2026-10-07: was briefly flipped to False as a real-hardware experiment
# (hypothesis: msx.get_scratch_view()'s permanent 4KB C-heap malloc() was
# shifting where the 48KB cart_cache malloc() lands, breaking pico2 Mega
# ROM loads). Disproving result: pico2's cart_cache malloc() failure
# ("load_cart() failed") reproduced identically on the 8b57de1 baseline
# firmware, which predates get_scratch_view() entirely — so that's a
# separate, pre-existing C-heap fragmentation issue (small in-RAM cart's
# cart[] freed right before the 48KB cart_cache request), not caused by
# this flag. Meanwhile, going back to the GC-heap bytearray(4096)
# fallback (False) reproduced the ORIGINAL problem this flag exists to
# avoid: "Load failed: memory allocation failed, allocating 4097 bytes"
# on Swap Cartridge, from msx_runtime_menu/msx_rom_browser's lazy imports
# fragmenting the GC heap. Restored to True — see get_rom_load_buf()'s
# own comment for why the C-heap-backed view is strictly safer here.
_USE_C_SCRATCH = True


def get_rom_load_buf():
    """Return the shared Mega-ROM-mapper-detection scratch buffer
    (_PREFIX_BUF_SIZE bytes), allocating it on first call. See the
    comment above for why this is lazy rather than a module-level
    `= bytearray(...)`, and for why it's tolerated failing outright
    (load_cart_smart()'s MemoryError fallback) even after the
    gc.collect() below and the 8KB->4KB shrink."""
    global _prefix_buf
    if _prefix_buf is None:
        # 2026-10-04: prefer the C-heap-backed view (msx.get_scratch_view(),
        # modmsx.c) — no contiguous GC-heap block needed at all, which is
        # what kept failing on real hardware (boot warm-up AND Mega ROM
        # load) despite tens of KB nominally free. bytearray fallback only
        # for firmware builds that predate it.
        if _USE_C_SCRATCH and _msx is not None and hasattr(_msx, 'get_scratch_view'):
            _prefix_buf = _msx.get_scratch_view()
        else:
            import gc
            gc.collect()
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
_FLASH_COPY_CHUNK = 4096  # must stay <= _PREFIX_BUF_SIZE below — see
                          # _copy_to_flash_cache()'s reuse of get_rom_load_buf()


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
    small chunks — never holds more than one chunk in RAM at a time.

    2026-10-03 real-hardware finding: this used to do
    `buf = src.read(min(_FLASH_COPY_CHUNK, remaining))`, which allocates a
    FRESH ~4KB bytes object every single chunk — for a several-hundred-KB
    Mega ROM that's hundreds of allocations, each one a fresh chance to
    hit a MemoryError on this board's non-compacting GC heap if it's
    fragmented enough (e.g. mid-session, after the ROM browser/menu have
    already run) even with plenty of *nominal* free bytes — reproduced as
    "Load failed: memory allocation failed, allocating 4097 bytes" via
    Swap Cartridge. Reuses the shared scratch buffer (get_rom_load_buf(),
    same 4096-byte size as _FLASH_COPY_CHUNK) via readinto() instead —
    same single-allocation-then-reuse pattern already used elsewhere in
    this file (readinto_chunked()) — so only the first-ever call anywhere
    in the session risks a MemoryError, not every chunk of every copy."""
    # Invalidate the meta file BEFORE writing any data: if the copy below
    # is interrupted (power loss, reset), the stale/incomplete .rom file
    # must not appear valid on the next boot's _flash_cache_valid() check
    # (which only inspects path+size, not actual content — a half-written
    # file with the right final size would otherwise look valid).
    try:
        uos.remove(_FLASH_CACHE_META)
    except OSError:
        pass

    buf = get_rom_load_buf()
    mv = memoryview(buf)
    with open(src_path, 'rb') as src, open(_FLASH_CACHE_PATH, 'wb') as dst:
        remaining = size
        while remaining > 0:
            chunk = min(_FLASH_COPY_CHUNK, remaining)
            n = src.readinto(mv[:chunk])
            if not n:
                break
            dst.write(mv[:n])
            remaining -= n
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

    if size > _CART_INRAM_MAX and _msx is not None and _msx.get_board_type() == "pizero":
        # 2026-10-05: Mega ROM (paged) loading disabled on pizero —
        # real-hardware finding: loading/swapping a paged cart reliably
        # triggers a core1 (onboard DVI) crash sooner or later (confirmed
        # via msx.dvi_debug(): heartbeat freezes permanently,
        # late_scanline_ctr stays 0 — a genuine core1 halt, not the DVI
        # TMDS buffer-starvation recovery issue already fixed in
        # src/msx/libdvi/dvi.c). Root cause not isolated after extensive
        # real-hardware investigation (ruled out: mapper type, cart_cache
        # reuse vs fresh allocation, the backlog-counter mechanism —
        # looks probabilistic/timing-dependent, not tied to any one of
        # those). Refusing up front (small in-RAM carts — regular, non-
        # bank-switched ROMs — are completely unaffected and keep working
        # normally) beats an unpredictable crash mid-game. See
        # doc/usage_guide.md §6 for the user-facing note. pico2 has no
        # DVI/core1 at all and is unaffected — this check never triggers
        # there.
        # Short on purpose: the screen shows at most two 31-char lines,
        # and msx_mode_switch adds a "Load failed: " prefix (13 chars).
        raise RuntimeError(
            f"Mega ROM {size // 1024}KB not supported on PiZero")

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
            # 2026-08-29: was a silent `return False` — collapsed into the
            # same generic "load_cart() failed" message as every other
            # failure mode here, with no way to tell a C-heap allocation
            # failure apart from a short SD read from the caller's message.
            raise RuntimeError(f"cart_alloc({size} bytes) failed (C heap exhausted/fragmented?)")
        with open(path, 'rb') as f:
            n = readinto_chunked(f, view, size)
        if n != size:
            raise RuntimeError(f"short read: got {n}/{size} bytes from {path}")
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
        #
        # 2026-09-08: get_rom_load_buf()'s own allocation can itself still
        # fail on a sufficiently fragmented heap (real-hardware finding —
        # see its comment) even after shrinking it and adding a
        # gc.collect(). Rather than let that MemoryError abort the whole
        # cart load, treat it exactly like "heuristic found no pattern"
        # below: fall back to KONAMI (the most common Mega ROM mapper) —
        # a load with a guessed mapper beats no load at all.
        mapper = msx_module.MAPPER_KONAMI
        try:
            buf = get_rom_load_buf()
            prefix_len = min(size, _PREFIX_BUF_SIZE)
            n = readinto_chunked(f, buf, prefix_len)
            detected = msx_module.detect_mapper(memoryview(buf)[:n])
            if detected != msx_module.MAPPER_PLAIN:
                mapper = detected
            # else: heuristic found no bank-switch pattern despite the ROM
            # being large — keep the KONAMI fallback above rather than
            # refusing to load at all; a game that never actually
            # switches banks still runs fine paged.
        except MemoryError:
            pass  # keep the KONAMI fallback above
        # 2026-10-05 DIAGNOSTIC: which mapper got used for this load — see
        # real-hardware report of core1 (DVI) freezing permanently (heart-
        # beat frozen, late_scanline_ctr stayed 0) specifically on some
        # Mega ROMs (SHALOM.ROM) but not others (NEMESIS.ROM) that are
        # otherwise handled by the exact same code path. Checking whether
        # this correlates with mapper type (ASCII16 calls
        # cart_page_refill() twice per bank-switch write, unlike ASCII8/
        # KONAMI's once — see cart_mapper_write()'s comment in msx_core.c).
        _mapper_names = {
            msx_module.MAPPER_PLAIN:   "PLAIN",
            msx_module.MAPPER_ASCII8:  "ASCII8",
            msx_module.MAPPER_ASCII16: "ASCII16",
            msx_module.MAPPER_KONAMI:  "KONAMI",
        }
        print(f"MAPPER DIAG: {path} -> {_mapper_names.get(mapper, mapper)}")
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


def load_config(config_path):
    """
    Read a simple key=value config file from SD.
    Returns a dict with keys like 'bios', 'cart'.

    Example /sd/msx.ini:
        bios=/sd/msx/MSX.ROM
        cart=/sd/msx/MySoftware.ROM

    Retries a few times on OSError before giving up: on marginal SD
    hardware, open()/read() can fail transiently, and this file is read
    once at boot before anything else has "warmed up" the SPI bus. A
    silent failure here used to be indistinguishable from "no msx.ini
    exists" — the caller would fall back to defaults (e.g. bios=MSX.ROM)
    with no indication that a perfectly valid bios= line was actually
    sitting unread on the card. Log loudly instead so that scenario is
    diagnosable from the boot log alone.
    """
    cfg = {}
    last_err = None
    for attempt in range(3):
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
            return cfg
        except OSError as e:
            last_err = e
            time.sleep_ms(50)
    print(f"WARNING: could not read {config_path} after 3 attempts ({last_err}) — using defaults")
    return {}


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
