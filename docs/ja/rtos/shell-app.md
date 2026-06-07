# Shell アプリ（`shell` / `flash-shell`）

これまで実装した shell の各レイヤ（[登録](shell-registration.md) / [パーサ](shell-parser.md) /
[コア](shell-core.md) / [出力](shell-output.md) / [dummy backend](shell-testing.md) /
[UART(VCP) backend](shell-backend-uart.md)）を **CMake でライブラリ化し、実機で動く `shell`
アプリ**として統合する。ST-Link 仮想 COM ポート（VCP）上で対話シェルを起動する。
マルチインスタンス構成（§4.2/§10）はホストテスト（2 つの dummy インスタンスで非混在を検証）と
#8 で実証済みのため、本デモは **VCP 単一インスタンス**に絞り、行編集セッションを混信なく観測できる
ようにしている（dummy backend はライブラリの正規 backend として残置）。

ここで起動する入力は [#9 の対話的行編集](shell-editing.md)（カーソル移動 / 行中挿入・削除 /
メタキー / VT100 escape / 端末幅折返し / 色）を備える。履歴は #10、補完は #11、
[`version`/`uptime`/`reboot`](shell-builtins.md) は #12、`thread` は #13、`devmem` は #14 で追加する。

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
static struct cli_instance *const shells[] = { &vcp_sh };
_Static_assert(SHELL_COUNT <= CLI_MAX_INSTANCES, ...);   /* §4.2 のコンパイル時ゲート */
```

| スレッド | 優先度 | 役割 |
|---|---|---|
| `vcp_sh` shell | `CLI_INSTANCE_PRIORITY`=16 | USART1 VCP 上の対話シェル |
| `led` | 10 | LD1（PI1）を 250 ms で点滅（他スレッドとの共存提示） |

`tx_application_define()` で `shells[]` を `cli_init()` → 成功時のみ `cli_start()`。
`cli_init`（backend / ThreadX オブジェクト生成）も `cli_start`（`tx_thread_create`）も失敗し得るので、
**どちらの失敗でも当該インスタンスだけ無効化して継続**（§9 fail-safe）。`enable()` 失敗は起動済み
スレッド内で uninit→exit するため、ここでは扱わない。`shells[]` に transport を足せば
インスタンスを増やせる（上限は §4.2 のコンパイル時ゲート）。

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
未知コマンドは赤字で `…: command not found`、Ctrl+C で入力行キャンセル。カーソル移動・行中編集・
メタキー・折返し再描画など [#9 の行編集](shell-editing.md)が効く。

!!! note "PA9 = VCP_TX / OTG_FS_VBUS 共用（UM1907）"
    PA9 は VCP_TX と OTG_FS_VBUS の共用ピン。**工場出荷のソルダーブリッジ**なら VCP が有効。
    USB-OTG ホスト用にブリッジを変更した個体では VCP_TX が使えない。

!!! warning "他スレッド printf との interleave（§10）"
    起動バナーや他スレッドの `printf` も**同じ USART1** に出る。編集中の行に printf が割り込むと
    表示が乱れ得るが、**状態は破壊されない**（インスタンス毎に分離）。`Ctrl+l` で入力行を
    [再描画](shell-editing.md)して復旧できる。

## 検証

- **ビルド**: `cmake --build build` で `threadx` と `shell` が通る。optional demo
  （`-DBUILD_COREMARK=ON` 等）も configure/build できる（既存 demo を壊さない, DoD）。
- **ホスト単体テスト**: `sh shell/test/run_host_tests.sh` が通る（本アプリ追加の影響を受けない別ビルド）。
- **実機**（`/dev/ttyACM0` @115200 8N1, §18）: プロンプト / エコー / [行編集](shell-editing.md)
  （カーソル移動・行中挿入削除・メタキー・折返し）/ `help` / `echo` / 未知コマンド / Ctrl+C /
  LD1 点滅を確認。エコー遅延 < 5 ms・操作応答 < 50 ms（§15）、長行・連続貼付けで受信リング容量内の取りこぼし無し。
