# 行編集・VT100・メタキー・色（#9）

[Shell コア](shell-core.md)の `cli_input_byte()`（`shell/core/cli_edit.c`）を、
末尾追記だけの最小入力から **対話的な行編集**へ拡張する層。カーソルを行長から分離し、
行中の挿入・上書き・削除・カーソル移動・単語操作・行クリア・**端末幅での折返し再描画**、
VT100 escape（矢印 / Home / End / Del / Insert）、Emacs 風メタキー、色出力を実装する
（要件 §2）。GNU readline / linenoise / Zephyr shell の**設計概念のみ参考**でコードは非流用。

`cli_edit.c` は `tx_*` を呼ばず、出力は #5 のバッファ出力 API（`cli_lock` / `cli_out_putc` /
`cli_out_flush`）経由なので、ホスト gcc で[単体テスト](shell-testing.md)できる。

## カーソルモデル

`struct cli_instance` に `cur`（カーソル index, 0..len）を `len` から分離して持つ。
編集操作は `(line, len, cur)` を更新してから再描画する。`overwrite`（insert/overwrite）、
`bs_swap`（backspace mode）、`term_width`、`old_rows` / `draw_row`（再描画の不変条件）、
`probing_cpr`（CPR 待ち）、`esc_p[2]` / `esc_np`（CSI パラメータ）も同構造体に持つ。

## キー割当

| 入力 | 動作 |
|---|---|
| 印字 `0x20–0x7E` | カーソル位置へ挿入（overwrite 時は置換） |
| `Ctrl+a` / `Ctrl+e` | 行頭 / 行末 |
| `Ctrl+b` / `Ctrl+f`、`←` / `→` | カーソル左 / 右 |
| `Alt+b` / `Alt+f`（`ESC b` / `ESC f`） | 単語単位で左 / 右 |
| `Home` / `End`（`ESC[H` `ESC[F` `ESC[1~` `ESC[4~`） | 行頭 / 行末 |
| `Backspace`（`0x08`） | カーソル直前を削除 |
| `Del`（`0x7F` 既定） / `Ctrl+d` / `Delete`（`ESC[3~`） | `0x7F` は既定で後方削除。`Ctrl+d`・`ESC[3~` はカーソル位置を前方削除 |
| `Ctrl+k` | カーソル以降を削除 |
| `Ctrl+u` | 行頭〜カーソルを削除（行末なら実質行クリア） |
| `Ctrl+w` | 直前の単語を削除（空白境界） |
| `Ctrl+l` | 画面クリア + プロンプト/行を再描画 |
| `Insert`（`ESC[2~`） | insert / overwrite を切替 |
| `Ctrl+c` | 入力行をキャンセル（任意の escape 途中からも復帰、§9） |
| `Enter`（`\r` / `\n`） | 行を dispatch（`\r\n` は 1 回） |
| `↑` / `↓`、`Ctrl+p` / `Ctrl+n` | 履歴を古い / 新しい方へ呼び出し（#10 固定リング） |

`Tab`（`0x09`）は #9 では無視（補完は #11）。非 ASCII（`0x80–0xFF`）と未対応・不正な
escape は無視して通常状態へ復帰する（要件 §13）。

## escape 状態機械

`enum cli_rx_state` を `NORMAL / ESC / CSI / SS3` に拡張。`ESC`（`0x1B`）の次バイトで
分岐する:

- `[` → **CSI**: 数値パラメータと `;` を蓄積し、final byte（`0x40–0x7E`）で確定。
  `A/B/C/D`=↑↓→←、`H`/`F`=Home/End、`~` 系は `1/7`=Home `2`=Insert `3`=Del `4/8`=End、
  `R`=CPR 応答。未知 final は無視（§13）。
- `O` → **SS3**: `ESC O A` 等のアプリケーションモード矢印（パラメータ無し）。
- `b` / `f` → Alt+単語移動。その他は無視。

## 再描画モデル（統一リフレッシュ + fast-path）

編集操作は基本的に **1 つの `cli_edit_refresh()`** で行全体を再描画してカーソルを再配置する。
端末の最終列 auto-wrap 挙動には依存せず、物理カーソル位置を自前で確定する:

記号 `cols=term_width`、`pend=prompt長+len`、`pcur=prompt長+cur`。

1. 直近 render（`old_rows` 行）を最下行→最上行へ `ESC[2K` で消去。
2. prompt + line を出力。`pend>0 && pend%cols==0`（幅境界ちょうど）のとき **`\r\n` を強制**し、
   物理カーソルを次行 col0 に確定。
3. `pcur` の行/列へ `ESC[<n>A` + `\r` + `ESC[<n>C` で再配置。
4. 不変条件 `old_rows = pend/cols + 1`、`draw_row = pcur/cols` を更新。

数値は固定長の小ヘルパで桁出力（`snprintf` や大きなローカル配列を使わずスタックを抑える）。

**fast-path**（よくある操作を最小バイトで、#4 の出力と byte 一致）:

- 末尾追記（`cur==len` かつ追記後も同一物理行）→ 1 バイト echo のみ。
- 末尾 Backspace（`cur==len>0` かつ行頭跨ぎでない）→ `\b \b` のみ。
- カーソルのみの移動（矢印 / Home / End / 単語移動）→ 行を再描画せずカーソルだけ移動。

それ以外（行中編集・wrap 跨ぎ）は必ずフル refresh。

## 端末幅の自動検出（CPR）

折返しには端末幅が要る。起動時に `cli_edit_session_start()` が **CPR プローブ**
（`ESC[999C` でカーソルを右端へ → `ESC[6n` で位置要求）を送り、**直後に通常 refresh を後置**して
可視カーソルを必ず復帰させる。これにより応答が来ても来なくても表示は壊れない。
端末は `ESC[<r>;<c>R` を返し、CSI パーサがこれを消費して `term_width=c` に反映する。

反映は **`probing_cpr` 立ち & パラメータ 2 個 & `20<=c<=255`** のときのみ。それ以外の `R`
（プローブ外・param 数不一致・範囲外・ペースト混入）は未知 final として無視し、行へ漏らさない。
未応答端末では `CLI_TERM_WIDTH`（既定 80）を使い続ける（非ブロッキング、停止しない）。
中途のリサイズは追わない（再接続で再プローブ）。

## insert / overwrite と backspace mode

- `Insert` キーで `overwrite` をトグル。overwrite 時はカーソル位置の文字を置換。コマンド間で保持。
- `CLI_BACKSPACE_MODE`（既定 0）が `bs_swap` を初期化。`0`=`0x08`/`0x7F` 双方が後方削除。
  `1`=`0x7F`(DEL) を前方削除に（Backspace=`0x08` の端末向け）。実機切替は `cli_set_backspace_mode()`。

## 色

`cli_error`/`warn`/`info` の SGR 色（赤/黄/緑）は #5 で実装済み。`CLI_USE_COLOR=0` で無色化。
カーソル/消去/CPR の制御 escape（`cli_vt100.h`）は**色とは別系統で常に出力**する（装飾でなく編集意味のため）。

## 設定

| パラメータ | 既定 | 意味 |
|---|---|---|
| `CLI_TERM_WIDTH` | 80 | CPR 検出前/未応答時の端末幅（折返し） |
| `CLI_BACKSPACE_MODE` | 0 | `bs_swap` 初期値（0=双方後方削除 / 1=DEL 前方削除） |

## 履歴（#10）

`↑`/`↓`・`Ctrl+p`/`Ctrl+n` は `cli_history_prev/next()` を呼び、dispatch は submit した行を
`cli_history_add()` で記録する（パーサが in-place トークン化する前に）。`shell/core/cli_history.c`
が **固定バイトリング**（要件 §8）を実装する:

- **格納** — `sh->hist[CLI_HISTORY_BUFFER_SIZE]`（既定 512 B）。エントリは古い→新しい順に詰め、
  各 `'\0'` 終端。追加時は収まるまで最古エントリを FIFO で破棄する。**動的確保なし**、リングは
  **インスタンス毎**で履歴が混ざらない（要件 §10）。
- **重複排除** — 直前エントリと同一の行は記録しない（直前重複排除）。空行、および安全に呼び出せない
  行（行バッファ長 or リング全体より長い）も記録しない。
- **呼び出し** — `↑`/`Ctrl+p` で古い方へ（初回は最新へジャンプ、最古で停止）、`↓`/`Ctrl+n` で新しい方へ。
  呼び出しはエントリを行バッファへコピー（カーソルは末尾）し、export した `cli_edit_redraw()` ラッパで
  再描画する（エディタ本体の refresh は `static`）。
- **draft 復帰なし（MVP）** — 最新の先で `↓` を押すと **空の** live 行に戻る。初回 `↑` 前に入力していた
  文字や、呼び出した行への編集は保存しない。
- **ナビゲーションリセット** — `cli_dispatch_line()` が `hist_nav_on`/`hist_nav` を無条件にクリアし
  （呼び出した行を空にして再 submit しても古い offset が残らない）、`Ctrl+c` でもナビゲーションを抜ける。

これらは当該インスタンスの shell スレッドからのみ実行される（RX ISR は event flag を立てるだけ）ため、
履歴コード自身のロックは不要。

## 検証

ホスト単体テスト `shell/test/test_edit.c`（`cli_input_byte` 直接駆動でモデルを assert）:
カーソル移動 / 挿入・上書き / 削除（BS・Ctrl+d・Del・Ctrl+k/u/w）/ 単語移動 /
不正 escape 無視 / CPR ガード / 折返し行数 / 画面クリア / backspace mode / dispatch 後のカーソル復帰。
`shell/test/test_history.c` は履歴リングを検証（32 B キャップでビルドし退避を誘発）:
追加・呼び出しの往復 / 重複排除 / 非連続重複の保持 / FIFO 退避 / 空行不追加 / ↑↓・Ctrl+p,n の配線 /
submit・Ctrl+c・空行再 submit でのナビゲーションリセット / MVP「draft 復帰なし」/ インスタンス分離。

```bash
sh shell/test/run_host_tests.sh   # => host tests passed
```

実機は VCP（`/dev/ttyACM0`, 115200 8N1）に端末を接続し、カーソル移動・行中編集・メタキー・
折返し再描画・色・操作応答（< 50 ms）/ エコー（< 5 ms）を確認する（要件 §15/§18）。
他スレッド `printf` が編集行に割り込むと表示が乱れ得るが状態は壊れず、`Ctrl+l` で復旧できる。
