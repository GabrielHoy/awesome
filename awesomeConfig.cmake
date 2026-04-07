set(PROJECT_AWE_NAME awesome)

# If ${SOURCE_DIR} is a git repository VERSION is set to
# `git describe` later.
set(VERSION devel)

set(CODENAME "Too long")

include(AutoOption.cmake)

autoOption(WITH_DBUS "build with D-BUS")
autoOption(GENERATE_MANPAGES "generate manpages")
option(COMPRESS_MANPAGES "compress manpages" ON)
option(GENERATE_DOC "generate API documentation" ON)
option(DO_COVERAGE "build with coverage" OFF)
autoOption(WITH_XCB_ERRORS "build with xcb-errors")
if (GENERATE_DOC AND DO_COVERAGE)
    message(STATUS "Not generating API documentation with DO_COVERAGE")
    set(GENERATE_DOC OFF)
endif()

# {{{ Find external utilities
macro(a_find_program var prg req)
    set(required ${req})
    find_program(${var} ${prg})
    if(NOT ${var})
        message(STATUS "${prg} not found.")
        if(required)
            message(FATAL_ERROR "${prg} is required to build awesome")
        endif()
    else()
        message(STATUS "${prg} -> ${${var}}")
    endif()
endmacro()

a_find_program(LUA_EXECUTABLE lua TRUE)
a_find_program(GIT_EXECUTABLE git FALSE)
# programs needed for man pages
a_find_program(ASCIIDOCTOR_EXECUTABLE asciidoctor FALSE)
a_find_program(GZIP_EXECUTABLE gzip FALSE)
# Lua documentation
if(GENERATE_DOC)
    a_find_program(LDOC_EXECUTABLE ldoc FALSE)
    if(NOT LDOC_EXECUTABLE)
        a_find_program(LDOC_EXECUTABLE ldoc.lua FALSE)
    endif()
    if(LDOC_EXECUTABLE)
        execute_process(COMMAND sh -c "${LDOC_EXECUTABLE} --sadly-ldoc-has-no-version-option 2>&1  | grep ' vs 1.4.5'"
                        OUTPUT_VARIABLE LDOC_VERSION_RESULT)
        if(NOT LDOC_VERSION_RESULT STREQUAL "")
            message(WARNING "Ignoring LDoc, because version 1.4.5 is known to be broken")
            unset(LDOC_EXECUTABLE CACHE)
        endif()
    endif()
    if(NOT LDOC_EXECUTABLE)
        message(STATUS "Not generating API documentation. Missing: ldoc.")
        set(GENERATE_DOC OFF)
    endif()
else()
    message(STATUS "Not generating API documentation.")
endif()
# theme graphics
a_find_program(CONVERT_EXECUTABLE convert TRUE)
# pkg-config
include(FindPkgConfig)
# Luau - embedded via git submodule @ third-party/luau
set(LUAU_SUBDIR "${SOURCE_DIR}/third-party/luau")
if(NOT EXISTS "${LUAU_SUBDIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "Luau submodule not initialised. Run the following:\n"
        "  git submodule update --init third-party/luau")
endif()

# LUAU_EXTERN_C is required so lua.h / luacode.h exposes extern "C" symbols
# callable from awesome's C translation units;
# LUA_USE_LONGJMP (set automatically by LUAU_EXTERN_C) makes luaL_error use
# longjmp instead of C++ exceptions, which is mandatory inside extern "C".
set(LUAU_BUILD_CLI   OFF CACHE BOOL "Build Luau CLI tools"           FORCE)
set(LUAU_BUILD_TESTS OFF CACHE BOOL "Build Luau tests"               FORCE)
set(LUAU_BUILD_WEB   OFF CACHE BOOL "Build Luau web module"          FORCE)
set(LUAU_EXTERN_C    ON  CACHE BOOL "Expose Luau C API via extern C" FORCE)
set(LUAU_WERROR      OFF CACHE BOOL "Luau warnings as errors"        FORCE)

add_subdirectory("${LUAU_SUBDIR}" "${CMAKE_BINARY_DIR}/luau" EXCLUDE_FROM_ALL)

# Remove LUA_API/LUACODE_API overrides from Luau targets' INTERFACE_DEFINITIONS:
# Prevents C "extern \"C\"" macro errors in awesome's C sources.
foreach(_luau_target Luau.VM Luau.Compiler)
    get_target_property(_iface_defs ${_luau_target} INTERFACE_COMPILE_DEFINITIONS)
    if(_iface_defs)
        list(FILTER _iface_defs EXCLUDE REGEX "^LUA_API=|^LUACODE_API=|^LUACODEGEN_API=")
        set_property(TARGET ${_luau_target} PROPERTY INTERFACE_COMPILE_DEFINITIONS "${_iface_defs}")
    endif()
endforeach()

# Luau.VM and Luau.Compiler carry their own include directories via
# target_include_directories; we also expose the paths explicitly for
# configure_file / lgi-check use.
set(LUAU_VM_INCLUDE_DIR       "${LUAU_SUBDIR}/VM/include")
set(LUAU_COMPILER_INCLUDE_DIR "${LUAU_SUBDIR}/Compiler/include")
message(STATUS "Luau -> ${LUAU_SUBDIR}")

# }}}



# {{{ Check if documentation can be build
if(GENERATE_MANPAGES)
    if(NOT ASCIIDOCTOR_EXECUTABLE OR (COMPRESS_MANPAGES AND NOT GZIP_EXECUTABLE))
        if(NOT ASCIIDOCTOR_EXECUTABLE)
            SET(missing "asciidoctor")
        endif()
        if(COMPRESS_MANPAGES AND NOT GZIP_EXECUTABLE)
            SET(missing ${missing} " gzip")
        endif()

        autoDisable(GENERATE_MANPAGES "Not generating manpages. Missing: " ${missing})
    endif()
endif()
# }}}

# {{{ Version stamp
if(OVERRIDE_VERSION)
    set(VERSION ${OVERRIDE_VERSION})
    message(STATUS "Using version from OVERRIDE_VERSION: ${VERSION}")
elseif(EXISTS ${SOURCE_DIR}/.git AND GIT_EXECUTABLE)
    # get current version
    execute_process(
        COMMAND ${GIT_EXECUTABLE} describe --dirty
        WORKING_DIRECTORY ${SOURCE_DIR}
        OUTPUT_VARIABLE VERSION
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    # File the build-utils/git-version-stamp.sh script will look into.
    set(VERSION_STAMP_FILE ${BUILD_DIR}/.version_stamp)
    file(WRITE ${VERSION_STAMP_FILE} ${VERSION})
    # create a version_stamp target later
    set(BUILD_FROM_GIT TRUE)
    message(STATUS "Using version from git: ${VERSION}")
elseif( EXISTS ${SOURCE_DIR}/.version_stamp )
    # get version from version stamp
    file(READ ${SOURCE_DIR}/.version_stamp VERSION)
    message(STATUS "Using version from ${SOURCE_DIR}/.version_stamp: ${VERSION}")
endif()
# }}}

# {{{ Required libraries
#
# this sets up:
# AWESOME_REQUIRED_LDFLAGS
# AWESOME_REQUIRED_INCLUDE_DIRS

# Use pkgconfig to get most of the libraries
pkg_check_modules(AWESOME_COMMON_REQUIRED REQUIRED
    xcb>=1.6)

set(AWESOME_DEPENDENCIES
    glib-2.0
    glib-2.0>=2.79.2
    gdk-pixbuf-2.0
    cairo
    x11
    xcb-cursor
    xcb-randr
    xcb-xtest
    xcb-xinerama
    xcb-shape
    xcb-util
    xcb-util>=0.3.8
    xcb-keysyms
    xcb-keysyms>=0.3.4
    xcb-icccm
    xcb-icccm>=0.3.8
    xcb-xfixes
    # NOTE: it's not clear what version is required, but 1.10 works at least.
    # See https://github.com/awesomeWM/awesome/pull/149#issuecomment-94208356.
    xcb-xkb
    xkbcommon
    xkbcommon-x11
    cairo-xcb
    libstartup-notification-1.0
    libstartup-notification-1.0>=0.10
    xproto
    xproto>=7.0.15
    libxdg-basedir
    libxdg-basedir>=1.0.0
    xcb-xrm
)
pkg_check_modules(AWESOME_REQUIRED REQUIRED ${AWESOME_DEPENDENCIES})

# Check for backtrace_symbols()
include(CheckSymbolExists)
check_symbol_exists(backtrace_symbols execinfo.h HAS_EXECINFO)
if(NOT HAS_EXECINFO)
    find_library(LIB_EXECINFO execinfo)
    if(LIB_EXECINFO)
        set(HAS_EXECINFO 1)
        set(AWESOME_REQUIRED_LDFLAGS
            ${AWESOME_REQUIRED_LDFLAGS}
            ${LIB_EXECINFO})
    endif()
endif()
if(HAS_EXECINFO)
    message(STATUS "checking for execinfo -- found")
else()
    message(STATUS "checking for execinfo -- not found")
endif()

# Do we need libm for round()?
check_symbol_exists(round math.h HAS_ROUND_WITHOUT_LIBM)
if(NOT HAS_ROUND_WITHOUT_LIBM)
    SET(CMAKE_REQUIRED_LIBRARIES ${CMAKE_REQUIRED_LIBRARIES} m)
    set(AWESOME_REQUIRED_LDFLAGS ${AWESOME_REQUIRED_LDFLAGS} m)
    check_symbol_exists(round math.h HAS_ROUND_WITH_LIBM)
    if(NOT HAS_ROUND_WITH_LIBM)
        message(FATAL_ERROR "Did not find round()")
    endif()
    message(STATUS "checking for round -- in libm")
else()
    message(STATUS "checking for round -- builtin")
endif()

set(AWESOME_REQUIRED_LDFLAGS
    ${AWESOME_COMMON_REQUIRED_LDFLAGS}
    ${AWESOME_REQUIRED_LDFLAGS}
    )

set(AWESOME_REQUIRED_INCLUDE_DIRS
    ${AWESOME_COMMON_REQUIRED_INCLUDE_DIRS}
    ${AWESOME_REQUIRED_INCLUDE_DIRS}
    "${LUAU_VM_INCLUDE_DIR}"
    "${LUAU_COMPILER_INCLUDE_DIR}"
)

# (stdc++ is required here due to Luau's static
# archives containing C++ object code.)
# dl is required for dlopen/dlsym used by the package.cpath C extension searcher.
list(APPEND AWESOME_REQUIRED_LDFLAGS Luau.VM Luau.Compiler stdc++ dl)

# }}}
# }}}

# {{{ Optional libraries
#
# this sets up:
# AWESOME_OPTIONAL_LDFLAGS
# AWESOME_OPTIONAL_INCLUDE_DIRS

if(WITH_DBUS)
    pkg_check_modules(DBUS dbus-1)
    if(DBUS_FOUND)
        set(AWESOME_OPTIONAL_LDFLAGS ${AWESOME_OPTIONAL_LDFLAGS} ${DBUS_LDFLAGS})
        set(AWESOME_OPTIONAL_INCLUDE_DIRS ${AWESOME_OPTIONAL_INCLUDE_DIRS} ${DBUS_INCLUDE_DIRS})
    else()
        autoDisable(WITH_DBUS "DBus not found.")
    endif()
endif()

if(WITH_XCB_ERRORS)
    pkg_check_modules(XCB_ERRORS xcb-errors)
    if(XCB_ERRORS_FOUND)
        set(AWESOME_OPTIONAL_LDFLAGS ${AWESOME_OPTIONAL_LDFLAGS} ${XCB_ERRORS_LDFLAGS})
        set(AWESOME_OPTIONAL_INCLUDE_DIRS ${AWESOME_OPTIONAL_INCLUDE_DIRS} ${XCB_ERRORS_INCLUDE_DIRS})
    else()
        autoDisable(WITH_XCB_ERRORS "xcb-errors not found.")
    endif()
endif()
# }}}

# {{{ Install path and configuration variables
#If a sysconfdir is specified, use it instead
#of the default configuration dir.
if(DEFINED SYSCONFDIR)
    set(SYSCONFDIR ${SYSCONFDIR} CACHE PATH "config directory")
else()
    set(SYSCONFDIR ${CMAKE_INSTALL_PREFIX}/etc CACHE PATH "config directory")
endif()

#If an XDG Config Dir is specified, use it instead
#of the default XDG configuration dir.
if(DEFINED XDG_CONFIG_DIR)
    set(XDG_CONFIG_DIR ${XDG_CONFIG_DIR} CACHE PATH "xdg config directory")
else()
    set(XDG_CONFIG_DIR ${SYSCONFDIR}/xdg CACHE PATH "xdg config directory")
endif()

# setting AWESOME_DATA_PATH
if(DEFINED AWESOME_DATA_PATH)
    set(AWESOME_DATA_PATH ${AWESOME_DATA_PATH} CACHE PATH "awesome share directory")
else()
    set(AWESOME_DATA_PATH ${CMAKE_INSTALL_PREFIX}/share/${PROJECT_AWE_NAME} CACHE PATH "awesome share directory")
endif()

# setting AWESOME_DOC_PATH
if(DEFINED AWESOME_DOC_PATH)
    set(AWESOME_DOC_PATH ${AWESOME_DOC_PATH} CACHE PATH "awesome docs directory")
else()
    set(AWESOME_DOC_PATH ${CMAKE_INSTALL_PREFIX}/share/doc/${PROJECT_AWE_NAME} CACHE PATH "awesome docs directory")
endif()

# setting AWESOME_XSESSION_PATH
if(DEFINED AWESOME_XSESSION_PATH)
    set(AWESOME_XSESSION_PATH ${AWESOME_XSESSION_PATH} CACHE PATH "awesome xsessions directory")
else()
    set(AWESOME_XSESSION_PATH ${CMAKE_INSTALL_PREFIX}/share/xsessions CACHE PATH "awesome xsessions directory")
endif()

# set man path
if(DEFINED AWESOME_MAN_PATH)
   set(AWESOME_MAN_PATH ${AWESOME_MAN_PATH} CACHE PATH "awesome manpage directory")
else()
   set(AWESOME_MAN_PATH ${CMAKE_INSTALL_PREFIX}/share/man CACHE PATH "awesome manpage directory")
endif()


# Hide to avoid confusion
mark_as_advanced(CMAKE_INSTALL_CMAKE_INSTALL_PREFIX)

set(AWESOME_VERSION          ${VERSION})
set(AWESOME_RELEASE          ${CODENAME})
set(AWESOME_SYSCONFDIR       ${XDG_CONFIG_DIR}/${PROJECT_AWE_NAME})
set(AWESOME_LUA_LIB_PATH     ${AWESOME_DATA_PATH}/lib)
set(AWESOME_ICON_PATH        ${AWESOME_DATA_PATH}/icons)

# Detect system Lua 5.1 paths for package.path and package.cpath.
# These paths are needed so that require("lgi") and other system Lua C modules
# can be found at runtime.  Try pkg-config first; fall back to standard FHS
# locations including Debian/Ubuntu multiarch paths.
execute_process(COMMAND pkg-config --variable=INSTALL_CMOD lua5.1
    OUTPUT_VARIABLE LUA_CMOD_PATH OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
execute_process(COMMAND pkg-config --variable=INSTALL_LMOD lua5.1
    OUTPUT_VARIABLE LUA_SHARE_PATH OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT LUA_CMOD_PATH)
    if(CMAKE_LIBRARY_ARCHITECTURE)
        set(LUA_CMOD_PATH "/usr/lib/${CMAKE_LIBRARY_ARCHITECTURE}/lua/5.1")
    else()
        set(LUA_CMOD_PATH "/usr/lib/lua/5.1")
    endif()
endif()
if(NOT LUA_SHARE_PATH)
    set(LUA_SHARE_PATH "/usr/share/lua/5.1")
endif()
message(STATUS "System Lua C module path: ${LUA_CMOD_PATH}")
message(STATUS "System Lua share path:    ${LUA_SHARE_PATH}")
set(AWESOME_THEMES_PATH      ${AWESOME_DATA_PATH}/themes)
set(AWESOME_API_LEVEL        4)

# {{{ Build lgi's C core against Luau headers (corelgiluau.so)
#
# lgi (https://github.com/lgi-devs/lgi) is included as a git submodule at
# third-party/lgi.  We compile its C source files against Luau's lua.h
# rather than the system Lua 5.1 headers, using compat/lgi_luau_shim.h
# (force-included) to bridge the two API differences:
#   - lua_pushcfunction: Luau 3-arg macro → 2-arg shim
#   - lua_setfenv/lua_getfenv on userdata → registry-keyed-via-pointer shim
#
# The output is ${CMAKE_BINARY_DIR}/lgi/corelgiluau.so, which is
# found by require("lgi.corelgiluau") via our lib/lgi/core.lua override
# which tries the Luau build first, falls back to the system lua51 build.

pkg_check_modules(LGI_GI REQUIRED gobject-introspection-1.0)
pkg_check_modules(LGI_FFI REQUIRED libffi)

set(LGI_SOURCE_DIR ${SOURCE_DIR}/third-party/lgi/lgi)

set(LGI_C_SOURCES
    ${LGI_SOURCE_DIR}/buffer.c
    ${LGI_SOURCE_DIR}/callable.c
    ${LGI_SOURCE_DIR}/core.c
    ${LGI_SOURCE_DIR}/gi.c
    ${LGI_SOURCE_DIR}/marshal.c
    ${LGI_SOURCE_DIR}/object.c
    ${LGI_SOURCE_DIR}/record.c
)

add_library(corelgiluau SHARED ${LGI_C_SOURCES})

set_target_properties(corelgiluau PROPERTIES
    # Lua C modules must have no library prefix - "libcorelgilua51.so" -> "corelgilua51.so")
    PREFIX ""
    # Keep the same filename as the system module so the original lgi/core.lua
    # (require('lgi.corelgilua51')) finds our Luau build
    # first, because BUILD_LGI_PATH is prepended to package.cpath at startup.
    OUTPUT_NAME "corelgilua51"
    # Place the .so in build/lgi/ so it's findable on package.cpath during development
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lgi"
)

target_include_directories(corelgiluau PRIVATE
    ${LGI_SOURCE_DIR}                                    # lgi.h
    ${SOURCE_DIR}/third-party/luau/VM/include            # Luau's lua.h (takes precedence over system Lua)
    ${SOURCE_DIR}/third-party/luau/Common/include        # Luau internal headers if needed
    ${SOURCE_DIR}/compat                                 # lgi_luau_shim.h
    ${LGI_GI_INCLUDE_DIRS}
    ${LGI_FFI_INCLUDE_DIRS}
)

target_compile_options(corelgiluau PRIVATE
    # Force-include the compat shim before any lgi source file is processed.
    # This ensures lua_pushcfunction and lua_setfenv/getfenv are shimmed
    # before lgi.h is included.
    -include ${SOURCE_DIR}/compat/lgi_luau_shim.h
    # lgi uses some deprecated GLib/GI API; suppress noise during build
    -Wno-deprecated-declarations
    # Suppress warnings from lgi source we don't control
    -Wno-unused-parameter
    -Wno-missing-prototypes
    -Wno-strict-prototypes
)

target_link_libraries(corelgiluau PRIVATE
    ${LGI_GI_LIBRARIES}
    ${LGI_FFI_LIBRARIES}
)

# Install corelgiluau.so alongside the awesome Lua library for installed builds.
# During development, use -p build/lgi or ensure LIBRARY_OUTPUT_DIRECTORY is on cpath.
install(TARGETS corelgiluau
    LIBRARY DESTINATION ${AWESOME_LUA_LIB_PATH}/lgi
)

message(STATUS "lgi Luau build: ${CMAKE_BINARY_DIR}/lgi/corelgilua51.so")
# }}}

if(GENERATE_DOC)
    # Load the common documentation
    include(docs/load_ldoc.cmake)

    # Generate the widget lists
    include(docs/widget_lists.cmake)
endif()

# Use `include`, rather than `add_subdirectory`, to keep the variables
# The file is a valid CMakeLists.txt and can be executed directly if only
# the image artefacts are needed.
include(tests/examples/CMakeLists.txt)

# {{{ Configure files
file(GLOB awesome_base_c_configure_files RELATIVE ${SOURCE_DIR}
    ${SOURCE_DIR}/*.c
    ${SOURCE_DIR}/*.h)

file(GLOB awesome_c_configure_files RELATIVE ${SOURCE_DIR}
    ${SOURCE_DIR}/common/*.c
    ${SOURCE_DIR}/common/*.h
    ${SOURCE_DIR}/objects/*.c
    ${SOURCE_DIR}/objects/*.h
    ${SOURCE_DIR}/compat/*.h
    ${SOURCE_DIR}/compat/*.c)

file(GLOB_RECURSE awesome_lua_configure_files RELATIVE ${SOURCE_DIR}
    ${SOURCE_DIR}/lib/*.lua)

file(GLOB_RECURSE awesome_theme_configure_files RELATIVE ${SOURCE_DIR}
    ${SOURCE_DIR}/themes/*/*.lua)

set(AWESOME_CONFIGURE_FILES
    ${awesome_base_c_configure_files}
    ${awesome_theme_configure_files}
    config.h
    docs/config.ld
    awesome-version-internal.h)

foreach(file ${AWESOME_CONFIGURE_FILES})
    configure_file(${SOURCE_DIR}/${file}
                   ${BUILD_DIR}/${file}
                   ESCAPE_QUOTES
                   @ONLY)
endforeach()

set(AWESOME_CONFIGURE_COPYONLY_WITHCOV_FILES
    ${awesome_c_configure_files}
    ${awesome_lua_configure_files}
)

if(DO_COVERAGE)
    foreach(file ${AWESOME_CONFIGURE_COPYONLY_WITHCOV_FILES})
        configure_file(${SOURCE_DIR}/${file}
                    ${BUILD_DIR}/${file}
                    COPYONLY)
    endforeach()
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -g -O0 --coverage -fprofile-arcs -ftest-coverage")
else()
    foreach(file ${AWESOME_CONFIGURE_COPYONLY_WITHCOV_FILES})
        configure_file(${SOURCE_DIR}/${file}
                    ${BUILD_DIR}/${file}
                    ESCAPE_QUOTES
                    @ONLY)
    endforeach()
endif()

#}}}

# {{{ Quick check for LGI presence
execute_process(
    COMMAND ${LUA_EXECUTABLE} -e
        "pcall(require, 'luarocks.loader') require('lgi') require('lgi.version')"
    RESULT_VARIABLE LGI_CHECK_RESULT
    ERROR_VARIABLE LGI_CHECK_ERR
)
if(NOT LGI_CHECK_RESULT EQUAL 0)
    if(NOT DEFINED ENV{AWESOME_IGNORE_LGI})
        message(FATAL_ERROR "LGI check failure: ${LGI_CHECK_ERR}")
    endif()
endif()

# }}}

# {{{ Generate some aggregated documentation from lua script

add_custom_target(setup_directories)

add_custom_command(TARGET setup_directories
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}/script_files/
        COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}/docs/common/
        COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}/doc/images/
        COMMAND ${CMAKE_COMMAND} -E copy ${SOURCE_DIR}/docs/_parser.lua ${BUILD_DIR}/docs/
)

add_custom_command(
        OUTPUT ${BUILD_DIR}/docs/06-appearance.md
        COMMAND ${LUA_EXECUTABLE} ${SOURCE_DIR}/docs/06-appearance.md.lua
        ${BUILD_DIR}/docs/06-appearance.md
        DEPENDS
            ${SOURCE_DIR}/docs/06-appearance.md.lua
            ${SOURCE_DIR}/docs/_parser.lua
)

foreach(RULE_TYPE client tag screen notification)
    add_custom_command(
        OUTPUT ${BUILD_DIR}/docs/common/${RULE_TYPE}_rules_index.ldoc
        COMMAND ${LUA_EXECUTABLE} ${SOURCE_DIR}/docs/build_rules_index.lua
            ${BUILD_DIR}/docs/common/${RULE_TYPE}_rules_index.ldoc
            ${RULE_TYPE}

        # Cheap trick until the ldoc `configure_file` is ported to be a build
        # step rather than part of cmake.
        COMMAND ${CMAKE_COMMAND} -E
            copy ${BUILD_DIR}/docs/common/${RULE_TYPE}_rules_index.ldoc
                 ${SOURCE_DIR}/docs/common/${RULE_TYPE}_rules_index.ldoc

        DEPENDS
            ${SOURCE_DIR}/docs/build_rules_index.lua
            ${SOURCE_DIR}/docs/_parser.lua
    )
endforeach()

add_custom_command(
        OUTPUT ${BUILD_DIR}/awesomerc.lua ${BUILD_DIR}/docs/05-awesomerc.md
            ${BUILD_DIR}/script_files/rc.lua
        COMMAND ${LUA_EXECUTABLE} ${SOURCE_DIR}/docs/05-awesomerc.md.lua
        ${BUILD_DIR}/docs/05-awesomerc.md ${SOURCE_DIR}/awesomerc.lua
        ${BUILD_DIR}/awesomerc.lua
        ${BUILD_DIR}/script_files/rc.lua
        DEPENDS ${SOURCE_DIR}/awesomerc.lua ${SOURCE_DIR}/docs/05-awesomerc.md.lua
)

add_custom_command(
        OUTPUT ${BUILD_DIR}/script_files/theme.lua
        COMMAND ${LUA_EXECUTABLE} ${SOURCE_DIR}/docs/sample_theme.lua ${BUILD_DIR}/script_files/
)

# Create a target for the auto-generated awesomerc.lua and other files
add_custom_target(generate_awesomerc DEPENDS
    setup_directories
    ${BUILD_DIR}/awesomerc.lua
    ${BUILD_DIR}/script_files/theme.lua
    ${BUILD_DIR}/script_files/rc.lua
    ${SOURCE_DIR}/awesomerc.lua
    ${BUILD_DIR}/docs/06-appearance.md
    ${SOURCE_DIR}/docs/05-awesomerc.md.lua
    ${SOURCE_DIR}/docs/build_rules_index.lua
    ${BUILD_DIR}/docs/common/client_rules_index.ldoc
    ${BUILD_DIR}/docs/common/tag_rules_index.ldoc
    ${BUILD_DIR}/docs/common/screen_rules_index.ldoc
    ${BUILD_DIR}/docs/common/notification_rules_index.ldoc
    ${SOURCE_DIR}/docs/sample_theme.lua
    ${SOURCE_DIR}/docs/sample_files.lua
    ${SOURCE_DIR}/awesomerc.lua
    ${awesome_c_configure_files}
    ${awesome_lua_configure_files}
)


#}}}

# {{{ Copy additional files
file(GLOB awesome_md_docs RELATIVE ${SOURCE_DIR}
    ${SOURCE_DIR}/docs/*.md)
set(AWESOME_ADDITIONAL_FILES
    ${awesome_md_docs})

foreach(file ${AWESOME_ADDITIONAL_FILES})
    configure_file(${SOURCE_DIR}/${file}
                   ${BUILD_DIR}/${file}
                   @ONLY)
endforeach()
#}}}

# vim: filetype=cmake:expandtab:shiftwidth=4:tabstop=8:softtabstop=4:textwidth=80:foldmethod=marker
