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
#include <stdbool.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <ctype.h>
#include "panwrap.h"

static FILE *log_output;

short panwrap_indent = 0;

void
panwrap_log_decoded_flags(const struct panwrap_flag_info *flag_info,
                          u64 flags)
{
        bool decodable_flags_found = false;

        for (int i = 0; flag_info[i].name; i++) {
                if ((flags & flag_info[i].flag) != flag_info[i].flag)
                        continue;

                if (!decodable_flags_found) {
                        decodable_flags_found = true;
                } else {
                        panwrap_log_cont(" | ");
                }

                panwrap_log_cont("%s", flag_info[i].name);

                flags &= ~flag_info[i].flag;
        }

        if (decodable_flags_found) {
                if (flags)
                        panwrap_log_cont(" | 0x%" PRIx64, flags);
        } else {
                panwrap_log_cont("0x%" PRIx64, flags);
        }
}

/**
 * Grab the location of a symbol from the system's libc instead of our
 * preloaded one
 */
void *
__rd_dlsym_helper(const char *name)
{
        static void *libc_dl;
        void *func;

        if (!libc_dl)
                libc_dl = dlopen("libc.so", RTLD_LAZY);

        if (!libc_dl)
                libc_dl = dlopen("libc.so.6", RTLD_LAZY);

        if (!libc_dl) {
                fprintf(stderr, "Failed to dlopen libc: %s\n", dlerror());
                exit(-1);
        }

        func = dlsym(libc_dl, name);

        if (!func) {
                fprintf(stderr, "Failed to find %s: %s\n", name, dlerror());
                exit(-1);
        }

        return func;
}

void
panwrap_log_empty()
{
        for (int i = 0; i < panwrap_indent; i++) {
                fputs("  ", log_output);
        }
}

void
panwrap_log_typed(enum panwrap_log_type type, const char *format, ...)
{
        va_list ap;

        panwrap_log_empty();

        if (type == PANWRAP_MESSAGE)
                fputs("// ", log_output);
        else if (type == PANWRAP_PROPERTY)
                fputs(".", log_output);

        va_start(ap, format);
        vfprintf(log_output, format, ap);
        va_end(ap);

        if (type == PANWRAP_PROPERTY)
                fputs(",\n", log_output);
}

/* Eventually this function might do more */
void
panwrap_log_cont(const char *format, ...)
{
        va_list ap;

        va_start(ap, format);
        vfprintf(log_output, format, ap);
        va_end(ap);
}

void
panwrap_log_flush()
{
        fflush(log_output);
}

PANLOADER_CONSTRUCTOR {
        log_output = stdout;
}
