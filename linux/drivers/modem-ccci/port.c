// SPDX-License-Identifier: GPL-2.0
/*
 * CCCI channels as character devices (BSP port/port_proxy.c, port_cfg.c).
 * "User header" ports pass whole packets: reads return the ccci_header
 * followed by the payload, writes start with a header of which the driver
 * sets the length (or CCCI_MAGIC for a header-only packet) and channel.
 * The others carry payload only; the driver frames each write. ccci_aud
 * (speech messages) also maps the audio share memory. The AT
 * channel is a WWAN port instead (/dev/wwan0at0, for ModemManager), present
 * only while the MD is ready.
 */
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/wwan.h>

#include "ccci.h"

#define CCCI_MTU		(3584 - 128)
#define CCCI_MAGIC		0xffffffff
#define RXQ_MAX			512

#define NO_WWAN			-1

static const struct {
	const char *name;
	u16 rx, tx;
	u8 txq;
	bool user_header;
	int wwan;		/* WWAN port type, or a char device */
	u32 smem_off, smem_size;	/* nc smem for mmap */
} port_cfg[] = {
	{ "ccci_fs", CCCI_FS_RX, CCCI_FS_TX, 4, true, NO_WWAN },	/* NVRAM files */
	{ "ccci_rpc", CCCI_RPC_RX, CCCI_RPC_TX, 1, true, NO_WWAN },	/* AP queries */
	{ "ccci_sys", CCCI_SYSTEM_RX, CCCI_SYSTEM_TX, 0, true, NO_WWAN },
	{ "ccci_status", CCCI_STATUS_RX, CCCI_STATUS_TX, 0, true, NO_WWAN },
	{ "ccci_ipc", CCCI_IPC_RX, CCCI_IPC_TX, 1, true, NO_WWAN },
	{ "at", CCCI_UART2_RX, CCCI_UART2_TX, 5, false, WWAN_PORT_AT },	/* ttyC0 */
	{ "ccci_mdlog", CCCI_MD_LOG_RX, CCCI_MD_LOG_TX, 2, false, NO_WWAN },	/* ttyC1 */
	{ "ccci_agps", CCCI_IPC_UART_RX, CCCI_IPC_UART_TX, 1, false, NO_WWAN },	/* ttyC2 */
	{ "ccci_mdlog_ctrl", CCCI_UART1_RX, CCCI_UART1_TX, 1, false, NO_WWAN },
	{ "ccci_aud", CCCI_PCM_RX, CCCI_PCM_TX, 0, true, NO_WWAN,
	  NC_AUDIO, NC_AUDIO_SIZE },					/* speech */
};

static const struct class ccci_class = { .name = "ccci" };

bool ccci_port_recv(struct ccci_md *md, const struct ccci_header *h,
		    const void *pkt, size_t len)
{
	struct ccci_port *port = NULL;
	struct sk_buff *skb;
	int i;

	for (i = 0; i < md->num_ports; i++)
		if (md->ports[i].rx_ch == h->channel)
			port = &md->ports[i];
	if (!port)
		return false;
	if (h->channel == CCCI_FS_RX)
		WRITE_ONCE(md->last_fs, jiffies);
	if (!atomic_read(&port->users) || skb_queue_len(&port->rxq) >= RXQ_MAX) {
		/* unsolicited AT results while nobody listens are normal */
		port->rx_drops++;
		if (port->user_header)
			dev_warn_ratelimited(md->dev, "%s: dropped rx (%s)\n", port->name,
					     atomic_read(&port->users) ? "full" : "closed");
		return true;
	}
	skb = alloc_skb(len, GFP_KERNEL);
	if (!skb)
		return true;
	skb_put_data(skb, pkt, len);
	if (!port->user_header)
		skb_pull(skb, sizeof(*h));
	if (port_cfg[port - md->ports].wwan != NO_WWAN) {
		mutex_lock(&md->ports_lock);
		if (port->wwan)
			wwan_port_rx(port->wwan, skb);
		else
			kfree_skb(skb);
		mutex_unlock(&md->ports_lock);
		return true;
	}
	skb_queue_tail(&port->rxq, skb);
	wake_up_interruptible(&port->wq);
	return true;
}

static int port_open(struct inode *inode, struct file *file)
{
	struct ccci_port *port = container_of(inode->i_cdev, struct ccci_port, cdev);

	if (atomic_inc_return(&port->users) > 1) {
		atomic_dec(&port->users);
		return -EBUSY;
	}
	skb_queue_purge(&port->rxq);
	file->private_data = port;
	return nonseekable_open(inode, file);
}

static int port_release(struct inode *inode, struct file *file)
{
	struct ccci_port *port = file->private_data;

	atomic_dec(&port->users);
	skb_queue_purge(&port->rxq);
	return 0;
}

static ssize_t port_read(struct file *file, char __user *buf, size_t count,
			 loff_t *ppos)
{
	struct ccci_port *port = file->private_data;
	struct sk_buff *skb;
	size_t n;
	int ret;

	for (;;) {
		skb = skb_dequeue(&port->rxq);
		if (skb)
			break;
		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;
		ret = wait_event_interruptible(port->wq, !skb_queue_empty(&port->rxq));
		if (ret)
			return ret;
	}
	n = min_t(size_t, count, skb->len);
	if (copy_to_user(buf, skb->data, n)) {
		skb_queue_head(&port->rxq, skb);
		return -EFAULT;
	}
	if (n < skb->len) {
		skb_pull(skb, n);
		skb_queue_head(&port->rxq, skb);
	} else {
		kfree_skb(skb);
	}
	return n;
}

static int send_wait(struct ccci_md *md, struct ccci_port *port, void *pkt,
		     size_t len, bool block)
{
	int ret, tries = 0;

	for (;;) {
		ret = ccci_send_raw(md, port->txq, pkt, len);
		if (ret != -EAGAIN || !block || ++tries > 1000)
			return ret;
		usleep_range(1000, 2000);	/* ring full: the MD is behind */
	}
}

/* frame count payload bytes that follow the header room at h */
static int send_payload(struct ccci_port *port, struct ccci_header *h,
			size_t count, bool block)
{
	h->data[0] = 0;
	h->data[1] = sizeof(*h) + count;
	h->channel = port->tx_ch;
	h->reserved = 0;
	return send_wait(port->md, port, h, sizeof(*h) + count, block);
}

static ssize_t port_write(struct file *file, const char __user *buf,
			  size_t count, loff_t *ppos)
{
	struct ccci_port *port = file->private_data;
	struct ccci_md *md = port->md;
	bool block = !(file->f_flags & O_NONBLOCK);
	size_t hlen = sizeof(struct ccci_header), max;
	struct ccci_header *h;
	int ret;

	if (!count)
		return -EINVAL;
	/* during the handshakes only the file service and RPC may talk */
	if (md->state != MD_STATE_READY &&
	    !(md->state == MD_STATE_HS1 &&
	      (port->tx_ch == CCCI_FS_TX || port->tx_ch == CCCI_RPC_TX)))
		return -ENODEV;
	if (port->user_header) {
		max = CCCI_MTU + hlen + (port->rx_ch == CCCI_FS_RX ? 4 : 0);
		if (count < hlen || count > max)
			return -EMSGSIZE;
		h = memdup_user(buf, count);
		if (IS_ERR(h))
			return PTR_ERR(h);
		if (count == hlen)
			h->data[0] = CCCI_MAGIC;
		else
			h->data[1] = count;
		h->channel = port->tx_ch;
		ret = send_wait(md, port, h, count, block);
		kfree(h);
		return ret < 0 ? ret : count;
	}
	count = min_t(size_t, count, CCCI_MTU);
	h = kmalloc(hlen + count, GFP_KERNEL);
	if (!h)
		return -ENOMEM;
	if (copy_from_user(h + 1, buf, count)) {
		kfree(h);
		return -EFAULT;
	}
	ret = send_payload(port, h, count, block);
	kfree(h);
	return ret < 0 ? ret : count;
}

static __poll_t port_poll(struct file *file, poll_table *wait)
{
	struct ccci_port *port = file->private_data;
	__poll_t mask = EPOLLOUT | EPOLLWRNORM;

	poll_wait(file, &port->wq, wait);
	if (!skb_queue_empty(&port->rxq))
		mask |= EPOLLIN | EPOLLRDNORM;
	return mask;
}

/* the port's share memory region, uncached like the kernel's mapping */
static int port_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct ccci_port *port = file->private_data;
	unsigned long len = vma->vm_end - vma->vm_start;

	if (!port->smem_size || vma->vm_pgoff || len > PAGE_ALIGN(port->smem_size))
		return -EINVAL;
	vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);
	return remap_pfn_range(vma, vma->vm_start, (NC_PHYS + port->smem_off) >> PAGE_SHIFT,
			       len, vma->vm_page_prot);
}

static const struct file_operations port_fops = {
	.owner = THIS_MODULE,
	.open = port_open,
	.release = port_release,
	.read = port_read,
	.write = port_write,
	.poll = port_poll,
	.mmap = port_mmap,
};

static int wwan_start(struct wwan_port *wp)
{
	struct ccci_port *port = wwan_port_get_drvdata(wp);

	atomic_inc(&port->users);
	return 0;
}

static void wwan_stop(struct wwan_port *wp)
{
	struct ccci_port *port = wwan_port_get_drvdata(wp);

	atomic_dec(&port->users);
}

static int wwan_tx(struct wwan_port *wp, struct sk_buff *skb)
{
	struct ccci_port *port = wwan_port_get_drvdata(wp);
	struct ccci_header *h;
	size_t off, n;
	int ret = 0;

	if (port->md->state != MD_STATE_READY)
		return -ENODEV;
	h = kmalloc(sizeof(*h) + CCCI_MTU, GFP_KERNEL);
	if (!h)
		return -ENOMEM;
	for (off = 0; off < skb->len && ret >= 0; off += n) {
		n = min_t(size_t, skb->len - off, CCCI_MTU);
		memcpy(h + 1, skb->data + off, n);
		ret = send_payload(port, h, n, true);
	}
	kfree(h);
	if (ret < 0)
		return ret;
	consume_skb(skb);
	return 0;
}

static const struct wwan_port_ops wwan_ops = {
	.start = wwan_start,
	.stop = wwan_stop,
	.tx = wwan_tx,
};

/* create or remove the WWAN ports as the MD becomes ready or leaves it */
void ccci_ports_online(struct ccci_md *md, bool on)
{
	int i;

	mutex_lock(&md->ports_lock);
	for (i = 0; i < md->num_ports; i++) {
		struct ccci_port *port = &md->ports[i];

		if (port_cfg[i].wwan == NO_WWAN || on == !!port->wwan)
			continue;
		if (!on) {
			wwan_remove_port(port->wwan);
			port->wwan = NULL;
			atomic_set(&port->users, 0);
			continue;
		}
		port->wwan = wwan_create_port(md->dev, port_cfg[i].wwan,
					      &wwan_ops, NULL, port);
		if (IS_ERR(port->wwan)) {
			dev_err(md->dev, "%s: WWAN port: %pe\n", port->name, port->wwan);
			port->wwan = NULL;
		}
	}
	mutex_unlock(&md->ports_lock);
}

static void port_destroy(struct ccci_md *md, int i)
{
	if (port_cfg[i].wwan != NO_WWAN)
		return;
	device_destroy(&ccci_class, md->devt + i);
	cdev_del(&md->ports[i].cdev);
}

int ccci_ports_init(struct ccci_md *md)
{
	int i, ret;

	mutex_init(&md->ports_lock);
	md->num_ports = ARRAY_SIZE(port_cfg);
	md->ports = devm_kcalloc(md->dev, md->num_ports, sizeof(*md->ports),
				 GFP_KERNEL);
	if (!md->ports)
		return -ENOMEM;
	ret = class_register(&ccci_class);
	if (ret)
		return ret;
	ret = alloc_chrdev_region(&md->devt, 0, md->num_ports, "ccci");
	if (ret)
		goto err_class;
	for (i = 0; i < md->num_ports; i++) {
		struct ccci_port *port = &md->ports[i];

		port->name = port_cfg[i].name;
		port->rx_ch = port_cfg[i].rx;
		port->tx_ch = port_cfg[i].tx;
		port->txq = port_cfg[i].txq;
		port->user_header = port_cfg[i].user_header;
		port->smem_off = port_cfg[i].smem_off;
		port->smem_size = port_cfg[i].smem_size;
		port->md = md;
		skb_queue_head_init(&port->rxq);
		init_waitqueue_head(&port->wq);
		if (port_cfg[i].wwan != NO_WWAN)
			continue;	/* created when the MD is ready */
		cdev_init(&port->cdev, &port_fops);
		port->cdev.owner = THIS_MODULE;
		ret = cdev_add(&port->cdev, md->devt + i, 1);
		if (ret)
			goto err_ports;
		port->dev = device_create(&ccci_class, md->dev, md->devt + i,
					  port, "%s", port->name);
		if (IS_ERR(port->dev)) {
			ret = PTR_ERR(port->dev);
			cdev_del(&port->cdev);
			goto err_ports;
		}
	}
	return 0;

err_ports:
	while (--i >= 0)
		port_destroy(md, i);
	unregister_chrdev_region(md->devt, md->num_ports);
err_class:
	class_unregister(&ccci_class);
	return ret;
}

void ccci_ports_exit(struct ccci_md *md)
{
	int i;

	ccci_ports_online(md, false);
	for (i = 0; i < md->num_ports; i++) {
		port_destroy(md, i);
		skb_queue_purge(&md->ports[i].rxq);
	}
	unregister_chrdev_region(md->devt, md->num_ports);
	class_unregister(&ccci_class);
}
