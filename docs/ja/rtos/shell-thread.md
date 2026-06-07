# スレッド情報コマンド（`thread`）

M4「組込みコマンド」の #13。`version`/`uptime`/`reboot`（[システム組込みコマンド](shell-builtins.md)）に続き、
ThreadX の全スレッドを **1 つの表**にまとめて表示する読み取り専用の診断コマンド `thread` を追加する。
`shell/cmds/cmd_thread.c` にあり、`cmd_system.c` と同じく **exe にのみ**リンクされる（ホストテストの
登録集合に影響しない）。スタックサイズ調整やデッドロック調査に使う。

## コマンド

| コマンド | 登録 | 動作 |
|---|---|---|
| `thread` | `CLI_CMD_REGISTER(thread, NULL, ..., cmd_thread, 1, 0)` | 全スレッドの state / priority / run count とスタック使用量を 1 表で表示 |

サブコマンドは持たない（リーフ 1 個）。`thread` 単体で動作し、`thread <arg>` は引数過多
（mandatory=1 / optional=0）で usage エラーになる。`thread` 渡された `sh` のみを出力 API 経由で
触るため、複数インスタンス同時実行でも再入安全（§10）。

```text
sh> thread
name                 state  pri   runs   size  peak  use%   cpu%
cli                  susp    16     14   2048   612   29%   0.0%
led                  sleep   10   1240   1024   312   30%   0.1%
System Timer Thread  susp     0      1   1024   180   17%   0.0%
(idle)                                                      99.5%
(isr)                                                       0.4%
```

| 列 | 意味 | 取得元 |
|---|---|---|
| name | スレッド名 | `tx_thread_name` |
| state | 実行状態（下表） | `tx_thread_state` |
| pri | 現在の優先度（0–31） | `tx_thread_priority` |
| runs | ディスパッチ（CPU に載って実行開始）された累計回数 | `tx_thread_run_count` |
| size | スタック総量（バイト） | `tx_thread_stack_size` |
| peak | スタック**高水位**使用量（バイト） | 0xEF スキャン（後述） |
| use% | `peak / size` の百分率 | 同上 |
| cpu% | 直近 `thread` 実行からの窓での CPU 占有率（top 風） | Execution Profile Kit（後述、issue #19） |

末尾の `(idle)` / `(isr)` は擬似行で、アイドル余力と割込み処理の cpu% を示す（合計が ~100% になる）。
余裕 `free`（= `size − peak`）は冗長なため列からは外したが、内部では peak 算出に使う（後述）。

実機の shell では `cli`（VCP シェルインスタンス, prio=`CLI_INSTANCE_PRIORITY`）/ `led`（prio 10）/
`System Timer Thread`（ThreadX のソフトタイマスレッド）が並ぶ。後者は shell ビルドが
`TX_TIMER_PROCESS_IN_ISR` を定義しないため生成され、列挙対象に含まれる。

## CPU 使用率（cpu%）

`cpu%` 列は各スレッドが**前回 `thread` 実行から今回まで**の窓で消費した CPU 時間の割合（`top` 風）。
末尾の `(idle)`（アイドル余力）/ `(isr)`（割込み処理）擬似行を含め、列の合計はおおむね 100% になる。

### 計測機構: ThreadX Execution Profile Kit + TIM2

- `port/threadx/tx_user.h` で `TX_EXECUTION_PROFILE_ENABLE` / `TX_CORTEX_M_EPK` を定義し、ThreadX 同梱の
  Execution Profile Kit（`lib/threadx/utility/execution_profile_kit/`）を有効化。コンテキストスイッチ毎に
  各スレッドの busy 時間を 64bit 累積する（port asm のフックが自動で効く）。
- **時間ソースは `TIM2->CNT`**（APB1・32bit・TIM2CLK = 2×PCLK1 = 108 MHz、`src/bsp.c` の `exec_timebase_init()`）。
  kit 既定の DWT_CYCCNT は WFI スリープ中にコアクロックがゲートされ凍結するが、TIM2 は Sleep 中も計数
  （TIM2LPEN 初期=1）するため、将来 WFI 省電力（[#20](https://github.com/owhinata/stm32f746g-disco/issues/20)）を
  有効化しても idle/cpu% が破綻しない。
- **ISR 計測**: 実 ISR（`SysTick_Handler` / `USART1_IRQHandler`）は素の HAL C ハンドラで ThreadX の
  context save/restore asm を通らないため、kit の `_tx_execution_isr_enter/exit` を**両ハンドラで明示 wrap**する。
  `TX_CORTEX_M_EPK` 必須（この port の `TX_THREAD_GET_SYSTEM_STATE()` は IPSR を OR するため、非 EPK パスの
  判定では ISR 時間を取りこぼす）。enter/exit は PRIMASK で保護し、ネスト（USART1 prio5 が SysTick prio14 を
  プリエンプト）でも kit の集計（nest counter / 64bit 合計の RMW）が壊れないようにする。
- 計測開始は最初のアプリスレッド `led` の冒頭（`tx_glue_profile_enable()`）から。これは
  `_tx_execution_initialize()` 完了後の確定点で、TIM2 起動前の読みや EPK 初期化前の呼び出しを避ける。

### cpu% の算出（差分窓）

`cmd_thread.c` は前回 `thread` 実行時の各スレッド累積時間＋グローバル合計（thread/isr/idle）を static に保持し、
今回との差分で算出する:

```text
cpu%(スレッド) = Δthread / 窓 ,   窓 = Δ(全スレッド) + Δisr + Δidle
```

- **初回**は前回値が無いので全行 `--`。以降は非ブロッキング（`thread` を 2 回叩いた間隔が窓）。
- 64bit 値は非アトミックなため、各スレッド累積は短い `TX_DISABLE` で読み、グローバル合計は head/count と同じ
  クリティカルセクションでスナップ。Δ<0 は 0 に、>100.0% は 100.0% にクランプ、窓=0 は `--`。
- `runs`（ディスパッチ回数）とは別物で、cpu% が実 CPU 時間の占有率を表す。

## スタック使用量の算出（0xEF 高水位スキャン）

ThreadX は `tx_thread_create()` 時、`TX_DISABLE_STACK_FILLING` を**定義しない限り**スタック全体を
`TX_STACK_FILL`（`0xEFEFEFEF`）で塗る（`lib/threadx/common/src/tx_thread_create.c`）。shell ターゲットは
これを定義しないため（定義するのは `thread_metric` ベンチのみ）、全スタックは生成時に `0xEF` で初期化済。

Cortex-M はスタックが**下方成長**する。ThreadX のスタック表現は:

- `tx_thread_stack_start` = バッファ先頭 = **最低位**アドレス
- `tx_thread_stack_end` = `start + size − 1` = **最高位**アドレス

スタックは `end`（高位）→`start`（低位）へ伸びるため、未使用域は `start` 側に `0xEF` のまま残る。
よって **free = `stack_start` から連続する `0xEF` バイト数**、**peak = `size − free`** で高水位を求められる。
これは ThreadX 自身の `tx_thread_stack_analyze()` と同じ方式で、`thread` 実行時のみ走る
（定常オーバーヘッド 0）。`TX_ENABLE_STACK_CHECKING` は不要。

!!! note "high-water であり瞬時値ではない / best-effort"
    `peak` は**過去最大**の使用量で、コマンド実行時点の瞬間値ではない（実行中スレッドの瞬時 SP は
    コンテキストスイッチ時の保存値で古くなり得るため採用しない）。また使用済み領域の境界バイトが
    偶然 `0xEF` だと数バイト過小評価しうる（ThreadX の analyze と同種の best-effort 制約）。
    本コマンドは既定のスタックフィルに依存するため、**`TX_DISABLE_STACK_FILLING` を付けてビルドしない**こと。

## スレッド列挙

ThreadX の created リスト（**循環双方向リスト**）の先頭 `_tx_thread_created_ptr` と総数
`_tx_thread_created_count` を `extern` 宣言し、`tx_thread_created_next` を **件数回**たどる
（NULL 終端ではなく循環なので件数でループ）。内部ヘッダ `tx_thread.h` は include せず、必要な
グローバル 2 つだけ自前 extern する（参照フィールドはすべて public な `TX_THREAD` typedef にある）。

本 FW はスレッドを `tx_application_define()` で一度だけ生成し動的削除しないため created リストは静的。
head+count を `TX_DISABLE`/`TX_RESTORE` で短くスナップショットしてから反復する（`cli_print` は mutex を
待つので**ロック保持中には出力しない**）。

## 状態名マッピング

`tx_thread_state`（`tx_api.h`、0..14）を短いラベルに変換する:

| 値 | define | 表示 | 値 | define | 表示 |
|---|---|---|---|---|---|
| 0 | TX_READY | `ready` | 8 | TX_BLOCK_MEMORY | `block` |
| 1 | TX_COMPLETED | `compl` | 9 | TX_BYTE_MEMORY | `byte` |
| 2 | TX_TERMINATED | `term` | 10 | TX_IO_DRIVER | `io` |
| 3 | TX_SUSPENDED | `susp` | 11 | TX_FILE | `file` |
| 4 | TX_SLEEP | `sleep` | 12 | TX_TCP_IP | `tcpip` |
| 5 | TX_QUEUE_SUSP | `queue` | 13 | TX_MUTEX_SUSP | `mutex` |
| 6 | TX_SEMAPHORE_SUSP | `sem` | 14 | TX_PRIORITY_CHANGE | `pchg` |
| 7 | TX_EVENT_FLAG | `event` | — | 範囲外 | `?` |

## 検証

- **ビルド**: `cmake --build build`（`shell` が通る／既存 demo 非破壊）。
- **実機**（`/dev/ttyACM0` @115200 8N1）:
  - `help` に `thread` が並ぶ。
  - `thread` で `cli` / `led` / `System Timer Thread` が state/pri/runs/size/peak/use%/cpu% 付きで表示。
    稼働スレッドの `runs` が時間経過で増加し、`peak < size`、`led` は `size=1024` と整合。
  - `thread` を 2 回叩く: 初回の cpu% は `--`、2 回目は差分窓で現実的な値。アイドル時は `(idle)` が支配的
    （~99%）、スレッド cpu% は小さく、cpu% 列の合計（スレッド＋`(idle)`＋`(isr)`）は ~100%。
  - Tab 補完で `thr` → `thread`。`thread x` は usage/arg エラー。
