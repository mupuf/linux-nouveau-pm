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
 * Authors: Ben Skeggs, Roy Spliet
 */

#include <drm/drmP.h>
#include "nouveau_drm.h"
#include "nouveau_bios.h"
#include "dispnv04/hw.h"
#include "nouveau_pm.h"

#include "nv50_display.h"

#include <subdev/bios/pll.h>
#include <subdev/clock.h>
#include <subdev/timer.h>
#include <engine/fifo.h>

enum clk_src {
	clk_src_crystal,
	clk_src_href,
	clk_src_hrefm4,
	clk_src_hrefm2d3,
	clk_src_host,
	clk_src_nvclk,
	clk_src_mclk,
	clk_src_sclk,
	clk_src_cclk,
	clk_src_vdec
};

static u32 read_clk(struct drm_device *, enum clk_src);

/* XXX: define 0x4600 as sth like NVAA_PCLOCK_DIV, remove function */
static u32
read_div(struct drm_device *dev)
{
	struct nouveau_device *device = nouveau_dev(dev);

	return nv_rd32(device, 0x004600);
}

static u32
read_pll(struct drm_device *dev, u32 base)
{
	struct nouveau_device *device = nouveau_dev(dev);
	u32 ctrl = nv_rd32(device, base + 0);
	u32 coef = nv_rd32(device, base + 4);
	u32 ref = read_clk(dev, clk_src_href);
	u32 post_div = 0;
	u32 clk = 0;
	int N1, M1;

	switch (base){
	case 0x4020:
		post_div = 1 << ((nv_rd32(device, 0x4070) & 0x000f0000) >> 16);
		break;
	case 0x4028:
		post_div = (nv_rd32(device, 0x4040) & 0x000f0000) >> 16;
		break;
	default:
		break;
	}

	N1 = (coef & 0x0000ff00) >> 8;
	M1 = (coef & 0x000000ff);
	if ((ctrl & 0x80000000) && M1) {
		clk = ref * N1 / M1;
		clk = clk / post_div;
	}

	return clk;
}

static u32
read_clk(struct drm_device *dev, enum clk_src src)
{
	struct nouveau_device *device = nouveau_dev(dev);
	struct nouveau_drm *drm = nouveau_drm(dev);
	u32 mast = nv_rd32(device, 0x00c054);
	u32 P = 0;

	switch (src) {
	case clk_src_crystal:
		return device->crystal;
	case clk_src_href:
		return 100000; /* PCIE reference clock */
	case clk_src_hrefm4:
		return read_clk(dev, clk_src_href) * 4;
	case clk_src_hrefm2d3:
		return read_clk(dev, clk_src_href) * 2 / 3;
	case clk_src_host:
		switch (mast & 0x000c0000) {
		case 0x00000000: return read_clk(dev, clk_src_hrefm2d3);
		case 0x00040000: break;
		case 0x00080000: return read_clk(dev, clk_src_hrefm4);
		case 0x000c0000: return read_clk(dev, clk_src_cclk);
		}
		break;
	case clk_src_nvclk:
		P = (nv_rd32(device, 0x004028) & 0x00070000) >> 16;

		switch (mast & 0x00000003) {
		case 0x00000000: return read_clk(dev, clk_src_crystal) >> P;
		case 0x00000001: return 0;
		case 0x00000002: return read_clk(dev, clk_src_hrefm4) >> P;
		case 0x00000003: return read_pll(dev, 0x004028) >> P;
		}
		break;
	case clk_src_cclk:
		if ((mast & 0x03000000) != 0x03000000)
			return read_clk(dev, clk_src_nvclk);

		if ((mast & 0x00000200) == 0x00000000)
			return read_clk(dev, clk_src_nvclk);

		switch (mast & 0x00000c00) {
		case 0x00000000: return read_clk(dev, clk_src_href);
		case 0x00000400: return read_clk(dev, clk_src_hrefm4);
		case 0x00000800: return read_clk(dev, clk_src_hrefm2d3);
		default: return 0;
		}
	case clk_src_sclk:
		P = (nv_rd32(device, 0x004020) & 0x00070000) >> 16;
		switch (mast & 0x00000030) {
		case 0x00000000:
			if (mast & 0x00000040)
				return read_clk(dev, clk_src_href) >> P;
			return read_clk(dev, clk_src_crystal) >> P;
		case 0x00000010: break;
		case 0x00000020: return read_pll(dev, 0x004028) >> P;
		case 0x00000030: return read_pll(dev, 0x004020) >> P;
		}
		break;
	case clk_src_mclk:
		return 0;
		break;
	case clk_src_vdec:
		P = (read_div(dev) & 0x00000700) >> 8;

		switch (mast & 0x00400000) {
		case 0x00400000:
			return read_clk(dev, clk_src_nvclk) >> P;
			break;
		default:
			return 500000 >> P;
			break;
		}
		break;
	default:
		break;
	}

	NV_DEBUG(drm, "unknown clock source %d 0x%08x\n", src, mast);
	return 0;
}

int
nvaa_pm_clocks_get(struct drm_device *dev, struct nouveau_pm_level *perflvl)
{
	perflvl->core   = read_clk(dev, clk_src_cclk);
	perflvl->shader = read_clk(dev, clk_src_sclk);
	perflvl->memory = 0;
	perflvl->vdec = read_clk(dev, clk_src_vdec);

	return 0;
}

struct nvaa_pm_state {
	struct nouveau_pm_level *perflvl;
	enum clk_src csrc; /* Core */
	u32 nvcoef;	/* 0x4028 */
	u32 nvctrl;	/* 0x402c */
	u32 nvpost;	/* 0x4040 */
	enum clk_src ssrc; /* Shader */
	u32 scoef;	/* 0x4020 */
	u32 sctrl;	/* 0x4024 */
	u32 spost;	/* 0x4070 */
	enum clk_src vsrc; /* Video */
	u32 vdiv;	/* 0x4600 */
};

static u32
calc_pll(struct drm_device *dev, u32 reg, struct nvbios_pll *pll,
	 u32 clk, int *N1, int *M1, int *log2P)
{
	struct nouveau_device *device = nouveau_dev(dev);
	struct nouveau_bios *bios = nouveau_bios(device);
	struct nouveau_clock *pclk = nouveau_clock(device);
	struct nouveau_pll_vals coef;
	int ret;

	ret = nvbios_pll_parse(bios, reg, pll);
	if (ret)
		return 0;

	pll->vco2.max_freq = 0;
	pll->refclk = read_clk(dev, clk_src_href);
	if (!pll->refclk)
		return 0;

	ret = pclk->pll_calc(pclk, pll, clk, &coef);
	if (ret == 0)
		return 0;

	*N1 = coef.N1;
	*M1 = coef.M1;
	*log2P = coef.log2P;
	return ret;
}

static inline u32
calc_P(u32 src, u32 target, int *div)
{
	u32 clk0 = src, clk1 = src;
	for (*div = 0; *div <= 7; (*div)++) {
		if (clk0 <= target) {
			clk1 = clk0 << (*div ? 1 : 0);
			break;
		}
		clk0 >>= 1;
	}

	if (target - clk0 <= clk1 - target)
		return clk0;
	(*div)--;
	return clk1;
}

void *
nvaa_pm_clocks_pre(struct drm_device *dev, struct nouveau_pm_level *perflvl)
{
	struct nouveau_drm *drm = nouveau_drm(dev);
	struct nvaa_pm_state *info;
	struct nvbios_pll pll;
	u32 out = 0, clk = 0, divs = 0;
	int N, M, P1, P2 = 0;

	info = kzalloc(sizeof(*info), GFP_KERNEL);
	if (!info)
		return ERR_PTR(-ENOMEM);
	info->perflvl = perflvl;

	/* cclk: find suitable source, disable PLL if we can */
	if (perflvl->core < read_clk(dev, clk_src_hrefm4))
		out = calc_P(read_clk(dev, clk_src_hrefm4), perflvl->core, &divs);

	/* Calculate clock * 2, so shader clock can use it too */
	clk = calc_pll(dev, 0x4028, &pll, (perflvl->core << 1), &N, &M, &P1);

	if (abs((int)perflvl->core - out) <=
	    abs((int)perflvl->core - (clk >> 1))) {
		info->csrc = clk_src_hrefm4;
		info->nvctrl = divs << 16;
	} else {
		/* NVCTRL is actually used _after_ NVPOST, and after what we
		 * call NVPLL. To make matters worse, NVPOST is an integer
		 * divider instead of a right-shift number. */
		if(P1 > 2) {
			P2 = P1 - 2;
			P1 = 2;
		}

		info->csrc = clk_src_nvclk;
		info->nvcoef = (N << 8) | M;

		info->nvctrl = (P2 + 1) << 16;
		info->nvpost = (1 << P1) << 16;
	}

	/* sclk: nvpll + divisor, href or spll */
	out = 0;
	if (perflvl->shader == read_clk(dev, clk_src_href)) {
		info->ssrc = clk_src_href;
	} else {
		clk = calc_pll(dev, 0x4020, &pll, perflvl->shader, &N, &M, &P1);
		if (info->csrc == clk_src_nvclk) {
			out = calc_P((perflvl->core << 1), perflvl->shader, &divs);
		}

		if (abs((int)perflvl->shader - out) <=
		    abs((int)perflvl->shader - clk) &&
		   (divs + P2) <= 7) {
			info->ssrc = clk_src_nvclk;
			info->sctrl = (divs + P2) << 16;
		} else {
			info->ssrc = clk_src_sclk;
			info->scoef = (N << 8) | M;
			info->sctrl = P1 << 16;
		}
	}

	/* vclk */
	out = calc_P(perflvl->core, perflvl->vdec, &divs);
	clk = calc_P(500000, perflvl->vdec, &P1);
	if(abs((int)perflvl->vdec - out) <=
	   abs((int)perflvl->vdec - clk)) {
		info->vsrc = clk_src_cclk;
		info->vdiv = divs << 16;
	} else {
		info->vsrc = clk_src_vdec;
		info->vdiv = P1 << 16;
	}

	/* Print strategy! */
	NV_DEBUG(drm, "nvpll: %08x %08x %08x\n", info->nvcoef, info->nvpost, info->nvctrl);
	NV_DEBUG(drm, " spll: %08x %08x %08x\n", info->scoef, info->spost, info->sctrl);
	NV_DEBUG(drm, " vdiv: %08x\n", info->vdiv);
	if (info->csrc == clk_src_hrefm4)
		NV_DEBUG(drm, "core: hrefm4\n");
	else
		NV_DEBUG(drm, "core: nvpll\n");

	if (info->ssrc == clk_src_hrefm4)
		NV_DEBUG(drm, "shader: hrefm4\n");
	else if (info->ssrc == clk_src_nvclk)
		NV_DEBUG(drm, "shader: nvpll\n");
	else
		NV_DEBUG(drm, "shader: spll\n");

	if (info->vsrc == clk_src_hrefm4)
		NV_DEBUG(drm, "vdec: 500MHz\n");
	else
		NV_DEBUG(drm, "vdec: core\n");

	return info;
}

int
nvaa_pm_clocks_set(struct drm_device *dev, void *data)
{
	struct nouveau_device *device = nouveau_dev(dev);
	struct nouveau_drm *drm = nouveau_drm(dev);
	struct nvaa_pm_state *info = data;
	struct nouveau_fifo *pfifo = nouveau_fifo(device);
	unsigned long flags;
	u32 pllmask = 0, mast, ptherm_gate;
	int ret = -EBUSY;

	/* halt and idle execution engines */
	ptherm_gate = nv_mask(device, 0x20060, 0x00070000, 0x00000000);
	nv_mask(device, 0x002504, 0x00000001, 0x00000001);
	/* Wait until the interrupt handler is finished */
	if (!nv_wait(device, 0x000100, 0xffffffff, 0x00000000))
		goto resume;

	pfifo->pause(pfifo, &flags);
	if (!nv_wait(device, 0x002504, 0x00000010, 0x00000010))
		goto resume;
	if (!nv_wait(device, 0x00251c, 0x0000003f, 0x0000003f))
		goto resume;

	/* First switch to safe clocks: href */
	mast = nv_mask(device, 0xc054, 0x03400e70, 0x03400640);
	mast &= ~0x00400e73;
	mast |= 0x03000000;

	switch (info->csrc) {
	case clk_src_hrefm4:
		nv_mask(device, 0x4028, 0x00070000, info->nvctrl);
		mast |= 0x00000002;
		break;
	case clk_src_nvclk:
		nv_wr32(device, 0x402c, info->nvcoef);
		nv_wr32(device, 0x4028, 0x80000000 | info->nvctrl);
		nv_wr32(device, 0x4040, info->nvpost);
		pllmask |= (0x3 << 8);
		mast |= 0x00000003;
		break;
	default:
		NV_WARN(drm,"Reclocking failed: unknown core clock\n");
		goto resume;
	}

	switch (info->ssrc) {
	case clk_src_href:
		nv_mask(device, 0x4020, 0x00070000, 0x00000000);
		/* mast |= 0x00000000; */
		break;
	case clk_src_nvclk:
		nv_mask(device, 0x4020, 0x00070000, info->sctrl);
		mast |= 0x00000020;
		break;
	case clk_src_sclk:
		nv_wr32(device, 0x4024, info->scoef);
		nv_wr32(device, 0x4020, 0x80000000 | info->sctrl);
		nv_wr32(device, 0x4070, info->spost);
		pllmask |= (0x3 << 12);
		mast |= 0x00000030;
		break;
	default:
		NV_WARN(drm,"Reclocking failed: unknown sclk clock\n");
		goto resume;
	}

	if (!nv_wait(device, 0x004080, pllmask, pllmask)) {
		NV_WARN(drm,"Reclocking failed: unstable PLLs\n");
		goto resume;
	}

	switch (info->vsrc) {
	case clk_src_cclk:
		mast |= 0x00400000;
	default:
		nv_wr32(device, 0x4600, info->vdiv);
	}

	nv_wr32(device, 0xc054, mast);
	ret = 0;

resume:
	pfifo->start(pfifo, &flags);
	nv_mask(device, 0x002504, 0x00000001, 0x00000000);
	nv_wr32(device, 0x20060, ptherm_gate);

	/* Disable some PLLs and dividers when unused */
	if (info->csrc != clk_src_nvclk) {
		nv_wr32(device, 0x4040, 0x00000000);
		nv_mask(device, 0x4028, 0x80000000, 0x00000000);
	}

	if (info->ssrc != clk_src_sclk) {
		nv_wr32(device, 0x4070, 0x00000000);
		nv_mask(device, 0x4020, 0x80000000, 0x00000000);
	}

	kfree(info);
	return ret;
}
