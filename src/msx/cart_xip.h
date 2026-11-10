/* cart_xip.h — Mega ROM resident in memory-mapped flash, see cart_xip.c */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Free flash gap between the firmware image and the filesystem (flash
 * offsets). false if unavailable on this board/build. */
bool cart_xip_region(uint32_t *offset, uint32_t *len);

/* Largest ROM that fits (0 = feature unavailable). */
uint32_t cart_xip_capacity(void);

/* XIP pointer to the ROM if the region already holds a complete copy of
 * (path, size) written by this firmware image; NULL otherwise. */
const uint8_t *cart_xip_lookup(const char *path, size_t path_len, uint32_t size);

/* Copy protocol: begin() (invalidates the old copy), write_sector() for
 * each 4KB ROM sector in order (pad the last one with 0xFF), then
 * commit() (writes the header, returns the XIP pointer or NULL).
 * scratch_page: >= 256 bytes of RAM for commit()'s header page. */
bool cart_xip_begin(uint32_t size);
bool cart_xip_write_sector(uint32_t index, const uint8_t *buf);
const uint8_t *cart_xip_commit(const char *path, size_t path_len, uint32_t size,
                               uint8_t *scratch_page);
