# カメラ（B-CAMS-OMV / OV5640）

ST のカメラモジュールバンドル **B-CAMS-OMV**（アダプタ MB1683 + カメラモジュール MB1379、センサ **OV5640** 5MP）をボードの **P1**（30-pin ZIF）に接続して使う。ドライバは `port/camera/camera.{c,h}`、シェルコマンドは `camera`（`shell/cmds/cmd_camera.c`）。

カメラ基盤（Epic #22）: Phase 1（#39）= センサ検出（PWR_EN + I2C1/SCCB chip ID）、Phase 2（#41）= **DCMI + DMA2 で QVGA RGB565 を 1 フレーム SDRAM へ snapshot**（`camera capture`）、Phase 3（#42）= **フレームの raw 保存 + PC で PNG 変換**（`camera save` + `scripts/rgb565_to_png.py`）。

## 配線（P1 ↔ B-CAMS-OMV CN5）

ボードの P1（UM1907 §7.2 Table 5）と B-CAMS-OMV の 30-pin ZIF CN5（UM2779 Table 7）は、付属 FFC で**全信号 1:1 対応**（コネクタのピン番号は互いに逆順）。

| 信号 | MCU ピン | 備考 |
|------|----------|------|
| I2C1_SCL（SCCB） | PB8 | AF4、open-drain。CN2 拡張コネクタと共用バス |
| I2C1_SDA（SCCB） | PB9 | AF4、open-drain |
| DCMI_PWR_EN | PH13 | **Low = 電源 ON**（モジュールの POWER_DOWN、active-high） |
| DCMI_NRST | -    | ボードの **NRST 網に直結**（GPIO 制御不可） |
| DCMI_HSYNC | PA4 | AF13 |
| DCMI_PIXCLK | PA6 | AF13 |
| DCMI_VSYNC | PG9 | AF13 |
| DCMI D0-D4 | PH9-PH12, PH14 | AF13 |
| DCMI D5 / D6 / D7 | PD3 / PE5 / PE6 | AF13 |

既存ペリフェラル（USART1=PA9/PB7、QSPI=PB2/PB6/PD11-13/PE2、SDMMC1=PC8-13/PD2、LED=PI1）と競合しない。

!!! note "クロック（XCLK）"
    MB1379 モジュールは**自前の 24 MHz クリスタル**（MB1379/X1）で OV5640 を駆動する（UM2779 §3.2）。ホスト側の MCO 供給は**不要**。P1 pin21 の Camera_CLK（ボード X1 由来の OSC_24M）は接続されるが使われない。

## I2C（SCCB）

| 項目 | 値 | 根拠 |
|------|----|------|
| インスタンス | **I2C1**（PB8/PB9） | UM1907 CN2 / P1 |
| アドレス | **0x78**（8-bit write） | OV5640 / H747I BSP |
| レジスタアドレス | **16-bit**（`I2C_MEMADD_SIZE_16BIT`） | OV5640 datasheet |
| chip ID | 0x300A/0x300B = **0x5640** | OV5640 datasheet |
| 速度 | ~100 kHz standard mode | SCCB |

TIMINGR は **PCLK1 = 54 MHz** 用に再計算した値を使う（RM0385 §30.4.10）: PRESC=11 / SCLL=24 / SCLH=19 / SCLDEL=5 / SDADEL=2 → SCL ≈ 99 kHz。ST BSP の定数 `0x40912732` は APB1=50 MHz 前提のため流用しない（54 MHz では ~118 kHz になる）。

## 電源とリセット

- PWR_EN（PH13）は `camera_init()` で**出力に切り替える前に OFF レベルを書き**、モジュールへのグリッチを防ぐ
- probe は毎回 **電源サイクル**（High 100 ms → Low → 20 ms 整定、H747I BSP の HwReset と同タイミング）から始める
- DCMI_NRST は GPIO で制御できないため、**PWR_EN サイクル + OV5640 ソフトリセット**（`OV5640_ReadID` が 0x3008=0x80 を書いて 500 ms 待つ）で代替する

## OV5640 component driver（submodule）

センサのレジスタ制御は ST 公式の **OV5640 BSP component driver** を使う:

- submodule: `lib/ov5640` = [STMicroelectronics/stm32-ov5640](https://github.com/STMicroelectronics/stm32-ov5640) **v4.0.3**（BSD-3-Clause、`NOTICE` 参照）
- BSP v2 API（`OV5640_Object_t` + `OV5640_IO_t`）。純 I2C で自己完結（DCMIPP 等への依存なし）
- バスグルーは `port/camera/camera.c` の `cam_io_*`（`HAL_I2C_Mem_Write/Read`）。`OV5640_ReadID()` が `IO.Init()` を無条件に呼ぶため、**no-op の Init/DeInit スタブを必ず登録**する

!!! note "stm32-mw-camera を使わない理由"
    ST の Camera Middleware（`stm32-mw-camera`）と X-CUBE-ISP は **DCMIPP**（STM32N6 系）専用で、F7 の classic DCMI では使えない。単体公開されている OV5640 component driver のみを取り込む。

## キャプチャパス（DCMI + DMA2、#41）

| 項目 | 値 | 根拠 |
|------|----|------|
| 解像度 / 形式 | **QVGA 320x240 RGB565**（153,600 B、little-endian） | `OV5640_Init(R320x240, RGB565)` |
| DCMI | 8-bit パラレル、HW 同期、**HSYNC=HIGH / VSYNC=HIGH / PCLK=RISING** | H747I-DISCO BSP の OV5640 実績値（`OV5640_Init` がセンサ側極性も設定） |
| DMA | **DMA2 Stream1 / Ch1**（RM0385 Table 26。SD は Stream3/6 で競合なし）、`DMA_NORMAL` 単発、word、FIFO full + MBURST INC4 | 38,400 word ≤ NDTR 上限 65,535 → 単発転送 |
| フレームバッファ | `.sdram` セクション（SDRAM 8MB、**MPU non-cacheable** → キャッシュ maintenance 不要） | [SDRAM](sdram.md) 参照 |
| NVIC | DCMI=8、DMA2_Stream1=8（USART1=5 / SDMMC=6 / SD-DMA=7 の下位、SysTick=14 の上位） | ThreadX は PRIMASK マスクなので任意優先度から `tx_semaphore_put` 可 |

完了モデル（HAL）: DMA がフレーム分の word を転送し終えると `DCMI_DMAXferCplt` が **FRAME 割込み**を arm → FRAME ISR が `HAL_DCMI_FrameEventCallback` → `tx_semaphore_put`。同期エラー / オーバーラン / DMA エラーはすべて `HAL_DCMI_ErrorCallback` に集約。SD ドライバと同じ **drain + `cam_xfer_active` ゲート + 有限タイムアウト（1 s）** で遅延 callback の誤通知を封じる。

センサ設定は capture 時に遅延実行: 未 probe なら probe → `OV5640_Init`（電源投入毎に 1 回、AEC/AWB 整定 300 ms）→ `OV5640_ColorbarModeConfig`（live / colorbar 切替時 100 ms 整定）→ snapshot。`sdram test` は `.sdram` を上書きするため、開始時に `camera_frame_invalidate()` でフレーム無効化フラグを立てる。

## 排他とスレッド文脈

- 公開 API は内部 ThreadX mutex（TX_INHERIT）で直列化。実体は `*_locked()` ヘルパに分離し、**公開 API が同一 mutex を再取得する経路を構造的に排除**（Phase 2 の capture が probe を内部で呼ぶ際は `_locked` 版を使う）
- **thread context 専用**。ISR から呼んではならない
- 初期化は `tx_application_define()`（`src/main.c`）で 1 回。**センサ I/O は発行しない**（GPIO/I2C1 と mutex 生成のみ）。センサへのアクセスは最初の `camera probe` で遅延実行

## `camera` シェルコマンド

```
camera probe             電源サイクル + OV5640 chip ID 読出し（~1 s）
camera info              ドライバ / センサ状態の表示
camera capture [test]    QVGA RGB565 を 1 フレーム snapshot（test = colorbar パターン）
camera save <sd|fs> <p>  キャプチャ済みフレームを raw RGB565 でファイル保存
camera off               モジュール電源 OFF
```

`camera capture` は取得後に R5/G6/B5 各チャネルの min/max/mean と先頭 16 画素の hexdump を表示する。`camera capture test` は OV5640 内蔵の **8 本縦帯 colorbar**（白/黄/シアン/緑/マゼンタ/赤/青/黒）を取り込むため、**光学系と無関係に** DCMI 極性・タイミング・配線を検証できる（min が 0 付近・max が飽和付近・帯ごとにフラット、が期待値）。live 撮影ではレンズを覆うと mean が下がる。

実行例:

```
sh> camera capture test
camera: capturing colorbar test frame ...
frame: 320x240 RGB565 (153600 bytes)
R5: min  0  max 31  mean 15.4
G6: min  0  max 63  mean 31.2
B5: min  0  max 31  mean 15.5
row0[0..15]:
00000000: ff ff ff ff ff ff ff ff  ff ff ff ff ff ff ff ff
00000010: ff ff ff ff ff ff ff ff  ff ff ff ff ff ff ff ff
sh> camera capture
camera: capturing live frame ...
frame: 320x240 RGB565 (153600 bytes)
...
```

## ファイル保存と PC での画像確認（#42）

`camera save <sd|fs> <path>` は、キャプチャ済みフレームを **raw little-endian RGB565（153,600 B）のまま**選択した媒体（`sd` = microSD FAT32、`fs` = QSPI NOR）へ書き出す。行単位（640 B × 240 行）で `camera_frame_read` → FileX write のストリーム書きなので追加バッファ不要。fs/sd コマンドと同じ **shared op slot** 下で動くため、保存中に `sd format`/`umount` が媒体を奪うことはない。Ctrl+C で中断可（部分ファイルが残る旨を表示）。

PC 側では microSD をカードリーダで読む（または `sd cat` 以外の転送手段）か、そのまま付属スクリプトで PNG に変換する:

```
sh> camera capture test
sh> camera save sd /bar.raw
wrote 153600 bytes (320x240 RGB565 raw) to sd:/bar.raw
PC: python3 scripts/rgb565_to_png.py <file> out.png
```

```console
$ pip install Pillow
$ python3 scripts/rgb565_to_png.py bar.raw bar.png
wrote bar.png (320x240)
```

チャネルは bit 複製で 8-bit へ拡張（5/6-bit のフルスケールが正確に 255 になる）。解像度が異なる場合は `--width/--height` で指定。

## 参照

- UM2779 — B-CAMS-OMV ユーザマニュアル（CN5 ピンアウト、MB1379/X1 クリスタル、JP1）
- UM1907 §7.2 — P1 カメラコネクタピンアウト
- RM0385 §30 — I2C（TIMINGR）
- `_ref/STM32Cube_FW_H7_V1.13.0/Drivers/BSP/STM32H747I-DISCO/stm32h747i_discovery_camera.c` — OV5640 の電源シーケンス / 極性の参照実装
