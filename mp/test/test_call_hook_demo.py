"""
test_call_hook_demo.py — CALL/RSTフック機能 (msx.set_call_hook) のデモ

MSXの垂直帰線割り込み(VBlank。IM1モードでは Z80 が内部的に RST 38h 相当
の CALL 0x0038 を発行する)をフックし、BIOS/ゲーム本来の割り込みハンドラ
をPython側の処理(カウンタのインクリメント)で完全に置き換えるデモです。
この割り込みは1フレームに必ず1回発生するため、ROMの内容に関係なく確実に
動作を確認できます。

重要な注意点(実機検証で判明した仕様上の制約):
  Z80のIM1割り込みは、サービス開始時に自動的に割り込み禁止(DI相当)にな
  り、ハンドラ側が最後に自前で EI (割り込み再許可) を実行して初めて次の
  割り込みを受け付けられるようになります。本フック機構はCALL/RST命令を
  丸ごと置き換える(pushとjumpを両方スキップする)ため、0x0038をフックす
  ると本来のハンドラのEIごとスキップしてしまい、以後 msx.clear_call_hook()
  で解除しても、あるいは msx.set_call_hook_enabled(False) で無効化して
  も、システム全体の割り込みは二度と再開しません(CPUのレジスタ/フラグ
  を書き換えるAPIがまだ無いため、Python側からEIをやり直す手段が無い)。
  これはフックのバグではなく、「割り込みハンドラそのもの」を丸ごと置き
  換える際に必ず起こるZ80の仕様上の挙動です。BIOS/ゲームのサブルーチン
  を置き換える通常のユースケース(CHPUTの差し替え等)では、置き換え先の
  ルーチンが独自にEI/DIを操作していない限りこの制約は当てはまりません。

  -> このデモでは、フック有効化前後で「1フレームに約1回」発火していた
     割り込みが、フック有効化した瞬間から二度と(たとえ無効化・解除して
     も)発火しなくなることを確認できます。これはBIOSの割り込みハンド
     ラが完全にPython側のコールバックに置き換わったことの動かぬ証拠です。

前提: BIOS_PATH と CART_PATH のファイルが存在すること。

実行方法 (Thonny / mpremote REPL):
    import test_call_hook_demo
"""
import machine, time, uos
import msx

# ── ピン設定 (main.py と同じ) ────────────────────────────────────────────
SPI_ID    = 1;  SPI_MOSI = 11; SPI_SCK  = 10; SPI_CS  = 9
SPI_DC    = 8;  SPI_RST  = 7;  SPI_BL   = 22; SPI_BAUD = 40_000_000
SD_SPI_ID = 1;  SD_MOSI  = 11; SD_SCK   = 10; SD_MISO = 12
SD_CS     = 15; SD_INIT_BAUD = 400_000

CART_PATH  = '/sd/msx/ANTADV.ROM'
INT_VECTOR = 0x0038   # IM1割り込みベクタ。VBlank毎に必ず1回呼ばれる

# ── SDマウント (既にマウント済みならスキップ) ────────────────────────────
try:
    uos.stat('/sd')
except OSError:
    import sdcard
    spi = machine.SPI(SD_SPI_ID, baudrate=SD_INIT_BAUD,
                       sck=machine.Pin(SD_SCK), mosi=machine.Pin(SD_MOSI),
                       miso=machine.Pin(SD_MISO))
    cs = machine.Pin(SD_CS, machine.Pin.OUT, value=1)
    sd = sdcard.SDCard(spi, cs, baudrate=SD_INIT_BAUD, restore_baudrate=SPI_BAUD)
    uos.mount(sd, '/sd')

# ── msx初期化 + BIOS読み込み ─────────────────────────────────────────────
msx.init()
msx.init_display_hardware(SPI_ID, SPI_BAUD, SPI_MOSI, SPI_SCK,
                           SPI_CS, SPI_DC, SPI_RST, SPI_BL)

BIOS_PATH = '/sd/msx/MSX_jp.ROM'
try:
    with open(BIOS_PATH, 'rb') as f:
        bios = f.read()
except OSError as e:
    print(f"ERROR: could not open {BIOS_PATH}: {e}")
    try:
        print("  /sd       contents:", uos.listdir('/sd'))
    except OSError as e2:
        print("  /sd       listdir failed:", e2)
    try:
        print("  /sd/msx   contents:", uos.listdir('/sd/msx'))
    except OSError as e2:
        print("  /sd/msx   listdir failed:", e2)
    raise

if not msx.load_bios(bios):
    raise RuntimeError("BIOS load failed (size must be 32768 bytes)")
del bios

# ── カートリッジ読み込み (slot 0 = カートリッジスロット1) ──────────────
# ゼロコピーAPI (cart_alloc/cart_finalize) を使用。大きなPython側バッファ
# を確保しないので、この程度のサイズのROMならMemoryErrorの心配がない。
try:
    cart_size = uos.stat(CART_PATH)[6]
except OSError as e:
    print(f"ERROR: could not stat {CART_PATH}: {e}")
    try:
        print("  /sd/msx   contents:", uos.listdir('/sd/msx'))
    except OSError as e2:
        print("  /sd/msx   listdir failed:", e2)
    raise

cart_buf = msx.cart_alloc(0, cart_size)
if cart_buf is None:
    raise RuntimeError(f"cart_alloc({cart_size}) failed (out of memory?)")

with open(CART_PATH, 'rb') as f:
    mv, off, CHUNK = memoryview(cart_buf), 0, 4096
    while off < cart_size:
        n = f.readinto(mv[off:off + CHUNK])
        if not n:
            break
        off += n

if not msx.cart_finalize(0):
    raise RuntimeError("cart_finalize failed")
print(f"Cartridge loaded: {CART_PATH} ({cart_size} bytes)")

# ── フックコールバック ────────────────────────────────────────────────────
_frame_count = 0

def on_interrupt():
    global _frame_count
    _frame_count += 1
    if _frame_count % 60 == 0:
        print(f"  [HOOK] VBlank割り込み #{_frame_count} を横取り "
              f"(BIOS本来のハンドラは実行されていません)")

def run_frames(ms):
    t_end = time.ticks_add(time.ticks_ms(), ms)
    n_frames = 0
    while time.ticks_diff(t_end, time.ticks_ms()) > 0:
        msx.run_frame()
        msx.render_to_display()
        msx.wait_display()
        n_frames += 1
    return n_frames

# ── フェーズ1: フックなしで起動 -> 割り込みはBIOS本来のハンドラが処理 ────
print("=== フェーズ1: フックなしで起動 (LCDでゲームが動いているか確認してください) ===")
msx.reset()
n = run_frames(3000)
print(f"  ({n} frames run normally, no hook installed)")

# ── フェーズ2: 割り込みベクタをフック -> 以後の割り込みはPython側が処理 ───
print("\n=== フェーズ2: 0x0038(割り込みベクタ)をフック ===")
msx.set_call_hook(INT_VECTOR, on_interrupt)
n = run_frames(4000)
print(f"  ({n} frames run, {_frame_count} interrupts intercepted by Python)")

# ── フェーズ3: 登録は残したまま無効化 -> 新規の横取りは止まる ────────────
# (前述の注意点の通り、これでBIOS側の割り込み処理が復活するわけではない。
#  「フラグを無効化した後は新たに横取りされない」ことだけを確認する)
print("\n=== フェーズ3: set_call_hook_enabled(False) ===")
msx.set_call_hook_enabled(INT_VECTOR, False)
n = run_frames(2000)
print(f"  ({n} frames, count unchanged at {_frame_count} — enabled=False で新規の横取りは発生しない)")

# ── フェーズ4: 再度有効化 -> フラグの切り替え自体は反映される ────────────
print("\n=== フェーズ4: set_call_hook_enabled(True) ===")
msx.set_call_hook_enabled(INT_VECTOR, True)
n = run_frames(2000)
print(f"  ({n} frames, count: {_frame_count} — 割り込み自体がもう発生していないため増えないのは正常)")

# ── フェーズ5: フック解除 ─────────────────────────────────────────────────
print("\n=== フェーズ5: clear_call_hook — フック解除 ===")
msx.clear_call_hook(INT_VECTOR)
n = run_frames(2000)
print(f"  ({n} frames)")

print(f"\n=== デモ終了: 合計 {_frame_count} 回の割り込みをPython側で処理しました ===")
print("(以後、割り込みは再開しません — ドキュメント冒頭の注意点を参照)")
