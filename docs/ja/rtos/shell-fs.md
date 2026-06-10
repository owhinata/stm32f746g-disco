# シェル / fs コマンド

QSPI NOR 上の FileX ファイルシステム（[ファイルシステム](filesystem.md)）を操作する。`shell/cmds/cmd_fs.c`（#30）。

```
fs format [full] yes   フォーマット（full = 全 256 ブロック消去から）
fs ls [path]           ディレクトリ一覧（既定 /）
fs cat <path>          ファイル内容を表示
fs write <path> <text> ファイル作成/上書き（スペースを含む場合は引用符で）
fs rm <path>           ファイル / 空ディレクトリの削除
fs mkdir <path>        ディレクトリ作成
fs info                容量 / 空き / FAT 種別
fs umount              flush してアンマウント
```

## 使用例

```
sh> fs format full yes        # 初回（出荷時デモデータが入っている場合は必須）
erase 256/256 (100%)
formatting: 32130 sectors x 512 B (16065 KiB)...
formatted and mounted
sh> fs mkdir /config
created /config
sh> fs write /config/app.ini "hello world"
wrote 11 bytes to /config/app.ini
sh> fs cat /config/app.ini
hello world
sh> fs ls /config
-       11  APP.INI
sh> fs info
state    : mounted
fat      : FAT16 (32104 clusters)
cluster  : 512 B
total    : 16052 KiB
free     : 16051 KiB
sh> reboot
...
sh> fs cat /config/app.ini    # reboot 後も読める（永続性）
hello world
```

## 動作の要点

- **lazy mount**: 最初の `fs` コマンドで自動マウント。未フォーマットなら `run fs format yes` エラー
- **書込みは即時永続**: write/rm/mkdir/format は `fx_media_flush` まで行うので、その後いつ reset/電源断しても直前の内容が残る（書込み**中**の電源断耐性は対象外）
- **`fs format` の安全ラッチ**: 破壊操作のため literal な `yes` が必須
- **`full` の要否**: LevelX フォーマット済みなら `fs format yes` は数秒（消去スキップ、wear 履歴保全）。出荷時デモデータ等の場合は自動的に full 消去へフォールバック（要 ~3 分、Ctrl+C 中断可・中断すると未フォーマットのまま）
- **Ctrl+C**: `format` はブロック間、`cat` はチャンク間、`ls` はエントリ間で中断可
- **bg 実行**: 状態は mutex 保護のシングルトンのみなので `fs write ... &` も安全
- **排他モデル**: 通常の fs コマンドは shared、`fs format` / `fs umount` / `qspi erase/test` は exclusive。実行中の fs コマンドがあると format/umount は `busy` エラーになり（逆も同様）、媒体をコマンド途中で奪われることはない
- **`qspi erase/test` との関係**: mounted 中は拒否される。`fs umount` してから実行する
- ファイル名は FAT の 8.3 + long name。`fs ls` の表示は FAT に保存された名前
