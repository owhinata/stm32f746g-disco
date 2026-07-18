# SDRAM（FMC、8MB 外部 RAM）

ボード搭載の 128-Mbit SDRAM（Micron **MT48LC4M32B2**、4M×32）を STM32F746 の **FMC SDRAM コントローラ**で駆動する。ドライバは `port/sdram/sdram.{c,h}`、シェルコマンドは `sdram`（`shell/cmds/cmd_sdram.c`）。

カメラ基盤（Epic #22）の **Phase 1.5**（#40）。主目的は**大容量 DMA ターゲットバッファ**の置き場所（カメラフレームバッファ、#41）。

## 構成

| 項目 | 値 | 根拠 |
|------|----|------|
| デバイス | MT48LC4M32B2（128 Mbit、4 banks × 4096 rows × 256 cols × 32 bit） | UM1907 §6.13 |
| バス幅 | **16-bit**（DQ16-31 は基板で 10kΩ pull-down、未接続） | UM1907 §6.13 |
| 実効容量 | **8 MB** @ **0xC0000000**（FMC bank1） | 4M ワード × 2 B |
| SDCLK | **HCLK/2 = 108 MHz**（`FMC_SDRAM_CLOCK_PERIOD_2`） | HCLK=216 MHz |
| CAS レイテンシ | **3**（コントローラ・モードレジスタとも） | datasheet 定格（下記 note） |
| リフレッシュ | COUNT=0x0603（BSP 値、100 MHz 用計算） | 108 MHz では ~14.4 µs/row と**早め**のリフレッシュ＝安全側 |
| タイミング | TMRD=2 / **TXSR=8** / **TRAS=5** / TRC=7 / TWR=2 / TRP=2 / TRCD=2（SDCLK サイクル） | datasheet 最小値を 9.26 ns 周期で満たすよう導出 |

!!! warning "BSP の CAS2 をそのまま使えない理由（codex-review 指摘）"
    ST BSP（`stm32746g_discovery_sdram.c`）は CAS2 だが、それは **SDCLK=100 MHz（HCLK 200 MHz）前提**。本ファームウェアは HCLK=216 MHz → SDCLK=108 MHz であり、MT48LC4M32B2-6 の **CL2 定格は tCK(2)≥10 ns（=100 MHz 上限）**で違反する。CL3 は tCK(3)≥6 ns（167 MHz まで）で定格内。TXSR/TRAS も 10 ns 前提の BSP 値（7/4 サイクル）では 9.26 ns 周期で datasheet 最小（67 ns / 42 ns）を割るため各 +1 サイクル。読出しサンプリング余裕はむしろ改善（tAC(3)≤5.4 ns vs 周期 9.26 ns）するので RBURST/RPIPE_0 は BSP のまま。

ピン（全 AF12、pull-up）: PC3(SDCKE0) / PD0,1,8-10,14,15(D2,D3,D13-15,D0,D1) / PE0,1,7-15(NBL0/1, D4-D12) / PF0-5,11-15(A0-A5, SDNRAS, A6-A9) / PG0,1,4,5,8,15(A10,A11,BA0,BA1,SDCLK,SDNCAS) / PH3(SDNE0), PH5(SDNWE)。既存ペリフェラル（USART1 / QSPI / SDMMC1 / DCMI / I2C1 / LED）と競合しない。

## MPU: キャッシュ属性はマスタで決める（DMA コヒーレンシ設計）

ARMv7-M のデフォルトメモリマップでは 0xC0000000 は **Device 属性（XN）**。`bsp_init()`（`src/bsp.c` の `mpu_config_sdram()`）が **D-cache 有効化より前に** 3 つの MPU リージョンを設定する。**キャッシュ可否は「その領域を触るマスタ」で決める** — DMA が書く領域は非キャッシュ（コヒーレント）、NN 演算の CPU 専用領域だけキャッシュ可。番号が大きい region が重なりで優先される（PMSAv7）。

**region 0（全域の土台、8MB non-cacheable）** — 8MB を **Normal・non-cacheable・RW・XN** に再マップ。bank0/1/2 はこれで動く:

- **Normal**: Device 属性のワード毎直列化を避け、通常データアクセスとして扱える
- **non-cacheable**（TEX=1, C=0, B=0）: DCMI / LTDC / DMA2D / ETH-MAC の DMA 書込 → CPU 読出が主用途。キャッシュラインが一切立たないため **DMA/CPU コヒーレンシ問題が構造的に存在しない**（clean/invalidate 不要、dirty eviction レースなし）。代償は CPU アクセスの低速化
- **PRIVDEFENA**: 他アドレスはデフォルトマップ維持（flash/SRAM/ペリフェラル挙動は不変）

**region 1（bank3 を cacheable へ上書き、#81）** — NN 推論アリーナ `.sdram.ai`（bank3、0xC0600000、2MB）だけを **Normal・cacheable WBWA（TEX=1, C=1, B=1）・XN** に上書きする。この領域は **CPU 専用（DMA が入らない）** ため cacheable でもコヒーレンシは崩れず、非キャッシュ比 **~20× 高速**（活性化バッファへの反復アクセスが D-cache に乗る）。DMA を bank3 に入れると保守が必要になり破綻するので、ETH/DCMI は非キャッシュ側（bank2/1）に置く。

**region 2（reloc モデル XIP 窓、#92）** — bank3 上位 1MB（0xC0700000）を cacheable かつ **命令フェッチ許可**にする窓。SD からロードした relocatable NN モデルを XIP するため。SDRAM は既定で XN なので、コード実行はこの窓に限定される。**既定は `HAL_MPU_DisableRegion`**（`stedgeai_reloc` backend 選択時のみ有効化）。

!!! note "SD bounce buffer（SRAM1）との方式の違い"
    SDMMC ドライバは**キャッシュされる SRAM1** の専用 32B 整列 bounce buffer + clean/invalidate で整合を取る（[microSD](sdmmc.md) 参照）。SDRAM 側は**領域ごと non-cacheable** にして maintenance 自体を消す。前者は小さく頻繁な転送＋キャッシュ恩恵が活きる用途、後者は大きな DMA ターゲット向き。

## リンカ `.sdram` セクション（NOLOAD 制約）

`MEMORY` に `SDRAM 0xC0000000 8M` を追加し、`.sdram (NOLOAD)` セクションを定義。**FMC は `sdram_init()` が走るまで未初期化**のため、startup（`.data` コピー / `.bss` ゼロ化）がこの領域に触れてはならない — `.sdram` 配置オブジェクトは**ロードイメージなし・ゼロ初期化なし・リセット非生存**。

```c
static uint16_t cam_view_buf[320*240] __attribute__((aligned(32), section(".sdram.fixed")));
```

## FMC 内部バンク配置（#65）

SDRAM は **4 つの内部バンク**（各 2 MB、CPU アドレスの **offset bit[22:21]** がバンク選択、RM0385 §13.5.3）を持ち、各バンクは独立に 1 本の **open row** を保持できる。`.sdram` を **4 つのバンク整列サブ領域**に分割し、リンカで配置を固定する（キャッシュ属性は上記 MPU region0/1 による）:

| サブ領域 | バンク | アドレス | キャッシュ | 内容 |
|---|---|---|---|---|
| `.sdram.fixed`（先頭 `.sdram.fixed.ltdc`） | **bank0** | 0xC0000000–0xC01FFFFF | 非キャッシュ | 固定常駐: **`ltdc_fb`（LTDC スキャンアウト READ 面、先頭に address-pin）** / `cam_frame`（snapshot）/ `cam_view_buf` / `sdram_bench_buf` 等。計 ~1.45 MB（< 2 MB） |
| `.sdram.cam` | **bank1** | 0xC0200000–0xC03FFFFF | 非キャッシュ | `cam_arena`（**2 MB のカメラ DMA リングアリーナ**、DCMI WRITE 先） |
| `.sdram.eth`（#49） | **bank2** | 0xC0400000–0xC05FFFFF | 非キャッシュ | ETH MAC-DMA descriptors + RX/TX パケットバッファ（NetX Duo）/ `mjpeg_buf`（HTTP MJPEG 送信用 private JPEG copy、256 KB） |
| `.sdram.ai` / `.sdram.ai.model`（#81/#92） | **bank3** | 0xC0600000–0xC07FFFFF | **キャッシュ WBWA** | NN activations / staging / RT-RAM（~320 KB、**CPU-only**）。上位 1 MB @0xC0700000 は reloc NN モデルの XIP 窓（**exec 許可**、既定 DISABLE） |

- **狙い（FE 削減レバー）**: LTDC の READ 面（`ltdc_fb`, bank0）と DCMI リングの WRITE 先（`cam_arena`, bank1）を**別バンク**に置くと、両バンクが各自の row を open したまま維持でき、同一バンク内の row activate/precharge スラッシングを避けられる → DMA FIFO error (FE) の低減を狙う。**物理 SDRAM チップは 1 個**なので総帯域は 4 バンクで共有される（分離が効くのは row-open の並走のみ）。リンカ `ASSERT` で「fixed が bank0 内（< 2 MB）/ cam が bank1 先頭・正確に 2 MB / eth が bank2 内 / ai が bank3・全体 ≤ 8 MB」を強制。
- **効果は実測依存**（後述）。効果が無くても配置自体は同一 MPU リージョン・アドレス差のみで害はない。
- **カメラリングは動的パーティション**: 旧 `cam_ring[4][256KB]`（固定 1 MB）を廃し、ストリーム開始時に現モードの `frame_bytes` から slot stride = `align32(frame_bytes)`、スロット数 = `min(2MB/stride, 8)` を実行時算出（< 2 スロットは reject）。小モードほど深いリングになる。`camera stream stats` の `ring: N slots x M B` で確認可能。
- `cam_frame`（snapshot）は bank0 で `ltdc_fb` と同居するが、snapshot は単発で FE 許容（#45 で FE/DME 割込み無効化済）。FE クリティカルな streaming 経路（bank1 vs bank0）は分離済み。

**実測（FE 削減、LTDC scanout ON・同条件、`camera stream stats` の `dma fe/s`）**:

| モード | 旧レイアウト（同一バンク混在） | #65（bank0 LTDC / bank1 ring） | 削減 |
|---|---|---|---|
| QVGA RGB565 | 1193.3 fe/s | **859.3 fe/s** | **−28%** |
| 480x272 RGB565 *(モードは #84 で削除)* | 2022.0 fe/s | **1489.5 fe/s** | **−26%** |

いずれも `ovr dcmi`/`ovr ring` = 0、fps 14.8 不変。バンク分離で **FE が ~26–28% 減**（不確実とされた副次効果が実機で確認できた）。動的アリーナにより両モードとも `ring: 8 slots`（旧固定 4 slots より深い）。

## バンク別データフロー

SDRAM を通るデータが、どのバンク・キャッシュ属性・マスタを経由するか（`非$` = 非キャッシュ）:

```
① camera capture (単発, base OFF)
   OV5640 → DCMI(DMA2 St1) → cam_frame(bank0,非$) → CPU read → save/send/stats

② camera streaming (base + subscriber cascade, Epic #99)
   OV5640 → DCMI(DMA2 St1) → cam_arena ring(bank1,非$) → frame_pipeline publish
     ├ GUI preview : DMA2D M2M  slot(bank1)→cam_view_buf/ltdc_fb(bank0,非$) → LTDC scanout
     ├ MJPEG       : CPU memcpy slot(bank1)→mjpeg_buf(bank2,非$) → ETH-MAC DMA → HTTP
     ├ NN(nncam)   : CPU 前処理 slot(bank1)→NN 入力/activations(bank3,$) → 推論 → dets
     └ save/send   : pin latest slot(bank1)→ CPU memcpy → cam_frame(bank0,非$) → SD/QSPI/YMODEM

③ LTDC 表示   : GUIX+DMA2D compose → ltdc_fb[2](bank0,非$) → LTDC-DMA scanout (VBR tear-free toggle)
④ Ethernet    : NetX Duo ⇄ ETH desc+pkt buf(bank2,非$) ⇄ ETH-MAC DMA (LAN8742 RMII)
⑤ NN 推論     : activations/staging(bank3,$) + reloc model .bin(bank3 上位, XIP exec) → CPU 推論
```

- **DMA 対象（bank0/1/2）は非キャッシュ**なので、DCMI / LTDC / DMA2D / ETH-MAC のいずれも cache 保守なしでコヒーレント。**NN(bank3)は CPU-only** なので cacheable（保守不要かつ高速）。
- **`camera save/send` が streaming 中に遅い**（#99/#102）のは②の分岐で、`cam_arena`(bank1) + `cam_frame`(bank0) + SD DMA + DCMI が単一 FMC を round-robin で共有するため。ライブ映像は全 fps 継続する（pin 方式が producer を止めない）が、低優先度の CLI save は後回しになる。速い保存は `camera stream stop` 後に。
- **NN 推論中の DCMI overrun**（#90）も同じ帯域競合で、bank3 の連続 SDRAM トラフィックが bank1 の DCMI DMA と物理 SDRAM を奪い合うことで起きる（F746 の bus matrix は round-robin で per-master QoS を持たない、RM0385 §2）。

## 初期化フロー

`sdram_init()`（`tx_application_define` から、`camera_init()` より前）:

1. GPIO（AF12）+ FMC クロック
2. `HAL_SDRAM_Init`（16-bit / CAS3 / SDCLK=HCLK/2、上記タイミング）
3. JEDEC パワーアップシーケンス: CLK enable → **100 µs 待ち**（`udelay`、ThreadX tick 未稼働のため TIM2 busy-wait）→ PALL → auto-refresh ×8 → mode register（BL=1 / sequential / CAS3 / single write）→ リフレッシュカウンタ

ポーリングのみ（割込み・DMA・ThreadX オブジェクト不使用）。idempotent・fail-soft（失敗時は `sdram`/`camera capture` が報告、他は継続）。

## `sdram` シェルコマンド

```
sdram info           ウィンドウ / 設定 / 状態
sdram test [bytes]   破壊的 write/read-back memtest（デフォルト全 8MB、4 の倍数）
```

`devmem` の許可リージョンにも SDRAM 窓（0xC0000000、8MB）を追加済みで、`devmem peek/poke/dump` で直接アクセスできる（8MB 窓の外は従来どおり拒否）。

`sdram test` は 3 パス: ①**アドレスパターン**（各ワードに自身のアドレス — アドレス線の固着/短絡/エイリアスを検出。単一値の繰り返しでは見えない）②0x55555555 ③0xAAAAAAAA（データ線の固着/結合を両極性で検出）。各パスは**全域 write → 全域 read-back** の順で行い、リフレッシュ動作（数十 ms のギャップ）も実効的に検証する。**破壊的**（`.sdram` 上のオブジェクト、例: キャプチャ済みフレームを上書き）。Ctrl+C で 64KB チャンク境界キャンセル可。

**破壊と整合する suspend/invalidate 契約（#65、#47 下流）**: test は `ltdc_fb`（bank0）も含む `.sdram` 全域を上書きするので、以下を行う — (1) camera streaming 中・GUIX 所有中は**拒否**、(2) LTDC scanout が動作中なら test 中**一時停止**（`ltdc_set_scanout(false)`、コントローラが破壊済み FB をフェッチして画面破損するのを防ぐ）、(3) snapshot フレームを `camera_frame_invalidate()`、(4) test 後（cancel/FAIL 含む単一 restore 経路）に **両 FB を黒クリア → scanout を元の状態へ復帰**（残骸表示なし）。元々 `lcd off` 済なら触らない。

実行例:

```
sh> sdram test
sdram: DESTRUCTIVE test over 8192 KB (clobbers .sdram contents, e.g. a captured frame)
pass 1/3 (address): write+verify 8192 KB ... OK
pass 2/3 (0x55555555): write+verify 8192 KB ... OK
pass 3/3 (0xAAAAAAAA): write+verify 8192 KB ... OK
sdram: 8192 KB tested, no errors
```

## 参照

- UM1907 §6.13 — SDRAM（16-bit 接続、64-Mbit アクセス可）
- RM0385 §13 — FMC SDRAM コントローラ
- MT48LC4M32B2 datasheet — タイミング定格・モードレジスタ
- `_ref/STM32Cube_FW_F7_V1.17.0/Drivers/BSP/STM32746G-Discovery/stm32746g_discovery_sdram.c` — タイミング/シーケンスの参照実装（read-only）
