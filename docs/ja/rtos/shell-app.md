# Shell アプリ（`threadx` / `flash`）

これまで実装した shell の各レイヤ（[登録](shell-registration.md) / [パーサ](shell-parser.md) /
[コア](shell-core.md) / [出力](shell-output.md) / [dummy backend](shell-testing.md) /
[UART(VCP) backend](shell-backend-uart.md)）を **CMake でライブラリ化し、実機で動く単一 firmware
`threadx`** として統合する。ST-Link 仮想 COM ポート（VCP）上で対話シェルを起動する。
マルチインスタンス構成（§4.2/§10）はホストテスト（2 つの dummy インスタンスで非混在を検証）と
#8 で実証済みのため、本デモは **VCP 単一インスタンス**に絞り、行編集セッションを混信なく観測できる
ようにしている（dummy backend はライブラリの正規 backend として残置）。

ここで起動する入力は [#9 の対話的行編集](shell-editing.md)（カーソル移動 / 行中挿入・削除 /
メタキー / VT100 escape / 端末幅折返し / 色）を備える。履歴は #10、補完は #11、
[`version`/`uptime`/`reboot`](shell-builtins.md) は #12、`thread` は #13、`devmem` は #14、
[`coremark`](shell-coremark.md) は #17 で追加。

## CMake 構成

`shell/` を 2 段で組み込み、唯一の firmware `threadx` を構成する:

| ターゲット | 種別 | 内容 |
|---|---|---|
| `shell_obj` | OBJECT library | shell コア（`shell/core/*.c`）+ backend（`cli_backend_uart.c` / `cli_backend_dummy.c`）。`bsp_iface`（HAL/CMSIS/`bsp.h`）+ ThreadX include を持つ |
| `coremark_obj` | OBJECT library | CoreMark（`lib/coremark` + `port/coremark`）を `-O3`・`MEM_METHOD=MEM_STATIC`・`core_main.c` を `-Dmain=coremark_main` でビルド（[CoreMark コマンド](shell-coremark.md)） |
| `threadx` | executable | `src/main.c` + `shell/cmds/cmd_*.c`（builtin/system/thread/devmem/coremark）+ `tx_glue.c` + ThreadX core/asm を `common` / `shell_obj` / `coremark_obj` にリンク。`-u _printf_float` 付与、`flash` を生やす |

- objlib 名は **`threadx` ではなく `shell_obj`**。CMake のターゲット名はグローバルに一意で、実行ファイルが
  `threadx` を取るため。
- **`TX_INCLUDE_USER_DEFINE_FILE` は objlib と exe の両方**に与える。`cli_instance.h` が `tx_api.h` を
  取り込むので、exe にリンクされる ThreadX core と objlib が**同じ `port/threadx/tx_user.h`** を見ない
  と `TX_THREAD` / event-flags / mutex のレイアウトがずれる（ABI 不一致）。
- `threadx` アプリと同様、**`src/stm32f7xx_it.c` は除外**（PendSV は ThreadX 提供）。`USART1_IRQHandler`
  は UART backend が提供する。
- `cmd_builtin.c`（`help`/`echo`）は **exe にのみ**コンパイルする。ホストテストは別ビルドなので、
  テスト側が登録するコマンド集合に影響しない。

## インスタンスとスレッド（`src/main.c`）

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
| `help` | `CLI_CMD_REGISTER(help, NULL, ...,1,CLI_MAX_SUBCMD_DEPTH+1)` | **階層的に**コマンド help を表示（`help` / `help <cmd>` / `help <cmd> <sub>`）。`CLI_ROOT_CMD_FOREACH` + サブコマンド集合の番兵走査（`.shell_root_cmds` 走査が実機で効く証明, §18.1）。詳細は下記の節 |
| `echo` | `CLI_CMD_REGISTER(echo, NULL, ...,1,CLI_ARG_RAW)` | RAW 引数で行の残りをそのまま返す |

両ハンドラは渡された `sh` のみを出力 API 経由で触るため、複数インスタンスが同一コマンドを同時実行
しても再入安全（§10）。

## 階層的 help と引数 usage（#37）

`help` は登録済みコマンドツリーを **public API のみ**（`CLI_ROOT_CMD_FOREACH` + サブコマンド集合の
番兵走査）で辿り、3 形式で表示する。`struct cli_cmd` に既にある**一行 `.help` を流用**し、descriptor へ
新フィールドは足さない。

| 形式 | 表示 |
|---|---|
| `help` | 全ルートコマンド一覧。サブコマンドを持つものは行末 `>`（command group マーカ）|
| `help <cmd>` | group なら自身の help + 直下サブコマンド一覧、leaf なら一行 help を usage 相当で表示 |
| `help <cmd> <sub> …` | パスに沿ってツリーを下り、解決ノードを上と同様に表示 |

未知のコマンド/サブコマンドは `help: no such command '<path>'` を赤字で出し非ゼロ終了。

```text
sh> help
Commands:
  echo       echo the rest of the line
  fs         QSPI flash filesystem (FileX + LevelX) >
  help       list commands
  ...
'>' marks a command group; type 'help <command> [subcommand]' for details.

sh> help fs
fs -- QSPI flash filesystem (FileX + LevelX)
Subcommands:
  ls         list directory [path]
  cat        print file <path>
  write      write <path> <text>
  ...
Type 'help fs <subcommand>' for details.

sh> help fs ls
fs ls  list directory [path]
```

**登録**: `help` は `optional = CLI_MAX_SUBCMD_DEPTH + 1`（`CLI_ARG_RAW` は**不可** — RAW は残りを 1
引数にまとめ `help fs ls` をトークン分割しない）。これで最深パス `help + root + CLI_MAX_SUBCMD_DEPTH
段のサブコマンド`まで受けられる。

### 引数エラー時の usage 自動表示

引数の数が不正（`CLI_PARSE_WRONG_ARGS`）なとき、`cli_dispatch_segment`（`cli_session.c`）は従来の
`<cmd>: invalid number of arguments` に続けて **usage 行**を出す。`cli_parse()` が `WRONG_ARGS` を返す前に
セットした解決済みコマンド（`pr.cmd` / `pr.cmd_level`）と `sh->argv` からフルコマンドパスを組み、`.help`
を併記する:

```text
sh> fs write
write: invalid number of arguments
usage: fs write  (write <path> <text>)
```

`(...)` 内は**コマンドの一行 `.help` を usage 相当として流用**したもの（`.help` は arg 構文寄り＝
`write <path> <text>` と説明寄り＝`capacity / free / state` が混在する点に注意）。usage 行はフルパスを
ローカルバッファに 1 度組み立て**単一 `cli_print()`** で出力し、背景ジョブ出力が行の途中に割り込まない
ようにする（§10/§11）。

## ビルド / フラッシュ / 接続

```bash
cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-toolchain.cmake
cmake --build build                          # 唯一のターゲット threadx
cmake --build build --target flash   # ST-Link 書き込み
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

- **ビルド**: `cmake --build build` で唯一のターゲット `threadx` が通る。
- **ホスト単体テスト**: `sh shell/test/run_host_tests.sh` が通る（本アプリ追加の影響を受けない別ビルド）。
- **実機**（`/dev/ttyACM0` @115200 8N1, §18）: プロンプト / エコー / [行編集](shell-editing.md)
  （カーソル移動・行中挿入削除・メタキー・折返し）/ `help` / `echo` / 未知コマンド / Ctrl+C /
  LD1 点滅を確認。エコー遅延 < 5 ms・操作応答 < 50 ms（§15）、長行・連続貼付けで受信リング容量内の取りこぼし無し。
