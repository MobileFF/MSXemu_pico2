/* scc_out.h — SCC external output (PicoChiptuneOrchestra slave_scc), see scc_out.c */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "pico/types.h"

/* Configure 3 GPIOs as a write-only bit-banged SPI master (mode 1) and
 * hook it up as msx_core's SCC output callback. baud 0 = default (~3MHz).
 * Sends NOP + RESET to the slave. Returns false on invalid pins. */
bool scc_out_init(uint sck, uint mosi, uint cs, uint32_t baud);

/* Mute the slave, unhook the callback and release the pins. */
void scc_out_deinit(void);

bool scc_out_active(void);
