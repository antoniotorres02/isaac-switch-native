#ifndef ISAAC_NATIVE_MODSTATE_H
#define ISAAC_NATIVE_MODSTATE_H

/* Same per-mod marker convention for the launcher and the in-game menu. */
int isaac_modstate_get(const char *saves, const char *folder);
int isaac_modstate_set(const char *saves, const char *folder, int on);

#endif
