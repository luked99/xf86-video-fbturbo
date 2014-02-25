/*
 * Copyright © 2013 Siarhei Siamashka <siarhei.siamashka@gmail.com>
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
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include "fb_copyarea.h"
#include "xf86.h"

/*
 * HACK: non-standard ioctl, which provides access to fb_copyarea accelerated
 * function in the kernel. It just accepts the standard fb_copyarea structure
 * defined in "linux/fb.h" header
 */
#ifndef FBIOCOPYAREA
#define FBIOCOPYAREA		_IOW('z', 0x21, struct fb_copyarea)
#endif

#ifndef FBIOFILLRECT
#define FBIOFILLRECT            _IOW('z', 0x22, struct fb_fillrect)
#endif


/* Fallback to CPU when handling less than COPYAREA_BLT_SIZE_THRESHOLD pixels */
#define COPYAREA_BLT_SIZE_THRESHOLD 90
#define COPYAREA_FILL_SIZE_THRESHOLD (1<<24)

static int do_log = 0;
#define LOG(...) if (do_log) { ErrorF(__VA_ARGS__); }

static int fb_fill(void *self,
                   uint32_t *dst_bits,
                   int dst_stride,
                   int dst_bpp,
                   int dst_x,
                   int dst_y,
                   int w,
                   int h,
                   uint32_t color);

/*
 * Check whether the FBIOCOPYAREA ioctl is supported by requesting to do
 * a copy of 1x1 rectangle in the top left corner to itself
 */
static Bool fb_has_copyarea(int fd)
{
    struct fb_copyarea copyarea;
    copyarea.sx = 0;
    copyarea.sy = 0;
    copyarea.dx = 0;
    copyarea.dy = 0;
    copyarea.width = 1;
    copyarea.height = 1;
    return ioctl(fd, FBIOCOPYAREA, &copyarea) == 0;
}

/*
 * A bit nasty, check if fillrect is supported by filling a
 * 0x0 rectangle.
 */

static Bool fb_has_fillrect(int fd)
{
    struct fb_fillrect rect;
    int ret;
    rect.dx = 0;
    rect.dy = 0;
    rect.width = 0;
    rect.height = 0;
    rect.rop = ROP_COPY;
    ret = ioctl(fd, FBIOFILLRECT, &rect) == 0;
    LOG("%s: ioctl 0x%x => %d\n", __func__, FBIOFILLRECT, ret);
    return ret;
}

fb_copyarea_t *fb_copyarea_init(const char *device, void *xserver_fbmem)
{
    fb_copyarea_t *ctx = calloc(sizeof(fb_copyarea_t), 1);
    struct fb_var_screeninfo fb_var;
    struct fb_fix_screeninfo fb_fix;
    struct fb_copyarea copyarea;
    Bool has_fillrect;

    int tmp, version;
    int gfx_layer_size;
    int ovl_layer_size;

    /* use /dev/fb0 by default */
    if (!device)
        device = "/dev/fb0";

    ctx->fd = open(device, O_RDWR);
    if (ctx->fd < 0) {
        close(ctx->fd);
        free(ctx);
        return NULL;
    }

    if (!fb_has_copyarea(ctx->fd)) {
        close(ctx->fd);
        free(ctx);
        return NULL;
    }

    has_fillrect = fb_has_fillrect(ctx->fd);

    if (ioctl(ctx->fd, FBIOGET_VSCREENINFO, &fb_var) < 0 ||
        ioctl(ctx->fd, FBIOGET_FSCREENINFO, &fb_fix) < 0)
    {
        close(ctx->fd);
        free(ctx);
        return NULL;
    }

    if (fb_fix.line_length % 4) {
        close(ctx->fd);
        free(ctx);
        return NULL;
    }

    /* store the already existing mapping done by xserver */
    ctx->xserver_fbmem = xserver_fbmem;

    ctx->xres = fb_var.xres;
    ctx->yres = fb_var.yres;
    ctx->bits_per_pixel = fb_var.bits_per_pixel;
    ctx->framebuffer_paddr = fb_fix.smem_start;
    ctx->framebuffer_size = fb_fix.smem_len;
    ctx->framebuffer_height = ctx->framebuffer_size /
                              (ctx->xres * ctx->bits_per_pixel / 8);
    ctx->gfx_layer_size = ctx->xres * ctx->yres * fb_var.bits_per_pixel / 8;
    ctx->framebuffer_stride = fb_fix.line_length / 4;

    if (ctx->framebuffer_size < ctx->gfx_layer_size) {
        close(ctx->fd);
        free(ctx);
        return NULL;
    }

    if (ctx->xserver_fbmem) {
        /* use already existing mapping */
        ctx->framebuffer_addr = ctx->xserver_fbmem;
    }
    else {
        /* mmap framebuffer memory */
        ctx->framebuffer_addr = (uint8_t *)mmap(0, ctx->framebuffer_size,
                                                PROT_READ | PROT_WRITE,
                                                MAP_SHARED, ctx->fd, 0);
        if (ctx->framebuffer_addr == MAP_FAILED) {
            close(ctx->fd);
            free(ctx);
            return NULL;
        }
    }

    ctx->blt2d.self = ctx;
    ctx->blt2d.overlapped_blt = fb_copyarea_blt;
    if (has_fillrect)
        ctx->blt2d.fill = fb_fill;

    ctx->dma_fill_threshold = COPYAREA_FILL_SIZE_THRESHOLD;
    ctx->dma_copy_threshold = COPYAREA_BLT_SIZE_THRESHOLD;

    return ctx;
}

void fb_copyarea_close(fb_copyarea_t *ctx)
{
    close(ctx->fd);
    free(ctx);
}

static inline int try_fallback_blt(void               *self,
                                   uint32_t           *src_bits,
                                   uint32_t           *dst_bits,
                                   int                 src_stride,
                                   int                 dst_stride,
                                   int                 src_bpp,
                                   int                 dst_bpp,
                                   int                 src_x,
                                   int                 src_y,
                                   int                 dst_x,
                                   int                 dst_y,
                                   int                 w,
                                   int                 h)
{
    fb_copyarea_t *ctx = (fb_copyarea_t *)self;
    if (ctx->fallback_blt2d)
        return ctx->fallback_blt2d->overlapped_blt(ctx->fallback_blt2d->self,
                                                   src_bits, dst_bits,
                                                   src_stride, dst_stride,
                                                   src_bpp, dst_bpp,
                                                   src_x, src_y,
                                                   dst_x, dst_y, w, h);
    return 0;
}

#define FALLBACK_BLT() try_fallback_blt(self, src_bits,        \
                                        dst_bits, src_stride,  \
                                        dst_stride, src_bpp,   \
                                        dst_bpp, src_x, src_y, \
                                        dst_x, dst_y, w, h);

int fb_copyarea_blt(void               *self,
                    uint32_t           *src_bits,
                    uint32_t           *dst_bits,
                    int                 src_stride,
                    int                 dst_stride,
                    int                 src_bpp,
                    int                 dst_bpp,
                    int                 src_x,
                    int                 src_y,
                    int                 dst_x,
                    int                 dst_y,
                    int                 w,
                    int                 h)
{
    fb_copyarea_t *ctx = (fb_copyarea_t *)self;
    struct fb_copyarea copyarea;
    uint32_t *framebuffer_addr = (uint32_t *)ctx->framebuffer_addr;
    int ret;

    /* Zero size blit, nothing to do */
    if (w <= 0 || h <= 0)
        return 1;

    if (src_bpp != dst_bpp || src_bpp != ctx->bits_per_pixel ||
        src_stride != dst_stride || src_stride != ctx->framebuffer_stride ||
        src_bits != dst_bits || src_bits != framebuffer_addr) {
        return FALLBACK_BLT();
    }

    if (w * h < ctx->dma_copy_threshold)
        return FALLBACK_BLT();

    copyarea.sx = src_x;
    copyarea.sy = src_y;
    copyarea.dx = dst_x;
    copyarea.dy = dst_y;
    copyarea.width = w;
    copyarea.height = h;

    LOG("%s: src %d,%d dst %d,%d size %dx%d\n",
        __func__,
        src_x, src_y, dst_x, dst_y, w, h);
    ret = ioctl(ctx->fd, FBIOCOPYAREA, &copyarea);

    LOG("%s: done\n");

    return ret == 0;
}

static int fb_fill(void *self,
                   uint32_t *dst_bits,
                   int dst_stride,
                   int dst_bpp,
                   int dst_x,
                   int dst_y,
                   int w,
                   int h,
                   uint32_t color)
{
    fb_copyarea_t *ctx = self;
    struct fb_fillrect fillrect;
    uint32_t *framebuffer_addr = (uint32_t *)ctx->framebuffer_addr;
    int rc;

    /* Zero size blit, nothing to do */
    if (w <= 0 || h <= 0)
        return 1;

    if ((dst_bits != framebuffer_addr) ||
        (w * h < ctx->dma_fill_threshold))
    {
        return 0;
    }

    fillrect.dx = dst_x;
    fillrect.dy = dst_y;
    fillrect.width = w;
    fillrect.height = h;
    fillrect.rop = ROP_COPY;
    fillrect.color = color;
    LOG("%s: %d,%d %dx%d bpp %d\n", __func__, dst_x, dst_y, w, h, dst_bpp);

    rc = ioctl(ctx->fd, FBIOFILLRECT, &fillrect);
    if (rc)
        LOG("%s: ioctl failed: %s\n", __func__, strerror(errno));

    LOG("%s: done ioctl fill\n", __func__);
    return rc == 0 ? 1 : 0;
}

void fb_copyarea_set_dma_fill_threshold(fb_copyarea_t *self, unsigned long thresh)
{
    self->dma_fill_threshold = thresh;
}

void fb_copyarea_set_dma_copy_threshold(fb_copyarea_t *self, unsigned long thresh)
{
    self->dma_copy_threshold = thresh;
}
