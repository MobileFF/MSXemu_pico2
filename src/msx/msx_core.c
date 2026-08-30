/*
 * MSX1 Emulator Core - Implementation
 *
 * Hardware emulated:
 *   CPU : Z80 @ 3.579545 MHz
 *   VDP : TMS9918A  (vrEmuTms9918, MIT)
 *   PSG : AY-3-8910 (emu2149, MIT)
 *   PPI : i8255 (slot select, keyboard matrix)
 *   ROM : slot 0 (BIOS), slot 1/2 (cartridge, optional mapper)
 *   RAM : slot 3, 64KB
 */

#include "msx_core.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Pico SDK headers — only used when display support is compiled in */
#ifdef __arm__
#include "hardware/spi.h"
#include "hardware/gpio.h"
#include "hardware/dma.h"
#include "hardware/clocks.h"
#include "hardware/structs/bus_ctrl.h"
#include "pico/time.h"
#define HAVE_PICO_SDK 1
#else
#define HAVE_PICO_SDK 0
#endif

/* -----------------------------------------------------------------------
 * TMS9918 color palette → RGB565 (big-endian for ILI9341)
 * RGB888 source values from the TMS9918 datasheet / de-facto standard.
 * ----------------------------------------------------------------------- */
static const uint32_t tms_palette_rgb888[16] = {
    0x000000, /* 0: transparent (rendered as background) */
    0x000000, /* 1: black */
    0x21C842, /* 2: medium green */
    0x5EDC78, /* 3: light green */
    0x5455ED, /* 4: dark blue */
    0x7D76FC, /* 5: light blue */
    0xD4524D, /* 6: dark red */
    0x42EBF5, /* 7: cyan */
    0xFC5554, /* 8: medium red */
    0xFF7978, /* 9: light red */
    0xD4C154, /* 10: dark yellow */
    0xE6CE80, /* 11: light yellow */
    0x21B03B, /* 12: dark green */
    0xC95BBB, /* 13: magenta */
    0xCCCCCC, /* 14: gray */
    0xFFFFFF, /* 15: white */
};

/* Convert RGB888 → RGB565 big-endian (byte-swapped) as ILI9341 expects. */
static inline uint16_t rgb888_to_565be(uint32_t rgb888) {
    uint8_t r = (rgb888 >> 16) & 0xFF;
    uint8_t g = (rgb888 >>  8) & 0xFF;
    uint8_t b =  rgb888        & 0xFF;
    uint16_t v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
    /* byte-swap for SPI big-endian transfer */
    return (uint16_t)((v >> 8) | (v << 8));
}

/* -----------------------------------------------------------------------
 * Cart mapper helpers
 * ----------------------------------------------------------------------- */

/* Scan for known bank-select write patterns (LD (nn),A into the various
 * mapper register address ranges). No size-based shortcut here — callers
 * decide whether/when scanning is worthwhile (see detect_mapper() vs
 * msx_detect_mapper() below). */
static uint8_t scan_mapper_patterns(const uint8_t *data, uint32_t size) {
    uint32_t ascii8_hits = 0, ascii16_hits = 0, konami_hits = 0;
    for (uint32_t i = 0; i + 3 < size; i++) {
        /* LD (nn),A  opcode = 0x32 followed by 16-bit address */
        if (data[i] == 0x32) {
            uint16_t addr = (uint16_t)(data[i+1] | (data[i+2] << 8));
            if (addr >= 0x6000 && addr <= 0x7FFF) {
                if      (addr >= 0x7800) ascii8_hits++;
                else if (addr >= 0x7000) { ascii8_hits++; ascii16_hits++; }
                else if (addr >= 0x6800) ascii8_hits++;
                else                     { ascii8_hits++; ascii16_hits++; }
            }
            if (addr == 0x6000 || addr == 0x8000 || addr == 0xA000) {
                konami_hits++;
            }
        }
    }

    if (konami_hits >= ascii8_hits && konami_hits > 0) return MSX_MAPPER_KONAMI;
    if (ascii16_hits > ascii8_hits / 2)                return MSX_MAPPER_ASCII16;
    if (ascii8_hits > 0)                               return MSX_MAPPER_ASCII8;
    return MSX_MAPPER_PLAIN;
}

/* Detect mapper type from a FULL ROM's size and content (used internally
 * by msx_load_cart()). Heuristic: scan for known bank-select write
 * patterns. ROMs ≤32KB can't be bank-switched at all (they fit in two
 * fixed 16KB pages), so skip scanning and return PLAIN outright. */
static uint8_t detect_mapper(const uint8_t *data, uint32_t size) {
    if (size <= 0x8000) {
        return MSX_MAPPER_PLAIN;  /* fits in two 16KB pages */
    }
    return scan_mapper_patterns(data, size);
}

/* Public: run the same heuristic against a caller-supplied buffer (e.g.
 * just a short prefix of a large ROM read for this purpose) — used by
 * msx_load_cart_paged() callers that don't have the whole ROM in RAM.
 * Deliberately has NO size-based PLAIN shortcut (unlike detect_mapper()
 * above): the caller already knows the ROM as a whole is large enough to
 * need paging, so a small prefix here is just an I/O-size tradeoff, not
 * evidence the ROM itself is unbanked. */
uint8_t msx_detect_mapper(const uint8_t *data, uint32_t size) {
    return scan_mapper_patterns(data, size);
}

/* Instrumentation for measuring megarom bank-switch/cache-miss frequency
 * on real hardware (see msx_get_cart_fetch_stats()) — bankswitch_count
 * counts every cart_page_refill() call (i.e. every bank-select register
 * write for a paged cart, hit or miss); fetch_count counts only the
 * subset that actually missed the cache and paid the synchronous
 * flash-file read in cart_fetch_from_pyfile() (modmsx.c). The gap between
 * the two is the cache hit rate; fetch_count directly correlates with the
 * per-frame stalls responsible for megarom's lower FPS vs. plain ROMs. */
static volatile uint32_t cart_bankswitch_count = 0;
static volatile uint32_t cart_fetch_count = 0;

/* Refill one 8KB physical cache window with ROM page `rom_page` (in
 * MSX_CART_PAGE_SIZE units), via the registered fetch callback. No-op if
 * that page is already resident. On fetch failure the stale cache
 * content is left in place (a visible/audible glitch, not a crash) and
 * cart_cache_page is left unchanged so the fetch is retried next time
 * this bank is selected. */
static void cart_page_refill(msx_state_t *msx, uint8_t slot_idx, uint8_t win,
                              uint32_t rom_page) {
    cart_bankswitch_count++;
    if (msx->cart_cache_page[slot_idx][win] == (int32_t)rom_page) return;
    cart_fetch_count++;

    uint32_t total_pages = msx->cart_size[slot_idx] / MSX_CART_PAGE_SIZE;
    if (total_pages == 0) total_pages = 1;
    uint32_t safe_page   = rom_page % total_pages;
    uint32_t byte_offset = safe_page * MSX_CART_PAGE_SIZE;
    uint8_t *dest = &msx->cart_cache[slot_idx][(uint32_t)win * MSX_CART_PAGE_SIZE];

    if (msx->cart_fetch_cb &&
        msx->cart_fetch_cb(msx->cart_fetch_userdata, slot_idx, byte_offset, dest)) {
        msx->cart_cache_page[slot_idx][win] = (int32_t)rom_page;
    }
}

/* Return pointer to the effective 8KB page for a cart slot address.
 * addr: 0x0000–0xFFFF (full Z80 address space). */
static const uint8_t *cart_page_ptr(msx_state_t *msx, uint8_t slot_idx,
                                     uint16_t addr) {
    if (msx->cart_size[slot_idx] == 0) return NULL;

    if (msx->cart_paged[slot_idx]) {
        /* Mega ROM mode: cache is always 4 uniform 8KB windows regardless
         * of mapper type (ASCII16's 16KB windows are just two adjacent
         * 8KB cache slots — see cart_mapper_write()'s paged branch). */
        if (!msx->cart_cache[slot_idx]) return NULL;
        if (addr < 0x4000 || addr >= 0xC000) return NULL;
        uint8_t win = (uint8_t)((addr - 0x4000) >> 13);  /* 0–3 */
        return &msx->cart_cache[slot_idx][(uint32_t)win * MSX_CART_PAGE_SIZE +
                                            (addr & 0x1FFF)];
    }

    if (!msx->cart[slot_idx]) return NULL;

    const uint8_t *rom   = msx->cart[slot_idx];
    uint32_t       size  = msx->cart_size[slot_idx];
    uint8_t        mtype = msx->cart_type[slot_idx];
    uint8_t       *bank  = msx->cart_bank[slot_idx];

    switch (mtype) {
    case MSX_MAPPER_PLAIN: {
        /* Linear mapping: slot starts at 0x4000 (page 1 of the slot).
         * ROM occupies 0x4000–0x4000+size-1.  Reads below or above → NULL. */
        if (addr < 0x4000) return NULL;
        uint32_t offset = addr - 0x4000;
        if (offset >= size) return NULL;
        return rom + offset;
    }
    case MSX_MAPPER_ASCII8: {
        /* Four 8KB windows: 0x4000–0x5FFF, 0x6000–0x7FFF,
         *                   0x8000–0x9FFF, 0xA000–0xBFFF */
        if (addr < 0x4000 || addr >= 0xC000) return NULL;
        uint8_t  win    = (addr - 0x4000) >> 13;  /* 0–3 */
        uint32_t offset = (uint32_t)bank[win] * 0x2000u + (addr & 0x1FFF);
        if (offset >= size) return NULL;
        return rom + offset;
    }
    case MSX_MAPPER_ASCII16: {
        /* Two 16KB windows: 0x4000–0x7FFF (bank[0]), 0x8000–0xBFFF (bank[1]) */
        if (addr < 0x4000 || addr >= 0xC000) return NULL;
        uint8_t  win    = (addr >= 0x8000) ? 1 : 0;
        uint32_t offset = (uint32_t)bank[win] * 0x4000u + (addr & 0x3FFF);
        if (offset >= size) return NULL;
        return rom + offset;
    }
    case MSX_MAPPER_KONAMI: {
        /* Four 8KB windows; page 0 fixed at 0x4000–0x5FFF.
         * Windows: 0x4000 (fixed=0), 0x6000 (bank[1]),
         *          0x8000 (bank[2]), 0xA000 (bank[3]) */
        if (addr < 0x4000 || addr >= 0xC000) return NULL;
        uint8_t win     = (addr - 0x4000) >> 13;  /* 0–3 */
        uint8_t pg      = (win == 0) ? 0 : bank[win];
        uint32_t offset = (uint32_t)pg * 0x2000u + (addr & 0x1FFF);
        if (offset >= size) return NULL;
        return rom + offset;
    }
    default:
        return NULL;
    }
}

/* Handle writes that update mapper bank registers. */
static void cart_mapper_write(msx_state_t *msx, uint8_t slot_idx,
                               uint16_t addr, uint8_t data) {
    uint8_t mtype = msx->cart_type[slot_idx];
    uint8_t *bank = msx->cart_bank[slot_idx];
    bool     paged = msx->cart_paged[slot_idx];

    switch (mtype) {
    case MSX_MAPPER_ASCII8:
        if      (addr >= 0x6000 && addr <= 0x67FF) {
            bank[0] = data;
            if (paged) cart_page_refill(msx, slot_idx, 0, data);
        } else if (addr >= 0x6800 && addr <= 0x6FFF) {
            bank[1] = data;
            if (paged) cart_page_refill(msx, slot_idx, 1, data);
        } else if (addr >= 0x7000 && addr <= 0x77FF) {
            bank[2] = data;
            if (paged) cart_page_refill(msx, slot_idx, 2, data);
        } else if (addr >= 0x7800 && addr <= 0x7FFF) {
            bank[3] = data;
            if (paged) cart_page_refill(msx, slot_idx, 3, data);
        }
        break;
    case MSX_MAPPER_ASCII16:
        /* Each 16KB logical page = 2 adjacent 8KB physical cache slots. */
        if (addr >= 0x6000 && addr <= 0x67FF) {
            bank[0] = data;
            if (paged) {
                cart_page_refill(msx, slot_idx, 0, (uint32_t)data * 2u);
                cart_page_refill(msx, slot_idx, 1, (uint32_t)data * 2u + 1u);
            }
        } else if (addr >= 0x7000 && addr <= 0x77FF) {
            bank[1] = data;
            if (paged) {
                cart_page_refill(msx, slot_idx, 2, (uint32_t)data * 2u);
                cart_page_refill(msx, slot_idx, 3, (uint32_t)data * 2u + 1u);
            }
        }
        break;
    case MSX_MAPPER_KONAMI:
        if      (addr >= 0x6000 && addr <= 0x6FFF) {
            bank[1] = data;
            if (paged) cart_page_refill(msx, slot_idx, 1, data);
        } else if (addr >= 0x8000 && addr <= 0x8FFF) {
            bank[2] = data;
            if (paged) cart_page_refill(msx, slot_idx, 2, data);
        } else if (addr >= 0xA000 && addr <= 0xAFFF) {
            bank[3] = data;
            if (paged) cart_page_refill(msx, slot_idx, 3, data);
        }
        break;
    default:
        break;
    }
}

/* -----------------------------------------------------------------------
 * Z80 memory read/write callbacks
 * ----------------------------------------------------------------------- */
uint8_t msx_mem_read(void *userdata, uint16_t addr) {
    msx_state_t *msx = (msx_state_t *)userdata;

    /* Determine which 16KB page and which slot services it */
    uint8_t page = addr >> 14;  /* 0–3 */
    uint8_t slot = (msx->slot_select >> (page * 2)) & 0x03;

    switch (slot) {
    case 0: /* BIOS/BASIC */
        if (addr < MSX_BIOS_SIZE) return msx->bios[addr];
        return 0xFF;

    case 1: /* Cartridge slot 1 */ {
        const uint8_t *p = cart_page_ptr(msx, 0, addr);
        return p ? *p : 0xFF;
    }
    case 2: /* Cartridge slot 2 */ {
        const uint8_t *p = cart_page_ptr(msx, 1, addr);
        return p ? *p : 0xFF;
    }
    case 3: /* RAM */
        return msx->ram[addr];

    default:
        return 0xFF;
    }
}

void msx_mem_write(void *userdata, uint16_t addr, uint8_t data) {
    msx_state_t *msx = (msx_state_t *)userdata;

    uint8_t page = addr >> 14;
    uint8_t slot = (msx->slot_select >> (page * 2)) & 0x03;

    switch (slot) {
    case 0:
        /* BIOS ROM is read-only */
        break;

    case 1:
        cart_mapper_write(msx, 0, addr, data);
        break;

    case 2:
        cart_mapper_write(msx, 1, addr, data);
        break;

    case 3:
        msx->ram[addr] = data;
        break;
    }
}

/* -----------------------------------------------------------------------
 * Z80 I/O port read/write callbacks
 *
 * superzazu/z80 port callbacks receive z80* (not userdata).
 * We recover the msx_state_t via the cpu field being first in the struct.
 * ----------------------------------------------------------------------- */
uint8_t msx_port_read(z80 *cpu, uint8_t port) {
    msx_state_t *msx = (msx_state_t *)cpu;  /* cpu is first member */

    switch (port) {
    /* VDP data port */
    case 0x98:
        return vrEmuTms9918ReadData(msx->vdp);

    /* VDP status port */
    case 0x99:
        return vrEmuTms9918ReadStatus(msx->vdp);

    /* PSG data read */
    case 0xA2:
        /* Register 14 (I/O Port A) is joystick state on real MSX hardware;
         * emu2149 treats R14/R15 as plain storage (no I/O simulation), so
         * intercept it here and return the port selected by the last
         * register-15 write (bit 6) instead of the stored (stale) value. */
        if (msx->psg_reg == 14) {
            return msx->joy_state[msx->joy_select & 1];
        }
        return PSG_readReg(msx->psg, msx->psg_reg);

    /* PPI port A: slot select */
    case 0xA8:
        return msx->slot_select;

    /* PPI port B: keyboard matrix row */
    case 0xA9:
        return msx->key_matrix[msx->key_row & 0x0F];

    /* PPI port C */
    case 0xAA:
        return msx->ppi_c;

    default:
        return 0xFF;
    }
}

void msx_port_write(z80 *cpu, uint8_t port, uint8_t data) {
    msx_state_t *msx = (msx_state_t *)cpu;

    switch (port) {
    /* VDP data write */
    case 0x98:
        vrEmuTms9918WriteData(msx->vdp, data);
        break;

    /* VDP register/address write */
    case 0x99:
        vrEmuTms9918WriteAddr(msx->vdp, data);
        break;

    /* PSG register select */
    case 0xA0:
        msx->psg_reg = data & 0x0F;
        PSG_writeIO(msx->psg, 0, data);
        break;

    /* PSG data write */
    case 0xA1:
        /* Register 15 (I/O Port B) bit 6 selects which joystick port
         * register 14 reads back on real MSX hardware. */
        if (msx->psg_reg == 15) {
            msx->joy_select = (data >> 6) & 1;
        }
        PSG_writeIO(msx->psg, 1, data);
        break;

    /* PPI port A: slot select (2 bits per page) */
    case 0xA8:
        msx->slot_select = data;
        break;

    /* PPI port C: keyboard row select (bits 3:0) + misc */
    case 0xAA:
        msx->ppi_c  = data;
        msx->key_row = data & 0x0F;
        break;

    /* PPI control word (mode set) — accept but ignore */
    case 0xAB:
        break;

    default:
        break;
    }
}

/* -----------------------------------------------------------------------
 * msx_set_joystick
 * ----------------------------------------------------------------------- */
void msx_set_joystick(msx_state_t *msx, uint8_t port, uint8_t state) {
    if (port > 1) return;
    msx->joy_state[port] = state | 0xC0;  /* bits 6-7 always 1 (unused) */
}

/* -----------------------------------------------------------------------
 * VDP interrupt helper
 * ----------------------------------------------------------------------- */
/* TMS9918A INT enable flag is in register 1 bit 5. */
#define TMS_R1_INT_ENABLE 0x20u

static inline bool vdp_int_enabled(msx_state_t *msx) {
    return (vrEmuTms9918RegValue(msx->vdp, TMS_REG_1) & TMS_R1_INT_ENABLE) != 0;
}

/* -----------------------------------------------------------------------
 * Audio: generate PSG samples for one scanline worth of time.
 * Called once per scanline during frame execution.
 * ----------------------------------------------------------------------- */
static void audio_generate_scanline(msx_state_t *msx) {
    /* samples_per_frame / lines_total rounded; we accumulate a fixed
     * number per frame (audio_samples_per_frame) spread across 192 active
     * lines. For simplicity we generate 2 samples per scanline and top up
     * at frame end. */
    /* Produce ~2 samples per active scanline */
    for (int s = 0; s < 2; s++) {
        if (msx->audio_write_pos < MSX_AUDIO_BUF_SIZE) {
            msx->audio_buf[msx->audio_write_pos++] = PSG_calc(msx->psg);
        }
    }
}

/* -----------------------------------------------------------------------
 * msx_init
 * ----------------------------------------------------------------------- */
void msx_init(msx_state_t *msx) {
    /* Free any VDP/PSG/cart heap allocations from a previous msx_init()
     * call before the memset() below drops those pointers — otherwise
     * every re-init (e.g. re-running the MicroPython script without a
     * hardware reset in between) permanently leaks ~16.8KB from the
     * small (tens of KB) C heap. Safe to call unconditionally: on the
     * very first-ever call, msx is all-zero static/BSS memory already,
     * so vdp/psg/cart are NULL and msx_destroy() is a no-op. */
    msx_destroy(msx);

    memset(msx, 0, sizeof(*msx));

    /* Build RGB565 palette */
    for (int i = 0; i < 16; i++) {
        msx->palette565[i] = rgb888_to_565be(tms_palette_rgb888[i]);
    }

    /* Precompute display-scale column mapping (avoids a division per pixel) */
    for (int ox = 0; ox < MSX_DISP_W; ox++) {
        msx->disp_col_src[ox] = (uint16_t)(ox * MSX_SCREEN_W / MSX_DISP_W);
    }

    /* Initialize VDP */
    msx->vdp = vrEmuTms9918New();

    /* Initialize PSG. Real MSX hardware feeds the AY-3-8910/YM2149 with
     * half the Z80 clock (3.579545MHz / 2 = 1.789772MHz) — passing the
     * full Z80 clock here (as emu2149's `clock` parameter, with
     * clk_div left at its default disabled state) doubled every tone
     * generator's computed frequency, making all PSG output sound
     * roughly an octave too high. */
    msx->psg = PSG_new(MSX_CLOCK_HZ / 2, MSX_AUDIO_RATE);
    PSG_setVolumeMode(msx->psg, 1);  /* YM2149 volume table */
    /* quality=1 (emu2149's oversampling rate converter) was tried to
     * reduce aliasing, but its ~20 update_output() calls per PSG_calc()
     * (vs 1 at quality=0) measurably slowed msx_run_frame(), which — since
     * MSX music sequencers are driven by the frame interrupt — was
     * perceived as BGM playing at the wrong tempo. Left at the default 0
     * (nearest-neighbor) to keep emulation speed correct. */

    /* Initialize Z80 */
    z80_init(&msx->cpu);
    msx->cpu.read_byte  = msx_mem_read;
    msx->cpu.write_byte = msx_mem_write;
    msx->cpu.port_in    = msx_port_read;
    msx->cpu.port_out   = msx_port_write;
    msx->cpu.userdata   = msx;

    /* Keyboard all keys released (active-low: 0xFF = no key) */
    memset(msx->key_matrix, 0xFF, sizeof(msx->key_matrix));

    /* Joystick: nothing pressed (active-low: 0xFF = neutral) */
    msx->joy_state[0] = 0xFF;
    msx->joy_state[1] = 0xFF;
    msx->joy_select   = 0;

    /* Default slot layout: BIOS in all pages until BIOS overrides */
    msx->slot_select = 0x00;  /* slot 0 everywhere; BIOS will set up RAM */

    msx->audio_samples_per_frame = MSX_AUDIO_RATE / MSX_FPS;
    msx->initialized = true;
}

/* -----------------------------------------------------------------------
 * msx_load_bios
 * ----------------------------------------------------------------------- */
bool msx_load_bios(msx_state_t *msx, const uint8_t *data, uint32_t size) {
    if (!msx->initialized || !data || size < 0x4000 || size > MSX_BIOS_SIZE) {
        return false;
    }
    memcpy(msx->bios, data, size);
    msx->bios_loaded = true;
    return true;
}

/* Direct pointer to the 32KB BIOS buffer (msx->bios), for zero-copy
 * loading — see msx_mark_bios_loaded()'s comment. Always valid once
 * msx_init() has run (bios[] is part of the static msx_state_t, not
 * separately allocated). */
uint8_t *msx_get_bios_ptr(msx_state_t *msx) {
    return msx->bios;
}

/* Companion to msx_get_bios_ptr(): call after writing the BIOS image
 * directly into that buffer (e.g. via Python's f.readinto(msx.get_bios_
 * view())) instead of going through msx_load_bios()'s own memcpy — same
 * size validation as msx_load_bios(), just without copying data that's
 * already in place. Exists because MicroPython's GC heap on this board is
 * tight enough that even a single ~32KB scratch bytearray for
 * msx_load_bios()'s `data` argument was observed to fail allocation on
 * real hardware; reading straight into msx->bios (already-resident,
 * always-there memory) avoids needing that scratch buffer at all — same
 * zero-copy principle as msx_get_ram_ptr()/msx_get_vram_ptr() for
 * save-states. */
bool msx_mark_bios_loaded(msx_state_t *msx, uint32_t size) {
    if (!msx->initialized || size < 0x4000 || size > MSX_BIOS_SIZE) {
        return false;
    }
    msx->bios_loaded = true;
    return true;
}

/* -----------------------------------------------------------------------
 * msx_load_cart
 * ----------------------------------------------------------------------- */
bool msx_load_cart(msx_state_t *msx, uint8_t slot, const uint8_t *data,
                   uint32_t size, uint8_t mapper) {
    if (!msx->initialized || slot > 1 || !data || size == 0) return false;
    if (size > MSX_CART_MAX_SIZE) return false;

    /* Free previous cart if any */
    msx_eject_cart(msx, slot);

    msx->cart[slot] = (uint8_t *)malloc(size);
    if (!msx->cart[slot]) return false;

    memcpy(msx->cart[slot], data, size);
    msx->cart_size[slot] = size;

    /* Auto-detect mapper if caller requests it */
    msx->cart_type[slot] = (mapper == MSX_MAPPER_PLAIN && size > 0x8000)
                            ? detect_mapper(data, size)
                            : mapper;

    /* Initialize bank registers: page 0 maps to ROM page 0, etc. */
    msx->cart_bank[slot][0] = 0;
    msx->cart_bank[slot][1] = 1;
    msx->cart_bank[slot][2] = 2;
    msx->cart_bank[slot][3] = 3;

    return true;
}

/* -----------------------------------------------------------------------
 * msx_cart_alloc / msx_cart_finalize
 *
 * Two-phase, zero-copy alternative to msx_load_cart() for callers that
 * can't hand over a `data` pointer with the whole cart already in a
 * Python-owned buffer — msx_cart_alloc() mallocs msx->cart[slot] and
 * returns it so the caller can write the ROM directly into that memory
 * (e.g. via Python's f.readinto(memoryview) straight from SD), then
 * msx_cart_finalize() does the mapper-detect/bank-init work
 * msx_load_cart() normally does right after its memcpy. Exists for the
 * same reason as msx_get_bios_ptr()/msx_mark_bios_loaded(): a full-size
 * (up to 32KB) Python scratch bytearray for msx_load_cart()'s `data`
 * argument was observed to fail allocation on this board's GC heap even
 * right after gc.collect(); writing straight into the already-malloc'd
 * msx->cart[slot] avoids needing that scratch buffer at all.
 * ----------------------------------------------------------------------- */
uint8_t *msx_cart_alloc(msx_state_t *msx, uint8_t slot, uint32_t size) {
    if (!msx->initialized || slot > 1 || size == 0 || size > MSX_CART_MAX_SIZE) {
        return NULL;
    }
    /* Deliberately NOT msx_eject_cart(msx, slot) here — that would free()
     * cart[slot], which is exactly the malloc()/free() cycle this
     * function exists to avoid (see cart_alloc_cap[]'s comment in
     * msx_core.h). Do the same paged-mode cleanup msx_eject_cart() does,
     * minus touching cart[]/cart_alloc_cap[]; msx_load_cart_paged() still
     * calls the real msx_eject_cart() when switching a slot *to* paged
     * mode, which correctly frees this buffer since it's genuinely not
     * needed in that mode. */
    free(msx->cart_cache[slot]);
    msx->cart_cache[slot] = NULL;
    msx->cart_paged[slot] = false;
    for (int i = 0; i < 4; i++) msx->cart_cache_page[slot][i] = -1;

    if (!(msx->cart[slot] && msx->cart_alloc_cap[slot] >= size)) {
        /* No existing block, or it's too small — round small carts up to
         * MSX_CART_INRAM_MAX so this capacity, once established, covers
         * every future in-RAM cart on this slot (the common case: nothing
         * bigger than that is ever routed here — see MSX_CART_INRAM_MAX's
         * comment). Bigger requests (rare/unused in practice) just get
         * exactly what they ask for, no reuse benefit either way. */
        uint32_t new_cap = (size <= MSX_CART_INRAM_MAX) ? MSX_CART_INRAM_MAX : size;
        uint8_t *fresh = (uint8_t *)malloc(new_cap);
        if (!fresh) return NULL;
        free(msx->cart[slot]);
        msx->cart[slot] = fresh;
        msx->cart_alloc_cap[slot] = new_cap;
    }
    msx->cart_size[slot] = size;
    return msx->cart[slot];
}

bool msx_cart_finalize(msx_state_t *msx, uint8_t slot, uint8_t mapper) {
    if (!msx->initialized || slot > 1 || !msx->cart[slot]) return false;
    uint32_t size = msx->cart_size[slot];

    msx->cart_type[slot] = (mapper == MSX_MAPPER_PLAIN && size > 0x8000)
                            ? detect_mapper(msx->cart[slot], size)
                            : mapper;

    msx->cart_bank[slot][0] = 0;
    msx->cart_bank[slot][1] = 1;
    msx->cart_bank[slot][2] = 2;
    msx->cart_bank[slot][3] = 3;

    return true;
}

/* -----------------------------------------------------------------------
 * msx_set_cart_fetch_cb / msx_load_cart_paged
 * ----------------------------------------------------------------------- */
void msx_set_cart_fetch_cb(msx_state_t *msx, msx_cart_fetch_fn cb, void *userdata) {
    msx->cart_fetch_cb       = cb;
    msx->cart_fetch_userdata = userdata;
}

/* See cart_bankswitch_count/cart_fetch_count's comment above
 * cart_page_refill(). Cumulative since boot — callers typically sample
 * the delta over a known interval (e.g. msx_main.py's periodic FPS print)
 * to get a fetches/sec figure comparable against the FPS drop. */
void msx_get_cart_fetch_stats(msx_state_t *msx, uint32_t *bankswitch_count,
                               uint32_t *fetch_count) {
    (void)msx;
    if (bankswitch_count) *bankswitch_count = cart_bankswitch_count;
    if (fetch_count) *fetch_count = cart_fetch_count;
}

bool msx_load_cart_paged(msx_state_t *msx, uint8_t slot, uint32_t total_size,
                          uint8_t mapper) {
    if (!msx->initialized || slot > 1 || total_size == 0) return false;
    if (mapper == MSX_MAPPER_PLAIN) return false;  /* paging needs banking */

    /* Free previous cart if any (either mode) */
    msx_eject_cart(msx, slot);

    msx->cart_cache[slot] = (uint8_t *)malloc(4u * MSX_CART_PAGE_SIZE);
    if (!msx->cart_cache[slot]) return false;

    msx->cart_paged[slot] = true;
    msx->cart_size[slot]  = total_size;
    msx->cart_type[slot]  = mapper;

    /* Same initial bank state as msx_load_cart(): page N maps to ROM
     * page N. Force an initial synchronous fetch for all 4 windows so
     * the cache holds valid data before the CPU executes anything. */
    msx->cart_bank[slot][0] = 0;
    msx->cart_bank[slot][1] = 1;
    msx->cart_bank[slot][2] = 2;
    msx->cart_bank[slot][3] = 3;

    for (uint8_t win = 0; win < 4; win++) {
        msx->cart_cache_page[slot][win] = -1;
        cart_page_refill(msx, slot, win, win);
    }

    return true;
}

/* -----------------------------------------------------------------------
 * msx_eject_cart
 * ----------------------------------------------------------------------- */
void msx_eject_cart(msx_state_t *msx, uint8_t slot) {
    if (slot > 1) return;
    /* Genuinely frees cart[slot] (unlike msx_cart_alloc()'s own internal
     * cleanup, which deliberately keeps reusing this block across swaps —
     * see cart_alloc_cap[]'s comment in msx_core.h). Called here for a
     * real "nothing loaded" state and by msx_load_cart_paged() when a
     * slot switches *to* paged mode, where this buffer genuinely isn't
     * needed any more. */
    free(msx->cart[slot]);
    msx->cart[slot]         = NULL;
    msx->cart_alloc_cap[slot] = 0;
    free(msx->cart_cache[slot]);
    msx->cart_cache[slot] = NULL;
    msx->cart_paged[slot] = false;
    msx->cart_size[slot] = 0;
    msx->cart_type[slot] = MSX_MAPPER_PLAIN;
    memset(msx->cart_bank[slot], 0, sizeof(msx->cart_bank[slot]));
    for (int i = 0; i < 4; i++) msx->cart_cache_page[slot][i] = -1;
}

/* -----------------------------------------------------------------------
 * msx_reset
 * ----------------------------------------------------------------------- */
void msx_reset(msx_state_t *msx) {
    /* Z80 reset: PC = 0, registers cleared */
    z80_init(&msx->cpu);
    msx->cpu.read_byte  = msx_mem_read;
    msx->cpu.write_byte = msx_mem_write;
    msx->cpu.port_in    = msx_port_read;
    msx->cpu.port_out   = msx_port_write;
    msx->cpu.userdata   = msx;

    vrEmuTms9918Reset(msx->vdp);
    PSG_reset(msx->psg);

    msx->slot_select    = 0x00;
    msx->key_row        = 0;
    msx->ppi_c          = 0;
    msx->psg_reg        = 0;
    msx->frame_tcycles  = 0;
    msx->current_line   = 0;
    msx->audio_write_pos = 0;

    memset(msx->key_matrix, 0xFF, sizeof(msx->key_matrix));

    msx->joy_state[0] = 0xFF;
    msx->joy_state[1] = 0xFF;
    msx->joy_select   = 0;
}

/* -----------------------------------------------------------------------
 * msx_setup_display
 * ----------------------------------------------------------------------- */
void msx_setup_display(msx_state_t *msx, void *spi_inst,
                       uint8_t cs_pin, uint8_t dc_pin) {
    msx->spi_inst     = spi_inst;
    msx->spi_cs_pin   = cs_pin;
    msx->spi_dc_pin   = dc_pin;
    msx->display_ready = (spi_inst != NULL);
}

/* -----------------------------------------------------------------------
 * msx_run_frame
 * Run one complete NTSC frame.
 * Returns: number of audio samples generated this frame.
 * ----------------------------------------------------------------------- */
uint32_t msx_run_frame(msx_state_t *msx) {
    if (!msx->initialized || !msx->bios_loaded) return 0;

    msx->audio_write_pos = 0;  /* reset per-frame audio buffer */

    /* Render into the buffer NOT currently exposed to the display/render
     * functions, so a concurrent DMA transfer of the previous frame can't
     * race with these writes. */
    uint16_t *wbuf = msx->framebuf[1 - msx->framebuf_ready_idx];

    for (uint32_t line = 0; line < MSX_LINES_TOTAL; line++) {
        /* Run Z80 for one scanline worth of T-states */
        unsigned long cycles_before = msx->cpu.cyc;
        while ((msx->cpu.cyc - cycles_before) < MSX_TCYCLES_LINE) {
            z80_step(&msx->cpu);
        }

        /* Render active scanline into framebuf */
        if (line < MSX_LINES_ACTIVE) {
            vrEmuTms9918ScanLine(msx->vdp, (uint8_t)line, msx->scanline_buf);
            uint16_t *dst = wbuf + line * MSX_SCREEN_W;
            for (int x = 0; x < MSX_SCREEN_W; x++) {
                dst[x] = msx->palette565[msx->scanline_buf[x] & 0x0F];
            }
            /* Generate audio for this active scanline */
            audio_generate_scanline(msx);
        }

        /* Assert VDP interrupt after last active line */
        if (line == MSX_LINES_ACTIVE && vdp_int_enabled(msx)) {
            z80_gen_int(&msx->cpu, 0xFF);
        }
    }

    msx->frame_tcycles = (uint32_t)msx->cpu.cyc;
    msx->framebuf_ready_idx = 1 - msx->framebuf_ready_idx;  /* publish this frame */
    return msx->audio_write_pos;
}

/* -----------------------------------------------------------------------
 * ILI9341 SPI helpers + DMA rendering (Pico SDK only)
 * ----------------------------------------------------------------------- */
#if HAVE_PICO_SDK

static int msx_dma_chan = -1;  /* claimed once, reused every frame */

/* --- Low-level SPI helpers (blocking, used only for commands) --- */

static inline void _ili_cs(msx_state_t *msx, bool low) {
    gpio_put(msx->spi_cs_pin, low ? 0 : 1);
}
static inline void _ili_dc(msx_state_t *msx, bool data) {
    gpio_put(msx->spi_dc_pin, data ? 1 : 0);
}
static void _ili_cmd(msx_state_t *msx, uint8_t cmd) {
    _ili_dc(msx, false);
    _ili_cs(msx, true);
    spi_write_blocking((spi_inst_t *)msx->spi_inst, &cmd, 1);
    _ili_cs(msx, false);
}
static void _ili_data(msx_state_t *msx, const uint8_t *d, size_t n) {
    _ili_dc(msx, true);
    _ili_cs(msx, true);
    spi_write_blocking((spi_inst_t *)msx->spi_inst, d, n);
    _ili_cs(msx, false);
}
static void _ili_data1(msx_state_t *msx, uint8_t v) {
    _ili_data(msx, &v, 1);
}

/* Set ILI9341 pixel window (CASET + PASET + RAMWR). CS stays LOW for pixel data. */
static void _ili_set_window(msx_state_t *msx,
                             uint16_t x0, uint16_t y0,
                             uint16_t x1, uint16_t y1) {
    uint8_t buf[4];

    buf[0]=x0>>8; buf[1]=x0; buf[2]=x1>>8; buf[3]=x1;
    _ili_cmd(msx, 0x2A); _ili_data(msx, buf, 4);  /* CASET */

    buf[0]=y0>>8; buf[1]=y0; buf[2]=y1>>8; buf[3]=y1;
    _ili_cmd(msx, 0x2B); _ili_data(msx, buf, 4);  /* PASET */

    _ili_cmd(msx, 0x2C);   /* RAMWR — CS will be pulled low by DMA caller */
}

/* -----------------------------------------------------------------------
 * msx_boost_peri_clock
 * ----------------------------------------------------------------------- */
void msx_boost_peri_clock(void) {
    uint32_t sys_hz = clock_get_hz(clk_sys);
    clock_configure(clk_peri,
                     0,
                     CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLK_SYS,
                     sys_hz,
                     sys_hz);
}

/* -----------------------------------------------------------------------
 * msx_boost_dma_priority
 * ----------------------------------------------------------------------- */
void msx_boost_dma_priority(void) {
    busctrl_hw->priority = BUSCTRL_BUS_PRIORITY_DMA_R_BITS |
                            BUSCTRL_BUS_PRIORITY_DMA_W_BITS;
}

/* -----------------------------------------------------------------------
 * msx_init_display_hardware
 * Full ILI9341 initialization. Call once at startup.
 * ----------------------------------------------------------------------- */
void msx_init_display_hardware(msx_state_t *msx,
                                uint8_t spi_id, uint32_t baudrate,
                                uint8_t mosi_pin, uint8_t sck_pin,
                                uint8_t cs_pin,  uint8_t dc_pin,
                                uint8_t rst_pin, uint8_t bl_pin,
                                uint16_t lcd_w, uint16_t lcd_h,
                                bool rotate_180) {
    spi_inst_t *spi = (spi_id == 0) ? spi0 : spi1;

    /* Store config */
    msx->spi_inst    = spi;
    msx->spi_cs_pin  = cs_pin;
    msx->spi_dc_pin  = dc_pin;
    msx->spi_rst_pin = rst_pin;
    msx->spi_bl_pin  = bl_pin;
    msx->spi_baudrate = baudrate;
    /* Clamp to the compile-time max (LCD_W/LCD_H) — that's also the size
     * of the static black_row clear buffer below. */
    msx->lcd_w = (lcd_w > 0 && lcd_w <= LCD_W) ? lcd_w : LCD_W;
    msx->lcd_h = (lcd_h > 0 && lcd_h <= LCD_H) ? lcd_h : LCD_H;
    msx->rotate_180 = rotate_180;

    /* Configure control GPIO (CS, DC, BL) — keep RST separate, see below */
    gpio_init(cs_pin);  gpio_set_dir(cs_pin,  GPIO_OUT); gpio_put(cs_pin,  1);
    gpio_init(dc_pin);  gpio_set_dir(dc_pin,  GPIO_OUT); gpio_put(dc_pin,  0);
    gpio_init(bl_pin);  gpio_set_dir(bl_pin,  GPIO_OUT); gpio_put(bl_pin,  0);

    /* Ensure SPI function pins are correct (may already be set by MicroPython) */
    gpio_set_function(mosi_pin, GPIO_FUNC_SPI);
    gpio_set_function(sck_pin,  GPIO_FUNC_SPI);

    /* SPI1_RX (GP12) doubles as SD-MISO; a pull-up ensures the line reads
     * 0xFF when the SD card tri-states MISO (CS deasserted).  Without this,
     * a floating MISO can appear as 0x00, causing sdcard.cmd() to return a
     * false "OK" and sdcard.readinto() to time out waiting for a 0xFE token.
     * gpio_set_function() in spi_init() preserves the pull-up bit, so this
     * survives subsequent machine.SPI.init() calls in the SD driver. */
    if (spi == spi1) {
        gpio_pull_up(12);   /* SPI1_RX = GP12 = SD MISO */
    }

    /* Configure SPI without calling spi_init().
     * spi_init() resets and re-enables the peripheral; when MicroPython's
     * machine.SPI already owns SPI1 (shared SD card bus), resetting it
     * destroys the Python driver state and breaks subsequent SD reads.
     * Python's spi_init already set the format and enabled the peripheral;
     * we only need to set the target baudrate and ensure DMA/SSE bits. */
    spi_set_baudrate(spi, baudrate);
    spi_set_format(spi, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    hw_set_bits(&spi_get_hw(spi)->dmacr,
                SPI_SSPDMACR_TXDMAE_BITS | SPI_SSPDMACR_RXDMAE_BITS);
    hw_set_bits(&spi_get_hw(spi)->cr1, SPI_SSPCR1_SSE_BITS);

    /* Hardware reset — rst_pin (GP7) is a dedicated GPIO, not shared with SPI.
     * Leave it as GPIO_OUT HIGH after the reset pulse. */
    gpio_init(rst_pin); gpio_set_dir(rst_pin, GPIO_OUT); gpio_put(rst_pin, 1);
    gpio_put(rst_pin, 0); sleep_ms(10);
    gpio_put(rst_pin, 1); sleep_ms(120);

    /* ILI9341 init sequence (matches ili9341.py on this board) */
    _ili_cmd(msx, 0x01);   /* SWRESET */
    sleep_ms(150);
    _ili_cmd(msx, 0x11);   /* SLPOUT */
    sleep_ms(255);

    _ili_cmd(msx, 0x3A); _ili_data1(msx, 0x55); /* PIXFMT: 16-bit RGB565 */
    /* MADCTL: 0x28 = normal landscape (MV=1,BGR=1, matches st7796.py/
     * ili9341.py); 0xE8 additionally inverts MX+MY for a 180° flip while
     * staying in landscape (MV/BGR unchanged) — CASET/PASET addressing
     * and the centering math below are unaffected either way. */
    _ili_cmd(msx, 0x36); _ili_data1(msx, msx->rotate_180 ? 0xE8 : 0x28);
    _ili_cmd(msx, 0x29);                         /* DISPON */
    sleep_ms(20);

    /* Clear screen to black — msx->lcd_w × msx->lcd_h pixels, one row at
     * a time. black_row is sized for the compile-time max (LCD_W); a
     * smaller runtime panel just uses a prefix of it per row. */
    _ili_set_window(msx, 0, 0, msx->lcd_w - 1, msx->lcd_h - 1);
    _ili_dc(msx, true);
    _ili_cs(msx, true);
    static const uint8_t black_row[LCD_W * 2] = {0};  /* zeros in flash */
    for (int row = 0; row < msx->lcd_h; row++) {
        spi_write_blocking(spi, black_row, (size_t)msx->lcd_w * 2u);
    }
    _ili_cs(msx, false);

    /* Backlight on */
    gpio_put(bl_pin, 1);

    msx->display_ready = true;
}

/* -----------------------------------------------------------------------
 * msx_wait_display — wait for any in-flight DMA transfer to finish
 * ----------------------------------------------------------------------- */
void msx_wait_display(msx_state_t *msx) {
    if (!msx->display_ready) return;
    if (msx_dma_chan >= 0 && dma_channel_is_busy(msx_dma_chan)) {
        dma_channel_wait_for_finish_blocking(msx_dma_chan);
    }
    spi_inst_t *spi = (spi_inst_t *)msx->spi_inst;
    /* Wait for SPI shift register to finish clocking out the last bytes */
    while (spi_is_busy(spi)) { tight_loop_contents(); }
    /* Drain SPI RX FIFO (filled with garbage during TX-only DMA) */
    while (spi_is_readable(spi)) { (void)spi_get_hw(spi)->dr; }
    /* Deassert CS */
    gpio_put(msx->spi_cs_pin, 1);
}

/* Nearest-neighbor scale a batch of `n_rows` output rows starting at
 * output row `oy0` (MSX_DISP_W wide each) into `dst`. Source row for each
 * maps as `oy * MSX_SCREEN_H / MSX_DISP_H` (exact 2:3). */
static inline void _build_scaled_rows(msx_state_t *msx, int oy0, int n_rows,
                                       uint16_t *dst) {
    const uint16_t *col_src = msx->disp_col_src;
    for (int i = 0; i < n_rows; i++) {
        int sy = (oy0 + i) * MSX_SCREEN_H / MSX_DISP_H;
        const uint16_t *srow = msx->framebuf[msx->framebuf_ready_idx] + sy * MSX_SCREEN_W;
        uint16_t *drow = dst + (size_t)i * MSX_DISP_W;
        for (int ox = 0; ox < MSX_DISP_W; ox++) {
            drow[ox] = srow[col_src[ox]];
        }
    }
}

static inline void _dma_wait_chan(void) {
    if (msx_dma_chan >= 0 && dma_channel_is_busy(msx_dma_chan)) {
        dma_channel_wait_for_finish_blocking(msx_dma_chan);
    }
}

static void _dma_start_rows(spi_inst_t *spi, const uint16_t *rows, uint32_t n_rows) {
    dma_channel_config cfg = dma_channel_get_default_config(msx_dma_chan);
    channel_config_set_transfer_data_size(&cfg, DMA_SIZE_8);
    channel_config_set_read_increment(&cfg, true);
    channel_config_set_write_increment(&cfg, false);
    channel_config_set_dreq(&cfg, spi_get_dreq(spi, true));

    dma_channel_configure(
        msx_dma_chan, &cfg,
        &spi_get_hw(spi)->dr,     /* write to SPI TX FIFO */
        (const uint8_t *)rows,    /* read from the scaled row-batch buffer */
        n_rows * MSX_DISP_W * 2u,
        true                      /* start immediately */
    );
}

/* -----------------------------------------------------------------------
 * msx_render_to_display — scales framebuf (256x192) by 1.5x to
 * MSX_DISP_W x MSX_DISP_H (384x288) and streams it to the ST7796 in
 * batches of MSX_DISP_ROW_BATCH rows via DMA (single-row transfers proved
 * far slower than the theoretical SPI rate due to per-transfer overhead).
 * Batches are double-buffered so the CPU can build the next batch while
 * DMA sends the current one.
 * The final batch's DMA is left in-flight; call msx_wait_display() before
 * the next render or SD card access.
 * ----------------------------------------------------------------------- */
void msx_render_to_display(msx_state_t *msx) {
    if (!msx->display_ready) return;

    spi_inst_t *spi = (spi_inst_t *)msx->spi_inst;

    /* Enforce SPI settings — sdcard readblocks() calls machine.SPI.init() which
     * internally calls spi_init(), resetting SPI1 and clearing DMACR.  Re-apply
     * baudrate, format, and DMA enable every render so DMA always fires.
     * (A full spi_init() here — hardware reset via RESETS, tried when this
     * bus started also being switched to the HDMI link's SPI mode every
     * frame — did not fix the underlying instability and introduced new
     * glitches of its own; see msx_render_to_hdmi()'s comment.) */
    msx->spi_actual_baudrate = spi_set_baudrate(spi, msx->spi_baudrate ? msx->spi_baudrate : 40000000u);
    spi_set_format(spi, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    hw_set_bits(&spi_get_hw(spi)->dmacr,
                SPI_SSPDMACR_TXDMAE_BITS | SPI_SSPDMACR_RXDMAE_BITS);
    hw_set_bits(&spi_get_hw(spi)->cr1, SPI_SSPCR1_SSE_BITS);

    /* Set pixel window — blocks briefly for the 3 command bytes */
    _ili_set_window(msx,
                    MSX_DISP_X_OFFSET,
                    MSX_DISP_Y_OFFSET,
                    MSX_DISP_X_OFFSET + MSX_DISP_W - 1,
                    MSX_DISP_Y_OFFSET + MSX_DISP_H - 1);

    /* Pull CS low and set DC=data for the pixel stream */
    gpio_put(msx->spi_dc_pin, 1);
    gpio_put(msx->spi_cs_pin, 0);

    /* Claim DMA channel on first use */
    if (msx_dma_chan < 0) {
        msx_dma_chan = dma_claim_unused_channel(true);
    }

    const int n_batches = MSX_DISP_H / MSX_DISP_ROW_BATCH;

    int cur = 0;
    _build_scaled_rows(msx, 0, MSX_DISP_ROW_BATCH, msx->disp_row_buf[cur]);
    _dma_start_rows(spi, msx->disp_row_buf[cur], MSX_DISP_ROW_BATCH);

    for (int b = 1; b < n_batches; b++) {
        int next = 1 - cur;
        _build_scaled_rows(msx, b * MSX_DISP_ROW_BATCH, MSX_DISP_ROW_BATCH,
                            msx->disp_row_buf[next]); /* overlaps in-flight DMA */
        _dma_wait_chan();
        _dma_start_rows(spi, msx->disp_row_buf[next], MSX_DISP_ROW_BATCH);
        cur = next;
    }
    /* Return with the last batch's DMA in-flight — caller does other work
     * while it completes, then calls msx_wait_display(). */
}

/* -----------------------------------------------------------------------
 * msx_render_to_display_1to1 — native 256x192 framebuf, centered, single
 * DMA transfer (no scaling). For A/B-comparing against the scaled path.
 * ----------------------------------------------------------------------- */
void msx_render_to_display_1to1(msx_state_t *msx) {
    if (!msx->display_ready) return;

    spi_inst_t *spi = (spi_inst_t *)msx->spi_inst;

    /* (See msx_render_to_display()'s comment above the equivalent lines —
     * a full spi_init() was tried here too when this bus started also
     * being switched to the HDMI link's SPI mode every frame; it neither
     * fixed the underlying instability nor avoided new glitches, so this
     * stays the lighter reconfiguration.) */
    msx->spi_actual_baudrate = spi_set_baudrate(spi, msx->spi_baudrate ? msx->spi_baudrate : 40000000u);
    spi_set_format(spi, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    hw_set_bits(&spi_get_hw(spi)->dmacr,
                SPI_SSPDMACR_TXDMAE_BITS | SPI_SSPDMACR_RXDMAE_BITS);
    hw_set_bits(&spi_get_hw(spi)->cr1, SPI_SSPCR1_SSE_BITS);

    const int x0 = (msx->lcd_w - MSX_SCREEN_W) / 2;
    const int y0 = (msx->lcd_h - MSX_SCREEN_H) / 2;
    _ili_set_window(msx, x0, y0, x0 + MSX_SCREEN_W - 1, y0 + MSX_SCREEN_H - 1);

    gpio_put(msx->spi_dc_pin, 1);
    gpio_put(msx->spi_cs_pin, 0);

    if (msx_dma_chan < 0) {
        msx_dma_chan = dma_claim_unused_channel(true);
    }

    dma_channel_config cfg = dma_channel_get_default_config(msx_dma_chan);
    channel_config_set_transfer_data_size(&cfg, DMA_SIZE_8);
    channel_config_set_read_increment(&cfg, true);
    channel_config_set_write_increment(&cfg, false);
    channel_config_set_dreq(&cfg, spi_get_dreq(spi, true));

    dma_channel_configure(
        msx_dma_chan, &cfg,
        &spi_get_hw(spi)->dr,
        (const uint8_t *)msx->framebuf[msx->framebuf_ready_idx],
        MSX_SCREEN_W * MSX_SCREEN_H * 2u,
        true
    );
}

/* -----------------------------------------------------------------------
 * HDMI bridge output — see hdmi_bridge/README.md for this project's own
 * HDMI addon background/history and doc/hdmi_bridge_phase2_report.md for
 * how it was originally validated. The receiver firmware itself now lives
 * in the shared ../../../hdmi_bridge_receiver sibling project (see its
 * README.md) since the protocol is fully self-describing and the receiver
 * is receiver-side generic — no longer specific to this project.
 *
 * Wire protocol: each CS-low burst is a packet = an 8-byte self-describing
 * header followed by a payload, matching
 * ../../../hdmi_bridge_receiver/main.c's protocol comment (the
 * authoritative spec — this same generic receiver firmware serves both
 * this project and PB-1000_emu_AG2's sender, auto-adapting to whichever is
 * plugged in without reflashing). Header: [0]=pkt_type, [1]=bpp (for
 * PKT_FRAME) or palette entry count (for PKT_PALETTE), [2..3]=width
 * (big-endian), [4..5]=height (big-endian), [6]=scale, [7]=reserved(0).
 * Kept in sync manually since the two sides build separately (pico-sdk C
 * vs MicroPython C module). MSX1 has a fixed 16-color hardware palette
 * (see tms_palette_rgb888[] above), so game frames are sent as PKT_FRAME
 * with bpp=4 (2 pixels/byte — half the bytes of direct RGB332) with the
 * palette itself sent once via PKT_PALETTE (not per-frame — MSX1's
 * palette never changes, so this costs nothing ongoing). PKT_FRAME with
 * bpp=8 (1 byte/pixel, no palette) is used for menu/UI screens that draw
 * outside the 16-color palette (see msx_render_to_hdmi_raw332()). */
#define HDMI_PKT_PALETTE      0x00u
#define HDMI_PKT_FRAME        0x01u
/* Matches the receiver's PKT_CLEAR_SCREEN (0x04, see hdmi_bridge_receiver's
 * main.c) — a dedicated, header-only (1 dummy payload byte) command that
 * memset()s the receiver's output framebuffer directly AND resets its
 * per-layer (game/menu/bezel) max-size window-centering tracking state.
 * Deliberately NOT the same as sending an all-black PKT_FRAME: doing that
 * instead would register as a real KIND_PIXEL frame on the receiver and
 * pollute its centering-window tracking for that layer (the receiver's
 * own comment on PKT_CLEAR_SCREEN explains why this was split out as its
 * own packet type instead of reusing PKT_FRAME/PKT_TEXT_CMDS). */
#define HDMI_PKT_CLEAR_SCREEN 0x04u
#define HDMI_SCALE       1u /* receiver upscale factor: MSX's 256x192 already fills most of a 640x480 screen unscaled */

static inline void hdmi_apply_spi_settings(msx_state_t *msx, spi_inst_t *spi) {
    /* Same bus as the LCD/SD (shared MOSI/SCK), switching between this
     * link's CPOL=1/CPHA=1 (mode 3 — required by the HDMI receiver's PL022
     * SPI slave, which was found on real hardware to lose synchronization
     * within a few bytes in mode 0; see doc/hdmi_bridge_phase2_report.md)
     * and the LCD's own CPOL=0/CPHA=0.
     *
     * KNOWN ISSUE (real hardware, config.txt display=both): switching
     * between these two SPI modes every single frame was found to be
     * unreliable on this hardware regardless of how the switch is done —
     * both a light spi_set_baudrate()+spi_set_format() and a full
     * spi_init() (hardware reset via RESETS) were tried; neither avoided
     * an eventual LCD white-out / HDMI signal loss, and the full-reset
     * version additionally introduced visible glitches (a shifted HDMI
     * image, LCD flicker at the top of the screen) with no improvement in
     * stability, so it was reverted in favor of the lighter, faster
     * reconfiguration below. display=lcd or display=hdmi (config.txt) —
     * which switch modes rarely, only around occasional SD access, not
     * every frame — have not shown these problems. See
     * doc/hdmi_bridge_phase2_report.md and hdmi_bridge/README.md for
     * the full investigation; frequent same-peripheral SPI mode
     * switching on RP2350 remains an open problem. */
    spi_set_baudrate(spi, msx->hdmi_baudrate ? msx->hdmi_baudrate : 10000000u);
    spi_set_format(spi, 8, SPI_CPOL_1, SPI_CPHA_1, SPI_MSB_FIRST);
}

/* RGB565 (byte-swapped, as stored in framebuf) -> RGB332 byte. Shared by
 * the palette send and (in the RAW332 fallback, not currently used) a
 * per-pixel path — top 3 bits of R, top 3 bits of G, top 2 bits of B,
 * matching the HDMI receiver's HSTX expand_tmds config (verified against
 * the phase2 test pattern on real hardware). */
static inline uint8_t hdmi_565be_to_332(uint16_t raw565be) {
    uint16_t px = (uint16_t)((raw565be >> 8) | (raw565be << 8));
    uint8_t r3 = (px >> 13) & 0x07;
    uint8_t g3 = (px >> 8)  & 0x07;
    uint8_t b2 = (px >> 3)  & 0x03;
    return (uint8_t)((r3 << 5) | (g3 << 2) | b2);
}

void msx_send_hdmi_palette(msx_state_t *msx) {
    if (!msx->hdmi_ready) return;

    spi_inst_t *spi = (spi_inst_t *)msx->spi_inst;
    hdmi_apply_spi_settings(msx, spi);

    uint8_t palette332[16];
    for (int i = 0; i < 16; i++) {
        palette332[i] = hdmi_565be_to_332(msx->palette565[i]);
    }

    uint8_t header[8] = { (uint8_t)HDMI_PKT_PALETTE, 16, 0, 0, 0, 0, 0, 0 };
    gpio_put(msx->hdmi_cs_pin, 0);
    spi_write_blocking(spi, header, sizeof(header));
    spi_write_blocking(spi, palette332, sizeof(palette332));
    gpio_put(msx->hdmi_cs_pin, 1);
}

void msx_init_hdmi_output(msx_state_t *msx, uint8_t cs_pin, uint32_t baudrate) {
    msx->hdmi_cs_pin  = cs_pin;
    msx->hdmi_baudrate = baudrate;
    /* Idle high (deasserted) — matches the HDMI receiver's SPI0 slave CSn
     * convention (active low), same as the LCD's own CS pin. */
    gpio_init(cs_pin);
    gpio_set_dir(cs_pin, GPIO_OUT);
    gpio_put(cs_pin, 1);
    msx->hdmi_ready = true;

    /* MSX1's 16-color palette is fixed hardware (tms_palette_rgb888[]
     * above) and never changes, so sending it once here — rather than
     * requiring every caller to remember a separate step — covers the
     * common case for free. Safe/idempotent to also call
     * msx_send_hdmi_palette() again manually later if ever needed. */
    msx_send_hdmi_palette(msx);
}

/* Packed-pixel row buffer for the HDMI link (2 pixels/byte, one 128-byte
 * row at a time, not the full 256x192 frame) — this board's RAM is already
 * tight (framebuf alone is 2x256x192x2 bytes); a full-frame scratch buffer
 * here was found on real hardware to starve MicroPython's GC heap and
 * cause a MemoryError at boot. CS is held low across all 192 row writes
 * below, so the HDMI receiver still sees one continuous packet. */
static uint8_t hdmi_row_buf[MSX_SCREEN_W / 2];

/* Reverse-lookup: which of the 16 palette entries does this (byte-swapped)
 * RGB565 pixel match? framebuf only ever contains exact palette565[]
 * values (the VDP always writes a palette lookup result, never a blended
 * color), so this always finds an exact match. Checking the previous
 * pixel's index first is a cheap win for typical content (solid color
 * runs — backgrounds, borders — are extremely common in MSX graphics). */
static inline uint8_t hdmi_find_palette_index(const msx_state_t *msx, uint16_t raw565be) {
    static uint8_t last_idx = 0;
    if (msx->palette565[last_idx] == raw565be) {
        return last_idx;
    }
    for (uint8_t i = 0; i < 16; i++) {
        if (msx->palette565[i] == raw565be) {
            last_idx = i;
            return i;
        }
    }
    return 0; /* shouldn't happen; falls back to palette entry 0 */
}

void msx_render_to_hdmi(msx_state_t *msx) {
    if (!msx->hdmi_ready) return;

    spi_inst_t *spi = (spi_inst_t *)msx->spi_inst;
    hdmi_apply_spi_settings(msx, spi);

    const uint16_t *fb = msx->framebuf[msx->framebuf_ready_idx];

    uint8_t header[8] = {
        (uint8_t)HDMI_PKT_FRAME, 4 /* bpp */,
        (uint8_t)(MSX_SCREEN_W >> 8), (uint8_t)(MSX_SCREEN_W & 0xFF),
        (uint8_t)(MSX_SCREEN_H >> 8), (uint8_t)(MSX_SCREEN_H & 0xFF),
        (uint8_t)HDMI_SCALE, 0 /* reserved */
    };
    gpio_put(msx->hdmi_cs_pin, 0);
    spi_write_blocking(spi, header, sizeof(header));
    for (int row = 0; row < MSX_SCREEN_H; row++) {
        const uint16_t *src = fb + (size_t)row * MSX_SCREEN_W;
        for (int x = 0; x < MSX_SCREEN_W; x += 2) {
            uint8_t idx0 = hdmi_find_palette_index(msx, src[x]);
            uint8_t idx1 = hdmi_find_palette_index(msx, src[x + 1]);
            hdmi_row_buf[x / 2] = (uint8_t)((idx0 << 4) | (idx1 & 0x0Fu));
        }
        spi_write_blocking(spi, hdmi_row_buf, sizeof(hdmi_row_buf));
    }
    gpio_put(msx->hdmi_cs_pin, 1);
}

/* Row buffer for the RAW332 fallback path — 1 byte/pixel, no palette
 * constraint. Used for the menu/UI screens (MenuCanvas in msx_menu.py),
 * which draw directly into the same C framebuf as the game screen but use
 * arbitrary UI colors outside the MSX's fixed 16-color hardware palette
 * (borders, highlights, etc.) — hdmi_find_palette_index() can't represent
 * those, so PAL4 would render everything that isn't an exact palette match
 * as black. The game's own render path (msx_render_to_hdmi() above) never
 * needs this: the VDP only ever writes exact palette565[] values. */
static uint8_t hdmi_row_buf_raw332[MSX_SCREEN_W];

void msx_render_to_hdmi_raw332(msx_state_t *msx) {
    if (!msx->hdmi_ready) return;

    spi_inst_t *spi = (spi_inst_t *)msx->spi_inst;
    hdmi_apply_spi_settings(msx, spi);

    const uint16_t *fb = msx->framebuf[msx->framebuf_ready_idx];

    uint8_t header[8] = {
        (uint8_t)HDMI_PKT_FRAME, 8 /* bpp */,
        (uint8_t)(MSX_SCREEN_W >> 8), (uint8_t)(MSX_SCREEN_W & 0xFF),
        (uint8_t)(MSX_SCREEN_H >> 8), (uint8_t)(MSX_SCREEN_H & 0xFF),
        (uint8_t)HDMI_SCALE, 0 /* reserved */
    };
    gpio_put(msx->hdmi_cs_pin, 0);
    spi_write_blocking(spi, header, sizeof(header));
    for (int row = 0; row < MSX_SCREEN_H; row++) {
        const uint16_t *src = fb + (size_t)row * MSX_SCREEN_W;
        for (int x = 0; x < MSX_SCREEN_W; x++) {
            hdmi_row_buf_raw332[x] = hdmi_565be_to_332(src[x]);
        }
        spi_write_blocking(spi, hdmi_row_buf_raw332, sizeof(hdmi_row_buf_raw332));
    }
    gpio_put(msx->hdmi_cs_pin, 1);
}

/* Sends the receiver's dedicated PKT_CLEAR_SCREEN command (header-only,
 * 1 dummy payload byte) so the receiver's screen — which just keeps
 * displaying whatever frame it last received, including one left over
 * from a previous session/emulator — is blanked before this emulator's
 * own first real frame is sent. Intended to be called once right after
 * msx_init_hdmi_output().
 *
 * Deliberately does NOT send an all-black PKT_FRAME (as an earlier
 * version of this function did): the receiver treats PKT_CLEAR_SCREEN
 * specially — a direct memset() of its output framebuffer, plus resetting
 * its per-layer (game/menu/bezel) window-centering max-size tracking —
 * whereas a PKT_FRAME, even an all-black one, would register as a real
 * game-layer frame and pollute that tracking (see hdmi_bridge_receiver's
 * main.c, PKT_CLEAR_SCREEN's comment). That mismatch — plus the ~40ms
 * a full 49KB PKT_FRAME takes to send blocking at typical baud rates,
 * versus this command's ~9 bytes — was also implicated in the receiver
 * intermittently losing HDMI sync ("No Signal") right after this call on
 * real hardware. */
void msx_clear_hdmi(msx_state_t *msx) {
    if (!msx->hdmi_ready) return;

    spi_inst_t *spi = (spi_inst_t *)msx->spi_inst;
    hdmi_apply_spi_settings(msx, spi);

    uint8_t header[8] = { (uint8_t)HDMI_PKT_CLEAR_SCREEN, 0, 0, 0, 0, 0, 0, 0 };
    uint8_t dummy = 0;
    gpio_put(msx->hdmi_cs_pin, 0);
    spi_write_blocking(spi, header, sizeof(header));
    spi_write_blocking(spi, &dummy, 1);
    gpio_put(msx->hdmi_cs_pin, 1);
}

#else  /* !HAVE_PICO_SDK — stubs for host builds */

void msx_boost_peri_clock(void) { }
void msx_boost_dma_priority(void) { }

void msx_init_display_hardware(msx_state_t *msx,
                                uint8_t spi_id, uint32_t baudrate,
                                uint8_t mosi_pin, uint8_t sck_pin,
                                uint8_t cs_pin,  uint8_t dc_pin,
                                uint8_t rst_pin, uint8_t bl_pin,
                                uint16_t lcd_w, uint16_t lcd_h,
                                bool rotate_180) {
    (void)msx; (void)spi_id; (void)baudrate; (void)mosi_pin; (void)sck_pin;
    (void)cs_pin; (void)dc_pin; (void)rst_pin; (void)bl_pin;
    (void)lcd_w; (void)lcd_h; (void)rotate_180;
}
void msx_wait_display(msx_state_t *msx)    { (void)msx; }
void msx_render_to_display(msx_state_t *msx) { (void)msx; }
void msx_render_to_display_1to1(msx_state_t *msx) { (void)msx; }
void msx_init_hdmi_output(msx_state_t *msx, uint8_t cs_pin, uint32_t baudrate) {
    (void)msx; (void)cs_pin; (void)baudrate;
}
void msx_send_hdmi_palette(msx_state_t *msx) { (void)msx; }
void msx_render_to_hdmi(msx_state_t *msx) { (void)msx; }
void msx_render_to_hdmi_raw332(msx_state_t *msx) { (void)msx; }
void msx_clear_hdmi(msx_state_t *msx) { (void)msx; }

#endif /* HAVE_PICO_SDK */

/* -----------------------------------------------------------------------
 * Debug helpers
 * ----------------------------------------------------------------------- */
uint16_t msx_debug_step(msx_state_t *msx, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        z80_step(&msx->cpu);
    }
    return msx->cpu.pc;
}

void msx_debug_get_cpu(const msx_state_t *msx, msx_debug_cpu_t *out) {
    const z80 *cpu = &msx->cpu;
    out->pc = cpu->pc;
    out->sp = cpu->sp;
    out->a  = cpu->a;
    out->f  = (uint8_t)((cpu->sf << 7) | (cpu->zf << 6) | (cpu->yf << 5) |
                        (cpu->hf << 4) | (cpu->xf << 3) | (cpu->pf << 2) |
                        (cpu->nf << 1) |  cpu->cf);
    out->bc = (uint16_t)((cpu->b << 8) | cpu->c);
    out->de = (uint16_t)((cpu->d << 8) | cpu->e);
    out->hl = (uint16_t)((cpu->h << 8) | cpu->l);
    out->ix = cpu->ix;
    out->iy = cpu->iy;
    out->cyc = (uint32_t)cpu->cyc;
    out->halted = (uint8_t)cpu->halted;
    out->iff1   = (uint8_t)cpu->iff1;
    out->int_mode = cpu->interrupt_mode;
}

uint8_t msx_debug_peek(msx_state_t *msx, uint16_t addr) {
    return msx_mem_read(msx, addr);
}

uint32_t msx_debug_get_spi_baud(const msx_state_t *msx) {
    return msx->spi_actual_baudrate;
}

void msx_debug_get_clocks(uint32_t *clk_sys_hz, uint32_t *clk_peri_hz) {
#if HAVE_PICO_SDK
    *clk_sys_hz  = (uint32_t)clock_get_hz(clk_sys);
    *clk_peri_hz = (uint32_t)clock_get_hz(clk_peri);
#else
    *clk_sys_hz  = 0;
    *clk_peri_hz = 0;
#endif
}

void msx_debug_run_line(msx_state_t *msx, uint32_t line,
                         bool do_video, bool do_audio, bool do_int) {
    unsigned long cycles_before = msx->cpu.cyc;
    while ((msx->cpu.cyc - cycles_before) < MSX_TCYCLES_LINE) {
        z80_step(&msx->cpu);
    }

    if (line < MSX_LINES_ACTIVE) {
        if (do_video) {
            vrEmuTms9918ScanLine(msx->vdp, (uint8_t)line, msx->scanline_buf);
            uint16_t *dst = msx->framebuf[msx->framebuf_ready_idx] + line * MSX_SCREEN_W;
            for (int x = 0; x < MSX_SCREEN_W; x++) {
                dst[x] = msx->palette565[msx->scanline_buf[x] & 0x0F];
            }
        }
        if (do_audio) {
            audio_generate_scanline(msx);
        }
    }

    if (do_int && line == MSX_LINES_ACTIVE && vdp_int_enabled(msx)) {
        z80_gen_int(&msx->cpu, 0xFF);
    }
}

void msx_debug_get_psg(const msx_state_t *msx, msx_debug_psg_t *out) {
    const PSG *p = msx->psg;
    out->quality  = p->quality;
    out->clk      = p->clk;
    out->rate     = p->rate;
    out->base_incr = p->base_incr;
    out->realstep = p->realstep;
    out->psgtime  = p->psgtime;
    out->psgstep  = p->psgstep;
    out->freq_limit = p->freq_limit;
    out->env_ptr  = p->env_ptr;
    out->env_pause = p->env_pause;
    out->env_continue = p->env_continue;
    out->env_face = p->env_face;
    out->env_freq = p->env_freq;
    out->env_count = p->env_count;
    out->freq0  = p->freq[0];
    out->count0 = p->count[0];
}

int16_t msx_debug_psg_calc_n(msx_state_t *msx, uint32_t n) {
    int16_t last = 0;
    for (uint32_t i = 0; i < n; i++) {
        last = PSG_calc(msx->psg);
    }
    return last;
}

/* -----------------------------------------------------------------------
 * Keyboard
 * ----------------------------------------------------------------------- */
void msx_set_key_matrix(msx_state_t *msx, uint8_t row, uint8_t col_mask) {
    if (row >= MSX_KEY_ROWS) return;
    msx->key_matrix[row] = col_mask;  /* active-low: 0 = pressed */
}

void msx_clear_keys(msx_state_t *msx) {
    memset(msx->key_matrix, 0xFF, sizeof(msx->key_matrix));
}

/* -----------------------------------------------------------------------
 * msx_destroy
 * ----------------------------------------------------------------------- */
void msx_destroy(msx_state_t *msx) {
    for (int i = 0; i < 2; i++) {
        msx_eject_cart(msx, i);
    }
    if (msx->vdp) { vrEmuTms9918Destroy(msx->vdp); msx->vdp = NULL; }
    if (msx->psg) { PSG_delete(msx->psg); msx->psg = NULL; }
    msx->initialized = false;
}

uint8_t *msx_get_vram_ptr(msx_state_t *msx) {
    return msx->vdp ? vrEmuTms9918VramPtr(msx->vdp) : NULL;
}

/* -----------------------------------------------------------------------
 * State save / load
 * ----------------------------------------------------------------------- */

/* Pack Z80 flag bitfields → one byte (S Z Y H X P N C). */
static inline uint8_t _z80_pack_f(const z80 *cpu) {
    return (uint8_t)((cpu->sf << 7) | (cpu->zf << 6) | (cpu->yf << 5) |
                     (cpu->hf << 4) | (cpu->xf << 3) | (cpu->pf << 2) |
                     (cpu->nf << 1) |  cpu->cf);
}

/* Unpack one byte → Z80 flag bitfields. */
static inline void _z80_unpack_f(z80 *cpu, uint8_t f) {
    cpu->sf = (f >> 7) & 1;
    cpu->zf = (f >> 6) & 1;
    cpu->yf = (f >> 5) & 1;
    cpu->hf = (f >> 4) & 1;
    cpu->xf = (f >> 3) & 1;
    cpu->pf = (f >> 2) & 1;
    cpu->nf = (f >> 1) & 1;
    cpu->cf =  f       & 1;
}

void msx_debug_set_cpu(msx_state_t *msx, uint16_t pc, uint16_t sp,
                        uint8_t a, uint8_t f, uint16_t bc, uint16_t de,
                        uint16_t hl, uint16_t ix, uint16_t iy) {
    z80 *cpu = &msx->cpu;
    cpu->pc = pc;
    cpu->sp = sp;
    cpu->a  = a;
    _z80_unpack_f(cpu, f);
    cpu->b = (uint8_t)(bc >> 8); cpu->c = (uint8_t)bc;
    cpu->d = (uint8_t)(de >> 8); cpu->e = (uint8_t)de;
    cpu->h = (uint8_t)(hl >> 8); cpu->l = (uint8_t)hl;
    cpu->ix = ix;
    cpu->iy = iy;
}

size_t msx_save_state_size(void) { return sizeof(msx_save_t); }
size_t msx_save_state_header_size(void) { return MSX_SAVE_HDR_SZ; }

/* Everything in msx_save_t except ram[] — MSX_SAVE_HDR_SZ (64) bytes.
 * Split out so callers can save/load the ~64KB RAM image via a direct,
 * zero-copy view of msx->ram (already-live emulator memory) instead of
 * needing a second ~64KB buffer just for state I/O. */
bool msx_save_state_header(const msx_state_t *msx, void *buf, size_t buf_size) {
    if (!msx->initialized || !msx->bios_loaded) return false;
    if (buf_size < MSX_SAVE_HDR_SZ) return false;

    msx_save_t *s = (msx_save_t *)buf;
    memcpy(s->magic, MSX_SAVE_MAGIC, 8);

    /* Z80 */
    s->pc  = msx->cpu.pc;   s->sp  = msx->cpu.sp;
    s->ix  = msx->cpu.ix;   s->iy  = msx->cpu.iy;
    s->a   = msx->cpu.a;    s->f   = _z80_pack_f(&msx->cpu);
    s->b   = msx->cpu.b;    s->c   = msx->cpu.c;
    s->d   = msx->cpu.d;    s->e   = msx->cpu.e;
    s->h   = msx->cpu.h;    s->l   = msx->cpu.l;
    s->a_  = msx->cpu.a_;   s->f_  = msx->cpu.f_;
    s->b_  = msx->cpu.b_;   s->c_  = msx->cpu.c_;
    s->d_  = msx->cpu.d_;   s->e_  = msx->cpu.e_;
    s->h_  = msx->cpu.h_;   s->l_  = msx->cpu.l_;
    s->i   = msx->cpu.i;    s->r   = msx->cpu.r;
    s->iff1     = (uint8_t)msx->cpu.iff1;
    s->iff2     = (uint8_t)msx->cpu.iff2;
    s->int_mode = msx->cpu.interrupt_mode;
    s->halted   = (uint8_t)msx->cpu.halted;

    /* I/O */
    s->slot_select = msx->slot_select;
    s->psg_reg     = msx->psg_reg;
    s->key_row     = msx->key_row;
    s->ppi_c       = msx->ppi_c;

    /* VDP registers */
    for (int i = 0; i < 8; i++)
        s->vdp_regs[i] = vrEmuTms9918RegValue(msx->vdp, (vrEmuTms9918Register)i);

    /* Mapper banks */
    memcpy(s->cart_bank, msx->cart_bank, sizeof(msx->cart_bank));

    memset(s->_pad, 0, sizeof(s->_pad));
    return true;
}

bool msx_load_state_header(msx_state_t *msx, const void *buf, size_t buf_size) {
    if (buf_size < MSX_SAVE_HDR_SZ) return false;

    const msx_save_t *s = (const msx_save_t *)buf;
    if (memcmp(s->magic, MSX_SAVE_MAGIC, 8) != 0) return false;

    /* Z80 — re-assign callbacks after init */
    z80_init(&msx->cpu);
    msx->cpu.read_byte  = msx_mem_read;
    msx->cpu.write_byte = msx_mem_write;
    msx->cpu.port_in    = msx_port_read;
    msx->cpu.port_out   = msx_port_write;
    msx->cpu.userdata   = msx;

    msx->cpu.pc = s->pc;   msx->cpu.sp = s->sp;
    msx->cpu.ix = s->ix;   msx->cpu.iy = s->iy;
    msx->cpu.a  = s->a;    _z80_unpack_f(&msx->cpu, s->f);
    msx->cpu.b  = s->b;    msx->cpu.c  = s->c;
    msx->cpu.d  = s->d;    msx->cpu.e  = s->e;
    msx->cpu.h  = s->h;    msx->cpu.l  = s->l;
    msx->cpu.a_ = s->a_;   msx->cpu.f_ = s->f_;
    msx->cpu.b_ = s->b_;   msx->cpu.c_ = s->c_;
    msx->cpu.d_ = s->d_;   msx->cpu.e_ = s->e_;
    msx->cpu.h_ = s->h_;   msx->cpu.l_ = s->l_;
    msx->cpu.i  = s->i;    msx->cpu.r  = s->r;
    msx->cpu.iff1           = (bool)s->iff1;
    msx->cpu.iff2           = (bool)s->iff2;
    msx->cpu.interrupt_mode = s->int_mode;
    msx->cpu.halted         = (bool)s->halted;

    /* I/O */
    msx->slot_select = s->slot_select;
    msx->psg_reg     = s->psg_reg;
    msx->key_row     = s->key_row;
    msx->ppi_c       = s->ppi_c;

    /* Mapper banks */
    memcpy(msx->cart_bank, s->cart_bank, sizeof(msx->cart_bank));

    /* VDP — soft-reset, then restore R0-R7 via the hardware protocol:
     * send value byte first, then (0x80 | reg_num) to commit.
     * VRAM content is NOT restored (program redraws on next frame). */
    vrEmuTms9918Reset(msx->vdp);
    for (int i = 0; i < 8; i++) {
        vrEmuTms9918WriteAddr(msx->vdp, s->vdp_regs[i]);
        vrEmuTms9918WriteAddr(msx->vdp, (uint8_t)(0x80 | i));
    }

    return true;
}

bool msx_save_state(const msx_state_t *msx, void *buf, size_t buf_size) {
    if (buf_size < sizeof(msx_save_t)) return false;
    if (!msx_save_state_header(msx, buf, buf_size)) return false;
    memcpy(((msx_save_t *)buf)->ram, msx->ram, MSX_RAM_SIZE);
    return true;
}

bool msx_load_state(msx_state_t *msx, const void *buf, size_t buf_size) {
    if (buf_size < sizeof(msx_save_t)) return false;
    if (!msx_load_state_header(msx, buf, buf_size)) return false;
    memcpy(msx->ram, ((const msx_save_t *)buf)->ram, MSX_RAM_SIZE);
    return true;
}
