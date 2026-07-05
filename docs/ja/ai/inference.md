# オンデバイス NN 推論

STM32F746G-DISCO 上で**ニューラルネット推論**を動かすための、backend 非依存の
`nn` 抽象層と `ai` シェルコマンド（Issue #81 / Epic #80）。第一ユースケースは
**カメラ顔検出**（OV5640/DCMI → 推論）。

!!! note "Phase 状況"
    **P1 完了** — null backend 土台 + X-CUBE-AI(`stedgeai`) backend + BlazeFace-128
    顔検出が実機で動作（`ai run`/`ai stream` が顔 bbox を出力、~1.5fps）。
    **P4 完了** — GUIX ライブプレビュー上に顔 bbox を描画（`gui overlay on`、#83）。
    **P3 完了** — TFLM(tflite-micro) backend。M1 spike（#86）で C++ 成立性を実証後、
    **M2（#88）で BlazeFace-front 128 int8 を本統合**。同じ `.tflite` を stedgeai と
    TFLM 双方が実行し、`ai info`/`ai bench`/`ai stream`/`gui overlay` が TFLM ランタイムで
    動作。**CMSIS-NN 最適化カーネル**で ~622ms/推論＝stedgeai(685ms) を僅かに上回る。
    **P2 完了** — microSD 上の `.tflite` を**ランタイムに `.sdram.ai` へロード**して
    実行（`ai model load <sd-path>`、#89）。共通ビジョン op superset で任意の小型 int8
    モデルを総取っ替え可能。
    **P5 完了** — X-CUBE-AI **relocatable network** で SD の `network_rel.bin` を
    ランタイムロードして **XIP 実行**（`CONFIG_NN_BACKEND=stedgeai_reloc`、
    `ai model load network_rel.bin`、#92）。P2(tflm) と同じ「SD 差し替え」を stedgeai
    ランタイム側でも実現＝**両対応完成**。詳細は下記
    [X-CUBE-AI relocatable backend](#x-cube-ai-relocatable-backend-p5-92)。

## 設計方針

- **backend 非依存の `nn` 抽象**（tensor-in / tensor-out）で、ランタイムを差し替え可能に。
  ビルドごとに 1 backend を選択（`CONFIG_NN_BACKEND`）。
  - `null` — ランタイム無し（既定）。stedgeai/モデルが無くても**本体ファームは常にビルド可**。
    BlazeFace と同型の入出力を持つ stub で plumbing を端から端まで動かせる。
  - `stedgeai` — X-CUBE-AI / ST Edge AI Core。`stedgeai generate` の生成 C（STAI API）
    + プリコンパイル静的ライブラリ `NetworkRuntime*_CM7_GCC.a` をリンク。tensor 記述子は
    生成ヘッダの `STAI_NETWORK_*` マクロから構築（1-in/1-out〜多出力までモデル非依存）。
  - `tflm` — LiteRT / TensorFlow Lite Micro（**P3, #86/#88**）。C++ の `MicroInterpreter`
    を別の静的ライブラリ `tflm` に閉じ込め、`CONFIG_NN_BACKEND=tflm` ビルドでのみ C++ を有効化
    する（既定 null/stedgeai ビルドはコード非改変＝byte-identical）。**BlazeFace-front 128 int8**
    を実行し、**CMSIS-NN 最適化カーネル**（既定 ON）で stedgeai 並みの速度。詳細は下記
    [TFLM backend](#tflm-backend-p3-8688)。
  - `stedgeai_reloc` — X-CUBE-AI **relocatable network**（**P5, #92**）。`stedgeai generate
    --relocatable` が吐く position-independent な単一 `network_rel.bin`（PIC コード + 重み同梱）
    を SD からロードし、ST ホストローダ + legacy `ai_rel_network_*` API で **XIP 実行**する。
    ランタイム `.a` はリンク不要（PIC カーネルは `.bin` に焼込み）。**SD 専用**（Flash 埋め込みの
    builtin は持たない）。詳細は下記 [X-CUBE-AI relocatable backend](#x-cube-ai-relocatable-backend-p5-92)。
- **モデル固有の前後処理**（BlazeFace の anchor デコード + NMS 等）は `nn` 層の**上**
  （`port/nn/models/`）に置き、汎用 `nn` 層はモデル非依存に保つ。
- **同じ int8 `.tflite` を両 backend が食える**ため、モデルは可搬でランタイム選択は
  後から差し替え可能。

## レイヤ構成

```
HAL/CMSIS/ThreadX  <-  svc  <-  port/nn  <-  ui  <-  shell/src
                                  |
             nn.h (API) / nn_backend.h (vtable)
             nn.c (dispatch + DWT 計測)
             nn_null.c | nn_stedgeai.c | nn_tflm.c   (1 つを選択)
             nn_camera.c (カメラ -> 推論 glue)
             models/blazeface.c (モデル固有 後処理)
```

| ファイル | 役割 |
|---|---|
| `port/nn/nn.h` | `nn_tensor`（int8 量子化パラメータ込み）/ `nn_model` / `nn_init`・`nn_model_open`・`nn_input`・`nn_output`・`nn_run`・`nn_last_cycles` |
| `port/nn/nn_backend.h` | 各 backend が実装する vtable（`nn_backend_vt_selected`） |
| `port/nn/nn.c` | 公開 API dispatch + **DWT CYCCNT** による推論レイテンシ計測 |
| `port/nn/nn_null.c` | null backend（BlazeFace 同型 stub） |
| `port/nn/nn_stedgeai.c` | X-CUBE-AI(STAI) backend（`generated/` + ST ランタイム .a） |
| `port/nn/nn_camera.{c,h}` | ライブカメラ → 推論の sink + worker |
| `port/nn/models/blazeface.{c,h}` | BlazeFace 顔検出デコード（anchor + NMS、モデル固有） |

## メモリ配置

- **`.sdram.ai` arena** — FMC 内部 **bank3**（`0xC0600000`〜`0xC07FFFFF`, 2MB）。
  他バンク（bank0 LTDC / bank1 カメラ DMA / bank2 ETH DMA）と分離した専用バンク。
  **`src/bsp.c` の MPU region1 で cacheable(WBWA) 化済み**（bank3 のみ region0 を上書き、
  ARMv7-M は高番号 region 優先）。arena は CPU-only（**DMA 流入なし**）なので D-cache は
  maintenance 不要でコヒーレント。**非 cacheable では NN 推論が約 20x 遅く**、cacheable 化で
  MNIST 実測 **9.5ms→2.5ms**（#6）。活性化が DTCM に収まる小モデルは DTCM も可。
    - **bank3 の分割は reloc 限定（P5 #92 / #95）** — `stedgeai_reloc` **のみ** bank3(2MB) を分ける。
      **下位 1MB**（`0xC0600000`〜, `.sdram.ai`）は data-only（activations / staging / XIP RW イメージ）で
      **XN 維持**（region1）。**上位 1MB**（`0xC0700000`〜, `.sdram.ai.model`）は `src/bsp.c` の
      **MPU region2 で命令フェッチ許可**し、relocatable `.bin` の double-slot を置いて **XIP 実行**する。
      W^X を保つため実行窓はモデルスロットのみ（data バッファは XN）。
    - **非 reloc（null/stedgeai/tflm）は `.sdram.ai` が bank3 全 2MB まで使える（#95）** — `.sdram.ai.model`
      は空。リンカは `. = MAX(., 0xC0700000)` で split を reloc のみに効かせる（大きい arena=TFLM ~1.9MB は
      0xC0700000 を越えて伸び、空 model が直後。小さい arena=stedgeai は空 model が 0xC0700000 へ丸まるだけで
      NOLOAD ゆえ無害）。**`src/bsp.c` は region2 を明示 disable** し bank3 全域を region1 の XN cacheable data に保つ。
      これで **TFLM の ~1.9MB arena（512KB + 2×512KB SD slot + 384KB staging）が収まる**（#95 以前は
      split が無条件で上位 1MB を死蔵し TFLM がリンク不能だった）。
- **重み** — P1 は Flash `.rodata`。**性能上の注意**: 本 linker は Flash を `0x08000000`
  = **AXIM 側**に置く。RM0385 §3.3.2 の ART accelerator は **ITCM interface 側**の flash
  access 用であり、AXIM `.rodata` の重みは ART/I$ ではなく **D-cache + Flash wait-state
  (216MHz = 7WS)** の話。性能は `ai bench` で**実測**して判断すること。

リンカ（`ldscript/STM32F746NGHx_FLASH.ld`）は ASSERT で配置を保証する: `_ssdram_ai == bank3 起点` /
`.sdram.ai(+ .sdram.ai.model) <= 2MB` / `.sdram.eth` を bank2 内に留める / **`.sdram.ai.model` は空
（非 reloc） or `0xC0700000`（reloc）** の backend 非依存 W^X ガード（#95、`.sdram.ai.model` を出すのは
`nn_stedgeai_reloc.c` のみ）。

## カメラ → 推論パイプライン（`nn_camera.c`）

```
DCMI producer (prio 10)          nn worker (prio 18, best-effort)
      |  RGB565 frame                    ^
      v                                   | READY stage
  consume():                              |
    最近傍縮小 + RGB565->int8  --> stage[0/1] --(copy)--> model 入力 --> nn_run()
    即 camera_frame_put()                 
```

- **同期コピー sink**: `consume()` は camera producer スレッドで動き、リサイズ+変換して
  即 `camera_frame_put()`。pipeline の in-flight は常に 0 で、カメラの async teardown が
  安全（`nx_mjpeg.c` eth_sink と同型）。
- **NN 入力の所有権**（重要）: pipeline の pin/put は**カメラ slot** の寿命しか守らない。
  NN 入力の上書き競合を防ぐため、**2 面の staging バッファ**に状態
  `FREE → FILLING → READY → RUNNING → FREE` を持たせ、短いクリティカルセクション
  （`TX_MUTEX`）で保護。worker が使用中（`RUNNING`）のバッファを sink が書くことは無い。
  空き面が無ければ**ドロップ**。
- **worker 優先度 = 18（完全 best-effort）**: camera(10)/net(12)/GUIX(14)/ETH-link(15)/
  CLI(16)/BG(17) を絶対に starve しない。推論は monolithic（途中 yield 不可）なので、
  CLI(16) より下に置くことで `ai stream stop` の到達を保証する。

## 顔検出（BlazeFace-front 128, #8）

第一モデル = ST Model Zoo の **BlazeFace Front 128×128**（MediaPipe/PINTO 由来の SSD 顔検出器）。

- **モデル**: `int8` 量子化（重み int8, 105KB）だが **I/O は float32**（入口 QUANTIZE / 出口
  DEQUANTIZE ノードが埋込み）。入力 `1×128×128×3 f32`、出力は 4 テンソル（2 スケール ×
  {box, score}）: `1×512×16`+`1×512×1`（16×16 grid×2）と `1×384×16`+`1×384×1`（8×8 grid×6）
  = **896 anchor**。box 16 = 4 bbox + 6 keypoints×2。
- **前処理**（`nn_camera.c`）: RGB565 → 最近傍縮小 → float32 **[0,1]**（`rgb/255`）。★モデル
  カードは [-1,1] と書くが**実機では [0,1] のみ顔検出成功**（maxscore 288 vs 11）＝ ST は
  [0,1] で再学習。`ai norm <0|1>` で実行時切替可（0=[0,1] 既定 / 1=[-1,1]）。
- **後処理**（`port/nn/models/blazeface.c`）: shape でテンソル同定（C=16→box, C=1→score;
  anchors=512→16×16, 384→8×8）、anchor をセル中心で生成、score 閾値は logit 比較（expf 非使用）、
  box = `raw/128 + anchor中心`、hard NMS（IoU 0.5）。非 BlazeFace モデルには安全な no-op。
- **性能**: 31.8M MACC → **~685ms / ~1.5fps**（~4.6 cyc/MACC）。real-time ではないがデモ可。
  `-O time` は逆効果（balanced 最良）。

生成（BlazeFace int8 tflite → stm32f7）:
```bash
$STEDGEAI_ROOT/Utilities/linux/stedgeai generate \
    --model blazeface_front_128_int8.tflite --target stm32f7 --type tflite \
    --name network --output port/nn/generated
```

## `ai` シェルコマンド

| コマンド | 説明 |
|---|---|
| `ai info` | backend / モデル名 / 入出力テンソル shape・dtype・量子化 / arena サイズ |
| `ai bench [n]` | 固定入力で n 回推論し、min/avg/max レイテンシ（µs, DWT）とスループット |
| `ai run` | カメラ 1 フレームで単発推論し、レイテンシ + 顔 bbox を表示 |
| `ai stream start [qqvga\|qvga]` | ライブ連続推論を開始（既定 QVGA） |
| `ai stream stop` | 停止 |
| `ai stream stats` | 推論レート / レイテンシ / drop / 顔 bbox（+ maxscore 診断） |
| `ai model` | 現在のモデル source（`builtin` / `sd:<name>`）を表示 |
| `ai model load <sd-path>` | microSD からモデルをロードして実行（`tflm`=`.tflite`／`stedgeai_reloc`=`network_rel.bin`、#89/#92） |
| `ai model builtin` | ビルトインモデルへ復帰（`stedgeai_reloc` は builtin 無しゆえアンロード=`(none)`） |
| `ai norm <0\|1>` | float 入力正規化切替（1=[-1,1] / 0=[0,1] 既定） |

**レイテンシ計測**は Cortex-M7 DWT CYCCNT（RM0385 §40.10/§40.13/§40.14.2）。M7 では
DWT の CoreSight ソフトロックを解除する必要があり、`DWT->LAR = 0xC5ACCE55`（CoreSight
lock key）を書く（`cmd_membench.c` #57 と同じ）。DWT は WFI 中のみ凍結するが、推論は
CPU-bound でスリープしないため実レイテンシが取れる。

## ビルド設定

```bash
# 既定（null backend、stedgeai 不要で常にビルド可）
cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-toolchain.cmake
cmake --build build

# X-CUBE-AI backend（stedgeai インストール + 生成モデルが前提, #6）
# 1) モデル生成（生成物は port/nn/generated/, .gitignore）:
$STEDGEAI_ROOT/Utilities/linux/stedgeai generate --model model.tflite \
    --target stm32f7 --type tflite --name network --output port/nn/generated
# 2) ビルド:
cmake -B build ... -DCONFIG_NN_BACKEND=stedgeai -DSTEDGEAI_ROOT=/opt/ST/STEdgeAI/4.0

# X-CUBE-AI relocatable backend（SD からモデルロード, P5 #92）
# 1) モデル .bin 生成（非コミット。プロジェクトのツールチェーンを PATH に載せて生成）:
scripts/gen-reloc-model.sh [model.tflite] [out]     # 既定=BlazeFace int8 → port/nn/reloc-out/
#    → out/network_rel.bin を microSD ルートへコピー
# 2) ビルド + フラッシュ:
cmake -B build-reloc ... -DCONFIG_NN_BACKEND=stedgeai_reloc
# 3) 実機で:  ai model load network_rel.bin
```

!!! warning "ライセンス"
    本リポジトリは public。ST SLA のランタイム `.a`・生成コード（`port/nn/generated/`）・
    ST ホストローダ `ai_reloc_network.c`・relocatable `network_rel.bin`・ST Model Zoo の
    モデル本体は **git にコミットしない**（`.gitignore`）。

## TFLM backend (P3, #86/#88)

TensorFlow Lite Micro（tflite-micro）は C++17。純 C/ASM プロジェクトへ C++ を持ち込む
のは最大のリスクなので、M1 spike（#86）で成立性を先に実証し、**M2（#88）で
BlazeFace-front 128 int8 を本統合**した。同じ int8 `.tflite` を stedgeai と TFLM 双方が食う。

- **C++ は tflm ビルドのみ**: `CONFIG_NN_BACKEND=tflm` の時だけ `cmake/tflite-micro.cmake`
  が `enable_language(CXX)` を呼ぶ。全 C++ は静的ライブラリ `tflm` に閉じ込め、`threadx` 本体の
  TU は C のまま＝**最終 link は C driver(gcc)**（`LINKER_LANGUAGE C`）。nano C++ ランタイム
  （`libstdc++_nano.a` / `libsupc++_nano.a`）+ `libm` を明示 link する。既定 null/stedgeai
  ビルドは C++ を一切見ない（byte-identical）。
- **ベアメタル C++ ランタイム**: startup の `__libc_init_array` が静的コンストラクタを走らせる
  （`.init_array` は linker script で `KEEP` 済）。`-fno-exceptions -fno-rtti
  -fno-threadsafe-statics -fno-use-cxa-atexit` + `TF_LITE_STATIC_MEMORY`（bump allocator＝
  ヒープ不使用）。自前の noexcept `operator new/delete`（`port/nn/tflm/cxx_runtime.cc`）で
  libstdc++ の**例外を投げる operator new**（→ `__cxa_throw`/`_Unwind_*`）の混入を防ぐ。
- **モデル配列は configure 時生成**: `.tflite`（`NN_TFLM_MODEL`、既定は stedgeai と同一の
  BlazeFace int8）は非コミット（`.gitignore`）。`cmake/gen_model_array.py` が `alignas(16)` の
  `g_blazeface_model_data[]` を **build ディレクトリ**に生成し tflm lib へ追加。model の
  path+size+mtime を stamp に持ち差替を検知（`-DNN_TFLM_MODEL=<path>` で上書き可）。
- **resolver / I/O**: `MicroMutableOpResolver<8>` に BlazeFace の op（QUANTIZE / CONV_2D /
  DEPTHWISE_CONV_2D / ADD / PAD / MAX_POOL_2D / RESHAPE / DEQUANTIZE）を登録。`nn_tflm.cc` は
  1-in / N-out をインタプリタから汎用に構築（`arena_used_bytes()` を `ai info` に表示）ので、
  出力 4 テンソルは `blazeface.c` が shape で同定＝モデル固有コードは不要。
- **arena**: `.sdram.ai`（bank3, cacheable WBWA, CPU-only）。BlazeFace の活性化は実測
  **~470KB**（512KB 予約、staging 384KB と合わせ bank3 の 2MB 内）。stedgeai と同じバンク。
- **FPU は単精度**（`fpv5-sp-d16`）＝`double` は soft-float。BlazeFace は Softmax/Logistic を
  持たない（活性化は fused）ため、`std::exp`(double) の重い経路が無い。
- **vendoring = configure 時 fetch（repo に非同梱）**: `cmake/tflite-micro.cmake` が CMake
  configure 時に pinned SHA を `git fetch` し、`create_tflm_tree.py` で self-contained tree を
  **build ディレクトリ**に生成、我々のフラグで compile する。SHA + kernel（ref/cmsis_nn）を鍵に
  した stamp で 2 回目以降はスキップ。**numpy/Pillow が必要**（`requirements.txt`、`./.venv` 自動生成）。

### CMSIS-NN 最適化カーネル（M2b）

`NN_TFLM_CMSIS_NN`（既定 **ON**）で `create_tflm_tree.py` に `OPTIMIZED_KERNEL_DIR=cmsis_nn`
を渡す。tflm Makefile が **CMSIS_6 core + ARM-software/CMSIS-NN**（それぞれ pinned SHA）を
自前 download し、参照カーネルの代わりに cmsis_nn kernel wrapper（CONV_2D / DEPTHWISE_CONV_2D /
ADD / PAD / MAX_POOL_2D）を使う。`-DCMSIS_NN` でカーネルヘッダを inline 参照 → extern 宣言に
切替（無いと optimized kernel が redefinition）。**Cortex-M7 は armv7e-m** で GCC が
`__ARM_FEATURE_DSP` を定義するため、CMSIS-NN の **SIMD(SMLAD 等) int8 パス**がコンパイルされる
（`nn_tflm.cc` の `#error` canary で保証、MVE は M7 に無く未定義）。`NN_TFLM_CMSIS_NN=OFF` で
純参照カーネルに戻せる（比較用）。

**`ai bench` 実測**（BlazeFace-front 128 int8, 31.8M MACC, @216MHz、単発 `nn_run`）:

| backend | kernel | レイテンシ/推論 | cyc/MACC |
|---|---|---|---|
| `stedgeai`（X-CUBE-AI） | ST 最適化 | ~685 ms | ~4.6 |
| `tflm` | reference | ~2,418 ms | ~16.4 |
| **`tflm`** | **CMSIS-NN** | **~622 ms** | **~4.2** |

CMSIS-NN は CONV_2D / DEPTHWISE_CONV_2D を SIMD 加速し、参照比 **~3.9x**・stedgeai を僅かに
上回る。検出結果（bbox / score）は 3 者一致。live `ai stream` は DCMI と bank3 帯域が競合する
ため ~1.5s/推論（~0.5 inf/s）＝単発 bench より遅い（F746 は bus matrix round-robin で SDRAM
QoS 無し）。Flash コストは null ベースラインから **+約60KB**（interpreter + CMSIS-NN lib + モデル
189KB）。

```bash
# TFLM backend（configure 時に tflite-micro + CMSIS-NN を fetch。network + python3 が必要）
cmake -B build-tflm -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-toolchain.cmake \
    -DCONFIG_NN_BACKEND=tflm                 # CMSIS-NN 既定 ON
cmake --build build-tflm
# 参照カーネルと比較する場合:
cmake -B build-tflm ... -DCONFIG_NN_BACKEND=tflm -DNN_TFLM_CMSIS_NN=OFF
# 実機で確認: ai info / ai bench / ai stream
```

## SD からモデルロード（P2, #89）

TFLM は `.tflite` flatbuffer を RAM 上で解釈するので、**stedgeai と違い再コンパイル
無しでモデルを総取っ替え**できる。P2 は microSD（FileX）上の `.tflite` を**ランタイムに
`.sdram.ai` へロード**して interpreter をその場で再構築する。

- **op superset**: 実行時にどのモデルが来るか分からないため、resolver に共通ビジョン op
  の**superset（23 op：Conv/DWConv/FC/Pool/Softmax/Logistic/Relu/Reshape/Quant/Dequant/
  Pad/Add/Mul/Sub/Mean/Concat/Resize/StridedSlice/LeakyRelu/Transpose 等）**を登録。任意の
  小型 int8 分類器/検出器が走る（BlazeFace は 8 op のみ使用＝既定は無害、未使用 kernel は
  `--gc-sections` で除去）。
- **二重 model slot**: `.sdram.ai` に **512KB slot を 2 本**持つ。`ai model load` は現 active
  でない**inactive slot** へ SD を読み込み、interpreter 再構築が成功した時だけ active を
  flip する。読込中に旧 flatbuffer を破壊しない＝**transactional**（失敗時は直前モデル維持）。
  bank3: staging 384KB + arena 512KB + model 2×512KB = 1920KB < 2MB。
- **安全な再構築**: `~MicroInterpreter()` は `FreeSubgraphs()` を呼ぶため、再構築は placement-new
  前に明示 destruct する。`.tflite` は `VerifyModelBuffer` で長さ検証してから使う（壊れた/短い
  ファイルはきれいに失敗）。SD 読込は full-size 事前チェック + short-read 検出。
- **排他**: interpreter/arena/model は singleton。`ai model load` は**ストリーム停止 + 単一
  セッション取得**の下で行う（セッションを slot 選択の前に取得＝他シェルによる slot 横取りを防ぐ）。
- **SDMMC DMA は `.sdram.ai` に直接書かない**: SD driver は固定 SRAM バウンスバッファへ DMA 後
  `memcpy` するので、bank3 への最終書込は CPU store＝D-cache コヒーレント（maintenance 不要）。

```
ai model                 # 現在のモデル source（builtin / sd:<name>）
ai model load bf.tflite  # microSD の .tflite をロード（sd ls で確認）
ai info                  # ロード後の I/O shape / arena を確認
ai model builtin         # ビルトインへ復帰
```

!!! note "tflm ライブストリームの帯域"
    `ai stream`（tflm + CMSIS-NN）は推論が SDRAM 帯域を飽和させ、DCMI FIFO overrun で
    カメラが停止することがある（F746 は bus matrix round-robin で SDRAM QoS 無し）。単発
    `ai bench` / `ai run` / GUI overlay（自動復帰あり）は問題ない。shell `ai stream` の
    overrun 耐性は後続で対応。

## X-CUBE-AI relocatable backend (P5, #92)

X-CUBE-AI は本来トポロジを Flash に焼き込む固定方式だが、**relocatable network** を使うと
position-independent 化でき、**再フラッシュ無しでモデルを差し替え**られる。P5 はこれを
`CONFIG_NN_BACKEND=stedgeai_reloc` として実装し、P2(tflm) と対になる「SD 差し替え」を
stedgeai ランタイム側でも実現する（両対応完成）。

- **生成物 = 単一 `network_rel.bin`**: `stedgeai generate --relocatable --target stm32f7` が
  PIC コード + 重み同梱の ~208KB バイナリを吐く（`scripts/gen-reloc-model.sh`）。**非コミット**
  （ST-SLA / model-zoo）。
- **firmware 側はローダ 1 本のみ**: ST ホストローダ `ai_reloc_network.c`（ST install から
  compile、`APP_DEBUG=0` で printf 除去）+ legacy `ai_rel_network_*` API。**ランタイム `.a` は
  リンクしない**（PIC カーネル・重み・memcpy 等は全て `.bin` に焼込み、モデルの各エントリは
  `.bin` 内オフセットを r9=GOT-base トランポリン経由で間接呼び出し）。
- **XIP + MPU**: `.bin` を bank3 上位 1MB（`.sdram.ai.model`, `0xC0700000`）にロードし、
  `src/bsp.c` の **MPU region2 が命令フェッチ許可**（下位は XN）。コードは `.bin` バッファ上で
  in-place 実行する（RT RAM に data/got/bss のみ配置）。★**キャッシュ**: XIP ではローダはキャッシュ
  保守を一切しないため、backend が install 前に **`SCB_CleanDCache_by_Addr` + `SCB_InvalidateICache`**
  を実施（Cortex-M7 は I/D cache 分離で I-fetch は D-cache を snoop しない、RM0385 §2.1.3 AXIM）。
- **transactional double-slot**: `.sdram.ai.model` に `.bin` slot を 2 本持つ。**XIP は旧コードを
  旧 `.bin` から実行中**なので単一バッファ差替は致命的 → inactive slot へ SD 読込 → install/init/
  get_report 検証が成功した時だけ active を flip、旧は検証完了まで生存（失敗時は旧モデルを維持）。
- **bounded `.bin` 検証**: ST ローダは長さを取らず object 内オフセットを盲目的に辿るため、backend が
  `.bin` ヘッダ（magic `0x4E49424E` / CM7・FPU・hard variant / 全セクション境界 / vec エントリ /
  `.rel` テーブル走査）を **ST API 呼出前に自前検証**する。sizing（`acts_sz` / RT RAM / weights）も
  `rt_get_info` を使わず**検証済みヘッダ + ctx file image から導出**する（`rt_get_info` は install 前に
  scaled-pointer で OOB を起こすため不使用）。
- **SD 専用**: builtin モデルは持たない。`ai model load network_rel.bin` するまでモデル無し
  （`ai run`/`ai stream`/`gui overlay` は未ロードを明示エラーで拒否）。`ai model builtin` は
  アンロード（`(none)`）。

**性能（実測 + 注意）**: `ai bench` は **LTDC/カメラ停止（`gui stop`/`lcd off`）＝SDRAM 帯域が
静穏な状態で ~592ms/推論**（BlazeFace-front 128, @216MHz）。★**ただし XIP はコードを SDRAM(bank3)
から fetch するため SDRAM 帯域競合に敏感**で、LTDC scan-out やカメラ DMA が動くと fetch が遅くなり
bench 値は上がる（Flash 焼込み `stedgeai` はコードが Flash 実行ゆえ SDRAM 競合に非依存）。よって
592ms は静穏時のベストケースで、**Flash 版(685ms)や TFLM CMSIS-NN(622ms) に対するクリーンな速度優位
ではない**（表示/カメラ稼働時はそれ以上になり得る）。live `ai stream`/`gui overlay` は DCMI と bank3
帯域が競合し ~1.4 inf/s（#90 と同根）。P5 の主眼は **X-CUBE-AI relocatable の実証 / Epic 完成**。

キーファイル: `port/nn/nn_stedgeai_reloc.c`（backend + `.bin` 検証 + transactional reload + XIP 実行）、
`src/bsp.c`（MPU region2）、`ldscript/STM32F746NGHx_FLASH.ld`（`.sdram.ai` 分割 + ASSERT）、
`scripts/gen-reloc-model.sh`（オフライン `.bin` 生成）。

## Roadmap（Epic #80）

| Phase | 内容 |
|---|---|
| **P1 ✅** | `nn` 抽象 + X-CUBE-AI backend + `ai` コマンド + **BlazeFace-128 顔検出 first-light** |
| **P2 ✅** | **SD からモデルロード（FileX → `.sdram.ai`、`ai model load`、#89）** |
| **P3 ✅** | TFLM backend。M1 spike（C++ 有効化 + hello_world, #86）→ **M2 BlazeFace 本統合 + CMSIS-NN + 比較ベンチ（#88）** |
| **P4 ✅** | GUIX オーバレイ（ライブプレビュー + 顔 bbox 同時表示、#83 → [GUIX カメラ UI](../rtos/guix.md#83-epic-80-p4)） |
| P5 | X-CUBE-AI relocatable network（SD からモデル総取っ替え） |
