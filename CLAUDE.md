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


## オーディオ経路（MB1939 回路図より）

### 出力
- コーデック: **WM8904**（U34）
- MCU接続: **SAI1**
  - `SAI1_MCLK_A` / `SAI1_CLK_A` / `SAI1_FS_A` / `SAI1_SD_A` / `SAI1_SD_B`
  - SD_A と SD_B が両方あるため、送受信を同時に張れる
- コーデック制御: **I2C2**（`I2C2_SCL` / `I2C2_SDA`）
- 割り込み: `Audio_INT`
- 出力端子: `HPOUTL` / `HPOUTR` → ステレオジャック **CN15**（PJ-3028B-4P）
- 他に `LINEOUTL` / `LINEOUTR` あり
- アナログ入力: `IN1L/DMICDAT1`, `IN1R/DMICDAT2`, `IN2L`, `IN2R`

### 入力（MEMSマイク）
- PDMマイク 2個（U13 / U14）
- 信号: `MIC_CK` / `MIC_D1` / `MIC_D2`（`CLK` / `DOUT` / `LR`）
- `MIC_DET` で外部マイクボードの装着を検出し、装着時はオンボードをバイパス

### 方針
- 外部ハードは使わない（郵送回避のため、オンボード部品のみで完結させる）
- まず出力（SAI1 → WM8904 → ジャック）を確立してから入力に進む
- 入力経路は暫定で WM8904 経由。余力があれば PDM → MDF に切り替える



## 部門
RTOSアプリケーション部門・学生部門。カーネル改変は部門違いになる

## このファイルについて
開発中の制約メモ。人間向けの説明は README.md を参照。