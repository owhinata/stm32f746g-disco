# ログとクラッシュダンプ（`dmesg` / fault）

Issue #28。RAM リングバッファのログサブシステムと、フォルト時のクラッシュダンプを追加する。
ログは **リセットを跨いで保持** され、リブート後に `dmesg` で直前ログ（クラッシュ情報含む）を
再生できる。clean-room 設計（NuttX ramlog / Zephyr logging の*概念*のみ借用、コードは流用しない）。

構成:

- `include/log.h` / `src/log.c` — DTCM 上のレベル付きリングバッファとログ API。
- `shell/cmds/cmd_dmesg.c` — ログを閲覧する `dmesg` コマンド。
- `src/fault.c` — HardFault/MemManage/BusFault/UsageFault ハンドラとクラッシュダンプ。
- `shell/cmds/cmd_crash.c` — フォルトを意図的に起こすテスト用 `crash` コマンド（危険コマンド）。
- 整形は[出力 API](shell-output.md) から切り出した clean-room フォーマッタ（`shell/core/cli_fmt.c`）を
  共有する。`cli_print` も `dmesg` も fault ダンプも同じ実装を使う。

## RAM ログリング（DTCM `.log_noinit`）

ログ領域はリンカスクリプト（`ldscript/STM32F746NGHx_FLASH.ld`）の `.log_noinit (NOLOAD)`
セクションに置き、RAM 先頭＝**DTCM（`0x20000000`）**に配置する。これが保持要件の要:

- **キャッシュ非経由**: Cortex-M7 の TCM は L1 D-Cache を通らない。フォルト文脈で書いた直後に
  リセットしても cache のクリーンバック不要で SRAM に確定する。SWD からも常に最新が見える。
- **リセット保持**: CMSIS `Reset_Handler` は `.data`/`.bss` のみ初期化し `.noinit` は触らない。
  よって **system reset**（reboot=SYSRESETREQ / fault / IWDG / WWDG / NRST / LPWR、RM0385 §5.1.1）
  では内容が保持される。一方 **power reset**（POR/PDR/BOR、§5.1.2）では保証されない →
  `log_init()` が magic を検証し、壊れていれば初期化する。
- `.log_noinit` は `.data`/`.bss` の前に挿入するので、それらは後方へずれるだけ（`_estack` は不変）。
  リンカの `ASSERT` で 64 KB DTCM 内に収まることを保証する。既定サイズ 8 KB。

### レコード形式

32 byte のヘッダ（magic / version / size / head / tail / seq / boot_count）に続く可変長レコード列。
`head`/`tail` は **free-running 32-bit バイトオフセット**（`& (size-1)` でアクセス、size は 2 の冪）。
空 = `head==tail`、満杯 = `head-tail==size` で一意に判別できる。

各レコード（4 byte 整列）: `{u16 total_len | u8 level | u8 magic} {u32 ts_ms} {u32 seq} {char tag[8]}
{text NUL 終端 + pad}`。**tag は 8 byte インライン固定**（flash 文字列ポインタを持たないので、
再フラッシュ後も旧レコードが読める）。レコードは物理末尾を跨がない: 跨ぐ場合は末尾断片を
**SKIP レコード**（先頭 4 byte のみ、`level=0xFF`）で埋めて先頭へ折り返す。空き不足時は tail から
**最古レコードを丸ごと退避**して新レコードを収める。

### 整合性

- 書き込みは **PRIMASK クリティカルセクション**（`tx_*` を呼ばない）。thread / ISR / fault の
  どの文脈からも安全。整形はセクション外（スタック上）で済ませ、区間内は退避＋コピー＋head 更新のみ。
- 本文を書いてから `__DMB()` を挟んで `head` を更新する。head 更新前にリセットが入っても boot 検証は
  旧 head までしか読まないため、**書きかけレコードは可視化されない**。
- boot 検証は tail→head を walk し、最初の不正レコードで head を切り詰める（resync）。書き込み途中の
  リセットで壊れるのは末尾 1 レコードだけ。

## ログ API

`#include "log.h"` の**前に** `LOG_TAG` を定義し、レベルマクロを呼ぶ:

```c
#define LOG_TAG "uart"
#include "log.h"

LOG_INF("baud=%u", baud);
LOG_ERR("init failed: %d", rc);
```

- レベルは `LOG_ERR` / `LOG_WRN` / `LOG_INF` / `LOG_DBG`（小さいほど重要）。
- **コンパイル時しきい値** `LOG_COMPILE_LEVEL`（既定 DBG）を超えるレベルは呼び出しごと消える。
- **実行時しきい値**（既定 INF）を超えるレベルは記録されない。`dmesg -n <lvl>` で変更。
- タイムスタンプは `HAL_GetTick()`（ms 単調）。**通常ログはコンソールにエコーしない**
  （対話シェルを汚さない）。閲覧は `dmesg` 専用。
- `log_init()` は `bsp_init()` が **`fault_init()` より前**に呼ぶ。リング検証 → `log_ready` 立て →
  boot マーカー記録（`boot: #<N> reset cause: ...`）の順。`log_write()` は `log_init()` 完了まで no-op。

## `dmesg` コマンド

```text
sh> dmesg
[     0.000] INF boot: #1 reset cause: POR
[    12.345] INF boot: #2 reset cause: SFT
[    60.001] ERR fault: bus cfsr=00000400 hfsr=00000000 mmfar=00000000 bfar=20080000
[    60.001] ERR fault: pc=080012a4 lr=08001233 psr=61000000 sp=2000c7d8 exc=fffffffd
sh> dmesg -c            # 表示後にクリア
sh> dmesg -n dbg        # 実行時しきい値を DBG に変更（以後 LOG_DBG も記録）
dmesg: level = DBG
```

- 出力フォーマット: `[<秒>.<ミリ秒>] <LVL> <tag>: <text>`、oldest → newest。
- `log_iter_next()` がレコード 1 件ずつ短い PRIMASK 区間でコピーアウトする。走査開始時に `head` を
  snapshot するので、背景ログが続いても live tail を追い続けない。退避に追い越されたら tail へ resync。
- レコード間で `cli_cancel_requested()` を覗くので **Ctrl+C で中断**できる（[出力 API](shell-output.md)
  の hexdump と同じ協調キャンセル）。

## クラッシュダンプ（fault ハンドラ）

`src/fault.c` が HardFault/MemManage/BusFault/UsageFault の **strong 定義**で CMSIS startup の
weak alias（`Default_Handler` の無限ループ）を上書きする。`fault_init()`（`bsp_init()` 内）が
`SCB->SHCSR` で MemManage/Bus/Usage を個別 enable し、`CCR.DIV_0_TRP` で 0 除算を UsageFault にする。

フォルト時の流れ:

1. naked スタブが `EXC_RETURN` bit2 で MSP/PSP を選び、例外フレーム・`EXC_RETURN`・callee-saved
   レジスタ（R4-R11）を C ハンドラへ渡す。種別は `SCB->ICSR` の VECTACTIVE から判定。
2. **RAM ログへ先に記録**（UART が死んでいても残る）: `CFSR/HFSR/MMFAR/BFAR` と
   `pc/lr/psr/sp/exc` の 2 行。
3. **USART1 へポーリング出力**（HAL/IRQ/ThreadX 非経由）: 種別、フォルト系レジスタ、
   R0-R12/SP/LR/PC/xPSR、SP 周辺スタックダンプ、スタック走査バックトレース。
4. **halt**（busy loop、WFI 不使用）。旧 ST-Link は sleep 中のコアに attach できない（#20/#24/#26）。
   halt 中は SWD で観測可能。

ダンプの詳細:

- **SP 復元**: `EXC_RETURN` bit4 で 8 word（基本）か 26 word（FPU 拡張）を選び、stacked xPSR bit9
  （STKALIGN パディング）を加味して再構成（PM0253 §2.4.7）。
- **スタックダンプ**: PSP（スレッド）時は `_tx_thread_current_ptr` の `tx_thread_stack_start/end` で、
  MSP 時は `_estack` でクランプ。SP が範囲外なら異常とみなし短縮ダンプ。
- **バックトレース**: フレームポインタ非使用。スタックを走査し、`.text`（`_stext`–`_etext`）内を指す
  Thumb タグ付きワードで、直前が `BL`/`BLX` のものを復帰アドレス候補とする（最大 16 件）。

```text
*** FAULT: BusFault ***
 CFSR=00000400 HFSR=00000000 MMFAR=00000000 BFAR=20080000(valid)
 R0=20080000 R1=00000000 ...
 ...
 stack @sp:
  2000c7d8: 080012a5 20000a40 ...
 backtrace:
  #0 080012a4 (pc)
  #1 08001232 (lr)
  #2 08001a90
 halted.
```

## `crash` テストコマンド（危険コマンド）

クラッシュダンプを実機検証するための専用コマンド。`devmem` は領域許可リストが Reserved を弾くため
フォルトを起こせない。`crash` は `CLI_ENABLE_DANGEROUS_CMDS` 有効時のみコンパイルされる
（`reboot`/`devmem` と同じゲート）。**実行すると halt して戻らない。**

| サブコマンド | 起こすフォルト |
|---|---|
| `crash bus` | `0x20080000`（Reserved）を read → precise BusFault（BFAR 検証） |
| `crash undef` | 未定義命令（`UDF #0`）→ UsageFault（UNDEFINSTR） |
| `crash div0` | 整数 0 除算 → UsageFault（DIVBYZERO） |

## 注意点

- タイムスタンプは `HAL_GetTick()`（ms）。約 **49.7 日**で 32-bit が wrap する（実用上問題ない）。
- リセット原因は `RCC->CSR`（RM0385 §5.3.21）から読み、`RMVF` で消去するので次回 boot は自前の原因を
  読む。複数フラグが立つ場合は LPWR > WWDG > IWDG > SFT > POR > BOR > PIN の優先で 1 つ表示。

## 検証

- **ビルド**: `cmake --build build`。ホストテスト（`shell/test/run_host_tests.sh`）が緑
  （フォーマッタ抽出後も `cli_print`/hexdump の出力は不変）。
- **実機**（`/dev/ttyACM0`, 115200 8N1, `flash`）:
    - boot 直後 `dmesg` に `boot: #N reset cause: ...` 行が出る。
    - `reboot` 後 `dmesg` に前回 boot のログが残り、新しい `reset cause: SFT` の boot 行が続く
      （**リセット跨ぎ保持**）。
    - `dmesg -c` で全消去、`dmesg -n dbg` 後に `LOG_DBG` が記録される。
    - `crash bus` で UART に即時ダンプ（`BusFault` / `BFAR=20080000` / スタック / バックトレース）→
      halt。ボードリセット後 `dmesg` に fault 2 行が新 boot 行の前に残る。`crash undef` / `crash div0`
      も同様に UsageFault を表示。
    - halt 中に SWD（`openocd` + `gdb-multiarch`）で attach 可能。
