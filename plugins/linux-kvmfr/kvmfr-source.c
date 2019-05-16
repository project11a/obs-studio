/*
linux-kvmfr OBS plugin
Copyright (C) 2019 Johnny Boss <tim@project11a.com>

Looking Glass - KVM FrameRelay (KVMFR) Client
Copyright (C) 2017-2019 Geoffrey McRae <geoff@hostfission.com>
https://looking-glass.hostfission.com

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; either version 2 of the License, or (at your option) any later
version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc., 59 Temple
Place, Suite 330, Boston, MA 02111-1307 USA
*/

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <inttypes.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include <obs-module.h>
#include <util/dstr.h>
#include <util/bmem.h>

#include "KVMFR.h"

#define LG_DATA(voidptr) struct lg_data *data = voidptr;

#define blog(level, msg, ...) \
        blog(level, "kvmfr-source: " msg, ##__VA_ARGS__)

struct lg_data {
	obs_source_t     *source;

	int              shmFD;
	KVMFRHeader      *header;

	char             *shmFile;
	int_fast32_t     shmSize;

	int_fast32_t     width;
	int_fast32_t     height;

	gs_texture_t     *texture;
	
	gs_texture_t     *cursor_texture;
	uint8_t          *cursor_image;
	bool             cursor_visible;
	gs_texture_t     *cursor_mono;
	int_fast32_t     cursor_x;
	int_fast32_t     cursor_y;
	int_fast32_t     cursor_w;
	int_fast32_t     cursor_h;
	int_fast32_t     cursor_version;

	bool             show_cursor;
};

/**
 * Resize the texture
 *
 * This will automatically create the texture if it does not exist
 *
 * @note requires to be called within the obs graphics context
 */
static inline void lg_resize_texture(struct lg_data *data,
                                     struct KVMFRFrame *header)
{
	enum gs_color_format colorFormat = GS_BGRA;

	if (data->texture) gs_texture_destroy(data->texture);

	switch (header->type) {
	case FRAME_TYPE_RGBA:
		colorFormat = GS_RGBA;
	case FRAME_TYPE_BGRA:
		break;
	case FRAME_TYPE_RGBA10:
		colorFormat = GS_R10G10B10A2;
		break;
	/* Wait... what's this for?
	case FRAME_TYPE_YUV420:
		colorFormat = GS_R10G10B10A2;
		break; */
	default:
		blog(LOG_ERROR, "Unsupported frameType in %s",
		     data->shmFile);
		return;
	}
	data->width = header->width;
	data->height = header->height;
	data->texture = gs_texture_create(data->width, data->height,
	                                  colorFormat, 1, NULL, GS_DYNAMIC);
}

/**
 * Map the shared memory file
 */
static void * map_memory(struct lg_data *data)
{
	struct stat st;

	if (!data->shmSize) {
		if (stat(data->shmFile, &st) < 0) {
			blog(LOG_ERROR, 
			     "Failed to stat the shared memory file: %s",
			     data->shmFile);
			return NULL;
		}
		data->shmSize = st.st_size;
	}

	data->shmFD   = open(data->shmFile, O_RDWR, (mode_t)0600);
	if (data->shmFD < 0) {
		blog(LOG_ERROR, "Failed to open the shared memory file: %s",
		     data->shmFile);
		return NULL;
	}

	void * map = mmap(0, data->shmSize, PROT_READ | PROT_WRITE, MAP_SHARED,
	                  data->shmFD, 0);
	if (map == MAP_FAILED) {
		blog(LOG_ERROR, "Failed to map the shared memory file: %s",
		     data->shmFile);
		close(data->shmFD);
		data->shmFD = 0;
		return NULL;
	}

	return map;
}

/**
 * Returns the name of the plugin
 */
static const char* lg_getname(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("lgScreenCapture");
}

/**
 * Convert cursor texture and load it
 */
static void lg_fetch_cursor(struct lg_data *data, struct KVMFRCursor *header)
{
	const uint8_t *dest = (const uint8_t *)data->header + header->dataPos;
	const uint32_t *src = (const uint32_t *)dest;
	uint8_t mask;
	int width = header->width;
	int height = header->height;
	
	if (header->type == CURSOR_TYPE_MONOCHROME) {
		if (data->cursor_w != width ||
		    data->cursor_h != height/2) {
			if (data->cursor_image) bfree(data->cursor_image);
			data->cursor_image = bmalloc(width * height * 4);
		}
	} else {
		if (data->cursor_w != width ||
		    data->cursor_h != height) {
			if (data->cursor_image) bfree(data->cursor_image);
			data->cursor_image = bmalloc(width * height * 4);
		}
	}
	uint32_t *texture = (uint32_t *)data->cursor_image;
	
	
	switch (header->type) {
	case CURSOR_TYPE_MONOCHROME:
		height = height / 2;
		for (int i = 0; i < width*height; ++i) {
			mask = 0x80 >> (i & 7);
			if (!(dest[i/8] & mask)) {
				texture[i] = 0xFF000000;
			} else {
				texture[i] = 0x00000000;
			}
		}
		for (int i = width*height; i < 2*width*height; ++i) {
			mask = 0x80 >> (i & 7);
			if (dest[(i)/8] & mask) {
				texture[i] = 0xFFFFFFFF;
			} else {
				texture[i] = 0x00000000;
			}
		}
		dest = data->cursor_image;
		obs_enter_graphics();
		if (!data->cursor_mono) {
			data->cursor_mono = gs_texture_create(width, height,
		                                              GS_BGRA, 1, NULL, 
		                                              GS_DYNAMIC);
		}
		gs_texture_set_image(data->cursor_mono,
		                     (const uint8_t *)(texture+width*height),
	                             width*4, false);
		break;
	case CURSOR_TYPE_MASKED_COLOR:
		for (int i = 0; i < width * height; ++i) {
			texture[i] = (src[i] & ~0xFF000000) |
			             (src[i] & 0xFF000000 ? 0x0 : 0xFF000000);
		}
		dest = data->cursor_image;
	default:
		obs_enter_graphics();
		gs_texture_destroy(data->cursor_mono);
		data->cursor_mono = NULL;
	}
	if (data->cursor_w != width ||
	    data->cursor_h != height) {
		if (data->cursor_texture) {
			gs_texture_destroy(data->cursor_texture);
		}
		data->cursor_w = width;
		data->cursor_h = height;
		data->cursor_texture = gs_texture_create(width, height,
		                                         GS_BGRA, 1, NULL, 
		                                         GS_DYNAMIC);
	}
	gs_texture_set_image(data->cursor_texture, dest,
	                     width*4, false);
	obs_leave_graphics();
	
}

/**
 * Prepare the capture data
 */
static void lg_video_tick_frame(struct lg_data *data)
{
	KVMFRFrame header;
	// we must take a copy of the header to prevent the contained
	// arguments from being abused to overflow buffers.
	memcpy(&header,
	       &data->header->frame, sizeof(struct KVMFRFrame));
	// tell the host to continue as the host buffers up to one frame
	// we can be sure the data for this frame wont be touched
	__sync_and_and_fetch(&data->header->frame.flags,
		             ~KVMFR_FRAME_FLAG_UPDATE);

	obs_enter_graphics();
	if (header.width != data->width || header.height != data->height) {
		lg_resize_texture(data, &header);
	}
	gs_texture_set_image(data->texture,
	                     (const uint8_t *)data->header + header.dataPos,
	                     header.pitch, false);
	obs_leave_graphics();
}
static void lg_video_tick_cursor(struct lg_data *data)
{
	KVMFRCursor header;
	// update cursor position
	if (data->header->cursor.flags && KVMFR_CURSOR_FLAG_POS) {
		data->cursor_x = data->header->cursor.x;
		data->cursor_y = data->header->cursor.y;
		__sync_and_and_fetch(&data->header->cursor.flags,
		                     ~KVMFR_CURSOR_FLAG_POS);
	}
	// update cursor
	if (!(data->header->cursor.flags && KVMFR_CURSOR_FLAG_UPDATE)) return;
	memcpy(&header,
	       &data->header->cursor, sizeof(struct KVMFRCursor));
	if (header.flags & KVMFR_CURSOR_FLAG_SHAPE &&
	    data->cursor_version != header.version) {
		lg_fetch_cursor(data, &header);
	}
	data->header->cursor.flags = 0;
	data->cursor_visible = header.flags & KVMFR_CURSOR_FLAG_VISIBLE;
}
static void lg_video_tick(void *vptr, float seconds)
{
	LG_DATA(vptr);
	UNUSED_PARAMETER(seconds);

	if (!obs_source_showing(data->source))
		return;
	if (!data->texture)
		return;
	
	if (data->header->frame.flags & KVMFR_FRAME_FLAG_UPDATE)
		lg_video_tick_frame(data);
	
	if (data->show_cursor) lg_video_tick_cursor(data);
	
}

/**
 * Stop the capture
 */
static void lg_capture_stop(struct lg_data *data)
{
	obs_enter_graphics();

	if (data->texture) {
		gs_texture_destroy(data->texture);
		data->texture = NULL;
	}
	if (data->cursor_texture) {
		gs_texture_destroy(data->cursor_texture);
		data->cursor_texture = NULL;
	}
	if (data->cursor_mono) {
		gs_texture_destroy(data->cursor_mono);
		data->cursor_mono = NULL;
	}
	data->cursor_visible = false;

	obs_leave_graphics();
	
	if (data->cursor_image) bfree(data->cursor_image);
	data->cursor_image = NULL;

	if (data->header) {
		munmap(data->header, data->shmSize);
		close(data->shmFD);
		data->header = NULL;
	}
}

/**
 * Start the capture
 */
static void lg_capture_start(struct lg_data *data)
{
	KVMFRCursor cursor;

	// get the memory ready
	data->header = (struct KVMFRHeader *) map_memory(data);
	if (!data->header) {
		blog(LOG_ERROR, "Failed to map memory");
		goto fail;
	}

	//this should fetch the cursor shape
	__sync_or_and_fetch(&data->header->flags, KVMFR_HEADER_FLAG_RESTART);

	// check the header's magic and version are valid
	if (memcmp(data->header->magic,
	           KVMFR_HEADER_MAGIC, sizeof(KVMFR_HEADER_MAGIC)) != 0) {
		blog(LOG_ERROR, "Invalid header magic, is the host running? %s",
		     data->shmFile);
		goto fail;
	}
	if (data->header->version != KVMFR_HEADER_VERSION) {
		blog(LOG_ERROR,
		     "KVMFR version missmatch, expected %u but got %u - %s",
		     KVMFR_HEADER_VERSION,
		     data->header->version, data->shmFile);
		blog(LOG_ERROR,
		     "This is not a bug, ensure you have the right version");
		goto fail;
	}

	// create texture
	data->height = 0;
	memcpy(&cursor, &data->header->cursor, sizeof(struct KVMFRCursor));
	obs_enter_graphics();
	if (data->show_cursor) {
		data->cursor_w = 1;
		data->cursor_w = 1;
		data->cursor_texture = gs_texture_create(1, 1,
		                                         GS_BGRA, 1, NULL,
		                                         GS_DYNAMIC);
	}
	obs_leave_graphics();
	lg_video_tick_frame(data); // this will initialize the texture data

	return;

fail:
	lg_capture_stop(data);
}

/**
 * Update the capture with changed settings
 */
static void lg_update(void *vptr, obs_data_t *settings)
{
	LG_DATA(vptr);

	lg_capture_stop(data);

	data->show_cursor = obs_data_get_bool(settings, "show_cursor");
	if( data->shmFile ) bfree(data->shmFile);
	data->shmFile      = bstrdup(obs_data_get_string(settings, "file"));

	lg_capture_start(data);
}

/**
 * Get the default settings for the capture
 */
static void lg_defaults(obs_data_t *defaults)
{
	obs_data_set_default_string(defaults, "file", "/dev/shm/looking-glass");
	obs_data_set_default_bool(defaults, "show_cursor", true);
}

/**
 * Get the properties for the capture
 */
static obs_properties_t *lg_properties(void *vptr)
{
	UNUSED_PARAMETER(vptr);

	obs_properties_t *props = obs_properties_create();

	obs_properties_add_text(props, "file",
	                        obs_module_text("shmFile"), OBS_TEXT_DEFAULT);
	obs_properties_add_bool(props, "show_cursor",
	                        obs_module_text("CaptureCursor"));

	return props;
}

/**
 * Show this source
 *
 * Implemented to allow for rescanning the shared memory in case
 * the capture didn't accept it in the beginning.
 */
static void lg_show(void *vptr)
{
	LG_DATA(vptr);
	if (!data->header) lg_capture_start(data);
}

/**
 * Destroy the capture
 */
static void lg_destroy(void *vptr)
{
	LG_DATA(vptr);

	lg_capture_stop(data);
	if( data->shmFile ) bfree(data->shmFile);

	bfree(data);
}

/**
 * Create the capture
 */
static void *lg_create(obs_data_t *settings, obs_source_t *source)
{
	// create instance and load settings
	struct lg_data *data = bzalloc(sizeof(struct lg_data));
	data->source = source;
	lg_update(data, settings);

	// done
	return data;
}

/**
 * Render the capture data
 */
static void lg_video_render_cursor(struct lg_data *data, gs_effect_t *effect)
{
	gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");
	gs_effect_set_texture(image, data->cursor_texture);

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA);
	gs_enable_color(true, true, true, false);

	gs_matrix_push();
	gs_matrix_translate3f(data->cursor_x, data->cursor_y, 0.0f);
	gs_draw_sprite(data->cursor_texture, 0, 0, 0);
	if (data->cursor_mono) {
		gs_effect_set_texture(image, data->cursor_mono);
		gs_draw_sprite(data->cursor_mono, 0, 0, 0);
	}
	gs_matrix_pop();

	gs_enable_color(true, true, true, true);
	gs_blend_state_pop();
}
static void lg_video_render(void *vptr, gs_effect_t *effect)
{
	LG_DATA(vptr);

	if (!data->texture)
		return;

	effect = obs_get_base_effect(OBS_EFFECT_OPAQUE);

	gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");
	gs_effect_set_texture(image, data->texture);

	while (gs_effect_loop(effect, "Draw")) {
		gs_draw_sprite(data->texture, 0, 0, 0);
	}

	if (data->cursor_visible) {
		effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
		while (gs_effect_loop(effect, "Draw")) {
			lg_video_render_cursor(data, effect);
		}
	}
}

/**
 * Width of the captured data
 */
static uint32_t lg_getwidth(void *vptr)
{
	LG_DATA(vptr);
	return data->width;
}

/**
 * Height of the captured data
 */
static uint32_t lg_getheight(void *vptr)
{
	LG_DATA(vptr);
	return data->height;
}

struct obs_source_info lg_capture = {
	.id             = "lg_capture",
	.type           = OBS_SOURCE_TYPE_INPUT,
	.output_flags   = OBS_SOURCE_VIDEO |
	                  OBS_SOURCE_CUSTOM_DRAW |
	                  OBS_SOURCE_DO_NOT_DUPLICATE,
	.get_name       = lg_getname,
	.create         = lg_create,
	.destroy        = lg_destroy,
	.update         = lg_update,
	.show           = lg_show,
	.get_defaults   = lg_defaults,
	.get_properties = lg_properties,
	.video_tick     = lg_video_tick,
	.video_render   = lg_video_render,
	.get_width      = lg_getwidth,
	.get_height     = lg_getheight
};
