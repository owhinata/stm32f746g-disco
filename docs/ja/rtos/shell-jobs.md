# バックグラウンド実行とジョブ制御（`&` / `jobs` / `kill`）

issue #25。コマンド行末の `&` で**コマンドをバックグラウンド実行**し、`jobs` / `kill %N` で最小限の
ジョブ制御を行う。#23 の `;`（同期・構文的なセグメント逐次実行）とは別物で、**並行実行**を要する。
基盤は #18（printf を呼び出し元端末へルーティング）と #16（協調キャンセル）。

## 使い方

```
sh> sleep 30 &        # [1] sleep 30  と出てプロンプトが即戻る
sh> jobs              # [1] Running    sleep 30
sh> kill %1           # [1] kill requested  → 次の Enter で [1] Done sleep 30
sh> coremark &        # 重いコマンドも bg 実行（対話プロンプトは応答し続ける）
sh> sleep 20 & ; sleep 20 &   # 別 id で 2 ジョブ並行
```

- **`cmd &`**: 行末（引用の外側・非エスケープ）の単独 `&` を検出するとそのコマンドを worker スレッドで
  実行し、ディスパッチャは即プロンプトへ戻る。`echo "a&"` / `echo a\&` の `&` は対象外。`&&`（論理 and）は
  非対応で bg 化しない。`&` はセグメント末尾のみ（行中の複数 `&` は非対象）。
- **`jobs`**: 実行中ジョブを `[id] Running cmd` で一覧。完了済みジョブはここで回収・通知される。
- **`kill %N`**: id `N` のジョブに**協調キャンセル**を要求（先頭 `%` は任意）。

## 実行モデル

1 ジョブ = 専用 `struct cli_instance`（固定プール `CLI_MAX_BG_JOBS`、既定 2）の worker スレッド。
ハンドラ signature は対話シェルと同一なので**既存コマンドが無改造で bg 動作**する。各 worker は

- **出力**: 起動元 foreground（fg）の `tx_lock` を共有し、`tr` も `fg->tr` をエイリアスする。bg の
  `cli_print` も `printf`（`_write`）も fg の出力ロック経由で直列化され、fg の端末へ届く（#18/#25）。
- **キャンセル**: fg の RX リング（厳密 SPSC・単一 consumer）には**一切触れない**。`kill %N` が worker の
  `cancel_req` を立て、worker 自身の event group で起床させる。協調キャンセルなので、`cli_sleep` や
  `cli_cancel_requested()` をポーリングするコマンド（`sleep`/`watch` 等）は止まるが、**非協調コマンド
  （`coremark`/`usleep`）は止まらず最後まで走る**（#16 と同じ制約）。
- **優先度**: `CLI_BG_JOB_PRIORITY = CLI_INSTANCE_PRIORITY + 1`（既定 17）。fg（16）より 1 段低いので、
  `coremark &` のような CPU 占有ジョブでも**対話入力が常に preempt** し、プロンプトは応答し続ける。
- **ライフサイクル**: worker はハンドラ実行後に return → `TX_COMPLETED`。自スレッドは自分を delete
  できないので、**fg スレッドが遅延回収**する（`cli_jobs_reap`：`tx_thread_state` を確認し COMPLETED の
  worker のみ `tx_thread_delete`、`[id] Done cmd` を fg 印字、slot 解放）。回収は Enter ごと・`jobs`・`kill`
  で走る。**ジョブ id は単調増加**で、`kill %N` は `{id 一致 かつ実行中}` のみ操作するため、完了 slot が
  再利用されても古い `kill %N` が新ジョブを誤って止めることはない。

## 出力の見え方（既知の制限）

bg ジョブが出力すると、fg の `tx_lock` 下で**行頭に改行を入れてからブロックとして**出力し、fg の入力行
描画を dirty 化する。次のキー入力時に既存の行エディタがプロンプト＋入力中の行を**その下へ再描画**する。

- 入力行は事前消去しないため、半入力行が bg 出力の上に残り下に再描画される（**二重表示**）。TX リングは
  byte 単位で安全なので**文字化け（破壊）はしない**が、表示が一時的に乱れることがある（次キーで是正）。
  これは行ブロック方式の意図した妥協（erase/redraw 統合は非ゴール）。
- 完了通知 `[id] Done` は worker でなく **fg スレッドが Enter 時に印字**するので、プロンプトと自然に直列化する。

## 設定（`cli_config.h`）

| ノブ | 既定 | 意味 |
|---|---|---|
| `CLI_MAX_BG_JOBS` | 2 | 同時 bg ジョブ数（worker プール）。超過は起動拒否 |
| `CLI_BG_JOB_STACK_SIZE` | =`CLI_INSTANCE_STACK_SIZE`（exe は 4096） | worker スタック。`coremark &` も動く |
| `CLI_BG_JOB_PRIORITY` | `CLI_INSTANCE_PRIORITY+1`（17） | worker 優先度（fg より低く starve させない） |
| `CLI_BG_TX_POLL_TICKS` | 2 | TX 満杯時の kill 待ちスライス（総期限は `CLI_TX_TIMEOUT`） |
| `CLI_THREAD_MAP_MAX` | `CLI_MAX_INSTANCES+CLI_MAX_BG_JOBS` | thread→instance レジストリ（printf ルーティング） |

## 非ゴール

パイプ `|`、`&&`/`||`、リダイレクト、行中の複数 `&`、`fg`/`bg`/`wait` 完全版、`Ctrl+Z` サスペンド、
erase/redraw 統合。

## 関連

- 出力ルーティング・printf 端末追従: #18（[出力 API](shell-output.md) / [UART backend](shell-backend-uart.md)）
- 協調キャンセル `cli_cancel_requested` / `cli_sleep`: #16（[行編集](shell-editing.md)）
- `;` 逐次実行と `&` 検出の引用尊重: #23（[パーサ](shell-parser.md)）
- 長時間コマンドの bg 化ユースケース: `watch ... &`（[遅延・監視](shell-sleep-watch.md)）
