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

#include <panfrost-ioctl.h>
#include "pan_nondrm.h"

/* From the kernel module */

#define USE_LEGACY_KERNEL
#define MALI_MEM_MAP_TRACKING_HANDLE (3ull << 12)

int
pandev_ioctl(int fd, unsigned long request, void *args)
{
        union mali_ioctl_header *h = args;
        h->id = ((_IOC_TYPE(request) & 0xF) << 8) | _IOC_NR(request);
        return ioctl(fd, request, args);
}

int
pandev_general_allocate(int fd, int va_pages, int commit_pages, int extent, int flags, u64 *out)
{
        struct mali_ioctl_mem_alloc args = {
                .va_pages = va_pages,
                .commit_pages = commit_pages,
                .extent = extent,
                .flags = flags
        };

        if (pandev_ioctl(fd, MALI_IOCTL_MEM_ALLOC, &args) != 0) {
                perror("pandev_ioctl MALI_IOCTL_MEM_ALLOC");
                abort();
        }

        *out = args.gpu_va;

        return 0;
}

int
pandev_standard_allocate(int fd, int va_pages, int flags, u64 *out)
{
        return pandev_general_allocate(fd, va_pages, va_pages, 0, flags, out);
}

int
pandev_open()
{

        int fd = open("/dev/mali0", O_RDWR | O_CLOEXEC);
        assert(fd != -1);

#ifdef USE_LEGACY_KERNEL
        struct mali_ioctl_get_version version = { .major = 10, .minor = 4 };
        struct mali_ioctl_set_flags args = {};

        if (pandev_ioctl(fd, MALI_IOCTL_GET_VERSION, &version) != 0) {
                perror("pandev_ioctl: MALI_IOCTL_GET_VERSION");
                abort();
        }

        printf("(%d, %d)\n", version.major, version.minor);

        if (mmap(NULL, 4096, PROT_NONE, MAP_SHARED, fd, MALI_MEM_MAP_TRACKING_HANDLE) == MAP_FAILED) {
                perror("mmap");
                abort();
        }

        if (pandev_ioctl(fd, MALI_IOCTL_SET_FLAGS, &args) != 0) {
                perror("pandev_ioctl: MALI_IOCTL_SET_FLAGS");
                abort();
        }

#endif

        return fd;
}
