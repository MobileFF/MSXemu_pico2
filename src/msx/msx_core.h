/*
 * MSX1 Emulator Core - Header
 * For Raspberry Pi Pico 2 (RP2350) + MicroPython
 *
 * CPU:  Z80 (superzazu/z80, MIT)
 * VDP:  TMS9918A (visrealm/vrEmuTms9918, MIT)
 * PSG:  AY-3-8910 (digital-sound-antiques/emu2149, MIT)
 * PPI:  i8255 (simplified)
 */
#ifndef MSX_CORE_H
#define MSX_CORE_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "z80/z80.h"
#include "tms9918/vrEmuTms9918.h"
#include "emu2149/emu2149.h"

/* -----------------------------------------------------------------------
 * Timing constants (NTSC)
 * ----------------------------------------------------------------------- */
#define MSX_CLOCK_HZ        3579545u
#define MSX_FPS             60u
#define MSX_LINES_TOTAL     262u
#define MSX_LINES_ACTIVE    192u
/* T-states per frame: 3579545 / 60 = 59659 */
#define MSX_TCYCLES_FRAME   (MSX_CLOCK_HZ / MSX_FPS)
/* T-states per scanline: 59659 / 262 = 227 (rounded down) */
#define MSX_TCYCLES_LINE    (MSX_TCYCLES_FRAME / MSX_LINES_TOTAL)

/* -----------------------------------------------------------------------
 * Display
 * ----------------------------------------------------------------------- */
#define MSX_SCREEN_W        256
#define MSX_SCREEN_H        192

/* ST7796 module (MSP4021) is 480x320 (landscape). Production uses
 * msx_render_to_display_1to1() (native 256x192, centered, no scaling —
 * scaling nearly doubled frame time and made gameplay feel like slow
 * motion, see msx_main.py); the scaled path below fills the panel
 * exactly at 3/2 (1.5x) with zero borders if ever re-enabled, using
 * nearest-neighbor sampling done per output row. */
#define LCD_W               480
#define LCD_H               320
#define MSX_DISP_SCALE_NUM  3
#define MSX_DISP_SCALE_DEN  2
#define MSX_DISP_W          (MSX_SCREEN_W * MSX_DISP_SCALE_NUM / MSX_DISP_SCALE_DEN)  /* 384 */
#define MSX_DISP_H          (MSX_SCREEN_H * MSX_DISP_SCALE_NUM / MSX_DISP_SCALE_DEN)  /* 288 */
#define MSX_DISP_X_OFFSET   ((LCD_W - MSX_DISP_W) / 2)   /* 48 */
#define MSX_DISP_Y_OFFSET   ((LCD_H - MSX_DISP_H) / 2)   /* 16 */

/* Rows are batched per DMA transfer to amortize per-transfer setup
 * overhead (288 single-row transfers measured far slower than the
 * theoretical SPI transfer time). Kept small: the scaled path isn't the
 * active production render (msx_render_to_display_1to1() is), and its
 * double buffer competes with the framebuf double-buffer for scarce
 * MicroPython GC heap — a large batch here caused MemoryError elsewhere. */
#define MSX_DISP_ROW_BATCH  4

/* -----------------------------------------------------------------------
 * Memory layout
 * ----------------------------------------------------------------------- */
#define MSX_BIOS_SIZE       0x8000u   /* 32KB BIOS/BASIC in slot 0 */
#define MSX_RAM_SIZE        0x10000u  /* 64KB RAM in slot 3 */
#define MSX_CART_MAX_SIZE   0x80000u  /* 512KB max cart ROM */
#define MSX_VRAM_SIZE       0x4000u   /* 16KB TMS9918A VRAM (vrEmuTms9918's own buffer) */

/* -----------------------------------------------------------------------
 * Cart ROM mapper types
 * ----------------------------------------------------------------------- */
#define MSX_MAPPER_PLAIN    0   /* linear, no banking (≤48KB) */
#define MSX_MAPPER_ASCII8   1   /* 8KB pages, selectors at 0x6000/0x6800/0x7000/0x7800 */
#define MSX_MAPPER_ASCII16  2   /* 16KB pages, selectors at 0x6000/0x7000 */
#define MSX_MAPPER_KONAMI   3   /* 8KB pages, selectors written to 0x6000/0x8000/0xA000 */

/* -----------------------------------------------------------------------
 * Mega ROM (SD-backed, lazily-paged) cart support
 *
 * msx_load_cart() copies the whole ROM into a heap buffer — fine for
 * small carts, but Mega ROMs (128KB-1MB, ASCII-8/ASCII-16/KONAMI) don't
 * fit in this board's RAM budget. msx_load_cart_paged() instead keeps
 * only the 4 currently bank-switched-in 8KB windows resident (32KB
 * total, regardless of the ROM's real size) and fetches a fresh page
 * on demand — via a caller-registered callback — whenever a mapper
 * write bank-switches a window to a page not already cached. In
 * practice the callback reads from an open file on SD.
 * ----------------------------------------------------------------------- */
#define MSX_CART_PAGE_SIZE  0x2000u   /* 8KB cache granularity (4 windows/slot) */

/* byte_offset/dest are always MSX_CART_PAGE_SIZE-aligned/sized. Return
 * true on success (dest fully filled); false leaves dest untouched
 * (stale cache content persists — a visible/audible glitch, not a
 * crash) and the fetch will be retried next time this bank is selected. */
typedef bool (*msx_cart_fetch_fn)(void *userdata, uint8_t slot,
                                    uint32_t byte_offset, uint8_t *dest);

/* -----------------------------------------------------------------------
 * Keyboard
 * ----------------------------------------------------------------------- */
#define MSX_KEY_ROWS        11

/* Row indices (matches standard MSX keyboard matrix) */
#define MSX_KEY_ROW_DIGIT   0   /* 0-7: digit row */
#define MSX_KEY_ROW_MISC    1   /* ; ] [ \ = - 9 8 */
#define MSX_KEY_ROW_ALPHA1  2   /* B A ~ ` / . , M */
#define MSX_KEY_ROW_ALPHA2  3   /* J I H G F E D C */
#define MSX_KEY_ROW_ALPHA3  4   /* R Q P O N M L K */
#define MSX_KEY_ROW_ALPHA4  5   /* Z Y X W V U T S */
#define MSX_KEY_ROW_CTRL    6   /* F3 F2 F1 CODE CAPS GRAPH CTRL SHIFT */
#define MSX_KEY_ROW_FUNC    7   /* RET SEL BS STOP TAB ESC F5 F4 */
#define MSX_KEY_ROW_ARROW   8   /* RIGHT DOWN UP LEFT DEL INS HOME SPACE */
#define MSX_KEY_ROW_NUM1    9   /* NUM4 NUM3 NUM2 NUM1 NUM0 / + * */
#define MSX_KEY_ROW_NUM2    10  /* . , - 9 8 7 6 5 (numpad) */

/* -----------------------------------------------------------------------
 * Audio
 * ----------------------------------------------------------------------- */
#define MSX_AUDIO_RATE      22050u
/* samples per frame = 22050 / 60 = 367; buffer with margin */
#define MSX_AUDIO_BUF_SIZE  512u

/* -----------------------------------------------------------------------
 * MSX system state
 * ----------------------------------------------------------------------- */
typedef struct {
    /* Z80 CPU */
    z80  cpu;

    /* VDP: TMS9918A */
    VrEmuTms9918 *vdp;

    /* PSG: AY-3-8910 */
    PSG *psg;

    /* Memory */
    uint8_t  bios[MSX_BIOS_SIZE];        /* slot 0 (32KB) */
    uint8_t  ram[MSX_RAM_SIZE];          /* slot 3 (64KB) */
    uint8_t *cart[2];                    /* slot 1, slot 2 (heap-allocated, full-ROM mode) */
    uint32_t cart_size[2];
    uint8_t  cart_type[2];               /* MSX_MAPPER_* */
    uint8_t  cart_bank[2][4];            /* active 8KB-page per window (ASCII8/Konami) */

    /* Mega ROM (SD-backed, lazily-paged) mode — see MSX_CART_PAGE_SIZE
     * above. cart_size[]/cart_type[]/cart_bank[] above are shared with
     * the full-ROM path; these are additional. */
    bool      cart_paged[2];             /* true = paged mode (cart[]/malloc unused) */
    uint8_t  *cart_cache[2];             /* 4 * MSX_CART_PAGE_SIZE, malloc'd when paged */
    int32_t   cart_cache_page[2][4];     /* ROM page (8KB units) resident per window, -1=none */
    msx_cart_fetch_fn cart_fetch_cb;     /* set once via msx_set_cart_fetch_cb() */
    void     *cart_fetch_userdata;

    /* I/O chip state */
    uint8_t  slot_select;                /* PPI port A: slot select per 16KB page */
    uint8_t  key_matrix[MSX_KEY_ROWS];  /* keyboard rows, active-low */
    uint8_t  key_row;                    /* currently selected matrix row (PPI port C[3:0]) */
    uint8_t  ppi_c;                      /* PPI port C mirror */
    uint8_t  psg_reg;                    /* PSG selected register index */

    /* Framebuffer: RGB565, 256 × 192 (stored big-endian for ILI9341 DMA).
     * Double-buffered so a display render (DMA) can run concurrently with
     * the next msx_run_frame() call. msx_run_frame() always writes into
     * the buffer NOT indexed by framebuf_ready_idx, then flips the index
     * at the end; the display/render functions always read
     * framebuf[framebuf_ready_idx] (the most recently completed frame). */
    uint16_t framebuf[2][MSX_SCREEN_W * MSX_SCREEN_H];
    uint8_t  framebuf_ready_idx;

    /* Pre-computed TMS9918 palette → RGB565 (big-endian) */
    uint16_t palette565[16];

    /* Per-scanline pixel row buffer (reused each scanline) */
    uint8_t  scanline_buf[MSX_SCREEN_W];

    /* Scaled (1.5x) output staging buffers, MSX_DISP_ROW_BATCH rows each —
     * double-buffered so the CPU can build the next batch while DMA sends
     * the current one. */
    uint16_t disp_row_buf[2][MSX_DISP_ROW_BATCH * MSX_DISP_W];

    /* Precomputed output-column -> source-column mapping for display
     * scaling (avoids a division per pixel in the hot render path). */
    uint16_t disp_col_src[MSX_DISP_W];

    /* Timing */
    uint32_t frame_tcycles;              /* T-states elapsed in current frame */
    uint8_t  current_line;              /* scanline counter (0–261) */

    /* Audio sample ring buffer (int16_t, one channel) */
    int16_t  audio_buf[MSX_AUDIO_BUF_SIZE];
    uint32_t audio_write_pos;           /* producer index */
    uint32_t audio_samples_per_frame;   /* pre-computed: MSX_AUDIO_RATE / MSX_FPS */

    /* SPI display hardware (set by msx_setup_display / msx_init_display_hardware) */
    void    *spi_inst;
    uint8_t  spi_cs_pin;
    uint8_t  spi_dc_pin;
    uint8_t  spi_rst_pin;
    uint8_t  spi_bl_pin;
    uint32_t spi_baudrate;
    uint32_t spi_actual_baudrate;   /* actual Hz reported by spi_set_baudrate() */
    bool     display_ready;

    /* Actual panel size in use (set by msx_init_display_hardware(); e.g.
     * 480x320 for the ST7796/MSP4021 or 320x240 for the ILI9341/MSP2402).
     * msx_render_to_display_1to1() centers the native 256x192 image
     * within this at runtime — LCD_W/LCD_H (msx_core.h) are only the
     * compile-time default/max, used to size the black-screen-clear
     * buffer and the (currently unused) scaled display path. */
    uint16_t lcd_w, lcd_h;
    bool     rotate_180;    /* MADCTL variant: false=0x28, true=0xE8 */

    /* HDMI bridge output (set by msx_init_hdmi_output()) — sends the native
     * 256x192 framebuf, converted to RGB332, over the same SPI1 bus as the
     * LCD/SD (shared MOSI/SCK) to a dedicated CS pin, for a second Pico 2 +
     * PICO-HDMI-PLUS running the shared ../../../hdmi_bridge_receiver
     * firmware (sibling project — see its README.md; originally developed
     * here as hdmi_bridge/phase2_receiver, later split out since the
     * protocol became fully self-describing and receiver-side generic).
     * See also hdmi_bridge/README.md for this project's own HDMI addon
     * background/history. Disabled (hdmi_ready=false) unless explicitly
     * initialized — must not affect users without the HDMI addon. */
    uint8_t  hdmi_cs_pin;
    uint32_t hdmi_baudrate;
    bool     hdmi_ready;

    /* Joystick (PSG I/O Port A/B, register 14/15): joy_state[0]=JOY1,
     * [1]=JOY2, active-low bitmask (bit0 Up,1 Down,2 Left,3 Right,
     * 4 TriggerA,5 TriggerB, bits 6-7 unused=1). joy_select tracks the
     * port last selected via a register-15 write (bit 6). */
    uint8_t  joy_state[2];
    uint8_t  joy_select;

    /* Flags */
    bool     bios_loaded;
    bool     initialized;
} msx_state_t;

/* -----------------------------------------------------------------------
 * Save-state layout (binary blob, ~64 KB)
 *
 * Captures: Z80 registers, RAM, mapper bank registers, I/O state, VDP regs.
 * VRAM is NOT saved (programs redraw every frame; this keeps the blob small).
 * ----------------------------------------------------------------------- */
#define MSX_SAVE_MAGIC   "MSX1SAV1"   /* 8 bytes, last byte = version */
#define MSX_SAVE_HDR_SZ  64u          /* fixed header, padded to 64 bytes */

typedef struct __attribute__((packed)) {
    char     magic[8];          /* "MSX1SAV1"                           */
    /* Z80 main registers */
    uint16_t pc, sp, ix, iy;
    uint8_t  a,  f;             /* f = packed flags (S Z Y H X P N C)  */
    uint8_t  b,  c, d, e, h, l;
    /* Z80 alternate registers */
    uint8_t  a_, f_, b_, c_, d_, e_, h_, l_;
    /* Z80 special */
    uint8_t  i,  r;
    uint8_t  iff1, iff2;
    uint8_t  int_mode;
    uint8_t  halted;
    /* I/O */
    uint8_t  slot_select;
    uint8_t  psg_reg;
    uint8_t  key_row;
    uint8_t  ppi_c;
    /* VDP registers (R0–R7) */
    uint8_t  vdp_regs[8];
    /* Mapper bank registers: [slot 0..1][page 0..3] */
    uint8_t  cart_bank[2][4];
    /* Reserved / padding to 80 bytes */
    uint8_t  _pad[6];
    /* RAM image (64 KB) */
    uint8_t  ram[MSX_RAM_SIZE];
} msx_save_t;

/* -----------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------- */

/* Initialize MSX state (does not start emulation). */
void msx_init(msx_state_t *msx);

/* Load 32KB MSX BIOS/BASIC ROM into slot 0. */
bool msx_load_bios(msx_state_t *msx, const uint8_t *data, uint32_t size);

/* Direct pointer to the 32KB BIOS buffer (msx->bios) plus a matching
 * "mark loaded" call, for zero-copy loading (e.g. reading straight from
 * SD into this buffer instead of a separate ~32KB scratch buffer) — see
 * msx_mark_bios_loaded()'s comment in msx_core.c. Always valid once
 * msx_init() has run. */
uint8_t *msx_get_bios_ptr(msx_state_t *msx);
bool msx_mark_bios_loaded(msx_state_t *msx, uint32_t size);

/* Load cartridge ROM into slot (0=slot1, 1=slot2).
 * data must remain valid for the lifetime of the emulator.
 * mapper: MSX_MAPPER_* (use MSX_MAPPER_PLAIN for auto-detect). */
bool msx_load_cart(msx_state_t *msx, uint8_t slot, const uint8_t *data,
                   uint32_t size, uint8_t mapper);

/* Two-phase, zero-copy alternative to msx_load_cart() — see
 * msx_cart_finalize()'s comment in msx_core.c. msx_cart_alloc() mallocs
 * msx->cart[slot] and returns it (NULL on failure) so the caller can
 * write the ROM directly into that memory; msx_cart_finalize() then does
 * the mapper-detect/bank-init work msx_load_cart() normally does right
 * after its memcpy. */
uint8_t *msx_cart_alloc(msx_state_t *msx, uint8_t slot, uint32_t size);
bool msx_cart_finalize(msx_state_t *msx, uint8_t slot, uint8_t mapper);

/* Register the callback used to fetch an 8KB page from backing storage
 * for paged/Mega-ROM carts (see msx_load_cart_paged()). One callback
 * serves both cart slots; it receives the slot number on each call.
 * Call once at startup (idempotent — safe to call again after msx_init()
 * re-zeroes the state, since that wipes this pointer too). */
void msx_set_cart_fetch_cb(msx_state_t *msx, msx_cart_fetch_fn cb, void *userdata);

/* Cumulative counts since boot, for measuring megarom bank-switch/cache-miss
 * frequency on real hardware: bankswitch_count is every bank-select
 * register write for a paged cart (hit or miss); fetch_count is the
 * subset that missed the cache and paid a synchronous backing-storage
 * read. Either output pointer may be NULL to skip it. */
void msx_get_cart_fetch_stats(msx_state_t *msx, uint32_t *bankswitch_count,
                               uint32_t *fetch_count);

/* Load a cartridge in "paged" mode: the ROM stays in backing storage and
 * only the 4 currently bank-switched-in 8KB windows are kept resident
 * (32KB total, regardless of the ROM's real size) — see the Mega ROM
 * comment block above. Requires msx_set_cart_fetch_cb() to have been
 * called first. mapper must be an explicit MSX_MAPPER_* (ASCII-8/
 * ASCII-16/KONAMI — MSX_MAPPER_PLAIN is rejected, paging only makes
 * sense for bank-switched carts). Use msx_detect_mapper() beforehand to
 * choose it from a prefix of the ROM without loading the whole thing. */
bool msx_load_cart_paged(msx_state_t *msx, uint8_t slot, uint32_t total_size,
                          uint8_t mapper);

/* Run the same mapper-detection heuristic msx_load_cart() uses
 * internally against a caller-supplied buffer (e.g. just the first 64KB
 * of a large ROM) — lets the caller pick a mapper for
 * msx_load_cart_paged() without needing the whole ROM in RAM. */
uint8_t msx_detect_mapper(const uint8_t *data, uint32_t size);

/* Eject cartridge. */
void msx_eject_cart(msx_state_t *msx, uint8_t slot);

/* Hard reset (re-runs BIOS boot sequence). */
void msx_reset(msx_state_t *msx);

/* Reconfigure clk_peri to track clk_sys directly (bypassing its divider).
 * clk_peri feeds the SPI/UART baud-rate dividers; on this board it defaults
 * to a fixed 48MHz regardless of machine.freq(), silently capping SPI baud
 * well below what's requested. Call once at startup, then re-create any
 * machine.UART objects afterward so their baud divisor is recalculated
 * against the new clk_peri. */
void msx_boost_peri_clock(void);

/* Give DMA (read+write) top priority on the bus fabric over the
 * processors. Measured fix for: msx.render_to_display_1to1() +
 * msx.wait_display() dropping from ~15ms to ~39ms per frame purely from
 * calling usb_host.init() (USB host mode contends with our SPI DMA
 * channel for bus arbitration; unrelated to audio or cart content —
 * verified empirically). Call once at startup, any time. */
void msx_boost_dma_priority(void);

/* Set joystick port state. port: 0=JOY1, 1=JOY2. state: active-low
 * bitmask — bit0 Up, bit1 Down, bit2 Left, bit3 Right, bit4 TriggerA,
 * bit5 TriggerB (0=pressed, 1=released); bits 6-7 ignored (forced to 1).
 * Call once per frame (or on change) from the Python GPIO poll. */
void msx_set_joystick(msx_state_t *msx, uint8_t port, uint8_t state);

/* Full ILI9341/ST7796 hardware initialization: configures GPIO, SPI,
 * reset sequence, and sends the init command sequence (identical for
 * both panels — same MADCTL etc.). Call once at boot.
 * spi_id: 0=spi0, 1=spi1. baudrate: SPI clock in Hz (e.g. 62500000).
 * mosi_pin, sck_pin: SPI function GPIO pins.
 * cs_pin, dc_pin, rst_pin, bl_pin: control GPIO pins.
 * lcd_w, lcd_h: panel size — 480,320 for ST7796 (MSP4021) or 320,240 for
 * ILI9341 (MSP2402). msx_render_to_display_1to1() centers the native
 * 256x192 image within this at runtime.
 * rotate_180: false=normal orientation (MADCTL 0x28), true=180°-flipped
 * (MADCTL 0xE8, i.e. MX/MY bits inverted vs 0x28 — same MV/BGR bits, so
 * the panel stays in landscape and the CASET/PASET window math in
 * msx_render_to_display_1to1() is unaffected). */
void msx_init_display_hardware(msx_state_t *msx,
                                uint8_t spi_id, uint32_t baudrate,
                                uint8_t mosi_pin, uint8_t sck_pin,
                                uint8_t cs_pin,  uint8_t dc_pin,
                                uint8_t rst_pin, uint8_t bl_pin,
                                uint16_t lcd_w, uint16_t lcd_h,
                                bool rotate_180);

/* Store SPI config for use by msx_render_to_display (when SPI is
 * already initialized by Python display driver). */
void msx_setup_display(msx_state_t *msx, void *spi_inst,
                       uint8_t cs_pin, uint8_t dc_pin);

/* Execute one full video frame (~59659 T-states).
 * Renders 192 VDP scanlines into framebuf and generates audio samples.
 * Returns the number of audio samples written to audio_buf this frame. */
uint32_t msx_run_frame(msx_state_t *msx);

/* Start DMA transfer of framebuf → ST7796 (non-blocking).
 * Call msx_wait_display() before the next call to msx_render_to_display(). */
void msx_render_to_display(msx_state_t *msx);

/* Same as msx_render_to_display(), but sends the native 256x192 framebuf
 * 1:1 (no 1.5x scaling), centered on the panel, in a single DMA transfer.
 * Sends ~2.25x less data — for comparing perceived speed/FPS against the
 * scaled path. Also non-blocking; call msx_wait_display() afterward. */
void msx_render_to_display_1to1(msx_state_t *msx);

/* Wait for the DMA display transfer to complete (blocking). */
void msx_wait_display(msx_state_t *msx);

/* Configure the HDMI bridge output (see hdmi_bridge/README.md). Reuses the
 * same SPI instance already set up for the LCD/SD (msx->spi_inst) — must
 * be called AFTER msx_init_display_hardware(). cs_pin is a new, dedicated
 * chip-select GPIO (GP28) so the bus can be shared without touching the
 * existing LCD/SD wiring. Does nothing (hdmi_ready stays false) unless
 * called — safe no-op for users without the addon. Also sends the current
 * 16-color palette (see msx_send_hdmi_palette()) once, since MSX1's
 * palette is fixed and this is the natural one-time setup point. */
void msx_init_hdmi_output(msx_state_t *msx, uint8_t cs_pin, uint32_t baudrate);

/* Send the current 16-color palette (msx->palette565, converted to RGB332)
 * to the HDMI bridge as a PKT_PALETTE packet (blocking). Already called
 * once by msx_init_hdmi_output(); only needs to be called again manually
 * if the palette were ever to change at runtime (it doesn't on MSX1
 * hardware, but the wire protocol supports it). No-op if
 * msx_init_hdmi_output() was never called. */
void msx_send_hdmi_palette(msx_state_t *msx);

/* Convert framebuf (RGB565) to 4-bit palette indices (2 pixels/byte) and
 * send it to the HDMI bridge Pico 2 over SPI1 as a PKT_FRAME packet
 * (bpp=4, blocking) — half the bytes of sending RGB332 directly, since
 * MSX1 only ever uses 16 colors (see msx_send_hdmi_palette()). Must only
 * be called when the LCD/SD SPI1 bus is idle — i.e. after
 * msx_wait_display() and not concurrently with SD card access — to avoid
 * the bus contention that previously crashed megarom bank-switch loads
 * sharing this same bus. No-op if msx_init_hdmi_output() was never
 * called. Only correct for the actual MSX game screen (guaranteed to use
 * only the 16 palette colors) — for the menu/UI screens, which draw
 * arbitrary colors into the same framebuf, use
 * msx_render_to_hdmi_raw332() instead.
 *
 * The packet header self-describes width/height/bpp/scale (see
 * ../../../hdmi_bridge_receiver/main.c's protocol comment for the
 * shared spec), so the same receiver firmware also serves the PB-1000
 * emulator's sender without reflashing. */
void msx_render_to_hdmi(msx_state_t *msx);

/* Same as msx_render_to_hdmi(), but sends full RGB332 (1 byte/pixel, no
 * palette lookup) as a PKT_FRAME packet with bpp=8. Use this for the
 * menu/UI screens (msx_menu.py's MenuCanvas), which draw arbitrary colors
 * outside the MSX's 16-color hardware palette directly into the same
 * framebuf — msx_render_to_hdmi()'s palette reverse-lookup would render
 * any non-matching color as black. Twice the bytes of msx_render_to_hdmi(),
 * but menu screens are drawn far less often than game frames so this
 * doesn't matter for overall performance. */
void msx_render_to_hdmi_raw332(msx_state_t *msx);

/* Sends one all-black PKT_FRAME (RAW332) to the HDMI receiver, independent
 * of the current framebuf content. Call once right after
 * msx_init_hdmi_output() to blank whatever frame the receiver was
 * previously displaying (e.g. left over from a different emulator/session)
 * before this emulator's own first real frame is sent. No-op if
 * msx_init_hdmi_output() was never called. */
void msx_clear_hdmi(msx_state_t *msx);

/* Set a keyboard matrix row value (active-low: 0 = key pressed).
 * row: 0–10, col_mask: bit per column (bit 0 = col 0). */
void msx_set_key_matrix(msx_state_t *msx, uint8_t row, uint8_t col_mask);

/* Clear all keys (release everything). */
void msx_clear_keys(msx_state_t *msx);

/* Free heap resources (cart ROM buffers, VDP, PSG). */
void msx_destroy(msx_state_t *msx);

/* Direct pointer to the VDP's 16KB VRAM buffer (MSX_VRAM_SIZE), for
 * zero-copy save-state read/write. NULL if the VDP isn't initialized. */
uint8_t *msx_get_vram_ptr(msx_state_t *msx);

/* Save/restore emulator state.
 * buf must be at least sizeof(msx_save_t) bytes.
 * msx_save_state_size() returns that exact size. */
size_t msx_save_state_size(void);
bool   msx_save_state(const msx_state_t *msx, void *buf, size_t buf_size);
bool   msx_load_state(msx_state_t *msx, const void *buf, size_t buf_size);

/* Header-only (everything but the 64KB ram[]) variants — MSX_SAVE_HDR_SZ
 * (64) bytes, small enough to never risk a GC/heap allocation failure.
 * Pair with a direct, zero-copy view of msx->ram for the RAM portion
 * (see modmsx.c's msx.get_ram_view()) instead of a second ~64KB buffer. */
size_t msx_save_state_header_size(void);
bool   msx_save_state_header(const msx_state_t *msx, void *buf, size_t buf_size);
bool   msx_load_state_header(msx_state_t *msx, const void *buf, size_t buf_size);

/* -----------------------------------------------------------------------
 * Internal helpers (used by modmsx.c)
 * ----------------------------------------------------------------------- */
uint8_t msx_mem_read(void *userdata, uint16_t addr);
void    msx_mem_write(void *userdata, uint16_t addr, uint8_t data);
uint8_t msx_port_read(z80 *cpu, uint8_t port);
void    msx_port_write(z80 *cpu, uint8_t port, uint8_t data);

/* -----------------------------------------------------------------------
 * Debug helpers (bisecting hangs / desyncs between host and target builds)
 * ----------------------------------------------------------------------- */
/* Execute exactly n raw Z80 instructions (no VDP/audio/timing). Returns the
 * PC after the last instruction executed. */
uint16_t msx_debug_step(msx_state_t *msx, uint32_t n);

/* Snapshot of CPU state for debug printing from Python. */
typedef struct {
    uint16_t pc, sp;
    uint8_t  a, f;
    uint32_t cyc;
    uint8_t  halted;
    uint8_t  iff1;
    uint8_t  int_mode;
} msx_debug_cpu_t;
void msx_debug_get_cpu(const msx_state_t *msx, msx_debug_cpu_t *out);

/* Read one byte of Z80 address space (through the current slot mapping). */
uint8_t msx_debug_peek(msx_state_t *msx, uint16_t addr);

/* Actual SPI baud rate (Hz) last achieved by msx_render_to_display(),
 * as reported by the hardware (may differ from the requested rate due to
 * clock-divider granularity). 0 if never rendered. */
uint32_t msx_debug_get_spi_baud(const msx_state_t *msx);

/* clk_sys and clk_peri frequencies (Hz) as currently configured.
 * clk_peri feeds the SPI baud-rate divider — if it's lower than expected,
 * the requested SPI baud can silently be clamped much lower. */
void msx_debug_get_clocks(uint32_t *clk_sys_hz, uint32_t *clk_peri_hz);

/* Run exactly one scanline's worth of T-states (like msx_run_frame's inner
 * loop for one line), with video/audio/interrupt sub-steps individually
 * toggleable, to bisect hangs between subsystems.
 * line: 0-261 (matches MSX_LINES_TOTAL); used for video active-line test
 * and interrupt-line check, but does NOT persist any frame state. */
void msx_debug_run_line(msx_state_t *msx, uint32_t line,
                         bool do_video, bool do_audio, bool do_int);

/* PSG internal state snapshot, for bisecting audio-path hangs. */
typedef struct {
    uint8_t  quality;
    uint32_t clk, rate, base_incr;
    uint32_t realstep, psgtime, psgstep, freq_limit;
    uint8_t  env_ptr, env_pause, env_continue, env_face;
    uint16_t env_freq;
    uint32_t env_count;
    uint16_t freq0, count0;
} msx_debug_psg_t;
void msx_debug_get_psg(const msx_state_t *msx, msx_debug_psg_t *out);

/* Call PSG_calc() exactly n times in a row; returns the last sample.
 * Used to bisect exactly which call hangs. */
int16_t msx_debug_psg_calc_n(msx_state_t *msx, uint32_t n);

#endif /* MSX_CORE_H */
