# 
STM32N6570-DK + μT-Kernel 3.0 / TRONプログラミングコンテスト2026 応募作品


## ライセンス

本リポジトリには μT-Kernel 3.0 BSP2 (TRONフォーラム) が含まれます。
該当部分は T-License 2.2 に従います。

- mtk3bsp2_stm32n657/Appli/mtk3_bsp2/ : μT-Kernel 3.0 BSP2 (無改変)
- mtk3bsp2_stm32n657/Drivers/, Middlewares/ : STマイクロエレクトロニクス提供

本プロジェクトで新規に作成したコードは以下です。
- mtk3bsp2_stm32n657/Appli/Application/ 配下
- mtk3bsp2_stm32n657/Appli/Core/Src/main.c への追加部分
  (MX_I2C2_Init, MX_SAI1_Init, MX_MDF1_Init, MPU_Config)