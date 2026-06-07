# STM32F746G-DISCO プロジェクト

STM32F746G-DISCO（STM32F746NGH6 / Cortex-M7）向けの **ベアメタル + Eclipse ThreadX** ファームウェア。ST 公式 HAL、ビルドは CMake + Ninja。HAL/CMSIS/ThreadX/CoreMark のソースと ARM GNU ツールチェーンは初回 configure で自動取得。

## 開発ワークフロー

### コード修正サイクル

以下を小さく繰り返す:

1. **コード修正** — 機能実装 or バグ修正
2. **ビルド** — `cmake --build build`
3. **フラッシュ** — `cmake --build build --target flash`（ST-Link）
4. **動作確認** — ユーザーが実機で確認（VCP 出力 / LED / 必要なら SWD で観測）
5. **ドキュメント更新** — 変更に対応する `docs/ja` と `docs/en` を更新（日英必須）
6. **コミット** — 動作確認後にコミット

**動作確認前にコミットしない。ドキュメント更新を忘れない。**

### Plan + Codex review ワークフロー

**Phase 系 / architecture を変える plan は、plan 確定前と実装後の両方で codex-review を実施する。**

対象となる plan:
- 新規ペリフェラル採用、クロック/メモリ/割込み優先度の構造変更
- HAL/ThreadX 統合方針の変更、リンカスクリプト/起動フローの変更
- 複数レイヤ（HW + HAL + RTOS + アプリ）に跨る変更

ゲートのタイミング:

1. **Plan 確定前**（実装着手前）: plan を `codex-review` skill で 3 面（設計 / MCU 実機能(RM0385 照合) / HW リソース競合）review。BLOCKING / CONCERN を全解消してから `ExitPlanMode`。
2. **実装後**（commit 前）: branch の diff を `codex-review` で再 review。BLOCKING 解消 → user に実機 verify 依頼 → commit。

`codex-review` skill は `.claude/skills/codex-review/` で定義済（sandbox は `danger-full-access`、approval-policy は `never`、cwd は絶対パス）。

**ゲートの強制**: `ExitPlanMode` の PreToolUse hook（`.claude/settings.json`）が `~/.claude/.plan-codex-reviewed` marker を確認する。marker が無い/古い（2h 超）と block される。codex-review が LGTM に至ると marker を更新して通過。trivial plan で skip する場合も **user 承認を得てから** `touch ~/.claude/.plan-codex-reviewed`。

不具合解析には `codex-debug` skill を使う。

## Git ワークフロー

**PR は作らない。** Issue 駆動で feature/fix ブランチを切り、ローカル `main` に `--ff-only` merge → push → Issue へ対応コメント → Issue クローズ、の流れ。

- **ブランチ**: `feat/`, `fix/`, `docs/`, `build/`, `refactor/`, `chore/`, `style/` prefix。`<prefix>/<N>-short-description`（`<N>` は Issue 番号）。ベースは常に `main`
- **コミット**: conventional commits 形式 `type: short description`。Issue 対応時は `type: #N short description` で **subject に Issue 番号**を含める（GitHub のリンク生成＋オートクローズ判定のため）
- コミットメッセージ末尾に `Co-Authored-By: Claude ...` を付与
- 動作確認していない変更を commit / push しない

### 1. Issue 作成

着手前にまず Issue を立てる（バグ報告 / 機能追加 / リファクタ いずれも同様）。

```bash
gh issue create --repo owhinata/stm32f746g-disco --title "short description" --body "$(cat <<'EOF'
## Summary
- 症状・問題・やりたいことの説明

## Reproduction (バグの場合)
- 再現手順

## Environment
- Board: STM32F746G-DISCO (STM32F746NGH6 / Cortex-M7)
- Reproduced at: <commit-hash>
- 関連する CONFIG / ビルド設定

## Notes
- 調査メモ・仮説・設計案
EOF
)"
```

### 2. ブランチを切って実装 → 動作確認 → コミット

```bash
git checkout -b feat/<N>-short-description    # <N> は Issue 番号
# ... 実装 → ビルド → フラッシュ → 実機で動作確認 ...
git commit -m "type: #<N> short description"
```

### 3. ローカル main に ff-merge

```bash
git checkout main
git merge --ff-only feat/<N>-short-description
```

`--ff-only` で必ず early return する。merge コミットは作らない。

### 4. push

```bash
git push origin main
```

push 時、コミットメッセージに `fix: #<N> ...` / `closes #<N>` 等の GitHub オートクローズキーワードが含まれていれば、対応する Issue は自動的に CLOSED になる。

### 5. Issue へ対応コメント

push 後、`gh issue comment` で対応サマリを残す。コミットレンジ（`<base>..<head>`）を必ず含める。

```bash
gh issue comment <N> --repo owhinata/stm32f746g-disco --body "$(cat <<'EOF'
## 対応完了

`feat/<N>-...` を `main` に ff-merge して push (`<base>..<head>`)。

### 修正内容
- 変更点を箇条書き

### 動作確認
- 実機で確認した挙動

### 補足 (任意)
- 関連 Issue / 後続フォローアップ
EOF
)"
```

### 6. Issue クローズ + ブランチ削除

オートクローズで CLOSED になっていなければ手動で閉じる。マージ済みブランチは削除する。

```bash
gh issue close <N> --repo owhinata/stm32f746g-disco          # 必要なら
git branch -d feat/<N>-short-description
```

### 注意

- **PR は作らない**。`gh pr create` / `gh pr merge` は使わない
- `main` への直 push なので `--ff-only` を厳守、**force push は禁止**
- 動作確認していないコミットを `main` に push しない
- Issue を立てずにコミットしない（`#<N>` 参照が無いコミットは追跡できない）。ただし Epic / 親 Issue は、クローズキーワードを使わず `#<epic>` 参照のリンクのみとする（誤クローズ防止）

### Upstream submodule は read-only

以下は upstream のミラー submodule。`gh` での書き込み操作（PR/issue/comment 等）を行ってはならない（PreToolUse hook がブロックする）:

- `STMicroelectronics/stm32f7xx_hal_driver`, `cmsis_device_f7`, `cmsis-core`
- `eclipse-threadx/threadx`
- `eembc/coremark`

submodule のコードは編集せず、必要なら `port/` 側のグルーで吸収する。

## ドキュメント

`docs/ja/` と `docs/en/` に**同名 `.md`（日英必須）**。mkdocs（Material + i18n）で GitHub Pages に publish。

### カテゴリ

- `hardware/` — ボード・クロック・ピン・メモリ配置
- `build/` — CMake ビルド・フラッシュ・SWD
- `rtos/` — ThreadX 統合

### ローカル確認

```bash
pip install -r requirements.txt
mkdocs serve            # http://localhost:8000
mkdocs build --strict   # ビルド検証（CI と同じ）
```

main への push 時、`docs/**` / `mkdocs.yml` / `requirements.txt` 変更があれば GitHub Actions が自動デプロイ（`.github/workflows/deploy.yml`）。

## ビルド / フラッシュ

```bash
cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-toolchain.cmake
cmake --build build                          # 唯一の firmware: threadx (= shell)
cmake --build build --target flash   # 書き込み
# CoreMark は shell の `coremark` コマンドとして実行（別 exe ではない）
```

詳細は `docs/ja/build/cmake.md`。

## ハードウェア要点

- MCU: STM32F746NGH6 / Cortex-M7、**216 MHz**（HSE 25MHz → PLL M25 N432 P2、VOS1+OD、7WS）
- FPU on、I-Cache / D-Cache on（`src/bsp.c`）
- VCP: USART1、TX=PA9 / RX=PB7、115200 8N1（`/dev/ttyACM0`）。**PA9 は OTG_FS_VBUS と共用**（既定ブリッジで VCP 有効、UM1907）
- LED: LD1（緑）= PI1
- メモリ: Flash 0x08000000(1MB) / ITCM 0x00000000(16KB) / DTCM 0x20000000(64KB) / SRAM 0x20010000(256KB)
- リファレンス: `_ref/`（git 管理外）に RM0385、UM1907、および ST 公式デモソース（下記）
- 出荷時デモ: `_ref/backup_full.bin` はボード出荷時プリインストール（STemWin 版「STM32Cube Demonstration」、V1.2.0/2016）のフラッシュ全域ダンプ。その**公式リファレンスソース**は `_ref/STM32Cube_FW_F7_V1.17.0/Projects/STM32746G-Discovery/Demonstrations/STemWin/`（FW V1.17.0 同梱の同デモ後継リビジョン＝byte 一致ではない）。ST 公式の BSP / クロック / ペリフェラル / RTOS 統合の参照実装として読む（read-only、編集しない）

### 既知の教訓

- **ThreadX SysTick 優先度**: SysTick は PendSV より高優先度（14 vs 15）。同一だと tick が止まりデッドロック。
- **ITCM vs キャッシュ**: I/D-cache 有効時は ITCM 配置の効果は ~0.6%（キャッシュが flash WS を隠蔽済み）。

## SWD デバッグ

- GDB サーバ: `st-util`（:4242）or `openocd -f interface/stlink.cfg -f target/stm32f7x.cfg`（:3333、SCS 読みが安定）
- GDB: システムの **`gdb-multiarch`**（toolchain 同梱 gdb は `libncursesw.so.5` 欠如で不可）
- VCP（picocom 等）と `st-flash`/読み出しは `/dev/ttyACM0` を奪い合うと文字化けする。SWD と VCP は別系統。
