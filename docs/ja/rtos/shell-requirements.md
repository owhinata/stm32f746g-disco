<!--
SPDX-License-Identifier: MIT
Copyright (c) 2026 ThreadX Shell Project
-->

# ThreadX Shell (CLI) 要件定義

STM32F746G-DISCO / Eclipse ThreadX 上で動作する、対話型コマンドラインシェル（CLI）の要件定義。
Zephyr RTOS の shell サブシステム（`_ref/zephyr`, v4.4.99）の**設計を参考にした clean-room 実装**であり、Zephyr のコードは一切流用しない。

- ステータス: **確定**（codex 要件レビュー LGTM v0.2 / 実装プラン未着手）
- 最終更新: 2026-06-06
- 関連: 親 Issue（Epic）#1

> **用語**: コンポーネント名・ディレクトリ名は **Shell**（`shell/`）、C 言語の API prefix は **`cli_` / `CLI_`**。本書では「shell インスタンス」「Shell コア」のように機能を指す語として Shell を、識別子の接頭辞として cli を用いる。

---

## 1. 目的・ゴール

ベアメタル + ThreadX のファームウェアに、開発・デバッグ・運用のための**実用的な対話型 CLI** を追加する。
コマンドは各ソースファイルから宣言的に登録でき、**複数の通信路（backend）で同時にシェルを稼働**できることを必須とする。

## 2. スコープ（標準 tier）

| 分類 | 機能 |
|---|---|
| 行編集 | カーソル左右 / Home / End / 単語単位移動、文字挿入・上書き（insert mode）、文字削除、Backspace、カーソル以降削除、単語削除、行クリア、端末幅での折り返し再描画 |
| 端末制御 | VT100 エスケープ解析（矢印 / Home / End / Delete / Insert）、色出力（error=赤 / warn=黄 / info=緑）、Backspace mode（0x08 / 0x7F）切替 |
| メタキー | Ctrl+a/b/c/d/e/f/k/l/n/p/u/w, Alt+b/f |
| 入力支援 | コマンド履歴（↑↓ / Ctrl+p,n、固定リングバッファ、直前重複排除）、Tab 補完（共通プレフィックス補完 + 候補一覧）、Ctrl+c による入力行キャンセル |
| コマンド | static サブコマンド木、argc/argv 検証（mandatory / optional / RAW）、コマンド戻り値 |
| 出力 API | `cli_print` / `cli_error` / `cli_warn` / `cli_info`、`cli_hexdump`、出力バッファリング + autoflush |
| 組込みコマンド | `help` `clear` `history` `backends` `version` `uptime` `reboot` `thread`(list/stacks/...) `devmem`(peek/poke/dump) |

## 3. 非ゴール（今回除外・将来拡張で対応）

将来追加できるよう拡張性は確保するが、初回実装では作らない:

- ワイルドカード展開（`*` `?`）、エイリアス（aliases）、`select` による root 切替、動的（dynamic）サブコマンド
- ログ backend 統合、ネットワーク backend（telnet / mqtt / websocket / ssh）、remote shell、getopt 連携
- UTF-8 / マルチバイト入力（ASCII のみ対応, → §13）
- 認証・ログイン（パスワードプロンプト）

## 4. アーキテクチャ要件

### 4.1 コア / backend 分離
- コアは backend に対し **`init` / `uninit` / `enable` / `write` / `read` / `update`** と **RX/TX イベント**のみを要求し、ハードウェアに非依存とする。
- backend はリングバッファ等の実装を自由に選べる。

### 4.2 マルチインスタンス（必須）
- **1 transport = 1 shell インスタンス**。各インスタンスは独立した **スレッド / イベントフラグ / mutex / 行バッファ / 履歴 / プロンプト / 端末状態**を持ち、**複数同時に稼働**できる（例: VCP & Ethernet & Bluetooth）。
- コマンドツリーはリンカセクション上の **read-only 共有データ**として全インスタンスが共通参照する。
- 並行性・出力宛先・競合の詳細要件は §10 を参照。

### 4.3 コマンド登録（リンカセクション方式）
- コマンドエントリを専用セクション `.shell_root_cmds` に集約し、起動時に走査する。
- 登録マクロ `CLI_CMD_REGISTER(name, subcmd, help, handler, mandatory, optional)` 等により、`#ifdef` なしでファイル横断登録できる。
- リンカスクリプトに `KEEP` + 整列 + `__cli_root_cmds_start` / `__cli_root_cmds_end` シンボルを追加する。

### 4.4 ThreadX プリミティブ対応

| 抽象 | ThreadX |
|---|---|
| インスタンス毎スレッド | `tx_thread_create` |
| RX / TX / KILL シグナル | `tx_event_flags_create` |
| ロック / TX 排他（timeout 付） | `tx_mutex_create` |
| 履歴メモリ | 固定リング配列（動的確保なし。`tx_byte_pool` は不採用） |
| 時間待ち | ThreadX tick（1 ms） |

## 5. 命名規約

- 関数 / 型: `cli_*`、マクロ / 定数: `CLI_*`、公開ヘッダ: `shell/include/cli.h`
- 主要型: `struct cli_instance`, `struct cli_transport`, `struct cli_transport_api`
- 例: `cli_print(sh, "...")`, `CLI_CMD_REGISTER(...)`

## 6. ディレクトリ構成

```
shell/                              # 第一級コンポーネント (MIT, RTOSアプリ非依存)
├── include/cli.h                   # 公開API（コマンド定義 / print / インスタンス定義マクロ）
├── core/                           # コア実装
│   ├── cli_core.c                  # スレッド / 状態機械 / dispatch
│   ├── cli_edit.c                  # 行編集・VT100・メタキー
│   ├── cli_history.c               # 履歴（固定リング）
│   ├── cli_complete.c              # Tab 補完
│   ├── cli_parse.c                 # トークン分割 / コマンドツリー探索
│   ├── cli_printf.c                # 出力バッファリング
│   ├── cli_vt100.h                 # VT100 エスケープコード
│   └── cli_internal.h
├── backend/
│   ├── cli_backend_uart_stm32.c    # USART1 VCP（IRQ 駆動）— 初期実装
│   ├── cli_backend_dummy.c         # テスト / loopback 用
│   └── （将来）cli_backend_net.c / cli_backend_bt.c
├── cmds/
│   ├── cmd_system.c                # version, uptime, reboot
│   ├── cmd_thread.c                # thread list/stacks/...
│   └── cmd_devmem.c                # devmem peek/poke/dump
└── port/
    └── cli_port_threadx.c          # ThreadX プリミティブ薄ラッパ（host test も視野・任意）

src/app_shell.c                     # VCP + dummy インスタンス定義 + tx_application_define
ldscript/STM32F746NGHx_FLASH.ld     # .shell_root_cmds セクション追加
```

## 7. 既存コードへの変更点

1. **リンカ** (`ldscript/STM32F746NGHx_FLASH.ld`): `.shell_root_cmds`（`KEEP` + `ALIGN` + `__cli_root_cmds_start/end`）を `.rodata` 後に追加。
2. **UART**: USART1 を **IRQ 駆動 RX/TX リングバッファ**化（HAL ベース）。`USART1_IRQHandler` 新設、`NVIC` で `USART1_IRQn` 有効化。
   - 現状は polling 送信のみ（`_write` = `HAL_UART_Transmit` ブロッキング、RX なし）。
3. **printf 共存**: 他スレッドの `printf`（ブロッキング）と shell 出力の競合方式を設計（候補: `_write` を指定インスタンスへ集約 / 別系統のまま許容、→ §10）。
4. **CMake**: `shell` を OBJECT/STATIC ライブラリ化。新規 `shell` アプリ + `flash-shell` ターゲット追加。既存 demo（threadx / coremark / thread_metric / exec_profile）は温存。

## 8. 構成パラメータ・リソース上限（コンパイル時定義・上書き可）

| パラメータ | 既定 | 超過時の扱い |
|---|---|---|
| コマンドバッファ長 | 256 B | これ以上の入力文字は受理せずベル（BEL）を鳴らし無視 |
| 最大引数数 (argc) | 20 | 超過分はエラー表示しコマンド不実行 |
| 履歴リング | 512 B | 最古エントリから破棄（FIFO） |
| printf バッファ | 32 B | 満杯で flush（§11） |
| プロンプトバッファ | 20 B | 切り詰め |
| インスタンス毎スタック | 2048 B | — |
| 最大 shell インスタンス数 | 4 | コンパイル時に確定。超過はビルド時エラー |
| 最大サブコマンドネスト深さ | 8 | 超過はエラー表示しコマンド不実行 |
| 最大 Tab 補完候補表示数 | 端末幅依存（追加メモリ確保なし。候補走査はコマンド木を線形走査し都度表示） | — |

- 登録コマンド数はリンカセクション容量に依存（実質無制限・走査は線形）。
- すべて静的確保。実行時のヒープ確保は行わない。

## 9. エラー処理・異常系方針

- **基本原則（fail-safe）**: いかなる異常でも shell インスタンスは停止せず、エラーを表示してプロンプトへ復帰する。1 インスタンスの異常が他インスタンスへ波及しない。
- **未知コマンド**: `<cmd>: command not found` を表示し戻り値非 0。
- **引数不正 / 個数不一致**: usage（help）またはエラーを表示し、ハンドラは呼ばない。戻り値非 0。
- **コマンド戻り値**: 直近コマンドの戻り値を保持（`retval` 相当は将来拡張、標準では非 0 をエラー表示に使用）。
- **入力行長超過**: §8 の通りベル + 以降の文字を無視。
- **RX overflow**（受信リング満杯）: 新規受信バイトを破棄し、ドロップ数を内部統計に記録（任意で warn 表示）。コア/他スレッドはブロックしない。
- **TX overflow / フロー制御**: §11。
- **backend init 失敗**: 当該インスタンスのみ無効化し、他インスタンス・システムは継続。失敗を起動ログに記録。
- **コマンド実行のキャンセル**: 入力行は Ctrl+c でキャンセル可能（実行中ハンドラの強制中断は標準スコープ外）。

## 10. 並行性・スレッド安全・マルチインスタンス競合

- **状態分離**: 行バッファ・履歴・端末状態・プロンプトはインスタンス毎に独立。コアの可変グローバル状態は持たない（コマンドツリーは read-only 共有）。
- **出力宛先**: 各インスタンスの出力は**自身の transport にのみ**送られ、インスタンス間で出力が混在しない。
- **ハンドラ再入性**: 複数インスタンスが**同一コマンドを同時実行し得る**。コマンドハンドラは再入される前提で書くこと。ハンドラ内で共有ハードウェア/共有状態を操作する場合は、**ハンドラ側の責務で排他**する（コアは保証しない）。組込み標準コマンドは再入安全に実装する。
- **全体影響コマンド**（`reboot` 等）: 即時実行。同時要求時の整合は問わない（最初に到達した実行が系をリセットする）。
- **他スレッド printf との共存**: 既定では他スレッドの `printf`（ブロッキング `_write`）と shell 出力は**別系統**とし、同一 UART を共有する VCP インスタンスでは出力が時間的に混在し得ることを許容する（既知の制約として明記）。任意で `_write` を特定インスタンスのスレッドセーフ出力へ集約する構成を選べる（将来拡張）。
- **ロック粒度**: インスタンス毎の TX/状態は mutex（timeout 付）で保護。ISR からコアへの通知はイベントフラグのみ（ISR 内でロックを取らない）。

## 11. フロー制御・バックプレッシャ

- **既定動作**: 出力は当該インスタンスのスレッドコンテキストで行い、backend TX バッファ満杯時は **TX 完了を待つブロッキング送信**で律速する（取りこぼし・破棄を起こさない）。
- **タイムアウト**: TX 待ちにコンパイル時タイムアウトを設定可能（既定値は実装プランで決定、`0` = 無限待ち）。**タイムアウト時の挙動は一意に定める**: 当該 shell インスタンスは停止せず継続し、残り出力を破棄して drop 統計を加算し、当該出力 API（`cli_print` 等）は失敗（非 0 / 負）を返し、**実行中コマンドは非 0 終了**とする。
- **大量出力コマンド**（`thread stacks`, `devmem dump` 等）も同方式。固定の最大出力量上限は設けず、ブロッキングで自然に律速する。
- **入力受付**: 出力ブロッキング中も RX は IRQ + 受信リングで取りこぼさない（バッファ溢れ時のみ §9 の RX overflow）。

## 12. 安全性・危険コマンド方針

- `reboot` と `devmem`（特に `poke`）は**開発・デバッグ用途**の機能であり、以下を要件とする:
  - **コンパイル時ゲート**: マクロ（例 `CLI_ENABLE_DANGEROUS_CMDS`）で一括無効化できる。**shell デモアプリでは既定 ON**、本番組込み構成では**既定 OFF を推奨**。
  - **devmem アドレス範囲**: 既定は開発前提で制限しない。コンパイル時に許可/禁止アドレス範囲を設定でき、範囲外アクセスはエラー表示で拒否できる。アライメント/幅（8/16/32）違反はエラー。
  - **reboot**: 即時実行（確認プロンプトは標準スコープ外）。help にて影響を明記。
- 認証・権限分離は非ゴール（§3）。危険コマンドの管理は上記コンパイル時ゲートで行う。

## 13. 入力文字集合・エンコーディング

- 受理する入力は **ASCII（0x00–0x7F）** のみ。表示可能文字は 0x20–0x7E。
- 非 ASCII バイト（0x80–0xFF）は既定で**破棄**（フィルタ）し、コマンドバッファへ渡さない。
- 未対応・不正なエスケープシーケンスは**無視**して通常状態へ復帰する（エラーにしない）。
- UTF-8 / マルチバイトは非ゴール（§3）。

## 14. 起動・停止・初期化順序

1. `bsp_init()`: クロック(216MHz) / キャッシュ / GPIO・UART ピン初期化。
2. `tx_kernel_enter()` → `tx_application_define()`。
3. 各インスタンスについて `cli_init()`: コマンドセクション（`.shell_root_cmds`）走査、インスタンス状態初期化、backend `init`。
4. backend `enable` → スレッド起動 → プロンプト表示。
5. **失敗時**: 当該インスタンスをスキップし他を継続（§9）。
- `uninit` / KILL（インスタンス停止・破棄）は API として用意するが、**標準スコープでは init/start のみを必須**とし、stop/uninit の完全なライフサイクルは将来拡張で検証する。

## 15. 性能・応答性要件（216 MHz / 115200 8N1 前提・検証可能な目標）

- **エコー遅延**: 1 文字入力からエコー表示まで **< 5 ms**（代表目標。IRQ→スレッド起床→エコー）。
- **操作応答**: Tab 補完・履歴呼び出し・通常コマンドのプロンプト応答 **< 50 ms**（代表目標）。
- **入力取りこぼし**: 通常操作入力、および**受信リング容量以内の連続入力**では取りこぼしゼロ（受信リングで担保）。大量出力中・複数インスタンス同時稼働中も同様。**リング容量を超える過負荷時**は取りこぼしゼロ保証の対象外で、§9 の RX overflow（破棄 + drop 統計）として扱う。
- 上記代表値は実装プランで最終確定し、受け入れは §18 で判定。

## 16. ライセンス / clean-room 方針

- Zephyr（Apache-2.0）の**ソースコードは一切流用しない**。参照してよいのは公開ドキュメント・設計概念・公開 API の振る舞い理解までとし、コード断片・コメント・固有の文言のコピーは禁止。
- 新規 shell ファイルは全て **MIT**、`SPDX-License-Identifier: MIT`、`Copyright (c) 2026 ThreadX Shell Project`。
- **証跡（必須）**: `NOTICE` または README に「設計は Zephyr RTOS shell を参考にした clean-room 実装」である旨を**必須記載**する。レビュー時に「Zephyr からのコード/文言流用がないこと」を確認観点に含める。

## 17. ドキュメント

- 実装フェーズで `docs/ja` / `docs/en` に同名 `.md`（日英必須）を追加（mkdocs RTOS カテゴリ）。
- 内容: アーキテクチャ図、コマンド定義方法、backend 追加手順、ピン / UART 前提、危険コマンドのゲート方法。

## 18. 受け入れ基準（Acceptance Criteria）

実装完了の判定は以下の pass/fail で行う:

1. **基本対話**: VCP に端末接続でプロンプト表示。`help` が登録コマンド一覧を表示する。
2. **行編集**: カーソル移動・挿入/削除・Backspace・単語削除・行クリアが端末上で正しく反映される。
3. **履歴**: ↑↓ / Ctrl+p,n で過去コマンドを呼び出せる。直前重複は記録されない。
4. **補完**: Tab で単一候補は自動補完、複数候補は共通プレフィックス補完 + 一覧表示。
5. **色 / メタキー**: error が赤等で表示。Ctrl+a/e/u/w 等が機能する。
6. **組込みコマンド**: `version` `uptime` `reboot` `thread list/stacks` `devmem`（peek/poke/dump）が期待動作する。
7. **異常系**: 未知コマンド・引数不正・行長超過で shell が落ちずプロンプトへ復帰し、適切なエラーを表示する（§9）。
8. **マルチインスタンス**: **VCP と dummy の 2 インスタンスが同時稼働**し、各々独立に入力/出力でき、出力が混在しない。
9. **取りこぼしゼロ**: 大量出力中（例 `thread stacks` 連打）でも、受信リング容量以内の通常操作入力を取りこぼさない（過負荷時は §9 RX overflow として許容、§15）。
10. **危険コマンドゲート**: `CLI_ENABLE_DANGEROUS_CMDS` を OFF にしたビルドで `reboot` / `devmem` が無効化される。
11. **自動テスト**: dummy backend 経由で、コマンド入力 → 出力検証の自動テストが少なくとも基本系・異常系で通る。
12. **ビルド**: `flash-shell` ターゲットがビルドでき、既存 demo（threadx 等）のビルドを壊さない。

## 19. 検証方法

- 115200 8N1 の端末（例: picocom）で `/dev/ttyACM0` に接続し §18 を確認。
- dummy backend を用いた自動テストでコア（パーサ・編集・補完・履歴・異常系）を検証。
- CLAUDE.md 規約に従い、実装プラン確定前と実装後に `codex-review`（設計 / MCU 実機能 RM0385 照合 / HW リソース競合）を実施し、実機 verify 後に commit。

## 20. 作業分解（親 Issue のサブタスク候補）

1. リンカセクション基盤（`.shell_root_cmds` + start/end シンボル）
2. コア: インスタンス / スレッド / 状態機械 / dispatch（ThreadX プリミティブ）
3. パーサ: トークン分割・コマンドツリー探索・argc/argv 検証
4. コマンド登録マクロ（`CLI_CMD_REGISTER` / サブコマンド / SUBCMD_SET）
5. 行編集・VT100・メタキー・色
6. 履歴（固定リング）
7. Tab 補完
8. 出力 API（`cli_print/error/warn/info`, hexdump, バッファリング, フロー制御 §11）
9. transport 抽象 + UART(VCP) backend（USART1 IRQ 駆動, IRQ handler, NVIC）
10. dummy backend + マルチインスタンス実証 + 自動テスト基盤
11. 組込みコマンド: version / uptime / reboot（危険コマンドゲート §12）
12. 組込みコマンド: thread (list / stacks / ...)
13. 組込みコマンド: devmem (peek / poke / dump、アドレス範囲ゲート §12)
14. アプリ統合: `src/app_shell.c` + CMake `shell` ライブラリ + `flash-shell`
15. ドキュメント `docs/ja` / `docs/en` + NOTICE/README（clean-room 証跡 §16）
16. codex-review（プラン前 / 実装後）+ 実機 verify
