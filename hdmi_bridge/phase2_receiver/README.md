# 受信側ファームウェアの現在地

このディレクトリにあった受信側(第2Pico 2)の現行ソース(`main.c`,
`CMakeLists.txt`, `pico_sdk_import.cmake`, `build.sh`,
`font_petme128_8x8.h`)は、プロトコルが完全に自己記述式になり本プロジェクト
専用ではなくなったため、PB-1000_emu_AG2と共有する独立プロジェクト
[`../../../hdmi_bridge_receiver/`](../../../hdmi_bridge_receiver/) へ
移動しました。最新のソース・ビルド方法・プロトコル仕様はそちらの
README.mdを参照してください。

このディレクトリに残っているのは、Phase 2の実機検証時に使った**診断用の
過去ビルド成果物**です(`doc/hdmi_bridge_phase2_report.md`参照)。現行の
ビルドとは無関係な履歴として保持しています:

- `hdmi_receiver_hstx_only_diag.uf2` / `build_hstx_only/` — HSTX単体動作確認用
- `hdmi_receiver_smallchunk_diag.uf2` / `build_smallchunk/` — SPI受信チャンクサイズ切り分け用
- `hdmi_receiver_failure_diag.uf2` — NO SIGNAL不具合の原因切り分け用
- `hdmi_receiver_verify.uf2` — 修正後の検証ビルド
