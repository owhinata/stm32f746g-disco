# ファイルシステム（FileX + LevelX）

外部 16MB QSPI NOR（[ハードウェア / QSPI NOR フラッシュ](../hardware/qspi-flash.md)）上の永続ファイルシステム。Epic #27 の Phase B（#30）。

```
shell `fs` コマンド (shell/cmds/cmd_fs.c)
  → FileX (FAT16)        fx_media_* / fx_file_* / fx_directory_*
     → FileX media driver (port/filex/fx_lx_nor_driver.c)
        → LevelX NOR      lx_nor_flash_*（論理セクタ⇔物理ブロック、wear leveling）
           → LevelX driver glue (port/levelx/lx_nor_qspi_driver.c)
              → QSPI ドライバ (port/qspi/qspi_flash.c, #29)
                 → N25Q128A 16MB
```

submodule `lib/filex` / `lib/levelx`（eclipse-threadx、read-only ミラー）を ThreadX と同じパターンで統合: ソース GLOB + `port/filex/fx_user.h` / `port/levelx/lx_user.h`（`FX_INCLUDE_USER_DEFINE_FILE` / `LX_INCLUDE_USER_DEFINE_FILE`）。NAND 系（`lx_nand_*`）と RAM シミュレータはビルドから除外。

## 用途の定義

QSPI NOR + LevelX + FileX が担うのは「**少量・頻繁更新・永続**」データ（設定ファイル、キャリブレーション値、小規模ログ等）。大量ログ・多数の画像はスコープ外（将来 microSD + FileX）。

## LevelX（wear leveling）

| 項目 | 値 |
|------|----|
| 物理ブロック | **64 KB 消去セクタ × 256**（erase 0xD8） |
| 論理セクタ | 512 B |
| ブロックあたり論理セクタ | 126（ヘッダ+マッピングで 2 セクタ相当を消費、overhead ~1.6%） |
| FS に渡す総セクタ | `lx_nor_flash_total_physical_sectors` − 1 ブロック分（reclaim 余裕の予約）≒ **32,130 ≒ 15.7 MB** |

- **`LX_DIRECT_READ` 無効**が D-Cache 回避の本体: LevelX はすべて RAM の sector buffer 経由で読み、0x90000000（memory-mapped 窓）への CPU アクセスは発生しない
- `LX_THREAD_SAFE_ENABLE`（LevelX 内部 mutex）、`LX_NOR_DISABLE_EXTENDED_CACHE`
- LevelX のメタデータ更新は **programmed page 内のワードへの bit-clearing 再 program**。N25Q128A はこれを明示的に許容（datasheet: "bits are programmed from one through zero"、page あたりの累積 program 回数制限なし）
- 消去回数はブロックヘッダ（word 0）に保存され、リブートをまたいで wear leveling が継続する

## FileX

- **FAT16**: 512 B/sector × 1 sector/cluster × ~32K clusters
- `fx_media_format` パラメータ: FAT×1、ルートエントリ 256
- **fault-tolerant は無効**（MVP）: 受け入れ基準は「正常 flush 後の reboot 永続」まで。書込み中の電源断耐性は後続 Issue
- `FX_NO_LOCAL_PATH`: パスは絶対パスのみ
- media 保護は FileX 内蔵の ThreadX mutex（`FX_SINGLE_THREAD` 無効）→ シェル fg/bg から並行アクセス可

## マウント戦略（lazy mount）

`port/filex/fs_glue.c` の media シングルトン。`fx_system_initialize()` とマウント mutex の生成のみ `tx_application_define()` で行い、**media のマウントは最初の `fs` コマンドで実施**:

1. `fs_media_acquire()` が mutex 下で初回のみ `fx_media_open`（driver INIT → `lx_nor_flash_open`）
2. 未フォーマット/出荷時デモデータの場合はマウント失敗 → 「run `fs format`」エラーで顕在化
3. 全 mutating コマンドが `fx_media_flush` で完結するため、明示 unmount なしで reboot 後も整合

media cache は 4 KB（512 B × 8 セクタ）static。LevelX の sector buffer / 検証バッファも static（RAM 追加は計 ~15 KB）。

## 制約・注意

- `fs format full` は 256 ブロックを順に消去（typ ~3 分 / max ~13 分）。進捗表示あり、ブロック間で Ctrl+C 可。**キャンセルすると未フォーマットのまま残る**（再実行が必要）
- `full` なしの `fs format` は LevelX 構造が健全なら消去をスキップ（数秒、消去回数の履歴も保全）。`full` は raw 全消去のため消去回数の履歴もリセットされる
- mounted 中は `qspi erase` / `qspi test` が拒否される。`fs umount` が正規の退避経路
- 電源断（書込み中断）耐性は **MVP スコープ外**

コマンドの使い方は [シェル / fs コマンド](shell-fs.md)。

## 媒体非依存コアと microSD（Epic #32 / #34）

`fs`(QSPI) と `sd`(microSD, [SDMMC1](../hardware/sdmmc.md)) は **同一の FileX コマンド本体**を共有する。媒体差は `struct fs_device`（`shell/cmds/fs_cmd_core.h`）の vtable で吸収:

```
shell `fs`/`sd` コマンド (cmd_fs.c / cmd_sd.c: device 束縛 + 登録)
  → fs_cmd_core.c   ls/cat/write/rm/mkdir/info(df)/umount  ← struct fs_device 経由
     → QSPI: fs_glue.c → fx_lx_nor_driver.c → LevelX → QSPI
     → SD  : sd_fs_glue.c → fx_sd_driver.c → sd_card_* (SDMMC1 + DMA)
```

- **`struct fs_device`**: `acquire`/`unmount`/`is_mounted`、所有権 gate(`op_begin/end`・`excl_begin/end`)、`dir_lock/unlock`、`mount_hint`、`info_extra`(QSPI=wear 行 / SD=なし)。CLI の leaf handler は argv で `fs`/`sd` を区別できないため、各コマンドファイルが自分の `fs_device` を閉じ込めた薄 wrapper を登録する。
- `fs info`(QSPI) と `sd df`(SD) は同じ `fs_core_info` 本体。容量は **`fx_media_extended_space_available()`(64-bit)** で算出し、8/16/32GB の microSD でも溢れない。
- 所有権モデル(shared/exclusive/raw + lazy mount)は QSPI/SD で**独立**（別 `FX_MEDIA`・cache・mutex）。`fx_system_initialize()` は `fs_glue_init()` が 1 回だけ呼び、`sd_fs_glue_init()` は mutex 生成のみ。

### microSD FileX ドライバ（`port/filex/fx_sd_driver.c`）

SD は内蔵 FTL を持つので **LevelX 不要**。FileX のドライバ要求を `sd_card_read/write_blocks()` に直接マップする。

- **MBR / superfloppy 対応**: FileX の `fx_media_open` は LBA0 を boot sector として読むだけで partition offset を計算しない。PC/カメラのフォーマットは **MBR(LBA0) + FAT32 VBR(partition 先頭)** 構成なので、ドライバが `INIT` で LBA0 を読み、**VBR 強判定**（jmpBoot + `55 aa` + BPB sanity）なら superfloppy(`sd_part_lba=0`)、**MBR**なら partition[0] の先頭 LBA/サイズを採用。以降の全 I/O は `物理 = sd_part_lba + 論理`。`fx_media_hidden_sectors` は使わない（二重加算回避）。
- **範囲ガード**: 全 request で `論理+セクタ数` が partition サイズ(`sd_part_blocks`)を超えないこと、物理加算が 32-bit wrap しないことを **subtraction-form** で検証し、壊れた MBR/VBR でも partition 外へ書き込まない。
- **DMA/D-Cache**: FileX が渡す caller バッファ（cached SRAM）は `sd_card_*` が内部で SRAM1 の bounce buffer 経由にし cache 管理するため、ドライバ層では追加のキャッシュ操作不要。

### microSD と PC の相互運用

`sd` はマウント時に**再フォーマットしない**ので、PC で作った FAT32 をそのまま読み書きでき、ボードで書いたファイルは PC で読める。低レベルの `sd info`(カード情報)/`sd read`(生ブロック) はカードを再識別しうるため、FS マウント中は raw gate で拒否される（`sd umount` が退避経路）。`sd format`（FAT32）は後続（Phase C）。コマンドは [microSD（SDMMC1 + DMA）](../hardware/sdmmc.md)。
