"""
sdcard.py の読み込み速度を実機で計測するための開発用ベンチマークスクリプト。

readinto()のデータトークン待ちループの sleep_ms(1) 削除(2026-10-13)が、
実際に体感できる差を生んでいるかを検証する目的。書き込みは一切行わない
(読み込みのみ)ので、SDカードの中身を壊すリスクはない。

使い方:
    mpremote run mp/test/bench_sdcard.py

比較手順:
    1. 現行(最適化後)の mp/sdcard.py がボードに入っている状態でこのスク
       リプトを実行し、結果を記録する。
    2. 最適化前のバージョンに戻す:
           git show HEAD:mp/sdcard.py > /tmp/sdcard_old.py
           mpremote cp /tmp/sdcard_old.py :sdcard.py
       (HEAD はまだ今回の変更をコミットしていない前提 -- 最適化前の内容
       がそのまま取れる)
    3. もう一度このスクリプトを実行し、結果を比較する。
    4. 最適化後の sdcard.py に戻す:
           mpremote cp mp/sdcard.py :sdcard.py

このスクリプト自体はボードに転送するだけで、SD上のファイルは一切変更
しない。マウントも本番と同じ board_config.py の定数(SPIピン・ボーレー
ト)をそのまま使う。
"""

import time
import machine
import sdcard

from board_config import (
    SD_SPI_ID, SD_MOSI_PIN, SD_SCK_PIN, SD_MISO_PIN, SD_CS_PIN,
    SD_INIT_BAUD, SD_DATA_BAUD, SPI_BAUD,
)

# 実運用(メガロムのSD->フラッシュコピー、BIOS/カート読み込み)と同じ
# チャンクサイズ: msx_menu.py の _READ_CHUNK / _FLASH_COPY_CHUNK = 4096
# (512バイトブロック x 8 を1回の CMD18 で読む)。
BLOCKS_PER_CALL = 8
BUF_SIZE = BLOCKS_PER_CALL * 512

# 計測に使う総ブロック数(= 総読み込みバイト数)。デフォルトで約2MB。
TOTAL_CALLS = 512
TOTAL_BLOCKS = TOTAL_CALLS * BLOCKS_PER_CALL
TOTAL_BYTES = TOTAL_BLOCKS * 512

# ファイルシステムの管理領域(FAT/ディレクトリ等)を踏まないよう、カード
# の先頭から十分離れた安全なオフセットから読む。読み込みのみなので万一
# 範囲が多少ずれても書き込み破損は起きないが、念のため sd.sectors で
# 上限もチェックする。
START_BLOCK = 20000


def mount_raw_sd():
    """os.mount() せず、SDCard オブジェクトだけを作って返す(ファイル
    システム層を経由しない生ブロックアクセス用)。"""
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


def bench_raw_sequential_read(sd):
    """生ブロックデバイスへの逐次マルチブロック読み込み(readblocks()
    を直接、ファイルシステムを経由せず呼ぶ)。sdcard.py 自体の速度だけ
    を切り分けて測る、最もノイズの少ない計測。"""
    start_block = START_BLOCK
    if start_block + TOTAL_BLOCKS > sd.sectors:
        # カードが小さい場合は末尾からTOTAL_BLOCKS分だけ手前に
        start_block = max(0, sd.sectors - TOTAL_BLOCKS - 16)

    buf = bytearray(BUF_SIZE)
    t0 = time.ticks_ms()
    for i in range(TOTAL_CALLS):
        sd.readblocks(start_block + i * BLOCKS_PER_CALL, buf)
    elapsed = time.ticks_diff(time.ticks_ms(), t0)

    kb = TOTAL_BYTES / 1024
    kbps = kb / (elapsed / 1000) if elapsed > 0 else float('inf')
    print("[raw block read]")
    print("  total: {} bytes ({} calls x {} bytes)".format(TOTAL_BYTES, TOTAL_CALLS, BUF_SIZE))
    print("  elapsed: {} ms".format(elapsed))
    print("  throughput: {:.1f} KB/s".format(kbps))
    return elapsed


def bench_mounted_file_read():
    """実運用と同じ経路(uos.mount() -> ファイルを開いて4KBずつ
    f.readinto())での計測。VFS/FATのオーバーヘッドも含んだ、より
    "体感"に近い数字。/sd 上に十分な大きさ(ここでは2MB超を想定)の
    既存ファイルが必要 -- TEST_FILE を環境に合わせて書き換えること。"""
    import uos
    # このプロジェクトのROM配置規約は /sd/msx/ (BIOS) および /sd 以下の
    # 任意フォルダ(カート、メニューのブラウザが走査する) -- 固定の
    # サブディレクトリを決め打ちせず、/sd 全体を再帰的に走査して最大の
    # ファイルを探す。
    TEST_FILE = "/sd"

    # ディレクトリの中から最大のファイルを探す簡易ロジック
    best_path, best_size = None, 0

    def scan(d):
        nonlocal best_path, best_size
        try:
            for name, typ, *_ in uos.ilistdir(d):
                p = d.rstrip('/') + '/' + name
                if typ & 0x4000:  # ディレクトリ
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
        print("[mounted file read] SKIP: no test file found under {}".format(TEST_FILE))
        return None

    print("[mounted file read] using {} ({} bytes)".format(best_path, best_size))
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
    print("=== sdcard.py benchmark ===")
    sd = mount_raw_sd()
    print("SD sectors: {} (~{} MB)".format(sd.sectors, sd.sectors // 2048))
    bench_raw_sequential_read(sd)
    print()
    # 生ブロックアクセスで使ったSPIバスをそのままファイルシステム経由の
    # 計測にも使い回すため、改めて uos.mount() する。
    import uos
    try:
        uos.mount(sd, '/sd')
    except OSError as e:
        print("mount failed (already mounted?): {}".format(e))
    bench_mounted_file_read()


main()
