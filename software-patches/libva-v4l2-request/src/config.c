// SPDX-License-Identifier: MIT
/* cezanne VA-API driver: profiles and configs */

#include "cvd.h"

static bool h264(VAProfile p)
{
	return p == VAProfileH264ConstrainedBaseline || p == VAProfileH264Main ||
	       p == VAProfileH264High;
}

static bool profile_ok(struct cvd_driver *d, VAProfile p)
{
	switch (p) {
	case VAProfileH264ConstrainedBaseline:
	case VAProfileH264Main:
	case VAProfileH264High:
		return d->has_h264 || d->has_venc;
	case VAProfileVP9Profile0:
	case VAProfileVP9Profile2:
		return d->has_vp9;
	case VAProfileVP8Version0_3:
		return d->has_vp8;
	case VAProfileAV1Profile0:
		return d->has_av1;
	case VAProfileHEVCMain:
	case VAProfileHEVCMain10:
		return d->has_hevc;
	case VAProfileNone:
		return true;		/* the video processor runs on the CPU */
	default:
		return false;
	}
}

/* the profile / entry point pairs there are */
static bool entrypoint_ok(struct cvd_driver *d, VAProfile p, VAEntrypoint e)
{
	if (!profile_ok(d, p))
		return false;
	switch (e) {
	case VAEntrypointVLD:
		return p != VAProfileNone && (!h264(p) || d->has_h264);
	case VAEntrypointEncSlice:
		return h264(p) && d->has_venc;
	case VAEntrypointVideoProc:
		return p == VAProfileNone;
	default:
		return false;
	}
}

VAStatus cvdQueryConfigProfiles(VADriverContextP ctx, VAProfile *list, int *n)
{
	static const VAProfile all[] = {
		VAProfileH264ConstrainedBaseline, VAProfileH264Main, VAProfileH264High,
		VAProfileVP9Profile0, VAProfileVP8Version0_3, VAProfileAV1Profile0, VAProfileHEVCMain,
		VAProfileHEVCMain10, VAProfileVP9Profile2, VAProfileNone,
	};
	unsigned int i;

	_Static_assert(sizeof(all) / sizeof(all[0]) <= CVD_MAX_PROFILES,
		       "CVD_MAX_PROFILES too small for the profile list");

	*n = 0;
	for (i = 0; i < sizeof(all) / sizeof(all[0]); i++)
		if (profile_ok(DRV(ctx), all[i]))
			list[(*n)++] = all[i];
	return VA_STATUS_SUCCESS;
}

/* AV1 main profile is 8 or 10 bit (the stream decides), HEVC Main10 is 10 bit */
unsigned int cvd_rt_formats(VAProfile p)
{
	return p == VAProfileHEVCMain10 || p == VAProfileVP9Profile2 ? VA_RT_FORMAT_YUV420_10 :
	       p == VAProfileAV1Profile0 ? VA_RT_FORMAT_YUV420 | VA_RT_FORMAT_YUV420_10 :
	       VA_RT_FORMAT_YUV420;
}

VAStatus cvdQueryConfigEntrypoints(VADriverContextP ctx, VAProfile p, VAEntrypoint *list, int *n)
{
	static const VAEntrypoint all[] = {
		VAEntrypointVLD, VAEntrypointEncSlice, VAEntrypointVideoProc,
	};
	struct cvd_driver *d = DRV(ctx);
	unsigned int i;

	*n = 0;
	if (!profile_ok(d, p))
		return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
	for (i = 0; i < sizeof(all) / sizeof(all[0]); i++)
		if (entrypoint_ok(d, p, all[i]) && *n < CVD_MAX_ENTRYPOINTS)
			list[(*n)++] = all[i];
	return VA_STATUS_SUCCESS;
}

/*
 * An attribute's value for a profile / entry point. The encoder is the
 * V4L2 stateful cezanne-venc: it writes SPS / PPS and slice headers itself
 * (no packed headers taken), one slice, P frames on one reference, rate
 * control in the kernel driver; input NV12 or RGB (converted to BT.709 by
 * the hardware).
 */
static uint32_t attrib_value(VAProfile p, VAEntrypoint e, VAConfigAttribType t)
{
	if (e == VAEntrypointVLD)
		return t == VAConfigAttribRTFormat ? cvd_rt_formats(p) : VA_ATTRIB_NOT_SUPPORTED;
	if (e == VAEntrypointVideoProc)
		return t == VAConfigAttribRTFormat ? VA_RT_FORMAT_YUV420 | VA_RT_FORMAT_RGB32 :
		       VA_ATTRIB_NOT_SUPPORTED;
	switch (t) {
	case VAConfigAttribRTFormat:
		return VA_RT_FORMAT_YUV420 | VA_RT_FORMAT_RGB32;
	case VAConfigAttribRateControl:
		return VA_RC_CQP | VA_RC_CBR | VA_RC_VBR;
	case VAConfigAttribEncPackedHeaders:
		return VA_ENC_PACKED_HEADER_NONE;
	case VAConfigAttribEncMaxRefFrames:
		return 1;			/* list 0: 1, list 1: none */
	case VAConfigAttribEncMaxSlices:
		return 1;
	case VAConfigAttribMaxPictureWidth:
	case VAConfigAttribMaxPictureHeight:
		return 4096;
	default:
		return VA_ATTRIB_NOT_SUPPORTED;
	}
}

VAStatus cvdGetConfigAttributes(VADriverContextP ctx, VAProfile p, VAEntrypoint e,
				VAConfigAttrib *attr, int n)
{
	struct cvd_driver *d = DRV(ctx);
	int i;

	if (!profile_ok(d, p))
		return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
	if (!entrypoint_ok(d, p, e))
		return VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT;
	for (i = 0; i < n; i++)
		attr[i].value = attrib_value(p, e, attr[i].type);
	return VA_STATUS_SUCCESS;
}

VAStatus cvdCreateConfig(VADriverContextP ctx, VAProfile p, VAEntrypoint e,
			 VAConfigAttrib *attr, int n, VAConfigID *id)
{
	struct cvd_driver *d = DRV(ctx);
	struct cvd_config *c;
	unsigned int rt, rc = VA_RC_CQP;
	int i, cid;

	if (!profile_ok(d, p))
		return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
	if (!entrypoint_ok(d, p, e))
		return VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT;
	rt = attrib_value(p, e, VAConfigAttribRTFormat);
	for (i = 0; i < n; i++) {
		if (attr[i].type == VAConfigAttribRTFormat) {
			if (!(attr[i].value & rt))
				return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
			rt &= attr[i].value;
		} else if (attr[i].type == VAConfigAttribRateControl && e == VAEntrypointEncSlice) {
			if (!(attr[i].value & attrib_value(p, e, VAConfigAttribRateControl)))
				return VA_STATUS_ERROR_INVALID_CONFIG;
			rc = attr[i].value & VA_RC_VBR ? VA_RC_VBR :
			     attr[i].value & VA_RC_CBR ? VA_RC_CBR : VA_RC_CQP;
		}
	}
	cid = object_heap_allocate(&d->config_heap);
	c = CONFIG(d, cid);
	if (!c)
		return VA_STATUS_ERROR_ALLOCATION_FAILED;
	c->profile = p;
	c->entrypoint = e;
	c->rt_format = rt;
	c->rc_mode = rc;
	*id = cid;
	return VA_STATUS_SUCCESS;
}

VAStatus cvdDestroyConfig(VADriverContextP ctx, VAConfigID id)
{
	struct cvd_driver *d = DRV(ctx);
	struct cvd_config *c = CONFIG(d, id);

	if (!c)
		return VA_STATUS_ERROR_INVALID_CONFIG;
	object_heap_free(&d->config_heap, &c->base);
	return VA_STATUS_SUCCESS;
}

VAStatus cvdQueryConfigAttributes(VADriverContextP ctx, VAConfigID id, VAProfile *p,
				  VAEntrypoint *e, VAConfigAttrib *attr, int *n)
{
	static const VAConfigAttribType enc[] = {
		VAConfigAttribRateControl, VAConfigAttribEncPackedHeaders,
		VAConfigAttribEncMaxRefFrames, VAConfigAttribEncMaxSlices,
	};
	struct cvd_config *c = CONFIG(DRV(ctx), id);
	unsigned int i;

	_Static_assert(1 + sizeof(enc) / sizeof(enc[0]) <= CVD_MAX_ATTRIBUTES,
		       "CVD_MAX_ATTRIBUTES too small");
	if (!c)
		return VA_STATUS_ERROR_INVALID_CONFIG;
	*p = c->profile;
	*e = c->entrypoint;
	attr[0].type = VAConfigAttribRTFormat;
	attr[0].value = c->rt_format;
	*n = 1;
	if (c->entrypoint == VAEntrypointEncSlice)
		for (i = 0; i < sizeof(enc) / sizeof(enc[0]); i++, (*n)++) {
			attr[*n].type = enc[i];
			attr[*n].value = enc[i] == VAConfigAttribRateControl ? c->rc_mode :
					 attrib_value(c->profile, c->entrypoint, enc[i]);
		}
	return VA_STATUS_SUCCESS;
}
