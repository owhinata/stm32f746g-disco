<!--
SPDX-License-Identifier: MIT
Copyright (c) 2026 ThreadX Shell Project
-->

# 所有権・状態モデル（display / DCMI / touch）

いくつかのサブシステムが 3 つのハードウェアを共有する: **LTDC ディスプレイ**、**DCMI /
カメラ**取り込み経路、I2C3 上の **FT5336 タッチ**コントローラ。shell、`lcd`/`camera`/`touch`
コマンド、GUIX カメラ UI がいずれもこれらを使いたがる。本ページは **どの状態で誰が何を所有し、
どの shell コマンドが許可されるか** の単一リファレンス — `shell/cmds/cmd_*.c` のクロスサブシステム
ガードの背後にあるモデルである。

- **LTDC ディスプレイ** と **FT5336 タッチ** は同時に **1 者**しか駆動できない **単一所有**。
- **DCMI / カメラ** は Epic #99（#100/#101）で単一所有をやめ、**明示 base capture（`camera
  stream`）+ subscriber cascade** になった: 1 本の取り込み経路（producer）に、内部 stat sink と
  最大 3 つの外部 subscriber（GUI preview / AI 推論(nncam) / MJPEG）が **同時に attach** できる。

## 共有リソースと所有者

| リソース | 所有/状態フラグ（場所） | 起動 | 停止 |
|---|---|---|---|
| **LTDC ディスプレイ**（描画 + scanout、単一所有） | `ltdc_gui_owned`（`port/ltdc/ltdc_display.c`） | `gui start` → `ltdc_gui_take(true)` | `gui stop` |
| **DCMI / カメラ**（base capture + subscriber cascade） | `cam_stream_active` + subscriber registry `cam_subs[]`（`{sink, fmt, enabled, attached, oneshot}`、`cam_lock` 下、`port/camera/camera.c`） | shell `camera stream start`（base の唯一の起動主体） | `camera stream stop` / `camera off`（cascade） |
| **FT5336 タッチ**（I2C3、単一所有） | *フラグ無し* — `guix_is_up()` が代理 | `gui` 稼働中の GUIX タッチスレッド（`port/guix/guix_touch.c`） | `gui stop` |

ストレージ媒体（`sd`/`fs`/`qspi`）は独自の `fs_raw_begin`/`fs_raw_end` スロットを持つ
（[ファイルシステム](../rtos/filesystem.md) 参照）。display/input の関心事ではないのでここでは扱わない。

## base capture + subscriber cascade（DCMI）

DCMI は 1 経路・1 フォーマット・1 解像度という物理制約を持つ（RM0385 §14）。旧「モード所有」
（上位機能が下位を単一 `cam_ext_sink` で抱き込む）は廃止され、次のモデルになった:

- **base = `camera stream`**: `camera stream start` で ON になる唯一の DCMI producer。format/res は
  base のプロパティ（`camera format`/`camera res`/`camera fps` で **base OFF 中に**設定）。ON 中は
  内部 `cam_stat_sink`（FPS/overrun カウント）が常時 attach する。
- **機能 = subscriber**: GUI preview / AI(nncam) / MJPEG は各自の start/stop で **有効化**（`enabled`）し、
  **base 稼働中かつフォーマット一致のときだけ** 自分の sink を pipeline に attach する。subscriber の
  有効状態は base の on/off と **直交**する（base OFF 中に有効化した subscriber は次の base start で attach）。
- **フォーマットクラス仲裁**: base は 1 フォーマット。**RGB565 raster** を消費する subscriber
  （GUI preview / AI / stat）は共存できる。**JPEG** は排他クラス（MJPEG のみ）。非互換な明示 attach 要求は
  「base の現フォーマット」を述べる明示エラーで拒否する（例: RGB565 base 中の `net mjpeg start`）。
- **`camera stream stop` / `camera off` = cascade**: 全 attached subscriber を `close()` で detach して
  producer を停止する（BUSY 拒否ではなく master switch）。**auto-stop は無い**（subscriber を全部止めても
  base は明示 stop まで ON）。cascade 時 GUI preview は最後のフレームで**凍結**（persistent、再 attach）、
  AI は PAUSE（run/session 維持）、MJPEG は**停止**（oneshot、完全リリース）。
- **カメラ電源は直交**: `camera on`/`off` は base とは別軸。base ON 中の `camera off` は cascade で
  stop してから電源断。
- **DCMI overrun 自動復帰は producer 側**（`cam_stream_recover`、escalating backoff）。一過性 overrun では
  subscriber を維持して re-attach、断念すると base OFF（旧 GUI overlay 専用 backoff は廃止）。

## 状態

DCMI は「排他 3 モード」ではなく、**base の on/off（+ フォーマット）** と **各 subscriber の
enabled/attached** の直交した状態で決まる。カメラ**電源**（on/off）はさらに直交軸。

```
   camera format {rgb565|jpeg|...}         camera stream start
   camera res / fps  (base OFF 中のみ) ──────────────────────────┐
        ▲                                                        ▼
┌───────────────────┐                          ┌────────────────────────────────┐
│  base OFF          │   camera stream start    │  base ON  (1 フォーマット)      │
│  (DCMI idle)       │ ───────────────────────▶ │  producer + cam_stat_sink       │
│  camera capture 可 │ ◀─────────────────────── │  + 有効 & 適合な subscriber を   │
│  format/res 変更可 │   camera stream stop      │    attach (cascade)             │
└───────────────────┘   camera off (cascade)    └────────────────────────────────┘
                                                     │  ▲ subscriber は各自 start/stop で
   subscriber (gui / ai / net mjpeg):                │  │ 有効化。base ON & fmt 一致で attach、
   有効化は base と直交。base ON & fmt 一致で attach ─┘  │ base OFF/不一致なら enabled のまま idle。
   base stop = cascade で gui 凍結 / ai PAUSE / mjpeg 停止 ┘

  • RGB565 raster (gui/ai/stat) は共存。JPEG (mjpeg) は排他クラス。
  • カメラ電源は直交: base ON 中は off/probe/res/format/fps を BUSY 拒否。
```

## 各状態でのコマンド可否

`OK` = 成功 · `✗` = 拒否（理由） · `read-only` = 既存の状態/フレームのみ読む（許可）。
列は base の状態（OFF / ON=RGB565 raster / ON=JPEG）。GUI/AI/MJPEG は subscriber として直交に効く。

| コマンド | base OFF | base ON (RGB565) | base ON (JPEG) |
|---|---|---|---|
| `camera probe` / `on` / `off` | OK | ✗ BUSY *(off は cascade stop 後に電源断)* | ✗ BUSY |
| `camera res` / `format` / `fps` / `set`*(timing)* | OK | ✗ BUSY *(`camera stream stop` first)* | ✗ BUSY |
| `camera capture` | OK | ✗ BUSY | ✗ BUSY |
| `camera stream start` | OK → base ON | *(既に稼働)* | *(既に稼働)* |
| `camera stream stop` | ✗ not streaming | OK → base OFF *(cascade)* | OK → base OFF *(cascade)* |
| `camera stream stats` / `info` | read-only | read-only | read-only |
| `camera save` / `send` | 最後の `camera capture`（無ければ no frame） | **最新 raster フレーム** | **最新 JPEG フレーム** |
| `gui start` | preview idle（"no capture"）で起動 | live preview（attach） | preview 不可（JPEG、"needs RGB565"） |
| `gui stop` | unsubscribe（base/AI は継続） | unsubscribe | unsubscribe |
| `ai stream start` / `stop` | 有効化/無効化（base off なら idle） | attach / detach（gui と独立） | idle *(RGB565 需要、JPEG base では未 attach)* |
| `net mjpeg start` | ✗ "no camera capture" | ✗ "raster; needs JPEG" | attach（JPEG-class subscriber） |
| `net mjpeg stop` | ✗ not running | detach | detach |
| `lcd` 描画系 | OK *(GUI 中は ✗)* | OK *(SDRAM 競合)* / GUI 中 ✗ | 同左 |
| `touch probe` / `read` | OK *(GUI 中は ✗ #73)* | 同左 | 同左 |
| `sd` / `fs` / `qspi` | OK | OK | OK |

（`gui` は LTDC + touch の単一所有も同時に取る。上表の GUI 行は DCMI subscriber としての側面。）

## インターロックの実体（ガードの所在）

- **Display** — `ltdc_gui_owns()` が公開描画/flip（`ltdc_fill`/`ltdc_flip`/…）をすべてゲートし、
  shell `lcd` 描画系は `lcd_can_draw()`（`shell/cmds/cmd_lcd.c`）で事前に拒否する。GUIX は所有者専用の
  `ltdc_gui_flip()` で present する。`ltdc_set_scanout()` は GUIX 所有中は拒否されるため、その状態の
  `lcd on`/`off` は backlight だけを切り替える。
- **DCMI / camera** — base ON（`cam_stream_active`）の間、`camera_set_format`/`fps`・`camera_capture`・
  `camera_probe`・`camera_power_off` は `CAM_ERR_BUSY`（`busy (streaming or preview active)` と表示）を返す
  ＝ base OFF にしてから設定/電源操作する。subscriber の membership は `cam_subs[]` registry が `cam_lock` 下で
  一元管理し、`camera_subscribe`（persistent: gui/nncam）/ `camera_subscribe_oneshot`（非永続: mjpeg）/
  `camera_unsubscribe` で出入りする（attach は base 稼働 && enabled && `sub.fmt==mode.format`）。read-only 経路
  （`info`/`save`/`send`/`stream stats`）はロックのみ取り常に許可。
- **NET-MJPEG**（#49 P5、#101 で subscriber 化）— `net mjpeg start` は base を所有せず、**稼働中の JPEG base に
  attach する JPEG-class subscriber**（`camera_subscribe_oneshot(&eth_sink, CAM_FMT_JPEG)`）。base off / RGB565
  base なら明示エラー（#97 解消）。`camera stream stop`/`camera off`（非 recover 停止）の cascade で完全リリース＝
  自動停止、一過性 overrun では pause→再開（`camera_subscribed()` を単一真実源に判別）。`net mjpeg stop` は自分の
  sink を detach するだけ（base 継続）。eth_sink は同期 copy sink ゆえ in-flight 0、producer の async teardown も安全。
- **Touch** — タッチの所有権フラグは無い（`guix_is_up()` が代理）。shell は 2 段で守る:
  (1) `touch_read()`（`port/touch/touch.c`）が FT5336 の全 1「タッチ無し」センチネル
  — idle や直後リリースのコントローラが *非ゼロ* の status count とともに報告するパネル外
  `0xFFF/0xFFF` 点（GUIX ドライバが捨てるのと同じ点）— を drop するので、ポーリングが phantom
  `P15 x=4095 y=4095 event=3` を出すことはない。
  (2) バスを叩く `touch probe`/`read` は `guix_is_up()` の間は拒否する（`shell/cmds/cmd_touch.c`、#73）。
  GUIX タッチスレッドと並行する shell ポーリングは 2 つ目の非同期 I2C3 マスタになるため。`touch info` は
  I/O 無しなので許可継続。

**ロック順序**: カメラ内部は `cam_lock → cam_pipe_lock`（pipeline mutex）で一貫。subscriber の
`close()` は producer thread から呼ばれ得るため、camera API を再入してはならない（非ブロッキング）。

## 注意点

- **GUI 中の `lcd on`/`off`** は backlight を切り替えるが scanout は動いたまま（GUIX が所有）— 意図的に
  許可している（パネル消灯は UI に無害）。`gui stop` までは *見える* ものはすべて GUIX のもの。
- **read-only コマンド**（`*info`・`camera save`/`send`・`camera stream stats`）は全状態で動作する。
  ハードウェアを再 arm しない。**`camera save`/`send`（#102 再定義）**: **base ON 中は常に最新の公開フレーム**を
  スナップショットして保存/転送する（raster/JPEG、MJPEG/GUI subscriber 併用中でも可）。base OFF 中は最後の
  `camera capture` フレーム（無ければ `CAM_ERR_NO_FRAME`）。#82 の「外部 sink 有無で mirror を二値ラッチ」する
  仕組みは撤去され、producer は per-frame コピーをしない（save/send 起動時に `frame_pipeline_pin_latest` で 1 回
  だけ pin コピー）。pin は producer の publish/DMA-repoint を止めないので **ライブ映像は全 fps で回り続ける**が、
  base ON 中の SD/QSPI 保存は continuous DCMI DMA と単一 16-bit FMC SDRAM を奪い合うため**遅い**（QVGA raster で
  数十秒、round-robin バス調停ゆえ低優先度の CLI save が後回しになる — #84/#90 と同じ帯域制約）。速い保存は
  `camera stream stop` してから行う。詳細は [カメラ](../hardware/camera.md) を参照。
- 各サブシステムの詳細は [カメラ](../hardware/camera.md)・[ディスプレイ](../hardware/display.md)・
  [タッチ](../hardware/touch.md)・[GUIX](../rtos/guix.md)・[フレームパイプライン](frame-pipeline.md) を参照。
