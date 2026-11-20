#pragma once
#include <array>
#include <string_view>

namespace snap
{
// The six modules in their default chain position. sub/hiss is fixed first and push fixed last;
// the middle four may be reordered (see chain/ChainOrder.h).
enum class Module { subHiss, shift, space, press, drive, push };

struct ModuleInfo
{
    Module module;
    std::string_view name, onId;
    std::array<std::string_view, 2> amountIds; // sub/hiss has two amounts; the others leave the second empty
};

inline constexpr std::array<ModuleInfo, 6> modules { {
    { Module::subHiss, "sub/hiss", "subhiss_on", { "sub_amt", "hiss_amt" } },
    { Module::shift, "shift", "shift_on", { "shift_amt" } },
    { Module::space, "space", "space_on", { "space_amt" } },
    { Module::press, "press", "press_on", { "press_amt" } },
    { Module::drive, "drive", "drive_on", { "drive_amt" } },
    { Module::push, "push", "push_on", { "push_amt" } },
} };
} // namespace snap
