/*
 * cart_xip.c — Mega ROM resident in memory-mapped (XIP) flash.
 *
 * 2026-11-10. Why: the RAM page cache (msx_load_cart_paged(), 4 x 8KB
 * windows, no victim pool) re-reads 8KB from onboard flash on nearly
 * every bank switch for titles that flip one window between pages —
 * real hardware, NEMESIS2.ROM (Konami SCC): ~31 fetches/frame (~0.5ms
 * each) in heavy scenes -> ~28 FPS. Keeping the whole ROM in flash and
 * pointing the windows straight into the XIP mapping makes a bank switch
 * a pointer update (msx_core.c cart_page_refill()), with no RAM cost.
 *
 * Where: the otherwise unused gap between the end of the firmware image
 * (__flash_binary_end) and the start of MicroPython's filesystem
 * (MICROPY_HW_FLASH_STORAGE_BASE, minus any ROMFS). Computed at run time
 * from the same symbols/macros rp2_flash.c uses — no build/linker/board
 * setting is changed (an earlier fixed-region attempt that did change
 * build settings was reverted, see doc/architecture.md). On Pico 2
 * (4MB flash, 3MB filesystem, ~0.47MB firmware) that's ~0.5MB.
 *
 * Layout: sector 0 = header (magic, ROM size, source path, firmware end
 * address); ROM from sector 1. The header is erased first and written
 * last, so an interrupted copy is never mistaken for a valid one; the
 * recorded firmware end invalidates the copy if a bigger firmware image
 * may have overwritten its start.
 *
 * Flash erase/program follows rp2_flash.c's own critical section
 * (other core locked out if it registered as a lockout victim, IRQs off,
 * then flash timing restored via rp2_flash_set_timing()). Not used on
 * PiZero (core1 drives DVI there; Mega ROMs are refused on that board
 * anyway — see modmsx_ui.c).
 */
#include "cart_xip.h"

#include <string.h>
#include "mpconfigport.h"
#include "pico.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "hardware/regs/addressmap.h"
#include "pico/multicore.h"

#ifndef MICROPY_HW_FLASH_STORAGE_BASE
#define MICROPY_HW_FLASH_STORAGE_BASE (PICO_FLASH_SIZE_BYTES - MICROPY_HW_FLASH_STORAGE_BYTES)
#endif
#ifndef MICROPY_HW_ROMFS_BYTES
#define MICROPY_HW_ROMFS_BYTES 0
#endif

extern void rp2_flash_set_timing(void);   /* ports/rp2/rp2_flash.h */
extern char __flash_binary_end;

#define XIP_SECTOR      FLASH_SECTOR_SIZE   /* 4096 */
#define XIP_HDR_MAGIC   "MSXXIP01"
#define XIP_PATH_MAX    200

typedef struct {
    char     magic[8];
    uint32_t rom_size;
    uint32_t fw_end;        /* firmware image end (flash offset) when written */
    uint16_t path_len;
    char     path[XIP_PATH_MAX];
} xip_hdr_t;               /* < FLASH_PAGE_SIZE (256), programmed as one page */

static uint32_t fw_end_offset(void) {
    return (uint32_t)((uintptr_t)&__flash_binary_end - XIP_BASE);
}

bool cart_xip_region(uint32_t *offset, uint32_t *len) {
#if defined(MSX_BOARD_PIZERO) && MSX_BOARD_PIZERO
    (void)offset; (void)len;
    return false;
#else
    uint32_t start = (fw_end_offset() + XIP_SECTOR - 1) & ~(uint32_t)(XIP_SECTOR - 1);
    uint32_t end   = (uint32_t)(MICROPY_HW_FLASH_STORAGE_BASE - MICROPY_HW_ROMFS_BYTES);
    end &= ~(uint32_t)(XIP_SECTOR - 1);
    if (end <= start || end - start < 16u * XIP_SECTOR) return false;
    *offset = start;
    *len    = end - start;
    return true;
#endif
}

uint32_t cart_xip_capacity(void) {
    uint32_t off, len;
    if (!cart_xip_region(&off, &len)) return 0;
    return len - XIP_SECTOR;
}

static const xip_hdr_t *hdr_ptr(uint32_t off) {
    return (const xip_hdr_t *)(XIP_BASE + off);
}

const uint8_t *cart_xip_lookup(const char *path, size_t path_len, uint32_t size) {
    uint32_t off, len;
    if (!cart_xip_region(&off, &len)) return NULL;
    if (path_len > XIP_PATH_MAX || size == 0 || size > len - XIP_SECTOR) return NULL;
    const xip_hdr_t *h = hdr_ptr(off);
    if (memcmp(h->magic, XIP_HDR_MAGIC, 8) != 0) return NULL;
    if (h->rom_size != size || h->fw_end != fw_end_offset()) return NULL;
    if (h->path_len != path_len || memcmp(h->path, path, path_len) != 0) return NULL;
    return (const uint8_t *)(XIP_BASE + off + XIP_SECTOR);
}

/* rp2_flash.c's begin/end_critical_flash_section(), minus PSRAM (not
 * enabled on these boards). */
static uint32_t flash_begin(bool *locked) {
    *locked = multicore_lockout_victim_is_initialized(1 - get_core_num());
    if (*locked) multicore_lockout_start_blocking();
    return save_and_disable_interrupts();
}

static void flash_end(uint32_t irq, bool locked) {
    restore_interrupts(irq);
    if (locked) multicore_lockout_end_blocking();
    /* The bootrom erase/program calls reset QMI to slow default timings. */
    rp2_flash_set_timing();
}

static void erase_sector(uint32_t off) {
    bool locked;
    uint32_t irq = flash_begin(&locked);
    flash_range_erase(off, XIP_SECTOR);
    flash_end(irq, locked);
}

static void program(uint32_t off, const uint8_t *buf, uint32_t n) {
    bool locked;
    uint32_t irq = flash_begin(&locked);
    flash_range_program(off, buf, n);
    flash_end(irq, locked);
}

bool cart_xip_begin(uint32_t size) {
    uint32_t off, len;
    if (!cart_xip_region(&off, &len)) return false;
    if (size == 0 || size > len - XIP_SECTOR) return false;
    erase_sector(off);   /* invalidate any previous copy first */
    return true;
}

bool cart_xip_write_sector(uint32_t index, const uint8_t *buf) {
    uint32_t off, len;
    if (!cart_xip_region(&off, &len)) return false;
    uint32_t dst = off + XIP_SECTOR + index * XIP_SECTOR;
    if (dst < off + XIP_SECTOR || dst + XIP_SECTOR > off + len) return false;  /* bounds */
    erase_sector(dst);
    program(dst, buf, XIP_SECTOR);
    return true;
}

const uint8_t *cart_xip_commit(const char *path, size_t path_len, uint32_t size,
                               uint8_t *scratch_page) {
    uint32_t off, len;
    if (!cart_xip_region(&off, &len)) return NULL;
    if (path_len > XIP_PATH_MAX) return NULL;
    memset(scratch_page, 0xFF, FLASH_PAGE_SIZE);
    xip_hdr_t *h = (xip_hdr_t *)scratch_page;
    memcpy(h->magic, XIP_HDR_MAGIC, 8);
    h->rom_size = size;
    h->fw_end   = fw_end_offset();
    h->path_len = (uint16_t)path_len;
    memcpy(h->path, path, path_len);
    program(off, scratch_page, FLASH_PAGE_SIZE);
    return cart_xip_lookup(path, path_len, size);
}
