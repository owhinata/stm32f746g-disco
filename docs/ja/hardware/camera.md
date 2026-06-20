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
camera info              ドライバ / センサ状態 + 現在のモード / 画質設定を表示
camera res <解像度>      解像度切替 qqvga|qvga|480x272|vga|wvga（#45）
camera format <fmt>      ピクセルフォーマット切替 rgb565|yuv422|y8|jpeg（#45）
camera fps <15|30>       フレームレート切替 15(PCLK24M) / 30(PCLK48M)。30 は lcd disable 必須（#67）
camera capture [test]    現在モードを 1 フレーム snapshot（test = colorbar パターン）
camera save <sd|fs> <p>  キャプチャ済みフレームを raw（モードのフォーマット）で保存
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

`camera save <sd|fs> <path>` は、キャプチャ済みフレームを**現在モードのフォーマットの raw のまま**（RGB565/YUV422/Y8 raster、または JPEG ストリーム）選択した媒体（`sd` = microSD FAT32、`fs` = QSPI NOR）へ書き出す。固定チャンク（512 B、shell の 2 KB スタックに収める）で `camera_frame_read` → FileX write のストリーム書きなので追加バッファ不要。有効長は `camera_get_info` の `frame_bytes`（JPEG は EOI まで切り詰めた可変長）。fs/sd コマンドと同じ **shared op slot** 下で動くため、保存中に `sd format`/`umount` が媒体を奪うことはない。Ctrl+C で中断可（部分ファイルが残る旨を表示）。フォーマット別の PC 変換は下記「解像度 / ピクセルフォーマット / フレームレート（#45）」節を参照。

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

## 解像度 / ピクセルフォーマット / フレームレート（#45）

`camera res` / `camera format` でキャプチャモードを切り替える。ドライバ内部の唯一の真実 `struct camera_mode`（`port/camera/camera.c`）が geometry / format / timing を保持し、`camera_set_format()` が OV5640 を再プログラム（解像度・フォーマットスケーラ + per-mode の HTS/VTS/PCLK fps 表）して反映する。`camera info` に現在の `WxH`・フォーマット・目標 fps・stream 可否を表示。

| 軸 | 値 |
|----|----|
| 解像度 | `qqvga`(160x120) / `qvga`(320x240) / `480x272` / `vga`(640x480) / `wvga`(800x480) |
| フォーマット | `rgb565`(2B/px) / `yuv422`(2B/px, YUYV) / `y8`(1B/px, グレースケール) / `jpeg`(可変長) |

### snapshot と stream の非対称（HW 制約由来）

- **snapshot は全モード可**: VGA/WVGA は 1 フレームが DMA NDTR 上限（65535 words）を超えるが、`HAL_DCMI_Start_DMA` が単一連続バッファへ **intra-frame DBM banding**（VGA=4×38400w, WVGA=4×48000w）して取り込む。FRAME はバンド最終で 1 回発火。
- **stream は frame_words ≤ 65535 のモードのみ**（QQVGA/QVGA/480x272 の RGB565・YUV422 等）。producer の手動 DBM は各 M-reg が full-slot を NDTR で指すため、これを超えるモードは `camera info` で `capture only` と表示し `camera stream start` を拒否する。
- **JPEG は snapshot-only・解像度 ≤ VGA**。可変長リング管理は別系統のため stream には載せない。

### フレームレート（per-mode HTS/VTS + fps 切替 15/30、#45/#67）

OV5640 の `lib/ov5640` 共通テーブルは HTS=1936/VTS=1088 を**全解像度共通**・PCLK 24MHz 固定で設定するため既定は **~11fps**。`camera res` で適用される **timing 表**（`mode_fps[]`）は、stream 対象の小モードに HTS/VTS 縮小（1600/1000 → **~15fps**、実機 14.9fps）を、snapshot 専用の VGA/WVGA に従来のタイミング（1936/1088, ~11fps）を与える。`fps = PCLK/(HTS×VTS)`。

**`camera fps <15|30>` で PCLK を 24/48MHz 切替（#67）**。小モード（QQVGA/QVGA/480x272）は HTS/VTS=1600/1000 据え置きなので `48e6/(1600×1000)=30.0fps`。既定は **15fps（安全側）**。`camera fps` は `camera res/format` と同様に即時センサ再適用し、stream/プレビュー所有中は拒否（live DMA target のセンサ PLL を触らない）。

!!! danger "30fps（48MHz）は LTDC scanout 停止が前提"
    48MHz は peak DCMI バーストレートを倍化し、LTDC がフレームバッファを連続リードする 16-bit SDRAM 競合下では **最小の QQVGA ですら数百 ms で DCMI OVR**（`camera stream stats` が `stopped (overrun)`）。実測では **`lcd disable`（LTDC scanout OFF, #66）なら** qqvga/qvga/480x272 RGB565 すべて ~30fps・OVR 0・FE 0（480x272=7.8MB/s でも完全クリーン）。

    そこで実効 PCLK は**単一述語**で決まる: **「fps 30 選択 ∧ 小モード ∧ `ltdc_scanout_active()`==false」が全部成立する時だけ 48MHz**、それ以外は **24MHz に自動クランプ**（reject ではなく clamp）。これを「センサ PCLK を書く全経路」（`camera_set_format` / stream arm 前 / snapshot arm 前）で適用するため、`lcd enable` 中・GUIX プレビュー中は fps 30 を選んでも安全に 15fps へ落ちる。`camera info` の `fps select` 行が選択値と clamp 理由（`lcd scanout active` / `snapshot-only mode`）を表示し、`camera stream start` 時も clamp note を出す。

    **残る制約（TOCTOU）**: 30fps stream を `lcd disable` 下で開始した後に `lcd enable` すると競合が復活して OVR → stream は **graceful auto-stop**（`stopped (overrun)` / `ovr dcmi` に計上）する。30fps stream 中は `lcd enable` しないこと。LTDC scanout を削って「プレビューでも 30fps」を狙う路線は別スコープ（#59/#65）。

!!! warning "VTS と AEC 最大露光のクランプ"
    VTS が AEC 最大露光行数（`0x3A02/03` と `0x3A14/15`、既定 0x3D8=984）を下回ると露光がクリップして暗化/バンディングする。`camera_set_format` は **設定再適用（night mode が AEC を書換える）後**に両系統を read-back し、`VTS ≥ max + margin` を保証する（不足なら VTS を引き上げ、fps は目標を下回る）。fps 表の値は**実機 `camera stream stats` で要調整**の出発点である（帯域競合下の最適化は #59）。

実行例:

```
sh> camera res vga
camera: res = vga
sh> camera format yuv422
camera: format = yuv422
sh> camera info
...
mode:       640x480 yuv422
fps target: 11.4  (capture only -- too large to stream)
sh> camera capture
frame: 640x480 yuv422 (614400 bytes)
Y:  min   3  max 251  mean 120.4
sh> camera save fs /shot.raw
wrote 614400 bytes (640x480 yuv422) to fs:/shot.raw
PC: python3 scripts/yuv422_to_png.py --width 640 --height 480 /shot.raw out.png
```

JPEG は可変長ストリームをそのまま保存する（変換不要、`.jpg` として開ける）:

```
sh> camera format jpeg
sh> camera capture
frame: 320x240 JPEG (12784 bytes)
SOI: ok (FFD8)
EOI: ok (FFD9)
sh> camera save sd /shot.jpg
wrote 12784 bytes (320x240 jpeg) to sd:/shot.jpg
PC: /shot.jpg is a JPEG stream -- open it directly
```

### PC 変換スクリプト

| フォーマット | 変換 |
|------|------|
| RGB565 | `python3 scripts/rgb565_to_png.py --width W --height H in.raw out.png` |
| YUV422 | `python3 scripts/yuv422_to_png.py --width W --height H in.raw out.png` |
| Y8 | `python3 scripts/y8_to_png.py --width W --height H in.raw out.png` |
| JPEG | 変換不要（`.jpg` を直接開く） |

`camera save` / `camera capture` の出力に、現在モードに応じた変換コマンドが表示される。

### SDRAM 予算

最大固定フォーマット WVGA RGB565（768,000 B）を `cam_frame` に連続確保（JPEG budget も流用）、stream リングは 480x272 RGB565 を内包する 256 KB スロット × 4 = 1 MB。`.sdram` 計 2.39 MB / 8 MB。リンカに `ASSERT(_esdram-_ssdram <= 0x800000)` を追加。

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

`start` は即座に戻り、CLI プロンプトを奪わない。取り込みは producer スレッドが回し、`--frames`/`--secs` 到達・`stream stop`・**DMA 転送エラー (TE)** のいずれかで自動停止する。stream と `camera capture` は同一 DCMI/DMA を共有するため**排他**（stream 中の capture は busy）。

**DMA エラーの扱い (#56)**: double-buffer arm（`HAL_DMAEx_MultiBufferStart_IT`）は、snapshot 経路（`HAL_DMA_Start_IT`）が有効化しない **FIFO error 割込み**を有効化する。FIFO threshold/burst が不整合な構成なら FE はストリームを HW disable し得るが、本実装の `FIFO_THRESHOLD_FULL + MBURST_INC4` は整合構成であり、LTDC がフレームバッファを常時リードする SDRAM 競合下で観測される FE（FIFO overrun/underrun）/ DME は**ストリームを停止させない**（snapshot は単に割込みを見ていないだけ）。したがってこれらは**計数して継続**（`stats` の `dma fe`、QVGA で数千/秒オーダー）し、ハードがストリームを実際に停止させる **TE のみを terminal** として扱う。DCMI FIFO オーバーラン (`ovr dcmi`) は別系統で、これは本来 0 近傍。**#59** はこのプレビュー時 FE を緩和した（LCD_CLK 9.6→4.8MHz + DMA2D copy-forward 全廃、指標は `stats` の `dma fe/s`。詳細は [GUIX](../rtos/guix.md) / [display](display.md)）。

### GUIX ライブプレビュー（#56）

`gui camera on` はこの streaming パイプラインに GUIX の push sink を attach し、QVGA を **等倍のまま** LTDC 画面（GUIX）に表示する。プレビュー開始時に **QVGA RGB565 を強制**（`camera_preview_start` が同一 lock 下で `camera_set_format_locked(QVGA,RGB565)` を呼ぶ）するため、直前の `camera res/format` に依らず #56 の sink（QVGA RGB565 専用）と一致する（プレビュー後はモードが QVGA RGB565 に変わる）。プレビュー所有中は `camera stream start/stop` と `camera res/format` が拒否される。詳細は [GUIX のカメラライブプレビュー節](../rtos/guix.md) を参照。

## 参照

- UM2779 — B-CAMS-OMV ユーザマニュアル（CN5 ピンアウト、MB1379/X1 クリスタル、JP1）
- UM1907 §7.2 — P1 カメラコネクタピンアウト
- RM0385 §30 — I2C（TIMINGR）
- `_ref/STM32Cube_FW_H7_V1.13.0/Drivers/BSP/STM32H747I-DISCO/stm32h747i_discovery_camera.c` — OV5640 の電源シーケンス / 極性の参照実装
