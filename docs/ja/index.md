# STM32F746G-DISCO ガイド

STM32F746G-DISCO（STM32F746NGH6 / Cortex-M7）向けの **ベアメタル + Eclipse ThreadX** ファームウェア。ST 公式 HAL を使い、ビルドは CMake + Ninja。HAL/CMSIS/ThreadX/CoreMark のソースと ARM GNU ツールチェーンは初回 configure 時に自動取得される。

## このガイドの内容

- [ハードウェア / ボード](hardware/board.md) — クロック・キャッシュ・VCP・LED・メモリ配置と参照資料
- [ビルド (CMake)](build/cmake.md) — configure / build / flash、ツールチェーン自動 DL、submodule
- [RTOS (ThreadX)](rtos/threadx.md) — ThreadX 統合と割込み優先度の注意点

## アプリ

| アプリ | 内容 |
|--------|------|
| `threadx` | Eclipse ThreadX：2 スレッド（LED 点滅 + UART 出力）|
| `coremark` | EEMBC CoreMark（オプション、`-DBUILD_COREMARK=ON`）|

## チップ構成

- Cortex-M7、ハードウェア FPU（`fpv5-sp-d16`）
- I-Cache / D-Cache 有効
- SYSCLK **216 MHz**（HSE 25 MHz → PLL M=25 N=432 P=2、VOS1 + over-drive、7 flash wait states）
- VCP: USART1（TX=PA9 / RX=PB7、115200 8N1、ST-Link 仮想 COM）
