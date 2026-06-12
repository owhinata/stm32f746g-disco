# microSD（SDMMC1 + DMA）

オンボードの microSD スロット（CN3）を STM32F746 の **SDMMC1** ペリフェラルで駆動する。ドライバは `port/sd/sd_card.{c,h}`、デバッグ用シェルコマンドは `sd`（`shell/cmds/cmd_sd.c`）。

microSD ファイルシステム基盤（Epic #32）の **Phase A**（#33）。Phase A は **読出し専用**（PC でフォーマットしたカードに対し非破壊）。FileX による FAT32 ファイルシステムと書込みは Phase B/C。

QSPI NOR が indirect モードで **DMA も D-Cache 整合も不要**だったのに対し、SD は DMA 転送を使うため、本ドライバは **DMA 完了の ISR 同期**と **D-Cache 整合**を正面から扱う。

## 配線（UM1907 CN3）

| 信号 | ピン | AF |
|------|------|----|
| SDMMC1_D0 | PC8 | AF12 |
| SDMMC1_D1 | PC9 | AF12 |
| SDMMC1_D2 | PC10 | AF12 |
| SDMMC1_D3 | PC11 | AF12 |
| SDMMC1_CK | PC12 | AF12 |
| SDMMC1_CMD | PD2 | AF12 |
| カード検出 | PC13 | 入力 pull-up、**active-low**（Low = 挿入） |

データ/CMD 線は very-high speed・pull-up。既存ペリフェラル（USART1=PA9/PB7、LED=PI1、TIM2、QSPI=PB2/PB6/PD11-13/PE2）と競合しない。

## クロック

| 項目 | 値 | 根拠 |
|------|----|------|
| クロック源 | **CLK48 = 48 MHz**（PLLQ、`bsp.c` で設定済） | RCC DCKCFGR2 SDMMC1SEL（既定）。`sd_card_init` が `__HAL_RCC_SDMMC1_CONFIG(RCC_SDMMC1CLKSOURCE_CLK48)` で明示 |
| 転送クロック | `SDMMC_CK = 48 MHz / (ClockDiv + 2) = 24 MHz`（ClockDiv=0） | RM0385 §35.8.2 |
| 識別フェーズ | < 400 kHz（HAL 内部 `SDMMC_INIT_CLK_DIV`） | カード規格 |
| バス幅 | **4-bit**（失敗時 1-bit にフォールバック） | `HAL_SD_ConfigWideBusOperation` |

!!! note "転送クロックの確定"
    `HAL_SD_Init` はカード識別後も `SDMMC_CK` を 400 kHz の識別用分周のままにする（`ClockDiv=0` を自動適用しない）。24 MHz / 4-bit を効かせるには `HAL_SD_ConfigWideBusOperation` が内部で `SDMMC_Init` を再実行する必要があるため、**4-bit / 1-bit いずれの経路でも必ず呼ぶ**。コンパイル時 `-DSD_BUS_WIDE_1B` で常時 1-bit を選べる（切り分け用 escape hatch）。

## DMA（DMA2）

| 方向 | ストリーム / チャネル | 設定 |
|------|----------------------|------|
| 受信（card→mem） | **DMA2 Stream3 / Ch4** | PERIPH→MEMORY |
| 送信（mem→card） | **DMA2 Stream6 / Ch4** | MEMORY→PERIPH |

共通設定: SDMMC をフロー制御（**`DMA_PFCTRL`**）、32-bit ワード、**INC4 バースト**、FIFO full しきい値、優先度 very-high（RM0385 §35.3.2 / Table 26）。本ファームウェアで **初めて DMA を使うサブシステム**。

## DMA 完了の同期

HAL_SD は DMA でデータ相を回し、完了を**非同期に**通知する:

- **Read**: DMA Rx の転送完了（`DMA2_Stream3_IRQHandler` → `SD_DMAReceiveCplt` → `HAL_SD_RxCpltCallback`）
- **Write**: DMA Tx 完了（`DMA2_Stream6_IRQHandler`）が DATAEND 割込みを有効化 → カードのプログラミング完了で **`SDMMC1_IRQHandler`** → `HAL_SD_TxCpltCallback`

よって **SDMMC1 と DMA2 Stream3/6 の 3 つの IRQ をすべて有効化**する。各 ISR は USART1 と同型に execution-profile（#19）の enter/exit を PRIMASK で挟む。NVIC 優先度は SDMMC1=6・DMA2_Stream3/6=7（USART1=5 より下、SysTick=14 より上）。ThreadX M7 port は PRIMASK ベースなので、ISR からの `tx_semaphore_put` は優先度に依らず安全。

完了通知は **count 0 の `TX_SEMAPHORE`** で行い、呼出しスレッドは**有限タイムアウト**で待つ（ビジーウェイトしない）。古い/遅延 callback が誤って成功を返さないよう:

- 各操作の発行直前に semaphore を **drain** し、`sd_xfer_active=1` を立てる
- callback は `sd_xfer_active` が立っている時だけ semaphore を put
- タイムアウト / エラー時は **先に `sd_xfer_active=0`** → `HAL_SD_Abort`（SDMMC 割込み/フラグの解除と DMA 中断を同期実行）→ 再 drain
- write は全チャンク後に再度 TRANSFER 状態を待ち、データの確定を保証

## D-Cache 整合（bounce buffer 方式）

DMA は常に **`sd_bounce`**（`8*512 = 4 KiB`、**32 B 整列**、リンカ `.sram1_dma` セクションで **SRAM1** に配置）にのみ発行する。呼出し側のバッファは CPU の memcpy だけが触るので、任意のアライメントで安全。

- **Read**: DMA 前に `SCB_InvalidateDCache_by_Addr(sd_bounce)`（dirty line の DMA 中 evict を防ぐ）→ DMA → 完了後に再度 invalidate（投機 prefetch を破棄）→ `memcpy` で呼出しバッファへ
- **Write**: `memcpy` で `sd_bounce` へ → `SCB_CleanDCache_by_Addr(sd_bounce)`（物理 SRAM へ flush）→ DMA

`sd_bounce` は 32 B 整列、チャンク長は 512 の倍数（= 32 B の倍数）なので、clean/invalidate は `sd_bounce` の cache line のみに作用し**隣接バッファを壊さない**。

!!! note "なぜ DTCM ではなく SRAM1 か"
    DMA2 は M7 の AHBS スレーブ経由で DTCM にも到達できる（RM0385 §2.1.1/§2.1.6）。SRAM1 を選ぶのは「DTCM に届かないから」ではなく、**D-Cache 管理を 32 B 整列の専用バッファ 1 箇所に閉じ込める**標準的な方式を採るため。リンカは `.sram1_dma` の始端 ≥ `0x20010000`・終端 ≤ `0x2004C000` を ASSERT で担保する（DTCM/SRAM1 領域分割は #33 で導入）。

## 排他とスレッド文脈

- 公開 API は **転送操作単位**（状態待ち → DMA → 完了 → cache）を内部 ThreadX mutex（TX_INHERIT）で直列化。シェル fg/bg から並行に呼んで安全
- **thread context 専用**。ISR から呼んではならない
- 初期化は `tx_application_define()`（`src/main.c`、`fs_glue_init()` の直後）で 1 回。カード I/O は発行しない（GPIO/DMA/NVIC/クロック源 mux と mutex/semaphore 生成のみ）。カードの認識（`HAL_SD_Init`）は最初の `sd` コマンドで遅延実行

## `sd` シェルコマンド

低レベル（カード）コマンド:

```
sd info        カード種別 / 容量 / ブロックジオメトリ / バス幅 / CID / CSD
sd read <lba>  1 ブロック（512 B）の hexdump（LBA アドレス）
```

ファイルシステム（FileX FAT、#34）コマンド — `fs`(QSPI) と共通コアを共有:

```
sd ls [path]        ディレクトリ一覧（既定 /）
sd cat <path>       ファイル表示
sd write <p> <txt>  ファイル作成/上書き（空白はクォート）
sd rm <path>        ファイル / 空ディレクトリ削除
sd mkdir <path>     ディレクトリ作成
sd df               FS 容量 / 空き / FAT 種別（64-bit）
sd umount           flush + アンマウント
```

- FS は最初の `sd` FS コマンドで **lazy mount**（再フォーマットしない）。PC で作った FAT32 をそのまま読み書きでき、相互運用できる。詳細・アーキテクチャは [ファイルシステム](../rtos/filesystem.md)。
- **`sd info`/`sd read` は raw gate**: カードを再識別しうるため、FS マウント中は拒否される（`sd umount` が退避経路）。未マウント時は従来どおり（必要時のみ probe）。
- MBR(PC 標準) と superfloppy(VBR@0) の両方をマウントできる（ドライバが LBA0 を判定）。
- `sd format`（FAT32）は Phase C。

## 動作確認

PC で FAT32 等にフォーマットした microSD（**非破壊**）を挿入:

```
sh> sd info
type      : SDHC/SDXC (v2.x)
capacity  : 29820 MiB (61071360 blocks x 512 B)
bus width : 4-bit
rca       : 0xaaaa
...
sh> sd read 0          # MBR / boot sector
00000000  eb 3c 90 ...
000001f0  ... 55 aa    # offset +510 に 55 aa
```

失敗時の切り分け:

- `no card in slot` → カード検出ピン（PC13）/ 挿入
- `sd info` は出るが `55 aa` が化ける → read の invalidate 順序
- コマンドがハングする → DMA 完了 signal の未配線 / NVIC / `__HAL_LINKDMA`
- 即エラー → クロック源 mux / 4-bit 昇格失敗（`-DSD_BUS_WIDE_1B` で切り分け）
