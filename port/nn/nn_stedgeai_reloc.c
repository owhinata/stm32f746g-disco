/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_stedgeai_reloc.c
 * @brief   X-CUBE-AI relocatable-network nn backend (issue #92, Epic #80 P5).
 *
 * Runtime model swap for the ST Edge AI Core runtime: instead of baking one model
 * into Flash (nn_stedgeai.c, STAI API), this backend loads a position-independent
 * `network_rel.bin` (PIC code + embedded weights, produced offline by
 * `stedgeai generate --relocatable --target stm32f7`) from the SD card at runtime
 * and runs it XIP via the ST host loader (ai_reloc_network.c) + the legacy
 * ai_rel_network_* API.  Same "SD swap, no reflash" capability the tflm backend
 * has (issue #89 P2), but on the X-CUBE-AI runtime -- so it plugs into the same
 * nn vtable load_region()/reload() path and cmd_ai's `ai model load`.
 *
 * SD-only: there is NO built-in model.  open() returns an empty handle (0 in/out);
 * `ai model load <bin>` installs a model; `ai model builtin` unloads it.
 *
 * Memory (all in .sdram.ai / .sdram.ai.model, FMC bank3):
 *   - g_model_slot[2]  (.sdram.ai.model, bank3 UPPER half -> bsp.c MPU region2 makes
 *                       it instruction-fetchable): the .bin double-slot.  XIP runs the
 *                       PIC code IN PLACE from here, so a live model's code is in the
 *                       active slot -- a reload reads the new .bin into the INACTIVE
 *                       slot and only flips on success (transactional; never clobbers
 *                       the running code).
 *   - g_rt_ram[2]      (.sdram.ai, XN): XIP RW image (data/got/bss) per slot.
 *   - g_acts           (.sdram.ai, XN): the single activation arena (I/O tensors are
 *                       carved from it -- the model is generated allocate-inputs/outputs).
 *
 * Cache: XIP executes CPU-written code out of cacheable SDRAM, and the ST loader does
 * NO cache maintenance in XIP mode -- so reload() cleans D-cache + invalidates I-cache
 * over the .bin slot before install (Cortex-M7 I/D caches are separate; I-fetch does
 * not snoop D-cache).
 *
 * The ST loader (.c) + the SD-loaded .bin are ST-SLA / not committed (public repo);
 * see CMakeLists.txt (CONFIG_NN_BACKEND=stedgeai_reloc) and .gitignore.
 */
#include "nn.h"
#include "nn_backend.h"

#include "stm32f7xx_hal.h"   /* SCB_CleanDCache_by_Addr / SCB_InvalidateICache / __DSB / __ISB */

#include <ai_reloc_network.h>  /* ai_rel_network_* API, struct ai_reloc_rt_ctx, XIP mode */
#include <ai_platform.h>       /* ai_error, ai_buffer, ai_network_report, AI_BUFFER_ / AI_SHAPE_ macros */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ---- fixed SDRAM budget (bank3) -------------------------------------------
 * BlazeFace-front 128: .bin ~208 KB, acts_sz 320 KB, rt_ram_xip ~8 KB.  Caps give
 * headroom for a larger swapped-in model and are bounds-checked at load time.
 * Layout constraint (ldscript ASSERTs): the two model slots (upper half) must fit
 * in bank3's upper 1 MB; g_rt_ram + g_acts + nn_camera's staging (lower half) must
 * fit in the lower 1 MB. */
#define RELOC_MODEL_SLOT_CAP  (448u * 1024u)   /* per slot; 2 x 448K = 896K < 1 MB */
#define RELOC_RT_RAM_CAP      (48u  * 1024u)   /* per slot; BlazeFace RW image (data+got+bss) ~36K + headroom */
#define RELOC_ACTS_CAP        (384u * 1024u)   /* acts_sz 320K + headroom */

/* Executable model .bin double-slot: bank3 UPPER half (bsp.c region2 = exec). */
static uint8_t g_model_slot[2][RELOC_MODEL_SLOT_CAP]
	__attribute__((section(".sdram.ai.model"), aligned(32)));
/* XIP RW image (data/got/bss) per slot, and the activation arena: bank3 lower half (XN). */
static uint8_t g_rt_ram[2][RELOC_RT_RAM_CAP]
	__attribute__((section(".sdram.ai"), aligned(32)));
static uint8_t g_acts[RELOC_ACTS_CAP]
	__attribute__((section(".sdram.ai"), aligned(32)));

/* ---- backend singleton state ---------------------------------------------- */
struct reloc_model {
	ai_handle         hdl;      /* active network handle; NULL == no model loaded */
	int               slot;     /* active .bin slot (0/1); -1 == none             */
	ai_network_report report;   /* I/O descriptors (report.inputs/outputs feed run) */
	struct nn_tensor  in[NN_MAX_IO];
	struct nn_tensor  out[NN_MAX_IO];
	int               n_in, n_out;
	uint32_t          acts_bytes;
	char              name[40];
};
static struct reloc_model g_reloc;

/* =====================================================================
 * Bounded .bin verifier (issue #92, codex BLOCKING).
 *
 * The ST loader takes no length and blindly follows in-object offsets
 * (rt_get_info / install / ram_update / entry dispatch), so a short/corrupt .bin
 * could OOB-read, OOB-write (the .rel table writes ram_addr+off(add)), or jump to a
 * bad PC.  Since XIP EXECUTES code from this buffer, we self-parse the header and
 * validate every offset the loader dereferences BEFORE the first ST API call.  The
 * header layout mirrors ai_reloc_network.c's private structs (kept in sync here);
 * validated field-by-field against a real stedgeai-generated .bin.
 * ===================================================================== */
#define RELOC_MAGIC        0x4E49424Eu   /* "NBIN" */
#define RELOC_MASK_OFFSET  0x0FFFFFFFu
#define RELOC_OFF(x)       ((uint32_t)((x) & RELOC_MASK_OFFSET))
#define RELOC_FLASH_BASE   0x20000000u
#define RELOC_RAM_BASE     0x80000000u
#define RELOC_CLASS(v)     ((v) & 0xF0000000u)
#define RELOC_IS_RAM(v)    (RELOC_CLASS(v) == RELOC_RAM_BASE)
#define RELOC_IS_FLASH(v)  (RELOC_CLASS(v) == RELOC_FLASH_BASE)

struct reloc_sec_info {
	uint32_t data_start, data_end, data_data;
	uint32_t bss_start, bss_end;
	uint32_t got_start, got_end;
	uint32_t rel_start, rel_end;
	uint32_t weights_start, weights_end;
};
struct reloc_net_entries {
	uint32_t create, init, init_v2, run, report, error, destroy, forward;
	uint32_t obs_reg, obs_unreg, obs_node, ctx;
};
struct reloc_bin_hdr {
	uint32_t magic, flags;
	struct reloc_sec_info    sect;
	struct reloc_net_entries vec;
};

/* Read a 32-bit little-endian word from a (4-byte-aligned) file offset. */
static uint32_t reloc_rd32(const uint8_t *buf, uint32_t off)
{
	uint32_t v;
	memcpy(&v, buf + off, sizeof(v));
	return v;
}

/* Return 0 if @p buf[0..len) is a well-formed, CM7/FPU/hard reloc .bin whose every
 * loader-dereferenced offset stays within [len] (file) or the RT-RAM cap (RW image);
 * <0 otherwise.  @p rtram_cap bounds the XIP RW image. */
static int reloc_bin_verify(const uint8_t *buf, uint32_t len, uint32_t rtram_cap)
{
	const struct reloc_bin_hdr *h;
	uint32_t data_s, data_e, data_d, bss_s, bss_e, got_s, got_e, rel_s, rel_e, w_s, w_e;
	uint32_t req_ram, tail;

	if (len < sizeof(struct reloc_bin_hdr)) return -1;
	if ((uintptr_t)buf & 0x3u)              return -1;

	h = (const struct reloc_bin_hdr *)buf;
	if (h->magic != RELOC_MAGIC) return -1;

	/* variant (hdr.flags): b11..0 CPUID, b12 FPU, b14..13 float-abi (2=hard). */
	if ((h->flags & 0xFFFu) != 0xC27u)        return -1;   /* Cortex-M7 */
	if (((h->flags >> 12) & 0x1u) != 0x1u)    return -1;   /* FPU used */
	if (((h->flags >> 13) & 0x3u) != 0x2u)    return -1;   /* hard float-abi */

	/* section-offset high-nibble class: RW image is RAM-coded, file image FLASH-coded. */
	if (!RELOC_IS_RAM(h->sect.data_start) || !RELOC_IS_RAM(h->sect.data_end)   ||
	    !RELOC_IS_RAM(h->sect.got_start)  || !RELOC_IS_RAM(h->sect.got_end)    ||
	    !RELOC_IS_RAM(h->sect.bss_start)  || !RELOC_IS_RAM(h->sect.bss_end))
		return -1;
	if (!RELOC_IS_FLASH(h->sect.data_data)    ||
	    !RELOC_IS_FLASH(h->sect.rel_start)     || !RELOC_IS_FLASH(h->sect.rel_end) ||
	    !RELOC_IS_FLASH(h->sect.weights_start) || !RELOC_IS_FLASH(h->sect.weights_end))
		return -1;

	data_s = RELOC_OFF(h->sect.data_start); data_e = RELOC_OFF(h->sect.data_end);
	data_d = RELOC_OFF(h->sect.data_data);
	bss_s  = RELOC_OFF(h->sect.bss_start);  bss_e  = RELOC_OFF(h->sect.bss_end);
	got_s  = RELOC_OFF(h->sect.got_start);  got_e  = RELOC_OFF(h->sect.got_end);
	rel_s  = RELOC_OFF(h->sect.rel_start);  rel_e  = RELOC_OFF(h->sect.rel_end);
	w_s    = RELOC_OFF(h->sect.weights_start); w_e = RELOC_OFF(h->sect.weights_end);

	/* all loader word-reads are 32-bit: every section offset must be 4-byte aligned. */
	if ((data_d | rel_s | rel_e | w_s | w_e | data_s | data_e | got_s | got_e |
	     bss_s | bss_e) & 0x3u)
		return -1;

	/* file-resident sections monotone and within len.  We do NOT require the last
	 * section to end exactly at len (the ST loader tolerates trailing bytes, and the
	 * unreferenced slot tail is zero-filled) -- only that nothing claims to run past EOF. */
	if (!(data_d <= rel_s && rel_s <= rel_e && rel_e <= len)) return -1;
	if (!(w_s <= w_e && w_e <= len))                          return -1;
	tail = (rel_e > w_e) ? rel_e : w_e;
	if (tail > len) return -1;

	/* RW image (data|got|bss) monotone; total RW must fit the RT-RAM slot. */
	if (!(data_s <= data_e && data_e <= got_s && got_s <= got_e &&
	      got_e <= bss_s && bss_s <= bss_e))
		return -1;
	req_ram = (bss_e + 3u) & ~3u;
	if (req_ram > rtram_cap) return -1;

	/* install copies file[data_data .. data_data + off(bss_start)) into the RT RAM. */
	if ((uint64_t)data_d + bss_s > len) return -1;

	/* entry points dispatched via the r9 trampoline: FLASH-class, Thumb, and inside the
	 * code/rodata region [sizeof(hdr), data_data) -- rejects trivial bad jump targets
	 * (offset 0 / into the header) that pass a Thumb-only check. */
	{
		const uint32_t ent[] = { h->vec.create, h->vec.init_v2, h->vec.run,
		                         h->vec.report, h->vec.error, h->vec.destroy };
		for (unsigned i = 0; i < sizeof(ent) / sizeof(ent[0]); i++) {
			uint32_t e = RELOC_OFF(ent[i]);
			if (!RELOC_IS_FLASH(ent[i]))                    return -1; /* code is FLASH-class */
			if (!(e & 0x1u))                                return -1; /* Thumb bit */
			if ((e & ~0x1u) < sizeof(struct reloc_bin_hdr)) return -1; /* past the header */
			if ((e & ~0x1u) >= data_d)                      return -1; /* inside .text/.rodata */
		}
	}

	/* RT context: RAM-class offset within the RW image, .data file image within len. */
	{
		uint32_t ctx = RELOC_OFF(h->vec.ctx);
		if (!RELOC_IS_RAM(h->vec.ctx))                        return -1;
		if (ctx & 0x3u)                                       return -1;
		if (ctx < data_s)                                     return -1;
		/* ctx must sit within the INITIALIZED image (data+got, ends at bss_start), since
		 * we read its act_size const from the .data file image the loader copies. */
		if ((uint64_t)ctx + sizeof(struct ai_reloc_rt_ctx) > bss_s) return -1;
		if ((uint64_t)data_d + (ctx - data_s) + sizeof(struct ai_reloc_rt_ctx) > len)
			return -1;
	}

	/* GOT file image: each nonzero pointer RAM-coded within RT RAM or FLASH-coded within len. */
	if (got_s >= data_s && got_e >= got_s) {
		uint32_t base = data_d + (got_s - data_s);
		uint32_t n    = got_e - got_s;
		if ((uint64_t)base + n > len) return -1;
		for (uint32_t o = 0; o + 4u <= n; o += 4u) {
			uint32_t v = reloc_rd32(buf, base + o);
			if (v == 0u) continue;
			if (RELOC_IS_RAM(v))       { if (RELOC_OFF(v) >= bss_e) return -1; }
			else if (RELOC_IS_FLASH(v)) { if (RELOC_OFF(v) >= len)  return -1; }
			else                        return -1;
		}
	}

	/* .rel table: each entry addresses a RW word the loader will write. */
	for (uint32_t off = rel_s; off + 4u <= rel_e; off += 4u) {
		uint32_t add = reloc_rd32(buf, off);
		if (!RELOC_IS_RAM(add))               return -1;
		if ((uint64_t)RELOC_OFF(add) + 4u > bss_e) return -1;
	}

	return 0;
}

/* =====================================================================
 * ai_buffer -> nn_tensor mapping
 * ===================================================================== */
static uint8_t reloc_dtype(ai_buffer_format fmt)
{
	uint32_t type = AI_BUFFER_FMT_GET_TYPE(fmt);
	uint32_t bits = AI_BUFFER_FMT_GET_BITS(fmt);
	uint32_t sign = AI_BUFFER_FMT_GET_SIGN(fmt);

	if (type == AI_BUFFER_FMT_TYPE_FLOAT && bits == 32) return NN_DTYPE_FLOAT32;
	if (type == AI_BUFFER_FMT_TYPE_Q && bits == 8)  return sign ? NN_DTYPE_INT8 : NN_DTYPE_UINT8;
	if (type == AI_BUFFER_FMT_TYPE_Q && bits == 16 && sign) return NN_DTYPE_INT16;
	if (type == AI_BUFFER_FMT_TYPE_Q && bits == 32 && sign) return NN_DTYPE_INT32;
	return NN_DTYPE_NONE;
}

/* The legacy runtime stores every shape as 4-D BCWH; reconstruct the model's logical
 * shape the same way nn_stedgeai.c (STAI) exposes it, so blazeface.c / nn_camera see an
 * identical tensor: input keeps NHWC 4-D [1,H,W,C]; the 3-D outputs (width==1) collapse
 * to [1,H,C].  Quantization is left at 0 (BlazeFace I/O is float32; an int8-I/O model
 * would additionally read ai_buffer.meta_info -- out of P5 scope). */
static int reloc_map(struct nn_tensor *t, const ai_buffer *b)
{
	uint32_t B = AI_BUFFER_SHAPE_ELEM(b, AI_SHAPE_BATCH);
	uint32_t C = AI_BUFFER_SHAPE_ELEM(b, AI_SHAPE_CHANNEL);
	uint32_t W = AI_BUFFER_SHAPE_ELEM(b, AI_SHAPE_WIDTH);
	uint32_t H = AI_BUFFER_SHAPE_ELEM(b, AI_SHAPE_HEIGHT);
	uint32_t bits = AI_BUFFER_FMT_GET_BITS(b->format);

	if (!b->data || bits == 0)
		return -1;

	t->data  = b->data;
	t->dtype = reloc_dtype(b->format);
	t->scale = 0.0f;
	t->zero_point = 0;

	if (W <= 1u) {                       /* 3-D logical tensor [1,H,C] */
		t->ndim = 3;
		t->dims[0] = (uint16_t)(B ? B : 1);
		t->dims[1] = (uint16_t)(H ? H : 1);
		t->dims[2] = (uint16_t)(C ? C : 1);
		t->dims[3] = 1;
	} else {                              /* 4-D NHWC [1,H,W,C] */
		t->ndim = 4;
		t->dims[0] = (uint16_t)(B ? B : 1);
		t->dims[1] = (uint16_t)(H ? H : 1);
		t->dims[2] = (uint16_t)W;
		t->dims[3] = (uint16_t)(C ? C : 1);
	}

	t->bytes = (B ? B : 1u) * (C ? C : 1u) * (W ? W : 1u) * (H ? H : 1u) * (bits / 8u);
	return 0;
}

static void reloc_set_name(const char *name)
{
	/* "sd:<basename>" for display parity with the tflm SD path (issue #89). */
	size_t n;
	g_reloc.name[0] = 's'; g_reloc.name[1] = 'd'; g_reloc.name[2] = ':';
	for (n = 0; name && name[n] && n + 4u < sizeof(g_reloc.name); n++)
		g_reloc.name[3 + n] = name[n];
	g_reloc.name[3 + n] = '\0';
}

/* =====================================================================
 * Build a model instance from slot @p slot's already-verified .bin.
 * On success sets *hdl_out + fills the report/tensor out-params; on failure returns
 * <0 with *hdl_out set to any partially-created handle (caller destroys it).
 * ===================================================================== */
static int reloc_build(int slot, uint32_t len, ai_handle *hdl_out,
                       ai_network_report *rep, struct nn_tensor *in, struct nn_tensor *out,
                       int *n_in, int *n_out, uint32_t *acts_out)
{
	uint8_t                        *obj   = g_model_slot[slot];
	void                           *rtram = g_rt_ram[slot];
	const struct reloc_bin_hdr     *h     = (const struct reloc_bin_hdr *)obj;
	ai_error                        err;
	ai_handle                       hdl = AI_HANDLE_NULL;
	ai_handle                       weights[1];
	ai_handle                       acts[1];
	uint32_t                        data_d, data_s, ctx_off, rt_ram_xip, acts_sz;
	const struct ai_reloc_rt_ctx   *ctxf;

	*hdl_out = AI_HANDLE_NULL;

	/* Derive the sizing directly from the already-verified header + ctx file image.
	 * We deliberately do NOT call ai_rel_network_rt_get_info(): it computes the ctx
	 * with `bin + off(data_data)` where bin is a `struct*`, i.e. offset-SCALED-by-100
	 * pointer arithmetic -> a wild OOB read before install (codex).  Everything it would
	 * return we compute safely here from bytes reloc_bin_verify() has already bounded. */
	data_d     = RELOC_OFF(h->sect.data_data);
	data_s     = RELOC_OFF(h->sect.data_start);
	ctx_off    = RELOC_OFF(h->vec.ctx);
	rt_ram_xip = (RELOC_OFF(h->sect.bss_end) + 3u) & ~3u;      /* data+got+bss RW image */
	ctxf       = (const struct ai_reloc_rt_ctx *)(obj + data_d + (ctx_off - data_s));
	acts_sz    = ctxf->act_size;                                /* const in the .data image */

	if (acts_sz > RELOC_ACTS_CAP)      return -2;
	if (rt_ram_xip > RELOC_RT_RAM_CAP) return -3;

	/* XIP runs code out of cacheable SDRAM: flush the CPU-written .bin to SDRAM and
	 * drop stale I-cache lines before the first fetch (the loader does neither). */
	SCB_CleanDCache_by_Addr((uint32_t *)obj, (int32_t)((len + 31u) & ~31u));
	__DSB();
	SCB_InvalidateICache();
	__ISB();

	err = ai_rel_network_load_and_create(obj, rtram, RELOC_RT_RAM_CAP,
	                                     AI_RELOC_RT_LOAD_MODE_XIP, &hdl);
	*hdl_out = hdl;                          /* publish so the caller destroys any handle */
	if (err.type != AI_ERROR_NONE || hdl == AI_HANDLE_NULL)
		return -4;

	weights[0] = (ai_handle)(obj + RELOC_OFF(h->sect.weights_start)); /* embedded weights */
	acts[0]    = (ai_handle)g_acts;
	if (!ai_rel_network_init(hdl, weights, acts))
		return -5;

	if (!ai_rel_network_get_report(hdl, rep))
		return -6;
	if (rep->n_inputs != 1 || rep->n_outputs < 1 || rep->n_outputs > NN_MAX_IO ||
	    !rep->inputs || !rep->outputs)
		return -7;

	for (int i = 0; i < rep->n_inputs; i++)
		if (reloc_map(&in[i], &rep->inputs[i]) != 0) return -8;
	for (int i = 0; i < rep->n_outputs; i++)
		if (reloc_map(&out[i], &rep->outputs[i]) != 0) return -8;

	*n_in    = rep->n_inputs;
	*n_out   = rep->n_outputs;
	*acts_out = acts_sz;
	return 0;
}

/* =====================================================================
 * nn backend vtable
 * ===================================================================== */
static int reloc_bk_init(void)
{
	return 0;   /* the loader is stateless until load_and_create(); nothing global to init */
}

static int reloc_bk_open(void **impl_out)
{
	/* SD-only: start empty.  A model appears only after `ai model load`. */
	g_reloc.hdl   = AI_HANDLE_NULL;
	g_reloc.slot  = -1;
	g_reloc.n_in  = 0;
	g_reloc.n_out = 0;
	g_reloc.acts_bytes = 0;
	strcpy(g_reloc.name, "(none)");
	*impl_out = &g_reloc;
	return 0;
}

static void reloc_bk_close(void *impl)
{
	struct reloc_model *m = impl;
	if (m->hdl != AI_HANDLE_NULL) {
		(void)ai_rel_network_destroy(m->hdl);
		m->hdl = AI_HANDLE_NULL;
	}
	m->slot = -1;
	m->n_in = m->n_out = 0;
}

static const char *reloc_bk_name(void *impl)     { return ((struct reloc_model *)impl)->name; }
static int reloc_bk_in_count(void *impl)         { return ((struct reloc_model *)impl)->n_in; }
static int reloc_bk_out_count(void *impl)        { return ((struct reloc_model *)impl)->n_out; }
static uint32_t reloc_bk_acts(void *impl)        { return ((struct reloc_model *)impl)->acts_bytes; }

static struct nn_tensor *reloc_bk_input(void *impl, int idx)
{
	struct reloc_model *m = impl;
	return (idx >= 0 && idx < m->n_in) ? &m->in[idx] : NULL;
}
static struct nn_tensor *reloc_bk_output(void *impl, int idx)
{
	struct reloc_model *m = impl;
	return (idx >= 0 && idx < m->n_out) ? &m->out[idx] : NULL;
}

static int reloc_bk_run(void *impl)
{
	struct reloc_model *m = impl;
	if (m->hdl == AI_HANDLE_NULL)
		return -1;                       /* no model loaded */
	return (ai_rel_network_run(m->hdl, m->report.inputs, m->report.outputs) >= 1) ? 0 : -1;
}

/* Return the INACTIVE .bin slot as the writable staging buffer for an SD read. */
static int reloc_bk_load_region(void **buf, uint32_t *cap)
{
	int s = (g_reloc.slot == 0) ? 1 : 0;   /* slot -1/1 -> 0, slot 0 -> 1 */
	*buf = g_model_slot[s];
	*cap = RELOC_MODEL_SLOT_CAP;
	return 0;
}

/* Rebuild the model from an SD-loaded .bin (transactional).  @p data==NULL unloads.
 * *impl_out is ALWAYS &g_reloc (non-NULL): "no model" is the internal hdl==NULL state
 * (open stays true; model_name "(none)"; run() fails until a model loads). */
static int reloc_bk_reload(const void *data, uint32_t len, const char *name, void **impl_out)
{
	int               new_slot;
	ai_handle         new_hdl = AI_HANDLE_NULL, old_hdl;
	ai_network_report new_rep;
	struct nn_tensor  new_in[NN_MAX_IO], new_out[NN_MAX_IO];
	int               nin = 0, nout = 0;
	uint32_t          acts = 0;
	int               rc;

	*impl_out = &g_reloc;

	if (data == NULL) {                      /* unload -> "(none)" */
		if (g_reloc.hdl != AI_HANDLE_NULL) {
			(void)ai_rel_network_destroy(g_reloc.hdl);
			g_reloc.hdl = AI_HANDLE_NULL;
		}
		g_reloc.slot = -1;
		g_reloc.n_in = g_reloc.n_out = 0;
		g_reloc.acts_bytes = 0;
		strcpy(g_reloc.name, "(none)");
		return 0;
	}

	/* data must be the inactive slot returned by load_region(). */
	if (data == g_model_slot[0])      new_slot = 0;
	else if (data == g_model_slot[1]) new_slot = 1;
	else                              return -1;
	if (new_slot == g_reloc.slot)     return -1;   /* would clobber the live model */

	if (len == 0 || len > RELOC_MODEL_SLOT_CAP)
		return -1;

	/* Zero the slot tail so the loader can never follow an offset into stale bytes,
	 * then validate every offset it will dereference before touching the ST API. */
	memset(g_model_slot[new_slot] + len, 0, RELOC_MODEL_SLOT_CAP - len);
	if (reloc_bin_verify(g_model_slot[new_slot], len, RELOC_RT_RAM_CAP) != 0)
		return -1;                       /* keep the old model (unchanged) */

	rc = reloc_build(new_slot, len, &new_hdl, &new_rep, new_in, new_out, &nin, &nout, &acts);
	if (rc != 0) {
		if (new_hdl != AI_HANDLE_NULL)
			(void)ai_rel_network_destroy(new_hdl);
		return rc;                       /* old model still active and untouched */
	}

	/* Success: publish the new model atomically, THEN retire the old handle. */
	old_hdl = g_reloc.hdl;
	g_reloc.hdl   = new_hdl;
	g_reloc.slot  = new_slot;
	g_reloc.report = new_rep;
	memcpy(g_reloc.in,  new_in,  sizeof(g_reloc.in));
	memcpy(g_reloc.out, new_out, sizeof(g_reloc.out));
	g_reloc.n_in  = nin;
	g_reloc.n_out = nout;
	g_reloc.acts_bytes = acts;
	reloc_set_name(name);

	if (old_hdl != AI_HANDLE_NULL)
		(void)ai_rel_network_destroy(old_hdl);
	return 0;
}

const struct nn_backend_vt nn_backend_vt_selected = {
	.info = &(const struct nn_backend_info){ .name = "stedgeai_reloc", .version = "X-CUBE-AI 4.0 reloc" },
	.init = reloc_bk_init,
	.open = reloc_bk_open,
	.close = reloc_bk_close,
	.model_name = reloc_bk_name,
	.in_count = reloc_bk_in_count,
	.out_count = reloc_bk_out_count,
	.input = reloc_bk_input,
	.output = reloc_bk_output,
	.activations_bytes = reloc_bk_acts,
	.run = reloc_bk_run,
	.load_region = reloc_bk_load_region,
	.reload = reloc_bk_reload,
};
