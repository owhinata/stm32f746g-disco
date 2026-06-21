# LCD（LTDC、4.3″ 480×272 RGB）

ボード搭載の 4.3 インチ RGB LCD（Rocktech **RK043FN48H-CT**、480×272、容量タッチ）を STM32F746 内蔵の **LTDC**（LCD-TFT ディスプレイコントローラ）で駆動する。ドライバは `port/ltdc/ltdc_display.{c,h}`、シェルコマンドは `lcd`（`shell/cmds/cmd_lcd.c`）。

LTDC + GUIX エピック（#48）の **Phase 1**（#52、単層 RGB565・静的表示）に続く **Phase 2**（#53）で **DMA2D 描画 + tear-free ダブルバッファ**を追加した（下記「ダブルバッファ + DMA2D + tear-free」節）。`lcd` コマンドで単色塗り・カラーバー・グラデ・アニメ・blit を出し、**配線 / RGB チャネル順 / ピクセルクロック / DMA2D / tear-free swap** を実機検証する。タッチ（#54）、GUIX（#55/#56）は後続。タッチはここでは未実装。

## 構成

| 項目 | 値 | 根拠 |
|------|----|------|
| パネル | RK043FN48H-CT（480×272、24-bit RGB、容量タッチ） | UM1907 / rk043fn48h.h |
| ピクセルフォーマット | **RGB565**（16 bpp） | LTDC layer 0 |
| フレームバッファ | 480×272×2 × **2 面** = **522240 B（~510 KB）**、`.sdram` 配置 | non-cacheable, double-buffer |
| LCD_CLK | **4.8 MHz**（PLLSAI、#59 で 9.6 MHz から低減・**out-of-spec**） | 下記導出 |
| フレームレート | 566×286 clk @ 4.8 MHz ≈ **29.6 Hz**（~33.7 ms/frame） | 下記タイミング |
| タイミング（spec） | HSYNC=41 / HBP=13 / HFP=32 / VSYNC=10 / VBP=2 / VFP=2 | rk043fn48h.h |
| 極性 | HS / VS / DE = **active-low**、PC = **IPC**（非反転） | ST BSP / RM0385 §18.7.5 |
| 描画 | **DMA2D**（R2M fill / M2M blit、小転送はポーリング・大転送は割込み完了 #64） | RM0385 §9 |
| 割込み | **reload-ready**（`LTDC_IT_RR`、prio 9）+ **DMA2D 完了**（`DMA2D_IRQn`、prio 10、#64）。underrun/transfer error はポーリング | RM0385 §18.7.9 / §9 |

LTDC が register に積むのは spec から **各 −1** した値（`HorizontalSync=40 / VerticalSync=9 / AccumulatedHBP=53 / AccumulatedVBP=11 / AccumulatedActiveW=533 / AccumulatedActiveH=283 / TotalWidth=565 / TotalHeigh=285`）。

## クロック: PLLSAI → LCD_CLK 4.8 MHz（#59、out-of-spec）

LTDC のピクセルクロックは、それまで未使用だった **PLLSAI** から作る。PLLSAI は**メイン PLL と入力分周 M（=25）を共有**する（RM0385 §5.3.8）ので:

```
VCO_in   = HSE / M       = 25 MHz / 25 = 1 MHz
PLLSAI   VCO = VCO_in × PLLSAIN(192) = 192 MHz      (100..432 MHz 内, RM0385 §5.3.24)
PLLLCDCLK = VCO / PLLSAIR(5)          = 38.4 MHz
LCD_CLK   = PLLLCDCLK / PLLSAIDIVR(8) = 4.8 MHz      (RM0385 §5.3.25 RCC_DCKCFGR1)
```

PLLSAI は**メイン PLL とは別系統**なので、`HAL_RCCEx_PeriphCLKConfig(RCC_PERIPHCLK_LTDC)` を投入しても **SYSCLK（216 MHz）/ FMC SDRAM クロック（108 MHz）には影響しない**。

!!! warning "4.8 MHz は意図的な out-of-spec（#59）"
    元は **9.6 MHz**（`PLLSAIN=192`）だった。**#59** で LTDC の連続 SDRAM リード圧と、それが招くカメラプレビュー時の DCMI DMA FIFO error を減らすため **4.8 MHz（~29.6 Hz）へ低減**した。ただし 4.8 MHz では **線周期 566 clk = 118 µs**（RK043FN48H の 55–65 µs spec 超：active 480 px だけで 100 µs）かつ **~29.6 Hz は ~50 Hz の下限未満**で、いずれも **パネル spec 外**。実機検証（画像安定・同期乱れ無し・underrun=0・フリッカ無し）を通した上での確信犯的な動作点であり、artifact が出る場合の **in-spec fallback は ~8.8 MHz / ~54 Hz（`PLLSAIN=176`）**。LTDC が読むのは active 画素のみなので帯域はリフレッシュにのみ比例し、in-spec ではこの程度（~54 Hz）までしか下げられない。

!!! note "tx_application_define から呼べる理由（HAL tick）"
    HAL の PLLSAI lock 待ちは `HAL_GetTick()` ベース（100 ms タイムアウト）。本プロジェクトの `SysTick_Handler` は ThreadX タイマ稼働の有無に関係なく**常に `HAL_IncTick()` を呼ぶ**（`port/threadx/tx_glue.c`）ので、スケジューラ起動前の `tx_application_define` 内でも tick は進み、lock 失敗時もタイムアウトで抜ける。`sdram_init()` が TIM2 busy-wait を使うのは ThreadX タイマ tick が別物だからで、HAL tick は別系統。

## ピン

LTDC ピンは **PG12 のみ AF9**、残りは **AF14**（UM1907 / ST BSP）:

`PE4 / PG12(AF9) / PI9,10,14,15 / PJ0-11,13-15 / PK0,1,2,4,5,6,7`

加えて手動駆動の GPIO 出力 2 本: **LCD_DISP = PI12**、**LCD_BL_CTRL = PK3**（表示有効化 + バックライト）。`ltdc_init()` の最後にまとめて assert する。既存ペリフェラル（VCP=PA9/PB7、LED=PI1、カメラ DCMI/I2C1、SDMMC1、QSPI、FMC SDRAM）とピン衝突しない（PI9-15 は LED の PI1 と別ピン）。

## フレームバッファ: `.sdram`（NOLOAD）+ MPU non-cacheable

フレームバッファはカメラフレームと同じ **`.sdram`（NOLOAD）セクション**に置く。`bsp_init()` が MPU region 0 で 8 MB 全域を **Normal・non-cacheable** に再マップ済みなので、**LTDC のリード DMA と CPU の書込はコヒーレント**（キャッシュ maintenance 不要）。詳細は [SDRAM](sdram.md)。

```c
static uint16_t ltdc_fb[2][480 * 272] __attribute__((aligned(32), section(".sdram")));
```

ダブルバッファ化（#53）で **2 面 = ~510 KB** を使う。`.sdram` 配置の現況（リセット非生存）: `cam_frame` 150 KB + `cam_ring[4]` 600 KB + `ltdc_fb[2]` ~510 KB ≈ **1.26 MB / 8 MB**。DMA2D も AHB マスタとしてこの領域を読み書きするが、MPU non-cacheable なので CPU / LTDC とコヒーレント。

## 初期化フロー

`ltdc_init()`（`tx_application_define` から、**`sdram_init()` が成功した場合のみ**）:

0. **SDRAM ガード**: `sdram_is_up()` でなければ `LTDC_ERR_STATE` で即 return。フレームバッファは `.sdram`（0xC0000000）にあり、FMC 未初期化で触ると fault するため、呼び出し側でもゲートする二重防御。
1. **ThreadX オブジェクト生成**: reload セマフォ + `ltdc_lock` mutex（`tx_application_define` での生成は正規。run-time の割込みは使わない）
2. PLLSAI → LCD_CLK 4.8 MHz（`HAL_RCCEx_PeriphCLKConfig`、#59）
3. LTDC クロック + GPIO（AF + DISP/BL 出力、この時点では消灯）
4. **2 面とも黒クリア**（`.sdram` は NOLOAD = 不定値。ConfigLayer / バックライト ON より前に消すことで起動時・初回 flip のゴミ表示を防ぐ）。`ltdc_front=0`
5. `HAL_LTDC_Init`（上記タイミング / 極性 / 背景黒）
6. `HAL_LTDC_ConfigLayer`（layer 0、RGB565、480×272、FB = `ltdc_fb[0]`）
7. **reload-ready 割込み**を NVIC で有効化（prio 9）。`HAL_LTDC_Reload()` が arm するまで発火しない
8. **最後に** LCD_DISP / LCD_BL_CTRL を assert（表示 + バックライト ON）

ポーリング中心（オブジェクト生成と NVIC 設定のみ）。idempotent・fail-soft（失敗時は DISP/BL off + `HAL_LTDC_DeInit` + オブジェクト削除で cleanup し `lcd` コマンドが報告、他は継続）。

## ダブルバッファ + DMA2D + tear-free（#53）

Phase 2（#53）で **DMA2D 描画**と **tear-free ダブルバッファ**を追加した。

### 2 面フレームバッファ

RGB565 のフレームバッファを **2 面**持つ（`ltdc_fb[2][480*272]`、ともに `.sdram` 配置 = 計 **~510 KB**）。描画は常に **back（非表示）面**に行い、`ltdc_flip()` で **front（表示）面**へ tear-free に切り替える。

```c
static uint16_t ltdc_fb[2][480 * 272] __attribute__((aligned(32), section(".sdram")));
```

- `ltdc_framebuffer()` … 現在表示中の front 面（**read-only**。直描画はしない）
- `ltdc_back_buffer()` … 描画対象の back 面（`ltdc_lock_frame()` 保持中のみ有効）
- `ltdc_lock_frame()` / `ltdc_unlock_frame()` … `ltdc_lock`（ThreadX mutex、同一スレッド再帰取得可）で draw→flip を直列化

### DMA2D（Chrom-ART）

描画は内蔵 **DMA2D** で加速する。AHB マスタとして CPU・LTDC リード DMA と**同じ MPU non-cacheable な `.sdram`** を読み書きするので、三者はキャッシュ maintenance 不要でコヒーレント。

- **R2M 単色 fill** … `ltdc_fill` / `ltdc_fill_rect` / `ltdc_colorbar`（各バーを 1 fill）
- **M2M blit** … `ltdc_blit`（密パック RGB565 ビットマップを back へコピー）/ `ltdc_blit_demo`（back の左半分を右半分へストライド付き自己コピー）
- グラデ（`ltdc_gradient`）は列ごとに色が変わるので CPU 直書き

各 op は ST BSP `LL_FillBuffer` / `LL_ConvertLineToARGB8888` と同じ「op ごとに `HAL_DMA2D_Init` → `ConfigLayer` → Start → 完了待ち」イディオム。完了待ちは転送サイズで切替える（#64）: **小転送**（`w*h < LTDC_DMA2D_IT_MIN_PIXELS = 16384px`）は従来どおり `PollForTransfer(30ms)` で spin、**大転送**（カメラ 320×240 プレビューや全画面 480×272 等）は `HAL_DMA2D_Start_IT` + `DMA2D_IRQn`（prio 10）の完了割込み → セマフォで **block**（待ちスレッドの CPU を解放／下位スレッドへ明け渡し）。DMA2D は単一エンジンで全 op が `ltdc_lock` 下で直列化されるので in-flight は常に高々 1 つ。完了 callback 未実行の timeout 時は `HAL_DMA2D_Abort` で engine idle + HAL ハンドル unlock を保証する。閾値は実機の `thread` CPU% で調整可能。

### tear-free フリップ（VBR authoritative + fail-closed）

`ltdc_flip()`（`ltdc_lock` 下）:

1. back 面アドレスを `HAL_LTDC_SetAddress_NoReload()` で **CFBAR にステージ**（即時には反映しない）
2. `HAL_LTDC_Reload(LTDC_RELOAD_VERTICAL_BLANKING)` で**次の垂直ブランキングでの register reload を要求**（同時に reload-ready 割込み `LTDC_IT_RR` が arm される）
3. reload-ready 割込み（`LTDC_IRQHandler` → `HAL_LTDC_ReloadEventCallback` → `ltdc_reload_sem`）を起床ヒントに待つ（最大 100 ms）
4. **真値判定**: HW reload は `LTDC->SRCR.VBR` を**コミット後にのみ HW が clear**する（RM0385 §18.7.6）。割込みは起床ヒントに過ぎず、**VBR==0 を authoritative**として最大 ~100 ms ポーリングで確認する
   - VBR==0 → `ltdc_front` を back に切替（swap 確定）、`LTDC_OK`
   - 上限到達でも VBR==1 → **fail-closed**: front は変えず `ltdc_fault` をラッチ（`ltdc_is_up()` が false に）し `LTDC_ERR_HAL`。復帰は system reset 前提

LTDC のフレーム周期は 566×286 clk @ 4.8 MHz ≈ **29.6 Hz**（1 フレーム ~33.7 ms、#59）なので、100 ms は数フレーム分の余裕。割込み優先度は **9**（DCMI/DMA2=8 より下、SD=6-7 / USART=5 と非衝突）。

## `lcd` シェルコマンド

```
lcd info            パネル / クロック / フレームバッファ / バッファ / DMA2D / LTDC エラーフラグ
lcd fill <color>    全面塗り（色名 black/blue/green/cyan/red/magenta/yellow/white または 0xRGB565）
lcd bar             8 縦カラーバー（RGB 配線・ビット順検証）
lcd grad            横グラデ（黒→白、ピクセルクロック検証）
lcd clear           黒
lcd anim            跳ね回る矩形（tear-free ダブルバッファのデモ。Ctrl+C で停止）
lcd blit            DMA2D M2M デモ（カラーバーの左半分を右半分へコピー）
lcd on | lcd off    表示全体の ON/OFF（バックライト + LTDC スキャンアウト）。`off` は SDRAM 帯域も空ける（#66、下記）
```

描画系コマンド（fill/bar/grad/clear/anim/blit）は **back に描画してから `ltdc_flip()` で present**する（`ltdc_lock_frame()`/`ltdc_unlock_frame()` で 1 フレームを atomic に）。`lcd anim` は `ltdc_flip()` が VSYNC reload まで block するので**自然にフレームレートで律速**され、追加 sleep 不要（毎フレーム Ctrl+C を polling）。

### `lcd off` / `lcd on`（LTDC スキャンアウト停止/再開、#66）

`lcd off` は **表示全体**を OFF する — バックライト**と** LTDC スキャンアウト。スキャンアウトを落とすと `LTDC_GCR.LTDCEN` がクリアされコントローラの **SDRAM フレームバッファリードが停止**する（スキャンしないパネルはゴミ表示になるのでバックライトも park）。`lcd on` で両方を再開する（layer/timing/front 面はそのまま）。つまり `lcd off` は LTDC の連続リードが消費する **SDRAM 帯域を空ける**手段でもある（例: 30fps の `camera` キャプチャ、#67）。スキャンアウト側は best-effort で、GUIX 所有中（`gui` 稼働中）と faulted/未初期化時はスキップ。停止中は flip/描画も拒否（VBR 待ちで fault しないため）。

用途は **帯域/電力制御**と **#59 の FE 主因の切り分け計測**。実機計測（plain stream, QVGA, 4.8MHz, 14.9fps）:

| | dma fe/s |
|---|---|
| LTDC ON | 1297 |
| **LTDC OFF**（`lcd off`） | **0** |

→ **DCMI streaming の FE は 100% が LTDC 連続リードとの SDRAM 競合**で、スキャンアウトを止めると **完全にゼロ**。DCMI→SDRAM 書込み単独（リフレッシュ/行管理込み）では FE は出ない＝**床は無い**。よって残 FE を 0 近傍へ追い込むには **DCMI を優先する FMC/AXI アービトレーション**が原理的に有効（#59 で deferred）。

`lcd info` の **errors 行**は `LTDC->ISR` の FIFO underrun / transfer error フラグ（RM0385 §18.7.9）を表示する — **underrun=YES は SDRAM 帯域不足の証拠**になる。`state` 行は `up` / `disabled (scanout off)` / `DOWN` を表示する。

`lcd info` の **errors 行**は `LTDC->ISR` の FIFO underrun / transfer error フラグ（RM0385 §18.7.9）を表示する — **underrun=YES は SDRAM 帯域不足の証拠**になる。

実行例:

```
sh> lcd info
panel:   RK043FN48H-CT 480x272 RGB565 (LTDC layer 0)
clock:   LCD_CLK 4.80 MHz (PLLSAI N=192 R=5, DIVR/8)
fb:      0xc00bb800 (.sdram, non-cacheable)
buffers: 2 (double, tear-free VBR)
front:   0
DMA2D:   on
state:   up
errors:  underrun=no transfer=no
sh> lcd bar
sh> lcd anim
^C
sh> lcd fill red
```

## 帯域

LTDC は active 期間に毎フレーム **front 面のみ**を連続リードする（ダブルバッファでも LTDC のリード帯域は 1 面分）。RGB565・**4.8 MHz** ピクセルクロック（#59）で上限 **4.8 Mpix/s × 2 B = 9.6 MB/s**（active fetch のみなら実効 ~7.8 MB/s）。SDRAM の理論帯域（16-bit @ 108 MHz = 216 MB/s）に対し約 4.4%。DMA2D の fill/blit は back 面への短いバースト書込（1 フレーム描画あたり最大 ~510 KB）で LTDC リードと時間的にも競合は限定的。小転送は `PollForTransfer` 完了まで CPU が spin し、大転送は完了割込みで待ちスレッドが block する（#64）が、いずれも DMA2D engine の SDRAM 帯域占有（FE 競合）は同じ＝IT 化は CPU/電力最適化であって帯域競合とは独立。DCMI 書込（QVGA ≈ 1.65 MB/s）や CPU と競合しても余裕がある（実機で `lcd anim`/`blit`、`camera capture`/`stream` 回帰、underrun=no を確認すること）。

!!! note "SDRAM 帯域競合とカメラプレビュー（#59）"
    LTDC の連続リードは**平均**帯域では 6% 程度だが、**瞬間的なバースト**が DCMI DMA の 16 byte FIFO とアービトレーションで競合し、カメラプレビュー時に DCMI DMA FIFO error (FE) を誘発する。#59 はこれを 2 本の lever で緩和した: **(A)** 本節の LCD_CLK を 9.6→4.8 MHz に下げて LTDC の瞬間 SDRAM 占有を低減、**(B)** プレビューの DMA2D ラウンドトリップ削減（[フレームパイプライン](../architecture/frame-pipeline.md) の帯域予算表を参照）。`camera stream stats` の **`dma fe/s`** と `lcd info` の **underrun** が定量指標。より高い解像度/fps では再評価する。

## 参照

- UM1907 — RK043FN48H-CT LCD（LTDC RGB 配線、LCD_DISP/LCD_BL_CTRL）
- RM0385 §18 — LTDC、§5（RCC / PLLSAI）
- rk043fn48h.h — パネルタイミング定数（解像度 / HSYNC / porch / 分周）
- `_ref/STM32Cube_FW_F7_V1.17.0/Drivers/BSP/STM32746G-Discovery/stm32746g_discovery_lcd.c` — LTDC / PLLSAI / GPIO 初期化の参照実装（read-only）
