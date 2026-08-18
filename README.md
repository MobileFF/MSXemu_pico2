# MSX1 エミュレータ for Raspberry Pi Pico 2

Raspberry Pi Pico 2 (RP2350) で動作する MSX1 エミュレータです。

## 概要

Z80 CPU・TMS9918A VDP・AY-3-8910 PSGを搭載した MSX1 をエミュレートします。高速なC言語によるエミュレーションコアと、周辺機器処理のための柔軟な MicroPython ロジックを組み合わせ、実機のBASIC/カートリッジソフト実行から独自拡張の開発まで対応できる環境を提供します。

## 主な特徴

- **高性能エミュレーションコア**: Z80 CPU([superzazu/z80](https://github.com/superzazu/z80))・TMS9918A VDP([vrEmuTms9918](https://github.com/visrealm/vrEmuTms9918))・AY-3-8910 PSG([emu2149](https://github.com/digital-sound-antiques/emu2149))をいずれもC言語で実装し、パイプライン化された描画処理により実機相当のフレームレート(実測 約64FPS)を達成。
- **MicroPython フレームワーク**: 起動フロー・UI・設定ロジックを MicroPython で記述しており、カスタマイズが容易。
- **表示サポート**: ST7796 (480×320) / ILI9341 (320×240) のTFT LCDに対応、`config.txt`で切替可能。画面180度回転にも対応。
- **外部キーボード対応**: USB HIDキーボード(Hostモード)による入力をサポート。
- **カートリッジ対応**: PLAIN / ASCII-8 / ASCII-16 / KONAMIマッパーに対応(自動判定あり)。128KB以上のバンク切替カートリッジ(メガロム)は、Pico 2内蔵フラッシュへのキャッシュ経由でRAM容量を超えるサイズにも対応。
- **ストレージ**: SDカードからのBIOS/カートリッジROM読み込み、ゼロコピー設計によるステートセーブ/ロード(RAM・VRAM込み)をサポート。
- **実行時メニュー**: USBキーボードの GUI+F7 でオンスクリーンメニューを起動。カートリッジ交換、セーブ/ロード、リセット、音量/音質調整、HDMI設定をゲームを止めずに操作可能。
- **ジョイスティック対応**: ATARI/MSX 9ピンジョイスティック(JOY1)の方向キー・トリガーA/Bに対応。
- **オプションのHDMI出力**: 2台目のPico 2 + PICO-HDMI-PLUSを使い、映像をHDMI経由でも出力可能([hdmi_bridge/README.md](hdmi_bridge/README.md)参照)。

## 制限事項

- **MSX1のみに対応**しています。MSX2/2+/turboRのエミュレーションはできません。
- **カートリッジROMは同時に1スロットのみ**利用可能です。2スロット同時ロードには対応していません。
- **Raspberry Pi Pico(初代、RP2040)では動作しません**。性能・RAM容量の都合上、Raspberry Pi Pico 2 (RP2350) が必須です。
- **メガロム(バンク切替対応の大容量カートリッジ)をHDMI出力と併用すると、動作がやや遅くなることがあります**(内蔵フラッシュキャッシュ読み込みの影響)。実行時メニューの「HDMI Settings」で Skip Frame (`hdmi_frame_skip`) を調整してください。
- **ジョイスティックポート2 (JOY2) はサポートしていません**。JOY1のみ利用可能です。
- **LCDとHDMIの同時出力 (`display=both`) は推奨しません**。LCD側の表示が一部乱れる場合があります。片方のみの出力(`display=lcd` または `display=hdmi`)を推奨します。

## クイックスタート

1.  **ハードウェア**: Raspberry Pi Pico 2、ST7796またはILI9341のLCD、SDカードモジュールを用意します。詳細は [Hardware Guide](doc/hardware_guide.md) を参照してください。
2.  **ビルド**: `bldfrm_msx.sh` でカスタム MicroPython ファームウェアをコンパイルします。詳細は [Build Guide](doc/build_guide.md) を参照してください。
3.  **書き込み**: 生成された `firmware/firmware_msx.uf2` を BOOTSEL 経由で Pico 2 に書き込みます。
4.  **セットアップ**: `mp/` ディレクトリの Python ファイルを Pico 2 に転送し、MSX BIOS ROM と任意のカートリッジROMを SD カードの `/sd/msx/` に配置します。詳細は [Usage Guide](doc/usage_guide.md) を参照してください。
5.  **実行**: `main.py` として配置すれば、電源投入時に自動的にエミュレータが起動します。

## ドキュメント・インデックス

- [Build Guide](doc/build_guide.md) - ビルド環境の構築とファームウェアのコンパイル方法
- [Hardware Guide](doc/hardware_guide.md) - 配線図、部品表 (BOM)、ピンアサイン
- [Usage Guide](doc/usage_guide.md) - 初期設定、ROM の準備、操作マニュアル
- [Development Guide](doc/dev_guide.md) - コード構造、内部 API、デバッグのヒント
- [Architecture](doc/architecture.md) - エミュレータ内部設計の全体像
- [Extension API](doc/extension_api.md) - `msx` Cモジュールの公開API一覧
- [Extension Hooks Guide](doc/ext_hooks_guide.md) - 実行時メニュー等の拡張ポイント解説
- [Memory Map](doc/memory_map.md) - セーブステートのバイナリレイアウト
- [Memory Usage](doc/memory_usage.md) - ヒープメモリ使用量の内訳

*(英語版ドキュメントは `_en.md` サフィックスのファイルを参照してください)*

## プロジェクト構造

```text
MSX_emu_pico2/
├── src/msx/                # C言語ソース (Z80/VDP/PSGコア & MicroPythonラッパー)
├── mp/                      # MicroPython コード (起動フロー・メニュー・キーマップ)
├── doc/                     # ドキュメント & ガイド
├── hdmi_bridge/             # オプションのHDMI出力ブリッジ(送信側)関連資料
├── sample/                  # サンプルBASICプログラム等
└── README.md                # 本ファイル
```

## ライセンス

MITライセンス。詳細は [LICENSE](LICENSE) を参照してください。

同梱のサードパーティコンポーネント(`src/msx/z80/`・`src/msx/tms9918/`・`src/msx/emu2149/`)は、それぞれ自身のディレクトリ内のLICENSEファイルに記載された各ライセンス(いずれもMIT)に従います。

MSX BIOS ROM等の著作権保護されたファイルは本リポジトリに含まれていません。各自でご用意ください。

## 謝辞

- エミュレーションコアの実装には、以下の優れたオープンソースライブラリを利用させていただいています。ありがとうございます。
  - [superzazu/z80](https://github.com/superzazu/z80) — Z80 CPUコア (MIT License)
  - [visrealm/vrEmuTms9918](https://github.com/visrealm/vrEmuTms9918) — TMS9918A VDPコア (MIT License)
  - [digital-sound-antiques/emu2149](https://github.com/digital-sound-antiques/emu2149) — AY-3-8910 PSGコア (MIT License)
- 本プロジェクトは、同著者によるCASIO PB-1000エミュレータ(C+MicroPythonの二層構造、USBホストスタック等)の設計・実装基盤を踏襲しています。
- MSXに関する情報を発信されているすべての皆様に感謝いたします。
- ソースコードの作成には主に以下のAIエージェントツールを利用しています。
  - Claude Code
  - OpenAI Codex
  - Google Antigravity

以上
