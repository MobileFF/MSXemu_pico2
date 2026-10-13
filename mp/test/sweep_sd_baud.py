"""
SD_DATA_BAUD(board_config.py)の引き上げ余地を安全に検証するための
開発用スクリプト。

board_config.py のコメントにある通り、20MHzは過去に実機でマウント自体が
失敗した実績がある一方、4MHzは「保守的な暫定値」とされている。この間の
どこまでが実際に安全か、速度だけでなく**データの正しさ**も含めて段階的
に検証する。

重要: スループットが出ている(エラーなく完了する)ことと、データが壊れて
いないことは別問題。doc/sdcard_spi_mode_bug.md にある通り、マージナルな
速度では「バイト数は正しく読めたことになっているが中身が壊れている」
ケースがあるため、各ボーレートで必ず次の2段階を行う:
    1. 同一ブロック範囲を複数回読み直し、基準(最も低い=最も安全な候補、
       既定では4MHz)で読んだ内容とバイト完全一致するか検証。
    2. 一致した場合のみ、スループットを計測。
不一致/OSErrorが出た時点でそのボーレートをNGとし、スイープを打ち切る
(それより高い値は試さない)。

書き込みは一切行わない(読み込みのみ)ので、SDカードの中身を壊すリスク
はない。

使い方:
    mpremote run mp/test/sweep_sd_baud.py

注意: ここでの検証は「カード初期化後、既に何度も読み込みを行った状態」
でのテスト。board_config.py に記録されている過去の20MHz失敗は
uos.mount()の*最初の*readblocks()呼び出し(CMD16直後、コールドな状態)
で起きたもので、状況が異なる可能性がある。このスクリプトで安全と出た
値を board_config.py の SD_DATA_BAUD に反映した後は、必ず実機で電源
再投入からの通常起動(uos.mount()を含む本来の起動シーケンス)を行って
最終確認すること。
"""

import time
import machine
import sdcard

from board_config import (
    SD_SPI_ID, SD_MOSI_PIN, SD_SCK_PIN, SD_MISO_PIN, SD_CS_PIN,
    SD_INIT_BAUD, SD_DATA_BAUD, SPI_BAUD,
)

# 現行値(4MHz)を基準=最も安全な候補として先頭に置き、以降は刻み幅を
# 抑えて段階的に引き上げる。20MHzは過去に実機でマウント失敗した実績が
# あるため、それより下の値で止める。
CANDIDATE_BAUDS = [
    SD_DATA_BAUD,   # 4,000,000 (現行値、基準)
    6_000_000,
    8_000_000,
    10_000_000,
    12_000_000,
    16_000_000,
]

VERIFY_BLOCKS = 16          # 整合性チェックに使うブロック数 (8KB)
VERIFY_REPEATS = 3          # 同一ボーレートで何回読み直して一致確認するか
PERF_CALLS = 256            # スループット計測の呼び出し回数
BLOCKS_PER_CALL = 8         # 実運用と同じ4KBチャンク (= 512 x 8)
START_BLOCK = 20000         # ファイルシステム管理領域を踏まない安全な位置


def mount_raw_sd():
    sd_spi = machine.SPI(SD_SPI_ID,
                          baudrate=SD_INIT_BAUD,
                          sck=machine.Pin(SD_SCK_PIN),
                          mosi=machine.Pin(SD_MOSI_PIN),
                          miso=machine.Pin(SD_MISO_PIN))
    sd_cs = machine.Pin(SD_CS_PIN, machine.Pin.OUT, value=1)
    sd = sdcard.SDCard(sd_spi, sd_cs,
                        baudrate=SD_DATA_BAUD,
                        restore_baudrate=SPI_BAUD if SPI_BAUD is not None else SD_DATA_BAUD)
    return sd


def read_region(sd, start_block, nblocks):
    buf = bytearray(nblocks * 512)
    sd.readblocks(start_block, buf)
    return buf


def measure_throughput(sd, start_block, perf_calls):
    buf = bytearray(BLOCKS_PER_CALL * 512)
    t0 = time.ticks_ms()
    for i in range(perf_calls):
        sd.readblocks(start_block + i * BLOCKS_PER_CALL, buf)
    elapsed = time.ticks_diff(time.ticks_ms(), t0)
    total_bytes = perf_calls * BLOCKS_PER_CALL * 512
    kbps = (total_bytes / 1024) / (elapsed / 1000) if elapsed > 0 else float('inf')
    return elapsed, kbps


def sweep(sd, start_block=START_BLOCK):
    print("=== SD_DATA_BAUD sweep ===")
    print("candidates:", CANDIDATE_BAUDS)
    print("verify region: block {} .. {} ({} bytes)".format(
        start_block, start_block + VERIFY_BLOCKS - 1, VERIFY_BLOCKS * 512))
    print()

    # 基準データを最も安全な候補(先頭=現行値)で取得
    baseline_baud = CANDIDATE_BAUDS[0]
    sd.baudrate = baseline_baud
    ref = read_region(sd, start_block, VERIFY_BLOCKS)
    print("[baseline {} Hz] captured {} bytes".format(baseline_baud, len(ref)))
    print()

    last_good = None
    for baud in CANDIDATE_BAUDS:
        sd.baudrate = baud
        ok = True
        err = None

        # 1. 整合性チェック: 複数回読み直して基準と完全一致するか
        for attempt in range(VERIFY_REPEATS):
            try:
                got = read_region(sd, start_block, VERIFY_BLOCKS)
            except OSError as e:
                ok = False
                err = "OSError: {}".format(e)
                break
            if got != ref:
                ok = False
                err = "data mismatch on attempt {}".format(attempt)
                break

        if not ok:
            print("[{} Hz] NG -- {}".format(baud, err))
            print()
            print("=== stopping sweep: last known-good = {} Hz ===".format(last_good))
            break

        # 2. スループット計測(整合性が確認できた場合のみ)
        try:
            elapsed, kbps = measure_throughput(sd, start_block, PERF_CALLS)
        except OSError as e:
            print("[{} Hz] verify OK, but throughput run failed: {}".format(baud, e))
            print()
            print("=== stopping sweep: last known-good = {} Hz ===".format(last_good))
            break

        print("[{} Hz] OK -- verify x{} passed, throughput {:.1f} KB/s ({} ms / {} bytes)".format(
            baud, VERIFY_REPEATS, kbps, elapsed, PERF_CALLS * BLOCKS_PER_CALL * 512))
        last_good = baud
        print()

    # 安全のため、最後に必ず基準(最も低い)ボーレートへ戻す
    sd.baudrate = baseline_baud

    print("=== summary ===")
    print("highest verified-safe SD_DATA_BAUD in this sweep: {} Hz".format(last_good))
    print("NOTE: this only tests mid-session reads after the card is already")
    print("initialized. Before committing a new SD_DATA_BAUD value, also do a")
    print("full cold power-cycle boot test (real uos.mount() at startup).")


def main():
    sd = mount_raw_sd()
    print("SD sectors: {} (~{} MB)".format(sd.sectors, sd.sectors // 2048))
    print()
    sweep(sd)


main()
