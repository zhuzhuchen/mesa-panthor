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

#ifndef __MMAP_TRACE_H__
#define __MMAP_TRACE_H__

#include <stdlib.h>
#include <stddef.h>
#include <mali-kbase-ioctl.h>
#include <panfrost-misc.h>
#include <panfrost-mali-base.h>
#include "panwrap.h"
#include "util/list.h"

struct panwrap_allocated_memory {
        struct list_head node;

        mali_ptr gpu_va;
        int flags;
        int allocation_number;
        size_t length;
};

struct panwrap_mapped_memory {
        struct list_head node;

        size_t length;

        void *addr;
        mali_ptr gpu_va;
        int prot;
        int flags;

        int allocation_number;
        char name[32];

        bool *touched;
};

/* Set this if you don't want your life to be hell while debugging */
#define DISABLE_CPU_CACHING 1

#define TOUCH_MEMSET(mem, addr, sz, offset) \
	memset((mem)->touched + (((addr) - (mem)->gpu_va) / sizeof(uint32_t)), 1, ((sz) - (offset)) / sizeof(uint32_t)); \
	panwrap_log("\n");

#define TOUCH_LEN(mem, addr, sz, ename, number, dyn) \
	TOUCH_MEMSET(mem, addr, sz, 0) \
	panwrap_log("mali_ptr %s_%d_p = pandev_upload(%d, NULL, alloc_gpu_va_%d, %s, &%s_%d, sizeof(%s_%d), true);\n\n", ename, number, (dyn && 0) ? -1 : (int) (((addr) - (mem)->gpu_va)), (mem)->allocation_number, (mem)->name, ename, number, ename, number);

/* Job payloads are touched somewhat different than other structures, due to the
 * variable lengths and odd packing requirements */

#define TOUCH_JOB_HEADER(mem, addr, sz, offset, number) \
	TOUCH_MEMSET(mem, addr, sz, offset) \
	panwrap_log("mali_ptr job_%d_p = pandev_upload(%d, NULL, alloc_gpu_va_%d, %s, &job_%d, sizeof(job_%d) - %d, true);\n\n", number, (uint32_t) ((addr) - (mem)->gpu_va), mem->allocation_number, mem->name, number, number, offset);

#define TOUCH_SEQUENTIAL(mem, addr, sz, ename, number) \
	TOUCH_MEMSET(mem, addr, sz, 0) \
	panwrap_log("mali_ptr %s_%d_p = pandev_upload_sequential(alloc_gpu_va_%d, %s, &%s_%d, sizeof(%s_%d));\n\n", ename, number, mem->allocation_number, mem->name, ename, number, ename, number);

/* Syntax sugar for sanely sized objects */

#define TOUCH(mem, addr, obj, ename, number, dyn) \
	//TOUCH_LEN(mem, addr, sizeof(typeof(obj)), ename, number, dyn)

void replay_memory();
void replay_memory_specific(struct panwrap_mapped_memory *pos, int offset, int len);
char *pointer_as_memory_reference(mali_ptr ptr);

void panwrap_track_allocation(mali_ptr gpu_va, int flags, int number, size_t length);
void panwrap_track_mmap(mali_ptr gpu_va, void *addr, size_t length,
                        int prot, int flags);
void panwrap_track_munmap(void *addr);

struct panwrap_mapped_memory *panwrap_find_mapped_mem(void *addr);
struct panwrap_mapped_memory *panwrap_find_mapped_mem_containing(void *addr);
struct panwrap_mapped_memory *panwrap_find_mapped_gpu_mem(mali_ptr addr);
struct panwrap_mapped_memory *panwrap_find_mapped_gpu_mem_containing(mali_ptr addr);

void panwrap_assert_gpu_same(const struct panwrap_mapped_memory *mem,
                             mali_ptr gpu_va, size_t size,
                             const unsigned char *data);
void panwrap_assert_gpu_mem_zero(const struct panwrap_mapped_memory *mem,
                                 mali_ptr gpu_va, size_t size);

void __attribute__((noreturn))
__panwrap_fetch_mem_err(const struct panwrap_mapped_memory *mem,
                        mali_ptr gpu_va, size_t size,
                        int line, const char *filename);

static inline void *
__panwrap_fetch_gpu_mem(const struct panwrap_mapped_memory *mem,
                        mali_ptr gpu_va, size_t size,
                        int line, const char *filename)
{
        if (!mem)
                mem = panwrap_find_mapped_gpu_mem_containing(gpu_va);

        if (!mem ||
                        size + (gpu_va - mem->gpu_va) > mem->length ||
                        !(mem->prot & BASE_MEM_PROT_CPU_RD))
                __panwrap_fetch_mem_err(mem, gpu_va, size, line, filename);

        return mem->addr + gpu_va - mem->gpu_va;
}

#define panwrap_fetch_gpu_mem(mem, gpu_va, size) \
	__panwrap_fetch_gpu_mem(mem, gpu_va, size, __LINE__, __FILE__)

/* Returns a validated pointer to mapped GPU memory with the given pointer type,
 * size automatically determined from the pointer type
 */
#define PANWRAP_PTR(mem, gpu_va, type) \
	((type*)(__panwrap_fetch_gpu_mem(mem, gpu_va, sizeof(type), \
					 __LINE__, __FILE__)))

/* Usage: <variable type> PANWRAP_PTR_VAR(name, mem, gpu_va) */
#define PANWRAP_PTR_VAR(name, mem, gpu_va) \
	name = __panwrap_fetch_gpu_mem(mem, gpu_va, sizeof(*name), \
				       __LINE__, __FILE__)

#endif /* __MMAP_TRACE_H__ */
