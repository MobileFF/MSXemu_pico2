"""
sdcard_c を一切介在させず、既存の実績ある mp/sdcard.py だけを単独でマウント
して region (block=0, n=16) を複数回読み直す、切り分け用の最小テスト。

目的: verify_sdcard_c.py で観測された「block=0 n=16 でタイムアウト」が、
   (a) sdcard_c との二重マウント(同一SPI1バスに2つの machine.SPI/ドライバ
       を順に構築すること)が引き金になっているのか、
   (b) それとも sdcard.py 単体・この電源投入セッションだけでも再現する、
       より根本的な問題(SD_DATA_BAUD=12MHzでのマージナルな信頼性、SD
       カード自体の状態など)なのか
を切り分ける。書き込みは一切行わない(読み込みのみ)。

使い方:
    mpremote run mp/test/isolate_sdcard_py.py
"""

import gc
import machine
import sdcard

from board_config import (
    SD_SPI_ID, SD_MOSI_PIN, SD_SCK_PIN, SD_MISO_PIN, SD_CS_PIN,
    SD_INIT_BAUD, SD_DATA_BAUD, SPI_BAUD,
)

REGIONS = [
    (0, 16),
    (0, 16),   # 同じ範囲をもう一度(直前の読み込みが次に影響するかも見る)
    (20000, 1),
    (20000, 32),
    (500000, 64),
]


# FNV-1a 32bit -- 2台のドライバを同一セッションで同時にマウントせずに
# 内容の一致を確認するための、単純で十分な決定的ハッシュ(暗号強度は不要)。
# 単体実行(このファイル/isolate_sdcard_c.py)それぞれでハッシュ値を出力し、
# 2回の実行結果を見比べれば、同時マウントせずにバイト内容の一致を確認できる。
def fnv1a(buf):
    h = 0x811c9dc5
    for b in buf:
        h ^= b
        h = (h * 0x01000193) & 0xFFFFFFFF
    return h


def main():
    print("=== sdcard.py (Python driver) solo isolation test ===")
    restore_baud = SPI_BAUD if SPI_BAUD is not None else SD_DATA_BAUD

    sd_spi = machine.SPI(SD_SPI_ID,
                          baudrate=SD_INIT_BAUD,
                          sck=machine.Pin(SD_SCK_PIN),
                          mosi=machine.Pin(SD_MOSI_PIN),
                          miso=machine.Pin(SD_MISO_PIN))
    sd_cs = machine.Pin(SD_CS_PIN, machine.Pin.OUT, value=1)
    sd = sdcard.SDCard(sd_spi, sd_cs, baudrate=SD_DATA_BAUD, restore_baudrate=restore_baud)
    print("sectors:", sd.sectors)

    all_ok = True
    for start_block, nblocks in REGIONS:
        # mpremote run のraw-REPL実行コンテキストでは、前のregionで確保した
        # bytearrayがまだGCされずヒープに残っていることがあり、次の大きな
        # 確保(32KB等)がそのまま失敗することがある(2026-10-14 実機で
        # 再現) -- ドライバのバグではなくこのスクリプト自身のヒープ管理の
        # 問題なので、region間で明示的にGCしておく。
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
