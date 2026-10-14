"""
C実装のSDカードドライバ(sdcard_c, modsdcard_c.c)の読み込み速度を実機で
計測するための開発用ベンチマークスクリプト。mp/test/bench_sdcard.py
(Python実装版)と同じ2種類の計測を行うので、同じSD_DATA_BAUD(12MHz)
での直接比較ができる。

使い方:
    mpremote run mp/test/bench_sdcard_c.py

比較対象(Python実装、sweep_sd_baud.pyで12MHzにて実測済み):
    raw block read: 849.8 KB/s

このスクリプトは main.py の mount_sd() を経由せず、sdcard_c.SDCard() を
直接構築する(C/Python どちらのドライバが使われているかを切り分けて
測るため)。書き込みは一切行わない(読み込みのみ)。
"""

import time
import machine
import sdcard_c

# GP12 (SD MISO) プルアップ -- verify_sdcard_c.py の同コメント参照。
# standalone実行(main.pyの通常起動を経由しない)でも安全なように明示設定。
machine.Pin(12, machine.Pin.IN, machine.Pin.PULL_UP)

from board_config import (
    SD_SPI_ID, SD_MOSI_PIN, SD_SCK_PIN, SD_MISO_PIN, SD_CS_PIN,
    SD_INIT_BAUD, SD_DATA_BAUD, SPI_BAUD,
)

BLOCKS_PER_CALL = 8
BUF_SIZE = BLOCKS_PER_CALL * 512
TOTAL_CALLS = 512
TOTAL_BLOCKS = TOTAL_CALLS * BLOCKS_PER_CALL
TOTAL_BYTES = TOTAL_BLOCKS * 512
START_BLOCK = 20000


def mount_raw_sd_c():
    # machine.SPI(...) is still constructed first, purely for its side
    # effect of configuring the physical bus (GPIO pin functions +
    # spi_init()) -- sdcard_c.SDCard() itself takes over the raw spi_inst_t*
    # afterward (see modsdcard_c.c's header comment). The returned
    # machine.SPI object is otherwise unused below.
    machine.SPI(SD_SPI_ID,
                baudrate=SD_INIT_BAUD,
                sck=machine.Pin(SD_SCK_PIN),
                mosi=machine.Pin(SD_MOSI_PIN),
                miso=machine.Pin(SD_MISO_PIN))
    restore_baud = SPI_BAUD if SPI_BAUD is not None else SD_DATA_BAUD
    sd = sdcard_c.SDCard(SD_SPI_ID, SD_CS_PIN,
                          baudrate=SD_DATA_BAUD,
                          restore_baudrate=restore_baud)
    return sd


def bench_raw_sequential_read(sd):
    start_block = START_BLOCK
    if start_block + TOTAL_BLOCKS > sd.sectors():
        start_block = max(0, sd.sectors() - TOTAL_BLOCKS - 16)

    buf = bytearray(BUF_SIZE)
    t0 = time.ticks_ms()
    for i in range(TOTAL_CALLS):
        sd.readblocks(start_block + i * BLOCKS_PER_CALL, buf)
    elapsed = time.ticks_diff(time.ticks_ms(), t0)

    kb = TOTAL_BYTES / 1024
    kbps = kb / (elapsed / 1000) if elapsed > 0 else float('inf')
    print("[raw block read, sdcard_c]")
    print("  total: {} bytes ({} calls x {} bytes)".format(TOTAL_BYTES, TOTAL_CALLS, BUF_SIZE))
    print("  elapsed: {} ms".format(elapsed))
    print("  throughput: {:.1f} KB/s".format(kbps))
    return elapsed


def bench_mounted_file_read():
    import uos
    TEST_FILE = "/sd"
    best_path, best_size = None, 0

    def scan(d):
        nonlocal best_path, best_size
        try:
            for name, typ, *_ in uos.ilistdir(d):
                p = d.rstrip('/') + '/' + name
                if typ & 0x4000:
                    scan(p)
                else:
                    try:
                        size = uos.stat(p)[6]
                    except OSError:
                        continue
                    if size > best_size:
                        best_path, best_size = p, size
        except OSError:
            pass

    scan(TEST_FILE)
    if best_path is None:
        print("[mounted file read, sdcard_c] SKIP: no test file found under {}".format(TEST_FILE))
        return None

    print("[mounted file read, sdcard_c] using {} ({} bytes)".format(best_path, best_size))
    buf = bytearray(BUF_SIZE)
    mv = memoryview(buf)
    total = 0
    t0 = time.ticks_ms()
    with open(best_path, 'rb') as f:
        while True:
            n = f.readinto(mv)
            if not n:
                break
            total += n
    elapsed = time.ticks_diff(time.ticks_ms(), t0)

    kb = total / 1024
    kbps = kb / (elapsed / 1000) if elapsed > 0 else float('inf')
    print("  total: {} bytes".format(total))
    print("  elapsed: {} ms".format(elapsed))
    print("  throughput: {:.1f} KB/s".format(kbps))
    return elapsed


def main():
    print("=== sdcard_c (C driver) benchmark ===")
    sd = mount_raw_sd_c()
    print("SD sectors: {} (~{} MB)".format(sd.sectors(), sd.sectors() // 2048))
    bench_raw_sequential_read(sd)
    print()
    import uos
    try:
        uos.mount(sd, '/sd')
    except OSError as e:
        print("mount failed (already mounted?): {}".format(e))
    bench_mounted_file_read()


main()
