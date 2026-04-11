/*
 * compat/lgi_luau_shim.h — Luau compatibility shim for lgi's C core.
 *
 * This header is force-included in CMake at the start of every lgi
 * C translation unit.  It patches API mismatches that prevent lgi
 * from compiling and working correctly against Luau's lua.h, since
 * it was created for Lua 5.x.
 *
 * This shim relies on the idea that (LUAU_VERSION_NUM < 502), if the
 * definition is even present at all; otherwise lgi.h ends up remapping
 * luaL_register etc. to functions that do not exist in Luau.
 *
 * GC STRATEGY (Fix 6)
 * ───────────────────
 * Luau does not call __gc metamethods on userdata.  This shim replaces
 * lgi's __gc finalizers with Luau's tag-based destructor mechanism:
 *
 *   All lgi userdata is allocated with tag LGI_UDATA_TAG (= 1) and a
 *   hidden PREFIX (sizeof(void*) bytes) prepended before the lgi struct:
 *
 *     raw_ptr  ──► [ type_byte | pad…  |  lgi struct … ]
 *                  ↑ PREFIX bytes ↑     ↑ visible_ptr
 *
 *   visible_ptr = raw_ptr + LGI_UDATA_PREFIX  is what lgi reads via
 *   lua_touserdata / luaL_checkudata (shimmed below).
 *
 *   A type byte (LgiUdataType) is written into the prefix when lgi calls
 *   lua_setmetatable on the freshly-created userdata (also shimmed below).
 *   The tag destructor lgi_udata_dtor() reads the type byte and dispatches
 *   the appropriate C-level cleanup (g_object_unref, g_base_info_unref …)
 *   then wipes the registry env-table entry created by _lgi_setfenv.
 *
 *   The destructor is self-registered: _lgi_newuserdata() calls
 *   lua_setuserdatadtor() on the first allocation from each translation
 *   unit (idempotent – multiple registrations just overwrite with the same
 *   function).
 */

#ifndef AWESOME_LGI_LUAU_SHIM_H
#define AWESOME_LGI_LUAU_SHIM_H

/*
 * Include Luau's headers first so that all type definitions and function
 * declarations are visible before we define our inline shims below.
 * lgi.h will #include <lua.h> again, but header guards make it a no-op.
 */
#include <lua.h>
#include <lualib.h>

/*
 * GLib / GI headers needed for the GC destructor types.
 * These are also included by lgi.h in every TU; double-inclusion is
 * harmless due to include guards.
 */
#include <string.h>       /* memcpy */
#include <stdint.h>       /* uint8_t, uintptr_t */
#include <glib.h>
#include <glib-object.h>
#include <girepository.h>
#include <gmodule.h>

/* ─── Fix 1: lua_pushcfunction ─────────────────────────────────────────────
 *
 * Luau:    #define lua_pushcfunction(L, fn, debugname)  (3 args)
 * Lua 5.1: #define lua_pushcfunction(L, f)               (2 args)
 *
 * Undefine Luau's 3-arg macro and substitute a 2-arg version that passes
 * NULL as the debug name (used only for profiling/stack traces).
 */
#ifdef lua_pushcfunction
#undef lua_pushcfunction
#endif
#define lua_pushcfunction(L, fn) lua_pushcclosurek(L, fn, NULL, 0, NULL)

/* ─── Fix 1b: luaL_ref / luaL_unref / luaL_checkint ───────────────────────
 *
 * Luau has lua_ref/lua_unref (which do NOT pop) instead of Lua 5.1's
 * luaL_ref/luaL_unref (which DO pop).  lgi calls the luaL_ variants.
 * luaL_checkint was a convenience macro in Lua 5.1 headers, absent in Luau.
 *
 * These must be defined here - not just in luau_compat.h - because this header
 * is force-included in the lgi .c files that get compiled into the .so.
 * Utilizing static inlines ensures they get compiled into the .so's object code
 * rather than remaining as unresolved external symbols.
 */
static inline int
_lgi_luaL_ref(lua_State *L, int t)
{
    (void)t; /* Luau refs are registry-only; 't' is always LUA_REGISTRYINDEX */
    int ref = lua_ref(L, -1);
    lua_pop(L, 1);
    return ref;
}
#define luaL_ref(L, t) _lgi_luaL_ref(L, t)

static inline void
_lgi_luaL_unref(lua_State *L, int t, int ref)
{
    (void)t;
    lua_unref(L, ref);
}
#define luaL_unref(L, t, ref) _lgi_luaL_unref(L, t, ref)

#define luaL_checkint(L, n) ((int)luaL_checkinteger(L, n))

/* ─── Fix 2: lua_setfenv / lua_getfenv on userdata ─────────────────────────
 *
 * Define our compat wrappers BEFORE the #define macros so their bodies
 * resolve lua_setfenv / lua_getfenv to Luau's real C functions (not to
 * ourselves, which would be infinite recursion).
 *
 * Forward-declare the tag and type enum needed by _lgi_setfenv's
 * GType-capture logic (the full definitions appear later in Fix 6).
 */
#ifndef LGI_UDATA_TAG
#define LGI_UDATA_TAG    1
#endif

/* Minimal forward enum — just the value _lgi_setfenv needs. */
enum { _LGI_UDATA_RECORD_FWD = 7 };

/*
 * Normalise a stack index to an absolute position.
 * Pseudo-indices (LUA_REGISTRYINDEX etc.) are returned unchanged.
 */
static inline int
_lgi_luau_absidx(lua_State *L, int idx)
{
    return lua_ispseudo(idx) ? idx
         : (idx >= 0        ? idx
                            : lua_gettop(L) + idx + 1);
}

/* Registry key for the centralized weak-keyed env table.
 * Using __mode="k" means entries are automatically cleared by the GC when
 * the userdata key becomes unreachable — no manual cleanup needed in the
 * tag destructor. */
#define LGI_ENVS_REGISTRY_KEY "_lgi_envs"

/*
 * _lgi_ensure_envtable — lazily create the weak-keyed env table in
 * the registry.  Pushes the table onto the stack.
 */
static inline void
_lgi_ensure_envtable(lua_State *L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, LGI_ENVS_REGISTRY_KEY);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);                                 /* envs            */
        lua_newtable(L);                                 /* envs, mt        */
        lua_pushliteral(L, "k");
        lua_setfield(L, -2, "__mode");                   /* mt.__mode = "k" */
        lua_setmetatable(L, -2);                         /* envs (weak-keyed) */
        lua_pushvalue(L, -1);                            /* envs, envs      */
        lua_setfield(L, LUA_REGISTRYINDEX, LGI_ENVS_REGISTRY_KEY);
        /* envs remains on stack */
    }
}

/*
 * _lgi_setfenv — drop-in replacement for lua_setfenv(L, idx).
 *
 * Stack protocol (matching Lua 5.1):
 *   Before: [..., table]   — table to set as the env is on top
 *   After:  [...]          — table has been consumed
 *
 * Returns 1 on success, 0 if the object type does not support environments.
 *
 * For LGI-tagged userdata, the env table is stored in a centralized
 * weak-keyed table (registry["_lgi_envs"][ud] = env) instead of directly
 * in the registry.  This lets the GC clean entries automatically when the
 * userdata dies — the tag destructor no longer needs any Lua stack ops.
 *
 * Additionally, for RECORD-type userdata, the GType from the env table's
 * _gtype field is captured into the hidden prefix so the destructor can
 * call g_boxed_free() without touching the Lua stack.
 */
static inline int
_lgi_setfenv(lua_State *L, int idx)
{
    idx = _lgi_luau_absidx(L, idx);
    if (lua_type(L, idx) == LUA_TUSERDATA) {
        int tbl = lua_gettop(L); /* absolute index of the env table at top */

        /* ── Capture _gtype into the prefix for RECORD userdata ── */
        if (lua_userdatatag(L, idx) == LGI_UDATA_TAG) {
            void *raw = lua_touserdata(L, idx); /* real fn (before macro) → raw ptr */
            uint8_t type_byte;
            memcpy(&type_byte, raw, sizeof(type_byte));
            if (type_byte == (uint8_t)_LGI_UDATA_RECORD_FWD) {
                lua_getfield(L, tbl, "_gtype");
                if (!lua_isnil(L, -1)) {
                    GType gt = (GType)(uintptr_t)lua_touserdata(L, -1);
                    memcpy((char *)raw + sizeof(void *), &gt, sizeof(gt));
                }
                lua_pop(L, 1); /* pop _gtype value */
            }
        }

        /* ── Store envs[ud] = table in the weak-keyed table ── */
        _lgi_ensure_envtable(L);         /* [..., table, envs]           */
        int envs = lua_gettop(L);
        lua_pushvalue(L, idx);           /* [..., table, envs, ud]       */
        lua_pushvalue(L, tbl);           /* [..., table, envs, ud, table]*/
        lua_rawset(L, envs);             /* [..., table, envs]           */
        lua_pop(L, 2);                   /* [...]  (consumed envs + table) */
        return 1;
    }
    /* For functions / threads, delegate to Luau's real implementation.
     * At this point lua_setfenv is still Luau's function (macro not yet
     * defined), so this call does NOT recurse. */
    return lua_setfenv(L, idx);
}

/*
 * _lgi_getfenv — drop-in replacement for lua_getfenv(L, idx).
 *
 * Stack protocol (matching Lua 5.1):
 *   Before: [...]
 *   After:  [..., env_table]
 *
 * For userdata: pushes envs[ud], or a freshly-created (and stored)
 * empty table if the entry is absent — matching Lua 5.1's guarantee
 * that every userdata always has an environment table.
 */
static inline void
_lgi_getfenv(lua_State *L, int idx)
{
    idx = _lgi_luau_absidx(L, idx);
    if (lua_type(L, idx) == LUA_TUSERDATA) {
        _lgi_ensure_envtable(L);                     /* [..., envs]        */
        int envs = lua_gettop(L);

        lua_pushvalue(L, idx);                       /* [..., envs, ud]    */
        lua_rawget(L, envs);                         /* [..., envs, env?]  */

        if (lua_isnil(L, -1)) {
            /* First access: create and store a fresh env table so that
             * subsequent getfenv calls return the same table object. */
            lua_pop(L, 1);                           /* [..., envs]        */
            lua_newtable(L);                         /* [..., envs, t]     */
            lua_pushvalue(L, idx);                   /* [..., envs, t, ud] */
            lua_pushvalue(L, -2);                    /* [..., envs, t, ud, t] */
            lua_rawset(L, envs);                     /* [..., envs, t]     */
            /* fresh table 't' remains on the stack                        */
        }

        lua_remove(L, envs);                        /* [..., env_table]   */
        return;
    }
    /* For functions / threads, delegate to Luau's real implementation. */
    lua_getfenv(L, idx);
}

/* Now install the macros.  All subsequent uses of lua_setfenv / lua_getfenv
 * in lgi's source files will call our compat wrappers. */
#define lua_setfenv(L, idx) _lgi_setfenv(L, idx)
#define lua_getfenv(L, idx) _lgi_getfenv(L, idx)

/* ─── Fix 3: lua_pushcclosure ───────────────────────────────────────────────
 *
 * Luau:    #define lua_pushcclosure(L, fn, debugname, nup)  (4 args)
 * Lua 5.1: lua_pushcclosure(L, fn, nup)                     (3 args)
 *
 * Undefine Luau's 4-arg macro and substitute a 3-arg version that passes
 * NULL as the debug name.
 */
#ifdef lua_pushcclosure
#undef lua_pushcclosure
#endif
#define lua_pushcclosure(L, fn, nup) lua_pushcclosurek(L, fn, NULL, nup, NULL)

/* ─── Fix 4: lua_resume ─────────────────────────────────────────────────────
 *
 * Luau:    lua_resume(L, from, narg)   — requires a "from" coroutine state
 * Lua 5.1: lua_resume(L, narg)         — no "from" parameter
 *
 * Define the inline wrapper BEFORE the macro so its body resolves to
 * Luau's real 3-arg function (not to our macro, which would recurse).
 * Passes NULL as "from" (acceptable when resuming from C, not a coroutine).
 */
static inline int
_lgi_lua_resume(lua_State *L, int narg)
{
    return lua_resume(L, NULL, narg);
}
#define lua_resume(L, narg) _lgi_lua_resume(L, narg)

/* ─── Fix 5: luaL_error / luaL_argerror / luaL_typeerror return type ────────
 *
 * In Lua 5.1 these return int, so lgi uses: return luaL_error(L, ...);
 * In Luau they return void (l_noret / [[noreturn]] void).
 *
 * The comma-operator trick (fn(...), 0) wraps a noreturn void call in an
 * int-typed expression.  The underlying *L functions never return, so the
 * ", 0" is unreachable but satisfies the C type system.
 */
#undef luaL_error
#define luaL_error(L, fmt, ...) (luaL_errorL(L, fmt, ##__VA_ARGS__), 0)

#undef luaL_argerror
#define luaL_argerror(L, narg, extramsg) (luaL_argerrorL(L, narg, extramsg), 0)

#undef luaL_typeerror
#define luaL_typeerror(L, narg, tname) (luaL_typeerrorL(L, narg, tname), 0)

/* ─── Fix 6: GC cleanup via Luau tag-based userdata destructors ──────────────
 *
 * Luau does not call __gc metamethods on userdata.  This section implements
 * a complete replacement using:
 *
 *   lua_setuserdatadtor(L, LGI_UDATA_TAG, lgi_udata_dtor)
 *
 * combined with a hidden PREFIX prepended to every lgi userdata allocation.
 * The PREFIX stores a single LgiUdataType byte that identifies which C
 * cleanup function to invoke at GC time.
 *
 * The destructor also removes the registry env-table entry installed by
 * _lgi_setfenv / _lgi_getfenv, eliminating that leak category entirely.
 */

/* Tag reserved for all lgi userdata (tags 0..LUA_UTAG_LIMIT-1 are available;
 * tag 0 is the default for lua_newuserdata, so we use tag 1).
 * (Already defined above in forward-declaration section.) */
#ifndef LGI_UDATA_TAG
#define LGI_UDATA_TAG    1
#endif

/* Hidden prefix size: two pointer-sized words prepended before lgi's data.
 *
 * Layout (x86-64, sizeof(void*) == 8):
 *   raw[0]      = LgiUdataType byte  (cleanup type tag)
 *   raw[1..7]   = padding
 *   raw[8..15]  = GType              (only meaningful for RECORD; 0 otherwise)
 *   raw[16..]   = visible_ptr        (what lgi sees via lua_touserdata)
 *
 * Pointer-alignment ensures lgi's struct starts on a suitable boundary.
 * The extra word stores the GType for RECORD/ALLOCATED userdata so that
 * the tag destructor can call g_boxed_free() without touching the Lua
 * stack (which is unsafe during GC — see ludata.cpp:28-29).
 */
#define LGI_UDATA_PREFIX (2u * (size_t)sizeof(void *))

/* Cleanup type stored in prefix byte 0. */
typedef enum {
    LGI_UDATA_NONE     = 0,   /* no C cleanup (namespace string, resolver …) */
    LGI_UDATA_GOBJECT  = 1,   /* g_object_unref(*(gpointer*)visible)          */
    LGI_UDATA_GIINFO   = 2,   /* g_base_info_unref(*(GIBaseInfo**)visible)    */
    LGI_UDATA_GIINFOS  = 3,   /* g_base_info_unref(((Infos*)visible)->info)   */
    LGI_UDATA_GUARD    = 4,   /* guard->destroy(guard->data)                  */
    LGI_UDATA_MODULE   = 5,   /* g_module_close(*(GModule**)visible)          */
    LGI_UDATA_CALLABLE = 6,   /* g_base_info_unref(((Callable*)visible)->info)*/
    LGI_UDATA_RECORD   = 7,   /* record store-dependent cleanup               */
} LgiUdataType;

/*
 * Minimal layout mirrors of lgi's internal structs — only the fields
 * needed for GC cleanup.  These must stay in sync with lgi's definitions
 * in gi.c, core.c, callable.c, and record.c respectively.
 */

/* gi.c — Infos: first field is the GIBaseInfo* reference to unref */
typedef struct { GIBaseInfo *info; } _LgiInfosHdr;

/* core.c — Guard: data pointer and its destroy notifier */
typedef struct {
    gpointer       data;
    GDestroyNotify destroy;
} _LgiGuardHdr;

/* callable.c — full layout mirrors for Callable and Param.
 *
 * These must stay in sync with lgi's definitions in callable.c (lines 29-99).
 * _Static_assert guards below will catch size/offset regressions at build time.
 *
 * Known sizes on x86-64 Linux:
 *   sizeof(GIArgInfo) = 72  (GIBaseInfo embedded by value in each Param)
 *   sizeof(ffi_cif)   = 32  (libffi CIF: abi/4 + nargs/4 + rtype/8 + atypes/8 + bytes/4 + flags/4)
 *
 * Both opaque fields are represented as byte arrays to avoid including
 * <ffi.h> here and to make the size assumptions explicit.
 */
#define _LGI_PARAM_AI_BYTES  72   /* sizeof(GIArgInfo) on x86-64            */
#define _LGI_CIF_BYTES       32   /* sizeof(ffi_cif)   on x86-64 linux/libffi */

/* Mirror of lgi Param (callable.c:29-68). */
typedef struct {
    GITypeInfo *ti;                        /* [0]  GI type-info ptr (unref at GC)       */
    uint8_t     _ai[_LGI_PARAM_AI_BYTES]; /* [8]  opaque GIArgInfo embedded by value    */
    guint       _bits;                     /* [80] packed Param bit-fields               */
    /* 4 bytes implicit pad → struct size 88 */
} _LgiParam;

/* Mirror of lgi Callable (callable.c:72-99). */
typedef struct {
    GICallableInfo *info;               /* [0]   GICallableInfo ptr (unref at GC)      */
    gpointer        address;            /* [8]   C function address                    */
    gpointer        user_data;          /* [16]  optional closure user_data            */
    guint           _bits;              /* [24]  has_self:1, throws:1, nargs:6, ...    */
    uint8_t         _pad[4];            /* [28]  explicit pad to 8-byte-align _cif     */
    uint8_t         _cif[_LGI_CIF_BYTES]; /* [32] opaque ffi_cif (32 bytes)          */
    _LgiParam       retval;             /* [64]  return-value Param (88 bytes)         */
    _LgiParam      *params;             /* [152] ptr into same userdata block          */
    /* total 160 bytes */
} _LgiCallable;

_Static_assert(sizeof(_LgiParam)    ==  88, "lgi Param mirror size mismatch — update _LGI_PARAM_AI_BYTES");
_Static_assert(sizeof(_LgiCallable) == 160, "lgi Callable mirror size mismatch — update _LGI_CIF_BYTES");
_Static_assert(offsetof(_LgiCallable, retval) ==  64, "lgi Callable.retval offset mismatch");
_Static_assert(offsetof(_LgiCallable, params) == 152, "lgi Callable.params offset mismatch");

/* record.c — Record: address of managed C data and ownership store mode */
typedef enum {
    _LGI_RECORD_EXTERNAL  = 0,
    _LGI_RECORD_EMBEDDED  = 1,
    _LGI_RECORD_NESTED    = 2,
    _LGI_RECORD_ALLOCATED = 3,
} _LgiRecordStore;

typedef struct {
    gpointer        addr;   /* pointer to the C record data  */
    _LgiRecordStore store;  /* ownership mode (int-sized enum) */
} _LgiRecordHdr;

/*
 * lgi_udata_dtor — Luau tag-based destructor for all lgi userdata.
 *
 * Called by Luau's GC during the sweep phase via luaU_freeudata().
 *
 * CRITICAL CONSTRAINT (ludata.cpp:28-29):
 *   "access to L here is highly unsafe since this is called during
 *    internal GC traversal"
 *
 * Therefore this function performs ONLY pure C cleanup (g_object_unref,
 * g_base_info_unref, etc.).  It makes ZERO lua_push* / lua_raw* calls.
 *
 * Env-table cleanup is handled automatically: env tables are stored in
 * a weak-keyed table (registry["_lgi_envs"]) whose entries are cleared
 * by the GC's atomic phase before this destructor runs during sweep.
 *
 * For RECORD/ALLOCATED userdata, the GType needed for g_boxed_free() was
 * captured into the hidden prefix by _lgi_setfenv at creation time.
 *
 * Layout:
 *   raw[0]                     = LgiUdataType byte
 *   raw + sizeof(void*)        = GType (for RECORD; 0 otherwise)
 *   raw + LGI_UDATA_PREFIX     = visible_ptr (lgi's struct data)
 */
static inline void
lgi_udata_dtor(lua_State *L, void *raw)
{
    (void)L; /* intentionally unused — Lua stack ops are unsafe here */

    uint8_t    type;
    void      *visible = (char *)raw + LGI_UDATA_PREFIX;

    memcpy(&type, raw, sizeof(type));

    switch ((LgiUdataType)type) {

    case LGI_UDATA_GOBJECT: {
        gpointer obj;
        memcpy(&obj, visible, sizeof(obj));
        if (obj)
            g_object_unref(obj);
        break;
    }

    case LGI_UDATA_GIINFO: {
        GIBaseInfo *info;
        memcpy(&info, visible, sizeof(info));
        if (info)
            g_base_info_unref(info);
        break;
    }

    case LGI_UDATA_GIINFOS: {
        _LgiInfosHdr hdr;
        memcpy(&hdr, visible, sizeof(hdr));
        if (hdr.info)
            g_base_info_unref(hdr.info);
        break;
    }

    case LGI_UDATA_GUARD: {
        _LgiGuardHdr hdr;
        memcpy(&hdr, visible, sizeof(hdr));
        if (hdr.destroy)
            hdr.destroy(hdr.data);
        break;
    }

    case LGI_UDATA_MODULE: {
        GModule *mod;
        memcpy(&mod, visible, sizeof(mod));
        if (mod)
            g_module_close(mod);
        break;
    }

    case LGI_UDATA_CALLABLE: {
        _LgiCallable callable;
        int           nargs, i;
        memcpy(&callable, visible, sizeof(callable));

        if (callable.info)
            g_base_info_unref((GIBaseInfo *)callable.info);

        nargs = (int)((callable._bits >> 2) & 0x3Fu);

        if (callable.retval.ti)
            g_base_info_unref(callable.retval.ti);

        if (callable.params) {
            for (i = 0; i < nargs; i++) {
                _LgiParam param;
                memcpy(&param, &callable.params[i], sizeof(param));
                if (param.ti)
                    g_base_info_unref(param.ti);
            }
        }
        break;
    }

    case LGI_UDATA_RECORD: {
        /* For ALLOCATED records, read the GType from the hidden prefix
         * (captured by _lgi_setfenv at creation time) and call g_boxed_free.
         * No Lua API calls needed. */
        _LgiRecordHdr rec;
        memcpy(&rec, visible, sizeof(rec));
        if (rec.store == _LGI_RECORD_ALLOCATED && rec.addr) {
            GType gtype;
            memcpy(&gtype, (char *)raw + sizeof(void *), sizeof(gtype));
            if (gtype && G_TYPE_IS_BOXED(gtype))
                g_boxed_free(gtype, rec.addr);
        }
        break;
    }

    default:
        break;
    }

    /* No manual env-table cleanup needed — the weak-keyed _lgi_envs
     * table handles this automatically during the GC atomic phase. */
}

/* ─── Fix 6a: lua_newuserdata ────────────────────────────────────────────────
 *
 * Allocate LGI_UDATA_PREFIX extra bytes before lgi's struct, tagged with
 * LGI_UDATA_TAG so the destructor fires at GC time.  The returned pointer
 * (visible_ptr) skips the prefix and is what lgi stores into its structs.
 *
 * Self-registers the tag destructor on first call from this TU.
 */

/* Raw (un-shimmed) lua_touserdata — defined here, before the macro, so
 * other helpers below can call the real function without macro recursion. */
static inline void *
_lgi_raw_touserdata(lua_State *L, int idx)
{
    return lua_touserdata(L, idx);
}

/* Raw (un-shimmed) lua_objlen — same pattern. */
static inline size_t
_lgi_raw_objlen(lua_State *L, int idx)
{
    return (size_t)lua_objlen(L, idx);
}

static int _lgi_gc_init_done; /* zero-initialised per-TU flag */

static inline void *
_lgi_newuserdata(lua_State *L, size_t sz)
{
    char *raw;

    /* Self-register the tag destructor on the first allocation from this
     * translation unit.  Multiple TUs may register; each call is idempotent
     * (just overwrites the global slot with the same logic). */
    if (!_lgi_gc_init_done) {
        lua_setuserdatadtor(L, LGI_UDATA_TAG, lgi_udata_dtor);
        _lgi_gc_init_done = 1;
    }

    raw = (char *)lua_newuserdatatagged(L, LGI_UDATA_PREFIX + sz,
                                        LGI_UDATA_TAG);
    raw[0] = (char)LGI_UDATA_NONE;              /* default: no C cleanup */
    memset(raw + sizeof(void *), 0, sizeof(GType)); /* clear GType slot   */
    return raw + LGI_UDATA_PREFIX;              /* visible_ptr */
}

#ifdef lua_newuserdata
#undef lua_newuserdata
#endif
#define lua_newuserdata(L, sz) _lgi_newuserdata(L, sz)

/* ─── Fix 6b: lua_touserdata ─────────────────────────────────────────────────
 *
 * For LGI-tagged full userdata, advance the returned pointer past the hidden
 * prefix so that lgi always sees visible_ptr.  Light userdata and other
 * types are returned unchanged.
 */
static inline void *
_lgi_touserdata(lua_State *L, int idx)
{
    void *ptr = _lgi_raw_touserdata(L, idx);
    if (ptr
            && lua_type(L, idx) == LUA_TUSERDATA
            && lua_userdatatag(L, idx) == LGI_UDATA_TAG)
        return (char *)ptr + LGI_UDATA_PREFIX;
    return ptr;
}
#define lua_touserdata(L, idx) _lgi_touserdata(L, idx)

/* ─── Fix 6c: luaL_checkudata ────────────────────────────────────────────────
 *
 * luaL_checkudata is a C library function that internally calls the real
 * lua_touserdata (not our macro).  Wrap it to add the prefix offset on the
 * returned pointer for LGI-tagged userdata.
 */
static inline void *
_lgi_checkudata(lua_State *L, int ud, const char *tname)
{
    void *ptr = luaL_checkudata(L, ud, tname); /* real library call */
    if (ptr
            && lua_type(L, ud) == LUA_TUSERDATA
            && lua_userdatatag(L, ud) == LGI_UDATA_TAG)
        return (char *)ptr + LGI_UDATA_PREFIX;
    return ptr;
}
#define luaL_checkudata(L, ud, tname) _lgi_checkudata(L, ud, tname)

/* ─── Fix 6d: lua_objlen for userdata ────────────────────────────────────────
 *
 * Luau's lua_objlen for userdata returns the full allocation size
 * (LGI_UDATA_PREFIX + lgi_sz).  Buffer code in lgi uses lua_objlen to
 * determine the usable byte count, so subtract the prefix for LGI-tagged
 * userdata.  Tables and strings are returned unchanged.
 */
static inline size_t
_lgi_objlen(lua_State *L, int idx)
{
    size_t len = _lgi_raw_objlen(L, idx);
    if (lua_type(L, idx) == LUA_TUSERDATA
            && lua_userdatatag(L, idx) == LGI_UDATA_TAG)
        return len > LGI_UDATA_PREFIX ? len - LGI_UDATA_PREFIX : 0;
    return len;
}
#ifdef lua_objlen
#undef lua_objlen
#endif
#define lua_objlen(L, idx) ((int)_lgi_objlen(L, idx))

/* ─── Fix 6e: lua_setmetatable — type detection ──────────────────────────────
 *
 * Intercept every lua_setmetatable call on LGI-tagged userdata.  The
 * metatable is always at the Lua stack top (it is about to be consumed by
 * the real lua_setmetatable call).  We inspect it to determine the lgi
 * resource type and record the LgiUdataType byte in the hidden prefix so
 * the tag destructor knows what to clean up.
 *
 * Detection strategy (applied in order):
 *   1. Compare against the known string-registered lgi metatables.
 *   2. For the three lightuserdata-registered metatables (object_mt,
 *      record_mt, callable_mt) use a method fingerprint:
 *        __gc + __tostring + __call  → Callable
 *        __gc + __tostring + __len   → Record
 *        __gc + __tostring           → GObject proxy
 *        __gc only (no __tostring)   → internal (call_mutex, env_mt) → NONE
 */

/* Helper: is the table at absolute stack index 'mt' the metatable
 * registered under the string key 'name' in the Lua registry? */
static inline int
_lgi_mt_is(lua_State *L, int mt, const char *name)
{
    int eq;
    lua_getfield(L, LUA_REGISTRYINDEX, name);  /* push registry[name] */
    eq = lua_rawequal(L, mt, -1);
    lua_pop(L, 1);
    return eq;
}

/* Helper: does the table at absolute stack index 'mt' have a non-nil
 * field named 'field'? */
static inline int
_lgi_mt_has(lua_State *L, int mt, const char *field)
{
    int found;
    lua_getfield(L, mt, field);
    found = !lua_isnil(L, -1);
    lua_pop(L, 1);
    return found;
}

static inline int
_lgi_setmetatable(lua_State *L, int idx)
{
    if (lua_type(L, idx) == LUA_TUSERDATA
            && lua_userdatatag(L, idx) == LGI_UDATA_TAG) {

        /* The metatable is at the stack top — capture its absolute index
         * before any push/pop operations change the relative indices. */
        int mt = lua_gettop(L);

        /* raw_ptr (with prefix) — use pre-macro raw accessor */
        void *raw = _lgi_raw_touserdata(L, idx);

        LgiUdataType type = LGI_UDATA_NONE;

        /* ── Step 1: string-registered metatables ── */
        if      (_lgi_mt_is(L, mt, "lgi.gi.info"))      type = LGI_UDATA_GIINFO;
        else if (_lgi_mt_is(L, mt, "lgi.gi.infos"))     type = LGI_UDATA_GIINFOS;
        else if (_lgi_mt_is(L, mt, "lgi.guard"))        type = LGI_UDATA_GUARD;
        else if (_lgi_mt_is(L, mt, "lgi.core.module"))  type = LGI_UDATA_MODULE;
        /* lgi.gi.resolver and lgi.gi.namespace have no __gc — NONE */
        /* bytes.bytearray has no C heap resource to free — NONE    */

        /* ── Step 2: lightuserdata-registered metatables (fingerprint) ── */
        else if (_lgi_mt_has(L, mt, "__gc")) {
            if (_lgi_mt_has(L, mt, "__tostring")) {
                if      (_lgi_mt_has(L, mt, "__call")) type = LGI_UDATA_CALLABLE;
                else if (_lgi_mt_has(L, mt, "__len"))  type = LGI_UDATA_RECORD;
                else                                   type = LGI_UDATA_GOBJECT;
            }
            /* else: __gc only (call_mutex, env_mt) → LGI_UDATA_NONE */
        }

        ((uint8_t *)raw)[0] = (uint8_t)type;
    }

    /* Call the real lua_setmetatable — resolved before the macro below. */
    return lua_setmetatable(L, idx);
}
#define lua_setmetatable(L, idx) _lgi_setmetatable(L, idx)

#endif /* AWESOME_LGI_LUAU_SHIM_H */
