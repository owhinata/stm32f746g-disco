# Shell アーキ概要

単一 firmware **`threadx`** は対話型 **ThreadX CLI シェル**（USART1 VCP）。コマンド・ベンチ（`coremark`）等はすべて shell コマンドとして起動する（[Shell アプリ](shell-app.md)）。本ページは全体像と「**コマンド定義 / backend 追加 / ピン・UART 前提 / 危険コマンドゲート**」を 1 本にまとめ、詳細は各トピックへリンクする。

shell（`shell/`）は **clean-room 実装**で、Zephyr RTOS shell の**設計・公開 API の振る舞いのみ参考**にしコードは流用していない（証跡は [NOTICE](https://github.com/owhinata/stm32f746g-disco/blob/main/NOTICE)）。新規ファイルは全て MIT（`SPDX-License-Identifier: MIT` / `Copyright (c) 2026 ThreadX Shell Project`）。

## レイヤ構成

受信から出力までの流れ（実機 UART backend の例）:

```
USART1 IRQ → backend(cli_backend_uart) RX ring
   → コア(cli_core: ThreadX thread/event flags/mutex)
   → 行編集(cli_edit) / 履歴(cli_history) / 補完(cli_complete)
   → Enter でパーサ(cli_parse) → dispatch
   → コマンドハンドラ(shell/cmds/*)
   → 出力API(cli_printf, ロック+フロー制御)
   → backend write() → USART1 TX ring → IRQ 送信
```

| レイヤ | 実体 | 役割 | 詳細 |
|---|---|---|---|
| 登録 | `.shell_root_cmds`(リンカセクション) + `CLI_CMD_REGISTER` | コマンドを宣言的に登録、起動時に走査 | [登録基盤](shell-registration.md) |
| パーサ | `cli_parse.c` | トークン分割 + コマンド木探索 + argc/argv 検証（`struct cli_instance` 非依存の純粋関数） | [パーサ](shell-parser.md) |
| コア | `cli_core.c`(ThreadX) + `cli_session.c`(純粋) | スレッド/イベントフラグ/mutex の lifecycle と RX→dispatch 状態機械 | [コア](shell-core.md) |
| 行編集 | `cli_edit.c` | カーソル/行中編集/メタキー/VT100/端末幅折返し/色 | [行編集](shell-editing.md) |
| 履歴 | `cli_history.c` | 固定 byte リング（per-instance, FIFO 退避） | [履歴](shell-editing.md) |
| 補完 | `cli_complete.c` | Tab 補完（bash 風 2 段階） | [補完](shell-completion.md) |
| 出力 | `cli_printf.c` | 最小 printf + 32B staging + flow control + 色/hexdump | [出力 API](shell-output.md) |
| backend | `cli_backend_uart.c` / `cli_backend_dummy.c` | transport 抽象（実機 UART / ホストテスト用ループバック） | [UART backend](shell-backend-uart.md) / [テスト](shell-testing.md) |

コア（`cli_core` ThreadX 依存 / `cli_session`・`cli_parse`・`cli_printf` 純粋）の **2 分割**により、純粋部分はホスト gcc 単体テストできる（[テスト基盤](shell-testing.md)）。全ストレージは静的確保で実行時ヒープ確保なし。

## インスタンスとスレッドモデル

- **1 transport = 1 `cli_instance`**（静的確保）。コマンド木は全インスタンス共有の read-only データ、コアは可変グローバルを持たないため**複数インスタンスが干渉せず同時稼働**できる（要件 §10）。マルチインスタンスはホストテスト（2 dummy）で検証。
- 各インスタンスは ThreadX スレッド（既定優先度 `CLI_INSTANCE_PRIORITY`=16）。ISR は event flag を立てるだけでスレッドを起こす。ThreadX 統合（SysTick が HAL tick と ThreadX tick を兼ねる・割込み優先度の注意点）は [ThreadX 統合](threadx.md)。
- 現状の firmware は **VCP 単一インスタンス** + LED ハートビート（優先度 10）。

!!! note "出力の 2 経路（printf vs cli_print）"
    `cli_print`/`cli_write` はインスタンスの transport `write()` 経由（**インスタンス毎**）。一方 C ライブラリ `printf` は UART backend の strong `_write` 経由で**単一グローバルコンソール `g_uart_console`** に出る（[UART backend](shell-backend-uart.md)）。`coremark` の出力は printf 経由。マルチ端末で printf を呼び出し端末へ追従させる改善は別 Issue（#18）。

## コマンドの定義方法

各ソースから**宣言的に登録**する（詳細は [登録基盤](shell-registration.md)）:

```c
#include "cli.h"

static int cmd_version(struct cli_instance *sh, int argc, char **argv) { /* ... */ return 0; }

/* サブコマンド木: thread { list, stacks } */
CLI_SUBCMD_SET_CREATE(sub_thread,
    CLI_CMD_ARG(list,   NULL, "list threads",     cmd_list,   1, 0),
    CLI_CMD_ARG(stacks, NULL, "show stack usage", cmd_stacks, 1, 0),
    CLI_SUBCMD_SET_END);

CLI_CMD_REGISTER(version, NULL,       "show version",      cmd_version, 1, 0);  /* root */
CLI_CMD_REGISTER(thread,  sub_thread, "thread operations", NULL,        2, 0);  /* 親 */
```

- ハンドラ型 `int (*)(struct cli_instance *sh, int argc, char **argv)`。`argv[0]` はコマンド名、戻り値 0 が成功。argc/argv 検証（`mandatory`/`optional`、`CLI_ARG_RAW` で行末を生取り）はパーサがハンドラ前に実施。
- ハンドラは渡された `sh` のみを出力 API 経由で触れば**再入安全**（複数インスタンス同時実行可）。
- コマンドソースは `cli.h` だけを include すれば ThreadX 非依存でコンパイルできる。`shell/cmds/cmd_*.c` は firmware exe のみにリンク（ホストテストは別ビルド）。

## backend の追加手順

新しい transport は `struct cli_transport_api` を実装する（詳細は [UART backend](shell-backend-uart.md) / dummy は [テスト](shell-testing.md)）:

1. **API テーブル**を実装: `init` / `enable` / `write` / `read` 必須、`uninit` / `update` 任意。
   - `write` 契約 = **非ブロッキング**。受理したバイト数（0..len）を返す。満杯で `n<len` のときは TX 空きが出たら `cli_transport_notify_tx()` を発火する義務（コアは `CLI_EVT_TX` で待ち、タイムアウト付きで「送信完了までブロック」を実現）。
   - `read` = 非ブロッキングに 0..cap バイトを drain。
2. ISR / コールバックから `cli_transport_notify_rx()` / `cli_transport_notify_tx()` でスレッドを起こす（**event flag を立てるだけ**・ロックや行状態に触れない）。
3. `CLI_BACKEND_xxx_DEFINE(name, ...)` で transport を静的定義し、`CLI_INSTANCE_DEFINE(inst, &name, "prompt> ")` でインスタンスに束ねる。
4. `tx_application_define()` で `cli_init()` → 成功時 `cli_start()`（失敗は当該インスタンスのみ無効化、§9 fail-safe）。

byte リング（`cli_uart_ring.h`）は HAL/ThreadX 非依存でホスト単体テスト可能。

## ピン / UART 前提

| 項目 | 値 | 備考 |
|---|---|---|
| VCP | USART1 TX=PA9 / RX=PB7、115200 8N1 | ST-Link 仮想 COM（`/dev/ttyACM0`） |
| PA9 共用 | VCP_TX / OTG_FS_VBUS | **工場出荷ソルダーブリッジ**で VCP 有効（UM1907）。USB-OTG ホスト用に変更した個体は VCP_TX 不可 |
| LED | LD1（緑）= PI1 | ハートビートスレッド |

`_write`（printf retarget）は printf 出力中の bare `\n` を `\r\n` に変換するため、端末側 LF→CRLF 設定（`picocom --imap lfcrlf`）無しでも `coremark` レポート等が崩れない。詳細は [ハードウェア/ボード](../hardware/board.md) と [Shell アプリ](shell-app.md)。

## 危険コマンドゲート

- **`CLI_ENABLE_DANGEROUS_CMDS`**（CMake option、既定 ON）: `reboot` / `devmem` をコンパイル時にゲート。OFF（本番想定: `-DCLI_ENABLE_DANGEROUS_CMDS=OFF`）でハンドラと登録ごと消え、`.shell_root_cmds` から外れて help・補完からも消える（要件 §12 / §18.10）。
- **`devmem` のアドレス範囲ゲート**: 単一 [min,max] ではなくコンパイル時の**リージョン許可リスト `devmem_map[]`**。アクセスは 1 リージョンに完全包含され、方向（r/w）と幅（8/16/32）が許可されることを要する。非該当（Reserved hole 等）は実行せずエラー復帰（未マップ域フォルト＝weak `Default_Handler` のハングを回避）。詳細は [devmem](shell-devmem.md) / [組込みコマンド](shell-builtins.md)。

## ビルドと構成

- 単一 firmware `threadx`。shell コア/backend は OBJECT lib `shell_obj`、CoreMark は `coremark_obj`（`-O3`）に分離して `threadx` exe へリンク。`cmake --build build` でビルド、`--target flash` で書き込み。詳細は [ビルド (CMake)](../build/cmake.md) / [Shell アプリ](shell-app.md) / [CoreMark コマンド](shell-coremark.md)。

## clean-room / ライセンス

設計は Zephyr RTOS shell（Apache-2.0）の**概念・公開 API の振る舞いのみ参考**で、コード・コメント・固有文言は一切流用していない（要件 §16）。shell 新規ファイルは MIT。第三者 component（ST HAL/CMSIS、ThreadX、CoreMark）の attribution と合わせて [NOTICE](https://github.com/owhinata/stm32f746g-disco/blob/main/NOTICE) に記載。
