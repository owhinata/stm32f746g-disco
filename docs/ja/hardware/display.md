# LCD（LTDC、4.3″ 480×272 RGB）

ボード搭載の 4.3 インチ RGB LCD（Rocktech **RK043FN48H-CT**、480×272、容量タッチ）を STM32F746 内蔵の **LTDC**（LCD-TFT ディスプレイコントローラ）で駆動する。ドライバは `port/ltdc/ltdc_display.{c,h}`、シェルコマンドは `lcd`（`shell/cmds/cmd_lcd.c`）。

LTDC + GUIX エピック（#48）の **Phase 1**（#52）。本フェーズは **単層 RGB565・静的表示**（SDRAM フレームバッファを LTDC が連続リード）まで。`lcd` コマンドで単色塗り・カラーバー・グラデを出し、**配線 / RGB チャネル順 / ピクセルクロック**を実機検証する。DMA2D・ダブルバッファ（#53）、タッチ（#54）、GUIX（#55/#56）は後続。タッチはここでは未実装。

## 構成

| 項目 | 値 | 根拠 |
|------|----|------|
| パネル | RK043FN48H-CT（480×272、24-bit RGB、容量タッチ） | UM1907 / rk043fn48h.h |
| ピクセルフォーマット | **RGB565**（16 bpp） | LTDC layer 0 |
| フレームバッファ | 480×272×2 = **261120 B（255 KB）**、`.sdram` 配置 | non-cacheable |
| LCD_CLK | **9.6 MHz**（PLLSAI） | 下記導出 |
| タイミング（spec） | HSYNC=41 / HBP=13 / HFP=32 / VSYNC=10 / VBP=2 / VFP=2 | rk043fn48h.h |
| 極性 | HS / VS / DE = **active-low**、PC = **IPC**（非反転） | ST BSP / RM0385 §18.7.5 |
| 割込み | **不使用**（静的表示）。underrun/transfer error はポーリング | RM0385 §18.7.9 |

LTDC が register に積むのは spec から **各 −1** した値（`HorizontalSync=40 / VerticalSync=9 / AccumulatedHBP=53 / AccumulatedVBP=11 / AccumulatedActiveW=533 / AccumulatedActiveH=283 / TotalWidth=565 / TotalHeigh=285`）。

## クロック: PLLSAI → LCD_CLK 9.6 MHz

LTDC のピクセルクロックは、それまで未使用だった **PLLSAI** から作る。PLLSAI は**メイン PLL と入力分周 M（=25）を共有**する（RM0385 §5.3.8）ので:

```
VCO_in   = HSE / M       = 25 MHz / 25 = 1 MHz
PLLSAI   VCO = VCO_in × PLLSAIN(192) = 192 MHz      (100..432 MHz 内, RM0385 §5.3.24)
PLLLCDCLK = VCO / PLLSAIR(5)          = 38.4 MHz
LCD_CLK   = PLLLCDCLK / PLLSAIDIVR(4) = 9.6 MHz      (RM0385 §5.3.25 RCC_DCKCFGR1)
```

PLLSAI は**メイン PLL とは別系統**なので、`HAL_RCCEx_PeriphCLKConfig(RCC_PERIPHCLK_LTDC)` を投入しても **SYSCLK（216 MHz）/ FMC SDRAM クロック（108 MHz）には影響しない**。

!!! note "tx_application_define から呼べる理由（HAL tick）"
    HAL の PLLSAI lock 待ちは `HAL_GetTick()` ベース（100 ms タイムアウト）。本プロジェクトの `SysTick_Handler` は ThreadX タイマ稼働の有無に関係なく**常に `HAL_IncTick()` を呼ぶ**（`port/threadx/tx_glue.c`）ので、スケジューラ起動前の `tx_application_define` 内でも tick は進み、lock 失敗時もタイムアウトで抜ける。`sdram_init()` が TIM2 busy-wait を使うのは ThreadX タイマ tick が別物だからで、HAL tick は別系統。

## ピン

LTDC ピンは **PG12 のみ AF9**、残りは **AF14**（UM1907 / ST BSP）:

`PE4 / PG12(AF9) / PI9,10,14,15 / PJ0-11,13-15 / PK0,1,2,4,5,6,7`

加えて手動駆動の GPIO 出力 2 本: **LCD_DISP = PI12**、**LCD_BL_CTRL = PK3**（表示有効化 + バックライト）。`ltdc_init()` の最後にまとめて assert する。既存ペリフェラル（VCP=PA9/PB7、LED=PI1、カメラ DCMI/I2C1、SDMMC1、QSPI、FMC SDRAM）とピン衝突しない（PI9-15 は LED の PI1 と別ピン）。

## フレームバッファ: `.sdram`（NOLOAD）+ MPU non-cacheable

フレームバッファはカメラフレームと同じ **`.sdram`（NOLOAD）セクション**に置く。`bsp_init()` が MPU region 0 で 8 MB 全域を **Normal・non-cacheable** に再マップ済みなので、**LTDC のリード DMA と CPU の書込はコヒーレント**（キャッシュ maintenance 不要）。詳細は [SDRAM](sdram.md)。

```c
static uint16_t ltdc_fb[480 * 272] __attribute__((aligned(32), section(".sdram")));
```

`.sdram` 配置の現況（リセット非生存）: `cam_frame` 150 KB + `cam_ring[4]` 600 KB + `ltdc_fb` 255 KB ≈ **1.0 MB / 8 MB**（実機 map で SDRAM 12.27% 使用、FB は `0xC00BB800`）。

## 初期化フロー

`ltdc_init()`（`tx_application_define` から、**`sdram_init()` が成功した場合のみ**）:

0. **SDRAM ガード**: `sdram_is_up()` でなければ `LTDC_ERR_STATE` で即 return。フレームバッファは `.sdram`（0xC0000000）にあり、FMC 未初期化で触ると fault するため、呼び出し側でもゲートする二重防御。
1. PLLSAI → LCD_CLK 9.6 MHz（`HAL_RCCEx_PeriphCLKConfig`）
2. LTDC クロック + GPIO（AF + DISP/BL 出力、この時点では消灯）
3. **フレームバッファを黒クリア**（`.sdram` は NOLOAD = 不定値。ConfigLayer / バックライト ON より前に消すことで起動時のゴミ表示を防ぐ）
4. `HAL_LTDC_Init`（上記タイミング / 極性 / 背景黒）
5. `HAL_LTDC_ConfigLayer`（layer 0、RGB565、480×272、FB = `ltdc_fb`）
6. **最後に** LCD_DISP / LCD_BL_CTRL を assert（表示 + バックライト ON）

ポーリングのみ（割込み・DMA・ThreadX オブジェクト不使用）。idempotent・fail-soft（失敗時は DISP/BL off + `HAL_LTDC_DeInit` で cleanup し `lcd` コマンドが報告、他は継続）。

## `lcd` シェルコマンド

```
lcd info            パネル / クロック / フレームバッファ / LTDC エラーフラグ
lcd fill <color>    全面塗り（色名 black/blue/green/cyan/red/magenta/yellow/white または 0xRGB565）
lcd bar             8 縦カラーバー（RGB 配線・ビット順検証）
lcd grad            横グラデ（黒→白、ピクセルクロック検証）
lcd clear           黒
lcd on | lcd off    表示有効化 + バックライト
```

Phase 1 は CPU 直書き（`.sdram` は non-cacheable）。`lcd info` の **errors 行**は `LTDC->ISR` の FIFO underrun / transfer error フラグ（RM0385 §18.7.9）を表示する — LTDC 割込みを使わなくても、**underrun=YES は SDRAM 帯域不足の証拠**になる。

実行例:

```
sh> lcd info
panel:   RK043FN48H-CT 480x272 RGB565 (LTDC layer 0)
clock:   LCD_CLK 9.60 MHz (PLLSAI N=192 R=5, DIVR/4)
fb:      0xc00bb800 (.sdram, non-cacheable)
state:   up
errors:  underrun=no transfer=no
sh> lcd bar
sh> lcd fill red
```

## 帯域

LTDC は active 期間に毎フレーム FB を連続リードする。RGB565・9.6 MHz ピクセルクロックで上限 **9.6 Mpix/s × 2 B = 19.2 MB/s**（active fetch のみなら実効 ~15.6 MB/s）。SDRAM の理論帯域（16-bit @ 108 MHz = 216 MB/s）に対し約 9% で、DCMI 書込（QVGA ~11 fps ≈ 1.65 MB/s）や CPU と競合しても余裕がある（実機で `camera capture`/`stream` 回帰、underrun=no を確認済）。ダブルバッファ + より高い解像度/fps（#53 以降）では再評価する。

## 参照

- UM1907 — RK043FN48H-CT LCD（LTDC RGB 配線、LCD_DISP/LCD_BL_CTRL）
- RM0385 §18 — LTDC、§5（RCC / PLLSAI）
- rk043fn48h.h — パネルタイミング定数（解像度 / HSYNC / porch / 分周）
- `_ref/STM32Cube_FW_F7_V1.17.0/Drivers/BSP/STM32746G-Discovery/stm32746g_discovery_lcd.c` — LTDC / PLLSAI / GPIO 初期化の参照実装（read-only）
