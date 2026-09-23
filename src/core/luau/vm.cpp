#include "vm.hpp"
#include "compiler.hpp"
#include "lua.h"
#include "lualib.h"
#include "luacode.h"

#include <string>
#include <iostream>

namespace sonata::luau {

VM::VM() {
	L = luaL_newstate();
	luaL_openlibs(L);
}

VM::~VM() {
	lua_close(L);
}

lua_State* VM::state() {
	return L;
}

int VM::execute(const Bytecode& bytecode) {
    int status = luau_load(
        L,
        "=execute",
        reinterpret_cast<const char*>(bytecode.data()),
        bytecode.size(),
        0
    );

    if (status != LUA_OK)
    {
        lua_pop(L, 1);
        return status;
    }

    status = lua_pcall(L, 0, LUA_MULTRET, 0);

    if (status != LUA_OK)
    {
        lua_pop(L, 1);
    }

    return status;
}

} // namespace sonata::luau