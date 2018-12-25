/*
 * © Copyright 2018 Alyssa Rosenzweig
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

#include <stdint.h>
#include "pan_slowfb.h"

#ifndef __ANDROID__

#include <X11/Xlib.h>
#include <X11/Xatom.h>

#include <stdio.h>
#include <stdlib.h>
#include <memory.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <X11/extensions/XShm.h>

Display *d;
Window w;
XImage *image;
GC gc;

struct slowfb_info slowfb_init(uint8_t *framebuffer, int width, int height)
{
        d = XOpenDisplay(NULL);
        int black = BlackPixel(d, DefaultScreen(d));
        w = XCreateSimpleWindow(d, DefaultRootWindow(d), 0, 0, 200, 100, 0, black, black);
        XSelectInput(d, w, StructureNotifyMask);

#if 0
        /* Set us up as a toolbar thingamajig so we can verify fullscreen operation under i3 */
        Atom window_type = XInternAtom(d, "_NET_WM_WINDOW_TYPE", 0);
        long value = XInternAtom(d, "_NET_WM_WINDOW_TYPE_TOOLBAR", 0);
        XChangeProperty(d, w, window_type, XA_ATOM, 32, PropModeReplace, (unsigned char *) &value, 1);
#endif

        XMapWindow(d, w);
        gc = XCreateGC(d, w, 0, NULL);

        for (;;) {
                XEvent e;
                XNextEvent(d, &e);

                if (e.type == MapNotify) break;
        }


        XShmSegmentInfo *shminfo = calloc(1, sizeof(XShmSegmentInfo));
        image = XShmCreateImage(d, DefaultVisual(d, 0), 24, ZPixmap, NULL, shminfo, width, height);
        shminfo->shmid = shmget(IPC_PRIVATE, image->bytes_per_line * image->height, IPC_CREAT | 0777);
        shminfo->shmaddr = image->data = shmat(shminfo->shmid, 0, 0);
        shminfo->readOnly = False;
        XShmAttach(d, shminfo);

        struct slowfb_info info = {
                .framebuffer = (uint8_t *) image->data,
                .stride = image->bytes_per_line
        };

        return info;
}
void
slowfb_update(uint8_t *framebuffer, int width, int height)
{
        XShmPutImage(d, w, gc, image, 0, 0, 0, 0, width, height, False);
        XFlush(d);
}

#else

struct slowfb_info slowfb_init(uint8_t *framebuffer, int width, int height)
{
}

void
slowfb_update(uint8_t *framebuffer, int width, int height)
{
}

#endif
