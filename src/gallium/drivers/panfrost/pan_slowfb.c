/*
 * © Copyright 2018 Alyssa Rosenzweig
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

#include <stdint.h>
#include "pan_slowfb.h"

#define USE_SHM

#ifndef __ANDROID__

#include <X11/Xlib.h>

#ifdef USE_SHM
#include <stdio.h>
#include <stdlib.h>
#include <memory.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <X11/extensions/XShm.h>
#endif

Display *d;
Window w;
XImage *image;
GC gc;

struct slowfb_info slowfb_init(uint8_t *framebuffer, int width, int height) {
	d = XOpenDisplay(NULL);
	int black = BlackPixel(d, DefaultScreen(d));
	w = XCreateSimpleWindow(d, DefaultRootWindow(d), 0, 0, 200, 100, 0, black, black);
	XSelectInput(d, w, StructureNotifyMask);
	XMapWindow(d, w);
	gc = XCreateGC(d, w, 0, NULL);
	for (;;) {
		XEvent e;
		XNextEvent(d, &e);
		if (e.type == MapNotify) break;
	}


#ifdef USE_SHM
	XShmSegmentInfo *shminfo = calloc(1, sizeof(XShmSegmentInfo));
	image = XShmCreateImage(d, DefaultVisual(d, 0), 24, ZPixmap, NULL, shminfo, width, height);
	shminfo->shmid = shmget(IPC_PRIVATE, image->bytes_per_line * image->height, IPC_CREAT|0777);
	shminfo->shmaddr = image->data = shmat(shminfo->shmid, 0, 0);
	shminfo->readOnly = False;
	XShmAttach(d, shminfo);
#else
	image = XCreateImage(d, DefaultVisual(d, 0), 24, ZPixmap, 0, (char *) framebuffer, 2048, 1280, 32, 0);
#endif

	struct slowfb_info info = {
		.framebuffer = (uint8_t *) image->data,
		.stride = image->bytes_per_line
	};

	return info;
}
void slowfb_update(uint8_t *framebuffer, int width, int height) {
#ifdef USE_SHM
	XShmPutImage(d, w, gc, image, 0, 0, 0, 0, 2048, 1280, False);
	XFlush(d);
#else
	XPutImage(d, w, gc, image, 0, 0, 0, 0, 2048, 1280);
#endif
}

#else

struct slowfb_info slowfb_init(uint8_t *framebuffer, int width, int height) {
}

void slowfb_update(uint8_t *framebuffer, int width, int height) {
}

#endif
