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
camera info              ドライバ / センサ状態 + 現在の画質設定を表示
camera capture [test]    QVGA RGB565 を 1 フレーム snapshot（test = colorbar パターン）
camera save <sd|fs> <p>  キャプチャ済みフレームを raw RGB565 でファイル保存
camera set [<name> <値>] OV5640 画質設定（引数なしで一覧表示）
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

## 画質設定（camera set, #44）

`camera set` で OV5640 内蔵 ISP の画質を調整する（B-CAMS-OMV はモジュール側に LED/AF を持たず、制御できるのはセンサ ISP 設定のみ）。設定は `port/camera` の **RAM キャッシュ**に保持し、センサが live なら即時 I2C 適用、未設定なら次回 capture の遅延 configure で一括適用する（`OV5640_Init` が SDE レジスタ群を書き潰すため、毎回 init 後に再適用が必要）。

| 設定 | 値 | 内容 |
|------|----|------|
| `brightness` | -4..4 | 明るさ |
| `contrast` | -4..4 | コントラスト |
| `saturation` | -4..4 | 彩度 |
| `hue` | -180..150（30° 刻み） | 色相（内部で -6..5 index に変換） |
| `awb` | auto / sunny / office / home / cloudy | ホワイトバランス（光源モード） |
| `effect` | none / bw / sepia / negative / blue / red / green | 特殊効果 |
| `flip` | none / mirror / flip / both | ミラー / 上下反転 |
| `zoom` | 1 / 2 / 4 / 8 | デジタルズーム（ISP スケーリング、QVGA 対応） |
| `night` | on / off | ナイトモード（AEC で 15→3.75fps へ自動延長） |
| `default` | — | 全設定を中立値へリセット |

各設定は `camera set` 配下の**サブコマンド**（`shell/cmds/cmd_camera.c` の `camera_set_subcmds`）なので、階層 help（#37）で一覧・個別 usage が引ける。値を省くと該当サブコマンドの usage が自動表示される。

```
sh> help camera set       # 設定項目を再帰的に一覧
camera set -- OV5640 image quality (no arg = show current)
Subcommands:
  brightness <-4..4>
  contrast   <-4..4>
  saturation <-4..4>
  hue        <-180..150> in 30 deg steps
  awb        <auto|sunny|office|home|cloudy>
  effect     <none|bw|sepia|negative|blue|red|green>
  flip       <none|mirror|flip|both>
  zoom       <1|2|4|8>
  night      <on|off>
  default    reset all settings to neutral
Type 'help camera set <subcommand>' for details.
sh> camera set brightness 2
camera: brightness = 2
sh> camera set brightness         # 値を省くと usage
camera set brightness: invalid number of arguments
usage: camera set brightness  (<-4..4>)
sh> camera set                    # 引数なしで現在値を一覧
brightness: 2
contrast:   0
saturation: 0
hue:        0 deg
awb:        auto
effect:     none
flip:       none
zoom:       x1
night:      off
type 'help camera set' for the list of settings
```

!!! warning "SDE_CTRL0 / SDE_CTRL8 の上書きバグと fixup"
    `lib/ov5640`（read-only submodule）の各 setter は SDE master enable レジスタ **`SDE_CTRL0`(0x5580)** を**自分の enable ビットだけで上書き**するため、ST ドライバそのままでは brightness/contrast/saturation/hue は**最後に適用した 1 系統しか効かない**。さらに sign/UV ビットを持つ **`SDE_CTRL8`(0x5588)** を vendored の `ov5640_modify_reg` で更新するが、これは **OR-only（ビットを立てられるが消せない）**。

    submodule は編集せず、`port/camera` 側で吸収する: setter 群を**固定順で適用**したあと、`SDE_CTRL0` を全有効機能の enable ビットの OR で、`SDE_CTRL8` をキャッシュから導いた**正確な値**でそれぞれ read-modify-write し直す。これにより 4 系統が併用可能になり、hue や負の brightness の sign ビットが残留しない（OV5640 datasheet table 7-26 の bit 定義に基づく）。

    **tint 系 effect（bw/sepia/blue/red/green）は `SDE_CTRL3/4` を fixed U/V で占有**し saturation/hue と register が衝突するため、tint 適用中は saturation/hue を skip する（U/V 固定中はどのみち無効）。

## 連続取り込み（streaming, #46）

`camera capture` の単発 snapshot（`DCMI_MODE_SNAPSHOT` + `DMA_NORMAL`）に対し、`camera stream` は **DCMI continuous + DMA double-buffer(DBM)** で途切れなく取り込む。取り込んだフレームは [フレームパイプライン](../architecture/frame-pipeline.md)（1 ソース→マルチシンク, #47）へ流し、#46 の一次成果は表示非依存の **FPS / オーバーラン計測**。LTDC 表示や連続保存は後続のシンクとして差し込む。

### リングと DBM

`.sdram` に **N=4 面**のリング（`cam_ring[4]`、`cam_frame[]` とは別）を確保し、`frame_pipeline_init` に注入する。DBM は常に 2 面を DMA ターゲット（M0AR/M1AR）にし、1 面が最新 published、1 面が次に acquire 可能（N=4 の根拠）。`HAL_DCMI_Start_DMA` の内部 DBM 分割は `Length>0xFFFF` 時のみ・かつ**フレーム内**分割なので使わず、**`HAL_DMAEx_MultiBufferStart_IT` + `HAL_DMAEx_ChangeMemory`** で **フレーム間** N 面リングを producer が明示制御する。

### スレッド構成（ISR は通知のみ）

| 層 | 役割 |
|---|---|
| DMA TC ISR（`DMA2_Stream1`, prio 8） | `cam_stream_sem` を post するだけ。リング/CT には触れない |
| **producer スレッド（prio 10, 専用）** | `CT` で完了面を特定 → 空きスロット `acquire` → 完了側 M-reg を `HAL_DMAEx_ChangeMemory` で張替え → **然る後 publish**。自動停止（--frames/--secs/OVR）と teardown も担う |
| CLI コマンド（prio 16） | `start`/`stop`/`stats` を発行して即 return。フレーム処理には関与しない |

**tear-free 不変条件**: 「acquire → repoint → publish」の順により、シンクに渡した面は決して live DMA ターゲットにならない。空きスロットが無ければ publish せず drop（`ring_ovr`）。

### コマンド（非ブロック）

```
camera stream start [test] [--frames N] [--secs S]   即 return（裏で取り込み開始）
camera stream stop                                    停止
camera stream stats                                   FPS / frames / overrun
```

`start` は即座に戻り、CLI プロンプトを奪わない。取り込みは producer スレッドが回し、`--frames`/`--secs` 到達・`stream stop`・DCMI OVR のいずれかで自動停止する。**OVR は terminal**: continuous の DCMI オーバーラン時に HAL が DMA を abort するため、計数して停止し `stats` に出す（QVGA の FMC 帯域では本来 0 近傍）。stream と `camera capture` は同一 DCMI/DMA を共有するため**排他**（stream 中の capture は busy）。

## 参照

- UM2779 — B-CAMS-OMV ユーザマニュアル（CN5 ピンアウト、MB1379/X1 クリスタル、JP1）
- UM1907 §7.2 — P1 カメラコネクタピンアウト
- RM0385 §30 — I2C（TIMINGR）
- `_ref/STM32Cube_FW_H7_V1.13.0/Drivers/BSP/STM32H747I-DISCO/stm32h747i_discovery_camera.c` — OV5640 の電源シーケンス / 極性の参照実装
