# オンデバイス NN 推論

STM32F746G-DISCO 上で**ニューラルネット推論**を動かすための、backend 非依存の
`nn` 抽象層と `ai` シェルコマンド（Issue #81 / Epic #80）。第一ユースケースは
**カメラ顔検出**（OV5640/DCMI → 推論）。

!!! note "Phase 状況"
    **P1 完了** — null backend 土台 + X-CUBE-AI(`stedgeai`) backend + BlazeFace-128
    顔検出が実機で動作（`ai run`/`ai stream` が顔 bbox を出力、~1.5fps）。
    **P4 完了** — GUIX ライブプレビュー上に顔 bbox を描画（`gui overlay on`、#83）。
    **P3 M1 完了** — TFLM(tflite-micro) backend の feasibility spike（#86）。純 C/ASM
    プロジェクトへ C++ を持ち込み、最小の `MicroInterpreter` が実機でビルド・静的初期化・
    1 推論できることを実証（`ai selftest` PASS）。BlazeFace 本統合（M2）と SD モデルロード
    （P2）は後続。

## 設計方針

- **backend 非依存の `nn` 抽象**（tensor-in / tensor-out）で、ランタイムを差し替え可能に。
  ビルドごとに 1 backend を選択（`CONFIG_NN_BACKEND`）。
  - `null` — ランタイム無し（既定）。stedgeai/モデルが無くても**本体ファームは常にビルド可**。
    BlazeFace と同型の入出力を持つ stub で plumbing を端から端まで動かせる。
  - `stedgeai` — X-CUBE-AI / ST Edge AI Core。`stedgeai generate` の生成 C（STAI API）
    + プリコンパイル静的ライブラリ `NetworkRuntime*_CM7_GCC.a` をリンク。tensor 記述子は
    生成ヘッダの `STAI_NETWORK_*` マクロから構築（1-in/1-out〜多出力までモデル非依存）。
  - `tflm` — LiteRT / TensorFlow Lite Micro（**P3 M1 spike, #86**）。C++ の `MicroInterpreter`
    を別の静的ライブラリ `tflm` に閉じ込め、`CONFIG_NN_BACKEND=tflm` ビルドでのみ C++ を有効化
    する（既定 null/stedgeai ビルドはコード非改変＝byte-identical）。詳細は下記 [TFLM backend](#tflm-backend-p3-m1-86)。
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
- **重み** — P1 は Flash `.rodata`。**性能上の注意**: 本 linker は Flash を `0x08000000`
  = **AXIM 側**に置く。RM0385 §3.3.2 の ART accelerator は **ITCM interface 側**の flash
  access 用であり、AXIM `.rodata` の重みは ART/I$ ではなく **D-cache + Flash wait-state
  (216MHz = 7WS)** の話。性能は `ai bench` で**実測**して判断すること。

リンカ（`ldscript/STM32F746NGHx_FLASH.ld`）は 3 本の ASSERT で配置を保証する:
`_ssdram_ai == bank3 起点` / `.sdram.ai <= 2MB` / `.sdram.eth` を bank2 内に留める。

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
```

!!! warning "ライセンス"
    本リポジトリは public。ST SLA のランタイム `.a`・生成コード（`port/nn/generated/`）・
    ST Model Zoo のモデル本体は **git にコミットしない**（`.gitignore`）。

## TFLM backend (P3 M1, #86)

TensorFlow Lite Micro（tflite-micro）は C++17。純 C/ASM プロジェクトへ C++ を持ち込む
のは最大のリスクなので、**feasibility spike** で成立性を先に実証する（`ai selftest`）。

- **C++ は tflm ビルドのみ**: `CONFIG_NN_BACKEND=tflm` の時だけ `cmake/tflite-micro.cmake`
  が `enable_language(CXX)` を呼ぶ。全 C++ は静的ライブラリ `tflm` に閉じ込め、`threadx` 本体の
  TU は C のまま＝**最終 link は C driver(gcc)**。nano C++ ランタイム（`libstdc++_nano.a` /
  `libsupc++_nano.a`）を明示 link する。既定 null/stedgeai ビルドは C++ を一切見ない。
- **ベアメタル C++ ランタイム**: startup の `__libc_init_array` が静的コンストラクタを走らせる
  （`.init_array` は linker script で `KEEP` 済）。`-fno-exceptions -fno-rtti
  -fno-threadsafe-statics -fno-use-cxa-atexit` + `TF_LITE_STATIC_MEMORY`（bump allocator＝
  ヒープ不使用）。自前の noexcept `operator new/delete`（`port/nn/tflm/cxx_runtime.cc`）で
  libstdc++ の**例外を投げる operator new**（→ `__cxa_throw`/`_Unwind_*`）の混入を防ぐ。
- **arena**: `.sdram.ai`（bank3, cacheable WBWA, CPU-only）。stedgeai と同じ配置。
- **FPU は単精度**（`fpv5-sp-d16` / multilib `v7e-m+fp/hard`）＝`double` は soft-float。spike の
  hello_world（FullyConnected のみ）は double 経路が無いため最適。
- **vendoring = configure 時 fetch（repo に非同梱）**: tflite-micro は third_party
  （flatbuffers/gemmlowp/ruy）を repo に持たないため、`cmake/tflite-micro.cmake` が **CMake
  configure 時**に pinned SHA を `git fetch` し、upstream の `create_tflm_tree.py -e hello_world`
  （reference kernel のみ）を回して self-contained tree を **build ディレクトリ**に生成、それを
  我々のフラグで compile する（ツールチェーン自動 DL と同じ「取得は build 時」方針）。SHA を鍵にした
  stamp で 2 回目以降はスキップ。**tflm ビルドのみ**が fetch コストを払い、既定 null/stedgeai は無影響。
  pin SHA と生成コマンドは `cmake/tflite-micro.cmake` 冒頭に記載。**numpy/Pillow が必要**（TFLM の
  Makefile 評価が使う）→ `requirements.txt` に追加、`./.venv` を自動生成して使う。
- **CMSIS-NN は M2**: vendored `lib/cmsis_core/NN` は legacy CMSIS-5 API で TFLM の cmsis_nn
  kernel と非互換。spike は reference kernel のみ、最適化 kernel は M2 で別 submodule。

`port/nn/tflm/nn_tflm.cc` が `nn_backend_vt`（`extern "C"`）を実装し、spike モデル
（`hello_world_int8`, 2.7KB, FullyConnected のみ, int8 1×1 I/O）を動かす。
`ai selftest`（`CONFIG_NN_BACKEND_TFLM` 限定）が 1 推論を実行して sine 近似を golden と照合し、
静的 ctor の sentinel と**ヒープ未使用**（`operator new` 呼び出し 0）を確認する。実測 Flash
コストは null ベースラインから **+22.8KB**（interpreter + FullyConnected reference kernel）。

```bash
# TFLM backend（configure 時に tflite-micro を fetch。network + python3 が必要）
cmake -B build_tflm -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-toolchain.cmake \
    -DCONFIG_NN_BACKEND=tflm
cmake --build build_tflm
# 実機で確認: ai selftest / ai info / ai bench
```

## Roadmap（Epic #80）

| Phase | 内容 |
|---|---|
| **P1 ✅** | `nn` 抽象 + X-CUBE-AI backend + `ai` コマンド + **BlazeFace-128 顔検出 first-light** |
| P2 | SD カードからモデル/重みロード（FileX → `.sdram.ai`）※TFLM 前提 |
| **P3 M1 ✅** | TFLM backend feasibility spike（C++ 有効化 + hello_world 推論, #86）。M2=BlazeFace 本統合 + CMSIS-NN, 比較ベンチ |
| **P4 ✅** | GUIX オーバレイ（ライブプレビュー + 顔 bbox 同時表示、#83 → [GUIX カメラ UI](../rtos/guix.md#83-epic-80-p4)） |
| P5 | X-CUBE-AI relocatable network（SD からモデル総取っ替え） |
