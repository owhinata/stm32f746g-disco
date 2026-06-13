# シリアルファイル転送（`xfer` / `camera send`）

ボード上のファイル（主に**撮影したカメラフレーム**）を、**シェルと同じ VCP シリアル
ポート**（USART1, `/dev/ttyACM0`, 115200 8N1）経由で、microSD を抜かずに PC へ取り出す。
ボード側は clean-room の **YMODEM** 送信機、PC 側は `lrzsz`（`rz`/`rb`）で受信する。

これまで（#42）は `camera save sd <path>` でファイル保存 → カードを物理的に抜くしか
手段がなかった。現在は:

```text
sh> camera capture                     # QVGA RGB565 を SDRAM へ撮影
sh> camera send                        # YMODEM で PC へ送出
ymodem: sending 'frame.raw' (153600 bytes) over the VCP
ymodem: start the receiver now -- e.g. `rb` (lrzsz YMODEM); Ctrl+C aborts
ymodem: sent 153600 bytes OK
```

## コマンド

| コマンド | 転送元 | 備考 |
|---|---|---|
| `camera send [name]` | SDRAM の撮影フレーム（FS 不要） | 既定名 `frame.raw`、`size` = 153600（QVGA RGB565） |
| `xfer send <sd\|fs> <path>` | microSD（`sd`）/ QSPI FS（`fs`）上の任意ファイル | 汎用。YMODEM block 0 に basename と正確なサイズを格納 |

どちらも同じプロトコルコアを `xfer_send_source()`（`shell/cmds/cmd_xfer.c`）経由で
駆動し、バイト源だけが異なる（`camera_frame_read` か `fx_file_read`）。カメラ経路は
`camera save` と同じ**世代チェック**を持ち、送信中に並行 `capture` が走るとフレームを
混在させずに転送を中断する。

## PC 側での受信

`lrzsz`（`rb` = YMODEM バッチ受信）が必要。**picocom** での対話的手順:

```sh
picocom -b 115200 /dev/ttyACM0 --receive-cmd "sh -c 'rb -y -vv'"
# picocom 内: `camera capture` → `camera send`
# ボードが送出を始めたら Ctrl-A Ctrl-R
# "*** file:" プロンプトは空のまま Enter（ファイル名は無視される）
# rb が frame.raw を picocom の作業ディレクトリへ書き出す
```

このレシピが吸収する 2 つの落とし穴:

- **bare `rz` でなく `rb` を使う** — `rz` は既定で ZMODEM になり、本 YMODEM 送信機が待つ
  `C` を送らない。`rb`（または `rz --ymodem`）が YMODEM 受信機。
- **`sh -c '…'` が picocom の付加ファイル名を握り潰す** — picocom は `*** file:` プロンプトに
  入力した名前を receive-cmd に付加するが、`rb` は受信時にファイル名引数を取らない（名前は
  block 0 由来）ため、素の `--receive-cmd "rb …"` だと "garbage on commandline" で失敗する。
  `sh -c` で包むと余分な引数が `$0` に入って無視される。

送信機は受信側の初期 `C` を最大 ~30 秒待つので、コマンド発行後に `rb` を起動する時間が
ある。受信後に変換:

```sh
python3 scripts/rgb565_to_png.py frame.raw out.png
```

`camera capture test`（カラーバー）なら `out.png` に既知の 8 バンドが出る — バイト順と
ブロック整合の end-to-end 確認になる。

## プロトコル（YMODEM-CRC）

clean-room 送信機を freestanding な **`svc/ymodem.c`** 層に置く（shell / HAL / ThreadX /
FileX 非依存 — transport と source は注入 vtable なので `shell/test/test_ymodem.c` で
host テスト可能）。**CRC-16/CCITT** のみ:

- 受信側が `C` を送ってバッチ開始。送信側は **block 0**（SOH/128）で
  `"name\0" "10進サイズ\0"` をゼロ詰めで返す;
- **データブロック**は STX/1024（最終が ≤128 B なら SOH/128）、短い最終ブロックは
  `0x1A` で pad。block 0 の正確なサイズで受信側が pad を切り詰める（153600 は 1024 の
  倍数なのでカメラフレームは pad 無し）;
- 各ブロックは `kind | seq | ~seq | data | crc_hi | crc_lo`、ブロックごとに ACK/NAK、
  `seq` は mod 256;
- **EOT**（1 回 NAK されたら再送）後、全 0 の block 0 で終端。

YMODEM を選んだ理由（XMODEM-1K より）は block 0 にファイル名と正確なサイズが載るため
で、任意ファイルの汎用 `xfer send` に両方が要る。lrzsz は GPL なので **PC 側のみ**再利用し、
ボード送信機はプロトコル仕様から自作している。

## シェルとの UART 共有

`camera send` / `xfer send` ハンドラは**シェルスレッド内**で動くため、実行中は main loop /
行エディタ / echo が動かず、ハンドラが UART を占有する。転送全体で新コア API
（[shell core](shell-core.md)）でコンソールを claim する:

- `cli_console_claim()` は転送中ずっと出力ロックを保持し（bg job 出力がバイト列に
  割り込めない）、グローバル `cli_xfer_active` を立てる;
- `cli_read_byte()` は**タイム付き raw RX read**で、`cli_cancel_requested()` /
  `cli_sleep()` と違い `0x03` を消費しない（これらは Ctrl+C 探索で RX をドレインし、
  YMODEM の ACK/`C`/NAK を食ってしまう）;
- `cli_xfer_active` は `cli_tx_send_blocking` の TX 満杯時 RX ドレインも止め、`_write`
  retarget の printf 出力を **drop** させる（さもなくば非シェルスレッド/ISR から
  `g_uart_console` へロック無しで届きストリームを壊す）;
- `cli_rx_flush()` を前（type-ahead 破棄）と後（末尾バイト破棄）に呼び、プロンプトを
  クリーンに復帰させる。

!!! note "ボード側ライブ進捗なし"
    同じ UART にバイナリが流れるため転送中に進捗は出せない。PC 側 `rb -vv` の進捗を見る。
    ボードは開始行と結果 1 行のみ出力する。

!!! note "転送中は bg job 出力が drop される"
    出力ロック保持により bg job の `printf` はブロックし、wedge 期限で drop する
    （[jobs](shell-jobs.md), #25）— ストリームを壊さないための意図した挙動。

!!! note "ストール timeout（`CLI_TX_TIMEOUT`）"
    PC/VCP が排出を止めると TX 経路は `CLI_TX_TIMEOUT`（既定 1000 tick ≈ 1 秒）で drop し、
    転送は transport error で失敗する。

## 中断とウォッチドッグ

- **PC 中断**: `rz` のキャンセル（CAN 2 連）で `YM_ERR_CANCEL` を返し、ボードは
  `ymodem: cancelled` を出してクリーンなプロンプトに戻る。
- **ローカル中断**: 転送中の Ctrl+C（`0x03`）は YMODEM abort にマップされ、`CAN×5` の
  teardown を送って `rz` も畳ませる。
- **ウォッチドッグ**: ~13 秒のフレーム転送でも ~3 秒の IWDG（[watchdog](iwdg.md)）を
  飢えさせない — petter は別の優先度 5 スレッドで、（優先度 16 の）シェルスレッドは待機中
  event flag で suspend するため petter が走り続ける。

## ライセンス

`svc/ymodem.c` と `xfer` / `camera send` のグルーは MIT（clean-room）。`lrzsz`（GPL）は
PC 側受信機としてのみ使い、vendoring しない。
