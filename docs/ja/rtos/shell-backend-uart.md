# Shell UART(VCP) backend（USART1 IRQ 駆動 RX/TX）

shell の[コア](shell-core.md)が定義する `struct cli_transport_api` を、**STM32 HAL の
UART を割込み(IT)モードで駆動**して実装する最初の実 backend（[dummy backend](shell-testing.md)
の次）。ST-Link 仮想 COM ポート（VCP, USART1, PA9=TX / PB7=RX, 115200 8N1）上で実機対話を可能にする。

クロック / GPIO / ボーレートの初期化は **行わない**。ボード起動で既に立ち上がっている
ハンドル（`src/bsp.c` の `VCP_UART_Init` → `huart1`）を再利用し、その上に**割込み駆動の
RX/TX リングバッファ**を載せるだけ。クリーンルーム設計（コード非流用）。

## transport_api マッピング

| op | 実装（`shell/backend/cli_backend_uart.c`） |
|---|---|
| `init` | リング/カウンタ初期化、`tr->sh` を退避、`g_uart_console` を自分に設定（`enabled=0`） |
| `enable` | `USART1_IRQn` を優先度 5 で NVIC 有効化 → `HAL_UART_Receive_IT(huart,&rx_byte,1)`。**HAL_OK のときだけ `enabled=1` で 0 返却**、失敗は NVIC を戻して非 0（コアが当該スレッドを落とす, §9） |
| `write` | TX リングへ受理可能分を投入し受理数(0..len)を返す（満杯=0）。投入後 `kick_tx()`。**非ブロッキング**（§11 のブロッキングはコア側） |
| `read` | RX リングを非ブロッキング drain（0..cap） |
| `uninit` | `HAL_UART_Abort()`（ブロッキング, READY 復帰）→ `enabled=0` → NVIC 無効化 |

## RX/TX リングと並行性

リング本体（`shell/backend/cli_uart_ring.h`）は **HAL/ThreadX 非依存のロックフリー helper**で、
ホストで単体テストできる（[テスト](shell-testing.md)）。並行性は backend 側が持つ。

- **RX**: producer = USART1 ISR、consumer = shell スレッド（`read()`）の **SPSC**。`volatile`
  head/tail のみで lock 不要。リング満杯時は drop し `sh->rx_dropped` を加算（dummy と同じ §9/§15）。
- **TX**: producer = shell スレッド（`write()`）と printf の `_write` の **2 系統**、consumer =
  TxComplete ISR。head 前進・in-flight 判定/開始を **PRIMASK クリティカルセクション**で原子化。

`kick_tx()`: in-flight でなければ tail から**バッファ末尾までの連続 run**を `HAL_UART_Transmit_IT`
で送出。**HAL_OK のときだけ `tx_in_flight=1`/`tx_chunk=run` を確定**（`tx_chunk` は `uint16_t`、
HAL の `Size` も `uint16_t`）。HAL_BUSY/ERROR ならバイトはリングに残し次回再試行。wrap した残りは
TxComplete が次 chunk として送る。`write()`・`_write`・TxComplete が同じ `kick_tx()` を通るので
HAL 戻り値処理は 1 箇所。

## ISR / HAL コールバック

HAL コールバックはハンドルしか受け取らないので、`g_uart_console`（単一 USART1 コンソール）で
コンテキストへ解決する（`huart != u->huart` は無視）。

| 関数 | 役割 |
|---|---|
| `USART1_IRQHandler` | `g_uart_console`/`huart` を NULL guard 後 `HAL_UART_IRQHandler(huart)` |
| `HAL_UART_RxCpltCallback` | `rx_byte` をリングへ put（満杯=drop+計数）→ 1 バイト RX 再 arm（失敗は `rx_rearm_fail++`）→ `cli_transport_notify_rx(sh)` |
| `HAL_UART_TxCpltCallback` | tail を `tx_chunk` 前進 → `tx_in_flight=0` → `kick_tx()` で次 chunk → `cli_transport_notify_tx(sh)`（空き通知） |
| `HAL_UART_ErrorCallback` | ORE なら `rx_overrun++`。`RxState==READY`（ORE/RTO で RX 停止）なら RX 再 arm。FE/NE/PE は受信継続のため再 arm しない |

`USART1_IRQHandler` は backend モジュール内に置く。`threadx` ターゲットは PendSV 衝突回避のため
`src/stm32f7xx_it.c` を除外しており、ISR を backend に持たせると shell をリンクするビルドでのみ
ベクタが定義される（既存 demo に影響しない）。

## 割込み優先度と ThreadX

本リポジトリの ThreadX Cortex-M7 port は **PRIMASK でクリティカルセクションを保護**する
（`TX_PORT_USE_BASEPRI` 未定義, `port/threadx/tx_glue.c`）。よって ISR から
`cli_transport_notify_rx/tx`（= `tx_event_flags_set`）を呼ぶのは **どの NVIC 優先度でも安全**
（ISR が ThreadX のクリティカルセクションを横取りできない）。USART1 優先度 **5**（SysTick=14 /
PendSV=15 より緊急）は **エコー遅延 / overrun 耐性のチューニング**であり、ThreadX 安全性の制約
ではない。backend 独自の PRIMASK 区間は ThreadX の `TX_DISABLE/RESTORE` と入れ子になっても安全。

## printf / `_write` 共存（単一 TX 所有者）

`src/bsp.c` の `_write`（ブロッキング polling）を **weak** 化し、本 backend が **strong** `_write`
を提供して上書きする（shell をリンクするビルドのみ）。

- コンソール有効後（`g_uart_console && enabled`）: printf を shell と**同じ TX リング**へ投入
  （満杯時は有界スピン、TX ISR が裏で drain）→ USART1 の TX 所有者を 1 本化。
- 有効前（起動直後の boot ログ等、`g_uart_console==NULL` / `!enabled`）: `huart1` へ
  `HAL_UART_Transmit` polling。IT TX が未 arm なので衝突しない。
- 本 backend をリンクしない demo（blink / coremark / threadx / thread_metric / exec_profile）は
  weak の polling `_write` のまま**従来動作**。

## 既知の制約

- **HW overrun (ORE)**: `HAL_UART_Receive_IT(1)` は 1 バイト完了ごとに RXNEIE/EIE を一旦 disable し
  callback を呼ぶため、callback 内で再 arm するまで RXNE が空く。8N1 は 10 bit/frame で
  **約 86.8 µs/byte**。優先度 5 の ISR 再 arm は十分速いが、割込みマスク区間が長引くバースト時は
  ORE が起こり得る。DoD の「取りこぼし無し」は **software リング容量内**の話で、HW ORE は
  `ErrorCallback` で `rx_overrun` として計数する。連続高速受信が要件化すれば **RXNE 割込み直叩きで
  RDR を読む方式**（1-byte 再 arm 窓を無くす）へ切替可能。

## 設定（`shell/include/cli_config.h` と併せて上書き可）

| マクロ | 既定 | 意味 |
|---|---|---|
| `CLI_UART_RX_BUFFER_SIZE` | 256 | RX リング深さ（実効 size-1 バイト） |
| `CLI_UART_TX_BUFFER_SIZE` | 512 | TX リング深さ（実効 size-1 バイト） |

## 使い方（#8 で結線）

```c
CLI_BACKEND_UART_DEFINE(vcp_tr, &huart1);          /* transport + ctx */
CLI_INSTANCE_DEFINE(vcp_sh, &vcp_tr, "uart:~$ ");  /* instance + stack */
cli_init(&vcp_sh);   /* backend init → event flags → mutex */
cli_start(&vcp_sh);  /* tx_thread_create（auto-start） */
```

shell をライブラリ化した `shell` アプリ本体・`flash-shell` ターゲット・dummy 2nd instance は #8。

## 検証

- **ホスト単体テスト**: `sh shell/test/run_host_tests.sh` がリング helper の
  fill/drain/wrap/overflow/contig を assert（HAL/ThreadX 不要）。
- **実機 verify**（#7 では一時配線, commit せず）: `app_threadx.c` へ一時的に backend を結線して
  `flash-threadx` → `picocom -b 115200 /dev/ttyACM0` で プロンプト表示・コマンドエコー・
  長行/連続貼付けでリング容量内の取りこぼし無し・boot ログの文字化け無しを確認。
