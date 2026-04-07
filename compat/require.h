/*
 * compat/require.h — Lua 5.x-compatible require() for Luau.
 *
 * Replaces Luau's built-in require() with a complete re-implementation that
 * honours the package global created by compat/package.c.
 *
 * Algorithm (mirrors Lua 5.2 §6.3):
 *   1. Check package.loaded[modname] — return immediately if cached.
 *   2. Iterate package.searchers in order.  Each searcher(modname) returns
 *      either a loader function (found) or a hint string (not found).
 *   3. Call the loader(modname); store the result in package.loaded[modname].
 *      A nil return is normalised to true (Lua 5.x compat).
 *   4. If no searcher returned a loader, raise an error listing all hints.
 *
 * A sentinel (true) is stored in package.loaded[modname] before the loader
 * is called so that circular requires return true rather than looping
 * infinitely.  If the loader returns a non-nil value it replaces the sentinel.
 *
 * Call luaA_require_open(L) after luaA_package_open(L) during VM init.
 *
 * Copyright © AwesomeWM Contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef AWESOME_COMPAT_REQUIRE_H
#define AWESOME_COMPAT_REQUIRE_H

#include <lua.h>

void luaA_require_open(lua_State *L);

#endif /* AWESOME_COMPAT_REQUIRE_H */
