# 開発ガイド

## 1. システム・アーキテクチャ

全体構成・設計方針は `architecture.md` を参照。要点:

- 性能クリティカルな部分（Z80/VDP/PSGエミュレーション）は C で `msx_core.c`/`.h` に実装し、`msx` という名前の MicroPython C ユーザーモジュール（`modmsx.c`）として公開する。
- 起動フロー・UI・設定は MicroPython (`mp/*.py`) で実装する。
- C コアは Pico SDK 依存部分を `#ifdef __arm__` で分離しており、**ネイティブ gcc でホストビルドしてロジックを検証できる**（後述）。

---

## 2. ディレクトリ構造

```
MSX_emu_pico2/
├── bldfrm_msx.sh              # ビルドスクリプト
├── firmware/                  # ビルド成功時に自動コピーされるfirmware_msx.uf2置き場
├── doc/                       # このドキュメント群
├── src/msx/
│   ├── msx_core.h             # msx_state_t / 公開API宣言
│   ├── msx_core.c             # MSXシステム本体（Z80/VDP/PSG統合、メモリ管理、フレームループ、表示DMA）
│   ├── modmsx.c               # MicroPython Cユーザーモジュール（`msx`）
│   ├── micropython_msx.cmake  # CMakeビルド設定
│   ├── z80/                   # superzazu/z80 (MIT) — Z80 CPUコア
│   ├── tms9918/                # visrealm/vrEmuTms9918 (MIT) — TMS9918A VDPコア
│   └── emu2149/                # digital-sound-antiques/emu2149 (MIT) — AY-3-8910 PSGコア
└── mp/
    ├── main.py             # 起動フロー・メインループ
    ├── msx_menu.py             # ROM選択・実行時メニュー・セーブステート・カートリッジロード
    └── msx_keymap.py           # USB HID → MSXキー行列マッピング
```

> 同じリポジトリ内には元になった PB-1000 エミュレータの C ソース資産（`src/*.c`）も同居しているが、MSXエミュレータとは独立しており、このガイドの対象外。USBホストコア（`src/usb_host_core.c`、`src/modusb_host.c`）のみ両者で共有されている。PB-1000のPythonソース（`mp/`配下）は別プロジェクトでソース管理されているため、このリポジトリの`mp/`にはMSXエミュレータのファイルのみが存在する。

---

## 3. C モジュール詳細

### `msx_core` モジュール（`src/msx/msx_core.c` / `.h`）

MSXシステムそのもの。Pico SDK に依存しないよう設計されており、以下のように２系統でコンパイルされる:

- **実機ビルド**（`#ifdef __arm__` 内）: LCD初期化・DMA転送・クロック設定など、実際のGPIO/SPI/DMAペリフェラルを操作する関数群
- **ホストビルド**（`#else` 側）: 上記のスタブ実装（何もしない）。ロジック検証専用

公開APIの主な分類:

| カテゴリ | 関数例 |
| :--- | :--- |
| 初期化 | `msx_init()`, `msx_load_bios()`, `msx_reset()`, `msx_destroy()` |
| カートリッジ | `msx_load_cart()`, `msx_load_cart_paged()`, `msx_set_cart_fetch_cb()`, `msx_detect_mapper()`, `msx_eject_cart()` |
| 実行 | `msx_run_frame()` |
| I/O | `msx_mem_read()`/`msx_mem_write()`, `msx_port_read()`/`msx_port_write()`, `msx_set_key_matrix()`, `msx_set_joystick()` |
| 表示 | `msx_init_display_hardware()`, `msx_render_to_display_1to1()`, `msx_render_to_display()`, `msx_wait_display()` |
| クロック調整 | `msx_boost_peri_clock()`, `msx_boost_dma_priority()` |
| セーブステート | `msx_save_state_header()`, `msx_load_state_header()`, `msx_get_vram_ptr()` |
| デバッグ | `msx_debug_step()`, `msx_debug_get_cpu()`, `msx_debug_peek()`, `msx_debug_get_psg()` 等 |

### `msx` モジュール（`src/msx/modmsx.c`）

`msx_core` の機能を Python から呼べるようにするブリッジ。完全な関数一覧・シグネチャは `extension_api.md` を参照。

実装上のポイント:

- グローバルな `static msx_state_t msx_state;` を1つだけ保持（Pico1台=MSX1台の前提）。
- Python バッファ（`bytes`/`bytearray`）とのやり取りは `mp_get_buffer_raise()` で取得したポインタをそのまま C コアに渡す（コピーしない）。
- 大きなデータ（RAM/VRAM/フレームバッファ）を Python に渡す際は `mp_obj_new_bytearray_by_ref()` でゼロコピーの参照ビューを返す。
- メガロムのページフェッチでは、Python 側で開いたファイルオブジェクトを `MP_REGISTER_ROOT_POINTER` で MicroPython の GC ルートポインタとして保持し、C から `mp_stream_seek()`/`mp_stream_read_exactly()` で直接読み込む（Pythonメソッド呼び出しのオーバーヘッドなし、例外はerrcodeベースで受け取る）。

---

## 4. ホストビルドによるロジック検証

実機への書き込み・接続は時間がかかるため、**ロジックの正しさはまず実機を使わずネイティブ gcc で検証する**のがこのプロジェクトの標準ワークフロー。

```bash
mkdir -p /tmp/msx_hosttest && cd /tmp/msx_hosttest
cp -r <repo>/src/msx ./msx
gcc -I. -o test_boot my_test.c msx/msx_core.c msx/z80/z80.c \
    msx/tms9918/vrEmuTms9918.c msx/emu2149/emu2149.c -lm -Wall
./test_boot
```

`msx_core.h`/`.c` は `#ifdef __arm__` により Pico SDK ヘッダへの依存を排除しているため、この方法でそのままコンパイルできる。`modmsx.c` は MicroPython のヘッダ（`py/runtime.h` 等）に依存するためホストビルドの対象外。

テスト用の `.c` ファイルは `msx_init()` → `msx_load_bios()` → `msx_reset()` → `msx_run_frame()` のループを回し、`msx.cpu.pc` や VRAM内容（`vrEmuTms9918VramValue()`）を直接検査する形が典型パターン。バンク切り替え・メガロムのページキャッシュ動作なども、フェイクの `msx_cart_fetch_fn` コールバック（メモリ上の配列から読むだけ）を登録して同様に検証できる。

---

## 5. 新しいマッパー種別を追加する

1. `msx_core.h` に `MSX_MAPPER_*` の新しい定数を追加する。
2. `msx_core.c` の `cart_page_ptr()`（読み取り経路）に、そのマッパーのウィンドウ→バンク番号のマッピングロジックを追加する。
3. `cart_mapper_write()` に、そのマッパーのバンク切替アドレス検知ロジックを追加する。ページング（メガロム）対応も同時に行う場合は `cart_page_refill()` の呼び出しも追加する。
4. 必要であれば `detect_mapper()`（`scan_mapper_patterns()`）のヒューリスティックに検出パターンを追加する。
5. `modmsx.c` の `MP_QSTR_MAPPER_*` 定数登録に新しいマッパーを追加する。
6. ホストビルドでバンク切替の読み書きが正しいアドレスに反映されることを確認してから実機テストする。

---

## 6. 新しい `msx.*` Python API を追加する

1. `modmsx.c` に `static mp_obj_t msx_py_xxx(...)` 関数を実装する。
2. 引数の数に応じて `MP_DEFINE_CONST_FUN_OBJ_0`〜`_3` または可変長なら `MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN` でラップする（固定引数は最大3つまで、4つ以上は可変長マクロを使う）。
3. `msx_module_globals_table[]` に `{ MP_ROM_QSTR(MP_QSTR_xxx), MP_ROM_PTR(&msx_py_xxx_obj) }` を追加する。
4. Pico SDK 依存の実装であれば `#if HAVE_PICO_SDK` で囲む。
5. `extension_api.md` に追記する。

---

## 7. 設定ファイル（`config.txt`）に新しいキーを追加する

`mp/msx_menu.py` の `load_config()` はシンプルな `key=value` パーサで、キーの意味づけは呼び出し側（`main.py`）が行う。新しいキーを追加する場合:

1. `main.py` の `run()` 内、`cfg = load_config(CONFIG_PATH)` 以降の該当箇所に `cfg.get('新キー', デフォルト値)` を追加する。
2. 実行時メニューからも変更可能にしたい場合、`msx_menu.py` の `save_config()`（既存キーを保持したまま追記/上書きする実装）を使って書き戻す処理を追加する。

> **注意**: MicroPython のファイルオブジェクトには `writelines()` が無い（`AttributeError`）。設定ファイルを書き換える処理は、複数行を `"".join(lines)` で1つの文字列にまとめてから `f.write()` で一括書き込みすること（部分書き込みによるファイル破損を防ぐため）。

---

## 8. デバッグ手法

### 実機での slot_select / メモリ内容の調査

`msx.get_state_header()` の返り値（64バイト）の各オフセット（`memory_map.md` 参照）を直接読むことで、実行を止めずにCPU状態やスロット選択状態をスナップショットできる。特定のスロット状態を強制したい場合は `msx.set_state_header()` で該当バイトを書き換えてから `msx.debug_peek()` で読むと、そのスロットマッピング下のメモリ内容を直接確認できる。

### FPS計測

`main.py` のメインループは300フレームごとに実測FPSと音声リングバッファの残量をprintする。`mpremote exec` で `main.run()` をバックグラウンドスレッドで起動し、しばらく待ってから出力を確認するワークフローが実機検証で有効。

### よくある調査の落とし穴

- **稼働中のバックグラウンドスレッドがSDカードをマウントしている状態で、別のワンショットスクリプトから`uos.mount()`/`uos.umount()`を行うと、稼働中スレッドのマウントが巻き添えで解除される。** 診断スクリプトを書く前に `uos.listdir('/')` に `sd` が出るか確認し、既にマウント済みなら再マウントしないこと。
- **`machine.reset()` は SRAM の内容をゼロクリアしない。** ヒープ上の未初期化領域は前回実行時のデータが残ったままになりうる。「初期化直後に取得したはずのデータが古い値に見える」場合、初期化処理の実行順序（例: コールバック登録が実際のデータ取得より後になっていないか）を疑うこと。

---

## 9. コーディング規約・注意点

- 大きな一時バッファ（数十KB以上）の確保は極力避け、既存のメモリ（RAM/VRAM/フレームバッファ）へのゼロコピー参照で済ませる。MicroPythonのGCヒープは非圧縮のため、名目上の空き容量が十分でも大きな連続割当は失敗しうる。
- 新しい静的バッファをCヒープや `.bss` に追加する際は、`gc.mem_free()` を実機で確認し、GCヒープを圧迫しすぎていないか検証する。
- Pico SDK のサブシステム（USBホスト等）を初期化すると、暗黙に `clk_sys`/`clk_peri` 等のクロック設定が変更されることがある。`msx.debug_clocks()` で実測して確認する習慣をつけること。
