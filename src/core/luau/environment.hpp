#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>

#include "datafile.hpp"
#include "lualib.h"
#include "vm.hpp"

namespace sonata::luau {

struct EnvNamespace;

struct Global {
    using Function = int (*)(VM&);
    using Namespace = std::shared_ptr<EnvNamespace>;

    /*
     * A custom Luau type: a C++ value exposed to Luau as full userdata with
     * a metatable, instead of one of the scalar types above.
     *
     * "typeName" must be a stable, unique string per C++ type. It doubles
     * as the registry name Luau's own luaL_newmetatable()/luaL_checkudata()
     * use internally, so a given type's metatable is built at most once per
     * lua_State no matter how many Custom globals of that type get loaded
     * into it.
     *
     * "build" fills in the metatable. It runs the first time this typeName
     * is loaded into a given lua_State, with a fresh, empty table on top of
     * the stack. A "__index = <the metatable itself>"" default is set before
     * build() runs, so registering ordinary methods is enough for OOP-style
     * access (self:method(...)); build() can overwrite __index (e.g. with a
     * C function) if it needs something else, such as computed properties.
     * "__gc" and "__type" are set automatically *after* build() runs and
     * always win, so build() must not set either itself.
     *
     * "value" is the instance, type-erased. Loading copies this shared_ptr
     * into the pushed userdata, so the object stays alive for as long as
     * either this Global or a live Luau reference to the pushed value does.
     *
     * See customSelf<T>() and makeCustomGlobal<T>() below for building and
     * consuming these without touching the type erasure by hand.
     */
    struct Custom {
        std::string typeName;
        void (*build)(lua_State* L);
        std::shared_ptr<void> value;
    };

    std::variant<
        Function,
        std::string,
        double,
        bool,
        Namespace,
        Custom
    > value;

    Global(Function function);
    Global(std::string string);
    Global(const char* string);
    Global(double number);
    Global(bool boolean);
    Global(Namespace namespace_);
    Global(Custom custom);

    Global(const Global&) = default;
    Global(Global&&) noexcept = default;

    Global& operator=(const Global&) = default;
    Global& operator=(Global&&) noexcept = default;
};

/*
 * Retrieves the T instance out of a Custom-type userdata argument at stack
 * index "index". Raises a Luau error (via luaL_checkudata) if that argument
 * isn't userdata tagged with "typeName"'s metatable.
 *
 * Call this from the C functions registered by a Global::Custom::build
 * callback, passing the same typeName that Custom was built with:
 *
 *     int getX(lua_State* L) {
 *         Vector3* self = customSelf<Vector3>(L, 1, "Vector3");
 *         lua_pushnumber(L, self->x);
 *         return 1;
 *     }
 */
template <typename T>
T* customSelf(lua_State* L, int index, const char* typeName)
{
    void* box = luaL_checkudata(L, index, typeName);
    return static_cast<T*>(static_cast<std::shared_ptr<void>*>(box)->get());
}

/*
 * Builds a Global::Custom wrapping a fresh copy of "value".
 *
 *     env.set("origin", makeCustomGlobal("Vector3", Vector3{0, 0, 0}, &buildVector3));
 */
template <typename T>
Global makeCustomGlobal(
    std::string typeName,
    T value,
    void (*build)(lua_State* L)
)
{
    return Global(Global::Custom{
        std::move(typeName),
        build,
        std::static_pointer_cast<void>(std::make_shared<T>(std::move(value)))
    });
}

struct EnvNamespace {
    using Members = std::unordered_map<std::string, Global>;

    Members members;

    EnvNamespace() = default;

    EnvNamespace& set(std::string name, Global global);

    EnvNamespace& setString(
        std::string name,
        std::string value
    );

    EnvNamespace& setNumber(
        std::string name,
        double value
    );

    EnvNamespace& setBoolean(
        std::string name,
        bool value
    );

    EnvNamespace& setFunction(
        std::string name,
        Global::Function function
    );

    EnvNamespace& setCustom(
        std::string name,
        Global::Custom custom
    );

    [[nodiscard]]
    Global* find(const std::string& name);

    [[nodiscard]]
    const Global* find(const std::string& name) const;

    bool remove(const std::string& name);

    [[nodiscard]]
    bool contains(const std::string& name) const;

    [[nodiscard]]
    bool empty() const noexcept;

    [[nodiscard]]
    std::size_t size() const noexcept;
};

/*
Represents the global environment available to a VM.

The Environment itself lives on the C++ side. Calling load()
translates its contents into actual Luau globals.
*/
class Environment {
public:
    using Globals = std::unordered_map<std::string, Global>;

    Environment() = default;

    /*
     * Add or replace a global.
     */
    Environment& set(std::string name, Global global);

    /*
     * Convenience setters.
     */
    Environment& setFunction(
        std::string name,
        Global::Function function
    );

    Environment& setString(
        std::string name,
        std::string value
    );

    Environment& setNumber(
        std::string name,
        double value
    );

    Environment& setBoolean(
        std::string name,
        bool value
    );

    Environment& setCustom(
        std::string name,
        Global::Custom custom
    );

    /*
     * Creates a namespace from an existing EnvNamespace.
     */
    Environment& setNamespace(
        std::string name,
        EnvNamespace namespace_
    );

    /*
     * Uses an already allocated namespace.
     */
    Environment& setNamespace(
        std::string name,
        std::shared_ptr<EnvNamespace> namespace_
    );

    /*
     * Look up a global stored by this environment.
     */
    [[nodiscard]]
    Global* find(const std::string& name);

    [[nodiscard]]
    const Global* find(const std::string& name) const;

    [[nodiscard]]
    bool contains(const std::string& name) const;

    /*
     * Remove a global from the Environment.

     * Note: this does not remove the already-loaded global from a VM.
     */
    bool remove(const std::string& name);

    void clear();

    [[nodiscard]]
    bool empty() const noexcept;

    [[nodiscard]]
    std::size_t size() const noexcept;

    /*
     * Load every stored global into the VM.
     *
     * Existing globals in the VM with the same names are replaced.
     */
    void load(VM& vm) const;

    /*
     * Load a single global into the VM.
     *
     * Returns false if the global isn't stored in this Environment.
     */
    bool load(VM& vm, const std::string& name) const;

    [[nodiscard]]
    const Globals& globals() const noexcept;

    [[nodiscard]]
    Globals& globals() noexcept;

private:
    Globals globals_;
};

/*
 * Optional bridge to DataFile (datafile.hpp): turns a parsed DataValue into
 * a Global, so config data read from a plain `return { ... }` .luau file
 * can also be loaded onto a VM as a real Luau global, not just read on the
 * C++ side. Nested tables become nested EnvNamespaces.
 *
 * A DataValue can never contain a Function or Custom (both require running
 * code to produce), so this only ever produces the scalar/Namespace
 * alternatives of Global. Nil entries are skipped when building a
 * namespace (there's no such thing as a "nil global"); calling toGlobal()
 * directly on a Nil value is an error.
 *
 * Positional (array-part) entries are exposed under their 1-based Luau
 * index as a string key ("1", "2", ...), since EnvNamespace/Environment key
 * everything by name.
 */
[[nodiscard]] Global toGlobal(const DataValue& value);
[[nodiscard]] EnvNamespace toNamespace(const DataValue& table);

} // namespace sonata::luau