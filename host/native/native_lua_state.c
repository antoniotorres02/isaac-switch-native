/* The game's Lua state, for the host's questions about mods.
 *
 * Lua's own state hooks (luai_userstateopen/close, defined in luaconf.h by
 * build_native.py) report the state the game opens; it closes it and opens
 * a new one when the mods reload.
 *
 * isaac_native_mod_menu_open says whether Mod Config Menu is on screen. MCM
 * does not pause the game: the tick keeps advancing under it, so the Switch
 * pad would read the face buttons by position as in play, and the right
 * button (Xbox B) would go back where the player means to accept
 * (native_switch.c). Raw reads only: no metamethod and no mod code runs.
 *
 * isaac_native_lua_running says whether the game's state is inside a call
 * (a mod callback, or the engine running a script): a pixel read asked then
 * comes from a mod's Sprite:GetTexel and must be exact (patch 0007). */
#include "lua.h"
#include "lstate.h"

static lua_State *g_L;

void isaac_native_lua_opened(lua_State *L) { g_L = L; }
void isaac_native_lua_closed(lua_State *L) { if (g_L == L) g_L = NULL; }

/* globals[table][field] is truthy; the globals table is at the top. */
static int global_field_set(lua_State *L, const char *table, const char *field) {
    int set = 0;
    lua_pushstring(L, table);
    if (lua_rawget(L, -2) == LUA_TTABLE) {
        lua_pushstring(L, field);
        lua_rawget(L, -2);
        set = lua_toboolean(L, -1);
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    return set;
}

int isaac_native_mod_menu_open(void) {
    lua_State *L = g_L;
    if (!L || !lua_checkstack(L, 3)) return 0;
    int top = lua_gettop(L);
    lua_rawgeti(L, LUA_REGISTRYINDEX, LUA_RIDX_GLOBALS);
    int open = global_field_set(L, "ModConfigMenu", "IsVisible");
    lua_settop(L, top);
    return open;
}

int isaac_native_lua_running(void) {
    lua_State *L = g_L;
    return L && L->ci != &L->base_ci;
}
