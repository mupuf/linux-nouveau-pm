/*
 * Copyright (C) 2011 Roy Spliet.
 *
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial
 * portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE COPYRIGHT OWNER(S) AND/OR ITS SUPPLIERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *   Roy Spliet <r.spliet@student.tudelft.nl>
 */


#include "drmP.h"
#include "nouveau_drv.h"

int
nv50_irq_user_reg(struct drm_device *dev,
		void (*sr)(struct drm_device *, u32 *))
{
	struct drm_nouveau_private *dev_priv = dev->dev_private;
	struct nouveau_user_irq_engine *user_irq = &dev_priv->engine.user_irq;
	struct nouveau_user_irq_handler *handler = &user_irq->handler[0];

	if (handler->service_routine != NULL)
		return -EBUSY;
	handler->service_routine = sr;

	nv_mask(dev, 0x1140, 0x04000000, 0x04000000);
	return 0;
}

void
nv50_irq_user_isr(struct drm_device *dev)
{
	struct drm_nouveau_private *dev_priv = dev->dev_private;
	struct nouveau_user_irq_engine *user_irq = &dev_priv->engine.user_irq;
	struct nouveau_user_irq_handler *handler = &user_irq->handler[0];
	u32 intr;

	nv_mask(dev, 0x1140, 0x04000000, 0);
	intr = nv_rd32(dev, 0x1100);

	if ((intr & 0x04000000) == 0) {
		NV_WARN(dev, "User ISR called without user interrupt");
		return;
	}

	nv_mask(dev, 0x1100, 0x04000000, 0x04000000);

	if (handler->service_routine == NULL) {
		NV_WARN(dev, "User IRQ raised without registered handler");
		return;
	}

	schedule_work(&handler->work);

	return;
}

void
nv50_irq_user_work(struct work_struct *work)
{
	struct nouveau_user_irq_handler *handler =
		container_of(work, struct nouveau_user_irq_handler, work);
	void (*sr)(struct drm_device *, u32 *) =
		handler->service_routine;
	struct drm_device *dev = handler->dev;

	u32 scratch[4];
	int i;

	for (i = 0; i < 4; i++) {
		scratch[i] = nv_rd32(dev, 0x1154 + (i*4));
		nv_wr32(dev, 0x1154 + (i*4), 0x0);
	}

	handler->service_routine = NULL;
	(*sr)(dev, scratch);
}

void
nv50_irq_user_init(struct drm_device *dev)
{
	struct drm_nouveau_private *dev_priv = dev->dev_private;
	struct nouveau_user_irq_engine *user_irq = &dev_priv->engine.user_irq;
	struct nouveau_user_irq_handler *handler = &user_irq->handler[0];

	INIT_WORK(&handler->work, nv50_irq_user_work);
	handler->dev = dev;

	nouveau_irq_register(dev, 28, &nv50_irq_user_isr);
}

void
nv50_irq_user_fini(struct drm_device *dev)
{
	struct drm_nouveau_private *dev_priv = dev->dev_private;
	struct nouveau_user_irq_engine *user_irq = &dev_priv->engine.user_irq;
	struct nouveau_user_irq_handler *handler = &user_irq->handler[0];

	handler->service_routine = NULL;

	nv_wr32(dev, 0x1140, 0x0);
	nouveau_irq_unregister(dev, 28);
	nv_mask(dev, 0x1100, 0x04000000, 0x04000000);
}
