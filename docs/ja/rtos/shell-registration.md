# Shell コマンド登録基盤

ThreadX Shell（対話型 CLI、Epic）の土台。コマンドを各ソースファイルから**宣言的に登録**し、起動時にリンカセクションを走査して列挙する仕組み。Zephyr shell の**設計のみ参考**にした clean-room 実装で、コードは流用していない。

公開ヘッダは `shell/include/cli.h`、構成ノブは `shell/include/cli_config.h`。新規 shell ファイルは MIT（`SPDX-License-Identifier: MIT` / `Copyright (c) 2026 ThreadX Shell Project`）。

!!! note "本ページの範囲"
    ここで扱うのはコマンド**登録**のみ。パーサ・コア・出力 API・行編集・組込みコマンドは後続タスクで実装する。全体アーキ概観は別途まとめる。

## リンカセクション `.shell_root_cmds`

各 root コマンドは `const struct cli_cmd` として専用セクション `.shell_root_cmds` に置かれ、境界シンボル `__cli_root_cmds_start` / `__cli_root_cmds_end` の間を**配列として走査**する。`ldscript/STM32F746NGHx_FLASH.ld` の `.rodata` 直後に追加（read-only テーブルなので FLASH）。

```ld
.shell_root_cmds (READONLY) :
{
  . = ALIGN(4);
  PROVIDE_HIDDEN (__cli_root_cmds_start = .);
  KEEP (*(SORT_BY_NAME(.shell_root_cmds.*)))
  KEEP (*(.shell_root_cmds))
  PROVIDE_HIDDEN (__cli_root_cmds_end = .);
  . = ALIGN(4);
} >FLASH
```

- `KEEP` … `-Wl,--gc-sections`（本リポ既定 ON）でエントリが破棄されないよう保持。エントリ側も `used` 属性を付ける。
- `SORT_BY_NAME` … 登録順をアルファベット決定的にする（リンク順依存を排除）。
- `aligned(__alignof__(struct cli_cmd))` … 各エントリのセクションアラインを型の自然アラインに固定し、エントリ間にパディングを入れさせない（＝ポインタ算術で配列走査できる）。
- 共有 ldscript なので全アプリに `.shell_root_cmds` が入るが、コマンド未登録なら**空セクション（start==end）**で無害。

## 登録マクロ

```c
#include "cli.h"

static int cmd_version(struct cli_instance *sh, int argc, char **argv) { /* ... */ return 0; }

/* サブコマンド木: thread { list, stacks } */
CLI_SUBCMD_SET_CREATE(sub_thread,
    CLI_CMD_ARG(list,   NULL, "list threads",     cmd_thread_list,   1, 0),
    CLI_CMD_ARG(stacks, NULL, "show stack usage", cmd_thread_stacks, 1, 0),
    CLI_SUBCMD_SET_END
);

/* root コマンド */
CLI_CMD_REGISTER(version, NULL,       "show firmware version", cmd_version, 1, 0);
CLI_CMD_REGISTER(thread,  sub_thread, "thread operations",     NULL,        2, 0);
```

| マクロ | 用途 |
|---|---|
| `CLI_CMD_REGISTER(name, subcmds, help, handler, mandatory, optional)` | root コマンドを `.shell_root_cmds` へ登録 |
| `CLI_SUBCMD_SET_CREATE(set_name, ...)` | サブコマンド配列を生成（末尾に `CLI_SUBCMD_SET_END`） |
| `CLI_CMD_ARG(name, subcmds, help, handler, mandatory, optional)` | set 内エントリ（引数数を明示） |
| `CLI_CMD(name, subcmds, help, handler)` | set 内エントリ（`mandatory=1, optional=0` 既定） |
| `CLI_SUBCMD_SET_END` | サブコマンド配列のセンチネル終端（`.name = NULL`） |

`name` は**素の C 識別子**で渡す（stringify して `.name` に格納し、セクション接尾辞 `.shell_root_cmds.<name>` とシンボル名生成に使う）。そのため `foo-bar` のようなハイフン入り名は登録できない。標準スコープのコマンド（`version`/`uptime`/`reboot`/`thread`/`devmem`/`help`/`clear`/`history`/`backends`）は全て識別子なので問題ない。

### コマンド記述子 `struct cli_cmd`

root コマンドとサブコマンドは同一型。

| フィールド | 意味 |
|---|---|
| `name` | コマンド名（`NULL` = サブコマンド木のセンチネル） |
| `help` | 1 行ヘルプ |
| `subcmds` | センチネル終端のサブコマンド配列 or `NULL` |
| `handler` | ハンドラ or `NULL`（純粋な親コマンド） |
| `mandatory` | 必須 argc（コマンド名含む） |
| `optional` | 許容 optional 引数数 |

ハンドラ型は `int (*)(struct cli_instance *sh, int argc, char **argv)`。`argv[0]` はコマンド名。戻り値 0 が成功、非 0 がエラー。argc/argv 検証（mandatory/optional）に失敗するとハンドラは呼ばれない（検証本体はパーサ側で実装）。

## 協調的キャンセル（Ctrl+C、#16）

1 インスタンス = 1 スレッドでハンドラを**同期実行**するため、走行中はそのスレッドが RX を drain
しない。`Ctrl+c`（`0x03`）で実行中コマンドを止めるには、ハンドラが**自分で応答ポイントを持つ**
（協調的キャンセル）。コアはハンドラスレッドを強制終了しない（mutex/ドライバ状態の破壊を避けるため、
要件 §9 で強制中断は標準スコープ外）。

公開 API（`cli.h`）:

- `bool cli_cancel_requested(struct cli_instance *sh)` — 実行中に `Ctrl+c` が押されたら `true`
  （sticky）。長ループ / 大量出力の境界で定期的に覗き、`true` なら速やかに非 0 で戻る。内部で RX を
  drain して `0x03` を探すので**スレッド文脈専用**（ISR から呼ばない）。
- `int cli_sleep(struct cli_instance *sh, unsigned ticks)` — キャンセル可能な遅延（**ThreadX tick**
  単位、ms ではない）。遅延満了で 0、`Ctrl+c` / 停止要求で非 0。`tx_thread_sleep` と違いイベントフラグ
  待ちなので RX で起床できる。`watch` / `sleep`（#21）の土台。

```c
static int cmd_long(struct cli_instance *sh, int argc, char **argv)
{
    for (int i = 0; i < n; i++) {
        if (cli_cancel_requested(sh))   /* 行 / 単位の境界で覗く */
            return 1;                   /* コアが ^C を表示しプロンプト復帰 */
        /* … 1 単位の処理 / 出力 … */
    }
    return 0;
}
```

メカニズム: 検出は既存の `CLI_EVT_RX` を wake 源に統一し、`0x03` はコア（スレッド文脈）で rx_ring を
drain して見つける（backend は dumb なバイトパイプのまま、新 event flag は追加しない）。大量出力が
TX 律速でブロックした場合は `cli_tx_send_blocking` が RX 起床して早期離脱する。実行中の type-ahead は
破棄。中断後は `^C` を表示してプロンプトへ復帰し、`last_result` は `CLI_DISPATCH_CANCELLED`。
標準コマンドでは `thread` / `devmem dump` が対応。`coremark` は単一ブロッキング呼び出し（read-only
submodule、出力も `printf` 経由）で応答ポイントが無く**キャンセル不可**。

## 走査

```c
extern const struct cli_cmd __cli_root_cmds_start[];
extern const struct cli_cmd __cli_root_cmds_end[];

CLI_ROOT_CMD_FOREACH(c) {        /* c は const struct cli_cmd * */
    /* c->name, c->help, c->subcmds, ... */
}
size_t n = cli_root_cmd_count();
```

## 構成ノブ `cli_config.h`（§8）

各ノブは `#ifndef` で囲まれ、ビルド時に上書きできる（例 `-DCLI_CMD_BUFFER_SIZE=512`）。本基盤では値の定義と sanity の `_Static_assert` のみ。各「超過時の扱い」の実行時挙動は後続タスクで実装する。

| ノブ | 既定 | 超過時の扱い |
|---|---|---|
| `CLI_CMD_BUFFER_SIZE` | 256 B | BEL を鳴らし以降の入力文字を無視 |
| `CLI_MAX_ARGC` | 20 | エラー表示・コマンド不実行 |
| `CLI_HISTORY_BUFFER_SIZE` | 512 B | 最古エントリから FIFO 破棄 |
| `CLI_PRINTF_BUFFER_SIZE` | 32 B | 満杯で flush |
| `CLI_PROMPT_BUFFER_SIZE` | 20 B | 切り詰め |
| `CLI_INSTANCE_STACK_SIZE` | 2048 B | — |
| `CLI_MAX_INSTANCES` | 4 | コンパイル時確定・超過はビルドエラー |
| `CLI_MAX_SUBCMD_DEPTH` | 8 | エラー表示・コマンド不実行 |

登録コマンド数はリンカセクション容量に依存（実質無制限・走査は線形）。全ストレージは静的確保で、実行時のヒープ確保は行わない。

## 検証（ホストテスト）

`shell/test/run_host_tests.sh` がホスト gcc 単体でビルド・実行する最小スモークテスト。`.shell_root_cmds` をホストリンカに供給する `host_sections.ld`（`INSERT AFTER .rodata`）を使い、コマンドを登録して走査し、**件数・連続配置・SORT 順・サブコマンド木のセンチネル終端**を assert する。

```bash
bash shell/test/run_host_tests.sh   # => host smoke test passed
```
