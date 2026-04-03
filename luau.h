/*
    luau.h - Luau Compatibility Layer

    This file provides compatibility and shim functions that
    attempt to provide functionality equivalent to the standard Lua 5.1
    library functions in a Luau environment.
*/
#ifndef AWESOME_LUAU_H
#define AWESOME_LUAU_H

#include <lua.h>
#include <luacode.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LUA_VERSION_NUM 504 //old value was 501 - find a good value for this since luau doesnt seem to define it

/*
 * LUA_ERRFILE is defined in standard Lua's lauxlib.h alongside luaL_loadfile,
 * which Luau omits entirely. Matches the Lua 5.1 lauxlib.h value (LUA_ERRERR+1).
 * Note: this numeric value equals Luau's LUA_BREAK, but LUA_BREAK is a debugger-only
 * status never returned by luau_load/lua_pcall, so there is no practical conflict.
 */
#ifndef LUA_ERRFILE
#define LUA_ERRFILE (LUA_ERRERR + 1)
#endif

// Luau defines `lua_pushcfunction(L, fn, dbgName)` with 3 args, not 2 as in 'standard' Lua C.
// We can fix this relatively simply.
#define luaA_pushcfunction(L, fn) lua_pushcfunction(L, fn, #fn)

// lua_ref in Luau does not pop from the stack, unlike luaL_ref in standard Lua5.x which does pop.
static inline int luaL_ref(lua_State *L, int t) {
    int ref = lua_ref(L, -1);
    lua_pop(L, 1);
    return ref;
}
static inline void luaL_unref(lua_State *L, int t, int ref) {
    lua_unref(L, ref);
}

/*
 * luaA_getuservalue / luaA_setuservalue
 *
 * Luau's lua_getfenv/lua_setfenv only work on functions and threads — not
 * userdata. We facilitate per-object environment tables by keying them in the
 * global registry via the userdata pointer cast to lightuserdata.
 *
 * Pairing: luaA_class_gc must nil out registry[ptr] when the object is
 * collected, otherwise the environment table leaks permanently.
 */
static inline void
luaA_getuservalue(lua_State *L, int idx)
{
    void *ptr = lua_touserdata(L, idx);
    lua_pushlightuserdata(L, ptr);
    lua_rawget(L, LUA_REGISTRYINDEX);
}

static inline void
luaA_setuservalue(lua_State *L, int idx)
{
    /* Stack on entry: ..., table (at top). The userdata lives at idx. */
    void *ptr = lua_touserdata(L, idx);
    lua_pushlightuserdata(L, ptr); /* push key */
    lua_insert(L, -2);             /* reorder: ..., key, table */
    lua_rawset(L, LUA_REGISTRYINDEX); /* registry[ptr] = table; pops both */
}

/*
Luau has no `luaL_dostring` / `luaL_loadfile` - this acts as a drop-in replacement
accordingly.

On LUA_OK any values returned by the chunk are left on the stack.
On failure the error is printed; popped from the stack; and the status returned.
*/
static inline int luaA_dostring_named(lua_State* L, const char* src, const char* chunkname) {
    size_t bcLen = 0;
    // Always use free() on the pointer returned by luau_compile.
    char* bc = luau_compile(src, strlen(src), NULL, &bcLen);
    int status = luau_load(L, chunkname, bc, bcLen, 0);
    free(bc);
    if (status != LUA_OK) {
        printf("  [load err] %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
        return status;
    }
    status = lua_pcall(L, 0, LUA_MULTRET, 0);
    if (status != LUA_OK) {
        printf("  [runtime err] %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
    }
    return status;
}

/*
Luau has no `luaL_dostring` / `luaL_loadfile` - this acts as a drop-in replacement
accordingly.

On LUA_OK any values returned by the chunk are left on the stack.
On failure the error is printed; popped from the stack; and the status returned.
*/
static inline int luaA_dostring(lua_State* L, const char* src) {
    return luaA_dostring_named(L, src, "=[C]");
}

/*
Luau has no `luaL_loadfile` - this acts as a drop-in replacement accordingly.
This function attempts to follow in the semantic footsteps of Lua 5.1:
  - `filename == NULL` reads from stdin.
  - The first line is silently ignored if it begins with a '#' shebang.
  - On success, the compiled chunk is pushed as a function; LUA_OK is returned.
  - On failure, an error message is pushed and the error code is returned.
    Unlike `luaA_dostring`, this does **NOT** call `pcall` — callers do that themselves.
*/
static inline int luaA_loadfile(lua_State* L, const char* filename) {
    FILE*       f              = NULL;
    int         close_f        = 0;
    const char* chunkname;
    char*       chunkname_heap = NULL;

    if (filename == NULL) {
        f         = stdin;
        chunkname = "=stdin";
    } else {
        f = fopen(filename, "rb");
        if (!f) {
            lua_pushfstring(L, "cannot open %s: %s", filename, strerror(errno));
            return LUA_ERRFILE;
        }
        close_f = 1;
        /* '@' prefix signals to Lua's debug system that this is a file path */
        size_t fnlen   = strlen(filename);
        chunkname_heap = (char*)malloc(fnlen + 2);
        if (!chunkname_heap) {
            fclose(f);
            lua_pushliteral(L, "not enough memory");
            return LUA_ERRMEM;
        }
        chunkname_heap[0] = '@';
        memcpy(chunkname_heap + 1, filename, fnlen + 1);
        chunkname = chunkname_heap;
    }

    /* Lua 5.1 §3: ignore the first line if it starts with '#' (shebang) */
    {
        int c = fgetc(f);
        if (c == '#') {
            while ((c = fgetc(f)) != EOF && c != '\n') {}
        } else if (c != EOF) {
            ungetc(c, f);
        }
    }

    /* Read entire file into a growable heap buffer */
    size_t cap = 8192;
    size_t len = 0;
    char*  src = (char*)malloc(cap);
    if (!src) {
        if (close_f) fclose(f);
        free(chunkname_heap);
        lua_pushliteral(L, "not enough memory");
        return LUA_ERRMEM;
    }
    {
        size_t nr;
        while ((nr = fread(src + len, 1, cap - len, f)) > 0) {
            len += nr;
            if (len == cap) {
                cap *= 2;
                char* tmp = (char*)realloc(src, cap);
                if (!tmp) {
                    free(src);
                    if (close_f) fclose(f);
                    free(chunkname_heap);
                    lua_pushliteral(L, "not enough memory");
                    return LUA_ERRMEM;
                }
                src = tmp;
            }
        }
    }
    if (close_f) fclose(f);

    /* Compile source → Luau bytecode, then load as a callable chunk */
    size_t bcLen = 0;
    char*  bc    = luau_compile(src, len, NULL, &bcLen);
    free(src);
    int status = luau_load(L, chunkname, bc, bcLen, 0);
    free(bc);
    free(chunkname_heap); /* free(NULL) is a safe no-op */

    return status;
}

#endif
