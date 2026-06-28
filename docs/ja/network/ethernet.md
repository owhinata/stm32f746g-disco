# Ethernet（ETH MAC + LAN8742A RMII）

STM32F746G-DISCO のオンボード Ethernet（RJ45 + LAN8742A RMII PHY）を STM32F746
内蔵 ETH MAC で立ち上げ、リンク状態を検出・報告する。Epic #49 の **P1** に相当し、
**リンク確立まで**を対象とする（NetX/LwIP や実トラフィックは P2 以降）。

## ハードウェア

- **PHY**: Microchip LAN8742A、**RMII** 接続。
- **リファレンスクロック**: ボードの 25 MHz オシレータ（X2）が MCU の HSE と
  LAN8742A の両方に供給される。PHY が内部で **50 MHz の RMII_REF_CLK** を生成し、
  MCU は **PA1** で受ける（UM1907 §6.12）。MCU 側のクロックツリー変更や MCO 出力は
  不要で、既存の 216 MHz 構成のまま動作する。
- **MAC アドレス**: ローカル管理・ユニキャストアドレス（先頭バイト `0x02`）に、
  STM32 96-bit UID（`UID_BASE` 0x1FF0F420、3 ワードを XOR で畳んだ値）から生成した
  3 バイトを付与して一意化する。

### RMII ピン（すべて `AF11_ETH`, push-pull, no-pull, very-high speed）

| 信号 | ピン | 信号 | ピン |
|---|---|---|---|
| REF_CLK | PA1 | RXER  | PG2  |
| MDIO    | PA2 | TX_EN | PG11 |
| CRS_DV  | PA7 | TXD0  | PG13 |
| MDC     | PC1 | TXD1  | PG14 |
| RXD0    | PC4 |       |      |
| RXD1    | PC5 |       |      |

ETH は MAC 内蔵の専用 DMA（AHB1）を持ち、カメラ DCMI（DMA2）や SDMMC（DMA2）とは
別系統なので競合しない。RMII 10 ピンも既存の DCMI/SDMMC/LTDC/SDRAM/USART/タッチと
重複しない。

## メモリ配置とキャッシュ

新しい HAL_ETH ドライバは **D-cache の maintenance を一切行わない**。そのため
ETH の DMA 記述子（および P2 以降の RX バッファ）は **non-cacheable** 領域に置く
必要がある。本実装では `.sdram.eth` セクション（FMC 内部 **bank2**, 0xC0400000）に
配置する。この領域は `bsp_init()` が MPU で SDRAM 全 8 MB を Normal non-cacheable に
マップ済みなので追加の MPU 設定は不要で、FMC は AHB 上で ETH MAC DMA から確実に
到達できる（カメラ/LTDC のバッファと同じ方式、RM0385 §2.1.10）。記述子は 32 バイト
（Cortex-M7 キャッシュライン）境界に揃える。

記述子が SDRAM 上にあるため、`eth_init()` は **SDRAM が up のときだけ**呼ばれる
（`src/main.c` が `sdram_is_up()` でゲートする）。`HAL_ETH_Init` は記述子を即座に
書き込むので、FMC が down のまま 0xC0400000 に触れるとフォルトするためである
（`ltdc_init()` が `sdram_init()` 成功でゲートされるのと同じ理屈）。

## 初期化フロー

`eth_init()`（`port/eth/eth_link.c`）はブート時（`tx_application_define`、スケジューラ
起動前）に fail-soft で 1 回呼ばれ、以下のみを行う（数 ms、**リンク確立は待たない**）:

1. `eth_lock` mutex 生成。
2. RMII GPIO + `__HAL_RCC_ETH_CLK_ENABLE`（インライン MspInit、`HAL_ETH_Init` の前）。
3. `HAL_ETH_Init`（内部で SYSCFG の RMII 選択 → MAC ソフトリセット → 記述子チェーン構築）。
4. `HAL_ETH_SetMDIOClockRange` → LAN8742 の探索（アドレス scan）→ ソフトリセット →
   auto-negotiation の有効化・再開。
5. `eth-link` 監視スレッド（優先度 15、スタック 1 KB）を生成。

MAC は `HAL_ETH_STATE_READY` のまま起動しない（`HAL_ETH_Start` は P2）。したがって
ETH DMA は一切動かず、ETH 割込みも発生しない。リンク確立（auto-neg）は数秒かかりうる
ため init では待たず、後段のスレッドに委ねる。

### PHY ドライバ（clean-room）

`port/eth/eth_phy.c` は ST の `lan8742.c` を vendoring せず、IEEE 802.3 clause-22 の
最小ドライバを自作している（HAL/ThreadX 非依存、MDIO アクセスは read/write vtable で
注入）。理由は (1) プロジェクトの clean-room port 方針、(2) ST `LAN8742_Init` の無条件
2 秒ビジーウェイトを避けるため。使用レジスタは BCR(0)/BSR(1)/PHYIDR(2,3) と
LAN8742 固有の Special Control/Status Register(0x1F、HCDSPEED で速度/duplex を解決)。

### eth-link 監視スレッド

約 200 ms 周期で PHY を polling し、リンクの up/down 遷移を dmesg ログに記録、
解決した速度/duplex を共有スナップショットに保持する。BSR のリンクビットは
latched-low なので 2 回読んで現在値を得る。

## `net` シェルコマンド

| コマンド | 説明 |
|---|---|
| `net info` | MAC アドレス、PHY アドレス/ID、リンク状態（up/down, 速度, duplex）|
| `net link` | auto-negotiation を再実行し、最大 5 秒（Ctrl+C で中断可）リンク確立を待って報告 |

`net info` はリンク down でも初期化済みなら情報を表示する（ドライバ初期化判定
`eth_is_initialized()` とリンク状態を分離している）。

## P1 の範囲と制限

- **対象**: リンク確立の検出・報告のみ。
- **未対応（P2 以降）**: `HAL_ETH_Start` による MAC/DMA 起動、RX/TX 実トラフィック、
  NetX Duo による IP/ARP/ICMP、RX バッファプールの実体確保。

## 参考

- RM0385 §38（Ethernet）/ §7.2.2・§38.4.4（RMII 選択）/ §2.1.10（ETH DMA バス）
- UM1907 §6.12（RMII 配線、REF_CLK 供給）
- ST 公式 F746G-DISCO LwIP 例 `ethernetif.c`（レジスタ/ピン参照のみ）
