"""
test_msx_debug.py — MSX emulator step-by-step diagnostic
Each step prints timestamp + result so you can see exactly where it hangs.

Run from Thonny / mpremote:
    import test_msx_debug
"""
import machine, time, uos, sys, framebuf

# ── Pin assignments (same as main.py) ──────────────────────────────────
SPI_ID   = 1;  SPI_MOSI = 11; SPI_SCK  = 10; SPI_CS  = 9
SPI_DC   = 8;  SPI_RST  = 7;  SPI_BL   = 22; SPI_BAUD = 40_000_000
SD_SPI_ID = 1; SD_MOSI  = 11; SD_SCK   = 10; SD_MISO = 12
SD_CS    = 15; SD_INIT_BAUD = 400_000; AUDIO_PIN = 14

# ── Helpers ─────────────────────────────────────────────────────────────────
def _t():
    return f"[{time.ticks_ms():7d}ms]"

def step(label):
    print(f"{_t()} {label} ...", end="")

def ok(note=""):
    print(f"  OK{(' ' + note) if note else ''}")

def fail(e):
    print(f"  FAIL: {e}")
    sys.exit(1)

# ── 1. msx C module ─────────────────────────────────────────────────────────
step("import msx")
try:
    import msx
    ok()
except ImportError as e:
    fail(e)

# ── 2. Mount SD (skip if already mounted) ───────────────────────────────────
step("SD mount")
try:
    uos.stat('/sd')
    ok("already mounted")
except OSError:
    try:
        import sdcard
        spi = machine.SPI(SD_SPI_ID, baudrate=SD_INIT_BAUD,
                          sck=machine.Pin(SD_SCK), mosi=machine.Pin(SD_MOSI),
                          miso=machine.Pin(SD_MISO))
        cs  = machine.Pin(SD_CS, machine.Pin.OUT, value=1)
        sd  = sdcard.SDCard(spi, cs,
                            baudrate=SD_INIT_BAUD, restore_baudrate=SPI_BAUD)
        uos.mount(sd, '/sd')
        ok()
    except Exception as e:
        fail(e)

# ── 3. msx.init ─────────────────────────────────────────────────────────────
step("msx.init()")
msx.init()
ok()

# ── 4. Display hardware init ─────────────────────────────────────────────────
step("msx.init_display_hardware()")
t0 = time.ticks_ms()
msx.init_display_hardware(SPI_ID, SPI_BAUD,
                           SPI_MOSI, SPI_SCK,
                           SPI_CS, SPI_DC, SPI_RST, SPI_BL)
ok(f"{time.ticks_diff(time.ticks_ms(), t0)} ms")

# ── 5. LCD color test ────────────────────────────────────────────────────────
# RGB565 big-endian (byte-swapped) colors for our DMA pipeline:
#   red  = 0xF800 native → stored as 0x00F8 → bytes [0xF8,0x00] → display=red
#   blue = 0x001F native → stored as 0x1F00 → bytes [0x00,0x1F] → display=blue
print(f"{_t()} LCD color test: filling framebuf ...")
buf = msx.get_framebuf()
fb  = framebuf.FrameBuffer(buf, 256, 192, framebuf.RGB565)
fb.fill(0x00F8)                         # red  (top half)
fb.fill_rect(0, 96, 256, 96, 0x1F00)   # blue (bottom half)
fb.text("LCD OK?", 88, 88, 0xFFFF)      # white text at center

step("render_to_display()")
t0 = time.ticks_ms()
msx.render_to_display()
ok(f"returned in {time.ticks_diff(time.ticks_ms(), t0)} ms")

step("wait_display()")
t0 = time.ticks_ms()
msx.wait_display()
ok(f"{time.ticks_diff(time.ticks_ms(), t0)} ms  <<< LCD should show RED/BLUE split + text")

print("      >>> Waiting 3 s — check LCD now ...")
time.sleep_ms(3000)

# ── 6. Load BIOS ─────────────────────────────────────────────────────────────
step("open MSX.ROM")
try:
    with open('/sd/msx/MSX.ROM', 'rb') as f:
        bios = f.read()
    ok(f"{len(bios)} bytes")
except Exception as e:
    fail(e)

step("msx.load_bios()")
if not msx.load_bios(bios):
    fail("returned False (size must be 32768 bytes)")
del bios
ok()

# ── 7. Audio PWM ─────────────────────────────────────────────────────────────
step(f"setup_audio_pwm(GP{AUDIO_PIN})")
result = msx.setup_audio_pwm(AUDIO_PIN)
ok(f"returned {result}")

# ── 8. Reset ─────────────────────────────────────────────────────────────────
step("msx.reset()")
msx.reset()
ok()

# ── 9. Frame timing ──────────────────────────────────────────────────────────
print(f"\n{_t()} Running 5 frames with per-call timing ...")
for i in range(5):
    t0 = time.ticks_ms()
    msx.run_frame()
    t1 = time.ticks_ms()

    msx.render_to_display()
    t2 = time.ticks_ms()

    msx.wait_display()
    t3 = time.ticks_ms()

    rf = time.ticks_diff(t1, t0)
    rd = time.ticks_diff(t2, t1)
    wd = time.ticks_diff(t3, t2)
    print(f"  frame[{i}] run={rf}ms  render={rd}ms  wait={wd}ms  total={rf+rd+wd}ms")

print(f"\n{_t()} === Test complete ===")
print("      LCD should now show the MSX BIOS boot screen.")
print("      If LCD is still white/blank, the issue is hardware-level RST wiring.")
