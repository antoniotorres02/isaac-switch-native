/* luaL_loadfilex through the host RAM-FS.
 *
 * The game loads each mod's main.lua with luaL_loadfilex("//mods/<mod>/main.lua")
 * and mods call dofile/loadfile; in the browser those names resolve in
 * Emscripten's MEMFS, which the page fills with the same files. Natively the
 * RAM-FS is the only place they exist, so the native Lua build renames the
 * original to luaL_loadfilex_disk (build_native.py) and this wrapper tries the
 * RAM-FS first, then the disk (resources/scripts relative to the instance).
 * Like the original it skips a UTF-8 BOM and a first line starting with '#'. */
#include <stdint.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

int luaL_loadfilex_disk(lua_State *L, const char *filename, const char *mode);
int isaac_fs_read_whole(const char *path, const uint8_t **data, uint32_t *len);

LUALIB_API int luaL_loadfilex(lua_State *L, const char *filename, const char *mode) {
    const uint8_t *d = NULL;
    uint32_t n = 0;
    if (!filename || !isaac_fs_read_whole(filename, &d, &n))
        return luaL_loadfilex_disk(L, filename, mode);
    const char *p = (const char *)d;
    size_t len = n;
    if (len >= 3 && memcmp(p, "\xEF\xBB\xBF", 3) == 0) { p += 3; len -= 3; }
    if (len && p[0] == '#') {                 /* keep the newline: line numbers stay right */
        while (len && *p != '\n') { ++p; --len; }
    }
    lua_pushfstring(L, "@%s", filename);
    int status = luaL_loadbufferx(L, p, len, lua_tostring(L, -1), mode);
    lua_remove(L, -2);                        /* the chunk name */
    return status;
}
