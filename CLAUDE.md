TRONプログラミングコンテスト2026 — 屋内音お知らせ機

聴覚障害者向けの屋内音お知らせ機。オンボードPDMマイクの音声を NPU で 音響イベント検出（AED）し、LCD・LED・UART(JSON) で通知する。 推論中も音声パススルーを途切れさせず、通知遅延の上限を実測で示す。

部門: RTOSアプリケーション部門・学生部門
締切: 2026-09-30 18:00（応募フォーム提出。ボード送付は不要＝規則の3-b）
規則 1.3: OSのAPI仕様を変えない限り μT-Kernel 3.0 の改変は許容される。 他者の既存ソフトウェアを使う場合は、名称・権利者・入手方法・機能・ 権利処理の保証をドキュメントに記載する義務がある（README に一覧を置く）
編集してよい場所
原則: mtk3bsp2_stm32n657/Appli/Application/ 配下のみ
触らない: Appli/mtk3_bsp2/（μT-Kernel 本体）, Drivers/, Middlewares/, Secure_nsclib/
例外として変更した場所（増やしたら必ずここに追記する）
場所	内容	理由
Appli/Core/Src/main.c	MX_I2C2_Init, MX_SAI1_Init, MX_MDF1_Init, MPU_Config を追加	音声経路（WM8904制御・SAI出力・PDM入力）に必要
FSBL/Core/Src/main.c（USER CODE 区画内）	Debug 経路で JumpToApplication() の直前に外部フラッシュのメモリマップを有効化	Debug 起動では BOOT_Application() を通らず XSPI2 がマップされない。重み(0x70180000)を読むために必要。Release 経路は元から同じ処理をしている
Appli/Core/Inc/stm32n6xx_hal_conf.h	（予定）HAL_RAMCFG / HAL_CACHEAXI / HAL_RIF の MODULE_ENABLED	NPU 用 AXISRAM3〜6 と NPU キャッシュ・RIF 設定に必要
変更は可能な限り USER CODE BEGIN/END 区画の中に書く（CubeMX 再生成で消えない）
コマンド
ビルド: bash scripts/build.sh
書き込み・実行: CubeIDE で mtk3bsp2_stm32n657_FSBL Debug 構成を Debug 実行
Startup タブに mtk3bsp2_stm32n657_Appli が追加されていること
Appli 単体の構成で起動すると usermain() に到達しない
直前に FSBL をビルドしておく
ログ: powershell scripts/log.ps1 → logs/uart.log
COM ポートは1プロセスしか開けない。他のターミナルを閉じてから
重みの書き込み（外部フラッシュ 0x70180000、署名不要）:
  STM32_Programmer_CLI -c port=SWD mode=HOTPLUG -el <ExternalLoader>/MX66UW1G45G_STM32N6570-DK.stldr -hardRst -w aed_weights.hex

CubeProgrammer は CubeIDE 同梱: C:\ST\STM32CubeIDE_*\STM32CubeIDE\plugins\*cubeprogrammer*\tools\bin 重み hex はリポジトリに入れない（ST ライセンス・容量）。取得元は README に記載

ハード
STM32N6570-DK / Cortex-M55 600MHz + Neural-ART NPU
内蔵ユーザFlashなし。外部フラッシュ + 署名必須
SW1(BOOT1) は 1-3 側（Development boot）
シリアル: COM3 / 115200 / 8N1 / フロー制御なし
メモリマップ
領域	アドレス	用途
アプリ（内部RAM）	ROM 0x34000400 +511K / RAM 0x34080000 +1536K（〜0x34200000）	BSP2 リンカ定義。コード領域 511K を超えないか監視
AXISRAM3〜5	0x34200000〜	未使用。LCD フレームバッファ候補
AXISRAM6	0x34350000〜（NPU は 144KB 使用）	NPU activations
外部フラッシュ: アプリ	0x70100000〜（FSBL ソース上。起動ログは 0x71000400 と表示、未解決）	署名済みアプリ
外部フラッシュ: 重み	0x70180000〜（3,282,785 B）	AED モデル重み
オーディオ経路（MB1939 回路図より）
入力: PDMマイク U13/U14 → MDF1 Filter0（CCK0=PE2, DATIN0=PE8, AF4）→ GPDMA1 ch0。 256サンプル/16ms。32bit 出力を /256 して16bitに飽和変換
出力: SAI1 Block A（マスタTX, I2S）→ WM8904（制御は I2C2）→ CN15。GPDMA1 ch2。400フレーム/25ms
MCKDIV は HAL の自動計算を使わず直接 12 を指定（自動計算が半分の値を返した）
起動ログの SAI1 ... Fs=7998Hz は表示式の誤り。実測は約16kHz
MDF の実レートは約16128Hz（+0.8%、分周設定由来）。実害なし
MIC_DET で外部マイクボード装着時はオンボードマイクがバイパスされる
タスク構成と優先度（小さいほど高い）
優先度	タスク	役割
5	task_pcm	DMA通知（イベントフラグ4ビット）を受けて入力変換・リング操作・出力充填
10	task_audio, task_1, task_2	パススルー制御・統計表示 / LED 点滅
20	reporter	1秒レート表示
32	dump	トレース状態機械・CSV ダンプ
DMA コールバックは TRACE → カウンタ更新 → tk_set_flg だけ行い即 return
出力バッファが不足したら無音を詰めて必ず全体を埋める
PCM リング（pcm_fifo）は SPSC。消費者を増やせない。推論用は音声タスクが別リング（tap）にもコピーする
実装規約
タスク間のデータ受け渡しは mbx / mbf。ISR→タスクの通知はイベントフラグかセマフォ
複数文脈から触るカウンタは DI/EI で最小区間を保護（BASEPRI=1 で PendSV もマスクされるのでタスク間排他にも効く）
共有データの排他が要るときは tk_cre_mtx に TA_INHERIT（優先度継承）
時間計測は DWT CYCCNT（NOW()）。差分は必ず UW 同士で引く。600MHz で約7.16秒で折り返す
タスクのスタック溢れはフォルト機構で捕まらない（USE_SPMON 無効）。大きなローカル配列を置かず静的領域を使う
ライセンスヘッダ（T-License 2.2、ST の各ライセンス）を消さない
デバッグ基盤
フォルト可視化（Application/fault/）: 起動時にベクタテーブルを RAM にコピーし、未実装 IRQ 180本とフォルト例外5本を差し替え。 CFSR/BFAR/スタック上の PC を UART 直叩きで出してから停止。デバッガ接続時は __BKPT で止まるので F8 で続行
FAULT_TEST（fault.h）: 0=無効 / 1=BusFault / 2=ゼロ除算 / 3=未実装IRQ / 4=STKOF。コミット時は必ず 0
トレース（Application/trace/）: TRACE(id,arg) で (CYCCNT, id, arg) を記録。trace_start(ms) で区間記録し、終了後に CSV ダンプ。 ダンプ中は trace_muted() で他の出力を抑制
テスト用スイッチ: AUDIO_PRIO_TEST（音声タスクを最低優先度にして負荷タスクを回す）、FLASH_PROBE（0x70180000 読み出し確認）。 コミット時は 0 に戻す
既知の罠
tm_printf はカーネル起動前（knl_start_mtkernel より前）に使えない。 起動前の初期化関数は Error_Handler() を呼ばず、結果を変数に記録してカーネル起動まで到達させる
FSBL がペリフェラルを触った状態でアプリが起動する。 HAL_xxx_Init が HAL_ERROR を返したら __HAL_RCC_xxx_FORCE_RESET()/RELEASE_RESET() で戻してから初期化（MDF1 で発生）
STM32N6 に DMA_CIRCULAR は無い。 循環DMAは HAL_DMAEx_List_* で組み、ノードは .noncacheable に置く
キャッシュ: CPU が書いて DMA が読む前は SCB_CleanDCache_by_Addr、DMA が書いて CPU が読む前は SCB_InvalidateDCache_by_Addr
割り込みを有効化したらハンドラを必ず用意する。 HAL_MDF_AcqStart_DMA は飽和/overrun 割り込みを自動で有効化する
外部フラッシュは Debug 起動ではメモリマップされていない（FSBL 例外変更で対処）
ST の GettingStarted-Audio を実機でそのまま動かさない。 起動時に OTP ヒューズを不可逆に焼き、外部フラッシュの FSBL・アプリも上書きする
ST の FreeRTOS 版コードを持ち込まない。 全 IRQ の優先度を上書きする処理がある
PowerShell 5.1 用スクリプトは UTF-8 BOM 付きで保存する（BOM 無しだと日本語コメントで param() が壊れる）
ビルド設定
Appli プロジェクトは親の Drivers/STM32N6xx_HAL_Driver/Src/ を .project で個別参照している。新しい HAL を使う場合:
stm32n6xx_hal_conf.h の HAL_xxx_MODULE_ENABLED を有効化（例外一覧に追記）
CubeIDE で New > File > Advanced > "Link to file in the file system" から .c を追加（_ex.c も）
Clean → Build
Application/ 配下に新規ファイル・フォルダを作ったら CubeIDE でプロジェクトを Refresh（F5）
Debug/ 配下の mk 系は CubeIDE が生成する。手動編集しない。ビルド対象の追加・除外は GUI で行い .cproject に永続化する
Git
コミットメッセージは日本語。1行目は Conventional Commits（feat:, fix:, refactor:, docs: など、スコープは audio/fault/trace/npu 等）、空行、なぜ変えたか
論理単位でステージする
基準点にタグ: phase0-baseline（10分連続 under/over/late=0、応答1〜2μs、CPU占有0.30%）
このファイルについて

開発中の制約メモ（Claude Code 向け）。人間向けの説明は README.md を参照。 記述が実態と食い違ったら、コードではなくこのファイルを直す。