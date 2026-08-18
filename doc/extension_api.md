# `msx` モジュール Python API リファレンス

## 概要

`msx` は `src/msx/modmsx.c` で実装されている MicroPython C ユーザーモジュールで、MSX1エミュレータのコア機能（`msx_core.c`）を Python から操作するための唯一の窓口です。

```python
import msx

msx.init()
with open('/sd/msx/MSX.ROM', 'rb') as f:
    msx.load_bios(f.read())
msx.load_cart(0, cart_bytes)
msx.init_display_hardware(1, 62_500_000, 11, 10, 9, 8, 7, 22, 480, 320)
msx.reset()

while True:
    msx.render_to_display_1to1()
    msx.run_frame()
    msx.wait_display()
```

エミュレータの状態はモジュール内部に単一のグローバルインスタンスとして保持されます（Pico1台につきMSXシステム1台）。全ての関数はこの暗黙のインスタンスに対して作用します。

---

## ライフサイクル

### `msx.init()`

エミュレータ状態を初期化する（ゼロクリア）。他のどの関数よりも先に一度だけ呼ぶ。二度目以降の呼び出しでも安全（前回確保したヒープメモリ・開いたままのメガロムファイルを自動的に解放してから初期化し直す）。

### `msx.reset()`

Z80 CPUをリセットする（PC=0から実行開始）。BIOS/カートリッジのロード後に呼ぶ。

### `msx.is_ready() -> bool`

`msx.init()` が呼ばれているかを返す。

---

## ROM 管理

### `msx.load_bios(data: bytes | bytearray) -> bool`

32KB の MSX BIOS/BASIC ROM をスロット0にロードする。`data` のサイズは 0x4000〜0x8000 の範囲内である必要がある。

### `msx.load_cart(slot: int, data: bytes | bytearray, mapper: int = 0) -> bool`

カートリッジROM全体をヒープにコピーしてロードする（フルRAMロード、32KB程度までの小容量カートリッジ向け）。

- `slot`: 0 = カートリッジスロット1、1 = カートリッジスロット2
- `mapper`: `msx.MAPPER_PLAIN`（0、デフォルト、32KB超なら自動判定）/ `msx.MAPPER_ASCII8`（1）/ `msx.MAPPER_ASCII16`（2）/ `msx.MAPPER_KONAMI`（3）

### `msx.load_cart_paged(slot: int, file_obj, size: int, mapper: int) -> bool`

大容量カートリッジ（メガロム、128KB〜1MB程度）をページング方式でロードする。ROM全体はRAMに載せず、バンク切替のたびに `file_obj` から該当8KBページを読み込む。

- `file_obj`: 既に `open(path, 'rb')` で開かれたシーク可能なファイルオブジェクト。**このモジュールが内部で参照を保持し続けるため、呼び出し側は close() してはいけない**（`msx.eject_cart()` が自動的に閉じる）。
- `mapper`: `msx.MAPPER_PLAIN` は不可（バンク切替が前提のため）。`msx.detect_mapper()` で事前に判定するのが一般的な使い方。
- 実用上は `mp/msx_menu.py` の `load_cart_smart()` を使うことを推奨（サイズに応じた自動振り分け、内蔵フラッシュへのキャッシュコピーも含む）。

### `msx.detect_mapper(data: bytes | bytearray) -> int`

指定したバイト列を対象に、既知のバンク切替書き込みパターン（`LD (nn),A` 命令が特定アドレス範囲に書き込むパターン）をスキャンしてマッパー種別を推定する。`msx.load_cart()` が32KB超のROMに対して内部的に使うのと同じヒューリスティックだが、`msx.load_cart_paged()` 用にROMの一部（先頭数KB程度で十分）だけを渡して判定するためにも使える（`msx.load_cart()` 内部のバージョンと異なり、渡したバイト列のサイズによる「32KB以下ならPLAIN」のショートカット判定は行わない）。

戻り値: `msx.MAPPER_*` のいずれか。

### `msx.eject_cart(slot: int)`

指定スロットのカートリッジを取り外す。フルRAMロード/ページング方式どちらでも、確保していたメモリとファイル参照を解放する。

### `msx.is_cart_paged(slot: int) -> bool`

指定スロットのカートリッジがページング（メガロム）方式でロードされているかを返す。

### `msx.get_cart_fetch_stats() -> (bankswitch_count: int, fetch_count: int)`

起動時からの累積カウンタを返す。`bankswitch_count`はページング方式カートのバンク選択レジスタへの書き込み回数（キャッシュヒット/ミス問わず全て）、`fetch_count`はそのうちキャッシュミスして内蔵フラッシュへの同期読み込みが発生した回数。メガロムのバンク切替頻度・キャッシュヒット率を実機で調査する際の低コストな診断用（常時カウントされるが取得しない限りコストなし）。

---

## 実行

### `msx.run_frame() -> int`

1フレーム分（約59659 T-states、60fps相当）のZ80実行・VDP走査線描画・PSGサンプル生成をまとめて実行する。生成された音声サンプルは内部リングバッファに自動的にプッシュされる。戻り値はこのフレームで生成されたサンプル数。

---

## クロック調整

### `msx.boost_peri_clock()`

`clk_peri`（SPI/UARTのボーレート生成元）を `clk_sys` に追従させる。この基板はデフォルトで `clk_peri` が48MHz固定になっており、SPIの実効ボーレートを大きく制限してしまうための対策。**呼び出し後は `machine.UART` オブジェクトを再生成する必要がある**（ボー分周が再計算されるため）。`usb_host.init()` も `clk_peri` を巻き戻すため、その後にもう一度呼ぶ必要がある。

### `msx.boost_dma_priority()`

DMAにバスファブリック上の最優先権を与える。過去にUSBホスト有効時の表示DMA遅延の原因として疑われ実装したが、実測では効果がないことが判明している（真因は `usb_host.init()` によるクロック巻き戻しだった）。副作用は無いため残置されている。

---

## 入力

### `msx.set_key_matrix(row: int, col_mask: int)`

キーボード行列の1行を設定する。`col_mask` はアクティブLow（0=押下、1=未押下）。通常は `msx_keymap.py` の `apply_hid_report()` 経由で呼ばれる。

### `msx.clear_keys()`

キーボード行列を全て未押下状態にリセットする。

### `msx.set_joystick(port: int, state: int)`

ジョイスティック状態を設定する。`port`: 0=JOY1, 1=JOY2。`state` はアクティブLowビットマスク: bit0=Up, bit1=Down, bit2=Left, bit3=Right, bit4=TriggerA, bit5=TriggerB（0=押下）。毎フレーム、GPIOポーリング結果を反映するために呼ぶ想定。

---

## 表示

### `msx.init_display_hardware(spi_id, baud, mosi, sck, cs, dc, rst, bl, lcd_w=480, lcd_h=320, rotate_180=False)`

LCD（ST7796/ILI9341、両者とも同一初期化シーケンス）をC側で完全初期化する（GPIO・SPI設定・リセットパルス・初期化コマンド送信・黒画面クリア・バックライトON）。

- `spi_id`: 0=spi0, 1=spi1
- `baud`: SPIクロック（Hz）。実測62.5MHzが安全上限。
- `mosi`/`sck`/`cs`/`dc`/`rst`/`bl`: 各GPIOピン番号
- `lcd_w`/`lcd_h`: パネルサイズ（省略時480×320=ST7796）。256×192のMSXネイティブ画面をこのサイズの中央に等倍表示する。
- `rotate_180`: `True` で画面を180度回転（MADCTLレジスタの変更のみ、座標計算には影響しない）

### `msx.setup_display(spi_id: int, cs_pin: int, dc_pin: int)`

軽量版。SPIが既にPython側ドライバで初期化済みの場合に、ハードウェアハンドルだけを記録する（`render_to_display()` 用）。`init_display_hardware()` を使う場合は不要。

### `msx.render_to_display_1to1()`

MSXネイティブ解像度256×192をそのままパネル中央にDMA転送開始する（非ブロッキング）。現在の本番描画経路。

### `msx.render_to_display()`

1.5倍拡大表示版（384×288にニアレストネイバー拡大してからDMA転送）。転送量増加により速度低下するため現在未使用だが、APIとして残置。

### `msx.wait_display()`

直前の `render_to_display*()` によるDMA転送の完了を待つ（ブロッキング）。次の `render_to_display*()` を呼ぶ前、またはSDカードアクセスの前に必ず呼ぶこと（SPIバスを共有しているため）。

### `msx.get_framebuf() -> memoryview`

内部フレームバッファ（RGB565、256×192×2バイト、ビッグエンディアン）への読み取り専用参照ビューを返す（ゼロコピー）。実行時メニュー等が `framebuf` モジュールで直接描画する際に使用。

---

## HDMIブリッジ出力

第2のPico 2 + PICO-HDMI-PLUS（`hdmi_bridge/README.md`参照）へLCDと同じSPI1バス（別CSピン）経由でフレームを送る、任意搭載のオプション機能。`config.txt`の`hdmi=1`で有効化（`mp/main.py`参照）。

### `msx.init_hdmi_output(cs_pin: int, baudrate: int)`

HDMIブリッジ出力を設定する。`init_display_hardware()`の**後**に呼ぶこと（LCD/SDと同じSPI1インスタンスを再利用するため）。`cs_pin`はLCD/SDとは別の専用チップセレクトGPIO（GP28）。呼び出し時に現在の16色パレットを1回送信する（`send_hdmi_palette()`を参照）。未呼び出しの間は他のHDMI系関数はすべてno-op。

### `msx.send_hdmi_palette()`

現在の16色パレット（RGB332に変換）をHDMIブリッジへ送信する。`init_hdmi_output()`が内部で1回呼ぶため、通常は手動で呼ぶ必要はない（MSX1のパレットは固定でハードウェア的に変化しないため）。

### `msx.render_to_hdmi()`

フレームバッファを4bitパレットインデックス（2ピクセル/バイト）に変換してHDMIブリッジへ送信する（ブロッキング）。LCD/SD SPI1バスがアイドルな時（`wait_display()`の後、SDアクセスと同時でない時）にのみ呼ぶこと。MSXの16色パレットの値しか使わないゲーム画面専用（メニュー/UI画面には`render_to_hdmi_raw332()`を使うこと）。

### `msx.render_to_hdmi_raw332()`

`render_to_hdmi()`と同様だが、パレット参照をせずフルRGB332（1ピクセル/バイト）で送信する。MSXの16色パレット外の任意色を使うメニュー/UI画面（`msx_menu.py`の`MenuCanvas`）向け。転送量は`render_to_hdmi()`の2倍。

### `msx.clear_hdmi()`

現在のフレームバッファの内容に関係なく、全黒の1フレームをHDMIブリッジへ送信する。`init_hdmi_output()`の直後に1回呼ぶことを想定しており、レシーバー側が表示し続けている前回のフレーム（別のエミュレータ/セッションの残像を含む）を、このエミュレータ自身の最初の実フレームが送られる前に消すために使う。

---

## 音声

### `msx.setup_audio_pwm(pin: int) -> bool`

指定GPIOピンにPWMオーディオ出力をセットアップする。10bit PWM（wrap=1023）、キャリア周波数は約234kHz（`clk_sys`依存）に固定し、22050Hzのリピートタイマー割り込みでサンプルを消費してPWMデューティに反映する。

### `msx.set_audio_volume(level: int)` / `msx.get_audio_volume() -> int`

音量を設定/取得する。`level` は 0〜256（256がデフォルト、元のフルスケール振幅）。実行時に即座に反映される。

### `msx.set_audio_filter(shift: int)` / `msx.get_audio_filter() -> int`

PSGサンプルに適用する1次IIRローパスフィルタの強さを設定/取得する。`shift`: 0（デフォルト、無効）〜15。値が大きいほど強くこもった音になる（カットオフ周波数 ≒ `(22050 >> shift) / (2π)` Hz）。パッシブブザーの耳障りな高周波成分を軽減する目的。搬送波自体（約234kHz）はハードウェアフィルタでしか除去できない点に注意。

### `msx.get_audio_ring_level() -> int`

オーディオリングバッファに現在キューされているサンプル数を返す。診断・ラグ監視用。

### `msx.get_audio_buf(n: int) -> memoryview`

このフレームで生成された音声サンプル（int16、先頭n個）への参照ビューを返す（ゼロコピー）。

---

## セーブステート

### `msx.get_ram_view() -> memoryview`

MSXのRAM（64KB）への読み取り可能な参照ビューを返す（ゼロコピー、`msx_state.ram` を直接参照）。

### `msx.get_vram_view() -> memoryview`

VDPのVRAM（16KB）への読み取り可能な参照ビューを返す（ゼロコピー）。

### `msx.get_state_header() -> bytearray` / `msx.set_state_header(data: bytes) -> bool`

CPUレジスタ・VDPレジスタ・マッパーバンク・I/O状態をまとめた64バイト固定長のヘッダを取得/設定する。詳細なフィールドレイアウトは `memory_map.md` を参照。RAM/VRAMはこのヘッダに含まれない（`get_ram_view()`/`get_vram_view()` で別途読み書きする）。

**セーブステートの実装例**（`mp/msx_menu.py` の `save_state_to()`/`load_state_from()` を参照）:

```python
def save_state_to(path):
    with open(path, 'wb') as f:
        f.write(msx.get_state_header())
        f.write(msx.get_ram_view())
        f.write(msx.get_vram_view())

def load_state_from(path):
    with open(path, 'rb') as f:
        msx.set_state_header(f.read(64))
        f.readinto(msx.get_ram_view())
        f.readinto(msx.get_vram_view())
```

大きな一時バッファを一切確保しない設計になっている点が重要（`dev_guide.md` 参照）。

---

## デバッグ API

いずれも通常のゲームプレイでは不要だが、ロジック検証や実機トラブルシューティングで有用。

### `msx.get_vdp_reg(reg: int) -> int`

VDPレジスタ（R0〜R7）の値を読む。

### `msx.debug_step(n: int) -> int`

VDP/オーディオ/フレームタイミングを一切考慮せず、生のZ80命令をn個実行する。実行後のPCを返す。

### `msx.debug_cpu() -> tuple`

`(pc, sp, a, f, cyc, halted, iff1, int_mode)` を返す。`cyc` は現在フレーム内の経過T-state数。

### `msx.debug_peek(addr: int) -> int`

現在の `slot_select` に従ってマッピングされたZ80アドレス空間の1バイトを読む（スロットマッピング経由）。

### `msx.debug_run_line(line: int, do_video: bool, do_audio: bool, do_int: bool)`

1スキャンライン分の処理を、映像/音声/割り込み生成をそれぞれ個別にON/OFFして実行する。特定サブシステムがハングの原因かどうかの切り分けに使う。

### `msx.debug_spi_baud() -> int`

直前の `render_to_display*()` で実際に達成されたSPIボーレート（Hz）を返す。

### `msx.debug_clocks() -> (clk_sys_hz, clk_peri_hz)`

現在の `clk_sys`/`clk_peri` の実測値を返す。USBホスト初期化等によるクロックの巻き戻しを検出するのに使う。

### `msx.debug_psg() -> tuple`

`(quality, clk, rate, base_incr, realstep, psgtime, psgstep, freq_limit, env_ptr, env_pause, env_continue, env_face, env_freq, env_count, freq0, count0)` — emu2149コアの内部状態を返す。

### `msx.debug_psg_calc(n: int)`

`PSG_calc()` をn回直接呼ぶ（音声生成のタイミング検証用）。

---

## モジュール定数

| 定数 | 値 | 意味 |
| :--- | :--- | :--- |
| `msx.MAPPER_PLAIN` | 0 | 非バンク切替 |
| `msx.MAPPER_ASCII8` | 1 | ASCII-8方式（8KBページ×4ウィンドウ） |
| `msx.MAPPER_ASCII16` | 2 | ASCII-16方式（16KBページ×2ウィンドウ） |
| `msx.MAPPER_KONAMI` | 3 | KONAMI方式（SCC無し、8KBページ×3可変ウィンドウ） |
| `msx.SCREEN_W` | 256 | MSXネイティブ画面幅 |
| `msx.SCREEN_H` | 192 | MSXネイティブ画面高さ |
| `msx.KEY_ROWS` | 11 | キーボード行列の行数 |
| `msx.AUDIO_RATE` | 22050 | オーディオサンプリングレート(Hz) |
| `msx.RAM_SIZE` | 65536 | RAM容量（バイト） |
| `msx.VRAM_SIZE` | 16384 | VRAM容量（バイト） |

---

## 関連ソースファイル

- `src/msx/modmsx.c` — 本APIの実装
- `src/msx/msx_core.h` — 対応するC言語APIの宣言・コメント
- `mp/main.py` / `mp/msx_menu.py` — 実際の使用例
