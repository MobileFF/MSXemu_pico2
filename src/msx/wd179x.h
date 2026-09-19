/*
 * wd179x.h — WD1793/WD2793-compatible FDC register emulation
 *
 * Clean-room implementation from the publicly documented WD179x family
 * datasheet-level command/status/register behavior (Type I: Restore/Seek/
 * Step*; Type II: Read/Write Sector; Type IV: Force Interrupt) — not
 * ported from any other emulator's source (this project is MIT-licensed;
 * openMSX, a natural reference for "how does a real MSX emulate this
 * chip", is GPL-2.0, so only its general approach — not its code — was a
 * design input here).
 *
 * Exists because a real Disk ROM's own boot-time code talks directly to
 * FDC hardware registers (memory-mapped into its own cart slot's ROM
 * page) rather than exclusively through the higher-level DSKIO/DSKCHG/
 * GETDPB BDOS entry points — confirmed by disassembling the ROM this was
 * built against: a `LD A,(7FB8) / RRA / JR C,-6` busy-wait loop against
 * what turned out to be a memory-mapped FDC register, plus 30+ other
 * read/write sites in the same small address range, is a textbook
 * WD179x STATUS register BUSY-bit poll. A BDOS-level hook (mp/msx_fdd.py's
 * DSKCHG hook is the one remaining exception — see its own docstring)
 * can never intercept that; only emulating the actual chip does.
 *
 * Scope (first pass): Restore, Seek, Step/Step-In/Step-Out (coarse — no
 * direction memory for the plain "Step" repeat-last-direction form),
 * Read Sector, Write Sector (single-sector only), Force Interrupt. Read
 * Address / Read Track / Write Track are decoded but complete as no-ops
 * (fine for ordinary MSX-DOS FAT12 file access, which never uses them).
 * No real-time motor-spin-up/seek-timing simulation — commands complete
 * synchronously within the register write that issues them. Verified
 * against real hardware for both Disk BASIC SAVE/LOAD and a full
 * MSX-DOS boot (MSXDOS.SYS + COMMAND.COM load, both disk sides).
 *
 * The drive-control latch (register +4) exists on essentially every real
 * MSX FDC interface board (motor/drive/side select — not part of the
 * WD179x chip itself, which only has 4 registers) but its exact bit
 * assignment is interface-specific; see FDC_CTRL_SIDE_BIT in wd179x.c.
 */
#ifndef MSX_WD179X_H_
#define MSX_WD179X_H_

#include <stdint.h>
#include <stdbool.h>

#define MSX_FDC_SECTOR_SIZE 512u
#define MSX_FDC_NUM_REGS    5u   /* Status/Command, Track, Sector, Data, drive-control latch */

/* Sector I/O callback — given a flat 0-based logical sector number
 * (already translated from track/side/sector via the configured
 * geometry, see compute_lba() in wd179x.c), either fill buf with 512
 * bytes read from backing storage (is_write=false) or write buf's 512
 * bytes to backing storage (is_write=true). Return false on any failure
 * (no disk mounted, I/O error, out-of-range sector) — the FDC then
 * reports RECORD NOT FOUND, same as a real drive that can't find the
 * requested sector. This is a plain C function pointer (not a MicroPython
 * mp_obj_t) so this module has no MicroPython dependency, matching
 * msx_cart_fetch_fn's separation — the Python-object-holding glue lives
 * in modmsx.c (see fdc_sector_io_from_pyfile there), not here. */
typedef bool (*msx_fdc_io_fn)(void *userdata, uint32_t lba_sector,
                                bool is_write, uint8_t *buf);

typedef struct {
    bool     enabled;
    uint16_t base_addr;          /* Z80 address of register +0 (Status/Command),
                                   * within whichever 16KB page currently hosts
                                   * cart slot 1 — see msx_fdc_addr_in_range()'s
                                   * comment in the .c file for why this is
                                   * page-relative, not one fixed absolute
                                   * address. */

    uint8_t  status;
    uint8_t  track_reg;
    uint8_t  sector_reg;
    uint8_t  data_reg;
    uint8_t  control_latch;      /* drive/side/motor select — see .c file */

    uint8_t  current_track;      /* actual head position; Type I commands move this */
    uint8_t  sectors_per_track;  /* geometry, set via msx_fdc_enable() */
    uint8_t  num_sides;

    /* This Disk ROM's drive-presence check waits for the Type I status
     * register's INDEX bit (bit1) to pulse — go 1 then back to 0,
     * confirming a real disk is physically rotating — with a large but
     * finite retry budget before giving up ("not ready"). Toggled on
     * every Status-register read while no Read/Write Sector transfer is
     * active (that's Type II territory, where the same bit position
     * means DRQ instead — see msx_fdc_read()) — not tied to real elapsed
     * time at all, just "changes fast enough that a bounded polling loop
     * always sees it flip eventually", which is all real INDEX pulsing
     * needs to satisfy here. */
    bool     index_toggle;

    uint8_t  sector_buf[MSX_FDC_SECTOR_SIZE];
    uint16_t buf_pos;            /* byte offset into sector_buf during an active transfer */
    bool     transfer_active;
    bool     transfer_is_write;

    msx_fdc_io_fn io_cb;
    void         *io_userdata;
} msx_fdc_t;

/* Resets transient state (registers, in-flight transfer, head position)
 * but preserves enabled/base_addr/geometry/io_cb — mirrors how
 * msx_reset() preserves msx.set_call_hook() registrations across a
 * plain reset (only msx_init()'s full memset() clears those). Called
 * from msx_reset(). */
void msx_fdc_reset(msx_fdc_t *fdc);

/* Enables the FDC, memory-mapped at [base_addr, base_addr+5) within
 * whatever cart slot currently hosts the Disk ROM (see msx_core.c's
 * msx_mem_read()/msx_mem_write() cartridge-slot-1 branch — this project
 * always loads a Disk ROM into cart slot 1, so the FDC is checked there
 * specifically, not slot 2). sectors_per_track/num_sides come from the
 * mounted disk image's own boot-sector BPB (mp/msx_fdd.py parses it —
 * same fields already used for GETDPB before this rewrite); 0 for either
 * falls back to the standard 9 sectors/track, 2-sided convention. */
void msx_fdc_enable(msx_fdc_t *fdc, uint16_t base_addr,
                     uint8_t sectors_per_track, uint8_t num_sides);

void msx_fdc_disable(msx_fdc_t *fdc);

/* One callback serves all sector I/O (read and write share the is_write
 * flag) — see msx_fdc_io_fn's comment. Re-register after every
 * msx.init() (which zeroes this along with everything else), same
 * requirement as msx_set_cart_fetch_cb(). */
void msx_fdc_set_io_cb(msx_fdc_t *fdc, msx_fdc_io_fn cb, void *userdata);

/* True if `addr` falls within the FDC's mapped register window (and the
 * FDC is currently enabled) — msx_mem_read()/msx_mem_write() check this
 * before falling through to normal cart ROM/RAM handling for slot 1. */
bool msx_fdc_addr_in_range(const msx_fdc_t *fdc, uint16_t addr);

uint8_t msx_fdc_read(msx_fdc_t *fdc, uint16_t addr);
void    msx_fdc_write(msx_fdc_t *fdc, uint16_t addr, uint8_t value);

#endif /* MSX_WD179X_H_ */
