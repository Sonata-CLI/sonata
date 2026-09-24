#include "environment.hpp"

#include <cstdio>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <unordered_set>
#include <utility>

#include "lualib.h"

namespace sonata::luau {

namespace {

struct FunctionBinding {
    VM* vm;
    Global::Function function;
};

/*
 * Trampoline used by Luau C closures.

 * Luau calls this with lua_State*. We recover our VM and the original
 * Function callback from the userdata upvalue.
 */
int callGlobalFunction(lua_State* L)
{
    auto* binding = static_cast<FunctionBinding*>(
        lua_touserdata(L, lua_upvalueindex(1))
    );

    if (!binding || !binding->vm || !binding->function)
    {
        lua_pushstring(L, "invalid Sonata global function binding");
        lua_error(L);
        return 0; // unreachable, satisfies the C++ return type
    }

    try
    {
        return binding->function(*binding->vm);
    }
    catch (const std::exception& e)
    {
        lua_pushstring(L, e.what());
        lua_error(L);
        return 0; // unreachable
    }
    catch (...)
    {
        lua_pushstring(L, "unknown C++ exception in Sonata global function");
        lua_error(L);
        return 0; // unreachable
    }
}

/*
 * __gc for every Custom-type userdata: destroys the shared_ptr<void> placed
 * in the userdata's storage by pushCustom(). This always wins over whatever
 * a Global::Custom::build() callback set, so build() must not set __gc.
 */
int customGC(lua_State* L)
{
    auto* slot = static_cast<std::shared_ptr<void>*>(lua_touserdata(L, 1));

    if (slot)
        slot->~shared_ptr();

    return 0;
}

/*
 * Pushes a Custom global: full userdata holding a copy of custom.value,
 * tagged with a metatable named custom.typeName. The metatable is created
 * and built (via custom.build) only the first time this typeName is seen
 * on this particular lua_State; luaL_newmetatable() itself tracks that
 * through the Lua registry, so repeated pushes of the same type just reuse
 * it.
 */
void pushCustom(lua_State* L, const Global::Custom& custom)
{
    auto* slot = static_cast<std::shared_ptr<void>*>(
        lua_newuserdata(L, sizeof(std::shared_ptr<void>))
    );

    // Placement-new a copy of the shared_ptr into the userdata: the pushed
    // Luau value keeps the C++ object alive independently of whatever
    // Environment/EnvNamespace produced this Global.
    new (slot) std::shared_ptr<void>(custom.value);

    if (luaL_newmetatable(L, custom.typeName.c_str()))
    {
        // First time this type name has been loaded into this lua_State.
        // Sane OOP default; build() can replace __index if it wants
        // something else (e.g. a C function for computed properties).
        lua_pushvalue(L, -1);
        lua_setfield(L, -2, "__index");

        custom.build(L);

        // These always win over whatever build() did.
        lua_pushcclosure(L, customGC, nullptr, 0);
        lua_setfield(L, -2, "__gc");

        lua_pushlstring(L, custom.typeName.data(), custom.typeName.size());
        lua_setfield(L, -2, "__type");
    }

    lua_setmetatable(L, -2);
}

using ActiveNamespaces = std::unordered_set<const EnvNamespace*>;

void pushNamespace(
    lua_State* L,
    VM& vm,
    const EnvNamespace& namespace_,
    ActiveNamespaces& active
);

void pushGlobal(
    lua_State* L,
    VM& vm,
    const Global& global,
    ActiveNamespaces& active
)
{
    std::visit(
        [&](const auto& value)
        {
            using T = std::decay_t<decltype(value)>;

            if constexpr (std::is_same_v<T, Global::Function>)
            {
                if (!value)
                {
                    throw std::runtime_error(
                        "Environment contains a null function"
                    );
                }

                /*
                 * Store both VM and function inside userdata.

                 * The userdata becomes an upvalue of the Lua closure,
                 * so it remains alive for as long as the closure does.
                 */
                auto* binding = static_cast<FunctionBinding*>(
                    lua_newuserdata(L, sizeof(FunctionBinding))
                );

                binding->vm = &vm;
                binding->function = value;

                /*
                 * Current Luau API:
                 *
                 * lua_pushcclosure(L, fn, debugname, nup)
                 */
                lua_pushcclosure(
                    L,
                    callGlobalFunction,
                    nullptr,
                    1
                );
            }
            else if constexpr (std::is_same_v<T, std::string>)
            {
                lua_pushlstring(
                    L,
                    value.data(),
                    value.size()
                );
            }
            else if constexpr (std::is_same_v<T, double>)
            {
                lua_pushnumber(L, value);
            }
            else if constexpr (std::is_same_v<T, bool>)
            {
                lua_pushboolean(L, value);
            }
            else if constexpr (std::is_same_v<T, Global::Namespace>)
            {
                if (!value)
                {
                    throw std::runtime_error(
                        "Environment contains a null namespace"
                    );
                }

                pushNamespace(
                    L,
                    vm,
                    *value,
                    active
                );
            }
            else if constexpr (std::is_same_v<T, Global::Custom>)
            {
                if (value.typeName.empty())
                {
                    throw std::runtime_error(
                        "Environment contains a custom global with an empty type name"
                    );
                }

                if (!value.build)
                {
                    throw std::runtime_error(
                        "Environment contains a custom global with a null build function"
                    );
                }

                if (!value.value)
                {
                    throw std::runtime_error(
                        "Environment contains a custom global with null data"
                    );
                }

                pushCustom(L, value);
            }
        },
        global.value
    );
}

void pushNamespace(
    lua_State* L,
    VM& vm,
    const EnvNamespace& namespace_,
    ActiveNamespaces& active
)
{
    /*
     * Because namespaces use shared_ptr, it is possible for somebody
     * to accidentally construct:
     *
     *     A -> B -> A
     *
     * Detect that instead of recursing forever.
     */
    if (!active.insert(&namespace_).second)
    {
        throw std::runtime_error(
            "cyclic namespace detected while loading Environment"
        );
    }

    lua_createtable(
        L,
        0,
        static_cast<int>(namespace_.members.size())
    );

    try
    {
        for (const auto& [name, global] : namespace_.members)
        {
            pushGlobal(
                L,
                vm,
                global,
                active
            );

            /*
             * Table is still below the value at -2.
             * lua_setfield pops the value.
             */
            lua_setfield(
                L,
                -2,
                name.c_str()
            );
        }
    }
    catch (...)
    {
        active.erase(&namespace_);
        throw;
    }

    active.erase(&namespace_);
}

std::string formatIndex(double value)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.17g", value);
    return buf;
}

} // namespace


// Global
Global::Global(Function function)
    : value(function)
{
}

Global::Global(std::string string)
    : value(std::move(string))
{
}

Global::Global(const char* string)
    : value(std::string(string))
{
}

Global::Global(double number)
    : value(number)
{
}

Global::Global(bool boolean)
    : value(boolean)
{
}

Global::Global(Namespace namespace_)
    : value(std::move(namespace_))
{
    if (!std::get<Namespace>(value))
    {
        throw std::invalid_argument(
            "Global namespace cannot be null"
        );
    }
}

Global::Global(Custom custom)
    : value(std::move(custom))
{
}


// EnvNamespace
EnvNamespace& EnvNamespace::set(
    std::string name,
    Global global
)
{
    members.insert_or_assign(
        std::move(name),
        std::move(global)
    );

    return *this;
}

EnvNamespace& EnvNamespace::setString(
    std::string name,
    std::string value
)
{
    return set(
        std::move(name),
        Global(std::move(value))
    );
}

EnvNamespace& EnvNamespace::setNumber(
    std::string name,
    double value
)
{
    return set(
        std::move(name),
        Global(value)
    );
}

EnvNamespace& EnvNamespace::setBoolean(
    std::string name,
    bool value
)
{
    return set(
        std::move(name),
        Global(value)
    );
}

EnvNamespace& EnvNamespace::setFunction(
    std::string name,
    Global::Function function
)
{
    return set(
        std::move(name),
        Global(function)
    );
}

EnvNamespace& EnvNamespace::setCustom(
    std::string name,
    Global::Custom custom
)
{
    return set(
        std::move(name),
        Global(std::move(custom))
    );
}

Global* EnvNamespace::find(const std::string& name)
{
    auto it = members.find(name);

    if (it == members.end())
        return nullptr;

    return &it->second;
}

const Global* EnvNamespace::find(const std::string& name) const
{
    auto it = members.find(name);

    if (it == members.end())
        return nullptr;

    return &it->second;
}

bool EnvNamespace::remove(const std::string& name)
{
    return members.erase(name) != 0;
}

bool EnvNamespace::contains(const std::string& name) const
{
    return members.find(name) != members.end();
}

bool EnvNamespace::empty() const noexcept
{
    return members.empty();
}

std::size_t EnvNamespace::size() const noexcept
{
    return members.size();
}

// ---------------------------------------------------------
// Environment
// ---------------------------------------------------------

Environment& Environment::set(
    std::string name,
    Global global
)
{
    globals_.insert_or_assign(
        std::move(name),
        std::move(global)
    );

    return *this;
}

Environment& Environment::setFunction(
    std::string name,
    Global::Function function
)
{
    return set(
        std::move(name),
        Global(function)
    );
}

Environment& Environment::setString(
    std::string name,
    std::string value
)
{
    return set(
        std::move(name),
        Global(std::move(value))
    );
}

Environment& Environment::setNumber(
    std::string name,
    double value
)
{
    return set(
        std::move(name),
        Global(value)
    );
}

Environment& Environment::setBoolean(
    std::string name,
    bool value
)
{
    return set(
        std::move(name),
        Global(value)
    );
}

Environment& Environment::setCustom(
    std::string name,
    Global::Custom custom
)
{
    return set(
        std::move(name),
        Global(std::move(custom))
    );
}

Environment& Environment::setNamespace(
    std::string name,
    EnvNamespace namespace_
)
{
    return set(
        std::move(name),
        Global(
            std::make_shared<EnvNamespace>(
                std::move(namespace_)
            )
        )
    );
}

Environment& Environment::setNamespace(
    std::string name,
    std::shared_ptr<EnvNamespace> namespace_
)
{
    return set(
        std::move(name),
        Global(std::move(namespace_))
    );
}

Global* Environment::find(const std::string& name)
{
    auto it = globals_.find(name);

    if (it == globals_.end())
        return nullptr;

    return &it->second;
}

const Global* Environment::find(const std::string& name) const
{
    auto it = globals_.find(name);

    if (it == globals_.end())
        return nullptr;

    return &it->second;
}

bool Environment::contains(const std::string& name) const
{
    return globals_.find(name) != globals_.end();
}

bool Environment::remove(const std::string& name)
{
    return globals_.erase(name) != 0;
}

void Environment::clear()
{
    globals_.clear();
}

bool Environment::empty() const noexcept
{
    return globals_.empty();
}

std::size_t Environment::size() const noexcept
{
    return globals_.size();
}

void Environment::load(VM& vm) const
{
    lua_State* L = vm.state();

    if (!L)
    {
        throw std::runtime_error(
            "Environment::load called with a VM containing a null lua_State"
        );
    }

    /*
     * Loading should not leave anything on the Lua stack.
     *
     * If a C++ validation error occurs while building a namespace,
     * restore the stack before rethrowing.
     */
    const int initialTop = lua_gettop(L);

    try
    {
        ActiveNamespaces active;

        for (const auto& [name, global] : globals_)
        {
            pushGlobal(
                L,
                vm,
                global,
                active
            );

            /*
             * The value produced by pushGlobal is on top of the stack.
             * lua_setglobal stores it into the VM's global table and pops it.
             */
            lua_setglobal(
                L,
                name.c_str()
            );
        }
    }
    catch (...)
    {
        lua_settop(L, initialTop);
        throw;
    }
}

bool Environment::load(
    VM& vm,
    const std::string& name
) const
{
    const auto* global = find(name);

    if (!global)
        return false;

    lua_State* L = vm.state();

    if (!L)
    {
        throw std::runtime_error(
            "Environment::load called with a VM containing a null lua_State"
        );
    }

    const int initialTop = lua_gettop(L);

    try
    {
        ActiveNamespaces active;

        pushGlobal(
            L,
            vm,
            *global,
            active
        );

        lua_setglobal(
            L,
            name.c_str()
        );
    }
    catch (...)
    {
        lua_settop(L, initialTop);
        throw;
    }

    return true;
}

const Environment::Globals& Environment::globals() const noexcept
{
    return globals_;
}

Environment::Globals& Environment::globals() noexcept
{
    return globals_;
}


// DataValue bridge
Global toGlobal(const DataValue& value)
{
    switch (value.kind())
    {
        case DataValue::Kind::Boolean:
            return Global(value.asBoolean());

        case DataValue::Kind::Number:
            return Global(value.asNumber());

        case DataValue::Kind::String:
            return Global(value.asString());

        case DataValue::Kind::Table:
            return Global(std::make_shared<EnvNamespace>(toNamespace(value)));

        case DataValue::Kind::Nil:
            break;
    }

    throw std::invalid_argument(
        "cannot convert a nil DataValue to a Global -- filter Nil entries "
        "out first (toNamespace() already does this for table fields)"
    );
}

EnvNamespace toNamespace(const DataValue& table)
{
    EnvNamespace ns;
    std::size_t nextIndex = 1;

    for (const auto& entry : table.asTable())
    {
        if (entry.value.isNil())
            continue;

        std::string key;

        if (!entry.key)
        {
            key = std::to_string(nextIndex++);
        }
        else if (entry.key->kind == DataValue::Key::Kind::String)
        {
            key = entry.key->string;
        }
        else
        {
            key = formatIndex(entry.key->number);
        }

        ns.set(std::move(key), toGlobal(entry.value));
    }

    return ns;
}

} // namespace sonata::luau