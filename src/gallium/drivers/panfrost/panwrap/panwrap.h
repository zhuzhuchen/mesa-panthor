/*
 * © Copyright 2017-2018 The Panfrost Community
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 */

/*
 * Various bits and pieces of this borrowed from the freedreno project, which
 * borrowed from the lima project.
 */

#ifndef __WRAP_H__
#define __WRAP_H__

#include <dlfcn.h>
#include <stdbool.h>
#include <time.h>
#include "panwrap-util.h"
#include "panwrap-mmap.h"
#include "panwrap-decoder.h"

struct panwrap_flag_info {
        u64 flag;
        const char *name;
};

#define PROLOG(func) 					\
	static __typeof__(func) *orig_##func = NULL;	\
	if (!orig_##func)				\
		orig_##func = __rd_dlsym_helper(#func);	\

void __attribute__((format (printf, 2, 3))) panwrap_log_typed(enum panwrap_log_type type, const char *format, ...);
void __attribute__((format (printf, 1, 2))) panwrap_log_cont(const char *format, ...);
void panwrap_log_empty(void);
void panwrap_log_flush(void);

void panwrap_log_decoded_flags(const struct panwrap_flag_info *flag_info,
                               u64 flags);
void ioctl_log_decoded_jd_core_req(mali_jd_core_req req);
void panwrap_log_hexdump(const void *data, size_t size);
void panwrap_log_hexdump_trimmed(const void *data, size_t size);

void panwrap_timestamp(struct timespec *);

bool panwrap_parse_env_bool(const char *env, bool def);
long panwrap_parse_env_long(const char *env, long def);
const char *panwrap_parse_env_string(const char *env, const char *def);

extern short panwrap_indent;

void *__rd_dlsym_helper(const char *name);

#endif /* __WRAP_H__ */
