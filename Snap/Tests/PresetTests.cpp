#include "Check.h"
#include "PluginProcessor.h"
#include "loops/Loops.h"
#include "presets/PresetStore.h"
#include <limits>

// The processor headers also declare the plug-in entry point.
JUCE_BEGIN_IGNORE_WARNINGS_GCC_LIKE("-Wredundant-decls")
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter();
JUCE_END_IGNORE_WARNINGS_GCC_LIKE

namespace
{
using namespace juce;
using namespace snap;
using enum Module;

constexpr int sampleRate = 48000, blockSize = 512;
const auto defaultKey = String("factory/") + defaultPresetName;
const auto userName = String::fromUTF8("Cr\xc3\xa8me & <Tap>");
const auto userKey = "user/" + userName;
const StringArray userTags { "Punchy", "Kick" };

struct TempRoot
{
    File file = File::createTempFile("-snap-presets");
    ~TempRoot() { CHECK(file.deleteRecursively()); }
};

void set(SnapProcessor& processor, const char* id, float value)
{
    auto* param = processor.apvts.getParameter(id);
    param->setValueNotifyingHost(param->convertTo0to1(value));
}

float normalised(SnapProcessor& processor, const char* id)
{
    return processor.apvts.getParameter(id)->getValue();
}

AudioBuffer<float> render(SnapProcessor& processor, const AudioBuffer<float>& input)
{
    AudioProcessor::BusesLayout layout;
    layout.inputBuses.add(AudioChannelSet::stereo());
    layout.outputBuses.add(AudioChannelSet::stereo());
    CHECK(processor.setBusesLayout(layout));
    processor.setPlayConfigDetails(2, 2, sampleRate, blockSize);
    processor.prepareToPlay(sampleRate, blockSize);

    auto output = input;
    MidiBuffer midi;
    takeAudioAllocations();
    for (int start = 0; start < output.getNumSamples(); start += blockSize)
    {
        AudioBuffer<float> block(output.getArrayOfWritePointers(), 2, start, std::min(blockSize, output.getNumSamples() - start));
        setAudioThread(true);
        processor.processBlock(block, midi);
        setAudioThread(false);
    }
    CHECK(takeAudioAllocations() == 0);
    return output;
}

void checkPresetXml(const Preset& preset, const SnapProcessor* saved = nullptr)
{
    const auto xml = XmlDocument::parse(preset.xml);
    CHECK(xml != nullptr && !preset.xml.contains("locks") && !preset.xml.contains("SNAPSHOT"));
    if (xml == nullptr)
        return;
    for (const auto* child : xml->getChildWithTagNameIterator("PARAM"))
        CHECK(!isSessionToggle(child->getStringAttribute("id").toStdString()));
    if (saved != nullptr)
    {
        CHECK(xml->getIntAttribute("stateVersion") == stateVersion);
        for (const auto& spec : parameterTable())
            if (!isSessionToggle(spec.id))
            {
                int matches = 0;
                for (const auto* child : xml->getChildWithTagNameIterator("PARAM"))
                    if (child->getStringAttribute("id") == spec.id)
                    {
                        ++matches;
                        CHECK(static_cast<float>(child->getDoubleAttribute("value")) == saved->apvts.getRawParameterValue(spec.id)->load());
                    }
                CHECK(matches == 1);
            }
    }
}

void testParse()
{
    CHECK(!parsePreset("<OTHER name=\"test\"/>", false));
    CHECK(!parsePreset("<snap name=\"test\"/>", false));
    CHECK(!parsePreset("<SNAP name=\"\"/>", false));
    const String xml = R"(<SNAP name="Test" tags=" Punchy, , Kick, " order="shift,space,press,press">
        <PARAM id="hiss_amt" value="22"/><PARAM id="shift_amt" value="-5"/>
        <PARAM id="drive_on" value="0"/><PARAM id="drive_amt" value="20"/>
        <PARAM id="push_on" value="1"/>
    </SNAP>)";
    const auto preset = parsePreset(xml, true);
    const std::array uses { true, true, false, false, false, false };
    CHECK(preset && preset->factory && preset->xml == xml && preset->tags == userTags);
    CHECK(preset && preset->order == defaultChainOrder && preset->uses == uses);
    const auto noOrder = parsePreset("<SNAP name=\"Test\"/>", false);
    CHECK(noOrder && noOrder->order == defaultChainOrder);
}

void testFactorySet(const PresetStore& store)
{
    Filter filter;
    filter.source = Source::factory;
    const auto factory = store.list(filter);
    CHECK(factory.size() == 30);
    CHECK(store.find(defaultKey) != nullptr);
    StringArray names;
    int drums = 0, kicks = 0, snares = 0, instruments = 0;
    for (const auto* preset : factory)
    {
        const int before = failures;
        CHECK(names.addIfNotAlreadyThere(preset->name, true));
        bool character = false, function = false;
        for (const auto& tag : preset->tags)
        {
            const auto isTag = [&](const char* candidate) { return tag == candidate; };
            const bool isCharacter = std::ranges::any_of(characterTags, isTag), isFunction = std::ranges::any_of(functionTags, isTag);
            CHECK(isCharacter || isFunction);
            character |= isCharacter;
            function |= isFunction;
        }
        CHECK(character && function);
        drums += preset->tags.contains("Drum Bus") || preset->tags.contains("Loop");
        kicks += preset->tags.contains("Kick");
        snares += preset->tags.contains("Snare");
        instruments += preset->tags.contains("Vocal") || preset->tags.contains("Synth") || preset->tags.contains("Instrument");
        checkPresetXml(*preset);
        if (failures != before)
            std::cerr << "Preset " << preset->name << '\n';
    }
    // The coverage totals apply to the complete factory set.
    if (factory.size() >= 30)
    {
        CHECK(drums >= 8);
        CHECK(kicks >= 1 && snares >= 1);
        CHECK(instruments >= 3);
    }
}

void testFactoryAudio(const PresetStore& store)
{
    const auto loop = loops::make(1, sampleRate, 1);
    auto dry = loop;
    dry.applyGain(0.5f / dry.getMagnitude(0, dry.getNumSamples()));
    for (const auto& preset : store.presets())
        if (preset.factory)
        {
            const int before = failures;
            SnapProcessor wetProcessor, dryProcessor;
            CHECK(loadPresetXml(wetProcessor, preset.xml));
            CHECK(wetProcessor.order() == preset.order);
            const auto wet = render(wetProcessor, loop);
            bool finite = true;
            for (int channel = 0; channel < 2; ++channel)
                finite &= std::ranges::all_of(std::span(wet.getReadPointer(channel), static_cast<size_t>(wet.getNumSamples())),
                                              [](float value) { return std::isfinite(value); });
            CHECK(finite && wet.getMagnitude(0, wet.getNumSamples()) < 4.0f);

            CHECK(loadPresetXml(dryProcessor, preset.xml));
            set(dryProcessor, "magnitude", 0);
            set(dryProcessor, "clip_on", 0);
            const auto neutral = render(dryProcessor, dry);
            const int latency = dryProcessor.getLatencySamples(), settle = sampleRate / 4;
            float residual = std::numeric_limits<float>::infinity();
            if (latency >= 0 && latency < neutral.getNumSamples() - settle)
            {
                residual = 0;
                for (int channel = 0; channel < 2; ++channel)
                    for (int i = settle; i < neutral.getNumSamples() - latency; ++i)
                    {
                        const float error = std::abs(neutral.getSample(channel, i + latency) - dry.getSample(channel, i));
                        residual = std::isfinite(error) ? std::max(residual, error) : std::numeric_limits<float>::infinity();
                    }
            }
            CHECK(residual <= 1.0e-3f);
            if (failures != before)
                std::cerr << "Preset " << preset.name << ", peak " << wet.getMagnitude(0, wet.getNumSamples()) << ", residual " << residual << '\n';
        }
}

void testSave(const File& root)
{
    PresetStore store(root);
    SnapProcessor a, b;
    Random random(42);
    for (const auto& spec : parameterTable())
        if (!isSessionToggle(spec.id))
        {
            const bool discrete = spec.unit == Unit::choice || spec.unit == Unit::toggle || spec.unit == Unit::note;
            const float offset = discrete ? static_cast<float>(random.nextInt(static_cast<int>(spec.hi - spec.lo) + 1))
                                          : random.nextFloat() * (spec.hi - spec.lo);
            set(a, spec.id, spec.lo + offset);
        }
        else
            set(a, spec.id, 1);
    auto order = defaultChainOrder;
    for (size_t i = order.size() - 1; i > 0; --i)
        std::swap(order[i], order[static_cast<size_t>(random.nextInt(static_cast<int>(i) + 1))]);
    a.setOrder(order);
    a.setLocked(3, true);
    a.storeSnapshot(0);

    CHECK(store.save(a, " \t ", userTags) == nullptr);
    const auto* preset = store.save(a, " " + userName + " ", userTags);
    CHECK(preset != nullptr);
    if (preset == nullptr)
        return;
    const auto file = root.getChildFile("Presets").getChildFile(File::createLegalFileName(userName) + presetExtension);
    CHECK(file.existsAsFile() && preset->file == file && preset->name == userName && preset->key() == userKey);
    CHECK(!preset->factory && preset->tags == userTags && preset->order == order);
    const std::array dots { subHiss, order[0], order[1], order[2], order[3], push };
    CHECK(preset->dots() == dots);
    checkPresetXml(*preset, &a);
    for (const auto& module : modules)
    {
        const bool on = normalised(a, module.onId.data()) == 1;
        const bool hasAmount = std::ranges::any_of(module.amountIds, [&](auto id) {
            return !id.empty() && a.apvts.getRawParameterValue(id.data())->load() != 0;
        });
        CHECK(preset->uses[static_cast<size_t>(module.module)] == (on && hasAmount));
    }
    CHECK(PresetStore::load(b, *preset));
    CHECK(b.order() == order);
    for (const auto& spec : parameterTable())
    {
        const float actual = normalised(b, spec.id);
        if (isSessionToggle(spec.id))
            CHECK(actual == b.apvts.getParameter(spec.id)->getDefaultValue());
        else
        {
            const float expected = normalised(a, spec.id);
            if (spec.unit == Unit::choice || spec.unit == Unit::toggle || spec.unit == Unit::note)
                CHECK(actual == expected);
            else
                CHECK(std::abs(actual - expected) <= 1.0e-6f);
        }
    }

    set(a, "shift_amt", normalised(a, "shift_amt") > 0.5f ? -100.0f : 100.0f);
    const auto* replacement = store.save(a, userName, userTags);
    CHECK(replacement != nullptr && replacement->file == file);
    Filter filter;
    filter.source = Source::user;
    CHECK(store.list(filter).size() == 1);
    const PresetStore reopened(root);
    const auto* onDisk = reopened.find(userKey);
    CHECK(reopened.list(filter).size() == 1 && onDisk != nullptr && onDisk->file == file);
    if (onDisk != nullptr)
        checkPresetXml(*onDisk, &a);
}

void testLocks(const Preset& preset)
{
    SnapProcessor processor, reference;
    CHECK(loadPresetXml(reference, preset.xml));
    for (const auto& spec : parameterTable())
        set(processor, spec.id, normalised(reference, spec.id) > 0.5f ? spec.lo : spec.hi);
    set(processor, "press_amt", 23);
    set(processor, "press_mode", 3);
    set(processor, "attack", -37);
    set(processor, "tone", 45);
    const ChainOrder original { press, drive, space, shift };
    processor.setOrder(original);
    processor.setLocked(3, true);
    processor.setLocked(masterLock, true);
    processor.storeSnapshot(0);
    std::vector<float> before;
    for (const auto& spec : parameterTable())
        before.push_back(normalised(processor, spec.id));

    CHECK(PresetStore::load(processor, preset));
    ChainOrder expected { press };
    std::copy_if(preset.order.begin(), preset.order.end(), expected.begin() + 1, [](Module module) { return module != press; });
    CHECK(processor.order() == expected && processor.hasSnapshot(0));
    for (int group = 0; group < lockCount; ++group)
        CHECK(processor.locked(group) == (group == 3 || group == masterLock));
    size_t i = 0;
    for (const auto& spec : parameterTable())
    {
        const int group = lockGroup(spec.id);
        const bool kept = group == 3 || group == masterLock || isSessionToggle(spec.id);
        CHECK(normalised(processor, spec.id) == (kept ? before[i] : normalised(reference, spec.id)));
        ++i;
    }
    CHECK(processor.undo() && processor.order() == original);
    i = 0;
    for (const auto& spec : parameterTable())
        CHECK(normalised(processor, spec.id) == before[i++]);
}

void testBrowser(const File& root)
{
    const auto library = root.getChildFile("library.xml");
    CHECK(library.replaceWithText("garbage"));
    PresetStore store(root);
    CHECK(store.collections().isEmpty() && !store.isFavorite(userKey));
    SnapProcessor processor;
    const auto* tie = store.save(processor, String(defaultPresetName).toUpperCase(), { "Punchy", "Snare" });
    CHECK(tie != nullptr && store.find(userKey) != nullptr);
    if (tie == nullptr || store.find(userKey) == nullptr)
        return;
    const auto tieKey = tie->key();
    const String first = "Session & Sketches", second = "To Try", gone = "user/gone";
    store.setFavorite(userKey, true);
    store.setFavorite(defaultKey, true);
    store.setFavorite(gone, true);
    CHECK(store.isFavorite(userKey) && store.isFavorite(defaultKey) && store.isFavorite(gone));
    Filter filter;
    filter.source = Source::favorites;
    CHECK(store.list(filter).size() == 2);
    store.setFavorite(defaultKey, false);
    CHECK(!store.isFavorite(defaultKey) && store.list(filter) == std::vector { store.find(userKey) });

    const StringArray collections { first, second };
    CHECK(store.addCollection(first) && store.addCollection(second));
    CHECK(!store.addCollection(first) && !store.addCollection("") && !store.addCollection("  "));
    store.addToCollection(first, userKey);
    store.addToCollection(first, userKey);
    store.addToCollection(first, gone);
    store.addToCollection(second, userKey);
    CHECK(store.collections() == collections && store.collectionsOf(userKey) == collections);
    filter.source = Source::collection;
    filter.collection = first;
    CHECK(store.list(filter) == std::vector { store.find(userKey) });
    PresetStore reopened(root);
    CHECK(reopened.isFavorite(userKey) && reopened.isFavorite(gone) && !reopened.isFavorite(defaultKey));
    CHECK(reopened.collections() == collections && reopened.collectionsOf(userKey) == collections);
    CHECK(reopened.list(filter) == std::vector { reopened.find(userKey) });
    reopened.removeFromCollection(first, userKey);
    CHECK(reopened.list(filter).empty() && reopened.collectionsOf(userKey) == StringArray { second });
    reopened.removeCollection(second);
    PresetStore updated(root);
    CHECK(updated.collections() == StringArray { first } && updated.collectionsOf(userKey).isEmpty());
    CHECK(updated.collectionsOf(gone) == StringArray { first });

    filter = {};
    const auto all = updated.list(filter);
    CHECK(all.size() == updated.presets().size());
    for (size_t i = 0; i < all.size(); ++i)
    {
        CHECK(all[i] == &updated.presets()[i]);
        if (i > 0)
        {
            const int order = all[i - 1]->name.compareIgnoreCase(all[i]->name);
            CHECK(order <= 0 && (order != 0 || all[i - 1]->factory || !all[i]->factory));
        }
    }
    filter.source = Source::factory;
    const auto factoryCount = updated.list(filter).size();
    filter.source = Source::user;
    CHECK(updated.list(filter).size() == 2 && all.size() == factoryCount + 2);
    filter.tags = { "pUNchy", "kICK" };
    CHECK(updated.list(filter) == std::vector { updated.find(userKey) });
    filter.tags.add("Snare");
    CHECK(updated.list(filter).empty());
    filter.tags.clear();
    filter.search = "tAP";
    CHECK(updated.list(filter) == std::vector { updated.find(userKey) });
    filter.search = "sNaR";
    CHECK(updated.list(filter) == std::vector { updated.find(tieKey) });
    filter = {};
    const auto index = static_cast<size_t>(push);
    filter.hideUsing[index] = true;
    auto expected = all;
    std::erase_if(expected, [=](const Preset* preset) { return preset->uses[index]; });
    CHECK(expected.size() < all.size() && !expected.empty());
    CHECK(updated.list(filter) == expected);

    updated.addToCollection(first, userKey);
    CHECK(updated.addCollection(second));
    updated.addToCollection(second, userKey);
    const auto* saved = updated.find(userKey);
    CHECK(saved != nullptr);
    if (saved == nullptr)
        return;
    const auto file = saved->file;
    CHECK(!updated.remove(defaultKey) && !updated.remove(gone));
    CHECK(updated.remove(userKey));
    CHECK(!file.exists() && updated.find(userKey) == nullptr);
    CHECK(!updated.isFavorite(userKey) && updated.collectionsOf(userKey).isEmpty());
    const PresetStore afterRemove(root);
    CHECK(afterRemove.find(userKey) == nullptr && !afterRemove.isFavorite(userKey) && afterRemove.collectionsOf(userKey).isEmpty());
}

void testFirstLoad(const Preset& preset)
{
    std::unique_ptr<AudioProcessor> fresh(createPluginFilter());
    auto* processor = dynamic_cast<SnapProcessor*>(fresh.get());
    CHECK(processor != nullptr);
    if (processor == nullptr)
        return;
    SnapProcessor reference, neutral;
    CHECK(loadPresetXml(reference, preset.xml));
    CHECK(processor->order() == reference.order() && !processor->undoManager.canUndo());
    CHECK(neutral.order() == defaultChainOrder);
    for (const auto& spec : parameterTable())
    {
        CHECK(normalised(*processor, spec.id) == normalised(reference, spec.id));
        CHECK(normalised(neutral, spec.id) == neutral.apvts.getParameter(spec.id)->getDefaultValue());
    }
}
} // namespace

void testPresets()
{
    const TempRoot root;
    const PresetStore factory(root.file);
    testParse();
    testFactorySet(factory);
    testFactoryAudio(factory);
    testSave(root.file);
    if (const auto* preset = factory.find(defaultKey))
    {
        testLocks(*preset);
        testFirstLoad(*preset);
    }
    testBrowser(root.file);
}
