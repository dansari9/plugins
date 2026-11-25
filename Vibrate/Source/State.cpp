#include "Vibrate.h"
#include <optional>
#include <sstream>

using namespace juce;

namespace
{
// The family's flat state grammar from Crumb. Validate the entire document before invoking JUCE's XML reader.
bool isFlatVibrateXml(std::string_view xml)
{
    if (xml.size() > 1 << 20 || !CharPointer_UTF8::isValidString(xml.data(), (int) xml.size()))
        return false;
    size_t pos = 0;
    auto skip = [&] { while (pos < xml.size() && std::string_view(" \t\r\n").find(xml[pos]) != std::string_view::npos) ++pos; };
    auto name = [&] {
        const size_t start = pos;
        auto first = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == ':'; };
        if (pos < xml.size() && first(xml[pos]))
            while (++pos < xml.size() && (first(xml[pos]) || (xml[pos] >= '0' && xml[pos] <= '9') || xml[pos] == '-' || xml[pos] == '.')) {}
        return xml.substr(start, pos - start);
    };
    std::string_view stack[2];
    int depth = 0;
    bool complete = false, declaration = false;
    skip();
    while (pos < xml.size() && !complete)
    {
        if (xml[pos++] != '<' || pos == xml.size()) return false;
        const bool decl = xml[pos] == '?', closing = xml[pos] == '/';
        pos += decl || closing ? 1 : 0;
        const auto tag = name();
        if (tag.empty() || (decl && (declaration || depth != 0 || tag != "xml"))) return false;
        if (closing)
        {
            skip();
            if (depth == 0 || stack[depth - 1] != tag || pos == xml.size() || xml[pos++] != '>') return false;
            complete = --depth == 0;
        }
        else
        {
            std::vector<std::string_view> keys;
            for (;;)
            {
                const auto before = pos;
                skip();
                if (pos == xml.size()) return false;
                if (xml[pos] == '/' || xml[pos] == '>' || (decl && xml[pos] == '?')) break;
                if (pos == before) return false; // attributes require separating whitespace
                const auto key = name();
                if (key.empty() || std::find(keys.begin(), keys.end(), key) != keys.end()) return false;
                keys.push_back(key);
                skip();
                if (pos == xml.size() || xml[pos++] != '=') return false;
                skip();
                if (pos == xml.size() || (xml[pos] != '\'' && xml[pos] != '"')) return false;
                const char quote = xml[pos++];
                while (pos < xml.size() && xml[pos] != quote)
                {
                    const auto c = (unsigned char) xml[pos++];
                    if (c == '<' || (c < 32 && c != 9 && c != 10 && c != 13)) return false;
                    if (c != '&') continue;
                    const auto end = xml.find(';', pos);
                    if (end == std::string_view::npos) return false;
                    const auto ref = xml.substr(pos, end - pos);
                    if (ref != "amp" && ref != "lt" && ref != "gt" && ref != "quot" && ref != "apos")
                    {
                        if (ref.empty() || ref[0] != '#') return false; // before substr: "&;" has nothing to skip
                        const bool hex = ref.size() > 1 && ref[1] == 'x';
                        const auto digits = ref.substr(hex ? 2 : 1);
                        if (digits.empty() || digits.size() > 7
                            || digits.find_first_not_of(hex ? "0123456789abcdefABCDEF" : "0123456789") != std::string_view::npos)
                            return false;
                        const auto code = std::stoul(std::string(digits), nullptr, hex ? 16 : 10);
                        if (!(code == 9 || code == 10 || code == 13 || (code >= 32 && code <= 0xd7ff) || (code >= 0xe000 && code <= 0xfffd) || (code >= 0x10000 && code <= 0x10ffff))) return false;
                    }
                    pos = end + 1;
                }
                if (pos++ == xml.size()) return false;
            }
            const bool empty = xml[pos] == '/';
            if ((decl || empty) && xml[pos++] != (decl ? '?' : '/')) return false;
            if (pos == xml.size() || xml[pos++] != '>') return false;
            if (decl) declaration = true;
            else
            {
                if (depth == 2 || (depth == 0 && tag != "VIBRATE")) return false;
                stack[depth] = tag;
                depth += empty ? 0 : 1;
                complete = depth == 0;
            }
        }
        skip();
    }
    return complete && pos == xml.size();
}

std::unique_ptr<XmlElement> decode(std::string_view x) { return isFlatVibrateXml(x) ? XmlDocument::parse(String::fromUTF8(x.data(), (int) x.size())) : nullptr; }
std::optional<double> number(const String& t) // the whole text is one finite number (C locale); else nothing
{
    std::istringstream in(t.toStdString());
    in.imbue(std::locale::classic());
    double v = 0;
    return in >> v && std::isfinite(v) && (in >> std::ws).eof() ? std::optional(jlimit(-1.0e6, 1.0e6, v)) : std::nullopt;
}
} // namespace

struct VibrateProcessor::Transaction
{
    VibrateProcessor& p;
    const ScopedLock writer;
    const bool outer;
    explicit Transaction(VibrateProcessor& processor) : p(processor), writer(p.txLock), outer(!p.busy)
    {
        const ScopedLock lock(p.stateLock);
        if (outer)
        {
            p.serialize(p.committed);
            p.busy = true;
        }
    }

    ~Transaction()
    {
        const ScopedLock lock(p.stateLock);
        if (outer) p.busy = false;
    }
};

void VibrateProcessor::getStateInformation(MemoryBlock& dest)
{
    const ScopedLock lock(stateLock);
    if (busy)
        dest = committed;
    else
        serialize(dest);
}

void VibrateProcessor::serialize(MemoryBlock& dest)
{
    auto state = apvts.copyState();
    state.setProperty("stateVersion", stateVersion, nullptr);
    state.setProperty("preset", preset.load(), nullptr);
    if (preset < 0 && userName.isNotEmpty())
        state.setProperty("presetName", userName, nullptr);
    state.setProperty("ab", ab.load(), nullptr);
    for (std::size_t k = 0; k < slots.size(); ++k)
        if (slotStored[k])
        {
            std::ostringstream text;
            text.imbue(std::locale::classic());
            text.precision(9);
            text << slotPreset[k] << ';';
            for (const auto& value : slots[k])
                text << value << (&value == &slots[k].back() ? "" : ",");
            state.setProperty(k == 0 ? "slotA" : "slotB", String(text.str()), nullptr);
            state.setProperty(k == 0 ? "slotNameA" : "slotNameB", slotName[k], nullptr);
        }
    if (editorWidth > 0)
        state.setProperty("editorWidth", editorWidth.load(), nullptr);
    if (const auto xml = state.createXml())
        copyXmlToBinary(*xml, dest);
}

void VibrateProcessor::applyParams(const XmlElement& state, bool includeBypass)
{
    const auto count = includeBypass ? vibrate::ids.size() : vibrate::ids.size() - 1;
    for (std::size_t i = 0; i < count; ++i)
    {
        auto* parameter = apvts.getParameter(vibrate::ids[i]);
        std::optional<double> value;
        for (const auto* entry : state.getChildWithTagNameIterator("PARAM"))
            if (entry->getStringAttribute("id") == vibrate::ids[i])
            {
                value = number(entry->getStringAttribute("value"));
                break;
            }
        const auto& range = parameter->getNormalisableRange();
        parameter->setValueNotifyingHost(value ? parameter->convertTo0to1(range.snapToLegalValue(static_cast<float>(*value)))
                                               : parameter->getDefaultValue());
    }
}

void VibrateProcessor::setIdentity(int index, const String& name)
{
    const ScopedLock lock(stateLock);
    userName = name;
    preset = index;
}

void VibrateProcessor::setStateInformation(const void* data, int size)
{
    const auto* bytes = static_cast<const char*>(data);
    if (bytes == nullptr || size < 8 || ByteOrder::littleEndianInt(bytes) != 0x21324356
        || ByteOrder::littleEndianInt(bytes + 4) > static_cast<uint32>(size - 8))
        return;
    const auto tree = decode({ bytes + 8, ByteOrder::littleEndianInt(bytes + 4) });
    if (tree == nullptr)
        return;

    const Transaction transaction(*this);
    applyParams(*tree, true);
    for (std::size_t k = 0; k < slots.size(); ++k)
    {
        const auto tokens = StringArray::fromTokens(tree->getStringAttribute(k == 0 ? "slotA" : "slotB"), ";,", "");
        const auto program = number(tokens[0]);
        slotStored[k] = program.has_value() && tokens.size() == vibrate::numControls + 1;
        slotPreset[k] = static_cast<int>(jlimit(-1.0, vibrate::numPresets - 1.0, program.value_or(-1.0)));
        slotName[k] = tree->getStringAttribute(k == 0 ? "slotNameA" : "slotNameB");
        for (int i = 0; i < vibrate::numControls && slotStored[k]; ++i)
        {
            const auto value = number(tokens[i + 1]);
            slotStored[k] = value && *value >= 0.0 && *value <= 1.0;
            slots[k][static_cast<std::size_t>(i)] = static_cast<float>(value.value_or(0.0));
        }
    }
    ab = number(tree->getStringAttribute("ab")).value_or(0) > 0.5 ? 1 : 0;
    editorWidth = static_cast<int>(jlimit(0.0, 4096.0, number(tree->getStringAttribute("editorWidth")).value_or(0)));
    const auto index = static_cast<int>(jlimit(-1.0, vibrate::numPresets - 1.0, number(tree->getStringAttribute("preset")).value_or(-1)));
    setIdentity(index, tree->getStringAttribute("presetName"));
}

void VibrateProcessor::setAll(const Slot& values)
{
    for (std::size_t i = 0; i < values.size(); ++i)
    {
        auto* p = apvts.getParameter(vibrate::ids[i]);
        if (std::abs(p->getValue() - values[i]) > 1e-7f)
        {
            p->beginChangeGesture();
            p->setValueNotifyingHost(values[i]);
            p->endChangeGesture();
        }
    }
}

void VibrateProcessor::setCurrentProgram(int index)
{
    if (!isPositiveAndBelow(index, vibrate::numPresets))
        return;
    {
        const Transaction transaction(*this);
        Slot normal {};
        for (std::size_t i = 0; i < normal.size(); ++i)
            normal[i] = static_cast<float>(vibrate::presets[index].values[i]);
        setAll(normal);
        setIdentity(index, {});
    }
    updateHostDisplay(ChangeDetails().withProgramChanged(true));
}

const String VibrateProcessor::getProgramName(int index)
{
    return isPositiveAndBelow(index, vibrate::numPresets) ? String(vibrate::presets[index].name) : String();
}

String VibrateProcessor::presetName() const
{
    const ScopedLock lock(stateLock);
    const int index = preset.load();
    return index >= 0 ? String(vibrate::presets[index].name) : userName.isEmpty() ? String("custom") : userName;
}

File VibrateProcessor::presetFolder() const
{
    return directory != File() ? directory
        : File::getSpecialLocation(File::userApplicationDataDirectory).getChildFile("daveydidit/vibrate/Presets");
}

StringArray VibrateProcessor::userPresets() const
{
    auto files = presetFolder().findChildFiles(File::findFiles, false, "*.xml");
    files.sort();
    StringArray names;
    for (const auto& file : files)
        names.add(file.getFileNameWithoutExtension());
    return names;
}

void VibrateProcessor::loadUserPreset(const String& name)
{
    if (name.isEmpty() || name.containsAnyOf("/\\"))
        return;
    if (const auto xml = decode(presetFolder().getChildFile(name + ".xml").loadFileAsString().toStdString()))
    {
        const Transaction transaction(*this);
        applyParams(*xml, false);
        setIdentity(-1, name);
    }
}

void VibrateProcessor::stepPreset(int delta)
{
    const auto files = userPresets();
    const int count = vibrate::numPresets + files.size();
    int index = preset.load();
    if (index < 0)
    {
        const int userIndex = files.indexOf(presetName());
        index = userIndex >= 0 ? vibrate::numPresets + userIndex : 0;
    }
    index = (index + delta % count + count) % count;
    if (index < vibrate::numPresets)
        setCurrentProgram(index);
    else
        loadUserPreset(files[index - vibrate::numPresets]);
}

File VibrateProcessor::saveUserPreset()
{
    const ScopedLock writer(txLock);
    const auto folder = presetFolder();
    if (folder.createDirectory().failed())
        return {};
    auto xml = apvts.copyState().createXml();
    if (xml == nullptr)
        return {};
    xml->setAttribute("stateVersion", stateVersion);
    for (int k = 1; k < 1000; ++k)
    {
        const auto file = folder.getChildFile("user " + String(k).paddedLeft('0', 2) + ".xml");
        if (file.exists())
            continue;
        if (!xml->writeTo(file))
            return {};
        setIdentity(-1, file.getFileNameWithoutExtension());
        return file;
    }
    return {};
}

void VibrateProcessor::selectAb(int slot)
{
    const Transaction transaction(*this);
    const int from = ab.load();
    if (slot == from || !isPositiveAndBelow(slot, 2))
        return;
    const auto oldSlot = static_cast<std::size_t>(from);
    const auto newSlot = static_cast<std::size_t>(slot);
    for (std::size_t i = 0; i < slots[oldSlot].size(); ++i)
        slots[oldSlot][i] = apvts.getParameter(vibrate::ids[i])->getValue();
    slotStored[oldSlot] = true;
    slotPreset[oldSlot] = preset.load();
    slotName[oldSlot] = userName;
    if (slotStored[newSlot])
    {
        setAll(slots[newSlot]);
        setIdentity(slotPreset[newSlot], slotName[newSlot]);
    }
    ab = slot;
}
