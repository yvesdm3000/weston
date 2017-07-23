#ifndef COMPOSITOR_RTPVIDEO_H
#define COMPOSITOR_RTPVIDEO_H

#ifdef  __cplusplus
extern "C" {
#endif

#include "compositor.h"
#include "plugin-registry.h"

#define RTPVIDEO_OUTPUT_API_NAME "rtpvideo_output_api_v1"

struct rtpvideo_output_api {
	int (*output_set_size)(struct weston_output *output,
			       int width, int height);
};

static inline const struct rtpvideo_output_api *
rtpvideo_output_get_api(struct weston_compositor *compositor)
{
	const void *api;
	api = weston_plugin_api_get(compositor, RTPVIDEO_OUTPUT_API_NAME,
				    sizeof(struct rtpvideo_output_api));

	return (const struct rtpvideo_output_api *)api;
}

#define RTPVIDEO_BACKEND_CONFIG_VERSION 1

struct rtpvideo_backend_config {
	struct weston_backend_config base;
	char *bind_address;
	int bind_port;
	char *destination_address;
        int destination_port;
	char* colorspace;
        int ssrc;
};

#ifdef  __cplusplus
}
#endif

#endif /* COMPOSITOR_RTPVIDEO_H */
