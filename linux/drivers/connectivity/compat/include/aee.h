/* SPDX-License-Identifier: GPL-2.0 */
/* Mainline stand-in for the vendor AEE (crash reporting) header */
#ifndef __AEE_H__
#define __AEE_H__
#define AEE_STR_LEN	300
#define DB_OPT_DEFAULT	0
#define DB_OPT_FTRACE	(1 << 0)
#define aee_kernel_exception(...)		do { } while (0)
#define aee_kernel_warning(...)			do { } while (0)
#define aee_kernel_warning_api(...)		do { } while (0)
#define aee_kernel_exception_api(...)		do { } while (0)
#endif
