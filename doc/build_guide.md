# ビルド・ガイド

## 事前準備

### 1. ツールチェーンと依存関係

#### Linux (Ubuntu/Debian) / WSL2 / ChromeOS Crostini

```bash
sudo apt update
sudo apt install -y cmake python3 python3-pip git build-essential gcc-arm-none-eabi libnewlib-arm-none-eabi
```

#### macOS

```bash
brew install cmake python3 git
brew install --cask gcc-arm-embedded
```

### 2. MicroPython のクローン

```bash
mkdir -p ~/projects && cd ~/projects
git clone https://github.com/micropython/micropython.git
cd micropython
git submodule update --init
```

### 3. mpy-cross のビルド（初回のみ）

```bash
cd ~/projects/micropython
make -C mpy-cross
```

### 4. Pico SDK の準備

MicroPython の `ports/rp2` は Pico SDK を submodule として取り込んでいるため、上記 `git submodule update --init` で完了しているはずです。個別に取得する場合は [pico-sdk](https://github.com/raspberrypi/pico-sdk) を参照してください。

---

## ビルド手順

`bldfrm_msx.sh` がビルド全体を自動化します。プロジェクトルート（`MSX_emu_pico2/`）で実行してください。3つのビルドターゲットがあります:

```bash
./bldfrm_msx.sh                # Pico 2、ネイティブUSBホスト（デフォルト）
./bldfrm_msx.sh pico2          # 同上、明示指定
./bldfrm_msx.sh pico2_piousb   # Pico 2、PIO-USBホスト(GP4/GP5) + ネイティブUSB CDC REPL
                                # — GP4(D+)/GP5(D-)に22Ω直列抵抗経由で第2のUSBコネクタを
                                #   実際に配線済みの機体専用。配線していない機体には書き込まないこと
                                #   （詳細はハードウェア・ガイドを参照）
./bldfrm_msx.sh pizero         # Waveshare RP2350-PiZero
```

初回は以下も必要です:

```bash
chmod +x bldfrm_msx.sh
```

### スクリプトが行うこと

1. `src/` フォルダ（Google Drive上のソース、正本）を `~/projects/msx_emu/src` へコピー（ビルドI/O速度のためローカルディスクを使用）
2. 前回のビルドディレクトリ（`~/projects/micropython/ports/rp2/build-RPI_PICO2`、pizeroの場合は`build-WAVESHARE_RP2350_PIZERO`）を削除し、cmakeを強制的に再構成
3. 以下のオプションで `make` を実行（`pico2`のデフォルト値を例示）:

```bash
make -C ~/projects/micropython/ports/rp2 \
     BOARD=RPI_PICO2 \
     USER_C_MODULES=~/projects/msx_emu/src/msx/micropython_msx.cmake \
     WERROR=0 \
     MICROPY_C_HEAP_SIZE=81920 \
     -j$(nproc)
```

`pico2_piousb`では、これに加えて`CMAKE_ARGS="-DUSE_PIO_USB=ON"`が渡され（`src/msx/micropython_msx.cmake`の`option(USE_PIO_USB ...)`を有効化）、`MICROPY_C_HEAP_SIZE`も`65536`に変更されます（後述）。

4. ビルド成功時、`firmware.uf2` を自動的にプロジェクトの `firmware/<対象別のファイル名>.uf2` にコピー（実機書き込みは常にこのパスから行う運用ルール）:
   - `pico2` → `firmware/firmware_msx.uf2`
   - `pico2_piousb` → `firmware/firmware_msx_piousb.uf2`
   - `pizero` → `firmware/firmware_msx_pizero.uf2`

### `MICROPY_C_HEAP_SIZE` について

MicroPython RP2ポートは、C言語の `malloc()`/`calloc()`（VDPコア・PSGコア・メガロムのページキャッシュがこれを使用）向けの専用ヒープサイズが**デフォルトで0バイト**です。これを明示的に設定しないと、これらの `malloc()` がMicroPythonのGCヒープ領域に静かに食い込んで破壊し、原因不明のハングを引き起こします（過去に実際に踏んだバグ）。

- `pico2`/`pizero`: **81920バイト(80KB)**。VDPコア（約16.3KB）＋メガロムページキャッシュ（32KB、`MSX_CART_VICTIM_SLOTS=0`）＋余裕を賄うサイズ。詳細は[Memory Usage](memory_usage.md)を参照。
- `pico2_piousb`: **65536バイト(64KB)**。PIO-USBライブラリ＋ネイティブUSB CDCスタックの追加分（約15.8KBの`.bss`増）でGCヒープが窮迫するため、Cヒープ側を削って補っている。これは victim プール導入前にこのプロジェクトが実際に使っていた値そのもので、新規の推測値ではない。実機で`msx_menu.py`コンパイル時の`MemoryError`を実際に踏んで調整した値なので、安易に変更しないこと（詳細は`bldfrm_msx.sh`自身のコメント参照）。

### `micropython_msx.cmake` のビルド最適化

- 全体のビルドタイプはデフォルト `MinSizeRel`（`-Os`、フラッシュ容量重視）だが、`msx_core.c` / `z80.c` / `vrEmuTms9918.c` / `emu2149.c` の4ファイル（エミュレーションのホットパス）のみ `-O3`（速度重視）でコンパイルするよう `set_source_files_properties()` で個別指定。
- `vrEmuTms9918.c` の `PICO_BUILD` マクロを明示的に定義し、同ライブラリのRAM常駐実行最適化（`__time_critical_func`）を有効化（デフォルトでは未定義のため無効化されたままだった）。

---

## 書き込み (フラッシュ)

1. Pico 2 の **BOOTSEL** ボタンを押しながら USB ケーブルを接続する
2. PC上に `RP2350` （または `RPI-RP2`）という名前のドライブがマウントされる
3. `firmware/firmware_msx.uf2` をそのドライブにコピーする
4. 自動的に再起動し、新しいファームウェアで起動する

---

## SD カードのセットアップ

```
/sd/msx/
  MSX_jp.rom       # 32KB 日本語版MSX BIOS/BASIC ROM（または MSX_en.ROM 等、任意の名前）
  <カートリッジ名>.ROM  # カートリッジROM（任意、複数可）
  config.txt       # 設定ファイル（任意）
  save.bin         # セーブステート（自動生成）
```

BIOS ROM は著作権の都合上このリポジトリには含まれていません。各自で用意し、`config.txt` の `bios=` で指定するか、デフォルトパス `/sd/msx/MSX.ROM` に配置してください。

`config.txt` の詳細は `usage_guide.md` を参照。

---

## デバイスへの Python ファイル転送

`mpremote` を使って `mp/` 以下の Python ファイルを Pico のファイルシステム（ルート `/`）に転送します。

```bash
mpremote connect /dev/ttyUSB0 fs cp mp/main.py :main.py
mpremote connect /dev/ttyUSB0 fs cp mp/msx_menu.py :msx_menu.py
mpremote connect /dev/ttyUSB0 fs cp mp/msx_keymap.py :msx_keymap.py
```

> シリアルポート名（`/dev/ttyUSB0` 等）は環境によって変わります。`ls /dev/ttyUSB*` で確認してください。USB-UART変換アダプタの抜き差しで番号が変わることがあります。

転送後、バイト数が一致しているか確認することを推奨します（`mpremote fs cp` は稀に転送が途中で終わることがあります）:

```bash
mpremote connect /dev/ttyUSB0 exec "import os; print(os.stat('main.py')[6])"
```

ローカルの `stat -c%s mp/main.py`（Linux）の値と比較してください。

---

## 実行

```bash
mpremote connect /dev/ttyUSB0
>>> import main
>>> main.run()
```

上記の転送コマンドは既に `main.py` という名前で配置しているため、これで電源投入時に自動起動します。REPLでの `import main; main.run()` は、フル電源再投入なしに手早く再実行したい開発時のショートカットです。

---

## トラブルシューティング

| 症状 | 原因・対処 |
| :--- | :--- |
| ビルドが `USER_C_MODULES` 関連のエラーで失敗する | `src/msx/micropython_msx.cmake` のパスが正しいか確認。`bldfrm_msx.sh` はソースを毎回ローカルにコピーするため、編集はGoogle Drive上の `src/` に対して行うこと |
| `MemoryError` が起動直後から頻発する | `MICROPY_C_HEAP_SIZE` が設定されているか確認（デフォルト0はGCヒープ破壊の原因になる） |
| 実機でハングする、REPLも無応答 | 物理的な電源再投入（USBケーブル抜き差し）で復旧を試す。`flash_nuke.uf2` はファイルシステムごと全消去する最終手段（セーブデータ等も消えるため注意） |
| `mpremote` が `could not enter raw repl` で失敗する | すでに `main.run()` のような無限ループが実行中の可能性。`machine.reset()` を送るか、物理リセットする |
| キーボードが反応しない | `usb_host.init()` の後に `usb_host.start_bg_timer(8)` が呼ばれているか確認（TinyUSBホストスタックの定期処理に必須） |
| SPI/SDカードが不安定 | LCDとSDが同じSPI1バスを共有している。配線不良の可能性が高いので `hardware_guide.md` を参照して確認 |

---

## 関連ソースファイル

- `bldfrm_msx.sh` — ビルドスクリプト本体
- `src/msx/micropython_msx.cmake` — CMakeモジュール定義（ソースファイル列挙・最適化フラグ・USBホストコアのリンク）
