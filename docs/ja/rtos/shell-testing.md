# Shell テスト基盤（dummy backend・ホスト自動テスト）

実機（UART/VCP）なしで Shell コアを**入力 → 実行 → 出力検証**まで通すための層。
in-memory な **dummy(loopback) backend** と、`cli_core.c` の ThreadX グルーを置き換える
**host glue** からなり、ホスト gcc で全自動テストを走らせる。Zephyr shell の**設計のみ参考**（コード非流用）。

## レイヤ

| ファイル | ThreadX | 役割 |
|---|---|---|
| `shell/backend/cli_backend_dummy.{c,h}` | 非依存 | loopback transport（`init/enable/write/read`）。RX FIFO / TX キャプチャログ / TX 容量モデル |
| `shell/test/host_glue.{c,h}` | 非依存 | `cli_core.c` 代替: `cli_lock/unlock`・notify no-op、忠実な `cli_tx_send_blocking`、RX pump |
| `shell/test/test_core.c` | 非依存 | #4 セッション単体（ASCII フィルタ / 状態機械 / dispatch / fail-safe） |
| `shell/test/test_output.c` | 非依存 | #5 出力単体（フォーマッタ / staging / 色 / hexdump / TX 失敗） |
| `shell/test/test_integration.c` | 非依存 | #6 backend 経由の end-to-end（入力→実行→出力 / フロー制御 / 異常系 / マルチインスタンス） |
| `shell/test/test_edit.c` | 非依存 | #9 [行編集](shell-editing.md)単体（カーソル / 挿入・上書き / 削除 / 単語 / VT100 / CPR / 折返し） |

dummy backend は移植可能（実機向け shell ライブラリにもそのまま載る、#8）。`cli_transport_notify_rx`
以外の `tx_*` を呼ばないため、host でも target でも同一コードでビルドできる。

## dummy backend

`struct cli_dummy`（transport の `ctx`）は**キャプチャログ**と **TX 空き容量**を分離する:

- **RX FIFO**: `cli_dummy_inject()` が投入し（実機 ISR と同じく**通知のみ**: `cli_transport_notify_rx`）、
  `read()` が drain する。容量超過分は破棄し、dummy と `sh->rx_dropped` の両方を加算（§9/§18 10e）。
- **TX キャプチャログ**: `write()` が受理したバイトを順序通り**恒久蓄積**（検証用、自動消去しない）。
- **TX 空き容量**: 別カウンタ。`write()` はこの空き分だけ受理し、尽きると `0`（満杯）を返す。
  **自動回復しない** — `cli_dummy_free_tx()` を呼んだ時だけ増える（実機の「TX 空き → `cli_transport_notify_tx`」を模す）。

| ヘルパ | 用途 |
|---|---|
| `cli_dummy_inject(tr, data, len)` | 入力注入 + RX 通知 |
| `cli_dummy_output(tr, &len)` / `_output_str(tr)` | キャプチャ取得（長さ付き / NUL 終端） |
| `cli_dummy_set_tx_cap(tr, n)` | 初期 TX 容量（`0`=無制限） |
| `cli_dummy_free_tx(tr, n)` | TX 空きを n 解放（notify_tx 相当） |
| `cli_dummy_set_tx_fail(tr, on)` | `write()` 即 `-1`（dead transport） |
| `cli_dummy_clear_output/clear_rx/reset_stats(tr)` | 用途別リセット |

## データフロー（end-to-end）

```
cli_dummy_inject ─▶ RX FIFO ─(notify_rx)─▶ cli_test_pump ─▶ read() ─▶ cli_input_byte
                                                                            │ dispatch
       capture log ◀─ write() ◀─ cli_tx_send_blocking ◀─ cli_out_flush ◀─ cli_print/echo
```

`cli_test_pump()` は `cli_core.c` のスレッドループ（RX 信号で `read()`→各バイト `cli_input_byte`）を
host で同期再現する。出力は実 `cli_printf.c` から **host glue の `cli_tx_send_blocking` 経由で
`tr->api->write()` を実際に叩く**ため、transport の read/write 契約とフロー制御が経路として検証される。

## §11 フロー制御の host モデル

host は単一スレッドなので、`cli_tx_send_blocking()` 実行中にテスト本体は空きを解放できない。
そこで満杯（`write()==0`）を観測するたび glue は **`on_tx_wait` フック**を呼ぶ（`cli_transport_notify_tx`
待ちの host 等価）。`cli_test_set_tx_wait_hook()` で登録する:

- **正常バックプレッシャ**: フックが `cli_dummy_free_tx()` で空きを与え、送信が順序通り完走（drop なし）。
- **タイムアウト drop**: フック未登録（NULL）→ 空きが来ず、有限リトライ上限で残りを破棄し `tx_dropped` 加算・`<0`。

glue の `cli_tx_send_blocking` は `cli_core.c` の挙動（部分受理→完走 / 満杯→タイムアウト drop /
`write()<0`→即失敗 / `n>残量` クランプ）を写す。`test_integration.c` は前 3 挙動を固定して drift を検出する
（クランプは正常な backend が過大受理しないため発火せず、防御的に `cli_core.c` を写すのみ）。

## カバレッジと非対象

実装済みの面（#2 登録 / #3 パーサ / #4 コア / #5 出力）を基本系・異常系で網羅する。

| §18 | 内容 | テスト |
|---|---|---|
| 基本 | dispatch / サブコマンド / argv / プロンプト復帰 | test_core, test_integration |
| §13 | ASCII フィルタ（高位バイト破棄）/ ESC・CSI swallow / CR-LF coalesce | test_core, test_integration |
| §11 | 色・staging・autoflush・hexdump / バックプレッシャ完走・timeout drop・即失敗・非0昇格 | test_output, test_integration |
| 10a-d,h | 未知コマンド / 引数不正 / 行長超過 BEL / Ctrl+C | test_core, test_integration |
| 10e | RX FIFO オーバーフロー drop + 統計 | test_integration |
| §10 | dummy×2 の出力非混在・状態独立 | test_core, test_integration |

**非対象（後続 issue で拡張）**: 行編集・カーソル・履歴・Tab 補完（#9-#11）、組込みコマンド（#12-#14）は
機能未実装のため対象外。`cli_transport_notify_tx` 契約（「TX 空きで backend が通知」）は host では
no-op で**未検証** → #7 UART backend で実機 verify する。真の ThreadX 並行スケジューリングでの同時稼働は
#8 の実機デモに委ね、ここでは交互駆動で「出力非混在」を実証する。

## 実行

```bash
sh shell/test/run_host_tests.sh   # => host tests passed
```

各テストは `host_sections.ld`（`.shell_root_cmds` 境界シンボル）と `shim/tx_api.h`（ThreadX 型 stub）を
使い、firmware ビルドを介さずホスト gcc で完結する。dummy backend と host glue は #4-#6 で共有リンクする。
