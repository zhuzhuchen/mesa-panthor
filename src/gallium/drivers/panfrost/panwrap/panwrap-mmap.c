/*
 * © Copyright 2017 The Panfrost Community
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
#include <sys/mman.h>
#include <stdbool.h>
#include <stdarg.h>
#include <memory.h>

#include <mali-kbase-ioctl.h>
#include "panwrap.h"
#include "panwrap-mmap.h"
#ifdef HAVE_LINUX_MMAN_H
#include <linux/mman.h>
#endif
#include "util/list.h"

static struct panwrap_allocated_memory allocations;
static struct panwrap_mapped_memory mmaps;

#define FLAG_INFO(flag) { flag, #flag }
static const struct panwrap_flag_info mmap_flags_flag_info[] = {
        FLAG_INFO(MAP_SHARED),
        FLAG_INFO(MAP_PRIVATE),
        FLAG_INFO(MAP_ANONYMOUS),
        FLAG_INFO(MAP_DENYWRITE),
        FLAG_INFO(MAP_FIXED),
        FLAG_INFO(MAP_GROWSDOWN),
        FLAG_INFO(MAP_HUGETLB),
        FLAG_INFO(MAP_LOCKED),
        FLAG_INFO(MAP_NONBLOCK),
        FLAG_INFO(MAP_NORESERVE),
        FLAG_INFO(MAP_POPULATE),
        FLAG_INFO(MAP_STACK),
#if MAP_UNINITIALIZED != 0
        FLAG_INFO(MAP_UNINITIALIZED),
#endif
        {}
};

static const struct panwrap_flag_info mmap_prot_flag_info[] = {
        FLAG_INFO(PROT_EXEC),
        FLAG_INFO(PROT_READ),
        FLAG_INFO(PROT_WRITE),
        {}
};
#undef FLAG_INFO

char *
pointer_as_memory_reference(mali_ptr ptr)
{
        struct panwrap_mapped_memory *mapped;
        char *out = malloc(128);

        /* First check for SAME_VA mappings, then look for non-SAME_VA
         * mappings, then for unmapped regions */

        if ((ptr == (uintptr_t) ptr && (mapped = panwrap_find_mapped_mem_containing((void *) (uintptr_t) ptr))) ||
                        (mapped = panwrap_find_mapped_gpu_mem_containing(ptr))) {

                snprintf(out, 128, "alloc_gpu_va_%d + %d", mapped->allocation_number, (int) (ptr - mapped->gpu_va));
                return out;
        }

        struct panwrap_allocated_memory *mem = NULL;

        /* Find the pending unmapped allocation for the memory */
        list_for_each_entry(struct panwrap_allocated_memory, pos, &allocations.node, node) {
                if (ptr >= pos->gpu_va && ptr < (pos->gpu_va + pos->length)) {
                        mem = pos;
                        break;
                }
        }

        if (mem) {
                snprintf(out, 128, "alloc_gpu_va_%d + %d", mem->allocation_number, (int) (ptr - mem->gpu_va));
                return out;
        }

        /* Just use the raw address if other options are exhausted */

        snprintf(out, 128, MALI_PTR_FMT, ptr);
        return out;
}

void
panwrap_track_allocation(mali_ptr addr, int flags, int number, size_t length)
{
        struct panwrap_allocated_memory *mem = malloc(sizeof(*mem));

        //list_inithead(&mem->node);
        mem->gpu_va = addr;
        mem->flags = flags;
        mem->allocation_number = number;
        mem->length = length;

        list_add(&mem->node, &allocations.node);

        /* XXX: Hacky workaround for cz's board */
        if (mem->gpu_va >> 28 == 0xb)
                panwrap_track_mmap(addr, (void *) (uintptr_t) addr, length, PROT_READ | PROT_WRITE, MAP_SHARED);
}

void
panwrap_track_mmap(mali_ptr gpu_va, void *addr, size_t length,
                   int prot, int flags)
{
        struct panwrap_mapped_memory *mapped_mem = NULL;
        struct panwrap_allocated_memory *mem = NULL;

        /* Find the pending unmapped allocation for the memory */
        list_for_each_entry(struct panwrap_allocated_memory, pos, &allocations.node, node) {
                if (pos->gpu_va == gpu_va) {
                        mem = pos;
                        break;
                }
        }

        if (!mem) {
                panwrap_msg("Error: Untracked gpu memory " MALI_PTR_FMT " mapped to %p\n",
                            gpu_va, addr);
                panwrap_msg("\tprot = ");
                panwrap_log_decoded_flags(mmap_prot_flag_info, prot);
                panwrap_log_cont("\n");
                panwrap_msg("\tflags = ");
                panwrap_log_decoded_flags(mmap_flags_flag_info, flags);
                panwrap_log_cont("\n");

                return;
        }

        mapped_mem = malloc(sizeof(*mapped_mem));
        list_inithead(&mapped_mem->node);

        /* Try not to break other systems... there are so many configurations
         * of userspaces/kernels/architectures and none of them are compatible,
         * ugh. */

#define MEM_COOKIE_VA 0x41000

        if (mem->flags & BASE_MEM_SAME_VA && gpu_va == MEM_COOKIE_VA) {
                mapped_mem->gpu_va = (mali_ptr) (uintptr_t) addr;
        } else {
                mapped_mem->gpu_va = gpu_va;
        }

        mapped_mem->length = length;
        mapped_mem->addr = addr;
        mapped_mem->prot = prot;
        mapped_mem->flags = mem->flags;
        mapped_mem->allocation_number = mem->allocation_number;
        mapped_mem->touched = calloc(length, sizeof(bool));

        list_add(&mapped_mem->node, &mmaps.node);

        list_del(&mem->node);
        free(mem);

        panwrap_msg("va %d mapped to %" PRIx64 "\n", mapped_mem->allocation_number,
                    mapped_mem->gpu_va);

        /* Generate somewhat semantic name for the region */
        snprintf(mapped_mem->name, sizeof(mapped_mem->name),
                 "%s_%d",
                 mem->flags & BASE_MEM_PROT_GPU_EX ? "shader" : "memory",
                 mapped_mem->allocation_number);

        /* Map region itself */

        panwrap_log("uint32_t *%s = mmap64(NULL, %zd, %d, %d, fd, alloc_gpu_va_%d);\n\n",
                    mapped_mem->name, length, prot, flags, mapped_mem->allocation_number);

        panwrap_log("if (%s == MAP_FAILED) printf(\"Error mapping %s\\n\");\n\n",
                    mapped_mem->name, mapped_mem->name);
}

void
panwrap_track_munmap(void *addr)
{
        struct panwrap_mapped_memory *mapped_mem =
                panwrap_find_mapped_mem(addr);

        if (!mapped_mem) {
                panwrap_msg("Unknown mmap %p unmapped\n", addr);
                return;
        }

        list_del(&mapped_mem->node);

        free(mapped_mem);
}

struct panwrap_mapped_memory *panwrap_find_mapped_mem(void *addr)
{
        list_for_each_entry(struct panwrap_mapped_memory, pos, &mmaps.node, node) {
                if (pos->addr == addr)
                        return pos;
        }

        return NULL;
}

struct panwrap_mapped_memory *panwrap_find_mapped_mem_containing(void *addr)
{
        list_for_each_entry(struct panwrap_mapped_memory, pos, &mmaps.node, node) {
                if (addr >= pos->addr && addr < pos->addr + pos->length)
                        return pos;
        }

        return NULL;
}

struct panwrap_mapped_memory *panwrap_find_mapped_gpu_mem(mali_ptr addr)
{
        list_for_each_entry(struct panwrap_mapped_memory, pos, &mmaps.node, node) {
                if (pos->gpu_va == addr)
                        return pos;
        }

        return NULL;
}

struct panwrap_mapped_memory *panwrap_find_mapped_gpu_mem_containing(mali_ptr addr)
{
        list_for_each_entry(struct panwrap_mapped_memory, pos, &mmaps.node, node) {
                if (addr >= pos->gpu_va && addr < pos->gpu_va + pos->length)
                        return pos;
        }

        return NULL;
}

void
__attribute__((noreturn))
__panwrap_fetch_mem_err(const struct panwrap_mapped_memory *mem,
                        mali_ptr gpu_va, size_t size,
                        int line, const char *filename)
{
        panwrap_indent = 0;
        panwrap_msg("\n");

        panwrap_msg("INVALID GPU MEMORY ACCESS @"
                    MALI_PTR_FMT " - " MALI_PTR_FMT ":\n",
                    gpu_va, gpu_va + size);
        panwrap_msg("Occurred at line %d of %s\n", line, filename);

        if (mem) {
                panwrap_msg("Mapping information:\n");
                panwrap_indent++;
                panwrap_msg("CPU VA: %p - %p\n",
                            mem->addr, mem->addr + mem->length - 1);
                panwrap_msg("GPU VA: " MALI_PTR_FMT " - " MALI_PTR_FMT "\n",
                            mem->gpu_va,
                            (mali_ptr)(mem->gpu_va + mem->length - 1));
                panwrap_msg("Length: %zu bytes\n", mem->length);
                panwrap_indent--;

                if (!(mem->prot & BASE_MEM_PROT_CPU_RD))
                        panwrap_msg("Memory is only accessible from GPU\n");
                else
                        panwrap_msg("Access length was out of bounds\n");
        } else {
                panwrap_msg("GPU memory is not contained within known GPU VA mappings\n");

                list_for_each_entry(struct panwrap_mapped_memory, pos, &mmaps.node, node) {
                        panwrap_msg(MALI_PTR_FMT " (%p)\n", pos->gpu_va, pos->addr);
                }
        }

        panwrap_log_flush();
        abort();
}

PANLOADER_CONSTRUCTOR {
        list_inithead(&allocations.node);
        list_inithead(&mmaps.node);
}
