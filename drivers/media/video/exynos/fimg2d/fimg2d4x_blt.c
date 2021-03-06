/* linux/drivers/media/video/samsung/fimg2d4x/fimg2d4x_blt.c
 *
 * Copyright (c) 2011 Samsung Electronics Co., Ltd.
 *	http://www.samsung.com/
 *
 * Samsung Graphics 2D driver
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
*/

#include <linux/slab.h>
#include <linux/interrupt.h>
#include <linux/sched.h>
#include <linux/uaccess.h>
#include <linux/atomic.h>
#include <linux/dma-mapping.h>
#include <asm/cacheflush.h>
#include <plat/sysmmu.h>
#ifdef CONFIG_PM_RUNTIME
#include <plat/devs.h>
#include <linux/pm_runtime.h>
#endif
#include "fimg2d.h"
#include "fimg2d_clk.h"
#include "fimg2d4x.h"
#include "fimg2d_ctx.h"
#include "fimg2d_helper.h"

#define CREATE_TRACE_POINTS
#include "fimg2d_trace.h"

#define BLIT_TIMEOUT	msecs_to_jiffies(2000)

static inline void fimg2d4x_blit_wait(struct fimg2d_control *info, struct fimg2d_bltcmd *cmd)
{
	if (!wait_event_timeout(info->wait_q, !atomic_read(&info->busy), BLIT_TIMEOUT)) {
		printk(KERN_ERR "[%s] blit wait timeout\n", __func__);
		fimg2d_dump_command(cmd);

		if (!fimg2d4x_blit_done_status(info))
			info->err = true; /* device error */
	}
}

static void fimg2d4x_pre_bitblt(struct fimg2d_control *info, struct fimg2d_bltcmd *cmd)
{
	/* TODO */
}

void fimg2d4x_bitblt(struct fimg2d_control *info)
{
	struct fimg2d_context *ctx;
	struct fimg2d_bltcmd *cmd;
	int ret;

	fimg2d_debug("enter blitter\n");

#ifdef CONFIG_PM_RUNTIME
	pm_runtime_get_sync(info->dev);
	fimg2d_debug("pm_runtime_get_sync\n");
#endif
	fimg2d_clk_on(info);

	while ((cmd = fimg2d_get_first_command(info))) {
		ctx = cmd->ctx;
		if (info->err) {
			printk(KERN_ERR "[%s] device error\n", __func__);
			goto blitend;
		}

		atomic_set(&info->busy, 1);

		ret = info->configure(info, cmd);
		if (ret)
			goto blitend;

		fimg2d4x_pre_bitblt(info, cmd);

		trace_fimg2d_bitblt_start(cmd->seq_no);
		/* start blit */
		info->run(info);
		fimg2d4x_blit_wait(info, cmd);
		trace_fimg2d_bitblt_end(cmd->seq_no);
blitend:
		fimg2d_del_command(info, cmd);

		/* wake up context */
		if (!atomic_read(&ctx->ncmd))
			wake_up_all(&ctx->wait_q);
	}

	atomic_set(&info->active, 0);

	fimg2d_clk_off(info);
#ifdef CONFIG_PM_RUNTIME
	pm_runtime_put_sync(info->dev);
	fimg2d_debug("pm_runtime_put_sync\n");
#endif

	fimg2d_debug("exit blitter\n");
}

static inline int is_opaque(enum color_format fmt)
{
	switch (fmt) {
	case CF_ARGB_8888:
	case CF_ARGB_1555:
	case CF_ARGB_4444:
		return 0;

	default:
		return 1;
	}
}

static int fast_op(struct fimg2d_bltcmd *cmd)
{
	int sa, da, ga;
	int fop = cmd->op;
	struct fimg2d_image *src, *msk, *dst;
	struct fimg2d_param *p = &cmd->param;

	src = &cmd->image[ISRC];
	msk = &cmd->image[IMSK];
	dst = &cmd->image[IDST];

	if (msk->addr.type)
		return fop;

	ga = p->g_alpha;
	da = is_opaque(dst->fmt) ? 0xff : 0;

	if (!src->addr.type)
		sa = (p->solid_color >> 24) & 0xff;
	else
		sa = is_opaque(src->fmt) ? 0xff : 0;

	switch (cmd->op) {
	case BLIT_OP_SRC_OVER:
		/* Sc + (1-Sa)*Dc = Sc */
		if (sa == 0xff && ga == 0xff)
			fop = BLIT_OP_SRC;
		break;
	case BLIT_OP_DST_OVER:
		/* (1-Da)*Sc + Dc = Dc */
		if (da == 0xff)
			fop = BLIT_OP_DST;	/* nop */
		break;
	case BLIT_OP_SRC_IN:
		/* Da*Sc = Sc */
		if (da == 0xff)
			fop = BLIT_OP_SRC;
		break;
	case BLIT_OP_DST_IN:
		/* Sa*Dc = Dc */
		if (sa == 0xff && ga == 0xff)
			fop = BLIT_OP_DST;	/* nop */
		break;
	case BLIT_OP_SRC_OUT:
		/* (1-Da)*Sc = 0 */
		if (da == 0xff)
			fop = BLIT_OP_CLR;
		break;
	case BLIT_OP_DST_OUT:
		/* (1-Sa)*Dc = 0 */
		if (sa == 0xff && ga == 0xff)
			fop = BLIT_OP_CLR;
		break;
	case BLIT_OP_SRC_ATOP:
		/* Da*Sc + (1-Sa)*Dc = Sc */
		if (sa == 0xff && da == 0xff && ga == 0xff)
			fop = BLIT_OP_SRC;
		break;
	case BLIT_OP_DST_ATOP:
		/* (1-Da)*Sc + Sa*Dc = Dc */
		if (sa == 0xff && da == 0xff && ga == 0xff)
			fop = BLIT_OP_DST;	/* nop */
		break;
	default:
		break;
	}

	if (fop == BLIT_OP_SRC && !src->addr.type && ga == 0xff)
		fop = BLIT_OP_SOLID_FILL;

	return fop;
}

static int fimg2d4x_configure(struct fimg2d_control *info,
				struct fimg2d_bltcmd *cmd)
{
	int op;
	enum image_sel srcsel, dstsel;
	struct fimg2d_param *p = &cmd->param;
	struct fimg2d_image *src, *msk, *dst;

	fimg2d_debug("ctx %p seq_no(%u)\n", cmd->ctx, cmd->seq_no);

	src = &cmd->image[ISRC];
	msk = &cmd->image[IMSK];
	dst = &cmd->image[IDST];

	/* TODO: batch blit */
	fimg2d4x_reset(info);

	/* src and dst select */
	srcsel = dstsel = IMG_MEMORY;

	op = fast_op(cmd);

	switch (op) {
	case BLIT_OP_SOLID_FILL:
		srcsel = dstsel = IMG_FGCOLOR;
		fimg2d4x_set_fgcolor(info, p->solid_color);
		break;
	case BLIT_OP_CLR:
		srcsel = dstsel = IMG_FGCOLOR;
		fimg2d4x_set_color_fill(info, 0);
		break;
	case BLIT_OP_DST:
		return -1;	/* nop */
	default:
		if (!src->addr.type) {
			srcsel = IMG_FGCOLOR;
			fimg2d4x_set_fgcolor(info, p->solid_color);
		}

		if (op == BLIT_OP_SRC)
			dstsel = IMG_FGCOLOR;

		fimg2d4x_enable_alpha(info, p->g_alpha);
		fimg2d4x_set_alpha_composite(info, op, p->g_alpha);
		if (p->premult == NON_PREMULTIPLIED)
			fimg2d4x_set_premultiplied(info);
		break;
	}

	fimg2d4x_set_src_type(info, srcsel);
	fimg2d4x_set_dst_type(info, dstsel);

	/* src */
	if (src->addr.type) {
		fimg2d4x_set_src_image(info, src, cmd->dma[ISRC]);
		fimg2d4x_set_src_rect(info, &src->rect);
		fimg2d4x_set_src_repeat(info, &p->repeat);
		if (p->scaling.mode)
			fimg2d4x_set_src_scaling(info, &p->scaling, &p->repeat);
	}

	/* msk */
	if (msk->addr.type) {
		fimg2d4x_enable_msk(info);
		fimg2d4x_set_msk_image(info, msk, cmd->dma[IMSK]);
		fimg2d4x_set_msk_rect(info, &msk->rect);
		fimg2d4x_set_msk_repeat(info, &p->repeat);
		if (p->scaling.mode)
			fimg2d4x_set_msk_scaling(info, &p->scaling, &p->repeat);
	}

	/* dst */
	if (dst->addr.type) {
		fimg2d4x_set_dst_image(info, dst, cmd->dma[IDST]);
		fimg2d4x_set_dst_rect(info, &dst->rect);
		if (p->clipping.enable)
			fimg2d4x_enable_clipping(info, &p->clipping);
	}

	/* bluescreen */
	if (p->bluscr.mode)
		fimg2d4x_set_bluescreen(info, &p->bluscr);

	/* rotation */
	if (p->rotate)
		fimg2d4x_set_rotation(info, p->rotate);

	/* dithering */
	if (p->dither)
		fimg2d4x_enable_dithering(info);

	return 0;
}

static void fimg2d4x_run(struct fimg2d_control *info)
{
	fimg2d_debug("start blit\n");
	fimg2d4x_enable_irq(info);
	fimg2d4x_clear_irq(info);
	fimg2d4x_start_blit(info);
}

static void fimg2d4x_stop(struct fimg2d_control *info)
{
	if (fimg2d4x_is_blit_done(info)) {
		fimg2d_debug("blit done\n");
		fimg2d4x_disable_irq(info);
		fimg2d4x_clear_irq(info);
		atomic_set(&info->busy, 0);
		wake_up(&info->wait_q);
	}
}

static void fimg2d4x_dump(struct fimg2d_control *info)
{
	fimg2d4x_dump_regs(info);
}

int fimg2d_register_ops(struct fimg2d_control *info)
{
	info->blit = fimg2d4x_bitblt;
	info->configure = fimg2d4x_configure;
	info->run = fimg2d4x_run;
	info->dump = fimg2d4x_dump;
	info->stop = fimg2d4x_stop;

	return 0;
}
