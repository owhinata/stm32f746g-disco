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
- `full` なしの `fs format` は LevelX 構造が健全なら消去をスキップ（数秒、消去回数の履歴も保全）
- mounted 中は `qspi erase` / `qspi test` が拒否される。`fs umount` が正規の退避経路
- 電源断（書込み中断）耐性は **MVP スコープ外**

コマンドの使い方は [シェル / fs コマンド](shell-fs.md)。
