/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2024, Unikraft GmbH and The Unikraft Authors.
 * Licensed under the BSD-3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 */

#include <string.h>
#include <kvm/efi.h>
#include <uk/console.h>
#include <uk/console/driver.h>
#include <uk/console/gop.h>
#include <uk/boot/earlytab.h>
#include <uk/plat/common/bootinfo.h>
#include <uk/prio.h>
#include "format.h"

#if CONFIG_HAVE_PAGING && CONFIG_HAVE_PAGING_DIRECTMAP
#include <uk/arch/paging.h>
#define DIRECTMAP_AREA_START	0xffffff8000000000UL
#endif

/* Framebuffer metadata — physical address saved before ExitBootServices */
static __paddr_t fb_paddr;
static volatile __u32 *fb;
static uk_efi_uintn_t fb_size;
static __u32 fb_real_width;
static __u32 fb_height;
static __u32 fb_width;

/* Terminal metadata */
static __u32 row_count;
static __u32 col_count;
static __u32 cursor_row;
static __u32 cursor_col;

/* Font metadata */
extern __u8 char_width;
extern __u8 char_height;
extern __u8 font[256][16]; /* Default 8x16 font */

/* Formatting */
extern __u8 format_fg;
extern __u8 format_bg;

#define TAB_ALIGNMENT 8

/* Pixel format detected from GOP mode info */
static enum uk_efi_graphics_pixel_format pixel_format;

static inline __u32 gop_color(__u8 r, __u8 g, __u8 b)
{
	switch (pixel_format) {
	case UK_EFI_PIXEL_RED_GREEN_BLUE_RESERVED_8BIT_PER_COLOR:
		return ((__u32)r) | ((__u32)g << 8) | ((__u32)b << 16);
	case UK_EFI_PIXEL_BLUE_GREEN_RED_RESERVED_8BIT_PER_COLOR:
	default:
		return ((__u32)r << 16) | ((__u32)g << 8) | ((__u32)b);
	}
}

static __u32 gop_colors[8];

static void gop_init_colors(void)
{
	gop_colors[0] = gop_color(  0,   0,   0); /* black */
	gop_colors[1] = gop_color(192,   0,   0); /* red */
	gop_colors[2] = gop_color(  0, 192,   0); /* green */
	gop_colors[3] = gop_color(192, 192,   0); /* yellow */
	gop_colors[4] = gop_color(  0,   0, 192); /* blue */
	gop_colors[5] = gop_color(192,   0, 192); /* magenta */
	gop_colors[6] = gop_color(  0, 192, 192); /* cyan */
	gop_colors[7] = gop_color(192, 192, 192); /* white */
}

#define GOP_FG_COLOR(fg) (gop_colors[(fg) - 30])
#define GOP_BG_COLOR(bg) (gop_colors[(bg) - 40])

static void gop_console_clear(void)
{
	for (__u32 y = 0; y < fb_height; y++)
		for (__u32 x = 0; x < fb_width; x++)
			fb[x + y * fb_real_width] = gop_colors[0];
}

/* Draw formatted character at given row and col */
static void gop_putc_at(char c, __u32 col, __u32 row)
{
	__u32 fg_color = GOP_FG_COLOR(format_fg);
	__u32 bg_color = GOP_BG_COLOR(format_bg);

	for (__u32 y = 0; y < char_height; y++) {
		const __u8 slice = font[(__u8)c][y];

		for (__u32 x = 0; x < char_width; x++) {
			const __u32 index = (x + col * char_width)
				+ (y + row * char_height) * fb_real_width;

			if ((slice >> (char_width - x - 1)) & 1)
				fb[index] = fg_color;
			else
				fb[index] = bg_color;
		}
	}
}

/* Enter a new line, scroll if necessary */
static void gop_newline(void)
{
	if (cursor_row == row_count - 1) {
		/* scroll content up one line */
		memmove((void *)fb, (void *)&fb[char_height * fb_real_width],
			fb_size - char_height * fb_real_width *
			sizeof(__u32));
		/* clean last row */
		for (__u32 col = 0; col < col_count; col++)
			gop_putc_at(' ', col, row_count - 1);
	} else {
		cursor_row++;
	}
}

static void gop_putc(char c)
{
	switch (c) {
	case '\a':
		break;
	case '\b':
		if (cursor_col > 0) {
			cursor_col--;
		} else if (cursor_row > 0) {
			cursor_col = col_count - 1;
			cursor_row--;
		}
		break;
	case '\n':
		gop_newline();
		__fallthrough;
	case '\r':
		cursor_col = 0;
		break;
	case '\t':
		cursor_col += TAB_ALIGNMENT - (cursor_col % TAB_ALIGNMENT);
		if (cursor_col >= col_count) {
			cursor_col = 0;
			gop_newline();
		}
		break;
	default:
		if (!format_char(c))
			return;
		gop_putc_at(c, cursor_col, cursor_row);
		if (++cursor_col == col_count) {
			cursor_col = 0;
			gop_newline();
		}
		break;
	}
}

/* Print string of size len from buf */
static __ssz gop_console_out(struct uk_console *dev __unused,
			     const char *buf, __sz len)
{
	for (__sz i = 0; i < len; i++)
		gop_putc(buf[i]);
	return len;
}

/* Placeholder function. Input not implemented for GOP console */
static __ssz gop_console_in(struct uk_console *dev __unused,
			    char *buf __unused, __sz maxlen __unused)
{
	return 0;
}

static struct uk_console_ops gop_ops = {
	.out = gop_console_out,
	.in = gop_console_in
};

static struct uk_console gop_dev;

/* Called from efi.c BEFORE ExitBootServices — saves framebuffer metadata.
 * The fb pointer uses the EFI identity-mapped physical address so
 * gop_console_clear() works immediately.  After paging is set up,
 * gop_console_activate() converts fb to the directmap virtual address.
 */
uk_efi_status_t gop_init(struct uk_efi_boot_services *bs)
{
	struct uk_efi_graphics_output_proto *gop;
	struct uk_efi_graphics_output_mode_information *gop_info;
	uk_efi_uintn_t gop_info_sz;
	uk_efi_status_t status;

	status = bs->locate_protocol(UK_EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID,
				     __NULL, (void **)&gop);
	if (status != UK_EFI_SUCCESS)
		return status;

	gop_info = __NULL;
	status = gop->query_mode(gop, gop->mode->mode,
				 &gop_info_sz, &gop_info);
	if (status != UK_EFI_SUCCESS)
		return status;

	/* Save physical address for later directmap conversion */
	fb_paddr = (__paddr_t)gop->mode->frame_buffer_base;
	fb_size = gop->mode->frame_buffer_size;
	fb_width = gop->mode->info->horizontal_resolution;
	fb_height = gop->mode->info->vertical_resolution;
	fb_real_width = gop->mode->info->pixels_per_scanline;
	pixel_format = gop->mode->info->pixel_format;

	/* Initialize color table based on pixel format */
	gop_init_colors();

	/* Use identity-mapped address for now (works before paging) */
	fb = (volatile __u32 *)(unsigned long)fb_paddr;

	/* Initialize terminal metadata */
	row_count = fb_height / char_height;
	col_count = fb_width / char_width;
	cursor_row = 0;
	cursor_col = 0;

	gop_console_clear();

	return UK_EFI_SUCCESS;
}

/* Activate the GOP console — called after paging is set up so the
 * framebuffer is accessible via the directmap virtual address.
 */
static int gop_early_init(struct ukplat_bootinfo *bi __unused)
{
	if (!fb_paddr)
		return 0;

#if CONFIG_HAVE_PAGING && CONFIG_HAVE_PAGING_DIRECTMAP
	/* Convert framebuffer pointer to directmap virtual address */
	fb = (volatile __u32 *)(DIRECTMAP_AREA_START + fb_paddr);
#endif

	gop_console_clear();
	uk_console_init(&gop_dev, "GOP", &gop_ops, UK_CONSOLE_FLAG_STDOUT);
	uk_console_register(&gop_dev);

	return 0;
}

UK_BOOT_EARLYTAB_ENTRY(gop_early_init, UK_PRIO_AFTER(UK_PRIO_EARLIEST));
