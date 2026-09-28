/*
 * disp_dvi.c — onboard DVI output driver for the Waveshare RP2350-PiZero.
 *
 * 2026-09-20 first draft, 2026-09-22/25 real-hardware bring-up (see
 * 調査用/RP2350-PiZero_HDMI出力適用調査.md §7 and
 * src/msx/libdvi/PROVENANCE.md). CONFIRMED WORKING on real hardware
 * (color-bar test pattern displayed correctly over DVI) as of 2026-09-25.
 * This file is only compiled when building for the WAVESHARE_RP2350_PIZERO
 * board (see micropython_msx.cmake's BOARD-conditioned target_sources); a
 * Pico 2 build never sees it.
 *
 * ---------------------------------------------------------------------
 * Hardware facts this file depends on (confirmed against the vendor
 * schematic by an independent working reference port, see PROVENANCE.md,
 * and now also directly confirmed by a real-hardware color-bar test):
 *   - DVI/HDMI-connector TMDS pins: D2±=GPIO32/33, D1±=GPIO34/35,
 *     D0±=GPIO36/37, CLK±=GPIO38/39 — all outside the RP2350's fixed
 *     HSTX pin range (GPIO12-19), so this MUST go through PIO, not HSTX
 *     (see the investigation doc §4.1).
 *   - RP2350B's PIO can only address a 32-pin *window* of its 48 GPIOs at
 *     a time (`pio_set_gpio_base()`); our TMDS pins (32-39) need the
 *     window based at 16 (covers 16-47). See hardware/pio.h's own
 *     PICO_PIO_USE_GPIO_BASE doc comment for the full explanation — this
 *     project's pico-sdk snapshot (2.2.0) already has this API.
 *   - DMA_IRQ_1, not DMA_IRQ_0 — something else in this MicroPython build
 *     already holds DMA_IRQ_0's exclusive handler slot (real-hardware
 *     "Hard assert" inside irq_set_exclusive_handler(), 2026-09-22, never
 *     traced further; DMA_IRQ_1 sidesteps it).
 *   - vreg 1.20V / 252MHz bit clock: confirmed locking correctly on real
 *     silicon (set_sys_clock_khz() returns true, and the color-bar test
 *     displayed a stable, correctly-timed picture).
 *
 * ---------------------------------------------------------------------
 * Core split: core1 owns DVI *entirely*, standalone — core0 never
 * touches DVI again after launching core1, freeing it for Z80/VDP/PSG
 * emulation + the MicroPython VM.
 *
 * 2026-09-25 real-hardware finding — no separate encoder "thread":
 * libdvi's documented two-stage pipeline (a `scanline_callback` feeding
 * a raw-pixel queue, consumed by a *separate* call to
 * dvi_scanbuf_main_16bpp() which TMDS-encodes and feeds a second queue)
 * is designed for TWO SEPARATE CORES (core0 producing, core1 encoding —
 * libdvi's own hello_dvi.c example) or a tight producer loop with no
 * other work to do. Running both stages *interleaved on one core* via
 * scanline_callback (an IRQ hook) — the only option available here,
 * since core0 is never free for this — produced a permanently solid RED
 * screen on real hardware (libdvi's own "no valid scanline ready"
 * fallback, see dvi.c's dvi_dma_irq_handler()), regardless of which of
 * the two queues fed it or how cheap the callback was.
 *
 * This version instead has the scanline_callback do the FULL job itself
 * — pop a free TMDS buffer, call tmds_encode_data_channel_16bpp()
 * directly (the same three-line body dvi.c's own file-static
 * _dvi_prepare_scanline_16bpp() uses internally, duplicated here since
 * that function isn't exported), and push straight to q_tmds_valid — no
 * q_colour_valid/free, no separate encoder call, no second core needed.
 * core1's own "main" context (after dvi_start()) is now just an idle
 * loop; the DMA IRQ (and our callback, invoked from inside it) does
 * everything.
 *
 * 2026-09-25 real-hardware finding — startup pre-fill is mandatory:
 * even with the single-stage design above, the screen stayed solid red
 * until q_tmds_valid was pre-filled with DVI_N_TMDS_BUFFERS (=3, see
 * dvi_config_defs.h — libdvi's hardcoded total buffer count) BEFORE
 * dvi_start() ever runs. Root cause, traced through dvi_dma_irq_handler():
 * the very first IRQ always finds q_tmds_valid empty (nothing produced
 * yet) and increments late_scanline_ctr; from then on, that same
 * handler's own "late scanline" backlog-drain step
 * (`while (late_scanline_ctr > 0 && queue_try_remove_u32(q_tmds_valid,
 * ...))`) treats ANY freshly-callback-produced buffer as stale backlog
 * to discard, not current data to display — a one-buffer-per-callback
 * producer can never out-produce that drain, so the system never
 * recovers once late_scanline_ctr goes positive. Pre-filling every
 * available buffer before the first IRQ ever fires means
 * late_scanline_ctr never goes positive in the first place.
 *
 * ---------------------------------------------------------------------
 * No persistent DVI framebuffer: converts directly from
 * msx->framebuf[ready_idx] on the fly, one row at a time, inside the
 * scanline_callback — see msx_state_t's own size (~305KB, dominated by
 * its own double-buffered 256x192 framebuf, needed regardless of board)
 * for why a further ~150KB persistent DVI-sized buffer doesn't fit
 * alongside a Mega ROM cart cache and MicroPython's own >64KB-minimum GC
 * heap (both failed outright in an earlier design that tried). The
 * conversion itself is cheap (a 16-bit byte-swap per pixel — framebuf is
 * byte-swapped for the LCD's SPI convention, DVI wants it native — no
 * palette lookup needed).
 *
 * msx->framebuf is double-buffered specifically so a reader (previously
 * the LCD's DMA, now this callback) can safely read framebuf[ready_idx]
 * on one core while msx_run_frame() concurrently writes the *other*
 * buffer on the other core — the same established, already-proven
 * pattern, not a new risk. framebuf_ready_idx itself is a single byte;
 * reads of it can't tear.
 *
 * msx_render_to_display_dvi() (called once per completed MSX frame from
 * Python, mirroring msx_render_to_display_1to1()) is kept only as a
 * stable API no-op — this file has no per-frame "compose" step at all,
 * but Python's call site shouldn't need to know that.
 *
 * ---------------------------------------------------------------------
 * MSX's native 256x192 is centered in the 320x240 (source-resolution)
 * output (32px left/right, 24px top/bottom border, matching main.py's
 * LCD centering convention) — libdvi's DVI_VERTICAL_REPEAT=2 timing plus
 * its TMDS encoder's own horizontal 2x pixel replication (feeds
 * h_active_pixels/2 = 320 source samples per line into a 640-wide
 * encode) scale this out to a locked 640x480p60 signal with no software
 * upscaling needed here — confirmed on real hardware via the color-bar
 * test.
 *
 * ---------------------------------------------------------------------
 * STILL OPEN for real-hardware bring-up: the color-bar test validated
 * the DVI signal path itself, but rendering the *actual MSX picture*
 * through this driver (reading real msx->framebuf content, not a static
 * test pattern) has not yet been confirmed on real hardware.
 */

#include "msx_core.h"

#ifdef MSX_BOARD_PIZERO

#include "pico/multicore.h"
#include "pico/sync.h"
#include "pico/time.h"
#include "hardware/clocks.h"
#include "hardware/sync.h"
#include "hardware/vreg.h"
#include "hardware/pio.h"

/* 2026-09-25 real-hardware finding: libdvi's default DVI_N_TMDS_BUFFERS
 * (3, dvi_config_defs.h) gives the scanline_callback producer ZERO spare
 * margin — since we can only ever supply exactly one buffer per unique-
 * scanline IRQ (there is no way to "catch up" faster than that), ANY
 * single missed deadline (e.g. a few-microsecond delay from an unrelated
 * spinlock/critical-section held elsewhere in the firmware — confirmed
 * on real hardware: plain gc.collect() was enough) pushes
 * dvi_dma_irq_handler()'s late_scanline_ctr positive, and its own "late
 * backlog" drain logic then discards every subsequent buffer as stale
 * before it can ever be displayed — permanently, since our producer can
 * never out-produce that drain at exactly 1-per-callback. Raising the
 * buffer count gives slack: a handful of buffers already queued up
 * survive a brief stall without letting q_tmds_valid run dry, so
 * late_scanline_ctr never goes positive in the first place for anything
 * short of a genuinely sustained delay. Must be defined before
 * dvi_config_defs.h's #ifndef guard is reached (via dvi.h below). Each
 * buffer costs 3*640/2*4 = 3840 bytes from the C heap; 8 costs ~30KB
 * total, affordable now that this file no longer mallocs a ~150KB
 * persistent framebuffer (see "No persistent DVI framebuffer" below). */
#define DVI_N_TMDS_BUFFERS 8

#include "dvi.h"
#include "dvi_serialiser.h"
#include "tmds_encode.h"

/* DVI pin config — Waveshare RP2350-PiZero onboard HDMI connector.
 * See this file's header comment for the schematic-confirmed pin facts. */
static const struct dvi_serialiser_cfg pizero_dvi_cfg = {
    .pio              = pio0,
    .sm_tmds          = {0, 1, 2},
    .pins_tmds        = {36, 34, 32},  /* D0+, D1+, D2+ (the +/odd pin of
                                         * each differential pair; the -
                                         * pin is always +1, handled by
                                         * dvi_serialiser_init() itself) */
    .pins_clk         = 38,
    .invert_diffpairs = false,
};

#define DVI_FB_W 320
#define DVI_FB_H 240
#define DVI_BORDER_X ((DVI_FB_W - MSX_SCREEN_W) / 2)  /* 32 */
#define DVI_BORDER_Y ((DVI_FB_H - MSX_SCREEN_H) / 2)  /* 24 */

/* One source-resolution (pre-TMDS-encode) scratch row — reused every
 * callback invocation. Only ever touched from within the scanline
 * callback (interrupt context on core1), never concurrently, so a
 * single static buffer (not a round-robin ring) is sufficient: by the
 * time tmds_encode_data_channel_16bpp() returns, its output is already
 * safely in a TMDS buffer (queued to q_tmds_valid), and this scratch
 * row's previous content is no longer needed. */
static uint16_t dvi_row_buf[DVI_FB_W];

static struct dvi_inst dvi0;
static msx_state_t *dvi_msx = NULL;
static uint16_t dvi_border565 = 0;  /* plain (non-byte-swapped) RGB565 black */
static bool dvi_ready = false;

/* 2026-09-26: incremented once per dvi_scanline_pump() call (i.e. once per
 * unique source scanline, regardless of whether a free TMDS buffer was
 * actually available) — a cheap way to tell from Python, mid-runtime,
 * whether core1's DMA IRQ is even still alive at all, vs. having gone
 * completely silent (crashed/stuck). See msx_dvi_debug_info() below. */
static volatile uint32_t dvi_heartbeat = 0;

/* -----------------------------------------------------------------------
 * dvi_scanline_pump — the entire DVI "producer + encoder" in one
 * function, called from core1's DMA IRQ once per unique source scanline
 * (DVI_VERTICAL_REPEAT-gated, see dvi.c's dvi_dma_irq_handler()). See
 * this file's header comment for why there is no separate encoder call.
 *
 * Converts one row straight from msx->framebuf (border rows: solid
 * fill; content rows: a 16-bit byte-swap per pixel) into dvi_row_buf,
 * TMDS-encodes it directly into a buffer popped from q_tmds_free
 * (mirroring dvi.c's own file-static _dvi_prepare_scanline_16bpp()),
 * and pushes the result to q_tmds_valid. Runs in interrupt context, so
 * kept to cheap, bounded-time work only (no malloc, no blocking calls).
 *
 * 2026-09-26 real-hardware finding — __not_in_flash_func is mandatory
 * here: dvi.c's own dvi_dma_irq_handler() (the caller, invoked every
 * scanline) and tmds_encode_data_channel_16bpp() (called below) are both
 * already __not_in_flash_func in upstream libdvi — this function was the
 * one piece of the hot path still flash-resident (XIP), fetched fresh
 * from flash on every single scanline IRQ. That's harmless when core0 is
 * idle/barely touches flash (confirmed via an isolated bare-metal test:
 * rock stable for 30+s), but once MicroPython is actually running and
 * executing code on core0 — which is itself mostly flash-resident bytecode
 * interpreter/library code, fetched continuously via the SAME shared XIP
 * bus core1 needs for this function — real-hardware testing showed a
 * reproducible "snow" corruption appearing consistently ~1s after *any*
 * REPL statement executed (even a near-no-op one), while leaving core0
 * completely idle for 15+s after DVI init never corrupted anything at
 * all. dvi0.late_scanline_ctr/msx.dvi_debug()'s heartbeat stayed healthy
 * throughout every occurrence, meaning the DVI queue/timing bookkeeping
 * itself never detected a missed deadline — pointing at *content*
 * corruption from a delayed/contended instruction fetch on this exact
 * function, not a timing failure the library's own accounting would
 * catch. Moving it into RAM removes it from that shared bus entirely.
 * ----------------------------------------------------------------------- */
static void __not_in_flash_func(dvi_scanline_pump)(void) {
    static uint y = 0;

    if (y < DVI_BORDER_Y || y >= DVI_BORDER_Y + MSX_SCREEN_H) {
        for (int x = 0; x < DVI_FB_W; x++) {
            dvi_row_buf[x] = dvi_border565;
        }
    } else {
        const uint16_t *src = &dvi_msx->framebuf[dvi_msx->framebuf_ready_idx]
                                                  [(size_t)(y - DVI_BORDER_Y) * MSX_SCREEN_W];
        for (int x = 0; x < DVI_BORDER_X; x++) {
            dvi_row_buf[x] = dvi_border565;
        }
        for (int x = 0; x < MSX_SCREEN_W; x++) {
            uint16_t v = src[x];
            dvi_row_buf[DVI_BORDER_X + x] = (uint16_t)((v >> 8) | (v << 8));  /* undo the LCD's byte-swap */
        }
        for (int x = DVI_BORDER_X + MSX_SCREEN_W; x < DVI_FB_W; x++) {
            dvi_row_buf[x] = dvi_border565;
        }
    }

    uint32_t *tmdsbuf;
    if (queue_try_remove_u32(&dvi0.q_tmds_free, &tmdsbuf)) {
        const uint32_t *scanbuf = (const uint32_t *)dvi_row_buf;
        uint pixwidth = dvi0.timing->h_active_pixels;              /* 640 */
        uint words_per_channel = pixwidth / DVI_SYMBOLS_PER_WORD;  /* 320 */
        tmds_encode_data_channel_16bpp(scanbuf, tmdsbuf + 0 * words_per_channel, pixwidth / 2, DVI_16BPP_BLUE_MSB,  DVI_16BPP_BLUE_LSB );
        tmds_encode_data_channel_16bpp(scanbuf, tmdsbuf + 1 * words_per_channel, pixwidth / 2, DVI_16BPP_GREEN_MSB, DVI_16BPP_GREEN_LSB);
        tmds_encode_data_channel_16bpp(scanbuf, tmdsbuf + 2 * words_per_channel, pixwidth / 2, DVI_16BPP_RED_MSB,   DVI_16BPP_RED_LSB  );
        queue_try_add_u32(&dvi0.q_tmds_valid, &tmdsbuf);
    }
    /* If no free TMDS buffer was available, this scanline is simply
     * skipped (dvi_dma_irq_handler() falls back to its own solid-red
     * "no valid scanline" pattern for it) — should not normally happen
     * once the startup pre-fill (see msx_init_display_hardware_dvi())
     * has run, since production and consumption both proceed at exactly
     * one buffer per unique scanline from then on. */

    dvi_heartbeat++;
    y = (y + 1 == DVI_FB_H) ? 0 : (y + 1);
}

static void core1_dvi_main(void) {
    /* 2026-09-26 real-hardware finding: dvi_scanline_pump() and everything
     * it calls (tmds_encode_data_channel_16bpp(), the queue_* functions,
     * this whole file) execute from flash (XIP) on core1, since nothing
     * here is marked __not_in_flash_func(). MicroPython's own internal
     * flash filesystem driver (rp2_flash.c, use_multicore_lockout()) only
     * pauses "the other core" via multicore_lockout_*_blocking() around
     * flash erase/program IF that other core has previously called
     * multicore_lockout_victim_init() to register as a lockout victim —
     * grep confirms the ONLY caller of that init function anywhere in the
     * MicroPython rp2 port is mpthreadport.c, gated on the `_thread`
     * module actually launching core1, which we bypass entirely (we call
     * multicore_launch_core1() ourselves). Without this call, core0 was
     * free to erase/program flash (e.g. the mega-ROM cache writes in
     * _load_bios()/_load_cart_or_disk(), or any internal filesystem
     * journal write during earlier steps) while core1 kept trying to
     * fetch DVI code from that same flash — XIP is unavailable for the
     * whole duration of a flash erase/program op, so any such fetch on
     * core1 hangs/faults, permanently stopping the DVI DMA IRQ from ever
     * running again (PIO stalls once its TX FIFO drains) — this matches
     * the real-hardware symptom exactly: full "No Signal" (not just
     * corrupted-but-locked "snow") appearing at multiple, seemingly
     * unrelated steps of the boot sequence, unrecoverable without a
     * hardware reset. Registering as a lockout victim here makes core0's
     * flash operations correctly pause core1 (parked in RAM-resident code
     * with interrupts off — see pico/multicore.h's own doc comment) for
     * their duration instead of racing it. DVI will still visibly stall
     * during any flash write (a bounded, recoverable gap once buffers
     * refill), which is expected and fine — the alternative was a
     * permanent core1 crash. */
    multicore_lockout_victim_init();

    dvi_register_irqs_this_core(&dvi0, DMA_IRQ_1);
    dvi_start(&dvi0);
    /* Everything DVI-related happens in the IRQ (dvi_scanline_pump(),
     * above) — core1's own execution context has nothing left to do
     * except stay responsive to lockout requests from core0. */
    while (true) {
        __wfe();
    }
}

/* -----------------------------------------------------------------------
 * msx_init_display_hardware_dvi
 * ----------------------------------------------------------------------- */
bool msx_init_display_hardware_dvi(msx_state_t *msx) {
    dvi_msx = msx;
    dvi_border565 = 0;  /* black; MSX palette entry 1 is also black, but we
                          * don't need the full palette anymore — see header. */

    /* Voltage/clock: confirmed locking correctly on real hardware via
     * the color-bar bring-up test (2026-09-25). */
    vreg_set_voltage(VREG_VOLTAGE_1_20);
    sleep_ms(10);
    if (!set_sys_clock_khz(dvi_timing_640x480p_60hz.bit_clk_khz, true)) {
        return false;
    }

    /* RP2350B: PIO0 must be told which 32-pin GPIO window to address
     * before dvi_serialiser_init() configures pins 32-39 (see header
     * comment / hardware/pio.h's PICO_PIO_USE_GPIO_BASE doc). */
    pio_set_gpio_base(pizero_dvi_cfg.pio, 16);

    /* 2026-09-25 real-hardware finding: next_striped_spin_lock_num()
     * hands out one of only 8 spinlocks (16-23) SHARED with every other
     * subsystem in the firmware that also asks for a striped spinlock
     * (comment in hardware/sync.h: this only "reduces the probability"
     * of a collision, it doesn't prevent one). A collision here was
     * confirmed on real hardware: gc.collect() alone (no flash access,
     * no explicit core1 interaction) visibly corrupted the DVI picture
     * ("snow") for its duration — consistent with q_tmds_valid/free's
     * spinlock being transiently held by an unrelated MicroPython
     * internal critical section during GC, blocking dvi_scanline_pump()
     * (running in core1's IRQ) from making progress. spin_lock_claim_
     * unused() instead reserves a spinlock from the dedicated-use range
     * (24-31), which nothing else in the firmware can also be handed. */
    dvi0.timing = &dvi_timing_640x480p_60hz;
    dvi0.ser_cfg = pizero_dvi_cfg;
    dvi0.scanline_callback = dvi_scanline_pump;
    dvi_init(&dvi0, (uint)spin_lock_claim_unused(true), (uint)spin_lock_claim_unused(true));

    /* Mandatory startup pre-fill — see this file's header comment
     * ("startup pre-fill is mandatory"). Must happen before
     * multicore_launch_core1()/dvi_start(), i.e. before the first DMA
     * IRQ can possibly fire. DVI_N_TMDS_BUFFERS (overridden to 8 above)
     * is the total buffer count available at all — filling every one of
     * them up front also gives dvi_scanline_pump() the largest possible
     * runtime margin against transient stalls (see this file's top
     * comment on DVI_N_TMDS_BUFFERS). */
    for (int i = 0; i < DVI_N_TMDS_BUFFERS; i++) {
        dvi_scanline_pump();
    }

    multicore_launch_core1(core1_dvi_main);

    dvi_ready = true;
    msx->display_ready = true;  /* reused as the generic "a display backend is up" flag */
    return true;
}

/* -----------------------------------------------------------------------
 * msx_render_to_display_dvi — kept as a stable no-op API surface; see
 * this file's header comment ("No persistent DVI framebuffer") for why
 * there is no longer any per-frame compose step to do here.
 * ----------------------------------------------------------------------- */
void msx_render_to_display_dvi(msx_state_t *msx) {
    (void)msx;
}

/* -----------------------------------------------------------------------
 * msx_dvi_debug_info — 2026-09-26 real-hardware bring-up diagnostic aid.
 * Lets Python peek at core1's health mid-runtime without needing another
 * blind fix-and-reflash cycle: call twice a second or so apart and compare
 * *heartbeat — if it has stopped advancing, core1's DMA IRQ has gone
 * completely silent (crashed/stuck, e.g. still executing a lockout wait,
 * or a hard fault); if it's still advancing at roughly the expected rate
 * (~1 count per unique scanline, DVI_VERTICAL_REPEAT-gated) but the
 * picture is still wrong, the problem is data content (garbage/stale
 * framebuf, or the late_scanline_ctr trap — see *late_ctr) rather than a
 * dead core1.
 * ----------------------------------------------------------------------- */
void msx_dvi_debug_info(uint32_t *heartbeat, uint32_t *late_ctr) {
    *heartbeat = dvi_heartbeat;
    *late_ctr = dvi0.late_scanline_ctr;
}

#endif /* MSX_BOARD_PIZERO */
