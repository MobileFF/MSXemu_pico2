# MSX1 Emulator — Main entry point
# Raspberry Pi Pico 2 (RP2350) + MicroPython
#
# Hardware (matches PB-1000 emulator board):
#   LCD              SPI1: MOSI=GP11  SCK=GP10  CS=GP9  DC=GP8  RST=GP7   BL=GP22
#                    ST7796 480x320 (MSP4021) or ILI9341 320x240 (MSP2402) —
#                    same wiring/init sequence for both, see LCD_SIZES below.
#   SD card          SPI1: MOSI=GP11  SCK=GP10  MISO=GP12 CS=GP15  (shared with LCD SPI)
#   USB keyboard     native host (GP24/25)
#   Audio PWM        GP14
#   HDMI bridge      SPI1: MOSI=GP11  SCK=GP10  CS=GP28  (optional, shares LCD/SD
#                    SPI bus; second Pico2+PICO-HDMI-PLUS, see hdmi_bridge/README.md;
#                    opt-in via msx.ini: display=hdmi — mutually exclusive
#                    with the LCD, see 'display' below)
#
# SD card layout:
#   /sd/msx.ini         — optional config (see below; was /sd/msx/config.txt)
#   /sd/msx/MSX.ROM     — 32KB MSX BIOS+BASIC (required)
#   /sd/<anywhere>/<name>.ROM — cartridge ROM (selector browses from the
#                         card root, subfolders included)
#
# msx.ini format (key=value, # = comment):
#   bios=/sd/msx/MSX.ROM
#   cart=/sd/msx/CART.ROM
#   mode=disk
#   diskrom=/sd/msx/DISK.ROM
#   disk=/sd/msx/GAME.DSK
#   fdc_base=0x7FB8
#   lcd=ILI9341
#   rotate=180
#   volume=128
#   audio_filter=2
#   display=hdmi
#   hdmi_frame_skip=2
#   hdmi_baud=8000000
#   hdmi_scale=2
#   # omit 'cart' to show the interactive ROM selector at boot (browses from
#   # the SD root incl. subfolders; UP/DOWN move, ENTER opens a folder/picks
#   # a ROM, ESC goes up a level/cancels at the root)
#   # mode=disk switches to virtual FDD support (see mp/msx_fdd.py):
#   # cart slot 1 loads 'diskrom' (a Disk ROM binary, same copyright-onus-
#   # on-user policy as bios=) instead of 'cart', and the runtime menu's
#   # "Swap Cartridge" becomes "Swap Disk" (browses .DSK files instead of
#   # .ROM). Mutually exclusive with 'cart' (heap-budget reasons — a Disk
#   # ROM + a Mega ROM cart loaded together risks exceeding the 64KB C
#   # heap) — 'cart'/'mode=disk' together in msx.ini is invalid; 'cart' is
#   # ignored when mode=disk. omit 'disk', or point it at a file that no
#   # longer exists, to show the interactive .DSK selector at boot, same
#   # UX as omitting/losing 'cart'. 'fdc_base' (decimal or 0x-hex) is the
#   # Z80 address of the emulated WD179x FDC's Status/Command register
#   # within the Disk ROM's own page — omit to use the default (0x7FB8,
#   # confirmed by disassembly for the Disk ROM this was built against); a
#   # different Disk ROM may memory-map its FDC elsewhere, in which case
#   # override this. First-pass scope: a single drive (A:), flat/non-paged
#   # Disk ROMs only, standard 360KB/720KB geometry — see mp/msx_fdd.py's
#   # docstring for the full list of caveats.
#   # omit 'lcd' to default to ST7796 (see LCD_SIZES for valid names)
#   # omit 'rotate' (or 0) for normal orientation; 180 flips the panel. Only
#   # 0/180 supported (landscape MADCTL flip only, not a true 90/270
#   # rotation); invalid values silently fall back to 0. LCD-only — has no
#   # effect on HDMI output.
#   # omit 'volume' to default to 256; lower (e.g. 128) reduces a passive
#   # piezo buzzer's overdrive/crackle
#   # omit 'audio_filter' to default to 0 (no smoothing); 1-8 = progressively
#   # heavier low-pass smoothing
#   # omit 'display' (or set to 'lcd') to use the LCD panel only — the
#   # optional HDMI bridge output (second Pico2+PICO-HDMI-PLUS, see
#   # hdmi_bridge/README.md) is only initialized at all when display=hdmi.
#   # LCD and HDMI are mutually exclusive: no combined mode (simultaneous
#   # LCD+HDMI was found to corrupt/lose the HDMI signal and glitch the LCD
#   # on real hardware from switching SPI mode every frame — see
#   # doc/hdmi_bridge_phase2_report.md). Only whichever side is skipped at
#   # boot has its hardware init deferred (real boot-time saving, and that
#   # side need not be physically present) — switching 'display' live from
#   # the menu brings the other side up on demand at that point.
#   # omit 'hdmi_frame_skip' to default to 2 (send to HDMI every other
#   # frame); 1 = every frame. Only matters when display=hdmi.
#   # omit 'hdmi_baud' to default to 8_000_000 (8MHz — 5/8MHz confirmed
#   # clean on real hardware, 10MHz corrupts the received palette on this
#   # wiring; see doc/hdmi_bridge_phase2_report.md). Only matters when
#   # display=hdmi.
#   # omit 'hdmi_scale' to default to 1 (native 256x192 centered on the
#   # receiver's 640x480 output); 2 = 512x384. Values above 2 behave as 2
#   # (the receiver steps the factor down until the frame fits). Applies to
#   # game and menu frames alike, costs no extra SPI traffic (the receiver
#   # does the upscaling). Only matters when display=hdmi.
#   # ext=off skips loading /sd/msx/ext/ and /ext/ extension modules (see
#   # mp/msx_ext.py, doc/extension_api.md). Default is to load them.
#   # Real-hardware finding: once ANY extension registers even one
#   # msx.set_call_hook(), the Z80 core's four hook-trigger points (see
#   # z80.h's call_hook comment) each pay a small per-instruction/per-CALL/
#   # JP/JR check for the rest of the session, whether or not that hook's
#   # address is ever actually reached — a measurable FPS cost for
#   # cart-mode gameplay that never touches a hook at all. ext=off avoids
#   # it entirely by never calling msx.set_call_hook() in the first place,
#   # which is different from mode=disk's own DSKCHG hook (mp/msx_fdd.py)
#   # — that one is unaffected by ext=off and still registers normally,
#   # since it's required for FDD disk-swap detection, not an ext/ plugin.
#   #
#   # All of the above except bios/cart (ROM selector instead) can be tuned
#   # live via GUI+F7 — Audio/Display Settings. ENTER writes it back to
#   # msx.ini; audio/display/frame_skip/hdmi_scale take effect live, while
#   # lcd/rotate/hdmi_baud need a restart (read once at boot; the same menu
#   # also offers a "Reinit ... now" action for the active side, no restart
#   # needed — see msx_display_settings.py).
#
# Joystick (Atari/MSX 9-pin port wired directly to GPIO, PULL_UP/active-low,
# JOY1 only): UP=GP18 DOWN=GP19 LEFT=GP20 RIGHT=GP21 TRIG-A=GP26 TRIG-B=GP27.
# Read via the PSG I/O ports (register 14/15, port 0xA1/0xA2) exactly like
# real MSX hardware — see msx_set_joystick()/poll_joystick() below.
#
# 2026-09-06: this header used to be a triple-quoted module docstring —
# converted to plain '#' comments after a MemoryError re-emerged compiling
# this file at boot. A docstring is compiled into a real, retained string
# constant; a '#' comment is skipped entirely by the lexer and costs
# nothing at runtime (same principle already applied to msx_menu.py during
# its own MemoryError fix earlier this session). Purely mechanical — no
# code depends on main.py.__doc__.

import sys
import time
import uos
import machine

try:
    import msx
except ImportError:
    print("ERROR: 'msx' C module not found — rebuild with micropython_msx.cmake")
    sys.exit(1)

# 2026-09-30: __DATE__/__TIME__, baked into the C module at actual compile
# time (msx.build_info(), modmsx.c) — printed first thing so a real-
# hardware log unambiguously shows which firmware.uf2 is actually running,
# not just which mp/*.py happened to be run. This project has repeatedly
# lost debugging time to testing against a stale build without noticing
# (see log/rp2350-pizero-bringup-2026-09-29.md §4).
print(f"msx module build: {msx.build_info()}")

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
from msx_keymap import (apply_hid_report, HID_F7, HID_P, HID_ESC, HID_DELETE,
                       MOD_LGUI, MOD_RGUI, MOD_LCTRL, MOD_RCTRL, MOD_LALT, MOD_RALT)
from msx_ext    import load_extensions
from msx_menu   import (select_rom, load_config, show_emulator_menu,
                        load_cart_smart, set_display_state, readinto_chunked,
                        log_mem, get_rom_load_buf)
log_mem("boot, after main.py/msx_menu.py compiled")

# -----------------------------------------------------------------------
# Pin / peripheral constants — per-board, see board_config.py (2026-09-20,
# phase 1 of the RP2350-PiZero port — 調査用/RP2350-PiZero_HDMI出力適用調査.md
# §7). Everything below this point stays 100% hardware-agnostic; only
# board_config.py differs between a Pico 2 and a PiZero build.
# -----------------------------------------------------------------------
from board_config import *

# ROM_DIR is the SD root — select_rom() browses subfolders too (see
# msx_rom_browser.py's _list_dir_entries()). BIOS stays under /sd/msx/.
ROM_DIR      = "/sd"
CONFIG_PATH  = "/sd/msx.ini"
DEFAULT_BIOS = "/sd/msx/MSX.ROM"
# 2026-09-05/06: per-cartridge saves sit next to the ROM itself, and keep
# a rotating history of up to msx_menu.MAX_SAVE_SLOTS past states (see
# save_base_for_cart()/rotate_and_save_state() in msx_menu.py — e.g.
# "/sd/games/Foo.ROM" -> "/sd/games/Foo.0.sav" (latest), "Foo.1.sav", ...).
# SAVE_BASE is only the fallback base used when no cart is loaded
# (BASIC-only session).
SAVE_BASE    = "/sd/msx/save"

# -----------------------------------------------------------------------
# Helpers
# 2026-09-30 A/B test (EIO investigation): confirmed the 1kHz
# pio_usb_sof_alarm_handler() interrupt is NOT the cause — real-hardware
# testing with _DIAG_ENABLE_USB=False still reproduced OSError([Errno 5]
# EIO) on cart load at the same rate (5/5 reboots). Reverted to True.
# Kept as a toggle in case it's useful again later; not currently used for
# active investigation (see SD_DATA_BAUD in board_config.py instead).
_DIAG_ENABLE_USB = True
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
        # restore_baudrate exists so pico2's SD driver can hand the shared
        # SPI1 bus back to the expected LCD speed after each SD
        # transaction (SD and LCD are on the same bus there). pizero has
        # no LCD at all (SPI_BAUD is None — see board_config.py) and SD
        # has its own independent bus, so there's nothing to "restore" to
        # — fall back to SD's own data baud (a no-op) instead of passing
        # None through to machine.SPI.init(), which can't convert it.
        sd = sdcard.SDCard(sd_spi, sd_cs,
                           baudrate=SD_DATA_BAUD,
                           restore_baudrate=SPI_BAUD if SPI_BAUD is not None else SD_DATA_BAUD)
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
    # Initialize USB host once; idempotent.
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

    # 2026-09-27 (Phase 4, PIO-USB): pizero's usb_host.init() does NOT
    # touch clk_sys/clk_peri at all — see hcd_pio_usb_pizero.c/
    # usb_host_core.c's usb_host_core_init_pizero() comment — clk_sys
    # must stay at onboard DVI's required 252MHz. Only the native
    # RP2350 host controller (pico2) needs the clk_sys/clk_peri dance
    # below; skip it entirely on pizero.
    if BOARD != "pizero":
        # usb_host.init() reconfigures clk_sys for USB PHY timing (see
        # usb_host_core.c: set_sys_clock_khz(240000, ...) — 240MHz is the
        # highest clean multiple of 12MHz this board runs reliably at with
        # USB host active; the original 144MHz choice there capped
        # CPU-bound emulation speed hard, roughly halving FPS) and resets
        # clk_peri to a fixed 48MHz independently of that — silently
        # capping SPI baud again. Re-sync clk_peri to the (now 240MHz)
        # clk_sys and refresh the UART's baud divisor to match, same as
        # the early boost_peri_clock() call in run(). Must happen BEFORE
        # start_bg_timer(): doing it after was measured to not stick
        # (likely raced against early port/enumeration activity resetting
        # clk_peri again).
        msx.boost_peri_clock()
        try:
            _uart = machine.UART(0, baudrate=115200,
                                  tx=machine.Pin(0), rx=machine.Pin(1), txbuf=32)
            uos.dupterm(_uart)
        except Exception as e:
            print(f"UART refresh after usb_host clk change failed: {e}")

    # 2026-09-28 (Phase 4, PIO-USB): pizero's usb_host_core_init_pizero()
    # already schedules its own dedicated hardware-alarm-based 1ms tick
    # that calls both pio_usb_host_frame() and tuh_task() itself (see
    # usb_host_core.c's pio_usb_sof_alarm_handler() — a deliberate,
    # hard-won design to avoid pico_time's alarm_pool machinery
    # entirely, after real-hardware hangs/panics tracing back to its
    # shared striped-spinlock use). Calling start_bg_timer() here too
    # would register a SECOND, redundant tuh_task() poller through
    # exactly the alarm_pool path just avoided — skip it on pizero.
    if BOARD != "pizero":
        try:
            if hasattr(usb_host, 'start_bg_timer'):
                usb_host.start_bg_timer(8)
        except Exception as e:
            print(f"USB host bg timer failed: {e}")


def _show_error(msg1, msg2=""):
    # Display a simple error screen and return.
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

_menu_held = False
_display_held = False  # GUI+P
_reinit_held = False   # GUI+ESC
_exit_requested = False  # Ctrl+Alt+Delete — see poll_keyboard()'s comment
_bios_name = ""
_cart_path = None  # currently loaded cart's full path, or None (BASIC
                   # only) — see save_base_for_cart() in msx_menu.py; the
                   # runtime menu's Save/Load State keys off this so each
                   # cart keeps its own rotating save history. (2026-09-08:
                   # F5/F8 used to double as quick-save/load hotkeys here
                   # too, but were removed — see poll_keyboard()'s comment.)
_fdd_mode  = False  # True when msx.ini's mode=disk — mutually exclusive
                    # with cartridge use (see mp/msx_fdd.py); cart slot 1
                    # holds the Disk ROM instead of a game cartridge in
                    # this mode, sidestepping the C-heap budget risk of
                    # loading both at once.
_disk_path = None  # currently mounted .dsk image's full path (mode=disk
                   # only), or None — mirrors _cart_path, used by the
                   # runtime menu's "Swap Disk" item.
_diskrom_path = None  # msx.ini's diskrom=, remembered even in cart mode
                      # (unlike _disk_path/_cart_path, not mode-gated) so
                      # the runtime menu's "Switch to Disk Mode" can reuse
                      # it without prompting again once known.
_display_mode = 'lcd'   # 'lcd' | 'hdmi' — mutually exclusive, see Display Settings menu
_hdmi_frame_skip = 1
_hdmi_baud = HDMI_BAUD    # override via msx.ini: hdmi_baud=9000000
_hdmi_scale = HDMI_SCALE  # override via msx.ini: hdmi_scale=2

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
    # Callback for the Display Settings menu (msx_display_settings.py) —
    # see msx_menu.show_emulator_menu(). Called the moment 'display'
    # switches to 'hdmi' live (boot may have started on 'lcd', which never
    # initializes HDMI hardware at all — see the exclusive boot logic in
    # run()); idempotent, safe to call more than once (e.g. if HDMI was
    # already the active side at boot, or the user picked "Reinit HDMI
    # now" — see msx_display_settings.py's show() docstring for why that
    # exists).
    msx.hdmi_reset_init(HDMI_RESET_PIN)
    msx.hdmi_reset_pulse()
    time.sleep_ms(HDMI_RESET_GRACE_MS)
    msx.init_hdmi_output(HDMI_CS_PIN, _hdmi_baud)
    # Blank whatever the receiver was last showing (e.g. left over from a
    # previous emulator/session) before this emulator's own frames start.
    msx.clear_hdmi()


def _init_lcd_output():
    # Callback for the Display Settings menu — mirrors _init_hdmi_output()
    # above for the other direction. Called the moment 'display' switches
    # to 'lcd' live (boot may have started on 'hdmi', which skips the LCD
    # panel's own init/reset sequence entirely — see run()). Idempotent:
    # msx.init_display_hardware() always re-runs the full panel reset
    # sequence, harmless to repeat (e.g. if LCD was already the active side
    # at boot, or the user picked "Reinit LCD now").
    msx.init_display_hardware(
        SPI_ID, SPI_BAUD, SPI_MOSI, SPI_SCK,
        SPI_CS, SPI_DC, SPI_RST, SPI_BL,
        _lcd_w, _lcd_h, _rotate_180)
    msx.set_backlight(True)


def poll_keyboard():
    global _last_modifier, _last_keycodes, _menu_held
    global _display_held, _reinit_held, _exit_requested
    global _display_mode, _hdmi_frame_skip, _hdmi_scale
    global _lcd_model, _rotate_180, _hdmi_baud, _cart_path, _disk_path
    global _fdd_mode, _diskrom_path
    if not _usb_ready:
        return
    if BOARD == "pizero":
        # pizero's dedicated hardware-alarm 1ms tick (usb_host_core.c's
        # pio_usb_sof_alarm_handler()) only drives pio_usb_host_frame()
        # itself — tuh_task() (actual device/HID enumeration + report
        # dequeuing) is deliberately NOT called from that interrupt
        # context (real-hardware hang, root cause unconfirmed — see that
        # function's own comment) and is instead polled here, once per
        # game frame, from ordinary (non-interrupt) context instead.
        try:
            usb_host.task()
        except Exception:
            pass
    try:
        report = usb_host.get_hid_report()
    except Exception:
        return
    if not (report and len(report) >= 8):
        return

    mod = report[0]
    kc  = report[2:8]

    # Ctrl+Alt+Delete: hard-exit main.py back to the REPL. Checked first,
    # unconditionally (no GUI-key gating, no edge-triggering — exiting is
    # a one-shot action) so it works regardless of what else is going on
    # (a menu open, gameplay running, another hotkey held). Added
    # 2026-09-13 after a real-hardware lockout: main.py's own USB-host
    # keyboard support (usb_host — pico2's native RP2350 host controller
    # on GP24/25, see this file's header; pizero uses PIO-USB instead,
    # see board_config.py) doesn't touch the native USB CDC
    # serial mpremote connects over, but a plain Ctrl+C sent over that
    # connection was not observed to interrupt a running main.py — so
    # this gives an escape hatch that only depends on the USB keyboard,
    # not on the PC/mpremote side working at all. See run()'s main loop
    # (`if _exit_requested: break`) and the cleanup right after it.
    if ((mod & (MOD_LCTRL | MOD_RCTRL)) and (mod & (MOD_LALT | MOD_RALT))
            and HID_DELETE in kc):
        _exit_requested = True
        return

    # 2026-09-08: F5/F8 used to be intercepted here as quick-save/load
    # hotkeys (edge-triggered, mirroring GUI+F7 below) — removed, since
    # they were *also* still forwarded to the MSX matrix as ordinary F5/
    # F8 keypresses afterward (apply_hid_report() doesn't know they'd
    # already been consumed), meaning any MSX software that itself uses
    # F5/F8 for something would see an unwanted keystroke every time the
    # emulator saved/loaded. Save/Load State remain available from the
    # runtime menu (GUI+F7) instead, which doesn't have this problem
    # (GUI+F7 is never forwarded to the matrix — see below).
    gui_down = (mod & (MOD_LGUI | MOD_RGUI)) != 0

    # GUI+P: toggle display=lcd/hdmi live, without opening any menu (and
    # so without importing msx_runtime_menu.py/msx_display_settings.py —
    # see their own MemoryError-under-heap-pressure history). Same effect
    # as Display Settings' "Display" field, just reachable in one
    # keystroke. Edge-triggered so holding the combo doesn't keep
    # toggling.
    display_toggle_down = gui_down and HID_P in kc
    if display_toggle_down and not _display_held:
        # Drain whatever the *previous* frame's render_to_hdmi() left
        # in-flight before init_display_hardware()/init_hdmi_output()
        # reconfigure the shared SPI1 peripheral out from under it —
        # msx_init_display_hardware() doesn't drain this itself (unlike
        # msx_send_hdmi_palette(), which init_hdmi_output() calls
        # internally and which already does). Same reasoning as GUI+F7's
        # wait_display() call below.
        msx.wait_display()
        _display_mode = 'lcd' if _display_mode == 'hdmi' else 'hdmi'
        set_display_state(_display_mode)
        if _display_mode == 'hdmi':
            _init_hdmi_output()
        else:
            _init_lcd_output()
        msx.set_backlight(_display_mode != 'hdmi')
    _display_held = display_toggle_down
    if display_toggle_down:
        return  # don't forward GUI/P to the MSX matrix while held

    # GUI+ESC: reinit the *currently active* display side in place —
    # same action as Display Settings' "Reinit ... now" row, without
    # opening any menu. Added for real-hardware "No Signal" recovery
    # during long display=hdmi sessions (see msx_send_hdmi_palette()'s
    # comment in msx_core.c for the suspected desync this resends) —
    # reaching it via the menu means importing two lazy modules first,
    # slower and heap-hungrier than this needs to be for what's meant to
    # be a quick recovery action. Edge-triggered.
    reinit_down = gui_down and HID_ESC in kc
    if reinit_down and not _reinit_held:
        # See GUI+P's identical wait_display() comment above.
        msx.wait_display()
        if _display_mode == 'hdmi':
            _init_hdmi_output()
        else:
            _init_lcd_output()
    _reinit_held = reinit_down
    if reinit_down:
        return  # don't forward GUI/ESC to the MSX matrix while held

    # GUI+F7: runtime emulator menu (cart swap, save/load, reset).
    # Edge-triggered so holding the combo doesn't reopen the menu.
    menu_down = gui_down and HID_F7 in kc
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
            display_state, _cart_path, _disk_path, _fdd_mode, _diskrom_path = show_emulator_menu(
                msx, usb_host, ROM_DIR, {_bios_name}, SAVE_BASE, CONFIG_PATH,
                init_hdmi_output=_init_hdmi_output,
                init_lcd_output=_init_lcd_output,
                display_state={'display': _display_mode,
                                'frame_skip': _hdmi_frame_skip,
                                'lcd': _lcd_model, 'rotate': _rotate_180,
                                'hdmi_baud': _hdmi_baud,
                                'hdmi_scale': _hdmi_scale},
                cart_path=_cart_path,
                fdd_mode=_fdd_mode, disk_path=_disk_path,
                diskrom_path=_diskrom_path)
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
            # Only meaningful if LCD was actually the active side when the
            # crash hit (display=hdmi never renders to it, so it can't
            # have been wedged by this).
            if _display_mode == 'lcd':
                try:
                    msx.init_display_hardware(
                        SPI_ID, SPI_BAUD, SPI_MOSI, SPI_SCK,
                        SPI_CS, SPI_DC, SPI_RST, SPI_BL,
                        _lcd_w, _lcd_h, _rotate_180)
                    print("Display re-init attempted after menu crash")
                except Exception as disp_e:
                    print(f"Display re-init also failed: {disp_e!r}")
            display_state = {'display': _display_mode,
                             'frame_skip': _hdmi_frame_skip,
                             'lcd': _lcd_model, 'rotate': _rotate_180,
                             'hdmi_baud': _hdmi_baud,
                             'hdmi_scale': _hdmi_scale}
        _display_mode    = display_state['display']
        _hdmi_frame_skip = display_state['frame_skip']
        _hdmi_scale      = display_state['hdmi_scale']  # already applied live by the menu
        # lcd/rotate/hdmi_baud have no live effect — kept only so Display
        # Settings shows the last-picked values if reopened this session.
        _lcd_model      = display_state['lcd']
        _rotate_180     = display_state['rotate']
        _hdmi_baud      = display_state['hdmi_baud']
        set_display_state(_display_mode)
        msx.set_backlight(_display_mode != 'hdmi')  # no-op if LCD wasn't initialized
        # Lightweight preventive re-assert of HDMI state on every menu
        # exit (display=hdmi only): re-applies the HDMI SPI settings and
        # re-sends the fixed 16-colour palette, nothing else — no reset
        # pulse, no clear_hdmi(), so it's invisible (~24 bytes blocking,
        # microseconds) with no black flash. Counters a receiver-side
        # palette/SPI-mode drift that the menu's raw332 draws +
        # hdmi_suspend()/lcd_suspend() cycling could plausibly leave
        # behind (the suspected cause of the long-session "goes black on
        # HDMI" reports). LCD deliberately not re-inited here — it's
        # reliable, and init_display_hardware()'s panel SWRESET *would*
        # flash. A full HDMI reinit (with the receiver reset pulse) is
        # still available on demand via GUI+ESC / Display Settings'
        # "Reinit ... now".
        if _display_mode == 'hdmi':
            msx.init_hdmi_output(HDMI_CS_PIN, _hdmi_baud)
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
    # Configure joystick GPIO as PULL_UP inputs. Safe to call even if the
    # port isn't physically connected — floating/pulled-up pins just read
    # as permanently released, which is the correct 'no joystick' state.
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
    # Read GPIO state into the PSG-visible joystick register (JOY1).
    # Pin.value() is already 1=released/0=pressed with PULL_UP wiring,
    # which matches the MSX joystick register's active-low bit convention
    # directly.
    if _joy_pins is None:
        return
    up, down, left, right, trig_a, trig_b = (p.value() for p in _joy_pins)
    state = (up | (down << 1) | (left << 2) | (right << 3) |
             (trig_a << 4) | (trig_b << 5) | 0xC0)
    msx.set_joystick(0, state)


# -----------------------------------------------------------------------
# Main
# -----------------------------------------------------------------------
def _init_msx_state():
    # 2026-09-21: split out of run() during real-hardware bring-up on the
    # RP2350-PiZero — see run()'s own comment for why.
    msx.init()


def run():
    # 2026-09-21: split into small phase functions during real-hardware
    # bring-up on the RP2350-PiZero — see msx_core.c's msx_init() history
    # and bldfrm_msx.sh's __micropy_extra_stack__ comment. The original
    # single ~445-line run() reproducibly corrupted UART output and hard-
    # panicked immediately after msx.init() on real hardware; splitting it
    # into small functions (each individually confirmed safe) fixed it.
    # The exact root cause inside MicroPython/the C call chain was never
    # fully identified (memory pressure, C stack size, and call depth were
    # all ruled out individually) — this is a confirmed-working structural
    # fix, not a fully explained one.
    _init_msx_state()
    # 2026-10-03 real-hardware finding: msx_menu.py's shared 4KB Mega ROM
    # scratch buffer (get_rom_load_buf()) used to allocate lazily, on
    # first actual use — which in practice means well into a session
    # (Swap Cartridge, after the ROM browser/menu have already run and
    # fragmented the GC heap), where even a single contiguous 4KB block
    # can fail to allocate despite tens of KB nominally free (non-
    # compacting GC — see get_rom_load_buf()'s own comment). Warming it up
    # here, as early as possible (right after msx.init(), before SD mount/
    # config/display/cart load have allocated and freed anything), means
    # this one-time 4KB allocation happens while the heap is as fresh/
    # unfragmented as it will ever be this session, instead of at the
    # least convenient possible moment. Cheap to hold for the rest of the
    # session even if no Mega ROM is ever loaded.
    # Best-effort only: right after main.py's own compile the heap can
    # already be too fragmented for one contiguous 4KB block (real
    # hardware, 2026-10-04: MemoryError here with ~24KB nominally free,
    # aborting boot). get_rom_load_buf() simply retries on first real use
    # and its callers already tolerate it failing, so never let the
    # warm-up itself be fatal.
    try:
        get_rom_load_buf()
    except MemoryError:
        print("get_rom_load_buf() warm-up failed (heap fragmented) — will retry on first use")
    has_sd, cfg = _mount_sd_and_load_config()
    _init_display(cfg)
    _init_hdmi_bridge(cfg)
    # 2026-09-29 (Phase 4, PIO-USB) real-hardware finding: on pizero,
    # _boost_clock_and_start_usb() used to run BEFORE _init_display() (the
    # original pico2-era order — USB kept skipping clk_sys entirely on
    # pizero until Phase 4, so the order never mattered before). Once USB
    # actually starts a recurring 1kHz hardware-alarm interrupt
    # (usb_host_core.c's pio_usb_sof_alarm_handler()), _init_display()'s
    # own set_sys_clock_khz(252000, ...) call (disp_dvi.c) running WHILE
    # that interrupt is already active reproducibly left the DVI output
    # black (BASIC screen never appeared) on real hardware, even though
    # each piece individually — DVI alone, or USB alone — was confirmed
    # working many times over. Moved USB init to AFTER display init so
    # onboard DVI's clock is fully stable (and nothing is yet touching
    # per-ms interrupts) before USB's alarm starts ticking; not yet
    # confirmed this specific reordering fixes it on real hardware.
    _boost_clock_and_start_usb()
    init_joystick()
    if not _load_bios(cfg, has_sd):
        return
    if not _load_cart_or_disk(cfg, has_sd):
        return
    _init_audio_and_ext(cfg)
    _main_loop()


def _boost_clock_and_start_usb():
    # 2026-09-21/22: msx.boost_peri_clock() (reconfigures clk_peri, which
    # the UART is clocked from, to track clk_sys) is SKIPPED on pizero
    # specifically — real-hardware bring-up found calling it corrupts
    # UART output (garbled bytes, sometimes a subsequent "Hard assert"
    # panic) extremely reliably, isolated down to that single C call via
    # extensive bisection. The exact mechanism was never confirmed
    # (looked consistent with the UART's baud divisor going stale
    # mid-clock-change, but rebuilding the UART object immediately after
    # did not reliably resolve it). Safe to skip on pizero: it's a
    # performance optimization only (raises SPI baud caps for the LCD/SD),
    # and pizero has no LCD. pico2 needs it (proven, unaffected by this
    # bug) — do NOT make this unconditional.
    #
    # 2026-09-27 (Phase 4, PIO-USB): init_usb() itself is NOT skipped on
    # pizero anymore — unlike boost_peri_clock(), it no longer touches
    # clk_sys/clk_peri at all on this board (see init_usb()'s own board
    # branch below) once PIO-USB keyboard support exists.
    if BOARD == "pizero":
        if _DIAG_ENABLE_USB:
            init_usb()
        else:
            print("USB host init SKIPPED (_DIAG_ENABLE_USB=False, EIO A/B test)")
        return

    msx.boost_peri_clock()
    try:
        _uart = machine.UART(0, baudrate=115200,
                              tx=machine.Pin(0), rx=machine.Pin(1), txbuf=32)
        uos.dupterm(_uart)
    except Exception as e:
        print(f"UART refresh after clk_peri change failed: {e}")

    # 2 — USB host
    init_usb()


def _mount_sd_and_load_config():
    # 3 — Mount SD card FIRST so SPI1 is configured by SD init before LCD.
    #     If LCD were initialized before SD, the SD driver would reconfigure
    #     SPI1 at 400 kHz and break subsequent DMA rendering.
    has_sd = mount_sd()

    # 4 — Read optional config file (before display init: it may select
    #     panel size — LCD_SIZES — and which side of the exclusive
    #     LCD/HDMI display= to bring up — see _display_mode below).
    cfg = load_config(CONFIG_PATH) if has_sd else {}
    return has_sd, cfg


def _init_display(cfg):
    lcd_model = cfg.get('lcd', DEFAULT_LCD_MODEL)
    # LCD_SIZES is {} / DEFAULT_LCD_MODEL is None on a pizero board (no LCD
    # hardware at all, see board_config.py) — guard against indexing an
    # empty dict with a None key; lcd_w/lcd_h are simply unused in that
    # case (display mode resolves to 'dvi' below, never 'lcd').
    if LCD_SIZES:
        lcd_w, lcd_h = LCD_SIZES.get(lcd_model, LCD_SIZES[DEFAULT_LCD_MODEL])
    else:
        lcd_w, lcd_h = 0, 0
    rotate_180 = cfg.get('rotate', '0').strip() == '180'

    # Parsed early (before LCD init) so it can decide whether that runs.
    # LCD/HDMI/DVI are mutually exclusive outputs (see HDMI_CS_PIN's
    # comment) — display=lcd, display=hdmi, or display=dvi only, no
    # combined mode. 2026-09-20/22 (phase 3 of the RP2350-PiZero port): a
    # pizero board has no LCD/HDMI-bridge hardware at all (board_config.py
    # sets those pin constants to None) — DVI is its only possible output,
    # so it's forced here UNCONDITIONALLY, ignoring msx.ini's display=
    # entirely. This is deliberate, not just a default: an msx.ini written
    # for a pico2 setup (e.g. display=hdmi for the HDMI-bridge feature)
    # would otherwise be silently accepted and crash trying to touch
    # nonexistent HDMI-bridge/LCD pins (all None) — real-hardware finding,
    # 2026-09-22. pico2 keeps the normal msx.ini-driven lcd/hdmi choice.
    global _display_mode
    if DISPLAY_TYPE == 'DVI':
        _display_mode = 'dvi'
    else:
        _display_mode = cfg.get('display', 'lcd').strip().lower()
        if _display_mode not in ('lcd', 'hdmi'):
            _display_mode = 'lcd'

    # 5 — Initialize display AFTER SD: SPI1 is now stable at SPI_BAUD.
    #     Skipped entirely when display=hdmi/dvi (msx_core.c's
    #     hdmi_apply_spi_settings() doesn't depend on this having run, and
    #     a pizero board has no SPI LCD pins to init in the first place) —
    #     _lcd_w/_lcd_h/_rotate_180/_lcd_model are still recorded either
    #     way so a later live switch to 'lcd' from the Display Settings menu
    #     (_init_lcd_output()) has the right panel parameters on hand (n/a
    #     on pizero — that menu path is LCD-only, see poll_keyboard()).
    global _lcd_w, _lcd_h, _rotate_180, _lcd_model
    _lcd_w, _lcd_h, _rotate_180, _lcd_model = lcd_w, lcd_h, rotate_180, lcd_model
    if _display_mode == 'hdmi':
        print("Display: LCD init skipped (display=hdmi, exclusive)")
    elif _display_mode == 'dvi':
        # NOT YET REAL-HARDWARE VERIFIED — see src/msx/display/disp_dvi.c's
        # header comment. getattr() guards against running this file on
        # older/pico2 firmware that predates init_display_hardware_dvi().
        print("Initializing onboard DVI…")
        _init_dvi = getattr(msx, "init_display_hardware_dvi", None)
        if _init_dvi is not None:
            if not _init_dvi():
                print("Display: init_display_hardware_dvi() failed "
                      "(out of C heap for the ~150KB framebuffer, or "
                      "PIO/DVI init failed) — no display output.")
        else:
            print("Display: msx.init_display_hardware_dvi() not present in "
                  "this firmware (built for pico2?) — no display output.")
    else:
        print(f"Initializing display… ({lcd_model} {lcd_w}x{lcd_h}"
              f"{', rotated 180' if rotate_180 else ''})")
        msx.init_display_hardware(
            SPI_ID, SPI_BAUD,
            SPI_MOSI, SPI_SCK,
            SPI_CS, SPI_DC, SPI_RST, SPI_BL,
            lcd_w, lcd_h, rotate_180
        )
        msx.set_backlight(True)


def _init_hdmi_bridge(cfg):
    # 5.1 — Optional HDMI bridge output (hdmi_bridge/README.md). Must come
    #       after init_display_hardware() (reuses its SPI1 instance).
    #       Only initialized when display=hdmi — users without the second
    #       Pico2+PICO-HDMI-PLUS just leave display unset/'lcd' and are
    #       completely unaffected. 'display'/'hdmi_frame_skip' can also be
    #       changed live afterward via the GUI+F7 "Display Settings" menu
    #       (see poll_keyboard()); the globals set here are just the
    #       msx.ini-driven starting point.
    #
    #       Initialized here — BEFORE BIOS/cart loading — so a display=hdmi
    #       setup shows the boot-time interactive ROM selector (step 7
    #       below) and any boot error screen on HDMI too, not just the LCD.
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
    # display already parsed above (step 4).
    global _hdmi_frame_skip, _hdmi_baud, _hdmi_scale

    try:
        _hdmi_frame_skip = max(1, int(cfg.get('hdmi_frame_skip', HDMI_FRAME_SKIP)))
    except (ValueError, TypeError):
        _hdmi_frame_skip = HDMI_FRAME_SKIP

    try:
        _hdmi_baud = max(1, int(cfg.get('hdmi_baud', HDMI_BAUD)))
    except (ValueError, TypeError):
        _hdmi_baud = HDMI_BAUD

    try:
        _hdmi_scale = max(1, min(4, int(cfg.get('hdmi_scale', HDMI_SCALE))))
    except (ValueError, TypeError):
        _hdmi_scale = HDMI_SCALE
    # Applied regardless of display mode (no SPI traffic — just the header
    # value), so a later live switch to 'hdmi' already uses it.
    if _hdmi_scale is not None:
        msx.set_hdmi_scale(_hdmi_scale)

    if _display_mode == 'hdmi':
        print(f"HDMI bridge output enabled (CS=GP{HDMI_CS_PIN}, "
              f"{_hdmi_baud/1e6:.1f}MHz, frame_skip={_hdmi_frame_skip}, "
              f"scale={_hdmi_scale})")
        # Hardware-reset the receiver before sending anything — see
        # HDMI_RESET_PIN's comment above. Must come before
        # init_hdmi_output() (which starts sending immediately).
        msx.hdmi_reset_init(HDMI_RESET_PIN)
        msx.hdmi_reset_pulse()
        time.sleep_ms(HDMI_RESET_GRACE_MS)
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
    set_display_state(_display_mode)


def _load_bios(cfg, has_sd):
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
        return False

    log_mem("after BIOS load")
    return True


def _load_cart_or_disk(cfg, has_sd):
    # 7 — Load cartridge, OR (mode=disk) a Disk ROM into the same slot —
    # mutually exclusive, see msx_fdd.py / the msx.ini comment above.
    # load_cart_smart() picks in-RAM vs SD-backed paged loading (Mega ROM,
    # >32KB) automatically — see msx_menu.py. HDMI is already active at
    # this point (step 5.1 above), so the interactive selector below shows
    # there too, not just on the LCD.
    global _cart_path, _fdd_mode, _disk_path, _diskrom_path
    _fdd_mode = cfg.get('mode', '').strip().lower() == 'disk'
    # Remembered even in cart mode (not just under mode=disk) so the
    # runtime menu's "Switch to Disk Mode" can reuse it without asking
    # again — see msx_runtime_menu.py.
    _diskrom_path = cfg.get('diskrom')

    if _fdd_mode:
        if _diskrom_path is None:
            print("ERROR: mode=disk but no diskrom= in msx.ini")
            _show_error("diskrom= not set", "required when mode=disk")
            return False
        if has_sd:
            try:
                ok = load_cart_smart(msx, 0, _diskrom_path)
            except Exception as e:
                # 2026-09-30: was a bare `except OSError: ok = False` — the
                # actual error (e.g. an intermittent SD read failure) was
                # silently discarded, unlike load_bios_file()'s equivalent
                # path. Surface it so a real-hardware "FAILED" is
                # diagnosable instead of a dead end. 2026-10-05: widened
                # from OSError to Exception — load_cart_smart() can also
                # raise plain RuntimeError (cart_alloc()/short-read
                # failures, and now the pizero Mega ROM refusal below),
                # none of which are OSError subclasses; those were an
                # existing uncaught-crash gap here even before today.
                ok = False
                print(f"Disk ROM load error: {e}")
            print(f"Disk ROM (config): {_diskrom_path}  {'OK' if ok else 'FAILED'}")
            if not ok:
                print(f"ERROR: Disk ROM not found/failed to load: {_diskrom_path}")
                _show_error("Disk ROM not found", _diskrom_path)
                return False
        else:
            print("ERROR: mode=disk but no SD card")
            _show_error("No SD card", "mode=disk needs an SD card")
            return False

        if 'disk' in cfg:
            try:
                uos.stat(cfg['disk'])
                _disk_path = cfg['disk']
            except OSError:
                print(f"WARNING: configured disk= not found: {cfg['disk']}")
        if _disk_path is None and has_sd:
            # Falls through here both when 'disk' was omitted and when the
            # configured path didn't exist (see the OSError case above) —
            # same "let the user pick instead of crashing" recovery already
            # used for a missing 'cart' file (see load_cart_smart() above).
            _disk_path = select_rom(
                msx, ROM_DIR,
                title="Select Disk Image",
                usb_host_mod=usb_host if _usb_ready else None,
                exclude_names={_bios_name},
                ext='.dsk',
            )
        if _disk_path:
            print(f"Disk image: {_disk_path}")
        else:
            print("No disk image — MSX-DOS will report drive A: not ready")

    elif 'cart' in cfg:
        # Explicit path from msx.ini
        if has_sd:
            try:
                ok = load_cart_smart(msx, 0, cfg['cart'])
            except Exception as e:
                # 2026-09-30: see the Disk ROM branch above's identical
                # comment — this swallowed the real OSError the same way.
                # 2026-10-05: widened to Exception — see that branch's
                # updated comment (RuntimeError, incl. the pizero Mega ROM
                # refusal, is not an OSError and was falling through
                # uncaught here).
                ok = False
                print(f"Cart load error: {e}")
            if ok:
                _cart_path = cfg['cart']  # own rotating save history — see save_base_for_cart()
            print(f"Cart (config): {cfg['cart']}  {'OK' if ok else 'FAILED'}")
        else:
            print(f"WARNING: configured cart not found: {cfg['cart']}")

    elif has_sd:
        # Interactive selector — no keyboard, or no response within the
        # timeout, means "no selection" (boots MSX BASIC), not an
        # auto-pick (2026-09-13; see msx_rom_browser.select()'s comments).
        # (exclude the BIOS file so it isn't offered as a cartridge)
        selected = select_rom(
            msx, ROM_DIR,
            title="Select Cartridge ROM",
            usb_host_mod=usb_host if _usb_ready else None,
            exclude_names={_bios_name},
        )
        if selected:
            # 2026-10-05: previously no try/except at all here — any
            # load_cart_smart() exception (short read, cart_alloc()
            # failure, or now the pizero Mega ROM refusal) crashed the
            # whole boot uncaught. Matches the msx.ini 'cart=' branch
            # above.
            try:
                ok = load_cart_smart(msx, 0, selected)
            except Exception as e:
                ok = False
                print(f"Cart load error: {e}")
            if ok:
                _cart_path = selected
            print(f"Cart (menu): {selected}  {'OK' if ok else 'FAILED'}")
        else:
            print("No cartridge — booting MSX BASIC")

    log_mem("after cart load")
    # 2026-10-04 DIAGNOSTIC: baseline C heap (used, free) right after the
    # first cart load — compare against msx_mode_switch.py's identical
    # print right before a later Swap Cartridge attempt, to see how much
    # the C heap moves (or doesn't) between the two.
    if BOARD == "pizero":
        try:
            print(f"C HEAP: {msx.c_heap_info()}")
        except Exception:
            pass
    return True


def _init_audio_and_ext(cfg):
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
    # 'ext=off' skips this — see the msx.ini comment above for why that
    # recovers performance for cart-mode sessions that don't use any
    # extension at all.
    if cfg.get('ext', '').strip().lower() == 'off':
        print("EXT: disabled via msx.ini (ext=off)")
    else:
        load_extensions(msx)

    # 8.6 — Virtual FDD (mode=disk only): WD179x FDC register emulation
    # (src/msx/wd179x.c), memory-mapped into cart slot 1's page. Mounted
    # before reset() so msx.reset()'s msx_fdc_reset() (clears transient
    # FDC registers, preserves the enabled/base_addr/geometry config —
    # see wd179x.h) has something to preserve.
    #
    # Always call mount() here, even with no disk image (_disk_path is
    # None) — msx_fdd.mount(None, ...) still enables the FDC itself (see
    # its own comment for why a *disabled* FDC breaks Disk BASIC entirely
    # rather than just leaving drive A: empty).
    if _fdd_mode:
        import msx_fdd
        msx_fdd.register(msx)
        try:
            fdc_base = int(cfg.get('fdc_base', str(msx_fdd.DEFAULT_FDC_BASE)), 0)
        except ValueError as e:
            print(f"Bad fdc_base in msx.ini: {e} — using default")
            fdc_base = msx_fdd.DEFAULT_FDC_BASE
        msx_fdd.mount(_disk_path, fdc_base)
        log_mem("after msx_fdd import")


def _main_loop():
    # 9 — Reset and start
    msx.reset()
    print("MSX started")

    frame = 0
    t0    = time.ticks_ms()

    # 2026-09-05: MSX_TCYCLES_FRAME (msx_core.h) advances exactly 1/60s of
    # MSX-clock time per msx.run_frame() call, regardless of how fast the
    # host actually executes it — that's what keeps game logic speed and
    # PSG audio pitch correct. The pipelined loop below was originally
    # always slower than 60fps on real hardware (~44fps LCD-only), so this
    # never mattered; the HDMI DMA optimization now regularly exceeds
    # 70fps on non-Mega-ROM carts, meaning MSX-time is advancing faster
    # than real time — the game plays fast-forward and audio pitches up.
    # Pace each iteration to a 60fps wall-clock budget by sleeping off
    # whatever's left over when a frame finished early; iterations that
    # are already at/below 60fps (slower carts, etc.) are completely
    # unaffected since there's nothing left to sleep off.
    FRAME_BUDGET_US = 1_000_000 // 60

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
        iter_start = time.ticks_us()

        # Reads the live global every iteration (not a cached boolean) so
        # a change made via the GUI+F7 "Display Settings" menu takes effect
        # on the very next frame, no restart needed. Mutually exclusive —
        # see _display_mode's comment — so exactly one of these is true.
        use_hdmi = _display_mode == 'hdmi'
        use_lcd  = _display_mode == 'lcd'
        use_dvi  = _display_mode == 'dvi'

        # Start DMA transfer of the just-completed frame (non-blocking).
        # 1:1 native 256x192 (no 1.5x scaling) — scaling nearly doubled
        # transfer time and made gameplay feel like slow motion.
        # Skipped entirely when display=hdmi — recovers the LCD's DMA+wait
        # cost for users who only care about the HDMI output.
        if use_lcd:
            msx.render_to_display_1to1()
        elif use_dvi:
            # Synchronous, not DMA — core1 reads the DVI framebuffer
            # continuously on its own; there is nothing to wait for here
            # (see msx_render_to_display_dvi()'s comment). NOT YET REAL-
            # HARDWARE VERIFIED.
            msx.render_to_display_dvi()

        # Compute the NEXT frame while the DMA above is still in flight —
        # msx_run_frame() writes into the other framebuf, so this is safe.
        msx.run_frame()

        # Poll USB keyboard and joystick while DMA runs
        poll_keyboard()
        poll_joystick()

        if _exit_requested:
            # Ctrl+Alt+Delete — see poll_keyboard()'s comment. Drain
            # whatever DMA transfer render_to_display_1to1() just started
            # above before we stop touching the display peripheral
            # entirely (same reasoning as every other mid-loop early-out
            # in this file, e.g. GUI+F7's wait_display() call below).
            if use_lcd:
                msx.wait_display()
            break

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

        # Sleep off whatever's left of this frame's 60fps budget — see the
        # FRAME_BUDGET_US comment above. A no-op once elapsed already
        # exceeds the budget (slower carts, etc.).
        elapsed_us = time.ticks_diff(time.ticks_us(), iter_start)
        if elapsed_us < FRAME_BUDGET_US:
            time.sleep_us(FRAME_BUDGET_US - elapsed_us)

        if frame % 300 == 0:
            elapsed = time.ticks_diff(time.ticks_ms(), t0)
            fps     = 300_000 / elapsed if elapsed > 0 else 0.0
            ring    = msx.get_audio_ring_level()
            print(f"FPS: {fps:.1f}  ring: {ring}")
            # 2026-10-05 DIAGNOSTIC (re-added): (heartbeat, late_scanline_ctr)
            # — see disp_dvi.c's msx_dvi_debug_info()/msx.dvi_debug() and
            # the dvi.c was_backlogged fix. Checking whether the fix
            # actually lets late_scanline_ctr recover, or whether "No
            # Signal" persists via some other mechanism entirely.
            if BOARD == "pizero":
                try:
                    print(f"DVI DIAG: {msx.dvi_debug()}")
                except Exception:
                    pass
            t0 = time.ticks_ms()

    # Ctrl+Alt+Delete landed here (the only way out of the loop above).
    # Stop the USB host's background timer so its ISR doesn't keep firing
    # into a script that's about to finish — best-effort, not required for
    # mpremote/the REPL to work again (that's the native USB CDC serial, a
    # separate peripheral from usb_host's PIO-USB pins — see
    # poll_keyboard()'s comment). Returning from run() here (main.py's only
    # top-level statement is `if __name__=='__main__': run()`) ends the
    # script and hands control back to the REPL, exactly like a normal
    # script finishing on its own.
    if usb_host is not None and hasattr(usb_host, 'stop_bg_timer'):
        try:
            usb_host.stop_bg_timer()
        except Exception as e:
            print(f"stop_bg_timer failed (ignoring): {e}")
    print("Ctrl+Alt+Delete: main.py exiting — REPL/mpremote should be usable now.")


if __name__ == '__main__':
    run()
