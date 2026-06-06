# Build (CMake)

The build is **CMake + Ninja**. The first configure auto-downloads the ARM GNU
toolchain into `./tools` and inits the git submodules.

## Prerequisites

`cmake`, `ninja`, `git`, `curl`, and `st-flash` (for flashing).

## configure / build

```bash
cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-toolchain.cmake
cmake --build build            # builds threadx
```

Artifacts: `build/threadx.{elf,hex,bin}`.

### Include CoreMark

```bash
cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-toolchain.cmake \
      -DBUILD_COREMARK=ON
cmake --build build            # threadx + coremark
```

`lib/coremark` (submodule) and `port/coremark/` are kept in the tree, so a single
option brings the app back.

## flash

```bash
cmake --build build --target flash-threadx     # ThreadX
cmake --build build --target flash-coremark    # CoreMark (needs -DBUILD_COREMARK=ON)
```

Each `flash-<app>` runs `st-flash --reset write <app>.bin 0x08000000`.

## Build structure

- `cmake/arm-none-eabi-toolchain.cmake`: toolchain auto-download (fetched if
  `tools/.../arm-none-eabi-gcc` is missing)
- `CMakeLists.txt`: HAL + CMSIS + `bsp.c` collected into a `common` object
  library shared by each app
- `stm32f7xx_hal_conf.h` is generated at configure time from the upstream
  template (default HSE 25 MHz matches the board)
- Linked with `-specs=nano.specs -specs=nosys.specs`; CoreMark also uses
  `-u _printf_float` (for `%f`)

## Cleanup

```bash
rm -rf build      # build artifacts
rm -rf tools      # also remove the downloaded toolchain
```

## SWD debugging

- GDB server: `st-util` (:4242) or `openocd -f interface/stlink.cfg -f target/stm32f7x.cfg` (:3333, more reliable for SCS reads)
- GDB: the system **`gdb-multiarch`** (the bundled gdb fails on missing `libncursesw.so.5`)
- VCP is `/dev/ttyACM0` (a terminal like picocom holding it conflicts with reads)
