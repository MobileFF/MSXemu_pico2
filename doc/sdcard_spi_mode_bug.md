# SDカードSPIモード(CPOL/CPHA)未リセットバグ

2026-08-18に発見・修正した、`mp/sdcard.py`のSPI通信モード管理バグの記録。

## 背景: SPIバスの共有と要求モードの違い

このボードでは、LCD・SDカード・(オプションの)HDMIブリッジ出力が**同じSPI1バス**(GP10=SCK, GP11=MOSI)を共有している。ただし要求するSPIモードが異なる。

| デバイス | 要求モード |
| :--- | :--- |
| LCD(ST7796/ILI9341) | モード0(CPOL=0, CPHA=0) |
| SDカード | モード0(CPOL=0, CPHA=0) |
| HDMIブリッジ | **モード3**(CPOL=1, CPHA=1) — `src/msx/msx_core.c`の`hdmi_apply_spi_settings()`が`spi_set_format()`を直接呼んで設定 |

## バグの本体

`mp/sdcard.py`の`readblocks()`/`writeblocks()`は、SDカードへアクセスする直前に毎回

```python
self.spi.init(baudrate=self.baudrate)
```

を呼んでいた。ボーレートだけ再設定すれば、SPIの通信モードも一緒にリセットされるはず、という前提で書かれていた。

しかし実際にビルドに使っているMicroPython本体のソース(`~/projects/micropython/ports/rp2/machine_spi.c`)を確認したところ、`machine.SPI.init()`の実装は以下のようになっていた。

```c
// machine_spi_init(), 233-259行目付近
if (args[ARG_baudrate].u_int != -1) {
    self->baudrate = spi_set_baudrate(...);   // ボーレートは常に反映される
}

bool set_format = false;
if (args[ARG_polarity].u_int != -1) { self->polarity = ...; set_format = true; }
if (args[ARG_phase].u_int != -1)    { self->phase    = ...; set_format = true; }
if (args[ARG_bits].u_int != -1)     { self->bits     = ...; set_format = true; }
if (args[ARG_firstbit].u_int != -1) { self->firstbit = ...; set_format = true; }
if (set_format) {
    spi_set_format(self->spi_inst, self->bits, self->polarity, self->phase, self->firstbit);
    // ↑ CPOL/CPHAをハードウェアレジスタへ書き込む関数。polarity/phase/bits/firstbit
    //   のいずれかを明示的に渡した時だけ呼ばれる。
}
```

つまり**`baudrate=`だけを渡した`.init()`呼び出しでは、`spi_set_format()`(CPOL/CPHAをハードウェアに書き込む関数)は一切実行されない**。`mp/sdcard.py`は`polarity`/`phase`を一度も明示的に渡していなかったため、SDカードアクセス前の「初期化」は実質ボーレートの再設定にしかなっておらず、**SPIモードは直前に他の処理が設定した状態のまま**残っていた。

なお、この関数の`spi_init(self->spi_inst, self->baudrate)`(ハードウェアの完全リセットを伴う)は`machine.SPI()`の**コンストラクタでのみ**呼ばれ、`.init()`メソッド自体からは呼ばれない。「`sdcard.py`の`readblocks()`は`machine.SPI.init()`経由で`spi_init()`相当のフルリセットをしている」という、修正前にコード中に残っていたコメントの前提は誤りだった。

## 実際に起きていたこと

1. `mp/main.py`の起動シーケンスで`msx.init_hdmi_output()`/`msx.clear_hdmi()`が実行される → SPI1がモード3になる。
2. 続けてBIOSやカートリッジをSDカードから読み込もうとする → `mp/sdcard.py`が`self.spi.init(baudrate=X)`を呼ぶが、モードはモード3のまま変わらない。
3. SDカードはモード3の通信を正しく解釈できず、コマンド応答が返ってこない。
4. 症状として2パターン現れた。
   - **はっきり失敗する場合**: `OSError: timeout waiting for response`(`readblocks()`内)。
   - **中途半端に通る場合**: バイト数は正しく読めた「ことになる」が、データの中身が壊れている。BIOSファイルが32768バイト読み込めたとログに出るのにMSXが正しく起動しない、という症状の原因もこれだった。

LCD側(`msx_render_to_display_1to1()`等、`src/msx/msx_core.c`)は**毎フレーム自分で`spi_set_format(...モード0...)`を明示的に再適用**する設計だったため影響を受けなかった。この非対称性(LCDは自衛していたがSDは自衛していなかった)が、「HDMI初期化をBIOS/カートロードより前に行うとSDだけ壊れる」という、一見すると初期化の順序に依存する不可解な現象として観測されていた。

## 修正

`mp/sdcard.py`の全6箇所の`self.spi.init(...)`呼び出しに`polarity=0, phase=0`を明示的に追加した。

```python
self.spi.init(baudrate=self.baudrate, polarity=0, phase=0)
```

これにより、SDカードアクセスのたびに**直前の状態に関係なく必ずモード0へ強制的に戻る**ようになった。結果として、HDMI初期化をBIOS/カートロードより前に行っても(=起動時のROM選択メニューをHDMIにも表示させても)安全になり、`mp/main.py`のHDMI初期化位置を元に戻すことができた。

## 教訓

- 「順序を変えると挙動が変わる」という現象に遭遇した場合、それは大抵「タイミングの偶然」ではなく、どこかに実在する具体的なバグである。
- 複数のデバイスが同一のハードウェアバス(SPI等)を共有し、それぞれ異なる通信モードを要求する設計では、**各デバイスのドライバが自分の使用直前に必ず自分の要求モードを明示的に再適用する**ことを徹底しないと、他デバイスの通信状態に引きずられる形の潜在バグを埋め込みやすい。
- ベンダー(この場合MicroPython本体)のソースがローカルで参照できる環境では、推測でワークアラウンドを重ねる前に、まず実装を直接確認して確定診断すること。
