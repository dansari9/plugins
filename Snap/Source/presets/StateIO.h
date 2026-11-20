#pragma once
#include "../Parameters.h"
#include "../chain/ChainOrder.h"
#include <optional>
#include <vector>

// Session state: the APVTS tree as XML, <SNAP stateVersion="2" order="..." locks="..."> with one <PARAM id value/> per
// parameter and up to four <SNAPSHOT slot order id="normalised value" .../>. Version 1 (snap 0.1.x) held attack, sustain,
// mix and output and is migrated on load.
namespace snap
{
inline constexpr int stateVersion = 2;

// A snapshot: every parameter but the session toggles, normalised (exact on recall), plus the middle order.
struct Snapshot
{
    std::vector<float> values; // parameterTable() order; session toggles are stored but never recalled
    ChainOrder order = defaultChainOrder;
};
using Snapshots = std::array<std::optional<Snapshot>, 4>;
using Locks = std::array<bool, lockCount>;

std::string toString(const Locks&);
Locks parseLocks(std::string_view);

void writeState(juce::AudioProcessorValueTreeState&, const Locks&, const Snapshots&, juce::MemoryBlock&);

// session: a host restore, everything is replaced (order, locks, snapshots). preset: the session toggles, locked groups,
// locks and snapshots stay; locked modules keep their slots. The order lands in the tree's "order" property, through the
// APVTS undo manager for presets. Data that is not one complete <SNAP> element changes nothing and returns false.
enum class Load { session, preset };
bool readState(juce::AudioProcessorValueTreeState&, Locks&, Snapshots&, const void* data, int size, Load);
} // namespace snap
