"""
MSX エミュレータ用 サブルーチンフック 拡張モジュール テンプレート (sample.py)

動作仕様:
- エミュレータ起動時に msx_ext.py により自動ロードされ、register(msx) が実行されます。
- SDカードの /sd/msx/ext/sample.py または内蔵フラッシュの /ext/sample.py に配置します。
- register(msx) 実行時に CALL_ADDR (0xD001) へフックを直接登録します。
- MSX BASIC から DEFUSR=&HD001: A=USR(0) で呼び出すと、CPUが0xD001へ到達した瞬間
  （CALL命令経由か、JP/間接ジャンプ経由かを問わず）にエミュレータの汎用フェッチ時
  トラップがその場で横取りし、Python の _read() 関数が実行されたうえで、USR()の
  呼び出し元へ即座に「実行して戻った」体で制御が返ります（詳細は
  doc/extension_api.md の「CALL/RSTフック」参照）。0xD001のRAM内容そのものは
  一切参照されないため、事前にスタブ命令を書き込んでおく必要はありません。

MSX BASIC 使用例:
    10 DEFUSR=&HD001: A=USR(0)
    20 PRINT "Sample hook executed!"
"""

CALL_ADDR = 0xD001

_msx = None


def register(msx):
    """msx_ext.py の load_extensions() から自動的に呼ばれます。"""
    global _msx
    _msx = msx
    msx.set_call_hook(CALL_ADDR, _read)
    print(f"sample: hook {CALL_ADDR:#06x} registered")


def _read():
    """CALL_ADDR フックハンドラ。戻り値を省略（None）することで横取り＝「呼び出し元
    へ即座に復帰」を意味する。BASICの元のコードを先に走らせてから前処理として割り込み
    たいだけなら、代わりに `return False` するとフックは横取りせず素通しになる
    （doc/extension_api.md 参照）。"""
    global _msx
    if _msx is None:
        return

    print(f"sample: hook {CALL_ADDR:#06x} called successfully!")
    print("-> return False")
    return False

def main():
    pass


if __name__ == '__main__':
    main()
