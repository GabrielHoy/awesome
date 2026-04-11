/*
 * compat/package.c — Lua 5.x-compatible `package` global for Luau.
 */

#include "package.h"
#include "luau_compat.h"

#include <lua.h>
#include <lualib.h>
#include <dlfcn.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --------------------------------------------------------------------------
 * Internal helpers
 * -------------------------------------------------------------------------- */

/*
 * Convert a Lua module name to a filesystem sub-path by replacing every '.'
 * with '/'.  Writes into buf (capacity bufsz, including NUL).
 * Returns buf on success, NULL if name would overflow the buffer.
 *
 * Example: "awful.rules" -> "awful/rules"
 */
static const char *
luaA_modname_to_path(const char *name, char *buf, size_t bufsz)
{
    size_t i = 0;
    for (; *name && i < bufsz - 1; name++, i++) {
        buf[i] = (*name == '.') ? '/' : *name;
    }
    if (*name) {
        return NULL; /* overflow */
    }
    buf[i] = '\0';
    return buf;
}

/* --------------------------------------------------------------------------
 * Searcher 1: package.preload
 *
 * Checks package.preload[modname].  Returns the loader function on success,
 * or a hint string ("\n\tno field package.preload['name']") on failure.
 * -------------------------------------------------------------------------- */
static int
luaA_searcher_preload(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1);

    lua_getglobal(L, "package");      /* [name, package]         */
    lua_getfield(L, -1, "preload");   /* [name, package, preload] */
    lua_remove(L, -2);                /* [name, preload]          */
    lua_getfield(L, -1, name);        /* [name, preload, result]  */

    if (lua_isnil(L, -1)) {
        lua_pop(L, 2); /* drop nil + preload */
        lua_pushfstring(L, "\n\tno field package.preload['%s']", name);
        return 1;
    }

    lua_remove(L, -2); /* drop preload, leave loader */
    return 1;
}

/* --------------------------------------------------------------------------
 * Searcher 2: package.path  (Lua source files)
 *
 * Iterates ';'-separated entries in package.path.  For each entry, replaces
 * '?' with the dot-to-slash converted module name and tries to open the
 * resulting path.  On the first match, loads the file via luaA_loadfile()
 * and returns the resulting chunk (or a load-error string).
 * If no entry matches, returns a concatenated string of "no file '...'" hints.
 * -------------------------------------------------------------------------- */
static int
luaA_searcher_lua(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1);
    int base = lua_gettop(L); /* = 1; used as absolute anchor for path string */

    /* Convert module name to path form */
    char modpath[512];
    if (!luaA_modname_to_path(name, modpath, sizeof(modpath))) {
        lua_pushliteral(L, "\n\tmodule name too long");
        return 1;
    }
    size_t modpathlen = strlen(modpath);

    /* Fetch package.path onto the stack (index base+1) */
    lua_getglobal(L, "package");
    lua_getfield(L, -1, "path");
    lua_remove(L, -2); /* drop package, keep path string */

    if (!lua_isstring(L, -1)) {
        lua_pop(L, 1);
        lua_pushliteral(L, "\n\tpackage.path is not a string");
        return 1;
    }

    size_t pathlen;
    const char *path = lua_tolstring(L, -1, &pathlen);

    /*
     * Walk the ';'-separated path entries.  Accumulate "no file '...'" hints
     * in a C heap buffer so we can return them all at once without polluting
     * the Lua stack with partial results.
     */
    size_t errlen = 0, errcap = 256;
    char  *errbuf = (char *)malloc(errcap);
    if (!errbuf) {
        lua_pop(L, 1);
        lua_pushliteral(L, "\n\tout of memory in package searcher");
        return 1;
    }
    errbuf[0] = '\0';

    const char *p       = path;
    const char *pathend = path + pathlen;

    while (p <= pathend) {
        const char *sep      = (const char *)memchr(p, ';', (size_t)(pathend - p));
        size_t      entrylen = sep ? (size_t)(sep - p) : (size_t)(pathend - p);
        const char *q        = (const char *)memchr(p, '?', entrylen);

        if (q) {
            /* Build the candidate filepath by substituting '?' with modpath */
            size_t prefixlen = (size_t)(q - p);
            size_t suffixlen = entrylen - prefixlen - 1;
            size_t total     = prefixlen + modpathlen + suffixlen;
            char  *filepath  = (char *)malloc(total + 1);

            if (filepath) {
                memcpy(filepath,                              p,       prefixlen);
                memcpy(filepath + prefixlen,                 modpath, modpathlen);
                memcpy(filepath + prefixlen + modpathlen,    q + 1,   suffixlen);
                filepath[total] = '\0';

                /* Cheap existence check before invoking the full compiler */
                FILE *f = fopen(filepath, "rb");
                if (f) {
                    fclose(f);
                    luaA_loadfile(L, filepath); /* pushes chunk (success) or error string */
                    free(filepath);
                    free(errbuf);
                    lua_remove(L, base + 1); /* remove path string below new value */
                    return 1; /* return chunk (success) or error string (load failure) */
                }

                /* File not present — append a hint to errbuf */
                const char *pfx    = "\n\tno file '";
                const char *sfx    = "'";
                size_t      needed = errlen + strlen(pfx) + total + strlen(sfx) + 1;
                if (needed > errcap) {
                    size_t newcap = needed * 2;
                    char *tmp = (char *)realloc(errbuf, newcap);
                    if (tmp) { errbuf = tmp; errcap = newcap; }
                }
                if (errlen + strlen(pfx) + total + strlen(sfx) < errcap) {
                    strcat(errbuf, pfx);
                    strcat(errbuf, filepath);
                    strcat(errbuf, sfx);
                    errlen = strlen(errbuf);
                }
                free(filepath);
            }
        }

        p = sep ? sep + 1 : pathend + 1;
    }

    /* Nothing matched */
    lua_pop(L, 1); /* pop path string */
    if (errlen == 0)
        lua_pushfstring(L, "\n\tno file matching '%s' found in package.path", name);
    else
        lua_pushstring(L, errbuf);
    free(errbuf);
    return 1;
}

/* --------------------------------------------------------------------------
 * Searcher 3: package.cpath  (C shared libraries via dlopen/dlsym)
 *
 * Walks ';'-separated entries in package.cpath.  For each entry, '?' is
 * substituted with the dot-to-slash module name (e.g. "lgi.core" → "lgi/core")
 * and dlopen() is attempted.  On success the open function is looked up as:
 *   1. luaopen_<full>   where <full>  = modname with every '.' → '_'
 *      (Lua 5.2+ convention; e.g. "lgi.core" → luaopen_lgi_core)
 *   2. luaopen_<short>  where <short> = everything after the last '.'
 *      (Lua 5.1 convention; e.g. "lgi.core" → luaopen_core)
 * The first symbol found is pushed as a lua_CFunction.
 * The dlopen handle is intentionally never closed — C modules must remain
 * resident for the lifetime of the Lua VM.
 * On failure the function returns a multi-line hint string (no file / no symbol).
 * -------------------------------------------------------------------------- */

/*
 * Build the two luaopen_ symbol names for a module.
 *
 *   modname = "lgi.core"
 *   full    = "luaopen_lgi_core"   (all dots → underscores)
 *   shrt    = "luaopen_core"       (last component only)
 *
 * Returns true on success; false if either name would overflow its buffer.
 */
static bool
luaA_build_luaopen_symbols(const char *modname,
                            char *full, size_t fullsz,
                            char *shrt, size_t shrtsz)
{
    static const char prefix[] = "luaopen_";
    const size_t      plen     = sizeof(prefix) - 1; /* strlen("luaopen_") */
    const size_t      mlen     = strlen(modname);

    /* Full symbol: "luaopen_" + modname with '.' → '_' */
    if (plen + mlen + 1 > fullsz)
        return false;
    memcpy(full, prefix, plen);
    for (size_t i = 0; i < mlen; i++)
        full[plen + i] = (modname[i] == '.') ? '_' : modname[i];
    full[plen + mlen] = '\0';

    /* Short symbol: "luaopen_" + everything after the last '.' */
    const char *lastdot   = strrchr(modname, '.');
    const char *shortpart = lastdot ? lastdot + 1 : modname;
    const size_t slen     = strlen(shortpart);
    if (plen + slen + 1 > shrtsz)
        return false;
    memcpy(shrt, prefix, plen);
    memcpy(shrt + plen, shortpart, slen + 1);

    return true;
}

static int
luaA_searcher_c(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1);

    /* dot-to-slash conversion: "lgi.core" → "lgi/core" */
    char modpath[512];
    if (!luaA_modname_to_path(name, modpath, sizeof(modpath))) {
        lua_pushliteral(L, "\n\tmodule name too long");
        return 1;
    }
    size_t modpathlen = strlen(modpath);

    /* Build the two candidate symbol names */
    char full_sym[256], short_sym[256];
    if (!luaA_build_luaopen_symbols(name, full_sym, sizeof(full_sym),
                                     short_sym, sizeof(short_sym))) {
        lua_pushliteral(L, "\n\tmodule name too long for symbol construction");
        return 1;
    }

    /* Fetch package.cpath onto the stack (index base+1) */
    lua_getglobal(L, "package");
    lua_getfield(L, -1, "cpath");
    lua_remove(L, -2); /* drop package, keep cpath string */

    if (!lua_isstring(L, -1)) {
        lua_pop(L, 1);
        lua_pushliteral(L, "\n\tpackage.cpath is not a string");
        return 1;
    }

    size_t pathlen;
    const char *path = lua_tolstring(L, -1, &pathlen);

    /* Accumulate "no file / no symbol" hints into a growing heap buffer */
    size_t errlen = 0, errcap = 256;
    char  *errbuf = (char *)malloc(errcap);
    if (!errbuf) {
        lua_pop(L, 1);
        lua_pushliteral(L, "\n\tout of memory in C extension searcher");
        return 1;
    }
    errbuf[0] = '\0';

    /* Helper: append a formatted hint to errbuf, growing as needed */
#define APPEND_HINT(fmt, ...) \
    do { \
        int need_ = snprintf(NULL, 0, fmt, ##__VA_ARGS__) + 1; \
        if ((size_t)(errlen + need_) > errcap) { \
            size_t newcap_ = (errlen + (size_t)need_) * 2; \
            char *tmp_ = (char *)realloc(errbuf, newcap_); \
            if (tmp_) { errbuf = tmp_; errcap = newcap_; } \
        } \
        if ((size_t)(errlen + need_) <= errcap) { \
            errlen += (size_t)snprintf(errbuf + errlen, errcap - errlen, fmt, ##__VA_ARGS__); \
        } \
    } while (0)

    const char *p       = path;
    const char *pathend = path + pathlen;

    while (p <= pathend) {
        const char *sep      = (const char *)memchr(p, ';', (size_t)(pathend - p));
        size_t      entrylen = sep ? (size_t)(sep - p) : (size_t)(pathend - p);
        const char *q        = (const char *)memchr(p, '?', entrylen);

        if (q) {
            /* Substitute '?' with the dot-to-slash module path */
            size_t prefixlen = (size_t)(q - p);
            size_t suffixlen = entrylen - prefixlen - 1;
            size_t total     = prefixlen + modpathlen + suffixlen;
            char  *filepath  = (char *)malloc(total + 1);

            if (filepath) {
                memcpy(filepath,                           p,       prefixlen);
                memcpy(filepath + prefixlen,               modpath, modpathlen);
                memcpy(filepath + prefixlen + modpathlen,  q + 1,   suffixlen);
                filepath[total] = '\0';

                /*
                 * Use RTLD_NOW  — resolve all symbols immediately so we get a
                 * clear error here rather than a crash later.
                 * Use RTLD_GLOBAL — makes the loaded library's symbols visible to
                 * subsequently loaded modules, which is required for multi-part
                 * C extension packages (e.g. lgi's main .so + lgi.core).
                 * This matches standard Lua 5.1's dlopen flags on Linux.
                 */
                void *handle = dlopen(filepath, RTLD_NOW | RTLD_GLOBAL);
                if (handle) {
                    /* Try full symbol first (Lua 5.2+ convention), then short */
                    lua_CFunction openfn = NULL;
                    *(void **)(&openfn) = dlsym(handle, full_sym);
                    if (!openfn)
                        *(void **)(&openfn) = dlsym(handle, short_sym);

                    if (openfn) {
                        /* Success: push the open function.  Handle intentionally
                         * leaks — the module must stay resident. */
                        free(errbuf);
                        free(filepath);
                        lua_pop(L, 1); /* pop cpath string (index base+1) */
                        luaA_pushcfunction(L, openfn);
                        return 1;
                    }

                    /* File opened but neither symbol was found */
                    APPEND_HINT("\n\tno symbol '%s' or '%s' in '%s'",
                                full_sym, short_sym, filepath);
                    dlclose(handle);
                } else {
                    APPEND_HINT("\n\tno file '%s'", filepath);
                }

                free(filepath);
            }
        }

        p = sep ? sep + 1 : pathend + 1;
    }

#undef APPEND_HINT

    lua_pop(L, 1); /* pop cpath string */
    if (errlen == 0)
        lua_pushfstring(L, "\n\tno C extension matching '%s' found in package.cpath", name);
    else
        lua_pushstring(L, errbuf);
    free(errbuf);
    return 1;
}

/* --------------------------------------------------------------------------
 * luaA_package_open(L)
 *
 * Creates and registers the `package` global.  Safe to call after
 * luaL_openlibs(); it merges with Luau's existing _LOADED registry table if
 * one is already present rather than replacing it.
 * -------------------------------------------------------------------------- */
void
luaA_package_open(lua_State *L)
{
    /*
     * Obtain (or create) the _LOADED registry table and keep it on the stack.
     * Making package.loaded point at the same underlying table ensures that
     * modules cached by Luau's own base lib and modules cached by our require()
     * shim share a single source of truth.
     */
    lua_getfield(L, LUA_REGISTRYINDEX, LUAU_PACKAGE_LOADED_KEY); /* [loaded] */
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);                          /* [loaded]        */
        lua_pushvalue(L, -1);                     /* [loaded, loaded] */
        lua_setfield(L, LUA_REGISTRYINDEX, LUAU_PACKAGE_LOADED_KEY);
        /* stack: [loaded] */
    }

    lua_newtable(L);  /* the package table; stack: [loaded, pkg] */

    /* package.loaded = shared _LOADED table */
    lua_pushvalue(L, -2);          /* [loaded, pkg, loaded] */
    lua_setfield(L, -2, "loaded"); /* pkg.loaded = loaded; [loaded, pkg] */

    /* package.preload = {} */
    lua_newtable(L);
    lua_setfield(L, -2, "preload");

    /* package.path / package.cpath — filled in by add_to_search_path() */
    lua_pushliteral(L, "");
    lua_setfield(L, -2, "path");
    lua_pushliteral(L, "");
    lua_setfield(L, -2, "cpath");

    /*
     * package.searchers = { searcher_preload, searcher_lua }
     * package.loaders   = package.searchers  (Lua 5.1 compat alias)
     */
    lua_newtable(L);                                    /* [loaded, pkg, searchers] */
    luaA_pushcfunction(L, luaA_searcher_preload);
    lua_rawseti(L, -2, 1);
    luaA_pushcfunction(L, luaA_searcher_lua);
    lua_rawseti(L, -2, 2);
    luaA_pushcfunction(L, luaA_searcher_c);
    lua_rawseti(L, -2, 3);

    lua_pushvalue(L, -1);              /* [loaded, pkg, searchers, searchers] */
    lua_setfield(L, -3, "searchers");  /* pkg.searchers = searchers; [loaded, pkg, searchers] */
    lua_setfield(L, -2, "loaders");    /* pkg.loaders   = searchers; [loaded, pkg] */

    lua_setglobal(L, "package");       /* _G.package = pkg; [loaded] */

    /*
     * Seed _LOADED with built-in modules so that `require('package')`,
     * `require('io')`, etc. return the corresponding globals — matching
     * Lua 5.1 behaviour.  lgi/init.lua line 16 does `require 'package'`
     * which would fail without this.
     */
    static const char *const builtins[] = {
        "package", "io", "os", "string", "table", "math",
        "coroutine", "debug", NULL
    };
    for (int i = 0; builtins[i]; i++) {
        lua_getglobal(L, builtins[i]);   /* [loaded, module_or_nil] */
        if (!lua_isnil(L, -1)) {
            lua_setfield(L, -2, builtins[i]); /* loaded[name] = module */
        }
        else {
            lua_pop(L, 1);
        }
    }

    lua_pop(L, 1);                     /* [] */
}
