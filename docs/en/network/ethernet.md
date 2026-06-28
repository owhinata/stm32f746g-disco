# Ethernet (ETH MAC + LAN8742A RMII)

Bring up the STM32F746G-DISCO on-board Ethernet (RJ45 + LAN8742A RMII PHY) on the
STM32F746 built-in ETH MAC and detect/report link state. This is **P1** of the
Ethernet epic (#49): scope is **link establishment only** (NetX/LwIP and actual
traffic come in P2+).

## Hardware

- **PHY**: Microchip LAN8742A, **RMII** mode.
- **Reference clock**: the board's 25 MHz oscillator (X2) feeds both the MCU HSE
  and the LAN8742A. The PHY generates the **50 MHz RMII_REF_CLK** internally and
  the MCU receives it on **PA1** (UM1907 §6.12). No MCU clock-tree change and no
  MCO output are needed; the existing 216 MHz configuration is kept as-is.
- **MAC address**: a locally-administered, unicast address (first byte `0x02`)
  uniquified with 3 bytes derived from the full STM32 96-bit UID (`UID_BASE`
  0x1FF0F420, all three words XOR-folded).

### RMII pins (all `AF11_ETH`, push-pull, no-pull, very-high speed)

| Signal | Pin | Signal | Pin |
|---|---|---|---|
| REF_CLK | PA1 | RXER  | PG2  |
| MDIO    | PA2 | TX_EN | PG11 |
| CRS_DV  | PA7 | TXD0  | PG13 |
| MDC     | PC1 | TXD1  | PG14 |
| RXD0    | PC4 |       |      |
| RXD1    | PC5 |       |      |

ETH has its own dedicated MAC DMA (AHB1), separate from the camera DCMI (DMA2)
and SDMMC (DMA2), so there is no DMA conflict. The 10 RMII pins also do not
overlap the existing DCMI/SDMMC/LTDC/SDRAM/USART/touch assignments.

## Memory placement and cache

The new HAL_ETH driver performs **no D-cache maintenance**, so the ETH DMA
descriptors (and, from P2, the RX buffers) must live in a **non-cacheable**
region. They are placed in the `.sdram.eth` section (FMC internal **bank2**,
0xC0400000). `bsp_init()` already maps the whole 8 MB SDRAM as MPU Normal
non-cacheable, so no extra MPU region is needed, and the FMC is unambiguously
reachable by the ETH MAC DMA over AHB (the same scheme as the camera/LTDC
buffers, RM0385 §2.1.10). Descriptors are 32-byte (Cortex-M7 cache line) aligned.

Because the descriptors live in SDRAM, `eth_init()` is only called **when SDRAM
is up** (`src/main.c` gates on `sdram_is_up()`). `HAL_ETH_Init` writes the
descriptors immediately, so touching 0xC0400000 with the FMC down would fault
(the same reasoning that gates `ltdc_init()` on `sdram_init()` success).

## Initialisation flow

`eth_init()` (`port/eth/eth_link.c`) is called once at boot
(`tx_application_define`, before the scheduler) fail-soft, and does only the
following (a few ms, and **never waits for link-up**):

1. Create the `eth_lock` mutex.
2. RMII GPIO + `__HAL_RCC_ETH_CLK_ENABLE` (inline MspInit, before `HAL_ETH_Init`).
3. `HAL_ETH_Init` (internally: SYSCFG RMII select → MAC soft reset → descriptor
   chain build).
4. `HAL_ETH_SetMDIOClockRange` → LAN8742 discovery (address scan) → soft reset →
   enable + restart auto-negotiation.
5. Spawn the `eth-link` monitor thread (priority 15, 1 KB stack).

The MAC is left in `HAL_ETH_STATE_READY` and is NOT started (`HAL_ETH_Start` is
P2), so no ETH DMA runs and no ETH interrupt fires. Link establishment
(auto-neg) can take seconds, so init does not wait for it -- the monitor thread
owns that.

### PHY driver (clean-room)

`port/eth/eth_phy.c` does not vendor ST's `lan8742.c`; it is a minimal IEEE
802.3 clause-22 driver (no HAL/ThreadX dependency, MDIO access injected as a
read/write vtable). The reasons are (1) the project's clean-room port policy and
(2) avoiding the unconditional 2-second busy wait in ST's `LAN8742_Init`. It uses
BCR(0)/BSR(1)/PHYIDR(2,3) and the LAN8742-specific Special Control/Status
Register (0x1F, whose HCDSPEED field resolves speed/duplex).

### eth-link monitor thread

Polls the PHY every ~200 ms, logs link up/down transitions to the dmesg ring, and
keeps the resolved speed/duplex in a shared snapshot. The BSR link bit latches
low, so it is read twice to get the present state.

## `net` shell command

| Command | Description |
|---|---|
| `net info` | MAC address, PHY address/ID, link state (up/down, speed, duplex) |
| `net link` | Restart auto-negotiation and wait up to 5 s (Ctrl+C to abort) for link, then report |

`net info` still works with the link down as long as the driver initialised (the
driver-initialised check `eth_is_initialized()` is separate from link state).

## P1 scope and limitations

- **In scope**: link establishment detection and reporting only.
- **Out of scope (P2+)**: `HAL_ETH_Start` (MAC/DMA start), RX/TX traffic, NetX Duo
  IP/ARP/ICMP, RX buffer pool allocation.

## References

- RM0385 §38 (Ethernet) / §7.2.2, §38.4.4 (RMII select) / §2.1.10 (ETH DMA bus)
- UM1907 §6.12 (RMII wiring, REF_CLK supply)
- ST official F746G-DISCO LwIP example `ethernetif.c` (register/pin reference only)
