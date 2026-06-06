/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cli_backend_uart.h
 * @brief   USART1 (ST-Link VCP) interrupt-driven transport for the shell (#7).
 *
 * A `struct cli_transport_api` implementation over the STM32 HAL UART in
 * interrupt mode, the first real backend after the dummy/loopback one.  It does
 * NOT initialise the UART clock/GPIO/baud: it reuses the handle already brought
 * up by the board (bsp.c VCP_UART_Init -> huart1) and only layers interrupt-
 * driven RX/TX ring buffers on top.
 *
 *   - RX: a 1-byte HAL_UART_Receive_IT, re-armed in HAL_UART_RxCpltCallback,
 *     pushes each byte into @ref cli_uart::rx_ring (filled by the USART1 ISR,
 *     drained by the shell thread via read() -- single producer/consumer).
 *   - TX: write() enqueues into @ref cli_uart::tx_ring and kicks a
 *     HAL_UART_Transmit_IT of the contiguous run; HAL_UART_TxCpltCallback
 *     advances the tail and starts the next chunk, or signals TX space.  The TX
 *     ring has two producers (the shell thread and the printf retarget _write),
 *     so its head/in-flight state is guarded by a short PRIMASK critical section.
 *
 * Concurrency rests on this port's ThreadX critical sections being PRIMASK-based
 * (TX_PORT_USE_BASEPRI is not defined; see port/threadx/tx_glue.c), so a USART1
 * ISR that calls cli_transport_notify_rx/tx (tx_event_flags_set) is safe at any
 * NVIC priority -- it can never preempt a ThreadX critical section.  The chosen
 * priority (5, above SysTick=14) is purely an echo-latency / overrun tuning.
 *
 * The byte ring itself (cli_uart_ring.h) is HAL/ThreadX-free and unit-tested on
 * the host (shell/test/test_uart_ring.c).  Clean-room design; no code reused.
 */
#ifndef CLI_BACKEND_UART_H
#define CLI_BACKEND_UART_H

#include <stddef.h>
#include <stdint.h>

#include "stm32f7xx_hal.h"   /* UART_HandleTypeDef */
#include "cli_instance.h"    /* struct cli_transport[_api], cli_transport_notify_* */
#include "cli_uart_ring.h"   /* struct cli_uart_ring + lock-free helpers */

#ifdef __cplusplus
extern "C" {
#endif

/* RX ring depth (bytes).  The ring holds CLI_UART_RX_BUFFER_SIZE-1 bytes; a burst
 * that outruns the shell thread overflows and is dropped + counted (req §9/§15). */
#ifndef CLI_UART_RX_BUFFER_SIZE
#define CLI_UART_RX_BUFFER_SIZE 256
#endif

/* TX ring depth (bytes).  write() returns a short/zero count when full, and the
 * core blocks on CLI_EVT_TX until TxComplete frees space (req §11). */
#ifndef CLI_UART_TX_BUFFER_SIZE
#define CLI_UART_TX_BUFFER_SIZE 512
#endif

/** Backend-private context (the `ctx` of a UART transport). */
struct cli_uart {
	UART_HandleTypeDef  *huart;   /**< bound at definition; clk/GPIO/baud done by bsp */
	struct cli_instance *sh;      /**< owning instance (cached from tr->sh in init) */

	/* RX: producer = USART1 ISR, consumer = shell thread (SPSC, lock-free). */
	struct cli_uart_ring rx_ring;
	uint8_t  rx_buf[CLI_UART_RX_BUFFER_SIZE];
	uint8_t  rx_byte;             /**< landing byte for HAL_UART_Receive_IT(1) */

	/* TX: producers = shell thread + _write, consumer = TxComplete ISR (PRIMASK). */
	struct cli_uart_ring tx_ring;
	uint8_t  tx_buf[CLI_UART_TX_BUFFER_SIZE];
	volatile uint8_t tx_in_flight; /**< a HAL_UART_Transmit_IT is in progress */
	uint16_t tx_chunk;             /**< length of the in-flight chunk (HAL Size is uint16_t) */

	volatile uint8_t enabled;      /**< enable() obtained HAL_OK and RX is armed */
	uint32_t rx_overrun;           /**< HW overrun (ORE) events; distinct from ring drops */
	uint32_t rx_rearm_fail;        /**< RX re-arm failures (surfaces silent RX stalls) */
};

/** The UART transport vtable (init/enable/write/read/uninit; update is NULL). */
extern const struct cli_transport_api cli_uart_api;

/**
 * Statically define a UART transport @p _name over the HAL handle @p _huart_ptr
 * (e.g. &huart1 for the VCP).  Bind it to an instance with
 * CLI_INSTANCE_DEFINE(inst, &_name, "prompt> ").
 */
#define CLI_BACKEND_UART_DEFINE(_name, _huart_ptr)                            \
	static struct cli_uart      _name##_ctx = { .huart = (_huart_ptr) };  \
	static struct cli_transport _name = { &cli_uart_api, NULL, &_name##_ctx }

/* Guard config overrides: a ring needs >= 2 bytes (one slot is the full/empty
 * sentinel), and the TX contiguous run (<= size-1) is passed to HAL_UART_Transmit_IT
 * whose Size is uint16_t, so the TX ring must not exceed 65536. */
_Static_assert(CLI_UART_RX_BUFFER_SIZE >= 2,      "CLI_UART_RX_BUFFER_SIZE must be >= 2");
_Static_assert(CLI_UART_TX_BUFFER_SIZE >= 2,      "CLI_UART_TX_BUFFER_SIZE must be >= 2");
_Static_assert(CLI_UART_TX_BUFFER_SIZE <= 65536u, "CLI_UART_TX_BUFFER_SIZE-1 must fit HAL uint16_t Size");

#ifdef __cplusplus
}
#endif

#endif /* CLI_BACKEND_UART_H */
