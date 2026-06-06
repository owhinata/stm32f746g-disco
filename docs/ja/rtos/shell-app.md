# Shell アプリ（`shell` / `flash-shell`）

これまで実装した shell の各レイヤ（[登録](shell-registration.md) / [パーサ](shell-parser.md) /
[コア](shell-core.md) / [出力](shell-output.md) / [dummy backend](shell-testing.md) /
[UART(VCP) backend](shell-backend-uart.md)）を **CMake でライブラリ化し、実機で動く `shell`
アプリ**として統合する。ST-Link 仮想 COM ポート（VCP）上で対話シェルを起動し、**VCP と dummy の
2 インスタンスを同時稼働**させてマルチインスタンス構成を実機で実証する。

ここで起動するのは現時点の**最小ライン入力**（エコー / Backspace / CR-LF dispatch / Ctrl+C /
エスケープ swallow）。行編集・VT100・色は #9、履歴は #10、補完は #11、`version`/`uptime`/`reboot` は
#12、`thread` は #13、`devmem` は #14 で追加する。

## CMake 構成

`shell/` を 2 段で組み込む（既存 demo のビルドは温存）:

| ターゲット | 種別 | 内容 |
|---|---|---|
| `shell_obj` | OBJECT library | shell コア（`shell/core/*.c`）+ backend（`cli_backend_uart.c` / `cli_backend_dummy.c`）。`bsp_iface`（HAL/CMSIS/`bsp.h`）+ ThreadX include を持つ |
| `shell` | executable | `src/app_shell.c` + `shell/cmds/cmd_builtin.c` + `tx_glue.c` + ThreadX core/asm を `common` と `shell_obj` にリンク。`flash-shell` を生やす |

- objlib 名は **`shell` ではなく `shell_obj`**。CMake のターゲット名はグローバルに一意で、実行ファイルが
  `shell` を取るため。
- **`TX_INCLUDE_USER_DEFINE_FILE` は objlib と exe の両方**に与える。`cli_instance.h` が `tx_api.h` を
  取り込むので、exe にリンクされる ThreadX core と objlib が**同じ `port/threadx/tx_user.h`** を見ない
  と `TX_THREAD` / event-flags / mutex のレイアウトがずれる（ABI 不一致）。
- `threadx` アプリと同様、**`src/stm32f7xx_it.c` は除外**（PendSV は ThreadX 提供）。`USART1_IRQHandler`
  は UART backend が提供する。
- `cmd_builtin.c`（`help`/`echo`）は **exe にのみ**コンパイルする。ホストテストは別ビルドなので、
  テスト側が登録するコマンド集合に影響しない。

## インスタンスとスレッド（`src/app_shell.c`）

```c
CLI_BACKEND_UART_DEFINE(vcp_tr, &huart1);   CLI_INSTANCE_DEFINE(vcp_sh, &vcp_tr, "sh> ");
CLI_BACKEND_DUMMY_DEFINE(dum_tr);           CLI_INSTANCE_DEFINE(dum_sh, &dum_tr, "dum> ");
static struct cli_instance *const shells[] = { &vcp_sh, &dum_sh };
_Static_assert(SHELL_COUNT <= CLI_MAX_INSTANCES, ...);   /* §4.2 のコンパイル時ゲート */
```

| スレッド | 優先度 | 役割 |
|---|---|---|
| `vcp_sh` shell | `CLI_INSTANCE_PRIORITY`=16 | USART1 VCP 上の対話シェル |
| `dum_sh` shell | 16 | dummy backend（I/O ピン無し）上のシェル |
| `led` | 10 | LD1（PI1）を 250 ms で点滅（他スレッドとの共存提示） |
| `dummy_drv` | 17 | dummy を駆動して transcript を VCP に mirror |

`tx_application_define()` で `shells[]` を `cli_init()` → 成功時のみ `cli_start()`。
`cli_init`（backend / ThreadX オブジェクト生成）も `cli_start`（`tx_thread_create`）も失敗し得るので、
**どちらの失敗でも当該インスタンスだけ無効化して継続**（§9 fail-safe）。`enable()` 失敗は起動済み
スレッド内で uninit→exit するため、ここでは扱わない。

### dummy 駆動の race-free 設計

`dummy_drv` は dummy shell スレッド（16）より**スケジューリング優先度が低い**（数値 17、ThreadX は
数値が大きいほど低優先度）。ThreadX の strict-priority preemption により、`dummy_drv` は dummy shell
スレッドが ready でない間（= RX イベント待ちで
block 中 = `dummy_write()` の外）にしか走らない。`cli_dummy_inject()` 末尾の
`cli_transport_notify_rx()` で高優先度の dummy shell が即 preempt して 1 行を処理し再 block するため、
`inject()` 復帰時点で処理は完了している。よって driver の `cli_dummy_clear_output()`（inject 前）と
capture スナップショット（inject 後）は `dummy_write()` と重ならない。dummy backend は無排他設計
（[テスト doc](shell-testing.md) 参照）だが、この優先度 invariant で安全。settle sleep は保険。
万一の綻びも **cosmetic な `[dummy]` 行を乱すだけ**で、状態が分離された VCP インスタンスには波及
しない（§10）。

## 組込みコマンド（`shell/cmds/cmd_builtin.c`）

| コマンド | 登録 | 動作 |
|---|---|---|
| `help` | `CLI_CMD_REGISTER(help, NULL, ...,1,0)` | `CLI_ROOT_CMD_FOREACH` で登録ルートコマンドを一覧（`.shell_root_cmds` 走査が実機で効く証明, §18.1） |
| `echo` | `CLI_CMD_REGISTER(echo, NULL, ...,1,CLI_ARG_RAW)` | RAW 引数で行の残りをそのまま返す |

両ハンドラは渡された `sh` のみを出力 API 経由で触るため、複数インスタンスが同一コマンドを同時実行
しても再入安全（§10）。

## ビルド / フラッシュ / 接続

```bash
cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-toolchain.cmake
cmake --build build                          # threadx + shell
cmake --build build --target flash-shell     # ST-Link 書き込み
picocom -b 115200 /dev/ttyACM0               # VCP 接続（8N1）
```

接続すると `sh> ` プロンプトが出る。`help` でコマンド一覧、`echo hello world` で `hello world`、
未知コマンドは赤字で `…: command not found`、Ctrl+C で入力行キャンセル。数秒ごとに `[dummy] …` が
2 番目のインスタンスの transcript を mirror する。

!!! note "PA9 = VCP_TX / OTG_FS_VBUS 共用（UM1907）"
    PA9 は VCP_TX と OTG_FS_VBUS の共用ピン。**工場出荷のソルダーブリッジ**なら VCP が有効。
    USB-OTG ホスト用にブリッジを変更した個体では VCP_TX が使えない。

!!! warning "表示の interleave は既知制約（§10）"
    `[dummy]` の mirror（printf）と `sh>` の入力行は**同じ USART1** に出るため、表示上は混在し得る。
    最小ライン編集は入力行を再描画しないため。**状態は破壊されない**（インスタンス毎に分離）。
    入力行の再描画は #9。

## 検証

- **ビルド**: `cmake --build build` で `threadx` と `shell` が通る。optional demo
  （`-DBUILD_COREMARK=ON` 等）も configure/build できる（既存 demo を壊さない, DoD）。
- **ホスト単体テスト**: `sh shell/test/run_host_tests.sh` が通る（本アプリ追加の影響を受けない別ビルド）。
- **実機**（`/dev/ttyACM0` @115200 8N1, §18）: プロンプト / エコー / Backspace / `help` / `echo` /
  未知コマンド / Ctrl+C / `[dummy]` 2 インスタンス共存 / LD1 点滅を確認。エコー遅延 < 5 ms（§15
  暫定）、長行・連続貼付けで受信リング容量内の取りこぼし無し。
