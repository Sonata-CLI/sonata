#include <fstream>
#include <stdexcept>

#include "compiler.hpp"

#include "lua.h"
#include "Luau/Compiler.h"
#include "Luau/BytecodeBuilder.h"

namespace sonata::luau {

Bytecode::Bytecode(const void* data, std::size_t size)
{
    if (data && size)
    {
        const auto* bytes = static_cast<const Bytecode::Byte*>(data);
        m_bytes.assign(bytes, bytes + size);
    }
}

Bytecode::Bytecode(const std::string& data)
    : Bytecode(data.data(), data.size())
{
}

Bytecode::Bytecode(std::vector<Bytecode::Byte> data)
    : m_bytes(std::move(data))
{
}

const Bytecode::Byte* Bytecode::data() const noexcept
{
    return m_bytes.data();
}

Bytecode::Byte* Bytecode::data() noexcept
{
    return m_bytes.data();
}

std::size_t Bytecode::size() const noexcept
{
    return m_bytes.size();
}

bool Bytecode::empty() const noexcept
{
    return m_bytes.empty();
}

const char* Bytecode::luauData() const noexcept
{
    return reinterpret_cast<const char*>(m_bytes.data());
}

bool Bytecode::save(const std::filesystem::path& path) const
{
    std::ofstream file(path, std::ios::binary);

    if (!file)
        return false;

    file.write(
        reinterpret_cast<const char*>(m_bytes.data()),
        static_cast<std::streamsize>(m_bytes.size())
    );

    return file.good();
}

Bytecode Bytecode::load(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);

    if (!file)
        throw std::runtime_error("Failed to open bytecode file");

    file.seekg(0, std::ios::end);
    const std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);

    if (size < 0)
        throw std::runtime_error("Failed to determine bytecode size");

    std::vector<Bytecode::Byte> data(static_cast<std::size_t>(size));

    if (size > 0)
    {
        file.read(
            reinterpret_cast<char*>(data.data()),
            size
        );

        if (!file)
            throw std::runtime_error("Failed to read bytecode file");
    }

    return Bytecode(std::move(data));
}

const std::vector<Bytecode::Byte>& Bytecode::bytes() const noexcept {
    return m_bytes;
}

std::vector<Bytecode::Byte>& Bytecode::bytes() noexcept {
    return m_bytes;
}

Bytecode Compiler::compile(std::string_view source) const
{
    Luau::CompileOptions coptions;
    Luau::ParseOptions poptions;
    //source, CompileOptions, ParseOptions, BytecodeEncoder
    std::string bytecode = Luau::compile(
        std::string(source),
        coptions,
        poptions
    );

    return Bytecode(std::move(bytecode));
}

} // namespace sonata::luau