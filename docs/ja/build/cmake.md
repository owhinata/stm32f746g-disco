# ビルド (CMake)

ビルドは **CMake + Ninja**。初回 configure で ARM GNU ツールチェーンを `./tools` に自動 DL し、git submodule も自動 init する。

## 前提ツール

`cmake`, `ninja`, `git`, `curl`、書き込みに `st-flash`。

## configure / build

```bash
cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-toolchain.cmake
cmake --build build            # threadx をビルド
```

成果物: `build/threadx.{elf,hex,bin}`。

### CoreMark を含める

```bash
cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-toolchain.cmake \
      -DBUILD_COREMARK=ON
cmake --build build            # threadx + coremark
```

`lib/coremark`（submodule）と `port/coremark/` はツリーに残してあるので、オプション 1 つで復活する。

## flash

```bash
cmake --build build --target flash-threadx     # ThreadX
cmake --build build --target flash-coremark    # CoreMark（-DBUILD_COREMARK=ON 必要）
```

各 `flash-<app>` は `st-flash --reset write <app>.bin 0x08000000` を実行。

## 構成のポイント

- `cmake/arm-none-eabi-toolchain.cmake`: ツールチェーン自動 DL（`tools/.../arm-none-eabi-gcc` が無ければ取得）
- `CMakeLists.txt`: HAL + CMSIS + `bsp.c` を `common` オブジェクトライブラリに集約し各アプリで共有
- `stm32f7xx_hal_conf.h` は configure 時に upstream テンプレートから生成（既定 HSE 25 MHz がボードと一致）
- リンクは `-specs=nano.specs -specs=nosys.specs`、CoreMark のみ `-u _printf_float`（`%f` 用）

## クリーンアップ

```bash
rm -rf build      # ビルド成果物
rm -rf tools      # DL 済みツールチェーンも削除
```

## SWD デバッグ

- GDB サーバ: `st-util`（:4242）または `openocd -f interface/stlink.cfg -f target/stm32f7x.cfg`（:3333、SCS 読み出しが安定）
- GDB: システムの **`gdb-multiarch`**（toolchain 同梱 gdb は `libncursesw.so.5` 欠如で不可）
- VCP は `/dev/ttyACM0`（picocom 等が掴むと読み出しと競合）
