// SPDX-License-Identifier: GPL-2.0
/*
 * CCIF transport: 16 ring queues in the non-cacheable shared memory, one
 * CCIF doorbell channel per queue (BSP hif/ccci_hif_ccif.c, ccci_ringbuf.c).
 * A ring holds packets framed as PKG_HEADER, length | ccci_header + data,
 * 8-byte aligned | PKG_FOOTER x2.
 */
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/slab.h>

#include "ccci.h"

/* KiB per queue, MD_GENERATION >= 6295 (matches MIUI 14's rings) */
static const u16 rx_kib[CCIF_QUEUES] = { 80, 80, 40, 80, 20, 20, 64, 0, 8 };
static const u16 tx_kib[CCIF_QUEUES] = { 128, 40, 8, 40, 20, 20, 64, 0, 8 };
static const u16 rx_exp_kib[CCIF_QUEUES] = { 12, 32, 8, 0, 0, 0, 8 };
static const u16 tx_exp_kib[CCIF_QUEUES] = { 12, 32, 8, 0, 0, 0, 8 };

static struct ccci_ringbuf *ring_create(void *buf, u32 rx, u32 tx)
{
	u32 len = RINGBUF_CTL_LEN + rx + tx;
	struct ccci_ringbuf *rb = buf + 8;
	u32 *w = buf;

	memset(buf, 0, len);
	w[0] = w[1] = RBF_HEADER;
	w = buf + len - 8;
	w[0] = w[1] = RBF_FOOTER;
	rb->rx.length = rx;
	rb->tx.length = tx;
	return rb;
}

static void ring_copy_in(u8 *ring, u32 len, u32 pos, const void *src, u32 n)
{
	u32 first = min(n, len - pos);

	memcpy(ring + pos, src, first);
	memcpy(ring, src + first, n - first);
}

static void ring_copy_out(void *dst, const u8 *ring, u32 len, u32 pos, u32 n)
{
	u32 first = min(n, len - pos);

	memcpy(dst, ring + pos, first);
	memcpy(dst + first, ring, n - first);
}

static u32 wrap(u32 pos, u32 len)
{
	return pos >= len ? pos - len : pos;
}

int ccci_ccif_init(struct ccci_md *md)
{
	void *buf = md->nc + NC_CCISM_MCU;
	int i;

	md->rings_size = 0;
	for (i = 0; i < CCIF_QUEUES; i++) {
		u32 rx = rx_kib[i] * 1024, tx = tx_kib[i] * 1024;
		struct ccci_ringbuf *rb = ring_create(buf, rx, tx);

		md->rxq[i].rb = md->txq[i].rb = rb;
		md->rxq[i].ch = md->txq[i].ch = i;
		spin_lock_init(&md->rxq[i].lock);
		spin_lock_init(&md->txq[i].lock);
		buf += RINGBUF_CTL_LEN + rx + tx;
		md->rings_size += RINGBUF_CTL_LEN + rx + tx;
	}
	/* exception rings follow in their own region; the MD expects them */
	buf = md->nc + NC_CCISM_MCU_EXP;
	for (i = 0; i < CCIF_QUEUES; i++) {
		u32 rx = rx_exp_kib[i] * 1024, tx = tx_exp_kib[i] * 1024;

		ring_create(buf, rx, tx);
		buf += RINGBUF_CTL_LEN + rx + tx;
	}
	wmb();
	return 0;
}

/* clear CCIF busy/channel/irq state and the SRAM (BSP ccci_reset_ccif_hw) */
void ccci_ccif_reset(struct ccci_md *md)
{
	int i;

	writel(BIT(8) | BIT(7), md->infra + 0x150);	/* CCIF0 / CCIF1 reset */
	writel(BIT(8) | BIT(7), md->infra + 0x154);
	for (i = 0; i < CCIF_SRAM_SIZE; i += 4) {
		writel(0, md->ccif_ap + APCCIF_CHDATA + i);
		writel(0, md->ccif_md + APCCIF_CHDATA + i);
	}
	/* SRAM trailer: where the MD keeps its bootup trace */
	writel(0x7274626e, md->ccif_ap + APCCIF_CHDATA + CCIF_SRAM_SIZE - 12);
	writel(NC_MD_VIEW + NC_MDSS_DBG, md->ccif_ap + APCCIF_CHDATA + CCIF_SRAM_SIZE - 8);
	writel(NC_MDSS_DBG_SIZE, md->ccif_ap + APCCIF_CHDATA + CCIF_SRAM_SIZE - 4);
	memset(md->seq, 0, sizeof(md->seq));
}

/* CCIF SRAM takes 32-bit accesses only (byte writes corrupt the word) */
void ccci_sram_write(struct ccci_md *md, u32 off, const void *src, size_t len)
{
	const u32 *w = src;
	size_t i;

	for (i = 0; i < len / 4; i++)
		writel(w[i], md->ccif_ap + APCCIF_CHDATA + off + 4 * i);
}

void ccci_sram_read(struct ccci_md *md, u32 off, void *dst, size_t len)
{
	u32 *w = dst;
	size_t i;

	for (i = 0; i < len / 4; i++)
		w[i] = readl(md->ccif_ap + APCCIF_CHDATA + off + 4 * i);
}

void ccci_ccif_send_ch(struct ccci_md *md, unsigned int ch)
{
	if (readl(md->ccif_ap + APCCIF_BUSY) & BIT(ch))
		return;		/* the MD has not taken the last one yet */
	writel(BIT(ch), md->ccif_ap + APCCIF_BUSY);
	writel(ch, md->ccif_ap + APCCIF_TCHNUM);
}

/* whole packet (ccci_header first) into tx ring txq; channel already set */
int ccci_send_raw(struct ccci_md *md, int txq, void *pkt, size_t len)
{
	struct ccci_queue *q = &md->txq[txq];
	struct ccci_ringbuf *rb = q->rb;
	struct ccci_header *h = pkt;
	u32 hdr[2] = { PKG_HEADER, len }, ftr[2] = { PKG_FOOTER, PKG_FOOTER };
	u32 rlen, rd, wr, need, room;
	u8 *ring;
	unsigned long flags;

	if (txq < 0 || txq >= CCIF_QUEUES || !rb->tx.length || len < sizeof(*h))
		return -EINVAL;
	spin_lock_irqsave(&q->lock, flags);
	rlen = rb->tx.length;
	rd = READ_ONCE(rb->tx.read);
	wr = rb->tx.write;
	need = ALIGN(len + 16, 8);
	room = rd > wr ? rd - wr - 1 : rlen - wr + rd - 1;
	if (need >= room) {
		spin_unlock_irqrestore(&q->lock, flags);
		return -EAGAIN;
	}
	if (h->channel < CCCI_MAX_CH) {
		h->seq_num = md->seq[h->channel]++;
		/* RPC / FS replies after boot must not carry the assert bit */
		h->assert_bit = !((h->channel == CCCI_RPC_TX || h->channel == CCCI_FS_TX) &&
				  md->state != MD_STATE_HS1);
	}
	ring = rb->buffer + rb->rx.length;
	ring_copy_in(ring, rlen, wr, hdr, 8);
	wr = wrap(wr + 8, rlen);
	ring_copy_in(ring, rlen, wr, pkt, len);
	wr = wrap(wr + ALIGN(len, 8), rlen);
	ring_copy_in(ring, rlen, wr, ftr, 8);
	wr = wrap(wr + 8, rlen);
	mb();
	WRITE_ONCE(rb->tx.write, wr);
	ccci_ccif_send_ch(md, q->ch);
	spin_unlock_irqrestore(&q->lock, flags);
	return len;
}

int ccci_send(struct ccci_md *md, int txq, u16 channel, u32 data0, u32 data1,
	      u32 reserved, const void *payload, size_t len)
{
	struct ccci_header *h;
	int ret;

	h = kzalloc(sizeof(*h) + len, GFP_KERNEL);
	if (!h)
		return -ENOMEM;
	h->data[0] = data0;
	h->data[1] = data1;
	h->channel = channel;
	h->reserved = reserved;
	if (len)
		memcpy(h + 1, payload, len);
	ret = ccci_send_raw(md, txq, h, sizeof(*h) + len);
	kfree(h);
	return ret;
}

/* one packet from rx ring q: length, or 0 if empty, <0 on a broken ring */
static int ring_read_pkt(struct ccci_md *md, int qno, void **out)
{
	struct ccci_ringbuf *rb = md->rxq[qno].rb;
	u32 rlen = rb->rx.length, rd = rb->rx.read, wr = READ_ONCE(rb->rx.write);
	u32 hdr[2], ftr[2], avail, plen, total;
	void *pkt;

	if (!rlen)
		return 0;
	avail = wr >= rd ? wr - rd : rlen - rd + wr;
	if (avail < 16 + sizeof(struct ccci_header))
		return 0;
	rmb();
	ring_copy_out(hdr, rb->buffer, rlen, rd, 8);
	if (hdr[0] != PKG_HEADER) {
		dev_err(md->dev, "rxq%d: bad header %#x at %u\n", qno, hdr[0], rd);
		return -EIO;
	}
	plen = hdr[1];
	total = ALIGN(plen + 16, 8);
	if (plen < sizeof(struct ccci_header) || total >= rlen) {
		dev_err(md->dev, "rxq%d: bad length %u\n", qno, plen);
		return -EIO;
	}
	if (total > avail)
		return 0;	/* not complete yet */
	ring_copy_out(ftr, rb->buffer, rlen, wrap(rd + total - 8, rlen), 8);
	if (ftr[0] != PKG_FOOTER || ftr[1] != PKG_FOOTER) {
		dev_err(md->dev, "rxq%d: bad footer\n", qno);
		return -EIO;
	}
	pkt = kmalloc(plen, GFP_KERNEL);
	if (!pkt)
		return -ENOMEM;
	ring_copy_out(pkt, rb->buffer, rlen, wrap(rd + 8, rlen), plen);
	mb();
	WRITE_ONCE(rb->rx.read, wrap(rd + total, rlen));
	*out = pkt;
	return plen;
}

static void dispatch(struct ccci_md *md, void *pkt, size_t len)
{
	struct ccci_header *h = pkt;

	switch (h->channel) {
	case CCCI_CONTROL_RX:
		ccci_recv_control(md, h, pkt, len);
		break;
	default:
		if (!ccci_port_recv(md, h, pkt, len))
			dev_info(md->dev, "rx ch %u len %zu %08x %08x %08x (no port)\n",
				 h->channel, len, h->data[0], h->data[1], h->reserved);
	}
}

void ccci_rx_work(struct work_struct *work)
{
	struct ccci_md *md = container_of(work, struct ccci_md, rx_work);
	int i, n;

	for (i = 0; i < CCIF_QUEUES; i++) {
		if (!test_and_clear_bit(i, &md->pending))
			continue;
		if (i == CCIF_CH_SRAM) {
			/* HS1: the MD's feature query arrives in CCIF SRAM */
			struct { struct ccci_header h; struct md_query_ap_feature f; } __packed q;

			ccci_sram_read(md, SRAM_DL_HEADER, &q, sizeof(q));
			ccci_recv_control(md, &q.h, &q, sizeof(q));
			continue;
		}
		if (i == CCIF_CH_CCB_WAKEUP)
			continue;
		for (;;) {
			void *pkt;

			n = ring_read_pkt(md, i, &pkt);
			if (n <= 0)
				break;
			dispatch(md, pkt, n);
			kfree(pkt);
		}
	}
}

irqreturn_t ccci_ccif_isr(int irq, void *data)
{
	struct ccci_md *md = data;
	u32 ch = readl(md->ccif_ap + APCCIF_RCHNUM);

	writel(ch, md->ccif_ap + APCCIF_ACK);
	if (ch & ~GENMASK(CCIF_CH_EXP_BASE, 0))
		dev_warn(md->dev, "exception channels %#x\n", ch);
	ch &= GENMASK(CCIF_CH_EXP_BASE, 0);
	while (ch) {
		set_bit(__ffs(ch), &md->pending);
		ch &= ch - 1;
	}
	schedule_work(&md->rx_work);
	return IRQ_HANDLED;
}
