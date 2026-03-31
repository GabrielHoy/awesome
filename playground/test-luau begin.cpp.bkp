#include <lua.h>
#include <lualib.h>
#include <luacode.h>
#include <cctype>
#include "util.hpp"
#include <iostream>

PlaygroundFunc(RegistryStorageAndRetrieval, {
    lua_State *L = luaL_newstate();
    luaL_openlibs(L);

    // Test: store and retrieve from registry using a pointer key
    int dummy = 42;
    void *key = &dummy;

    // Push a table, then set the registry key for the &dummy'key' to the table
    lua_createtable(L, 0, 0);                                 // push a table
    lua_pushlightuserdatatagged(L, key, 0);                       // push key
    lua_pushvalue(L, -2);                                           // dup the table
    lua_rawset(L, LUA_REGISTRYINDEX);                               // registry[key] = table
    lua_pop(L, 1);                                                  // pop the original table

    // Retrieve the table from the registry using the &dummy 'key'
    lua_pushlightuserdatatagged(L, key, 0);
    lua_rawget(L, LUA_REGISTRYINDEX);                                // pushes registry[key]
    printf("type: %s", lua_typename(L, lua_type(L, -1)));  // should print "table"

    lua_close(L);
})

int main() {
    std::cout << BRACKET_STR << std::endl;

    CheckPlaygroundFunc(RegistryStorageAndRetrieval);

    std::cout << BRACKET_STR << std::endl;
    return 0;
}
