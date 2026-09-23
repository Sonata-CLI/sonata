#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "Luau/Compiler.h"
#include "lua.h"

struct lua_State;

namespace sonata::luau {
class Bytecode {
public:
    using Byte = std::uint8_t;

    Bytecode() = default;

    // Construct from raw bytes.
    Bytecode(const void* data, std::size_t size);

    // Construct from Luau's std::string output.
    explicit Bytecode(const std::string& data);

    // Construct by taking ownership of a byte vector.
    explicit Bytecode(std::vector<Byte> data);

    // Access
    const Byte* data() const noexcept;
    Byte* data() noexcept;

    std::size_t size() const noexcept;
    bool empty() const noexcept;

    // Convenient conversion for Luau APIs.
    const char* luauData() const noexcept;

    // File IO
    bool save(const std::filesystem::path& path) const;
    static Bytecode load(const std::filesystem::path& path);

    // Direct access if needed.
    const std::vector<Byte>& bytes() const noexcept;
    std::vector<Byte>& bytes() noexcept;

private:
    std::vector<Byte> m_bytes;
};

class Compiler {
public:
    Bytecode compile(std::string_view source) const;
};
}