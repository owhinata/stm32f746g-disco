<!--
SPDX-License-Identifier: MIT
Copyright (c) 2026 ThreadX Shell Project
-->

# レイヤ構成と依存方向

本ファームのソースは、依存が一方向（下位 → 上位を知らない）になるよう層に分かれている。issue #43 で freestanding な共通サービス層 `svc/` を新設し、ドライバ層 `port/` が上位の `src/` / `include/` を include する逆流を解消した。issue #61 で GUIX の presentation（カメラ UI アプリ）を `port/` から分離した presentation 層 `ui/` を新設した。

```
HAL / CMSIS / ThreadX   ←   svc/   ←   port/   ←   ui/   ←   shell/ , src/
        (lib/)            (共通)     (ドライバ)    (UI)        (アプリ)
```

矢印は「左が右に依存される」向き。`svc/` は `port/` を知らず、`port/` は `ui/`・`shell/`・`src/` を知らず、`ui/` は `shell/`・`src/` を知らない。

## 各ディレクトリの役割

| ディレクトリ | 役割 | 依存してよい先 |
|---|---|---|
| `lib/` | upstream submodule（ST HAL / CMSIS / ThreadX / FileX / LevelX / OV5640 / CoreMark）。編集しない。 | — |
| `svc/` | freestanding な共通サービス。`fmt`（clean-room printf）、`log`（DTCM RAM ログ）、`timebase`（TIM2 free-run + `udelay`）。 | HAL / CMSIS |
| `port/` | ペリフェラルドライバのグルー（qspi / sd / sdram / camera / ltdc / touch / guix / filex / levelx / threadx）。 | `svc/`, HAL / CMSIS / ThreadX / GUIX |
| `ui/` | GUIX presentation（カメラ UI アプリ: widget tree + frame sink + preview 制御、`guix_camera_ui`）。 | `svc/`, `port/`, GUIX / ThreadX |
| `shell/` | CLI シェル（core / backend / cmds）。 | `svc/`, `port/`, `ui/`, ThreadX |
| `src/` | アプリ起動（`main` / `bsp` / `fault` / `iwdg`）。 | `svc/`, `port/`, `ui/`, `shell/`, HAL |
| `include/` | アプリ層の公開ヘッダ（`bsp.h` / `main.h` / `iwdg.h`）。 | — |

## svc/ レイヤ（issue #43）

`svc/` は「HAL/CMSIS 以外の上位層に一切依存しない」共通機能だけを持つ。これにより `port/` のドライバが `log` や `udelay` を `svc/` から取得でき、`shell/` や `src/` のヘッダへ逆流しなくなる。

- `svc/fmt.{h,c}` — putc コールバックを sink にする最小 printf フォーマッタ。シェル出力 API・RAM ログ・fault ダンプが共有。依存は `<stdarg.h>` / `<stddef.h>` のみ。
- `svc/log.{h,c}` — DTCM の `.log_noinit` 上に置くリセット永続リングバッファ。`dmesg` で再生。詳細は[ロギング](../rtos/logging.md)。
- `svc/timebase.{h,c}` — TIM2 を 108 MHz free-run の 32-bit カウンタとして起動（ThreadX 実行プロファイル用、issue #19）し、`udelay()` のビジーウェイト源にもなる。

## ui/ レイヤ（issue #61）

`ui/` は GUIX の presentation（UI 構成）だけを持つ。`port/guix`（display ドライバ / 入力 / lifecycle）と `port/camera` の上に、画面とユーザー操作を組み立てる。

- `ui/guix_camera_ui.{h,c}` — カメラライブプレビュー専用の GUIX アプリ。手書きの widget tree（単一 camera 画面 + `GX_ICON`）、`frame_pipeline` の push sink、プレビュー lifecycle（`camera_ui_init`/`start`/`stop`）を 1 つにまとめる。#55 の `guix_app`（デモ UI）と #56 の `guix_camera`（sink + 制御）を統合・移設したもの。

**依存逆転**: `port/guix/guix_glue` は GUIX 本体の bring-up を担うが、UI の widget tree 構築は `ui/` の仕事。`port → ui` の逆流を避けるため、`ui/` が widget builder を `guix_register_app_builder()` で**コールバック登録**し、`guix_glue` は登録された関数ポインタ（引数は `void*`）を呼ぶ。これで `guix_glue` は `ui/` のヘッダを include しない。`guix_display`/`guix_touch`/`guix_glue`/`gx_user.h` は driver/integration/config として `port/guix/` に残る（特に DMA2D の B2 copy-forward など LTDC 内部に密結合する部分）。

## CMake への反映

`svc/` は OBJECT ライブラリ `svc_obj`（`svc/fmt.c` / `svc/log.c` / `svc/timebase.c`）として最終 exe にリンクされる。`svc/` の include パスは `bsp_iface` INTERFACE に載るため、全ターゲットから `fmt.h` / `log.h` / `timebase.h` に到達できる。

`ui/guix_camera_ui.c` は `port/` のグルー群と同じく `threadx` ターゲットへ直接列挙し、`ui/` を include パスに追加する（OBJECT ライブラリ化はしない）。
