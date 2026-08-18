# 切り分け専用: ハードウェアSPI(machine.SPI)を使わず、GPIOを手動でゆっくり
# ビットバンギングして送信するテストスクリプト。本体MSX側Pico2のREPLから手動で
# importして実行する(本番コードには組み込まない)。
#
# 目的: ハードウェアSPIマスターでは64バイトの受信すら数秒に1回しか成功しない
# (ボーレートを1MHz/20MHzのどちらにしても同じペース)という現象が起きている。
# 手動ビットバンギングなら1ビットずつ数百us〜数msの余裕を持って送れるため、
# これで確実に受信できるなら「ハードウェアSPIペリフェラルの信号特性(エッジの
# 質・タイミング)に問題がある」、これでも受信できないなら「もっと根本的な
# 問題(配線・受信側ロジック等)がある」と切り分けられる。
#
# 使い方 (実機REPLで):
#   >>> import bitbang_sender
#   >>> bitbang_sender.run()

import machine
import time

CS_PIN = 28
SCK_PIN = 10
MOSI_PIN = 11


def run(bit_delay_us=500, n_bytes=64, repeat=1000):
    cs = machine.Pin(CS_PIN, machine.Pin.OUT, value=1)
    sck = machine.Pin(SCK_PIN, machine.Pin.OUT, value=0)
    mosi = machine.Pin(MOSI_PIN, machine.Pin.OUT, value=0)

    # テストパターン: 0x00,0x01,...,n_bytes-1 を繰り返す(受信側で内容確認しやすいように)
    data = bytes([i & 0xFF for i in range(n_bytes)])

    print("bitbang: sending %d bytes x %d回, bit_delay_us=%d (CS=GP%d SCK=GP%d MOSI=GP%d)"
          % (n_bytes, repeat, bit_delay_us, CS_PIN, SCK_PIN, MOSI_PIN))

    for i in range(repeat):
        cs.value(0)
        time.sleep_us(bit_delay_us)
        for byte in data:
            for bit in range(7, -1, -1):  # MSB first
                mosi.value((byte >> bit) & 1)
                time.sleep_us(bit_delay_us)
                sck.value(1)
                time.sleep_us(bit_delay_us)
                sck.value(0)
                time.sleep_us(bit_delay_us)
        time.sleep_us(bit_delay_us)
        cs.value(1)
        time.sleep_ms(200)
        if (i + 1) % 5 == 0:
            print("sent", i + 1, "frames")

    print("done")


def run_cs_held(bit_delay_us=500, total_bytes=2000):
    # 切り分け専用その2: CS(GP28)を最初に一度Lowにしたまま最後まで一切離さず、
    # バイトを延々と送り続ける。run()はバイト列ごとにCSを毎回High/Lowし直すが、
    # こちらはCSの再アサートを一切行わない。これで受信側のマーカーが動くように
    # なるなら、「CSの再アサート(フレーム区切り)がSPI0スレーブ側の同期を崩して
    # いる」ことが濃厚と分かる。
    cs = machine.Pin(CS_PIN, machine.Pin.OUT, value=1)
    sck = machine.Pin(SCK_PIN, machine.Pin.OUT, value=0)
    mosi = machine.Pin(MOSI_PIN, machine.Pin.OUT, value=0)

    print("bitbang(CS保持): CS=GP%d SCK=GP%d MOSI=GP%d, bit_delay_us=%d, total_bytes=%d"
          % (CS_PIN, SCK_PIN, MOSI_PIN, bit_delay_us, total_bytes))

    cs.value(0)
    time.sleep_ms(5)
    for i in range(total_bytes):
        byte = i & 0xFF
        for bit in range(7, -1, -1):
            mosi.value((byte >> bit) & 1)
            time.sleep_us(bit_delay_us)
            sck.value(1)
            time.sleep_us(bit_delay_us)
            sck.value(0)
            time.sleep_us(bit_delay_us)
        if (i + 1) % 50 == 0:
            print("sent", i + 1, "bytes (CS held low the whole time)")
    cs.value(1)
    print("done (CS released at the very end)")
