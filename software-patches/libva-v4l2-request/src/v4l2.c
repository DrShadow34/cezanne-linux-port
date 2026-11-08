// SPDX-License-Identifier: MIT
/* cezanne VA-API driver: V4L2 / media device discovery and helpers */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/media.h>

#include "cvd.h"

int cvd_ioctl(int fd, unsigned long req, void *arg)
{
	int r;

	do
		r = ioctl(fd, req, arg);
	while (r < 0 && errno == EINTR);
	return r;
}

bool cvd_has_output_format(int fd, uint32_t pixfmt)
{
	struct v4l2_fmtdesc f;

	for (f = (struct v4l2_fmtdesc){ .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE };
	     !cvd_ioctl(fd, VIDIOC_ENUM_FMT, &f); f.index++)
		if (f.pixelformat == pixfmt)
			return true;
	return false;
}

/*
 * The decoder: a mem2mem node with stateless H.264 or VP9 input and MM21
 * output (LIBVA_V4L2_REQUEST_VIDEO_PATH / _MEDIA_PATH override), and the
 * media device with the same bus_info.
 */
int cvd_find_devices(struct cvd_driver *d)
{
	const char *vpath = getenv("LIBVA_V4L2_REQUEST_VIDEO_PATH");
	const char *mpath = getenv("LIBVA_V4L2_REQUEST_MEDIA_PATH");
	struct v4l2_capability cap;
	char path[32];
	int i, fd = -1;

	for (i = vpath ? -1 : 0; i < 64; i++) {
		if (i < 0)
			snprintf(path, sizeof(path), "%s", vpath);
		else
			snprintf(path, sizeof(path), "/dev/video%d", i);
		fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0)
			continue;
		memset(&cap, 0, sizeof(cap));
		if (!cvd_ioctl(fd, VIDIOC_QUERYCAP, &cap) &&
		    (cap.device_caps & V4L2_CAP_VIDEO_M2M_MPLANE) &&
		    (cvd_has_output_format(fd, V4L2_PIX_FMT_H264_SLICE) ||
		     cvd_has_output_format(fd, V4L2_PIX_FMT_VP9_FRAME)))
			break;
		close(fd);
		fd = -1;
		if (i < 0)
			break;
	}
	if (fd < 0)
		return -1;
	d->video_fd = fd;
	snprintf(d->video_path, sizeof(d->video_path), "%s", path);
	d->has_h264 = cvd_has_output_format(fd, V4L2_PIX_FMT_H264_SLICE);
	d->has_vp9 = cvd_has_output_format(fd, V4L2_PIX_FMT_VP9_FRAME);
	d->has_vp8 = cvd_has_output_format(fd, V4L2_PIX_FMT_VP8_FRAME);
	d->has_av1 = cvd_has_output_format(fd, V4L2_PIX_FMT_AV1_FRAME);
	d->has_hevc = cvd_has_output_format(fd, V4L2_PIX_FMT_HEVC_SLICE);
	cvd_dbg("video device %s (%s)\n", path, cap.driver);

	for (i = mpath ? -1 : 0; i < 16; i++) {
		struct media_device_info info;
		int mfd;

		if (i < 0)
			snprintf(path, sizeof(path), "%s", mpath);
		else
			snprintf(path, sizeof(path), "/dev/media%d", i);
		mfd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
		if (mfd < 0)
			continue;
		memset(&info, 0, sizeof(info));
		if (i < 0 || (!cvd_ioctl(mfd, MEDIA_IOC_DEVICE_INFO, &info) &&
			      !strncmp(info.bus_info, (char *)cap.bus_info, sizeof(info.bus_info)))) {
			d->media_fd = mfd;
			cvd_dbg("media device %s\n", path);
			return 0;
		}
		close(mfd);
	}
	/* no media node (e.g. a sandbox): requests come from the video node */
	d->media_fd = -1;
	cvd_dbg("no media device, using the video node for requests\n");
	return 0;
}

/*
 * The stateful H.264 encoder, by its sysfs name (no camera node is opened
 * on the way; CEZANNE_VA_ENCODER overrides). Each encode context opens it.
 */
bool cvd_find_encoder(struct cvd_driver *d)
{
	const char *path = getenv("CEZANNE_VA_ENCODER");
	char name[64], sysname[64];
	int i;

	for (i = path ? -1 : 0; i < 64; i++) {
		FILE *f;
		size_t len;

		if (i >= 0) {
			snprintf(sysname, sizeof(sysname), "/sys/class/video4linux/video%d/name", i);
			f = fopen(sysname, "re");
			if (!f)
				continue;
			len = fread(name, 1, sizeof(name) - 1, f);
			fclose(f);
			name[len] = 0;
			if (strncmp(name, "cezanne-venc", 12) || (name[12] && name[12] != '\n'))
				continue;
			snprintf(d->venc_path, sizeof(d->venc_path), "/dev/video%d", i);
		} else {
			snprintf(d->venc_path, sizeof(d->venc_path), "%s", path);
		}
		if (!access(d->venc_path, R_OK | W_OK)) {
			cvd_dbg("encoder %s\n", d->venc_path);
			return true;
		}
		if (i < 0)
			break;
	}
	d->venc_path[0] = 0;
	return false;
}

int cvd_request_alloc(struct cvd_driver *d)
{
	int fd = -1;

	if (d->media_fd >= 0) {
		if (cvd_ioctl(d->media_fd, MEDIA_IOC_REQUEST_ALLOC, &fd) < 0)
			return -1;
		return fd;
	}
	if (cvd_ioctl(d->video_fd, CEZANNE_VDEC_IOC_REQUEST_ALLOC, &fd) < 0)
		return -1;
	return fd;
}

/* request ioctls: on the request fd, or through the video node where a
 * sandbox's syscall filter blocks media ioctls */
int cvd_request_queue(struct cvd_driver *d, int req)
{
	if (d->media_fd >= 0)
		return cvd_ioctl(req, MEDIA_REQUEST_IOC_QUEUE, NULL);
	return ioctl(d->video_fd, CEZANNE_VDEC_IOC_REQUEST_QUEUE, (unsigned long)req);
}

int cvd_request_reinit(struct cvd_driver *d, int req)
{
	if (d->media_fd >= 0)
		return cvd_ioctl(req, MEDIA_REQUEST_IOC_REINIT, NULL);
	return ioctl(d->video_fd, CEZANNE_VDEC_IOC_REQUEST_REINIT, (unsigned long)req);
}
