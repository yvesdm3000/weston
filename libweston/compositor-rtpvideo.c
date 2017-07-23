/*
 * Copyright © 2017 Yves De Muyter <yves@alfavisio.be>
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial
 * portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "config.h"
#include "compositor-rtpvideo.h"

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <linux/input.h>

#include "shared/helpers.h"
#include "compositor.h"
#include "pixman-renderer.h"
#include <rtpvideotx.h>

#define MAX_FDS 32
#define REFRESH_FREQ 60000

struct rtpvideo_output;

struct rtpvideo_backend {
	struct weston_backend base;
	struct weston_compositor *compositor;

	struct rtpvideo_output *output;
};

struct rtpvideo_output {
	struct weston_output base;
	struct wl_event_source *finish_frame_timer;
	struct wl_event_source *redraw_frame_timer;
	pixman_image_t *shadow_surface;
	RtpVideoTx_t video_out;
};

static inline struct rtpvideo_output *
to_rtpvideo_output(struct weston_output *base)
{
	return container_of(base, struct rtpvideo_output, base);
}

static inline struct rtpvideo_backend *
to_rtpvideo_backend(struct weston_compositor *base)
{
	return container_of(base->backend, struct rtpvideo_backend, base);
}

static void
rtpvideo_output_start_repaint_loop(struct weston_output *output)
{
	struct timespec ts;

	weston_compositor_read_presentation_clock(output->compositor, &ts);
	weston_output_finish_frame(output, &ts, WP_PRESENTATION_FEEDBACK_INVALID);
}

static int
finish_frame_handler(void *data)
{
	struct rtpvideo_output *output = data;
	struct timespec ts;

	weston_compositor_read_presentation_clock(output->base.compositor, &ts);
	weston_output_finish_frame(&output->base, &ts, 0);

	return 1;
}

static int
rtpvideo_output_repaint(struct rtpvideo_output *output, pixman_region32_t *damage)
{
	uint32_t* ptr;
	struct timespec ts;
	int y;

	//weston_log("rtpvideo_output_repaint %d %d %d %d\n", damage->extents.x1, damage->extents.x2, damage->extents.y1, damage->extents.y2);

	weston_compositor_read_presentation_clock(output->base.compositor, &ts);
	RtpVideoTx_beginFrame(output->video_out, (ts.tv_sec * 25)+(ts.tv_nsec/40000) );
	ptr = pixman_image_get_data(output->shadow_surface) + damage->extents.x1 + damage->extents.y1 * (pixman_image_get_stride(output->shadow_surface)/sizeof(uint32_t));
	for(y=0; y < damage->extents.y2 - damage->extents.y1; ++y) {
		uint8_t* buffer;
		uint32_t length = (damage->extents.x2 - damage->extents.x1)*3;
		unsigned long flags = 0;
		if (y == damage->extents.y2 - damage->extents.y1 - 1)
			flags = 1;
		if (RtpVideoTx_getLineBuffer(output->video_out, length, &buffer) == 0) {
			memcpy(buffer, ptr, length);
			RtpVideoTx_addLine(output->video_out, damage->extents.y1 + y, damage->extents.x1, length, buffer, flags);
		} else {
			weston_log("Could not get line buffer!\n");
		}
	
		ptr += pixman_image_get_stride(output->shadow_surface)/sizeof(uint32_t);
	}
	RtpVideoTx_flush(output->video_out);
	return 0;
}

static int
redraw_frame_handler(void *data)
{
	struct rtpvideo_output *output = data;
	struct timespec ts;
	weston_compositor_read_presentation_clock(output->base.compositor, &ts);
	pixman_region32_t damage;
	damage.data = NULL;
	damage.extents.x1 = 0;
	damage.extents.x2 = output->base.mm_width;
	damage.extents.y1 = 0;
	damage.extents.y2 = output->base.mm_height;
	
	rtpvideo_output_repaint(output, &damage);
	
	wl_event_source_timer_update(output->redraw_frame_timer, 1000);
	return 0;
}

static int
rtpvideo_output_weston_repaint(struct weston_output *output_base, pixman_region32_t *damage,
		   void *repaint_data)
{
	struct rtpvideo_output *output = container_of(output_base, struct rtpvideo_output, base);
	struct weston_compositor *ec = output->base.compositor;

	pixman_renderer_output_set_buffer(output_base, output->shadow_surface);
	ec->renderer->repaint_output(&output->base, damage);

	if (pixman_region32_not_empty(damage)) {
		rtpvideo_output_repaint(output,damage);
	}

	pixman_region32_subtract(&ec->primary_plane.damage,
				 &ec->primary_plane.damage, damage);

	wl_event_source_timer_update(output->finish_frame_timer, 16);
	return 0;
}


static struct weston_mode *
rtpvideo_insert_new_mode(struct weston_output *output, int width, int height, int rate)
{
	struct weston_mode *ret;
	ret = zalloc(sizeof *ret);
	if (!ret)
		return NULL;
	ret->width = width;
	ret->height = height;
	ret->refresh = rate;
	wl_list_insert(&output->mode_list, &ret->link);
	return ret;
}

static struct weston_mode *
ensure_matching_mode(struct weston_output *output, struct weston_mode *target)
{
	struct weston_mode *local;

	wl_list_for_each(local, &output->mode_list, link) {
		if ((local->width == target->width) && (local->height == target->height))
			return local;
	}

	return rtpvideo_insert_new_mode(output, target->width, target->height, REFRESH_FREQ);
}

static int
rtpvideo_switch_mode(struct weston_output *output, struct weston_mode *target_mode)
{
	struct rtpvideo_output *rtpvideoOutput = container_of(output, struct rtpvideo_output, base);
	pixman_image_t *new_shadow_buffer;
	struct weston_mode *local_mode;
	pixman_format_code_t format = PIXMAN_b8g8r8;
	RtpVideoTx_Format fmt;

	local_mode = ensure_matching_mode(output, target_mode);
	if (!local_mode) {
		weston_log("mode %dx%d not available\n", target_mode->width, target_mode->height);
		return -ENOENT;
	}

	if (local_mode == output->current_mode)
		return 0;

	output->current_mode->flags &= ~WL_OUTPUT_MODE_CURRENT;

	output->current_mode = local_mode;
	output->current_mode->flags |= WL_OUTPUT_MODE_CURRENT;

	pixman_renderer_output_destroy(output);
	pixman_renderer_output_create(output);


	RtpVideoTx_getVideoFormat(rtpvideoOutput->video_out, &fmt);
	switch(fmt) {
	case RtpVideoTx_Format_RGBA_8bit:
		format = PIXMAN_a8b8g8r8;
		break;
	case RtpVideoTx_Format_BGR_8bit:
		format = PIXMAN_r8g8b8;
		break;
	case RtpVideoTx_Format_BGRA_8bit:
		format = PIXMAN_a8r8g8b8;
		break;
	case RtpVideoTx_Format_RGB_8bit:
	default:
		format = PIXMAN_b8g8r8;
		break;
	}

	new_shadow_buffer = pixman_image_create_bits(format,
						     target_mode->width,
						     target_mode->height,
						     NULL,
						     target_mode->width * 3);
	pixman_image_composite32(PIXMAN_OP_SRC, rtpvideoOutput->shadow_surface, 0, new_shadow_buffer,
			0, 0, 0, 0, 0, 0, target_mode->width, target_mode->height);
	pixman_image_unref(rtpvideoOutput->shadow_surface);
	rtpvideoOutput->shadow_surface = new_shadow_buffer;

	return 0;
}

static int
rtpvideo_output_set_size(struct weston_output *base,
		    int width, int height)
{
	struct rtpvideo_output *output = to_rtpvideo_output(base);
	struct weston_mode *currentMode;
	struct weston_mode initMode;

	/* We can only be called once. */
	assert(!output->base.current_mode);

	initMode.flags = WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED;
	initMode.width = width;
	initMode.height = height;
	initMode.refresh = REFRESH_FREQ;
	wl_list_init(&output->base.mode_list);

	currentMode = ensure_matching_mode(&output->base, &initMode);
	if (!currentMode)
		return -1;

	output->base.current_mode = output->base.native_mode = currentMode;
	output->base.make = "weston";
	output->base.model = "rtpvideo";

	/* XXX: Calculate proper size. */
	output->base.mm_width = width;
	output->base.mm_height = height;

	output->base.start_repaint_loop = rtpvideo_output_start_repaint_loop;
	output->base.repaint = rtpvideo_output_weston_repaint;
	output->base.assign_planes = NULL;
	output->base.set_backlight = NULL;
	output->base.set_dpms = NULL;
	output->base.switch_mode = rtpvideo_switch_mode;

	return 0;
}

static int
rtpvideo_output_enable(struct weston_output *base)
{
	struct rtpvideo_output *output = to_rtpvideo_output(base);
	struct rtpvideo_backend *b = to_rtpvideo_backend(base->compositor);
	struct wl_event_loop *loop;

	pixman_format_code_t format = PIXMAN_b8g8r8;
	RtpVideoTx_Format fmt;
	RtpVideoTx_getVideoFormat(output->video_out, &fmt);
	switch(fmt) {
	case RtpVideoTx_Format_RGBA_8bit:
		format = PIXMAN_a8b8g8r8;
		break;
	case RtpVideoTx_Format_BGR_8bit:
		format = PIXMAN_r8g8b8;
		break;
	case RtpVideoTx_Format_BGRA_8bit:
		format = PIXMAN_a8r8g8b8;
		break;
	case RtpVideoTx_Format_RGB_8bit:
	default:
		format = PIXMAN_b8g8r8;
		break;
	}

	output->shadow_surface = pixman_image_create_bits(format,
							  output->base.current_mode->width,
							  output->base.current_mode->height,
							  NULL,
							  output->base.current_mode->width * 3);
	if (output->shadow_surface == NULL) {
		weston_log("Failed to create surface for frame buffer.\n");
		return -1;
	}

	if (pixman_renderer_output_create(&output->base) < 0) {
		pixman_image_unref(output->shadow_surface);
		return -1;
	}

	loop = wl_display_get_event_loop(b->compositor->wl_display);
	output->finish_frame_timer = wl_event_loop_add_timer(loop, finish_frame_handler, output);
	output->redraw_frame_timer = wl_event_loop_add_timer(loop, redraw_frame_handler, output);
	wl_event_source_timer_update(output->redraw_frame_timer, 1000);

	b->output = output;

	return 0;
}

static int
rtpvideo_output_disable(struct weston_output *base)
{
	struct rtpvideo_output *output = to_rtpvideo_output(base);
	struct rtpvideo_backend *b = to_rtpvideo_backend(base->compositor);

	if (!output->base.enabled)
		return 0;

	pixman_image_unref(output->shadow_surface);
	pixman_renderer_output_destroy(&output->base);

	wl_event_source_remove(output->finish_frame_timer);
	wl_event_source_remove(output->redraw_frame_timer);
	b->output = NULL;

	return 0;
}

static void
rtpvideo_output_destroy(struct weston_output *base)
{
	struct rtpvideo_output *output = to_rtpvideo_output(base);

	rtpvideo_output_disable(&output->base);
	weston_output_destroy(&output->base);

	free(output);
}

static int
rtpvideo_backend_create_output(struct weston_compositor *compositor,
			       const char* destination_address,
			       int destination_port,
			       RtpVideoTx_Format colorspace,
			       int ssrc)
{
	struct rtpvideo_output *output;

	output = zalloc(sizeof *output);
	if (output == NULL)
		return -1;

	output->base.name =  strdup("rdp");
	output->base.destroy = rtpvideo_output_destroy;
	output->base.disable = rtpvideo_output_disable;
	output->base.enable = rtpvideo_output_enable;

	weston_output_init(&output->base, compositor);
	weston_compositor_add_pending_output(&output->base, compositor);

	output->video_out = RtpVideoTx_new(-1, colorspace);

	if (destination_address) {
		RtpVideoTx_addDestination(output->video_out,
		destination_address, destination_port);
	} else {
		weston_log("No RTP destination configured\n");
	}
	if (ssrc >= 0) {
		RtpVideoTx_setSSRC(output->video_out, ssrc);
	}

	return 0;
}

static void
rtpvideo_restore(struct weston_compositor *ec)
{
}

static void
rtpvideo_destroy(struct weston_compositor *ec)
{
	struct rtpvideo_backend *b = to_rtpvideo_backend(ec);

	weston_compositor_shutdown(ec);
	free(b);
}

static const struct rtpvideo_output_api api = {
	rtpvideo_output_set_size,
};


static struct rtpvideo_backend *
rtpvideo_backend_create(struct weston_compositor *compositor,
		   struct rtpvideo_backend_config *config)
{
	struct rtpvideo_backend *b;
	int ret;
	RtpVideoTx_Format colorspace = RtpVideoTx_Format_RGB_8bit;

	b = zalloc(sizeof *b);
	if (b == NULL)
		return NULL;

	b->compositor = compositor;
	b->base.destroy = rtpvideo_destroy;
	b->base.restore = rtpvideo_restore;
	if (weston_compositor_set_presentation_clock_software(compositor) < 0)
		goto err_compositor;

	if (pixman_renderer_init(compositor) < 0)
		goto err_compositor;

	if (config->colorspace == NULL)
		colorspace = RtpVideoTx_Format_RGB_8bit;
	else if (strcmp(config->colorspace,"RGB") == 0)
		colorspace = RtpVideoTx_Format_RGB_8bit;
        else if (strcmp(config->colorspace,"ARGB") == 0)
		colorspace = RtpVideoTx_Format_RGBA_8bit;
        else if (strcmp(config->colorspace,"BGR") == 0)
		colorspace = RtpVideoTx_Format_BGR_8bit;
        else if (strcmp(config->colorspace,"ABGR") == 0)
		colorspace = RtpVideoTx_Format_BGRA_8bit;
        else {
		weston_log("Unknown colorspace: %s. Must be one of RGB, ARGB, BGR, ABGR\n", config->colorspace);
		goto err_compositor;
	}

	if (rtpvideo_backend_create_output(compositor, config->destination_address,
					   config->destination_port, colorspace,
					   config->ssrc) < 0)
		goto err_compositor;

	compositor->capabilities |= WESTON_CAP_ARBITRARY_MODES;

	compositor->backend = &b->base;

	ret = weston_plugin_api_register(compositor, RTPVIDEO_OUTPUT_API_NAME,
					 &api, sizeof(api));

	if (ret < 0) {
		weston_log("Failed to register output API.\n");
		goto err_output;
	}

	return b;

err_output:
	weston_output_destroy(&b->output->base);
err_compositor:
	weston_compositor_shutdown(compositor);
	free(b);
	return NULL;
}

static void
config_init_to_defaults(struct rtpvideo_backend_config *config)
{
	config->bind_address = NULL;
	config->bind_port = 0;
	config->destination_address = "232.0.0.1";
	config->destination_port = 49410;
	config->ssrc = -1;
	config->colorspace = NULL;
}

WL_EXPORT int
weston_backend_init(struct weston_compositor *compositor,
		    struct weston_backend_config *config_base)
{
	struct rtpvideo_backend *b;
	struct rtpvideo_backend_config config = {{ 0, }};

	if (config_base == NULL ||
		config_base->struct_version != RTPVIDEO_BACKEND_CONFIG_VERSION ||
		config_base->struct_size > sizeof(struct rtpvideo_backend_config)) {
		weston_log("RTPVIDEO backend config structure is invalid\n");
		return -1;
	}

	config_init_to_defaults(&config);
	memcpy(&config, config_base, config_base->struct_size);

	b = rtpvideo_backend_create(compositor, &config);
	if (b == NULL)
		return -1;
	return 0;
}
