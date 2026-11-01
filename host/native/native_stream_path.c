/* A doubled '/' in a path the engine opens.
 *
 * The engine names a mod's metadata "<mod folder>/" + "/metadata.xml". On
 * Windows the doubled separator reads as one, and the PC mod folder is an
 * absolute path anyway (z:\...\mods/<mod>/ in a Steam log). Here it is the
 * engine root, "//mods/<mod>/", which the engine's stream open (0xa178d0)
 * looks up among the files it listed at start; ".../<mod>//metadata.xml" is
 * not among them. Every mod then had no metadata: its <name> became the
 * folder, so mods ran in folder order instead of name order (Mod Config Menu,
 * named " Mod Config Menu - Impure", ran after External Item Descriptions,
 * which then found no MCM to add its settings to), and the engine rewrote
 * each metadata.xml with defaults.
 *
 * isaac_hook_stream_path runs at the stream open, where the path is at
 * [ESP+4], and hands it a copy with each run of '/' after the leading "//"
 * (the engine root) collapsed to one. Paths without one are left alone. The
 * copy lives in the guest heap and is not freed: a few dozen short paths per
 * start. */
#include <stdint.h>
#include <string.h>

extern uint8_t *recomp_mem_base;
extern uint32_t isaac_guest_alloc(uint32_t n);

void isaac_hook_stream_path(uint32_t esp) {
    uint32_t va;
    memcpy(&va, recomp_mem_base + esp + 4, 4);
    if (!va) return;
    const char *p = (const char *)(recomp_mem_base + va);
    size_t n = strnlen(p, 1024);
    if (n < 3 || n == 1024 || !strstr(p + 2, "//")) return;
    uint32_t copy = isaac_guest_alloc((uint32_t)n + 1);
    if (!copy) return;
    char *c = (char *)(recomp_mem_base + copy);
    size_t k = 0;
    for (size_t i = 0; i < n; ++i) {
        if (i >= 2 && p[i] == '/' && c[k - 1] == '/') continue;
        c[k++] = p[i];
    }
    c[k] = 0;
    memcpy(recomp_mem_base + esp + 4, &copy, 4);
}
