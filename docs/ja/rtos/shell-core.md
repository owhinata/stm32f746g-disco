# Shell コア（インスタンス / ThreadX / 状態機械 / dispatch）

[登録基盤](shell-registration.md)と[パーサ](shell-parser.md)を実行時に束ねるコア。
1 つの通信路（transport）= 1 つの **shell インスタンス**として、ThreadX スレッド上で
受信バイトを行に組み立て、Enter でパーサを呼び、ハンドラを実行する。Zephyr shell の
**設計のみ参考**（コード非流用）。

## レイヤ分割（ThreadX 非依存の seam）

| ファイル | ThreadX | 役割 |
|---|---|---|
| `shell/include/cli.h` | 非依存 | コマンド登録の公開 API（`struct cli_instance` は前方宣言のみ） |
| `shell/include/cli_instance.h` | 依存（`tx_api.h`） | `struct cli_instance` / transport 型 / `CLI_INSTANCE_DEFINE` / ライフサイクル API |
| `shell/core/cli_core.c` | 依存 | `cli_init` / `cli_start` / スレッドループ / ISR 通知（`tx_*` を呼ぶ唯一のコア） |
| `shell/core/cli_session.c` | **非呼び出し** | ASCII フィルタ / RX 状態機械 / dispatch / raw 出力 |

`cli_session.c` は `tx_*` 関数を一切呼ばないため、ホスト gcc で
型 shim（`shell/test/shim/tx_api.h`）と共にコンパイルし
**状態機械と dispatch を実機なしで単体テスト**できる。`cli.h` 側は ThreadX 非依存を保ち、
コマンド定義ファイルやパーサのホストテストは ThreadX ヘッダ無しでビルドできる。

## transport 抽象

コアはハードウェアに直接触れず、backend が実装する `struct cli_transport_api` 経由で通信する。

```c
struct cli_transport_api {
    int  (*init)(struct cli_transport *tr);                          /* 必須 */
    int  (*enable)(struct cli_transport *tr);                        /* 必須: RX 開始 */
    int  (*write)(struct cli_transport *tr, const uint8_t *d, size_t n);/* 必須: 非ブロッキング, 受理 0..n 返却 (#5 で確定) */
    int  (*read)(struct cli_transport *tr, uint8_t *d, size_t cap);  /* 必須: 非ブロッキング, 0..cap */
    void (*uninit)(struct cli_transport *tr);                        /* 任意（NULL 可） */
    void (*update)(struct cli_transport *tr);                        /* 任意（NULL 可） */
};
```

- backend → コア通知は `cli_transport_notify_rx(sh)`（ISR からも安全：イベントフラグを
  set するだけで、ロック・サスペンドを取らない）。`cli_transport_notify_tx` は #5 用に予約。
- dummy backend（#6）、USART1 VCP backend（#7）が本抽象を実装する。

## ThreadX プリミティブ（インスタンス毎）

| 抽象 | ThreadX | 用途 |
|---|---|---|
| インスタンススレッド | `tx_thread`（`CLI_INSTANCE_PRIORITY` 既定 16） | 受信処理 + dispatch |
| RX / TX / KILL シグナル | `tx_event_flags`（`CLI_EVT_RX/TX/KILL`） | ISR/backend からの起床 |
| TX 排他 | `tx_mutex`（`TX_INHERIT`） | #4 は作成のみ。ロック付き出力は #5 |

スレッドループ: `tx_event_flags_get(RX|KILL)` で起床 → `read()` で取りこぼさず drain →
各バイトを状態機械へ → プロンプトへ復帰。`KILL` で終了（完全な stop/uninit は将来）。

## 受信状態機械と最小入力パイプライン（ASCII 前提）

`cli_input_byte()` が 1 バイトずつ処理する。

- **ASCII フィルタ**: 非 ASCII（`0x80–0xFF`）は破棄して行へ渡さない。
- 印字可能（`0x20–0x7E`）: 行へ追加しエコー。満杯（`CLI_CMD_BUFFER_SIZE-1`）でベル（BEL）。
- 改行: `\r` / `\n` で dispatch。`prev_cr` により `\r\n` は **1 回だけ** dispatch。
- Backspace（`0x08`/`0x7F`）: 末尾 1 文字削除（最小行編集。完全版は #9）。
- Ctrl+C（`0x03`）: 入力行を破棄して新プロンプト。
- ESC（`0x1B`）→ `ESC`/`CSI` 状態で**エスケープを読み捨て**（矢印キー等が行を汚さない。
  完全な VT100 解析は #9 がこの状態を拡張する）。
- それ以外の制御文字は無視。

## dispatch とエラー写像

`cli_dispatch_line()` はパーサの `enum cli_parse_status` をメッセージへ写像する。

| ステータス | 挙動 |
|---|---|
| `OK` | ハンドラを `pr.argv` / `pr.argc`（leaf 相対, `argv[0]`=leaf 名）で実行、戻り値を `last_result` へ |
| `EMPTY` | 何もしない（空行） |
| `NOT_FOUND` | `<cmd>: command not found` |
| `NO_HANDLER` | `<cmd>: missing or unknown subcommand` |
| `WRONG_ARGS` | `<cmd>: invalid number of arguments` |
| `TOO_MANY_TOKENS` / `NESTING_TOO_DEEP` / `UNTERMINATED_QUOTE` | 各エラー文言 |

パースエラー（`OK`・`EMPTY` 以外）は `last_result = CLI_DISPATCH_ERR (-1)`（`EMPTY` は不変）。
**いかなる経路でも必ずプロンプトへ復帰**し、不正コマンドやハンドラの非 0 戻り値でも
shell は停止しない（fail-safe, 要件 §9）。

## マルチインスタンス状態分離

行バッファ・履歴・端末状態・パーサ scratch・プロンプトはすべて `struct cli_instance` 内に持ち、
**コアは可変グローバル状態を持たない**。コマンドツリーはリンカセクション上の read-only 共有データ。
よって複数インスタンスが同時稼働しても出力・状態が混在しない（要件 §10）。

```c
/* 例: backend と結線して起動（#6/#8） */
CLI_INSTANCE_DEFINE(vcp_shell, &vcp_transport, "uart:~$ ");
cli_init(&vcp_shell);    /* backend init → event flags → mutex */
cli_start(&vcp_shell);   /* tx_thread_create（auto-start） */
```

`cli_init` は backend init → `tx_event_flags_create` → `tx_mutex_create` の順。失敗は
当該インスタンスのみ無効化し他は継続（`tx_*_delete` は初期化中に呼べないため、init 中の
失敗では ThreadX オブジェクトを削除しない）。

## スコープ（#4）と後続

#4 は骨格まで。出力 API/バッファ/色（#5）、dummy backend と通し自動テスト（#6）、
USART1 VCP backend（#7）、shell アプリ + `flash-shell`（#8）、行編集/履歴/補完（#9–#11）は後続。

## 検証（ホスト単体テスト）

`shell/test/test_core.c` を `cli_session.c` + `cli_parse.c` と同時コンパイル（`shim/tx_api.h`
を include パス先頭に、`CLI_CMD_BUFFER_SIZE`/`CLI_MAX_ARGC` を小さく上書きして満杯/トークン超過を検査）。
capture transport で出力を記録し、ASCII フィルタ・状態機械・CR/LF/CR-LF・dispatch 写像・
fail-safe・**2 インスタンスの出力非混在**を assert する。

```bash
sh shell/test/run_host_tests.sh   # => host tests passed
```
