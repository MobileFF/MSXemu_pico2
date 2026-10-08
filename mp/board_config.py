# board_config.py — per-board pin/clock definitions for the MSX emulator.
#
# 2026-09-20, phase 1 of the RP2350-PiZero port (see
# 調査用/RP2350-PiZero_HDMI出力適用調査.md, §7): extracted verbatim out of
# main.py's former "Pin / peripheral constants" block so main.py/msx_menu.py/
# msx_keymap.py/msx_fdd.py can stay 100% hardware-agnostic and shared between
# targets — only this one file differs per board. This phase is a pure
# refactor: the "pico2" branch below is byte-for-byte the same pin/baud
# values main.py hardcoded before, so a pico2 build's behavior must not
# change at all. The "pizero" branch is best-effort from the feasibility
# report above (§2.2's pin table, §5's clock note) — not yet real-hardware
# verified, since msx.get_board_type() (the thing that would actually select
# it) doesn't exist until phase 3's C-side board-type API lands. Until then
# board resolves to "pico2" unconditionally via the getattr() fallback below,
# so importing this module has zero effect on the current firmware.
#
# Usage: `from board_config import *` — every name below becomes available
# as a plain module-level constant in the importer, matching how main.py
# used to define them directly.

import msx

BOARD = getattr(msx, "get_board_type", lambda: "pico2")()

if BOARD == "pizero":
    # Waveshare RP2350-PiZero (RP2350B, 48 GPIO) — onboard DVI + onboard
    # microSD, no LCD, no external HDMI bridge. See the feasibility report's
    # §2.2 pin table. NOT YET REAL-HARDWARE VERIFIED (phase 1/2 land the
    # plumbing; phase 3 is the actual PicoDVI bring-up — see
    # src/msx/display/disp_dvi.c).
    SYS_CLOCK_HZ = 252_000_000  # matches the DVI 640x480@60 TMDS bit clock
                                 # (25.2MHz pixel clock x10) exactly, and is
                                 # also a clean multiple of USB full-speed's
                                 # 12MHz base rate (see the report's §3④) —
                                 # not yet wired into boot.py's early
                                 # machine.freq() call (that still always
                                 # sets 250MHz regardless of board; doing
                                 # this properly needs boot.py to be able to
                                 # tell the boards apart before msx/board_
                                 # config are even importable — left for
                                 # when disp_dvi.c actually needs it).

    DISPLAY_TYPE = "DVI"   # onboard DVI only — no display=lcd/hdmi choice

    # No SPI LCD panel on this board at all — LCD-only constants below are
    # meaningless here; left unset (None) rather than pointing at unused
    # pins, so any code path that still tries to use them fails loudly
    # instead of silently touching the wrong GPIO.
    SPI_ID   = None
    SPI_MOSI = None
    SPI_SCK  = None
    SPI_CS   = None
    SPI_DC   = None
    SPI_RST  = None
    SPI_BL   = None
    SPI_BAUD = None
    LCD_SIZES = {}
    DEFAULT_LCD_MODEL = None

    # No external HDMI bridge (onboard DVI replaces it entirely).
    HDMI_CS_PIN = None
    HDMI_RESET_PIN = None
    HDMI_RESET_GRACE_MS = None
    HDMI_BAUD = None
    HDMI_FRAME_SKIP = None
    HDMI_SCALE = None

    # Onboard TF (microSD) card slot — independent SPI-capable pins, not
    # shared with anything else (see report §2.2/§3①). 2026-09-20: these
    # 4 pin numbers are now independently confirmed against the vendor
    # schematic by a working reference port (see
    # src/msx/libdvi/PROVENANCE.md) — this phase-1 guess turned out to be
    # exactly right. SD_CARD_DETECT_PIN is new information from that same
    # source, not wired up anywhere yet.
    SD_SPI_ID    = 1
    SD_MOSI_PIN  = 31
    SD_SCK_PIN   = 30
    SD_MISO_PIN  = 40
    SD_CS_PIN    = 43
    SD_CARD_DETECT_PIN = 22
    SD_INIT_BAUD = 400_000
    # 2026-09-30 EIO investigation: reproducible OSError([Errno 5] EIO)
    # loading the SECOND SD file read in a boot session (any file, not a
    # specific one) was root-caused to sdcard.py's CMD12 (stop
    # transmission) not waiting for the card's busy signal to clear before
    # releasing CS — see sdcard.py's _readblocks_once() comment. Not a
    # baud/timing-margin issue after all (confirmed: neither the PIO-USB
    # interrupt (_DIAG_ENABLE_USB=False) nor dropping this to 1MHz changed
    # the failure rate). Restored to the original 4MHz.
    SD_DATA_BAUD = 4_000_000

    # 2026-09-20 IMPORTANT CAVEAT found during phase 3 research (not
    # covered by the original investigation doc, which only looked at
    # video output): this board's USB-host port is NOT wired to the
    # RP2350's native/hardware USB host controller the way Pico 2's is
    # (see src/msx/usb_host_core.c's hcd_rp2040.c-based stack, which this
    # board's host port cannot use as-is) — the schematic wires it as a
    # dedicated PIO-USB Type-C port instead (D+/D- = GPIO28/29, D- always
    # D+ +1), because RP2350 only has one native USB controller and the
    # vendor spent it on the *other* Type-C port (power/programming/
    # BOOTSEL). Confirmed independently by the same reference port (see
    # src/msx/libdvi/PROVENANCE.md) actually using PIO-USB (Sekigon/
    # Pico-PIO-USB) for its keyboard, not TinyUSB's native host driver.
    # USB keyboard input will NOT work on this board until that separate
    # library is integrated — out of scope for this DVI-focused pass;
    # left as pin facts only (USB_HOST_DP_PIN) for whoever picks that up.
    USB_HOST_DP_PIN = 28  # D-, if needed, is always this + 1 (GPIO29)

    AUDIO_PIN = 14  # 40-pin header (Pi Zero-compatible layout), same as pico2

    # Joystick — same convention as pico2 (PULL_UP, active-low), pins are
    # arbitrary picks from the free 40-pin header (GP0-27) since none of
    # them are claimed by onboard peripherals on this board (report §3①).
    JOY_UP_PIN     = 18
    JOY_DOWN_PIN   = 19
    JOY_LEFT_PIN   = 20
    JOY_RIGHT_PIN  = 21
    JOY_TRIG_A_PIN = 26
    JOY_TRIG_B_PIN = 27

else:
    # Raspberry Pi Pico 2 (RP2350A) — current/default hardware. Unchanged
    # from main.py's original hardcoded values.
    SYS_CLOCK_HZ = 250_000_000  # set in boot.py, not by this module (see
                                 # the "pizero" branch's SYS_CLOCK_HZ comment)

    DISPLAY_TYPE = "LCD"  # lcd/hdmi chosen live via msx.ini's display= key

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
    # LCD and HDMI are mutually exclusive outputs — msx.ini: display=lcd
    # (default) or display=hdmi. No separate on/off flag: HDMI hardware is
    # only ever initialized when display=hdmi (at boot, or live from the
    # Display Settings menu — see main.py's _init_hdmi_output()/poll_keyboard()).
    # 2026-09-06: simultaneous LCD+HDMI ('both') used to also be selectable
    # but was found unreliable on real hardware (switching SPI mode every
    # frame between the two eventually corrupts/loses the HDMI signal and
    # glitches the LCD — see msx_core.c's hdmi_apply_spi_settings() comment
    # and doc/hdmi_bridge_phase2_report.md) and was removed rather than kept
    # around as a known-broken option.
    HDMI_CS_PIN = 28
    # 2026-09-05: HDMI receiver hardware-reset line (see
    # hdmi_bridge_receiver's notes/sender_reset_line.md) — a spare GPIO wired
    # directly to the receiver Pico 2's RUN pin, pulsed low briefly before
    # init_hdmi_output() to force a real hardware reset. Fixes a real-hardware
    # issue where the receiver can come up in a bad state if it's powered on
    # (or hot-plugged) while the HDMI cable is already connected (suspected
    # backfeed through the TMDS lines' series resistors). Completely free
    # GPIO, not shared with anything else.
    HDMI_RESET_PIN = 13
    HDMI_RESET_GRACE_MS = 100  # let the receiver finish booting before we start sending
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
    HDMI_SCALE = 1            # receiver-side upscale of the 256x192 frame
                              # (msx.ini: hdmi_scale=); 2 = 512x384, the
                              # largest that fits the receiver's 640x480.

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
