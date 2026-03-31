# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build

The preferred dev build uses **Ninja** + **ccache** (faster than the default Make path):

```bash
# Debug build (preferred for development)
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_COMPILER_LAUNCHER=ccache \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
ninja -C build

# ASan + UBSan build (separate directory, for memory/UB debugging)
cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_COMPILER_LAUNCHER=ccache \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  "-DCMAKE_C_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
ninja -C build-asan

# Skip doc generation (faster configure)
cmake ... -DGENERATE_DOC=OFF

# Clean
make distclean   # removes build/
```

`CMAKE_EXPORT_COMPILE_COMMANDS=ON` writes `build/compile_commands.json`, which clangd/LSP tools use for accurate completion and diagnostics.

## Running & Debugging

awesome must run inside an X server. Use Xephyr to test without replacing your live session:

```bash
Xephyr :1 &
DISPLAY=:1 ./build/awesome --replace

# ASan run — logs written to /tmp/asan_awesome.* and /tmp/ubsan_awesome.*
DISPLAY=:1 \
  ASAN_OPTIONS="halt_on_error=0:print_stacktrace=1:log_path=/tmp/asan_awesome" \
  UBSAN_OPTIONS="print_stacktrace=1:log_path=/tmp/ubsan_awesome" \
  ./build-asan/awesome --replace
```

The VS Code launch configs in `.vscode/launch.json` automate this with lldb on `DISPLAY=:1`.

## Tests & Linting

```bash
# All checks (integration + unit + QA)
make -C build check

# Unit tests only (Busted framework, Lua)
make -C build check-unit

# Run a single spec file
busted spec/awful/rules_spec.lua

# Integration tests
make -C build check-integration

# Lua static analysis
make -C build luacheck

# Theme tests
make -C build check-themes
```

Busted is configured via `.busted` — it loads `spec/preload.lua` as helper and sets `lpath` to `lib/`.
Luacheck is configured via `.luacheckrc` — globals like `awesome`, `client`, `screen`, `mouse`, `root` are pre-declared.

## Architecture

AwesomeWM is a two-layer system:

### C Core (`objects/`, `common/`)
Implements X11/XCB window management primitives exposed to Lua as objects:
- `objects/client.c` — window lifecycle, properties, signals (~155KB, the largest file)
- `objects/screen.c`, `objects/tag.c` — monitor and workspace management
- `objects/drawable.c`, `objects/drawin.c` — Cairo-backed drawing surfaces
- `common/luaclass.c`, `common/luaobject.c` — the Lua object binding system used by all C objects
- `awesome.c` — main loop; `event.c` — X11 event dispatch; `luaa.c` — Lua API surface

### Lua Library (`lib/`)
The full standard library, loaded by `awesomerc.lua` (the user config entry point):
- `awful/` — client management, layouts, key/mouse bindings, rules, popups
- `wibox/` — widget box rendering (containers, widgets, layout engine)
- `gears/` — pure-Lua utilities: `color`, `geometry`, `timer`, `object` (signal system), `string`, `table`
- `beautiful/` — theme engine (loaded once at startup, read globally via `beautiful.*`)
- `naughty/` — notification system
- `ruled/` — declarative rules engine used by `awful.rules` and `awful.tag`
- `menubar/` — XDG application menu

### Signal System
Everything uses `gears.object` for signals. C objects emit signals into Lua; Lua code connects handlers via `obj:connect_signal("name", fn)`. Understanding this is essential for tracing event flows between C and Lua.

### Lua Version Compatibility
The codebase targets Lua 5.1–5.4 and LuaJIT. `.luacheckrc` uses `std = "min"` to enforce the common subset. Avoid APIs available only in newer versions.
