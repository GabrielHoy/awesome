#ifndef _CONFIG_H_
#define _CONFIG_H_

#define AWESOME_LUA_LIB_PATH  "@AWESOME_LUA_LIB_PATH@"
#define LUA_CMOD_PATH         "@LUA_CMOD_PATH@"
#define LUA_SHARE_PATH        "@LUA_SHARE_PATH@"
/* Directory containing corelgiluau.so — placed on package.cpath at startup */
#define BUILD_LGI_PATH        "@CMAKE_BINARY_DIR@/lgi"
#define XDG_CONFIG_DIR        "@XDG_CONFIG_DIR@"
#define AWESOME_THEMES_PATH   "@AWESOME_THEMES_PATH@"
#define AWESOME_ICON_PATH     "@AWESOME_ICON_PATH@"
#define AWESOME_DEFAULT_CONF  "@AWESOME_SYSCONFDIR@/rc.lua"

#ifdef __CMAKE_GENERATING__
#cmakedefine WITH_DBUS
#cmakedefine WITH_XCB_ERRORS
#cmakedefine HAS_EXECINFO
#endif // __CMAKE_GENERATING__

#endif //_CONFIG_H_

// vim: filetype=c:expandtab:shiftwidth=4:tabstop=8:softtabstop=4:textwidth=80
