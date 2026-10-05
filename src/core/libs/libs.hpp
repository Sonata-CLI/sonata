#pragma once

struct lua_State;

// Openers for the built-in libraries. Private to src/core: the outside world
// only ever sees them through LibraryRegistry / require("@sonata/<name>").
namespace sonata::lib::libs {

void openExample(lua_State* L); // @sonata/example
void openFs(lua_State* L);      // @sonata/fs

} // namespace sonata::luau::libs