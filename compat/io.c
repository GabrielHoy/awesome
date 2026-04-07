/*
 * compat/io.c — Minimal Lua 5.1-compatible `io` library for Luau.
 *
 * Copyright © AwesomeWM Contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "io.h"

#include <lua.h>
#include <lualib.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* is_popened value for standard streams — should never be fclose/pclose'd */
#define IO_STD_STREAM (-1)

/*
 * Luau userdata tag for AwesomeIOFile handles.
 *
 * Luau does not support the __gc metamethod.  Instead it uses tag-based
 * destructors registered via lua_setuserdatadtor().  Each tag (0..LUA_UTAG_LIMIT-1)
 * maps to one destructor that fires just before the userdata memory is freed.
 *
 * Tag 0 is reserved for the general luaclass object system (lua_newuserdata
 * is #define'd to lua_newuserdatatagged(L, s, 0) in Luau).  Tag 1 is
 * reserved for lgi userdata (LGI_UDATA_TAG in lgi_luau_shim.h).  We use
 * tag 2 for AwesomeIOFile so our destructor fires only on our handles.
 */
#define LUAU_IO_FILE_TAG 2

/*
 * Lua registry keys for the mutable default input/output files.
 * io.input() and io.output() read/write these; io.read(), io.write(),
 * io.lines(), io.flush(), and io.close() all operate through them.
 */
#define LUAU_IO_DEFAULT_INPUT  "awesome.io.defin"
#define LUAU_IO_DEFAULT_OUTPUT "awesome.io.defout"

/* =========================================================================
 * Internal helpers
 * ====================================================================== */

/*
 * Allocate a new AwesomeIOFile userdata, attach the file metatable, and push
 * it onto the stack.  Callers must fill in h->file and h->is_popened.
 */
static AwesomeIOFile *
luaA_io_newfile(lua_State *L)
{
    AwesomeIOFile *h = (AwesomeIOFile *)lua_newuserdatatagged(L, sizeof(AwesomeIOFile), LUAU_IO_FILE_TAG);
    h->file       = NULL;
    h->is_popened = 0;
    luaL_getmetatable(L, LUAU_IO_FILE_META);
    lua_setmetatable(L, -2);
    return h;
}

/* Get a checked, open file handle from stack position idx.  Raises a Lua
 * error if the object is not a file handle or if the file is already closed. */
static AwesomeIOFile *
luaA_io_checkfile(lua_State *L, int idx)
{
    AwesomeIOFile *h = (AwesomeIOFile *)luaL_checkudata(L, idx, LUAU_IO_FILE_META);
    if (!h->file) {
        luaL_error(L, "attempt to use a closed file");
        return NULL; /* unreachable; satisfies compiler */
    }
    return h;
}

/*
 * Close the underlying FILE* and set h->file = NULL.
 * Returns 0 on success for fopen files, -1 on system error for popen files,
 * or the child exit status (>= 0) for popen files.
 * No-op for already-closed or standard-stream handles.
 */
static int
luaA_io_closefile(AwesomeIOFile *h)
{
    if (!h->file || h->is_popened == IO_STD_STREAM)
        return 0;
    FILE *f = h->file;
    h->file = NULL;
    return h->is_popened ? pclose(f) : fclose(f);
}

/*
 * Shared close logic used by both file:close() and io.close().
 *
 * For fopen files:  fclose() == 0 is success.
 * For popen files:  pclose() == -1 is a system error; any other return value
 *   (including non-zero child exit codes) is treated as success, matching
 *   Lua 5.1 semantics where lua_pclose returns (pclose(f) != -1).
 * Standard streams: cannot be closed; returns nil + errmsg.
 */
static int
luaA_io_do_close(lua_State *L, AwesomeIOFile *h)
{
    if (h->is_popened == IO_STD_STREAM) {
        lua_pushnil(L);
        lua_pushliteral(L, "cannot close standard streams");
        return 2;
    }
    if (!h->file) {
        /* Already closed — idempotent success */
        lua_pushboolean(L, 1);
        return 1;
    }
    int ret = luaA_io_closefile(h);
    if (h->is_popened == 1) {
        /* popen: -1 means system error; >= 0 (incl. non-zero exit) = success */
        if (ret == -1) {
            lua_pushnil(L);
            lua_pushstring(L, strerror(errno));
            return 2;
        }
        lua_pushboolean(L, 1);
        return 1;
    }
    /* fopen: 0 is success */
    if (ret == 0) {
        lua_pushboolean(L, 1);
        return 1;
    }
    lua_pushnil(L);
    lua_pushstring(L, strerror(errno));
    return 2;
}

/* =========================================================================
 * Default input/output helpers
 * ====================================================================== */

/*
 * Get the FILE* for the current default input or output from the registry.
 * Does NOT push anything onto the stack.
 * Raises a Lua error if the default handle is closed.
 */
static FILE *
luaA_io_getdefault_file(lua_State *L, const char *regkey)
{
    lua_pushstring(L, regkey);
    lua_rawget(L, LUA_REGISTRYINDEX);
    AwesomeIOFile *h = (AwesomeIOFile *)lua_touserdata(L, -1);
    lua_pop(L, 1);
    if (!h || !h->file)
        luaL_error(L, "default file is closed");
    return h->file;
}

/*
 * Store the AwesomeIOFile userdata currently at the top of the stack as the
 * default input or output in the registry.  The handle remains at the top
 * after this call (so the caller can return 1 to return it to Lua).
 */
static void
luaA_io_setdefault(lua_State *L, const char *regkey)
{
    lua_pushstring(L, regkey);  /* [..., handle, key]        */
    lua_pushvalue(L, -2);       /* [..., handle, key, handle] */
    lua_rawset(L, LUA_REGISTRYINDEX); /* registry[key]=handle; [..., handle] */
}

/* =========================================================================
 * Read helpers — shared by file:read() and io.read()
 * ====================================================================== */

/*
 * Read one line from f.  chop==1 strips trailing newline ("*l");
 * chop==0 keeps it ("*L").
 * Returns 1 and pushes the string on success; 0 at EOF with nothing pushed.
 */
static int
io_readline(lua_State *L, FILE *f, int chop)
{
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    int c, any = 0;
    while ((c = fgetc(f)) != EOF) {
        any = 1;
        if (c == '\n') {
            if (!chop)
                luaL_addchar(&b, '\n');
            break;
        }
        luaL_addchar(&b, (char)c);
    }
    if (!any && c == EOF)
        return 0;
    luaL_pushresult(&b);
    return 1;
}

/*
 * Read up to n bytes from f.
 *
 * n == 0: Per Lua 5.1 spec, returns "" if not at EOF, nil if at EOF.
 *         Uses fgetc/ungetc to peek without consuming a byte.
 * n >  0: Returns a (possibly shorter) string, or nil if nothing could be
 *         read (EOF).
 */
static int
io_readbytes(lua_State *L, FILE *f, size_t n)
{
    if (n == 0) {
        int c = fgetc(f);
        if (c == EOF) {
            lua_pushnil(L);
        } else {
            ungetc(c, f);
            lua_pushliteral(L, "");
        }
        return 1;
    }
    char *buf = (char *)malloc(n);
    if (!buf) {
        luaL_error(L, "not enough memory");
        return 1; /* unreachable */
    }
    size_t nr = fread(buf, 1, n, f);
    if (nr == 0) {
        free(buf);
        lua_pushnil(L);
    } else {
        lua_pushlstring(L, buf, nr);
        free(buf);
    }
    return 1;
}

/*
 * Read all remaining content of f and push as a string.
 * Returns "" at EOF, matching Lua 5.1 ("*a" never returns nil).
 */
static void
io_readall(lua_State *L, FILE *f)
{
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    char   tmp[8192];
    size_t nr;
    while ((nr = fread(tmp, 1, sizeof(tmp), f)) > 0)
        luaL_addlstring(&b, tmp, nr);
    luaL_pushresult(&b);
}

/*
 * Process one read-format argument at stack index idx; reads from f.
 * Pushes one value (string, number, or nil); returns 1.
 *
 * Accepted formats (Lua 5.1 "*x" prefix and bare "x" both work):
 *   "l"/"*l"  — line without trailing newline (default)
 *   "L"/"*L"  — line with trailing newline
 *   "n"/"*n"  — number (via fscanf)
 *   "a"/"*a"  — all remaining content (returns "" at EOF, never nil)
 *   <integer> — that many raw bytes ("" if 0 and not EOF, nil if 0 and EOF)
 */
static int
io_readfmt(lua_State *L, FILE *f, int idx)
{
    if (lua_type(L, idx) == LUA_TNUMBER) {
        size_t n = (size_t)lua_tointeger(L, idx);
        return io_readbytes(L, f, n);
    }

    const char *fmt = luaL_checkstring(L, idx);
    if (*fmt == '*') fmt++;          /* accept both "*l" and "l" */

    switch (*fmt) {
    case 'l':
        if (!io_readline(L, f, 1)) lua_pushnil(L);
        return 1;
    case 'L':
        if (!io_readline(L, f, 0)) lua_pushnil(L);
        return 1;
    case 'n': {
        double num;
        if (fscanf(f, "%lf", &num) == 1)
            lua_pushnumber(L, (lua_Number)num);
        else
            lua_pushnil(L);
        return 1;
    }
    case 'a':
        io_readall(L, f);
        return 1;
    default:
        luaL_argerror(L, idx, "invalid format");
        return 0; /* unreachable */
    }
}

/* =========================================================================
 * File handle methods  (receiver is userdata at stack index 1)
 * ====================================================================== */

static int
luaA_file_read(lua_State *L)
{
    AwesomeIOFile *h = luaA_io_checkfile(L, 1);
    int n = lua_gettop(L) - 1;
    if (n == 0) {
        /* Default format: read one line without newline */
        if (!io_readline(L, h->file, 1)) lua_pushnil(L);
        return 1;
    }
    int total = 0;
    for (int i = 2; i <= n + 1; i++) {
        io_readfmt(L, h->file, i);
        total++;
    }
    return total;
}

static int
luaA_file_write(lua_State *L)
{
    AwesomeIOFile *h = luaA_io_checkfile(L, 1);
    int n = lua_gettop(L);
    for (int i = 2; i <= n; i++) {
        if (lua_type(L, i) == LUA_TSTRING) {
            size_t      len;
            const char *s = lua_tolstring(L, i, &len);
            if (fwrite(s, 1, len, h->file) < len) {
                lua_pushnil(L);
                lua_pushstring(L, strerror(errno));
                return 2;
            }
        } else if (lua_type(L, i) == LUA_TNUMBER) {
            if (fprintf(h->file, "%.14g", (double)lua_tonumber(L, i)) < 0) {
                lua_pushnil(L);
                lua_pushstring(L, strerror(errno));
                return 2;
            }
        } else {
            luaL_argerror(L, i, "string or number expected");
            return 0; /* unreachable */
        }
    }
    lua_pushvalue(L, 1); /* return handle for method chaining: f:write(a):write(b) */
    return 1;
}

static int
luaA_file_close(lua_State *L)
{
    AwesomeIOFile *h = (AwesomeIOFile *)luaL_checkudata(L, 1, LUAU_IO_FILE_META);
    return luaA_io_do_close(L, h);
}

static int
luaA_file_flush(lua_State *L)
{
    AwesomeIOFile *h = luaA_io_checkfile(L, 1);
    if (fflush(h->file) == 0) {
        lua_pushboolean(L, 1);
        return 1;
    }
    lua_pushnil(L);
    lua_pushstring(L, strerror(errno));
    return 2;
}

static int
luaA_file_seek(lua_State *L)
{
    static const int         whence_vals[] = { SEEK_SET, SEEK_CUR, SEEK_END };
    static const char *const whence_opts[] = { "set", "cur", "end", NULL };

    AwesomeIOFile *h      = luaA_io_checkfile(L, 1);
    int            whence = whence_vals[luaL_checkoption(L, 2, "cur", whence_opts)];
    long           offset = (long)luaL_optinteger(L, 3, 0);

    if (fseek(h->file, offset, whence) != 0) {
        lua_pushnil(L);
        lua_pushstring(L, strerror(errno));
        return 2;
    }
    long pos = ftell(h->file);
    if (pos == -1L) {
        lua_pushnil(L);
        lua_pushstring(L, strerror(errno));
        return 2;
    }
    lua_pushinteger(L, (lua_Integer)pos);
    return 1;
}

/*
 * file:setvbuf(mode [, size]) — set the buffering mode for an output file.
 *
 * mode  C constant  behaviour
 * "no"   _IONBF     no buffering; output appears immediately
 * "full" _IOFBF     block-buffered until buffer full or explicit flush
 * "line" _IOLBF     line-buffered; flushed on newline or terminal input
 *
 * size (optional) is the buffer size in bytes; default is BUFSIZ.
 * Ignored for "no" mode.
 */
static int
luaA_file_setvbuf(lua_State *L)
{
    static const int         modes[]     = { _IONBF, _IOFBF, _IOLBF };
    static const char *const mode_opts[] = { "no", "full", "line", NULL };

    AwesomeIOFile *h    = luaA_io_checkfile(L, 1);
    int            mode = modes[luaL_checkoption(L, 2, NULL, mode_opts)];
    size_t         sz   = (size_t)luaL_optinteger(L, 3, BUFSIZ);

    if (setvbuf(h->file, NULL, mode, sz) != 0) {
        lua_pushnil(L);
        lua_pushstring(L, strerror(errno));
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

/*
 * Upvalue-based line iterator body, shared by file:lines() and io.lines().
 *
 * Upvalue 1: AwesomeIOFile userdata (keeps the handle alive via reference)
 * Upvalue 2: boolean — close the file on EOF (true for io.lines(filename))
 *
 * If the iterator is abandoned (break / goes out of scope), the closure is
 * GC'd, releasing its reference to upvalue 1.  The file userdata then becomes
 * collectable and its tag destructor (luaA_file_dtor) closes the underlying FILE*.
 */
static int
luaA_io_lines_iter(lua_State *L)
{
    AwesomeIOFile *h = (AwesomeIOFile *)lua_touserdata(L, lua_upvalueindex(1));
    if (!h->file) {
        lua_pushnil(L);
        return 1;
    }
    if (!io_readline(L, h->file, 1)) {
        if (lua_toboolean(L, lua_upvalueindex(2)))
            luaA_io_closefile(h);
        lua_pushnil(L);
        return 1;
    }
    return 1;
}

static int
luaA_file_lines(lua_State *L)
{
    luaA_io_checkfile(L, 1);             /* validate: open and correct type */
    lua_pushvalue(L, 1);                 /* upvalue 1: file userdata */
    lua_pushboolean(L, 0);               /* upvalue 2: don't close on EOF */
    lua_pushcclosure(L, luaA_io_lines_iter, "luaA_io_lines_iter", 2);
    return 1;
}

/*
 * Tag-based destructor — Luau's replacement for the __gc metamethod.
 *
 * Called by the GC immediately before the userdata memory is freed.
 * Receives the raw C pointer to the AwesomeIOFile struct (not a Lua value),
 * so we must NOT call back into the Lua API here — just do the C-level close.
 */
static void
luaA_file_dtor(lua_State *L, void *ud)
{
    (void)L;
    luaA_io_closefile((AwesomeIOFile *)ud);
}

/* __tostring metamethod */
static int
luaA_file_tostring(lua_State *L)
{
    AwesomeIOFile *h = (AwesomeIOFile *)luaL_checkudata(L, 1, LUAU_IO_FILE_META);
    if (h->file)
        lua_pushfstring(L, "file (%p)", (void *)h->file);
    else
        lua_pushliteral(L, "file (closed)");
    return 1;
}

/* =========================================================================
 * io module functions
 * ====================================================================== */

static int
luaA_io_openf(lua_State *L)
{
    const char    *path = luaL_checkstring(L, 1);
    const char    *mode = luaL_optstring(L, 2, "r");
    AwesomeIOFile *h    = luaA_io_newfile(L);
    h->file = fopen(path, mode);
    if (!h->file) {
        /* Match Lua 5.x: return nil, errmsg, errno */
        lua_pushnil(L);
        lua_pushstring(L, strerror(errno));
        lua_pushinteger(L, (lua_Integer)errno);
        return 3;
    }
    return 1;
}

static int
luaA_io_popen(lua_State *L)
{
    const char    *cmd  = luaL_checkstring(L, 1);
    const char    *mode = luaL_optstring(L, 2, "r");
    AwesomeIOFile *h    = luaA_io_newfile(L);
    h->is_popened = 1;
    h->file = popen(cmd, mode);
    if (!h->file) {
        lua_pushnil(L);
        lua_pushstring(L, strerror(errno));
        return 2;
    }
    return 1;
}

/*
 * io.close([file]) — Lua 5.1 spec: "Equivalent to file:close(). Without a
 * file, closes the default output file."
 */
static int
luaA_io_close(lua_State *L)
{
    if (lua_isnoneornil(L, 1)) {
        lua_pushstring(L, LUAU_IO_DEFAULT_OUTPUT);
        lua_rawget(L, LUA_REGISTRYINDEX);
        AwesomeIOFile *h = (AwesomeIOFile *)lua_touserdata(L, -1);
        lua_pop(L, 1);
        if (!h) {
            lua_pushboolean(L, 1);
            return 1;
        }
        return luaA_io_do_close(L, h);
    }
    return luaA_file_close(L);
}

/*
 * io.input([file]) — get or set the default input file.
 *
 * No arg / nil : returns current default input file handle.
 * String       : opens the named file in read mode ("r"), sets it as the
 *                new default input, and returns the new handle.
 * File handle  : validates the handle is open, sets it as the new default
 *                input, and returns it.
 *
 * Errors are raised (not returned) as required by Lua 5.1 spec.
 */
static int
luaA_io_input(lua_State *L)
{
    if (lua_isnoneornil(L, 1)) {
        lua_pushstring(L, LUAU_IO_DEFAULT_INPUT);
        lua_rawget(L, LUA_REGISTRYINDEX);
        return 1;
    }
    if (lua_type(L, 1) == LUA_TSTRING) {
        const char    *path = luaL_checkstring(L, 1);
        AwesomeIOFile *h    = luaA_io_newfile(L);
        h->file = fopen(path, "r");
        if (!h->file)
            luaL_error(L, "cannot open '%s': %s", path, strerror(errno));
    } else {
        luaA_io_checkfile(L, 1);  /* raises error if closed or wrong type */
        lua_pushvalue(L, 1);
    }
    luaA_io_setdefault(L, LUAU_IO_DEFAULT_INPUT);
    return 1;
}

/*
 * io.output([file]) — get or set the default output file.
 *
 * Mirrors io.input() but opens strings in write mode ("w").
 */
static int
luaA_io_output(lua_State *L)
{
    if (lua_isnoneornil(L, 1)) {
        lua_pushstring(L, LUAU_IO_DEFAULT_OUTPUT);
        lua_rawget(L, LUA_REGISTRYINDEX);
        return 1;
    }
    if (lua_type(L, 1) == LUA_TSTRING) {
        const char    *path = luaL_checkstring(L, 1);
        AwesomeIOFile *h    = luaA_io_newfile(L);
        h->file = fopen(path, "w");
        if (!h->file)
            luaL_error(L, "cannot open '%s': %s", path, strerror(errno));
    } else {
        luaA_io_checkfile(L, 1);
        lua_pushvalue(L, 1);
    }
    luaA_io_setdefault(L, LUAU_IO_DEFAULT_OUTPUT);
    return 1;
}

/*
 * io.flush() — "Equivalent to file:flush over the default output file."
 */
static int
luaA_io_flush(lua_State *L)
{
    FILE *f = luaA_io_getdefault_file(L, LUAU_IO_DEFAULT_OUTPUT);
    if (fflush(f) == 0) {
        lua_pushboolean(L, 1);
        return 1;
    }
    lua_pushnil(L);
    lua_pushstring(L, strerror(errno));
    return 2;
}

/*
 * io.read(...) — "Equivalent to io.input():read".
 * Reads from the current default input file.
 */
static int
luaA_io_read(lua_State *L)
{
    FILE *f = luaA_io_getdefault_file(L, LUAU_IO_DEFAULT_INPUT);
    int   n = lua_gettop(L);
    if (n == 0) {
        if (!io_readline(L, f, 1)) lua_pushnil(L);
        return 1;
    }
    int total = 0;
    for (int i = 1; i <= n; i++) {
        io_readfmt(L, f, i);
        total++;
    }
    return total;
}

/*
 * io.write(...) — "Equivalent to io.output():write".
 * Writes to the current default output file.
 * Returns the default output file handle on success, nil + errmsg on error.
 */
static int
luaA_io_write(lua_State *L)
{
    FILE *f = luaA_io_getdefault_file(L, LUAU_IO_DEFAULT_OUTPUT);
    int   n = lua_gettop(L);
    for (int i = 1; i <= n; i++) {
        if (lua_type(L, i) == LUA_TSTRING) {
            size_t      len;
            const char *s = lua_tolstring(L, i, &len);
            if (fwrite(s, 1, len, f) < len) {
                lua_pushnil(L);
                lua_pushstring(L, strerror(errno));
                return 2;
            }
        } else if (lua_type(L, i) == LUA_TNUMBER) {
            if (fprintf(f, "%.14g", (double)lua_tonumber(L, i)) < 0) {
                lua_pushnil(L);
                lua_pushstring(L, strerror(errno));
                return 2;
            }
        } else {
            luaL_argerror(L, i, "string or number expected");
            return 0; /* unreachable */
        }
    }
    /* Return the default output handle */
    lua_pushstring(L, LUAU_IO_DEFAULT_OUTPUT);
    lua_rawget(L, LUA_REGISTRYINDEX);
    return 1;
}

/*
 * io.lines([filename]) — returns a line iterator.
 *
 * With filename   : opens the file, closes it automatically at EOF or when
 *                   the iterator closure is GC'd (early abandonment).
 * Without filename: "equivalent to io.input():lines()" — iterates the current
 *                   default input file and does NOT close it on EOF.
 */
static int
luaA_io_lines(lua_State *L)
{
    if (lua_isnoneornil(L, 1)) {
        /* Use the default input handle as upvalue 1 */
        lua_pushstring(L, LUAU_IO_DEFAULT_INPUT);
        lua_rawget(L, LUA_REGISTRYINDEX);
        lua_pushboolean(L, 0);         /* upvalue 2: don't close on EOF */
    } else {
        const char    *path = luaL_checkstring(L, 1);
        AwesomeIOFile *h    = luaA_io_newfile(L);
        h->file = fopen(path, "r");
        if (!h->file) {
            luaL_error(L, "cannot open '%s': %s", path, strerror(errno));
            return 0; /* unreachable */
        }
        lua_pushboolean(L, 1);         /* upvalue 2: close on EOF */
    }
    lua_pushcclosure(L, luaA_io_lines_iter, "luaA_io_lines_iter", 2);
    return 1;
}

static int
luaA_io_tmpfile(lua_State *L)
{
    AwesomeIOFile *h = luaA_io_newfile(L);
    h->file = tmpfile();
    if (!h->file) {
        lua_pushnil(L);
        lua_pushstring(L, strerror(errno));
        return 2;
    }
    return 1;
}

/*
 * io.type(obj) — "file", "closed file", or nil.
 *
 * Lua 5.1 spec: returns nil (not false) for objects that are not file handles.
 */
static int
luaA_io_type(lua_State *L)
{
    if (lua_type(L, 1) != LUA_TUSERDATA ||
        lua_userdatatag(L, 1) != LUAU_IO_FILE_TAG) {
        lua_pushnil(L);
        return 1;
    }
    AwesomeIOFile *h = (AwesomeIOFile *)lua_touserdata(L, 1);
    lua_pushstring(L, h->file ? "file" : "closed file");
    return 1;
}

/* =========================================================================
 * Registration
 * ====================================================================== */

void
luaA_io_open(lua_State *L)
{
    /* ------------------------------------------------------------------
     * 0. Register the tag-based destructor for AwesomeIOFile userdata.
     *    This replaces Lua 5.1's __gc metamethod, which Luau does not support.
     *    Must be done before any file handle userdata is allocated so that
     *    even handles that are never explicitly closed get cleaned up.
     * ------------------------------------------------------------------ */
    lua_setuserdatadtor(L, LUAU_IO_FILE_TAG, luaA_file_dtor);

    /* ------------------------------------------------------------------
     * 1. Create the file handle metatable.
     *    __index = itself so that f:read() resolves via the metatable.
     * ------------------------------------------------------------------ */
    luaL_newmetatable(L, LUAU_IO_FILE_META);  /* [mt] */
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");           /* mt.__index = mt */

    struct { const char *name; lua_CFunction fn; } fmethods[] = {
        { "close",      luaA_file_close     },
        { "flush",      luaA_file_flush     },
        { "lines",      luaA_file_lines     },
        { "read",       luaA_file_read      },
        { "seek",       luaA_file_seek      },
        { "setvbuf",    luaA_file_setvbuf   },
        { "write",      luaA_file_write     },
        { "__tostring", luaA_file_tostring  },
        { NULL, NULL }
    };
    for (int i = 0; fmethods[i].name; i++) {
        lua_pushcfunction(L, fmethods[i].fn, fmethods[i].name);
        lua_setfield(L, -2, fmethods[i].name);
    }
    lua_pop(L, 1); /* pop metatable */

    /* ------------------------------------------------------------------
     * 2. Create the io table and register module-level functions.
     * ------------------------------------------------------------------ */
    lua_newtable(L);  /* [io] */

    struct { const char *name; lua_CFunction fn; } mfns[] = {
        { "close",   luaA_io_close   },
        { "flush",   luaA_io_flush   },
        { "input",   luaA_io_input   },
        { "lines",   luaA_io_lines   },
        { "open",    luaA_io_openf   },
        { "output",  luaA_io_output  },
        { "popen",   luaA_io_popen   },
        { "read",    luaA_io_read    },
        { "tmpfile", luaA_io_tmpfile },
        { "type",    luaA_io_type    },
        { "write",   luaA_io_write   },
        { NULL, NULL }
    };
    for (int i = 0; mfns[i].name; i++) {
        lua_pushcfunction(L, mfns[i].fn, mfns[i].name);
        lua_setfield(L, -2, mfns[i].name);
    }

    /* ------------------------------------------------------------------
     * 3. Standard stream handles + seed the default input/output.
     *
     *    The default input and output start as stdin and stdout per Lua 5.1.
     *    We seed the registry entries while the handles are still on the stack
     *    (before lua_setfield consumes them), avoiding a second lookup later.
     *
     *    is_popened = IO_STD_STREAM prevents the tag destructor and :close()
     *    from ever calling fclose/pclose on stdin, stdout, or stderr.
     * ------------------------------------------------------------------ */
    AwesomeIOFile *h;

    h = luaA_io_newfile(L); h->file = stdin;  h->is_popened = IO_STD_STREAM;
    lua_pushstring(L, LUAU_IO_DEFAULT_INPUT);
    lua_pushvalue(L, -2);
    lua_rawset(L, LUA_REGISTRYINDEX);   /* registry[DEFAULT_INPUT] = stdin_h */
    lua_setfield(L, -2, "stdin");       /* io.stdin = stdin_h                */

    h = luaA_io_newfile(L); h->file = stdout; h->is_popened = IO_STD_STREAM;
    lua_pushstring(L, LUAU_IO_DEFAULT_OUTPUT);
    lua_pushvalue(L, -2);
    lua_rawset(L, LUA_REGISTRYINDEX);   /* registry[DEFAULT_OUTPUT] = stdout_h */
    lua_setfield(L, -2, "stdout");      /* io.stdout = stdout_h                */

    h = luaA_io_newfile(L); h->file = stderr; h->is_popened = IO_STD_STREAM;
    lua_setfield(L, -2, "stderr");      /* io.stderr = stderr_h (no default)   */

    lua_setglobal(L, "io");  /* _G.io = io table; [] */
}
