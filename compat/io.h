/*
 * compat/io.h — Minimal Lua 5.1-compatible `io` library for Luau.
 *
 * Luau omits the entire `io` library.  This reimplements the full subset used
 * by AwesomeWM's Lua library (`lib/`), with 1:1 parity to the Lua 5.1 spec:
 *
 *   io.open(path [, mode])       — fopen(); returns file handle or nil, errmsg
 *   io.popen(cmd [, mode])       — popen(); returns file handle or nil, errmsg
 *   io.close([file])             — close file, or default output if omitted
 *   io.input([file])             — get or set the default input file
 *   io.output([file])            — get or set the default output file
 *   io.flush()                   — flush the default output file
 *   io.read(...)                 — read from the default input file
 *   io.write(...)                — write to the default output file
 *   io.lines([filename])         — line iterator (auto-closes if filename given)
 *   io.tmpfile()                 — create a temporary file (w+b mode)
 *   io.type(obj)                 — "file" / "closed file" / nil
 *   io.stdin, io.stdout, io.stderr  — standard stream handles (never closed)
 *
 * File handle methods (returned by open/popen):
 *   file:read([fmt, ...])        — "*l"/"*L"/"*n"/"*a" or number of bytes
 *   file:write(str, ...)         — write strings/numbers; returns file handle
 *   file:close()                 — close the handle
 *   file:flush()                 — flush write buffer
 *   file:seek([whence [,offset]])— whence: "set"/"cur"/"end"; returns position
 *   file:setvbuf(mode [, size])  — buffering: "no" / "full" / "line"
 *   file:lines()                 — line iterator (does not close on EOF)
 *
 * Call luaA_io_open(L) once during VM initialisation (from luaA_fixups).
 * It registers the `io` global and seeds the default input/output to
 * io.stdin and io.stdout respectively.
 *
 * Copyright © AwesomeWM Contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef AWESOME_COMPAT_IO_H
#define AWESOME_COMPAT_IO_H

#include <lua.h>
#include <stdio.h>

/* Metatable name for file handle userdata */
#define LUAU_IO_FILE_META "awesome.io.file"

/*
 * File handle userdata.  is_popened encodes three states:
 *  0  — opened via fopen(); close with fclose()
 *  1  — opened via popen(); close with pclose()
 * -1  — a standard stream (stdin/stdout/stderr); never close
 */
typedef struct {
    FILE *file;      /* NULL = already closed */
    int   is_popened;
} AwesomeIOFile;

/* Registers the `io` global, all functions, and standard stream handles. */
void luaA_io_open(lua_State *L);

#endif /* AWESOME_COMPAT_IO_H */
