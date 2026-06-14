# タッチ（FT5336 容量タッチ、I2C3）

ボード搭載の 4.3″ RK043FN48H パネルには **FocalTech FT5336** 容量タッチコントローラが載っている。ドライバは `port/touch/touch.{c,h}`、シェルコマンドは `touch`（`shell/cmds/cmd_touch.c`）。

LTDC + GUIX エピック（#48）の **#54**。ディスプレイ立ち上げ（[LCD](display.md)、#52）の後続フェーズ。`touch` コマンドでチップ ID をプローブし、アクティブな点を読み出して、配線と X/Y マッピングを実機検証する。**#62** で FT5336 INT（PI13）の **EXTI13 割込み駆動化**と I2C の **割込み（IT）化**を導入し、GUIX 入力スレッドは無タッチ時に割込み待ちで idle（CPU ≈ 0 %）になる（下記）。

## 構成

| 項目 | 値 | 根拠 |
|------|----|------|
| コントローラ | FocalTech **FT5336** | UM1907 / ft5336.h |
| バス | **I2C3**（PH7=SCL, PH8=SDA, AF4, オープンドレイン） | UM1907 DISCOVERY I2C |
| アドレス | **0x70**（8-bit） | ft5336.h `FT5336_I2C_SLAVE_ADDRESS` |
| レジスタアドレス | **8-bit**（`I2C_MEMADD_SIZE_8BIT`） | ft5336.h |
| チップ ID | reg `0xA8` = **0x51** | ft5336.h `FT5336_CHIP_ID_REG` / `FT5336_ID_VALUE` |
| 最大点数 | **5** | ft5336.h `FT5336_MAX_DETECTABLE_TOUCH` |
| 座標 | **パネルピクセル**（x 0..479 / y 0..271）、**TS_SWAP_XY** 適用 | stm32746g_discovery_ts.c / 実機確認 |
| 速度 | ~100 kHz 標準モード | RM0385 §34（I2C） |
| 方式 | **EXTI13 割込み駆動 + I2C IT**（INT = PI13、#62） | 本ドライバ |

TIMINGR はカメラの I2C1 と同じ値で、**PCLK1 = 54 MHz** 前提（PRESC=11 / SCLL=24 / SCLH=19 / SCLDEL=5 / SDADEL=2 → SCL ≈ 99 kHz）。ST BSP 定数 `0x40912732` は APB1 = 50 MHz 前提のため意図的に流用しない。

!!! note "カメラとは別バス"
    カメラ（OV5640）は **I2C1**（PB8/PB9）、タッチコントローラは **I2C3**（PH7/PH8）を使う。両者は独立したバスなので `camera` と `touch` で **競合しない**。

## タッチの読み出し

FT5336 レジスタマップ（ft5336.h）:

- **TD_STATUS（0x02）** — 下位ニブル = アクティブなタッチ点数（0..5）。
- 点 `n` ごとに `0x03 + 6·n` から 6 バイト: `XH, XL, YH, YL, WEIGHT, MISC`。
  - `XH`: bit 7..6 = **イベントフラグ**、bit 3..0 = X 座標 MSB。
  - `XL`: X 座標 LSB → `rawx = ((XH & 0x0F) << 8) | XL`（12-bit）。
  - `YH`: bit 7..4 = **タッチ ID タグ**、bit 3..0 = Y 座標 MSB。
  - `YL`: Y 座標 LSB → `rawy = ((YH & 0x0F) << 8) | YL`（12-bit）。

**イベントフラグ** の意味: `0` = press-down（押下）、`1` = lift-up（離し）、`2` = contact（保持）、`3` = no-event。

このパネルは **TS_SWAP_XY** が必要（コントローラのネイティブ軸が LCD に対して入れ替わっている）ので、組み立て座標は入替: `x = rawy`、`y = rawx`（`stm32746g_discovery_ts.c` と同じ）。**このパネルの FT5336 はパネルピクセル座標を直接返す**（x 0..479 / y 0..271、原点は左上）ため **スケーリングは不要**。実機の 4 隅タップで確認済（左上 ≈(8,5) / 右上 (479,5) / 左下 (1,271) / 右下 (479,271) / 中央 ≈(245,135)）。GUIX（#55）側で必要なら校正を持つ。

## ロックとスレッドコンテキスト

- 公開 API は内部 ThreadX ミューテックス（TX_INHERIT）で直列化する。
- **スレッドコンテキスト専用** — ISR から呼んではならない。
- I2C の読み書きは **割込み（IT）駆動**（`HAL_I2C_Mem_Read_IT` / `_Write_IT` + 完了セマフォ、#62）。呼び出しスレッドはトランザクション中 busy-wait せずブロックする。完了 callback（Rx/Tx/Error）は全 I2C ユニット共通の weak シンボルなので **`Instance == I2C3` でフィルタ**する（カメラの I2C1 はブロッキングで踏まない）。同期 abort が無いため、タイムアウト/エラーは `HAL_I2C_DeInit`+`Init` で復旧する。
- `touch_init()` は `tx_application_define()`（`src/main.c`）から一度だけ実行され、**I2C I/O を一切行わない**（GPIO/I2C3 セットアップ・ミューテックス・セマフォ・I2C3 NVIC のみ）ので、スケジューラ開始前でも安全。最初のバストランザクションは `touch probe` / `touch read` / `touch_irq_enable()` で遅延実行される。

## `touch` シェルコマンド

```
touch probe   FT5336 チップ ID を読む（0x51 = 検出）
touch info    バス / アドレス / 状態 / 点数 / 方式
touch read    アクティブなタッチ点を Ctrl+C までポーリング（100 ms 周期）
```

`touch read` は各アクティブ点を `P<id>: x=<x> y=<y> event=<e>` で表示する（`<id>` は FT5336 タッチ ID タグ）。Ctrl+C で停止。

セッション例:

```
sh> touch probe
FT5336 detected: chip ID 0x51
sh> touch read
polling FT5336 (Ctrl+C to stop) ...
P0: x=132 y=210 event=2
P0: x=133 y=211 event=2
^C
```

## EXTI13 割込み駆動 + I2C IT（#62）

GUIX 入力（`port/guix/guix_touch.c`）のポーリングは、無タッチでも 60 Hz で I2C を叩き続け、`HAL_I2C_Mem_Read`（ブロッキング）の busy-wait で **idle でも CPU ≈ 20 %** を消費していた。#62 でこれを 2 段で解消した。

**1. EXTI13 割込み起床。** FT5336 の INT 出力は **PI13**（`EXTI15_10`）に配線されている。`touch_irq_enable()` が PI13 を立ち上がりエッジ EXTI として arm し、FT5336 を**割込み（トリガ）モード**（GMODE reg `0xA4` = `0x01`）にして INT を出させる。順序は ST BSP（`stm32746g_discovery_ts.c`）に倣い **GPIO/NVIC を先に arm → 最後に GMODE 書込**（有効化〜arm の窓でエッジを取りこぼさない）。EXTI ISR は `gx_system_event_send` を呼ばず（GUIX はスレッドコンテキスト限定）、ウェイク用セマフォを post するだけ。

**2. ハイブリッド state machine。** GUIX 入力スレッドは無タッチ時にこのセマフォを `TX_WAIT_FOREVER` で待つ（**CPU ≈ 0 %**）。INT エッジで起床し、**指が触れている間だけ ~60 Hz でポーリング**して DOWN/DRAG/UP を送出する（FT5336 は接触継続中は INT を出さないため、ドラッグ追従にはポーリングが必須）。指が離れたら再びセマフォ待ちに戻る。起床直後の最初のサンプルが FT5336 の `0xFFF/0xFFF`（無効初回値）でも取りこぼさないよう、短いポーリング窓（settle）で実座標を待つ。`gui stop`（park）はウェイクを disarm してスレッドをセマフォ待ちから起こす。

!!! note "EXTI ライン共有（SD_DETECT）— 排他 mux"
    microSD カード検出（SD_DETECT = **PC13**）とタッチ INT（**PI13**）はどちらも EXTI ライン 13（`EXTI15_10_IRQn`）に乗る。`SYSCFG_EXTICR4` はライン 13 に **1 ポートだけ**割り当てる（PC13 と PI13 は**同時利用不可**＝排他 mux）。SD_DETECT は現状 **ポーリング**（`port/sd/sd_card.c`）なので PI13 がライン 13 を占有できる。将来 SD_DETECT を EXTI13 化する場合はこの排他性を解く（片方をポーリング維持する）必要がある。

なお `touch read` シェルコマンドは従来どおり自前の 100 ms 周期でポーリングする（各読み出しは内部で IT 駆動）。

## 参照

- RM0385 §34（I2C）
- UM1907（ボードユーザマニュアル、DISCOVERY I2C / タッチ配線）
- ft5336.h（FocalTech FT5336 レジスタマップ、ST BSP コンポーネントドライバ）
- stm32746g_discovery_ts.c（TS_SWAP_XY と座標組み立て）
