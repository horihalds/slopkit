#include "trainer/runtime/resolve.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <span>
#include <string>
#include <vector>

#include "expr/expression.hpp"
#include "scan/types.hpp"

namespace slopkit::trainer
{
    namespace
    {
        // Every pointer level of an expression goes through this: a level the target
        // cannot read fails the whole expression, which is what marks the entry
        // unresolved instead of silently guessing an address.
        std::expected<std::uint64_t, std::string>
        read_pointer(const Target& target, std::size_t pointer_size, std::uint64_t address)
        {
            const auto bytes = target.read(address, pointer_size);
            if (!bytes)
            {
                return std::unexpected(bytes.error());
            }

            std::uint64_t value = 0;
            for (std::size_t index = 0; index < bytes->size(); ++index)
            {
                value |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>((*bytes)[index])) << (8 * index);
            }
            return value;
        }
    }

    std::size_t entry_size(const PayloadEntry& entry)
    {
        const auto fixed = scan::value_size(entry.type);
        return fixed != 0 ? fixed : entry.bytes.size();
    }

    Resolver::Resolver(const Target& target, std::size_t pointer_size)
        : target_ {target},
          pointer_size_ {pointer_size == 4 || pointer_size == 8 ? pointer_size : target.pointer_size()}
    {
        refresh_modules();
    }

    void Resolver::refresh_modules()
    {
        modules_.clear();
        for (const auto& module : target_.modules())
        {
            modules_.push_back(expr::ModuleRef {module.name, module.base});
        }
    }

    void Resolver::refresh(const PayloadEntry& entry, LiveEntry& live) const
    {
        const auto expression = expr::parse(entry.expression);
        if (!expression)
        {
            live.resolved = false;
            live.note     = expression.error().message;
            live.bytes.clear();
            return;
        }

        const auto evaluated = expr::evaluate(
            *expression,
            modules_,
            {},
            [this](std::uint64_t address)
            {
                return read_pointer(target_, pointer_size_, address);
            },
            expr::Options {pointer_size_},
            [this](std::uint64_t address, std::size_t size)
            {
                return target_.readable(address, size);
            });
        if (!evaluated)
        {
            live.resolved = false;
            live.note     = evaluated.error().message;
            live.bytes.clear();
            return;
        }

        const auto size = entry_size(entry);
        if (size == 0)
        {
            live.resolved = false;
            live.note     = "the entry has no length recorded";
            live.bytes.clear();
            return;
        }

        const auto bytes = target_.read(*evaluated, size);
        if (!bytes)
        {
            live.resolved = false;
            live.note     = bytes.error();
            live.bytes.clear();
            return;
        }

        live.address  = *evaluated;
        live.resolved = true;
        live.note.clear();
        live.bytes = *bytes;
    }

    std::expected<void, std::string>
    Resolver::write(const PayloadEntry& entry, const LiveEntry& live, std::span<const std::byte> bytes) const
    {
        if (!live.resolved)
        {
            return std::unexpected(
                std::format("the address of entry \"{}\" is unresolved; refusing to write", entry.description));
        }
        return target_.write(live.address, bytes);
    }

}
