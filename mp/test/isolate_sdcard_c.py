"""
sdcard.py (Python版)を一切介在させず、新しい sdcard_c (C実装)だけを単独で
マウントして isolate_sdcard_py.py と全く同じ範囲を読み直す、切り分け用の
最小テスト。

目的: isolate_sdcard_py.py で Python版単体が全region成功したのと同じ条件
(通常起動でメニュー表示を確認済み = GP12プルアップが設定済みの状態)で、
C版単体が同じように成功するかを確認する。これが通れば、以前
verify_sdcard_c.py で見られた失敗は「二重マウント(同一SPI1バスに2つの
ドライバを順に構築すること)」固有の問題に絞り込める。書き込みは一切
行わない(読み込みのみ)。

使い方:
    mpremote run mp/test/isolate_sdcard_c.py
"""

import gc
import machine
import sdcard_c

from board_config import (
    SD_SPI_ID, SD_MOSI_PIN, SD_SCK_PIN, SD_MISO_PIN, SD_CS_PIN,
    SD_INIT_BAUD, SD_DATA_BAUD, SPI_BAUD,
)

REGIONS = [
    (0, 16),
    (0, 16),
    (20000, 1),
    (20000, 32),
    (500000, 64),
]


# isolate_sdcard_py.py と同じFNV-1a実装 -- 出力されたハッシュ値を見比べて
# バイト内容の一致を確認する(2台を同時マウントしないため、こちらで照合)。
def fnv1a(buf):
    h = 0x811c9dc5
    for b in buf:
        h ^= b
        h = (h * 0x01000193) & 0xFFFFFFFF
    return h


def main():
    print("=== sdcard_c (C driver) solo isolation test ===")
    restore_baud = SPI_BAUD if SPI_BAUD is not None else SD_DATA_BAUD

    machine.SPI(SD_SPI_ID,
                baudrate=SD_INIT_BAUD,
                sck=machine.Pin(SD_SCK_PIN),
                mosi=machine.Pin(SD_MOSI_PIN),
                miso=machine.Pin(SD_MISO_PIN))
    sd = sdcard_c.SDCard(SD_SPI_ID, SD_CS_PIN,
                          baudrate=SD_DATA_BAUD,
                          restore_baudrate=restore_baud)
    print("sectors:", sd.sectors())

    all_ok = True
    for start_block, nblocks in REGIONS:
        # isolate_sdcard_py.py と同じ理由(raw-REPL実行コンテキストでの
        # ヒープ断片化)でregion間にGCを挟む。
        gc.collect()
        size = nblocks * 512
        buf = bytearray(size)
        try:
            sd.readblocks(start_block, buf)
            print("OK    block={:>7} n={:>3} ({} bytes) fnv1a=0x{:08X}".format(
                start_block, nblocks, size, fnv1a(buf)))
        except OSError as e:
            all_ok = False
            print("FAILED block={:>7} n={:>3}: {}".format(start_block, nblocks, e))

    print()
    print("=== ALL OK ===" if all_ok else "=== FAILURE DETECTED (see above) ===")


main()
