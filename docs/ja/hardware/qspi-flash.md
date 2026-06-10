# QSPI NOR フラッシュ

オンボードの外部フラッシュ **Micron N25Q128A13EF840F**（128 Mbit = **16 MB**。新ロットでは MT25QL128ABA1EW9 に置換、ファームウェア互換）を STM32F746 の **QUADSPI** ペリフェラルで駆動する。ドライバは `port/qspi/qspi_flash.{c,h}`、デバッグ用シェルコマンドは `qspi`（`shell/cmds/cmd_qspi.c`）。

QSPI NOR + LevelX + FileX ファイルシステム基盤（Epic #27）の Phase A（#29）。

## 配線（UM1907 Table 13）

| 信号 | ピン | AF |
|------|------|----|
| QUADSPI_CLK | PB2 | AF9 |
| QUADSPI_BK1_NCS | PB6 | AF10（pull-up） |
| QUADSPI_BK1_IO0 | PD11 | AF9 |
| QUADSPI_BK1_IO1 | PD12 | AF9 |
| QUADSPI_BK1_IO2 | PE2 | AF9 |
| QUADSPI_BK1_IO3 | PD13 | AF9 |

!!! note "PB2 = BOOT1 共用"
    PB2 はブートストラップ BOOT1 と共用だが、サンプリングされるのはリセット時のみ。実行中に CLK として駆動しても無害（ボード既定の割当、UM1907）。

## コントローラ設定

| 項目 | 値 | 根拠 |
|------|----|------|
| クロック | AHB3 = HCLK 216 MHz、プリスケーラ 3 → **SCLK 54 MHz** | FAST_READ 0x0B の定格 108 MHz に対し 2 倍のマージン。全 1-line コマンドが合法 |
| モード | **indirect（FIFO 経由のポーリング転送）のみ** | memory-mapped（0x90000000）を使わないため **D-Cache 整合の考慮が不要** |
| FSIZE | 23（2^24 = 16 MB） | RM0385 §14.5.2 |
| サンプリング | 1/2 サイクルシフト | ST BSP と同設定 |
| CS high time | 6 サイクル（≥50 ns） | デバイス要件 |

## コマンドセット（全て 1-1-1）

| 操作 | opcode | 備考 |
|------|--------|------|
| JEDEC ID | 0x9F | 期待値 `20 BA 18` |
| Read | 0x0B FAST_READ | dummy 8 サイクル（電源投入時デフォルト、設定レジスタ変更不要） |
| Write enable | 0x06 | program/erase 前に毎回。WEL ラッチを read-back 確認 |
| Page program | 0x02 | 256 B 単位。ドライバがページ境界で自動分割 |
| Erase | 0x20（4 KB）/ 0xD8（64 KB）/ 0xC7（全チップ） | typ 0.25 s / 0.7 s / 数分 |
| Status | 0x05（WIP ポーリング）、0x70 Flag Status（P_ERR/E_ERR 確認、0x50 でクリア） | エラーは WIP では区別できないため FSR を毎回確認 |

quad read（0x6B）は Epic #27 Phase C で追加予定。

## 排他とスレッド文脈

- 公開 API は **flash 操作単位**（WREN → コマンド → WIP 待ち → FSR 確認）を内部 ThreadX mutex（TX_INHERIT）で直列化。シェル fg/bg や上位層（LevelX 予定）から並行に呼んで安全
- **thread context 専用**。ISR から呼んではならない。erase 中は `tx_thread_sleep(1)` を挟んで WIP をポーリングし、他スレッドの実行を妨げない
- 初期化は `tx_application_define()`（`src/main.c`）で 1 回。flash トランザクションは発行しない（GPIO/RCC/QUADSPI と mutex 生成のみ）

## `qspi` シェルコマンド

```
qspi id                JEDEC ID 読み出し（期待: 20 BA 18）
qspi info              ジオメトリ / クロック / ステータス
qspi read <addr> [len] 内容の hexdump（最大 256 B）
qspi erase <addr>      4 KB subsector 消去（要 4 KB 境界、dangerous）
qspi test <addr>       破壊自己テスト（dangerous）
```

`qspi erase` / `qspi test` は devmem と同じく `CLI_ENABLE_DANGEROUS_CMDS`（既定 ON）でのみコンパイルされる。`qspi test` は 4 KB subsector に対して **erase → ブランク確認 → カウンティングパターンをページ単位で program → read-back 照合**を行い PASS/FAIL を返す。ページ間で Ctrl+C を受け付ける。

!!! warning "出荷時デモデータ"
    ボード出荷時、QSPI には ST デモ（STemWin 版）のリソースが書かれている。`qspi erase` / `qspi test` はこれを破壊する。バックアップは `_ref/backup_full.bin` 参照（docs/hardware/board.md）。

## 動作確認

```
sh> qspi id
JEDEC ID: 20 BA 18  (Micron N25Q128A/MT25QL128, 16 MiB)
sh> qspi read 0 64        # 出荷時デモデータ or FF
sh> qspi test 0xFF0000    # 末尾側の scratch 領域で実施
...
PASS: erase/program/verify 4 KB at 0x00ff0000
```

失敗時の切り分け:

- ID が `00 00 00` / `FF FF FF` → ピン / AF（PB6 のみ AF10）/ RCC enable を確認
- ID は正しいが read 化け → dummy サイクル数かプリスケーラ
- program 失敗（FSR P_ERR） → WREN 漏れ or ページ境界跨ぎ
