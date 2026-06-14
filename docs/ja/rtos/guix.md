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

!!! note "完了待ちはポーリング＝呼んだスレッドが spin する（#59 で CPU が下がった理由）"
    各 DMA2D 操作は **`HAL_DMA2D_PollForTransfer`** で**同期完了待ち**する。**コピーは DMA2D エンジンが行う**が、**呼んだスレッドは完了まで busy-wait（spin）して CPU を消費**する。よって DMA2D 操作を 1 本減らすと、その **SDRAM トラフィック**と**スレッドの spin 時間**の両方が消える。これが #59 の B2（カメラ **copy-forward**＝毎フレーム GUIX スレッド上で走る 320×240 ≈ 150KB の M2M blit、を全廃）で **GUIX スレッド CPU がほぼ半減（≈5.5%→2.4%、`thread` で実測）**した理由：GUIX スレッドの polled DMA2D 待ちが 1 フレーム 2 回（`pixelmap_draw` + copy-forward）から 1 回に減った。ポーリング採用は実装の単純さと `ltdc_lock` クリティカルセクションを短く保つためで、転送は短く（数十µs）CPU 余裕も大きい（idle ~73%）ので spin コストは実害にならない。**割り込み駆動化**（`HAL_DMA2D_Start_IT` + DMA2D IRQ → セマフォ）すれば待ちスレッドの CPU を idle（WFI で省電力）/下位スレッドに回せるが、DMA2D は単一エンジンで `ltdc_lock` 直列のため**並行性は増えず**、短転送では context switch/ISR overhead が転送時間に匹敵する。将来最適化として **#64** で追跡。

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
| SDRAM | canvas は増分なし（既存 LTDC ダブルバッファを流用） | カメラプレビュー（#56）は view バッファ +150 KB（320×240×2） |

## カメラライブプレビュー（#56）

`port/guix/guix_camera.c`。DCMI ストリーミング producer（#46, `port/camera`）が `frame_pipeline` に publish する QVGA RGB565 フレームを、GUIX 画面に **等倍（スケールなし）** でライブ表示する。

データフロー:

1. **push sink**（`guix_cam_sink`, `FRAME_POLICY_DROP`）を `camera_preview_start()` で pipeline に attach。producer スレッド（prio 10）上で `consume()` が呼ばれる。
2. `consume()` は ring slot を **DMA2D M2M でプライベート view バッファ `cam_view_buf`（320×240, `.sdram` 非キャッシュ）へコピー**（`guix_display_copy_rgb565`, ltdc_lock 下）し、slot を即 `put`（同期完結 — pin をスレッド跨ぎ保持しない）。コピー成功時のみ coalesce フラグを立て `GX_EVENT_CAMERA_FRAME` を root へ post（`gx_system_event_send`, スレッドセーフ）。送信失敗時はフラグを立てず次フレームで再試行（freeze 回避）。
3. GUIX システムスレッド（prio 14）の **root イベントハンドラ**が `GX_EVENT_CAMERA_FRAME` を受け、`gx_system_dirty_mark(cam_icon)`（dirty 操作は GUIX スレッド限定 — 他スレッドからは event 送信のみ）。
4. `cam_icon`（`GX_ICON`, 画面 2, 配置 (80,16)）の再描画が `guix_pixelmap_draw` を呼び、view バッファを **DMA2D M2M で後段バッファへ等倍 blit** → `guix_buffer_toggle` が SRCR.VBR で tear-free present。

**view バッファを 1 枚挟む**ことで、GUIX の再描画（タッチ・画面遷移・初回 show）が ring slot のライフタイムから分離される。slot→view と view→後段 の両 blit は **ltdc_lock 直列**の DMA2D なので、producer（prio 10）が GUIX（prio 14）をプリエンプトしても 1 フレーム内 tear は起きない（ltdc_lock は TX_INHERIT で優先度逆転も防ぐ）。

**所有権モデル**（`ltdc_gui_take` と同型）: preview 稼働中は `cam_ext_sink` がセットされ、公開 `camera stream start/stop` は `CAM_ERR_STATE` で拒否。逆に plain `camera stream` 稼働中は `gui camera on` が失敗する。DCMI overrun 等の **非同期 teardown** も `cam_ext_sink` を detach + 解放するので、次の `frame_pipeline_init` が稼働中 sink を memset することはない。`gui camera off`（または画面 2 の Back ボタン）は stream 停止（owner 一致時のみ）→ in-flight `consume()` を bounded drain、の順。

制御は shell から:

- `gui camera on` … GUIX 未起動なら自動で `gui start` → streaming 開始 + sink attach → 画面 2 に切替（probe/configure を伴うため shell スレッドで実行）。
- `gui camera off` … 画面 0 へ復帰 → stream 停止 + sink detach/drain。
- 画面 2 の **Back ボタン**でも停止+復帰（`guix_camera_off` は bounded drain のみなので GUIX スレッドから呼んでも安全）。

**帯域**: QVGA でも LTDC 連続 read + DCMI write + DMA2D blit/表示フレームが SDRAM（16bit FMC@108MHz）に同時に乗る。平均は帯域内だが、実機では `camera stream stats` の `dcmi_ovr` / `cam_ring_ovr` / `dma fe/s` と LTDC underrun を監視し 0 近傍を確認すること。

!!! note "#59: DMA2D ラウンドトリップ削減（FE 緩和）"
    当初は表示フレームあたり DMA2D を 3 回（slot→view, view→後段, toggle copy-forward）回しており、これが SDRAM 競合と DCMI DMA FIFO error (FE) の一因だった。#59 で 2 本削った:

    - **B1**: `consume()` は `cam_redraw_pending` 中（前フレーム未描画）は slot→view コピーを**コアレス**（slot は必ず `put`）。フレームが redraw を上回る競合時にこそ効く。
    - **B2**: カメラ矩形は毎フレーム全面再描画されるので `guix_buffer_toggle` の **copy-forward を定常で全廃**。整合は **per-buffer の stale フラグ（`cam_buf_stale[2]`、不変条件「false ⟺ その面のカメラ矩形 == 最新 view」）+ flip 直前の corrective copy** で担保する。stale フラグの遷移は対応する DMA2D コピーと同一 `ltdc_lock` 区間に閉じ込め、producer が view を進めても false-negative を出さない。toggle の B2 経路は `cam_visible`（SHOW/HIDE で開閉）でゲートし、他画面にカメラ画素を焼かない。あわせて LCD_CLK を 9.6→4.8 MHz に下げた（[display](../hardware/display.md)）。指標は `dma fe/s`。

## 使い方

```text
sh> lcd info          # 起動直後は従来どおり LCD テストが使える
sh> gui start         # GUIX UI を表示（LCD + タッチを占有）
sh> gui info          # 状態
# 画面の「Next >」をタップ → 画面 2 へ。「< Back」で戻る。
sh> gui camera on     # カメラライブプレビュー（320x240 等倍）。GUIX 未起動なら自動 start
sh> gui camera off    # プレビュー停止（画面 2 の Back ボタンでも可）
sh> lcd fill red      # GUIX 稼働中は拒否される（display owned by gui）
sh> gui stop          # 画面を消して LCD を lcd コマンドへ返す
```

## 実装メモ

- `gx_draw_context_pitch` は**ピクセル単位**（byte ではない）。`gx_display_driver_row_pitch_get` は byte を返すが、ドライバ内部の `USHORT*` 行送りはピクセル。
- `GX_RECTANGLE` の left/top/right/bottom はすべて **inclusive**。
- error-checking 有効ビルドでは `gx_prompt_text_color_set` / `gx_text_button_text_color_set` は **4 引数**（normal/selected/**disabled**）。
- クリーンルーム実装。GUIX 本体は read-only submodule で編集しない。グルーは `port/guix/`。
