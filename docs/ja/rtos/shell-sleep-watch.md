# 遅延・監視コマンド（`sleep` / `usleep` / `watch`）

issue #21。遅延 2 種（`sleep`/`usleep`）と定点監視（`watch`）を追加する。`sleep` と `watch` は
#16 の協調キャンセル（`cli_sleep` / `cli_cancel_requested`）に乗って **Ctrl+C で中断**できる。

## `sleep N` — N 秒スリープ（中断可）

```
sh> sleep 3        # 約 3 秒ブロック。Ctrl+C で ^C を出して即復帰
```

- 引数は整数秒（MVP）。内部は `cli_sleep(sh, N*1000)`（1 tick = 1 ms）。
- `cli_sleep` はイベントフラグ待ちなので、待機中に `0x03` が来ると起床してキャンセルされ、ディスパッチャが `^C` を表示してプロンプトへ戻る（[コマンド登録](shell-registration.md) の「協調的キャンセル」）。
- 上限 `CLI_SLEEP_MAX_SEC`（既定 86400=1日）。範囲外・数値不正はエラー（非0終了）。

## `usleep N` — N マイクロ秒ビジーウェイト（**中断不可**）

```
sh> usleep 500     # 約 500us 待つ
```

- free-run の **TIM2**（108 MHz, #19 で起動）を使った `bsp_udelay()` のビジーウェイト（108 カウント = 1us）。
- **CPU を占有し yield しない＝中断不可**。割込み（SysTick / UART）は動くので ThreadX tick・LED・上位スレッドは止まらないが、シェルスレッドはその間ブロックする。
- 上限 `CLI_USLEEP_MAX_US`（既定 10000=10ms）。長い遅延は `sleep` を使う。範囲外・不正はエラー。

## `watch [-n SEC] CMD...` — CMD を繰り返し実行

```
sh> watch -n 1 thread     # 1 秒間隔で thread を再実行(画面クリア)。Ctrl+C で停止
sh> watch thread          # 既定 2 秒間隔
```

- 各反復で画面クリア（`\x1b[2J\x1b[H`）→ ヘッダ → CMD 実行 → `-n SEC`（既定 `CLI_WATCH_DEFAULT_SEC`=2、上限 `CLI_WATCH_MAX_SEC`）待ち。Ctrl+C で停止しプロンプト復帰。`thread` の cpu% を定点観測する `top` 的用途に向く。
- **再ディスパッチ**: CMD（RAW tail）を**ローカルバッファにコピーして `cli_parse`、解決した handler を直接呼ぶ**。`sh` の行バッファ・パーサ scratch は汚さない。サブコマンド（`watch thread list`）も解決する。
- **キャンセル**: 内部コマンドの `cli_cancel_requested()` ポーリング、反復間の `cli_sleep`、ループ先頭の明示チェックのいずれでも Ctrl+C を拾う（`cancel_req` は外側ディスパッチ終了まで sticky）。
- **再帰・危険ガード**: tokenizer 正規化後の root トークンを照合し、`watch`（スタック無限再帰）/ `coremark`（~12s・中断不可・printf 経路で画面クリアと干渉）/ `reboot`（初回で即リブート）は拒否する（`watch "reboot"` 等の引用符・エスケープ回避も塞ぐ）。
- パースエラーの CMD（未知コマンド等）はループに入らず 1 回でエラー復帰する。

!!! warning "watch は任意コマンドを反復実行する"
    denylist 以外は何でも反復できる。`devmem peek`/`dump` や `thread` は安全だが、`devmem poke` のような**副作用のあるコマンドの反復は利用者責任**。

## 設定（`cli_config.h`）

| ノブ | 既定 | 意味 |
|---|---|---|
| `CLI_SLEEP_MAX_SEC` | 86400 | `sleep` の上限秒 |
| `CLI_USLEEP_MAX_US` | 10000 | `usleep` の上限µs（ビジーウェイト） |
| `CLI_WATCH_DEFAULT_SEC` | 2 | `watch` 既定間隔秒 |
| `CLI_WATCH_MAX_SEC` | 3600 | `watch -n` 上限秒 |

## 関連
- 協調キャンセルの仕組み: [コマンド登録](shell-registration.md) / [行編集](shell-editing.md)
- `cli_sleep` / `cli_cancel_requested`: #16。TIM2 時刻源: #19（[ThreadX 統合](threadx.md)）。
