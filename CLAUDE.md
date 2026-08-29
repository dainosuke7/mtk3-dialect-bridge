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


## ビルド設定の変更について
Debug/ 配下の makefile / sources.mk / subdir.mk / objects.list は
CubeIDE が生成する。手動編集しても Clean で消えるため編集しないこと。

ビルド対象の追加・除外は CubeIDE の GUI で行い、.cproject に
永続化させる(右クリック → Resource Configurations → Exclude from Build)。

## tm_printf の制約
tm_printf は knl_start_mtkernel() より前では使用できない。
UART初期化(libtm_init)がカーネル起動経路でしか呼ばれないため。
main() 内での初期化エラーは Error_Handler() に落ちると
__disable_irq(); while(1){} で無出力のままハングする。

## HALドライバを追加で使うとき
Appliプロジェクトは親の Drivers/STM32N6xx_HAL_Driver/Src/ を
.project の <link> で個別参照している。新しいHALを使う場合は
CubeIDE で New > File > Advanced > "Link to file in the file system"
から追加すること。conf.h の MODULE_ENABLED だけでは足りない。
## HALドライバを追加で使うとき
Appliプロジェクトは親の Drivers/STM32N6xx_HAL_Driver/Src/ を
.project の <link> で個別参照している。新しいHALを使う場合:
1. stm32n6xx_hal_conf.h の HAL_xxx_MODULE_ENABLED を有効化
2. CubeIDE で New > File > Advanced > "Link to file in the file system"
   から該当 .c を追加(_ex.c も忘れずに)
3. Clean → Build
Debug/配下のmk系を手動編集してはいけない(Cleanで消える)。

## tm_printf の制約
knl_start_mtkernel() より前では使用不可。UART初期化(libtm_init)が
カーネル起動経路でしか呼ばれないため。カーネル起動前の初期化関数は
Error_Handler() を呼ばず、結果を変数に記録してカーネル起動まで
必ず到達させること。

## FSBLの残留状態
FSBLがペリフェラルを触った状態でアプリが起動するため、
HAL_xxx_Init が「既に有効」と判断してHAL_ERRORを返すことがある。
__HAL_RCC_xxx_FORCE_RESET() / RELEASE_RESET() で
パワーオンデフォルトに戻してから初期化すること。
MDF1で実際に発生した。

## デバッグ実行
必ず mtk3bsp2_stm32n657_FSBL Debug 構成で起動する。
Appli 単体で起動するとブートシーケンスが成立せず usermain() に
到達しない。Startup タブに Appli が追加されていることも確認。


## 部門
RTOSアプリケーション部門・学生部門。カーネル改変は部門違いになる

## このファイルについて
開発中の制約メモ。人間向けの説明は README.md を参照。