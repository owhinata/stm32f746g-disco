# ビルド (CMake)

ビルドは **CMake + Ninja**。初回 configure で ARM GNU ツールチェーンを `./tools` に自動 DL し、git submodule も自動 init する。

## 前提ツール

`cmake`, `ninja`, `git`, `curl`、書き込みに `st-flash`。

## configure / build

```bash
cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-toolchain.cmake
cmake --build build            # 唯一のターゲット threadx をビルド
```

成果物: `build/threadx.{elf,hex,bin}`。`threadx` は対話 ThreadX シェルで、**唯一の firmware**。CoreMark は別イメージではなく shell の `coremark` コマンドとして実行する（[CoreMark コマンド](../rtos/shell-coremark.md)）。

## flash

```bash
cmake --build build --target flash     # ST-Link で書き込み
```

`flash` は `st-flash --connect-under-reset --reset write threadx.bin 0x08000000` を実行。
`--connect-under-reset` は #20(`TX_ENABLE_WFI`) 以降必須: 走行中 firmware が idle で WFI sleep に
入るため、旧オンボード ST-Link は reset 保持なしでは sleep 中の core に接続できない（#24）。

## 構成のポイント

- `cmake/arm-none-eabi-toolchain.cmake`: ツールチェーン自動 DL（`tools/.../arm-none-eabi-gcc` が無ければ取得）
- `CMakeLists.txt`: HAL + CMSIS + `bsp.c` を `common` オブジェクトライブラリに集約。shell コア/backend は `shell_obj`、CoreMark は `coremark_obj`（`-O3`）に分離し、`threadx` exe へまとめてリンク
- `stm32f7xx_hal_conf.h` は configure 時に upstream テンプレートから生成（既定 HSE 25 MHz がボードと一致）
- リンクは `-specs=nano.specs -specs=nosys.specs`。`threadx` は CoreMark の `%f` スコア用に `-u _printf_float` を付与

## クリーンアップ

```bash
rm -rf build      # ビルド成果物
rm -rf tools      # DL 済みツールチェーンも削除
```

## SWD デバッグ

- GDB サーバ: `st-util`（:4242）または `openocd -f interface/stlink.cfg -f target/stm32f7x.cfg`（:3333、SCS 読み出しが安定）
- GDB: システムの **`gdb-multiarch`**（toolchain 同梱 gdb は `libncursesw.so.5` 欠如で不可）
- VCP は `/dev/ttyACM0`（picocom 等が掴むと読み出しと競合）
