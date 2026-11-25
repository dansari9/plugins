#pragma once
// The preset list under the preset's name, the same in gulp, silt, snap and crumb (and on the site, js/preset-menu.js):
// every preset once, in short groups that flow into columns, the current one inverted as A / B is. A click (or return)
// loads and closes; esc, the name again or a click outside closes; the arrow keys walk it; the wheel scrolls it when it is
// longer than the window. It stands on the editor's cells: `cw` x `ch` design pixels each, Geist Mono at cw / 0.6.
#include <functional>
#include <juce_gui_basics/juce_gui_basics.h>
#include <vector>

class PresetMenu : public juce::Component
{
public:
    struct Item { juce::String name; int id; };
    struct Group { juce::String name; std::vector<Item> items; };
    std::function<void(int)> onChoose;
    std::function<juce::Font(float size, bool bold)> font;
    float cw = 8, ch = 16, drop = 0; // drop: rows the list starts under its cells' top (an editor's rule may stand in the middle of a row)
    juce::Colour page { 0xff0d0d0d }, box { 0xff454545 }, rule { 0xff2c2c2c }, header { 0xff7a7a7a }, text { 0xffbcbcbc }, lit { 0xffefe6d2 };

    PresetMenu()
    {
        setComponentID("presetMenu"), setTitle("presets");
        setWantsKeyboardFocus(true);
    }

    // room: the cells of the component's parent the list may use; it hangs from its top left corner.
    void open(std::vector<Group> groups, int currentId, juce::Rectangle<int> room)
    {
        rows.clear(), heads.clear();
        current = currentId, scroll = 0, width = 0;
        std::vector<Group> parts; // a long group (the user's files) goes on in unnamed parts
        for (auto& g : groups)
            for (size_t from = 0; from < g.items.size(); from += longest)
                parts.push_back({ from == 0 ? g.name : juce::String(),
                                  std::vector<Item>(g.items.begin() + (long) from, g.items.begin() + (long) std::min(g.items.size(), from + longest)) });
        for (auto& g : parts)
        {
            width = juce::jmax(width, g.name.length() + 3);
            for (auto& i : g.items) i.name = i.name.substring(0, cut), width = juce::jmax(width, i.name.length() + 2);
        }
        const int most = juce::jmax(1, (room.getWidth() - 4 + gap) / (width + gap)), tall = juce::jmax(1, room.getHeight() - 2);
        for (int columns = juce::jmin(4, most);; ++columns)
        {
            for (height = 1; flow(parts, height, false) > columns; ++height) {}
            if (height <= tall || columns >= most) break;
        }
        const int columns = flow(parts, height, true);
        view = juce::jmin(height, tall);
        const int w = columns * (width + gap) - gap + 4;
        area = { room.getX(), room.getY(), w, view + 2 };
        at = 0;
        for (size_t k = 0; k < rows.size(); ++k)
            if (rows[k]->id == current) at = (int) k;
        if (auto* parent = getParentComponent()) setBounds(parent->getLocalBounds());
        reveal(), setVisible(true), toFront(false);
        if (isShowing()) grabKeyboardFocus();
    }
    void close() { setVisible(false); }
    juce::Rectangle<int> boxBounds() const { return pixels(area); }

    void paint(juce::Graphics& g) override
    {
        const auto r = pixels(area).toFloat();
        g.setColour(page), g.fillRect(r);
        g.setColour(box), g.drawRect(r, 1.0f);
        for (auto& h : heads)
            if (const int y = h.y - scroll; y >= 0 && y < view)
            {
                const float x = cw * (float) (area.getX() + 2 + h.x), top = ch * ((float) (area.getY() + 1 + y) + drop);
                write(g, h.name, x + cw, top, header, false);
                const float from = x + cw * (float) (h.name.isEmpty() ? 1 : h.name.length() + 2);
                g.setColour(rule), g.fillRect(from, top + ch / 2, x + cw * (float) (width - 1) - from, 1.0f);
            }
        if (scroll > 0) write(g, "^", cw * (float) area.getCentreX(), ch * ((float) area.getY() + drop), header, false, true);
        if (scroll < height - view) write(g, "v", cw * (float) area.getCentreX(), ch * ((float) (area.getBottom() - 1) + drop), header, false, true);
    }
    void mouseDown(const juce::MouseEvent& e) override
    {
        if (!pixels(area).contains(e.getPosition())) close();
    }
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails& w) override
    {
        wheel += (w.isReversed ? -1.0f : 1.0f) * w.deltaY;
        if (std::abs(wheel) < 0.1f) return;
        scroll = juce::jlimit(0, juce::jmax(0, height - view), scroll + (wheel > 0 ? -2 : 2)), wheel = 0;
        place();
    }
    bool keyPressed(const juce::KeyPress& k) override
    {
        const int code = k.getKeyCode(), last = (int) rows.size() - 1;
        if (code == juce::KeyPress::escapeKey) return close(), true;
        if (last < 0) return false;
        if (code == juce::KeyPress::returnKey || code == juce::KeyPress::spaceKey) return choose(rows[(size_t) at]->id), true;
        if (code == juce::KeyPress::upKey) at = juce::jmax(0, at - 1);
        else if (code == juce::KeyPress::downKey) at = juce::jmin(last, at + 1);
        else if (code == juce::KeyPress::homeKey) at = 0;
        else if (code == juce::KeyPress::endKey) at = last;
        else if (code == juce::KeyPress::leftKey || code == juce::KeyPress::rightKey) at = beside(code == juce::KeyPress::rightKey ? 1 : -1);
        else return false;
        reveal();
        return true;
    }
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override
    {
        return std::make_unique<juce::AccessibilityHandler>(*this, juce::AccessibilityRole::popupMenu);
    }

private:
    static constexpr size_t longest = 16; // items in one column part
    static constexpr int cut = 26, gap = 2;

    struct Row : juce::Button
    {
        PresetMenu& menu;
        int id, index, x, y;
        Row(PresetMenu& m, const Item& item, int k, int cx, int cy) : juce::Button(item.name), menu(m), id(item.id), index(k), x(cx), y(cy)
        {
            setComponentID("preset" + juce::String(item.id)), setTitle(item.name);
            setWantsKeyboardFocus(false), setMouseClickGrabsKeyboardFocus(false);
            onClick = [this] { menu.choose(id); };
        }
        void mouseEnter(const juce::MouseEvent& e) override { menu.point(index), juce::Button::mouseEnter(e); }
        void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override { menu.mouseWheelMove(e, w); }
        void paintButton(juce::Graphics& g, bool, bool) override
        {
            const bool now = id == menu.current;
            if (now) g.setColour(menu.lit), g.fillRect(getLocalBounds());
            menu.write(g, getName(), menu.cw, 0, now ? menu.page : menu.at == index ? menu.lit : menu.text, now || menu.at == index);
        }
    };
    struct Head { juce::String name; int x, y; };
    std::vector<std::unique_ptr<Row>> rows;
    std::vector<Head> heads;
    juce::Rectangle<int> area; // the box, in cells
    int current = -1, at = 0, scroll = 0, width = 0, height = 0, view = 0;
    float wheel = 0;

    juce::Rectangle<int> pixels(juce::Rectangle<int> c) const
    {
        return c.toFloat().translated(0, drop).transformedBy(juce::AffineTransform::scale(cw, ch)).toNearestInt();
    }
    // Groups one under the other, a blank row between, a new column where the next would not fit; returns the columns.
    int flow(const std::vector<Group>& groups, int tall, bool build)
    {
        int column = 0, y = 0;
        for (auto& g : groups)
        {
            const int need = 1 + (int) g.items.size();
            if (need > tall) return 1 << 20;
            if (y > 0 && y + 1 + need > tall) ++column, y = 0;
            if (y > 0) ++y;
            if (build)
            {
                const int x = column * (width + gap);
                heads.push_back({ g.name, x, y });
                for (size_t k = 0; k < g.items.size(); ++k)
                {
                    rows.push_back(std::make_unique<Row>(*this, g.items[k], (int) rows.size(), x, y + 1 + (int) k));
                    addChildComponent(*rows.back());
                }
            }
            y += need;
        }
        return column + 1;
    }
    int beside(int by) const // the row of the next column nearest this one
    {
        const auto& from = *rows[(size_t) at];
        int best = at, distance = 1 << 20;
        for (auto& r : rows)
            if (r->x == from.x + by * (width + gap))
                if (const int d = std::abs(r->y - from.y); d < distance) distance = d, best = r->index;
        return best;
    }
    void point(int index)
    {
        if (at == index) return;
        rows[(size_t) at]->repaint(), at = index, rows[(size_t) at]->repaint();
    }
    void reveal() // the marked row is in view
    {
        if (!rows.empty())
        {
            const int y = rows[(size_t) at]->y;
            scroll = juce::jlimit(juce::jmax(0, y - view + 1), juce::jmax(0, juce::jmin(y, height - view)), scroll);
        }
        place();
    }
    void place()
    {
        for (auto& r : rows)
        {
            const int y = r->y - scroll;
            r->setBounds(pixels({ area.getX() + 2 + r->x, area.getY() + 1 + y, width, 1 }));
            r->setVisible(y >= 0 && y < view);
        }
        repaint();
    }
    void choose(int id)
    {
        close();
        if (onChoose) onChoose(id);
    }
    void write(juce::Graphics& g, const juce::String& s, float x, float y, juce::Colour c, bool bold, bool masked = false) const
    {
        if (masked) g.setColour(page), g.fillRect(x - cw, y, 3 * cw, ch);
        const float size = cw / 0.6f; // Geist Mono: advance 0.6 em, ascent 1.005 + descent 0.295 em
        juce::GlyphArrangement line;
        line.addLineOfText(font ? font(size, bold) : juce::Font(juce::FontOptions(size)), s, x, y + (ch - 1.3f * size) / 2 + 1.005f * size);
        g.setColour(c), line.draw(g);
    }
};
