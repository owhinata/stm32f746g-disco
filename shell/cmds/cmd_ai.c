/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cmd_ai.c
 * @brief   `ai` shell command: on-device NN inference (issue #81, Epic #80).
 *
 * Front-end to the backend-agnostic nn API (port/nn/nn.h).  P1 scaffold covers:
 *   ai info        -- backend/model identity, I/O tensor shapes, arena size
 *   ai bench [n]   -- run inference n times on a fixed input, report latency
 * `ai run` / `ai stream` (live camera inference) land with the inference sink.
 *
 * bench timing is the nn layer's DWT CYCCNT (nn_last_cycles); with the `null`
 * backend run() is a no-op, so bench there measures only dispatch overhead --
 * useful to confirm the plumbing and the DWT before a real runtime is present.
 */
#include "cli.h"
#include "nn.h"
#include "nn_camera.h"        /* live camera inference (ai run / ai stream) */
#include "camera.h"           /* enum camera_res */
#include "sdram.h"            /* sdram_is_up() */

#include "stm32f7xx_hal.h"   /* HAL_RCC_GetHCLKFreq */

#include <stdint.h>
#include <string.h>

#define AI_BENCH_DEFAULT_ITERS 50u
#define AI_BENCH_MAX_ITERS     100000u

static const char *ai_dtype_name(uint8_t dt)
{
	switch (dt) {
	case NN_DTYPE_INT8:    return "int8";
	case NN_DTYPE_UINT8:   return "uint8";
	case NN_DTYPE_INT16:   return "int16";
	case NN_DTYPE_INT32:   return "int32";
	case NN_DTYPE_FLOAT32: return "f32";
	default:               return "none";
	}
}

/* Print one tensor line: "  in[0]  1x128x128x3 int8  q(s=0.007812 zp=0)  49152B" */
static void ai_print_tensor(struct cli_instance *sh, const char *tag, int idx,
                            const struct nn_tensor *t)
{
	char shape[48];

	/* Build "d0xd1xd2..." without snprintf-in-loop churn. */
	{
		int pos = 0;
		for (int i = 0; i < t->ndim && i < NN_MAX_DIMS; i++) {
			unsigned v = t->dims[i];
			char tmp[8];
			int tl = 0;
			if (v == 0) { tmp[tl++] = '0'; }
			while (v) { tmp[tl++] = (char)('0' + v % 10u); v /= 10u; }
			if (i && pos < (int)sizeof(shape) - 1) shape[pos++] = 'x';
			while (tl && pos < (int)sizeof(shape) - 1) shape[pos++] = tmp[--tl];
		}
		shape[pos] = '\0';
	}

	if (t->dtype == NN_DTYPE_INT8 || t->dtype == NN_DTYPE_UINT8) {
		/* scale as parts-per-million to avoid %f dependence in this path */
		uint32_t s_ppm = (uint32_t)(t->scale * 1000000.0f + 0.5f);
		cli_print(sh, "  %s[%d]  %s %s  q(s=0.%06lu zp=%ld)  %luB\r\n",
		          tag, idx, shape, ai_dtype_name(t->dtype),
		          (unsigned long)s_ppm, (long)t->zero_point,
		          (unsigned long)t->bytes);
	} else {
		cli_print(sh, "  %s[%d]  %s %s  %luB\r\n",
		          tag, idx, shape, ai_dtype_name(t->dtype),
		          (unsigned long)t->bytes);
	}
}

static int cmd_ai_info(struct cli_instance *sh, int argc, char **argv)
{
	const struct nn_backend_info *bi = nn_backend();
	struct nn_model *m = NULL;
	int rc;

	(void)argc; (void)argv;

	cli_print(sh, "backend : %s (%s)\r\n",
	          bi->name, (bi->version && bi->version[0]) ? bi->version : "-");

	rc = nn_model_open(&m);
	if (rc != 0) {
		cli_error(sh, "model open failed (%d)\r\n", rc);
		return 1;
	}

	cli_print(sh, "model   : %s\r\n", nn_model_name(m));
	cli_print(sh, "arena   : %lu B (activations)\r\n",
	          (unsigned long)nn_activations_bytes(m));

	for (int i = 0; i < nn_input_count(m); i++)
		ai_print_tensor(sh, "in", i, nn_input(m, i));
	for (int i = 0; i < nn_output_count(m); i++)
		ai_print_tensor(sh, "out", i, nn_output(m, i));

	{
		uint32_t c = nn_last_cycles(m);
		if (c)
			cli_print(sh, "last    : %lu cyc\r\n", (unsigned long)c);
	}
	return 0;
}

static int cmd_ai_bench(struct cli_instance *sh, int argc, char **argv)
{
	struct nn_model *m = NULL;
	uint32_t iters = AI_BENCH_DEFAULT_ITERS;
	uint32_t cmin = 0xFFFFFFFFu, cmax = 0, done = 0;
	uint64_t csum = 0;
	uint32_t hclk = HAL_RCC_GetHCLKFreq();
	uint32_t mhz = hclk / 1000000u ? hclk / 1000000u : 1u;
	int rc;

	if (argc > 1) {
		/* parse a small decimal iteration count */
		uint32_t v = 0;
		const char *p = argv[1];
		while (*p >= '0' && *p <= '9') { v = v * 10u + (uint32_t)(*p - '0'); p++; }
		if (*p != '\0' || v == 0) {
			cli_error(sh, "usage: ai bench [iterations]\r\n");
			return 1;
		}
		iters = v > AI_BENCH_MAX_ITERS ? AI_BENCH_MAX_ITERS : v;
	}

	rc = nn_model_open(&m);
	if (rc != 0) {
		cli_error(sh, "model open failed (%d)\r\n", rc);
		return 1;
	}

	/* The input buffer may live in .sdram.ai (bank3) -- ensure SDRAM is up. */
	if (!sdram_is_up()) {
		cli_error(sh, "SDRAM not up (needed for the NN arena)\r\n");
		return 1;
	}

	/* Claim the single inference session so bench cannot run concurrently with a
	 * live `ai stream`/`ai run` (or another bench) on the non-reentrant model. */
	if (nn_session_try_acquire() != 0) {
		cli_error(sh, "NN busy (a stream/run or another bench is active)\r\n");
		return 1;
	}

	/* Fixed input: fill each input tensor with a constant so runs are deterministic. */
	for (int i = 0; i < nn_input_count(m); i++) {
		struct nn_tensor *t = nn_input(m, i);
		if (t && t->data)
			memset(t->data, 0, t->bytes);
	}

	cli_print(sh, "bench %s x%lu ...\r\n", nn_model_name(m), (unsigned long)iters);

	for (uint32_t i = 0; i < iters; i++) {
		uint32_t c;
		if (cli_cancel_requested(sh)) {
			cli_print(sh, "^C after %lu\r\n", (unsigned long)done);
			break;
		}
		rc = nn_run(m);
		if (rc != 0) {
			cli_error(sh, "nn_run failed (%d) at %lu\r\n", rc, (unsigned long)i);
			nn_session_release();
			return 1;
		}
		c = nn_last_cycles(m);
		if (c < cmin) cmin = c;
		if (c > cmax) cmax = c;
		csum += c;
		done++;
	}
	nn_session_release();

	if (done == 0) {
		cli_warn(sh, "no runs completed\r\n");
		return 0;
	}
	{
		uint32_t cavg = (uint32_t)(csum / done);
		cli_print(sh, "cycles  min %lu  avg %lu  max %lu  (%lu runs)\r\n",
		          (unsigned long)cmin, (unsigned long)cavg, (unsigned long)cmax,
		          (unsigned long)done);
		cli_print(sh, "latency min %lu  avg %lu  max %lu us  (@%lu MHz)\r\n",
		          (unsigned long)(cmin / mhz), (unsigned long)(cavg / mhz),
		          (unsigned long)(cmax / mhz), (unsigned long)mhz);
		if (cmin == 0)
			cli_warn(sh, "note: 0-cycle runs -- DWT CYCCNT may be unavailable "
			             "or backend='null' (no real inference)\r\n");
	}
	return 0;
}

/* ---- live camera inference (ai run / ai stream) --------------------------- */

static enum camera_res ai_parse_res(const char *s, int *ok)
{
	*ok = 1;
	if (!strcmp(s, "qqvga"))                       return CAM_RES_QQVGA;
	if (!strcmp(s, "qvga"))                        return CAM_RES_QVGA;
	if (!strcmp(s, "480x272") || !strcmp(s, "480")) return CAM_RES_480x272;
	*ok = 0;
	return CAM_RES_QVGA;
}

static void ai_print_stats(struct cli_instance *sh, const struct nn_camera_stats *s)
{
	cli_print(sh, "running %d  res %u  frames %lu  drops %lu  infers %lu  err %lu\r\n",
	          (int)s->running, (unsigned)s->res,
	          (unsigned long)s->frames, (unsigned long)s->drops,
	          (unsigned long)s->infers, (unsigned long)s->errors);
	cli_print(sh, "last %lu us  ~%lu.%02lu inf/s  det %lu\r\n",
	          (unsigned long)s->last_us,
	          (unsigned long)(s->fps_x100 / 100u), (unsigned long)(s->fps_x100 % 100u),
	          (unsigned long)s->detections);
}

/* Print the latest detections as percent-of-frame boxes (avoids %f). */
static void ai_print_dets(struct cli_instance *sh)
{
	struct bf_det d[BF_MAX_DET];
	int n = nn_camera_dets_get(d, BF_MAX_DET);

	for (int i = 0; i < n; i++)
		cli_print(sh, "  face[%d]  x %ld%% y %ld%% w %ld%% h %ld%%  score %ld%%\r\n",
		          i, (long)(d[i].x * 100.0f), (long)(d[i].y * 100.0f),
		          (long)(d[i].w * 100.0f), (long)(d[i].h * 100.0f),
		          (long)(d[i].score * 100.0f));
}

static int cmd_ai_run(struct cli_instance *sh, int argc, char **argv)
{
	struct nn_camera_stats s;
	int rc;

	(void)argc; (void)argv;

	if (nn_camera_running()) {
		cli_error(sh, "stream already running -- use 'ai stream stats'\r\n");
		return 1;
	}
	rc = nn_camera_start(CAM_RES_QVGA);
	if (rc != 0) {
		cli_error(sh, "start failed (%d): camera busy or no sensor?\r\n", rc);
		return 1;
	}
	/* Wait (bounded ~2 s, cancellable) for one inference to complete. */
	for (int i = 0; i < 200; i++) {
		nn_camera_stats_get(&s);
		if (s.infers >= 1)
			break;
		if (cli_sleep(sh, 10))          /* 10 ticks ~= 10 ms; non-zero = Ctrl+C */
			break;
	}
	nn_camera_stats_get(&s);
	nn_camera_stop();

	if (s.infers < 1) {
		cli_warn(sh, "no inference completed (no frames? camera connected?)\r\n");
		return 0;
	}
	cli_print(sh, "inference: %lu us, %lu detections\r\n",
	          (unsigned long)s.last_us, (unsigned long)s.detections);
	ai_print_dets(sh);
	return 0;
}

static int cmd_ai_stream_start(struct cli_instance *sh, int argc, char **argv)
{
	enum camera_res res = CAM_RES_QVGA;
	int rc;

	if (argc > 1) {
		int ok;
		res = ai_parse_res(argv[1], &ok);
		if (!ok) {
			cli_error(sh, "usage: ai stream start [qqvga|qvga|480x272]\r\n");
			return 1;
		}
	}
	rc = nn_camera_start(res);
	if (rc != 0) {
		cli_error(sh, "start failed (%d): camera busy / no sensor / SDRAM down?\r\n", rc);
		return 1;
	}
	cli_print(sh, "inference stream started (worker prio 18, best-effort)\r\n");
	return 0;
}

static int cmd_ai_stream_stop(struct cli_instance *sh, int argc, char **argv)
{
	int rc;

	(void)argc; (void)argv;
	rc = nn_camera_stop();
	if (rc == -1) {
		cli_warn(sh, "not running\r\n");
		return 0;
	}
	if (rc != 0) {
		cli_error(sh, "stop timed out (%d)\r\n", rc);
		return 1;
	}
	cli_print(sh, "stopped\r\n");
	return 0;
}

static int cmd_ai_stream_stats(struct cli_instance *sh, int argc, char **argv)
{
	struct nn_camera_stats s;

	(void)argc; (void)argv;
	nn_camera_stats_get(&s);
	ai_print_stats(sh, &s);
	ai_print_dets(sh);
	cli_print(sh, "maxscore %ld  norm %s  (diagnostic)\r\n",
	          (long)(blazeface_last_max_score() * 100.0f),
	          nn_camera_get_norm() ? "[-1,1]" : "[0,1]");
	return 0;
}

static int cmd_ai_norm(struct cli_instance *sh, int argc, char **argv)
{
	if (argc > 1) {
		if (!strcmp(argv[1], "0"))      nn_camera_set_norm(0);
		else if (!strcmp(argv[1], "1")) nn_camera_set_norm(1);
		else { cli_error(sh, "usage: ai norm <0|1>  (1=[-1,1], 0=[0,1])\r\n"); return 1; }
	}
	cli_print(sh, "norm = %s\r\n",
	          nn_camera_get_norm() ? "[-1,1] (signed)" : "[0,1] (unsigned)");
	return 0;
}

CLI_SUBCMD_SET_CREATE(ai_stream_subcmds,
	CLI_CMD_ARG(start, NULL, "start live inference [qqvga|qvga|480x272]",
	            cmd_ai_stream_start, 1, 1),
	CLI_CMD(stop,  NULL, "stop live inference", cmd_ai_stream_stop),
	CLI_CMD(stats, NULL, "inference rate / latency / drops", cmd_ai_stream_stats),
	CLI_SUBCMD_SET_END);

CLI_SUBCMD_SET_CREATE(ai_subcmds,
	CLI_CMD(info, NULL, "backend / model / tensor shapes / arena", cmd_ai_info),
	CLI_CMD_ARG(bench, NULL, "run inference [n] times, report latency",
	            cmd_ai_bench, 1, 1),
	CLI_CMD(run, NULL, "single-shot inference on one camera frame", cmd_ai_run),
	CLI_CMD(stream, ai_stream_subcmds, "live camera inference", NULL),
	CLI_CMD_ARG(norm, NULL, "float input norm <0|1> (1=[-1,1], 0=[0,1])",
	            cmd_ai_norm, 1, 1),
	CLI_SUBCMD_SET_END);

CLI_CMD_REGISTER(ai, ai_subcmds,
	"on-device NN inference (issue #81)", NULL, 1, 0);
