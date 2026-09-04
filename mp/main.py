"""
MSX1 Emulator — Main entry point
Raspberry Pi Pico 2 (RP2350) + MicroPython

Hardware (matches PB-1000 emulator board):
  LCD              SPI1: MOSI=GP11  SCK=GP10  CS=GP9  DC=GP8  RST=GP7   BL=GP22
                   ST7796 480x320 (MSP4021) or ILI9341 320x240 (MSP2402) —
                   same wiring/init sequence for both, see LCD_SIZES below.
  SD card          SPI1: MOSI=GP11  SCK=GP10  MISO=GP12 CS=GP15  (shared with LCD SPI)
  USB keyboard     native host (GP24/25)
  Audio PWM        GP14
  HDMI bridge      SPI1: MOSI=GP11  SCK=GP10  CS=GP28  (optional, shares LCD/SD
                   SPI bus; second Pico2+PICO-HDMI-PLUS, see hdmi_bridge/README.md;
                   opt-in via msx.ini: hdmi=1)

SD card layout:
  /sd/msx.ini         — optional config (see below; was /sd/msx/config.txt)
  /sd/msx/MSX.ROM     — 32KB MSX BIOS+BASIC (required)
  /sd/<anywhere>/<name>.ROM — cartridge ROM (selector browses from the
                        card root, subfolders included)

msx.ini format (key=value, # = comment):
  bios=/sd/msx/MSX.ROM
  cart=/sd/msx/CART.ROM
  lcd=ILI9341
  rotate=180
  volume=128
  audio_filter=2
  hdmi=1
  display=both
  boot_exclusive=0
  hdmi_frame_skip=2
  hdmi_baud=8000000
  # omit 'cart' to show the interactive ROM selector at boot (browses from
  # the SD root incl. subfolders; UP/DOWN move, ENTER opens a folder/picks
  # a ROM, ESC goes up a level/cancels at the root)
  # omit 'lcd' to default to ST7796 (see LCD_SIZES for valid names)
  # omit 'rotate' (or 0) for normal orientation; 180 flips the panel. Only
  # 0/180 supported (landscape MADCTL flip only, not a true 90/270
  # rotation); invalid values silently fall back to 0. LCD-only — has no
  # effect on HDMI output.
  # omit 'volume' to default to 256; lower (e.g. 128) reduces a passive
  # piezo buzzer's overdrive/crackle
  # omit 'audio_filter' to default to 0 (no smoothing); 1-8 = progressively
  # heavier low-pass smoothing
  # omit 'hdmi' (or 0) to disable the optional HDMI bridge output (second
  # Pico2+PICO-HDMI-PLUS, see hdmi_bridge/README.md)
  # omit 'display' to default to 'both' (LCD + HDMI every hdmi_frame_skip-
  # th frame). 'lcd'/'hdmi' send to only one, recovering the other's
  # per-frame cost. Only matters when hdmi=1.
  # omit 'boot_exclusive' (or 0): both LCD and HDMI hardware are always
  # initialized at boot regardless of 'display'. Set to 1 with
  # display=lcd or display=hdmi (not 'both') to skip the unused side's
  # hardware init entirely at boot (real boot-time saving, and the unused
  # one need not be physically present); can still be turned on live from
  # the menu afterward. Restart-only.
  # omit 'hdmi_frame_skip' to default to 2 (send to HDMI every other
  # frame); 1 = every frame. Only matters when hdmi=1.
  # omit 'hdmi_baud' to default to 8_000_000 (8MHz — 5/8MHz confirmed
  # clean on real hardware, 10MHz corrupts the received palette on this
  # wiring; see doc/hdmi_bridge_phase2_report.md). Only matters when hdmi=1.
  #
  # All of the above except bios/cart (ROM selector instead) can be tuned
  # live via GUI+F7 — Audio/HDMI/Display Settings. ENTER writes it back to
  # msx.ini; audio/hdmi/display/frame_skip take effect live, while
  # lcd/rotate/hdmi_baud/boot_exclusive need a restart (read once at boot).

Joystick (Atari/MSX 9-pin port wired directly to GPIO, PULL_UP/active-low,
JOY1 only): UP=GP18 DOWN=GP19 LEFT=GP20 RIGHT=GP21 TRIG-A=GP26 TRIG-B=GP27.
Read via the PSG I/O ports (register 14/15, port 0xA1/0xA2) exactly like
real MSX hardware — see msx_set_joystick()/poll_joystick() below.
"""

import sys
import time
import uos
import machine

try:
    import msx
except ImportError:
    print("ERROR: 'msx' C module not found — rebuild with micropython_msx.cmake")
    sys.exit(1)

try:
    import usb_host
    _usb_ready = False
except ImportError:
    usb_host   = None
    _usb_ready = False

import gc
gc.collect()  # maximize contiguous free heap before compiling the next
              # (larger) imports below — non-compacting GC, so this is
              # cheap insurance against a marginal MemoryError here.
from msx_keymap import apply_hid_report, HID_F5, HID_F7, HID_F8, MOD_LGUI, MOD_RGUI
from msx_ext    import load_extensions
from msx_menu   import (select_rom, load_config, show_emulator_menu,
                        save_state_to, load_state_from, load_cart_smart,
                        set_display_state, readinto_chunked)

# -----------------------------------------------------------------------
# Pin / peripheral constants
# -----------------------------------------------------------------------
SPI_ID   = 1
SPI_MOSI = 11
SPI_SCK  = 10
SPI_CS   = 9
SPI_DC   = 8
SPI_RST  = 7
SPI_BL   = 22
SPI_BAUD = 62_500_000   # 62.5 MHz = clk_peri(250MHz)/4 — the highest clean rate found
                        # empirically (125MHz = clk_peri/2 visibly corrupts the display;
                        # there's no achievable rate between them since the SPI clock
                        # divider only produces even divisors of clk_peri). Verified safe
                        # on both panels below.
                        # 2026-08-27: lowering this to 20MHz on the second board did NOT
                        # fix its SD/LCD instability — ruled out as an SPI signal-
                        # integrity margin issue. See project notes for the ongoing
                        # investigation.

# Panel size — selects how msx.init_display_hardware() centers the native
# 256x192 image (no scaling; see main.py's render_to_display_1to1()
# comment for why). Override via msx.ini: lcd=ILI9341
LCD_SIZES = {
    "ST7796":  (480, 320),   # MSP4021 (default)
    "ILI9341": (320, 240),   # MSP2402
}
DEFAULT_LCD_MODEL = "ST7796"

# HDMI bridge output (hdmi_bridge/README.md) — optional second Pico 2 +
# PICO-HDMI-PLUS. Shares SPI1 (SCK=GP10/MOSI=GP11) with the LCD/SD; GP28 is
# a new, dedicated CS added only for this link (no existing pin touched).
# Off by default — enable via msx.ini: hdmi=1
HDMI_CS_PIN = 28
# Real-hardware finding: 10MHz reliably corrupts the received palette on
# this wiring (electrical margin, not a transport bug — see
# doc/hdmi_bridge_phase2_report.md); 5/8MHz both confirmed clean, 8MHz
# slightly faster. 9-10MHz not narrowed further.
HDMI_BAUD   = 8_000_000  # default/fallback; see doc/hdmi_bridge_phase2_report.md.
HDMI_FRAME_SKIP = 2       # send to HDMI every Nth emulator frame — the
                          # blocking SPI send (~40ms at 10MHz) roughly
                          # halved FPS when sent every frame on real
                          # hardware; 2 trades HDMI update rate for LCD/
                          # emulation speed. Set to 1 to send every frame.

# SD card shares SPI1 with the LCD (same SCK/MOSI pins); MISO=GP12, CS=GP15.
# restore_baudrate returns SPI1 to SPI_BAUD after each SD operation so the
# LCD/touch drivers are not affected.
SD_SPI_ID   = 1
SD_MOSI_PIN = 11
SD_SCK_PIN  = 10
SD_MISO_PIN = 12
SD_CS_PIN   = 15
SD_INIT_BAUD = 400_000    # SD spec's mandatory low-speed handshake rate —
                          # sdcard.py's init_card() already hardcodes this
                          # itself for that handshake regardless of what's
                          # passed in here; this constant only seeds the
                          # initial machine.SPI() constructor call, which
                          # init_card() immediately overrides anyway.
# 2026-08-29: SDCard()'s own baudrate= parameter (used for every
# readblocks()/writeblocks() data transfer, NOT just the handshake — see
# sdcard.py) used to be given SD_INIT_BAUD too, meaning every SD block
# read/write ran at 400kHz. Split out as its own constant so the handshake
# rate and the data rate can be tuned independently.
#
# First attempt (20MHz) made SD mounting fail outright ("timeout waiting
# for response" from readinto(), in uos.mount()'s very first readblocks()
# call reading the filesystem header) — on real hardware. The 62.5MHz
# proven stable for the *LCD* on this same bus says nothing about what the
# *SD card* itself can tolerate: an LCD controller and an SD card in SPI
# mode have very different input timing margins, so "the bus already runs
# this fast for something else" is not evidence it's safe for the card.
# 4MHz is a deliberately conservative starting point (still 10x the
# previous 400kHz) — raise it later only after confirming reads are
# reliable at this rate first.
SD_DATA_BAUD = 4_000_000

AUDIO_PIN    = 14

# Joystick: Atari/MSX 9-pin port wired directly to GPIO (PULL_UP, active-low
# switches to GND — matches the PB-1000 board's joystick convention). JOY1
# only; JOY2 (port 1) is left at its neutral 0xFF default (no GPIO wired).
JOY_UP_PIN     = 18
JOY_DOWN_PIN   = 19
JOY_LEFT_PIN   = 20
JOY_RIGHT_PIN  = 21
JOY_TRIG_A_PIN = 26
JOY_TRIG_B_PIN = 27

# ROM_DIR is the SD root — select_rom() browses subfolders too (see
# msx_menu.py's _list_dir_entries()/_find_first_rom_recursive()). BIOS/
# save state stay under /sd/msx/.
ROM_DIR      = "/sd"
CONFIG_PATH  = "/sd/msx.ini"
DEFAULT_BIOS = "/sd/msx/MSX.ROM"
SAVE_PATH    = "/sd/msx/save.bin"

# -----------------------------------------------------------------------
# Helpers
# -----------------------------------------------------------------------
def mount_sd():
    try:
        import machine, sdcard
        sd_spi = machine.SPI(SD_SPI_ID,
                              baudrate=SD_INIT_BAUD,
                              sck=machine.Pin(SD_SCK_PIN),
                              mosi=machine.Pin(SD_MOSI_PIN),
                              miso=machine.Pin(SD_MISO_PIN))
        sd_cs = machine.Pin(SD_CS_PIN, machine.Pin.OUT, value=1)
        sd = sdcard.SDCard(sd_spi, sd_cs,
                           baudrate=SD_DATA_BAUD,
                           restore_baudrate=SPI_BAUD)
        uos.mount(sd, '/sd')
        print("SD mounted at /sd")
        return True
    except Exception as e:
        print(f"SD mount failed: {e}")
        return False


def load_bios_file(path):
    # Reads straight into msx.get_bios_view() — a zero-copy view of the
    # already-resident 32KB msx->bios buffer (C side) — a few KB at a time
    # via readinto_chunked(), rather than reading into any kind of
    # separate Python-owned scratch buffer first. This board's GC heap is
    # tight enough that even a *shared, lazily-allocated* ~32KB scratch
    # bytearray was observed to fail allocation right after gc.collect()
    # on real hardware; reading directly into memory the emulator already
    # has (same principle as msx.get_ram_view()/get_vram_view() for
    # save-states) needs no GC-heap allocation at all for this. Chunked
    # reads also avoid one single large multi-block SD transaction, which
    # separately was observed to occasionally corrupt the read silently
    # (BIOS "loaded" with the right byte count, but the MSX never produced
    # a visible boot screen afterward) or outright raise OSError from
    # sdcard.py's readblocks().
    #
    # Returns True on success (msx.mark_bios_loaded() also validates size
    # is in BIOS's required [0x4000, 0x8000] range — an oversized/corrupt
    # file is rejected up front instead of being silently truncated to fit
    # the view, which bit us once before with a different kind of
    # truncated ROM file).
    try:
        view = msx.get_bios_view()
        size = uos.stat(path)[6]
        if size > len(view):
            print(f"Cannot load {path}: {size} bytes exceeds {len(view)}-byte limit")
            return False
        with open(path, 'rb') as f:
            n = readinto_chunked(f, view, size)
        if not msx.mark_bios_loaded(n):
            print(f"Cannot load {path}: {n} bytes is not a valid BIOS size")
            return False
        print(f"Loaded {path}  ({n} bytes)")
        return True
    except OSError as e:
        print(f"Cannot open {path}: {e}")
        return False


def init_usb():
    """Initialize USB host once; idempotent."""
    global _usb_ready
    if usb_host is None or _usb_ready:
        return
    try:
        usb_host.init()
        _usb_ready = True
        print("USB host ready")
    except Exception as e:
        print(f"USB host init failed: {e}")
        return

    # usb_host.init() reconfigures clk_sys for USB PHY timing (see
    # usb_host_core.c: set_sys_clock_khz(240000, ...) — 240MHz is the
    # highest clean multiple of 12MHz this board runs reliably at with USB
    # host active; the original 144MHz choice there capped CPU-bound
    # emulation speed hard, roughly halving FPS) and resets clk_peri to a
    # fixed 48MHz independently of that — silently capping SPI baud again.
    # Re-sync clk_peri to the (now 240MHz) clk_sys and refresh the UART's
    # baud divisor to match, same as the early boost_peri_clock() call in
    # run(). Must happen BEFORE start_bg_timer(): doing it after was
    # measured to not stick (likely raced against early port/enumeration
    # activity resetting clk_peri again).
    msx.boost_peri_clock()
    try:
        _uart = machine.UART(0, baudrate=115200,
                              tx=machine.Pin(0), rx=machine.Pin(1), txbuf=32)
        uos.dupterm(_uart)
    except Exception as e:
        print(f"UART refresh after usb_host clk change failed: {e}")

    try:
        if hasattr(usb_host, 'start_bg_timer'):
            usb_host.start_bg_timer(8)
    except Exception as e:
        print(f"USB host bg timer failed: {e}")


def save_state():
    print("Saving state…")
    try:
        save_state_to(msx, SAVE_PATH)
        print(f"State saved to {SAVE_PATH}")
    except Exception as e:
        print(f"Save failed: {e}")


def load_state():
    print("Loading state…")
    try:
        if load_state_from(msx, SAVE_PATH):
            print(f"State loaded from {SAVE_PATH}")
        else:
            print("load_state_from() failed — invalid save file?")
    except Exception as e:
        print(f"Load failed: {e}")


def _show_error(msg1, msg2=""):
    """Display a simple error screen and return."""
    try:
        from msx_menu import MenuCanvas, C_RED, C_WHITE, C_GRAY
        c = MenuCanvas(msx)
        c.clear()
        c.text("ERROR", 4, 20, C_RED)
        c.text(msg1[:31], 4, 40, C_WHITE)
        if msg2:
            c.text(msg2[:31], 4, 52, C_GRAY)
        c.text("Reboot to retry", 4, 80, C_GRAY)
        c.flush()
    except Exception:
        pass


# -----------------------------------------------------------------------
# USB keyboard
# -----------------------------------------------------------------------
_last_modifier = 0
_last_keycodes = b'\x00' * 6

_save_held = False
_load_held = False
_menu_held = False
_bios_name = ""
_hdmi_enabled = False
_display_mode = 'both'   # 'both' | 'lcd' | 'hdmi' — see HDMI Settings menu
_boot_exclusive = False  # msx.ini: boot_exclusive=1 (only meaningful with
                         # display='lcd'/'hdmi', never 'both'); restart-only.
_hdmi_frame_skip = 1
_hdmi_baud = HDMI_BAUD    # override via msx.ini: hdmi_baud=9000000

# Set once in run() right after display init, from the same values passed to
# msx.init_display_hardware() — kept around so poll_keyboard()'s menu-crash
# recovery path (see the OSError handler around show_emulator_menu()) can
# re-run that same call to try to unstick the LCD after an SD I/O error
# leaves it frozen, without needing run()'s locals.
_lcd_w = 0
_lcd_h = 0
_rotate_180 = False
_lcd_model = DEFAULT_LCD_MODEL  # 'ST7796'|'ILI9341'; restart-only, see Display Settings.

def _init_hdmi_output():
    """Callback for the HDMI Settings menu — see msx_menu.show_emulator_menu().
    Called the moment HDMI is turned on live from Off (e.g. msx.ini had
    hdmi=0/absent at boot); idempotent, safe to call more than once."""
    msx.init_hdmi_output(HDMI_CS_PIN, _hdmi_baud)
    # Blank whatever the receiver was last showing (e.g. left over from a
    # previous emulator/session) before this emulator's own frames start.
    msx.clear_hdmi()


def poll_keyboard():
    global _last_modifier, _last_keycodes, _save_held, _load_held, _menu_held
    global _hdmi_enabled, _display_mode, _hdmi_frame_skip
    global _lcd_model, _rotate_180, _hdmi_baud, _boot_exclusive
    if not _usb_ready:
        return
    try:
        report = usb_host.get_hid_report()
    except Exception:
        return
    if not (report and len(report) >= 8):
        return

    mod = report[0]
    kc  = report[2:8]

    # Intercept F5 (save) and F8 (load) before passing to MSX matrix.
    # Edge-triggered: only fire once per physical key-press.
    f5_down = HID_F5 in kc
    f8_down = HID_F8 in kc
    if f5_down and not _save_held:
        save_state()
    if f8_down and not _load_held:
        load_state()
    _save_held = f5_down
    _load_held = f8_down

    # GUI+F7: runtime emulator menu (cart swap, save/load, reset).
    # Edge-triggered so holding the combo doesn't reopen the menu.
    menu_down = (mod & (MOD_LGUI | MOD_RGUI)) != 0 and HID_F7 in kc
    if menu_down and not _menu_held:
        # 2026-08-29: poll_keyboard() (this function) is deliberately called
        # from run()'s main loop *while the current frame's LCD DMA transfer
        # is still in flight* (started by render_to_display_1to1() just
        # before this call, not waited-for via wait_display() until after —
        # see the "Poll USB keyboard... while DMA runs" comment there). If
        # GUI+F7 lands in that window, show_emulator_menu()'s very first
        # draw (_draw_runtime_menu() -> MenuCanvas.flush() ->
        # render_to_display_1to1()) reconfigures and restarts the *same*
        # DMA channel/SPI peripheral without msx_render_to_display_1to1()
        # ever checking whether the previous transfer actually finished —
        # a real, timing-dependent race, independent of which physical
        # board is running this firmware (matches: swapping boards didn't
        # change the failure rate). Settle the in-flight transfer here,
        # before the menu can touch the bus at all, at the (rare) cost of
        # a wait_display() the pipeline was specifically trying to avoid —
        # only on the GUI+F7 path, not every frame.
        msx.wait_display()
        # Safety net: show_emulator_menu() already catches OSError around
        # most of its own SD access, but an unhandled OSError from *some*
        # path inside it (still being tracked down — see 2026-08-29
        # troubleshooting notes) was observed on real hardware to kill this
        # whole run() loop, ending the game session over what should be a
        # recoverable SD hiccup. Catch broadly here as a last resort, and
        # log the full traceback to the Pico's *internal flash* (not /sd —
        # logging to the same card that just failed would be circular, and
        # serial output has repeatedly shown dropped/garbled characters —
        # sometimes whole lines — on this hardware during this
        # investigation, so a clean file is more trustworthy than trusting
        # what scrolled by on the terminal). Retrieve with:
        #   mpremote cp :crashlog.txt .
        try:
            hdmi_state, display_state = show_emulator_menu(
                msx, usb_host, ROM_DIR, {_bios_name}, SAVE_PATH, CONFIG_PATH,
                hdmi_state={'enabled': _hdmi_enabled, 'display': _display_mode,
                            'frame_skip': _hdmi_frame_skip},
                init_hdmi_output=_init_hdmi_output,
                display_state={'lcd': _lcd_model, 'rotate': _rotate_180,
                                'hdmi_baud': _hdmi_baud,
                                'boot_exclusive': _boot_exclusive})
        except Exception as e:
            print(f"Menu crashed: {e!r} — resuming gameplay")
            try:
                import sys
                with open('/crashlog.txt', 'w') as _f:
                    sys.print_exception(e, _f)
            except Exception as log_e:
                print(f"(also failed to write /crashlog.txt: {log_e!r})")
            # Observed on real hardware (2026-08-29): after this exact
            # OSError, gameplay resumes but the LCD never draws again —
            # whatever the failed SD transaction left the shared SPI1
            # peripheral in, msx_render_to_display_1to1()'s lightweight
            # per-frame reconfigure (baudrate/format/DMA-enable bits only,
            # see its comment in msx_core.c) isn't enough to recover from.
            # msx.init_display_hardware() re-runs the LCD's own full panel
            # init sequence (SWRESET etc.) — a real recovery attempt, not
            # guaranteed to fix a wedged SPI *peripheral* specifically
            # (it deliberately avoids the hardware spi_init() reset, to
            # not pull the rug out from under the SD driver's own SPI
            # object — see that function's comment), but cheap to try
            # before giving up on the display for the rest of the session.
            if _lcd_w:
                try:
                    msx.init_display_hardware(
                        SPI_ID, SPI_BAUD, SPI_MOSI, SPI_SCK,
                        SPI_CS, SPI_DC, SPI_RST, SPI_BL,
                        _lcd_w, _lcd_h, _rotate_180)
                    print("Display re-init attempted after menu crash")
                except Exception as disp_e:
                    print(f"Display re-init also failed: {disp_e!r}")
            hdmi_state = {'enabled': _hdmi_enabled, 'display': _display_mode,
                          'frame_skip': _hdmi_frame_skip}
            display_state = {'lcd': _lcd_model, 'rotate': _rotate_180,
                             'hdmi_baud': _hdmi_baud,
                             'boot_exclusive': _boot_exclusive}
        _hdmi_enabled    = hdmi_state['enabled']
        _display_mode    = hdmi_state['display']
        _hdmi_frame_skip = hdmi_state['frame_skip']
        # display_state has no live effect — kept only so Display Settings
        # shows the last-picked values if reopened this session.
        _lcd_model      = display_state['lcd']
        _rotate_180     = display_state['rotate']
        _hdmi_baud      = display_state['hdmi_baud']
        _boot_exclusive = display_state['boot_exclusive']
        set_display_state(_display_mode, _hdmi_enabled)
        # Force the next report through regardless of whether it matches
        # what was last applied (keys held during the menu shouldn't leak
        # into the MSX matrix, and the menu's own key reads may have left
        # _last_keycodes stale).
        _last_modifier, _last_keycodes = -1, b''
        msx.clear_keys()
    _menu_held = menu_down
    if menu_down:
        return  # don't also forward GUI/F7 to the MSX matrix while held

    # Pass remaining keys to MSX matrix (F5/F8 are still forwarded;
    # MSX F5 maps to the STOP/BREAK key area which is rarely harmful)
    if mod != _last_modifier or kc != _last_keycodes:
        _last_modifier, _last_keycodes = mod, kc
        apply_hid_report(msx, mod, kc)


# -----------------------------------------------------------------------
# Joystick (Atari/MSX 9-pin port, direct GPIO)
# -----------------------------------------------------------------------
_joy_pins = None

def init_joystick():
    """Configure joystick GPIO as PULL_UP inputs. Safe to call even if the
    port isn't physically connected — floating/pulled-up pins just read as
    permanently released, which is the correct 'no joystick' state."""
    global _joy_pins
    try:
        pull = machine.Pin.PULL_UP
        _joy_pins = (
            machine.Pin(JOY_UP_PIN,     machine.Pin.IN, pull),
            machine.Pin(JOY_DOWN_PIN,   machine.Pin.IN, pull),
            machine.Pin(JOY_LEFT_PIN,   machine.Pin.IN, pull),
            machine.Pin(JOY_RIGHT_PIN,  machine.Pin.IN, pull),
            machine.Pin(JOY_TRIG_A_PIN, machine.Pin.IN, pull),
            machine.Pin(JOY_TRIG_B_PIN, machine.Pin.IN, pull),
        )
    except Exception as e:
        print(f"Joystick GPIO init failed: {e}")
        _joy_pins = None


def poll_joystick():
    """Read GPIO state into the PSG-visible joystick register (JOY1).
    Pin.value() is already 1=released/0=pressed with PULL_UP wiring, which
    matches the MSX joystick register's active-low bit convention directly."""
    if _joy_pins is None:
        return
    up, down, left, right, trig_a, trig_b = (p.value() for p in _joy_pins)
    state = (up | (down << 1) | (left << 2) | (right << 3) |
             (trig_a << 4) | (trig_b << 5) | 0xC0)
    msx.set_joystick(0, state)


# -----------------------------------------------------------------------
# Main
# -----------------------------------------------------------------------
def run():
    # 1 — Init MSX state (no hardware yet)
    msx.init()

    # 1.5 — clk_peri defaults to a fixed 48 MHz on this board regardless of
    #       machine.freq(), silently capping SPI baud well below what's
    #       requested (e.g. a 40 MHz request clamps to 24 MHz). Reconfigure
    #       clk_peri to track clk_sys, then re-create the UART so its baud
    #       divisor is recalculated against the new clock.
    msx.boost_peri_clock()
    try:
        _uart = machine.UART(0, baudrate=115200,
                              tx=machine.Pin(0), rx=machine.Pin(1), txbuf=32)
        uos.dupterm(_uart)
    except Exception as e:
        print(f"UART refresh after clk_peri change failed: {e}")

    # 2 — USB host
    init_usb()

    # 3 — Mount SD card FIRST so SPI1 is configured by SD init before LCD.
    #     If LCD were initialized before SD, the SD driver would reconfigure
    #     SPI1 at 400 kHz and break subsequent DMA rendering.
    has_sd = mount_sd()

    # 4 — Read optional config file (before display init: it may select
    #     panel size — LCD_SIZES — and whether to skip LCD/HDMI init
    #     entirely for an exclusive boot — see boot_exclusive below).
    cfg = load_config(CONFIG_PATH) if has_sd else {}
    lcd_model = cfg.get('lcd', DEFAULT_LCD_MODEL)
    lcd_w, lcd_h = LCD_SIZES.get(lcd_model, LCD_SIZES[DEFAULT_LCD_MODEL])
    rotate_180 = cfg.get('rotate', '0').strip() == '180'

    # Parsed early (before LCD init) so it can decide whether that runs.
    # 'both'+boot_exclusive is contradictory, treated as boot_exclusive=0.
    global _display_mode, _boot_exclusive
    _display_mode = cfg.get('display', 'both').strip().lower()
    if _display_mode not in ('both', 'lcd', 'hdmi'):
        _display_mode = 'both'
    _boot_exclusive = (cfg.get('boot_exclusive', '0').strip() == '1'
                       and _display_mode != 'both')

    # 5 — Initialize display AFTER SD: SPI1 is now stable at SPI_BAUD.
    #     Skipped when boot_exclusive+display=hdmi (msx_core.c's
    #     hdmi_apply_spi_settings() no longer depends on this having run).
    global _lcd_w, _lcd_h, _rotate_180, _lcd_model
    if _boot_exclusive and _display_mode == 'hdmi':
        print("Display: LCD init skipped (boot_exclusive + display=hdmi)")
        _lcd_w, _lcd_h, _rotate_180, _lcd_model = 0, 0, False, lcd_model
    else:
        print(f"Initializing display… ({lcd_model} {lcd_w}x{lcd_h}"
              f"{', rotated 180' if rotate_180 else ''})")
        msx.init_display_hardware(
            SPI_ID, SPI_BAUD,
            SPI_MOSI, SPI_SCK,
            SPI_CS, SPI_DC, SPI_RST, SPI_BL,
            lcd_w, lcd_h, rotate_180
        )
        _lcd_w, _lcd_h, _rotate_180 = lcd_w, lcd_h, rotate_180
        _lcd_model = lcd_model

    # 5.1 — Optional HDMI bridge output (hdmi_bridge/README.md). Must come
    #       after init_display_hardware() (reuses its SPI1 instance). Off
    #       by default so users without the second Pico2+PICO-HDMI-PLUS are
    #       unaffected — msx.ini: hdmi=1. All three settings (hdmi/
    #       display/hdmi_frame_skip) can also be changed live afterward via
    #       the GUI+F7 "HDMI Settings" menu (see poll_keyboard()); the
    #       globals set here are just the msx.ini-driven starting point.
    #
    #       Initialized here — BEFORE BIOS/cart loading — so a display=hdmi
    #       (or display=both) setup shows the boot-time interactive ROM
    #       selector (step 7 below) and any boot error screen on HDMI too,
    #       not just the LCD.
    #
    #       This used to be placed after BIOS/cart loading instead, to
    #       dodge a real bug: HDMI's SPI mode 3 (CPOL=1,CPHA=1, see
    #       hdmi_apply_spi_settings() in msx_core.c) would get left on the
    #       shared SPI1 bus, and mp/sdcard.py's readblocks()/writeblocks()
    #       only ever called self.spi.init(baudrate=...) — MicroPython's
    #       machine.SPI.init() (ports/rp2/machine_spi.c) only reprograms
    #       CPOL/CPHA when polarity=/phase= are passed explicitly, so a
    #       baudrate-only call silently left the bus in mode 3 and every SD
    #       read after any HDMI activity failed with "OSError: timeout
    #       waiting for response". Root-caused and fixed by making
    #       sdcard.py always pass polarity=0, phase=0 too (see its comment
    #       above readblocks()) — SD access is now correct regardless of
    #       what else touched the bus beforehand, so HDMI can safely be
    #       initialized this early again.
    # display/boot_exclusive already parsed above (step 4).
    global _hdmi_enabled, _hdmi_frame_skip, _hdmi_baud
    _hdmi_enabled = cfg.get('hdmi', '0').strip() == '1'

    try:
        _hdmi_frame_skip = max(1, int(cfg.get('hdmi_frame_skip', HDMI_FRAME_SKIP)))
    except (ValueError, TypeError):
        _hdmi_frame_skip = HDMI_FRAME_SKIP

    try:
        _hdmi_baud = max(1, int(cfg.get('hdmi_baud', HDMI_BAUD)))
    except (ValueError, TypeError):
        _hdmi_baud = HDMI_BAUD

    # boot_exclusive + display=lcd: skip HDMI init even if hdmi=1 (mirrors
    # the LCD-skip above); can still be turned on later via the menu.
    if _hdmi_enabled and _boot_exclusive and _display_mode == 'lcd':
        print("HDMI bridge: init skipped (boot_exclusive + display=lcd)")
        _hdmi_enabled = False
    elif _hdmi_enabled:
        print(f"HDMI bridge output enabled (CS=GP{HDMI_CS_PIN}, "
              f"{_hdmi_baud/1e6:.1f}MHz, display={_display_mode}, "
              f"frame_skip={_hdmi_frame_skip})")
        msx.init_hdmi_output(HDMI_CS_PIN, _hdmi_baud)
        # Blank whatever the receiver was last showing (e.g. left over from
        # a previous emulator/session) before this emulator's own frames
        # start — otherwise the old screen just sits there until the first
        # HDMI frame is actually rendered.
        msx.clear_hdmi()

    # Mirror the display state into msx_menu so MenuCanvas.flush() (used by
    # the GUI+F7 menus, the interactive ROM selector, and the boot error
    # screen below) also updates HDMI instead of only ever repainting the
    # LCD.
    set_display_state(_display_mode, _hdmi_enabled)

    # 5.5 — Joystick GPIO (Atari/MSX 9-pin port, JOY1)
    init_joystick()

    # 6 — Load BIOS
    global _bios_name
    bios_path = cfg.get('bios', DEFAULT_BIOS)
    _bios_name = bios_path.rsplit('/', 1)[-1].lower()
    # gc.collect() here is no longer load-bearing for the BIOS read itself
    # (load_bios_file() reads straight into C-side memory now, no GC-heap
    # allocation needed for it) but is still cheap insurance against
    # whatever else has accumulated by this point in boot.
    import gc
    gc.collect()
    bios_ok = load_bios_file(bios_path) if has_sd else False

    if not bios_ok and bios_path != DEFAULT_BIOS:
        print(f"Config BIOS not found, trying fallback: {DEFAULT_BIOS}")
        bios_ok = load_bios_file(DEFAULT_BIOS)

    if not bios_ok:
        print(f"ERROR: MSX BIOS not found at {bios_path}")
        _show_error("MSX BIOS not found", DEFAULT_BIOS)
        return

    # 7 — Load cartridge (config file → interactive selector → none → BASIC)
    # load_cart_smart() picks in-RAM vs SD-backed paged loading (Mega ROM,
    # >32KB) automatically — see msx_menu.py. HDMI is already active at
    # this point (step 5.1 above), so the interactive selector below shows
    # there too, not just on the LCD.
    if 'cart' in cfg:
        # Explicit path from msx.ini
        if has_sd:
            try:
                ok = load_cart_smart(msx, 0, cfg['cart'])
            except OSError:
                ok = False
            print(f"Cart (config): {cfg['cart']}  {'OK' if ok else 'FAILED'}")
        else:
            print(f"WARNING: configured cart not found: {cfg['cart']}")

    elif has_sd:
        # Interactive selector — auto-select if no keyboard or only one file
        # (exclude the BIOS file so it isn't offered as a cartridge)
        selected = select_rom(
            msx, ROM_DIR,
            title="Select Cartridge ROM",
            usb_host_mod=usb_host if _usb_ready else None,
            auto_if_one=not _usb_ready,   # auto-select when no keyboard
            exclude_names={_bios_name},
        )
        if selected:
            ok = load_cart_smart(msx, 0, selected)
            print(f"Cart (menu): {selected}  {'OK' if ok else 'FAILED'}")
        else:
            print("No cartridge — booting MSX BASIC")

    # 8 — Audio: PWM + 22050 Hz repeating timer (ISR feeds ring buffer)
    msx.setup_audio_pwm(AUDIO_PIN)
    try:
        msx.set_audio_volume(int(cfg.get('volume', 256)))
        msx.set_audio_filter(int(cfg.get('audio_filter', 0)))
    except (ValueError, TypeError) as e:
        print(f"Bad volume/audio_filter in msx.ini: {e}")

    # 8.5 — Load /sd/msx/ext/ and /ext/ extension modules (CALL/RST hook
    # plugins — see mp/msx_ext.py and doc/extension_api.md). Runs before
    # reset() so any hooks are already in place when the machine starts.
    load_extensions(msx)

    # 9 — Reset and start
    msx.reset()
    print("MSX started")

    frame = 0
    t0    = time.ticks_ms()

    # Mega ROM carts fetch bank-switched pages mid-frame (inside
    # msx.run_frame()) from a copy on the Pico's own onboard flash — see
    # load_cart_smart()'s flash-cache in msx_menu.py. An earlier version
    # fetched straight from SD instead, which shares SPI1 with the LCD;
    # overlapping that with a concurrent display DMA transfer caused real
    # SD I/O errors on hardware, forcing a slower non-pipelined fallback
    # loop. Onboard flash is a separate QSPI peripheral with no SPI1
    # contention, so the normal pipelined loop below is safe again
    # regardless of whether a paged cart is loaded.
    #
    # Pipelined loop: framebuf is double-buffered (see msx_core.c), so the
    # display DMA for frame N can run concurrently with the Z80/VDP
    # emulation of frame N+1 — this roughly halved per-frame time versus
    # running them back-to-back (measured ~29fps -> ~44fps at native
    # 256x192 resolution). Prime the pipeline with one frame first.
    msx.run_frame()

    while True:
        # These read the live globals every iteration (not cached booleans)
        # so changes made via the GUI+F7 "HDMI Settings" menu take effect
        # on the very next frame, no restart needed.
        use_hdmi = _hdmi_enabled and _display_mode in ('both', 'hdmi')
        # Safety fallback: always fall back to the LCD if HDMI isn't
        # actually usable right now (e.g. display=hdmi in msx.ini but
        # hdmi=1 was never set, or HDMI was just turned Off from the menu
        # while display was still 'hdmi') — otherwise neither output would
        # render anything and the screen would just freeze with no way to
        # see what's happening (found on real hardware).
        use_lcd  = _display_mode in ('both', 'lcd') or not use_hdmi

        # Start DMA transfer of the just-completed frame (non-blocking).
        # 1:1 native 256x192 (no 1.5x scaling) — scaling nearly doubled
        # transfer time and made gameplay feel like slow motion.
        # Skipped entirely when display=hdmi — recovers the LCD's DMA+wait
        # cost for users who only care about the HDMI output.
        if use_lcd:
            msx.render_to_display_1to1()

        # Compute the NEXT frame while the DMA above is still in flight —
        # msx_run_frame() writes into the other framebuf, so this is safe.
        msx.run_frame()

        # Poll USB keyboard and joystick while DMA runs
        poll_keyboard()
        poll_joystick()

        # Block until DMA and SPI shift register finish; deasserts CS.
        # Safe/cheap no-op if render_to_display_1to1() wasn't called above.
        if use_lcd:
            msx.wait_display()

        # Optional HDMI bridge output — sends the same frame just shown on
        # the LCD, over the same SPI1 bus but a different CS (GP28). Placed
        # strictly after wait_display() (LCD SPI1 use fully finished) and
        # before the next render_to_display_1to1() call (which re-applies
        # the LCD's own SPI settings), so the two never overlap on the
        # shared bus — see hdmi_bridge/README.md's bus-contention note.
        # Blocking (~20ms at the default 10MHz with PAL4) — sending every
        # frame noticeably slows overall FPS on real hardware when the LCD
        # was also active, so only send every hdmi_frame_skip-th frame by
        # default; the HDMI picture updates at a lower rate than the LCD
        # but the emulation speed recovers. With display=hdmi (LCD fully
        # skipped above), consider frame_skip=1 for full-rate HDMI updates
        # since the LCD's cost is no longer also being paid.
        if use_hdmi and frame % _hdmi_frame_skip == 0:
            msx.render_to_hdmi()

        frame += 1

        if frame % 300 == 0:
            elapsed = time.ticks_diff(time.ticks_ms(), t0)
            fps     = 300_000 / elapsed if elapsed > 0 else 0.0
            ring    = msx.get_audio_ring_level()
            print(f"FPS: {fps:.1f}  ring: {ring}")
            t0 = time.ticks_ms()


if __name__ == '__main__':
    run()
