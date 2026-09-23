#include "environment.hpp"

#include <stdexcept>
#include <type_traits>
#include <unordered_set>
#include <utility>

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

} // namespace

// ---------------------------------------------------------
// Global
// ---------------------------------------------------------

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

// ---------------------------------------------------------
// EnvNamespace
// ---------------------------------------------------------

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

} // namespace sonata::luau