# メモリアクセスコマンド（`devmem`）

M4「組込みコマンド」の Issue #14。`version`/`uptime`/`reboot`
（[システム組込み](shell-builtins.md)）と `thread`（[スレッド情報](shell-thread.md)）に続き、
メモリを直接 read/write する開発・デバッグ用コマンド `devmem` を追加する。`shell/cmds/cmd_devmem.c`
にあり、`cmd_system.c` 同様 **exe にのみ**リンクされる（ホストテスト基盤には載らない）。

`devmem` は**危険コマンド**（要件 §12）。`CLI_ENABLE_DANGEROUS_CMDS` が有効なときだけファイル全体が
コンパイルされる。OFF にするとハンドラと登録ごと消え、`devmem` は `.shell_root_cmds` から外れて
`help` と Tab 補完からも消える（`reboot` と同じゲート）。

## サブコマンド

| コマンド | 引数（必須 / 任意） | 動作 |
|---|---|---|
| `devmem peek <addr> [8\|16\|32]` | 2 / 1 | 8/16/32-bit を 1 ワード read（既定 32-bit） |
| `devmem poke <addr> <val> [8\|16\|32]` | 3 / 1 | 1 ワード write 後にリードバック表示 |
| `devmem dump <addr> [len]` | 2 / 1 | `[addr, addr+len)` を canonical hex+ASCII ダンプ（既定 64 byte） |

`devmem` は純粋な親（handler `NULL`）で、`devmem` 単体は *missing or unknown subcommand* となる。
Tab 補完はコマンド木を走査する（Issue #11）ため `peek`/`poke`/`dump` が自動で出る。各ハンドラは
渡された `sh` のみをバッファ出力 API 経由で触るので、複数インスタンス同時実行でも再入安全（§10）。

```text
sh> devmem peek 0x20000000
0x20000000: 0xdeadbeef
sh> devmem peek 0x08000000 16
0x08000000: 0x2000
sh> devmem poke 0x20000000 0x1234
0x20000000: 0x00001234
sh> devmem dump 0x08000000 32
08000000  00 04 02 20 c5 01 00 08  d1 01 00 08 d3 01 00 08  ... ............
08000010  d5 01 00 08 d7 01 00 08  d9 01 00 08 00 00 00 00  ................
```

`peek`/`poke` は `0x<addr>: 0x<value>` を表示し、値はアクセス幅に合わせてゼロ詰めする。`poke` は
write 後に必ずリードバックするので、表示値はバス上の実値（後述の read-clear に注意）。`dump` は
`cli_hexdump_base()`（[出力 API](shell-output.md)）でオフセット列に**絶対アドレス**を表示する。

## 数値・幅・アライメント

- **アドレス / 値 / 長さ**は `0x`/`0X` 16 進または 10 進を受理。パースは厳格で、不正な桁・末尾ゴミ・
  32-bit 桁あふれはエラー。
- **幅**は `8` / `16` / `32`（bit）。省略時は 32。`poke` は幅に収まらない値も拒否する
  （例 `poke … 0x1ff 8`）。
- **アライメント**は幅に従う。16-bit は 2-byte 境界、32-bit は 4-byte 境界が必要（8-bit は制約なし）。
  非アライメントアクセスは拒否。

## アドレス範囲ゲート（リージョン許可リスト）

単一の `[min,max]` ではなく、コンパイル時の**リージョン許可リスト**（`cmd_devmem.c` の
`devmem_map[]`）でアクセスを検査する。アクセスは 1 つのリージョンに**完全に収まり**、方向（read/write）が
許可され、そのリージョンが許可する幅であること。いずれか外れれば**実行せずエラーで拒否**する。

理由は F746 のメモリマップ（RM0385 §2.2.2）が多数の Reserved hole を持つため。hole への read/write は
CMSIS の weak `Default_Handler` にフォルトし、その無限ループで**シェルがハング**しリセットが要る。
ゲートはタイプミスを hole から守り、通常の CLI エラーに変える。

既定テーブル:

| region | 範囲 | read | write | 幅 |
|---|---|---|---|---|
| ITCM-RAM | `0x00000000`–`0x00003FFF`（16 KB） | ✓ | ✓ | 8/16/32 |
| Flash (AXIM) | `0x08000000`–`0x080FFFFF`（1 MB） | ✓ | ✗ | 8/16/32 |
| DTCM | `0x20000000`–`0x2000FFFF`（64 KB） | ✓ | ✓ | 8/16/32 |
| SRAM1+2 | `0x20010000`–`0x2004FFFF`（256 KB） | ✓ | ✓ | 8/16/32 |
| APB1 | `0x40000000`–`0x40007FFF` | ✓ | ✓ | 32 のみ |
| APB2 | `0x40010000`–`0x40016BFF` | ✓ | ✓ | 32 のみ |
| AHB1 | `0x40020000`–`0x4007FFFF` | ✓ | ✓ | 32 のみ |
| AHB2 | `0x50000000`–`0x50060BFF` | ✓ | ✓ | 32 のみ |
| PPB | `0xE0000000`–`0xE00FFFFF`（SCB/NVIC/SysTick） | ✓ | ✓ | 32 のみ |

- 既定で実在 RAM/Flash + on-chip peripheral バス窓 + PPB を許可。**ブロックは Reserved hole と
  Flash write のみ**。Flash への plain store は Flash programming 手順ではないので read-only。
- Flash は **AXIM alias**（`0x08000000`）のみ。ITCM Flash alias（`0x00200000`）は表に含めない。
- peripheral バスと PPB は **word（32-bit）アクセスのみ** — sub-word で fault/誤動作するレジスタが多い。
  `dump` はバイト粒度なので RAM/Flash 限定。peripheral レジスタは `peek <addr> 32` で読む。
- 外部メモリ（FMC SDRAM `0xC0000000` / QSPI）は表に**含めない**。シェルが FMC/QSPI 未初期化のため
  アクセスは fault する。

許可リージョンを変えるには `devmem_map[]`（コンパイル時定数）を編集する。本番では危険コマンドゲートで
`devmem` ごと除去するのが筋。

!!! warning "ゲートはバス窓粒度（レジスタ粒度ではない）"
    peripheral/PPB のリージョンはバス窓全体を覆うため、その中には未実装/Reserved の
    レジスタオフセットも含まれる。そこへのアクセス（例: APB1 窓内の未使用オフセット）は
    ゲートを通過するが、依然 fault してシェルがハングし得る。ゲートが防ぐのは**窓と窓の間の
    Reserved hole** への典型的なタイプミスであり、窓内の全オフセットを検証するわけではない。

!!! warning "peripheral / PPB の write はシステム全体に副作用"
    `read-clear` レジスタは読むだけで状態が変わる（`poke` のリードバックが書込値と異なる/`peek` も
    無副作用ではない）。NVIC/SCB/RCC/GPIO/SysTick への `poke` は割込み・クロック・ピン状態を変え、
    システム全体を不安定にし得る。デバッグ利便のため既定許可だが、意図して使うこと。

## `dump` 長 cap

`cli_hexdump` は実行中インスタンスの出力ロックを保持し続けるため、無制限の `dump` は他インスタンスの
出力を長時間塞ぐ（§10）。`CLI_DEVMEM_DUMP_MAX_LEN`（既定 **256** byte、同名の CMake cache 変数で
override 可）を超える要求は拒否する。長さ 0 は no-op。

!!! note "D-cache と stale データ"
    D-cache は有効（`src/bsp.c`）。`dump`/`peek` は cache maintenance なしで読むため、DMA や他バス
    マスタが更新したメモリを観察すると値が stale な場合がある。

## ゲート OFF ビルド

```bash
cmake -B build-safe -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-toolchain.cmake \
      -DCLI_ENABLE_DANGEROUS_CMDS=OFF
cmake --build build-safe --target threadx
arm-none-eabi-nm build-safe/threadx.elf | grep -i devmem   # 何も出ない
```

## 検証

- **ビルド**: 既定 ON で `cmake --build build`（threadx + shell）が通る。ホストテスト
  （`shell/test/run_host_tests.sh`）が緑（`cli_hexdump_base` ケース含む）。
- **ゲート OFF**: `build-safe` の ELF に `devmem`（と `reboot`）シンボルが無い（要件 §18.10）。
- **実機**（`/dev/ttyACM0`, 115200 8N1, `flash`）:
    - `devmem peek 0x20000000` で DTCM read / `devmem peek 0x08000000 16` で Flash read。
    - `devmem dump 0x08000000 64` で先頭ベクタ表を絶対アドレス付き表示。
    - `devmem poke 0x20000000 0xdeadbeef` で write+リードバック、`peek` で確認。
    - 拒否（ハングしない）: `devmem peek 0x20000001 32`（アライメント）/
      `devmem peek 0x20000000 64`（幅）/ `devmem poke 0x20000000 0x1ff 8`（値幅）/
      `devmem peek 0x10000000`（Reserved hole）/ `devmem poke 0x08000000 1`（Flash write）/
      `devmem dump 0x40020000`（32-bit 専用域の dump）/ `devmem dump 0x20000000 99999`（cap 超過）。
    - `devmem peek 0x40020000 32` で GPIOA（peripheral, 32-bit）read。
    - Tab 補完: `devmem ` + Tab → `peek poke dump`。
    - 大きめの `devmem dump` 出力中に `Ctrl+c`: 16 バイト行ごとに `cli_cancel_requested()` を覗くので
      中断でき、`^C` でプロンプト復帰（協調キャンセル #16、[コマンド登録](shell-registration.md)）。
