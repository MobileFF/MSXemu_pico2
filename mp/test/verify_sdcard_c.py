"""
C実装のSDカードドライバ(sdcard_c)が、既存の実績あるPython実装
(mp/sdcard.py)と**バイト完全一致**でデータを読めているかを検証する
スクリプト。速度より先に、まずこれが通ることを確認すること。

両ドライバで同じ範囲を読み、内容を直接比較する。書き込みは一切行わない
(読み込みのみ)ので、SDカードの中身を壊すリスクはない。

使い方:
    mpremote run mp/test/verify_sdcard_c.py

注意(2026-10-14 実機で確認): このスクリプトのように両ドライバを**同一
セッションに同時マウント**すると、SDカードに対して完全な再初期化
(CMD0)を2回連続で送ることになる。実機ではこれが引き金となり、カードが
マルチブロック転送の途中で止まったまま(実績あるPython版のリトライ
ロジックでも同一セッション内では回復できない)になる事象を確認した。
mount_sd()はドライバを1つしか構築しないため本番では発生しないが、この
スクリプト単体を実行して原因不明の失敗が出た場合は、ドライバのバグでは
なくこの同時マウント自体が引き金になっている可能性が高い。より確実な
検証には、mp/test/isolate_sdcard_py.py と mp/test/isolate_sdcard_c.py を
別々のセッションで実行し、出力されるFNV-1aハッシュ値を突き合わせる方法
を推奨する(本プロジェクトではこちらで5リージョン全て一致を確認済み)。
"""

import machine

import sdcard
import sdcard_c

# SPI1_RX (GP12) = SD MISO。通常は起動シーケンス中に msx_core.c の
# msx_init_display_hardware() がLCD初期化の副作用として一度だけプルアップ
# を設定しているが、このスクリプトを mpremote run で単体実行した場合は
# main.py の通常起動シーケンスを経由しないため、そのプルアップが一度も
# 設定されないまま SD アクセスが行われる。結果、MISOがフローティングで
# 0x00を読み、CMD18が見かけ上成功(フローティング0x00はR1成功と区別が
# つかない)した直後にデータトークン待ちが本当にタイムアウトし、カードが
# マルチブロック送信の途中で止まったまま(CMD12未到達)になる--という
# 障害を2026-10-14に実機で再現した。standalone実行時にも安全なように、
# ここで明示的に設定しておく(既に設定済みでも副作用はない)。
machine.Pin(12, machine.Pin.IN, machine.Pin.PULL_UP)

from board_config import (
    SD_SPI_ID, SD_MOSI_PIN, SD_SCK_PIN, SD_MISO_PIN, SD_CS_PIN,
    SD_INIT_BAUD, SD_DATA_BAUD, SPI_BAUD,
)

# 複数の範囲・サイズで検証(先頭に近い領域、カード中盤、単一ブロック、
# マルチブロックの両方をカバー)。
REGIONS = [
    (0, 16),        # 先頭16ブロック(MBR/FATヘッダ含む領域)
    (20000, 1),     # 単一ブロック読み込み(CMD17パス)
    (20000, 32),    # マルチブロック読み込み(CMD18パス、256KB相当のテスト)
    (500000, 64),   # カード中盤
]


def restore_baud():
    return SPI_BAUD if SPI_BAUD is not None else SD_DATA_BAUD


def mount_python():
    sd_spi = machine.SPI(SD_SPI_ID,
                          baudrate=SD_INIT_BAUD,
                          sck=machine.Pin(SD_SCK_PIN),
                          mosi=machine.Pin(SD_MOSI_PIN),
                          miso=machine.Pin(SD_MISO_PIN))
    sd_cs = machine.Pin(SD_CS_PIN, machine.Pin.OUT, value=1)
    return sdcard.SDCard(sd_spi, sd_cs, baudrate=SD_DATA_BAUD, restore_baudrate=restore_baud())


def mount_c():
    machine.SPI(SD_SPI_ID,
                baudrate=SD_INIT_BAUD,
                sck=machine.Pin(SD_SCK_PIN),
                mosi=machine.Pin(SD_MOSI_PIN),
                miso=machine.Pin(SD_MISO_PIN))
    return sdcard_c.SDCard(SD_SPI_ID, SD_CS_PIN, baudrate=SD_DATA_BAUD, restore_baudrate=restore_baud())


def main():
    print("=== sdcard_c vs sdcard.py byte-exact verification ===")

    print("[1/2] mounting via Python driver (mp/sdcard.py)...")
    sd_py = mount_python()
    print("  sectors (Python):", sd_py.sectors)

    print("[2/2] mounting via C driver (sdcard_c)...")
    sd_c = mount_c()
    print("  sectors (C):     ", sd_c.sectors())

    if sd_py.sectors != sd_c.sectors():
        print("MISMATCH: sector count differs! Python={} C={}".format(sd_py.sectors, sd_c.sectors()))
        print("=== FAILED ===")
        return

    all_ok = True
    for start_block, nblocks in REGIONS:
        size = nblocks * 512
        if start_block + nblocks > sd_py.sectors:
            print("SKIP region (block={}, n={}): beyond card size".format(start_block, nblocks))
            continue

        buf_py = bytearray(size)
        buf_c = bytearray(size)

        try:
            sd_py.readblocks(start_block, buf_py)
        except OSError as e:
            print("Python driver FAILED at block={} n={}: {}".format(start_block, nblocks, e))
            all_ok = False
            continue

        try:
            sd_c.readblocks(start_block, buf_c)
        except OSError as e:
            print("C driver FAILED at block={} n={}: {}".format(start_block, nblocks, e))
            all_ok = False
            continue

        if buf_py == buf_c:
            print("OK    block={:>7} n={:>3} ({} bytes) -- byte-exact match".format(start_block, nblocks, size))
        else:
            all_ok = False
            # 最初に食い違うバイトの位置を報告(デバッグの足がかり)
            first_diff = -1
            for i in range(size):
                if buf_py[i] != buf_c[i]:
                    first_diff = i
                    break
            print("MISMATCH block={:>7} n={:>3} ({} bytes) -- first diff at offset {}: "
                  "python=0x{:02X} c=0x{:02X}".format(
                      start_block, nblocks, size, first_diff,
                      buf_py[first_diff], buf_c[first_diff]))

    print()
    if all_ok:
        print("=== ALL REGIONS MATCH -- sdcard_c is byte-exact with mp/sdcard.py ===")
    else:
        print("=== MISMATCH DETECTED -- do NOT trust sdcard_c yet, see above ===")


main()
