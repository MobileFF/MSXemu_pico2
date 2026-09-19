/*
 * wd179x.c — see wd179x.h for the design rationale/scope.
 */
#include <string.h>
#include "wd179x.h"

/* -----------------------------------------------------------------------
 * Status register bits. Two different bit meanings depending on which
 * command last ran (real WD179x behavior) — Type I (seek/step family) vs
 * Type II/III (read/write/format family). Callers only ever read the
 * single `status` byte; which meaning applies is implicit in what
 * command was issued last, exactly like the real chip.
 * ----------------------------------------------------------------------- */
#define ST_BUSY             0x01  /* both formats */
/* Type I */
#define ST1_INDEX           0x02
#define ST1_TRACK00         0x04
#define ST1_CRC_ERROR       0x08
#define ST1_SEEK_ERROR      0x10
#define ST1_HEAD_LOADED     0x20
#define ST1_WRITE_PROTECT   0x40
#define ST1_NOT_READY       0x80
/* Type II/III */
#define ST2_DRQ             0x02
#define ST2_LOST_DATA       0x04
#define ST2_CRC_ERROR       0x08
#define ST2_RECORD_NOT_FOUND 0x10
#define ST2_RECORD_TYPE     0x20
#define ST2_WRITE_PROTECT   0x40
#define ST2_NOT_READY       0x80

/* Drive-control latch (register +4) — NOT part of the WD179x chip
 * itself; every real MSX FDC interface board adds its own latch for
 * drive select / side select / motor-on, decoded separately from the
 * chip's 4 registers. Bit assignment is interface-specific; bit2 is
 * this Disk ROM's side-select bit, confirmed against real hardware for
 * both sides (bit1, an earlier guess, only coincidentally matched side 0
 * — see git history if the exact repro matters again). */
#define FDC_CTRL_SIDE_BIT   0x04

static uint8_t side_from_latch(uint8_t latch) {
    return (latch & FDC_CTRL_SIDE_BIT) ? 1 : 0;
}

static uint32_t compute_lba(const msx_fdc_t *fdc, uint8_t side) {
    /* Standard CHS -> LBA for a flat, non-interleaved sector-dump image:
     * sectors are 1-based (WD179x/FAT convention), tracks and sides are
     * 0-based. */
    uint32_t spt   = fdc->sectors_per_track ? fdc->sectors_per_track : 9;
    uint32_t sides = fdc->num_sides ? fdc->num_sides : 2;
    uint32_t sector0 = (fdc->sector_reg > 0) ? (fdc->sector_reg - 1) : 0;
    return ((uint32_t)fdc->current_track * sides + side) * spt + sector0;
}

void msx_fdc_reset(msx_fdc_t *fdc) {
    bool          enabled  = fdc->enabled;
    uint16_t      base     = fdc->base_addr;
    uint8_t       spt      = fdc->sectors_per_track;
    uint8_t       sides    = fdc->num_sides;
    msx_fdc_io_fn cb       = fdc->io_cb;
    void         *userdata = fdc->io_userdata;

    memset(fdc, 0, sizeof(*fdc));

    fdc->enabled            = enabled;
    fdc->base_addr          = base;
    fdc->sectors_per_track  = spt;
    fdc->num_sides          = sides;
    fdc->io_cb              = cb;
    fdc->io_userdata        = userdata;
    fdc->status             = ST1_TRACK00;  /* head at track 0 after reset, not busy */
}

void msx_fdc_enable(msx_fdc_t *fdc, uint16_t base_addr,
                    uint8_t sectors_per_track, uint8_t num_sides) {
    fdc->enabled           = true;
    fdc->base_addr         = base_addr;
    fdc->sectors_per_track = sectors_per_track ? sectors_per_track : 9;
    fdc->num_sides         = num_sides ? num_sides : 2;
    fdc->status            = ST1_TRACK00;
    fdc->current_track     = 0;
    fdc->track_reg         = 0;
    fdc->transfer_active   = false;
}

void msx_fdc_disable(msx_fdc_t *fdc) {
    fdc->enabled = false;
    fdc->io_cb   = NULL;
}

void msx_fdc_set_io_cb(msx_fdc_t *fdc, msx_fdc_io_fn cb, void *userdata) {
    fdc->io_cb       = cb;
    fdc->io_userdata = userdata;
}

/* Real WD179x registers are physically wired into the FDC interface
 * board, memory-mapped into whichever cart-slot page that board's ROM
 * currently occupies — they move WITH the page, at a fixed offset within
 * it, never at one fixed absolute Z80 address. MSXDOS.SYS's own loaded
 * code re-pages the Disk ROM from page 1 (0x4000-0x7FFF, where boot-time
 * code accesses the FDC) into page 2 (0x8000-0xBFFF) once it takes over,
 * and keeps talking to the FDC there (same in-ROM byte offset, different
 * window) — so this compares addr's offset within its containing 16KB
 * page against base_addr's offset within its page, not the two raw
 * addresses. msx_mem_read()/msx_mem_write() already only call this when
 * the current page's slot is cart slot 1 (see their `case 1:` branches),
 * so no separate page-vs-slot check is needed here. */
bool msx_fdc_addr_in_range(const msx_fdc_t *fdc, uint16_t addr) {
    uint16_t base_off = fdc->base_addr & 0x3FFF;
    uint16_t addr_off = addr & 0x3FFF;
    return fdc->enabled && addr_off >= base_off &&
           addr_off < (uint16_t)(base_off + MSX_FDC_NUM_REGS);
}

/* -----------------------------------------------------------------------
 * Command execution (register +0 write). Commands complete synchronously
 * — no real-time motor/seek delay simulation (see wd179x.h). Read/Write
 * Sector leave transfer_active set so the subsequent 512 Data-register
 * accesses (handled in msx_fdc_read()/msx_fdc_write() below) can drive
 * the byte-at-a-time transfer exactly like a real driver's DRQ-polling
 * loop expects.
 * ----------------------------------------------------------------------- */
static void exec_command(msx_fdc_t *fdc, uint8_t cmd) {
    fdc->transfer_active = false;

    if (cmd < 0x80) {
        /* ---- Type I: Restore / Seek / Step family ---- */
        uint8_t top = (uint8_t)(cmd >> 4);
        if (top == 0x0) {                       /* Restore: seek to track 0 */
            fdc->current_track = 0;
            fdc->track_reg     = 0;
        } else if (top == 0x1) {                /* Seek: target track preloaded in Data reg */
            fdc->current_track = fdc->data_reg;
            fdc->track_reg     = fdc->current_track;
        } else {
            /* Step/Step-In/Step-Out (0x20-0x7F). Direction: Step-In
             * (0100-0101xxxx) moves toward higher track numbers,
             * Step-Out (0110-0111xxxx) toward track 0. Plain Step
             * (0010-0011xxxx, "repeat last direction") isn't tracked in
             * this simplified model — treated as a no-op distance, which
             * is harmless for standard MSX-DOS FAT12 access (drivers use
             * Seek, not a manual Step sequence, for anything but fine
             * head-settling adjustments this emulator doesn't need). */
            uint8_t grp = (uint8_t)(cmd >> 5);
            int     dir = 0;
            if (grp == 0x2) dir = +1;            /* Step-In  (0x40-0x5F) */
            else if (grp == 0x3) dir = -1;        /* Step-Out (0x60-0x7F) */
            if (dir != 0) {
                int t = (int)fdc->current_track + dir;
                if (t < 0) t = 0;
                if (t > 255) t = 255;
                fdc->current_track = (uint8_t)t;
            }
            if (cmd & 0x10) fdc->track_reg = fdc->current_track;  /* "U" update-track-register bit */
        }
        fdc->status = (fdc->current_track == 0) ? ST1_TRACK00 : 0x00;
        return;
    }

    if ((cmd & 0xF0) == 0xD0) {                  /* Type IV: Force Interrupt — abort */
        fdc->transfer_active = false;
        fdc->status = 0x00;
        return;
    }

    uint8_t side = side_from_latch(fdc->control_latch);

    if ((cmd & 0xE0) == 0x80) {                  /* Type II: Read Sector */
        uint32_t lba = compute_lba(fdc, side);
        bool ok = fdc->io_cb && fdc->io_cb(fdc->io_userdata, lba, false, fdc->sector_buf);
        if (ok) {
            fdc->buf_pos          = 0;
            fdc->transfer_active  = true;
            fdc->transfer_is_write = false;
            fdc->status = ST_BUSY | ST2_DRQ;
        } else {
            fdc->status = ST2_RECORD_NOT_FOUND;
        }
        return;
    }

    if ((cmd & 0xE0) == 0xA0) {                  /* Type II: Write Sector */
        /* Fetched lazily once all 512 bytes have arrived (see
         * msx_fdc_write()'s Data-register handling) rather than here —
         * nothing to write to backing storage yet. */
        fdc->buf_pos           = 0;
        fdc->transfer_active   = true;
        fdc->transfer_is_write = true;
        fdc->status = ST_BUSY | ST2_DRQ;
        return;
    }

    /* Type III (Read Address / Read Track / Write Track): decoded but
     * not implemented — standard MSX-DOS FAT12 access never issues
     * these. Complete immediately with no error rather than leaving a
     * driver that DOES try one stuck forever. */
    fdc->status = 0x00;
}

uint8_t msx_fdc_read(msx_fdc_t *fdc, uint16_t addr) {
    /* Page-relative, matching msx_fdc_addr_in_range() — see its comment. */
    uint8_t reg = (uint8_t)((addr & 0x3FFF) - (fdc->base_addr & 0x3FFF));
    switch (reg) {
    case 0:
        /* See index_toggle's comment in wd179x.h. Only outside an active
         * Read/Write Sector transfer — bit1 means DRQ, not INDEX, in
         * that context, and is already handled by transfer_active/
         * buf_pos elsewhere. */
        if (!fdc->transfer_active) {
            fdc->index_toggle = !fdc->index_toggle;
            return (uint8_t)((fdc->status & ~ST1_INDEX) |
                              (fdc->index_toggle ? ST1_INDEX : 0));
        }
        return fdc->status;
    case 1: return fdc->track_reg;
    case 2: return fdc->sector_reg;
    case 3:
        if (fdc->transfer_active && !fdc->transfer_is_write) {
            uint8_t b = fdc->sector_buf[fdc->buf_pos];
            fdc->data_reg = b;
            fdc->buf_pos++;
            if (fdc->buf_pos >= MSX_FDC_SECTOR_SIZE) {
                fdc->transfer_active = false;
                fdc->status = 0x00;             /* transfer complete, not busy */
            }
            return b;
        }
        return fdc->data_reg;
    case 4:
        /* This Disk ROM reads register +4 back (indirectly, via
         * `LD A,(BC)` with BC preloaded) as its per-byte DRQ-wait loop
         * during a sector transfer:
         *
         *   763A: LD A,(BC) / ADD A,A / RET C / JP M,763A
         *   7640: LD A,(DE) / LD (HL),A / INC HL / JP 763A
         *
         * — bit7 (tested via the post-shift Carry) signals "transfer
         * over, stop asking" (RET C ends the loop); bit6 (tested via the
         * post-shift Sign) is DRQ, "wait, no byte ready yet" (JP M loops
         * back). Bit6 is always clear here (DRQ instantly ready — every
         * command completes synchronously, so there's never actually
         * anything to wait for); bit7 mirrors !transfer_active (clear
         * while a Read/Write Sector transfer is still consuming bytes,
         * set the instant it's done or when no transfer is active at
         * all) so the loop above terminates at the correct byte, neither
         * early nor forever. Bits 0-5 (whatever the driver itself last
         * wrote for drive/motor/side select) are still echoed
         * faithfully. */
        return (fdc->transfer_active ? 0x00 : 0x80) | (fdc->control_latch & 0x3F);
    default: return 0xFF;
    }
}

void msx_fdc_write(msx_fdc_t *fdc, uint16_t addr, uint8_t value) {
    /* Page-relative, matching msx_fdc_addr_in_range() — see its comment. */
    uint8_t reg = (uint8_t)((addr & 0x3FFF) - (fdc->base_addr & 0x3FFF));
    switch (reg) {
    case 0:
        exec_command(fdc, value);
        break;
    case 1:
        fdc->track_reg = value;
        break;
    case 2:
        fdc->sector_reg = value;
        break;
    case 3:
        fdc->data_reg = value;
        if (fdc->transfer_active && fdc->transfer_is_write) {
            fdc->sector_buf[fdc->buf_pos] = value;
            fdc->buf_pos++;
            if (fdc->buf_pos >= MSX_FDC_SECTOR_SIZE) {
                uint8_t  side = side_from_latch(fdc->control_latch);
                uint32_t lba  = compute_lba(fdc, side);
                bool ok = fdc->io_cb &&
                          fdc->io_cb(fdc->io_userdata, lba, true, fdc->sector_buf);
                fdc->transfer_active = false;
                fdc->status = ok ? 0x00 : ST2_WRITE_PROTECT;
            }
        }
        break;
    case 4:
        fdc->control_latch = value;
        break;
    default:
        break;
    }
}
