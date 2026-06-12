<!--
SPDX-License-Identifier: MIT
Copyright (c) 2026 ThreadX Shell Project
-->

# レイヤ構成と依存方向

本ファームのソースは、依存が一方向（下位 → 上位を知らない）になるよう層に分かれている。issue #43 で freestanding な共通サービス層 `svc/` を新設し、ドライバ層 `port/` が上位の `src/` / `include/` を include する逆流を解消した。

```
HAL / CMSIS / ThreadX   ←   svc/   ←   port/   ←   shell/ , src/
        (lib/)            (共通)     (ドライバ)      (アプリ)
```

矢印は「左が右に依存される」向き。`svc/` は `port/` を知らず、`port/` は `shell/` や `src/` を知らない。

## 各ディレクトリの役割

| ディレクトリ | 役割 | 依存してよい先 |
|---|---|---|
| `lib/` | upstream submodule（ST HAL / CMSIS / ThreadX / FileX / LevelX / OV5640 / CoreMark）。編集しない。 | — |
| `svc/` | freestanding な共通サービス。`fmt`（clean-room printf）、`log`（DTCM RAM ログ）、`timebase`（TIM2 free-run + `udelay`）。 | HAL / CMSIS |
| `port/` | ペリフェラルドライバのグルー（qspi / sd / sdram / camera / filex / levelx / threadx）。 | `svc/`, HAL / CMSIS / ThreadX |
| `shell/` | CLI シェル（core / backend / cmds）。 | `svc/`, `port/`, ThreadX |
| `src/` | アプリ起動（`main` / `bsp` / `fault` / `iwdg`）。 | `svc/`, `port/`, `shell/`, HAL |
| `include/` | アプリ層の公開ヘッダ（`bsp.h` / `main.h` / `iwdg.h`）。 | — |

## svc/ レイヤ（issue #43）

`svc/` は「HAL/CMSIS 以外の上位層に一切依存しない」共通機能だけを持つ。これにより `port/` のドライバが `log` や `udelay` を `svc/` から取得でき、`shell/` や `src/` のヘッダへ逆流しなくなる。

- `svc/fmt.{h,c}` — putc コールバックを sink にする最小 printf フォーマッタ。シェル出力 API・RAM ログ・fault ダンプが共有。依存は `<stdarg.h>` / `<stddef.h>` のみ。
- `svc/log.{h,c}` — DTCM の `.log_noinit` 上に置くリセット永続リングバッファ。`dmesg` で再生。詳細は[ロギング](../rtos/logging.md)。
- `svc/timebase.{h,c}` — TIM2 を 108 MHz free-run の 32-bit カウンタとして起動（ThreadX 実行プロファイル用、issue #19）し、`udelay()` のビジーウェイト源にもなる。

## CMake への反映

`svc/` は OBJECT ライブラリ `svc_obj`（`svc/fmt.c` / `svc/log.c` / `svc/timebase.c`）として最終 exe にリンクされる。`svc/` の include パスは `bsp_iface` INTERFACE に載るため、全ターゲットから `fmt.h` / `log.h` / `timebase.h` に到達できる。
