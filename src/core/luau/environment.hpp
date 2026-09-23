#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <variant>

#include "vm.hpp"

namespace sonata::luau {

struct EnvNamespace;

struct Global {
    using Function = int (*)(VM&);
    using Namespace = std::shared_ptr<EnvNamespace>;

    std::variant<
        Function,
        std::string,
        double,
        bool,
        Namespace
    > value;

    Global(Function function);
    Global(std::string string);
    Global(const char* string);
    Global(double number);
    Global(bool boolean);
    Global(Namespace namespace_);

    Global(const Global&) = default;
    Global(Global&&) noexcept = default;

    Global& operator=(const Global&) = default;
    Global& operator=(Global&&) noexcept = default;
};

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

} // namespace sonata::luau