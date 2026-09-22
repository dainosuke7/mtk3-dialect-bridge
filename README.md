# 屋内音お知らせ機

STM32N6570-DK + μT-Kernel 3.0 / TRONプログラミングコンテスト2026 応募作品
（RTOSアプリケーション部門・学生部門）

聴覚障害のある人に向けた、屋内の音のお知らせ機です。
オンボードの PDM マイクで拾った音を Neural-ART NPU で音響イベント検出（AED）し、
結果を LCD・LED・UART(JSON) で知らせます。
推論中も音声のパススルーを途切れさせず、通知遅延の上限を実測で示すことを目標にしています。
外付けハードは使わず、STM32N6570-DK 単体で完結します。

検出クラスは現時点では ST 公開の AED モデル（YAMNet 1024 派生、ESC-10 の10クラス:
赤ちゃんの泣き声・犬・くしゃみ・時計の秒針音 など）に従います。

## 現在の状態

| 段階 | 内容 | 状態 |
|---|---|---|
| Phase 0 | PDMマイク → MDF1 → SAI1 → WM8904 のパススルー、時間計測基盤、フォルト可視化 | 完了 |
| Phase 1 | 外部フラッシュ上のモデル重み領域へのアクセス確認、NPU 用メモリと NPU 周辺の初期化 | 進行中 |
| Phase 2 | Neural-ART NPU での音響イベント検出 | 未着手 |

## ハードウェア

- STM32N6570-DK (MB1939) / Cortex-M55 + Neural-ART NPU
- 入力: オンボード PDM MEMS マイク → MDF1_Filter0 + GPDMA
- 出力: SAI1 → WM8904 コーデック → ヘッドホンジャック (CN15)
- シリアル: 115200 / 8N1 / フロー制御なし

## ビルドと実行

### 必要なもの

- STM32CubeIDE 2.2.0
- STM32CubeProgrammer（外部フラッシュへの書き込みに使用）
- STM32N6570-DK 実機

### ビルド

```bash
bash scripts/build.sh
```

FSBL と Appli の両方をビルドします。CubeIDE の GUI からビルドしても同じです。

### 実行（デバッグ起動）

1. SW1 (BOOT1) を **1-3 側（Development boot）** にする
2. CubeIDE で **`mtk3bsp2_stm32n657_FSBL Debug`** 構成を実行する

   この起動構成は `mtk3bsp2_stm32n657/FSBL/mtk3bsp2_stm32n657_FSBL Debug.launch`
   としてリポジトリに含まれており、Startup の loadList に Appli 側の .elf も
   登録済みです。クローン直後にそのまま実行できます。

   Appli 単体で起動するとブートシーケンスが成立せず `usermain()` に到達しません。

3. ログの取得

   ```bash
   powershell scripts/log.ps1   # → logs/uart.log
   ```

### モデル重みの書き込み（Phase 2 以降）

モデルの重みファイルはリポジトリに含めていません（ST のライセンス下の
配布物であり、3MB を超えるため）。下記から取得して外部フラッシュに
書き込んでください。

```bash
git clone https://github.com/STMicroelectronics/STM32N6-GettingStarted-Audio.git
cd STM32N6-GettingStarted-Audio
# 取得したコミット: 46f1f976442cd1697fedf98f3b5875d998c61980 (v2.3.0)

export DKEL="<STM32CubeProgrammer_N6>/bin/ExternalLoader/MX66UW1G45G_STM32N6570-DK.stldr"
STM32_Programmer_CLI -c port=SWD mode=HOTPLUG -el $DKEL -hardRst \
  -w Projects/X-CUBE-AI/models/aed_weights.hex
```

書き込み先は hex に埋め込まれた `0x70180000` です（署名不要）。

## ディレクトリ構成

```
mtk3bsp2_stm32n657/
  Appli/
    Application/        本プロジェクトで新規に作成したコード
      audio/            マイク入力・音声出力・FIFO・WM8904 制御
      trace/            DWT CYCCNT による時間計測とCSVダンプ
      fault/            フォルト・未実装IRQのUART可視化
      extflash/         外部フラッシュ（重み）を XSPI2 でメモリマップ
        mx66uw1g45g/    ST のフラッシュ用コンポーネントドライバ（下表）
      npu/              NPU 用内部メモリ・NPU・NPU キャッシュ・RIF の初期化
      usermain.c        タスク生成とアプリのエントリ
    Core/               CubeMX 生成コード（一部を手で追加。CLAUDE.md 参照）
    mtk3_bsp2/          μT-Kernel 3.0 BSP2（無改変）
  FSBL/                 First Stage Boot Loader（CubeMX 生成）
  Drivers/ Middlewares/ ST 提供
scripts/                ビルド・ログ取得スクリプト
CLAUDE.md               開発中の制約メモ
```

## 本プロジェクトが新規に作成した部分

- `mtk3bsp2_stm32n657/Appli/Application/` 配下すべて（`extflash/mx66uw1g45g/` を除く。下表の ST 製ドライバ）
- `mtk3bsp2_stm32n657/Appli/Core/` への追加部分
  - `Src/main.c`: `MX_I2C2_Init()` / `MX_SAI1_Init()` / `MX_MDF1_Init()` / `MPU_Config()`
  - `Src/stm32n6xx_it.c`: `GPDMA1_Channel0/2_IRQHandler`, `MDF1_FLT0_IRQHandler`
  - `Inc/main.h`, `Inc/stm32n6xx_hal_conf.h`: 上記に伴う宣言とモジュール有効化
- `scripts/` 配下

変更理由の詳細は [CLAUDE.md](CLAUDE.md) の「例外として変更した場所」を参照。

## 使用している既存ソフトウェア

コンテスト規則 1.3（名称・権利者・入手方法・機能・権利処理の保証）に基づく一覧です。
「ライセンス」欄はリポジトリ内の LICENSE ファイルまたはソースのヘッダで確認した値です。

### 同梱しているもの

| 名称 | 権利者 | 機能 | ライセンス（確認元） | 入手方法 | 権利処理 |
|---|---|---|---|---|---|
| μT-Kernel 3.0 BSP2 | Ken Sakamura / TRON Forum | リアルタイムOS本体と STM32N6 向け BSP | T-License 2.1 / 2.2（ファイルごとのヘッダに混在） | TRON Forum 配布物 | 無改変で同梱。ヘッダ保持 |
| STM32N6xx HAL/LL Driver | STMicroelectronics | ペリフェラルドライバ | BSD-3-Clause（`Drivers/STM32N6xx_HAL_Driver/LICENSE.txt`） | STM32CubeN6 | ヘッダ保持 |
| CMSIS Core | Arm Limited | Cortex-M55 コア定義 | Apache-2.0（`Drivers/CMSIS/LICENSE`） | STM32CubeN6 | ヘッダ保持 |
| CMSIS Device STM32N6xx | STMicroelectronics | デバイスレジスタ定義・スタートアップ | Apache-2.0（`Drivers/CMSIS/Device/ST/STM32N6xx/LICENSE.txt`） | STM32CubeN6 | ヘッダ保持 |
| STM32 ExtMem Manager | STMicroelectronics | FSBL の外部フラッシュ制御・アプリ起動 | **要確認**（ヘッダは「コンポーネント直下の LICENSE に従う、無ければ AS-IS」。LICENSE は未同梱） | STM32CubeN6 | 要確認 |
| MX66UW1G45G Component Driver V1.1.0 | STMicroelectronics | 外部 NOR フラッシュへのコマンド（リセット・DTR-OPI 設定・メモリマップ）。`Appli/Application/extflash/mx66uw1g45g/` | BSD-3-Clause（同梱の `LICENSE.txt`。GettingStarted-Audio の `LICENSE.md` でも「BSP Components」は BSD-3-Clause） | STM32N6-GettingStarted-Audio v2.3.0（commit 46f1f97）の `Drivers/BSP/Components/mx66uw1g45g/` | `.c`/`.h` は無改変。`mx66uw1g45g_conf.h` は同梱テンプレートからインクルードとダミーサイクル値だけ変更（変更点をファイル内に記載）。ヘッダ保持 |
| STM32N6570-DK BSP（XSPI NOR 部分） | STMicroelectronics | `extflash.c` の初期化手順の参照元（`stm32n6570_discovery_xspi.c`） | BSD-3-Clause（GettingStarted-Audio の `LICENSE.md` で「STM32N6570-DK BSP Drivers」） | 同上の `Drivers/BSP/STM32N6570-DK/` | ファイルは同梱せず、必要な手順だけを `extflash.c` に書き起こした。参照箇所と相違点をソースのコメントに記載 |
| STM32N6-GettingStarted-Audio アプリ部（`Int_Mem_Config()` / `NPU_Config()`）と NPU デバイス定義（`ATON.h`） | STMicroelectronics | `npu_hw.c` の初期化手順と、NPU のバージョンレジスタの番地・期待値の参照元 | SLA0044（`Projects/LICENSE.md`。GettingStarted-Audio の `LICENSE.md` で「Projects」「AI Runtime」は SLA0044） | STM32N6-GettingStarted-Audio v2.3.0（commit 46f1f97）の `Projects/GS/Src/audio_bm.c`、`Projects/Common/misc_toolbox.c`、`Middlewares/ST/AI/Npu/Devices/STM32N6xx/ATON.h` | ファイルは同梱せず、手順と定数だけを `npu_hw.c` に書き起こした。参照箇所と相違点をソースのコメントに記載。SLA0044 は ST 製デバイス上での使用に限る条件で、本機は STM32N6 上でのみ動く |
| STM32N6xx HAL（RAMCFG / RIF / CACHEAXI 部分） | STMicroelectronics | `npu_hw.c` のレジスタ操作の参照元（`stm32n6xx_hal_ramcfg.c` / `_rif.c` / `_cacheaxi.c`） | BSD-3-Clause（GettingStarted-Audio の `LICENSE.md` で「STM32N6xx HAL/LL Drivers」） | 同上の `Drivers/STM32N6xx_HAL_Driver/`（本リポジトリの `Drivers/` には含まれていない） | ファイルは同梱せず、同じレジスタ操作を `npu_hw.c` に書き起こした。参照箇所をソースのコメントに記載 |

### Phase 2 で追加予定のもの

導入時に上の表へ移し、ライセンスを実ファイルで確認してから記載する。

| 名称 | 権利者 | 機能 | 入手方法 | 同梱 |
|---|---|---|---|---|
| ST Edge AI ランタイム（ll_aton, NetworkRuntime*.a） | STMicroelectronics | NPU 推論ランタイム | STM32N6-GettingStarted-Audio | 検討中 |
| STM32 AI AudioPreprocessing Library | STMicroelectronics | log-mel スペクトログラム計算 | 同上 | 検討中 |
| CMSIS-DSP | Arm Limited | FFT 等 | 同上 | 検討中 |
| AED モデル重み（YAMNet 1024 派生, aed_weights.hex） | STMicroelectronics | 学習済みモデル | 同上 | **含めない**（上記手順で取得） |

> **TODO（提出前）**
> - ExtMem Manager のライセンスを STM32CubeN6 の配布物で確認する
> - Phase 2 分は各配布物の LICENSE を読んで再配布可否を確認する
> - 同梱するものはライセンス全文を `licenses/` に置く

## ライセンス

本プロジェクトで新規に作成したコードの扱いは提出までに確定させます。

同梱している既存ソフトウェアは、それぞれ上表のライセンスに従います。
`mtk3bsp2_stm32n657/Appli/mtk3_bsp2/` 配下は各ファイルのヘッダに記載された
T-License（2.1 または 2.2）に従い、ヘッダは削除していません。
