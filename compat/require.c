/*
 * compat/require.c — Lua 5.x-compatible require() for Luau.
 *
 * Copyright © AwesomeWM Contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "require.h"
#include "package.h"   /* for LUAU_PACKAGE_LOADED_KEY */
#include "luau_compat.h"

#include <lua.h>
#include <lualib.h>

/* --------------------------------------------------------------------------
 * luaA_require — drop-in replacement for the global require().
 *
 * Stack contract:
 *   Entry:  [modname]           (exactly 1 argument, a string)
 *   Return: [result]            (exactly 1 result — the module value)
 *
 * Errors raised:
 *   • modname is not a string
 *   • package global is missing or not a table
 *   • package.searchers is missing or not a table
 *   • no searcher found a loader (lists all hints)
 *   • any error thrown by the loader itself (propagated unchanged)
 * -------------------------------------------------------------------------- */
static int
luaA_require(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1);
    lua_settop(L, 1); /* normalise: only [name] on stack */

    /* ------------------------------------------------------------------
     * 1 — Cache lookup via the shared _LOADED registry table.
     *     Using the registry key directly avoids a global table walk and
     *     keeps us in sync with any Luau-internal caching.
     *     Absolute index 2 after the push lets us use it safely in a loop.
     * ------------------------------------------------------------------ */
    lua_getfield(L, LUA_REGISTRYINDEX, LUAU_PACKAGE_LOADED_KEY); /* [name, loaded] */
    lua_getfield(L, 2, name);                                     /* [name, loaded, val] */
    if (lua_toboolean(L, 3))
        return 1; /* already cached — leave value at top */
    lua_pop(L, 1); /* drop nil/false; [name, loaded] */

    /* ------------------------------------------------------------------
     * 2 — Fetch package.searchers (absolute index 3 after manipulations).
     * ------------------------------------------------------------------ */
    lua_getglobal(L, "package");        /* [name, loaded, pkg]           */
    if (!lua_istable(L, 3))
        luaL_error(L, "package global is missing or not a table");
    lua_getfield(L, 3, "searchers");    /* [name, loaded, pkg, searchers] */
    lua_remove(L, 3);                   /* [name, loaded, searchers]      */
    if (!lua_istable(L, 3))
        luaL_error(L, "package.searchers must be a table");

    /* ------------------------------------------------------------------
     * 3 — Iterate searchers.  Accumulate hint strings from failures into
     *     a single Lua string at index 4 for the final error message.
     * ------------------------------------------------------------------ */
    lua_pushliteral(L, ""); /* error hint accumulator; [name, loaded, searchers, errmsg] */

    for (int i = 1; ; i++) {
        lua_rawgeti(L, 3, i);    /* [..., searcher_or_nil] */
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            break; /* exhausted all searchers */
        }

        lua_pushvalue(L, 1);     /* [..., searcher, name] */
        lua_call(L, 1, 1);       /* [..., loader_or_hint] */

        if (lua_isfunction(L, -1)) {
            /*
             * Loader found.  Store a sentinel first to break circular
             * require loops, then call the loader.
             */
            lua_pushboolean(L, 1); /* sentinel */
            lua_setfield(L, 2, name); /* loaded[name] = true (sentinel) */

            lua_pushvalue(L, 1);     /* [..., loader, name] */
            lua_call(L, 1, 1);       /* [..., result_or_nil] */

            /*
             * Normalise nil return → true  (Lua 5.x spec: "if the module
             * returns no value… assign true to the entry").
             */
            if (lua_isnil(L, -1)) {
                lua_pop(L, 1);
                lua_pushboolean(L, 1);
            }

            /* Overwrite sentinel with actual result */
            lua_pushvalue(L, -1);
            lua_setfield(L, 2, name); /* loaded[name] = result */
            return 1;
        }

        /* Searcher returned a string hint or something unexpected */
        if (lua_isstring(L, -1))
            lua_concat(L, 2); /* errmsg = errmsg .. hint */
        else
            lua_pop(L, 1); /* discard non-string non-function results */
    }

    /* ------------------------------------------------------------------
     * 4 — No loader found.
     * ------------------------------------------------------------------ */
    luaL_error(L, "module '%s' not found:%s", name, lua_tostring(L, 4));
    return 0; /* unreachable */
}

/* --------------------------------------------------------------------------
 * luaA_require_open(L)
 * Replaces the global `require` with luaA_require.
 * Must be called after luaA_package_open().
 * -------------------------------------------------------------------------- */
void
luaA_require_open(lua_State *L)
{
    luaA_pushcfunction(L, luaA_require);
    lua_setglobal(L, "require");
}
