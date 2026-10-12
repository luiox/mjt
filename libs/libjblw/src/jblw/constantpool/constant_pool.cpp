#include "constant_pool.hpp"

#include <stdexcept>

namespace jblw::constantpool {
namespace {
bool isWide(const Entry& entry) noexcept
{
    const EntryKind kind = entry.getType();
    return kind == EntryKind::LONG || kind == EntryKind::DOUBLE;
}
}   //namespace

ListConstantPool::ListConstantPool(std::vector<EntryPtr> entries)
    : entries_(std::move(entries))
{
    bool reserved = false;
    for (const auto& entry : entries_) {
        if (reserved) {
            if (entry) {
                throw std::invalid_argument(
                    "Long/Double constant must be followed by a reserved slot");
            }
            reserved = false;
            continue;
        }
        if (!entry) {
            throw std::invalid_argument("Reserved constant-pool slot must follow Long or Double");
        }
        reserved = isWide(*entry);
    }
    if (reserved) {
        throw std::invalid_argument("Long/Double constant must be followed by a reserved slot");
    }
}

const EntryPtr& ListConstantPool::get(ca::i32 index) const
{
    if (index < 0) throw std::out_of_range("Constant-pool index must not be negative");
    return entries_.at(static_cast<ca::usize>(index));
}

std::vector<ca::i32> ListConstantPool::indices() const
{
    std::vector<ca::i32> result;
    result.reserve(entries_.size());
    for (ca::usize i = 0; i < entries_.size(); ++i) {
        if (entries_[i]) result.push_back(static_cast<ca::i32>(i));
    }
    return result;
}

}   //namespace jblw::constantpool
