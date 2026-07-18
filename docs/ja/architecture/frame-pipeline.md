<!--
SPDX-License-Identifier: MIT
Copyright (c) 2026 ThreadX Shell Project
-->

# カメラフレームパイプライン（1 ソース → マルチシンク）

カメラフレームを **1 プロデューサ（DCMI 取り込み）→ マルチシンク（file / LTDC / Ethernet / VCP）** で分配するアーキテクチャ設計（issue #47）。`fs_device`（[ファイルシステム](../rtos/filesystem.md)、#34）が媒体を、`ym_source`（[xfer](../rtos/shell-xfer.md)、#50）がバイトソースを抽象化したのと同型の「薄い vtable + 複数バックエンド」を、フレーム配信に適用する。

!!! note "実装済み（#46）— 本ページは設計と実装の両方を記述"
    本ページは元々 #47 の IF 提案だったが、リング/ディスパッチエンジン・DMA ダブルバッファ・各シンクは **#46 で実装済み**（`svc/frame_pipeline.c`）。base capture + subscriber cascade（内部 stat sink + GUI preview / nncam / MJPEG の最大 3 subscriber の同時 attach）は **#100/#101 で稼働中**（[所有権モデル](ownership.md)）。以下は確定した抽象 IF（`svc/frame.h`・`svc/frame_pipeline.h`）と、その実装の設計根拠である。

## 背景

単発 snapshot のカメラ（[カメラ](../hardware/camera.md)、Epic #22）は `.sdram` の **1 枚の静的バッファ** `cam_frame[]` に QVGA RGB565 を撮り、`cam_frame_gen`（世代カウンタ）と `cam_done`（DCMI FRAME 完了セマフォ）で読み出しを直列化していた。その後 **LTDC ライブ表示（#48）** と **Ethernet 配信（#49）** が加わり、フレームの「ソース」と「出力先」が多対多になった。シンク直結のまま streaming を実装すると出力先を増やすたびにフレーム分配を作り直すことになる — それを避けるのが本パイプライン（#46 で実装）。

| シンク | 帯域目安 | QVGA RGB565 ライブ |
|---|---|---|
| VCP（USART1 115200） | ~0.09 Mbps | ✗ プレビュー専用 |
| File（SD / QSPI） | 数 MB/s | △ バースト / タイムラプス |
| LTDC（#48） | 内部 | ◎ ローカル表示（GUIX preview #56） |
| Ethernet（#49） | 100 Mbps | ◎ ネットワーク配信（MJPEG/HTTP #49 P5） |

## レイヤ配置（#43 整合）

パイプラインの**純 core** は freestanding な [`svc/` 層](layering.md)に置く。`tx_*`（ThreadX）も HAL も呼ばず、依存は `<stdint.h>` と注入される相互排他 vtable（`frame_os`）のみ。これによりリング/refcount/ポリシのロジックは **ホスト単体テスト可能**（no-op ロック + モックシンク）になり、`svc/ymodem.c`・`svc/fmt.c` と同じクラスに収まる（`svc/timebase.c` は HAL を含む例外だが、pipeline はそこに寄せない）。

```
HAL / CMSIS / ThreadX   ←   svc/frame_pipeline (純 core)   ←   port/camera (producer)
        (lib/)                                              ←   各 sink (shell / port / port-net)
```

ThreadX（producer/各 sink のスレッド、ISR→スレッド通知、`frame_os` の裏の `TX_MUTEX`）は **すべて glue 側**に閉じ込める。core は常にスレッド文脈から注入ロック下で呼ばれ、DCMI ISR はリングに触れず `cam_done` を post するだけ（既存規律のまま）。

| 要素 | 配置 | 依存方向 |
|---|---|---|
| パイプライン純 core | `svc/frame_pipeline.{c,h}` + `svc/frame.h` | ThreadX/HAL/FileX/shell **非依存** |
| ThreadX glue | producer / 各 sink 側 | core を駆動、`tx_*` はここだけ |
| producer | `port/camera/`（SDRAM スロット列を所有し core に注入、publish） | `port/camera → svc/`（`port/sdram → svc/timebase` と同方向） |
| push sinks | file/vcp = `shell/`、ltdc = `port/`(#48)、eth = `port/`(#49) | 各 sink → `svc/` + 自層 |
| 配線 | shell コマンド / `src/main` | sink を attach |

## 2 つのアクセス様式

```
 [Producer]               [Frame Ring (svc/ 管理 / SDRAM 実体は producer 所有)]   [Push Sinks]
 port/camera              ┌─ slot0 ─┐  各スロット: gen + refcount + state         ├─ ltdc_sink (#48)
 DCMI(+DMA2)        ──▶    │  ...    │  producer は refcount==0 のみ再利用          ├─ eth_sink  (#49)
 snapshot→continuous       └─ slotN ─┘  非同期 sink は get/put で pin               └─ vcp_sink  (preview)
       │                        │
       │ cam_done ISR(既存)      └── pull: read_latest(最新 published slot) → save / send / stats(#42/#50)
```

- **push シンク**（streaming）… `frame_pipeline_attach()` で登録し、publish ごとに `consume()` が呼ばれる。LTDC / Ethernet / VCP プレビュー。
- **pull アクセス**（snapshot）… 最新 published スロットを `frame_pipeline_read_latest()` で引く。`camera_frame_read()` の一般化。save / send / stats。

### snapshot は pipeline の縮退形

単発 `camera capture` は **N=1 リングへ 1 フレーム publish、push シンク無し、pull で読み出す**特殊例にすぎない。実装上、`camera capture` は `cam_frame[]` に直接撮り、streaming（#46）は別に多スロットのリングを持つ。`camera save`（#42）/ `camera send`（#50）は #102 で **base ON 中はリングの最新公開フレームを `frame_pipeline_pin_latest` で 1 回 pin コピー**して読み（base OFF は従来の `cam_frame[]` snapshot）、shell コマンドの意味論も公開 API も不変のまま「常に最新フレーム」を得る。

## データ契約: `frame_desc`

フレームは SDRAM スロットに置かれ、producer と sink の間で**コピーされない**。受け渡すのはこの記述子だけ。

```c
enum frame_format { FRAME_FMT_RGB565 = 0, FRAME_FMT_YUV422, FRAME_FMT_Y8, FRAME_FMT_JPEG };

struct frame_desc {
    void    *data;     /* SDRAM スロット先頭（非キャッシュ #40）。sink は read-only  */
    uint32_t bytes;    /* 有効ペイロード長（JPEG は可変、有効 JPEG 長）             */
    uint32_t gen;      /* 単調増加の世代（cam_frame_gen の一般化）                   */
    uint16_t width, height, stride;   /* stride = バイト/行（JPEG は 0）             */
    uint8_t  format;   /* enum frame_format                                          */
    uint8_t  flags;
    void    *_slot;    /* core 内部ハンドル（sink は触らない）                       */
};
```

`consume()` には `const struct frame_desc *` が渡り、`data` は sink から **read-only**（書き込むのは producer のみ）。`format` がフォーマット可変（#45）と JPEG を記述子に織り込む口で、sink は `open()` で対応可否を判定する。

## シンク契約: `frame_sink`

```c
enum frame_policy { FRAME_POLICY_DROP = 0, FRAME_POLICY_LATEST };

struct frame_sink {
    const char *name;
    void       *ctx;
    uint8_t     policy;
    int  (*open)(void *ctx, enum frame_format fmt, uint16_t w, uint16_t h); /* <0 で拒否 */
    int  (*consume)(void *ctx, const struct frame_desc *f);                /* <0 = sink エラー */
    void (*close)(void *ctx);
    uint32_t delivered, dropped, errors;          /* core が lock 下で更新 */
    /* 以下 core 内部（owner は触らない） */
    struct frame_sink *_next; const struct frame_desc *_pending; int _busy;
};
```

`consume()` は publish が **lock 外**で呼ぶ。core は呼出前にそのシンクの分のスロット参照を **1 回 pre-pin** 済みなので、同期シンクは処理して戻る前に `put()`、非同期シンクは `f` を自スレッドの queue に積んで即戻る（pre-pin がスロットを延命）→ 完了後に自スレッドから `put()`。エラー return でも **pin は必ずちょうど 1 回 put**（refcount leak 防止）。`attach()` は producer が持つ現在のフォーマット/ジオメトリ（現状は固定 QVGA RGB565、#45 で可変＝変更時に close+open）で `open()` を呼び、非対応なら attach 失敗。

## 並行性・寿命契約

core は注入ロックだけで wait 原語を持たない（待機/通知/スレッドは glue 所有）。その前提で所有権を破綻なく解く 3 契約。

### (1) publish の lock 規律と二段階 fan-out

lock が守るのは簿記（空きスロット選択 / refcount / registry・`_busy`・`_pending` / stats）だけで、**`consume()` を lock 保持のまま呼ばない**。

```text
publish(f):
  LOCK
    f.gen = ++published ; slot(f).state = READY
    deliver = []
    for s in registry:
        if s._busy:
            if s.policy == DROP:   s.dropped++ ; continue
            if s.policy == LATEST:                       # 最新だけ保持（今は配らない）
                if s._pending: put_locked(s, s._pending)  # 旧 pending の pin を落とす
                s._pending = f ; slot(f).refcount++      # 新 pending を pre-pin
                continue
        s._busy = 1 ; slot(f).refcount++                 # pre-pin（配る分だけ）
        deliver.append(s)
  UNLOCK
  for s in deliver: s.consume(f)                         # lock 外。stats/put は consume 後に短い lock で
```

`frame_pipeline_get/put` は自身が短く lock を取る。`consume()` から呼ぶ時点で publish の lock は解放済みなので、**非再帰 mutex でも自己デッドロックしない**。複数シンクが同じ記述子を受け取るため、**get/put は対象シンク `s` も引数に取る**（どのシンクの pin / `_busy` / `_pending` かはスロットだけでは特定できない）。producer の `acquire()` は **refcount==0 かつ「最新 published でない」**スロットのみ選ぶ（pull 読者の最新面を潰さない）。全スロット pin → NULL → overrun カウントして drop。

### (2) LATEST の pending 移譲

`_pending` は coalesce 時に pre-pin 済み。LATEST シンクの完了 `put()` 時の順序を固定する: ① lock ② `_pending` をローカルへ取り出し `_pending=NULL`、`_busy=1` 維持（**pin はそのまま移譲、新規 pin しない**）③ unlock ④ lock 外で `consume(local)` ⑤ 完了後に stats/put。`_pending` が無ければ ②で `_busy=0`。よって LATEST シンクの `consume` は publish（producer スレッド）か put（自スレッド完了）から呼ばれるが、`_busy` により**同一シンクで並行しない**。pin 会計は coalesce の +1 と再配布完了の put の −1 で均衡。

### (3) detach と in-flight シンクの寿命

`frame_sink` は呼出側所有なので、detach 後に queued frame や実行中 `consume()` が残ると UAF になる。2 段契約:

1. `frame_pipeline_detach()` … lock 下で registry から unlink し detaching 印。**戻り後そのシンクへ新規 `consume()` は発生しない**。戻り値 = まだ保持中の pin 数（in-flight、**LATEST の pending pin も算入**）。
2. シンク / ctx を free する前に、glue（シンクのスレッドと queue を所有）が **自スレッドを drain** し、実行中 consume を完走させ全 pin を `put()` させて in-flight==0 を確認する。core は wait しない。

publish の deliver 確定も detach も lock 下なので、detach は「確定前（除外）」か「確定後（最大 1 フレーム配布→以後 in-flight 観測）」のどちらかに直列化され、中間状態の UAF は起きない。

### pull（read_latest）の tear 検出

`read_latest(off, dst, len, gen)` は **lock 下で memcpy + gen 返却**（`camera_frame_read` と同一規律）。単発呼出はコピー中にスロットが再利用されないので tear しない。複数回読み（save の行単位）は **gen 比較で間の publish を検出**する（#42/#50 の現契約をそのまま一般化）。

## 決定事項まとめ

| 決定事項 | 確定内容 |
|---|---|
| **バックプレッシャ** | push policy = drop / latest のみ。must-complete（保存）は pull + snapshot で表現（DCMI は途中停止不可＝遅いシンクは drop） |
| **所有権 / 寿命** | 世代スタンプ + per-slot refcount。publish が配る分だけ lock 下で pre-pin、sink は put() で 1 回落とす。producer は refcount==0 かつ非最新スロットのみ再利用。N 既定 3（DMA ping-pong 2 + シンク保持 1） |
| **スレッド** | ISR（DCMI FRAME）→ `cam_done` post のみ。fan-out は producer スレッド文脈の publish()。遅いシンクは自前スレッド + queue |
| **フォーマット結合** | `frame_desc.format` + 可変 `bytes`。sink は `open(fmt,w,h)` で判定。ltdc=RGB565(or DMA2D)、eth(MJPEG)=JPEG、vcp=downscale |
| **snapshot 統一** | N=1 縮退で既存 capture/frame_read/save/send を無改造表現 |
| **エラー / OVR** | DCMI OVR → producer overrun カウント + 破棄 + re-arm 継続。sink consume()<0 → per-sink errors、producer 停止せず |

## ハードウェア根拠

- **メモリ**: リング実体は [`.sdram`](../hardware/sdram.md)（0xC0000000, 8MB, NOLOAD, MPU 非キャッシュ #40）。QVGA RGB565=150KB/面 → N=3 で 450KB、VGA=600KB/面 → N=3 で 1.8MB（8MB に余裕）。**非キャッシュ**ゆえ DCMI-DMA write / CPU read / 将来の LTDC scanout / ETH MAC-DMA いずれも cache maintenance 不要（RM0385 §2.1.10–13 で全 master が FMC へ到達可能）。
- **DMA NDTR ≤ 65535**: QVGA（38400 word）は単発 OK。VGA 以上は band 分割 / DBM が必須（#45/#46 の producer 内部事情で、`frame_desc` / リング自体は不変）。**#45 実装済**: snapshot は `HAL_DCMI_Start_DMA` の intra-frame banding で全解像度可。streaming は `frame_words ≤ 65535` のモードのみ（producer の手動 DBM が full-slot を NDTR で指すため）で、リングスロットは最大 streamable フレームを内包する 256KB 固定 stride・`frame_pipeline_init` に現モード `frame_bytes` を slot_size として渡す（背後固定 1MB）。JPEG は snapshot-only。`frame_pipeline_publish` は `bytes ≤ slot_size` を検査しないので producer 側で境界 assert。
- **DCMI モード**: snapshot は自動停止（既存）、continuous は DBM/circular（#46）。DBM の inactive `M0AR/M1AR` を完了コールバックで次の空きスロットへ張り替えて N 面リング化する（RM0385 §8.3.10）。producer は **DBM ポインタ更新を明示所有**し、`HAL_DCMI_Start_DMA(Length>0xFFFF)` の HAL 内部 contiguous DBM 分割パスには乗らない。
- **割込み / 優先度**: DCMI/DMA2_Stream1 = NVIC prio 8、SysTick 14 > PendSV 15（ThreadX 必須）。ThreadX スレッド優先度は [IWDG](../rtos/iwdg.md) petter 5 / LED 10 / shell 16 / jobs 17 で、producer ≈ 9–12・非同期 sink はその下に置く（watchdog を starve しない）。`tx_semaphore_put` は ISR 安全。

## 下流要件（#48/#49 で確定）

- **SDRAM 帯域**: リスクは容量でなく帯域。DCMI write + LTDC scanout + DMA2D + ETH DMA + CPU read が単一 FMC SDRAM に集中する。**#48/#49 で「帯域実測 + slot / 表示バッファ予算表」を必須**とする（下記予算表 #59 で着手。実スループット計測は #57 membench へ委譲）。
- **`.sdram` teardown**: `sdram test` は `.sdram` を破壊する。`camera_frame_invalidate()` だけでは LTDC framebuffer を保護できない → 「全 SDRAM 常駐 consumer を停止 / 無効化する suspend/invalidate 契約」。**実装済（#65、LTDC + camera の 2 consumer 最小実装）**: `sdram test` は camera streaming / GUIX 所有を拒否、LTDC scanout を `ltdc_set_scanout(false)` で一時停止、test 後に両 FB 黒クリア＋元状態へ復帰（[SDRAM](../hardware/sdram.md)）。将来 consumer（NetX 等）は同じ前段ガードに追記。

### SDRAM 帯域予算（#59）

QVGA RGB565 プレビューの定常時、単一 16-bit FMC SDRAM（理論 216 MB/s @ 108 MHz CAS3）を奪い合う主要コンシューマの予算。**「実測スループット」ではなく予算 + 実測カウンタ**で評価する（実 fps と `dma fe/s` は実機 verify で確定。フル membench は #57）:

| コンシューマ | 1 フレーム | 代表レート | 帯域 | #59 |
|---|---|---|---|---|
| LTDC scanout（front 面 read） | 480×272×2 = 255 KB | 29.6 Hz | ~7.8 MB/s | **(A)** 9.6→4.8 MHz で 15.5→7.8 |
| DCMI write（QVGA RGB565） | 320×240×2 = 150 KB | ~15 fps | ~2.3 MB/s | 不変 |
| DMA2D slot→view | 150 KB R+W = 300 KB | 表示 fps 依存 | — | **(B1)** redraw pending 時は省略 |
| DMA2D view→back（icon draw） | 300 KB | 表示 fps 依存 | — | 不変（必須） |
| DMA2D copy-forward | 300 KB | — | — | **(B2)** 定常で全廃 |

- **(A)** LTDC を 9.6→4.8 MHz に下げ瞬間 SDRAM 占有とバースト頻度を低減（out-of-spec、[display](../hardware/display.md)）。LTDC は active 画素のみ read するので帯域はリフレッシュにのみ比例。
- **(B1)** `cam_redraw_pending` 中の slot→view コピーをコアレス（フレームが redraw を上回る競合時にこそ効く）。
- **(B2)** カメラ矩形は毎フレーム全面再描画されるので copy-forward（旧来 3 本目の全面 DMA2D コピー ≈ 300 KB/表示frame）を定常で全廃。ダブルバッファ整合は per-buffer の stale 追跡 + present 直前の corrective copy で担保（[guix](../rtos/guix.md)）。
- 指標: `camera stream stats` の **`dma fe/s`**（理想 0 近傍）、`lcd info` の **underrun=no**、表示 tear なし。

**実測（プレビュー, QVGA RGB565, 同一 14.9 fps, 10 秒法, underrun=no）** — レバーを順に足した切り分け:

| 版 | LCD_CLK | DMA2D | dma fe/s |
|---|---|---|---|
| レバー無し | 9.6 MHz | 3 | 3408 |
| A のみ | 6.4 MHz | 3 | 2539 |
| A+B（40Hz） | 6.4 MHz | 2 | 2005 |
| **A+B（最終, 30Hz）** | 4.8 MHz | 2 | **1670** |

レバー無し **3408 fe/s** → 最終 **1670 fe/s（−51%）**。内訳: **A（クロック 9.6→6.4 MHz, 3 回固定）−869**、**B（DMA2D 3→2 回, 6.4 MHz 固定）−534**、さらに **クロック 6.4→4.8 MHz（30Hz 化）−335**。A が支配的だが B も約 4 割寄与。`ovr dcmi/ring=0`・表示安定。FE は 0 にはならない（残 ~1.17%/バースト＝調停レイテンシ由来）が**非致命・実害ゼロ**。0 近傍を狙うなら今回スコープ外の **AXI QoS / DMA FIFO 調停**が必要（#59 受け入れ基準の「FE 残存でも実害ゼロ」を満たす）。

## 後続 issue との関係

```
#47 (本設計) ── 固める ──▶ #46 (producer 土台: DCMI continuous + DMA double-buffer)
                              ├─ #45 (解像度 / フォーマット: JPEG を frame_desc に)
                              ├─ #48 (LTDC + GUIX: ltdc_sink を差し込む)
                              └─ #49 (Ethernet + NetX Duo: eth_sink を差し込む)
```

本設計は **#46 の実装 plan の前提**。#46 のダブルバッファは「複数シンクに記述子を配る」前提で設計し、#45 の JPEG / フォーマット可変は `frame_desc` に織り込む。#44（画質設定）は独立に先行可能。LTDC / Ethernet のブリングアップ自体は別 issue だが、本設計はそれらの sink が後から差し込める形を保証する。
