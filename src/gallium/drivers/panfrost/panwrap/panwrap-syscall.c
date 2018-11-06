/*
 * © Copyright 2017 The Panfrost Community
 *
 * This program is free software and is provided to you under the terms of the
 * GNU General Public License version 2 as published by the Free Software
 * Foundation, and any use by you of this program is subject to the terms
 * of such GNU licence.
 *
 * A copy of the licence is included with the program, and can also be obtained
 * from Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <pthread.h>
#include <linux/ioctl.h>
#include <sys/ioctl.h>
#include <math.h>
#include <sys/mman.h>
#include <unistd.h>
#include <linux/limits.h>
#include <sys/stat.h>
#include <errno.h>
#include <ctype.h>

#include <assert.h>
#include <mali-ioctl.h>
#include "panwrap.h"

static pthread_mutex_t l;
PANLOADER_CONSTRUCTOR {
	pthread_mutexattr_t mattr;

	pthread_mutexattr_init(&mattr);
	pthread_mutexattr_settype(&mattr, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(&l, &mattr);
	pthread_mutexattr_destroy(&mattr);
}

#define IOCTL_CASE(request) (_IOWR(_IOC_TYPE(request), _IOC_NR(request), \
				   _IOC_SIZE(request)))

struct ioctl_info {
	const char *name;
};

struct device_info {
	const char *name;
	const struct ioctl_info info[MALI_IOCTL_TYPE_COUNT][_IOC_NR(0xffffffff)];
};

typedef void* (mmap_func)(void *, size_t, int, int, int, loff_t);
typedef int (open_func)(const char *, int flags, ...);

#define IOCTL_TYPE(type) [type - MALI_IOCTL_TYPE_BASE] =
#define IOCTL_INFO(n) [_IOC_NR(MALI_IOCTL_##n)] = { .name = #n }
static struct device_info mali_info = {
	.name = "mali",
	.info = {
		IOCTL_TYPE(0x80) {
			IOCTL_INFO(GET_VERSION),
		},
		IOCTL_TYPE(0x82) {
			IOCTL_INFO(MEM_ALLOC),
			IOCTL_INFO(MEM_IMPORT),
			IOCTL_INFO(JOB_SUBMIT),
		},
	},
};
#undef IOCTL_INFO
#undef IOCTL_TYPE

static inline const struct ioctl_info *
ioctl_get_info(unsigned long int request)
{
	return &mali_info.info[_IOC_TYPE(request) - MALI_IOCTL_TYPE_BASE]
	                      [_IOC_NR(request)];
}

static int mali_fd = 0;

#define LOCK()   pthread_mutex_lock(&l);
#define UNLOCK() panwrap_log_flush(); pthread_mutex_unlock(&l)

#define FLAG_INFO(flag) { MALI_JD_REQ_##flag, "MALI_JD_REQ_" #flag }
static const struct panwrap_flag_info jd_req_flag_info[] = {
	FLAG_INFO(FS),
	FLAG_INFO(CS),
	FLAG_INFO(T),
	FLAG_INFO(CF),
	FLAG_INFO(V),
	FLAG_INFO(FS_AFBC),
	FLAG_INFO(EVENT_COALESCE),
	FLAG_INFO(COHERENT_GROUP),
	FLAG_INFO(PERMON),
	FLAG_INFO(EXTERNAL_RESOURCES),
	FLAG_INFO(ONLY_COMPUTE),
	FLAG_INFO(SPECIFIC_COHERENT_GROUP),
	FLAG_INFO(EVENT_ONLY_ON_FAILURE),
	FLAG_INFO(EVENT_NEVER),
	FLAG_INFO(SKIP_CACHE_START),
	FLAG_INFO(SKIP_CACHE_END),
	{}
};
#undef FLAG_INFO

#define SOFT_FLAG(flag)                                  \
	case MALI_JD_REQ_SOFT_##flag:                    \
		panwrap_log_cont("MALI_JD_REQ_%s", "SOFT_" #flag); \
		break
/* Decodes the actual jd_core_req flags, but not their meanings */
void
ioctl_log_decoded_jd_core_req(mali_jd_core_req req)
{
	if (req & MALI_JD_REQ_SOFT_JOB) {
		/* External resources are allowed in e.g. replay jobs */

		if (req & MALI_JD_REQ_EXTERNAL_RESOURCES) {
			panwrap_log_cont("MALI_JD_REQ_EXTERNAL_RESOURCES | ");
			req &= ~(MALI_JD_REQ_EXTERNAL_RESOURCES);
		}

		switch (req) {
		SOFT_FLAG(DUMP_CPU_GPU_TIME);
		SOFT_FLAG(FENCE_TRIGGER);
		SOFT_FLAG(FENCE_WAIT);
		SOFT_FLAG(REPLAY);
		SOFT_FLAG(EVENT_WAIT);
		SOFT_FLAG(EVENT_SET);
		SOFT_FLAG(EVENT_RESET);
		SOFT_FLAG(DEBUG_COPY);
		SOFT_FLAG(JIT_ALLOC);
		SOFT_FLAG(JIT_FREE);
		SOFT_FLAG(EXT_RES_MAP);
		SOFT_FLAG(EXT_RES_UNMAP);
		default: panwrap_log_cont("0x%010x", req); break;
		}
	} else {
		panwrap_log_decoded_flags(jd_req_flag_info, req);
	}
}
#undef SOFT_FLAG

static int job_count = 0;

static void emit_atoms(void *ptr, bool bifrost) {
	const struct mali_ioctl_job_submit *args = ptr;
	const struct mali_jd_atom_v2 *atoms = args->addr;

	int job_no = job_count++;

	int job_numbers[256] = { 0 };

	for (int i = 0; i < args->nr_atoms; i++) {
		const struct mali_jd_atom_v2 *a = &atoms[i];

		if (a->jc) {
			int req = a->core_req | a->compat_core_req;

			if (!(req & MALI_JD_REQ_SOFT_JOB))
				job_numbers[i] = panwrap_replay_jc(a->jc, bifrost);
			else if (req & MALI_JD_REQ_SOFT_REPLAY)
				job_numbers[i] = panwrap_replay_soft_replay(a->jc);
		}
	}

	for (int i = 0; i < args->nr_atoms; i++) {
		const struct mali_jd_atom_v2 *a = &atoms[i];

		if (a->ext_res_list) {
			panwrap_log("mali_external_resource resources_%d_%d[] = {\n", job_no, i);
			panwrap_indent++;

			for (int j = 0; j < a->nr_ext_res; j++) {
				/* Substitute in our framebuffer */
				panwrap_log("framebuffer_va | MALI_EXT_RES_ACCESS_EXCLUSIVE,\n");
			}

			panwrap_indent--;
			panwrap_log("};\n\n");

		}
	}

	panwrap_log("struct mali_jd_atom_v2 atoms_%d[] = {\n", job_no);
	panwrap_indent++;

	for (int i = 0; i < args->nr_atoms; i++) {
		const struct mali_jd_atom_v2 *a = &atoms[i];

		panwrap_log("{\n");
		panwrap_indent++;

		panwrap_prop("jc = job_%d_p", job_numbers[i]);

		/* Don't passthrough udata; it's nondeterministic and for userspace use only */

		panwrap_prop("nr_ext_res = %d", a->nr_ext_res);

		if (a->ext_res_list)
			panwrap_prop("ext_res_list = resources_%d_%d", job_no, i);

		if (a->compat_core_req)
			panwrap_prop("compat_core_req = 0x%x", a->compat_core_req);

		if (a->core_req) {
			/* Note that older kernels prefer compat_core_req... */
			panwrap_log(".core_req = ");
			ioctl_log_decoded_jd_core_req(a->core_req);
			panwrap_log_cont(",\n");
		}

		panwrap_log(".pre_dep = {\n");
		panwrap_indent++;
		for (int j = 0; j < ARRAY_SIZE(a->pre_dep); j++) {
			if (a->pre_dep[j].dependency_type || a->pre_dep[j].atom_id)
				panwrap_log("{ .atom_id = %d, .dependency_type = %d },\n",
					    a->pre_dep[j].atom_id, a->pre_dep[j].dependency_type);
		}
		panwrap_indent--;
		panwrap_log("},\n");

		/* TODO: Compute atom numbers dynamically and correctly */
		panwrap_prop("atom_number = %d + %d*%s", a->atom_number, 3, "i");

		panwrap_prop("prio = %d", a->prio);
		panwrap_prop("device_nr = %d", a->device_nr);

		panwrap_indent--;
		panwrap_log("},\n");

	}

	panwrap_indent--;
	panwrap_log("};\n\n");
}

static inline void
ioctl_decode_pre_job_submit(unsigned long int request, void *ptr)
{
	const struct mali_ioctl_job_submit *args = ptr;
	const struct mali_jd_atom_v2 *atoms = args->addr;

	panwrap_prop("addr = atoms_%d", job_count - 1); /* XXX */
	panwrap_prop("nr_atoms = %d", args->nr_atoms);
	panwrap_prop("stride = %d", args->stride);

	assert (args->stride == sizeof(*atoms));
}

/**
 * Overriden libc functions start here
 */
static inline int
panwrap_open_wrap(open_func *func, const char *path, int flags, va_list args)
{
	mode_t mode = 0;
	int ret;

	if (flags & O_CREAT) {
		mode = (mode_t) va_arg(args, int);
		ret = func(path, flags, mode);
	} else {
		ret = func(path, flags);
	}

	LOCK();
	if (ret != -1 && strcmp(path, "/dev/mali0") == 0)
		mali_fd = ret;
	UNLOCK();

	return ret;
}

//#ifdef IS_OPEN64_SEPERATE_SYMBOL
int
open(const char *path, int flags, ...)
{
	PROLOG(open);
	va_list args;
	va_start(args, flags);
	int o = panwrap_open_wrap(orig_open, path, flags, args);
	va_end(args);
	return o;
}
//#endif

#if 0
int
open64(const char *path, int flags, ...)
{
	PROLOG(open64);
	va_list args;
	va_start(args, flags);
	int o = panwrap_open_wrap(orig_open64, path, flags, args);
	va_end(args);
	return o;
}
#endif

int
close(int fd)
{
	PROLOG(close);

        /* Intentionally racy: prevents us from trying to hold the global mutex
         * in calls from system libraries */
        if (fd <= 0 || !mali_fd || fd != mali_fd)
                return orig_close(fd);

	LOCK();
	if (!fd || fd != mali_fd) {
		panwrap_log("/dev/mali0 closed\n");
		mali_fd = 0;
	}
	UNLOCK();

	return orig_close(fd);
}

/* Global count of ioctls, for replay purposes */

static int ioctl_count = 0;

/* HW version */
static bool bifrost = false;

/* XXX: Android has a messed up ioctl signature */
int ioctl(int fd, unsigned long int _request, ...)
{
	int number;
	PROLOG(ioctl);
	unsigned long int request = _request;
	int ioc_size = _IOC_SIZE(request);
	int ret;
	void *ptr;

	if (ioc_size) {
		va_list args;

		va_start(args, _request);
		ptr = va_arg(args, void *);
		va_end(args);
	} else {
		ptr = NULL;
	}

	if (fd && fd != mali_fd)
		return orig_ioctl(fd, request, ptr);

	LOCK();

	number = ioctl_count++;

	if (IOCTL_CASE(request) == IOCTL_CASE(MALI_IOCTL_JOB_SUBMIT)) {
		emit_atoms(ptr, bifrost);
		ioctl_decode_pre_job_submit(request, ptr);
	}


	ret = orig_ioctl(fd, request, ptr);

	/* Track memory allocation if needed  */
	if (IOCTL_CASE(request) == IOCTL_CASE(MALI_IOCTL_MEM_ALLOC)) {
		const struct mali_ioctl_mem_alloc *args = ptr;

		panwrap_track_allocation(args->gpu_va, args->flags, number, args->va_pages * 4096);
	}

	/* Call the actual ioctl */

	UNLOCK();
	return ret;
}

static inline void *panwrap_mmap_wrap(mmap_func *func,
				      void *addr, size_t length, int prot,
				      int flags, int fd, loff_t offset)
{
	void *ret;

	if (!mali_fd || fd != mali_fd)
		return func(addr, length, prot, flags, fd, offset);

	LOCK();
	ret = func(addr, length, prot, flags, fd, offset);

	switch (offset) { /* offset == gpu_va */
	case MALI_MEM_MAP_TRACKING_HANDLE:
		/* MTP is mapped automatically for us by pandev_open */
		break;
	default:
		panwrap_track_mmap(offset, ret, length, prot, flags);
		break;
	}

	UNLOCK();
	return ret;
}

#if 0
void *mmap64(void *addr, size_t length, int prot, int flags, int fd,
	     loff_t offset)
{
	PROLOG(mmap64);

	return panwrap_mmap_wrap(orig_mmap64, addr, length, prot, flags, fd,
				 offset);
}
#endif

//#ifdef IS_MMAP64_SEPERATE_SYMBOL
void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
#ifdef __LP64__
	PROLOG(mmap);

	return panwrap_mmap_wrap(orig_mmap, addr, length, prot, flags, fd,
				 offset);
#else
	return mmap64(addr, length, prot, flags, fd, (loff_t) offset);
#endif
}
//#endif

int munmap(void *addr, size_t length)
{
	int ret;
	struct panwrap_mapped_memory *mem;
	PROLOG(munmap);

	if (!mali_fd)
		return orig_munmap(addr, length);

	LOCK();
	ret = orig_munmap(addr, length);
	mem = panwrap_find_mapped_mem(addr);
	if (!mem)
		goto out;

	free(mem);
out:
	UNLOCK();
	return ret;
}
