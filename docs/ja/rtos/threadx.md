# RTOS (Eclipse ThreadX)

`threadx` アプリは上流 **Eclipse ThreadX**（MIT、`lib/threadx` submodule、Cortex-M7/GNU ポート）を動かす。`tx_application_define()` で 2 スレッドを生成:

- `led` — LD1 を 250 ms ごとにトグル
- `print` — VCP に 1 秒ごとにカウンタ出力

## 統合（`port/threadx/`）

- `tx_glue.c`: `_tx_initialize_low_level()` と `SysTick_Handler` を提供。単一の 1 ms SysTick で HAL tick（`HAL_IncTick`）と ThreadX tick（`_tx_timer_interrupt`）の両方を駆動。ThreadX の `PendSV_Handler` はポート（`tx_thread_schedule.S`）が定義するため、`src/stm32f7xx_it.c` はこのアプリから除外。
- `tx_user.h`: `TX_TIMER_TICKS_PER_SECOND = 1000`（1 tick = 1 ms）。

## 割込み優先度の注意点（重要）

**SysTick は PendSV より高優先度**でなければならない（PendSV=15、SysTick=14）。

アイドル時、ThreadX は PendSV ハンドラ内でスピンしながら「次に起床すべきスレッド」を待つ。SysTick が PendSV と同一優先度だと、実行中の PendSV を SysTick が割り込めず（同一優先度はプリエンプト不可）、tick が止まり、スリープ中のスレッドが永久に起床しない（デッドロック）。

クリティカルセクションは PRIMASK（`CPSID i`）で保護されるため、SysTick を高優先度にしても ThreadX 内部は安全にマスクされる。

!!! note "検証方法"
    SWD で `_tx_timer_system_clock` を 1 秒間隔で読むと、正常時は約 +1000/秒。停止していれば tick 問題。`$pc` が `__tx_ts_wait`（PendSV 内アイドル）で固着していないか確認する。

## ビルド時の要点

- `common/src/*.c`（コア）+ `ports/cortex_m7/gnu/src/*.S`（コンテキストスイッチ等）をビルド。`tx_misra`（`.c`/`.S` とも）は重複定義のため**除外**。
- ポート `.S` は `tx_user.h` を `#include` するため、ASM にも include パスを通し `TX_INCLUDE_USER_DEFINE_FILE` を定義。

## Thread-Metric ベンチマーク（オプション）

ThreadX 同梱の Thread-Metric（RTOS 性能ベンチ、8 テスト）を `thread_metric` アプリとして実行できる。

```bash
cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-toolchain.cmake \
      -DBUILD_THREAD_METRIC=ON -DTHREAD_METRIC_TEST=basic
cmake --build build --target flash-thread_metric
```

`THREAD_METRIC_TEST`：`basic`（既定）/ `cooperative` / `preemptive` / `memory` / `message` / `sync` / `interrupt` / `interrupt_preempt`。結果は 30 秒ごとに VCP 出力（例：basic は `Time Period Total: 252879`）。

統合上の要点：

- ThreadX tick を **100 Hz** に（`TX_GLUE_TICK_DIV=10` + `TX_TIMER_TICKS_PER_SECOND=100`）。移植層の `TM_THREADX_TICKS_PER_SECOND` に合わせ、`tm_thread_sleep(30)` が実 30 秒になる。
- 割込みテストは `SVC #0`（`TM_CAUSE_INTERRUPT`）。`port/threadx/tm_svc.c` が `SVC_Handler`→`tm_interrupt_handler` を配線（該当テストのみリンク）。
- ベンチのカウンタは非 volatile グローバルを無限ループで更新するため、テストソースのみ `-O0` でビルド（-O2 だとレジスタ保持でメモリに書き戻らず report が 0 を読み "died" になる）。ThreadX 本体は `-O2` のまま。

## Execution Profile Kit（オプション）

`exec_profile` アプリで、スレッド/ISR/アイドルの CPU 時間を Cortex-M7 の **DWT サイクルカウンタ**で計測する（kit の既定 `TX_EXECUTION_TIME_SOURCE` = `DWT->CYCCNT` @0xE0001004）。

```bash
cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-toolchain.cmake \
      -DBUILD_EXEC_PROFILE=ON
cmake --build build --target flash-exec_profile
```

3 秒ごとに配分を出力（worker_a は worker_b の 2.5 倍の仕事量）:

```
  worker_a :  273166163  (42%)
  worker_b :  109277261  (16%)
  ISR      :          0  ( 0%)
  idle     :  265319044  (40%)
```

要点:

- `-DBUILD_EXEC_PROFILE=ON` が `TX_EXECUTION_PROFILE_ENABLE` を定義し、ポート asm がプロファイルフックを呼ぶ。ThreadX コア+ポートを再ビルドし `tx_execution_profile.c` を追加。
- **Cortex-M7 の DWT ロック**：DWT Lock Access Register の解除（`*(uint32_t*)0xE0001FB0 = 0xC5ACCE55`）をしないと `CYCCNT` が 0 のまま＝全測定 0 になる。
- **ISR 時間は 0**：kit は ThreadX の context save/restore 経由で ISR 時間を数えるが、本ポートの `SysTick_Handler` は素の C ハンドラなので計上されない。ISR を計測するには本体を `_tx_execution_isr_enter()`/`_exit()` で囲む。
