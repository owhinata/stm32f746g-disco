# ハードウェア / ボード

対象ボード: **STM32F746G-DISCO**（MCU: STM32F746NGH6、Cortex-M7、最大 216 MHz）。

## クロック

| 項目 | 設定 |
|------|------|
| 入力 | HSE 25 MHz（オンボード）|
| PLL | M=25, N=432, P=2 → SYSCLK 216 MHz |
| 電源 | VOS スケール 1 + over-drive |
| Flash | 7 wait states（210 MHz 超）|
| バス | HCLK 216 / APB1 54 / APB2 108 MHz |

設定は `src/bsp.c` の `SystemClock_Config()`。

## FPU / キャッシュ

- FPU: `SystemInit()`（CMSIS）で有効化 + ハードフロートでビルド（`-mfpu=fpv5-sp-d16 -mfloat-abi=hard`）
- I-Cache / D-Cache: `bsp_init()` 内で `SCB_EnableICache()` / `SCB_EnableDCache()`

## VCP（仮想 COM ポート）

ST-Link V2.1 の VCP は **USART1** に接続:

| 信号 | ピン | AF |
|------|------|----|
| VCP_TX | PA9 | AF7 |
| VCP_RX | PB7 | AF7 |

115200 8N1。`printf` は newlib の `_write()` を UART 送信にリターゲット（`src/bsp.c`）。

!!! warning "PA9 の共用"
    UM1907 より、PA9 は **VCP_TX と OTG_FS_VBUS の共用**。工場出荷の既定ソルダーブリッジ構成（R64=ON, R63=OFF, R58=ON）で VCP_TX が有効。USB OTG ホスト用に変更している場合は VCP_TX が無効になる。

## LED

- ユーザー LED **LD1（緑）= PI1**（アクティブ High）

## メモリ配置

| 領域 | アドレス | サイズ |
|------|----------|--------|
| Flash | 0x08000000 | 1 MB |
| ITCM-RAM | 0x00000000 | 16 KB |
| DTCM-RAM | 0x20000000 | 64 KB |
| SRAM1+2 | 0x20010000 | 256 KB |

リンカスクリプト: `ldscript/STM32F746NGHx_FLASH.ld`（startup の `_estack`/`_sidata`/`_sbss` 等と整合）。

## 参照資料

`_ref/`（git 管理外）に配置:

- `rm0385-*.pdf` — STM32F75x/F74x リファレンスマニュアル
- `um1907-*.pdf` — Discovery キット ユーザーマニュアル

## バックアップ / 復元

```bash
st-flash read backup_full.bin 0x08000000 0x100000   # 全 Flash 吸い出し
st-flash --reset write backup_full.bin 0x08000000   # 書き戻し
```
