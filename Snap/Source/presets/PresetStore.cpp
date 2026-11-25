#include "PresetStore.h"
#include "../PluginProcessor.h"
#include "FactoryPresetData.h"
#include <limits>

using namespace juce;

namespace snap
{
namespace
{
using Collection = std::pair<String, StringArray>;

std::optional<Preset> factoryPreset(const char* resource)
{
    int size = 0;
    if (const auto* data = FactoryPresetData::getNamedResource(resource, size))
        return parsePreset(String::fromUTF8(data, size), true);
    return std::nullopt;
}
} // namespace

std::optional<Preset> parsePreset(const String& text, bool factory)
{
    const auto xml = XmlDocument::parse(text);
    if (xml == nullptr || xml->getTagName() != "SNAP")
        return std::nullopt;

    Preset preset;
    preset.name = xml->getStringAttribute("name");
    if (preset.name.isEmpty())
        return std::nullopt;
    preset.factory = factory;
    preset.xml = text;
    preset.description = xml->getStringAttribute("description");
    preset.tags.addTokens(xml->getStringAttribute("tags"), ",", "");
    preset.tags.trim();
    preset.tags.removeEmptyStrings();
    preset.order = parseChainOrder(xml->getStringAttribute("order").toStdString()).value_or(defaultChainOrder);

    const auto value = [&](std::string_view id, double fallback) {
        const String paramId(id.data(), id.size());
        for (const auto* param : xml->getChildIterator())
            if (param->getTagName() == "PARAM" && param->getStringAttribute("id") == paramId)
                return param->getDoubleAttribute("value", fallback);
        return fallback;
    };
    for (const auto& module : modules)
        preset.uses[static_cast<size_t>(module.module)] = value(module.onId, 1) > 0.5
            && std::ranges::any_of(module.amountIds, [&](auto id) { return !id.empty() && value(id, 0) != 0; });
    return preset;
}

PresetStore::PresetStore(File directory) : root(std::move(directory))
{
    rescan();
}

File PresetStore::defaultRoot()
{
    return File::getSpecialLocation(File::userApplicationDataDirectory).getChildFile("daveydidit").getChildFile("snap");
}

void PresetStore::rescan()
{
    readLibrary();
    all.clear();
    for (int i = 0; i < FactoryPresetData::namedResourceListSize; ++i)
        if (auto preset = factoryPreset(FactoryPresetData::namedResourceList[i]))
            all.push_back(std::move(*preset));
    for (const auto& file : root.getChildFile("Presets").findChildFiles(File::findFiles, false, String("*") + presetExtension))
        if (auto preset = parsePreset(file.loadFileAsString(), false))
        {
            preset->file = file;
            all.push_back(std::move(*preset));
        }
    std::ranges::sort(all, [](const Preset& a, const Preset& b) {
        const int order = a.name.compareIgnoreCase(b.name);
        return order == 0 ? a.factory > b.factory : order < 0;
    });
}

const Preset* PresetStore::find(const String& key) const
{
    const auto it = std::ranges::find(all, key, &Preset::key);
    return it == all.end() ? nullptr : &*it;
}

std::vector<const Preset*> PresetStore::list(const Filter& filter) const
{
    const auto collection = std::ranges::find(collectionList, filter.collection, &Collection::first);
    const auto matchesSource = [&](const Preset& preset) {
        switch (filter.source)
        {
            case Source::all: return true;
            case Source::favorites: return isFavorite(preset.key());
            case Source::user: return !preset.factory;
            case Source::collection: return collection != collectionList.end() && collection->second.contains(preset.key());
            case Source::factory: return preset.factory;
        }
        return false;
    };
    std::vector<const Preset*> result;
    for (const auto& preset : all)
    {
        if (!matchesSource(preset) || !std::ranges::all_of(filter.tags, [&](const String& tag) { return preset.tags.contains(tag, true); }))
            continue;
        const bool hidden = std::ranges::any_of(modules, [&](const auto& module) {
            const auto index = static_cast<size_t>(module.module);
            return preset.uses[index] && filter.hideUsing[index];
        });
        if (!hidden && (filter.search.isEmpty() || preset.name.containsIgnoreCase(filter.search)
                        || std::ranges::any_of(preset.tags, [&](const String& tag) { return tag.containsIgnoreCase(filter.search); })))
            result.push_back(&preset);
    }
    return result;
}

// Every library change re-reads library.xml first, so two open instances never write back each other's stale copy.
void PresetStore::readLibrary()
{
    favorites.clear(), collectionList.clear();
    const auto xml = XmlDocument::parse(root.getChildFile("library.xml"));
    if (xml == nullptr || xml->getTagName() != "LIBRARY")
        return;
    for (const auto* child : xml->getChildIterator())
    {
        if (child->getTagName() == "FAVORITE")
            favorites.addIfNotAlreadyThere(child->getStringAttribute("key"));
        else if (child->getTagName() == "COLLECTION")
        {
            auto& keys = collectionList.emplace_back(child->getStringAttribute("name"), StringArray {}).second;
            for (const auto* item : child->getChildIterator())
                if (item->getTagName() == "ITEM")
                    keys.addIfNotAlreadyThere(item->getStringAttribute("key"));
        }
    }
}

void PresetStore::writeLibrary() const
{
    if (root.createDirectory().failed())
        return;
    XmlElement xml("LIBRARY");
    for (const auto& key : favorites)
        xml.createNewChildElement("FAVORITE")->setAttribute("key", key);
    for (const auto& [name, keys] : collectionList)
    {
        auto* collection = xml.createNewChildElement("COLLECTION");
        collection->setAttribute("name", name);
        for (const auto& key : keys)
            collection->createNewChildElement("ITEM")->setAttribute("key", key);
    }
    xml.writeTo(root.getChildFile("library.xml"));
}

void PresetStore::setFavorite(const String& key, bool favorite)
{
    readLibrary();
    if (favorite == isFavorite(key))
        return;
    if (favorite)
        favorites.add(key);
    else
        favorites.removeString(key);
    writeLibrary();
}

StringArray PresetStore::collections() const
{
    StringArray names;
    for (const auto& collection : collectionList)
        names.add(collection.first);
    return names;
}

bool PresetStore::addCollection(const String& name)
{
    readLibrary();
    if (name.trim().isEmpty() || std::ranges::find(collectionList, name, &Collection::first) != collectionList.end())
        return false;
    collectionList.emplace_back(name, StringArray {});
    writeLibrary();
    return true;
}

void PresetStore::removeCollection(const String& name)
{
    readLibrary();
    if (const auto it = std::ranges::find(collectionList, name, &Collection::first); it != collectionList.end())
    {
        collectionList.erase(it);
        writeLibrary();
    }
}

void PresetStore::addToCollection(const String& collection, const String& key)
{
    readLibrary();
    const auto it = std::ranges::find(collectionList, collection, &Collection::first);
    if (it != collectionList.end() && it->second.addIfNotAlreadyThere(key))
        writeLibrary();
}

void PresetStore::removeFromCollection(const String& collection, const String& key)
{
    readLibrary();
    const auto it = std::ranges::find(collectionList, collection, &Collection::first);
    if (it != collectionList.end() && it->second.contains(key))
    {
        it->second.removeString(key);
        writeLibrary();
    }
}

StringArray PresetStore::collectionsOf(const String& key) const
{
    StringArray names;
    for (const auto& [name, keys] : collectionList)
        if (keys.contains(key))
            names.add(name);
    return names;
}

bool PresetStore::load(SnapProcessor& processor, const Preset& preset)
{
    return loadPresetXml(processor, preset.xml);
}

const Preset* PresetStore::save(SnapProcessor& processor, const String& name, const StringArray& tags)
{
    const auto trimmed = name.trim();
    if (trimmed.isEmpty())
        return nullptr;
    const auto directory = root.getChildFile("Presets");
    if (directory.createDirectory().failed())
        return nullptr;

    XmlElement xml("SNAP");
    xml.setAttribute("stateVersion", stateVersion);
    xml.setAttribute("order", String(toString(processor.order())));
    xml.setAttribute("name", trimmed);
    xml.setAttribute("tags", tags.joinIntoString(","));
    for (const auto& spec : parameterTable())
        if (!isSessionToggle(spec.id))
        {
            auto* param = xml.createNewChildElement("PARAM");
            param->setAttribute("id", spec.id);
            const auto value = processor.apvts.getRawParameterValue(spec.id)->load();
            param->setAttribute("value", String(static_cast<double>(value), std::numeric_limits<float>::max_digits10, true));
        }
    const auto file = directory.getChildFile(File::createLegalFileName(trimmed) + presetExtension);
    if (!xml.writeTo(file))
        return nullptr;
    rescan();
    return find("user/" + trimmed);
}

bool PresetStore::remove(const String& key)
{
    const auto* preset = find(key);
    if (preset == nullptr || preset->factory || !preset->file.deleteFile())
        return false;
    readLibrary();
    favorites.removeString(key);
    for (auto& collection : collectionList)
        collection.second.removeString(key);
    writeLibrary();
    rescan();
    return true;
}

// The file is normalised through JUCE's parser first (any valid XML spelling reads the same), and a preset without a
// version is a version 2 preset: the version 1 migration is for 0.1.x sessions only.
const std::vector<PresetGroup>& presetGroups()
{
    static const std::vector<PresetGroup> groups {
        { "drum bus", { "hit hard", "first coat of polish", "seams sit flush", "wood and skin", "rafters hold still", "soft rim around", "floor underfoot", "fist in velvet", "plaster shakes loose", "blown into confetti" } },
        { "loop", { "shine between cracks", "floor breathes with you", "breaks run hot", "corners pull inward", "buckles at edges", "grit between hits", "dust in the grooves", "left on the dashboard", "sparks in dust", "tails crowd in" } },
        { "kick", { "point on the thump", "heel clears gravel", "weight at heel", "thud through felt", "ground keeps humming", "sub learns to talk", "storm under concrete" } },
        { "snare", { "crack from the rim", "plank snaps shut", "paper at wrist", "wires under the skin", "wire bites back", "hall door slams shut" } },
        { "hats", { "foil catches light", "sparks scatter sideways", "glints on tin", "tin goes soft" } },
        { "percussion", { "knuckles on chalk", "pebbles in palm", "hollow hide bends", "hollows answer back" } },
        { "instrument", { "grain rests easy", "roots hold fast", "threads cross slowly", "breath between syllables" } },
        { "fx", { "bottom falls out", "ceiling folds upward", "shards lose gravity", "teeth in the low end", "walls swallow sparks" } },
    };
    return groups;
}

bool loadPresetXml(SnapProcessor& processor, const String& text)
{
    const auto parsed = XmlDocument::parse(text);
    if (parsed == nullptr || parsed->getTagName() != "SNAP")
        return false;
    if (!parsed->hasAttribute("stateVersion"))
        parsed->setAttribute("stateVersion", stateVersion);
    const auto xml = parsed->toString(XmlElement::TextFormat().withoutHeader().singleLine());
    const auto size = xml.getNumBytesAsUTF8(); // the envelope AudioProcessor::copyXmlToBinary writes
    MemoryOutputStream blob;
    blob.writeInt(0x21324356);
    blob.writeInt(static_cast<int>(size));
    blob.write(xml.toRawUTF8(), size);
    blob.writeByte(0);
    return processor.loadPreset(blob.getData(), static_cast<int>(blob.getDataSize()));
}

void loadDefaultPreset(SnapProcessor& processor)
{
    for (int i = 0; i < FactoryPresetData::namedResourceListSize; ++i)
        if (const auto preset = factoryPreset(FactoryPresetData::namedResourceList[i]); preset && preset->name == defaultPresetName)
        {
            const bool loaded = loadPresetXml(processor, preset->xml);
            jassert(loaded);
            ignoreUnused(loaded);
            processor.apvts.state.setProperty("preset", preset->key(), nullptr);
            processor.undoManager.clearUndoHistory();
            return;
        }
    jassertfalse;
}
} // namespace snap
