/*
 * © Copyright 2017-2018 Alyssa Rosenzweig
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
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <string.h>
#include <sys/mman.h>
#include <assert.h>

#include <mali-kbase-ioctl.h>
#include <panfrost-misc.h>
#include "pan_nondrm.h"

/* From the kernel module */

#define USE_LEGACY_KERNEL
#define BASE_MEM_MAP_TRACKING_HANDLE (3ull << 12)

int
pandev_ioctl(int fd, unsigned long request, void *args)
{
        return ioctl(fd, request, args);
}

int
pandev_general_allocate(int fd, int va_pages, int commit_pages,
                        int extent, int flags,
                        u64 *out, int *out_flags)
{
        int ret;
        union kbase_ioctl_mem_alloc args = {
                .in.va_pages = va_pages,
                .in.commit_pages = commit_pages,
                .in.extent = extent,
                .in.flags = flags,
        };

        ret = ioctl(fd, KBASE_IOCTL_MEM_ALLOC, &args);
        if (ret) {
                fprintf(stderr, "panfrost: Failed to allocate memory, va_pages=%d commit_pages=%d extent=%d flags=0x%x rc=%d\n",
                        va_pages, commit_pages, extent, flags, ret);
                abort();
        }
        *out = args.out.gpu_va;
        *out_flags = args.out.flags;

        return 0;
}

int
pandev_standard_allocate(int fd, int va_pages, int flags,
                         u64 *out, int *out_flags)
{
        return pandev_general_allocate(fd, va_pages, va_pages, 0, flags, out,
                                       out_flags);
}

int
pandev_open(int fd)
{
#ifdef USE_LEGACY_KERNEL
        struct kbase_ioctl_version_check version = { .major = 11, .minor = 11 };
        struct kbase_ioctl_set_flags set_flags = {};
        int ret;

        ret = ioctl(fd, KBASE_IOCTL_VERSION_CHECK, &version);
        if (ret != 0) {
                fprintf(stderr, "Version check failed with %d (reporting UK %d.%d)\n",
                        ret, version.major, version.minor);
                abort();
        }
        printf("panfrost: Using kbase UK version %d.%d, fd %d\n", version.major, version.minor, fd);

        if (mmap(NULL, 4096, PROT_NONE, MAP_SHARED, fd, BASE_MEM_MAP_TRACKING_HANDLE) == MAP_FAILED) {
                perror("mmap");
                abort();
        }
        ret = ioctl(fd, KBASE_IOCTL_SET_FLAGS, &set_flags);
        if (ret != 0) {
                fprintf(stderr, "Setting context flags failed with %d\n", ret);
                abort();
        }

#endif

        return fd;
}
