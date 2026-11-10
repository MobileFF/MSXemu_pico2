/*
 * scc_out.c — SCC (K051649) external output to a PicoChiptuneOrchestra
 * slave board (slave_scc.uf2), over a bit-banged, write-only SPI link.
 *
 * 2026-11-08: stage 2 of 調査用/PicoChiptuneOrchestra連携_SCC_YM2413外部演奏検討.md.
 *
 * Wire format: PicoChiptuneOrchestra src/protocol/vgm_spi_protocol.h —
 * 3-byte frames {opcode, reg, data}, SPI mode 1 (CPOL=0, CPHA=1), MSB
 * first, the whole frame under ONE CS-low assertion ("burst"; the
 * per-byte-CS/20us-gap scheme that document's §3.1 assumed was the
 * pre-mode-1 protocol — slave_spi_rx.c now runs mode 1, which clocks a
 * continuous 3-byte burst correctly, see slave_bus.c send_frame()).
 *
 * Why bit-banged GPIO instead of a hardware SPI block or PIO:
 *  - Pico 2 shares SPI1 between LCD/SD/touch (the LCD's DMA transfer runs
 *    concurrently with emulation), and the free SPI0 pin pairs aren't
 *    guaranteed on either board's wiring — plain GPIOs can be any 3 pins
 *    (chosen at runtime, msx.ini scc_out=sck,mosi,cs).
 *  - One frame is ~24 bit-times + a short CS gap, ~10us at the default
 *    ~3MHz clock, i.e. ~0.3ms for a full 32-byte waveform rewrite — small
 *    next to a 16.7ms frame, so the PIO+DMA queue (stage 3) isn't needed
 *    for now. Revisit only if a real-hardware FPS measurement says so.
 *
 * The slave's PL022 in slave mode tolerates any SCK slower than its own
 * clk_peri/12 (~10MHz on a 125MHz RP2040), and an interrupt stretching a
 * clock phase mid-frame is harmless (mode 1 samples on SCK edges only).
 */
#include "scc_out.h"
#include "msx_core.h"

#include "hardware/gpio.h"
#include "hardware/clocks.h"
#include "pico/platform.h"
#include "pico/time.h"

static bool     s_active   = false;
static uint     s_sck, s_mosi, s_cs;
static uint32_t s_half_cyc = 40;   /* CPU cycles per SCK half period */

static inline void half_delay(void) {
    busy_wait_at_least_cycles(s_half_cyc);
}

static void send_byte(uint8_t b) {
    for (int i = 7; i >= 0; i--) {
        /* Mode 1: master shifts on the leading (rising) edge, slave
         * samples on the trailing (falling) edge. */
        gpio_put(s_sck, 1);
        gpio_put(s_mosi, (b >> i) & 1);
        half_delay();
        gpio_put(s_sck, 0);
        half_delay();
    }
}

static void send_frame(uint8_t opcode, uint8_t reg, uint8_t data) {
    gpio_put(s_cs, 0);
    half_delay();
    send_byte(opcode);
    send_byte(reg);
    send_byte(data);
    half_delay();
    gpio_put(s_cs, 1);
    /* CS-high gap between frames (lets the slave's RX loop drain and
     * re-arm its frame boundary). ~1us. */
    busy_wait_at_least_cycles(s_half_cyc * 6);
}

static void scc_out_cb(uint8_t opcode, uint8_t reg, uint8_t data) {
    if (!s_active) return;
    send_frame(opcode, reg, data);
}

bool scc_out_init(uint sck, uint mosi, uint cs, uint32_t baud) {
    if (sck >= NUM_BANK0_GPIOS || mosi >= NUM_BANK0_GPIOS || cs >= NUM_BANK0_GPIOS ||
        sck == mosi || sck == cs || mosi == cs) {
        return false;
    }
    if (s_active) scc_out_deinit();

    s_sck = sck; s_mosi = mosi; s_cs = cs;
    if (baud == 0) baud = 3000000u;
    uint32_t sys = clock_get_hz(clk_sys);
    uint32_t half = sys / (baud * 2u);
    s_half_cyc = half ? half : 1;

    gpio_init(s_cs);   gpio_put(s_cs, 1);   gpio_set_dir(s_cs, GPIO_OUT);
    gpio_init(s_sck);  gpio_put(s_sck, 0);  gpio_set_dir(s_sck, GPIO_OUT);
    gpio_init(s_mosi); gpio_put(s_mosi, 0); gpio_set_dir(s_mosi, GPIO_OUT);

    s_active = true;
    /* NOP first: a slave that saw garbage while the pins were floating
     * resyncs on the opcode-mask check (slave_spi_rx.c), then RESET to
     * power-on state at the default 3.579545MHz clock preset. */
    send_frame(0x00, 0, 0);
    send_frame(MSX_SCC_OP_RESET, 0, 0);
    msx_set_scc_out_cb(scc_out_cb);
    return true;
}

void scc_out_deinit(void) {
    if (!s_active) return;
    send_frame(MSX_SCC_OP_MUTE, 0, 0);
    msx_set_scc_out_cb(NULL);
    s_active = false;
    gpio_deinit(s_sck);
    gpio_deinit(s_mosi);
    gpio_deinit(s_cs);
}

bool scc_out_active(void) {
    return s_active;
}
