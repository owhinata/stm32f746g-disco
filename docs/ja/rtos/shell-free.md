# メモリ使用量コマンド（`free`）

issue #58。ビルド時の `size` 出力の**実行時・動的版**として、4 つの物理メモリリージョン
（Flash / DTCM / SRAM / SDRAM）ごとに total / used / free を表示し、加えて newlib heap の
占有量を出す読み取り専用の診断コマンド `free` を追加する。`shell/cmds/cmd_free.c` にあり、
`thread`（[スレッド情報](shell-thread.md)）等と同じく **exe にのみ**リンクされる（ホストテストの
登録集合に影響しない）。状態を一切変えず、渡された `sh` のみを出力 API 経由で触るため、
複数インスタンス同時実行でも再入安全（§10）。

## コマンド

| コマンド | 登録 | 動作 |
|---|---|---|
| `free` | `CLI_CMD_REGISTER(free, NULL, ..., cmd_free, 1, 0)` | 4 リージョンの total/used/free + heap 占有量を表示 |

サブコマンドは持たない（リーフ 1 個）。`free <arg>` は引数過多（mandatory=1 / optional=0）で
usage エラーになる。

```text
sh> free
region start          total      used      free use%
Flash  0x08000000   1048576    295796    752780  28%  .isr/.text/.rodata/.data
DTCM   0x20000000     65536      8224     57312  12%  .log_noinit (reset-persistent)
SRAM   0x20010000    262144     82656    179488  31%  .data/.bss/.sram1_dma + heap
SDRAM  0xC0000000   8388608   4657024   3731584  55%  .sdram fixed/cam/eth/ai/model (banks0-3)

heap:  base 0x200242E0  arena 0  in-use 0  free-pool 0
stack: top  0x20050000  main-reserve 1024 B (MSP/ISR grow down into SRAM free)
```

| 列 | 意味 |
|---|---|
| region | リージョン名（Flash / DTCM / SRAM / SDRAM） |
| start | リージョン先頭アドレス（リンカ MEMORY の ORIGIN） |
| total | リージョン総量（LENGTH、バイト） |
| used | 静的使用量（下表の算出、heap 含む） |
| free | `total − used` |
| use% | `used / total` の百分率 |

## リージョン別の算出（リンカシンボル）

`ldscript/STM32F746NGHx_FLASH.ld` が PROVIDE 済のシンボルから各リージョンの used を求める。
シンボルの**アドレス**が値を持つ（`(uintptr_t)&sym`）。

| リージョン | total | used | 典拠 |
|---|---|---|---|
| **Flash** 1MB @0x08000000 | LENGTH | `LOADADDR(.data) + sizeof(.data) − ORIGIN(FLASH)` | `.data` のロードイメージは FLASH 配置の最後尾なので、これが全フットプリント（= `size` の text+data） |
| **DTCM** 64KB @0x20000000 | LENGTH | `_elog_noinit − _slog_noinit` | `.log_noinit`（リセット永続ログリング）が唯一の常駐 |
| **SRAM** 256KB @0x20010000 | LENGTH | `(heap break) − ORIGIN(RAM)` | 静的（`_end − ORIGIN` = `.data`+`.bss`+`.sram1_dma`）+ heap arena |
| **SDRAM** 8MB @0xC0000000 | LENGTH | バンク別 NOLOAD サブリージョンの**合算**（下記） | `.sdram.fixed`(bank0 LTDC/表示) + `.sdram.cam`(bank1 カメラ) + `.sdram.eth`(bank2 ETH DMA, #49) + `.sdram.ai`(bank3 下位 NN アリーナ, #81/#88) + `.sdram.ai.model`(bank3 上位 reloc スロット, #92) |

リージョンの ORIGIN/LENGTH はリンカ MEMORY ブロックを典拠にした**コンパイル時定数**として
`cmd_free.c` に持つ（リンカを単一の真実源とし、`free` を**リンカ無改変・読み取り専用**に保つ）。
これらは bsp.c の MPU 設定や SDRAM/QSPI ドライバでも同じ値をハードコードしており、リンカ編集なしには
変わらない。

!!! note "SDRAM は `_esdram − _ssdram` ではなくバンク合算"
    `.sdram` は FMC 内部バンク境界（2MB/1MB アライン）で `fixed`/`cam`/`eth`/`ai`/`ai.model` の
    サブリージョンに分かれ、各バンク末尾から次バンク先頭までに**アライメントの穴**が空く（#65/#81/#92）。
    `_esdram − _ssdram` は穴込みのスパン（本例では 7MB/87.5%）になり実使用を過大報告するため、`free` は
    各サブリージョン（`_e… − _s…`）を**個別に合算**して穴を計上しない。ビルドが使わないサブリージョンは
    空（start == end）で 0 加算になる（例: reloc NN backend 以外では `.sdram.ai.model` = 0）。

### SRAM のレイアウト（heap と stack の共有）

SRAM リージョン（0x20010000..0x20050000）は下から `.data` → `.bss` → `.sram1_dma` と積まれ、その上端が
`_end`（= heap base）。**heap は `_end` から上へ**、**main/ISR スタックは `_estack`（0x20050000）から
下へ**伸び、両者は同じ未使用域を共有する。よって:

- `used` = `(heap break) − ORIGIN(RAM)`（静的 + heap arena）
- `free` = `_estack − (heap break)`（heap 成長とスタック降下が共有する余地）

末尾の `stack:` 行は `_estack`（初期 MSP）と `_Min_Stack_Size`（リンク時に保証されるメインスタック予約）を示す。

## heap 占有量（newlib `mallinfo`）

heap 行は newlib の `mallinfo()` から取得する:

- **base** = `_end`（heap の最下位アドレス）
- **arena** = システム（sbrk）から獲得した総バイト数 = 現在の break − heap base
- **in-use** = `uordblks`（プログラムが現在使用中）
- **free-pool** = `fordblks`（free リストに残る再利用可能バイト）

!!! note "なぜ `sbrk(0)` ではなく `mallinfo()` か"
    ツールチェーン同梱の stock `_sbrk()`（libnosys）は要求 break を**現在のスタックポインタ**と
    比較するが、ThreadX のスレッドコンテキストでは SP はスレッドの PSP（`.bss` 内＝heap より下位）で
    あるため、`sbrk(0)` はスレッドから誤った値/失敗を返しうる。`mallinfo()` は malloc 自身の
    accounting を読み `_sbrk` を経由しないため安全。malloc 未使用時は arena=0 で heap 行は全て 0。

!!! note "best-effort"
    free リストの走査は他スレッドの同時 malloc/free に対してロックしていない。`thread` の統計同様
    best-effort の診断であり、本 FW は heap をほとんど使わない（ThreadX のスレッドスタックは `.bss`、
    動的確保は byte pool が担う）。スレッド単位のスタック高水位は [`thread`](shell-thread.md) が担当し、
    `free` はリージョン俯瞰に限定する（重複させない）。

## 検証

- **ビルド**: `cmake --build build`（`shell` が通る／既存 demo 非破壊）。リンカの `Used Size`
  （DTCM 8224B / FLASH 295796B 等）と `free` の表示が整合すること。
- **実機**（`/dev/ttyACM0` @115200 8N1）:
  - `help` に `free` が並ぶ。Tab 補完で `fr` → `free`。
  - `free` で 4 リージョンの total/used/free + heap 行が表示され、ビルド時 `size` と整合。
  - `free x` は usage/arg エラー。
