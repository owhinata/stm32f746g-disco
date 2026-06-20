# メモリベンチマークコマンド（`membench`）

issue #57。`size` / `free`（[メモリ使用量](shell-free.md)）が**容量**を示すのに対し、`membench` は各メモリの
**速度**——シーケンシャル帯域（read/write/copy, MB/s）と**ポインタチェイス・レイテンシ曲線**（ns/access,
ワーキングセット掃引）——を Cortex-M7 の **DWT CYCCNT** でサイクル精度計測する。狙いは
**L1 D$(16KB) → SRAM → SDRAM のレイテンシ段差**を実機で可視化すること（#65 `.sdram` 配置最適化・#49 NetX
バッファ設計の判断材料）。`shell/cmds/cmd_membench.c` にあり、`coremark`/`sdram` と同じく **exe にのみ**
リンクされる。

lmbench は POSIX/プロセス/mmap/FS 依存の大規模スイートなので**移植しない**。その中核 2 アイデア
（`bw_mem` 帯域 / `lat_mem_rd` ポインタチェイス）だけを borrow した自前マイクロベンチ（~280 行）。

## コマンド

| コマンド | 登録 | 動作 |
|---|---|---|
| `membench [region]` | `CLI_CMD_REGISTER(membench, NULL, ..., cmd_membench, 1, 1)` | 帯域 + レイテンシ曲線を表示 |

`region` ∈ `{dtcm, sram, sdram, flash, all}`、省略時 `all`。`membench x` は未知リージョンでエラー。
全体で 1 秒未満。各計測点境界で `cli_cancel_requested()` を覗くので **Ctrl+C で中断**でき、`^C` 表示後
プロンプト復帰（協調キャンセル #16）。

```text
sh> membench
DWT CYCCNT @216MHz; warm-up + tick-guarded min; D$=16KB/32B line; SDRAM/DTCM non-cacheable.

bandwidth (MB/s)             read    write     copy
  DTCM   (16KB)               618      756      425
  SRAM   ( 4KB, cached)       550      751      421
  SRAM   (64KB, refill)       384      756      297
  SDRAM  (64KB, non-cache)     69      208       31
  Flash  (64KB, AXIM+L1D$)    276       --       --

latency (ns/access, dependent-load chain, 64B stride)
  WSS       DTCM    SRAM   SDRAM
  1KB       14.0    14.0   104.4
  2KB       14.0    14.0   103.6
  4KB       14.0    14.4    99.2
  8KB       14.0    36.1   104.5
  16KB      14.0    42.2   104.1
  32KB        --    44.6   104.5
  64KB        --    46.0   104.3
```

実測（216MHz, LTDC/カメラ idle）の一例。SRAM latency が **WSS≤4KB の ~14ns（L1 D$ ヒット）から 8KB 以降
~36→46ns へ段差**（D$ 溢れ→SRAM refill）、DTCM はフラット最速 ~14ns、SDRAM は非キャッシュで ~104ns
フラット——狙い通り D$→SRAM→SDRAM の階層が見える。帯域も DTCM > SRAM(cached) > SRAM(refill,read) > SDRAM。
SRAM write が cached/refill とも ~750 なのは write-back キャッシュが吸収する CPU 観測値（前述の caveat）。

## 計測機構

### クロック: DWT CYCCNT（サイクル精度）

DWT は通常未使用（ThreadX exec-profile は TIM2。DWT は WFI スリープでコアクロックと共に凍るため、
[#19](https://github.com/owhinata/stm32f746g-disco/issues/19) は TIM2 を採用）。`membench` 局所で有効化:

```c
CoreDebug->DEMCR |= TRCENA;                  /* trace enable             */
*(volatile uint32_t *)0xE0001FB0 = 0xC5ACCE55;  /* DWT_LAR unlock (M7)   */
if (DWT->CTRL & DWT_CTRL_NOCYCCNT_Msk) abort; /* CYCCNT 未実装判定        */
DWT->CYCCNT = 0; DWT->CTRL |= CYCCNTENA;
/* self-test: CYCCNT が進むまで最大3回 retry、進まなければ abort（ハング防止）*/
```

!!! warning "Cortex-M7 の DWT ソフトロック（debugger 非接続で CYCCNT が 0 に凍る罠）"
    M7 は **DWT にソフトロック**があり、TRCENA + CYCCNTENA を立てても **debugger 非接続だと CYCCNT が 0 のまま
    回らない**。`DWT->LAR`（0xE0001FB0）に鍵 `0xC5ACCE55` を書いて**アンロックすると計数開始**する。これを
    怠ると計測サイクルが 0 → 較正で reps が上限に張り付き **数分間ハング + 全値 0** になる。`membench` は
    アンロック後に **CYCCNT が進むことを自己テスト**し、進まなければ即 abort（`calibrate` も異常時 reps=1 に
    クランプ）してハングを防ぐ。

debugger 非接続でも TRCENA はソフトから設定可（RM0385 §40.13、CMSIS `core_cm7.h` の NOCYCCNT bit25 /
CYCCNTENA bit0 / DEMCR.TRCENA bit24）。**ベンチ中は core が sleep しない**ので DWT 凍結問題（WFI）は非該当。
クロック実値は `HAL_RCC_GetHCLKFreq()`（216MHz 固定値ではなく）。サンプリング境界に `__DSB();__ISB();`。

- `MB/s = bytes × hclk / (cycles × 1e6)`、`ns/access = cycles × 1e9 / (hclk × accesses)`（64-bit 中間）。

### プリエンプション除去: 短い run + tick-guard 棄却 + min

各計測点は較正 run で 1 アクセスあたりサイクルを測り、**1 run が ~0.3ms（< 1 SysTick 周期 = 1ms）になる
反復数**に固定する。各 run の前後で `HAL_GetTick()` を読み、**ミリ秒 tick が進んだ run（= SysTick ISR 混入）
は棄却**し、tick-clean な run の**最小値**を採る（最大 16 試行、clean 3 つで打ち切り）。`__disable_irq()` は
**張らない**（issue 制約: 長い IRQ-off 禁止、prio-5 IWDG petter ~1s が refresh を続けられるよう）。

!!! note "tick-guard が保証する範囲"
    tick-guard が除去できるのは **SysTick ISR の混入のみ**（`SysTick_Handler → HAL_IncTick`）。USART/DMA/DCMI
    完了 IRQ・camera producer wake・LTDC/FMC バスマスタ競合は tick 進行で検出できない。これらは min と
    **「計測中は `camera stream stop` / `lcd disable`」** で軽減する。「完全 uncontended を保証」するものではない。

### DCE / line 再利用対策

`-O2/-O3` がループを消さないよう: read は `const volatile uint32_t*` で各 load を強制し非 volatile
accumulator → 末尾 volatile sink、write は `volatile uint32_t*` ストア、いずれも unroll ×8。**latency は
依存ロード鎖** `p = *(void**)p`（各 load が前の結果に依存＝CPU の先行発行を抑制し line 再利用を避ける）。

## 帯域（bw_mem）— cache-aware ラベリング

| 行 | 意味 |
|---|---|
| **DTCM (16KB)** | tightly-coupled・非 D-cache（単サイクル級）の実メモリ帯域 |
| **SRAM (4KB, cached)** | D$(16KB) 内に常駐 ＝ **L1 D$ 速度** |
| **SRAM (64KB, refill)** | D$ 超。read は refill 律速で実 SRAM read に近い。**write/copy は CPU 観測ストリーム性能であり、純粋な外部 write-completion 帯域ではない**（write-back+write-allocate のため、計測終了時に D-cache 上の最後の dirty footprint は clean しない限り外部 SRAM へ未書戻し） |
| **SDRAM (64KB, non-cache)** | MPU Normal non-cacheable（`src/bsp.c`）＝実 SDRAM 帯域 |
| **Flash (64KB, AXIM+L1D$)** | read のみ。0x08000000 のデータ read は **AXIM 経由で L1 D-cache 対象**（ART アクセラレータは命令フェッチ経路で、このデータ read ではない。RM0385 §3.2/§3.4） |

`copy` は各バッファ前半→後半をコピーするので、**MB/s の分母は実コピーバイト数（= スパンの半分）**。

## レイテンシ曲線（lat_mem_rd）

**決定的ストライド置換**でチェイス鎖を構築: ワーキングセット W に対し node 数 `n = W/64`（stride 64B ≥ D$
ライン 32B、各 node が別ラインを叩く）、`slot[i] → slot[(i+1)%n]` の単一巡回（乱数不使用・再現可能）。
W を **1〜64KB** 掃引（リージョン毎にバッファ上限まで）:

- **DTCM**: W ≤ 16KB（バッファ 16KB）。非キャッシュ・ほぼフラット最速のベースライン。32/64KB は `--`。
- **SRAM**: W ≤ 64KB。D$ 対象＝**16KB 超で段差**（D$ 溢れ → SRAM refill）。
- **SDRAM**: W ≤ 64KB。非キャッシュ＝フラットだが SRAM より高 latency。

Flash latency は対象外（自己参照 const 鎖のリンク時生成が困難＝read 帯域のみ）。

## バッファ配置

| リージョン | バッファ | 配置 |
|---|---|---|
| DTCM | `dtcm_bench_buf[16KB]` | **`.dtcm_bench`(NOLOAD) 新設**（`ldscript/STM32F746NGHx_FLASH.ld`、`.log_noinit` の後、ASSERT で 64KB 内を保証） |
| SRAM | `sram_bench_buf[64KB]` | `.bss`（zero-init） |
| SDRAM | `sdram_bench_buf[64KB]` | 既存 `.sdram`(NOLOAD) |
| Flash | バッファ無し | Flash 先頭（`FLASH_BASE`）から read |

新規予約は DTCM 16KB / SRAM 64KB / SDRAM 64KB（全て診断専用、`free` の used に反映）。`.dtcm_bench` 追加が
**唯一のリンカ変更**。`sdram`/`all` は `sdram_is_up()` を確認し、FMC down なら SDRAM 計測を skip（`down` 表示）。

## 検証

- **ビルド**: `cmake --build build`。リンカ `Used Size` が DTCM +16KB（24608B）/ RAM +64KB / SDRAM +64KB。
- **実機**（`/dev/ttyACM0` @115200 8N1）:
  - `help`/Tab 補完に `membench`。`membench` / `membench sram` / `membench x`(usage err)。
  - 帯域: DTCM > SRAM(cached) ≳ SRAM(refill,read) > SDRAM の序列、Flash read 表示。
  - レイテンシ曲線: **SRAM が WSS≤16KB でフラット低 → 32/64KB で段差上昇**、DTCM フラット最速、SDRAM 高 latency。
  - Ctrl+C で中断、連続実行で値が安定（tick-guarded min）、watchdog リセット無し。
