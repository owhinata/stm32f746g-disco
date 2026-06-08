# Build (CMake)

The build is **CMake + Ninja**. The first configure auto-downloads the ARM GNU
toolchain into `./tools` and inits the git submodules.

## Prerequisites

`cmake`, `ninja`, `git`, `curl`, and `st-flash` (for flashing).

## configure / build

```bash
cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-toolchain.cmake
cmake --build build            # builds the single target, threadx
```

Artifacts: `build/threadx.{elf,hex,bin}`. `threadx` is the interactive ThreadX
shell and the **only firmware**; CoreMark is not a separate image but the shell's
`coremark` command (see [CoreMark command](../rtos/shell-coremark.md)).

## flash

```bash
cmake --build build --target flash     # write over ST-Link
```

`flash` runs `st-flash --connect-under-reset --reset write threadx.bin 0x08000000`.
`--connect-under-reset` is required since #20 (`TX_ENABLE_WFI`): the running firmware sleeps
in WFI when idle, and the old onboard ST-Link cannot attach to a sleeping core without holding
it in reset (#24).

## Build structure

- `cmake/arm-none-eabi-toolchain.cmake`: toolchain auto-download (fetched if
  `tools/.../arm-none-eabi-gcc` is missing)
- `CMakeLists.txt`: HAL + CMSIS + `bsp.c` collected into a `common` object
  library; the shell core/backends are a `shell_obj` object library and CoreMark
  a `coremark_obj` one (`-O3`), all linked into the `threadx` exe
- `stm32f7xx_hal_conf.h` is generated at configure time from the upstream
  template (default HSE 25 MHz matches the board)
- Linked with `-specs=nano.specs -specs=nosys.specs`; `threadx` adds
  `-u _printf_float` for CoreMark's `%f` score line

## Cleanup

```bash
rm -rf build      # build artifacts
rm -rf tools      # also remove the downloaded toolchain
```

## SWD debugging

- GDB server: `st-util` (:4242) or `openocd -f interface/stlink.cfg -f target/stm32f7x.cfg` (:3333, more reliable for SCS reads)
- GDB: the system **`gdb-multiarch`** (the bundled gdb fails on missing `libncursesw.so.5`)
- VCP is `/dev/ttyACM0` (a terminal like picocom holding it conflicts with reads)
