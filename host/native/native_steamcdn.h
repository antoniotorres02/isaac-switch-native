#ifndef ISAAC_NATIVE_STEAMCDN_H
#define ISAAC_NATIVE_STEAMCDN_H

#include "native_modmanager.h"

#define WORKSHOP_THUMB_W 256
#define WORKSHOP_THUMB_H 144
#define WORKSHOP_MEDIA_FRAMES 16

typedef struct {
    uint32_t pixels[WORKSHOP_THUMB_W * WORKSHOP_THUMB_H];
    unsigned width, height;
} workshop_thumb;

typedef struct {
    workshop_thumb *frames[WORKSHOP_MEDIA_FRAMES];
    unsigned delay[WORKSHOP_MEDIA_FRAMES];
    unsigned count;
} workshop_animation;

int workshop_install(const workshop_net *, const mod_store *, const workshop_item *,
                     char [WORKSHOP_ERR]);
int workshop_preview(const workshop_net *, const char *, workshop_animation *, char [WORKSHOP_ERR]);
int workshop_illustration(const workshop_net *, const char *, workshop_animation *, char [WORKSHOP_ERR]);
void workshop_animation_clear(workshop_animation *);

#endif
