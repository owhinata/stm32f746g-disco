# GUIX（Eclipse ThreadX GUIX 統合）

ボード搭載 4.3″ 480×272 LCD（LTDC、#52/#53）と FT5336 容量タッチ（I2C3、#54）の上に、**Eclipse ThreadX GUIX**（GUI フレームワーク）を載せる。GUIX を ThreadX スレッドとして起動し、565rgb ディスプレイドライバを LTDC の tear-free ダブルバッファ + DMA2D に、入力ドライバを FT5336 に接続して、基本ウィジェット（テキスト + ボタン + 2 画面遷移）を表示する。LTDC + GUIX エピック（#48）の **Phase 4**（#55）。

- submodule: `lib/guix`（[eclipse-threadx/guix](https://github.com/eclipse-threadx/guix) v6.5.1、**MIT**）。`threadx`/`filex`/`levelx` と同じ read-only ミラー。
- グルー: `port/guix/`、シェルコマンドは `gui`（`shell/cmds/cmd_gui.c`）。
- 遅延起動: `gui start` で初めて GUIX が LCD とタッチを占有する。未起動時は `lcd`/`touch` のテストコマンドが従来どおり使える。

## 構成

| 項目 | 値 | 備考 |
|------|----|------|
| GUIX | Eclipse ThreadX GUIX **v6.5.1**（MIT） | `lib/guix`、`common/src/*.c` を glob |
| port | Cortex-M7 / GNU（`ports/cortex_m7/gnu/inc/gx_port.h`） | **asm 無し**（ThreadX に bind） |
| 設定 | `port/guix/gx_user.h`（`GX_INCLUDE_USER_DEFINE_FILE`） | FX/LX と同作法 |
| ディスプレイ | 480×272 **RGB565** 1 layer、LTDC ダブルバッファ | canvas = LTDC back buffer |
| 描画加速 | **DMA2D**: 単色塗り（R2M）+ ダブルバッファ copy-forward（M2M） | RM0385 §9 |
| 入力 | FT5336（I2C3）ポーリング → GUIX pen イベント | `port/guix/guix_touch.c` |
| スレッド | GUIX システム=prio **14** / 入力=prio **13** | `gx_user.h` / `guix_touch.c` |

## スレッドと優先度

GUIX の既定 `GX_SYSTEM_THREAD_PRIORITY` は 16 で、本プロジェクトの shell インスタンス（cli prio 16）と衝突する。`gx_user.h` で **14** に再割当する。全体像（小さいほど高優先）:

```
IWDG petter (5)  >  camera/LED (10)  >  touch-input (13)  >  GUIX (14)  >  shell (16)  >  bg job (17)
```

GUIX はイベント駆動で、入力もアニメも無いときはイベントキューで sleep する。よって shell より高優先でもコンソールを枯渇させない。入力スレッド（13）を GUIX（14）より僅かに高くして、GUIX が起きる前に pen イベントがキューに積まれているようにする。これらは **ThreadX スレッド優先度**で、SysTick 例外優先度（14）とは無関係。

## ディスプレイドライバ（565rgb + ダブルバッファ + DMA2D）

`port/guix/guix_display.c`。`_gx_display_driver_565rgb_setup()` で GUIX のソフトウェア 565rgb ドライバを敷き、その上に以下を差し替える。

### tear-free ダブルバッファ（buffer toggle）

GUIX の canvas メモリを **LTDC の back buffer**（`ltdc_back_buffer()`）に束ねる。GUIX が 1 フレーム分の dirty を back に合成し終えると `gx_display_driver_buffer_toggle` が **1 回**呼ばれる。その中で:

1. `ltdc_gui_flip()` で tear-free に提示（SRCR.VBR 確定、#53）。
2. canvas メモリを新しい back buffer に付け替える。
3. dirty 矩形を **新 front（今提示した面）→ 新 back** へ DMA2D M2M で copy-forward。

これで両 FB が常に一致し、次フレームも GUIX は変化分だけを正しい前フレーム上に再描画できる。両 FB は `ltdc_init()` で黒クリア済なので、初フレームから invariant が成立する。

!!! note "単一 visible canvas / 部分 FB 無効が前提"
    この toggle は「1 フレームの蓄積 dirty に対し toggle が 1 回」を前提とする。`gx_user.h` で **`GX_ENABLE_CANVAS_PARTIAL_FRAME_BUFFER` を定義しない**（既定 off）こと、UI は**単一の managed + visible なフルサイズ canvas**のみとすること、で成立する。

### DMA2D 加速の範囲

GUIX のソフトウェア 565rgb ドライバの関数ポインタを、DMA2D で加速できる経路だけ差し替える（各 override は **対応条件をガードし、非対応は保存済み SW 関数へフォールバック**。SW 経路が正確性ベースライン）:

| GUIX driver | DMA2D | 加速条件 / フォールバック |
|---|---|---|
| `horizontal_line_draw`（単色塗り） | **R2M** | 常時。色は native RGB565 → ARGB8888 展開（F7 HAL の R2M クセ、#53）。半透明 brush(alpha≠0xff)は SW |
| `pixelmap_draw`（画像） | **M2M** | 非圧縮・不透明・非透過の **RGB565** かつ source が DMA2D-coherent（SDRAM/Flash）。それ以外は SW |
| `pixelmap_blend` | **M2M_BLEND** | RGB565+global alpha（REPLACE）/ ARGB4444 per-pixel×global（COMBINE）。565+alpha plane・圧縮等は SW |
| `canvas_copy` / `canvas_blend` | **M2M / M2M_BLEND** | 多キャンバス合成用（本 UI の単一 canvas では非実行）。cacheable SRAM canvas は SW |
| buffer toggle の copy-forward | **M2M** | 上記ダブルバッファ |
| `block_move`（スクロール） | — | **常に SW**。同一バッファ内移動は本質的に src/dst が重なり、DMA2D M2M は memmove ではないため安全に加速できない |
| グリフ（文字）・線 | — | SW（GUIX 565rgb） |

`gx_draw_context_pitch` は**ピクセル単位**（= canvas 幅 480、`gx_display_driver_row_pitch_get` が返す byte 値とは別）。pixelmap_draw は #56 カメラフレーム（SDRAM の RGB565）の blit 経路でもある。検証用に起動時に小さな RGB565 カラーバー画像を SDRAM に生成し `GX_ICON` で表示する（pixelmap_draw の DMA2D M2M 経路を実機 golden 確認）。

### キャッシュコヒーレンシ

出力先 FB は SDRAM の MPU non-cacheable 領域（#40）なので、CPU のグリフ描画・DMA2D・LTDC 読み出しは構造的に coherent でキャッシュ操作不要。フォント/色テーブルは flash 常駐の `const`（DMA2D の source になっても read-only で clean 不要）。将来 SRAM 常駐の pixelmap を DMA2D source にする場合は、起動前に 32 バイト境界にアラインした `SCB_CleanDCache_by_Addr()` を行うか SW フォールバックする。

### DMA2D エンジン共有

DMA2D は単一エンジンで、`lcd` コマンド（`ltdc_display.c`）と GUIX の両方が使う。GUIX 側の DMA2D 操作はすべて `ltdc_lock_frame()` 配下で実行し、`lcd` 側と同一の `ltdc_lock`（再帰 mutex）で直列化する。さらに GUIX 稼働中は下記の所有権インターロックで `lcd` 描画自体が無効化されるため、両者が画面を奪い合うこともない。

## LTDC 所有権インターロック

`ltdc_lock` は個々のアクセスを直列化するだけで、GUIX の canvas が back を指している最中に shell の `lcd fill`→`ltdc_flip()` が `ltdc_front` を動かすのを防げない（GUIX が表示面に描いてしまう）。そこで GUIX 稼働中は **表示の所有権**を取る（`port/ltdc/ltdc_display.c`）:

- `ltdc_gui_take(true/false)` … `ltdc_lock` 配下でフラグ set/clear（in-flight な描画と atomic）。
- GUIX 所有中、公開描画ヘルパ（`ltdc_fill` 等）と公開 `ltdc_flip()` は `ltdc_lock` 保持下で **no-op / 拒否**になる。GUIX は専用入口 `ltdc_gui_flip()`（所有ガード無し）で提示する。
- `cmd_lcd` の描画系サブコマンドは開始時に `ltdc_gui_owns()` を見て「display owned by gui; run `gui stop` first」を返す。長時間ジョブ `lcd anim` はループ毎に再チェックして自発終了する。

これで前面・バックグラウンド・in-flight いずれの `lcd` 描画も GUIX のフレームバッファ不変条件を壊さない。

## 入力ドライバ（FT5336 → GUIX pen イベント）

`port/guix/guix_touch.c`。専用スレッド（prio 13、stack 1 KB）が ~60 Hz で `touch_read()`（#54）をポーリングし、最初のタッチ点を GUIX の pen イベント（`GX_EVENT_PEN_DOWN` / `_PEN_DRAG` / `_PEN_UP`）に変換して `gx_system_event_send()` する。座標は FT5336 が返すパネルピクセル直値（スケール不要、#54）。`gx_system_event_send()` は内部で `TX_NO_WAIT` の `tx_queue_send` なので、キュー満杯時はそのサンプルを捨てて次ポーリングで回復する。

停止は**協調式**で、`active` フラグを見てループ先頭（= I2C トランザクションの外）で自ら sleep/park する。`tx_thread_suspend()` は使わない（touch mutex を保持/待機中に止まり得るため）。

## ライフサイクル（`gui` コマンド）

`port/guix/guix_glue.c`。`gx_system_initialize()` + display/canvas/widget 生成 + `gx_system_start()` は**初回 1 回だけ**（GUIX システムスレッドと global オブジェクトは tear-down しない）。

- `gui start` … 初回は上記初期化 → 所有権取得 → `gx_widget_show(root)` → `gx_system_start()` → 入力スレッド起動。2 回目以降（stop 後の再開）は**再 initialize せず**、所有権再取得 → canvas を back に再同期 → 再 show → `GX_EVENT_REDRAW` を 1 発 post して sleep 中の GUIX スレッドを起こし全再描画。
- `gui stop` … 入力スレッド park → `gx_widget_hide(root)` → DMA2D で画面を黒 blank（公開 `ltdc_fill` は所有中 no-op なので owner 経路）→ 所有権解放（`lcd` 描画が再び通る）。
- `gui info` … 状態・GUIX システムスレッド優先度・display handle・canvas アドレス。

UI（`port/guix/guix_app.c`）は GUIX Studio を使わず手書き。色/フォントテーブルを手で組み、テキストは GUIX 内蔵の `_gx_system_font_8bpp`（`lib/guix/common/src/gx_system_font_8bpp.c` を glob でコンパイル）を使う。画面 0（タイトル + 「Next」ボタン）と画面 1（「Back」ボタン）を root の子ウィンドウとして作り、ボタンの `GX_EVENT_CLICKED` を親ウィンドウの event process で受けて `gx_widget_show/hide` で切り替える。全ウィジェット static なので GUIX のメモリアロケータは不要。

## メモリ / フラッシュ

| 領域 | 増分 | 備考 |
|------|------|------|
| FLASH | ~107 KB（170→277 KB / 1 MB の 26.5%） | GUIX core（gc-sections + binres 除外後） |
| RAM | GUIX スレッド stack 8 KB + 入力 stack 1 KB + GUIX 内部 static | 256 KB 中 ~32% |
| SDRAM | 増分なし | canvas は既存 LTDC ダブルバッファを流用 |

## 使い方

```text
sh> lcd info          # 起動直後は従来どおり LCD テストが使える
sh> gui start         # GUIX UI を表示（LCD + タッチを占有）
sh> gui info          # 状態
# 画面の「Next >」をタップ → 画面 2 へ。「< Back」で戻る。
sh> lcd fill red      # GUIX 稼働中は拒否される（display owned by gui）
sh> gui stop          # 画面を消して LCD を lcd コマンドへ返す
```

## 実装メモ

- `gx_draw_context_pitch` は**ピクセル単位**（byte ではない）。`gx_display_driver_row_pitch_get` は byte を返すが、ドライバ内部の `USHORT*` 行送りはピクセル。
- `GX_RECTANGLE` の left/top/right/bottom はすべて **inclusive**。
- error-checking 有効ビルドでは `gx_prompt_text_color_set` / `gx_text_button_text_color_set` は **4 引数**（normal/selected/**disabled**）。
- クリーンルーム実装。GUIX 本体は read-only submodule で編集しない。グルーは `port/guix/`。
