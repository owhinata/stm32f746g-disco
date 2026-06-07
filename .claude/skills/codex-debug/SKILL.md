---
name: codex-debug
description: Codex による不具合解析。根本原因の仮説列挙、検証方法の提案、関連コード特定を依頼する。症状・再現手順・観測結果を入力として受け取る。STM32F746/ベアメタル/ThreadX の組込み不具合向け。
argument-hint: <Issue number, symptom description, or file path>
---

# Codex 不具合解析 (STM32F746G-DISCO)

## 対象の判定

`$ARGUMENTS` の内容に応じて解析対象を決定する:

- **Issue 番号** (例: `#12`, `12`): `gh issue view` で症状・再現手順・観測結果を取得
- **テキスト**: 症状の説明としてそのまま解析対象とする
- **ファイルパス**: 問題が発生しているコードを起点に解析

## 解析実行手順

1. 不具合の情報を収集する
   - 症状、再現手順、観測結果（VCP ログ、LED 挙動、SWD で読んだレジスタ/変数）
   - 関連するソースコード、`build/<app>.map`、直近の変更（`git log`, `git diff`）
2. 問題の層を特定する（後述「層の切り分け」）
3. 収集情報と以下の「解析観点」を Codex プロンプトに構成する
4. `mcp__codex__codex` で Codex に解析を依頼する
5. 結果を整理してユーザーに報告する

## 解析観点

### 1. 根本原因の仮説列挙

Codex に以下を求める:

- **複数の仮説を確度順に列挙** — 1 つに絞らない
- **各仮説の根拠コード箇所を特定** — ファイルパスと行番号
- **各仮説の検証方法を提案** — 最小実験 / SWD 観測 / ログ追加で切り分けられる手順

### 2. 層の切り分け

| 層 | 例 |
|---|---|
| HW / 電気的 | 電源、クロック源（HSE 25MHz）、信号品質、ソルダーブリッジ構成 |
| MCU ペリフェラル | レジスタ設定ミス、クロック/PLL/Flash WS、キャッシュコヒーレンシ、割込み優先度 |
| CMSIS / startup | ベクタテーブル、`SystemInit`、`_estack`/`.data`/`.bss` 初期化、リンカスクリプト |
| ST HAL | HAL 初期化順序、`HAL_GetTick`/timebase、周辺ドライバ |
| RTOS (ThreadX) | スケジューラ/PendSV、tick 供給、優先度、スタックオーバーフロー、クリティカルセクション |
| アプリ | `src/app_*.c`、`port/*` の統合コード |

### 3. HW / 割込み起因を必ず検討する

組込み特有の問題は SW だけでは説明できないことが多い。Codex に必ず検討させる:

- 割込み優先度・プリエンプション関係（特に ThreadX: **SysTick > PendSV** か、HAL ISR との関係）
- クリティカルセクション（PRIMASK/BASEPRI）で tick やイベントが落ちていないか
- キャッシュ（D-Cache）と DMA/共有メモリのコヒーレンシ
- Flash wait states / over-drive / VOS とクロックの不整合
- HardFault の有無（`CFSR`/`HFSR`/`BFAR`、スタックフレーム）。ハンドラ未定義で `Default_Handler` の無限ループに落ちていないか
- スタックサイズ不足（ThreadX スレッドスタック、MSP）
- ST 公式デモ（`_ref/STM32Cube_FW_F7_V1.17.0/Projects/STM32746G-Discovery/Demonstrations/STemWin/`）の同等ペリフェラル/クロック/キャッシュ/BSP 設定と突き合わせ（出荷時 `backup_full.bin` の参照ソース、read-only）

### 4. 観測の確実性

- 確定的に再現するか、間欠的（flaky）か
- VCP 出力が途中で止まる場合、ハングか HardFault か tick 停止かを SWD で切り分ける
  （例: `_tx_timer_system_clock` が進むか、`$pc` が `Default_Handler`/`__tx_ts_wait` か）

## SWD デバッグ補助（この環境固有）

- toolchain 同梱の `arm-none-eabi-gdb` は `libncursesw.so.5` 欠如で動かない → システムの **`gdb-multiarch`** を使う
- GDB サーバは `st-util`（:4242）か **OpenOCD**（`openocd -f interface/stlink.cfg -f target/stm32f7x.cfg`、:3333）。OpenOCD の方が SCS レジスタ読み出しが安定
- VCP は `/dev/ttyACM0`（picocom 等が掴むと `st-flash`/読み出しと競合し文字化けする）。SWD と VCP は別系統

## Codex へのプロンプト構成

1. 症状の正確な記述（エラー/ログ、期待値 vs 実際値、LED/VCP 挙動）
2. 再現手順と再現率
3. 関連ソースコード（問題箇所の周辺）と直近 diff
4. プロジェクトコンテキスト（MCU: STM32F746NGH6 / Cortex-M7、ベアメタル + Eclipse ThreadX、ST HAL、CMake+Ninja、216MHz/FPU/I$/D$、VCP=USART1 PA9/PB7）
5. 該当する解析観点を明示
6. 「仮説は確度順に並べ、各仮説に検証方法を付けること」という指示

## `mcp__codex__codex` 呼び出しパラメータ

```
sandbox: "danger-full-access"
approval-policy: "never"
cwd: "/home/ouwa/work/stm32f746g-disco"   (絶対パス)
```

理由は `codex-review` skill と同じ（bwrap loopback 回避でローカルファイルを参照させるため。ユーザー承認済み）。
