#pragma once
#include "../dsp/modules/Modules.h"
#include <algorithm>
#include <optional>
#include <string>

namespace snap
{
// Order of the four reorderable middle slots, stored in the state (not a parameter) as e.g. "shift,space,press,drive".
using ChainOrder = std::array<Module, 4>;
inline constexpr ChainOrder defaultChainOrder { Module::shift, Module::space, Module::press, Module::drive };

inline std::string toString(const ChainOrder& order)
{
    std::string s;
    for (auto m : order)
        s += (s.empty() ? "" : ",") + std::string(modules[(size_t) m].name);
    return s;
}

// Accepts only a permutation of the four middle modules.
inline std::optional<ChainOrder> parseChainOrder(std::string_view text)
{
    ChainOrder order {};
    for (size_t i = 0; i < order.size(); ++i)
    {
        const auto comma = text.find(',');
        const auto name = text.substr(0, comma);
        const auto* found = std::find_if(modules.begin() + 1, modules.end() - 1, [&](auto& m) { return m.name == name; });
        if (found == modules.end() - 1 || std::find(order.begin(), order.begin() + (long) i, found->module) != order.begin() + (long) i)
            return std::nullopt;
        order[i] = found->module;
        if ((comma == std::string_view::npos) != (i + 1 == order.size()))
            return std::nullopt;
        text.remove_prefix(comma == std::string_view::npos ? text.size() : comma + 1);
    }
    return order;
}
} // namespace snap
