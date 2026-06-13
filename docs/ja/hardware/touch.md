# タッチ（FT5336 容量タッチ、I2C3）

ボード搭載の 4.3″ RK043FN48H パネルには **FocalTech FT5336** 容量タッチコントローラが載っている。ドライバは `port/touch/touch.{c,h}`、シェルコマンドは `touch`（`shell/cmds/cmd_touch.c`）。

LTDC + GUIX エピック（#48）の **#54**。ディスプレイ立ち上げ（[LCD](display.md)、#52）の後続フェーズ。本フェーズは **ポーリング** による複数点タッチ（最大 5 点）まで。`touch` コマンドでチップ ID をプローブし、アクティブな点を読み出して、配線と X/Y マッピングを実機検証する。EXTI 割込み駆動と GUIX 入力は #55 で後続。

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
| 方式 | **ポーリング**（INT ピン PI13 は未使用） | 本ドライバ |

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
- `touch_init()` は `tx_application_define()`（`src/main.c`）から一度だけ実行され、**I2C I/O を一切行わない**（GPIO/I2C3 セットアップとミューテックスのみ）ので、スケジューラ開始前でも安全。最初のバストランザクションは `touch probe` / `touch read` で遅延実行される。

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

## 割込みライン（将来）

FT5336 の INT 出力は **PI13**（`EXTI15_10`）に配線されている。本ポーリングドライバでは **未使用**。EXTI 割込み駆動のタッチディスパッチは GUIX（#55）で、ピンごとの EXTI ディスパッチャを追加して導入する。

!!! note "EXTI ライン共有（SD_DETECT）"
    microSD カード検出（SD_DETECT = **PC13**）とタッチ INT（**PI13**）はどちらも EXTI ライン 13（`EXTI15_10_IRQn`）に乗る。SD_DETECT は現状 **ポーリング**で EXTI 駆動ではないため今は競合しないが、将来の EXTI13 ハンドラは発生源ピンで振り分ける必要がある。

## 参照

- RM0385 §34（I2C）
- UM1907（ボードユーザマニュアル、DISCOVERY I2C / タッチ配線）
- ft5336.h（FocalTech FT5336 レジスタマップ、ST BSP コンポーネントドライバ）
- stm32746g_discovery_ts.c（TS_SWAP_XY と座標組み立て）
