#ifndef ISAAC_NATIVE_WORKSHOP_MEDIA_H
#define ISAAC_NATIVE_WORKSHOP_MEDIA_H

#include "native_steamcdn.h"

#define WORKSHOP_MEDIA_IMAGES 12
#define WORKSHOP_MEDIA_URL 1024
/* Steam text is sanitized before this marker is inserted. */
#define WORKSHOP_MEDIA_MARKER 0x1e
#define WORKSHOP_STYLE_MARKER 0x1d
#define WORKSHOP_STYLE_CODES "BIUS123QCL"

unsigned workshop_media_description(const char *source, char *text, size_t capacity,
                                    char urls[WORKSHOP_MEDIA_IMAGES][WORKSHOP_MEDIA_URL]);
int workshop_gif(const uint8_t *bytes, size_t len, workshop_animation *animation, unsigned limit);
void workshop_thumbnail_scale(workshop_thumb *thumb, const uint8_t *pixels, unsigned width,
                              unsigned height, unsigned channels);

#endif
