/*
 * Scan the scanout buffer (lk framebuffer, via /dev/mem) for full-width
 * near-white rows and log row/time. Usage: whiteline <seconds>
 */
#define FB_PHYS	0x7b806000UL
#define W	1080
#define H	2400
#define PITCH	(W * 4)

static char buf[64];

static void out(const char *s) { write(1, s, strlen(s)); }

int main(int argc, char **argv)
{
	long secs = argc > 1 ? atol(argv[1]) : 60, hits = 0, scans = 0;
	struct timespec t0, t;
	int fd = open("/dev/mem", O_RDONLY | O_SYNC);
	unsigned int *fb;
	int y, x, prev_hit_row = -1;

	if (fd < 0) { out("open /dev/mem failed\n"); return 1; }
	fb = mmap(NULL, PITCH * H, PROT_READ, MAP_SHARED, fd, FB_PHYS);
	if (fb == MAP_FAILED) { out("mmap failed\n"); return 1; }
	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (;;) {
		clock_gettime(CLOCK_MONOTONIC, &t);
		if (t.tv_sec - t0.tv_sec >= secs)
			break;
		scans++;
		for (y = 0; y < H; y++) {
			unsigned int *row = fb + y * W;
			/* cheap reject: sample 8 pixels first */
			for (x = 0; x < W; x += W / 8)
				if ((row[x] & 0xf0f0f0) != 0xf0f0f0)
					break;
			if (x < W)
				continue;
			for (x = 0; x < W; x++)
				if ((row[x] & 0xf0f0f0) != 0xf0f0f0)
					break;
			if (x < W || y == prev_hit_row)
				continue;
			prev_hit_row = y;
			hits++;
			out("white row "); out(ltoa_r(y, buf));
			out(" at "); out(ltoa_r(t.tv_sec - t0.tv_sec, buf)); out("."); out(ltoa_r(t.tv_nsec / 1000000, buf)); out("s\n");
		}
	}
	out("scans "); out(ltoa_r(scans, buf)); out(" hits "); out(ltoa_r(hits, buf)); out("\n");
	return 0;
}
