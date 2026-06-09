# Shell コマンドラインパーサ

入力 1 行を解釈する core 内部のパーサ（[コマンド登録基盤](shell-registration.md)の上に載る）。
(1) トークン分割（クォート/エスケープ）、(2) static サブコマンド木の探索、(3) argc/argv 検証（RAW 含む）を行う。

パーサは**純粋関数**で、`struct cli_instance` に依存せずハンドラも呼ばない。解決したコマンドと
handler 用 argc/argv、判定ステータスを返すだけで、実際の dispatch／メッセージ表示はコア側が担う。
内部 API は `shell/core/cli_internal.h`、実装は `shell/core/cli_parse.c`。Zephyr shell の**設計のみ参考**（コード非流用）。

## トークナイザ規則（ASCII 前提・in-place 破壊的分割）

行バッファを破壊的に書き換え、各 `argv[i]` はバッファ内を指す NUL 終端文字列。クォート/エスケープは
各トークンの span 内で左詰め圧縮し、**未読のテールは pristine のまま**（RAW がこれを利用）。

| 入力 | 規則 |
|---|---|
| 空白 / タブ | トークン区切り。先頭末尾は無視、連続空白は 1 区切り |
| `"…"`（ダブル） | 閉じるまでリテラル（空白も含む）。中の `\` は次 1 文字をエスケープ |
| `'…'`（シングル） | 完全リテラル（`\` もリテラル、`'` のみで閉じる） |
| `\c`（無クォート） | 次 1 文字をリテラル（`\ ` は空白で分割しない） |
| 空クォート `""` `''` | **空文字列の引数 1 個**として保持 |
| 末尾 `\`（後続なし） | リテラル `\`（エラーにしない） |
| 未終端クォート | **エラー**（`UNTERMINATED_QUOTE`）でコマンド不実行 |

- エスケープは「次 1 文字をリテラル化」のみ（`\x##`/`\0###` hex/octal は非対応＝将来拡張）。
- トークン数が `CLI_MAX_ARGC`(=20) を超えると **`TOO_MANY_TOKENS`**（§8。暗黙切り捨てはしない）。

## コマンド区切り `;`（順次実行）

1 入力行は `;` で複数コマンドに分割され、**左から順に実行**される（例 `thread ; coremark ; thread`）。
分割はディスパッチ前段の `cli_next_segment()`（`cli_parse.c`）が担い、トークナイザと**同一の quote/escape
状態機械**で走査する（各セグメントは内容 verbatim のまま、区切りの `;` だけを `NUL` に置換）。

- **引用尊重**: クォート内・エスケープされた `;` は区切りにしない（`echo "a;b"` / `echo 'a;b'` /
  `echo a\;b` は 1 コマンド）。未終端クォートを含む残りはそのまま 1 セグメントとなり、`cli_parse` が
  `UNTERMINATED_QUOTE` を返す（単一行と同じ扱い）。
- **継続実行**: あるセグメントが not found / 引数エラー / handler 非 0 でも、残りは実行を続ける（bash の `;` 相当）。
- **Ctrl+C で中断**: 実行中コマンドが協調的キャンセル（#16）を検知すると、**残りのセグメントは実行しない**（`^C` は 1 回）。
- **空セグメント**: 先頭/末尾/連続 `;`（`;a`・`a;`・`a;;b`）の空白のみセグメントはスキップ（`EMPTY` の no-op）。
- **履歴は 1 行 = 1 エントリ**: `;` を含む行全体を 1 エントリとして記録。↑ 再呼び出しで行ごと再パースしシーケンスを再実行。
- **プロンプトは行末に 1 回**。各コマンドの出力は単独実行時と同一（セグメント間に余分な改行を挿入しない）。
- **スコープ**: 順次実行 `;` のみ。`&&` / `||` / パイプ `|` / リダイレクトは非対応（将来拡張）。`;` 直後の語の Tab 補完も
  非対応（補完器は `;` を通常文字扱い）。RAW コマンドでは `;` 直前の空白が前セグメントに残る（verbatim テール契約と一貫）。

セグメントループ自体は `cli_session.c` の `cli_dispatch_line()`（`;` 走査）→ `cli_dispatch_one()`（1 コマンドの
パース + 実行）にある。

## サブコマンド木の探索

- token0 を root（`.shell_root_cmds`）で照合。重複 root 名は SORT 順の先頭一致を採用（検出はしない）。
- 以降は現ノードの `subcmds`（センチネル終端配列）を**一致する限り貪欲に降下**。降下停止 = 次トークンが
  未一致 / 現ノードに `subcmds` 無し / 深さ上限。
- **深さ上限** `CLI_MAX_SUBCMD_DEPTH`(=8) は「root から降下したサブコマンド段数」。root + 最大 8 段まで許可、
  超過は `NESTING_TOO_DEEP`。
- **親パスは除去**: handler の `argv[0]` は leaf コマンド名（`thread list` なら handler は `argv[0]="list"`）。
- pure parent（`subcmds` あり / `handler==NULL`）に止まる、または未一致サブコマンドが渡された parent に
  handler が無い場合は `NO_HANDLER`。parent に handler があれば、未一致トークンはその引数になる。

## argc/argv 検証と RAW

- 受理条件: `mandatory <= handler_argc <= mandatory + optional`（`mandatory` は leaf 名を含む。`mandatory=1` は引数なし）。範囲外は `WRONG_ARGS`。
- **RAW**（`optional == CLI_ARG_RAW`, 値 `0xFF`・leaf コマンド向け）: 上限チェックを外し、`mandatory` 個の
  トークンを通常分割した後、**行の残りを未分割の生文字列 1 個**として handler の `argv[mandatory]` に渡す
  （クォート/エスケープ非処理・先頭空白のみトリム・verbatim）。残りが空なら生引数なし。

## 返却ステータス

| ステータス | 意味 | `out` の内容 |
|---|---|---|
| `OK` | handler 実行可 | cmd / handler argv・argc / cmd_level |
| `EMPTY` | 空行 | ゼロ |
| `NOT_FOUND` | root 未知 | `argv[0]`=未知トークン, argc=1 |
| `NO_HANDLER` | pure parent（handler 無し） | cmd / handler argv・argc |
| `WRONG_ARGS` | 引数数が範囲外 | cmd / handler argv・argc |
| `TOO_MANY_TOKENS` | `CLI_MAX_ARGC` 超過 | ゼロ |
| `NESTING_TOO_DEEP` | `CLI_MAX_SUBCMD_DEPTH` 超過 | ゼロ |
| `UNTERMINATED_QUOTE` | クォート未閉 | ゼロ |

`out` は冒頭でゼロ初期化され、`argc` が長さの契約。解決できた範囲で `argv[argc]=NULL` sentinel も書く
（呼び元 argv 配列の容量は `CLI_ARGV_CAP = CLI_MAX_ARGC + 2`）。

## 検証（ホスト単体テスト）

`shell/test/test_parse.c` を `shell/core/cli_parse.c` と同時コンパイル（`CLI_MAX_ARGC`/`CLI_MAX_SUBCMD_DEPTH`
を小さく上書きしてコンパクトな木で上限系を検査）。分割（クォート/エスケープ/空クォート/末尾 `\`/未終端/
NULL sentinel）・探索（多段/親パス除去/pure parent/未一致→親引数/深さ超過）・検証（範囲内外/TOO_MANY）・
RAW（多段 leaf の handler 相対 index/verbatim テール/残り空/不足）・`;` 分割（`cli_next_segment`：引用/エスケープ内
`;` を割らない/空・先頭・末尾セグメント/未終端クォートは 1 セグメント/エスケープ境界）を assert。
`;` 順次実行の end-to-end（継続実行・Ctrl+C 中断・行末プロンプト 1 回）は `shell/test/test_integration.c`。

```bash
bash shell/test/run_host_tests.sh   # => host tests passed
```
