/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    guix_app.c
 * @brief   Hand-coded two-screen GUIX demo UI (issue #55).  See guix_app.h.
 */
#include "guix_app.h"
#include "guix_camera.h"         /* camera preview screen: pixmap, events, off() */
#include "ltdc_display.h"        /* LTDC_LCD_WIDTH / HEIGHT */

#include "gx_api.h"

#include <stddef.h>

/* Built-in GUIX font (compiled from lib/guix/common/src/gx_system_font_8bpp.c). */
extern GX_CONST GX_FONT _gx_system_font_8bpp;

/* ---- Theme: colour + font tables (resource IDs are indices into these). ----
 * For a 565rgb display GUIX stores NATIVE colours in the table -- the value is
 * read back unchanged by _gx_context_color_get() and written straight to the
 * frame buffer by the 16bpp driver as (USHORT)colour.  So the entries must be
 * packed RGB565, not 0x00RRGGBB.  Build them from 8-bit components at compile
 * time. */
#define RGB565(r, g, b) ((GX_COLOR)((((r) >> 3) << 11) | (((g) >> 2) << 5) | \
                                    ((b) >> 3)))
enum {
	C_BG = 0, C_TEXT, C_BTN, C_BTN_TEXT, C_BG2, C_ACCENT
};
static GX_CONST GX_COLOR guix_color_table[] = {
	RGB565(0x10, 0x20, 0x38),   /* C_BG       dark blue            */
	RGB565(0xff, 0xff, 0xff),   /* C_TEXT     white                */
	RGB565(0x29, 0x5f, 0xa6),   /* C_BTN      steel-blue face      */
	RGB565(0xff, 0xff, 0xff),   /* C_BTN_TEXT white                */
	RGB565(0x20, 0x38, 0x28),   /* C_BG2      screen-2 background  */
	RGB565(0x40, 0xc0, 0xff),   /* C_ACCENT   pressed-button face  */
};

enum { F_TEXT = 0 };
static GX_CONST GX_FONT *guix_font_table[] = {
	&_gx_system_font_8bpp,        /* F_TEXT */
};

/* ---- Widget IDs. ---- */
#define ID_SCREEN0      0x10u
#define ID_SCREEN1      0x11u
#define ID_SCREEN2      0x12u
#define ID_BTN_NEXT     0x20u
#define ID_BTN_BACK     0x21u
#define ID_BTN_CAM_BACK 0x22u
#define ID_ICON         0x30u
#define ID_CAM_ICON     0x31u

/* ---- Demo pixelmap (exercises the DMA2D pixelmap_draw path, #55 full DMA2D).
 * A small RGB565 image generated at startup into MPU non-cacheable SDRAM (so it
 * is a DMA2D-coherent source, like a camera frame in #56) and shown via a
 * GX_ICON on screen 0.  Resource IDs into the pixelmap table are direct indices;
 * index 0 is left NULL and the image is at index 1 (PIX_DEMO). */
#define DEMO_W      96
#define DEMO_H      72
#define PIX_DEMO    1u
#define PIX_CAMERA  2u    /* camera live-preview view buffer (guix_camera.c) */
static uint16_t guix_demo_img[DEMO_W * DEMO_H]
	__attribute__((aligned(32), section(".sdram")));
static GX_PIXELMAP  guix_demo_pixmap;
static GX_PIXELMAP *guix_pixelmap_table[3];

/* ---- Static widget instances. ---- */
static GX_WINDOW      screen0, screen1, screen2;
static GX_PROMPT      title0, sub0, title1, sub1, title2;
static GX_TEXT_BUTTON btn_next, btn_back, btn_cam_back;
static GX_ICON        demo_icon, cam_icon;

/* Build the demo image: eight vertical RGB565 colour bars inside a white frame.
   M2M pixelmap draw copies RGB565 verbatim, so the on-screen result equals this
   exactly -- any colour/orientation error would be a DMA2D geometry bug. */
static void guix_make_demo_image(void)
{
	static const uint16_t bars[8] = {
		0xF800u, 0xFFE0u, 0x07E0u, 0x07FFu,    /* red yellow green cyan   */
		0x001Fu, 0xF81Fu, 0xFFFFu, 0x0000u,    /* blue magenta white black */
	};
	int x, y;

	for (y = 0; y < DEMO_H; y++) {
		for (x = 0; x < DEMO_W; x++) {
			uint16_t c = bars[(x * 8) / DEMO_W];

			if (x < 2 || x >= DEMO_W - 2 || y < 2 || y >= DEMO_H - 2)
				c = 0xFFFFu;                   /* 2-px white frame */
			guix_demo_img[y * DEMO_W + x] = c;
		}
	}
	guix_demo_pixmap.gx_pixelmap_format        = GX_COLOR_FORMAT_565RGB;
	guix_demo_pixmap.gx_pixelmap_flags         = 0;
	guix_demo_pixmap.gx_pixelmap_data          = (GX_UBYTE *)guix_demo_img;
	guix_demo_pixmap.gx_pixelmap_data_size     = sizeof guix_demo_img;
	guix_demo_pixmap.gx_pixelmap_aux_data      = GX_NULL;
	guix_demo_pixmap.gx_pixelmap_aux_data_size = 0;
	guix_demo_pixmap.gx_pixelmap_width         = DEMO_W;
	guix_demo_pixmap.gx_pixelmap_height        = DEMO_H;
	guix_pixelmap_table[0] = GX_NULL;
	guix_pixelmap_table[1] = &guix_demo_pixmap;
}

static void rect_set(GX_RECTANGLE *r, INT l, INT t, INT rt, INT b)
{
	r->gx_rectangle_left   = (GX_VALUE)l;
	r->gx_rectangle_top    = (GX_VALUE)t;
	r->gx_rectangle_right  = (GX_VALUE)rt;
	r->gx_rectangle_bottom = (GX_VALUE)b;
}

static void guix_app_select_screen(int n)
{
	gx_widget_hide(&screen0);
	gx_widget_hide(&screen1);
	gx_widget_hide(&screen2);
	switch (n) {
	case 1:  gx_widget_show(&screen1); break;
	case 2:  gx_widget_show(&screen2); break;
	default: gx_widget_show(&screen0); break;
	}
}

/*
 * Screen windows intercept their button's CLICKED notification.  GUIX delivers a
 * child notification to the parent with the event type RE-ENCODED as
 * GX_SIGNAL(sender_id, base_event) -- not the bare base event -- so the match is
 * against GX_SIGNAL(button_id, GX_EVENT_CLICKED) (the GUIX Studio idiom).
 * Everything else defers to the default window handler.
 */
static UINT screen0_event(GX_WIDGET *widget, GX_EVENT *event_ptr)
{
	if (event_ptr->gx_event_type == GX_SIGNAL(ID_BTN_NEXT, GX_EVENT_CLICKED)) {
		guix_app_select_screen(1);
		return GX_SUCCESS;
	}
	return gx_window_event_process((GX_WINDOW *)widget, event_ptr);
}

static UINT screen1_event(GX_WIDGET *widget, GX_EVENT *event_ptr)
{
	if (event_ptr->gx_event_type == GX_SIGNAL(ID_BTN_BACK, GX_EVENT_CLICKED)) {
		guix_app_select_screen(0);
		return GX_SUCCESS;
	}
	return gx_window_event_process((GX_WINDOW *)widget, event_ptr);
}

/* Camera screen: the Back button stops the live preview and returns to screen 0
   (guix_camera_off does a bounded drain only, safe to call from the GUIX thread). */
static UINT screen2_event(GX_WIDGET *widget, GX_EVENT *event_ptr)
{
	if (event_ptr->gx_event_type ==
	    GX_SIGNAL(ID_BTN_CAM_BACK, GX_EVENT_CLICKED)) {
		(void)guix_camera_off();
		return GX_SUCCESS;
	}
	return gx_window_event_process((GX_WINDOW *)widget, event_ptr);
}

/*
 * Root event handler (#56): catches the camera-preview control events the
 * producer/shell post via guix_post_root_event() and runs them ON the GUIX
 * thread -- screen show/hide and dirty marking must not be done from another
 * thread.  GX_EVENT_CAMERA_FRAME repaints the live image (the sink already
 * copied it into the view buffer); SHOW/HIDE switch screens.  Everything else
 * defers to the default root processing so normal pen/draw routing is intact
 * (the demo buttons notify their own screen window, not the root, so this
 * override does not affect the Next/Back flow).
 */
static UINT guix_root_event(GX_WIDGET *widget, GX_EVENT *event_ptr)
{
	switch (event_ptr->gx_event_type) {
	case GX_EVENT_CAMERA_FRAME:
		guix_camera_mark_drawn();
		gx_system_dirty_mark((GX_WIDGET *)&cam_icon);
		return GX_SUCCESS;
	case GX_EVENT_CAMERA_SHOW:
		guix_app_select_screen(2);
		return GX_SUCCESS;
	case GX_EVENT_CAMERA_HIDE:
		guix_app_select_screen(0);
		return GX_SUCCESS;
	default:
		return gx_window_root_event_process((GX_WINDOW_ROOT *)widget,
		                                    event_ptr);
	}
}

#define PROMPT_STYLE  (GX_STYLE_TRANSPARENT | GX_STYLE_ENABLED | \
                       GX_STYLE_HALIGN_CENTER | GX_STYLE_VALIGN_CENTER)
#define BUTTON_STYLE  (GX_STYLE_BORDER_RAISED | GX_STYLE_ENABLED | \
                       GX_STYLE_HALIGN_CENTER | GX_STYLE_VALIGN_CENTER)

UINT guix_app_create(GX_DISPLAY *display, GX_WINDOW_ROOT *root)
{
	GX_RECTANGLE size;
	UINT status;

	gx_display_color_table_set(display, (GX_COLOR *)guix_color_table,
	    (INT)(sizeof guix_color_table / sizeof guix_color_table[0]));
	gx_display_font_table_set(display, (GX_FONT **)guix_font_table,
	    (UINT)(sizeof guix_font_table / sizeof guix_font_table[0]));
	guix_make_demo_image();
	guix_pixelmap_table[PIX_CAMERA] = guix_camera_pixmap();   /* live preview (#56) */
	gx_display_pixelmap_table_set(display, guix_pixelmap_table,
	    (UINT)(sizeof guix_pixelmap_table / sizeof guix_pixelmap_table[0]));

	/* ---------------- Screen 0 ---------------- */
	rect_set(&size, 0, 0, (INT)LTDC_LCD_WIDTH - 1, (INT)LTDC_LCD_HEIGHT - 1);
	status = gx_window_create(&screen0, "screen0", root,
	                          GX_STYLE_BORDER_NONE | GX_STYLE_ENABLED,
	                          ID_SCREEN0, &size);
	if (status != GX_SUCCESS)
		return status;
	screen0.gx_widget_normal_fill_color      = C_BG;
	screen0.gx_widget_event_process_function = screen0_event;

	rect_set(&size, 20, 50, (INT)LTDC_LCD_WIDTH - 21, 90);
	status = gx_prompt_create(&title0, "title0", &screen0, 0, PROMPT_STYLE,
	                          0, &size);
	if (status != GX_SUCCESS)
		return status;
	gx_prompt_text_set(&title0, "GUIX on STM32F746  (#55)");
	gx_prompt_font_set(&title0, F_TEXT);
	gx_prompt_text_color_set(&title0, C_TEXT, C_TEXT, C_TEXT);

	rect_set(&size, 20, 110, (INT)LTDC_LCD_WIDTH - 21, 150);
	status = gx_prompt_create(&sub0, "sub0", &screen0, 0, PROMPT_STYLE, 0, &size);
	if (status != GX_SUCCESS)
		return status;
	gx_prompt_text_set(&sub0, "LTDC + DMA2D + FT5336 touch");
	gx_prompt_font_set(&sub0, F_TEXT);
	gx_prompt_text_color_set(&sub0, C_TEXT, C_TEXT, C_TEXT);

	/* Demo image (DMA2D pixelmap_draw): a GX_ICON drawing the RGB565 pixelmap.
	   GX_STYLE_BORDER_NONE (not ENABLED) so it is not selectable and never
	   intercepts touches meant for the button. */
	status = gx_icon_create(&demo_icon, "demo", &screen0, PIX_DEMO,
	                        GX_STYLE_BORDER_NONE, ID_ICON, 30, 156);
	if (status != GX_SUCCESS)
		return status;

	rect_set(&size, 300, 200, (INT)LTDC_LCD_WIDTH - 31, 245);
	status = gx_text_button_create(&btn_next, "next", &screen0, 0, BUTTON_STYLE,
	                               ID_BTN_NEXT, &size);
	if (status != GX_SUCCESS)
		return status;
	gx_text_button_text_set(&btn_next, "Next  >");
	gx_text_button_font_set(&btn_next, F_TEXT);
	gx_text_button_text_color_set(&btn_next, C_BTN_TEXT, C_BTN_TEXT, C_BTN_TEXT);
	btn_next.gx_widget_normal_fill_color   = C_BTN;
	btn_next.gx_widget_selected_fill_color = C_ACCENT;

	/* ---------------- Screen 1 ---------------- */
	rect_set(&size, 0, 0, (INT)LTDC_LCD_WIDTH - 1, (INT)LTDC_LCD_HEIGHT - 1);
	status = gx_window_create(&screen1, "screen1", root,
	                          GX_STYLE_BORDER_NONE | GX_STYLE_ENABLED,
	                          ID_SCREEN1, &size);
	if (status != GX_SUCCESS)
		return status;
	screen1.gx_widget_normal_fill_color      = C_BG2;
	screen1.gx_widget_event_process_function = screen1_event;

	rect_set(&size, 20, 50, (INT)LTDC_LCD_WIDTH - 21, 90);
	status = gx_prompt_create(&title1, "title1", &screen1, 0, PROMPT_STYLE,
	                          0, &size);
	if (status != GX_SUCCESS)
		return status;
	gx_prompt_text_set(&title1, "Screen 2");
	gx_prompt_font_set(&title1, F_TEXT);
	gx_prompt_text_color_set(&title1, C_TEXT, C_TEXT, C_TEXT);

	rect_set(&size, 20, 110, (INT)LTDC_LCD_WIDTH - 21, 150);
	status = gx_prompt_create(&sub1, "sub1", &screen1, 0, PROMPT_STYLE, 0, &size);
	if (status != GX_SUCCESS)
		return status;
	gx_prompt_text_set(&sub1, "Eclipse ThreadX GUIX v6.5.1");
	gx_prompt_font_set(&sub1, F_TEXT);
	gx_prompt_text_color_set(&sub1, C_TEXT, C_TEXT, C_TEXT);

	rect_set(&size, 30, 200, 179, 245);
	status = gx_text_button_create(&btn_back, "back", &screen1, 0, BUTTON_STYLE,
	                               ID_BTN_BACK, &size);
	if (status != GX_SUCCESS)
		return status;
	gx_text_button_text_set(&btn_back, "<  Back");
	gx_text_button_font_set(&btn_back, F_TEXT);
	gx_text_button_text_color_set(&btn_back, C_BTN_TEXT, C_BTN_TEXT, C_BTN_TEXT);
	btn_back.gx_widget_normal_fill_color   = C_BTN;
	btn_back.gx_widget_selected_fill_color = C_ACCENT;

	/* ---------------- Screen 2 (camera live preview, #56) ---------------- */
	rect_set(&size, 0, 0, (INT)LTDC_LCD_WIDTH - 1, (INT)LTDC_LCD_HEIGHT - 1);
	status = gx_window_create(&screen2, "screen2", root,
	                          GX_STYLE_BORDER_NONE | GX_STYLE_ENABLED,
	                          ID_SCREEN2, &size);
	if (status != GX_SUCCESS)
		return status;
	screen2.gx_widget_normal_fill_color      = C_BG;
	screen2.gx_widget_event_process_function = screen2_event;

	/* QVGA camera image, drawn native 1:1 (no scaling) centred on 480x272.
	   BORDER_NONE (not ENABLED) so it is not selectable and never steals the
	   touch meant for the Back button.  Its pixelmap is the view buffer that the
	   preview sink refreshes in place (guix_camera.c). */
	status = gx_icon_create(&cam_icon, "cam", &screen2, PIX_CAMERA,
	                        GX_STYLE_BORDER_NONE, ID_CAM_ICON,
	                        CAM_VIEW_X, CAM_VIEW_Y);
	if (status != GX_SUCCESS)
		return status;

	rect_set(&size, 2, 110, 78, 150);
	status = gx_prompt_create(&title2, "title2", &screen2, 0, PROMPT_STYLE, 0,
	                          &size);
	if (status != GX_SUCCESS)
		return status;
	gx_prompt_text_set(&title2, "Camera");
	gx_prompt_font_set(&title2, F_TEXT);
	gx_prompt_text_color_set(&title2, C_TEXT, C_TEXT, C_TEXT);

	rect_set(&size, 402, 110, 477, 155);
	status = gx_text_button_create(&btn_cam_back, "camback", &screen2, 0,
	                               BUTTON_STYLE, ID_BTN_CAM_BACK, &size);
	if (status != GX_SUCCESS)
		return status;
	gx_text_button_text_set(&btn_cam_back, "Back");
	gx_text_button_font_set(&btn_cam_back, F_TEXT);
	gx_text_button_text_color_set(&btn_cam_back, C_BTN_TEXT, C_BTN_TEXT,
	                              C_BTN_TEXT);
	btn_cam_back.gx_widget_normal_fill_color   = C_BTN;
	btn_cam_back.gx_widget_selected_fill_color = C_ACCENT;

	/* Route the camera-preview control events (#56) through the root handler so
	   show/hide/dirty run on the GUIX thread. */
	root->gx_widget_event_process_function = guix_root_event;

	/* Visibility is NOT set here.  The canonical GUIX flow is: build the tree,
	   then the caller does gx_widget_show(root) -- which cascades GX_EVENT_SHOW
	   and recomputes every descendant's clip with the root visible -- and finally
	   guix_app_show_screen(0) to hide the inactive screen.  Doing show/hide here
	   (while the root is still invisible) computes the children's clips against
	   an empty root clip, so the buttons draw but are not hit-testable. */
	return GX_SUCCESS;
}

void guix_app_show_screen(int n)
{
	guix_app_select_screen(n);
}
