# 'Playground' — scratch targets for exploring the Luau C API...
# Every .cpp file in this directory becomes its own executable, linked against
# Luau.VM and Luau.Compiler (targets defined in awesomeConfig.cmake).
# Nothing here is built by default; targets must be requested explicitly, e.g.:
#   ninja -C build test-luau

file(GLOB PLAYGROUND_SRCS "*.cpp")

foreach(_src ${PLAYGROUND_SRCS})
    get_filename_component(_name ${_src} NAME_WE)
    add_executable(${_name} EXCLUDE_FROM_ALL ${_src})
    target_link_libraries(${_name} PRIVATE Luau.VM Luau.Compiler stdc++)
    # The global include_directories() adds the system lua5.3 headers before the
    # Luau ones (via LUA_INCLUDE_DIR). Use BEFORE to prepend Luau's headers so
    # <lua.h> / <lualib.h> resolve to Luau and not the system Lua interpreter.
    target_include_directories(${_name} BEFORE PRIVATE
        "${LUAU_VM_INCLUDE_DIR}"
        "${LUAU_COMPILER_INCLUDE_DIR}")
endforeach()
