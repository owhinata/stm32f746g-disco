# Shell 出力 API（バッファリング・色・フロー制御）

[Shell コア](shell-core.md)の上に載る出力層。ハンドラはここを通して印字し、各呼び出しは
**整形 → 32B staging → autoflush → 自分の transport へ送信**を、TX ロックとフロー制御の下で行う。
Zephyr shell の**設計のみ参考**（コード非流用）。

## レイヤ（ThreadX 非依存 seam の踏襲）

| ファイル | ThreadX | 役割 |
|---|---|---|
| `shell/include/cli.h` | 非依存 | 公開出力 API プロトタイプ |
| `shell/core/cli_printf.c` | **非呼び出し** | 最小 vprintf→char sink / 32B staging+autoflush / 色 / hexdump |
| `shell/core/cli_vt100.h` | 非依存 | VT100 SGR 色 + `CLI_USE_COLOR` ゲート |
| `shell/core/cli_core.c` | 依存 | `cli_lock`/`cli_unlock`(TX mutex) + `cli_tx_send_blocking`(フロー制御) |

`cli_printf.c` は `tx_*` を呼ばず、3 つの hook（`cli_lock`/`cli_unlock`/`cli_tx_send_blocking`）経由で
ThreadX 層に触れる。よって最小フォーマッタと staging はホスト gcc で単体テストできる（[#4](shell-core.md) と同方式）。

## 公開 API

```c
int cli_write (struct cli_instance *sh, const void *data, size_t len);   /* 生バイト（echo 等） */
int cli_print (struct cli_instance *sh, const char *fmt, ...);           /* 既定色 */
int cli_error (struct cli_instance *sh, const char *fmt, ...);           /* 赤 */
int cli_warn  (struct cli_instance *sh, const char *fmt, ...);           /* 黄 */
int cli_info  (struct cli_instance *sh, const char *fmt, ...);           /* 緑 */
int cli_hexdump(struct cli_instance *sh, const void *data, size_t len);  /* 正準 hex+ASCII */
```

- 各呼び出しは `cli_lock` → 整形/staging → autoflush → `cli_unlock` で囲み、`out_buf`/`out_len` を
  フォーマット全体にわたり排他する（要件 §10）。`format(printf,2,3)` 属性で誤用を検出。
- 返値: **0 = 全送信成功、`<0` = 出力失敗（TX timeout で drop、またはロック取得失敗）**。
- **thread context 専用・ISR 非対応**（`tx_mutex_get`/`tx_event_flags_get` の wait は ISR 不可）。
  backend の TX 通知は ISR から `cli_transport_notify_tx`（event flag set）のみ。
- 色は `cli_error`=赤 / `cli_warn`=黄 / `cli_info`=緑（要件 §2）。`CLI_USE_COLOR=0` で SGR を一切出さない。

## 最小フォーマッタ（§8 準拠）

§8 の「32B printf バッファ・満杯で flush」を**追加バッファ無し**で満たすため、フォーマッタは char を
1 文字ずつ staging に流す（`cli_out_putc`、満杯で `cli_out_flush`）。clean-room の独自実装で、
newlib / Zephyr の printf コードは流用しない。

- 対応: `%% %c %s %d %i %u %x %X %p`、長さ修飾 `l` / `ll` / `z`、フィールド幅 + `0` 埋め / `-` 左詰め。
- 非対応: 精度、`+` / 空白 / `#` フラグ。`INT_MIN`/`LLONG_MIN` は符号なし magnitude で安全に変換。
- `cli_hexdump`: 16 バイト/行で `08x` オフセット + hex + ASCII（非印字は `.`）。

## フロー制御 / TX timeout（要件 §11）

`cli_tx_send_blocking`（cli_core.c, ロック保持下で実行）が §11 を一意化する:

1. transport `write` は**非ブロッキング**で受理バイト数（`0..len`）を返す。`n<len`（TX 満杯）を返した
   backend は、以後 TX 空きが生じたら必ず `cli_transport_notify_tx` を発火する義務を負う。
2. TX 満杯時、コアは `CLI_EVT_TX | CLI_EVT_KILL` を `tx_event_flags_get` で待つ（= §11 のブロッキング送信
   ＝スレッド suspend）。待ち時間は `CLI_TX_TIMEOUT`（tick、`0`=無限）。
3. timeout → 残バイトを破棄し `tx_dropped` 加算、`tx_failed` セット、出力 API は `<0`。
   KILL 観測 → 再ポストして abort（無限待ちでも kill 可能）。
4. **コマンド非0終了の強制**: `cli_dispatch_line` は handler 後に
   `last_result = (ret==0 && tx_failed) ? 非0 : ret` とし、handler が戻り値を無視しても TX 失敗は非0終了。

`tx_failed` はコマンド毎（dispatch 冒頭）にリセットされ、一度立つと当該コマンドの残出力は drop される。

協調キャンセル（#16）との連携: 実行中（`dispatching`）の `cli_tx_send_blocking` は待ち集合に
`CLI_EVT_RX` も加え、TX 律速でブロック中に `Ctrl+c`（`0x03`）が届くと wait 前後の `cli_cancel_poll`
で検出して早期に `<0` を返す（＝大量出力コマンドが即座に止まる）。詳細は
[コマンド登録](shell-registration.md)の「協調的キャンセル」。

## 構成パラメータ

| ノブ | 既定 | 意味 |
|---|---|---|
| `CLI_PRINTF_BUFFER_SIZE` | 32 | staging サイズ（満杯で flush） |
| `CLI_TX_TIMEOUT` | 1000 | TX 空き待ち上限（tick、`0`=無限）。1kHz tick で約 1s |
| `CLI_TX_MUTEX_WAIT` | 0 | 出力ロック取得待ち（tick、`0`=無限） |
| `CLI_USE_COLOR` | 1 | 0 で色を出さない |

## 検証（ホスト単体テスト）

`shell/test/test_output.c` を `cli_printf.c` と同時コンパイル（`shim/tx_api.h`、出力は共有
[dummy backend + host glue](shell-testing.md) 経由で実際に `tr->api->write()` を叩く）。フォーマッタ
（境界: `INT_MIN`/`LLONG_MIN`/`%p`/`NULL %s`/未知 spec/幅・フラグ）・32B 超の autoflush・色（SGR）・
hexdump・TX 即失敗時の `<0`/`tx_failed`/無出力を assert。フロー制御（バックプレッシャ完走・timeout drop・
`tx_dropped`・clamp）は [#6 統合テスト](shell-testing.md)が host glue で経路検証する
（`cli_core.c` の ThreadX 待ち/KILL は ARM スモークとレビューで担保）。

```bash
sh shell/test/run_host_tests.sh   # => host tests passed
```
