# システム組込みコマンド（`version` / `uptime` / `reboot`）

M4「組込みコマンド」の最初の一歩（#12）。`help`/`echo`（[shell アプリ](shell-app.md)）に続き、
**`version` / `uptime` / `reboot`** の 3 つを追加し、同時に **危険コマンドのコンパイル時ゲート
基盤 `CLI_ENABLE_DANGEROUS_CMDS`**（要件 §12）を導入する。これらは `shell/cmds/cmd_system.c`
にあり、`cmd_builtin.c` と同じく **exe にのみ**リンクされる（ホストテストの登録集合に影響しない）。

## コマンド一覧

| コマンド | 登録 | 動作 |
|---|---|---|
| `version` | `CLI_CMD_REGISTER(version, NULL, ...,1,0)` | FW 名/版・git describe・ビルド日時・ThreadX 版・MCU シリコン id（device/rev/flash/UID）を表示 |
| `uptime` | `CLI_CMD_REGISTER(uptime, NULL, ...,1,0)` | 起動からの経過時間を `Dd HH:MM:SS (N ms)` で表示 |
| `reboot` | `CLI_CMD_REGISTER(reboot, NULL, ...,1,0)`（`#if CLI_ENABLE_DANGEROUS_CMDS`） | 即時ソフトリセット。**危険コマンド**ゲートで囲まれ、OFF ビルドでは無効化 |

3 ハンドラとも渡された `sh` のみを出力 API 経由で触るため、複数インスタンス同時実行でも再入安全（§10）。

## `version`

```text
sh> version
ThreadX Shell v0.1.0 (d264989-dirty)
Built:    Jun  7 2026 12:00:00
ThreadX:  6.5.0
MCU:      STM32F746 (devid 0x449 rev 0x1001)
Flash:    1024 KB
UID:      0x00370027 0x32355119 0x...
```

ハードウェア識別は CMSIS のレジスタマクロから直接読む（RM0385 照合済）:

| 項目 | 取得元 | RM0385 |
|---|---|---|
| device id / revision | `DBGMCU->IDCODE`（`DBGMCU_IDCODE_DEV_ID_Msk` / `REV_ID_Pos`） | §40.6.1（`0xE0042000`） |
| 96-bit 固有 UID | `UID_BASE`（`0x1FF0F420`, 3×u32） | §41.1 |
| Flash サイズ(KB) | `FLASHSIZE_BASE`（`0x1FF0F442`, u16） | §41.2 |

- **git describe** は CMake configure 時に `git describe --always --dirty --tags` を取得し
  `cmake/cli_version.h.in` → `build/gen/cli_version.h` に埋め込む（**configure 時スナップショット**。
  コミット後に更新したいときは再 configure）。
- **ビルド日時**はコンパイラの `__DATE__` / `__TIME__`（当該 TU の再コンパイルで更新）。
- 32-bit 値は `(unsigned long)` cast + `%lu` / `%08lx` で出力（`cli_print` の `%l*` 経路に合わせる）。

## `uptime`

`HAL_GetTick()`（SysTick → `HAL_IncTick`、1 kHz = 1 ms tick、`port/threadx/tx_glue.c`）の
ミリ秒カウンタを日/時/分/秒に変換し、生 ms も併記する。

```text
sh> uptime
up 0d 00:03:12 (192341 ms)
```

!!! note "49.7 日で wrap"
    `HAL_GetTick()` は `uint32_t` のため約 49.7 日で 0 に巻き戻る。長期連続稼働の表示は参考値。

## `reboot`（危険コマンド）

`HAL_NVIC_SystemReset()`（SCB→AIRCR の `SYSRESETREQ`、RM0385 §5.1.1）で**即時**リセットする。
確認プロンプトは要件 §12 でスコープ外。

```text
sh> reboot
rebooting...
（ボードが再起動し、起動バナーが再表示される）
```

- `cli_print()` は IRQ 駆動の UART TX リングへ **enqueue** して返る（on-wire 送信完了ではない）。
  そのままリセットすると最終行が途切れるため、`tx_thread_sleep(50)`（~50 ms）でリングを流し切ってから
  リセットする。50 ms は **512 B フルリングの最悪ケース**（115200 8N1 で ~44 ms）をカバーする best-effort。
- リセット直前に `__disable_irq()` は**しない**。TX 完了 IRQ がリングを drain するので、sleep 中は
  割込みを有効のままにする。

## 危険コマンドゲート `CLI_ENABLE_DANGEROUS_CMDS`（§12）

`reboot` と `devmem`（[メモリアクセス](shell-devmem.md)）はコンパイル時に一括無効化できる:

- 既定値は `shell/include/cli_config.h` の `#ifndef` で **ON (=1)**（他 knob と同スタイル。define
  未指定 target＝ホストテスト等の安全網）。
- **CMake cache 変数は compiler define に自動伝播しない**ため、`shell` ターゲットが CMake オプションを
  define へ明示転送する:

```cmake
option(CLI_ENABLE_DANGEROUS_CMDS "Build the dangerous shell commands (reboot, devmem)" ON)
target_compile_definitions(shell PRIVATE
    CLI_ENABLE_DANGEROUS_CMDS=$<BOOL:${CLI_ENABLE_DANGEROUS_CMDS}>)
```

OFF にすると `reboot` のハンドラと `CLI_CMD_REGISTER` が丸ごと `#if` で除外され、`.shell_root_cmds`
に出ない＝`help`・Tab 補完からも消える。

```bash
# 本番想定（危険コマンド無効）ビルド
cmake -B build-safe -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-toolchain.cmake \
      -DCLI_ENABLE_DANGEROUS_CMDS=OFF
cmake --build build-safe --target threadx
arm-none-eabi-nm build-safe/threadx.elf | grep -i reboot   # 何も出ない（reboot 無効）
```

## 検証

- **ビルド**: 既定 ON で `cmake --build build`（`threadx` + `shell`）が通る（既存 demo 非破壊）。
- **ゲート OFF**: 上記 `build-safe` で `reboot` シンボルが消え、`version`/`uptime` は残る（§18.10）。
- **実機**（`/dev/ttyACM0` @115200 8N1）:
  `help` に 3 コマンドが並ぶ / `version` がリッチ出力 / `uptime` が時間経過で増加 /
  `reboot` で再起動（バナー再表示）/ Tab 補完で `ver`→`version`・`re`→`reboot`。
