# Tab 補完（#11）

[行編集](shell-editing.md)層の `cli_input_byte()` が `Tab`（`0x09`）を受けると
`cli_tab_complete()`（`shell/core/cli_complete.c`）へ委譲する。コマンド名・サブコマンド名の
**Tab 補完**を実装する（要件 §2 / §8 / §18.4）。bash / readline / Zephyr shell の
**振る舞いの概念のみ参考**でコードは非流用。

`cli_complete.c` は `tx_*` を呼ばず、入力行（`sh->line`）と[登録コマンドツリー](shell-registration.md)を
**read-only** で走査し（`cli_parse.c` の in-place トークン化とは違い行を壊さない）、**動的確保なし**で
コマンド木を**線形走査**する（要件 §8）。出力は #5 のバッファ出力 API 経由なので、ホスト gcc で
[単体テスト](shell-testing.md)できる。

## 補完アルゴリズム

1. **補完対象の語** — カーソル `cur` から左へ非空白を遡り、語の先頭 `ws` を求める。
   `prefix = line[ws..cur)`。補完テキストは `cur` 位置へ挿入し、後続（tail）は右へずらすので、
   行末でも行中でも正しく動く。
2. **照合するコマンド集合** — 語の手前 `line[0..ws)` のトークンを **read-only** に左から辿る
   （長さ境界の `memcmp`。行に `'\0'` を書かない）。判定は **手前トークン数**で行い、`ws==0` では
   行わない:
   - 手前トークンが 0（空行、または `"  h"` のような先頭空白のみ）→ **root 集合**。
   - 第 1 トークンが root に完全一致し、その `subcmds` があれば降りる。leaf（`subcmds==NULL`）に
     当たった／未知トークンなら、そこから先は引数領域なので**候補なし**。
3. **候補走査（1 パス・候補を保存しない）** — 集合を走査し、`prefix` で前方一致する候補数を数え、
   最初の候補を覚え、**共通プレフィックス長（LCP, prefix より先の共通部分）**を `first` と縮約して求める。
4. **結果**（要件 §18.4）:

| 候補数 | 動作 |
|---|---|
| 0 | BEL（`0x07`）。行は変えない |
| 1 | 残りを補完し、行末ならセパレータの空白を 1 つ付す（行中では付さない） |
| 2 以上 | **bash 風 2 段階**: まず共通プレフィックスまで補完。これ以上伸びないとき、**次の連続 Tab** で候補一覧を表示 |

## 2 段階（共通プレフィックス → 一覧）

複数候補のときは `struct cli_instance` の `tab_list_armed`（1 バイト）で 2 段階を実現する:

- LCP が伸びる → 補完して `tab_list_armed=1`（次 Tab で一覧）。
- これ以上伸びない → `tab_list_armed==0` なら BEL して arm のみ、`==1` なら**一覧表示**。
- `tab_list_armed` は `cli_input_byte()` の冒頭で **Tab 以外の任意キー**で 0 にリセットされる
  （`0x09` だけが保持）。よって編集を挟むと 2 押下フローがやり直しになる。

例: `ve`+Tab → `ver` に補完（arm）。続けて Tab → `version` と `verbose` を一覧。
共通プレフィックス上で 1 回目の Tab は一覧を出さず、**2 回目の連続 Tab** で出す。

## 候補一覧の描画

一覧は[行編集](shell-editing.md)の再描画不変条件（`old_rows` / `draw_row`）に整合させる。
`cli_edit.c` の static 出力ヘルパは別 TU から呼べないため、内部の staging プリミティブ
（`cli_lock` / `cli_out_putc` / `cli_out_flush`）を **1 ロック**で使い、ローカルの小ヘルパ
（`c_csi_n`）で出力する（他スレッドの同一インスタンス出力と混ざらないため。続く
`cli_edit_redraw()` は別ロック ＝ シームだが shell スレッド主導運用では許容、§10）:

1. 現在 render の最下行へ移動（`ESC[<n>B`）し `\r\n`。折返した入力行全体の**下**に一覧が出る
   （`cli_dispatch_line()` と同じ要領）。入力行はそのまま上に残る。
2. 前方一致する名前を**端末幅依存の桁**で印字（`cli_printf` は `%-*s` 非対応なので空白を手動パディング）。
   1 行 `per_row=cols/colw` 個、末尾は必ず `\r\n` で桁 0 の新規行へ。**追加メモリ確保なし**、
   最大表示数は端末幅依存（固定上限なし、要件 §8）。
3. `old_rows=0; draw_row=0` にして `cli_edit_redraw()`（`op_clear_screen` 末尾と同じ）。
   `sh->cur` は一覧表示で変えないのでカーソルは復帰する。

## 既知の制約

- **引数は補完しない** — `struct cli_cmd` に引数メタデータがないため、コマンドパスを越えた語は BEL。
- **クォート付きコマンドパス** — read-only トークン walk はクォート/バックスラッシュ処理を再現しない。
  `"help"` のような語は submit では parse できても補完対象外。コマンド名は C 識別子なので通常は無問題。
- ASCII のみ（要件 §13）。マルチインスタンスでは `tab_list_armed` はインスタンス毎、コマンド木走査は
  read-only 共有なので競合しない（要件 §10）。

## 検証

ホスト単体テスト `shell/test/test_complete.c`（`cli_input_byte` に Tab を直接流しモデルを assert）:
root 単一補完 / root 曖昧 → LCP 補完 / 2 段階一覧（+ 非 Tab キーで arm リセット）/ サブコマンド補完 /
no-match BEL / 引数領域 BEL / 空行 2 段階一覧 / 行中補完（空白を二重に入れない）/ 先頭空白の root 補完。
小バッファビルド（`CLI_CMD_BUFFER_SIZE=8`）で buffer-full の BEL と、**LCP が収まらなくても次 Tab で
一覧に到達する**ことを検証する。

```bash
sh shell/test/run_host_tests.sh   # => host tests passed
```

実機は VCP（`/dev/ttyACM0`, 115200 8N1）に端末を接続し、`h`+Tab → `help `、空行で Tab 2 回 →
`echo`/`help` 一覧、操作応答 < 50 ms を確認する（要件 §15 / §18.4）。
