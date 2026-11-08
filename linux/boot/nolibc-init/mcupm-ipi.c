/*
 * Send one CPU-DVFS IPI to MCUPM (mbox1) and poll for the reply.
 * Usage: mcupm-ipi <cmd> [arg0 arg1 arg2]
 * Protocol from the BSP (mtk_rpmsg.c mtk_mbox_send / mtk-mbox.c polling).
 */
#define MBOX_REGS	0x10300000UL
#define REG_SET		0x154	/* mboxN_set: raise irq to MCUPM */
#define REG_CLR		0x074	/* mboxN_clr: ack reply irq */
#define REG_SEND	0x150	/* send status */
#define REG_RECV	0x078	/* recv status */
#define MBOX1_SRAM	0x1031fd80UL
#define PIN		1	/* CPU DVFS pin index == mbox 1 */
#define REPLY_SLOT	0x14	/* PIN_R_OFFSET_CPU_DVFS, in 4-byte slots */

static char buf[32];
static void out(const char *s) { write(1, s, strlen(s)); }
static void hex(const char *p, unsigned int v) { out(p); utoh_r(v, buf); out(buf); out(" "); }

int main(int argc, char **argv)
{
	int fd = open("/dev/mem", O_RDWR | O_SYNC);
	volatile unsigned int *regs, *sram;
	unsigned int msg[4] = { 0 }, i, t;
	char *page;

	if (fd < 0 || argc < 2) { out("usage\n"); return 1; }
	for (i = 0; i < 4 && i + 1 < (unsigned)argc; i++)
		msg[i] = atol(argv[i + 1]);
	regs = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, MBOX_REGS);
	page = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, MBOX1_SRAM & ~0xfffUL);
	if (regs == MAP_FAILED || page == MAP_FAILED) { out("mmap failed\n"); return 1; }
	sram = (volatile unsigned int *)(page + (MBOX1_SRAM & 0xfff));

	hex("send_sta", regs[REG_SEND / 4]); hex("recv_sta", regs[REG_RECV / 4]); out("\n");
	if (regs[REG_SEND / 4] & (1 << PIN)) { out("pin busy\n"); return 2; }
	for (i = 0; i < 4; i++)
		sram[i] = msg[i];
	__sync_synchronize();
	regs[REG_SET / 4] = 1 << PIN;
	for (t = 0; t < 2000; t++) {
		if (regs[REG_RECV / 4] & (1 << PIN))
			break;
		usleep(1000);
	}
	if (t == 2000) { hex("timeout; send_sta", regs[REG_SEND / 4]); out("\n"); return 3; }
	regs[REG_CLR / 4] = 1 << PIN;
	out("reply after "); utoa_r(t, buf); out(buf); out(" ms: ");
	for (i = 0; i < 4; i++)
		hex("", sram[REPLY_SLOT + i]);
	out("\n");
	return 0;
}
