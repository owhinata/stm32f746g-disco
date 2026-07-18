<!--
SPDX-License-Identifier: MIT
Copyright (c) 2026 ThreadX Shell Project
-->

# 所有権・状態モデル（display / DCMI / touch）

いくつかのサブシステムが、同時に **1 者** しか駆動できない 3 つのハードウェアを共有する:
**LTDC ディスプレイ**、**DCMI / カメラ**取り込み経路、I2C3 上の **FT5336 タッチ**
コントローラ。shell、`lcd`/`camera`/`touch` コマンド、GUIX カメラ UI がいずれもこれらを
使いたがる。本ページは **どの状態で誰が何を所有し、どの shell コマンドが許可されるか** の
単一リファレンス — `shell/cmds/cmd_*.c` のクロスサブシステムガードの背後にあるモデルである。

!!! warning "DCMI/カメラの所有権モデルは #100 で刷新（本節は Phase 3 #102 で全面改訂予定）"
    Epic #99 Phase 1（#100）で、カメラは「上位機能が下位を抱き込むモード所有（単一
    `cam_ext_sink`）」から **「明示 base capture（`camera stream`）+ subscriber cascade」**
    に変わった。要点:

    - **base = `camera stream`**（`camera stream start [res]` で ON）が唯一の DCMI 取り込み。
      `port/camera/camera.c` の subscriber registry `cam_subs[]`（`{sink, fmt, enabled,
      attached}`、`cam_lock` 下）が membership を一元管理する（`cam_ext_sink` 単一ポインタは廃止）。
    - **機能 = subscriber**: GUI preview / AI 推論(nncam) / MJPEG は各自 start/stop で「有効化」し、
      base 稼働中かつ format 一致（gui/ai=RGB565, mjpeg=JPEG）のときだけ自分の sink を attach する。
      subscriber の有効状態は base の on/off と **直交**。
    - **`camera stream stop` / `camera off` = cascade**: 全 attached subscriber を `close()` で
      detach して producer を停止（BUSY 拒否ではなく master switch）。**auto-stop は無い**
      （subscriber を全部止めても base は明示 stop まで ON）。
    - **`gui overlay` コマンドは廃止**: AI 稼働中（`ai stream start`）は preview に常時 bbox を描画。
    - **DCMI overrun 自動復帰は base capture(producer)側**へ移設（旧 GUI overlay 専用 backoff は廃止）。
    - boot は preview を subscribe した上で base を 1 回自動起動する（out-of-box で live preview）。

    以下の「3 モード（SHELL / CAM-STREAM / GUI）」表は旧モデルの記述であり、#102 で置き換える。

## 共有リソースと所有者

| リソース | 所有フラグ（場所） | 取得 | 解放 |
|---|---|---|---|
| **LTDC ディスプレイ**（描画 + scanout） | `ltdc_gui_owned`（`port/ltdc/ltdc_display.c`） | `gui start` → `ltdc_gui_take(true)` | `gui stop` |
| **DCMI / カメラ**（単一の取り込み経路） | `cam_stream_active` + `cam_ext_sink`（`port/camera/camera.c`） | shell `camera stream start` **または** GUIX ライブプレビュー | `camera stream stop` / `gui stop` |
| **FT5336 タッチ**（I2C3） | *フラグ無し* — `guix_is_up()` が代理 | `gui` 稼働中の GUIX タッチスレッド（`port/guix/guix_touch.c`） | `gui stop` |

ストレージ媒体（`sd`/`fs`/`qspi`）は独自の `fs_raw_begin`/`fs_raw_end` スロットを持つ
（[ファイルシステム](../rtos/filesystem.md) 参照）。display/input の関心事ではないのでここでは扱わない。

## 状態

所有権は相互に影響する 3 つの **モード** で決まる。カメラの **電源**（on/off）は直交軸で、
`camera stream`/preview は必ず on にし、off にできるのは `SHELL` からのみ。

```
                       camera stream start
   ┌────────────────────────────────────────────────┐
   │                                                 ▼
┌──────────────┐                            ┌────────────────────┐
│    SHELL      │     camera stream stop     │    CAM-STREAM      │
│   (idle)      │ ◀──────────────────────────│  shell が DCMI 所有 │
│               │                            └────────────────────┘
│ display 自由  │
│ DCMI 空き     │          gui start         ┌────────────────────┐
│ touch 自由    │ ─────────────────────────▶ │       GUI          │
│ camera 電源   │                            │ display + DCMI     │
│   on/off 任意 │ ◀───────────────────────── │ (preview) + touch  │
└──────────────┘          gui stop           │ (poller) を所有    │
                                             └────────────────────┘

  • CAM-STREAM と GUI は DCMI で排他: 取り込み経路は 1 本なので、`gui start` は
    カメラが空いている必要があり、GUIX preview が握っている間は
    `camera stream start` は拒否される。
  • カメラ電源は直交: on/off は SHELL からのみ。STREAM/GUI 中は常に on
    （off / probe / on / res / format / fps / timing 系 set は全て BUSY）。
```

## 各状態でのコマンド可否

`OK` = 成功 · `✗` = 拒否（理由） · `read-only` = 既存の状態/フレームのみ読む（許可）。

| コマンド | SHELL (idle) | CAM-STREAM | GUI 稼働中 |
|---|---|---|---|
| `lcd` 描画（`fill`/`bar`/`grad`/`clear`/`anim`/`blit`） | OK | OK *(SDRAM 競合あり)* | ✗ `owned by gui; gui stop first` |
| `lcd on` / `lcd off` | OK | OK | backlight のみ *(scanout は gui 保持)* |
| `lcd info` | OK | OK | OK |
| `camera probe` / `on` / `off` | OK | ✗ BUSY | ✗ BUSY |
| `camera res` / `format` / `fps` / `set`*(timing)* | OK | ✗ BUSY | ✗ BUSY |
| `camera stream start` | OK → CAM-STREAM | *(既に稼働)* | ✗ BUSY *(preview が DCMI 所有)* |
| `camera stream stop` | ✗ not streaming | OK → SHELL | ✗ BUSY *(gui の preview は止められない)* |
| `camera stream stats` | read-only | read-only | read-only *(preview の統計)* |
| `camera capture` | OK | ✗ BUSY | ✗ BUSY |
| `camera info` / `save` / `send` | OK | read-only | read-only |
| `gui start` | OK → GUI | ✗ camera busy | *(既に稼働)* |
| `gui stop` | ✗ not running | ✗ not running | OK → SHELL |
| `touch probe` / `touch read` | OK | OK | ✗ `owned by gui; gui stop first` *(#73)* |
| `touch info` | OK | OK | OK |
| `sd` / `fs` / `qspi` | OK | OK | OK |

## インターロックの実体（ガードの所在）

- **Display** — `ltdc_gui_owns()` が公開描画/flip（`ltdc_fill`/`ltdc_flip`/…）を
  すべてゲートし、shell `lcd` 描画系は `lcd_can_draw()`（`shell/cmds/cmd_lcd.c`）で
  事前に拒否する。GUIX は所有者専用の `ltdc_gui_flip()` で present する。
  `ltdc_set_scanout()` は GUIX 所有中は拒否されるため、その状態の `lcd on`/`off` は
  backlight だけを切り替える。
- **DCMI / camera** — `cam_stream_active` か `cam_ext_sink` がセットされている間、
  `camera_stream_start`/`stop`・`camera_capture`・`camera_set_format`/`fps`・
  `camera_power_off`・`camera_probe` は `CAM_ERR_BUSY`（`busy (streaming or preview
  active)` と表示）を返す。GUIX preview 自体が stream なので `cam_stream_active` を
  立てる。read-only 経路（`info`/`save`/`send`/`stream stats`）はカメラロックのみ取り
  常に許可。
- **NET-MJPEG**（#49 P5、**#101 Phase 2 で subscriber 化**）— `net mjpeg start` は base を
  所有せず、**稼働中の JPEG base に attach する JPEG-class subscriber**（`camera_subscribe_oneshot(&eth_sink, CAM_FMT_JPEG)`）。base off / RGB565 base なら明示エラー（#97 解消）。`camera stream stop`/`camera off`（非 recover 停止）の cascade で完全リリース＝自動停止、一過性 overrun では pause→再開（`camera_subscribed()` を単一真実源に判別）。`net mjpeg stop` は自分の sink を detach するだけ（base 継続）。eth_sink は同期 copy sink ゆえ in-flight 0、producer の async teardown もそのまま安全。（旧 `camera_mjpeg_start` の base 所有ゲートは撤去。全面改訂は #102。）
- **Touch** — タッチの所有権フラグは無い（`guix_is_up()` が代理）。shell は 2 段で守る:
  (1) `touch_read()`（`port/touch/touch.c`）が FT5336 の全 1「タッチ無し」センチネル
  — idle や直後リリースのコントローラが *非ゼロ* の status count とともに報告する
  パネル外 `0xFFF/0xFFF` 点（GUIX ドライバが捨てるのと同じ点）— を drop するので、
  ポーリングが phantom `P15 x=4095 y=4095 event=3` を出すことはない。
  (2) バスを叩く `touch probe`/`read` は `guix_is_up()` の間は拒否する
  （`shell/cmds/cmd_touch.c`、#73）。GUIX タッチスレッドと並行する shell ポーリングは
  2 つ目の非同期 I2C3 マスタになる（`touch_lock` は単一トランザクションしか直列化しない）
  ため。`touch info` は I/O 無しなので許可継続。

## 注意点

- **GUI 中の `lcd on`/`off`** は backlight を切り替えるが scanout は動いたまま（GUIX が
  所有）— 意図的に許可している（パネル消灯は UI に無害）。`gui stop` までは *見える* もの
  はすべて GUIX のもの。
- **read-only コマンド**（`*info`・`camera save`/`send`・`camera stream stats`）は全状態で
  動作する。ハードウェアを再 arm しない。ただし外部 sink 付き配信中（MJPEG / GUIX preview）は
  `camera save`/`send` が **最後に外部 sink 無しで撮ったフレーム**（無ければ `CAM_ERR_NO_FRAME`）を
  返す（#82、[カメラ](../hardware/camera.md) の JPEG streaming 節を参照）。
- 各サブシステムの詳細は [カメラ](../hardware/camera.md)・[ディスプレイ](../hardware/display.md)・
  [タッチ](../hardware/touch.md)・[GUIX](../rtos/guix.md) を参照。
