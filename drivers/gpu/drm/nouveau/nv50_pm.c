/*
 * Copyright 2010 Red Hat Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * Authors: Ben Skeggs
 */

#include "drmP.h"
#include "nouveau_drv.h"
#include "nouveau_bios.h"
#include "nouveau_pm.h"
#include "nouveau_hwsq.h"

struct nv50_pm_state {
	struct nouveau_pm_level *perflvl;
	struct hwsq_ucode ucode;
	struct pll_lims pll;
	enum pll_types type;
	int N, M, P;
};

struct nv50_pm_clocks_state {
	struct nv50_pm_state *core;
	struct nv50_pm_state *memory;
	struct nv50_pm_state *shader;
	struct nv50_pm_state *vdec;
};

static int
nv50_pm_clock_get(struct drm_device *dev, u32 id)
{
	struct pll_lims pll;
	int P, N, M, ret;
	u32 reg0, reg1;

	ret = get_pll_limits(dev, id, &pll);
	if (ret)
		return ret;

	reg0 = nv_rd32(dev, pll.reg + 0);
	reg1 = nv_rd32(dev, pll.reg + 4);

	if ((reg0 & 0x80000000) == 0) {
		if (id == PLL_SHADER) {
			NV_DEBUG(dev, "Shader PLL is disabled. "
				"Shader clock is twice the core\n");
			ret = nv50_pm_clock_get(dev, PLL_CORE);
			if (ret > 0)
				return ret << 1;
		} else if (id == PLL_MEMORY) {
			NV_DEBUG(dev, "Memory PLL is disabled. "
				"Memory clock is equal to the ref_clk\n");
			return pll.refclk;
		}
	}

	P = (reg0 & 0x00070000) >> 16;
	N = (reg1 & 0x0000ff00) >> 8;
	M = (reg1 & 0x000000ff);

	if (!M)
		return 0;

	return ((pll.refclk * N / M) >> P);
}

int
nv50_pm_clocks_get(struct drm_device *dev, struct nouveau_pm_level *perflvl)
{
	perflvl->core   = nv50_pm_clock_get(dev, PLL_CORE);
	perflvl->shader = nv50_pm_clock_get(dev, PLL_SHADER);
	perflvl->memory = nv50_pm_clock_get(dev, PLL_MEMORY);
	perflvl->vdec  = nv50_pm_clock_get(dev, PLL_VDEC);
	perflvl->rop    = 0;
	perflvl->copy   = 0;
	perflvl->daemon = 0;
	perflvl->unka0  = 0;
	perflvl->hub01  = 0;
	perflvl->hub06  = 0;
	perflvl->hub07  = 0;

	return 0;
}

static void
nv50_mem_hwsq_default_pre(struct drm_device *dev, u32 id, struct nv50_pm_state *state)
{
	struct hwsq_ucode *hwsq;
	u32 reg0_old, reg0_new;
	u32 crtc_mask;
	int i;

	if (id != PLL_MEMORY)
	return;

	hwsq = &state->ucode;

	reg0_old = nv_rd32(dev, state->pll.reg + 0);
	reg0_new = 0x80000000 | (state->P << 16) | (reg0_old & 0xfff8ffff);

	crtc_mask = 0;
	for (i = 0; i < 2; i++) {
		if (nv_rd32(dev, NV50_PDISPLAY_CRTC_C(i, CLOCK)))
			crtc_mask |= (1 << i);
	}

	hwsq_init(hwsq);

	/* Wait for vblank on all the CRTCs */
	if (crtc_mask) {
		hwsq_op5f(hwsq, crtc_mask, 0x00); /* wait for scanout */
		hwsq_op5f(hwsq, crtc_mask, 0x01); /* wait for vblank */
	}

	/* maximise the amount of time to reclock memory before the next scanout */
	hwsq_wr32(hwsq, NV50_PFIFO_FREEZE, 0x00000001);
	hwsq_unkn(hwsq, 0x06); /* wait a few µs */
	hwsq_unkn(hwsq, 0xb0); /* disable bus access */
	hwsq_op5f(hwsq, 0x00, 0x01); /* no idea :s */

	/* Prepare the memory controller */
	hwsq_wr32(hwsq, 0x1002d4, 0x00000001); /* precharge all banks and idle */
	hwsq_wr32(hwsq, 0x1002d0, 0x00000001); /* force refresh */
	hwsq_wr32(hwsq, 0x100210, 0x00000000); /* stop the automatic refresh */
	hwsq_wr32(hwsq, 0x1002dc, 0x00000001); /* start self refresh mode */

	/* reclock memory */
	hwsq_wr32(hwsq, state->pll.reg + 0, reg0_old);
	hwsq_wr32(hwsq, state->pll.reg + 4, (state->N << 8) | state->M);
	hwsq_wr32(hwsq, state->pll.reg + 0, reg0_new);

	/* precharge all banks and idle */
	hwsq_wr32(hwsq, 0x1002d4, 0x00000001);

	/* Restart the memory controller */
	hwsq_wr32(hwsq, 0x1002dc, 0x00000000); /* stop self refresh mode */
	hwsq_wr32(hwsq, 0x100210, 0x80000000); /* restart automatic refresh */
	hwsq_unkn(hwsq, 0x07); /* wait for the PLL to stabilize (12us) */

	/* may be necessary: causes flickering */
	hwsq_unkn(hwsq, 0x0b);

	hwsq_unkn(hwsq, 0xd0); /* Enable bus access again */
	hwsq_op5f(hwsq, 0x00, 0x01);
	hwsq_wr32(hwsq, NV50_PFIFO_FREEZE, 0x00000000);

	hwsq_fini(hwsq);
}

static void
nv50_mem_hwsq_ddr2_pre(struct drm_device *dev, u32 id, struct nv50_pm_state *state)
{
	struct nouveau_pm_memtiming *timings = state->perflvl->timing;
	struct drm_nouveau_private *dev_priv = dev->dev_private;
	struct nouveau_pm_engine *pm = &dev_priv->engine.pm;
	struct hwsq_ucode *hwsq;
	u32 reg0_old, reg0_new, mr, emr1, cl, wr;
	u32 crtc_mask;
	int i;

	if (id != PLL_MEMORY)
		return;

	hwsq = &state->ucode;

	reg0_old = nv_rd32(dev, state->pll.reg + 0);
	reg0_new = 0x80000000 | (state->P << 16) | (reg0_old & 0xfff8ffff);
	mr = nv_rd32(dev, 0x1002c0) & 0xf0feff;
	emr1 = nv_rd32(dev,0x1002c4);
	/* Assuming ODT to be off */

	crtc_mask = 0;
	for (i = 0; i < 2; i++) {
		if (nv_rd32(dev, NV50_PDISPLAY_CRTC_C(i, CLOCK)))
			crtc_mask |= (1 << i);
	}

	hwsq_init(hwsq);

	/* Wait for vblank on all the CRTCs */
	if (crtc_mask) {
		hwsq_op5f(hwsq, crtc_mask, 0x00); /* wait for scanout */
		hwsq_op5f(hwsq, crtc_mask, 0x01); /* wait for vblank */
	}

	/* maximise the amount of time to reclock memory before the next scanout */
	hwsq_wr32(hwsq, NV50_PFIFO_FREEZE, 0x00000001);
	hwsq_unkn(hwsq, 0x06); /* wait a few Âµs */
	hwsq_unkn(hwsq, 0xb0); /* disable bus access */
	hwsq_op5f(hwsq, 0x00, 0x01); /* no idea :s */

	/* Disable DLL: Lowers the reclocking stability */
	if (pm->memtimings.supported && timings) {
		hwsq_wr32(hwsq, 0x1002c4, emr1 & 0xf0ffbb);
		hwsq_unkn(hwsq, 0x06); /* wait a few Âµs (perhaps too much?)*/
	}

	/* Prepare the memory controller */
	hwsq_wr32(hwsq, 0x1002d4, 0x00000001); /* precharge all banks and idle */
	hwsq_wr32(hwsq, 0x1002d0, 0x00000001); /* force refresh */
	hwsq_wr32(hwsq, 0x100210, 0x00000000); /* stop the automatic refresh */
	hwsq_wr32(hwsq, 0x1002dc, 0x00000001); /* start self refresh mode */

	/* reclock memory */
	hwsq_wr32(hwsq, state->pll.reg + 0, reg0_old);
	hwsq_wr32(hwsq, state->pll.reg + 4, (state->N << 8) | state->M);
	hwsq_wr32(hwsq, state->pll.reg + 0, reg0_new);

	/* precharge all banks and idle */
	hwsq_wr32(hwsq, 0x1002d4, 0x00000001);

	/* Restart the memory controller */
	hwsq_wr32(hwsq, 0x1002dc, 0x00000000); /* stop self refresh mode */
	hwsq_wr32(hwsq, 0x100210, 0x80000000); /* restart automatic refresh */
	hwsq_unkn(hwsq, 0x07); /* wait for the PLL to stabilize (12us) */

	if (pm->memtimings.supported && timings) {
		/* Set CL and WR */
		cl = timings->CL;
		wr = timings->WR;

		mr &= 0xf0f08f;
		mr |= ((cl & 0x7) << 4) | (((wr - 1) & 0x7) << 9);

		hwsq_wr32(hwsq, 0x1002c0, mr);

		hwsq_wr32(hwsq, 0x10022c, timings->reg_3);
		hwsq_wr32(hwsq, 0x100224, timings->reg_1);
		hwsq_wr32(hwsq, 0x100238, timings->reg_6);
		hwsq_wr32(hwsq, 0x10023c, timings->reg_7);
		hwsq_wr32(hwsq, 0x100240, timings->reg_8);
		hwsq_wr32(hwsq, 0x100220, timings->reg_0);

		hwsq_wr32(hwsq, 0x100228, timings->reg_2);
		hwsq_wr32(hwsq, 0x100230, timings->reg_4);
		hwsq_wr32(hwsq, 0x100234, timings->reg_5);
	}

	/* reset DLL */
	hwsq_wr32(hwsq, 0x1002c0, mr | 0x100);
	hwsq_wr32(hwsq, 0x1002c0, mr);
	hwsq_unkn(hwsq, 0x0b); /* wait for DLL to stabilize (48us) */
	hwsq_wr32(hwsq, 0x1002c4, emr1); /* Re-enable ODT */
	hwsq_wr32(hwsq, 0x1002d4, 0x00000001); /* precharge again */
	hwsq_wr32(hwsq, 0x1002d0, 0x00000001); /* force refresh */

	hwsq_unkn(hwsq, 0xd0); /* Enable bus access again */
	hwsq_op5f(hwsq, 0x00, 0x01);
	hwsq_wr32(hwsq, NV50_PFIFO_FREEZE, 0x00000000);

	hwsq_fini(hwsq);
}

uint8_t nv_mem_cl_lut_gddr3[16] = {0,0,0,0,4,5,6,7,0,1,2,3,8,9,10,11};
uint8_t nv_mem_wr_lut_gddr3[18] = {0,0,0,0,0,2,3,8,9,10,11,0,0,1,1,0,3};

static void
nv50_mem_hwsq_gddr3_pre(struct drm_device *dev, u32 id, struct nv50_pm_state *state)
{
	struct nouveau_pm_memtiming *timings = state->perflvl->timing;
	struct drm_nouveau_private *dev_priv = dev->dev_private;
	struct nouveau_pm_engine *pm = &dev_priv->engine.pm;
	struct hwsq_ucode *hwsq;
	u32 reg0_old, reg0_new, mr = 0, emr1 = 0, emr1_old, cl, wr;
	u32 crtc_mask;
	int i;

	if (id != PLL_MEMORY)
	return;

	hwsq = &state->ucode;

	reg0_old = nv_rd32(dev, state->pll.reg + 0);
	reg0_new = 0x80000000 | (state->P << 16) | (reg0_old & 0xfff8ffff);

	if (pm->memtimings.supported && timings) {
		mr = nv_rd32(dev, 0x1002c0) & 0xf0fe8b;
		emr1_old = nv_rd32(dev,0x1002c4);
		emr1 = emr1_old & 0xf0ff0f;
		cl = nv_mem_cl_lut_gddr3[timings->CL];
		wr = nv_mem_wr_lut_gddr3[timings->WR];
		mr |= ((cl & 0x7) << 4) | ((cl & 0x8) << 2);
		emr1 |= (wr & 0xf) << 4;
	}

	crtc_mask = 0;
	for (i = 0; i < 2; i++) {
		if (nv_rd32(dev, NV50_PDISPLAY_CRTC_C(i, CLOCK)))
			crtc_mask |= (1 << i);
	}

	hwsq_init(hwsq);

	/* Wait for vblank on all the CRTCs */
	if (crtc_mask) {
		hwsq_op5f(hwsq, crtc_mask, 0x00); /* wait for scanout */
		hwsq_op5f(hwsq, crtc_mask, 0x01); /* wait for vblank */
	}

	/* maximise the amount of time to reclock memory before the next scanout */
	hwsq_wr32(hwsq, NV50_PFIFO_FREEZE, 0x00000001);
	hwsq_unkn(hwsq, 0x06); /* wait a few µs */
	hwsq_unkn(hwsq, 0xb0); /* disable bus access */
	hwsq_op5f(hwsq, 0x00, 0x01); /* no idea :s */

	/* Disable DLL */
	if (pm->memtimings.supported && timings) {
		/*hwsq_wr32(hwsq, 0x1002c4, emr1_old | (0x1 << 6));*/
	}

	/* Prepare the memory controller */
	hwsq_wr32(hwsq, 0x1002d4, 0x00000001); /* precharge all banks and idle */
	hwsq_wr32(hwsq, 0x1002d0, 0x00000001); /* force refresh */
	hwsq_wr32(hwsq, 0x100210, 0x00000000); /* stop the automatic refresh */
	hwsq_wr32(hwsq, 0x1002dc, 0x00000001); /* start self refresh mode */

	/* reclock memory */
	hwsq_wr32(hwsq, state->pll.reg + 0, reg0_old);
	hwsq_wr32(hwsq, state->pll.reg + 4, (state->N << 8) | state->M);
	hwsq_wr32(hwsq, state->pll.reg + 0, reg0_new);

	/* precharge all banks and idle */
	hwsq_wr32(hwsq, 0x1002d4, 0x00000001);

	/* Restart the memory controller */
	hwsq_wr32(hwsq, 0x1002dc, 0x00000000); /* stop self refresh mode */
	hwsq_wr32(hwsq, 0x100210, 0x80000000); /* restart automatic refresh */
	hwsq_unkn(hwsq, 0x07); /* wait for the PLL to stabilize (12us) */

	if (pm->memtimings.supported && timings) {
		/* Set CL and WR */
		hwsq_wr32(hwsq, 0x1002c4, emr1);
		hwsq_wr32(hwsq, 0x1002c0, mr);

		hwsq_wr32(hwsq, 0x10022c, timings->reg_3);
		hwsq_wr32(hwsq, 0x100224, timings->reg_1);
		hwsq_wr32(hwsq, 0x100238, timings->reg_6);
		hwsq_wr32(hwsq, 0x10023c, timings->reg_7);
		hwsq_wr32(hwsq, 0x100240, timings->reg_8);
		hwsq_wr32(hwsq, 0x100220, timings->reg_0);

		hwsq_wr32(hwsq, 0x100228, timings->reg_2);
		hwsq_wr32(hwsq, 0x100230, timings->reg_4);
		hwsq_wr32(hwsq, 0x100234, timings->reg_5);

		/* reset DLL */
		hwsq_wr32(hwsq, 0x1002c0, mr | 0x100);
		hwsq_wr32(hwsq, 0x1002c0, mr);
		hwsq_unkn(hwsq, 0x0b); /* wait for DLL to stabilize (48us) */
		hwsq_wr32(hwsq, 0x1002d4, 0x00000001); /* precharge again */
		hwsq_wr32(hwsq, 0x1002d0, 0x00000001); /* force refresh */
	}

	hwsq_unkn(hwsq, 0xd0); /* Enable bus access again */
	hwsq_op5f(hwsq, 0x00, 0x01);
	hwsq_wr32(hwsq, NV50_PFIFO_FREEZE, 0x00000000);

	hwsq_fini(hwsq);
}

/**
 * Prepares MR register values, as documented by RAM vendors
 * @pre timing entry is initialized by nouveau_mem_timing_init or equivalent
 */
static void
nv50_mem_hwsq_pre(struct drm_device *dev, u32 id, struct nv50_pm_state *state) {
	struct drm_nouveau_private *dev_priv = dev->dev_private;
	struct nouveau_pm_engine *pm = &dev_priv->engine.pm;

	switch(pm->memtimings.mem_type) {
	case NV_MEM_DDR2:
		nv50_mem_hwsq_ddr2_pre(dev, id, state);
		break;
	case NV_MEM_GDDR3:
		nv50_mem_hwsq_gddr3_pre(dev, id, state);
		break;
	default:
		nv50_mem_hwsq_default_pre(dev, id, state);
	}
	return;
}

static void *
nv50_pm_clock_pre(struct drm_device *dev, struct nouveau_pm_level *perflvl,
		  u32 id, int khz)
{
	struct nv50_pm_state *state;
	int dummy, ret;

	state = kzalloc(sizeof(*state), GFP_KERNEL);
	if (!state)
		return ERR_PTR(-ENOMEM);
	state->type = id;
	state->perflvl = perflvl;

	ret = get_pll_limits(dev, id, &state->pll);
	if (ret < 0) {
		kfree(state);
		return NULL;
	}

	ret = nv50_calc_pll(dev, &state->pll, khz, &state->N, &state->M,
			    &dummy, &dummy, &state->P);
	if (ret <= 0) {
		kfree(state);
		return NULL;
	}

	nv50_mem_hwsq_pre(dev, id, state);

	return state;
}

void *
nv50_pm_clocks_pre(struct drm_device *dev, struct nouveau_pm_level *perflvl)
{
	struct nv50_pm_clocks_state *state;

	state = kzalloc(sizeof(*state), GFP_KERNEL);
	if (!state)
		return ERR_PTR(-ENOMEM);

	state->core = nv50_pm_clock_pre(dev, perflvl, PLL_CORE,
					perflvl->core);

	state->shader = nv50_pm_clock_pre(dev, perflvl, PLL_SHADER,
					  perflvl->shader);

	state->memory = nv50_pm_clock_pre(dev, perflvl, PLL_MEMORY,
					  perflvl->memory);

	state->vdec = nv50_pm_clock_pre(dev, perflvl, PLL_VDEC,
					 perflvl->vdec);

	return state;
}

static void
nv50_pm_pll_set(struct drm_device *dev, struct nv50_pm_state *state)
{
	u32 reg, tmp;
	int N, M, P;

	if (!state || state->type == PLL_MEMORY)
		return;

	reg = state->pll.reg;
	N = state->N;
	M = state->M;
	P = state->P;

	tmp  = nv_rd32(dev, reg + 0) & 0xfff8ffff;
	tmp |= 0x80000000 | (P << 16);
	nv_wr32(dev, reg + 0, tmp);
	nv_wr32(dev, reg + 4, (N << 8) | M);

	kfree(state);
}

static int
nv50_pm_pll_memory_set(struct drm_device *dev, struct nv50_pm_state *state)
{
	struct nouveau_pm_level *perflvl;
	struct hwsq_ucode *hwsq;
	struct bit_entry BIT_M;
	u32 r100b0c, r619f00;
	u16 script;
	int ret = 0;

	if (!state || state->type != PLL_MEMORY)
		return -EPERM;

	hwsq = &state->ucode;
	perflvl = state->perflvl;

	if (perflvl->memscript && bit_table(dev, 'M', &BIT_M) == 0 &&
	    BIT_M.version == 1 && BIT_M.length >= 0x0b) {
		script = ROM16(BIT_M.data[0x05]);
		if (script)
			nouveau_bios_run_init_table(dev, script, NULL, -1);
		script = ROM16(BIT_M.data[0x07]);
		if (script)
			nouveau_bios_run_init_table(dev, script, NULL, -1);
		script = ROM16(BIT_M.data[0x09]);
		if (script)
			nouveau_bios_run_init_table(dev, script, NULL, -1);

		nouveau_bios_run_init_table(dev, perflvl->memscript, NULL, -1);
	}

	hwsq_upload(dev, hwsq);

	nv_mask(dev, 0x616308, 0x00000000, 0x00000010);
	nv_mask(dev, 0x616b08, 0x00000000, 0x00000010);
	nv_mask(dev, 0x100200, 0x00000800, 0x00000000);
	r100b0c = nv_mask(dev, 0x100b0c, 0x000000ff, 0x00000012);
	r619f00 = nv_mask(dev, 0x619f00, 0x00000008, 0x00000000);

	ret = hwsq_launch(dev, hwsq);

	nv_wr32(dev, 0x619f00, r619f00);
	nv_wr32(dev, 0x100b0c, r100b0c);
	nv_mask(dev, 0x616308, 0x00000000, 0x00000010);
	nv_mask(dev, 0x616b08, 0x00000000, 0x00000010);
	nv_mask(dev, 0x100200, 0x00000000, 0x00000800);

	kfree(state);

	return ret;
}

static bool
nv50_pm_grcp_idle(void *data)
{
	struct drm_device *dev = data;

	if (!(nv_rd32(dev, 0x400304) & 0x00000001))
		return true;
	if (nv_rd32(dev, 0x400308) == 0x0050001c)
		return true;
	return false;
}

int
nv50_pm_clocks_set(struct drm_device *dev, void *pre_state)
{
	struct drm_nouveau_private *dev_priv = dev->dev_private;
	struct nv50_pm_clocks_state *state = pre_state;
	uint64_t start_time;
	unsigned long flags;
	/* initial guess... */
	uint32_t mask380 = 0xffffffff;
	uint32_t mask384 = 0xffffffff;
	uint32_t mask388 = 0xffffffff;
	uint32_t mask504 = 0x00000001;
	uint32_t mask700 = 0x00000001;
	int i = 0, ret = -EAGAIN;

	if (!state)
		return -EFAULT;

	start_time = nv04_timer_read(dev);

	/* Reclock memory first */
	nv50_pm_pll_memory_set(dev, state->memory);

	/* prevent any new grctx switches from starting */
	spin_lock_irqsave(&dev_priv->context_switch_lock, flags);
	nv_wr32(dev, 0x400324, 0x00000000);
	nv_wr32(dev, 0x400328, 0x0050001c); /* wait flag 0x1c */
	/* wait for any pending grctx switches to complete */
	if (!nv_wait_cb(dev, nv50_pm_grcp_idle, dev)) {
		NV_ERROR(dev, "pm: ctxprog didn't go idle\n");
		goto cleanup;
	}
	/* freeze PFIFO */
	nv_mask(dev, 0x002504, 0x00000001, 0x00000001);
	if (!nv_wait(dev, 0x002504, 0x00000010, 0x00000010)) {
		NV_ERROR(dev, "pm: fifo didn't go idle\n");
		goto cleanup;
	}

	/* Empty PGRAPH's FIFO */
	do {
		/* Un-pause PGRAPH's FIFO (in case it was) */
		nv_mask(dev, NV50_PGRAPH_CONTROL, 0, 0x1);

		/* Wait for PGRAPH's FIFO to deplete */
		if (!nouveau_wait_eq(dev, 100000, NV50_PGRAPH_FIFO_STATUS,
					mask504, 0x1)) {
			if (nv_rd32(dev, NV04_PGRAPH_STATUS) & 0x100) {
				NV_ERROR(dev,
				"pm: PGRAPH got paused while running a ctxprog,"
				" NV40_PGRAPH_CTXCTL_0310 = 0x%x\n",
				nv_rd32(dev, NV40_PGRAPH_CTXCTL_0310));
			}

			goto cleanup;
		}

		/* Pause PGRAPH's FIFO */
		nv_mask(dev, NV50_PGRAPH_CONTROL, 0x1, 0);

		/* Limit the number of loops to 2 */
		i++;
		if (i > 1)
			goto cleanup;
	} while ((nv_rd32(dev, NV50_PGRAPH_FIFO_STATUS) & mask504) == 0);

	/* Wait for PGRAPH engines to stop */
	if (!nouveau_wait_eq(dev, 1000000, 0x400380, mask380, 0x0) ||
	    !nouveau_wait_eq(dev, 1000000, 0x400384, mask384, 0x0) ||
	    !nouveau_wait_eq(dev, 1000000, 0x400388, mask388, 0x0) ||
	    !nouveau_wait_eq(dev, 5000000, NV04_PGRAPH_STATUS, mask700, 0x0)) {
		/* if you see this message,
		* mask* above probably need to be adjusted
		* to not contain the bits you see failing */
		NV_ERROR(dev,
		    "pm: PGRAPH didn't go idle: %08x %08x %08x %08x %08x!\n",
		    nv_rd32(dev, 0x400380),
		    nv_rd32(dev, 0x400384),
		    nv_rd32(dev, 0x400388),
		    nv_rd32(dev, NV50_PGRAPH_FIFO_STATUS),
		    nv_rd32(dev, NV04_PGRAPH_STATUS));

		goto cleanup;
	}

	nv50_pm_pll_set(dev, state->core);
	nv50_pm_pll_set(dev, state->shader);
	nv50_pm_pll_set(dev, state->vdec);

	/* Wait at least 20k clocks */
	udelay(300);

	ret = 0;

cleanup:
	/* unfreeze PFIFO */
	nv_mask(dev, 0x002504, 0x00000001, 0x00000000);

	/* Unpause PGRAPH */
	nv_mask(dev, NV50_PGRAPH_CONTROL, 0, 0x1);

	/* restore ctxprog to normal */
	nv_wr32(dev, 0x400324, 0x00000000);
	nv_wr32(dev, 0x400328, 0x0070009c); /* set flag 0x1c */
	/* unblock it if necessary */
	if (nv_rd32(dev, 0x400308) == 0x0050001c)
		nv_mask(dev, 0x400824, 0x10000000, 0x10000000);

	spin_unlock_irqrestore(&dev_priv->context_switch_lock, flags);

	NV_INFO(dev, "pm: reclocking took %lluns\n",
		(nv04_timer_read(dev) - start_time));

	kfree(pre_state);

	return 0;
}

struct pwm_info {
	int id;
	int invert;
	u8  tag;
	u32 ctrl;
	int line;
};

static int
pwm_info(struct drm_device *dev, struct dcb_gpio_entry *gpio,
	 int *ctrl, int *line, int *indx)
{
	if (gpio->line == 0x04) {
		*ctrl = 0x00e100;
		*line = 4;
		*indx = 0;
	} else
	if (gpio->line == 0x09) {
		*ctrl = 0x00e100;
		*line = 9;
		*indx = 1;
	} else
	if (gpio->line == 0x10) {
		*ctrl = 0x00e28c;
		*line = 0;
		*indx = 0;
	} else {
		NV_ERROR(dev, "unknown pwm ctrl for gpio %d\n", gpio->line);
		return -ENODEV;
	}

	return 0;
}

int
nv50_pm_pwm_get(struct drm_device *dev, struct dcb_gpio_entry *gpio,
		u32 *divs, u32 *duty)
{
	int ctrl, line, id, ret = pwm_info(dev, gpio, &ctrl, &line, &id);
	if (ret)
		return ret;

	if (nv_rd32(dev, ctrl) & (1 << line)) {
		*divs = nv_rd32(dev, 0x00e114 + (id * 8));
		*duty = nv_rd32(dev, 0x00e118 + (id * 8));
		return 0;
	}

	return -EINVAL;
}

int
nv50_pm_pwm_set(struct drm_device *dev, struct dcb_gpio_entry *gpio,
		u32 divs, u32 duty)
{
	int ctrl, line, id, ret = pwm_info(dev, gpio, &ctrl, &line, &id);
	if (ret)
		return ret;

	nv_mask(dev, ctrl, 0x00010001 << line, 0x00000001 << line);
	nv_wr32(dev, 0x00e114 + (id * 8), divs);
	nv_wr32(dev, 0x00e118 + (id * 8), duty | 0x80000000);
	return 0;
}
