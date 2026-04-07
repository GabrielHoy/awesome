/*
 * compat/lauxlib.h — Forwards <lauxlib.h> (standard Lua name) to Luau's
 * equivalent <lualib.h>.  lgi and other Lua C extensions include the former;
 * Luau ships the latter.
 */
#pragma once
#include <lualib.h>
