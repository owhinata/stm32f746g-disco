# NetX Duo IPv4（ネットワークスタック）

P1（[Ethernet リンク](ethernet.md)）の上に **Eclipse ThreadX NetX Duo（本家、MIT）** で
IPv4 TCP/IP スタックを立ち上げ、ARP / ICMP(ping) / DHCP まで通す。Epic #49 の **P2**。
MAC を実際に Start し、RX/TX 実トラフィックを流す最初のフェーズ。

## ライセンスと clean-room ドライバ

- **NetX Duo コアは本家 `eclipse-threadx/netxduo`（MIT）を submodule**（threadx/filex/levelx/guix と同じ
  eclipse-threadx mirror）。
- **ETH ↔ NetX のネットワークドライバ（`port/netxduo/nx_eth_driver.c`）は clean-room 自作**。
  ST 製 `nx_stm32_eth_driver.c` は x-cube-azrtos-f7 / stm32-mw-netxduo の両方とも
  **Microsoft Azure RTOS EULA（非 OSI、Licensed Hardware 限定）**で、MIT 公開の本リポジトリには
  持ち込めないため。NetX Duo の公開ドライバ契約（`nx_ip_create` の driver entry、`NX_LINK_*`
  コマンド、`NX_PACKET`）と新版 HAL_ETH API のみを参照した（ST ドライバのコードは非流用）。

## ★最重要設定: `NX_IP_PERIODIC_RATE`

`ports/cortex_m7/gnu/inc/nx_port.h` は `NX_IP_PERIODIC_RATE` を `#ifndef` で **100** に
ハードコードするが、本プロジェクトの ThreadX は **1000 Hz**（`TX_TIMER_TICKS_PER_SECOND=1000`、
1 tick=1ms）。NetX は ARP aging / TCP 再送 / ICMP / DHCP lease/renew の全タイマをこの値から
導くため、100 のままだと **全タイマが 10 倍遅延**する。`port/netxduo/nx_user.h` で
**`NX_IP_PERIODIC_RATE 1000`** に override 必須（override 後は wait_option = ミリ秒）。

## パケットプールとキャッシュ

新版 HAL_ETH は **D-cache の maintenance を一切行わない**。MAC DMA が触る全ペイロード
（RX バッファ・TX ペイロード・DHCP データグラム）は **non-cacheable** でなければならない。
そこで **単一のパケットプールを `.sdram.eth`（FMC bank2, 0xC0400000, MPU non-cacheable）に置き**、
RxAllocate 供給元 / `nx_ip_create` の default pool / TX ペイロード源 / DHCP（`nx_dhcp_packet_pool_set`）
で共用する → cache maintenance ゼロでコヒーレント（カメラ/LTDC バッファと同方式）。payload 1600 B、
~38 packets（64 KB）。NetX の制御ブロック（`NX_IP`/`NX_DHCP`/プール構造体）や IP スレッドスタック・
ARP キャッシュは CPU 専用なので通常 SRAM（`.bss`）に置く。

## RX / TX（zero-copy）

- **RX**: ETH ISR は `_nx_ip_driver_deferred_processing()`（イベントフラグのみ、ISR-safe）だけを行い、
  NetX IP helper スレッドが `NX_LINK_DEFERRED_PROCESSING` でドライバを再入して
  `HAL_ETH_ReadData()` ループで drain する。HAL の zero-copy コールバック（`HAL_ETH_RxAllocateCallback`
  /`RxLinkCallback`、`USE_HAL_ETH_REGISTER_CALLBACKS=0` ゆえ weak の strong override）で、プールから
  確保した `NX_PACKET` のペイロードを DMA バッファとして渡し、受信後に **実測オフセット法**
  （プール生成時に `packet→payload` offset を 1 度測る）で `NX_PACKET` を復元する。ethertype で
  IPv4→`_nx_ip_packet_receive` / ARP→`_nx_arp_packet_deferred_receive` / RARP→`_nx_rarp_packet_deferred_receive`
  に分岐。受信フレームは 2 バイトの整列パッドを挟み、Ethernet 14 B を剥がした後の IP ヘッダが
  4 バイト境界（payload+16）に揃う（TX と対称）。
- **TX**: 14 B の Ethernet ヘッダを prepend（NetX の `NX_PHYSICAL_HEADER=16` の余地に収まる）し、
  `ETH_BufferTypeDef` チェーン → `HAL_ETH_Transmit_IT`。送信完了（`HAL_ETH_TxFreeCallback`）で
  `NX_PACKET` を解放。fragment 数が `ETH_TX_DESC_CNT`(4) を超える稀なケースは scratch バッファへ
  coalesce する。
- **RX プール枯渇からの回復**: プールが枯れて RxAllocate が NULL を返すと descriptor の再 arm が
  止まるが、`HAL_ETH_ReadData()` は `RxBuildDescCnt!=0` のとき毎回 `ETH_UpdateDescriptor()`（=starved
  descriptor の再 arm）を呼ぶため、eth-link スレッドが 200 ms 毎に deferred を kick する watchdog で
  最大 200 ms で回復する。

## リンク状態と MAC Start/Stop

NetX は PHY を polling しない。P1 の `eth-link` スレッド（prio 15）が PHY 遷移を検出し、登録
コールバック（依存逆転、`eth_link.c` は NetX 非依存）でドライバへ通知する。**MAC は
「stack ENABLE 済み AND リンク up」のときだけ Start** する: 遷移 down→up で `eth_lock` 下に PHY の
speed/duplex を `HAL_ETH_SetMACConfig`（READY 必須なので Stop→config→Start 順）→`HAL_ETH_Start_IT`。
up→down で `HAL_ETH_Stop_IT`。NetX への反映は内部 field 直接操作でなく `nx_ip_link_status_change_notify_set`
で登録した callback（IP スレッド文脈）が `nx_interface_link_up` を更新し、リンク up で DHCP を開始する。

すべての `HAL_ETH` 操作は単一の `eth_lock` で直列化する（MDIO polling / Start・Stop / Transmit / ReadData）。

## スレッド優先度

| スレッド | 優先度 | 備考 |
|---|---|---|
| NetX IP helper | 12 | camera producer(10) より下＝DCMI overrun を守る、touch(13)/GUIX(14)/cli(16) より上 |
| DHCP | 13 | IP スレッドを starve させない |
| eth-link（P1） | 15 | 200ms PHY poll + watchdog kick |
| ETH_IRQn | preempt 7 | ISR は deferred event のみ |

## `net` シェルコマンド（P2 追加分）

| コマンド | 説明 |
|---|---|
| `net info` | P1 の MAC/PHY/link に加え IP / mask / gateway（DHCP or static） |
| `net ping <a.b.c.d> [count]` | ICMP echo を count 回（既定 4）、RTT・min/avg/max・loss を表示 |
| `net ip <a.b.c.d/mask> [gw]` | 静的アドレスに切替（DHCP 停止） |
| `net dhcp` | DHCP で(再)取得 |

ブート時の既定は **DHCP**。リンク up で `nx_dhcp_start` が走り、`net info` でリースを確認できる。

## TCP echo サーバ（P3）

NetX の TCP ソケット経路を検証する最小の echo サービス（`port/netxduo/nx_echo.c`）。専用スレッド
（prio 14、一度生成し stop で park）が単一接続を捌く: `nx_tcp_socket_create` → `nx_tcp_server_socket_listen`
→ ループ{`nx_tcp_server_socket_accept` → 受信を `nx_tcp_socket_send` でそのまま echo → `nx_tcp_socket_disconnect`
→ `unaccept` → `relisten`}。受信パケットは直接再送する（NetX が IP/TCP ヘッダの余地を残す）。accept/receive を
短い timeout で回し、`net echo stop` で `echo_run=0` → スレッドが socket を teardown して park する。
**start/stop は同期**: `echo_active`（lifecycle 所有フラグ）を start で先に立て stop が teardown 完了まで待つので、
連続した stop→start で古い listener が残らない。この socket 取り回しが **P4（network shell, cli_backend_tcp）の雛形**。

| コマンド | 説明 |
|---|---|
| `net echo start [port]` | echo サーバ起動（既定ポート 7） |
| `net echo stop` | echo サーバ停止 |

`net info` に稼働中の `echo: listening on :7 (N conns, M bytes)` を表示。PC から `nc <board-ip> 7` で接続し
入力行が echo される。

## ネットワーク shell（telnet, P4）

既存の clean-room CLI shell を **TCP（telnet）経由**でも操作できる（`port/netxduo/nx_shell.c`）。
CLI は transport 抽象（VCP=USART1 backend）を持つので、**TCP backend を 1 つ足して 2 つ目の
`cli_instance` を bind するだけ**でコア無改造に近い形で載る。`telnet <board-ip>` か `nc <board-ip> 23`
で接続すると、VCP と同じ shell（`dmesg`/`fs`/`camera`/`net` 等）がネットワーク越しに使える。
VCP shell と同時稼働。認証なし（LAN 内前提）。

- **単一セッション（N=1）**: CLI の §14 KILL/uninit ライフサイクル未実装のため、接続のたびに
  破棄/生成せず**静的 `cli_instance` を再利用**する。同時接続は 1。
- **接続時のクリーンセッション**: 接続のたびに行/履歴/描画状態をリセットし prompt を再表示する。
  CLI コアに最小追加（`cli_session_reset_state()` + `CLI_EVT_CONN` イベント + optional transport
  メソッド `session_begin`）。サーバスレッドは accept で `CLI_EVT_CONN` を投げるだけで、**実際の
  リセット/出力有効化は CLI スレッド上**で行う（編集状態の変更を 1 スレッドに保ち競合を排除）。
  出力有効化（`connected`）は CLI スレッドが `session_begin` で立て、切断で切るので、再接続時に
  前コマンドの遅延出力が新クライアントへ漏れない。
- **telnet IAC ストリップ**: 接続時の telnet ネゴシエーション（`0xFF ...`）を RX 経路で破棄し、
  telnet/nc 両方で文字化けしない。
- **TX フロー制御**: 出力は `nx_tcp_socket_send(NX_NO_WAIT)`。window/queue full は
  `window_update`/`queue_depth` notify で `cli_transport_notify_tx` を起こして再開
  （`NX_ENABLE_TCP_QUEUE_DEPTH_UPDATE_NOTIFY` 必須）。TX queue を 8 に制限し共用 pool を保護。
- **注意（printf retarget）**: C ライブラリの生 `printf` は VCP(USART1) に出る（`_write` の
  フォールバック）。shell の `cli_print` 経由の出力は TCP に正しく出るので、コマンドの通常出力は
  telnet に届く。

## MJPEG-over-HTTP カメラ配信（P5）

OV5640 のハードウェア JPEG フレームを **`multipart/x-mixed-replace`** で PC ブラウザへ
ストリーム配信する（`port/netxduo/nx_mjpeg.c`）。Epic #49 の **P5**、カメラ frame pipeline
（#46/#47）の **eth_sink**（30fps JPEG パスの本命ネット消費先）。

```
net mjpeg start [res]   # camera を JPEG で所有 + :80 listen（res 既定 qvga、qqvga|qvga|vga）
net mjpeg stop          # 配信停止・camera 解放
net mjpeg stats         # client 接続 / 配信 frames・bytes / drop / error
```

ブラウザで `http://<board-ip>/` を開くとライブ映像が見える。N=1 単一クライアント。

- **明示コマンド方式**: `net mjpeg start` が camera を JPEG モードで所有し続け（client の有無に
  関わらず）、:80 で listen。GUIX preview / `camera stream` が DCMI を所有中は `CAM_ERR_BUSY` で
  拒否（`gui stop` 等が必要）。所有権は既存の単一所有モデル（`cam_ext_sink`）で、camera stream /
  GUI と自動的に排他（#73）。
- **eth_sink = 同期 copy sink**: frame pipeline の consume（producer スレッド文脈）は重い HTTP 送信を
  せず、JPEG フレームを private バッファ（`.sdram.eth`、JPEG budget=256KB）へ memcpy して**即 put**
  （in-flight 常に 0＝GUIX sink と同じ同期型）。これで camera の async teardown（DCMI overrun）が
  そのまま安全に回る。HTTP サーバスレッドが private バッファから送信。`buf_busy` フラグで 1-deep
  ハンドオフし、送信中の新フレームは drop（遅い client に自然 back-pressure）。
- **HTTP サーバスレッド（prio14）**: listen→accept→GET 読み飛ばし→multipart ヘッダ→フレーム毎に
  `--boundary` + `Content-Type: image/jpeg` + `Content-Length` + JPEG + `\r\n` を送信。切断で relisten、
  `stop` で park（nx_echo と同型、accept/送信は bounded wait）。socket create+listen はスレッド内
  （thread-only NetX API）。
- **★MSS チャンク送信**: `nx_tcp_socket_send` は payload>MSS で内部分割し pool を二重消費するため、
  JPEG を **MSS（=min(1400, peer MSS)）単位**に分割して `nx_packet_allocate`+`data_append`+`send`。
  `transmit_configure` で TX queue を 8 に制限し共用 eth_pool を保護。フレーム途中で詰まると
  multipart が壊れるので、ドロップはフレーム先頭で判定する。
- **帯域**: 単一 16-bit FMC SDRAM を DCMI write + LTDC scanout + DMA2D + **ETH DMA** + memcpy が
  競合する。JPEG は圧縮済みで ETH read 帯域は小さいが、`net mjpeg stats` と `camera stream stats`
  （dma fe/s, ovr）で実測して確認する。

## 参考

- RM0385 §38（Ethernet）/ §2.1.10（ETH DMA バス）
- [eclipse-threadx/netxduo](https://github.com/eclipse-threadx/netxduo)（本家, MIT）
- 新版 HAL_ETH（`stm32f7xx_hal_eth.c`、ディスクリプタモデル + zero-copy コールバック）
