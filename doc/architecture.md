# MSX1 エミュレータ アーキテクチャ

## 目的

この文書は、Raspberry Pi Pico 2 (RP2350) + MicroPython で動作する MSX1 エミュレータの全体構成を整理し、各モジュールの責務と依存関係を明確にするためのものである。

## 設計方針

- **性能が必要な部分（CPU/VDP/PSGエミュレーション、メモリ管理、表示DMA）は C 言語で実装**し、MicroPython の C ユーザーモジュールとして `msx` モジュールに公開する。
- **起動フロー・設定・UI・入力処理は MicroPython (Python) で実装**し、素早い試行錯誤とファイル操作（SDカード、config.txt等）のしやすさを活かす。
- C 側は「MSXシステムそのもの」を表現し、ホスト環境（Pico2 実機か、開発用ネイティブビルドか）を意識しない設計にする（`#ifdef __arm__` で Pico SDK 依存部分のみ分離）。これにより、ロジックをネイティブ gcc でホストビルドして実機を使わずに検証できる。

---

## レイヤー構成

```text
┌─────────────────────────────────────────────┐
│  mp/main.py   起動フロー・メインループ    │
│  mp/msx_menu.py   ROM選択/実行時メニュー/セーブ │
│  mp/msx_keymap.py USB HID → MSXキー行列変換   │
└───────────────────┬───────────────────────────┘
                     │ import msx  (Cユーザーモジュール)
┌───────────────────▼───────────────────────────┐
│  src/msx/modmsx.c     MicroPython ⇔ C 橋渡し層   │
│    - msx_state_t のグローバルインスタンスを保持    │
│    - Python オブジェクト ⇔ C 構造体の変換          │
│    - PWM音声・ファイルI/O(メガロムfetch)のPico SDK依存部 │
└───────────────────┬───────────────────────────┘
                     │
┌───────────────────▼───────────────────────────┐
│  src/msx/msx_core.c/h   MSXシステム本体（純C99）  │
│    - スロット/メモリ管理・I/Oポート               │
│    - フレームループ（run_frame）                 │
│    - セーブステート                              │
│    - LCD初期化・DMA転送                          │
├─────────────────────────────────────────────┤
│  src/msx/z80/          Z80 CPUコア (superzazu/z80) │
│  src/msx/tms9918/      VDPコア (vrEmuTms9918)     │
│  src/msx/emu2149/      PSGコア (emu2149)          │
└─────────────────────────────────────────────┘
```

---

## C コア (`src/msx/msx_core.c` / `.h`)

### エミュレートしているハードウェア

| コンポーネント | 実装 | 備考 |
| :--- | :--- | :--- |
| CPU | Z80 @ 3.579545MHz | [superzazu/z80](https://github.com/superzazu/z80)（MIT）を使用 |
| VDP | TMS9918A | [vrEmuTms9918](https://github.com/visrealm/vrEmuTms9918)（MIT）を使用。256×192描画、パレット16色 |
| PSG | AY-3-8910 相当 | [emu2149](https://github.com/digital-sound-antiques/emu2149)（MIT）を使用。クロックは実機同様 Z80クロックの半分（1.789772MHz） |
| PPI | i8255 簡易実装 | スロット選択・キーボード行列読み取り |
| メモリ | 4スロット×4ページ(16KB) | スロット0=BIOS, 1/2=カートリッジ, 3=RAM |

### メモリ管理とカートリッジマッパー

`msx_state_t`（`msx_core.h`）が全エミュレーション状態を保持する単一の構造体。カートリッジは2つのロード方式を持つ：

1. **フル RAM ロード** (`msx_load_cart()`) — ROM全体をヒープに `malloc()` してコピー。32KB以下の非バンク切替カートリッジ向け。
2. **ページング方式** (`msx_load_cart_paged()`) — バンク切替の大容量カートリッジ（いわゆるメガロム、128KB〜1MB）向け。ROM全体をRAMに載せず、**現在バンク切替で選択されている4つの8KBウィンドウ分（常時32KB固定）だけ**をキャッシュに保持する。バンク切替の書き込みを検知すると、呼び出し側が登録したコールバック（`msx_cart_fetch_fn`）経由で該当8KBページを取得する。C コア自体はストレージの種類（SDカードか内蔵フラッシュか）を意識しない。

対応マッパー: `MSX_MAPPER_PLAIN`（非バンク切替）/ `MSX_MAPPER_ASCII8` / `MSX_MAPPER_ASCII16` / `MSX_MAPPER_KONAMI`（SCC無し）。ROMの内容をスキャンしてバンク切替パターンを検出する簡易ヒューリスティック（`detect_mapper()`）による自動判定にも対応。

詳細なアドレスマップは `memory_map.md` を参照。

**メガロムのFPS低下について(2026-08-14実機計測)**: 非メガロムが52〜54FPS前後なのに対し、メガロムは40〜50FPS(激しいバンク切替時は18FPSまで落ちることも)。`msx_get_cart_fetch_stats()`(`msx_core.c`、`msx.get_cart_fetch_stats()`としてPythonにも公開済み)で計測したところ、少なくとも1タイトルでは**バンク切替のキャッシュヒット率が0%**(切替のたびに毎回内蔵フラッシュへの同期読み込みが発生)だった。1フェッチあたりのコストは実測で約0.4〜0.7ms/8KBで、これはRP2350のQSPIフラッシュのXIP読み出し帯域(概算20MB/s程度)からみて、ソフトウェア(littlefs/MicroPythonストリームAPI)のオーバーヘッドではなく**物理的な転送時間そのもの**が支配的である可能性が高いと判断した。内蔵フラッシュの固定領域を確保しlittlefsを完全に迂回する高速化案も検討したが、実機テストでビルド設定に想定外の副作用(誤ったボード向けバイナリが生成される現象)が発生したため撤回した。この経緯の詳細はプロジェクトメモリを参照。

### フレームループ

`msx_run_frame()` が1フレーム（59659 T-states、60fps相当）分の Z80 実行・VDP走査線描画・PSGサンプル生成をまとめて実行する。フレームバッファはダブルバッファ構成（`framebuf[2]`）になっており、表示用DMA転送と次フレームのエミュレーションを並行実行できる（後述のパイプライン処理）。

### 表示出力

`msx_render_to_display_1to1()` が MSX ネイティブ解像度 256×192 をそのままパネル中央にDMA転送する（本番の描画経路）。1.5倍拡大表示するパス（`msx_render_to_display()`）も実装として残っているが、転送量増加により体感速度が大きく低下するため現在は未使用。

---

## MicroPython ブリッジ層 (`src/msx/modmsx.c`)

`msx` という名前の MicroPython C ユーザーモジュールとして、`msx_core.c` の機能を Python から呼べる関数群として公開する。主な役割:

- グローバルな `static msx_state_t msx_state` インスタンスの保持（Pico1台につきMSXシステム1台）
- Python の `bytes`/`bytearray`/ファイルオブジェクトと C 側バッファの受け渡し（ゼロコピー実装が基本方針、下記参照）
- PWM オーディオ出力（`hardware/pwm.h`、リングバッファ経由でフレーム生成した音声サンプルを22050Hzのタイマ割り込みでPWMデューティに反映）
- メガロムのページング fetch コールバック（`cart_fetch_from_pyfile()`）— Python 側で開いたファイルオブジェクトを MicroPython の GC ルートポインタ（`MP_REGISTER_ROOT_POINTER`）として保持し、`mp_stream_seek()`/`mp_stream_read_exactly()` で直接読み込む
- デバッグ用API群（`debug_step`/`debug_cpu`/`debug_peek`/`debug_psg`等）

完全な関数一覧は `extension_api.md` を参照。

### ゼロコピー設計の徹底

MicroPythonのGCヒープは**非圧縮（non-compacting）**であり、生きたオブジェクトが多少散らかった状態では、たとえ「合計の空き容量」が十分でも大きな連続ブロックの確保が高確率で失敗する（`MemoryError`）。この教訓から、以下は徹底してゼロコピー（既存のC側メモリをそのまま `bytearray_by_ref` 等で参照）にしている:

- `msx.get_ram_view()` — `msx_state.ram`（64KB）を直接参照
- `msx.get_vram_view()` — VDPコアの内部VRAM（16KB）を直接参照
- `msx.get_framebuf()` — 表示用フレームバッファを直接参照
- セーブステート — ヘッダ（64バイトのみ、CPUレジスタ・VDPレジスタ等）と RAM/VRAM を別々に読み書きし、80KB超の一時バッファを一切確保しない

---

## Python 層 (`mp/`)

### `main.py` — 起動フローとメインループ

役割:

- ハードウェア初期化（クロック調整、UART、SDマウント、LCD初期化、USBホスト、PWMオーディオ）
- `config.txt` の読み込みと反映（BIOS/カートリッジパス、LCDパネル種別、回転、音量/フィルタ）
- BIOS・カートリッジのロード（`msx_menu.load_cart_smart()` に委譲）
- USBキーボード・ジョイスティックのポーリング
- メインループ（下記）

メインループは**パイプライン処理**を採用しており、フレーム N の表示DMA転送とフレーム N+1 のZ80/VDPエミュレーションを並行実行する（ダブルバッファのフレームバッファにより安全）。これにより単純な逐次実行に比べて実測で約1.5倍のFPS向上を達成している。

> **注意**: バンク切替時のROMフェッチ（メガロム）が LCD/SD と同じ SPI1 バスを使う SD カードに対して行われる設計だと、このパイプライン処理と衝突して DMA 転送中の SD 読み込みが `I/O error` を起こす。現在の実装ではメガロムのフェッチ元を Pico2 内蔵フラッシュ（QSPIバス、SPI1とは独立）に限定することでこの問題を回避し、パイプライン処理を安全に維持している（詳細は `usage_guide.md` のメガロム節を参照）。

### `msx_menu.py` — ROM選択・実行時メニュー・カートリッジロード

- `select_rom()` — SDカード上の `.ROM` ファイル一覧から選択するUI（USBキーボード操作、タイムアウトで自動選択）
- `load_cart_smart()` — カートリッジサイズに応じてフル RAM ロード / メガロムページングを自動選択。メガロムの場合、SDカードから Pico2 内蔵フラッシュへ一度だけコピーし（`/megarom_cache.rom`）、以降はそこから読み込む
- `save_state_to()` / `load_state_from()` — ゼロコピー設計のセーブステート実装
- `show_emulator_menu()` — GUI+F7 で起動するランタイムメニュー（カートリッジ交換・セーブ/ロード・音量/フィルタ調整・リセット）
- `load_config()` / `save_config()` — `config.txt` の読み書き

### `msx_keymap.py` — キーボードマッピング

USB HID キーコード（修飾キー＋最大6キー同時押し）を MSX のキーボード行列（11行×8ビット、アクティブLow）に変換する。実機と同様、実際にどの文字がどのキーに割り当たるかは使用しているBIOS ROMの地域仕様（日本語版/英語版）に依存する。

---

## 依存方向

```text
main.py
  -> msx (Cモジュール, import msx)
  -> msx_keymap.py
  -> msx_menu.py

msx_menu.py
  -> msx (Cモジュール)
  -> uos / framebuf (MicroPython標準)

msx (modmsx.c)
  -> msx_core.c/h
  -> hardware/pwm.h, hardware/spi.h 等 (Pico SDK、#ifdef __arm__ でガード)

msx_core.c
  -> z80/z80.c   (Z80コア)
  -> tms9918/vrEmuTms9918.c  (VDPコア)
  -> emu2149/emu2149.c       (PSGコア)
```

---

## 起動シーケンス（実行時フロー）

1. `main.py` の `run()` が呼ばれる（通常 `boot.py` からの手動 `import main; main.run()`、または本番運用では `main.py` として自動起動）
2. `msx.init()` — C側状態をゼロ初期化、フェッチコールバック登録
3. `msx.boost_peri_clock()` — `clk_peri` を `clk_sys` に追従させ、SPI/UARTのボーレート制限を解除
4. USBホスト初期化（`usb_host.init()`。この際 `clk_sys` が240MHzに再設定されるため `boost_peri_clock()` を再度呼ぶ必要がある）
5. SDカードマウント
6. `config.txt` 読み込み
7. LCD初期化（`msx.init_display_hardware()`、パネル種別/回転設定を反映）
8. BIOS ROM ロード（`msx.load_bios()`）
9. カートリッジロード（`config.txt` の `cart=` 指定 → 未指定なら対話式選択メニュー → 選択なしなら MSX BASIC 起動）
10. PWMオーディオセットアップ、音量/フィルタ設定を反映
11. `msx.reset()` でZ80リセット、メインループ開始
12. メインループ: 表示DMA開始 → 次フレーム計算（パイプライン） → キーボード/ジョイスティックポーリング → DMA完了待ち、を無限に繰り返す

---

## 今後の拡張ポイント

- `main.py` としての自動起動デプロイ（現状は手動 `import main; main.run()` でのテスト運用）
- マルチスロットカートリッジ（スロット2）・ASCII-8/ASCII-16マッパーの実機動作検証（現状 KONAMI マッパーのみ実機確認済み）
- KONAMI SCC（拡張音源チップ）搭載カートリッジへの対応（現在は SCC 無しの KONAMI4 相当マッパーのみ対応）
