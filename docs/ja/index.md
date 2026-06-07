# STM32F746G-DISCO ガイド

STM32F746G-DISCO（STM32F746NGH6 / Cortex-M7）向けの **ベアメタル + Eclipse ThreadX** ファームウェア。ST 公式 HAL を使い、ビルドは CMake + Ninja。HAL/CMSIS/ThreadX/CoreMark のソースと ARM GNU ツールチェーンは初回 configure 時に自動取得される。

## このガイドの内容

- [ハードウェア / ボード](hardware/board.md) — クロック・キャッシュ・VCP・LED・メモリ配置と参照資料
- [ビルド (CMake)](build/cmake.md) — configure / build / flash、ツールチェーン自動 DL、submodule
- [RTOS (ThreadX)](rtos/threadx.md) — ThreadX 統合と割込み優先度の注意点
- [Shell アーキ概要](rtos/shell-architecture.md) — シェルの全体像（コマンド定義 / backend 追加 / ピン・UART 前提 / 危険コマンドゲート）

## ファームウェア

単一の firmware **`threadx`** = 対話 ThreadX シェル（USART1 VCP）。アプリは shell コマンドとして起動する。

| コマンド | 内容 |
|--------|------|
| `help` / `echo` | コマンド一覧 / エコー |
| `version` / `uptime` / `reboot` | FW・MCU 情報 / 稼働時間 / 再起動 |
| `thread` | スレッド一覧 + スタック使用量 |
| `devmem` | メモリ peek/poke/dump（アドレス範囲ゲート付き） |
| `coremark` | EEMBC CoreMark ベンチを実行（~12s）|

## チップ構成

- Cortex-M7、ハードウェア FPU（`fpv5-sp-d16`）
- I-Cache / D-Cache 有効
- SYSCLK **216 MHz**（HSE 25 MHz → PLL M=25 N=432 P=2、VOS1 + over-drive、7 flash wait states）
- VCP: USART1（TX=PA9 / RX=PB7、115200 8N1、ST-Link 仮想 COM）
