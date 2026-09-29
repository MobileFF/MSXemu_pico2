"""
DHT20 温湿度センサー MSX エミュレータ用 拡張モジュール

接続: Pico 2 I2C1  GP2=SDA  GP3=SCL
回路: VDD=3.3V, GND, SDA/SCL に 4.7kΩ プルアップ抵抗

動作仕様:
- エミュレータ起動時に msx_ext.py により自動ロードされ、register(msx) が実行されます。
- SDカードの /sd/msx/ext/dht20.py または内蔵フラッシュの /ext/dht20.py に配置します。
- register(msx) 実行時に MSX RAM の 0xD000 へ Z80 スタブ [CALL 0xD000, RET] (CD 00 D0 C9)
  を自動配置し、0xD000 に CALL/RST フックを登録します。
- BASIC から DEFUSR=&HD000: A=USR(0) (内部 JP 0xD000) で呼び出すと、0xD000 の CALL 命令を
  エミュレータが横取り発火して DHT20 から温湿度を取得し、直後の RET 命令で BASIC に正常復帰します。

MSX BASIC 使用例:
    10 DEFUSR=&HD000: A=USR(0)
    20 IF PEEK(&HD100)<>0 THEN PRINT "Error": END
    30 T=PEEK(&HD101)*256+PEEK(&HD102)
    40 IF T>32767 THEN T=T-65536
    50 H=PEEK(&HD103)*256+PEEK(&HD104)
    60 PRINT "Temp:";T/10;"C  Hum:";H/10;"%"

ワークエリアレイアウト:
    [0xD000〜0xD003]: Z80 スタブ命令 (CD 00 D0 C9)
    [0xD100]: ステータスコード  0x00=OK / 0xFF=Error
    [0xD101]: 温度×10 上位バイト (signed int16 big-endian)
    [0xD102]: 温度×10 下位バイト  例: 0x00EB=235 → 23.5°C
    [0xD103]: 湿度×10 上位バイト (uint16 big-endian)
    [0xD104]: 湿度×10 下位バイト  例: 0x025D=605 → 60.5%
"""

import time

CALL_ADDR    = 0xD000
STATUS_ADDR  = 0xD100
DATA_ADDR    = 0xD101

EXT_OK          = 0x00
EXT_ERR_GENERAL = 0xFF

_I2C_ADDR  = 0x38
_MEAS_CMD  = bytes([0xAC, 0x33, 0x00])
_MEAS_WAIT = 80   # ms

_i2c = None  # 遅延初期化
_msx = None


def _setup_stub():
    """MSX RAM の CALL_ADDR (0xD000) に Z80 スタブ [CALL 0xD000, RET] (CD 00 D0 C9) を書き込む。"""
    if _msx is None:
        return
    ram = _msx.get_ram_view()
    low = CALL_ADDR & 0xFF
    high = (CALL_ADDR >> 8) & 0xFF
    ram[CALL_ADDR + 0] = 0xCD  # CALL nn
    ram[CALL_ADDR + 1] = low
    ram[CALL_ADDR + 2] = high
    ram[CALL_ADDR + 3] = 0xC9  # RET


def register(msx):
    """msx_ext.py の load_extensions() から自動的に呼ばれます。"""
    global _msx
    _msx = msx
    #_setup_stub()
    msx.set_call_hook(CALL_ADDR, _read)
    print(f"dht20: hook {CALL_ADDR:#06x} registered with Z80 stub (I2C1 GP2=SDA GP3=SCL)")


def _ensure_i2c():
    """I2C1 を初期化してセンサー応答を確認する。失敗したら例外を送出。"""
    global _i2c
    import machine
    i2c = machine.I2C(1, sda=machine.Pin(2), scl=machine.Pin(3), freq=100_000)
    i2c.readfrom(_I2C_ADDR, 1)  # センサー不在なら OSError
    _i2c = i2c
    print("dht20: I2C1 initialized (GP2=SDA, GP3=SCL)")


def _read():
    """CALL 0xD000 ハンドラ: 必要なら初期化してから DHT20 を読み取り、MSX RAM へ格納。"""
    global _i2c, _msx
    if _msx is None:
        return

    # スタブが消去・上書きされた場合に備えて再セットアップ
    _setup_stub()

    ram = _msx.get_ram_view()

    try:
        if _i2c is None:
            _ensure_i2c()

        _i2c.writeto(_I2C_ADDR, _MEAS_CMD)
        time.sleep_ms(_MEAS_WAIT)
        buf = _i2c.readfrom(_I2C_ADDR, 6)

        if buf[0] & 0x80:   # bit7=1: 測定未完了
            print("dht20: measurement not ready")
            ram[STATUS_ADDR] = EXT_ERR_GENERAL
            return

        # 湿度 (20-bit raw → ×10 整数, 0–1000)
        raw_hum  = (buf[1] << 12) | (buf[2] << 4) | (buf[3] >> 4)
        hum_x10  = raw_hum * 1000 // (1 << 20)

        # 温度 (20-bit raw → ×10 整数, -50°C オフセット込み, -500–1550)
        raw_temp = ((buf[3] & 0x0F) << 16) | (buf[4] << 8) | buf[5]
        temp_x10 = raw_temp * 2000 // (1 << 20) - 500

        ram[STATUS_ADDR]     = EXT_OK
        ram[DATA_ADDR + 0]   = (temp_x10 >> 8) & 0xFF
        ram[DATA_ADDR + 1]   =  temp_x10       & 0xFF
        ram[DATA_ADDR + 2]   = (hum_x10  >> 8) & 0xFF
        ram[DATA_ADDR + 3]   =  hum_x10        & 0xFF

        print(f"dht20: temp={temp_x10/10:.1f}C hum={hum_x10/10:.1f}%")

    except Exception as e:
        _i2c = None  # 次回呼び出し時に再初期化を試みる
        print(f"dht20: error: {e}")
        ram[STATUS_ADDR] = EXT_ERR_GENERAL


def main():
    """単体テスト用スタンドアロン実行"""
    import machine
    i2c = machine.I2C(1, sda=machine.Pin(2), scl=machine.Pin(3), freq=100_000)
    while True:
        try:
            i2c.writeto(_I2C_ADDR, _MEAS_CMD)
            time.sleep_ms(_MEAS_WAIT)
            buf = i2c.readfrom(_I2C_ADDR, 6)

            if buf[0] & 0x80:
                print("測定未完了")
                continue

            raw_hum  = (buf[1] << 12) | (buf[2] << 4) | (buf[3] >> 4)
            hum_x10  = raw_hum * 1000 // (1 << 20)

            raw_temp = ((buf[3] & 0x0F) << 16) | (buf[4] << 8) | buf[5]
            temp_x10 = raw_temp * 2000 // (1 << 20) - 500

            print(f"temp: {temp_x10/10:.1f} °C, humi: {hum_x10/10:.1f} %")
        except Exception as e:
            print(f"Error: {e}")
        time.sleep(2)


if __name__ == '__main__':
    main()