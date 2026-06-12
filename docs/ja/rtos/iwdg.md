# 独立ウォッチドッグ（`wdt` / IWDG）

Issue #38。**IWDG（独立ウォッチドッグ）** を追加し、ハングや暴走したファームウェアが手動の
電源再投入を待たずに **自動でリセット復帰** するようにする。専用の高優先 *petter* スレッドが
ウォッチドッグを refresh し、短い timeout でストールを素早く捕捉し、fault ハンドラは
debugger 接続中のみボードを halt 状態に保つ。クリーンルーム設計（流用コードなし）。

構成要素:

- `include/iwdg.h` / `src/iwdg.c` — IWDG ドライバ（`iwdg_init` / `iwdg_refresh`）と
  コンパイル時 gate `BSP_ENABLE_IWDG`。
- `src/bsp.c` — `SystemClock_Config()` で LSI を有効化し、`bsp_init()` で debug halt 時の
  IWDG 凍結（`__HAL_DBGMCU_FREEZE_IWDG()`）を設定。
- `src/main.c` — 優先度 5 の petter スレッドと、`tx_application_define()` 末尾での IWDG *arm*。
- `src/fault.c` — fault halt ループが、debugger がコアを占有している間のみ IWDG を refresh。
- `src/log.c` — `wdt info` が使う `log_reset_cause()`（`IWDG` の decode 自体は既存、RM0385 §5.3.21）。
- `shell/cmds/cmd_wdt.c` — `wdt` コマンド（`info` は常時、`starve` は dangerous）。

## なぜ IWDG か

IWDG は **LSI**（約 32 kHz の RC 発振器）で駆動され、HSE + PLL の 216 MHz ツリーから完全に
独立している（RM0385 §32）。メインクロックや ThreadX スケジューラが停止してもカウントを
続けるため、ソフトウェアタイマでは捕捉できない故障を捕らえられる。

本設計で **捕捉する**:

- tick / scheduler の停止（どのスレッドも走らない）、
- IRQ-off ロックアップ（`__disable_irq()` + spin）、
- 優先度 < 5 の暴走スレッド（petter がスケジュールされない）、
- **未接続時の fault halt**（debugger なし）。

**捕捉しない**（仕様）: shell スレッド（優先度 16）*内部* の論理無限ループ。優先度 5 の petter が
refresh し続けるためウォッチドッグは発火しない — `coremark`（約 12 s, yield なし）も同じ理屈で
走り、リセットされてはならない。この種は Ctrl+C やスレッド単位の liveness チェックで対応する。

## タイミング

LSI は公差の大きい RC 発振器（データシート）: **最小 17 kHz / 標準 32 kHz / 最大 47 kHz**。
timeout は `T = prescaler × (RLR+1) / f_LSI`。

| 設定 | 値 |
|---|---|
| プリスケーラ | `/64` |
| リロード（RLR） | `1499` |
| ウィンドウ | 無効 |

| LSI コーナー | timeout `T` |
|---|---|
| 32 kHz（標準） | **3.00 s** |
| 47 kHz（高速） | **2.04 s** ← worst-case 最小 |
| 17 kHz（低速） | 5.65 s |

petter は `tx_thread_sleep(1000)`（≈1.0 s）で sleep する。worst-case 最小の **2.04 s** に対して、
これは「`T/2` 以内に refresh する」標準マージン（約 2 倍）にあたる。よって約 1 s を超えるハングは
リセットを引き起こす — それが狙いどおり。

## pet 戦略

petter（`iwdg_entry`, `src/main.c`）は **優先度 5** — led(10) / shell(16) / bg ジョブ worker(17)
より上 — で走るため、CoreMark や優先度の低い暴走を preempt し、コード各所に refresh を撒かずとも
pet し続ける。上位は System Timer スレッド(0) と ISR のみ。起動直後に 1 回 pet し、以後約 1 s 周期。

## LSI 有効化

`SystemClock_Config()` は既存のオシレータ集合に `RCC_OSCILLATORTYPE_LSI` を OR し、
`LSIState = RCC_LSI_ON` を設定する（HSE/PLL は不変）。`HAL_RCC_OscConfig()` が `LSIRDY` を待つ
（RM0385 §5.2）。LSI は 216 MHz ツリーから独立しているため、クロック bring-up に影響しない。

## DBGMCU 凍結

`bsp_init()` は `HAL_Init()` の直後に `__HAL_DBGMCU_FREEZE_IWDG()`（`DBGMCU_APB1_FZ.DBG_IWDG_STOP`
を設定、RM0385 §40.16.5）を呼び、**debugger でコアが halt している間 IWDG のカウントを止める**。
これが無いと SWD ブレークポイント 1 つでウォッチドッグがセッション中にボードをリセットしてしまう。

## arm のタイミング（意図的に遅らせる）

ウォッチドッグは最後、**`tx_application_define()` の末尾** で、固定順
*petter create → `iwdg_init()` → `tx_glue_timer_enable()`* で arm する:

- arm 前の fail-soft な bring-up — 特に QSPI / SD の HAL init は媒体異常時に **最大約 5 s
  ブロックし得る** — が、ウォッチドッグ arm の **前** に完走する。媒体異常時は「qspi/sd disabled、
  shell は継続」のままで、IWDG リセットループに化けない。
- petter は既に存在し、直後にスケジューラが起動するため、init→初回 pet 窓は **sub-ms** で、
  2.04 s の最小値を大きく下回る。
- `HAL_IWDG_Init()` はプリスケーラ/リロード更新（SR PVU/RVU）を `HAL_GetTick()` timeout で poll
  する。SysTick ISR は起動時から `HAL_IncTick()` を回す（`_tx_timer_interrupt()` のみ
  `tx_glue_timer_enable()` まで gate）ので、ここでも既存の QSPI/SD init と同様に tick は進む。

## fault halt 連携

`src/fault.c` は全 halt 経路を `fault_halt()` に集約する:

- **debugger 接続中**（`CoreDebug->DHCSR & C_DEBUGEN`）: ループがリロードキー
  （`IWDG->KR = 0xAAAA`）を書き、ボードを halt に保ち SWD 事後解析を可能にする。キーは
  HAL ハンドル非依存で直書きするため、IRQ 無効・ドライバ状態無効でも安全。
- **debugger 未接続**: ループは pet **しない** ので IWDG が timeout（約 2〜6 s）してボードを
  リセットする — 未接続のクラッシュは自力で復帰する。

次回リセット後も `dmesg` にはクラッシュ行が残り（ログは DTCM で生存、[logging](logging.md) 参照）、
続いて新しい `reset cause: IWDG` の boot 行が現れる。

## `wdt` コマンド

```text
sh> wdt info
IWDG:       enabled
  timeout:  ~3.0s typ (2.0-5.7s over LSI tol), prescaler /64 RLR=1499
  pet:      ~1s by the priority-5 petter thread
  last reset: IWDG
sh> wdt starve            # dangerous: pet を止めてボードをリセット
wdt: starving the IWDG (IRQ off, spin) -> reset in ~timeout
```

- `wdt info` は **常時** 利用可能。`BSP_ENABLE_IWDG=OFF` では `IWDG: disabled (build)` を表示し、
  `HAL_IWDG_Init()` がエラーを返していれば `init failed (may be armed)` を表示する（HAL は
  SR poll の前に IWDG を start するので、失敗が「未 arm」を意味しない）。`last reset` は
  `log_reset_cause()` から得る。
- `wdt starve` は **dangerous** コマンド（spec §12）かつウォッチドッグが組み込まれている必要が
  あるため、`CLI_ENABLE_DANGEROUS_CMDS` **かつ** `BSP_ENABLE_IWDG` が両方有効なときだけコンパイル
  される。UART を drain（約 50 ms、`reboot` と同様）し、`__disable_irq()` で petter を止め、
  spin する — IWDG がボードをリセットする。

## コンパイル時 gate `BSP_ENABLE_IWDG`

既定 ON。`-DBSP_ENABLE_IWDG=OFF` でビルドすると IWDG を完全に除外する: LSI 有効化なし、
DBGMCU 凍結なし、petter スレッドなし、IWDG arm なし、`wdt info` は `disabled (build)` を表示。
この define は **`bsp_iface` INTERFACE** ターゲットに置く（`threadx` 実行ファイルではない）。
`src/bsp.c` は `common` オブジェクトライブラリでコンパイルされ、`bsp_iface` の定義しか見えない
ため、全翻訳単位に同じ値が届く。`nm threadx.elf | grep -i iwdg` で検証できる（OFF ビルドでは
`iwdg_*`/`hiwdg` シンボルが消える）。

## 注意点

- **bring-up 失敗は自動復帰しない。** IWDG は bring-up 完走後にのみ arm するため、
  HSE/PLL/UART/QSPI/SD の初期化失敗はウォッチドッグ arm 前に halt する（`Error_Handler()` が
  spin、SWD attach 可）。これは意図的 — 恒久的なクロック故障は、さもなければ永久にリセット
  ループするため。
- **`C_DEBUGEN` は開発向けのヒューリスティック** であり、今 debugger が物理的に接続されている
  証明ではない。detach 手順次第で `C_DEBUGEN` が残り、fault halt が refresh を続けて（リセット
  されず）しまう場合がある。量産では別途 gate / 文書化すること。

## 検証

- **ビルド**: `cmake --build build`（既定 ON）。OFF ビルドは
  `cmake -B build-off -DBSP_ENABLE_IWDG=OFF ...` の後、
  `nm build-off/threadx.elf | grep -i iwdg` で IWDG シンボルが消えていることを確認。
- **実機**（`/dev/ttyACM0`, 115200 8N1, `flash`）:
    - 通常動作でリセットされない: shell 入力、`coremark`（約 12 s）、`sleep`、`watch`。
    - `wdt info` が `enabled` / timeout / `last reset` を表示。
    - SWD ブレークポイント（`openocd`/`st-util` + `gdb-multiarch`）でボードが **リセットされない**
      （DBGMCU 凍結）。
    - `wdt starve` でボードがリセットし、再起動後 `dmesg` / `wdt info` の reset cause が `IWDG`。
    - debugger 未接続では `crash bus` の fault halt が IWDG でリセットし、接続中は halt のまま
      解析できる。
