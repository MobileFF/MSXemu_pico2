/*
 * modmsx.c — MicroPython C module for MSX1 emulator
 *
 * Exposes the `msx` module to MicroPython.
 *
 * Usage from Python:
 *   import msx
 *   msx.init()
 *   msx.load_bios(bytes_obj)
 *   msx.load_cart(0, bytes_obj)       # slot 0 = cartridge slot 1
 *   msx.reset()
 *   while True:
 *       n_samples = msx.run_frame()
 *       msx.render_to_display()
 *       fb = msx.get_framebuf()       # memoryview, RGB565 big-endian
 *       audio = msx.get_audio_buf(n_samples)
 */

#include "mpconfigport.h"
#include "py/runtime.h"
#include "py/obj.h"
#include "py/objstr.h"
#include "py/mphal.h"
#include "py/stream.h"

#include "msx_core.h"

#ifdef __arm__
#include "hardware/spi.h"
#include "hardware/gpio.h"
#include "hardware/pwm.h"
#include "hardware/clocks.h"
#include "pico/time.h"
#define HAVE_PICO_SDK 1
#else
#define HAVE_PICO_SDK 0
#endif

/* -----------------------------------------------------------------------
 * Single global MSX state (one MSX per Pico)
 * ----------------------------------------------------------------------- */
static msx_state_t msx_state;
static bool msx_global_initialized = false;

/* Save-state header scratch buffer — MSX_SAVE_HDR_SZ (64) bytes only.
 * The ~64KB RAM portion is NOT duplicated here: msx.get_ram_view() hands
 * out a zero-copy view of msx_state.ram directly (that memory already
 * exists as part of the live emulator state). A one-shot ~64KB
 * *contiguous* allocation from MicroPython's non-compacting GC heap
 * reliably fails with MemoryError once the heap has any scattered live
 * objects in it (measured: failed with 100KB+ nominally free after normal
 * gameplay/menu use) — this design avoids ever needing one. */
static uint8_t g_state_hdr_buf[MSX_SAVE_HDR_SZ];

/* -----------------------------------------------------------------------
 * Mega ROM (SD-backed paged cart) support
 *
 * msx.load_cart_paged() keeps an open Python file object per cart slot
 * so msx_core.c's cart_fetch_cb (cart_fetch_from_pyfile below) can seek +
 * read a fresh 8KB page whenever a bank-switch write needs one not
 * already cached. These are declared as MicroPython GC root pointers —
 * without that, the GC would have no other reference to the file object
 * and could collect it out from under this module's raw mp_obj_t copy. */
MP_REGISTER_ROOT_POINTER(mp_obj_t msx_cart_file0);
MP_REGISTER_ROOT_POINTER(mp_obj_t msx_cart_file1);

static mp_obj_t *cart_file_slot(uint8_t slot) {
    return (slot == 0) ? &MP_STATE_VM(msx_cart_file0) : &MP_STATE_VM(msx_cart_file1);
}

static bool cart_fetch_from_pyfile(void *userdata, uint8_t slot,
                                     uint32_t byte_offset, uint8_t *dest) {
    (void)userdata;
    mp_obj_t file_obj = *cart_file_slot(slot);
    if (file_obj == MP_OBJ_NULL || file_obj == mp_const_none) return false;

    int errcode = 0;
    mp_off_t pos = mp_stream_seek(file_obj, (mp_off_t)byte_offset, MP_SEEK_SET, &errcode);
    if (errcode != 0 || pos < 0) return false;

    mp_uint_t n = mp_stream_read_exactly(file_obj, dest, MSX_CART_PAGE_SIZE, &errcode);
    return (errcode == 0 && n == MSX_CART_PAGE_SIZE);
}

/* PWM audio state */
static uint8_t  audio_pwm_pin   = 0xFF;
static uint8_t  audio_pwm_slice = 0;
static bool     audio_pwm_ready = false;

/* Runtime volume: val = (sample * audio_volume) >> 12. audio_volume=256
 * reproduces the original "sample >> 4" behavior (max ~765/1023, 75% of
 * the PWM range) — adjustable via msx.set_audio_volume() without a
 * firmware rebuild, e.g. to tame passive-piezo-buzzer overdrive/crackle
 * at high amplitude. */
static uint16_t audio_volume = 256;

/* Runtime one-pole IIR low-pass on the raw PSG sample stream, applied
 * BEFORE the volume scale above: state += (sample - state) >> audio_lpf_shift.
 * shift=0 disables it (state tracks sample exactly every tick — identical
 * to the unfiltered original behavior). Higher shift = heavier smoothing
 * (lower cutoff): approx cutoff_hz = (MSX_AUDIO_RATE >> shift) / (2*pi).
 * This can't remove the ~234kHz PWM carrier itself (that's a hardware
 * reconstruction-filter problem — see doc discussion), but it does smooth
 * the actual audio waveform's sample-to-sample jumps, which reduces
 * audible harshness/"stair-stepping" from the 22050 Hz sample-and-hold
 * output — adjustable live via msx.set_audio_filter() without a rebuild. */
static uint8_t audio_lpf_shift = 0;
static int32_t audio_lpf_state = 0;

/* -----------------------------------------------------------------------
 * Audio ring buffer — lock-free single-producer (main) / single-consumer (ISR)
 * Size must be a power of 2 and at least 2× (MSX_AUDIO_RATE / MSX_FPS).
 * ----------------------------------------------------------------------- */
#define AUDIO_RING_SIZE 2048u   /* ~5.6 frames of margin at 22050 Hz, 60 fps */
#define AUDIO_RING_MASK (AUDIO_RING_SIZE - 1u)

static volatile int16_t audio_ring_buf[AUDIO_RING_SIZE];
static volatile uint32_t audio_ring_write = 0; /* written by main loop only */
static volatile uint32_t audio_ring_read  = 0; /* written by ISR only */

#if HAVE_PICO_SDK
static repeating_timer_t audio_rep_timer;
static bool audio_timer_running = false;

/* ISR: fires at 22050 Hz, pops one sample from ring and updates PWM. */
static bool audio_timer_cb(repeating_timer_t *t) {
    (void)t;
    uint32_t r = audio_ring_read;
    uint32_t w = audio_ring_write;
    if (r != w) {
        int16_t sample = audio_ring_buf[r & AUDIO_RING_MASK];
        /* Fence: ensure sample is read before advancing read pointer */
        __sync_synchronize();
        audio_ring_read = r + 1u;
        /* PSG_calc() output is a sum of up to 3 channels, each 0..0xFF0
         * (never negative — see emu2149's mix_output), so max ~0x2FD0
         * (12240). Scale that directly to the 0..1023 (10-bit) PWM duty
         * range (>>4 caps at ~765, comfortably under 1023, no clamp
         * needed). The previous ">>8 + 128" on an 8-bit PWM only used
         * levels 128-175 (18% of the range, and only the upper half),
         * which produced a very quiet, distorted signal — this was the
         * main cause of PSG audio sounding like noise rather than clean
         * tones (along with the PWM carrier frequency bug fixed above). */
        int32_t filtered = sample;
        if (audio_lpf_shift > 0) {
            audio_lpf_state += (sample - audio_lpf_state) >> audio_lpf_shift;
            filtered = audio_lpf_state;
        }
        uint32_t scaled = ((uint32_t)filtered * audio_volume) >> 12;
        uint16_t val = (uint16_t)(scaled > 1023u ? 1023u : scaled);
        pwm_set_gpio_level(audio_pwm_pin, val);
    }
    /* Ring underrun: hold last PWM level (no pop) */
    return true;
}
#endif /* HAVE_PICO_SDK */

/* -----------------------------------------------------------------------
 * msx.init()
 * ----------------------------------------------------------------------- */
static mp_obj_t msx_py_init(void) {
    /* Close any cart file object left open by a previous msx.init() call
     * (e.g. re-running the script without a hardware reset in between) —
     * mirrors msx_init()'s own destroy-before-memset fix for the C-heap
     * leak this project hit earlier with the same re-run scenario. */
    for (uint8_t slot = 0; slot < 2; slot++) {
        mp_obj_t *fp = cart_file_slot(slot);
        if (*fp != MP_OBJ_NULL && *fp != mp_const_none) {
            mp_stream_close(*fp);
        }
        *fp = mp_const_none;
    }

    msx_init(&msx_state);
    /* msx_init() zeroes the whole state including cart_fetch_cb, so this
     * must be re-registered every time (harmless/idempotent otherwise). */
    msx_set_cart_fetch_cb(&msx_state, cart_fetch_from_pyfile, NULL);
    msx_global_initialized = true;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(msx_py_init_obj, msx_py_init);

/* -----------------------------------------------------------------------
 * msx.load_bios(data: bytes | bytearray) -> bool
 * ----------------------------------------------------------------------- */
static mp_obj_t msx_py_load_bios(mp_obj_t data_obj) {
    mp_buffer_info_t buf;
    mp_get_buffer_raise(data_obj, &buf, MP_BUFFER_READ);
    bool ok = msx_load_bios(&msx_state, (const uint8_t *)buf.buf,
                             (uint32_t)buf.len);
    return mp_obj_new_bool(ok);
}
static MP_DEFINE_CONST_FUN_OBJ_1(msx_py_load_bios_obj, msx_py_load_bios);

/* -----------------------------------------------------------------------
 * msx.get_bios_view() -> bytearray
 * Zero-copy view of the 32KB BIOS buffer (msx->bios) — always available,
 * whether or not a BIOS has been loaded yet. Pair with f.readinto() to
 * load the BIOS straight from a file without a separate ~32KB scratch
 * buffer, then msx.mark_bios_loaded(n) to validate/finalize. See
 * msx_mark_bios_loaded()'s comment in msx_core.c for why this exists.
 * ----------------------------------------------------------------------- */
static mp_obj_t msx_py_get_bios_view(void) {
    return mp_obj_new_bytearray_by_ref(MSX_BIOS_SIZE, msx_get_bios_ptr(&msx_state));
}
static MP_DEFINE_CONST_FUN_OBJ_0(msx_py_get_bios_view_obj, msx_py_get_bios_view);

/* -----------------------------------------------------------------------
 * msx.mark_bios_loaded(size: int) -> bool
 * Call after writing the BIOS image directly into msx.get_bios_view()
 * (e.g. via f.readinto()) instead of msx.load_bios()'s own copy.
 * ----------------------------------------------------------------------- */
static mp_obj_t msx_py_mark_bios_loaded(mp_obj_t size_obj) {
    bool ok = msx_mark_bios_loaded(&msx_state, (uint32_t)mp_obj_get_int(size_obj));
    return mp_obj_new_bool(ok);
}
static MP_DEFINE_CONST_FUN_OBJ_1(msx_py_mark_bios_loaded_obj, msx_py_mark_bios_loaded);

/* -----------------------------------------------------------------------
 * msx.load_cart(slot: int, data: bytes | bytearray, mapper: int = 0) -> bool
 * slot: 0 = cartridge slot 1, 1 = cartridge slot 2
 * mapper: 0=PLAIN/auto-detect, 1=ASCII8, 2=ASCII16, 3=KONAMI
 * ----------------------------------------------------------------------- */
static mp_obj_t msx_py_load_cart(size_t n_args, const mp_obj_t *args) {
    uint8_t slot   = (uint8_t)mp_obj_get_int(args[0]);
    mp_buffer_info_t buf;
    mp_get_buffer_raise(args[1], &buf, MP_BUFFER_READ);
    uint8_t mapper = (n_args >= 3) ? (uint8_t)mp_obj_get_int(args[2]) : MSX_MAPPER_PLAIN;
    bool ok = msx_load_cart(&msx_state, slot,
                             (const uint8_t *)buf.buf, (uint32_t)buf.len,
                             mapper);
    return mp_obj_new_bool(ok);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(msx_py_load_cart_obj, 2, 3,
                                            msx_py_load_cart);

/* -----------------------------------------------------------------------
 * msx.cart_alloc(slot: int, size: int) -> bytearray | None
 * Two-phase, zero-copy alternative to msx.load_cart() — see
 * msx_cart_alloc()'s comment in msx_core.c. Mallocs msx->cart[slot] and
 * returns a zero-copy view of it (None on failure) so the caller can
 * f.readinto() the ROM straight into it without a separate scratch
 * buffer; pair with msx.cart_finalize().
 * ----------------------------------------------------------------------- */
static mp_obj_t msx_py_cart_alloc(mp_obj_t slot_obj, mp_obj_t size_obj) {
    uint8_t slot = (uint8_t)mp_obj_get_int(slot_obj);
    uint32_t size = (uint32_t)mp_obj_get_int(size_obj);
    uint8_t *ptr = msx_cart_alloc(&msx_state, slot, size);
    if (!ptr) return mp_const_none;
    return mp_obj_new_bytearray_by_ref(size, ptr);
}
static MP_DEFINE_CONST_FUN_OBJ_2(msx_py_cart_alloc_obj, msx_py_cart_alloc);

/* -----------------------------------------------------------------------
 * msx.cart_finalize(slot: int, mapper: int = 0) -> bool
 * Call after writing the ROM into msx.cart_alloc()'s returned view —
 * does the mapper-detect/bank-init work msx.load_cart() normally does
 * right after its own copy.
 * ----------------------------------------------------------------------- */
static mp_obj_t msx_py_cart_finalize(size_t n_args, const mp_obj_t *args) {
    uint8_t slot   = (uint8_t)mp_obj_get_int(args[0]);
    uint8_t mapper = (n_args >= 2) ? (uint8_t)mp_obj_get_int(args[1]) : MSX_MAPPER_PLAIN;
    bool ok = msx_cart_finalize(&msx_state, slot, mapper);
    return mp_obj_new_bool(ok);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(msx_py_cart_finalize_obj, 1, 2,
                                            msx_py_cart_finalize);

/* -----------------------------------------------------------------------
 * msx.load_cart_paged(slot: int, file_obj, size: int, mapper: int) -> bool
 *
 * Mega ROM support: load a large (>~64KB) bank-switched cartridge
 * without ever holding the whole ROM in RAM. file_obj must already be
 * open (e.g. open(path, 'rb')) and seekable; this module keeps its own
 * reference to it (rooted against the GC) and reads individual 8KB
 * pages from it on demand as the game bank-switches — only 32KB is ever
 * resident at once, regardless of the ROM's real size. Do not close
 * file_obj yourself; msx.eject_cart() closes it. mapper must be
 * explicit (ASCII8/ASCII16/KONAMI — see msx.detect_mapper()); PLAIN is
 * rejected since paging only makes sense for bank-switched carts.
 * ----------------------------------------------------------------------- */
static mp_obj_t msx_py_load_cart_paged(size_t n_args, const mp_obj_t *args) {
    uint8_t  slot   = (uint8_t)mp_obj_get_int(args[0]);
    mp_obj_t file_obj = args[1];
    uint32_t size   = (uint32_t)mp_obj_get_int(args[2]);
    uint8_t  mapper = (uint8_t)mp_obj_get_int(args[3]);
    (void)n_args;

    if (slot > 1) return mp_obj_new_bool(false);

    /* Store the file reference BEFORE calling msx_load_cart_paged():
     * its initial cache prefill (loading the 4 starting pages) happens
     * synchronously inside that call via the fetch callback, which reads
     * this same reference — setting it afterward left the prefill
     * fetching against no file (silently failing) and the cache holding
     * whatever stale/uninitialized heap bytes happened to be there. */
    *cart_file_slot(slot) = file_obj;

    bool ok = msx_load_cart_paged(&msx_state, slot, size, mapper);
    if (!ok) {
        *cart_file_slot(slot) = mp_const_none;
    }
    return mp_obj_new_bool(ok);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(msx_py_load_cart_paged_obj, 4, 4,
                                            msx_py_load_cart_paged);

/* -----------------------------------------------------------------------
 * msx.detect_mapper(data: bytes | bytearray) -> int
 * Run the mapper-detection heuristic against a caller-supplied buffer
 * (e.g. just the first 64KB of a large ROM, read separately) — lets
 * Python choose the mapper for msx.load_cart_paged() without needing
 * the whole ROM in RAM.
 * ----------------------------------------------------------------------- */
static mp_obj_t msx_py_detect_mapper(mp_obj_t data_obj) {
    mp_buffer_info_t buf;
    mp_get_buffer_raise(data_obj, &buf, MP_BUFFER_READ);
    uint8_t mapper = msx_detect_mapper((const uint8_t *)buf.buf, (uint32_t)buf.len);
    return mp_obj_new_int(mapper);
}
static MP_DEFINE_CONST_FUN_OBJ_1(msx_py_detect_mapper_obj, msx_py_detect_mapper);

/* -----------------------------------------------------------------------
 * msx.eject_cart(slot: int)
 * ----------------------------------------------------------------------- */
static mp_obj_t msx_py_eject_cart(mp_obj_t slot_obj) {
    uint8_t slot = (uint8_t)mp_obj_get_int(slot_obj);
    msx_eject_cart(&msx_state, slot);
    if (slot <= 1) {
        mp_obj_t *fp = cart_file_slot(slot);
        if (*fp != MP_OBJ_NULL && *fp != mp_const_none) {
            mp_stream_close(*fp);
        }
        *fp = mp_const_none;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(msx_py_eject_cart_obj, msx_py_eject_cart);

/* -----------------------------------------------------------------------
 * msx.is_cart_paged(slot: int) -> bool
 * True if the cart in `slot` is SD-backed/paged (Mega ROM) rather than
 * fully in-RAM. Bank-switch fetches for a paged cart read from SD mid-
 * frame, which conflicts with a concurrent display DMA transfer on the
 * shared SPI1 bus — callers should skip the pipelined
 * render-while-computing-next-frame loop ordering when this is true.
 * ----------------------------------------------------------------------- */
static mp_obj_t msx_py_is_cart_paged(mp_obj_t slot_obj) {
    uint8_t slot = (uint8_t)mp_obj_get_int(slot_obj);
    if (slot > 1) return mp_obj_new_bool(false);
    return mp_obj_new_bool(msx_state.cart_paged[slot]);
}
static MP_DEFINE_CONST_FUN_OBJ_1(msx_py_is_cart_paged_obj, msx_py_is_cart_paged);

/* -----------------------------------------------------------------------
 * msx.reset()
 * ----------------------------------------------------------------------- */
static mp_obj_t msx_py_reset(void) {
    msx_reset(&msx_state);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(msx_py_reset_obj, msx_py_reset);

/* -----------------------------------------------------------------------
 * msx.run_frame() -> int
 * Executes one MSX frame (~59659 Z80 T-states), renders VDP to framebuf,
 * generates PSG audio samples, and pushes them to the audio ring buffer.
 * Returns: number of audio samples pushed this frame.
 * ----------------------------------------------------------------------- */
static mp_obj_t msx_py_run_frame(void) {
    uint32_t n = msx_run_frame(&msx_state);

#if HAVE_PICO_SDK
    /* Push audio samples into ring buffer (non-blocking, drop if full) */
    for (uint32_t i = 0; i < n; i++) {
        uint32_t w = audio_ring_write;
        /* Ring is full when (w - r) >= AUDIO_RING_SIZE */
        if ((w - audio_ring_read) < AUDIO_RING_SIZE) {
            audio_ring_buf[w & AUDIO_RING_MASK] = msx_state.audio_buf[i];
            __sync_synchronize();       /* ensure data visible before pointer */
            audio_ring_write = w + 1u;
        }
        /* If full, silently drop — prevents lag if display is slow */
    }
#endif

    return mp_obj_new_int((mp_int_t)n);
}
static MP_DEFINE_CONST_FUN_OBJ_0(msx_py_run_frame_obj, msx_py_run_frame);

/* -----------------------------------------------------------------------
 * msx.boost_peri_clock()
 * Reconfigures clk_peri to track clk_sys (see msx_core.h for why). Call
 * once at startup, then re-create any machine.UART objects so their baud
 * divisor is recalculated against the new clk_peri.
 * ----------------------------------------------------------------------- */
static mp_obj_t msx_py_boost_peri_clock(void) {
    msx_boost_peri_clock();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(msx_py_boost_peri_clock_obj, msx_py_boost_peri_clock);

/* -----------------------------------------------------------------------
 * msx.boost_dma_priority()
 * Gives DMA top priority on the bus fabric over the processors — fixes
 * display DMA slowdown caused by usb_host.init() (see msx_core.h).
 * ----------------------------------------------------------------------- */
static mp_obj_t msx_py_boost_dma_priority(void) {
    msx_boost_dma_priority();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(msx_py_boost_dma_priority_obj, msx_py_boost_dma_priority);

/* -----------------------------------------------------------------------
 * msx.set_joystick(port, state)
 * port: 0=JOY1, 1=JOY2. state: active-low bitmask — bit0 Up, bit1 Down,
 * bit2 Left, bit3 Right, bit4 TriggerA, bit5 TriggerB (0=pressed).
 * Call once per frame from the Python GPIO poll.
 * ----------------------------------------------------------------------- */
static mp_obj_t msx_py_set_joystick(mp_obj_t port_obj, mp_obj_t state_obj) {
    uint8_t port  = (uint8_t)mp_obj_get_int(port_obj);
    uint8_t state = (uint8_t)mp_obj_get_int(state_obj);
    msx_set_joystick(&msx_state, port, state);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(msx_py_set_joystick_obj, msx_py_set_joystick);

/* -----------------------------------------------------------------------
 * msx.init_display_hardware(spi_id, baud, mosi, sck, cs, dc, rst, bl,
 *                            lcd_w=480, lcd_h=320, rotate_180=False)
 * Full ILI9341/ST7796 initialization from C. Call once at startup instead
 * of using a Python display driver. lcd_w/lcd_h select the panel: 480,320
 * for ST7796 (MSP4021) or 320,240 for ILI9341 (MSP2402) — both share the
 * same init sequence/wiring on this board, this is just the size used to
 * center the native 256x192 image. Defaults to the ST7796 size if omitted.
 * rotate_180: flip the panel 180° (MADCTL variant), for upside-down
 * mounting. Defaults to False.
 * ----------------------------------------------------------------------- */
static mp_obj_t msx_py_init_display_hardware(size_t n_args,
                                               const mp_obj_t *args) {
    uint8_t  spi_id  = (uint8_t)mp_obj_get_int(args[0]);
    uint32_t baud    = (uint32_t)mp_obj_get_int(args[1]);
    uint8_t  mosi    = (uint8_t)mp_obj_get_int(args[2]);
    uint8_t  sck     = (uint8_t)mp_obj_get_int(args[3]);
    uint8_t  cs      = (uint8_t)mp_obj_get_int(args[4]);
    uint8_t  dc      = (uint8_t)mp_obj_get_int(args[5]);
    uint8_t  rst     = (uint8_t)mp_obj_get_int(args[6]);
    uint8_t  bl      = (uint8_t)mp_obj_get_int(args[7]);
    uint16_t lcd_w   = (n_args >= 9)  ? (uint16_t)mp_obj_get_int(args[8]) : LCD_W;
    uint16_t lcd_h   = (n_args >= 10) ? (uint16_t)mp_obj_get_int(args[9]) : LCD_H;
    bool     rotate_180 = (n_args >= 11) ? mp_obj_is_true(args[10]) : false;
    msx_init_display_hardware(&msx_state,
                               spi_id, baud, mosi, sck, cs, dc, rst, bl,
                               lcd_w, lcd_h, rotate_180);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(msx_py_init_display_hardware_obj,
                                            8, 11,
                                            msx_py_init_display_hardware);

/* -----------------------------------------------------------------------
 * msx.setup_display(spi_id: int, cs_pin: int, dc_pin: int)
 * Lightweight variant: SPI already initialized by Python display driver.
 * Just stores the hardware handles so render_to_display() can use them.
 * ----------------------------------------------------------------------- */
static mp_obj_t msx_py_setup_display(mp_obj_t spi_id_obj,
                                      mp_obj_t cs_pin_obj,
                                      mp_obj_t dc_pin_obj) {
#if HAVE_PICO_SDK
    uint8_t spi_id = (uint8_t)mp_obj_get_int(spi_id_obj);
    uint8_t cs_pin = (uint8_t)mp_obj_get_int(cs_pin_obj);
    uint8_t dc_pin = (uint8_t)mp_obj_get_int(dc_pin_obj);
    spi_inst_t *spi = (spi_id == 0) ? spi0 : spi1;
    msx_setup_display(&msx_state, spi, cs_pin, dc_pin);
    msx_state.spi_baudrate = 40000000u;  /* default */
#else
    (void)spi_id_obj; (void)cs_pin_obj; (void)dc_pin_obj;
#endif
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_3(msx_py_setup_display_obj,
                                   msx_py_setup_display);

/* -----------------------------------------------------------------------
 * msx.render_to_display()
 * Starts DMA transfer of framebuf → ST7796 (non-blocking).
 * Call msx.wait_display() before the next render or SD card access.
 * ----------------------------------------------------------------------- */
static mp_obj_t msx_py_render_to_display(void) {
    msx_render_to_display(&msx_state);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(msx_py_render_to_display_obj,
                                   msx_py_render_to_display);

/* -----------------------------------------------------------------------
 * msx.render_to_display_1to1()
 * Native 256x192, centered, no scaling — for A/B speed comparison.
 * ----------------------------------------------------------------------- */
static mp_obj_t msx_py_render_to_display_1to1(void) {
    msx_render_to_display_1to1(&msx_state);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(msx_py_render_to_display_1to1_obj,
                                   msx_py_render_to_display_1to1);

/* -----------------------------------------------------------------------
 * msx.wait_display()
 * Wait for the DMA display transfer to finish (blocking).
 * Must be called before SD card access or next render.
 * ----------------------------------------------------------------------- */
static mp_obj_t msx_py_wait_display(void) {
    msx_wait_display(&msx_state);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(msx_py_wait_display_obj, msx_py_wait_display);

/* -----------------------------------------------------------------------
 * msx.init_hdmi_output(cs_pin, baudrate)
 * Configure the HDMI bridge output (hdmi_bridge/README.md). Call once at
 * startup, after msx.init_display_hardware(). Reuses the LCD/SD's SPI1
 * instance — cs_pin is a new, dedicated chip-select GPIO (GP28). Also
 * sends the current 16-color palette once (see msx.send_hdmi_palette()).
 * ----------------------------------------------------------------------- */
static mp_obj_t msx_py_init_hdmi_output(mp_obj_t cs_pin_obj, mp_obj_t baud_obj) {
    uint8_t  cs_pin = (uint8_t)mp_obj_get_int(cs_pin_obj);
    uint32_t baud   = (uint32_t)mp_obj_get_int(baud_obj);
    msx_init_hdmi_output(&msx_state, cs_pin, baud);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(msx_py_init_hdmi_output_obj,
                                   msx_py_init_hdmi_output);

/* -----------------------------------------------------------------------
 * msx.send_hdmi_palette()
 * Send the current 16-color palette to the HDMI bridge (blocking). Already
 * called once by msx.init_hdmi_output(); exposed separately only in case
 * the palette ever needs to be resent manually. No-op unless
 * msx.init_hdmi_output() was called first.
 * ----------------------------------------------------------------------- */
static mp_obj_t msx_py_send_hdmi_palette(void) {
    msx_send_hdmi_palette(&msx_state);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(msx_py_send_hdmi_palette_obj,
                                   msx_py_send_hdmi_palette);

/* -----------------------------------------------------------------------
 * msx.render_to_hdmi()
 * Convert framebuf to 4-bit palette indices (2px/byte) and send it to the
 * HDMI bridge Pico 2 (blocking). Only call when the LCD/SD SPI1 bus is
 * idle (i.e. after msx.wait_display(), not concurrently with SD access).
 * No-op unless msx.init_hdmi_output() was called first. Only correct for
 * the game screen (guaranteed 16 palette colors) — menus should use
 * msx.render_to_hdmi_raw332() instead.
 * ----------------------------------------------------------------------- */
static mp_obj_t msx_py_render_to_hdmi(void) {
    msx_render_to_hdmi(&msx_state);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(msx_py_render_to_hdmi_obj,
                                   msx_py_render_to_hdmi);

/* -----------------------------------------------------------------------
 * msx.render_to_hdmi_raw332()
 * Same as msx.render_to_hdmi(), but sends full RGB332 (no palette lookup).
 * Use for menu/UI screens, which use colors outside the MSX's 16-color
 * palette (msx.render_to_hdmi() would render those as black).
 * ----------------------------------------------------------------------- */
static mp_obj_t msx_py_render_to_hdmi_raw332(void) {
    msx_render_to_hdmi_raw332(&msx_state);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(msx_py_render_to_hdmi_raw332_obj,
                                   msx_py_render_to_hdmi_raw332);

/*
 * msx.clear_hdmi()
 * Sends one all-black frame to the HDMI receiver, independent of the
 * current framebuf. Call once right after msx.init_hdmi_output() to blank
 * whatever the receiver was previously displaying (e.g. left over from a
 * different emulator/session) before this emulator's own first real frame.
 */
static mp_obj_t msx_py_clear_hdmi(void) {
    msx_clear_hdmi(&msx_state);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(msx_py_clear_hdmi_obj,
                                   msx_py_clear_hdmi);

/* -----------------------------------------------------------------------
 * msx.get_framebuf() -> memoryview
 * Returns a read-only view of the internal RGB565 framebuffer (256×192×2 bytes).
 * ----------------------------------------------------------------------- */
static mp_obj_t msx_py_get_framebuf(void) {
    return mp_obj_new_bytearray_by_ref(
        MSX_SCREEN_W * MSX_SCREEN_H * sizeof(uint16_t),
        msx_state.framebuf[msx_state.framebuf_ready_idx]);
}
static MP_DEFINE_CONST_FUN_OBJ_0(msx_py_get_framebuf_obj, msx_py_get_framebuf);

/* -----------------------------------------------------------------------
 * msx.get_audio_buf(n: int) -> memoryview
 * Returns a view of the first n audio samples (int16_t) from this frame.
 * ----------------------------------------------------------------------- */
static mp_obj_t msx_py_get_audio_buf(mp_obj_t n_obj) {
    uint32_t n = (uint32_t)mp_obj_get_int(n_obj);
    if (n > MSX_AUDIO_BUF_SIZE) n = MSX_AUDIO_BUF_SIZE;
    return mp_obj_new_bytearray_by_ref(n * sizeof(int16_t),
                                        msx_state.audio_buf);
}
static MP_DEFINE_CONST_FUN_OBJ_1(msx_py_get_audio_buf_obj, msx_py_get_audio_buf);

/* -----------------------------------------------------------------------
 * msx.set_key_matrix(row: int, col_mask: int)
 * Set one row of the MSX keyboard matrix.
 * col_mask is active-low: 0 = key pressed, 1 = key released (per bit).
 * ----------------------------------------------------------------------- */
static mp_obj_t msx_py_set_key_matrix(mp_obj_t row_obj, mp_obj_t mask_obj) {
    uint8_t row  = (uint8_t)mp_obj_get_int(row_obj);
    uint8_t mask = (uint8_t)mp_obj_get_int(mask_obj);
    msx_set_key_matrix(&msx_state, row, mask);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(msx_py_set_key_matrix_obj,
                                   msx_py_set_key_matrix);

/* -----------------------------------------------------------------------
 * msx.clear_keys()
 * ----------------------------------------------------------------------- */
static mp_obj_t msx_py_clear_keys(void) {
    msx_clear_keys(&msx_state);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(msx_py_clear_keys_obj, msx_py_clear_keys);

/* -----------------------------------------------------------------------
 * msx.setup_audio_pwm(pin: int) -> bool
 * Configure GPIO for 10-bit PWM audio and start the repeating timer that
 * feeds samples from the ring buffer at 22050 Hz.
 * ----------------------------------------------------------------------- */
static mp_obj_t msx_py_setup_audio_pwm(mp_obj_t pin_obj) {
#if HAVE_PICO_SDK
    uint8_t pin = (uint8_t)mp_obj_get_int(pin_obj);
    audio_pwm_pin   = pin;
    audio_pwm_slice = pwm_gpio_to_slice_num(pin);

    gpio_set_function(pin, GPIO_FUNC_PWM);

    /* 10-bit PWM: wrap = 1023, divider at its minimum (1.0) — the PWM
     * *carrier* must run far above the audio sample-update rate (22050 Hz,
     * governed separately by the repeating timer below) or its own
     * switching frequency is itself audible as a buzz/whine. This was
     * previously set to target ~22050 Hz for the carrier too, which is
     * exactly why PSG audio sounded like noise instead of clean tones.
     * At div16=16 (divider=1.0): carrier = sys_hz/1024, e.g. ~244 kHz at
     * 250 MHz or ~141 kHz at 144 MHz (clk_sys drops when USB host is
     * active) — both far above the audible range. */
    uint32_t div16 = 16u;  /* minimum divider = 1.0 -> fastest, quietest carrier */

    pwm_config cfg = pwm_get_default_config();
    pwm_config_set_clkdiv_int_frac(&cfg,
                                    (uint8_t)(div16 >> 4),
                                    (uint8_t)(div16 & 0x0F));
    pwm_config_set_wrap(&cfg, 1023);
    pwm_init(audio_pwm_slice, &cfg, true);
    pwm_set_gpio_level(pin, 0);  /* matches the unipolar 0..~765 sample scale below */

    audio_pwm_ready = true;

    /* Start repeating timer at 22050 Hz (negative = restart after callback) */
    if (!audio_timer_running) {
        add_repeating_timer_us(-45, audio_timer_cb, NULL, &audio_rep_timer);
        audio_timer_running = true;
    }

    return mp_obj_new_bool(true);
#else
    (void)pin_obj;
    return mp_obj_new_bool(false);
#endif
}
static MP_DEFINE_CONST_FUN_OBJ_1(msx_py_setup_audio_pwm_obj,
                                   msx_py_setup_audio_pwm);

/* -----------------------------------------------------------------------
 * msx.set_audio_volume(level: int)
 * level: 0-256+ (256 = original/default amplitude, ~75% PWM duty max;
 * lower values reduce amplitude — try e.g. 128 or 96 to tame a passive
 * piezo buzzer's mechanical overdrive/crackle on loud passages). Values
 * above 256 are allowed but will clip (hard-limited to the 10-bit PWM
 * range in the ISR). Takes effect immediately, no reinit needed.
 * ----------------------------------------------------------------------- */
static mp_obj_t msx_py_set_audio_volume(mp_obj_t level_obj) {
    mp_int_t level = mp_obj_get_int(level_obj);
    if (level < 0) level = 0;
    if (level > 0xFFFF) level = 0xFFFF;
    audio_volume = (uint16_t)level;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(msx_py_set_audio_volume_obj,
                                   msx_py_set_audio_volume);

/* -----------------------------------------------------------------------
 * msx.set_audio_filter(shift: int)
 * shift: 0 disables the low-pass (default, original unfiltered sound).
 * 1-6 apply progressively heavier one-pole IIR smoothing on the raw PSG
 * sample stream (approx cutoff_hz = (22050 >> shift) / (2*pi) — e.g.
 * shift=2 -> ~877Hz, shift=1 -> ~1755Hz). Smooths sample-to-sample jumps
 * to reduce harshness/crackle on a passive piezo buzzer; trades away some
 * high-frequency clarity. Takes effect immediately, no reinit needed.
 * ----------------------------------------------------------------------- */
static mp_obj_t msx_py_set_audio_filter(mp_obj_t shift_obj) {
    mp_int_t shift = mp_obj_get_int(shift_obj);
    if (shift < 0) shift = 0;
    if (shift > 15) shift = 15;
    audio_lpf_shift = (uint8_t)shift;
    audio_lpf_state = 0;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(msx_py_set_audio_filter_obj,
                                   msx_py_set_audio_filter);

/* -----------------------------------------------------------------------
 * msx.get_audio_volume() -> int / msx.get_audio_filter() -> int
 * Read back the current runtime audio settings (e.g. for a settings menu
 * to display current values before adjusting them).
 * ----------------------------------------------------------------------- */
static mp_obj_t msx_py_get_audio_volume(void) {
    return mp_obj_new_int(audio_volume);
}
static MP_DEFINE_CONST_FUN_OBJ_0(msx_py_get_audio_volume_obj,
                                   msx_py_get_audio_volume);

static mp_obj_t msx_py_get_audio_filter(void) {
    return mp_obj_new_int(audio_lpf_shift);
}
static MP_DEFINE_CONST_FUN_OBJ_0(msx_py_get_audio_filter_obj,
                                   msx_py_get_audio_filter);

/* -----------------------------------------------------------------------
 * msx.get_audio_ring_level() -> int
 * Returns: number of samples currently queued in the audio ring buffer.
 * Useful for monitoring lag / underrun from Python.
 * ----------------------------------------------------------------------- */
static mp_obj_t msx_py_get_audio_ring_level(void) {
    uint32_t level = audio_ring_write - audio_ring_read;
    return mp_obj_new_int((mp_int_t)level);
}
static MP_DEFINE_CONST_FUN_OBJ_0(msx_py_get_audio_ring_level_obj,
                                   msx_py_get_audio_ring_level);

/* -----------------------------------------------------------------------
 * msx.get_cart_fetch_stats() -> (bankswitch_count, fetch_count)
 * Cumulative since boot — see msx_get_cart_fetch_stats()'s comment.
 * bankswitch_count: every bank-select register write for a paged
 * (megarom) cart. fetch_count: the subset that missed the 32KB resident
 * cache and paid a synchronous flash-file read, stalling that frame.
 * Sample the delta over a known interval (e.g. every N frames) to get a
 * fetches/sec figure comparable against the FPS drop on megarom titles.
 * Always (0, 0) for non-paged (plain ROM) carts.
 * ----------------------------------------------------------------------- */
static mp_obj_t msx_py_get_cart_fetch_stats(void) {
    uint32_t bankswitch_count, fetch_count;
    msx_get_cart_fetch_stats(&msx_state, &bankswitch_count, &fetch_count);
    mp_obj_t items[2] = {
        mp_obj_new_int_from_uint(bankswitch_count),
        mp_obj_new_int_from_uint(fetch_count),
    };
    return mp_obj_new_tuple(2, items);
}
static MP_DEFINE_CONST_FUN_OBJ_0(msx_py_get_cart_fetch_stats_obj,
                                   msx_py_get_cart_fetch_stats);

/* -----------------------------------------------------------------------
 * msx.is_ready() -> bool
 * Returns True if init() and load_bios() have been called.
 * ----------------------------------------------------------------------- */
static mp_obj_t msx_py_is_ready(void) {
    return mp_obj_new_bool(msx_global_initialized && msx_state.bios_loaded);
}
static MP_DEFINE_CONST_FUN_OBJ_0(msx_py_is_ready_obj, msx_py_is_ready);

/* -----------------------------------------------------------------------
 * msx.get_vdp_reg(reg: int) -> int
 * Read a TMS9918A register value (0–7) for debugging.
 * ----------------------------------------------------------------------- */
static mp_obj_t msx_py_get_vdp_reg(mp_obj_t reg_obj) {
    uint8_t reg = (uint8_t)mp_obj_get_int(reg_obj);
    if (!msx_state.vdp || reg >= TMS_NUM_REGISTERS) return mp_obj_new_int(0);
    return mp_obj_new_int(vrEmuTms9918RegValue(msx_state.vdp,
                                                (vrEmuTms9918Register)reg));
}
static MP_DEFINE_CONST_FUN_OBJ_1(msx_py_get_vdp_reg_obj, msx_py_get_vdp_reg);

/* -----------------------------------------------------------------------
 * msx.get_ram_view() -> bytearray
 * Zero-copy view of the live 64KB MSX RAM (msx_state.ram) — for
 * save-state, f.write() this directly; for load-state, f.readinto() this
 * directly. No allocation at all: this is memory the emulator already
 * has, whether or not save-state is ever used.
 * ----------------------------------------------------------------------- */
static mp_obj_t msx_py_get_ram_view(void) {
    return mp_obj_new_bytearray_by_ref(MSX_RAM_SIZE, msx_state.ram);
}
static MP_DEFINE_CONST_FUN_OBJ_0(msx_py_get_ram_view_obj, msx_py_get_ram_view);

/* -----------------------------------------------------------------------
 * msx.get_vram_view() -> bytearray
 * Zero-copy view of the live 16KB VDP VRAM (pattern/name/color tables,
 * sprites) — same rationale as get_ram_view(). Save/restoring this too
 * (in addition to RAM + registers) means the screen is correct
 * immediately after loading a save-state instead of looking corrupted
 * until the game's own code next redraws it.
 * ----------------------------------------------------------------------- */
static mp_obj_t msx_py_get_vram_view(void) {
    uint8_t *vram = msx_get_vram_ptr(&msx_state);
    if (!vram) return mp_const_false;
    return mp_obj_new_bytearray_by_ref(MSX_VRAM_SIZE, vram);
}
static MP_DEFINE_CONST_FUN_OBJ_0(msx_py_get_vram_view_obj, msx_py_get_vram_view);

/* -----------------------------------------------------------------------
 * msx.get_state_header() -> bytearray
 * Serializes everything BUT ram[] (CPU/VDP/mapper registers etc, 64
 * bytes total — see MSX_SAVE_HDR_SZ) into a small static buffer and
 * returns a zero-copy view of it. Pair with get_ram_view() for the full
 * save-state; splitting it this way means save/load never needs a large
 * (~64KB) contiguous allocation (see g_state_hdr_buf's comment for why
 * that reliably fails with MemoryError under normal use).
 * ----------------------------------------------------------------------- */
static mp_obj_t msx_py_get_state_header(void) {
    if (!msx_save_state_header(&msx_state, g_state_hdr_buf, sizeof(g_state_hdr_buf))) {
        return mp_const_false;
    }
    return mp_obj_new_bytearray_by_ref(sizeof(g_state_hdr_buf), g_state_hdr_buf);
}
static MP_DEFINE_CONST_FUN_OBJ_0(msx_py_get_state_header_obj, msx_py_get_state_header);

/* -----------------------------------------------------------------------
 * msx.set_state_header(data: bytes | bytearray) -> bool
 * Applies a header previously returned by get_state_header() (CPU/VDP/
 * mapper registers). Call this, then f.readinto(msx.get_ram_view()), to
 * fully restore a save-state.
 * ----------------------------------------------------------------------- */
static mp_obj_t msx_py_set_state_header(mp_obj_t data_obj) {
    mp_buffer_info_t buf;
    mp_get_buffer_raise(data_obj, &buf, MP_BUFFER_READ);
    bool ok = msx_load_state_header(&msx_state, buf.buf, buf.len);
    return mp_obj_new_bool(ok);
}
static MP_DEFINE_CONST_FUN_OBJ_1(msx_py_set_state_header_obj, msx_py_set_state_header);

/* -----------------------------------------------------------------------
 * Debug: msx.debug_step(n) -> pc
 * Executes exactly n raw Z80 instructions (no VDP/audio/frame timing).
 * ----------------------------------------------------------------------- */
static mp_obj_t msx_py_debug_step(mp_obj_t n_obj) {
    uint32_t n = (uint32_t)mp_obj_get_int(n_obj);
    uint16_t pc = msx_debug_step(&msx_state, n);
    return mp_obj_new_int(pc);
}
static MP_DEFINE_CONST_FUN_OBJ_1(msx_py_debug_step_obj, msx_py_debug_step);

/* -----------------------------------------------------------------------
 * Debug: msx.debug_cpu() -> (pc, sp, a, f, cyc, halted, iff1, int_mode)
 * ----------------------------------------------------------------------- */
static mp_obj_t msx_py_debug_cpu(void) {
    msx_debug_cpu_t c;
    msx_debug_get_cpu(&msx_state, &c);
    mp_obj_t items[8] = {
        mp_obj_new_int(c.pc),
        mp_obj_new_int(c.sp),
        mp_obj_new_int(c.a),
        mp_obj_new_int(c.f),
        mp_obj_new_int(c.cyc),
        mp_obj_new_bool(c.halted),
        mp_obj_new_bool(c.iff1),
        mp_obj_new_int(c.int_mode),
    };
    return mp_obj_new_tuple(8, items);
}
static MP_DEFINE_CONST_FUN_OBJ_0(msx_py_debug_cpu_obj, msx_py_debug_cpu);

/* -----------------------------------------------------------------------
 * Debug: msx.debug_peek(addr) -> byte
 * ----------------------------------------------------------------------- */
static mp_obj_t msx_py_debug_peek(mp_obj_t addr_obj) {
    uint16_t addr = (uint16_t)mp_obj_get_int(addr_obj);
    return mp_obj_new_int(msx_debug_peek(&msx_state, addr));
}
static MP_DEFINE_CONST_FUN_OBJ_1(msx_py_debug_peek_obj, msx_py_debug_peek);

/* -----------------------------------------------------------------------
 * Debug: msx.debug_spi_baud() -> actual Hz achieved by the last render
 * ----------------------------------------------------------------------- */
static mp_obj_t msx_py_debug_spi_baud(void) {
    return mp_obj_new_int_from_uint(msx_debug_get_spi_baud(&msx_state));
}
static MP_DEFINE_CONST_FUN_OBJ_0(msx_py_debug_spi_baud_obj, msx_py_debug_spi_baud);

/* -----------------------------------------------------------------------
 * Debug: msx.debug_clocks() -> (clk_sys_hz, clk_peri_hz)
 * ----------------------------------------------------------------------- */
static mp_obj_t msx_py_debug_clocks(void) {
    uint32_t sys_hz, peri_hz;
    msx_debug_get_clocks(&sys_hz, &peri_hz);
    mp_obj_t items[2] = {
        mp_obj_new_int_from_uint(sys_hz),
        mp_obj_new_int_from_uint(peri_hz),
    };
    return mp_obj_new_tuple(2, items);
}
static MP_DEFINE_CONST_FUN_OBJ_0(msx_py_debug_clocks_obj, msx_py_debug_clocks);

/* -----------------------------------------------------------------------
 * Debug: msx.debug_run_line(line, do_video, do_audio, do_int)
 * ----------------------------------------------------------------------- */
static mp_obj_t msx_py_debug_run_line(size_t n_args, const mp_obj_t *args) {
    uint32_t line     = (uint32_t)mp_obj_get_int(args[0]);
    bool do_video = mp_obj_is_true(args[1]);
    bool do_audio = mp_obj_is_true(args[2]);
    bool do_int   = mp_obj_is_true(args[3]);
    msx_debug_run_line(&msx_state, line, do_video, do_audio, do_int);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(msx_py_debug_run_line_obj, 4, 4,
                                            msx_py_debug_run_line);

/* -----------------------------------------------------------------------
 * Debug: msx.debug_psg() -> (quality, clk, rate, base_incr, realstep,
 *                             psgtime, psgstep, freq_limit, env_ptr,
 *                             env_pause, env_continue, env_face,
 *                             env_freq, env_count, freq0, count0)
 * ----------------------------------------------------------------------- */
static mp_obj_t msx_py_debug_psg(void) {
    msx_debug_psg_t p;
    msx_debug_get_psg(&msx_state, &p);
    mp_obj_t items[16] = {
        mp_obj_new_int(p.quality),
        mp_obj_new_int(p.clk),
        mp_obj_new_int(p.rate),
        mp_obj_new_int(p.base_incr),
        mp_obj_new_int(p.realstep),
        mp_obj_new_int(p.psgtime),
        mp_obj_new_int(p.psgstep),
        mp_obj_new_int(p.freq_limit),
        mp_obj_new_int(p.env_ptr),
        mp_obj_new_int(p.env_pause),
        mp_obj_new_int(p.env_continue),
        mp_obj_new_int(p.env_face),
        mp_obj_new_int(p.env_freq),
        mp_obj_new_int(p.env_count),
        mp_obj_new_int(p.freq0),
        mp_obj_new_int(p.count0),
    };
    return mp_obj_new_tuple(16, items);
}
static MP_DEFINE_CONST_FUN_OBJ_0(msx_py_debug_psg_obj, msx_py_debug_psg);

/* -----------------------------------------------------------------------
 * Debug: msx.debug_psg_calc(n) -> last sample
 * Calls PSG_calc() exactly n times; used to bisect audio-path hangs.
 * ----------------------------------------------------------------------- */
static mp_obj_t msx_py_debug_psg_calc(mp_obj_t n_obj) {
    uint32_t n = (uint32_t)mp_obj_get_int(n_obj);
    int16_t last = msx_debug_psg_calc_n(&msx_state, n);
    return mp_obj_new_int(last);
}
static MP_DEFINE_CONST_FUN_OBJ_1(msx_py_debug_psg_calc_obj, msx_py_debug_psg_calc);

/* -----------------------------------------------------------------------
 * Module table
 * ----------------------------------------------------------------------- */
static const mp_rom_map_elem_t msx_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__),               MP_ROM_QSTR(MP_QSTR_msx) },
    /* Lifecycle */
    { MP_ROM_QSTR(MP_QSTR_init),                   MP_ROM_PTR(&msx_py_init_obj) },
    { MP_ROM_QSTR(MP_QSTR_reset),                  MP_ROM_PTR(&msx_py_reset_obj) },
    { MP_ROM_QSTR(MP_QSTR_is_ready),               MP_ROM_PTR(&msx_py_is_ready_obj) },
    /* ROM management */
    { MP_ROM_QSTR(MP_QSTR_load_bios),              MP_ROM_PTR(&msx_py_load_bios_obj) },
    { MP_ROM_QSTR(MP_QSTR_get_bios_view),          MP_ROM_PTR(&msx_py_get_bios_view_obj) },
    { MP_ROM_QSTR(MP_QSTR_mark_bios_loaded),       MP_ROM_PTR(&msx_py_mark_bios_loaded_obj) },
    { MP_ROM_QSTR(MP_QSTR_load_cart),              MP_ROM_PTR(&msx_py_load_cart_obj) },
    { MP_ROM_QSTR(MP_QSTR_cart_alloc),             MP_ROM_PTR(&msx_py_cart_alloc_obj) },
    { MP_ROM_QSTR(MP_QSTR_cart_finalize),          MP_ROM_PTR(&msx_py_cart_finalize_obj) },
    { MP_ROM_QSTR(MP_QSTR_load_cart_paged),        MP_ROM_PTR(&msx_py_load_cart_paged_obj) },
    { MP_ROM_QSTR(MP_QSTR_detect_mapper),          MP_ROM_PTR(&msx_py_detect_mapper_obj) },
    { MP_ROM_QSTR(MP_QSTR_eject_cart),             MP_ROM_PTR(&msx_py_eject_cart_obj) },
    { MP_ROM_QSTR(MP_QSTR_is_cart_paged),          MP_ROM_PTR(&msx_py_is_cart_paged_obj) },
    /* Emulation */
    { MP_ROM_QSTR(MP_QSTR_run_frame),              MP_ROM_PTR(&msx_py_run_frame_obj) },
    { MP_ROM_QSTR(MP_QSTR_boost_peri_clock),       MP_ROM_PTR(&msx_py_boost_peri_clock_obj) },
    { MP_ROM_QSTR(MP_QSTR_boost_dma_priority),     MP_ROM_PTR(&msx_py_boost_dma_priority_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_joystick),           MP_ROM_PTR(&msx_py_set_joystick_obj) },
    /* Display — full init (C manages SPI + RST + BL) */
    { MP_ROM_QSTR(MP_QSTR_init_display_hardware),  MP_ROM_PTR(&msx_py_init_display_hardware_obj) },
    /* Display — lightweight (Python already initialized SPI) */
    { MP_ROM_QSTR(MP_QSTR_setup_display),          MP_ROM_PTR(&msx_py_setup_display_obj) },
    { MP_ROM_QSTR(MP_QSTR_render_to_display),      MP_ROM_PTR(&msx_py_render_to_display_obj) },
    { MP_ROM_QSTR(MP_QSTR_render_to_display_1to1), MP_ROM_PTR(&msx_py_render_to_display_1to1_obj) },
    { MP_ROM_QSTR(MP_QSTR_wait_display),           MP_ROM_PTR(&msx_py_wait_display_obj) },
    { MP_ROM_QSTR(MP_QSTR_get_framebuf),           MP_ROM_PTR(&msx_py_get_framebuf_obj) },
    /* Display — HDMI bridge (hdmi_bridge/README.md), opt-in via config.txt */
    { MP_ROM_QSTR(MP_QSTR_init_hdmi_output),       MP_ROM_PTR(&msx_py_init_hdmi_output_obj) },
    { MP_ROM_QSTR(MP_QSTR_send_hdmi_palette),      MP_ROM_PTR(&msx_py_send_hdmi_palette_obj) },
    { MP_ROM_QSTR(MP_QSTR_render_to_hdmi),         MP_ROM_PTR(&msx_py_render_to_hdmi_obj) },
    { MP_ROM_QSTR(MP_QSTR_render_to_hdmi_raw332),  MP_ROM_PTR(&msx_py_render_to_hdmi_raw332_obj) },
    { MP_ROM_QSTR(MP_QSTR_clear_hdmi),             MP_ROM_PTR(&msx_py_clear_hdmi_obj) },
    /* Keyboard */
    { MP_ROM_QSTR(MP_QSTR_set_key_matrix),         MP_ROM_PTR(&msx_py_set_key_matrix_obj) },
    { MP_ROM_QSTR(MP_QSTR_clear_keys),             MP_ROM_PTR(&msx_py_clear_keys_obj) },
    /* Audio — ring buffer + timer (no Python involvement needed) */
    { MP_ROM_QSTR(MP_QSTR_setup_audio_pwm),        MP_ROM_PTR(&msx_py_setup_audio_pwm_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_audio_volume),       MP_ROM_PTR(&msx_py_set_audio_volume_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_audio_filter),       MP_ROM_PTR(&msx_py_set_audio_filter_obj) },
    { MP_ROM_QSTR(MP_QSTR_get_audio_volume),       MP_ROM_PTR(&msx_py_get_audio_volume_obj) },
    { MP_ROM_QSTR(MP_QSTR_get_audio_filter),       MP_ROM_PTR(&msx_py_get_audio_filter_obj) },
    { MP_ROM_QSTR(MP_QSTR_get_audio_ring_level),   MP_ROM_PTR(&msx_py_get_audio_ring_level_obj) },
    { MP_ROM_QSTR(MP_QSTR_get_cart_fetch_stats),   MP_ROM_PTR(&msx_py_get_cart_fetch_stats_obj) },
    /* State save / load */
    { MP_ROM_QSTR(MP_QSTR_get_ram_view),           MP_ROM_PTR(&msx_py_get_ram_view_obj) },
    { MP_ROM_QSTR(MP_QSTR_get_vram_view),          MP_ROM_PTR(&msx_py_get_vram_view_obj) },
    { MP_ROM_QSTR(MP_QSTR_get_state_header),       MP_ROM_PTR(&msx_py_get_state_header_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_state_header),       MP_ROM_PTR(&msx_py_set_state_header_obj) },
    /* Debug */
    { MP_ROM_QSTR(MP_QSTR_get_audio_buf),          MP_ROM_PTR(&msx_py_get_audio_buf_obj) },
    { MP_ROM_QSTR(MP_QSTR_get_vdp_reg),            MP_ROM_PTR(&msx_py_get_vdp_reg_obj) },
    { MP_ROM_QSTR(MP_QSTR_debug_step),             MP_ROM_PTR(&msx_py_debug_step_obj) },
    { MP_ROM_QSTR(MP_QSTR_debug_cpu),              MP_ROM_PTR(&msx_py_debug_cpu_obj) },
    { MP_ROM_QSTR(MP_QSTR_debug_peek),             MP_ROM_PTR(&msx_py_debug_peek_obj) },
    { MP_ROM_QSTR(MP_QSTR_debug_run_line),         MP_ROM_PTR(&msx_py_debug_run_line_obj) },
    { MP_ROM_QSTR(MP_QSTR_debug_spi_baud),         MP_ROM_PTR(&msx_py_debug_spi_baud_obj) },
    { MP_ROM_QSTR(MP_QSTR_debug_clocks),           MP_ROM_PTR(&msx_py_debug_clocks_obj) },
    { MP_ROM_QSTR(MP_QSTR_debug_psg),              MP_ROM_PTR(&msx_py_debug_psg_obj) },
    { MP_ROM_QSTR(MP_QSTR_debug_psg_calc),         MP_ROM_PTR(&msx_py_debug_psg_calc_obj) },
    /* Constants */
    { MP_ROM_QSTR(MP_QSTR_MAPPER_PLAIN),           MP_ROM_INT(MSX_MAPPER_PLAIN) },
    { MP_ROM_QSTR(MP_QSTR_MAPPER_ASCII8),          MP_ROM_INT(MSX_MAPPER_ASCII8) },
    { MP_ROM_QSTR(MP_QSTR_MAPPER_ASCII16),         MP_ROM_INT(MSX_MAPPER_ASCII16) },
    { MP_ROM_QSTR(MP_QSTR_MAPPER_KONAMI),          MP_ROM_INT(MSX_MAPPER_KONAMI) },
    { MP_ROM_QSTR(MP_QSTR_SCREEN_W),               MP_ROM_INT(MSX_SCREEN_W) },
    { MP_ROM_QSTR(MP_QSTR_SCREEN_H),               MP_ROM_INT(MSX_SCREEN_H) },
    { MP_ROM_QSTR(MP_QSTR_KEY_ROWS),               MP_ROM_INT(MSX_KEY_ROWS) },
    { MP_ROM_QSTR(MP_QSTR_AUDIO_RATE),             MP_ROM_INT(MSX_AUDIO_RATE) },
    { MP_ROM_QSTR(MP_QSTR_RAM_SIZE),               MP_ROM_INT(MSX_RAM_SIZE) },
    { MP_ROM_QSTR(MP_QSTR_VRAM_SIZE),              MP_ROM_INT(MSX_VRAM_SIZE) },
};
static MP_DEFINE_CONST_DICT(msx_module_globals, msx_module_globals_table);

const mp_obj_module_t msx_user_cmodule = {
    .base    = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&msx_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_msx, msx_user_cmodule);
