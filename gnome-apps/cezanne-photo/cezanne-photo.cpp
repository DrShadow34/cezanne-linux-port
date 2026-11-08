// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * cezanne-photo: post-processing of Snapshot pictures on the Redmi K30 Ultra
 * (in place, in the background):
 *
 *   cezanne-photo [--denoise 1|2|3 [--lowlight <bracket prefix>]]
 *                 [--portrait <bracket prefix>]
 *                 [--hdr <bracket prefix>] [--upscale 2|4]
 *                 [--quality N] [--marker <file>] <picture> [out]
 *
 * The picture is written back in its format (by extension): PNG, or JPEG at
 * --quality (default 95).
 *
 * --denoise: noise reduction (low light), before the HDR step whose shadow
 * lift would amplify the noise.  Luma: non-local means (h 2 / 3 / 4.5; the
 * strongest level with the wide search window, ~7 s for 16 MP instead of
 * ~2 s), part of the original blended back.  Chroma: smoothed at a quarter
 * of the resolution, where the blotchy colour noise of high gains lives.
 *
 * --lowlight: the noise reduction guided by the mono frame taken with the
 * picture (lowLight() below), at the --denoise level; the plain one where
 * the frames do not match.
 *
 * --portrait: the background blurred around the subject, after noise
 * reduction and HDR (the bracket's frames are sharp: merged after the blur
 * they put background detail back): the subject from the mask of
 * cezanne-upscaled on the APU without what its depth puts well behind the
 * subject, the blur by that depth (portraitApu(), seconds; --subject
 * objects: the salient object, the default; --subject people: the people
 * in the picture), else by depth from the main + mono stereo pair
 * (portrait(), minutes; it uses the bracket's middle (EV 0) frame;
 * CEZANNE_PORTRAIT=stereo always).  The networks see the picture upright
 * by its EXIF orientation (sideways they find no subject).
 *
 * --upscale: AI super-resolution as the last step, natively 4x; 2x is that
 * reduced by Lanczos (a 16 MP picture becomes 64 MP).  On the APU when
 * cezanne-upscaled is there (package cezanne-upscale): Real-ESRGAN x4plus
 * (Upscayl's standard model) in int16 on both MDLA cores, ~7.5 minutes
 * for 16 MP.  Otherwise on the GPU
 * (realesrgan-ncnn-vulkan through Vulkan) with Upscayl's compact
 * "upscayl-lite-4x", ~10 minutes for 16 MP on the Mali-G77 (x4plus there
 * would take ~2 hours).
 *
 * --marker: a file deleted when done, whatever the outcome (Snapshot shows
 * the picture as being processed while it exists, also after a restart);
 * the helper's pid is appended to it, so long jobs are told from dead ones.
 * The bracket files are deleted after use.
 *
 * --hdr: widens the dynamic range of a colour picture (main or ultrawide
 * camera) with an exposure bracket from the monochrome sensor taken at the
 * same moment (cezanne-mono):
 *
 * 1. The bracket (<prefix>-0/1/2.pgm: EV -2, 0, +1.5, 8-bit, black level 16,
 *    exposure lines and gain code in the PGM comment) merges into a mono
 *    radiance map: linear sensor, hat weights away from black and clipping.
 * 2. Registration: ORB features of the radiance map (log tone mapped) and of
 *    the picture's luma at the mono resolution, homography by RANSAC.  Both
 *    90 degree orientations of the mono frame are tried (its mounting
 *    differs from the colour sensors'); too few inliers: the picture stays
 *    as it is.
 * 3. HDR luminance at that resolution: the picture's own (linearised) luma
 *    where it is well exposed, the mono radiance (scaled to it) in clipped
 *    highlights and deep shadows.
 * 4. Local tone mapping in the log domain on a smooth base layer: midtones
 *    unchanged, highlights compressed, shadows lifted; detail kept.
 * 5. The ratio to the picture's luma, a smooth gain map, is upsampled and
 *    applied to the full-resolution picture in linear light, so colour and
 *    fine detail stay the colour camera's.  Outside the mono field of view
 *    (ultrawide) the gain fades to 1.
 * EXIF is copied from the input.  Exit status 0: written; 2: HDR not
 * aligned, 3: upscaling failed, 4: no portrait (the picture is still
 * written if another step changed it).
 */
#include <cmath>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include <exiv2/exiv2.hpp>
#include <opencv2/calib3d.hpp>
#include <opencv2/features2d.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/photo.hpp>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

constexpr int kMinInliers = 40;

/*
 * A mono frame (cezanne-mono): 10-bit (maxval 1023, black 64) or, from
 * older helpers, 8-bit (maxval 255, black 16); as float in its own levels
 */
struct Frame {
	cv::Mat img;		/* CV_32F */
	float black, white;	/* sensor black level, clipping */
	double scale;		/* exposure lines x gain (1x = 16) */
};

bool readPgm(const std::string &path, Frame &f)
{
	std::ifstream in(path, std::ios::binary);
	std::string magic, line;
	int w = 0, h = 0, maxv = 0, lines = 1, gain = 16;

	if (!(in >> magic) || magic != "P5")
		return false;
	in.get();
	while (in.peek() == '#') {
		std::getline(in, line);
		std::sscanf(line.c_str(), "# exposure %d gain %d", &lines, &gain);
	}
	in >> w >> h >> maxv;
	in.get();
	if (maxv > 255) {
		/* 16-bit samples, big-endian */
		cv::Mat be(h, w, CV_16UC1);
		in.read(reinterpret_cast<char *>(be.data), size_t(w) * h * 2);
		for (auto it = be.begin<uint16_t>(); it != be.end<uint16_t>(); ++it)
			*it = uint16_t(*it << 8 | *it >> 8);
		be.convertTo(f.img, CV_32F);
	} else {
		cv::Mat u8(h, w, CV_8UC1);
		in.read(reinterpret_cast<char *>(u8.data), size_t(w) * h);
		u8.convertTo(f.img, CV_32F);
	}
	/* MIPI sensors: black at 1/16 of the range */
	f.black = (maxv + 1) / 16.0f;
	f.white = maxv * 0.98f;
	f.scale = double(lines) * gain / 16.0;
	return bool(in);
}

/* sRGB-ish 8-bit <-> linear (the soft ISP output: gamma ~2.2) */
cv::Mat lutLinear()
{
	cv::Mat lut(1, 256, CV_32F);
	for (int i = 0; i < 256; i++)
		lut.at<float>(i) = std::pow(i / 255.0f, 2.2f);
	return lut;
}

cv::Mat toLinear(const cv::Mat &u8)
{
	static const cv::Mat lut = lutLinear();
	cv::Mat f;
	cv::LUT(u8, lut, f);
	return f;
}

/* weighted merge of the bracket: radiance in "per unit of exposure" */
cv::Mat mergeBracket(const std::vector<Frame> &frames)
{
	cv::Size size = frames[0].img.size();
	cv::Mat num = cv::Mat::zeros(size, CV_32F), den = cv::Mat::zeros(size, CV_32F);

	for (const Frame &f : frames) {
		cv::Mat v = f.img, w;
		/* hat: 0 just above black and at clipping, 1 in the middle */
		float lo = f.black + (f.white - f.black) / 60, hi = f.white;
		cv::Mat fromBlack = v - lo, toClip = hi - v;
		cv::min(fromBlack, toClip, w);
		w = cv::max(w, 0) / ((hi - lo) / 2.0f);
		w = cv::min(w, 1.0f);
		/* radiance in 8-bit levels (240 to white) per unit of exposure */
		cv::Mat r = cv::max(v - f.black, 0) * (240.0f / (f.white - f.black)) / f.scale;
		num += w.mul(r);
		den += w;
	}
	/* nothing well exposed: the shortest frame (highlights) */
	cv::Mat r0, none = den < 1e-3f;
	const Frame &f0 = frames[0];
	r0 = cv::max(f0.img - f0.black, 0) * (240.0f / (f0.white - f0.black)) / f0.scale;
	cv::Mat rad = num / cv::max(den, 1e-3f);
	r0.copyTo(rad, none);
	return rad;
}

/* log tone map to 8 bits, for feature matching */
cv::Mat viewU8(const cv::Mat &rad)
{
	cv::Mat l, u8;
	cv::log(rad + 1e-4f, l);
	double lo, hi;
	cv::minMaxLoc(l, &lo, &hi);
	l.convertTo(u8, CV_8U, 255.0 / (hi - lo), -lo * 255.0 / (hi - lo));
	cv::equalizeHist(u8, u8);
	return u8;
}

/* homography mono -> picture (small); empty if it does not hold */
cv::Mat align(const cv::Mat &mono8, const cv::Mat &pic8, int &inliers)
{
	auto orb = cv::ORB::create(5000);
	std::vector<cv::KeyPoint> ka, kb;
	cv::Mat da, db;

	orb->detectAndCompute(mono8, cv::noArray(), ka, da);
	orb->detectAndCompute(pic8, cv::noArray(), kb, db);
	inliers = 0;
	if (da.empty() || db.empty())
		return {};

	cv::BFMatcher matcher(cv::NORM_HAMMING);
	std::vector<std::vector<cv::DMatch>> knn;
	matcher.knnMatch(da, db, knn, 2);
	std::vector<cv::Point2f> pa, pb;
	for (const auto &m : knn) {
		if (m.size() == 2 && m[0].distance < 0.8f * m[1].distance) {
			pa.push_back(ka[m[0].queryIdx].pt);
			pb.push_back(kb[m[0].trainIdx].pt);
		}
	}
	if (pa.size() < size_t(kMinInliers))
		return {};
	std::vector<uchar> mask;
	cv::Mat H = cv::findHomography(pa, pb, cv::RANSAC, 4.0, mask);
	for (uchar m : mask)
		inliers += m;
	return inliers >= kMinInliers ? H : cv::Mat();
}

/* rotate by a cv::RotateFlags code, -1: none */
cv::Mat rotated(const cv::Mat &m, int code)
{
	if (code < 0)
		return m.clone();
	cv::Mat r;
	cv::rotate(m, r, code);
	return r;
}

int inverseRotation(int code)
{
	switch (code) {
	case cv::ROTATE_90_CLOCKWISE: return cv::ROTATE_90_COUNTERCLOCKWISE;
	case cv::ROTATE_90_COUNTERCLOCKWISE: return cv::ROTATE_90_CLOCKWISE;
	default: return code;
	}
}

/*
 * The quarter turn that shows the picture upright (a cv::RotateFlags code,
 * -1: none) from its Exif orientation: Snapshot stores the frame as the
 * sensor sees it held upright and tags the device's orientation.
 */
int uprightRotation(const std::string &path)
{
	try {
		auto img = Exiv2::ImageFactory::open(path);
		img->readMetadata();
		auto &exif = img->exifData();
		auto it = exif.findKey(Exiv2::ExifKey("Exif.Image.Orientation"));
		if (it != exif.end()) {
			switch (it->toInt64()) {
			case 3: return cv::ROTATE_180;
			case 6: return cv::ROTATE_90_CLOCKWISE;
			case 8: return cv::ROTATE_90_COUNTERCLOCKWISE;
			}
		}
	} catch (const std::exception &e) {
		std::fprintf(stderr, "cezanne-photo: exif: %s\n", e.what());
	}
	return -1;
}

bool copyExif(const std::string &from, const std::string &to)
{
	try {
		auto src = Exiv2::ImageFactory::open(from);
		src->readMetadata();
		auto dst = Exiv2::ImageFactory::open(to);
		dst->setExifData(src->exifData());
		dst->setXmpData(src->xmpData());
		dst->writeMetadata();
		return true;
	} catch (const std::exception &e) {
		std::fprintf(stderr, "cezanne-photo: exif: %s\n", e.what());
		return false;
	}
}

cv::Mat denoise(const cv::Mat &pic, int level)
{
	/*
	 * Luma gently, with part of the original detail blended back: full
	 * strength NLM (h 3 / 5 / 8) kept 68 / 42 / 22 % of the picture's
	 * high-frequency energy and made surfaces waxy.  Chroma (the blotchy
	 * colour noise) fully.
	 */
	static const float lumaH[] = { 0, 2, 3, 4.5 };
	static const double keep[] = { 1, 0.35, 0.3, 0.25 };
	static const double chromaSigma[] = { 0, 1.0, 1.5, 2.0 };
	cv::Mat ycc, out;
	std::vector<cv::Mat> ch;

	cv::cvtColor(pic, ycc, cv::COLOR_BGR2YCrCb);
	cv::split(ycc, ch);
	cv::Mat luma;
	if (level >= 3)
		cv::fastNlMeansDenoising(ch[0], luma, lumaH[level], 7, 21);
	else
		cv::fastNlMeansDenoising(ch[0], luma, lumaH[level], 5, 11);
	cv::addWeighted(luma, 1 - keep[level], ch[0], keep[level], 0, ch[0]);
	for (int c = 1; c < 3; c++) {
		cv::Mat small;
		cv::resize(ch[c], small, cv::Size(), 0.25, 0.25, cv::INTER_AREA);
		cv::GaussianBlur(small, small, cv::Size(0, 0), chromaSigma[level]);
		cv::resize(small, ch[c], ch[c].size(), 0, 0, cv::INTER_LINEAR);
	}
	cv::merge(ch, ycc);
	cv::cvtColor(ycc, out, cv::COLOR_YCrCb2BGR);
	return out;
}

constexpr const char *kApuUpscaler = "/run/cezanne-upscale.sock";
constexpr int kSegSize = 320;		/* cezanne-upscaled's segment job */
constexpr int kDepthSize = 256;		/* and its depth job */
constexpr const char *kUpscaler = "realesrgan-ncnn-vulkan";
constexpr const char *kUpscaleModels = "/usr/share/realesrgan/models";
constexpr const char *kUpscaleModel = "upscayl-lite-4x";

/* msg with two file descriptors (SCM_RIGHTS) */
bool sendFds(int s, const char *msg, size_t len, int a, int b)
{
	iovec iov = { const_cast<char *>(msg), len };
	alignas(cmsghdr) char ctl[CMSG_SPACE(2 * sizeof(int))] = {};
	msghdr m = {};
	m.msg_iov = &iov;
	m.msg_iovlen = 1;
	m.msg_control = ctl;
	m.msg_controllen = sizeof(ctl);
	cmsghdr *c = CMSG_FIRSTHDR(&m);
	c->cmsg_level = SOL_SOCKET;
	c->cmsg_type = SCM_RIGHTS;
	c->cmsg_len = CMSG_LEN(2 * sizeof(int));
	int fds[2] = { a, b };
	std::memcpy(CMSG_DATA(c), fds, sizeof(fds));
	return sendmsg(s, &m, MSG_NOSIGNAL) == static_cast<ssize_t>(len);
}

/* cezanne-upscaled's replies until "ok" (true) or "error" / a closed socket;
 * progress to stderr every 10 % */
bool upscaleReplies(int s)
{
	int tiles = 0, shown = -1;
	char buf[512];
	ssize_t n;
	while ((n = recv(s, buf, sizeof(buf) - 1, 0)) > 0) {
		buf[n] = 0;
		int k;
		if (std::strstr(buf, "\"ok\""))
			return true;
		if (std::strstr(buf, "\"error\"")) {
			std::fprintf(stderr, "cezanne-photo: cezanne-upscaled: %s\n", buf);
			return false;
		}
		if (std::sscanf(buf, "{\"tiles\": %d}", &k) == 1)
			tiles = k;
		else if (std::sscanf(buf, "{\"done\": %d}", &k) == 1 && tiles &&
			 k * 10 / tiles != shown) {
			shown = k * 10 / tiles;
			std::fprintf(stderr, "cezanne-photo: upscaling on the APU: %d %%\n", shown * 10);
		}
	}
	return false;
}

/*
 * The picture 4x through cezanne-upscaled (Real-ESRGAN x4plus on the APU's
 * MDLA cores): it gets the pixels and the result buffer as memfds.  The
 * result stays in that mapping (768 MB for 16 MP, not copied; munmap it with
 * total() * 3 bytes).  Empty when the service is not there or fails.
 */
/* a connection to cezanne-upscaled, or -1 */
int connectApu()
{
	int s = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
	if (s < 0)
		return -1;
	sockaddr_un addr = {};
	addr.sun_family = AF_UNIX;
	std::strncpy(addr.sun_path, kApuUpscaler, sizeof(addr.sun_path) - 1);
	if (connect(s, reinterpret_cast<sockaddr *>(&addr), sizeof(addr))) {
		close(s);
		return -1;
	}
	return s;
}

cv::Mat upscaleApu(const cv::Mat &pic)
{
	if (pic.type() != CV_8UC3)
		return {};
	int s = connectApu();
	if (s < 0)
		return {};
	size_t inSize = pic.total() * 3, outSize = inSize * 16;
	int in = memfd_create("upscale-in", MFD_CLOEXEC);
	int out = memfd_create("upscale-out", MFD_CLOEXEC);
	void *po = MAP_FAILED;
	if (in >= 0 && out >= 0 && !ftruncate(in, inSize) && !ftruncate(out, outSize)) {
		void *pi = mmap(nullptr, inSize, PROT_READ | PROT_WRITE, MAP_SHARED, in, 0);
		if (pi != MAP_FAILED) {
			pic.copyTo(cv::Mat(pic.rows, pic.cols, CV_8UC3, pi));
			munmap(pi, inSize);
			char req[96];
			int n = std::snprintf(req, sizeof(req), "{\"width\": %d, \"height\": %d, \"order\": \"bgr\"}",
					      pic.cols, pic.rows);
			if (sendFds(s, req, n, in, out) && upscaleReplies(s))
				po = mmap(nullptr, outSize, PROT_READ | PROT_WRITE, MAP_SHARED, out, 0);
		}
	}
	for (int fd : { in, out, s })
		if (fd >= 0)
			close(fd);
	if (po == MAP_FAILED)
		return {};
	return cv::Mat(pic.rows * 4, pic.cols * 4, CV_8UC3, po);
}

/*
 * A float map of the picture from one of cezanne-upscaled's networks (job
 * `job`, `model` if not null): the picture scaled to size x size here (area
 * averaging) and turned upright by `upright` (the networks know upright
 * scenes: a sideways bottle lost its neck), the size x size float32 result
 * back in the picture's orientation.  Empty when the service is not there
 * or fails.
 */
cv::Mat apuMap(const cv::Mat &pic, int upright, int size, const char *job, const char *model)
{
	if (pic.type() != CV_8UC3)
		return {};
	int s = connectApu();
	if (s < 0)
		return {};
	const size_t inSize = size_t(size) * size * 3, outSize = size_t(size) * size * 4;
	int in = memfd_create("apu-map-in", MFD_CLOEXEC);
	int out = memfd_create("apu-map-out", MFD_CLOEXEC);
	cv::Mat map;
	if (in >= 0 && out >= 0 && !ftruncate(in, inSize) && !ftruncate(out, outSize)) {
		void *pi = mmap(nullptr, inSize, PROT_READ | PROT_WRITE, MAP_SHARED, in, 0);
		if (pi != MAP_FAILED) {
			cv::Mat small;
			cv::resize(pic, small, cv::Size(size, size), 0, 0, cv::INTER_AREA);
			rotated(small, upright).copyTo(cv::Mat(size, size, CV_8UC3, pi));
			munmap(pi, inSize);
			char req[160];
			int n = std::snprintf(req, sizeof(req),
					      "{\"job\": \"%s\", \"width\": %d, \"height\": %d, \"order\": \"bgr\"%s%s%s}",
					      job, size, size, model ? ", \"model\": \"" : "", model ? model : "",
					      model ? "\"" : "");
			if (sendFds(s, req, n, in, out) && upscaleReplies(s)) {
				void *po = mmap(nullptr, outSize, PROT_READ, MAP_SHARED, out, 0);
				if (po != MAP_FAILED) {
					map = rotated(cv::Mat(size, size, CV_32F, po), inverseRotation(upright));
					munmap(po, outSize);
				}
			}
		}
	}
	for (int fd : { in, out, s })
		if (fd >= 0)
			close(fd);
	return map;
}

/*
 * The picture's subject mask (U^2-Net on an MDLA core, ~3 s; `people`: its
 * human segmentation model, else the salient object), float 0..1 at
 * kSegSize x kSegSize.
 */
cv::Mat segmentApu(const cv::Mat &pic, int upright, bool people)
{
	return apuMap(pic, upright, kSegSize, "segment", people ? "people" : "objects");
}

/*
 * The picture's relative depth (MiDaS small on an MDLA core, ~1 s): float
 * at kDepthSize x kDepthSize, larger = nearer, arbitrary scale and offset
 * (inverse depth: like the stereo pair's disparity).
 */
cv::Mat depthApu(const cv::Mat &pic, int upright)
{
	return apuMap(pic, upright, kDepthSize, "depth", nullptr);
}

/* the picture `factor` times larger; empty on failure */
cv::Mat upscale(const cv::Mat &pic, int factor, const std::string &workDir)
{
	cv::Mat apu = upscaleApu(pic);
	if (!apu.empty()) {
		if (factor == 4)
			return apu;
		cv::Mat res;
		cv::resize(apu, res, cv::Size(pic.cols * factor, pic.rows * factor), 0, 0,
			   cv::INTER_LANCZOS4);
		munmap(apu.data, apu.total() * 3);
		return res;
	}
	std::fprintf(stderr, "cezanne-photo: no APU upscaler, %s on the GPU\n", kUpscaler);
	std::string in = workDir + "/upscale-" + std::to_string(getpid()) + "-in.png";
	std::string out = workDir + "/upscale-" + std::to_string(getpid()) + "-out.png";
	if (!cv::imwrite(in, pic, { cv::IMWRITE_PNG_COMPRESSION, 1 }))
		return {};
	pid_t child = fork();
	if (child == 0) {
		execlp(kUpscaler, kUpscaler, "-i", in.c_str(), "-o", out.c_str(),
		       "-m", kUpscaleModels, "-n", kUpscaleModel, "-s", "4", "-f", "png",
		       static_cast<char *>(nullptr));
		_exit(127);
	}
	int status = -1;
	if (child > 0)
		waitpid(child, &status, 0);
	std::remove(in.c_str());
	cv::Mat big;
	if (status == 0)
		big = cv::imread(out, cv::IMREAD_COLOR);
	std::remove(out.c_str());
	if (big.empty()) {
		std::fprintf(stderr, "cezanne-photo: %s failed (%d)\n", kUpscaler, status);
		return {};
	}
	if (factor == 4)
		return big;
	cv::Mat res;
	cv::resize(big, res, cv::Size(pic.cols * factor, pic.rows * factor), 0, 0,
		   cv::INTER_LANCZOS4);
	return res;
}

/* ---- portrait ---- */

/*
 * Focal lengths in sensor pixels from Xiaomi's golden main + mono
 * calibration (/vendor/etc/camera/com.xiaomi.dcal.wt.golden, doubles XOR
 * 0xaa), only to bring both views to the same scale: the geometry itself is
 * fitted on every picture (uncalibrated rectification; the golden
 * intrinsics left ~1 px of row error on the processed pictures).
 */
constexpr double kMainF = 3466.4, kMainWidth = 4624;
constexpr double kMonoF = 1124.2;
constexpr int kMinPortraitMatches = 60;


/* $CEZANNE_PHOTO_DEBUG=<dir>: intermediate maps as PNG there */
void debugMap(const char *name, const cv::Mat &m, double scale = 255)
{
	const char *dir = std::getenv("CEZANNE_PHOTO_DEBUG");
	if (!dir)
		return;
	cv::Mat u8;
	m.convertTo(u8, CV_8U, scale);
	cv::imwrite(std::string(dir) + "/" + name + ".png", u8);
}

/* guided filter (He et al.), single-channel guide */
cv::Mat guided(const cv::Mat &I, const cv::Mat &p, int r, double eps)
{
	auto box = [r](const cv::Mat &x) {
		cv::Mat y;
		cv::boxFilter(x, y, -1, cv::Size(2 * r + 1, 2 * r + 1));
		return y;
	};
	cv::Mat mI = box(I), mp = box(p);
	cv::Mat a = (box(I.mul(p)) - mI.mul(mp)) / (box(I.mul(I)) - mI.mul(mI) + eps);
	cv::Mat b = mp - a.mul(mI);
	return box(a).mul(I) + box(b);
}

struct Matches {
	std::vector<cv::Point2f> a, b;		/* main (scaled), mono */
};

Matches siftMatches(const cv::Mat &a8, const cv::Mat &b8)
{
	auto sift = cv::SIFT::create(6000);
	std::vector<cv::KeyPoint> ka, kb;
	cv::Mat da, db;
	sift->detectAndCompute(a8, cv::noArray(), ka, da);
	sift->detectAndCompute(b8, cv::noArray(), kb, db);
	Matches m;
	if (da.empty() || db.empty())
		return m;
	cv::BFMatcher matcher(cv::NORM_L2);
	std::vector<std::vector<cv::DMatch>> knn;
	matcher.knnMatch(da, db, knn, 2);
	for (const auto &k : knn) {
		if (k.size() == 2 && k[0].distance < 0.75f * k[1].distance) {
			m.a.push_back(ka[k[0].queryIdx].pt);
			m.b.push_back(kb[k[0].trainIdx].pt);
		}
	}
	return m;
}

/*
 * The subject: the regions of a mask that the focus seed (sharp pixels at
 * the focus disparity, centre of the frame) lies on, notches closed, holes
 * (not reaching the border) filled.  Stray regions elsewhere (wrong matches
 * at the frame edges, repetitive blinds) are dropped.
 */
cv::Mat subjectShape(const cv::Mat &bw8, const cv::Mat &seed8)
{
	cv::Mat lab, st, cen;
	int n = cv::connectedComponentsWithStats(bw8, lab, st, cen, 8);
	std::vector<int> hits(n, 0);
	for (int y = 0; y < lab.rows; y++)
		for (int x = 0; x < lab.cols; x++)
			if (seed8.at<uchar>(y, x))
				hits[lab.at<int>(y, x)]++;
	int seedTotal = cv::countNonZero(seed8);
	cv::Mat blob = cv::Mat::zeros(bw8.size(), CV_8U);
	for (int i = 1; i < n; i++)
		if (st.at<int>(i, cv::CC_STAT_AREA) > 0.01 * bw8.total() &&
		    hits[i] > 0.1 * seedTotal)
			blob.setTo(1, lab == i);
	int kc = int(0.10 * std::min(blob.rows, blob.cols)) | 1;
	cv::morphologyEx(blob, blob, cv::MORPH_CLOSE,
			 cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(kc, kc)));
	cv::Mat inv = blob == 0, lab2;
	int n2 = cv::connectedComponents(inv, lab2, 4);
	std::vector<bool> border(n2, false);
	for (int x = 0; x < lab2.cols; x++)
		border[lab2.at<int>(0, x)] = border[lab2.at<int>(lab2.rows - 1, x)] = true;
	for (int y = 0; y < lab2.rows; y++)
		border[lab2.at<int>(y, 0)] = border[lab2.at<int>(y, lab2.cols - 1)] = true;
	for (int y = 0; y < lab2.rows; y++)
		for (int x = 0; x < lab2.cols; x++)
			if (inv.at<uchar>(y, x) && !border[lab2.at<int>(y, x)])
				blob.at<uchar>(y, x) = 1;
	return blob;
}

} /* namespace */

/*
 * Portrait: the background blurred by depth, from the main + mono stereo
 * pair (the mono frame <prefix>-1.pgm taken with the picture).  Empty if
 * the frames do not match.
 *
 * 1-2. The disparity in the main frame (stereoPair).
 * 3. Focus disparity: the histogram of the central disparities weighted by
 *    the picture's sharpness (the lens focused on the subject).
 * 4. Matte from three cues: stereo (near the focus disparity / clearly not),
 *    spread edge-aware by confidence weighted guided filters; the picture's
 *    own focus (fine over coarse detail energy: high only at sharp edges,
 *    whatever their contrast), which decides where stereo is blind or fooled
 *    (glossy surfaces show the reflected room's depth); GrabCut seeded with
 *    the confident labels for what neither sees.  Then the large regions,
 *    notches closed, holes filled, edges refined on the full picture.
 * 5. Blur: disc kernels at a quarter of the resolution, the radius growing
 *    with the disparity difference (the circle of confusion of a real lens
 *    is proportional to it), from the background pixels only (the subject
 *    does not bleed into its surroundings), the subject kept sharp.
 */
/*
 * The main + mono stereo pair (shared by portrait and low light):
 *
 * 1. The picture turned to the sensors' orientation (the quarter turn whose
 *    SIFT matches to the mono frame agree on no rotation) at the mono focal
 *    length, matched in linear light like the mono frame.
 * 2. Uncalibrated rectification (fundamental matrix of the matches,
 *    Hartley's homographies) keeping the whole main frame, the disparity
 *    search range from the matches, SGBM, and the disparity back in the
 *    main frame.
 */
struct StereoPair {
	Frame mono;		/* <prefix>-1.pgm */
	cv::Mat gray;		/* the picture's luma (8-bit, as it is) */
	int rot;		/* turn of the picture to the sensors' orientation */
	cv::Mat mainS;		/* gray, so turned, at the mono focal length */
	cv::Size size;		/* of mainS */
	cv::Mat H1, H2;		/* rectification: mainS, mono -> rectified */
	int sgn;		/* rectified mono x = main x - sgn * disparity */
	int mind, nd;		/* disparity search range */
	cv::Mat dispM;		/* in mainS: disparity - mind + 1, 0: none */
	cv::Mat validM;
};

bool stereoPair(const cv::Mat &pic, const std::string &prefix, const char *what, StereoPair &sp)
{
	Frame &mono = sp.mono;
	if (!readPgm(prefix + "-1.pgm", mono)) {
		std::fprintf(stderr, "cezanne-photo: %s: no mono frame\n", what);
		return false;
	}
	cv::Mat mono8;
	{
		/* to 8 bits by the 99.5th percentile: hot pixels at high gain */
		cv::Mat f = cv::max(mono.img - mono.black, 0);
		std::vector<float> v;
		for (int y = 0; y < f.rows; y += 4)
			for (int x = 0; x < f.cols; x += 4)
				v.push_back(f.at<float>(y, x));
		std::nth_element(v.begin(), v.begin() + v.size() * 995 / 1000, v.end());
		f.convertTo(mono8, CV_8U, 255.0 / std::max(v[v.size() * 995 / 1000], 1.0f));
	}
	/*
	 * matched slightly smoothed: in low light the noise made most features
	 * (1.5 px: 45 % more fundamental matrix inliers in a dark room)
	 */
	auto clahe = cv::createCLAHE(2.0, cv::Size(8, 8));
	cv::Mat monoEq;
	cv::GaussianBlur(mono8, monoEq, cv::Size(0, 0), 1.5);
	clahe->apply(monoEq, monoEq);

	/* 1. orientation and scale: the main picture at the mono focal length */
	cv::Mat &gray = sp.gray, grayLin;
	cv::cvtColor(pic, gray, cv::COLOR_BGR2GRAY);
	/*
	 * matched in linear light like the mono frame: SGBM's cost does not
	 * survive the picture's gamma (sparse, noisy disparities)
	 */
	{
		cv::Mat lin = toLinear(gray);
		std::vector<float> v;
		for (int y = 0; y < lin.rows; y += 8)
			for (int x = 0; x < lin.cols; x += 8)
				v.push_back(lin.at<float>(y, x));
		std::nth_element(v.begin(), v.begin() + v.size() * 995 / 1000, v.end());
		lin.convertTo(grayLin, CV_8U, 255.0 / std::max(v[v.size() * 995 / 1000], 1e-3f));
	}
	bool landscape = mono8.cols >= mono8.rows;
	int &rot = sp.rot;
	rot = -2;
	Matches best;
	cv::Mat &mainS = sp.mainS, mainEq;
	for (int code : { -1, int(cv::ROTATE_90_CLOCKWISE), int(cv::ROTATE_180),
			  int(cv::ROTATE_90_COUNTERCLOCKWISE) }) {
		cv::Mat g = rotated(grayLin, code);
		if ((g.cols >= g.rows) != landscape)
			continue;
		double sc = kMonoF / kMainF * (kMainWidth / g.cols);	/* picture may be cropped */
		cv::Mat gs, eq;
		cv::resize(g, eq, cv::Size(), sc, sc, cv::INTER_AREA);
		cv::GaussianBlur(eq, eq, cv::Size(0, 0), 1.5);
		clahe->apply(eq, eq);
		/* the gamma encoded picture at that scale: guide and sharpness */
		cv::resize(rotated(gray, code), gs, eq.size(), 0, 0, cv::INTER_AREA);
		Matches m = siftMatches(eq, monoEq);
		/*
		 * SIFT matches either way round: the right turn is the one whose
		 * matches agree on a similarity transform without rotation
		 */
		std::vector<uchar> inl;
		cv::Mat M;
		if (m.a.size() >= 3)
			M = cv::estimateAffinePartial2D(m.a, m.b, inl, cv::RANSAC, 8.0);
		double angle = M.empty() ? 180.0
			: std::atan2(M.at<double>(1, 0), M.at<double>(0, 0)) * 180 / CV_PI;
		size_t n = std::count(inl.begin(), inl.end(), 1);
		std::fprintf(stderr, "cezanne-photo: %s rotation %d: %zu matches, %zu agree, %.1f deg\n",
			     what, code, m.a.size(), n, angle);
		if (std::abs(angle) > 20.0)
			continue;
		if (m.a.size() > best.a.size()) {
			best = m;
			rot = code;
			mainS = gs;
			mainEq = eq;
		}
	}
	if (best.a.size() < size_t(kMinPortraitMatches)) {
		std::fprintf(stderr, "cezanne-photo: %s: frames do not match\n", what);
		return false;
	}
	/* 2. rectification from the fundamental matrix, disparity */
	std::vector<uchar> finl;
	cv::Mat F = cv::findFundamentalMat(best.a, best.b, cv::FM_RANSAC, 1.5, 0.999, 2000, finl);
	std::vector<cv::Point2f> fa, fb;
	for (size_t i = 0; i < finl.size(); i++)
		if (finl[i]) {
			fa.push_back(best.a[i]);
			fb.push_back(best.b[i]);
		}
	cv::Size &size = sp.size;
	size = mainS.size();
	cv::Mat &H1 = sp.H1, &H2 = sp.H2;
	bool rectified = F.rows == 3 && fa.size() >= size_t(kMinPortraitMatches) &&
			 cv::stereoRectifyUncalibrated(fa, fb, F, size, H1, H2, 1.5);
	std::fprintf(stderr, "cezanne-photo: %s: %zu matches, %zu fit the geometry\n",
		     what, best.a.size(), fa.size());
	if (!rectified) {
		std::fprintf(stderr, "cezanne-photo: %s: no rectification\n", what);
		return false;
	}
	/* the whole main frame in view: shift both by the same amount */
	std::vector<cv::Point2f> corners = { { 0, 0 }, { float(size.width), 0 },
		{ float(size.width), float(size.height) }, { 0, float(size.height) } }, wc;
	cv::perspectiveTransform(corners, wc, H1);
	cv::Rect2f bb = cv::boundingRect(wc);
	if (bb.width > 2.5 * size.width || bb.height > 2.5 * size.height) {
		std::fprintf(stderr, "cezanne-photo: %s: rectification too skewed\n", what);
		return false;
	}
	cv::Matx33d T(1, 0, -bb.x, 0, 1, -bb.y, 0, 0, 1);
	H1 = cv::Mat(T) * H1;
	H2 = cv::Mat(T) * H2;
	cv::Size rsize(int(std::ceil(bb.width)), int(std::ceil(bb.height)));
	cv::Mat ra, rb;
	cv::warpPerspective(mainEq, ra, H1, rsize, cv::INTER_LINEAR);
	cv::warpPerspective(monoEq, rb, H2, rsize, cv::INTER_LINEAR);

	/* search direction and range from the matches (main - mono) */
	std::vector<cv::Point2f> qa, qb;
	cv::perspectiveTransform(fa, qa, H1);
	cv::perspectiveTransform(fb, qb, H2);
	std::vector<float> dx, dyAbs;
	for (size_t i = 0; i < qa.size(); i++) {
		dyAbs.push_back(std::abs(qa[i].y - qb[i].y));
		if (dyAbs.back() < 2.0f)
			dx.push_back(qa[i].x - qb[i].x);
	}
	std::nth_element(dyAbs.begin(), dyAbs.begin() + dyAbs.size() / 2, dyAbs.end());
	if (dx.size() < size_t(kMinPortraitMatches)) {
		std::fprintf(stderr, "cezanne-photo: %s: rectification failed\n", what);
		return false;
	}
	std::sort(dx.begin(), dx.end());
	/*
	 * In the sensors' orientation the main camera is the right-hand view
	 * (its picture of a near object lies left of the mono's): match from
	 * the mono side, unless the rectification mirrored x.  (Hartley's
	 * homographies shift x arbitrarily: the matches' sign tells nothing.)
	 */
	cv::Point2f c0(size.width / 2.0f, size.height / 2.0f), c1 = c0 + cv::Point2f(1, 0);
	std::vector<cv::Point2f> cc = { c0, c1 }, cw;
	cv::perspectiveTransform(cc, cw, H1);
	int &sgn = sp.sgn;
	sgn = cw[1].x > cw[0].x ? -1 : 1;
	float lo = sgn < 0 ? -dx[dx.size() - 1 - dx.size() / 100] : dx[dx.size() / 100];
	float hi = sgn < 0 ? -dx[dx.size() / 100] : dx[dx.size() - 1 - dx.size() / 100];
	std::fprintf(stderr, "cezanne-photo: %s rows %.2f px, disparities %.1f .. %.1f (%zu matches)\n",
		     what, dyAbs[dyAbs.size() / 2], lo, hi, dx.size());
	int &mind = sp.mind, &nd = sp.nd;
	mind = int(std::floor(lo)) - 16;
	nd = int(std::ceil((hi - mind + 16) / 16.0)) * 16;
	auto sgbm = cv::StereoSGBM::create(mind, nd, 5, 8 * 25, 48 * 25, 1, 0, 10, 200, 2,
					   cv::StereoSGBM::MODE_SGBM_3WAY);
	cv::Mat disp16, disp;
	if (sgn < 0) {
		cv::Mat fa8, fb8;
		cv::flip(ra, fa8, 1);
		cv::flip(rb, fb8, 1);
		sgbm->compute(fa8, fb8, disp16);
		cv::flip(disp16, disp16, 1);
	} else {
		sgbm->compute(ra, rb, disp16);
	}
	disp16.convertTo(disp, CV_32F, 1.0 / 16);
	/* relative to the far end of the range, 0: no match */
	cv::Mat validR = disp >= mind;
	disp = disp - (mind - 1);
	disp.setTo(0, ~validR);

	/* back to the (scaled) main frame */
	cv::Mat &dispM = sp.dispM;
	cv::warpPerspective(disp, dispM, H1, size, cv::INTER_NEAREST | cv::WARP_INVERSE_MAP);
	sp.validM = dispM > 0;
	debugMap("disp", dispM, 255.0 / nd);
	debugMap("rect", ra, 1);
	return true;
}

/*
 * 5. The background blurred, layered disc blur at a quarter of the
 * resolution: A the subject (0..1), C the blur strength (0..1: up to a disc
 * of 1.8 % of the picture width), both at the picture's size.  Sharp where
 * the subject is or the blur is small.
 */
cv::Mat blurBackground(const cv::Mat &pic, const cv::Mat &A, const cv::Mat &C)
{
	const int q = 4;
	cv::Size qs(pic.cols / q, pic.rows / q);
	cv::Mat small, As, Cs;
	cv::resize(pic, small, qs, 0, 0, cv::INTER_AREA);
	small.convertTo(small, CV_32FC3);
	cv::resize(A, As, qs, 0, 0, cv::INTER_AREA);
	cv::resize(C, Cs, qs, 0, 0, cv::INTER_AREA);
	cv::Mat wB = 1 - As;
	const double rmax = pic.cols * 0.018 / q;
	const float levels[] = { 0, 0.15f, 0.3f, 0.5f, 0.75f, 1.0f };
	const int nl = 6;
	cv::Mat acc = cv::Mat::zeros(qs, CV_32FC3), wsum = cv::Mat::zeros(qs, CV_32F);
	for (int i = 0; i < nl; i++) {
		cv::Mat layer;
		double rad = levels[i] * rmax;
		if (rad < 0.5) {
			layer = small;
		} else {
			int k = int(std::ceil(rad));
			cv::Mat disc(2 * k + 1, 2 * k + 1, CV_32F);
			for (int y = -k; y <= k; y++)
				for (int x = -k; x <= k; x++)
					disc.at<float>(y + k, x + k) = std::hypot(x, y) <= rad + 0.5 ? 1.f : 0.f;
			disc /= cv::sum(disc)[0];
			cv::Mat wb3, num, den;
			cv::cvtColor(wB, wb3, cv::COLOR_GRAY2BGR);
			cv::filter2D(small.mul(wb3), num, -1, disc);
			cv::filter2D(wB, den, -1, disc);
			cv::cvtColor(cv::max(den, 1e-3), den, cv::COLOR_GRAY2BGR);
			layer = num / den;
		}
		/* hat weight around this level */
		float l0 = i ? levels[i - 1] : levels[i] - 1, l1 = i + 1 < nl ? levels[i + 1] : levels[i] + 1;
		cv::Mat w(qs, CV_32F);
		for (int y = 0; y < qs.height; y++)
			for (int x = 0; x < qs.width; x++) {
				float c = Cs.at<float>(y, x);
				float v = c <= levels[i] ? (c - l0) / (levels[i] - l0) : (l1 - c) / (l1 - levels[i]);
				w.at<float>(y, x) = std::clamp(v, 0.f, 1.f);
			}
		cv::Mat w3;
		cv::cvtColor(w, w3, cv::COLOR_GRAY2BGR);
		acc += layer.mul(w3);
		wsum += w;
	}
	cv::Mat ws3;
	cv::cvtColor(cv::max(wsum, 1e-3), ws3, cv::COLOR_GRAY2BGR);
	cv::Mat blurred = acc / ws3;
	cv::resize(blurred, blurred, pic.size(), 0, 0, cv::INTER_LINEAR);

	/* sharp where the subject is or the blur is small */
	cv::Mat keep = cv::min(A + (1 - cv::min(C * 4, 1)), 1), keep3, picF, out;
	cv::cvtColor(keep, keep3, cv::COLOR_GRAY2BGR);
	pic.convertTo(picF, CV_32FC3);
	out = keep3.mul(picF) + (cv::Scalar::all(1) - keep3).mul(blurred);
	out.convertTo(out, CV_8UC3);
	return out;
}

cv::Mat portrait(const cv::Mat &pic, const std::string &prefix)
{
	StereoPair sp;
	if (!stereoPair(pic, prefix, "portrait", sp))
		return {};
	const cv::Mat &gray = sp.gray, &mainS = sp.mainS, &dispM = sp.dispM, &validM = sp.validM;
	const cv::Size size = sp.size;
	const int rot = sp.rot, nd = sp.nd;


	/* 3. focus disparity */
	cv::Mat lap, sharp;
	{
		cv::Mat gf;
		cv::GaussianBlur(mainS, gf, cv::Size(0, 0), 1.0);
		gf.convertTo(gf, CV_32F);
		cv::Laplacian(gf, lap, CV_32F);
		cv::GaussianBlur(lap.mul(lap), sharp, cv::Size(0, 0), 6);
	}
	std::vector<double> hist(nd + 2, 0.0);
	for (int y = size.height / 5; y < size.height * 4 / 5; y++)
		for (int x = size.width / 5; x < size.width * 4 / 5; x++) {
			float d = dispM.at<float>(y, x);
			if (d > 0 && d < nd + 2)
				hist[int(d)] += sharp.at<float>(y, x);
		}
	int peak = 1;
	double peakV = -1;
	for (int i = 1; i + 1 < int(hist.size()); i++) {
		double v = hist[i - 1] + hist[i] + hist[i + 1];
		if (v > peakV) {
			peakV = v;
			peak = i;
		}
	}
	/*
	 * disparities carry an arbitrary offset (Hartley's rectification): the
	 * depth step from the far background (10th percentile) to the focus is
	 * the scale for the subject's tolerance and the blur; background from
	 * a fifth of it on (things just behind the subject too)
	 */
	std::vector<float> dv;
	for (int y = 0; y < dispM.rows; y += 4)
		for (int x = 0; x < dispM.cols; x += 4)
			if (dispM.at<float>(y, x) > 0)
				dv.push_back(dispM.at<float>(y, x));
	std::nth_element(dv.begin(), dv.begin() + dv.size() / 10, dv.end());
	float df = peak + 0.5f, dfar = dv.empty() ? 0.f : dv[dv.size() / 10];
	float depthStep = std::max(df - dfar, 8.0f);
	float tol = std::max(2.5f, 0.1f * depthStep);
	std::fprintf(stderr, "cezanne-photo: portrait focus disparity %.1f, far %.1f, tolerance %.1f\n",
		     df, dfar, tol);

	/* 4. matte */
	cv::Mat I;
	mainS.convertTo(I, CV_32F, 1.0 / 255);
	cv::Mat diff = cv::abs(dispM - df);
	cv::Mat subj, back;
	cv::Mat((diff < tol) & validM).convertTo(subj, CV_32F, 1.0 / 255);
	cv::Mat((diff > 2.0f * tol) & validM).convertTo(back, CV_32F, 1.0 / 255);
	cv::Mat alpha;
	for (auto [r, eps] : { std::pair{ 40, 1e-2 }, std::pair{ 12, 1e-3 }, std::pair{ 4, 1e-4 } }) {
		/*
		 * (guided filters overshoot below 0 on flat areas: a negative
		 * background sum made the ratio blow up there)
		 */
		cv::Mat ns = cv::max(guided(I, subj, r, eps), 0), nbk = cv::max(guided(I, back, r, eps), 0);
		/*
		 * little stereo data around (outside the mono view, a few stray
		 * matches on repetitive blinds): not subject; full confidence
		 * from a quarter of the pixels labelled
		 */
		cv::Mat est = ns / cv::max(ns + nbk, 0.25);
		est = cv::min(cv::max(est, 0), 1);
		cv::Mat conf = cv::min((ns + nbk) * 3, 1);
		alpha = alpha.empty() ? est : conf.mul(est) + (1 - conf).mul(alpha);
	}
	alpha = cv::min(cv::max((alpha - 0.3) / 0.4, 0), 1);
	debugMap("alpha-stereo", alpha);

	/* the picture's own focus, half sensor resolution, square root domain */
	cv::Mat focus, strong, backConf;
	{
		cv::Mat g = rotated(gray, rot), half;
		double h2 = 0.5 * kMainWidth / g.cols;		/* half sensor resolution */
		cv::resize(g, half, cv::Size(), h2, h2, cv::INTER_AREA);
		cv::Mat mf;
		half.convertTo(mf, CV_32F);
		/* the soft ISP's gamma: about the square root of linear already */
		cv::Mat gs[4];
		const double sig[4] = { 0.8, 1.6, 3.2, 6.4 };
		for (int k = 0; k < 4; k++)
			cv::GaussianBlur(mf, gs[k], cv::Size(0, 0), sig[k]);
		cv::Mat fineE, coarseE, d0 = gs[0] - gs[1], d1 = gs[2] - gs[3];
		cv::GaussianBlur(d0.mul(d0), fineE, cv::Size(0, 0), 10);
		cv::GaussianBlur(d1.mul(d1), coarseE, cv::Size(0, 0), 10);
		/* noise: median absolute fine detail */
		std::vector<float> ad;
		ad.reserve(d0.total() / 16);
		for (int y = 0; y < d0.rows; y += 4)
			for (int x = 0; x < d0.cols; x += 4)
				ad.push_back(std::abs(d0.at<float>(y, x)));
		std::nth_element(ad.begin(), ad.begin() + ad.size() / 2, ad.end());
		float med = ad[ad.size() / 2];
		float noise = med * med * 2 + 1e-3f;
		cv::Mat ratio = fineE / (coarseE + 4 * noise);
		cv::Mat texture;
		cv::Mat(coarseE > 6 * noise).convertTo(texture, CV_32F, 1.0 / 255);
		cv::resize(ratio, ratio, size, 0, 0, cv::INTER_AREA);
		cv::resize(texture, texture, size, 0, 0, cv::INTER_AREA);
		texture = texture > 0.5;
		texture.convertTo(texture, CV_32F, 1.0 / 255);
		std::vector<float> rs;
		for (int y = 0; y < size.height; y++)
			for (int x = 0; x < size.width; x++)
				if (alpha.at<float>(y, x) > 0.5f && texture.at<float>(y, x) > 0)
					rs.push_back(ratio.at<float>(y, x));
		float ref = 1;
		if (!rs.empty()) {
			std::nth_element(rs.begin(), rs.begin() + rs.size() * 6 / 10, rs.end());
			ref = rs[rs.size() * 6 / 10];
		}
		cv::Mat rel = ratio / std::max(ref, 1e-6f);
		focus = cv::min(cv::max((rel - 0.35) / 0.3, 0), 1).mul(texture);
		strong = cv::min(cv::max((rel - 0.6) / 0.3, 0), 1).mul(texture);
		backConf = cv::min(cv::max(guided(I, back, 16, 1e-3) * 4, 0), 1);
		cv::Mat focusW = focus.mul(1 - backConf);
		alpha = cv::max(alpha, cv::max(strong, focusW));
	}

	debugMap("alpha-cues", alpha);
	debugMap("strong", strong);
	debugMap("subj", subj);
	debugMap("back", back);
	debugMap("focus", focus);
	debugMap("backconf", backConf);
	/* GrabCut on the colour picture, seeded with the confident labels */
	cv::Mat picS;
	cv::resize(rotated(pic, rot), picS, size, 0, 0, cv::INTER_AREA);
	cv::Mat gc(size, CV_8U, cv::Scalar(cv::GC_PR_BGD));
	gc.setTo(cv::GC_PR_FGD, alpha > 0.4);
	cv::Mat sureFg, nearFg;
	cv::erode(alpha > 0.8, sureFg, cv::Mat::ones(9, 9, CV_8U));
	cv::dilate(alpha > 0.4, nearFg, cv::Mat::ones(31, 31, CV_8U));
	gc.setTo(cv::GC_FGD, sureFg);
	gc.setTo(cv::GC_BGD, (backConf > 0.8) & (focus < 0.05) & (nearFg == 0));
	debugMap("seeds", gc, 85);
	cv::Mat bgm, fgm;
	cv::grabCut(picS, gc, cv::Rect(), bgm, fgm, 5, cv::GC_INIT_WITH_MASK);
	cv::Mat fg = (gc == cv::GC_FGD) | (gc == cv::GC_PR_FGD);
	/*
	 * GrabCut fills holes and follows edges near the evidence, it does not
	 * claim new regions (its colour model took the white table along the
	 * frame for the cream label)
	 */
	{
		int kb = int(0.04 * std::min(size.width, size.height)) | 1;
		cv::Mat band;
		cv::dilate(alpha > 0.5, band, cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(kb, kb)));
		fg &= band;
	}
	debugMap("grabcut", fg, 1);
	/* the focus seed: sharp, at the focus disparity, in the centre */
	cv::Mat seed = cv::Mat::zeros(size, CV_8U);
	{
		cv::Mat sharpC = sharp(cv::Rect(size.width / 5, size.height / 5, size.width * 3 / 5, size.height * 3 / 5));
		std::vector<float> sv(sharpC.begin<float>(), sharpC.end<float>());
		std::nth_element(sv.begin(), sv.begin() + sv.size() * 3 / 4, sv.end());
		float sharpRef = sv[sv.size() * 3 / 4];
		for (int y = size.height / 5; y < size.height * 4 / 5; y++)
			for (int x = size.width / 5; x < size.width * 4 / 5; x++)
				if (std::abs(dispM.at<float>(y, x) - df) < tol && sharp.at<float>(y, x) >= sharpRef)
					seed.at<uchar>(y, x) = 1;
	}
	debugMap("seed", seed);
	cv::Mat blob = subjectShape(fg / 255, seed);
	cv::Mat blobF, soft;
	blob.convertTo(blobF, CV_32F);
	cv::GaussianBlur(blobF, soft, cv::Size(0, 0), 3);
	cv::GaussianBlur(blobF.mul(cv::max(soft, blobF)), alpha, cv::Size(0, 0), 1.5);
	if (cv::countNonZero(blob) < 0.01 * blob.total()) {
		std::fprintf(stderr, "cezanne-photo: portrait: no subject\n");
		return {};
	}

	debugMap("alpha", alpha);
	/* background blur strength from the filled disparity */
	cv::Mat validF;
	validM.convertTo(validF, CV_32F, 1.0 / 255);
	cv::Mat dfill = guided(I, dispM.mul(validF), 24, 1e-3) / cv::max(guided(I, validF, 24, 1e-3), 1e-3);
	cv::Mat coc = cv::min(cv::max(cv::abs(dfill - df) / (0.5 * depthStep), 0.35), 1.0);
	coc = coc.mul(1 - alpha);

	/* back to the picture's orientation and size; edges on the full picture */
	int back_ = inverseRotation(rot);
	cv::Mat A = rotated(alpha, back_), C = rotated(coc, back_);
	cv::resize(A, A, pic.size(), 0, 0, cv::INTER_LINEAR);
	cv::resize(C, C, pic.size(), 0, 0, cv::INTER_LINEAR);
	cv::Mat Ig;
	gray.convertTo(Ig, CV_32F, 1.0 / 255);
	A = cv::min(cv::max(guided(Ig, A, 12, 1e-3), 0), 1);
	A = cv::min(cv::max((A - 0.25) / 0.5, 0), 1);

	cv::Mat out = blurBackground(pic, A, C);
	std::fprintf(stderr, "cezanne-photo: portrait done\n");
	return out;
}

/*
 * Portrait from the APU's subject mask (cezanne-upscaled's segment
 * job, U^2-Net for the salient object or for `people`, on the picture
 * turned upright) instead of stereo depth: seconds instead of minutes, no
 * mono frame needed.  The 320 x 320 mask scaled up and snapped to the
 * picture's edges by guided filters at a quarter and at full resolution;
 * the background blurred by the depth job's map (MiDaS small: more blur
 * the further from the subject, in front or behind), at one strength
 * without it.  Empty without the service or a subject (under 1 % of the
 * picture): the stereo path then.
 */
cv::Mat portraitApu(const cv::Mat &pic, int upright, bool people)
{
	cv::Mat sal = segmentApu(pic, upright, people);
	if (sal.empty())
		return {};
	debugMap("apu-mask", sal);
	double area = cv::countNonZero(sal > 0.5) / double(sal.total());
	if (area < 0.01) {
		std::fprintf(stderr, "cezanne-photo: portrait: no %s on the APU\n",
			     people ? "people" : "salient subject");
		return {};
	}
	cv::Mat gray, Ig, gq, aq;
	cv::cvtColor(pic, gray, cv::COLOR_BGR2GRAY);
	gray.convertTo(Ig, CV_32F, 1.0 / 255);
	cv::Size qs(pic.cols / 4, pic.rows / 4);
	cv::resize(Ig, gq, qs, 0, 0, cv::INTER_AREA);
	cv::resize(sal, aq, qs, 0, 0, cv::INTER_LINEAR);

	/*
	 * Depth (the blur strength, as the stereo path's portrait()): the depth
	 * map normalised and snapped to the picture's edges at a quarter of the
	 * resolution, the focus the subject's median depth, the step from the
	 * far background (10th percentile) to it the scale.
	 */
	cv::Mat depth = depthApu(pic, upright), dq;
	float df = 1.f, dfar = 0.f, step = 1.f;
	if (!depth.empty()) {
		double mn, mx;
		cv::minMaxLoc(depth, &mn, &mx);
		cv::resize((depth - mn) / std::max(mx - mn, 1e-6), dq, qs, 0, 0, cv::INTER_LINEAR);
		dq = guided(gq, dq, 8, 1e-3);
		std::vector<float> fv, av;
		for (int y = 0; y < qs.height; y += 2)
			for (int x = 0; x < qs.width; x += 2) {
				av.push_back(dq.at<float>(y, x));
				if (aq.at<float>(y, x) > 0.5f)
					fv.push_back(dq.at<float>(y, x));
			}
		std::nth_element(av.begin(), av.begin() + av.size() / 10, av.end());
		std::nth_element(fv.begin(), fv.begin() + fv.size() / 2, fv.end());
		df = fv.empty() ? 1.f : fv[fv.size() / 2];
		dfar = av[av.size() / 10];
		step = std::max(df - dfar, 0.1f);
		debugMap("apu-depth", dq);
		/*
		 * A subject is in front of most of the picture; a mask behind the
		 * median depth took something in the background (on a sideways
		 * picture: the window blinds beside a figure, which then stayed
		 * sharp while the figure was blurred)
		 */
		std::nth_element(av.begin(), av.begin() + av.size() / 2, av.end());
		float dmed = av[av.size() / 2];
		if (df < dmed - 0.05f) {
			std::fprintf(stderr, "cezanne-photo: portrait: the mask lies behind most of the picture "
				     "(depth %.2f, median %.2f): not used\n", df, dmed);
			return {};
		}
		/*
		 * The mask without what lies well behind the subject: the salient
		 * object network also takes background things that touch the
		 * subject in the picture (a monitor stand behind a figure's head,
		 * keys beside it), and they stayed sharp in the blur.  Kept: from
		 * about halfway in depth between the far background and the
		 * subject, with a soft ramp.
		 */
		cv::Mat gate = cv::min(cv::max((dq - (dfar + 0.35f * step)) / (0.3f * step), 0), 1);
		aq = aq.mul(gate);
	}
	aq = cv::min(cv::max(guided(gq, aq, 8, 1e-3), 0), 1);
	area = cv::countNonZero(aq > 0.5f) / double(aq.total());
	if (area < 0.01) {
		std::fprintf(stderr, "cezanne-photo: portrait: no %s in front of the background\n",
			     people ? "people" : "subject");
		return {};
	}
	cv::Mat A, C;
	cv::resize(aq, A, pic.size(), 0, 0, cv::INTER_LINEAR);
	A = cv::min(cv::max(guided(Ig, A, 12, 1e-3), 0), 1);
	A = cv::min(cv::max((A - 0.25) / 0.5, 0), 1);
	debugMap("apu-alpha", A);

	/* blur strength: at least 0.35 outside the subject; without depth one
	 * strength */
	if (depth.empty()) {
		C = 1 - A;
	} else {
		cv::Mat coc = cv::min(cv::max(cv::abs(dq - df) / (0.5 * step), 0.35), 1.0);
		/*
		 * Smoothed over about the blur radius: snapped to the picture's
		 * edges it changed in a step, and a nearer background object came
		 * out as a less blurred cut-out with a sharp outline
		 */
		cv::GaussianBlur(coc, coc, cv::Size(0, 0), pic.cols * 0.018 / 4 / 2);
		cv::resize(coc, C, pic.size(), 0, 0, cv::INTER_LINEAR);
		C = C.mul(1 - A);
		debugMap("apu-coc", C);
		std::fprintf(stderr, "cezanne-photo: portrait depth: focus %.2f, far %.2f\n", df, dfar);
	}
	cv::Mat out = blurBackground(pic, A, C);
	std::fprintf(stderr, "cezanne-photo: portrait done (APU mask%s, %.0f %% subject)\n",
		     depth.empty() ? "" : " and depth", area * 100);
	return out;
}

/*
 * Low light: noise reduction guided by the mono frame (<prefix>-1.pgm) taken
 * with the picture; the mono sensor has no colour filter, about twice the
 * light per pixel, and from cezanne-mono 10 bits.  Empty if the frames do
 * not match (the caller then uses denoise()).
 *
 * 1. The mono frame warped onto the picture pixel by pixel: the disparity of
 *    the stereo pair (stereoPair), filled where it found no match (edge-aware
 *    by the main view, the rest from the surroundings), each main pixel's
 *    rectified position moved by it into the mono frame.  (A homography
 *    would leave the parallax of near things: ghost edges.)
 * 2. Luma: a guided filter of the picture's luma with the warped mono frame
 *    (at the luma's level, gamma encoded) as the guide: the mono's clean
 *    structure, the picture's local brightness and contrast (which also
 *    absorbs the sensors' different colour response); part of the picture's
 *    own fine detail added back.
 * 3. Chroma: guided filters with the mono frame at a quarter of the
 *    resolution (where the blotchy colour noise lives).
 * 4. Where the two disagree (band-pass correlation low: occlusions, a
 *    wrong disparity, outside the mono view) the picture's own noise
 *    reduction (denoise() at `level`) is used instead.
 */
cv::Mat lowLight(const cv::Mat &pic, const std::string &prefix, int level)
{
	StereoPair sp;
	if (!stereoPair(pic, prefix, "low light", sp))
		return {};
	const cv::Size size = sp.size;

	/* 1. disparity in the main frame (raw units), filled */
	cv::Mat I, vF, dRaw;
	sp.mainS.convertTo(I, CV_32F, 1.0 / 255);
	sp.validM.convertTo(vF, CV_32F, 1.0 / 255);
	dRaw = sp.dispM + float(sp.mind - 1);		/* where valid */
	cv::Mat num = guided(I, dRaw.mul(vF), 16, 1e-3), den = cv::max(guided(I, vF, 16, 1e-3), 0);
	cv::Mat dF = num / cv::max(den, 1e-3), hole = den <= 0.05;
	if (cv::countNonZero(hole)) {
		/* the rest: normalised convolution at 1/16, growing into the holes */
		cv::Mat w = ~hole, wf, small, wsm;
		w.convertTo(wf, CV_32F, 1.0 / 255);
		cv::Size ss(std::max(size.width / 16, 1), std::max(size.height / 16, 1));
		cv::resize(dF.mul(wf), small, ss, 0, 0, cv::INTER_AREA);
		cv::resize(wf, wsm, ss, 0, 0, cv::INTER_AREA);
		for (int it = 0; it < 30; it++) {
			cv::Mat bs, bw;
			cv::blur(small, bs, cv::Size(5, 5));
			cv::blur(wsm, bw, cv::Size(5, 5));
			cv::Mat known = wsm > 0.1;
			cv::Mat est = bs / cv::max(bw, 1e-3);
			cv::Mat val = small / cv::max(wsm, 1e-3);
			val.copyTo(est, known);
			cv::Mat grown = (bw > 0.01) | known;
			grown.convertTo(wsm, CV_32F, 1.0 / 255);
			small = est.mul(wsm);
		}
		cv::Mat big;
		cv::resize(small, big, size, 0, 0, cv::INTER_LINEAR);
		big.copyTo(dF, hole);
	}

	/* mono pixel of each main pixel: rectified x moved by the disparity */
	cv::Mat mapX(size, CV_32F), mapY(size, CV_32F);
	cv::Matx33d A(sp.H1), Binv(cv::Mat(sp.H2.inv()));
	for (int y = 0; y < size.height; y++)
		for (int x = 0; x < size.width; x++) {
			cv::Vec3d q = A * cv::Vec3d(x, y, 1);
			double qx = q[0] / q[2] - sp.sgn * dF.at<float>(y, x), qy = q[1] / q[2];
			cv::Vec3d m = Binv * cv::Vec3d(qx, qy, 1);
			mapX.at<float>(y, x) = float(m[0] / m[2]);
			mapY.at<float>(y, x) = float(m[1] / m[2]);
		}
	const Frame &mono = sp.mono;
	cv::Mat monoLin = cv::max(mono.img - mono.black, 0) / (mono.white - mono.black);
	cv::Mat monoW, cover;
	cv::remap(monoLin, monoW, mapX, mapY, cv::INTER_CUBIC, cv::BORDER_CONSTANT, -1);
	cover = monoW >= 0;
	monoW = cv::max(monoW, 0);

	/* back to the picture's orientation and size */
	int back = inverseRotation(sp.rot);
	cv::Mat MW, CV;
	cv::resize(rotated(monoW, back), MW, pic.size(), 0, 0, cv::INTER_CUBIC);
	cv::Mat coverF;
	cover.convertTo(coverF, CV_32F, 1.0 / 255);
	cv::resize(rotated(coverF, back), CV, pic.size(), 0, 0, cv::INTER_LINEAR);
	MW = cv::max(MW, 0);

	/* 2. luma, guided by the mono at the luma's level */
	cv::Mat ycc;
	cv::cvtColor(pic, ycc, cv::COLOR_BGR2YCrCb);
	ycc.convertTo(ycc, CV_32FC3, 1.0 / 255);
	std::vector<cv::Mat> ch;
	cv::split(ycc, ch);
	cv::Mat Y = ch[0], Ylin;
	cv::pow(Y, 2.2, Ylin);
	std::vector<float> ratios;
	for (int y = 0; y < Y.rows; y += 4)
		for (int x = 0; x < Y.cols; x += 4) {
			float yl = Ylin.at<float>(y, x), m = MW.at<float>(y, x);
			if (CV.at<float>(y, x) > 0.99f && yl > 0.01f && yl < 0.8f && m > 1e-4f)
				ratios.push_back(yl / m);
		}
	if (ratios.size() < 10000) {
		std::fprintf(stderr, "cezanne-photo: low light: too little overlap\n");
		return {};
	}
	std::nth_element(ratios.begin(), ratios.begin() + ratios.size() / 2, ratios.end());
	cv::Mat G;
	cv::pow(cv::min(MW * ratios[ratios.size() / 2], 1), 1 / 2.2, G);

	/* the luma's noise: median absolute fine detail */
	float sig;
	{
		cv::Mat bl, hp;
		cv::GaussianBlur(Y, bl, cv::Size(0, 0), 1.0);
		hp = cv::abs(Y - bl);
		std::vector<float> v;
		for (int y = 0; y < hp.rows; y += 3)
			for (int x = 0; x < hp.cols; x += 3)
				v.push_back(hp.at<float>(y, x));
		std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
		sig = v[v.size() / 2] * 1.4826f * 1.6f;
	}
	static const double keep[] = { 0.5, 0.4, 0.3, 0.2 };
	cv::Mat Yg = guided(G, Y, 6, double(2 * sig) * (2 * sig));
	cv::Mat Yout = Yg + keep[std::clamp(level, 0, 3)] * (Y - Yg);

	/* 3. chroma at a quarter of the resolution, guided by the mono */
	cv::Size qs(pic.cols / 4, pic.rows / 4);
	cv::Mat Gq;
	cv::resize(G, Gq, qs, 0, 0, cv::INTER_AREA);
	cv::Mat chOut[2];
	for (int k = 0; k < 2; k++) {
		cv::Mat Cq;
		cv::resize(ch[k + 1], Cq, qs, 0, 0, cv::INTER_AREA);
		cv::resize(guided(Gq, Cq, 6, 0.02 * 0.02), chOut[k], pic.size(), 0, 0, cv::INTER_LINEAR);
	}

	/* 4. confidence: band-pass correlation of luma and mono (quarter res) */
	auto band = [&](const cv::Mat &x) {
		cv::Mat a, b, q;
		cv::GaussianBlur(x, a, cv::Size(0, 0), 1.5);
		cv::GaussianBlur(x, b, cv::Size(0, 0), 6);
		cv::resize(a - b, q, qs, 0, 0, cv::INTER_AREA);
		return q;
	};
	auto win = [](const cv::Mat &x) {
		cv::Mat y;
		cv::GaussianBlur(x, y, cv::Size(0, 0), 6);
		return y;
	};
	cv::Mat bY = band(Y), bG = band(G);
	cv::Mat vY = win(bY.mul(bY)), vG = win(bG.mul(bG));
	cv::Mat ncc;
	cv::sqrt(vY.mul(vG) + 1e-8, ncc);
	ncc = win(bY.mul(bG)) / ncc;
	cv::Mat conf = cv::min(cv::max((ncc - 0.2) / 0.4, 0), 1);
	/*
	 * Nothing to correlate in the picture (flat): the mono's smoothing is
	 * safe only where the mono is flat as well.  Close up, the main
	 * camera's own defocus blur is flat while the fixed-focus mono stays
	 * sharp, and the mono's edges there (warped by a disparity that is
	 * wrong in occlusions) came out as sharp, misplaced cut-outs in the
	 * blurred background.  The decision is smoothed: a hard switch left
	 * speckled borders.
	 */
	const float flat = (sig * 0.7f) * (sig * 0.7f);
	cv::Mat flatY = vY < flat, flatG = vG < flat;
	conf.setTo(1.0f, flatY & flatG);
	conf.setTo(0.0f, flatY & ~flatG);
	cv::GaussianBlur(conf, conf, cv::Size(0, 0), 4);
	cv::resize(conf, conf, pic.size(), 0, 0, cv::INTER_LINEAR);
	conf = conf.mul(cv::min(cv::max(CV, 0), 1));
	debugMap("lowlight-conf", conf);
	std::fprintf(stderr, "cezanne-photo: low light: noise %.2f levels, confidence %.2f\n",
		     sig * 255, cv::mean(conf)[0]);

	/* fall back to the picture's own noise reduction where they disagree */
	cv::Mat fb = denoise(pic, std::clamp(level, 1, 3)), fbF;
	cv::cvtColor(fb, fbF, cv::COLOR_BGR2YCrCb);
	fbF.convertTo(fbF, CV_32FC3, 1.0 / 255);
	std::vector<cv::Mat> fch;
	cv::split(fbF, fch);
	cv::Mat inv = 1 - conf;
	std::vector<cv::Mat> outCh = { conf.mul(Yout) + inv.mul(fch[0]),
				       conf.mul(chOut[0]) + inv.mul(fch[1]),
				       conf.mul(chOut[1]) + inv.mul(fch[2]) };
	cv::Mat out;
	cv::merge(outCh, out);
	out.convertTo(out, CV_8UC3, 255.0);
	cv::cvtColor(out, out, cv::COLOR_YCrCb2BGR);
	return out;
}

/* the picture with the bracket's dynamic range; empty if it cannot be used */
cv::Mat hdr(const cv::Mat &pic, const std::string &prefix)
{
	std::vector<Frame> frames(3);
	for (int i = 0; i < 3; i++) {
		if (!readPgm(prefix + "-" + std::to_string(i) + ".pgm", frames[i])) {
			std::fprintf(stderr, "cezanne-photo: bracket %d missing\n", i);
			return {};
		}
	}
	cv::Mat rad = mergeBracket(frames);

	/* picture at the mono frame's resolution (long side), luma */
	int monoLong = std::max(rad.cols, rad.rows);
	double s = double(monoLong) / std::max(pic.cols, pic.rows);
	cv::Mat picSmall, ySmall8;
	cv::resize(pic, picSmall, cv::Size(), s, s, cv::INTER_AREA);
	cv::cvtColor(picSmall, ySmall8, cv::COLOR_BGR2GRAY);
	cv::Mat ySmallEq;
	cv::equalizeHist(ySmall8, ySmallEq);

	/* the mono frame in either quarter turn */
	cv::Mat H, radRot, best;
	int bestInliers = 0;
	for (int code : { cv::ROTATE_90_CLOCKWISE, cv::ROTATE_90_COUNTERCLOCKWISE }) {
		cv::Mat r, h;
		cv::rotate(rad, r, code);
		int n;
		h = align(viewU8(r), ySmallEq, n);
		std::fprintf(stderr, "cezanne-photo: rotation %d: %d inliers\n", code, n);
		if (!h.empty() && n > bestInliers) {
			bestInliers = n;
			H = h;
			radRot = r;
		}
	}
	if (H.empty()) {
		std::fprintf(stderr, "cezanne-photo: could not align the mono frames\n");
		return {};
	}

	/* mono radiance in the picture's (small) geometry, and its coverage */
	cv::Mat radW, cover;
	cv::warpPerspective(radRot, radW, H, picSmall.size(), cv::INTER_LINEAR,
			    cv::BORDER_CONSTANT, 0);
	cv::warpPerspective(cv::Mat::ones(radRot.size(), CV_32F), cover, H, picSmall.size(),
			    cv::INTER_LINEAR, cv::BORDER_CONSTANT, 0);
	/* feather the edge of the mono field of view */
	cv::erode(cover, cover, cv::Mat(), cv::Point(-1, -1), 8);
	cv::GaussianBlur(cover, cover, cv::Size(0, 0), 12);

	/* the picture's linear luma; where it is reliable */
	cv::Mat lin = toLinear(ySmall8);
	cv::Mat good = (lin > 0.02f) & (lin < 0.7f) & (cover > 0.99f) & (radW > 0);

	/* scale the mono radiance to the picture: median ratio */
	std::vector<float> ratios;
	for (int y = 0; y < lin.rows; y += 2)
		for (int x = 0; x < lin.cols; x += 2)
			if (good.at<uchar>(y, x))
				ratios.push_back(lin.at<float>(y, x) / radW.at<float>(y, x));
	if (ratios.size() < 1000) {
		std::fprintf(stderr, "cezanne-photo: too little overlap\n");
		return {};
	}
	std::nth_element(ratios.begin(), ratios.begin() + ratios.size() / 2, ratios.end());
	cv::Mat monoLin = radW * ratios[ratios.size() / 2];
	/*
	 * The mono's level, not its detail: a homography cannot align a second
	 * camera's view of near things (parallax), and the fixed-focus mono is
	 * sharp where the main camera's view is defocused; its detail came out
	 * as misplaced sharp edges in the bright parts of blurred backgrounds
	 * (white keys behind a close-up subject)
	 */
	cv::GaussianBlur(monoLin, monoLin, cv::Size(0, 0), 6);

	/*
	 * HDR luma: the picture's where well exposed (its own detail and
	 * noise), the mono's towards clipping and black
	 */
	cv::Mat fromBlack = (lin - 0.004f) / 0.03f, toClip = (0.95f - lin) / 0.2f, wPic;
	cv::min(fromBlack, toClip, wPic);
	wPic = cv::min(cv::max(wPic, 0), 1);
	wPic = 1 - (1 - wPic).mul(cover);
	cv::Mat hdr = lin.mul(wPic) + monoLin.mul(1 - wPic);
	hdr = cv::max(hdr, 1e-4f);

	/*
	 * Local tone mapping on the smooth base layer (log luma), the detail
	 * kept: midtones as the picture has them, highlights above kKnee
	 * compressed (where the mono frames saw past the picture's clipping),
	 * shadows below kToe lifted.  No global renormalisation: a compressed
	 * global curve made the whole picture flat and grey.
	 */
	const float kKnee = std::log(0.45f), kHighSlope = 0.45f;
	const float kToe = std::log(0.04f), kLowSlope = 0.7f;
	cv::Mat logH, base, detail;
	cv::log(hdr, logH);
	cv::bilateralFilter(logH, base, 15, 0.6, 12);
	detail = logH - base;
	cv::Mat baseOut = base.clone();
	baseOut.forEach<float>([&](float &v, const int *) {
		if (v > kKnee)
			v = kKnee + (v - kKnee) * kHighSlope;
		else if (v < kToe)
			v = kToe + (v - kToe) * kLowSlope;
	});
	cv::Mat tm;
	cv::exp(baseOut + detail * 1.1f, tm);

	/* gain map, smooth, 1 outside the mono view */
	cv::Mat gain = tm / cv::max(lin, 1e-3f);
	gain = cv::min(cv::max(gain, 0.35f), 4.0f);
	gain = gain.mul(cover) + (1 - cover);
	cv::GaussianBlur(gain, gain, cv::Size(0, 0), 1.0);

	/* apply in linear light at full resolution */
	cv::Mat gainFull;
	cv::resize(gain, gainFull, pic.size(), 0, 0, cv::INTER_CUBIC);
	cv::Mat picLin = toLinear(pic);		/* 3 channels */
	std::vector<cv::Mat> ch;
	cv::split(picLin, ch);
	for (cv::Mat &c : ch)
		c = c.mul(gainFull);
	cv::merge(ch, picLin);
	cv::Mat out;
	cv::pow(cv::min(cv::max(picLin, 0), 1), 1 / 2.2, out);
	out.convertTo(out, CV_8U, 255.0);

	std::fprintf(stderr, "cezanne-photo: HDR with %d inliers\n", bestInliers);
	return out;
}

int main(int argc, char **argv)
{
	int level = 0, status = 0, quality = 95, factor = 1;
	std::string prefix, portraitPrefix, lowPrefix;
	bool people = false;
	static std::string marker;
	int i = 1;

	for (; i < argc && !std::strncmp(argv[i], "--", 2); i++) {
		if (!std::strcmp(argv[i], "--denoise") && i + 1 < argc)
			level = std::clamp(std::atoi(argv[++i]), 0, 3);
		else if (!std::strcmp(argv[i], "--hdr") && i + 1 < argc)
			prefix = argv[++i];
		else if (!std::strcmp(argv[i], "--portrait") && i + 1 < argc)
			portraitPrefix = argv[++i];
		else if (!std::strcmp(argv[i], "--subject") && i + 1 < argc)
			people = !std::strcmp(argv[++i], "people");
		else if (!std::strcmp(argv[i], "--lowlight") && i + 1 < argc)
			lowPrefix = argv[++i];
		else if (!std::strcmp(argv[i], "--upscale") && i + 1 < argc)
			factor = std::atoi(argv[++i]) >= 4 ? 4 : 2;
		else if (!std::strcmp(argv[i], "--quality") && i + 1 < argc)
			quality = std::clamp(std::atoi(argv[++i]), 1, 100);
		else if (!std::strcmp(argv[i], "--marker") && i + 1 < argc)
			marker = argv[++i];
	}
	std::atexit([] {
		if (!marker.empty())
			std::remove(marker.c_str());
	});
	if (!marker.empty()) {
		FILE *m = std::fopen(marker.c_str(), "a");
		if (m) {
			std::fprintf(m, "\n%d\n", getpid());
			std::fclose(m);
		}
	}
	if (i >= argc) {
		std::fprintf(stderr, "usage: cezanne-photo [--denoise 1|2|3 [--lowlight <bracket prefix>]] [--portrait <bracket prefix> [--subject objects|people]] [--hdr <bracket prefix>] [--upscale 2|4] [--quality N] [--marker <file>] <picture> [out]\n");
		return 1;
	}
	std::string inPath = argv[i], outPath = i + 1 < argc ? argv[i + 1] : inPath;

	cv::Mat pic = cv::imread(inPath, cv::IMREAD_COLOR | cv::IMREAD_IGNORE_ORIENTATION);
	if (pic.empty()) {
		std::fprintf(stderr, "cezanne-photo: cannot read %s\n", inPath.c_str());
		return 1;
	}
	bool changed = false;
	if (level) {
		cv::Mat l = lowPrefix.empty() ? cv::Mat() : lowLight(pic, lowPrefix, level);
		pic = l.empty() ? denoise(pic, level) : l;
		changed = true;
	}
	/* HDR before the portrait blur: the bracket's frames are sharp, and
	 * merged after the blur they put background detail back */
	if (!prefix.empty()) {
		cv::Mat h = hdr(pic, prefix);
		if (h.empty())
			status = 2;
		else {
			pic = h;
			changed = true;
		}
	}
	if (!portraitPrefix.empty()) {
		/* the APU's mask first (CEZANNE_PORTRAIT=stereo: depth only) */
		const char *mode = std::getenv("CEZANNE_PORTRAIT");
		cv::Mat p = mode && !std::strcmp(mode, "stereo") ? cv::Mat() : portraitApu(pic, uprightRotation(inPath), people);
		if (p.empty())
			p = portrait(pic, portraitPrefix);
		if (p.empty())
			status = 4;
		else {
			pic = p;
			changed = true;
		}
	}
	for (const std::string &p : { prefix, portraitPrefix, lowPrefix })
		for (int b = 0; !p.empty() && b < 3; b++)
			std::remove((p + "-" + std::to_string(b) + ".pgm").c_str());
	if (factor > 1) {
		/* the markers' parent directory (Snapshot's HDR cache), else /tmp */
		std::string dir = "/tmp";
		if (marker.find('/') != std::string::npos) {
			dir = marker.substr(0, marker.find_last_of('/'));
			dir = dir.substr(0, dir.find_last_of('/'));
		}
		cv::Mat up = upscale(pic, factor, dir);
		if (up.empty())
			status = 3;
		else {
			pic = up;
			changed = true;
		}
	}
	if (!changed)
		return status;

	std::string ext = outPath.substr(outPath.find_last_of('.') + 1);
	std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
	bool png = ext == "png";
	std::string tmp = outPath + (png ? ".photo-tmp.png" : ".photo-tmp.jpg");
	std::vector<int> params = png
		? std::vector<int>{ cv::IMWRITE_PNG_COMPRESSION, 3 }
		: std::vector<int>{ cv::IMWRITE_JPEG_QUALITY, quality };
	if (!cv::imwrite(tmp, pic, params)) {
		std::fprintf(stderr, "cezanne-photo: cannot write %s\n", tmp.c_str());
		return 1;
	}
	copyExif(inPath, tmp);
	if (std::rename(tmp.c_str(), outPath.c_str())) {
		std::perror("cezanne-photo: rename");
		return 1;
	}
	return status;
}
