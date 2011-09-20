/*
 * Copyright 2011 - Nouveau Community
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
#include "nouveau_pm.h"

static u8
nv40_counter_signal(struct drm_device *dev, enum nouveau_counter_signal s,
		    u8 *set, u8 *signal)
{
	struct drm_nouveau_private *dev_priv = dev->dev_private;
	u8 chipset = dev_priv->chipset;

	if (set)
		*set = 0;
	if (signal)
		*signal = 0;

	switch (s) {
		case NONE:
		{
			return 0;
		}
		case TIMER_B12:
		{
			if (set)
				*set = 0;
			if (!signal)
				return 0;

			if (chipset == 0x50)
				*signal = 0x27;
			else if (chipset == 0x86)
				*signal = 0x2c;
			else if (chipset == 0x98)
				*signal = 0x37;
			else if (chipset == 0xac)
				*signal = 0x53;
			else if (chipset == 0xa5)
				*signal = 0xa3;
			else
				return -ENOENT;

			return 0;
		}
		case PGRAPH_IDLE:
		{
			if (set)
				*set = 1;
			if (!signal)
				return 0;

			if (chipset == 0x50)
				*signal = 0xc8;
			else if (chipset == 0x86 || chipset == 0x98)
				*signal = 0xbd;
			else if (chipset == 0xac)
				*signal = 0xc9;
			else if (chipset == 0xa5)
				*signal = 0xcb;
			else
				return -ENOENT;

			return 0;
		}
		case PGRAPH_INTR_PENDING:
		{
			if (set)
				*set = 1;
			if (!signal)
				return 0;

			if (chipset == 0x50)
				*signal = 0xca;
			else if (chipset == 0x86 || chipset == 0x98)
				*signal = 0xbf;
			else if (chipset == 0xac)
				*signal = 0xcb;
			else if (chipset == 0xa5)
				*signal = 0xcd;
			else
				return -ENOENT;

			return 0;
		}
		case CTXPROG_ACTIVE:
		{
			if (set)
				*set = 1;
			if (!signal)
				return 0;

			if (chipset == 0x50)
				*signal = 0xd2;
			else if (chipset == 0x86 || chipset == 0x98)
				*signal = 0xc7;
			else if (chipset == 0xac)
				*signal = 0x1c;
			else if (chipset == 0xa5)
				*signal = 0xd5;
			else
				return -ENOENT;

			return 0;
		}
	};

	return -ENOENT;
}

static void
nv40_counter_monitor_signals(struct drm_device *dev, uint8_t set,
			     uint8_t s1, uint8_t s2, uint8_t s3, uint8_t s4)
{
	struct drm_nouveau_private *dev_priv = dev->dev_private;
	struct nouveau_pm_counter *counter = &dev_priv->engine.pm.counter;

	nv_wr32(dev, 0xa7c0 + set * 4, 0x1);
	nv_wr32(dev, 0xa500 + set * 4, 0);
	nv_wr32(dev, 0xa520 + set * 4, 0);

	nv_wr32(dev, 0xa400 + set * 4, s1);
	nv_wr32(dev, 0xa440 + set * 4, s2);
	nv_wr32(dev, 0xa480 + set * 4, s3);
	nv_wr32(dev, 0xa4c0 + set * 4, s4);

	nv_wr32(dev, 0xa420 + set * 4, 0xaaaa);
	nv_wr32(dev, 0xa460 + set * 4, 0xaaaa);
	nv_wr32(dev, 0xa4a0 + set * 4, 0xaaaa);
	nv_wr32(dev, 0xa4e0 + set * 4, 0xaaaa);

	counter->signals[set][0] = s1;
	counter->signals[set][1] = s1;
	counter->signals[set][2] = s2;
	counter->signals[set][3] = s3;
}

static void
pcounter_counters_readout(struct drm_device *dev, bool init)
{
	struct drm_nouveau_private *dev_priv = dev->dev_private;
	struct nouveau_pm_counter *counter = &dev_priv->engine.pm.counter;
	unsigned long flags;
	int s;

	spin_lock_irqsave(&counter->counter_lock, flags);

	u32 debug1 = nv_rd32(dev, 0x400084);
	nv_wr32(dev, 0x400084, debug1 | 0x20);

	for (s = 0; s < 8; s++) {
		counter->sets[s].cycles = nv_rd32(dev, 0xa600 + s * 4);
		counter->sets[s].signals[0] = nv_rd32(dev, 0xa700 + s * 4);
		counter->sets[s].signals[1] = nv_rd32(dev, 0xa6c0 + s * 4);
		counter->sets[s].signals[2] = nv_rd32(dev, 0xa680 + s * 4);
		counter->sets[s].signals[3] = nv_rd32(dev, 0xa740 + s * 4);
	}

	spin_unlock_irqrestore(&counter->counter_lock, flags);

	if (!init && counter->state && counter->on_update)
		counter->on_update(dev);

	/* schedule an update in 100ms (will be improved later on) */
	if (counter->state)
		mod_timer(&counter->readout_timer, jiffies + (HZ / 10));

}

static void
pcounter_counters_readout_periodic(unsigned long data)
{
	struct drm_device *dev = (struct drm_device *)data;
	pcounter_counters_readout(dev, 0);
}

int
nv40_counter_init(struct drm_device *dev)
{
	struct drm_nouveau_private *dev_priv = dev->dev_private;
	struct nouveau_pm_counter *counter = &dev_priv->engine.pm.counter;

	/* initialise the periodic timer */
	setup_timer(&counter->readout_timer,
		    pcounter_counters_readout_periodic, (unsigned long)dev);

	return 0;
}

void
nv40_counter_fini(struct drm_device *dev)
{
	nv40_counter_stop(dev);
}

void
nv40_counter_start(struct drm_device *dev)
{
	struct drm_nouveau_private *dev_priv = dev->dev_private;
	struct nouveau_pm_counter *counter = &dev_priv->engine.pm.counter;
	unsigned long flags;
	u8 timer_b12_sig;
	u8 pgraph_idle_sig;
	u8 pgraph_intr_sig;
	u8 ctxprog_active_sig;

	/* no need to tweak enable on nv40 */

	spin_lock_irqsave(&counter->counter_lock, flags);

	nv40_counter_signal(dev, TIMER_B12, NULL, &timer_b12_sig);
	nv40_counter_signal(dev, PGRAPH_IDLE, NULL, &pgraph_idle_sig);
	nv40_counter_signal(dev, PGRAPH_INTR_PENDING, NULL, &pgraph_intr_sig);
	nv40_counter_signal(dev, CTXPROG_ACTIVE, NULL, &ctxprog_active_sig);

	nv40_counter_monitor_signals(dev, 0, timer_b12_sig, 0, 0, 0);
	nv40_counter_monitor_signals(dev, 1, pgraph_idle_sig, pgraph_intr_sig,
				     ctxprog_active_sig, 0);

	counter->state = 1;

	spin_unlock_irqrestore(&counter->counter_lock, flags);

	pcounter_counters_readout(dev, 1);
}

void
nv40_counter_stop(struct drm_device *dev)
{
	struct drm_nouveau_private *dev_priv = dev->dev_private;
	struct nouveau_pm_counter *counter = &dev_priv->engine.pm.counter;
	unsigned long flags;

	spin_lock_irqsave(&counter->counter_lock, flags);
	counter->state = 0;
	spin_unlock_irqrestore(&counter->counter_lock, flags);

	del_timer_sync(&counter->readout_timer);
}

int
nv40_counter_value(struct drm_device *dev, enum nouveau_counter_signal signal,
		   u32 *val, u32 *count)
{
	struct drm_nouveau_private *dev_priv = dev->dev_private;
	struct nouveau_pm_counter *counter = &dev_priv->engine.pm.counter;
	unsigned long flags;
	u8 set, sig, i;

	nv40_counter_signal(dev, signal, &set, &sig);

	spin_lock_irqsave(&counter->counter_lock, flags);
	for (i = 0; i < 4; i++) {
		if (counter->signals[set][i] == sig) {
			*count = counter->sets[set].cycles;
			*val = counter->sets[set].signals[i];
			spin_unlock_irqrestore(&counter->counter_lock, flags);
			return 0;
		}
	}
	spin_unlock_irqrestore(&counter->counter_lock, flags);

	return -ENOENT;
}