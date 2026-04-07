/*
 * compat/package.h — Lua 5.x-compatible `package` global for Luau.
 *
 * Luau has no `package` global.  This header recreates it with semantics
 * equivalent to Lua 5.2's package library:
 *
 *   package.loaded    — loaded-module cache (unified with Luau's own _LOADED
 *                       registry key so the two caches are one table)
 *   package.preload   — table of pre-provided loader functions
 *   package.path      — ';'-delimited Lua source file search path
 *   package.cpath     — ';'-delimited C module search path
 *   package.searchers — array of searcher functions (Lua 5.2+ name)
 *   package.loaders   — alias for package.searchers (Lua 5.1 compat)
 *
 * Three built-in searchers are registered:
 *   [1] preload searcher  — checks package.preload[modname]
 *   [2] Lua file searcher — searches package.path with '?' substitution,
 *                           loading matching files via luaA_loadfile()
 *   [3] C ext searcher    — searches package.cpath with '?' substitution,
 *                           dlopen()s the file, and calls luaopen_<modname>
 *                           (full dots-to-underscores name first, then last
 *                           component only, matching Lua 5.1/5.2 conventions)
 *
 * package.path and package.cpath are initialised to empty strings; they are
 * populated immediately after by add_to_search_path() in luaA_init().
 *
 * Call luaA_package_open(L) once during VM initialisation, before any
 * require() calls are made.
 *
 * Copyright © AwesomeWM Contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef AWESOME_COMPAT_PACKAGE_H
#define AWESOME_COMPAT_PACKAGE_H

#include <lua.h>

/*
 * The registry key used to share the loaded-module cache between
 * package.loaded and any Luau-internal _LOADED references.
 * We expose this so compat/require.c can access the same table without
 * going through the package global.
 */
#define LUAU_PACKAGE_LOADED_KEY "_LOADED"

void luaA_package_open(lua_State *L);

#endif /* AWESOME_COMPAT_PACKAGE_H */
