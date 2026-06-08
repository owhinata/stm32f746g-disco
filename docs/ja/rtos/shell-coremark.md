# CoreMark コマンド（`coremark`）

EEMBC **CoreMark** ベンチを shell の `coremark` コマンドとして実行する。かつての単独ベアメタル
`coremark` イメージを置き換え、ファームウェア（`threadx`）に統合した。アプリは別イメージではなく
shell から起動する、という方針の最初の例。

```text
sh> coremark
Running CoreMark (auto-calibrated, ~12s)...
2K performance run parameters for coremark.
CoreMark Size    : 666
Total ticks      : 11800
Total time (secs): 11.800000
Iterations/Sec   : 932.203390
Iterations       : 11000
...
CoreMark 1.0 : 932.203390 / GCC13.3.1 ... / STATIC
```

スコアは構成依存だが、216 MHz・GCC 13.3・`-O3` で **≈ 928〜932（≈ 4.3 CoreMark/MHz）**。
最終行の `/ STATIC` はメモリ法（下記）の表記で、スコア自体はメモリ法に依存しない。

## ビルド統合（`coremark_obj`）

CoreMark 一式は `shell_obj` と同様に **OBJECT ライブラリ `coremark_obj`** にまとめ、`threadx` exe に
リンクする（[Shell アプリ](shell-app.md)）。

| 項目 | 値 | 理由 |
|---|---|---|
| ソース | `lib/coremark/core_*.c` + `port/coremark/core_portme.c` | upstream CoreMark + ボード port |
| 最適化 | `-O3 -funroll-loops` | 正準スコアを得るための CoreMark 標準ビルド（`threadx` 本体は `-O2`） |
| `MEM_METHOD=MEM_STATIC` | データブロック（`TOTAL_DATA_SIZE=2000`）を **静的 `.bss`** へ | shell スレッドのスタックに載せない／`malloc` を引き込まない |
| `main` → `coremark_main` | `core_main.c` のみ `-Dmain=coremark_main` | firmware の `main()`（`src/main.c`）との衝突回避 |
| `-u _printf_float` | `threadx` のリンクに付与 | CoreMark のスコア行が `%f`（float printf）を使うため |

`cmd_coremark.c` は `coremark_main()` を宣言して呼ぶだけ:

```c
int coremark_main(void);   /* core_main.c を -Dmain=coremark_main でビルド */

static int cmd_coremark(struct cli_instance *sh, int argc, char **argv) {
    cli_info(sh, "Running CoreMark (auto-calibrated, ~12s)...\r\n");
    coremark_main();        /* 正準レポートを printf -> VCP で出力 */
    return 0;
}
CLI_CMD_REGISTER(coremark, NULL, "run the EEMBC CoreMark benchmark (~12s)",
                 cmd_coremark, 1, 0);
```

## 実行モデル

- **前景・同期実行**: 呼び出した shell インスタンススレッド（優先度 16）内で `coremark_main()` が走る。
  auto-calibration で **約 12 秒**かかり、その間プロンプトはブロックする（現状 Ctrl+C キャンセルは未対応、#16 予定）。
- **LED は止まらない**: `led` ハートビート（優先度 10）が CoreMark 実行中も preempt して 250 ms 点滅を続ける。
- **スタック**: `MEM_STATIC` で大きいデータは `.bss` に置くため、スレッドスタックの消費は
  `coremark_main` のコールフレーム + `%f` 整形程度。安全マージンとして `threadx` ターゲットでは
  shell インスタンススタックを **4096 B** にしている（`CLI_INSTANCE_STACK_SIZE=4096`、既定の 2048 から増量）。
  実行後に [`thread`](shell-thread.md) コマンドでスタック使用量に余裕があることを確認できる。
- **計時**: `HAL_GetTick()`（1 ms）。ThreadX 下でも SysTick が `HAL_IncTick` と
  `_tx_timer_interrupt` の両方を駆動する（[ThreadX 統合](threadx.md)）ため正確。
- **出力**: CoreMark の `ee_printf` → `printf` → UART backend の strong `_write` → IRQ TX ring。
  `_write` は呼び出しスレッドの所有 shell インスタンスを解決する（#18、[UART backend](shell-backend-uart.md)）。
  ベンチマークは shell スレッドで同期実行されるため、レポートは `coremark` を起動した端末に追従する
  （coremark 側の変更は不要）。単一スレッドの前景実行なので並行出力は起きず、計測区間
  （`start_time()`〜`stop_time()`）内には出力が無いので TX の背圧はスコアに影響しない。

!!! note "改行 (LF→CRLF)"
    CoreMark のレポートは仕様どおり `\n` 改行だが、UART backend の `_write` が printf 出力中の
    bare `\n` を `\r\n` に変換する（[UART backend](shell-backend-uart.md)）。そのため
    `picocom --imap lfcrlf` のような端末側変換が無くても段つきにならない。shell の `cli_print` は
    元々 `\r\n` 出力で `_write` を通らないため影響しない。

## キャンセル不可（#16）

`coremark` は実行中に `Ctrl+c` で**中断できない**。`coremark_main()` は read-only submodule への
単一ブロッキング呼び出しで応答ポイントが無く、出力も shell の `cli_tx_send_blocking` ではなく
`printf`/`_write` 経由のため、協調キャンセル（`cli_cancel_requested` / TX 律速の RX 起床）が効かない。
実行前メッセージにも `not interruptible` と明示する。Ctrl+c は次のプロンプト用に queue されるだけ。

## ライセンス

`lib/coremark`（EEMBC、Apache-2.0）と `port/coremark`（barebones port 由来）はそのまま流用。
`shell/cmds/cmd_coremark.c` のグルーは MIT（clean-room）。
