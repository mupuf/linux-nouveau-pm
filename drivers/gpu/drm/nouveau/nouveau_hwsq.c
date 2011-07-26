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
 * Authors: Martin Peres
 */

#include "drmP.h"
#include "nouveau_drv.h"
#include "nouveau_hwsq.h"

void
hwsq_upload(struct drm_device *dev, struct hwsq_ucode *hwsq)
{
	struct drm_nouveau_private *dev_priv = dev->dev_private;
	u32 pbus1098 = 0, hwsq_data;
	int i;

	/* The code address base address depends on the chipset */
	if (dev_priv->chipset < 0x90)
		hwsq_data = 0x001400;
	else
		hwsq_data = 0x080000;

	/* disable hwsq mmio access on NV41+ */
	if (dev_priv->chipset > 0x40)
		pbus1098 = nv_mask(dev, 0x001098, 0x00000008, 0x00000000);

	/* upload ucode */
	nv_wr32(dev, 0x001304, 0x00000000);
	for (i = 0; i < hwsq->len / 4; i++)
		nv_wr32(dev, hwsq_data + (i * 4), hwsq->ptr.u32[i]);

	/* Re-enable hwsq mmio access on NV41+ */
	if (dev_priv->chipset > 0x40)
		nv_wr32(dev, 0x001098, pbus1098 | 0x18);
}

int
hwsq_launch(struct drm_device *dev, struct hwsq_ucode *hwsq)
{
	struct drm_nouveau_private *dev_priv = dev->dev_private;
	u32 hwsq_kick;
	int i;

	if (dev_priv->chipset < 0x90)
		hwsq_kick = 0x00000003;
	else
		hwsq_kick = 0x00000001;

	nv_wr32(dev, 0x00130c, hwsq_kick);
	if (!nv_wait(dev, 0x001308, 0x00000100, 0x00000000)) {
		NV_ERROR(dev, "hwsq ucode exec timed out\n");
		NV_ERROR(dev, "0x001308: 0x%08x\n",
			nv_rd32(dev, 0x001308));
		for (i = 0; i < hwsq->len / 4; i++) {
			NV_ERROR(dev, "0x%06x: 0x%08x\n",
				0x1400 + (i * 4),
				nv_rd32(dev, 0x001400 + (i * 4)));
		}
		return -EIO;
	}

	return 0;
}
