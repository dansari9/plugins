#include "StateIO.h"
#include "../Parameters.h"
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
// per ID wins); returns false unless the text is one complete <SNAP> element without "<!".
bool scan(std::string_view xml, Entries& root, Entries& params)
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

void writeState(AudioProcessorValueTreeState& apvts, const ChainOrder& order, MemoryBlock& dest)
{
    auto state = apvts.copyState();
    state.setProperty("stateVersion", stateVersion, nullptr).setProperty("order", String(toString(order)), nullptr);
    if (auto xml = state.createXml())
        AudioProcessor::copyXmlToBinary(*xml, dest);
}

void readState(AudioProcessorValueTreeState& apvts, ChainOrder& order, const void* data, int size)
{
    const auto* bytes = static_cast<const char*>(data);
    if (bytes == nullptr || size < 8 || ByteOrder::littleEndianInt(bytes) != 0x21324356 || ByteOrder::littleEndianInt(bytes + 4) > (uint32) size - 8)
        return;
    Entries root, params;
    if (!scan({ bytes + 8, ByteOrder::littleEndianInt(bytes + 4) }, root, params))
        return;
    order = defaultChainOrder;
    if (number(root, "stateVersion").value_or(1) < 2) // 0.1.0 and 0.1.1 always wrote 1; treat a missing version as theirs
        return migrateVersion1(apvts, params);
    for (auto& s : parameterTable())
        set(apvts, s.id, number(params, s.id));
    if (const auto it = root.find("order"); it != root.end())
        order = parseChainOrder(it->second).value_or(defaultChainOrder);
}
} // namespace snap
