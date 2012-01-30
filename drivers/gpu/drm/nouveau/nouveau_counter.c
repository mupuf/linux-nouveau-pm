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

static void
nouveau_counter_readout_periodic(struct work_struct *work);
static int
nouveau_counter_signal(struct drm_device *dev, enum nouveau_counter_signal s,
		    u8 *set, u8 *signal, const char **disp_name);

static enum nouveau_counter_signal
nouveau_counter_find_signal(struct drm_device *dev, const char *sig_name)
{
	enum nouveau_counter_signal s;
	const char *disp_name = NULL;
	u8 signal;

	/* for all the available signals (not including signal 0) */
	for (s = 1; s < SIG_MAX; s++) {
		nouveau_counter_signal(dev, s, NULL, &signal, &disp_name);
		if (!strncmp(disp_name, sig_name, strlen(disp_name)))
			return s;
		disp_name = NULL;
	}

	return SIG_MAX;
}

static ssize_t
nouveau_counter_show_signal(struct device *d, struct device_attribute *attr,
			    char *buf)
{
	struct drm_device *dev = dev_get_drvdata(d);
	enum nouveau_counter_signal signal;
	u32 val, count;
	u64 tmp;
	int ret;

	signal = nouveau_counter_find_signal(dev, attr->attr.name);
	if (signal == SIG_MAX)
		return -EINVAL;

	nouveau_counter_poll(dev);

	ret = nouveau_counter_value(dev, signal, &val, &count);
	if (ret)
		return ret;

	if (count > 0) {
		tmp = val * 100;
		do_div(tmp, count);
		return sprintf(buf, "%u %u %u\n", val, count, (u8)tmp);
	} else
		return sprintf(buf, "-1 -1 -1\n");
}

static ssize_t
nouveau_counter_get_signal_available(struct device *d,
			struct device_attribute *attr, char *buf)
{
	struct drm_device *dev = dev_get_drvdata(d);
	const char *s_name = NULL;
	size_t pos = 0;
	int s;

	/* for all the available signals (not including signal 0)
	 *
	 * make sure there is always enough space in buf for the final \n\0
	 */
	for (s = 1; s < SIG_MAX; s++) {
		nouveau_counter_signal(dev, s, NULL, NULL, &s_name);
		pos += snprintf(buf + pos, PAGE_SIZE - pos - 2, "%s ", s_name);
		s_name = NULL;
	}
	pos += snprintf(buf + pos, PAGE_SIZE - pos, "\n");

	return pos;
}
static DEVICE_ATTR(signal_available, S_IRUGO,
		   nouveau_counter_get_signal_available,
		   NULL);

static ssize_t
nouveau_counter_set_signal_watch(struct device *d, struct device_attribute *a,
		       const char *buf, size_t count)
{
	struct drm_device *dev = pci_get_drvdata(to_pci_dev(d));
	enum nouveau_counter_signal s = nouveau_counter_find_signal(dev, buf);
	int ret;

	if (s < SIG_MAX) {
		ret = nouveau_counter_watch_signal(dev, s);
		if (!ret)
			return count;
	}

	return -EINVAL;
}
static DEVICE_ATTR(signal_watch, S_IWUSR,
		   NULL,
		   nouveau_counter_set_signal_watch);

static ssize_t
nouveau_counter_set_signal_unwatch(struct device *d, struct device_attribute *a,
		       const char *buf, size_t count)
{
	struct drm_device *dev = pci_get_drvdata(to_pci_dev(d));
	enum nouveau_counter_signal s = nouveau_counter_find_signal(dev, buf);
	int ret;

	if (s < SIG_MAX) {
		ret = nouveau_counter_unwatch_signal(dev, s);
		if (!ret)
			return count;
	}

	return -EINVAL;
}
static DEVICE_ATTR(signal_unwatch, S_IWUSR,
		   NULL,
		   nouveau_counter_set_signal_unwatch);

static ssize_t
nouveau_counter_get_signal_auto_polling(struct device *d,
		struct device_attribute *attr, char *buf)
{
	struct drm_device *dev = dev_get_drvdata(d);
	struct drm_nouveau_private *dev_priv = dev->dev_private;
	struct nouveau_pm_counter *counter = &dev_priv->engine.pm.counter;

	return sprintf(buf, "%i\n", atomic_read(&counter->periodic_polling));
}
static ssize_t
nouveau_counter_set_signal_auto_polling(struct device *d,
		struct device_attribute *a, const char *buf, size_t count)
{
	struct drm_device *dev = pci_get_drvdata(to_pci_dev(d));
	long value;

	if (kstrtol(buf, 10, &value) == -EINVAL)
		return -EINVAL;

	if (value == 1)
		nouveau_counter_start(dev);
	else if (value == 0)
		nouveau_counter_stop(dev);
	else
		return -EINVAL;

	return count;
}
static DEVICE_ATTR(signal_auto_polling, S_IRUGO | S_IWUSR,
		   nouveau_counter_get_signal_auto_polling,
		   nouveau_counter_set_signal_auto_polling);

int
nouveau_counter_init(struct drm_device *dev)
{
	struct drm_nouveau_private *dev_priv = dev->dev_private;
	struct nouveau_pm_counter *counter = &dev_priv->engine.pm.counter;
	struct device *d = &dev->pdev->dev;
	int ret;

	/* enable pcounter */
	atomic_set(&counter->enabled, 1);
	smp_mb();

	/* initialise the periodic timer */
	INIT_DELAYED_WORK(&counter->work_data.d_work,
			  nouveau_counter_readout_periodic);
	counter->work_data.queue = create_singlethread_workqueue("nouveau");
	counter->work_data.dev = dev;

	ret = device_create_file(d, &dev_attr_signal_available);
	if (ret)
		return ret;

	ret = device_create_file(d, &dev_attr_signal_watch);
	if (ret)
		return ret;

	ret = device_create_file(d, &dev_attr_signal_unwatch);
	if (ret)
		return ret;

	ret = device_create_file(d, &dev_attr_signal_auto_polling);
	if (ret)
		return ret;

	return 0;
}

void
nouveau_counter_fini(struct drm_device *dev)
{
	struct drm_nouveau_private *dev_priv = dev->dev_private;
	struct nouveau_pm_counter *counter = &dev_priv->engine.pm.counter;
	struct device *d = &dev->pdev->dev;
	int set, sig;

	device_remove_file(d, &dev_attr_signal_available);
	device_remove_file(d, &dev_attr_signal_watch);
	device_remove_file(d, &dev_attr_signal_unwatch);
	device_remove_file(d, &dev_attr_signal_auto_polling);

	nouveau_counter_stop(dev);

	/* disabled pcounter */
	atomic_set(&counter->enabled, 0);
	smp_mb();

	/* reset the current state and delete the sysfs signal  files */
	for (set = 0; set < 8; set++) {
		for (sig = 0; sig < 4; sig++) {
			counter->sets[set].signals[sig] = 0;
			if (counter->sysfs_attr[set][sig].attr.name != NULL) {
				device_remove_file(d,
					&counter->sysfs_attr[set][sig]);
				counter->sysfs_attr[set][sig].attr.name = NULL;
			}
		}
	}

	destroy_workqueue(counter->work_data.queue);
}

void
nouveau_counter_resume(struct drm_device *dev)
{
	struct drm_nouveau_private *dev_priv = dev->dev_private;
	struct nouveau_pm_counter *counter = &dev_priv->engine.pm.counter;

	/* allow a new pcounter task to be rescheduled */
	atomic_set(&counter->enabled, 1);
	smp_mb();

	if (atomic_read(&counter->periodic_polling))
		nouveau_counter_start(dev);
}

void
nouveau_counter_suspend(struct drm_device *dev)
{
	struct drm_nouveau_private *dev_priv = dev->dev_private;
	struct nouveau_pm_counter *counter = &dev_priv->engine.pm.counter;

	/* forbid a new pcounter task to be rescheduled */
	atomic_set(&counter->enabled, 0);
	smp_mb();

	cancel_delayed_work(&counter->work_data.d_work);
	flush_workqueue(counter->work_data.queue);
}

static int
nouveau_counter_signal(struct drm_device *dev, enum nouveau_counter_signal s,
		    u8 *set, u8 *signal, const char **disp_name)
{
	struct drm_nouveau_private *dev_priv = dev->dev_private;
	u8 chipset = dev_priv->chipset;

	if (set)
		*set = 0;
	if (signal)
		*signal = 0;

	switch (s) {
		case SIG_NONE:
		case SIG_MAX:
		{
			return -EINVAL;
		}
		case SIG_HOST_MEM_WR:
		{
			if (set)
				*set = 0;
			if (disp_name)
				*disp_name = "host_mem_wr";
			if (!signal)
				return 0;

			switch (chipset) {
				case 0x50:
					*signal = 0x00;
					break;
				case 0x86:
				case 0x92:
				case 0x94:
				case 0x98:
					*signal = 0x04;
					break;
				case 0xa0:
					*signal = 0x05;
					break;
				default:
					return -EINVAL;
			}

			return 0;
		}
		case SIG_HOST_MEM_RD:
		{
			if (set)
				*set = 0;
			if (disp_name)
				*disp_name = "host_mem_rd";
			if (!signal)
				return 0;

			switch (chipset) {
				case 0x50:
					*signal = 0x1a;
					break;
				case 0x86:
					*signal = 0x1f;
					break;
				case 0x92:
				case 0x94:
				case 0x98:
					*signal = 0x2a;
					break;
				case 0xa0:
					*signal = 0x2e;
					break;
				case 0xa3:
					*signal = 0x96;
					break;
				default:
					return -EINVAL;
			}

			return 0;
		}
		case SIG_PBUS_PCIE_RD:
		{
			if (set)
				*set = 0;
			if (disp_name)
				*disp_name = "pbus_pcie_rd";
			if (!signal)
				return 0;

			switch (chipset) {
				case 0x50:
					*signal = 0x1d;
					break;
				case 0x86:
					*signal = 0x22;
					break;
				case 0x94:
				case 0x98:
					*signal = 0x2d;
					break;
				case 0xa0:
					*signal = 0x31;
					break;
				case 0xa3:
				case 0xa5:
					*signal = 0x99;
					break;
				default:
					return -EINVAL;
			}

			return 0;
		}
		case SIG_PTIMER_TIME_B12:
		{
			if (set)
				*set = 0;
			if (disp_name)
				*disp_name = "ptimer_time_b12";
			if (!signal)
				return 0;

			switch (chipset) {
				case 0x50:
					*signal = 0x27;
					break;
				case 0x84:
				case 0x86:
					*signal = 0x2c;
					break;
				case 0x92:
					*signal = 0x34;
					break;
				case 0x94:
				case 0x96:
				case 0x98:
					*signal = 0x37;
					break;
				case 0xa0:
					*signal = 0x3b;
					break;
				case 0xac:
					*signal = 0x53;
					break;
				case 0xa3:
				case 0xa5:
				case 0xa8:
					*signal = 0xa3;
					break;
				default:
					return -EINVAL;
			}

			return 0;
		}
		case SIG_PBUS_PCIE_TRANS:
		{
			if (set)
				*set = 0;
			if (disp_name)
				*disp_name = "pbus_pcie_trans";
			if (!signal)
				return 0;

			switch (chipset) {
				case 0x50:
					*signal = 0x29;
					break;
				case 0x86:
					*signal = 0x2e;
					break;
				case 0x92:
					*signal = 0x36;
					break;
				case 0x94:
				case 0x98:
					*signal = 0x39;
					break;
				case 0xa0:
					*signal = 0x3d;
					break;
				case 0xa3:
				case 0xa5:
					*signal = 0xa5;
					break;
				default:
					return -EINVAL;
			}

			return 0;
		}
		case SIG_PBUS_PCIE_WR:
		{
			if (set)
				*set = 0;
			if (disp_name)
				*disp_name = "pbus_pcie_wr";
			if (!signal)
				return 0;

			switch (chipset) {
				case 0x50:
					*signal = 0x2a;
					break;
				case 0x86:
					*signal = 0x2f;
					break;
				case 0x92:
					*signal = 0x37;
					break;
				case 0x94:
				case 0x98:
					*signal = 0x3a;
					break;
				case 0xa0:
					*signal = 0x3e;
					break;
				case 0xa3:
				case 0xa5:
					*signal = 0xa6;
					break;
				default:
					return -EINVAL;
			}

			return 0;
		}
		case SIG_PGRAPH_IDLE:
		{
			if (set)
				*set = 1;
			if (disp_name)
				*disp_name = "pgraph_idle";
			if (!signal)
				return 0;

			switch (chipset) {
				case 0x50:
					*signal = 0xc8;
					break;
				case 0x84:
				case 0x86:
				case 0x92:
				case 0x94:
				case 0x96:
				case 0x98:
					*signal = 0xbd;
					break;
				case 0xa0:
				case 0xac:
					*signal = 0xc9;
					break;
				case 0xa3:
				case 0xa5:
				case 0xa8:
					*signal = 0xcb;
					break;
				default:
					return -EINVAL;
			}

			return 0;
		}
		case SIG_PGRAPH_INTR_PENDING:
		{
			if (set)
				*set = 1;
			if (disp_name)
				*disp_name = "pgraph_intr_pending";
			if (!signal)
				return 0;

			switch (chipset) {
				case 0x50:
					*signal = 0xca;
					break;
				case 0x84:
				case 0x86:
				case 0x92:
				case 0x94:
				case 0x96:
				case 0x98:
					*signal = 0xbf;
					break;
				case 0xa0:
				case 0xac:
					*signal = 0xcb;
					break;
				case 0xa3:
				case 0xa5:
				case 0xa8:
					*signal = 0xcd;
					break;
				default:
					return -EINVAL;
			}

			return 0;
		}
		case SIG_CTXFLAG_1c:
		case SIG_CTXFLAG_1d:
		case SIG_CTXFLAG_1e:
		case SIG_CTXFLAG_1f:
		{
			if (set)
				*set = 1;
			if (disp_name) {
				if (s == SIG_CTXFLAG_1c)
					*disp_name = "ctxflag_1c";
				else if (s == SIG_CTXFLAG_1d)
					*disp_name = "ctxflag_1d";
				else if (s == SIG_CTXFLAG_1e)
					*disp_name = "ctxflag_1e";
				else if (s == SIG_CTXFLAG_1f)
					*disp_name = "ctxflag_1f";
			}
			if (!signal)
				return 0;

			/* find the ctxflag base */
			switch (chipset) {
				case 0x50:
					*signal = 0xd2;
					break;
				case 0x84:
				case 0x86:
				case 0x92:
				case 0x94:
				case 0x96:
				case 0x98:
					*signal = 0xc7;
					break;
				case 0xa0:
				case 0xac:
					*signal = 0x1c;
					break;
				case 0xa3:
				case 0xa5:
				case 0xa8:
					*signal = 0xd5;
					break;
				default:
					return -EINVAL;
			}

			/* set the offset */
			*signal += (s - SIG_CTXFLAG_1c);

			return 0;
		}
	};

	return -EINVAL;
}

static void
nv40_counter_reprogram(struct drm_device *dev)
{
	struct drm_nouveau_private *dev_priv = dev->dev_private;
	struct nouveau_pm_counter *counter = &dev_priv->engine.pm.counter;
	unsigned long flags;
	int set;

	spin_lock_irqsave(&counter->counter_lock, flags);

	for (set = 0; set < 8; set++) {
		nv_wr32(dev, 0xa7c0 + set * 4, 0x1);
		nv_wr32(dev, 0xa500 + set * 4, 0);
		nv_wr32(dev, 0xa520 + set * 4, 0);

		nv_wr32(dev, 0xa400 + set * 4, counter->signals[set][0]);
		nv_wr32(dev, 0xa440 + set * 4, counter->signals[set][1]);
		nv_wr32(dev, 0xa480 + set * 4, counter->signals[set][2]);
		nv_wr32(dev, 0xa4c0 + set * 4, counter->signals[set][3]);

		nv_wr32(dev, 0xa420 + set * 4, 0xaaaa);
		nv_wr32(dev, 0xa460 + set * 4, 0xaaaa);
		nv_wr32(dev, 0xa4a0 + set * 4, 0xaaaa);
		nv_wr32(dev, 0xa4e0 + set * 4, 0xaaaa);
	}

	/* reset the counters */
	nv_mask(dev, 0x400084, 0x20, 0x20);

	/* unmark the must_reprogram state */
	atomic_set(&counter->must_reprogram, 0);
	smp_mb();

	spin_unlock_irqrestore(&counter->counter_lock, flags);
}

static void
nv40_counter_readout(struct drm_device *dev)
{
	struct drm_nouveau_private *dev_priv = dev->dev_private;
	struct nouveau_pm_counter *counter = &dev_priv->engine.pm.counter;
	unsigned long flags;
	int s;

	spin_lock_irqsave(&counter->counter_lock, flags);

	/* readout */
	nv_mask(dev, 0x400084, 0x0, 0x20);

	for (s = 0; s < 8; s++) {
		counter->sets[s].cycles = nv_rd32(dev, 0xa600 + s * 4);
		counter->sets[s].signals[0] = nv_rd32(dev, 0xa700 + s * 4);
		counter->sets[s].signals[1] = nv_rd32(dev, 0xa6c0 + s * 4);
		counter->sets[s].signals[2] = nv_rd32(dev, 0xa680 + s * 4);
		counter->sets[s].signals[3] = nv_rd32(dev, 0xa740 + s * 4);
	}

	spin_unlock_irqrestore(&counter->counter_lock, flags);

	if (counter->on_update)
		counter->on_update(dev);
}

int
nouveau_counter_watch_signal(struct drm_device *dev,
			    enum nouveau_counter_signal wanted_signal)
{
	struct drm_nouveau_private *dev_priv = dev->dev_private;
	struct nouveau_pm_counter *counter = &dev_priv->engine.pm.counter;
	struct device *d = &dev->pdev->dev;
	const char *sig_name = NULL;
	unsigned long flags;
	u8 signal, set;
	int i, ret;

	ret = nouveau_counter_signal(dev, wanted_signal,
				     &set, &signal, &sig_name);
	if (ret)
		return ret;

	spin_lock_irqsave(&counter->counter_lock, flags);

	for (i = 0; i < 4; i++) {
		if (counter->signals[set][i] == 0 ||
		    counter->signals[set][i] == signal) {
			counter->signals[set][i] = signal;

			/* create the sysfs entry */
			counter->sysfs_attr[set][i].attr.name = sig_name;
			counter->sysfs_attr[set][i].attr.mode = S_IRUGO;
			counter->sysfs_attr[set][i].show =
						nouveau_counter_show_signal;
			counter->sysfs_attr[set][i].store = NULL;
			device_create_file(d, &counter->sysfs_attr[set][i]);

			/* set the must_reprogram state */
			atomic_set(&counter->must_reprogram, 1);
			smp_mb();

			spin_unlock_irqrestore(&counter->counter_lock, flags);
			return 0;
		}
	}

	spin_unlock_irqrestore(&counter->counter_lock, flags);

	return -ENOSPC;
}

int
nouveau_counter_unwatch_signal(struct drm_device *dev,
			    enum nouveau_counter_signal wanted_signal)
{
	struct drm_nouveau_private *dev_priv = dev->dev_private;
	struct nouveau_pm_counter *counter = &dev_priv->engine.pm.counter;
	struct device *d = &dev->pdev->dev;
	unsigned long flags;
	u8 signal, set;
	int i, ret;

	ret = nouveau_counter_signal(dev, wanted_signal, &set, &signal, NULL);
	if (ret)
		return ret;

	spin_lock_irqsave(&counter->counter_lock, flags);

	for (i = 0; i < 4; i++) {
		if (counter->signals[set][i] == signal) {
			counter->signals[set][i] = 0;

			/* delete the sysfs entry */
			device_remove_file(d, &counter->sysfs_attr[set][i]);
			counter->sysfs_attr[set][i].attr.name = NULL;

			spin_unlock_irqrestore(&counter->counter_lock, flags);
			return 0;
		}
	}

	spin_unlock_irqrestore(&counter->counter_lock, flags);

	return -ENOENT;
}

void
nouveau_counter_poll(struct drm_device *dev)
{
	struct drm_nouveau_private *dev_priv = dev->dev_private;
	struct nouveau_pm_counter *counter = &dev_priv->engine.pm.counter;

	/* do not poll if we are in continuous mode */
	if (atomic_read(&counter->periodic_polling))
		return;

	nv40_counter_reprogram(dev);
	msleep(100);

	if (dev_priv->card_type >= NV_40 && dev_priv->card_type < NV_C0)
		nv40_counter_readout(dev);
}

static void
nouveau_counter_readout_periodic(struct work_struct *work)
{
	struct nouveau_pm_counter_wd *data =
		container_of(work, struct nouveau_pm_counter_wd, d_work.work);
	struct drm_nouveau_private *dev_priv = data->dev->dev_private;
	struct nouveau_pm_counter *counter = &dev_priv->engine.pm.counter;

	nv40_counter_readout(data->dev);

	if (atomic_read(&counter->must_reprogram))
		nv40_counter_reprogram(data->dev);

	/* re-schedule the work if needed */
	if (atomic_read(&counter->enabled) &&
	    atomic_read(&counter->periodic_polling))
		queue_delayed_work(counter->work_data.queue,
				   &counter->work_data.d_work, (HZ / 10));
}

void
nouveau_counter_start(struct drm_device *dev)
{
	struct drm_nouveau_private *dev_priv = dev->dev_private;
	struct nouveau_pm_counter *counter = &dev_priv->engine.pm.counter;

	nv40_counter_reprogram(dev);

	atomic_set(&counter->periodic_polling, 1);
	smp_mb();

	queue_delayed_work(counter->work_data.queue,
			   &counter->work_data.d_work, (HZ / 10));
}

void
nouveau_counter_stop(struct drm_device *dev)
{
	struct drm_nouveau_private *dev_priv = dev->dev_private;
	struct nouveau_pm_counter *counter = &dev_priv->engine.pm.counter;

	atomic_set(&counter->periodic_polling, 0);
	smp_mb();

	cancel_delayed_work(&counter->work_data.d_work);
	flush_workqueue(counter->work_data.queue);
}

int
nouveau_counter_value(struct drm_device *dev,
		enum nouveau_counter_signal signal, u32 *val, u32 *count)
{
	struct drm_nouveau_private *dev_priv = dev->dev_private;
	struct nouveau_pm_counter *counter = &dev_priv->engine.pm.counter;
	unsigned long flags;
	u8 set, sig, i;

	nouveau_counter_signal(dev, signal, &set, &sig, NULL);

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
