#pragma once

#include "lua.h"
#include "compiler.hpp"

namespace sonata::luau {
	
class VM {
public:
    VM();
    ~VM();

    VM(const VM&) = delete; // Prevent duplicating by VM b = a;
	VM& operator=(const VM&) = delete; // Prevent duplicating by VM(b) = VM(a)

    lua_State* state();

    int execute(const Bytecode& bytecode);

private:
    lua_State* L;
};

} // namespace sonata::luau