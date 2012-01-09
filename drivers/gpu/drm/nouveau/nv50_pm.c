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
#include "nouveau_hw.h"
#include "nouveau_pm.h"
#include "nouveau_hwsq.h"

enum clk_src {
	clk_src_crystal,
	clk_src_href,
	clk_src_hclk,
	clk_src_hclkm3,
	clk_src_hclkm3d2,
	clk_src_host,
	clk_src_nvclk,
	clk_src_sclk,
	clk_src_mclk,
	clk_src_vdec,
	clk_src_dom6
};

static u32 read_clk(struct drm_device *, enum clk_src);

static u32
read_div(struct drm_device *dev)
{
	struct drm_nouveau_private *dev_priv = dev->dev_private;

	switch (dev_priv->chipset) {
	case 0x50: /* it exists, but only has bit 31, not the dividers.. */
	case 0x84:
	case 0x86:
	case 0x98:
	case 0xa0:
		return nv_rd32(dev, 0x004700);
	case 0x92:
	case 0x94:
	case 0x96:
		return nv_rd32(dev, 0x004800);
	default:
		return 0x00000000;
	}
}

static u32
read_pll_src(struct drm_device *dev, u32 base)
{
	struct drm_nouveau_private *dev_priv = dev->dev_private;
	u32 coef, ref = read_clk(dev, clk_src_crystal);
	u32 rsel = nv_rd32(dev, 0x00e18c);
	int P, N, M, id;

	switch (dev_priv->chipset) {
	case 0x50:
	case 0xa0:
		switch (base) {
		case 0x4020:
		case 0x4028: id = !!(rsel & 0x00000004); break;
		case 0x4008: id = !!(rsel & 0x00000008); break;
		case 0x4030: id = 0; break;
		default:
			NV_ERROR(dev, "ref: bad pll 0x%06x\n", base);
			return 0;
		}

		coef = nv_rd32(dev, 0x00e81c + (id * 0x0c));
		ref *=  (coef & 0x01000000) ? 2 : 4;
		P    =  (coef & 0x00070000) >> 16;
		N    = ((coef & 0x0000ff00) >> 8) + 1;
		M    = ((coef & 0x000000ff) >> 0) + 1;
		break;
	case 0x84:
	case 0x86:
	case 0x92:
		coef = nv_rd32(dev, 0x00e81c);
		P    = (coef & 0x00070000) >> 16;
		N    = (coef & 0x0000ff00) >> 8;
		M    = (coef & 0x000000ff) >> 0;
		break;
	case 0x94:
	case 0x96:
	case 0x98:
		rsel = nv_rd32(dev, 0x00c050);
		switch (base) {
		case 0x4020: rsel = (rsel & 0x00000003) >> 0; break;
		case 0x4008: rsel = (rsel & 0x0000000c) >> 2; break;
		case 0x4028: rsel = (rsel & 0x00001800) >> 11; break;
		case 0x4030: rsel = 3; break;
		default:
			NV_ERROR(dev, "ref: bad pll 0x%06x\n", base);
			return 0;
		}

		switch (rsel) {
		case 0: id = 1; break;
		case 1: return read_clk(dev, clk_src_crystal);
		case 2: return read_clk(dev, clk_src_href);
		case 3: id = 0; break;
		}

		coef =  nv_rd32(dev, 0x00e81c + (id * 0x28));
		P    = (nv_rd32(dev, 0x00e824 + (id * 0x28)) >> 16) & 7;
		P   += (coef & 0x00070000) >> 16;
		N    = (coef & 0x0000ff00) >> 8;
		M    = (coef & 0x000000ff) >> 0;
		break;
	default:
		BUG_ON(1);
	}

	if (M)
		return (ref * N / M) >> P;
	return 0;
}

static u32
read_pll_ref(struct drm_device *dev, u32 base)
{
	u32 src, mast = nv_rd32(dev, 0x00c040);

	switch (base) {
	case 0x004028:
		src = !!(mast & 0x00200000);
		break;
	case 0x004020:
		src = !!(mast & 0x00400000);
		break;
	case 0x004008:
		src = !!(mast & 0x00010000);
		break;
	case 0x004030:
		src = !!(mast & 0x02000000);
		break;
	case 0x00e810:
		return read_clk(dev, clk_src_crystal);
	default:
		NV_ERROR(dev, "bad pll 0x%06x\n", base);
		return 0;
	}

	if (src)
		return read_clk(dev, clk_src_href);
	return read_pll_src(dev, base);
}

static u32
read_pll(struct drm_device *dev, u32 base)
{
	struct drm_nouveau_private *dev_priv = dev->dev_private;
	u32 mast = nv_rd32(dev, 0x00c040);
	u32 ctrl = nv_rd32(dev, base + 0);
	u32 coef = nv_rd32(dev, base + 4);
	u32 ref = read_pll_ref(dev, base);
	u32 clk = 0;
	int N1, N2, M1, M2;

	if (base == 0x004028 && (mast & 0x00100000)) {
		/* wtf, appears to only disable post-divider on nva0 */
		if (dev_priv->chipset != 0xa0)
			return read_clk(dev, clk_src_dom6);
	}

	N2 = (coef & 0xff000000) >> 24;
	M2 = (coef & 0x00ff0000) >> 16;
	N1 = (coef & 0x0000ff00) >> 8;
	M1 = (coef & 0x000000ff);
	if ((ctrl & 0x80000000) && M1) {
		clk = ref * N1 / M1;
		if ((ctrl & 0x40000100) == 0x40000000) {
			if (M2)
				clk = clk * N2 / M2;
			else
				clk = 0;
		}
	}

	return clk;
}

static u32
read_clk(struct drm_device *dev, enum clk_src src)
{
	struct drm_nouveau_private *dev_priv = dev->dev_private;
	u32 mast = nv_rd32(dev, 0x00c040);
	u32 P = 0;

	switch (src) {
	case clk_src_crystal:
		return dev_priv->crystal;
	case clk_src_href:
		return 100000; /* PCIE reference clock */
	case clk_src_hclk:
		return read_clk(dev, clk_src_href) * 27778 / 10000;
	case clk_src_hclkm3:
		return read_clk(dev, clk_src_hclk) * 3;
	case clk_src_hclkm3d2:
		return read_clk(dev, clk_src_hclk) * 3 / 2;
	case clk_src_host:
		switch (mast & 0x30000000) {
		case 0x00000000: return read_clk(dev, clk_src_href);
		case 0x10000000: break;
		case 0x20000000: /* !0x50 */
		case 0x30000000: return read_clk(dev, clk_src_hclk);
		}
		break;
	case clk_src_nvclk:
		if (!(mast & 0x00100000))
			P = (nv_rd32(dev, 0x004028) & 0x00070000) >> 16;
		switch (mast & 0x00000003) {
		case 0x00000000: return read_clk(dev, clk_src_crystal) >> P;
		case 0x00000001: return read_clk(dev, clk_src_dom6);
		case 0x00000002: return read_pll(dev, 0x004020) >> P;
		case 0x00000003: return read_pll(dev, 0x004028) >> P;
		}
		break;
	case clk_src_sclk:
		P = (nv_rd32(dev, 0x004020) & 0x00070000) >> 16;
		switch (mast & 0x00000030) {
		case 0x00000000:
			if (mast & 0x00000080)
				return read_clk(dev, clk_src_host) >> P;
			return read_clk(dev, clk_src_crystal) >> P;
		case 0x00000010: break;
		case 0x00000020: return read_pll(dev, 0x004028) >> P;
		case 0x00000030: return read_pll(dev, 0x004020) >> P;
		}
		break;
	case clk_src_mclk:
		P = (nv_rd32(dev, 0x004008) & 0x00070000) >> 16;
		if (nv_rd32(dev, 0x004008) & 0x00000200) {
			switch (mast & 0x0000c000) {
			case 0x00000000:
				return read_clk(dev, clk_src_crystal) >> P;
			case 0x00008000:
			case 0x0000c000:
				return read_clk(dev, clk_src_href) >> P;
			}
		} else {
			return read_pll(dev, 0x004008) >> P;
		}
		break;
	case clk_src_vdec:
		P = (read_div(dev) & 0x00000700) >> 8;
		switch (dev_priv->chipset) {
		case 0x84:
		case 0x86:
		case 0x92:
		case 0x94:
		case 0x96:
		case 0xa0:
			switch (mast & 0x00000c00) {
			case 0x00000000:
				if (dev_priv->chipset == 0xa0) /* wtf?? */
					return read_clk(dev, clk_src_nvclk) >> P;
				return read_clk(dev, clk_src_crystal) >> P;
			case 0x00000400:
				return 0;
			case 0x00000800:
				if (mast & 0x01000000)
					return read_pll(dev, 0x004028) >> P;
				return read_pll(dev, 0x004030) >> P;
			case 0x00000c00:
				return read_clk(dev, clk_src_nvclk) >> P;
			}
			break;
		case 0x98:
			switch (mast & 0x00000c00) {
			case 0x00000000:
				return read_clk(dev, clk_src_nvclk) >> P;
			case 0x00000400:
				return 0;
			case 0x00000800:
				return read_clk(dev, clk_src_hclkm3d2) >> P;
			case 0x00000c00:
				return read_clk(dev, clk_src_mclk) >> P;
			}
			break;
		}
		break;
	case clk_src_dom6:
		switch (dev_priv->chipset) {
		case 0x50:
		case 0xa0:
			return read_pll(dev, 0x00e810) >> 2;
		case 0x84:
		case 0x86:
		case 0x92:
		case 0x94:
		case 0x96:
		case 0x98:
			P = (read_div(dev) & 0x00000007) >> 0;
			switch (mast & 0x0c000000) {
			case 0x00000000: return read_clk(dev, clk_src_href);
			case 0x04000000: break;
			case 0x08000000: return read_clk(dev, clk_src_hclk);
			case 0x0c000000:
				return read_clk(dev, clk_src_hclkm3) >> P;
			}
			break;
		default:
			break;
		}
	default:
		break;
	}

	NV_DEBUG(dev, "unknown clock source %d 0x%08x\n", src, mast);
	return 0;
}

int
nv50_pm_clocks_get(struct drm_device *dev, struct nouveau_pm_level *perflvl)
{
	struct drm_nouveau_private *dev_priv = dev->dev_private;
	if (dev_priv->chipset == 0xaa ||
	    dev_priv->chipset == 0xac)
		return 0;

	perflvl->core   = read_clk(dev, clk_src_nvclk);
	perflvl->shader = read_clk(dev, clk_src_sclk);
	perflvl->memory = read_clk(dev, clk_src_mclk);
	if (dev_priv->chipset != 0x50) {
		perflvl->vdec = read_clk(dev, clk_src_vdec);
		perflvl->dom6 = read_clk(dev, clk_src_dom6);
	}

	return 0;
}

struct nv50_pm_state {
	struct hwsq_ucode eclk_hwsq;
	struct hwsq_ucode mclk_hwsq;
	u32 mscript;
};

static u32
calc_pll(struct drm_device *dev, u32 reg, struct pll_lims *pll,
	 u32 clk, int *N1, int *M1, int *log2P)
{
	struct nouveau_pll_vals coef;
	int ret;

	ret = get_pll_limits(dev, reg, pll);
	if (ret)
		return 0;

	pll->vco2.maxfreq = 0;
	pll->refclk = read_pll_ref(dev, reg);
	if (!pll->refclk)
		return 0;

	ret = nouveau_calc_pll_mnp(dev, pll, clk, &coef);
	if (ret == 0)
		return 0;

	*N1 = coef.N1;
	*M1 = coef.M1;
	*log2P = coef.log2P;
	return ret;
}

static inline u32
calc_div(u32 src, u32 target, int *div)
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

static inline u32
clk_same(u32 a, u32 b)
{
	return ((a / 1000) == (b / 1000));
}

static void
nv50_pm_ddr2_hwsq_post(struct drm_device *dev, struct hwsq_ucode *hwsq,
		       struct nouveau_pm_memtiming *timing) {
	struct drm_nouveau_private *dev_priv = dev->dev_private;
	hwsq_wr32(hwsq, 0x1002c4, timing->mr[1]);
	/* Specs say DLL resets automatically when leaving self-refresh
	 * but doesn't harm either */
	hwsq_wr32(hwsq, 0x1002c0, timing->mr[0] | 0x100);
	hwsq_wr32(hwsq, 0x1002c0, timing->mr[0]);
	if (dev_priv->vram_rank_B) {
		hwsq_wr32(hwsq, 0x1002cc, timing->mr[1]);
		hwsq_wr32(hwsq, 0x1002c8, timing->mr[0] | 0x100);
		hwsq_wr32(hwsq, 0x1002c8, timing->mr[0]);
	}
	hwsq_usec(hwsq, 2); /* wait a few Âµs (perhaps too much?)*/
}

static void
nv50_pm_ddr3_hwsq_post(struct drm_device *dev, struct hwsq_ucode *hwsq,
		       struct nouveau_pm_memtiming *timing) {
	struct drm_nouveau_private *dev_priv = dev->dev_private;
	hwsq_wr32(hwsq, 0x1002e0, timing->mr[2]);
	hwsq_wr32(hwsq, 0x1002c4, timing->mr[1]);
	/* Specs say DLL resets automatically when leaving self-refresh
	 * but doesn't harm either */
	hwsq_wr32(hwsq, 0x1002c0, timing->mr[0] | 0x100);
	hwsq_wr32(hwsq, 0x1002c0, timing->mr[0]);
	if (dev_priv->vram_rank_B) {
		hwsq_wr32(hwsq, 0x1002e8, timing->mr[2]);
		hwsq_wr32(hwsq, 0x1002cc, timing->mr[1]);
		hwsq_wr32(hwsq, 0x1002c8, timing->mr[0] | 0x100);
		hwsq_wr32(hwsq, 0x1002c8, timing->mr[0]);
	}
	hwsq_usec(hwsq, 12); /* wait a few Âµs (perhaps too much?)*/
}

static void
nv50_pm_gddr3_hwsq_post(struct drm_device *dev, struct hwsq_ucode *hwsq,
			struct nouveau_pm_memtiming *timing) {
	struct drm_nouveau_private *dev_priv = dev->dev_private;
	hwsq_wr32(hwsq, 0x1002c4, timing->mr[1]);
	if (dev_priv->vram_rank_B)
		hwsq_wr32(hwsq, 0x1002cc, timing->mr[1]);
	/* Reset DLL */
	hwsq_wr32(hwsq, 0x1002c0, timing->mr[0] | 0x100);
	hwsq_wr32(hwsq, 0x1002c0, timing->mr[0]);
	if (dev_priv->vram_rank_B) {
		hwsq_wr32(hwsq, 0x1002c8, timing->mr[0] | 0x100);
		hwsq_wr32(hwsq, 0x1002c8, timing->mr[0]);
	}
	hwsq_usec(hwsq, 1);
	hwsq_wr32(hwsq, 0x1002d4, 0x00000001); /* precharge again */
	hwsq_usec(hwsq, 24); /* wait for DLL to stabilize (40us) */
	hwsq_usec(hwsq, 16);
}

static int
calc_mclk(struct drm_device *dev,
	struct nouveau_pm_level *perflvl,
	struct hwsq_ucode *hwsq, u32 *pmast)
{
	struct drm_nouveau_private *dev_priv = dev->dev_private;
	struct pll_lims pll;
	struct nouveau_pm_engine *pm = &dev_priv->engine.pm;
	struct nouveau_pm_memtiming *timing = perflvl->timing;
	u32 ctrl = nv_rd32(dev, 0x004008);
	u32 coef = nv_rd32(dev, 0x00400c);
	u32 orig = ctrl;
	u32 mast = *pmast;
	u32 crtc_mask = 0;
	u32 freq = perflvl->memory;
	int N, M, P;
	int ret, i;
	u32 full_reclock = 0;

	/* use pcie refclock if possible, otherwise use mpll */
	ctrl &= ~0x81ff0200;
	if (clk_same(freq, read_clk(dev, clk_src_href))) {
		ctrl |= 0x00000200 | (pll.log2p_bias << 19);
	} else {
		ret = calc_pll(dev, 0x4008, &pll, freq, &N, &M, &P);
		if (ret == 0)
			return -EINVAL;

		ctrl |= 0x80000000 | (P << 22) | (P << 16);
		ctrl |= pll.log2p_bias << 19;
		coef  = (N << 8) | M;
	}

	mast &= ~0xc0000000; /* get MCLK_2 from HREF */
	mast |=  0x0000c000; /* use MCLK_2 as MPLL_BYPASS clock */

	/* determine active crtcs */
	for (i = 0; i < 2; i++) {
		if (nv_rd32(dev, NV50_PDISPLAY_CRTC_C(i, CLOCK)))
			crtc_mask |= (1 << i);
	}

	/* Full reclocking capable? */
	if (timing) {
		switch (dev_priv->vram_type) {
		case NV_MEM_TYPE_DDR2:
		case NV_MEM_TYPE_DDR3:
		case NV_MEM_TYPE_GDDR3:
			full_reclock = 0xff;
			break;
		default:
			NV_WARN(dev, "Attempting unsafe memory reclock.");
			break;
		}
	} else {
		NV_WARN(dev, "Attempting unsafe memory reclock.");
	}

	/* build the ucode which will reclock the memory for us */
	hwsq_init(hwsq);
	if (crtc_mask) {
		hwsq_op5f(hwsq, crtc_mask, 0x00); /* wait for scanout */
		hwsq_op5f(hwsq, crtc_mask, 0x01); /* wait for vblank */
	}
	if (dev_priv->chipset >= 0x92)
		hwsq_wr32(hwsq, 0x611200, 0x00003300); /* disable scanout */
	hwsq_setf(hwsq, 0x10, 0); /* disable bus access */
	hwsq_op5f(hwsq, 0x00, 0x01); /* no idea :s */

	/* prepare memory controller */
	hwsq_wr32(hwsq, 0x1002d4, 0x00000001); /* precharge banks and idle */
	hwsq_wr32(hwsq, 0x1002d0, 0x00000001); /* force refresh */
	hwsq_wr32(hwsq, 0x100210, 0x00000000); /* stop the automatic refresh */
	hwsq_wr32(hwsq, 0x1002dc, 0x00000001); /* start self refresh mode */

	/* reclock memory */
	hwsq_wr32(hwsq, 0xc040, mast);
	hwsq_wr32(hwsq, 0x4008, orig | 0x00000200); /* bypass MPLL */
	hwsq_wr32(hwsq, 0x400c, coef);
	hwsq_wr32(hwsq, 0x4008, ctrl);

	/* restart memory controller */
	hwsq_wr32(hwsq, 0x1002d4, 0x00000001); /* precharge banks and idle */
	hwsq_wr32(hwsq, 0x1002dc, 0x00000000); /* stop self refresh mode */
	hwsq_wr32(hwsq, 0x100210, 0x80000000); /* restart automatic refresh */
	hwsq_usec(hwsq, 12); /* wait tXSRD... roughly */

	if (full_reclock) {
		/* Write memory timings */
		if (pm->memtimings.supported && timing->id >= 0) {
			for (i = 0; i < 9; i++)
				hwsq_wr32(hwsq, 0x100220 + (i * 4), timing->reg[i]);
		}
		/* Update the MRs and reset DLL */
		switch (dev_priv->vram_type) {
		case NV_MEM_TYPE_DDR2:
			nv50_pm_ddr2_hwsq_post(dev, hwsq, timing);
			break;
		case NV_MEM_TYPE_DDR3:
			nv50_pm_ddr3_hwsq_post(dev, hwsq, timing);
			break;
		case NV_MEM_TYPE_GDDR3:
			nv50_pm_gddr3_hwsq_post(dev, hwsq, timing);
			break;
		default:
			break;
		}
		if(timing->odt > 0)
			hwsq_wr32(hwsq, 0x1002d4, 0x00000001); /* Precharge */
		hwsq_wr32(hwsq, 0x1002d0, 0x00000001); /* force refresh */
	} else {
		hwsq_usec(hwsq, 48); /* may be unnecessary: causes flickering */
	}
	hwsq_setf(hwsq, 0x10, 1); /* enable bus access */
	hwsq_op5f(hwsq, 0x00, 0x00); /* no idea, reverse of 0x00, 0x01? */
	if (dev_priv->chipset >= 0x92)
		hwsq_wr32(hwsq, 0x611200, 0x00003330); /* enable scanout */
	hwsq_fini(hwsq);

	*pmast = mast;
	return 0;
}

void *
nv50_pm_clocks_pre(struct drm_device *dev, struct nouveau_pm_level *perflvl)
{
	struct drm_nouveau_private *dev_priv = dev->dev_private;
	struct nv50_pm_state *info;
	struct hwsq_ucode *hwsq;
	struct pll_lims pll;
	int clk, ret = -EINVAL;
	int N, M, P1, P2;
	u32 mast = nv_rd32(dev, 0x00c040);
	u32 divs = read_div(dev);
	u32 ctrl, out;

	if (dev_priv->chipset == 0xaa ||
	    dev_priv->chipset == 0xac)
		return ERR_PTR(-ENODEV);

	info = kmalloc(sizeof(*info), GFP_KERNEL);
	if (!info)
		return ERR_PTR(-ENOMEM);

	/* memory: build hwsq ucode which we'll use to reclock memory */
	info->mclk_hwsq.len = 0;
	if (perflvl->memory) {
		clk = calc_mclk(dev, perflvl, &info->mclk_hwsq, &mast);
		if (clk < 0) {
			ret = clk;
			goto error;
		}

		info->mscript = perflvl->memscript;
	}

	/* start building HWSQ script for engine reclocking */
	hwsq = &info->eclk_hwsq;
	hwsq_init(hwsq);
	hwsq_setf(hwsq, 0x10, 0); /* disable bus access */
	hwsq_op5f(hwsq, 0x00, 0x01); /* wait for access disabled? */

	/* vdec/dom6: switch to "safe" clocks temporarily */
	if (perflvl->vdec) {
		mast &= ~0x00000c00;
		divs &= ~0x00000700;
	}

	if (perflvl->dom6) {
		mast &= ~0x0c000000;
		divs &= ~0x00000007;
	}

	hwsq_wr32(hwsq, 0x00c040, mast);

	/* vdec: avoid modifying xpll until we know exactly how the other
	 * clock domains work, i suspect at least some of them can also be
	 * tied to xpll...
	 */
	if (perflvl->vdec) {
		/* see how close we can get using nvclk as a source */
		clk = calc_div(perflvl->core, perflvl->vdec, &P1);

		/* see how close we can get using xpll/hclk as a source */
		if (dev_priv->chipset != 0x98)
			out = read_pll(dev, 0x004030);
		else
			out = read_clk(dev, clk_src_hclkm3d2);
		out = calc_div(out, perflvl->vdec, &P2);

		/* select whichever gets us closest */
		if (abs((int)perflvl->vdec - clk) <=
		    abs((int)perflvl->vdec - out)) {
			if (dev_priv->chipset != 0x98)
				mast |= 0x00000c00;
			divs |= P1 << 8;
		} else {
			mast |= 0x00000800;
			divs |= P2 << 8;
		}
	}

	/* dom6: nfi what this is, but we're limited to various combinations
	 * of the host clock frequency
	 */
	if (perflvl->dom6) {
		if (clk_same(perflvl->dom6, read_clk(dev, clk_src_href))) {
			mast |= 0x00000000;
		} else
		if (clk_same(perflvl->dom6, read_clk(dev, clk_src_hclk))) {
			mast |= 0x08000000;
		} else {
			clk = read_clk(dev, clk_src_hclk) * 3;
			clk = calc_div(clk, perflvl->dom6, &P1);

			mast |= 0x0c000000;
			divs |= P1;
		}
	}

	/* vdec/dom6: complete switch to new clocks */
	switch (dev_priv->chipset) {
	case 0x92:
	case 0x94:
	case 0x96:
		hwsq_wr32(hwsq, 0x004800, divs);
		break;
	default:
		hwsq_wr32(hwsq, 0x004700, divs);
		break;
	}

	hwsq_wr32(hwsq, 0x00c040, mast);

	/* core/shader: make sure sclk/nvclk are disconnected from their
	 * PLLs (nvclk to dom6, sclk to hclk)
	 */
	if (dev_priv->chipset < 0x92)
		mast = (mast & ~0x001000b0) | 0x00100080;
	else
		mast = (mast & ~0x000000b3) | 0x00000081;

	hwsq_wr32(hwsq, 0x00c040, mast);

	/* core: for the moment at least, always use nvpll */
	clk = calc_pll(dev, 0x4028, &pll, perflvl->core, &N, &M, &P1);
	if (clk == 0)
		goto error;

	ctrl  = nv_rd32(dev, 0x004028) & ~0xc03f0100;
	mast &= ~0x00100000;
	mast |= 3;

	hwsq_wr32(hwsq, 0x004028, 0x80000000 | (P1 << 19) | (P1 << 16) | ctrl);
	hwsq_wr32(hwsq, 0x00402c, (N << 8) | M);

	/* shader: tie to nvclk if possible, otherwise use spll.  have to be
	 * very careful that the shader clock is at least twice the core, or
	 * some chipsets will be very unhappy.  i expect most or all of these
	 * cases will be handled by tying to nvclk, but it's possible there's
	 * corners
	 */
	ctrl = nv_rd32(dev, 0x004020) & ~0xc03f0100;

	if (P1-- && perflvl->shader == (perflvl->core << 1)) {
		hwsq_wr32(hwsq, 0x004020, (P1 << 19) | (P1 << 16) | ctrl);
		hwsq_wr32(hwsq, 0x00c040, 0x00000020 | mast);
	} else {
		clk = calc_pll(dev, 0x4020, &pll, perflvl->shader, &N, &M, &P1);
		if (clk == 0)
			goto error;
		ctrl |= 0x80000000;

		hwsq_wr32(hwsq, 0x004020, (P1 << 19) | (P1 << 16) | ctrl);
		hwsq_wr32(hwsq, 0x004024, (N << 8) | M);
		hwsq_wr32(hwsq, 0x00c040, 0x00000030 | mast);
	}

	hwsq_setf(hwsq, 0x10, 1); /* enable bus access */
	hwsq_op5f(hwsq, 0x00, 0x00); /* wait for access enabled? */
	hwsq_fini(hwsq);

	return info;
error:
	kfree(info);
	return ERR_PTR(ret);
}

static int
prog_hwsq(struct drm_device *dev, struct hwsq_ucode *hwsq)
{
	struct drm_nouveau_private *dev_priv = dev->dev_private;
	u32 hwsq_data, hwsq_kick;
	int i;

	if (dev_priv->chipset < 0x90) {
		hwsq_data = 0x001400;
		hwsq_kick = 0x00000003;
	} else {
		hwsq_data = 0x080000;
		hwsq_kick = 0x00000001;
	}

	/* upload hwsq ucode */
	nv_mask(dev, 0x001098, 0x00000008, 0x00000000);
	nv_wr32(dev, 0x001304, 0x00000000);
	for (i = 0; i < hwsq->len / 4; i++)
		nv_wr32(dev, hwsq_data + (i * 4), hwsq->ptr.u32[i]);
	nv_mask(dev, 0x001098, 0x00000018, 0x00000018);

	/* launch, and wait for completion */
	nv_wr32(dev, 0x00130c, hwsq_kick);
	if (!nv_wait(dev, 0x001308, 0x00000100, 0x00000000)) {
		NV_ERROR(dev, "hwsq ucode exec timed out\n");
		NV_ERROR(dev, "0x001308: 0x%08x\n", nv_rd32(dev, 0x001308));
		for (i = 0; i < hwsq->len / 4; i++) {
			NV_ERROR(dev, "0x%06x: 0x%08x\n", 0x1400 + (i * 4),
				 nv_rd32(dev, 0x001400 + (i * 4)));
		}

		return -EIO;
	}

	return 0;
}

int
nv50_pm_clocks_set(struct drm_device *dev, void *data)
{
	struct nv50_pm_state *info = data;
	struct bit_entry M;
	int ret = -EBUSY;

	/* halt and idle execution engines */
	nv_mask(dev, 0x002504, 0x00000001, 0x00000001);
	if (!nv_wait(dev, 0x002504, 0x00000010, 0x00000010))
		goto resume;

	/* program memory clock, if necessary - must come before engine
	 * clock reprogramming due to how we construct the hwsq scripts
	 * in pre()
	 */
	if (info->mclk_hwsq.len) {
		/* execute some scripts that do ??? from the vbios.. */
		if (!bit_table(dev, 'M', &M) && M.version == 1) {
			if (M.length >= 6)
				nouveau_bios_init_exec(dev, ROM16(M.data[5]));
			if (M.length >= 8)
				nouveau_bios_init_exec(dev, ROM16(M.data[7]));
			if (M.length >= 10)
				nouveau_bios_init_exec(dev, ROM16(M.data[9]));
			nouveau_bios_init_exec(dev, info->mscript);
		}

		ret = prog_hwsq(dev, &info->mclk_hwsq);
		if (ret) {
			ret = -EIO;
			goto resume;
		}
	}

	/* program engine clocks */
	ret = prog_hwsq(dev, &info->eclk_hwsq);

resume:
	nv_mask(dev, 0x002504, 0x00000001, 0x00000000);
	kfree(info);
	return ret;
}

static int
pwm_info(struct drm_device *dev, int *line, int *ctrl, int *indx)
{
	if (*line == 0x04) {
		*ctrl = 0x00e100;
		*line = 4;
		*indx = 0;
	} else
	if (*line == 0x09) {
		*ctrl = 0x00e100;
		*line = 9;
		*indx = 1;
	} else
	if (*line == 0x10) {
		*ctrl = 0x00e28c;
		*line = 0;
		*indx = 0;
	} else {
		NV_ERROR(dev, "unknown pwm ctrl for gpio %d\n", *line);
		return -ENODEV;
	}

	return 0;
}

int
nv50_pm_pwm_get(struct drm_device *dev, int line, u32 *divs, u32 *duty)
{
	int ctrl, id, ret = pwm_info(dev, &line, &ctrl, &id);
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
nv50_pm_pwm_set(struct drm_device *dev, int line, u32 divs, u32 duty)
{
	int ctrl, id, ret = pwm_info(dev, &line, &ctrl, &id);
	if (ret)
		return ret;

	nv_mask(dev, ctrl, 0x00010001 << line, 0x00000001 << line);
	nv_wr32(dev, 0x00e114 + (id * 8), divs);
	nv_wr32(dev, 0x00e118 + (id * 8), duty | 0x80000000);
	return 0;
}
