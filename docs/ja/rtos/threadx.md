# RTOS (Eclipse ThreadX)

ファームウェア（CMake ターゲット `threadx`）は上流 **Eclipse ThreadX**（MIT、`lib/threadx` submodule、Cortex-M7/GNU ポート）上で対話シェルを動かす。`tx_application_define()` が生成するスレッド:

- shell インスタンススレッド（`vcp_sh`、優先度 16）— USART1 VCP 上の対話 CLI（[Shell アプリ](shell-app.md)）
- `led`（優先度 10）— LD1（PI1）を 250 ms ごとにトグルするハートビート

## 統合（`port/threadx/`）

- `tx_glue.c`: `_tx_initialize_low_level()` と `SysTick_Handler` を提供。単一の 1 ms SysTick で HAL tick（`HAL_IncTick`）と ThreadX tick（`_tx_timer_interrupt`）の両方を駆動。ThreadX の `PendSV_Handler` はポート asm が定義するため、ファームウェアは `src/stm32f7xx_it.c` を持たない（SVC は本 firmware では未使用、その他ベクタは startup の weak `Default_Handler` が受ける）。
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

## ベンチマーク

CoreMark は単独イメージではなく **shell の `coremark` コマンド**として実行する（[CoreMark コマンド](shell-coremark.md)）。

!!! note "撤去したベンチ (thread_metric / exec_profile)"
    旧 `thread_metric`（ThreadX tick を 100 Hz にし `TX_DISABLE_*` 群を付ける）と `exec_profile`（`TX_EXECUTION_PROFILE_ENABLE` で ThreadX ポート asm を再ビルド）は、対話シェルが要求する ThreadX 構成（1 ms tick・エラーチェック有・asm オーバーヘッド無）と非互換で単一 firmware に同居できないため撤去した。必要なら git 履歴（〜`5078914`）から復元できる。
