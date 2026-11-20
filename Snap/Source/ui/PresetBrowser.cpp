#include "PresetBrowser.h"
#include "../PluginProcessor.h"

using namespace juce;

namespace snap::ui
{
namespace
{
using namespace colours;

const Rectangle<int> searchBox { 590, 52, 470, 24 }, listPane { 590, 52, 470, 370 },
    sourcePane { 12, 71, 178, 351 }, cancelBox { 1290, 10, 44, 20 }, okBox { 1346, 10, 30, 20 },
    clearBox { 525, 52, 33, 14 };
constexpr int visibleRows = 12, visibleSources = 14, infoX = 1092;

Rectangle<int> rowBox(int i) { return { 590, 138 + i * 22, 470, 20 }; }
Rectangle<int> sourceBox(int i) { return { 12, 71 + i * 24, 178, 20 }; }
Rectangle<int> moduleBox(int i) { return { 656 + i * 18, 86, 18, 16 }; }
Rectangle<int> heartBox(Rectangle<int> row) { return { row.getX() + 444, row.getY() + 4, 12, 12 }; }
int lastOffset(size_t size, int rows) { return jmax(0, (int) size - rows); }
int scrolledOffset(int offset, int delta, int end) { return jlimit(0, end, jmin(offset, end) + delta); }

String moduleName(size_t i) { return String(modules[i].name.data(), modules[i].name.size()); }
String tagLabel(const String& name) { return name.equalsIgnoreCase("General Purpose") ? String("general") : name.toLowerCase(); }

void fill(Graphics& g, Rectangle<float> bounds, Colour colour) { g.setColour(colour), g.fillRect(bounds); }

struct ChipFlow
{
    float left, y, width, x = left;
    static constexpr float height = 18;

    Rectangle<float> next(const String& label)
    {
        const float w = jmin(width, textWidth(label, 11) + 12);
        if (x + w > left + width && x > left)
        {
            x = left;
            y += height + 6;
        }
        const Rectangle<float> bounds { x, y, w, height };
        x += w + 4;
        return bounds;
    }
};

void drawChip(Graphics& g, Rectangle<float> box, const String& label, bool active, bool hovered = false)
{
    if (active)
        fill(g, box, tag);
    text(g, label, box.getX() + 6, box.getY() + 2, active || hovered ? primary : muted, 11, active, box.getWidth() - 12);
}

void paragraph(Graphics& g, const String& content, Rectangle<int> box, Colour colour)
{
    Graphics::ScopedSaveState clip(g);
    if (content.isEmpty() || box.isEmpty() || !g.reduceClipRegion(box))
        return;
    const auto font = mono(11);
    g.setFont(font);
    g.setColour(colour);
    g.drawMultiLineText(content, box.getX(), box.getY() + roundToInt(font.getAscent()), box.getWidth());
}

void styleEditor(TextEditor& editor, int inset, const String& title)
{
    editor.setTitle(title);
    editor.setFont(mono(12));
    editor.setBorder(BorderSize<int>(1));
    editor.setIndents(inset, 0);
    editor.setColour(TextEditor::backgroundColourId, Colours::transparentBlack);
    editor.setColour(TextEditor::outlineColourId, Colours::transparentBlack);
    editor.setColour(TextEditor::focusedOutlineColourId, Colours::transparentBlack);
    editor.setColour(TextEditor::textColourId, primary);
    editor.setColour(TextEditor::highlightColourId, tag);
    editor.setColour(TextEditor::highlightedTextColourId, primary);
    editor.setColour(CaretComponent::caretColourId, accent);
    editor.setEscapeAndReturnKeysConsumed(false);
}

struct SearchField : TextEditor
{
    bool keyPressed(const KeyPress& key) override
    {
        return !key.isKeyCode(KeyPress::upKey) && !key.isKeyCode(KeyPress::downKey) && TextEditor::keyPressed(key);
    }
};
} // namespace

struct PresetBrowser::Impl
{
    struct Chip { String tagName, label; Rectangle<float> bounds; };
    struct SourceRow { Source source; String name; bool add = false; };

    PresetBrowser& browser;
    SnapProcessor& proc;
    PresetStore& store;
    std::function<void(const Preset&)> audition;
    SearchField search;
    TextEditor collection;
    Filter filter;
    int undoDepth = 0; // undo steps that existed when the browser opened
    std::vector<Chip> chips;
    String selectedKey, openKey; // openKey: the current preset when the browser opened
    Point<float> hover { -1, -1 };
    float functionY = 0;
    int scroll = 0, sourceScroll = 0;
    unsigned session = 0;
    bool opened = false;

    Impl(PresetBrowser& owner, SnapProcessor& processor, PresetStore& library, std::function<void(const Preset&)> load)
        : browser(owner), proc(processor), store(library), audition(std::move(load))
    {
        ChipFlow flow { 218, 100, 340 };
        const auto addChips = [&](const auto& names)
        {
            for (const auto* name : names)
            {
                const auto label = tagLabel(name);
                chips.push_back({ name, label, flow.next(label) });
            }
        };
        addChips(characterTags);
        functionY = flow.y + ChipFlow::height + 10;
        flow = { 218, functionY + 24, 340 };
        addChips(functionTags);

        styleEditor(search, 9, "search presets");
        styleEditor(collection, 5, "collection name");
        browser.addAndMakeVisible(search);
        browser.addChildComponent(collection);
        search.onTextChange = [this] { filter.search = search.getText(); resetList(); };
        collection.onFocusLost = [this]
        {
            if (!collection.hasKeyboardFocus(false))
                collection.setVisible(false);
        };
    }

    std::vector<SourceRow> sources() const
    {
        std::vector<SourceRow> rows { { Source::all, "all" }, { Source::favorites, "favorites" }, { Source::user, "user" } };
        for (const auto& name : store.collections())
            rows.push_back({ Source::collection, name });
        rows.push_back({ Source::all, "+ collection", true });
        rows.push_back({ Source::factory, "factory" });
        return rows;
    }

    void resetList() { scroll = 0; browser.repaint(); }

    void beginCollection()
    {
        collection.setText({}, false);
        collection.setBounds(sourceBox(store.collections().size() + 3 - sourceScroll));
        collection.setVisible(true);
        collection.grabKeyboardFocus();
        browser.repaint();
    }

    void finish(bool cancel)
    {
        if (!opened)
            return;
        opened = false;
        collection.setVisible(false);
        const auto close = browser.onClose;
        if (cancel)
        {
            while (proc.undoManager.getUndoDescriptions().size() > undoDepth && proc.undo())
                ;
            proc.apvts.state.setProperty("preset", openKey, nullptr);
        }
        if (close)
            close();
    }

    // Keyboard browsing: select and audition the previous / next row, keeping it in view.
    void step(int delta)
    {
        const auto rows = store.list(filter);
        if (rows.empty())
            return;
        const auto at = std::ranges::find(rows, selectedKey, &Preset::key);
        const int i = at == rows.end() ? (delta > 0 ? 0 : (int) rows.size() - 1) : jlimit(0, (int) rows.size() - 1, (int) (at - rows.begin()) + delta);
        selectedKey = rows[(size_t) i]->key();
        scroll = jlimit(i - visibleRows + 1, i, scroll);
        audition(*rows[(size_t) i]);
        browser.repaint();
    }

    void toggleFavorite(const String& key) { store.setFavorite(key, !store.isFavorite(key)); }

    void moveList(int delta)
    {
        scroll = scrolledOffset(scroll, delta, lastOffset(store.list(filter).size(), visibleRows));
        browser.repaint();
    }

    void addAction(PopupMenu& menu, const String& label, std::function<void(Impl&)> action)
    {
        menu.addItem(label, [safe = Component::SafePointer<PresetBrowser>(&browser), id = session, action = std::move(action)]
        {
            if (auto* owner = safe.getComponent(); owner != nullptr && owner->impl->opened && owner->impl->session == id)
            {
                action(*owner->impl);
                owner->repaint();
            }
        });
    }

    void sourceMenu(const String& name)
    {
        PopupMenu menu;
        addAction(menu, "remove collection", [name](Impl& p)
        {
            p.store.removeCollection(name);
            if (p.filter.source == Source::collection && p.filter.collection == name)
            {
                p.filter.source = Source::all;
                p.filter.collection.clear();
                p.resetList();
            }
        });
        menu.showMenuAsync(PopupMenu::Options().withTargetComponent(&browser).withMousePosition());
    }

    void presetMenu(const Preset& preset)
    {
        PopupMenu menu;
        const auto key = preset.key();
        addAction(menu, store.isFavorite(key) ? "unfavorite" : "favorite", [key](Impl& p) { p.toggleFavorite(key); });
        const auto memberships = store.collectionsOf(key);
        for (const auto& name : store.collections())
        {
            addAction(menu, "add to " + name, [name, key](Impl& p) { p.store.addToCollection(name, key); });
            if (memberships.contains(name))
                addAction(menu, "remove from " + name, [name, key](Impl& p) { p.store.removeFromCollection(name, key); });
        }
        if (!preset.factory)
            addAction(menu, "delete", [key](Impl& p) { p.store.remove(key); });
        menu.showMenuAsync(PopupMenu::Options().withTargetComponent(&browser).withMousePosition());
    }

    void paintSources(Graphics& g) const
    {
        text(g, "sources", 16, 52, muted, 11);
        const auto rows = sources();
        const int first = jmin(sourceScroll, lastOffset(rows.size(), visibleSources));
        for (int i = first; i < jmin(first + visibleSources, (int) rows.size()); ++i)
        {
            const auto& row = rows[(size_t) i];
            const auto box = sourceBox(i - first).toFloat();
            const float y = box.getY() + 3;
            const bool isCollection = row.source == Source::collection;
            const bool active = !row.add && row.source == filter.source && (!isCollection || row.name == filter.collection);
            if (active)
                fill(g, box, tag);
            if (row.add && collection.isVisible())
            {
                g.setColour(rule);
                g.drawRect(box, 1.0f);
                continue;
            }
            text(g, row.name, isCollection ? 32 : 18, y, row.add ? accent : (active || box.contains(hover) ? primary : muted),
                 12, active, isCollection ? 114 : 132);
            if (!row.add)
            {
                const Filter count { row.source, isCollection ? row.name : String(), {}, {}, {} };
                right(g, String(store.list(count).size()), 150, y, 34, muted, 11);
            }
        }
    }

    void paintInfo(Graphics& g, const Preset* preset) const
    {
        text(g, "info", infoX, 52, muted, 11);
        if (preset != nullptr)
        {
            const auto& p = *preset;
            text(g, p.name, infoX, 72, primary, 15, true, 284);
            const auto function = std::ranges::find_first_of(p.tags, functionTags,
                [](const String& name, const char* candidate) { return name.equalsIgnoreCase(candidate); });
            String meta = p.factory ? "factory" : "user";
            if (function != p.tags.end())
                meta += u(" · ") + tagLabel(*function);
            text(g, meta, infoX, 94, muted, 11);

            ChipFlow flow { infoX - 6, 114, 290 };
            int y = 146;
            for (auto it = p.tags.begin(); it != p.tags.end(); ++it)
            {
                if (it == function)
                    continue;
                const auto label = tagLabel(*it);
                const auto box = flow.next(label);
                if (box.getBottom() > (p.description.isEmpty() ? 200 : 160))
                    break;
                drawChip(g, box, label, true);
                y = roundToInt(box.getBottom()) + 14;
            }
            paragraph(g, p.description, { infoX, y, 284, 206 - y }, secondary);
            text(g, "chain", infoX, 206, muted, 11);
            const auto chain = p.dots();
            for (size_t i = 0; i < chain.size(); ++i)
            {
                const auto m = static_cast<size_t>(chain[i]);
                const float top = 226 + 18.0f * static_cast<float>(i);
                text(g, String(i + 1), infoX, top, dim, 11);
                moduleGlyph(g, chain[i], infoX + 16, top - 2, p.uses[m] ? accent : dim);
                text(g, moduleName(m) + (p.uses[m] ? String() : u(" · off")), infoX + 34, top, p.uses[m] ? primary : dim, 11);
            }
        }

        StringArray locks;
        for (size_t i = 0; i < modules.size(); ++i)
            if (proc.locked(static_cast<int>(i)))
                locks.add(moduleName(i));
        if (proc.locked(masterLock))
            locks.add("master");
        text(g, "locks", infoX, 346, muted, 11);
        if (locks.isEmpty())
            text(g, "none", infoX + 44, 346, dim, 11);
        else
        {
            padlock(g, infoX + 44, 347, accent);
            paragraph(g, locks.joinIntoString(", ") + " kept on load", { infoX + 60, 346, 224, 54 }, primary);
        }
        text(g, u("click a row to audition · ok commits"), infoX, 404, dim, 11);
    }
};

PresetBrowser::PresetBrowser(SnapProcessor& processor, PresetStore& store, std::function<void(const Preset&)> audition)
    : impl(std::make_unique<Impl>(*this, processor, store, std::move(audition)))
{
    setTitle("preset browser");
    setWantsKeyboardFocus(true);
}

PresetBrowser::~PresetBrowser() = default;

void PresetBrowser::open(const String& currentKey)
{
    auto& p = *impl;
    p.proc.beginUndoStep(); // edits before the browser opened stay out of what cancel undoes
    p.undoDepth = p.proc.undoManager.getUndoDescriptions().size();
    p.store.rescan();
    p.filter = {};
    p.search.setText({}, false);
    p.collection.setVisible(false);
    p.opened = true;
    ++p.session;
    p.hover = { -1, -1 };
    p.selectedKey = p.openKey = currentKey;
    p.sourceScroll = p.scroll = 0;
    const auto& presets = p.store.presets();
    const auto it = std::ranges::find(presets, currentKey, &Preset::key);
    if (it != presets.end())
        p.scroll = jmax(0, static_cast<int>(it - presets.begin()) - visibleRows + 1);
    setVisible(true);
    if (isShowing())
        grabKeyboardFocus();
    repaint();
}

void PresetBrowser::finish(bool cancel) { impl->finish(cancel); }

void PresetBrowser::resized() { impl->search.setBounds(searchBox); }

void PresetBrowser::paint(Graphics& g)
{
    const auto& p = *impl;
    const auto rows = p.store.list(p.filter);
    const auto selected = std::ranges::find(rows, p.selectedKey, &Preset::key);
    g.fillAll(bg);
    g.setColour(rule);
    g.drawRect(getLocalBounds());
    text(g, "presets", 16, 12, primary, 12, true);
    text(g, String(rows.size()) + u(" presets · ") + (selected != rows.end() ? "1 selected" : "0 selected"), 92, 13, muted, 11);
    text(g, "cancel", 1290, 12, cancelBox.toFloat().contains(p.hover) ? primary : muted);
    fill(g, okBox.toFloat(), primary);
    centred(g, "ok", 1346, 12, 30, bg, 12, true);
    hline(g, 16, 38, 1360);
    for (const float x : { 202, 574, 1076 })
        vline(g, x, 52, 370);

    p.paintSources(g);
    text(g, "filters", 218, 52, muted, 11);
    text(g, "clear", 525, 52, accent, 11);
    text(g, "character", 218, 76, secondary, 11);
    text(g, "function", 218, p.functionY, secondary, 11);
    for (const auto& chip : p.chips)
        drawChip(g, chip.bounds, chip.label, p.filter.tags.contains(chip.tagName), chip.bounds.contains(p.hover));

    g.setColour(rule);
    g.drawRect(searchBox);
    if (p.search.isEmpty())
        text(g, "search presets", 608, 56, dim);
    text(g, "modules", 590, 88, muted, 11);
    StringArray hidden;
    for (size_t i = 0; i < modules.size(); ++i)
    {
        const auto box = moduleBox(static_cast<int>(i));
        moduleGlyph(g, modules[i].module, static_cast<float>(box.getX()), static_cast<float>(box.getY()), p.filter.hideUsing[i] ? dim : primary);
        if (p.filter.hideUsing[i])
            hidden.add(moduleName(i));
    }
    if (!hidden.isEmpty())
        text(g, hidden.joinIntoString(" ") + " hidden", 770, 88, dim, 11, false, 298);
    text(g, "name", 604, 112, muted, 11);
    text(g, "chain", 820, 112, muted, 11);
    text(g, "fav", 1030, 112, muted, 11);
    hline(g, 590, 130, 470);
    const int count = (int) rows.size(), first = jmin(p.scroll, lastOffset(rows.size(), visibleRows));
    for (int i = first; i < jmin(first + visibleRows, count); ++i)
    {
        const auto& preset = *rows[static_cast<size_t>(i)];
        const auto bounds = rowBox(i - first);
        const auto box = bounds.toFloat();
        const bool active = selected != rows.end() && &preset == *selected;
        if (active)
        {
            fill(g, box, tag);
            fill(g, box.withWidth(2), accent);
        }
        text(g, preset.name, box.getX() + 14, box.getY() + 2, active || box.contains(p.hover) ? primary : secondary, 12, active, 212);
        const auto chain = preset.dots();
        for (size_t j = 0; j < chain.size(); ++j)
            moduleGlyph(g, chain[j], box.getX() + 230 + 18.0f * static_cast<float>(j), box.getY() + 1,
                        preset.uses[static_cast<size_t>(chain[j])] ? (active ? accent : primary) : dim);
        const auto icon = heartBox(bounds).toFloat();
        heart(g, icon.getX(), icon.getY(), p.store.isFavorite(preset.key()));
    }
    if (const int more = count - first - visibleRows; more > 0)
        text(g, String(more) + u(" more · scroll"), 590, 406, dim, 11);
    p.paintInfo(g, selected == rows.end() ? nullptr : *selected);
}

void PresetBrowser::mouseDown(const MouseEvent& e)
{
    impl->collection.setVisible(false);
    mouseMove(e);
}

void PresetBrowser::mouseUp(const MouseEvent& e)
{
    auto& p = *impl;
    if (!p.opened || !e.mouseWasClicked())
        return;
    const auto hit = [&](auto box)
    {
        const auto rect = box.toFloat();
        return rect.contains(e.position) && rect.contains(e.mouseDownPosition);
    };
    const auto rowAt = [&](auto bounds, int count)
    {
        for (int i = 0; i < count; ++i)
            if (hit(bounds(i)))
                return i;
        return -1;
    };
    const bool popup = e.mods.isPopupMenu();
    if (!popup && !e.mods.isLeftButtonDown())
        return;
    if (!popup && (hit(okBox) || hit(cancelBox)))
        return p.finish(hit(cancelBox));

    if (sourcePane.contains(e.getPosition()))
    {
        const auto rows = p.sources();
        p.sourceScroll = jmin(p.sourceScroll, lastOffset(rows.size(), visibleSources));
        const int i = rowAt(sourceBox, jmin(visibleSources, (int) rows.size() - p.sourceScroll));
        if (i < 0)
            return;
        const auto& row = rows[(size_t) (p.sourceScroll + i)];
        if (popup)
        {
            if (row.source == Source::collection)
                p.sourceMenu(row.name);
        }
        else if (row.add)
            p.beginCollection();
        else
        {
            p.filter.source = row.source;
            p.filter.collection = row.source == Source::collection ? row.name : String();
            p.resetList();
        }
        return;
    }

    if (!popup)
    {
        if (hit(clearBox))
        {
            p.filter.tags.clear();
            return p.resetList();
        }
        for (const auto& chip : p.chips)
            if (hit(chip.bounds))
            {
                if (!p.filter.tags.addIfNotAlreadyThere(chip.tagName))
                    p.filter.tags.removeString(chip.tagName);
                return p.resetList();
            }
        if (const int i = rowAt(moduleBox, (int) modules.size()); i >= 0)
        {
            auto& hidden = p.filter.hideUsing[(size_t) i];
            hidden = !hidden;
            return p.resetList();
        }
    }

    if (!listPane.contains(e.getPosition()))
        return;
    const auto rows = p.store.list(p.filter);
    p.scroll = jmin(p.scroll, lastOffset(rows.size(), visibleRows));
    const int i = rowAt(rowBox, jmin(visibleRows, (int) rows.size() - p.scroll));
    if (i < 0)
        return;
    const auto& preset = *rows[(size_t) (p.scroll + i)];
    if (popup)
        p.presetMenu(preset);
    else if (hit(heartBox(rowBox(i))))
    {
        p.toggleFavorite(preset.key());
        repaint();
    }
    else
    {
        p.selectedKey = preset.key();
        repaint();
        const SafePointer<PresetBrowser> safe(this);
        p.audition(preset);
        if (safe != nullptr && e.getNumberOfClicks() == 2)
            safe->impl->finish(false);
    }
}

void PresetBrowser::mouseMove(const MouseEvent& e) { impl->hover = e.position; repaint(); }
void PresetBrowser::mouseExit(const MouseEvent&) { impl->hover = { -1, -1 }; repaint(); }

void PresetBrowser::mouseWheelMove(const MouseEvent& e, const MouseWheelDetails& wheel)
{
    auto& p = *impl;
    if (!p.opened || wheel.deltaY == 0)
        return;
    const int amount = (wheel.deltaY > 0 ? -1 : 1) * jmax(1, roundToInt(std::abs(wheel.deltaY) * 10));
    if (sourcePane.contains(e.getPosition()) && !p.collection.isVisible())
    {
        p.sourceScroll = scrolledOffset(p.sourceScroll, amount, lastOffset(p.sources().size(), visibleSources));
        repaint();
    }
    else if (listPane.contains(e.getPosition()))
        p.moveList(amount);
}

bool PresetBrowser::keyPressed(const KeyPress& key)
{
    auto& p = *impl;
    if (!p.opened)
        return false;
    const bool cancel = key.isKeyCode(KeyPress::escapeKey);
    if (key.isKeyCode(KeyPress::upKey) || key.isKeyCode(KeyPress::downKey))
        p.step(key.isKeyCode(KeyPress::upKey) ? -1 : 1);
    else if (!cancel && !key.isKeyCode(KeyPress::returnKey))
        return false;
    else if (!p.collection.isVisible())
        p.finish(cancel);
    else if (cancel || p.store.addCollection(p.collection.getText().trim()))
    {
        p.collection.setVisible(false);
        grabKeyboardFocus();
        repaint();
    }
    return true;
}
} // namespace snap::ui
