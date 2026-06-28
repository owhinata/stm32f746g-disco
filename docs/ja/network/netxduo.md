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

## 参考

- RM0385 §38（Ethernet）/ §2.1.10（ETH DMA バス）
- [eclipse-threadx/netxduo](https://github.com/eclipse-threadx/netxduo)（本家, MIT）
- 新版 HAL_ETH（`stm32f7xx_hal_eth.c`、ディスクリプタモデル + zero-copy コールバック）
