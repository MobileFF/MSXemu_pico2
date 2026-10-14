/*
 * modsdcard_c.c — C-implemented SD card SPI block device.
 *
 * 2026-10-13: a from-scratch C port of mp/sdcard.py, written after a round
 * of Python-level tuning (sleep removal + SD_DATA_BAUD 4->12MHz, both
 * real-hardware confirmed, see CHANGELOG.md) showed that 12MHz throughput
 * plateaus well below the SPI clock's own theoretical ceiling — i.e. the
 * remaining bottleneck above 12MHz is per-block/per-command overhead
 * (Python interpreter dispatch, machine.SPI's own per-call DMA channel
 * claim/unclaim), not the SPI bus itself. This module targets exactly
 * that overhead:
 *
 *   - Two DMA channels are claimed ONCE, at construction time, and reused
 *     for the lifetime of the object — machine.SPI's own transfer path
 *     (ports/rp2/machine_spi.c's machine_spi_transfer()) claims/unclaims a
 *     fresh pair on *every single* >=32-byte transfer, which this driver's
 *     bulk per-block transfers are (512 bytes each).
 *   - The whole SD SPI protocol (command bytes, R1/data-token polling,
 *     busy-wait) runs as plain C function calls instead of MicroPython
 *     bytecode, removing interpreter dispatch overhead from the hot path
 *     entirely.
 *
 * This is a NEW, separately-named module (`sdcard_c`, class `SDCard`) —
 * NOT a drop-in replacement for mp/sdcard.py's `sdcard.SDCard` class (the
 * constructor signature differs, see below). mp/main.py's mount_sd() picks
 * whichever is available, falling back to the proven pure-Python driver if
 * this module raises or isn't present (e.g. an older firmware build).
 *
 * Constructor: sdcard_c.SDCard(spi_id, cs_pin, baudrate=.., restore_baudrate=..)
 *   - spi_id: 0 or 1 (matches machine.SPI's own numbering).
 *   - cs_pin: a raw GPIO number (int) — NOT a machine.Pin object. This
 *     driver owns the CS pin entirely via direct gpio_init()/gpio_put()
 *     calls, independent of any Python-side Pin wrapper.
 *   - IMPORTANT: unlike mp/sdcard.py, this constructor does NOT take a
 *     machine.SPI object and does NOT itself call spi_init() or configure
 *     the SCK/MOSI/MISO pins' GPIO function — it assumes a
 *     machine.SPI(spi_id, ...) object has ALREADY been constructed once by
 *     the caller (exactly as mount_sd() in mp/main.py already does before
 *     building the SDCard object), which is what actually performs that
 *     one-time pin-function/spi_init() setup. From that point on, this
 *     driver "takes over" the raw spi_inst_t* directly, the same
 *     established convention src/msx/msx_core.c already uses to share
 *     SPI1 with the LCD/HDMI code (see its own comment: "Configure SPI
 *     without calling spi_init() ... Python's spi_init already set the
 *     format"). Every transaction here still explicitly re-applies 8-bit/
 *     CPOL0/CPHA0/MSB-first before touching the bus and restores
 *     restore_baudrate's rate+format afterward, matching mp/sdcard.py's own
 *     defensive discipline (see doc/sdcard_spi_mode_bug.md) — required
 *     because this SPI bus is shared with the LCD (always mode 0) and,
 *     when the optional HDMI bridge is enabled, with code that temporarily
 *     switches it to mode 3 (msx_core.c's hdmi_apply_spi_settings()).
 *
 * Behavioural note (deliberate improvement over mp/sdcard.py): the
 * original Python write() silently swallows a bad data-response token (no
 * exception raised at all, even for a multi-block CMD25 write) — this port
 * instead treats that as a failure and raises OSError(EIO), which is more
 * correct. Since SD writes only happen for save-states (not performance-
 * critical, not the hot path this module targets), this doesn't trade any
 * speed for the improvement.
 */

#include <string.h>

#include "py/runtime.h"
#include "py/mperrno.h"
#include "extmod/vfs.h"

#include "hardware/spi.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "pico/stdlib.h"

// ---- SD SPI protocol constants (mirrors mp/sdcard.py) ----------------
#define SD_CMD_TIMEOUT      (500)
#define SD_RETRY_COUNT      (3)
#define SD_RETRY_DELAY_MS   (5)

// 2026-10-13: same reasoning as mp/sdcard.py's own _FAST_POLL (see that
// file's comment) — busy-poll for the data-start token instead of
// sleeping on every failed attempt, only falling back to a real sleep
// once the fast phase is exhausted.
#define SD_FAST_POLL        (100)

#define R1_IDLE_STATE        (1 << 0)
#define R1_ILLEGAL_COMMAND   (1 << 2)

#define TOKEN_CMD25      (0xFC)
#define TOKEN_STOP_TRAN  (0xFD)
#define TOKEN_DATA       (0xFE)

// DMA is only worth the setup cost above this many bytes (matches
// ports/rp2/machine_spi.c's own dma_min_size_threshold exactly, for the
// same reason: below this, blocking spi_write_read_blocking() is both
// simpler and about as fast).
#define SD_DMA_MIN_SIZE  (32)

typedef struct _sdcard_c_obj_t {
    mp_obj_base_t base;
    spi_inst_t *spi;
    uint cs_pin;
    uint32_t data_baud;
    uint32_t restore_baud;
    uint32_t sectors;
    uint32_t cdv;           // 512 for v1 cards (byte-addressed), 1 for v2/SDHC (block-addressed)
    int dma_tx_chan;        // persistently claimed for this object's lifetime; -1 if unavailable
    int dma_rx_chan;
} sdcard_c_obj_t;

// Forward declaration — MP_DEFINE_CONST_OBJ_TYPE() below expands to a
// non-static (external-linkage) definition (see py/obj.h), so this
// declaration must match (no `static`) or the two conflict.
extern const mp_obj_type_t sdcard_c_type;

// ---- low-level bus helpers ---------------------------------------------

static inline void cs_select(sdcard_c_obj_t *self) {
    gpio_put(self->cs_pin, 0);
}

static inline void cs_deselect_release(sdcard_c_obj_t *self) {
    gpio_put(self->cs_pin, 1);
    uint8_t ff = 0xFF;
    spi_write_blocking(self->spi, &ff, 1);
}

static inline void sdcard_apply_format(spi_inst_t *spi, uint32_t baud) {
    spi_set_baudrate(spi, baud);
    spi_set_format(spi, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
}

// Bulk transfer with persistent DMA channels (no per-call claim/unclaim —
// see this file's header comment for why that matters). Mirrors
// ports/rp2/machine_spi.c's machine_spi_transfer() exactly, minus the
// claim/unclaim, since self->dma_tx_chan/dma_rx_chan are already owned.
//   src == NULL  -> read-only transfer (TX drives constant 0xFF)
//   dest == NULL -> write-only transfer (RX discarded)
static void sdcard_spi_transfer(sdcard_c_obj_t *self, size_t len, const uint8_t *src, uint8_t *dest) {
    static const uint8_t ff_byte = 0xFF;
    bool read_only = (src == NULL);
    bool write_only = (dest == NULL);

    if (len < SD_DMA_MIN_SIZE || self->dma_tx_chan < 0 || self->dma_rx_chan < 0) {
        if (read_only) {
            spi_read_blocking(self->spi, 0xFF, dest, len);
        } else if (write_only) {
            spi_write_blocking(self->spi, src, len);
        } else {
            spi_write_read_blocking(self->spi, src, dest, len);
        }
        return;
    }

    uint8_t dev_null;

    dma_channel_config tx_cfg = dma_channel_get_default_config(self->dma_tx_chan);
    channel_config_set_transfer_data_size(&tx_cfg, DMA_SIZE_8);
    channel_config_set_dreq(&tx_cfg, spi_get_index(self->spi) ? DREQ_SPI1_TX : DREQ_SPI0_TX);
    channel_config_set_read_increment(&tx_cfg, !read_only);
    dma_channel_configure(self->dma_tx_chan, &tx_cfg,
        &spi_get_hw(self->spi)->dr,
        read_only ? &ff_byte : src,
        len,
        false);

    dma_channel_config rx_cfg = dma_channel_get_default_config(self->dma_rx_chan);
    channel_config_set_transfer_data_size(&rx_cfg, DMA_SIZE_8);
    channel_config_set_dreq(&rx_cfg, spi_get_index(self->spi) ? DREQ_SPI1_RX : DREQ_SPI0_RX);
    channel_config_set_read_increment(&rx_cfg, false);
    channel_config_set_write_increment(&rx_cfg, !write_only);
    dma_channel_configure(self->dma_rx_chan, &rx_cfg,
        write_only ? &dev_null : dest,
        &spi_get_hw(self->spi)->dr,
        len,
        false);

    dma_start_channel_mask((1u << self->dma_rx_chan) | (1u << self->dma_tx_chan));
    dma_channel_wait_for_finish_blocking(self->dma_tx_chan);

    // Wait for the SPI peripheral to finish shifting AND for the RX FIFO
    // to drain — by the time both are true, the RX DMA channel has
    // necessarily also finished (it services the FIFO on every byte it
    // receives). Matches machine_spi_transfer()'s own proven logic.
    while (spi_is_busy(self->spi) || spi_is_readable(self->spi)) {
    }
    if (spi_get_const_hw(self->spi)->ris & SPI_SSPRIS_RORRIS_BITS) {
        spi_get_hw(self->spi)->icr = SPI_SSPICR_RORIC_BITS;
        dma_channel_abort(self->dma_rx_chan);
    }
}

// Send a 6-byte SD command, poll for the R1 response (busy-poll, no
// sleep — matches mp/sdcard.py's cmd(), which never slept here either).
//   final > 0   : discard `final` more bytes after R1 (R3/R7 payload).
//   skip1       : read and discard one byte before starting the R1 poll
//                 (CMD12's stop-transmission quirk).
//   release     : deselect CS (and send the trailing 0xFF) before
//                 returning. Callers that need to read a data block right
//                 after (CMD17/18) pass release=false and deselect
//                 themselves once the data phase is done.
// Returns the R1 byte, or -1 on timeout (matches mp/sdcard.py's cmd()).
static int sdcard_cmd(sdcard_c_obj_t *self, uint8_t cmd, uint32_t arg, uint8_t crc,
                      int final, bool release, bool skip1) {
    cs_select(self);

    uint8_t buf[6];
    buf[0] = 0x40 | cmd;
    buf[1] = (uint8_t)(arg >> 24);
    buf[2] = (uint8_t)(arg >> 16);
    buf[3] = (uint8_t)(arg >> 8);
    buf[4] = (uint8_t)arg;
    buf[5] = crc;
    spi_write_blocking(self->spi, buf, 6);

    uint8_t token;
    if (skip1) {
        spi_read_blocking(self->spi, 0xFF, &token, 1);
    }

    int response = -1;
    for (int i = 0; i < SD_CMD_TIMEOUT; i++) {
        spi_read_blocking(self->spi, 0xFF, &token, 1);
        if (!(token & 0x80)) {
            response = token;
            uint8_t discard;
            for (int j = 0; j < final; j++) {
                spi_read_blocking(self->spi, 0xFF, &discard, 1);
            }
            break;
        }
    }

    // On a genuine R1 timeout, always release CS regardless of the
    // caller's `release` flag -- matches mp/sdcard.py's cmd(), whose
    // for/else timeout branch unconditionally does self.cs(1) (the
    // `release` arg there only governs the *success* path). A caller
    // passing release=false (CMD17/18, expecting to read a data block
    // right after) still wants CS deasserted if the command itself never
    // got a response at all -- leaving it asserted here would otherwise
    // leak into whatever the caller does next.
    if (release || response < 0) {
        cs_deselect_release(self);
    }
    return response;
}

// Read one data block (512 bytes) after a CMD17/18 that already got R1==0.
// Raises OSError on data-start-token timeout.
static void sdcard_readinto(sdcard_c_obj_t *self, uint8_t *buf, size_t len) {
    cs_select(self);

    uint8_t token = 0;
    int i;
    for (i = 0; i < SD_CMD_TIMEOUT; i++) {
        spi_read_blocking(self->spi, 0xFF, &token, 1);
        if (token == TOKEN_DATA) {
            break;
        }
        if (i >= SD_FAST_POLL) {
            sleep_ms(1);
        }
    }
    if (i == SD_CMD_TIMEOUT) {
        gpio_put(self->cs_pin, 1);
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("timeout waiting for response"));
    }

    sdcard_spi_transfer(self, len, NULL, buf);

    uint8_t csum[2];
    spi_read_blocking(self->spi, 0xFF, csum, 2);

    cs_deselect_release(self);
}

// Write one data block with the given start token; returns false if the
// card's data-response token indicates failure (caller raises OSError —
// see this file's header comment on this being a deliberate improvement
// over mp/sdcard.py's silent-swallow behaviour here).
static bool sdcard_write_block(sdcard_c_obj_t *self, uint8_t token, const uint8_t *buf, size_t len) {
    cs_select(self);

    spi_write_blocking(self->spi, &token, 1);
    sdcard_spi_transfer(self, len, buf, NULL);

    uint8_t csum[2] = {0xFF, 0xFF};
    spi_write_blocking(self->spi, csum, 2);

    uint8_t resp;
    spi_read_blocking(self->spi, 0xFF, &resp, 1);
    if ((resp & 0x1F) != 0x05) {
        cs_deselect_release(self);
        return false;
    }

    uint8_t busy;
    do {
        spi_read_blocking(self->spi, 0xFF, &busy, 1);
    } while (busy == 0x00);

    cs_deselect_release(self);
    return true;
}

static void sdcard_write_stop_token(sdcard_c_obj_t *self, uint8_t token) {
    cs_select(self);
    spi_write_blocking(self->spi, &token, 1);
    uint8_t ff = 0xFF;
    spi_write_blocking(self->spi, &ff, 1);

    uint8_t busy;
    do {
        spi_read_blocking(self->spi, 0xFF, &busy, 1);
    } while (busy == 0x00);

    cs_deselect_release(self);
}

// ---- card initialization (mirrors mp/sdcard.py's init_card()/
//      init_card_v1()/init_card_v2()) ----------------------------------

static void sdcard_init_card_v1(sdcard_c_obj_t *self) {
    for (int i = 0; i < SD_CMD_TIMEOUT; i++) {
        sdcard_cmd(self, 55, 0, 0, 0, true, false);
        if (sdcard_cmd(self, 41, 0, 0, 0, true, false) == 0) {
            self->cdv = 512;
            return;
        }
    }
    mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("timeout waiting for v1 card"));
}

static void sdcard_init_card_v2(sdcard_c_obj_t *self) {
    for (int i = 0; i < SD_CMD_TIMEOUT; i++) {
        sleep_ms(50);
        sdcard_cmd(self, 55, 0, 0, 0, true, false);
        if (sdcard_cmd(self, 41, 0x40000000, 0, 0, true, false) == 0) {
            sdcard_cmd(self, 58, 0, 0, 4, true, false);  // check CCS; response discarded, matches mp/sdcard.py
            self->cdv = 1;
            return;
        }
    }
    mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("timeout waiting for v2 card"));
}

static void sdcard_init_card(sdcard_c_obj_t *self) {
    // Mandatory low-speed handshake rate (SD spec).
    sdcard_apply_format(self->spi, 400000);
    gpio_put(self->cs_pin, 1);

    sleep_ms(1);
    uint8_t ff = 0xFF;
    for (int i = 0; i < 16; i++) {
        spi_write_blocking(self->spi, &ff, 1);
    }

    bool idle_ok = false;
    for (int i = 0; i < 5; i++) {
        if (sdcard_cmd(self, 0, 0, 0x95, 0, true, false) == R1_IDLE_STATE) {
            idle_ok = true;
            break;
        }
    }
    if (!idle_ok) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("no SD card"));
    }

    int r = sdcard_cmd(self, 8, 0x01AA, 0x87, 4, true, false);
    if (r == R1_IDLE_STATE) {
        sdcard_init_card_v2(self);
    } else if (r == (R1_IDLE_STATE | R1_ILLEGAL_COMMAND)) {
        sdcard_init_card_v1(self);
    } else {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("couldn't determine SD card version"));
    }

    // CMD9: read CSD (R1 + 16-byte block)
    r = sdcard_cmd(self, 9, 0, 0, 0, false, false);
    if (r != 0) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("no response from SD card"));
    }
    uint8_t csd[16];
    sdcard_readinto(self, csd, 16);
    if ((csd[0] & 0xC0) == 0x40) {
        // CSD version 2.0
        self->sectors = ((((uint32_t)csd[8] << 8) | csd[9]) + 1) * 1024;
    } else if ((csd[0] & 0xC0) == 0x00) {
        // CSD version 1.0
        uint32_t c_size = ((uint32_t)(csd[6] & 0x03) << 10) | ((uint32_t)csd[7] << 2) | (csd[8] >> 6);
        uint32_t c_size_mult = ((uint32_t)(csd[9] & 0x03) << 1) | (csd[10] >> 7);
        uint32_t read_bl_len = csd[5] & 0x0F;
        uint64_t capacity = (uint64_t)(c_size + 1) * (1ull << (c_size_mult + 2)) * (1ull << read_bl_len);
        self->sectors = (uint32_t)(capacity / 512);
    } else {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("SD card CSD format not supported"));
    }

    // CMD16: set block length to 512 (mandatory for SDSC/v1 only; SDHC
    // often ignores/errors this and is fine either way — matches
    // mp/sdcard.py's own tolerance here).
    r = sdcard_cmd(self, 16, 512, 0, 0, true, false);
    if (r != 0 && self->cdv == 512) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("can't set 512 block size"));
    }

    // Leave the bus in the "idle/shared" state (matches mp/sdcard.py's own
    // init_card(), which does the same thing for the same reason — every
    // subsequent readblocks()/writeblocks() call re-applies data_baud at
    // its own start regardless).
    sdcard_apply_format(self->spi, self->restore_baud);
}

// ---- readblocks/writeblocks (mirrors mp/sdcard.py's retry wrapper) ----

static void sdcard_readblocks_once(sdcard_c_obj_t *self, uint32_t block_num, uint8_t *buf, size_t len) {
    sdcard_apply_format(self->spi, self->data_baud);

    size_t nblocks = len / 512;
    bool ok = true;

    if (nblocks == 0 || (len % 512) != 0) {
        sdcard_apply_format(self->spi, self->restore_baud);
        mp_raise_ValueError(MP_ERROR_TEXT("Buffer length is invalid"));
    }

    if (nblocks == 1) {
        int r = sdcard_cmd(self, 17, block_num * self->cdv, 0, 0, false, false);
        if (r != 0) {
            gpio_put(self->cs_pin, 1);
            ok = false;
        } else {
            sdcard_readinto(self, buf, 512);
        }
    } else {
        int r = sdcard_cmd(self, 18, block_num * self->cdv, 0, 0, false, false);
        if (r != 0) {
            gpio_put(self->cs_pin, 1);
            ok = false;
        } else {
            for (size_t i = 0; i < nblocks; i++) {
                sdcard_readinto(self, buf + i * 512, 512);
            }
            r = sdcard_cmd(self, 12, 0, 0xFF, 0, false, true);
            if (r != 0) {
                gpio_put(self->cs_pin, 1);
                ok = false;
            } else {
                // CMD12 stop-transmission: wait for the card's post-stop
                // busy signal to clear before releasing CS (see
                // mp/sdcard.py's 2026-09-30 comment on this exact point —
                // skipping this wait reliably broke the very next command).
                uint8_t busy;
                do {
                    spi_read_blocking(self->spi, 0xFF, &busy, 1);
                } while (busy == 0x00);
                cs_deselect_release(self);
            }
        }
    }

    sdcard_apply_format(self->spi, self->restore_baud);

    if (!ok) {
        mp_raise_OSError(MP_EIO);
    }
}

static void sdcard_writeblocks_once(sdcard_c_obj_t *self, uint32_t block_num, const uint8_t *buf, size_t len) {
    sdcard_apply_format(self->spi, self->data_baud);

    size_t nblocks = len / 512;
    bool ok = true;

    if (nblocks == 0 || (len % 512) != 0) {
        sdcard_apply_format(self->spi, self->restore_baud);
        mp_raise_ValueError(MP_ERROR_TEXT("Buffer length is invalid"));
    }

    if (nblocks == 1) {
        if (sdcard_cmd(self, 24, block_num * self->cdv, 0, 0, true, false) != 0) {
            ok = false;
        } else if (!sdcard_write_block(self, TOKEN_DATA, buf, 512)) {
            ok = false;
        }
    } else {
        if (sdcard_cmd(self, 25, block_num * self->cdv, 0, 0, true, false) != 0) {
            ok = false;
        } else {
            for (size_t i = 0; i < nblocks && ok; i++) {
                if (!sdcard_write_block(self, TOKEN_CMD25, buf + i * 512, 512)) {
                    ok = false;
                }
            }
            if (ok) {
                sdcard_write_stop_token(self, TOKEN_STOP_TRAN);
            }
        }
    }

    sdcard_apply_format(self->spi, self->restore_baud);

    if (!ok) {
        mp_raise_OSError(MP_EIO);
    }
}

// Retry wrapper (matches mp/sdcard.py's readblocks()/writeblocks(): up to
// SD_RETRY_COUNT attempts, SD_RETRY_DELAY_MS between them, re-applying the
// SPI format every attempt) — turns a one-off marginal-bus hiccup back
// into a transient, recoverable retry instead of a session-ending OSError
// (see mp/sdcard.py's 2026-09-06 comment on why this matters for
// uos.mount()'s VFS layer specifically).
static void sdcard_readblocks(sdcard_c_obj_t *self, uint32_t block_num, uint8_t *buf, size_t len) {
    for (int attempt = 0; attempt < SD_RETRY_COUNT; attempt++) {
        if (attempt) {
            sleep_ms(SD_RETRY_DELAY_MS);
        }
        nlr_buf_t nlr;
        if (nlr_push(&nlr) == 0) {
            sdcard_readblocks_once(self, block_num, buf, len);
            nlr_pop();
            return;
        } else {
            if (attempt == SD_RETRY_COUNT - 1) {
                nlr_jump(nlr.ret_val);
            }
        }
    }
}

static void sdcard_writeblocks(sdcard_c_obj_t *self, uint32_t block_num, const uint8_t *buf, size_t len) {
    for (int attempt = 0; attempt < SD_RETRY_COUNT; attempt++) {
        if (attempt) {
            sleep_ms(SD_RETRY_DELAY_MS);
        }
        nlr_buf_t nlr;
        if (nlr_push(&nlr) == 0) {
            sdcard_writeblocks_once(self, block_num, buf, len);
            nlr_pop();
            return;
        } else {
            if (attempt == SD_RETRY_COUNT - 1) {
                nlr_jump(nlr.ret_val);
            }
        }
    }
}

// ---- MicroPython bindings ---------------------------------------------

static mp_obj_t sdcard_c_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_spi_id, ARG_cs, ARG_baudrate, ARG_restore_baudrate };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_spi_id,           MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 1} },
        { MP_QSTR_cs,               MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_baudrate,         MP_ARG_INT, {.u_int = 1000000} },
        { MP_QSTR_restore_baudrate, MP_ARG_INT, {.u_int = 4000000} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    sdcard_c_obj_t *self = mp_obj_malloc(sdcard_c_obj_t, &sdcard_c_type);

    mp_int_t spi_id = args[ARG_spi_id].u_int;
    self->spi = (spi_id == 0) ? spi0 : spi1;
    self->cs_pin = (uint)args[ARG_cs].u_int;
    self->data_baud = (uint32_t)args[ARG_baudrate].u_int;
    self->restore_baud = (uint32_t)args[ARG_restore_baudrate].u_int;
    self->sectors = 0;
    self->cdv = 512;

    gpio_init(self->cs_pin);
    gpio_set_dir(self->cs_pin, GPIO_OUT);
    gpio_put(self->cs_pin, 1);

    // SPI1_RX (GP12) doubles as SD-MISO and needs a pull-up so the line
    // reads 0xFF (not a floating 0x00) whenever the card tri-states MISO
    // (CS deasserted) -- see src/msx/msx_core.c's msx_init_display_hardware()
    // comment on this exact point, and doc/sdcard_spi_mode_bug.md. That
    // function normally sets this pull-up once at boot, but only as a side
    // effect of LCD/display init running *before* mount_sd() -- this
    // constructor has no such guarantee (e.g. a standalone dev/test script
    // run via `mpremote run` that never executes main.py's boot sequence
    // at all). Setting it again here is idempotent and makes this driver
    // self-sufficient regardless of what else has (or hasn't) run yet.
    // 2026-10-14 real-hardware finding: without this, mp/test/
    // verify_sdcard_c.py run as a cold standalone script reproduced exactly
    // the failure mode the comment above describes -- CMD18 falsely read
    // as succeeding (floating MISO -> 0x00 looks like a clean R1), then
    // readinto()'s data-token wait genuinely timed out (floating byte
    // never coincides with 0xFE), leaving the card wedged mid multi-block
    // stream (no CMD12 ever reached it) and breaking even the proven
    // Python driver's subsequent reads in the same session.
    if (self->spi == spi1) {
        gpio_pull_up(12);
    }

    self->dma_tx_chan = dma_claim_unused_channel(false);
    self->dma_rx_chan = dma_claim_unused_channel(false);
    if (self->dma_tx_chan < 0 || self->dma_rx_chan < 0) {
        // Not fatal — sdcard_spi_transfer() falls back to blocking SPI
        // when either channel is unavailable. Just slower, not broken.
        if (self->dma_tx_chan >= 0) {
            dma_channel_unclaim(self->dma_tx_chan);
        }
        if (self->dma_rx_chan >= 0) {
            dma_channel_unclaim(self->dma_rx_chan);
        }
        self->dma_tx_chan = -1;
        self->dma_rx_chan = -1;
    }

    sdcard_init_card(self);

    return MP_OBJ_FROM_PTR(self);
}

static mp_obj_t sdcard_c_readblocks(mp_obj_t self_in, mp_obj_t block_num_in, mp_obj_t buf_in) {
    sdcard_c_obj_t *self = MP_OBJ_TO_PTR(self_in);
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(buf_in, &bufinfo, MP_BUFFER_WRITE);
    sdcard_readblocks(self, (uint32_t)mp_obj_get_int(block_num_in), (uint8_t *)bufinfo.buf, bufinfo.len);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_3(sdcard_c_readblocks_obj, sdcard_c_readblocks);

static mp_obj_t sdcard_c_writeblocks(mp_obj_t self_in, mp_obj_t block_num_in, mp_obj_t buf_in) {
    sdcard_c_obj_t *self = MP_OBJ_TO_PTR(self_in);
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(buf_in, &bufinfo, MP_BUFFER_READ);
    sdcard_writeblocks(self, (uint32_t)mp_obj_get_int(block_num_in), (const uint8_t *)bufinfo.buf, bufinfo.len);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_3(sdcard_c_writeblocks_obj, sdcard_c_writeblocks);

static mp_obj_t sdcard_c_ioctl(mp_obj_t self_in, mp_obj_t cmd_in, mp_obj_t arg_in) {
    (void)arg_in;
    sdcard_c_obj_t *self = MP_OBJ_TO_PTR(self_in);
    switch (mp_obj_get_int(cmd_in)) {
        case MP_BLOCKDEV_IOCTL_INIT:
        case MP_BLOCKDEV_IOCTL_DEINIT:
        case MP_BLOCKDEV_IOCTL_SYNC:
            return MP_OBJ_NEW_SMALL_INT(0);
        case MP_BLOCKDEV_IOCTL_BLOCK_COUNT:
            return mp_obj_new_int_from_uint(self->sectors);
        case MP_BLOCKDEV_IOCTL_BLOCK_SIZE:
            return MP_OBJ_NEW_SMALL_INT(512);
        default:
            return mp_const_none;
    }
}
static MP_DEFINE_CONST_FUN_OBJ_3(sdcard_c_ioctl_obj, sdcard_c_ioctl);

// Convenience accessors — not needed by uos.mount()/VfsFat (which only
// ever calls ioctl(4, 0) for the block count) but used by this project's
// own dev-only benchmark/sweep scripts (mp/test/).
static mp_obj_t sdcard_c_sectors(mp_obj_t self_in) {
    sdcard_c_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return mp_obj_new_int_from_uint(self->sectors);
}
static MP_DEFINE_CONST_FUN_OBJ_1(sdcard_c_sectors_obj, sdcard_c_sectors);

static mp_obj_t sdcard_c_get_data_baud(mp_obj_t self_in) {
    sdcard_c_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return mp_obj_new_int_from_uint(self->data_baud);
}
static MP_DEFINE_CONST_FUN_OBJ_1(sdcard_c_get_data_baud_obj, sdcard_c_get_data_baud);

static mp_obj_t sdcard_c_set_data_baud(mp_obj_t self_in, mp_obj_t baud_in) {
    sdcard_c_obj_t *self = MP_OBJ_TO_PTR(self_in);
    self->data_baud = (uint32_t)mp_obj_get_int(baud_in);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(sdcard_c_set_data_baud_obj, sdcard_c_set_data_baud);

static const mp_rom_map_elem_t sdcard_c_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_readblocks),    MP_ROM_PTR(&sdcard_c_readblocks_obj) },
    { MP_ROM_QSTR(MP_QSTR_writeblocks),   MP_ROM_PTR(&sdcard_c_writeblocks_obj) },
    { MP_ROM_QSTR(MP_QSTR_ioctl),         MP_ROM_PTR(&sdcard_c_ioctl_obj) },
    { MP_ROM_QSTR(MP_QSTR_sectors),       MP_ROM_PTR(&sdcard_c_sectors_obj) },
    { MP_ROM_QSTR(MP_QSTR_get_data_baud), MP_ROM_PTR(&sdcard_c_get_data_baud_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_data_baud), MP_ROM_PTR(&sdcard_c_set_data_baud_obj) },
};
static MP_DEFINE_CONST_DICT(sdcard_c_locals_dict, sdcard_c_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    sdcard_c_type,
    MP_QSTR_SDCard,
    MP_TYPE_FLAG_NONE,
    make_new, sdcard_c_make_new,
    locals_dict, &sdcard_c_locals_dict
    );

static const mp_rom_map_elem_t sdcard_c_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_sdcard_c) },
    { MP_ROM_QSTR(MP_QSTR_SDCard),   MP_ROM_PTR(&sdcard_c_type) },
};
static MP_DEFINE_CONST_DICT(sdcard_c_module_globals, sdcard_c_module_globals_table);

const mp_obj_module_t mp_module_sdcard_c = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&sdcard_c_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_sdcard_c, mp_module_sdcard_c);
