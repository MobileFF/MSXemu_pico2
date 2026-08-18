# MSX1 エミュレータ メモリマップ

## 概要

MSX1の標準的なスロット/ページ方式メモリ構成をエミュレートしている。Z80の16ビットアドレス空間（0x0000–0xFFFF）は4つの16KBページに分割され、各ページは独立に4つの「スロット」のいずれかを選択できる。

```
0x0000 ┌─────────────────┐
       │ ページ0 (16KB)    │ ← slot_select bit[1:0]
0x4000 ├─────────────────┤
       │ ページ1 (16KB)    │ ← slot_select bit[3:2]
0x8000 ├─────────────────┤
       │ ページ2 (16KB)    │ ← slot_select bit[5:4]
0xC000 ├─────────────────┤
       │ ページ3 (16KB)    │ ← slot_select bit[7:6]
0xFFFF └─────────────────┘
```

`slot_select`（PPIポートA、I/Oポート0xA8）の各2ビットが対応ページのスロット番号（0-3）を選択する。

| スロット番号 | 内容 | 実装 |
| :--- | :--- | :--- |
| 0 | BIOS/BASIC ROM (32KB) | `msx->bios[]`（固定、0x0000-0x7FFF相当を占有。0x8000以降は0xFFを返す） |
| 1 | カートリッジスロット1 | `msx->cart[0]` または ページング方式の `msx->cart_cache[0]` |
| 2 | カートリッジスロット2 | `msx->cart[1]` または ページング方式の `msx->cart_cache[1]`（現状ほぼ未使用・未検証） |
| 3 | RAM (64KB) | `msx->ram[]`（固定） |

起動直後（`msx_reset()` 直後）は `slot_select = 0x00`、つまり全ページがスロット0（BIOS）を指す。実際のスロット切り替えはBIOSの起動処理がポート0xA8への書き込みを通じて行う。

---

## カートリッジ領域内のバンク切り替え（アドレス 0x4000–0xBFFF）

非バンク切替（`MSX_MAPPER_PLAIN`）のカートリッジは 0x4000 起点でROMを線形にマッピングするだけだが、バンク切替マッパーは 0x4000–0xBFFF を4つの8KBウィンドウに分けて、それぞれ独立にROM上の任意の8KBページを選択できる。

| ウィンドウ | アドレス範囲 | ASCII-8 バンクレジスタ書き込み先 | ASCII-16 | KONAMI |
| :--- | :--- | :--- | :--- | :--- |
| 0 | 0x4000–0x5FFF | 0x6000–0x67FF | 0x6000–0x67FF（16KB単位、win0+1をまとめて選択） | 固定（常にROMページ0） |
| 1 | 0x6000–0x7FFF | 0x6800–0x6FFF | 〃 | 0x6000–0x6FFF |
| 2 | 0x8000–0x9FFF | 0x7000–0x77FF | 0x7000–0x77FF（16KB単位、win2+3をまとめて選択） | 0x8000–0x8FFF |
| 3 | 0xA000–0xBFFF | 0x7800–0x7FFF | 〃 | 0xA000–0xAFFF |

- **ASCII-8**: 4ウィンドウそれぞれ独立に8KBページ番号（0-255）を書き込む。
- **ASCII-16**: 2つの16KBレジスタ（0x6000系・0x7000系）で、それぞれ2ウィンドウ分（16KB＝8KB×2）を同時に切り替える。
- **KONAMI**（SCC無し）: ウィンドウ0は常にROMページ0固定（切替不可）。ウィンドウ1-3のみ切替可能。

実装は `src/msx/msx_core.c` の `cart_page_ptr()`（読み取り経路）と `cart_mapper_write()`（バンク切替書き込みの検知）を参照。

### メガロム（ページング方式）でのキャッシュ

大容量カートリッジ（`msx_load_cart_paged()` でロードしたもの）は、上記4ウィンドウに対応する8KB×4＝32KBのキャッシュ（`msx->cart_cache[]`）のみを常時保持する。バンク切替が発生すると、該当ウィンドウの8KB分だけを登録済みコールバック経由で取得し直す。ROMの実サイズ（最大1MB程度まで想定）に関係なく、常にこの32KBしかRAM/Cヒープを消費しない。

---

## I/O ポートマップ

| ポート | 方向 | 機能 |
| :--- | :--- | :--- |
| 0x98 | R/W | VDP データポート（VRAM読み書き） |
| 0x99 | R/W | VDP ステータス/アドレス・レジスタポート |
| 0xA0 | W | PSG レジスタ選択（下位4bitがレジスタ番号） |
| 0xA1 | W | PSG データ書き込み |
| 0xA2 | R | PSG データ読み取り（選択中レジスタの値） |
| 0xA8 | R/W | PPI ポートA — スロット選択（`slot_select`、ページごと2bit） |
| 0xA9 | R | PPI ポートB — キーボード行列読み取り（`key_row` で選択中の行、アクティブLow） |
| 0xAA | R/W | PPI ポートC — bit[3:0]がキーボード行選択（`key_row`）、書き込み値全体は `ppi_c` にミラー |
| 0xAB | W | PPI コントロールワード（モード設定）。受理はするが無視 |

### PSG レジスタ 14/15 経由のジョイスティック読み取り

実機のMSXと同様、ジョイスティック入力はPSGのI/Oポート（レジスタ14=ポートA、レジスタ15=ポートB）経由でエミュレートしている。

- レジスタ15への書き込みのbit6が JOY1(0)/JOY2(1) の選択を切り替える（`msx->joy_select`）。
- レジスタ14（ポート0xA2からの読み取り、`psg_reg==14` の場合）は選択中ポートの `msx->joy_state[port]` を返す。ビット割り当て: bit0=Up, bit1=Down, bit2=Left, bit3=Right, bit4=TriggerA, bit5=TriggerB（すべてアクティブLow、0=押下）。
- Python側からは `msx.set_joystick(port, state)` で毎フレーム更新する。

---

## `msx_state_t` 構造体（`src/msx/msx_core.h`）の主要フィールド

```c
typedef struct {
    z80 cpu;                    // Z80 CPUコア (superzazu/z80)
    VrEmuTms9918 *vdp;           // VDPコア（ヒープ確保、内部にVRAM 16KBを保持）
    PSG *psg;                    // PSGコア（ヒープ確保）

    uint8_t bios[0x8000];        // スロット0 (32KB)
    uint8_t ram[0x10000];        // スロット3 (64KB)
    uint8_t *cart[2];            // スロット1/2（フルRAMロード時、ヒープ確保）
    uint32_t cart_size[2];
    uint8_t cart_type[2];        // MSX_MAPPER_*
    uint8_t cart_bank[2][4];     // 各ウィンドウの選択中バンク番号

    bool cart_paged[2];          // true=メガロム(ページング)モード
    uint8_t *cart_cache[2];      // 4×8KB、ページング時のみヒープ確保
    int32_t cart_cache_page[2][4]; // 各ウィンドウの現在キャッシュ済みページ番号

    uint8_t slot_select;         // PPIポートA
    uint8_t key_matrix[11];      // キーボード行列（11行、アクティブLow）
    uint8_t key_row;             // 現在選択中の行（PPIポートC下位4bit）
    uint8_t psg_reg;             // PSG選択中レジスタ

    uint16_t framebuf[2][256*192]; // 表示用フレームバッファ（ダブルバッファ）
    uint8_t framebuf_ready_idx;

    int16_t audio_buf[512];      // 1フレーム分のPSGオーディオサンプル

    uint16_t lcd_w, lcd_h;       // 実行時に選択されたパネルサイズ
    bool rotate_180;             // 画面回転設定

    uint8_t joy_state[2];        // ジョイスティック状態（JOY1/JOY2）
    uint8_t joy_select;
} msx_state_t;
```

---

## セーブステート・ヘッダ（64バイト固定）

`msx_save_t`（`msx_core.h`）は CPUレジスタ・VDPレジスタ・マッパーバンク・I/O状態をまとめた固定64バイトのヘッダ構造体。RAM（64KB）・VRAM（16KB）はヘッダとは別にゼロコピーで読み書きされる（`msx.get_ram_view()` / `msx.get_vram_view()`）。

| オフセット | サイズ | フィールド | 内容 |
| :--- | :--- | :--- | :--- |
| 0 | 8 | `magic` | `"MSX1SAV1"` |
| 8 | 2 | `pc` | プログラムカウンタ |
| 10 | 2 | `sp` | スタックポインタ |
| 12 | 2 | `ix` | インデックスレジスタIX |
| 14 | 2 | `iy` | インデックスレジスタIY |
| 16 | 1 | `a` | アキュムレータ |
| 17 | 1 | `f` | フラグ（packed: S Z Y H X P N C） |
| 18 | 1 | `b` | レジスタB |
| 19 | 1 | `c` | レジスタC |
| 20 | 1 | `d` | レジスタD |
| 21 | 1 | `e` | レジスタE |
| 22 | 1 | `h` | レジスタH |
| 23 | 1 | `l` | レジスタL |
| 24 | 1 | `a_` | 裏レジスタA' |
| 25 | 1 | `f_` | 裏レジスタF' |
| 26 | 1 | `b_` | 裏レジスタB' |
| 27 | 1 | `c_` | 裏レジスタC' |
| 28 | 1 | `d_` | 裏レジスタD' |
| 29 | 1 | `e_` | 裏レジスタE' |
| 30 | 1 | `h_` | 裏レジスタH' |
| 31 | 1 | `l_` | 裏レジスタL' |
| 32 | 1 | `i` | インタラプトレジスタI |
| 33 | 1 | `r` | リフレッシュレジスタR |
| 34 | 1 | `iff1` | 割り込みフリップフロップ1 |
| 35 | 1 | `iff2` | 割り込みフリップフロップ2 |
| 36 | 1 | `int_mode` | 割り込みモード（0/1/2） |
| 37 | 1 | `halted` | HALT状態フラグ |
| 38 | 1 | `slot_select` | PPIポートA（スロット選択） |
| 39 | 1 | `psg_reg` | PSG選択中レジスタ |
| 40 | 1 | `key_row` | キーボード選択中行 |
| 41 | 1 | `ppi_c` | PPIポートCミラー |
| 42 | 8 | `vdp_regs[8]` | VDPレジスタ R0-R7 |
| 50 | 8 | `cart_bank[2][4]` | 各スロット×各ウィンドウのバンク番号 |
| 58 | 6 | `_pad` | 予約（80バイトへのパディング目的、未使用） |

セーブファイル形式（`msx_menu.py` の `save_state_to()`）: ヘッダ64バイト + RAM 64KB + VRAM 16KB を連結した約80.06KBのバイナリ。ヘッダとRAM/VRAMは別々の `f.write()` 呼び出しで書き込まれ、一度も大きな一時バッファを確保しない。

---

## Python側の関連定数

`mp/msx_keymap.py`:

- MSXキーボード行インデックス（`MSX_KEY_ROW_*`、`msx_core.h` の定義と対応）
- USB HIDキーコード → (行, ビットマスク) の変換テーブル

`mp/main.py`:

- GPIOピン割り当て定数（`hardware_guide.md` 参照）
- `LCD_SIZES` — パネル種別ごとの解像度

---

## 関連ソースファイル

- `src/msx/msx_core.h` — `msx_state_t` / `msx_save_t` の完全な定義
- `src/msx/msx_core.c` — `msx_mem_read()` / `msx_mem_write()`（アドレスデコード）、`msx_port_read()` / `msx_port_write()`（I/Oポート）、`cart_page_ptr()` / `cart_mapper_write()`（バンク切替）
- `src/msx/modmsx.c` — セーブステートのPython側バインディング（`get_state_header` / `set_state_header` / `get_ram_view` / `get_vram_view`）
