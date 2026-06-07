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
name                 state  pri  runs   size  peak  free use%
cli                  event   16    12   2048   612  1436  29%
led                  sleep   10  1234   1024   312   712  30%
System Timer Thread  susp     0     1   1024   180   844  17%
```

| 列 | 意味 | 取得元 |
|---|---|---|
| name | スレッド名 | `tx_thread_name` |
| state | 実行状態（下表） | `tx_thread_state` |
| pri | 現在の優先度（0–31） | `tx_thread_priority` |
| runs | ディスパッチ（CPU に載って実行開始）された累計回数 | `tx_thread_run_count` |
| size | スタック総量（バイト） | `tx_thread_stack_size` |
| peak | スタック**高水位**使用量（バイト） | 0xEF スキャン（後述） |
| free | 余裕（`size − peak`） | 同上 |
| use% | `peak / size` の百分率 | 同上 |

実機の shell では `cli`（VCP シェルインスタンス, prio=`CLI_INSTANCE_PRIORITY`）/ `led`（prio 10）/
`System Timer Thread`（ThreadX のソフトタイマスレッド）が並ぶ。後者は shell ビルドが
`TX_TIMER_PROCESS_IN_ISR` を定義しないため生成され、列挙対象に含まれる。

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
  - `thread` で `cli` / `led` / `System Timer Thread` が state/pri/runs/size/peak/free/use% 付きで表示。
    稼働スレッドの `runs` が時間経過で増加し、`peak < size`、`led` は `size=1024` と整合。
  - Tab 補完で `thr` → `thread`。`thread x` は usage/arg エラー。
