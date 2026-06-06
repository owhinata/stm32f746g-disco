---
name: codex-review
description: Codex による設計・実装レビュー。STM32F746 のペリフェラル設計、クロック/キャッシュ/割込み優先度、HAL/ThreadX 統合などハードウェア制約を伴う変更に使う。plan モードの計画レビューにも対応。
argument-hint: <plan | PR number | file path | design description>
---

# Codex 設計・実装レビュー (STM32F746G-DISCO)

## 対象の判定

`$ARGUMENTS` の内容に応じてレビュー対象を決定する:

- **`plan`**: 現在の会話コンテキストにある実装計画をレビュー対象とする。使用予定のペリフェラル・クロック/メモリ構成・割込み優先度・依存する HAL/ThreadX API を抽出してから Codex に送る。**実装着手前のゲートとして機能する。**
- **PR 番号 / `git diff`**: diff と説明を取得してレビュー対象とする
- **ファイルパス**: 指定ファイルの内容をレビュー対象とする
- **その他テキスト**: 設計説明としてそのままレビュー対象とする

## レビュー実行手順

1. レビュー対象の情報を収集する（plan / diff / ファイル）
2. 必要に応じて `_ref/`（git 管理外）のリファレンスを参照する
   - `_ref/rm0385-*.pdf` — STM32F75x/F74x リファレンスマニュアル（レジスタ/ペリフェラル根拠）
   - `_ref/um1907-*.pdf` — Discovery キット UM（ボード配線・ピン・ソルダーブリッジ）
3. 以下の「3 面レビュー観点」に基づいてレビュープロンプトを構成する
4. `mcp__codex__codex` で Codex にレビューを依頼する
5. 結果を整理してユーザーに報告する
6. **plan レビューの場合**: 3 面すべてで問題なしなら「実装着手 OK」とし、`touch ~/.claude/.plan-codex-reviewed` で marker を更新する。問題ありなら具体的な修正提案を示し、**marker は更新しない**（BLOCKING/CONCERN を解消し再 review して LGTM に至ってから touch する）。
   - この marker は `ExitPlanMode` の PreToolUse gate（`.claude/settings.json`）が確認する。marker が無い/古い（2h 超）と ExitPlanMode は block される。**plan review を通さずに ExitPlanMode することは構造的に防がれる。**
   - trivial plan で skip する場合も、user 承認を得てから `touch ~/.claude/.plan-codex-reviewed` すれば gate を通過できる。

## 3 面レビュー観点

それぞれ**独立したチェック**として実施する。1 面が LGTM でも、他面が未確認なら全体 LGTM にしない。

### 観点 1: 設計レビュー

- アーキテクチャの妥当性、レイヤ分離、API 設計（`bsp.c` の共通化境界など）
- ST HAL の使い方・初期化順序が HAL の前提と整合しているか
- ThreadX 統合の正しさ（`tx_application_define`、スタックサイズ、`_tx_initialize_low_level`、tick 供給）
- エラーハンドリング、排他制御、エッジケース

### 観点 2: MCU 実機能レビュー (RM0385 / UM1907 照合)

**「API がコンパイルできる」≠「F746 で期待通り動く」**。レジスタ/能力の根拠を RM0385 で確認する。

確認する例:
- クロック構成: PLL 係数・VOS スケール・over-drive・**Flash wait states** が目標 SYSCLK と整合するか（216MHz は VOS1+OD+7WS）
- FPU / I-Cache / D-Cache の有効化順序、キャッシュコヒーレンシ（DMA 使用時）
- タイマの種別と能力（OPM/TRGO/PWM/DMA req/32-bit/CC チャネル数）が用途に合うか
- ピンの AF 番号が RM0385 の alternate function mapping と一致するか
- ボード固有のソルダーブリッジ依存（例: **PA9 は VCP_TX と OTG_FS_VBUS の共用** — UM1907）

過去の事例（この repo の教訓）:
- **ThreadX SysTick 優先度**: SysTick を PendSV と同一優先度にすると、アイドル時の PendSV スピンを tick が割り込めず tick が停止 → スリープ中スレッドが起床しないデッドロック。**SysTick > PendSV**（14 vs 15）必須。
- **ITCM vs キャッシュ**: I/D-cache 有効時はホットコードを ITCM に移しても効果は ~0.6%（キャッシュが既に flash WS を隠蔽）。キャッシュ OFF か非常駐コードで初めて効く。

### 観点 3: HW リソース競合レビュー

該当するリソースを使う場合は現状の割当と照合する。

- **割込み優先度 (NVIC/SCB)**: Cortex-M7 の優先度ビット数（F746=4bit）。ThreadX 使用時は PendSV=最低、SysTick>PendSV。HAL ISR と ThreadX クリティカルセクション（PRIMASK/BASEPRI）の関係。
- **タイマ / DMA**: 使用ストリーム/チャネルの競合、DMA と D-Cache のコヒーレンシ。
- **GPIO / AF**: ピンの多重割当（VCP=PA9/PB7、LED LD1=PI1 など既存用途と衝突しないか）。
- **メモリ領域**: Flash(0x08000000) / ITCM(0x00000000) / DTCM(0x20000000) / SRAM の配置。リンカスクリプトと startup の symbol 整合（`_estack`, `_sidata` 等）。

## 成立性の証拠

HW 依存の設計には、LGTM 前に成立性の証拠を要求する:
- RM0385 のレジスタ記述に基づく根拠（節番号まで）
- 最小実機テスト or SWD(GDB/OpenOCD) での観測（例: `_tx_timer_system_clock` が進むか）
- 「コンパイルが通った」は証拠にならない

## Codex へのプロンプト構成

1. レビュー対象のコード/設計の説明
2. 該当する 3 面観点を明示的に指示
3. プロジェクトコンテキスト（MCU: STM32F746NGH6 / Cortex-M7、ベアメタル + Eclipse ThreadX、ST HAL、CMake+Ninja、216MHz/FPU/I$/D$）
4. 関連するリファレンス（RM0385 節、UM1907 配線）
5. 「LGTM を出す場合は該当面すべてについて根拠を示すこと」という指示

## `mcp__codex__codex` 呼び出しパラメータ

レビュー目的の Codex 呼び出しは以下を既定にする（ユーザー承認済み・spike-nx 踏襲）:

```
sandbox: "danger-full-access"
approval-policy: "never"
cwd: "/home/ouwa/work/stm32f746g-disco"   (絶対パス)
```

### 理由

- `sandbox: "read-only"` / `"workspace-write"` は環境によって bwrap loopback で失敗し（`bwrap: loopback: Failed RTM_NEWADDR`）、Codex のローカル shell が起動せず「推測レビュー」になる。
- `danger-full-access` は bwrap を経由しないので Codex が `Read` / `git diff` / `grep` でローカルファイルを参照できる。レビュー目的なら破壊操作は提案されないため実害なし。
- 避けたい場合は plan + diff を **prompt に inline で貼り付ける** fallback も可（精度はやや落ちる）。
