#pragma once
#include "../chain/ChainOrder.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <optional>
#include <vector>

class SnapProcessor;

// Presets. A preset file is the flat element the session uses, <SNAP stateVersion="2" order="..." name="..." tags="...">
// with one <PARAM id value/> per parameter in host units. A PARAM left out takes its default, so the factory files list
// only what they change; saved presets list every parameter except the session toggles. Locks and snapshots are never
// part of a preset. Factory presets are embedded and read-only; user presets are <root>/Presets/<name>.snappreset files,
// and favourites and collections live in <root>/library.xml. Offline-first: plain files, so sync can be added later.
// Message thread only.
namespace snap
{
inline constexpr const char* presetExtension = ".snappreset";
inline constexpr const char* defaultPresetName = "hit hard"; // the bold factory preset a new instance starts with

// Browser tag vocabularies (filter chips).
inline constexpr std::array<const char*, 17> characterTags { "Dark", "Warm", "FUN!", "Bright", "Lo-Fi", "Dirty", "Clean", "Punchy", "Crunchy",
                                                             "Smooth", "Tight", "Hi-Fi", "Classic", "Huge", "Fat", "Aggressive", "Roomy" };
inline constexpr std::array<const char*, 16> functionTags { "General Purpose", "Kick", "Cymbals", "Vocal", "Drum Bus", "Synth", "FX", "Hi-Hat",
                                                            "Loop", "Snare", "Toms", "Glue", "Artist", "Transitions", "Percussion", "Instrument" };

struct Preset
{
    juce::String name;
    juce::StringArray tags; // Character and Function tags
    bool factory = false;
    ChainOrder order = defaultChainOrder;
    std::array<bool, 6> uses {}; // per snap::Module: switched on with an amount other than 0 (sub/hiss: either amount)
    juce::String xml;            // the file's text
    juce::File file;             // user presets only

    juce::String key() const { return (factory ? "factory/" : "user/") + name; } // favourites and collections refer to this
    std::array<Module, 6> dots() const { return { Module::subHiss, order[0], order[1], order[2], order[3], Module::push }; } // chain order
};

enum class Source { all, favorites, user, collection, factory };

struct Filter
{
    Source source = Source::all;
    juce::String collection;          // Source::collection
    juce::StringArray tags;           // selected chips: a preset must carry every one
    std::array<bool, 6> hideUsing {}; // module filter: hide presets that use any of these modules
    juce::String search;              // case-insensitive, matches the name or a tag
};

class PresetStore
{
public:
    explicit PresetStore(juce::File root = defaultRoot());
    static juce::File defaultRoot(); // <user application data>/daveydidit/snap

    void rescan(); // factory + user presets, sorted by name (case-insensitive, factory first on a tie)
    const std::vector<Preset>& presets() const { return all; }
    const Preset* find(const juce::String& key) const;
    std::vector<const Preset*> list(const Filter&) const; // in presets() order

    bool isFavorite(const juce::String& key) const { return favorites.contains(key); }
    void setFavorite(const juce::String& key, bool);
    juce::StringArray collections() const; // names, in creation order
    bool addCollection(const juce::String& name); // false for an empty or existing name
    void removeCollection(const juce::String& name);
    void addToCollection(const juce::String& collection, const juce::String& key); // a preset may sit in several
    void removeFromCollection(const juce::String& collection, const juce::String& key);
    juce::StringArray collectionsOf(const juce::String& key) const;

    // Through SnapProcessor::loadPreset: one undo step; locks, locked groups, the session toggles and snapshots stay.
    static bool load(SnapProcessor&, const Preset&);
    // Writes (or overwrites) <root>/Presets/<name>.snappreset from the processor's current state, then rescans.
    const Preset* save(SnapProcessor&, const juce::String& name, const juce::StringArray& tags);
    bool remove(const juce::String& key); // user presets only; also drops it from favourites and collections

private:
    void readLibrary();
    void writeLibrary() const;

    juce::File root;
    std::vector<Preset> all;
    juce::StringArray favorites;
    std::vector<std::pair<juce::String, juce::StringArray>> collectionList; // name, preset keys
};

std::optional<Preset> parsePreset(const juce::String& xml, bool factory); // nullopt unless it is a <SNAP> with a name
bool loadPresetXml(SnapProcessor&, const juce::String& xml);               // wraps the text as a state blob for loadPreset
void loadDefaultPreset(SnapProcessor&);                                     // a new instance: the default preset, no undo step
} // namespace snap
