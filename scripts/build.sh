#!/bin/bash
set -e

CUBE="/c/ST/STM32CubeIDE_2.2.0/STM32CubeIDE/plugins"
export PATH="$(echo $CUBE/com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.*/tools/bin):$PATH"
export PATH="$(echo $CUBE/com.st.stm32cube.ide.mcu.externaltools.make.*/tools/bin):$PATH"

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BSP="$ROOT/mtk3bsp2_stm32n657"

make -C "$BSP/FSBL/Debug"  -j16 all
make -C "$BSP/Appli/Debug" -j16 all