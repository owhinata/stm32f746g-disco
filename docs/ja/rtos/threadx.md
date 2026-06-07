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

## アイドル省電力（WFI、`TX_ENABLE_WFI`）

`tx_user.h` で `TX_ENABLE_WFI` を定義しているため、ready スレッドが無いときポート asm のアイドルループ（`__tx_ts_wait`）は busy-spin せず `DSB; WFI; ISB` で **CPU を Sleep** させ、割込みが来るまで停止する。実利用ではシェルが UART RX 待ちで suspend、`led` も `tx_thread_sleep` でほぼアイドルのため、busy-spin を WFI に置き換えると消費電力が下がる。

Cortex-M7 の WFI Sleep は **CPU の命令実行だけを止め、HCLK/APB クロックは供給を継続**する（RM0385 Sleep mode）。これにより以下の起床・計時経路が成立する:

- **SysTick は止まらない**: SysTick は HCLK 源で Sleep 中も計数を続け、1 ms tick が進む（HAL tick / ThreadX tick とも）。よって `tx_thread_sleep` の満了でスレッドが起床する（`led` の 250 ms 点滅は継続）。
- **UART RX で wake**: USART1 RX は割込み駆動（[UART バックエンド](shell-backend-uart.md)、IRQ 優先度 5）。バイト到来で USART1 割込みが WFI から CPU を起こす（WFI の wake 判定は PRIMASK を無視する）。アイドルループは `CPSID i` で割込みを無効化してから WFI に入るが、pending になった割込みで wake し、`CPSIE i` 後に ISR が走って `CLI_EVT_RX` を set、シェルスレッドが復帰する。
- **計時前提（TIM2）**: 実行プロファイル（`thread` の cpu%、[thread コマンド](shell-thread.md)）の time source は `DWT_CYCCNT` ではなく **TIM2**。DWT は core クロックがゲートされる WFI Sleep 中に凍結し得るが、TIM2 は Sleep 中もクロック供給が続く（`TIM2LPEN` リセット値 = 1）ため、WFI 有効時でも cpu%/idle 計測が正しく保たれる。

!!! note "デバッグ時の注意（DBG_SLEEP）"
    OpenOCD の `target/stm32f7x.cfg` は examine 時に `DBGMCU_CR` の `DBG_SLEEP`/`DBG_STOP`/`DBG_STANDBY` を set し得る。`DBG_SLEEP` が立つとデバッグ接続中だけ Sleep でも core クロックが維持され、WFI/tick/消費電流の観測が偽陽性になる。本 firmware は `DBG_SLEEP` を焼かない方針なので、SWD で計測する際は `DBGMCU->CR`（`0xE0042004`）bit0 が 0 であることを確認する（立っていればクリアしてから測る）。`tx_thread_sleep` 起床・UART 応答・消費電流の最終確認は SWD 非接続で行うのが確実。

    逆に `DBG_SLEEP=0` の genuine WFI sleep 中は、オンボードの古い ST-Link（V2J28M16）では core を読めない（通常の `st-info --probe` / OpenOCD examine が失敗し、メモリ読みは `0x01010001` 等の garbage を返す）。attach するには connect-under-reset（`openocd ... -c "reset_config srst_only connect_assert_srst"` でリセット保持中に接続 → `reset halt`/`reset run`）するか、attach 後に `DBG_SLEEP` を立てる。この「眠っていると読めない」症状自体が genuine sleep の証拠でもある。

!!! note "非対象: `TX_LOW_POWER`（tickless）"
    SysTick を止めて深いスリープに入る tickless フレームワーク（`TX_LOW_POWER` + `tx_low_power.c`、ユーザ HW マクロ必須）は本 firmware では未採用（将来・任意）。本節は port マクロ `TX_ENABLE_WFI` 単体による浅い Sleep のみ。

## ビルド時の要点

- `common/src/*.c`（コア）+ `ports/cortex_m7/gnu/src/*.S`（コンテキストスイッチ等）をビルド。`tx_misra`（`.c`/`.S` とも）は重複定義のため**除外**。
- ポート `.S` は `tx_user.h` を `#include` するため、ASM にも include パスを通し `TX_INCLUDE_USER_DEFINE_FILE` を定義。

## ベンチマーク

CoreMark は単独イメージではなく **shell の `coremark` コマンド**として実行する（[CoreMark コマンド](shell-coremark.md)）。

!!! note "撤去したベンチ (thread_metric / exec_profile)"
    旧 `thread_metric`（ThreadX tick を 100 Hz にし `TX_DISABLE_*` 群を付ける）と `exec_profile`（`TX_EXECUTION_PROFILE_ENABLE` で ThreadX ポート asm を再ビルド）は、対話シェルが要求する ThreadX 構成（1 ms tick・エラーチェック有・asm オーバーヘッド無）と非互換で単一 firmware に同居できないため撤去した。必要なら git 履歴（〜`5078914`）から復元できる。
