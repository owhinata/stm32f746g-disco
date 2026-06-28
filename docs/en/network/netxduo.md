# NetX Duo IPv4 (network stack)

On top of P1 ([Ethernet link](ethernet.md)), bring up an IPv4 TCP/IP stack with
**Eclipse ThreadX NetX Duo (upstream, MIT)** and get ARP / ICMP (ping) / DHCP
working. This is **P2** of the Ethernet epic (#49): the MAC is actually started
and RX/TX traffic flows for the first time.

## License and the clean-room driver

- **The NetX Duo core is the upstream `eclipse-threadx/netxduo` (MIT)** submodule
  (the same eclipse-threadx mirror family as threadx/filex/levelx/guix).
- **The ETH<->NetX network driver (`port/netxduo/nx_eth_driver.c`) is clean-room.**
  ST's `nx_stm32_eth_driver.c` (in both x-cube-azrtos-f7 and stm32-mw-netxduo)
  carries the **Microsoft Azure RTOS EULA** (not OSI, restricted to "Licensed
  Hardware"), so it cannot be vendored into this MIT, publicly-published repo.
  Only the NetX Duo public driver contract (the `nx_ip_create` driver entry,
  `NX_LINK_*` commands, `NX_PACKET`) and the new HAL_ETH API were referenced
  (ST's driver code was not reused).

## Critical setting: `NX_IP_PERIODIC_RATE`

`ports/cortex_m7/gnu/inc/nx_port.h` hard-defaults `NX_IP_PERIODIC_RATE` to **100**
(via `#ifndef`), but this project's ThreadX runs at **1000 Hz**
(`TX_TIMER_TICKS_PER_SECOND = 1000`, 1 tick = 1 ms). NetX derives ALL its time
bases (ARP aging, TCP retransmit, ICMP, DHCP lease/renew) from this value, so
leaving it at 100 makes every NetX timer run 10x slow. `port/netxduo/nx_user.h`
overrides it to **1000** (after which wait_option values are in ms).

## Packet pool and cache

The new HAL_ETH does **no D-cache maintenance**, so every DMA-visible payload (RX
buffers, TX payloads, DHCP datagrams) must be **non-cacheable**. A **single packet
pool lives in `.sdram.eth` (FMC bank2, 0xC0400000, MPU non-cacheable)** and is
shared as the RxAllocate source, the `nx_ip_create` default pool, the TX payload
source, and DHCP's pool (`nx_dhcp_packet_pool_set`) -- so the whole data path is
coherent with zero cache maintenance (the same scheme as the camera/LTDC
buffers). Payload 1600 B, ~38 packets (64 KB). The NetX control blocks
(`NX_IP`/`NX_DHCP`/pool struct), the IP thread stack and the ARP cache are
CPU-only, so they stay in regular SRAM (`.bss`).

## RX / TX (zero-copy)

- **RX**: the ETH ISR only does `_nx_ip_driver_deferred_processing()` (event flag,
  ISR-safe); the NetX IP helper thread re-enters the driver via
  `NX_LINK_DEFERRED_PROCESSING` and drains with a `HAL_ETH_ReadData()` loop. The
  HAL zero-copy callbacks (`HAL_ETH_RxAllocateCallback`/`RxLinkCallback`, strong
  overrides of the weak symbols since `USE_HAL_ETH_REGISTER_CALLBACKS = 0`) hand
  the DMA an `NX_PACKET` payload from the pool, then recover the `NX_PACKET` by a
  **measured offset** (the packet->payload offset is measured once at pool setup).
  Ethertype dispatch: IPv4 -> `_nx_ip_packet_receive`, ARP ->
  `_nx_arp_packet_deferred_receive`, RARP -> `_nx_rarp_packet_deferred_receive`.
  A 2-byte align pad puts the IP header on a 4-byte boundary (payload+16) after
  the 14-byte Ethernet header is stripped (symmetric with TX).
- **TX**: prepend the 14-byte Ethernet header (fits in NetX's `NX_PHYSICAL_HEADER
  = 16` headroom), build an `ETH_BufferTypeDef` chain, `HAL_ETH_Transmit_IT`; on
  completion (`HAL_ETH_TxFreeCallback`) release the `NX_PACKET`. A chain longer
  than `ETH_TX_DESC_CNT` (4) -- rare -- is coalesced into a scratch buffer.
- **RX pool-exhaustion recovery**: when the pool empties and RxAllocate returns
  NULL the descriptors stop re-arming, but `HAL_ETH_ReadData()` calls
  `ETH_UpdateDescriptor()` (which retries RxAllocate for starved descriptors)
  whenever `RxBuildDescCnt != 0`, so the eth-link thread's 200 ms deferred-kick
  watchdog recovers within 200 ms.

## Link state and MAC start/stop

NetX does not poll the PHY. P1's `eth-link` thread (prio 15) detects PHY
transitions and notifies the driver through a registered callback (dependency
inversion -- `eth_link.c` never depends on NetX). **The MAC is started only when
"stack ENABLE'd AND link up"**: on down->up, under `eth_lock`, the PHY
speed/duplex is applied with `HAL_ETH_SetMACConfig` (which requires READY, so
Stop->config->Start) then `HAL_ETH_Start_IT`; on up->down, `HAL_ETH_Stop_IT`. The
NetX side is updated not by touching internal fields directly but by the callback
registered with `nx_ip_link_status_change_notify_set` (IP thread context), which
sets `nx_interface_link_up` and starts DHCP on link-up.

Every `HAL_ETH` operation is serialised by a single `eth_lock` (MDIO polling,
start/stop, Transmit, ReadData).

## Thread priorities

| Thread | Priority | Note |
|---|---|---|
| NetX IP helper | 12 | below camera producer(10) to protect DCMI from overruns; above touch(13)/GUIX(14)/cli(16) |
| DHCP | 13 | must not starve the IP thread |
| eth-link (P1) | 15 | 200 ms PHY poll + watchdog kick |
| ETH_IRQn | preempt 7 | ISR only posts the deferred event |

## `net` shell commands (added in P2)

| Command | Description |
|---|---|
| `net info` | P1's MAC/PHY/link plus IP / mask / gateway (DHCP or static) |
| `net ping <a.b.c.d> [count]` | ICMP echo count times (default 4) with RTT, min/avg/max, loss |
| `net ip <a.b.c.d/mask> [gw]` | switch to a static address (stops DHCP) |
| `net dhcp` | (re)acquire an address via DHCP |

The boot default is **DHCP**: `nx_dhcp_start` runs on link-up and `net info`
shows the lease.

## TCP echo server (P3)

A minimal echo service (`port/netxduo/nx_echo.c`) that validates the NetX TCP
socket path. A dedicated thread (prio 14, created once and parked when stopped)
serves one connection at a time: `nx_tcp_socket_create` ->
`nx_tcp_server_socket_listen` -> loop { `nx_tcp_server_socket_accept` -> echo each
received packet straight back with `nx_tcp_socket_send` -> `nx_tcp_socket_disconnect`
-> `unaccept` -> `relisten` }. The received packet is re-sent directly (NetX keeps
the IP/TCP header room). accept/receive use a short timeout so `net echo stop`
(`echo_run = 0`) lets the thread tear the socket down and park. **start/stop are
synchronous**: `echo_active` (a lifecycle-owned flag) is claimed in start before
the thread runs and stop waits for teardown, so a back-to-back stop/start never
leaves the old listener running. This socket lifecycle is the **template for P4
(the network shell, cli_backend_tcp)**.

| Command | Description |
|---|---|
| `net echo start [port]` | start the echo server (default port 7) |
| `net echo stop` | stop the echo server |

`net info` shows `echo: listening on :7 (N conns, M bytes)` while running. Connect
with `nc <board-ip> 7` and each input line is echoed back.

## Network shell (telnet, P4)

The existing clean-room CLI shell is also driveable over **TCP (telnet)**
(`port/netxduo/nx_shell.c`). The CLI has a transport abstraction (VCP = USART1
backend), so a network shell is **a TCP backend plus a second `cli_instance`
bound to it** -- almost no core change. `telnet <board-ip>` or `nc <board-ip> 23`
gives the same shell (`dmesg`/`fs`/`camera`/`net` ...) over the network,
concurrent with the VCP shell. No authentication (LAN-only).

- **Single session (N=1)**: the CLI §14 KILL/uninit lifecycle is not implemented,
  so a static `cli_instance` is reused across connections rather than
  created/destroyed per client. One connection at a time.
- **Clean session on connect**: each connection resets the line/history/render
  state and redraws the prompt, via a minimal CLI-core addition
  (`cli_session_reset_state()` + a `CLI_EVT_CONN` event + an optional transport
  method `session_begin`). The server thread only posts `CLI_EVT_CONN` on accept;
  the actual reset and output-enable happen **on the CLI thread** (keeping all
  editor-state mutation single-threaded -- no race). Output enable (`connected`)
  is set by the CLI thread in `session_begin` and cleared on disconnect, so a
  previous command's late output never leaks to a new client after a reconnect.
- **telnet IAC strip**: the telnet negotiation (`0xFF ...`) at connect is dropped
  in the RX path, so both telnet and nc are garbage-free.
- **TX flow control**: output uses `nx_tcp_socket_send(NX_NO_WAIT)`; a full
  window/queue resumes via the `window_update`/`queue_depth` notifies firing
  `cli_transport_notify_tx` (needs `NX_ENABLE_TCP_QUEUE_DEPTH_UPDATE_NOTIFY`). The
  TX queue is capped at 8 to protect the shared pool.
- **Note (printf retarget)**: raw C-library `printf` goes to the VCP (USART1) via
  the `_write` fallback; the shell's `cli_print` output goes to TCP correctly, so
  normal command output reaches telnet.

## MJPEG-over-HTTP camera streaming (P5)

Streams the OV5640 hardware-JPEG frames to a PC browser as
**`multipart/x-mixed-replace`** (`port/netxduo/nx_mjpeg.c`). Epic #49 **P5**: the
camera frame pipeline's (#46/#47) **eth_sink** -- the real network consumer of
the 30 fps JPEG path.

```
net mjpeg start [res]   # own the camera in JPEG + listen on :80 (res default qvga; qqvga|qvga|480x272|vga)
net mjpeg stop          # stop streaming, release the camera
net mjpeg stats         # client / sent frames+bytes / drops / errors
```

Open `http://<board-ip>/` in a browser for the live stream. One client at a time (N=1).

- **Explicit command**: `net mjpeg start` keeps the camera owned in JPEG (whether
  or not a client is connected) and listens on :80. Refused with `CAM_ERR_BUSY`
  while a GUIX preview / `camera stream` owns the DCMI (stop it first). Ownership
  is the existing single-owner model (`cam_ext_sink`), auto-exclusive with the
  camera stream / GUI (#73).
- **eth_sink = synchronous copy sink**: the pipeline consume() (producer-thread
  context) does no heavy HTTP send -- it memcpy's the JPEG into a private buffer
  (`.sdram.eth`, JPEG budget = 256 KB) and puts the pin immediately (in-flight is
  always 0, like the GUIX sink), so the camera's async teardown (DCMI overrun)
  stays correct. The HTTP server thread sends from the private buffer. A `buf_busy`
  flag gives a one-deep handoff; frames arriving mid-send are dropped (natural
  back-pressure to a slow client).
- **HTTP server thread (prio 14)**: listen -> accept -> skip GET -> multipart
  header -> per frame `--boundary` + `Content-Type: image/jpeg` + `Content-Length`
  + JPEG + `\r\n`. Relisten on disconnect; park on `stop` (the nx_echo idiom,
  bounded accept/send). Socket create + listen run in the thread (thread-only NetX).
- **MSS chunking**: `nx_tcp_socket_send` fragments a >MSS payload internally
  (double-spending the pool), so the JPEG is sent in MSS (= min(1400, peer MSS))
  chunks via `nx_packet_allocate` + `data_append` + `send`. `transmit_configure`
  caps the TX queue at 8 to protect the shared eth_pool. A frame is dropped at its
  start (not mid-frame) to keep the multipart stream intact.
- **Bandwidth**: the single 16-bit FMC SDRAM is shared by DCMI write + LTDC
  scanout + DMA2D + **ETH DMA** + the memcpy. JPEG is compressed so the ETH read
  bandwidth is small, but verify with `net mjpeg stats` + `camera stream stats`
  (dma fe/s, ovr).

## References

- RM0385 §38 (Ethernet) / §2.1.10 (ETH DMA bus)
- [eclipse-threadx/netxduo](https://github.com/eclipse-threadx/netxduo) (upstream, MIT)
- new HAL_ETH (`stm32f7xx_hal_eth.c`, descriptor model + zero-copy callbacks)
