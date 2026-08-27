# TRONプログラミングコンテスト2026

## 触っていい場所
- `mtk3bsp2_stm32n657/Appli/Application/` のみ

## 触ってはいけない場所
- `mtk3bsp2_stm32n657/Appli/mtk3_bsp2/` — μT-Kernel 3.0 BSP2 本体。無改変が原則
- `mtk3bsp2_stm32n657/Drivers/`, `Middlewares/`, `FSBL/`, `Secure_nsclib/`

## コマンド
- ビルド: `bash scripts/build.sh`
- 書き込み: CubeIDE の Debug 実行（FSBL構成、Startup に Appli 追加済み）
- ログ: `powershell scripts/log.ps1` → `logs/uart.log`

## ハード
- STM32N6570-DK / Cortex-M55 + Neural-ART NPU
- 内蔵ユーザFlashなし。外部フラッシュ + 署名必須
- SW1(BOOT1) は 1-3 側（Development boot）
- シリアル: COM3 / 115200 / 8N1 / フロー制御なし

## 実装規約
- タスク間通信は `tk_cre_mbf`（メッセージバッファ）。共有変数を使わない
- 排他は `tk_cre_mtx` に `TA_INHERIT`（優先度継承）
- 時間計測は `tk_get_otm`(ms) ではなく DWT CYCCNT
- ライセンスヘッダ（T-License 2.2）を消さない

## 部門
RTOSアプリケーション部門・学生部門。カーネル改変は部門違いになる

## このファイルについて
開発中の制約メモ。人間向けの説明は README.md を参照。