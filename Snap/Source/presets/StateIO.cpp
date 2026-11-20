#include "StateIO.h"
#include <map>
#include <sstream>

using namespace juce;

namespace snap
{
namespace
{
using Entries = std::map<std::string_view, std::string_view, std::less<>>;

// Reads the flat <SNAP a=".."><PARAM id=".." value=".."/>..</SNAP> text inside JUCE's binary envelope with a single
// non-recursive scan instead of JUCE's XML parser (which recurses per level, expands DTD entities and over-reads
// truncated character references). JUCE's writer escapes '<', '>' and '"' inside values, so every state it writes
// splits cleanly into tags and attributes. Collects the root attributes and the top-level PARAMs (the first entry
// per ID wins) and the SNAPSHOT elements' attributes; returns false unless the text is one complete <SNAP> element without "<!".
bool scan(std::string_view xml, Entries& root, Entries& params, std::vector<Entries>& snapshots)
{
    const std::string_view space(" \t\r\n");
    int depth = 0;
    for (size_t open = xml.find('<'), close = 0; open != std::string_view::npos; open = xml.find('<', close))
    {
        if ((close = xml.find('>', open)) == std::string_view::npos)
            return false;
        auto tag = xml.substr(open + 1, close - open - 1);
        if (tag.empty() || tag[0] == '!' || (tag[0] == '/' && depth == 0))
            return false;
        if (tag[0] == '?') // the <?xml ?> declaration
            continue;
        if (tag[0] == '/' && --depth == 0)
            return true;
        if (tag[0] == '/')
            continue;
        const bool empty = tag.back() == '/';
        tag.remove_suffix(empty ? 1 : 0);
        const auto name = tag.substr(0, tag.find_first_of(space));
        if (depth == 0 && name != "SNAP")
            return false;
        Entries attributes;
        for (auto rest = tag.substr(name.size());;) // attributes: name="text"
        {
            const auto eq = rest.find("=\""), end = eq == std::string_view::npos ? eq : rest.find('"', eq + 2);
            if (end == std::string_view::npos)
                break;
            auto key = rest.substr(0, eq);
            key.remove_prefix(std::min(key.size(), key.find_first_not_of(space)));
            attributes.emplace(key, rest.substr(eq + 2, end - eq - 2));
            rest.remove_prefix(end + 1);
        }
        if (depth == 0)
            root = attributes;
        else if (depth == 1 && name == "PARAM" && attributes.count("id") > 0)
            params.emplace(attributes["id"], attributes.count("value") > 0 ? attributes["value"] : "?");
        else if (depth == 1 && name == "SNAPSHOT")
            snapshots.push_back(attributes);
        depth += empty ? 0 : 1;
        if (depth == 0)
            return true;
    }
    return false;
}

// Wholly a finite number, locale-independent; absurd magnitudes are clamped so later float casts stay defined.
std::optional<double> number(const Entries& entries, std::string_view key)
{
    const auto it = entries.find(key);
    if (it == entries.end())
        return std::nullopt;
    std::istringstream in { std::string(it->second) };
    in.imbue(std::locale::classic());
    double d = 0;
    if (in >> d && (in >> std::ws).eof())
        return jlimit(-1.0e6, 1.0e6, d);
    return std::nullopt;
}

void set(AudioProcessorValueTreeState& apvts, std::string_view id, std::optional<double> value)
{
    auto* p = apvts.getParameter(String(id.data(), id.size()));
    p->setValueNotifyingHost(value ? p->convertTo0to1(p->getNormalisableRange().snapToLegalValue((float) *value)) : p->getDefaultValue());
}

// 0.1.x: Mix was the shaper's dry/wet, so it scales Attack and Sustain; Output becomes the master Output.
// Modules did not exist, so they load switched off and the sound stays the shaper plus output trim.
void migrateVersion1(AudioProcessorValueTreeState& apvts, const Entries& params)
{
    const auto v1 = [&](std::string_view id, double lo, double hi, double def) { return jlimit(lo, hi, number(params, id).value_or(def)); };
    const double mix = v1("mix", 0, 100, 100) / 100;
    for (auto& s : parameterTable())
        set(apvts, s.id, std::nullopt);
    for (auto& m : modules)
        set(apvts, m.onId, 0.0);
    set(apvts, "attack", v1("attack", -100, 100, 0) * mix);
    set(apvts, "sustain", v1("sustain", -100, 100, 0) * mix);
    set(apvts, "out_gain", v1("output", -24, 12, 0));
}
} // namespace

std::string toString(const Locks& locks)
{
    std::string s;
    for (size_t g = 0; g < locks.size(); ++g)
        if (locks[g])
            s += (s.empty() ? "" : ",") + std::string(g == masterLock ? "master" : modules[g].name);
    return s;
}

Locks parseLocks(std::string_view text)
{
    Locks locks {};
    for (size_t g = 0; g < locks.size(); ++g)
    {
        const std::string_view name = g == masterLock ? "master" : modules[g].name;
        for (auto rest = text; !rest.empty();)
        {
            const auto comma = rest.find(',');
            locks[g] = locks[g] || rest.substr(0, comma) == name;
            rest.remove_prefix(comma == std::string_view::npos ? rest.size() : comma + 1);
        }
    }
    return locks;
}

void writeState(AudioProcessorValueTreeState& apvts, const Locks& locks, const Snapshots& snapshots, MemoryBlock& dest)
{
    auto state = apvts.copyState();
    state.setProperty("stateVersion", stateVersion, nullptr).setProperty("locks", String(toString(locks)), nullptr);
    for (size_t slot = 0; slot < snapshots.size(); ++slot)
        if (const auto& snapshot = snapshots[slot])
        {
            ValueTree child("SNAPSHOT");
            child.setProperty("slot", (int) slot, nullptr).setProperty("order", String(toString(snapshot->order)), nullptr);
            for (size_t i = 0; i < parameterTable().size(); ++i)
                child.setProperty(parameterTable()[i].id, String(snapshot->values[i], 9), nullptr);
            state.appendChild(child, nullptr);
        }
    if (auto xml = state.createXml())
        AudioProcessor::copyXmlToBinary(*xml, dest);
}

bool readState(AudioProcessorValueTreeState& apvts, Locks& locks, Snapshots& snapshots, const void* data, int size, Load mode)
{
    const auto* bytes = static_cast<const char*>(data);
    if (bytes == nullptr || size < 8 || ByteOrder::littleEndianInt(bytes) != 0x21324356 || ByteOrder::littleEndianInt(bytes + 4) > (uint32) size - 8)
        return false;
    Entries root, params;
    std::vector<Entries> stored;
    if (!scan({ bytes + 8, ByteOrder::littleEndianInt(bytes + 4) }, root, params, stored))
        return false;
    const bool preset = mode == Load::preset;
    const auto current = parseChainOrder(apvts.state.getProperty("order").toString().toStdString()).value_or(defaultChainOrder);
    const auto setOrder = [&](const ChainOrder& o) { apvts.state.setProperty("order", String(toString(o)), preset ? apvts.undoManager : nullptr); };
    if (!preset)
    {
        locks = parseLocks(root.count("locks") > 0 ? root["locks"] : "");
        snapshots = {};
        for (const char* editorProperty : { "preset", "ui_scale" }) // the current preset's key and the editor's size
            if (root.count(editorProperty) == 0)
                apvts.state.removeProperty(editorProperty, nullptr);
            else
                apvts.state.setProperty(editorProperty, String::fromUTF8(root[editorProperty].data(), (int) root[editorProperty].size())
                                                            .replace("&quot;", "\"").replace("&apos;", "'").replace("&lt;", "<")
                                                            .replace("&gt;", ">").replace("&amp;", "&"),
                                        nullptr);
    }
    if (number(root, "stateVersion").value_or(1) < 2) // 0.1.0 and 0.1.1 always wrote 1; treat a missing version as theirs
    {
        migrateVersion1(apvts, params);
        setOrder(defaultChainOrder);
        return true;
    }
    for (auto& s : parameterTable())
        if (!preset || (!isSessionToggle(s.id) && (lockGroup(s.id) < 0 || !locks[(size_t) lockGroup(s.id)])))
            set(apvts, s.id, number(params, s.id));
    const auto order = parseChainOrder(root.count("order") > 0 ? root["order"] : "").value_or(defaultChainOrder);
    setOrder(preset ? pinLocked(current, order, { locks[0], locks[1], locks[2], locks[3], locks[4], locks[5] }) : order);
    for (auto& entries : preset ? std::vector<Entries> {} : stored)
        if (const auto slot = number(entries, "slot"); slot && *slot >= 0 && *slot < (double) snapshots.size())
        {
            Snapshot snapshot { {}, parseChainOrder(entries.count("order") > 0 ? entries["order"] : "").value_or(defaultChainOrder) };
            for (auto& s : parameterTable())
                snapshot.values.push_back((float) jlimit(0.0, 1.0, number(entries, s.id).value_or(apvts.getParameter(s.id)->getDefaultValue())));
            snapshots[(size_t) *slot] = std::move(snapshot);
        }
    return true;
}
} // namespace snap
