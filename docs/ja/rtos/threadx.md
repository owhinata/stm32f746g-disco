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
