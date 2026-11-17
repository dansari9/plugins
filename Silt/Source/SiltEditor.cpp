// One cell grid and device-pixel backing image, with the renderer and dial gestures shared with gulp.
// Layout only lives in panels[]; the cloud and each control group own their hit cells independently.
#include "BinaryData.h"
#include "Silt.h"
#include <optional>
#include <tuple>
using namespace juce;
namespace
{
constexpr int C = 120, R = 24, W = 1024, H = 410;
constexpr float cw = (float) W / C, ch = cw * 2;
const Colour page { 0xff0d0d0d }, white { 0xffececec }, grey { 0xff7a7a7a }, rule { 0xff2a2a2a }, dark { 0xff454545 }, red { 0xffff3d1f };
String u(const char* s) { return String::fromUTF8(s); }
Rectangle<int> cells(int x, int y, int w, int h)
{
    return Rectangle<int>::leftTopRightBottom(roundToInt(cw * (float) x), roundToInt(ch * (float) y), roundToInt(cw * (float) (x + w)), roundToInt(ch * (float) (y + h)));
}
constexpr uint8 brailleBit(int dx, int dy) { return (uint8) (dy == 3 ? 0x40 << dx : 1 << (dy + 3 * dx)); }
Font mono(float size, bool bold)
{
    static const Typeface::Ptr faces[] { Typeface::createSystemTypefaceFor(BinaryData::GeistMonoRegular_ttf, BinaryData::GeistMonoRegular_ttfSize),
                                         Typeface::createSystemTypefaceFor(BinaryData::GeistMonoSemiBold_ttf, BinaryData::GeistMonoSemiBold_ttfSize) };
    return Font(FontOptions(faces[bold ? 1 : 0]).withPointHeight(size));
}
struct Cell
{
    juce_wchar c = ' ';
    Colour fg, bg; // bg transparent: the page
    bool bold = false;
    bool operator!=(const Cell& o) const { return c != o.c || fg != o.fg || bg != o.bg || bold != o.bold; }
};
enum Mode { merge, under, over }; // braille onto braille: or the dots, keep the old cell, replace it (grid.py)
using Dots = std::vector<Point<int>>;
struct Grid
{
    Cell at[R][C];
    int ox = 0, oy = 0;
    Cell& cell(int x, int y) { return at[oy + y][ox + x]; }
    void put(int x, int y, const String& s, Colour fg, bool bold = false, Colour bg = {})
    {
        x += ox, y += oy;
        for (auto p = s.getCharPointer(); !p.isEmpty(); ++x)
            if (const auto c = p.getAndAdvance(); isPositiveAndBelow(x, C) && isPositiveAndBelow(y, R))
                at[y][x] = { c, fg, bg, bold };
    }
    void centre(int x, int w, int y, String s, Colour fg, bool bold = false)
    {
        s = s.substring(0, w), x += (w - s.length()) / 2;
        put(x, y, s, fg, bold);
    }
    void right(int x, int y, const String& s, Colour fg) { put(x - s.length() + 1, y, s, fg); }
    void braille(int x, int y, int w, int h, const Dots& dots, Colour fg, Mode mode = merge) // dots in a 2w x 4h space
    {
        x += ox, y += oy;
        std::vector<uint8> bits((size_t) (w * h));
        for (auto d : dots)
            if (isPositiveAndBelow(d.x, 2 * w) && isPositiveAndBelow(d.y, 4 * h))
                bits[(size_t) (d.y / 4 * w + d.x / 2)] |= brailleBit(d.x % 2, d.y % 4);
        for (int j = 0; j < h; ++j)
            for (int i = 0; i < w; ++i)
                if (int v = bits[(size_t) (j * w + i)]; v != 0 && isPositiveAndBelow(x + i, C) && isPositiveAndBelow(y + j, R))
                {
                    auto& cell = at[y + j][x + i];
                    const bool old = cell.c >= 0x2800 && cell.c <= 0x28ff;
                    if (old && mode == under) continue;
                    cell = { (juce_wchar) (0x2800 + (v | (old && mode == merge ? (int) cell.c - 0x2800 : 0))), fg, {}, false };
                }
    }
    void toggle(int x, int y, const String& label, bool on, bool dim = false)
    {
        put(x, y, u(on ? "■" : "□"), dim ? dark : on ? red : grey), put(x + 2, y, label, dim ? dark : on ? white : grey, false);
    }
};

// Controls draw nothing: the grid shows them. The value box only appears while a value is typed.
struct Look : LookAndFeel_V4
{
    Font getLabelFont(Label&) override { return mono(cw / 0.6f, false); }
    void drawLabel(Graphics&, Label&) override {}
    void drawLinearSlider(Graphics&, int, int, int, int, float, float, float, Slider::SliderStyle, Slider&) override {}
    void drawRotarySlider(Graphics&, int, int, int, int, float, float, float, Slider&) override {}
    Label* createSliderTextBox(Slider& s) override
    {
        auto* l = LookAndFeel_V4::createSliderTextBox(s);
        l->setColour(Label::backgroundWhenEditingColourId, page), l->setColour(Label::textWhenEditingColourId, white);
        l->setColour(Label::outlineWhenEditingColourId, {}), l->setColour(TextEditor::highlightColourId, red.withAlpha(0.4f));
        l->setColour(TextEditor::highlightedTextColourId, white), l->setColour(CaretComponent::caretColourId, red);
        return l;
    }
};

struct Key : Button
{
    explicit Key(const String& id) : Button(id) { setComponentID(id), setRepaintsOnMouseActivity(false); }
    void paintButton(Graphics&, bool, bool) override {}
};
// A round braille dial, 8 x 4 cells = 16 x 16 dots: a dotted 270 degree track, the value arc (centred: from the top)
// and a two-dot needle, layered as common.py's knob: track grey, arc red (merged), needle white (over).
static void knob(Grid& g, int x, int y, float norm, const String& name, const String& valueText, bool centred, bool dim)
{
    Dots track, arc, needle;
    const double c = 7.5, r = 7.2, v = 270.0 * norm;
    for (int t = 0; t <= 270; t += 3)
    {
        const double a = degreesToRadians(225.0 + t);
        ((centred ? jmin(135.0, v) <= t && t <= jmax(135.0, v) : t <= v) ? arc : track).push_back({ (int) (c + r * std::sin(a)), (int) (c - r * std::cos(a)) });
    }
    const double a = degreesToRadians(225 + v), sn = std::sin(a), cs = std::cos(a);
    for (auto [x0, y0, len] : { std::tuple { c, c, r - 2 }, { c + 0.5, c + 0.5, r - 2.5 } }) // common.py's dotline, Python rounding
    {
        const int steps = (int) jmax(std::abs(len * sn), std::abs(len * cs)) + 1;
        for (int k = 0; k < steps; ++k)
            needle.push_back({ (int) std::nearbyint(x0 + len * sn * k / jmax(1, steps - 1)), (int) std::nearbyint(y0 - len * cs * k / jmax(1, steps - 1)) });
    }
    g.braille(x, y, 8, 4, track, dim ? rule : dark), g.braille(x, y, 8, 4, arc, dim ? dark : red), g.braille(x, y, 8, 4, needle, dim ? grey : white, over);
    g.centre(x - 3, 14, y - 1, name, dim ? dark : grey);
    g.centre(x - 3, 14, y + 4, valueText, dim ? grey : white, !dim);
}

struct Panel { const char* name; int x, y, w, h; bool leftRule = false; };
// The final option 1 arrangement. All control coordinates below are local to their group.
constexpr Panel panels[] { {"header",0,0,120,3}, {"cloud",2,4,116,12}, {"grain",0,16,51,7},
    {"pitch",52,16,13,7,true}, {"trigger",66,16,16,7}, {"space / output",84,16,36,7,true},
    {"presets",2,3,108,1}, {"macros",2,3,108,1}, {"freeze",110,3,8,1} };
struct Group : Component
{
    SiltProcessor& p;
    Panel spec;
    OwnedArray<Key> keys;
    std::vector<std::unique_ptr<ParameterAttachment>> attachments;
    Group(SiltProcessor& proc, Panel panel) : p(proc), spec(panel)
    { setComponentID(String("group:") + panel.name), setBounds(cells(panel.x,panel.y,panel.w,panel.h)); }
    virtual void draw(Grid&) = 0;
    float value(const char* id) const { return p.apvts.getRawParameterValue(id)->load(); }
    Key& key(const String& id, int x, int y, int w, std::function<void()> action)
    {
        auto& k = *keys.add(new Key(id)); k.onClick = std::move(action);
        addAndMakeVisible(k), k.setBounds(cells(x,y,w,1)); return k;
    }
    void word(const char* id, int index, int x, int y, const char* text)
    {
        auto a = std::make_unique<ParameterAttachment>(*p.apvts.getParameter(id), [](float) {});
        key(String(id)+":"+String(index),x,y,String(text).length(),[index,att=a.get()] { att->setValueAsCompleteGesture((float) index); });
        attachments.push_back(std::move(a));
    }
    void toggle(const char* id, const char* text, int x, int y)
    {
        auto a = std::make_unique<ParameterAttachment>(*p.apvts.getParameter(id), [](float) {});
        key(id,x,y,String(text).length()+2,[this,id,att=a.get()] { att->setValueAsCompleteGesture(value(id)>.5f?0.0f:1.0f); });
        attachments.push_back(std::move(a));
    }
};
struct Header : Group
{
    std::function<void()> changed;
    enum Kind { action, slot, quality, macro };
    struct Item { const char *id,*text; int x; Kind kind=action; int index=0; std::function<void()> run; };
    std::array<Item,11> items;
    explicit Header(SiltProcessor& proc) : Group(proc,panels[0]), items {{
        {"prev","‹",10,action,0,[this] {p.stepPreset(-1);}}, {"next","›",20,action,0,[this] {p.stepPreset(1);}},
        {"save","save",27,action,0,[this] {p.saveUserPreset();}},
        {"A","A",33,slot,0,[this] {p.selectAb(0);}}, {"B","B",35,slot,1,[this] {p.selectAb(1);}},
        {"rnd","randomize",82,action,0,[this] {p.randomise(Time::getHighResolutionTicks());}},
        {"macro","macros",95,macro,0,[this] {p.showMacros=!p.showMacros;if(changed)changed();}},
        {"oversample:0","off",63,quality,0,{}}, {"oversample:1","2x",67,quality,1,{}},
        {"oversample:2","4x",70,quality,2,{}}, {"oversample:3","8x",73,quality,3,{}} }}
    {
        for(const auto& item:items)
            if(item.kind==quality)word("oversample",item.index,item.x,1,item.text);
            else key(item.id,item.x,1,String::fromUTF8(item.text).length()+(item.kind==macro?2:0),item.run);
    }
    void draw(Grid& g) override
    {
        const auto name=p.presetName().substring(0,10);
        g.put(2,1,"silt",white,true);
        g.put(12,1,p.preset>=0?String(p.preset.load()+1).paddedLeft('0',2):String("--"),grey);
        g.put(15,1,name,white,true);
        items[1].x=16+name.length();
        keys[1]->setBounds(cells(items[1].x,1,1,1));
        for(const auto& item:items)
        {
            const int x=item.x;
            if(item.kind==macro)g.toggle(x,1,item.text,p.showMacros);
            else if(item.kind==slot)
            {
                const bool selected=p.abSlot()==item.index;
                g.put(x,1,item.text,selected?page:grey,selected,selected?white:Colour());
            }
            else g.put(x,1,u(item.text),item.kind==quality?(roundToInt(value("oversample"))==item.index?white:dark):grey);
        }
        g.put(52,1,"oversample",dark);
        g.right(117,1,"daveydidit",dark),g.put(2,2,String::repeatedString(u("─"),116),rule);
    }
};
struct Presets : Group
{
    explicit Presets(SiltProcessor& proc) : Group(proc,panels[6])
    { for(int i=0;i<silt::numPresets;++i) key("preset:"+String(i),11*i,0,String(silt::presets[i].name).length(),[this,i] { p.setCurrentProgram(i); }); }
    void draw(Grid& g) override
    { for(int i=0;i<silt::numPresets;++i) g.put(11*i,0,silt::presets[i].name,p.preset==i?red:grey); }
};
struct Freeze : Group
{
    explicit Freeze(SiltProcessor& proc) : Group(proc,panels[8]) { toggle("freeze","freeze",0,0); }
    void draw(Grid& g) override { g.toggle(0,0,"freeze",value("freeze")>.5f); }
};

// Event history is published by the audio thread, so even an editor opened mid-note has a cloud. It advances
// only with processed audio: stopped/idle visuals retain their last frame, and frozen capture stops scrolling.
struct Cloud : Group
{
    explicit Cloud(SiltProcessor& proc) : Group(proc,panels[1]) {}
    void draw(Grid& g) override
    {
        constexpr int w=116,h=11,ww=2*w,hh=4*h;
        const auto& f=p.feed;
        const auto now=f.time.load(); const double fs=f.sampleRate.load();
        const bool frozen=f.frozen.load();
        const double pos=jlimit(0.0,4.0,(double)f.centre.load()), scatter=value("scatter")/100.0;
        const auto dot=[](double sec) {return (1-sec/4)*(ww-1);};
        const auto cell=[&](double sec) {return (int)std::nearbyint(dot(sec))/2;};
        const int xa=roundToInt(dot(jmin(4.0,pos+scatter*2))),xb=roundToInt(dot(jmax(0.0,pos-scatter*2)));
        const double mid=hh/2.0-.5;
        // Faint mirrored input waveform (brighter inside the read window), then grain spindles, as silt3.py.
        for(int shade=0;shade<5;++shade)
        {
            Dots pts;
            for(int k=0;k<ww;k+=2)
            {
                const int bin=(f.at.load()-1-(int)std::nearbyint((ww-1-k)*4.0/ww/8*silt::Feed::bins)+silt::Feed::bins*2)%silt::Feed::bins;
                const double amplitude=jlimit(0.0,1.0,(double)f.wave[(size_t)bin].load()*3.0);
                const bool inside=xa<=k && k<=xb;
                if((inside?4:jmin(3,k*4/ww))!=shade)continue;
                const double e=amplitude*hh*.34;
                for(int t=(int)(mid-e);t<=(int)(mid+e);++t) if((t+k/2)%2==0)pts.push_back({k,t});
            }
            const uint32 shades[]{0xff171717,0xff1c1c1c,0xff232323,0xff2d2d2d,0xff3a3a3a};
            g.braille(0,0,w,h,pts,Colour(frozen && shade<4?0xff333333:shades[shade]));
        }
        Dots zero;for(int k=0;k<ww;k+=4)zero.push_back({k,roundToInt(mid)});g.braille(0,0,w,h,zero,rule);
        constexpr double limits[]{0,.025,.07,.17,.29,.44,.60,.78,1.01};
        const uint32 ramp[]{0xffff5a33,0xffff3d1f,0xfff4f4f4,0xffc4c4c4,0xff949494,0xff686868,0xff474747,0xff2e2e2e};
        const auto count=f.events.load(std::memory_order_acquire);
        const auto head=f.captureHead.load();
        Dots byShade[8]; // one pass over the history, drawn oldest shade first
        for(auto i=jmax((juce::int64)0,count-(juce::int64)silt::Feed::historySize);i<count;++i)
        {
            const auto& e=f.history[(size_t)i%silt::Feed::historySize];
            const auto sequence=e.sequence.load(std::memory_order_acquire);
            if(sequence&1u)continue;
            const auto frame=e.frame.load(std::memory_order_relaxed),eventHead=e.captureHead.load(std::memory_order_relaxed);
            const auto grain=e.grain.load(std::memory_order_relaxed);
            const float eventBack=e.back.load(std::memory_order_relaxed),eventPitch=e.pitch.load(std::memory_order_relaxed),eventSize=e.size.load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire); // the field reads complete before the sequence re-check
            if(sequence!=e.sequence.load(std::memory_order_relaxed))continue;
            const double age=(now-frame)/fs/3.0;
            const int shade=(int)(std::upper_bound(std::begin(limits),std::end(limits),age)-std::begin(limits))-1;
            if(shade<0 || shade>7)continue;
            const double back=eventBack+(head-eventHead)/fs;
            if(back<0 || back>4)continue;
            auto& pts=byShade[shade];
            // Stable scatter gives each actual grain a legible pitch lane; pitch remains the y-axis.
            const uint32 hash=grain*2654435761u;
            const double jitter=std::sqrt(-2*std::log(jmax(.001,(double)(hash>>8)*0x1p-24)))*std::cos((hash&255)*MathConstants<double>::twoPi/256)*(1.8+scatter*hh*.27);
            const double py=jlimit(4.0,(double)hh-4,mid-eventPitch/24.0*(hh/2-3)*.3+jitter);
            const int len=jmax(2,roundToInt(eventSize/4*ww));
            const double u=(double)(hash&255)/255,amp=shade==0?jmax(.8,std::pow(u,1.5)):std::pow(u,1.5); // mostly thin spindles
            for(int k=0;k<len;++k)
            {
                const int thickness=(int)(std::sin(MathConstants<double>::pi*(k+.5)/len)*amp*jmin(2.0,len/6.0)+.35);
                for(int d=-thickness;d<=thickness;++d)pts.push_back({(int)(dot(back)-len/2.0+k),roundToInt(py+d)});
            }
        }
        for(int shade=7;shade>=0;--shade)g.braille(0,0,w,h,byShade[shade],Colour(ramp[shade]),over);
        for(auto [edge,dir]:{std::pair{xa,1},std::pair{xb,-1}})
        {
            Dots dots,ends;for(int t=0;t<hh;t+=2)dots.push_back({edge,t});
            for(int k=1;k<4;++k) for(int t:{0,hh-1})ends.push_back({edge+dir*k,t});
            g.braille(0,0,w,h,dots,Colour(0xff4a4a4a),under),g.braille(0,0,w,h,ends,Colour(0xff8a8a8a),over);
        }
        Dots marker;for(int t=0;t<hh;t+=3)for(int d:{0,1})marker.push_back({2*cell(pos)+d,t});
        g.braille(0,0,w,h,marker,Colour(0xff5c1d12),under);
        Dots edge;const double e=jlimit(0.0,1.0,(double)f.wave[(size_t)((f.at.load()+silt::Feed::bins-1)%silt::Feed::bins)].load()*3)*hh*.34;
        for(int t=(int)(mid-e);t<=(int)(mid+e);++t)edge.push_back({ww-1,t});
        g.braille(0,0,w,h,edge,Colour(0xff6a6a6a),over);
        const int ry=h;
        g.put(0,ry,String::repeatedString(u("─"),w),rule);
        for(int k=0;k<=8;++k)g.put(cell(k/2.0),ry,u("┴"),k%2==0?dark:rule);
        const int a=cell(jmin(4.0,pos+scatter*2)),b=cell(jmax(0.0,pos-scatter*2));
        for(int k=a;k<=b;++k)g.cell(k,ry).fg=Colour(0xffbcbcbc);
        g.put(a,ry,u("├"),Colour(0xffbcbcbc)),g.put(b,ry,u("┤"),Colour(0xffbcbcbc));
        g.put(0,ry,"4 s ",dark),g.right(w-1,ry,frozen?" held":" now",frozen?red:dark);
        for(int sec:{3,2,1})g.put(cell(sec)-2,ry," "+String(sec)+" s ",dark);
        g.put(cell(pos),ry,u("▲"),red);
    }
};
struct Dial { const char *id,*name; int cx; bool centred=false; std::function<bool()> dim={}; }; // dim: the control does nothing now
struct Controls : Group
{
    std::vector<Dial> dials;
    bool fader;
    std::function<void(Grid&)> extra;
    OwnedArray<Slider> sliders;
    std::vector<std::unique_ptr<AudioProcessorValueTreeState::SliderAttachment>> sliderAttachments;
    Controls(SiltProcessor& proc,Panel panel,std::initializer_list<Dial> ds,bool horizontal=false) : Group(proc,panel),dials(ds),fader(horizontal)
    {
        for(const auto& d:dials)
        {
            auto& k=*sliders.add(new Slider());
            k.setSliderStyle(fader?Slider::LinearHorizontal:Slider::RotaryVerticalDrag);
            if(fader)k.setSliderSnapsToMousePosition(false);
            k.setTextBoxStyle(fader?Slider::TextBoxRight:Slider::TextBoxBelow,false,roundToInt(cw*(fader?5:10)),roundToInt(ch));
            k.setMouseDragSensitivity(250),k.setComponentID(d.id),k.setRepaintsOnMouseActivity(false);
            sliderAttachments.push_back(std::make_unique<AudioProcessorValueTreeState::SliderAttachment>(p.apvts,d.id,k));
            auto* q=p.apvts.getParameter(d.id);k.setDoubleClickReturnValue(true,q->convertFrom0to1(q->getDefaultValue()));
            addAndMakeVisible(k),k.setBounds(fader?cells(d.cx+8,0,16,1):cells(d.cx-5,1,10,5));k.updateText();
        }
    }
    void draw(Grid& g) override
    {
        for(const auto& d:dials)
        {
            auto* q=p.apvts.getParameter(d.id);
            const int cx=d.cx;
            if(!fader)knob(g,cx-4,1,q->getValue(),d.name,q->getCurrentValueAsText(),d.centred,d.dim && d.dim());
            else
            {
                g.put(cx,0,d.name,grey);
                const int thumb=roundToInt(q->getValue()*21);
                Dots off,on,tip;
                for(int i=0;i<22;++i)if(i<=thumb)on.push_back({i,2});else if(i%2==0)off.push_back({i,2});
                for(int t=0;t<4;++t)tip.push_back({thumb,t});
                for(int t=1;t<4;++t)tip.push_back({thumb+1,t});
                g.braille(cx+8,0,11,1,off,rule),g.braille(cx+8,0,11,1,on,red,over),g.braille(cx+8,0,11,1,tip,white);
                g.right(cx+23,0,q->getCurrentValueAsText(),white);
            }
        }
        if(extra)extra(g);
        for(auto* k:sliders)for(auto* child:k->getChildren())if(auto* label=dynamic_cast<Label*>(child);label && !label->isBeingEdited())k->updateText();
    }
};
struct TriggerGroup : Group
{
    explicit TriggerGroup(SiltProcessor& proc) : Group(proc,panels[4])
    { word("trigger",0,0,1,"clock"),word("trigger",1,0,2,"transient"),word("trigger",2,0,3,"sidechain"); }
    void draw(Grid& g) override
    {
        g.put(0,0,"trigger",grey);
        const char* names[]{"clock","transient","sidechain"};
        for(int i=0;i<3;++i){const bool selected=roundToInt(value("trigger"))==i;g.put(0,1+i,names[i],selected?red:grey);if(selected)g.put(14,1+i,u("◀"),red);}
        const auto note=MidiMessage::getMidiNoteName(p.feed.note.load(),true,true,3).toLowerCase();
        g.put(0,5,"midi",grey),g.put(5,5,u("●"),p.feed.held?red:dark),g.put(7,5,note,white),g.put(0,6,"sc",grey);
        Dots off,on,tick;for(int k=0;k<18;++k)for(int t:{1,2})(k<p.feed.meter[2].load()*3*18?on:off).push_back({k,t});
        for(int t=0;t<4;++t)tick.push_back({8,t});
        g.braille(3,6,9,1,off,rule),g.braille(3,6,9,1,on,grey,over),g.braille(3,6,9,1,tick,white);
    }
};
struct Editor : AudioProcessorEditor, Timer
{
    SiltProcessor& proc;
    Look look;
    Component layer;
    Header header;
    Cloud cloud;
    Controls grain,pitch,space,macros;
    TriggerGroup trigger;
    Presets presets;
    Freeze freeze;
    Grid shown;
    std::vector<float> drawnFrom;
    String drawnName;
    Image image;
    Font fonts[2] {mono(1,false),mono(1,true)};
    explicit Editor(SiltProcessor& p) : AudioProcessorEditor(p),proc(p),header(p),cloud(p),
        grain(p,panels[2],{{"size","size",8},{"density","density",20},{"position","position",32},{"scatter","scatter",44}}),
        pitch(p,panels[3],{{"pitch","pitch",6,true}}),
        space(p,panels[5],{{"reverb","",6},{"mix","mix",18},{"output","output",30}}),
        macros(p,panels[7],{{"motion","motion",0},{"texture","texture",27},{"bloom","bloom",54},{"chaos","chaos",81}},true),
        trigger(p),presets(p),freeze(p)
    {
        grain.word("sizeSync",0,4,6,"sync"),grain.word("sizeSync",1,9,6,"free");
        pitch.word("pitchMode",0,1,6,"shift"),pitch.word("pitchMode",1,7,6,"tape");
        space.toggle("reverbOn","reverb",1,0);
        grain.dials[1].dim=[this] {return grain.value("trigger")>.5f;}; // transient/sidechain: one grain per onset
        space.dials[0].dim=[this] {return space.value("reverbOn")<.5f;};
        grain.extra=[this](Grid& g) {g.put(4,6,"sync",grain.value("sizeSync")<.5f?red:grey),g.put(9,6,"free",grain.value("sizeSync")>.5f?red:grey);};
        pitch.extra=[this](Grid& g) {g.put(1,6,"shift",pitch.value("pitchMode")<.5f?red:grey),g.put(7,6,"tape",pitch.value("pitchMode")>.5f?red:grey);};
        space.extra=[this](Grid& g) {
            g.put(3,0,"reverb",space.value("reverbOn")>.5f?white:grey),g.put(1,0,u(space.value("reverbOn")>.5f?"■":"□"),space.value("reverbOn")>.5f?red:grey);
            Dots off,on,tick;
            for(int k=0;k<16;++k)for(int t=0;t<4;++t)
                (k<jlimit(0.0f,1.0f,proc.feed.meter[(size_t)(t/2)].load()*3)*16?on:off).push_back({k,t});
            const int peak=roundToInt(jlimit(0.0f,1.0f,jmax(proc.feed.meter[0].load(),proc.feed.meter[1].load())*3)*15);
            for(int t=0;t<4;++t)tick.push_back({peak,t});
            g.braille(26,6,8,1,off,rule),g.braille(26,6,8,1,on,Colour(0xffbcbcbc),over),g.braille(26,6,8,1,tick,white,over);
        };
        setOpaque(true),setLookAndFeel(&look),addAndMakeVisible(layer);layer.setBounds(0,0,W,H);
        for(auto* group:groups())layer.addAndMakeVisible(group);
        header.changed=[this] {macros.setVisible(proc.showMacros),presets.setVisible(!proc.showMacros);refresh();};
        macros.setVisible(proc.showMacros),presets.setVisible(!proc.showMacros);
        const int stored=proc.editorWidth;
        setResizable(true,false),setResizeLimits(W/2,H/2,W*2,H*2);getConstrainer()->setFixedAspectRatio((double)W/H);
        const int width=stored>0?jlimit(W/2,W*2,stored):W;
        setSize(width,roundToInt((double)width*H/W)),timerCallback(),startTimerHz(30);
    }
    ~Editor() override {setLookAndFeel(nullptr);}
    std::array<Group*,9> groups() {return {&header,&cloud,&grain,&pitch,&trigger,&space,&presets,&macros,&freeze};}
    void resized() override
    {
        layer.setTransform(AffineTransform::scale((float) getWidth() / W, (float) getHeight() / H));
        proc.editorWidth = getWidth();
    }
    void timerCallback() override
    {
        std::vector<float> now;
        for (auto* param : proc.getParameters()) now.push_back(param->getValue());
        auto& f = proc.feed;
        now.insert(now.end(), { (float) f.time.load(), (float) f.events.load(), (float) f.note.load(), (float) f.held.load(), f.meter[0].load(), f.meter[1].load(), f.meter[2].load(), (float) f.at.load(), f.centre.load(), (float) f.frozen.load(), (float) proc.showMacros.load(), (float) proc.abSlot(), (float) proc.preset.load() });
        for (size_t k = 0; k < silt::Feed::bins; ++k) now.insert(now.end(), { f.wave[k].load() });
        if (now == drawnFrom && proc.presetName() == drawnName) return;
        drawnFrom = std::move(now), drawnName = proc.presetName();
        macros.setVisible(proc.showMacros), presets.setVisible(!proc.showMacros), refresh();
    }
    void refresh()
    {
        Grid g;
        for (auto* group : groups()) if (group->isVisible())
        {
            g.ox=group->spec.x, g.oy=group->spec.y;
            group->draw(g);
            if(group->spec.leftRule)for(int y=0;y<group->spec.h;++y)g.put(-1,y,u("│"),rule);
        }
        show(g);
    }
    // Cells that differ from the shown grid are redrawn into the image; runs of them on a row are repainted.
    void show(const Grid& g)
    {
        std::optional<Graphics> context;
        if (image.isValid()) context.emplace(image);
        for (int j = 0; j < R; ++j)
            for (int i = 0, first = -1, last = -1; i <= C; ++i)
            {
                if (i < C && g.at[j][i] != shown.at[j][i])
                {
                    shown.at[j][i] = g.at[j][i], first = first < 0 ? i : first, last = i;
                    if (context) drawCell(*context, i, j);
                }
                else if (first >= 0 && (i == C || i - last > 8))
                {
                    const float sx = (float) getWidth() / C, sy = (float) getHeight() / R;
                    repaint(Rectangle<float>(sx * (float) first, sy * (float) j, sx * (float) (last + 1 - first), sy).getSmallestIntegerContainer());
                    first = -1;
                }
            }
    }
    void paint(Graphics& g) override
    {
        const float scale = g.getInternalContext().getPhysicalPixelScaleFactor();
        const int iw = jmax(1, roundToInt((float) getWidth() * scale)), ih = jmax(1, roundToInt((float) getHeight() * scale));
        if (image.getWidth() != iw || image.getHeight() != ih)
        {
            image = Image(Image::RGB, iw, ih, false);
            fonts[0] = mono((float) iw / C / 0.6f, false), fonts[1] = mono((float) iw / C / 0.6f, true);
            Graphics ig(image);
            for (int j = 0; j < R; ++j)
                for (int i = 0; i < C; ++i)
                    drawCell(ig, i, j);
        }
        g.drawImageTransformed(image, AffineTransform::scale((float) getWidth() / (float) iw, (float) getHeight() / (float) ih));
    }
    // One cell in device pixels, snapped to whole pixels so lines and blocks join; vector glyphs as grid.py draws them.
    void drawCell(Graphics& g, int i, int j)
    {
        const auto& c = shown.at[j][i];
        const auto px = [this](int k) { return roundToInt((float) k * (float) image.getWidth() / C); };
        const auto py = [this](int k) { return roundToInt((float) k * (float) image.getHeight() / R); };
        const auto r = Rectangle<int>::leftTopRightBottom(px(i), py(j), px(i + 1), py(j + 1));
        g.setColour(c.bg.isTransparent() ? page : c.bg), g.fillRect(r);
        if (c.c == ' ') return;
        Graphics::ScopedSaveState state(g);
        g.reduceClipRegion(r), g.setColour(c.fg); // nothing spills into a neighbour, which may be redrawn alone
        const float w = (float) image.getWidth() / C, h = (float) image.getHeight() / R, x = (float) r.getX(), y = (float) r.getY();
        const float cx = x + (float) r.getWidth() / 2, cy = y + (float) r.getHeight() / 2, lw = jmax(1.0f, w * 0.12f);
        const int t = jmax(1, roundToInt(lw)), hx = roundToInt(cx - (float) t / 2), hy = roundToInt(cy - (float) t / 2);
        const auto glyph = [&](juce_wchar code) {
            if (const auto id = fonts[c.bold ? 1 : 0].getTypefacePtr()->getNominalGlyphForCodepoint(code))
            {
                const float size = w / 0.6f; // Geist Mono: advance 0.6 em, ascent 1.005 + descent 0.295 em
                const uint16_t index = (uint16_t) *id;
                const Point<float> at { x + ((float) r.getWidth() - w) / 2, y + (h - 1.3f * size) / 2 + 1.005f * size };
                g.setFont(fonts[c.bold ? 1 : 0]);
                g.getInternalContext().drawGlyphs({ &index, 1 }, { &at, 1 }, {});
            }
        };
        const int arms = c.c == 0x2500 ? 10 : c.c == 0x2502 ? 5 : c.c == 0x2534 ? 11 : c.c == 0x251c ? 7 : c.c == 0x2524 ? 13 : 0; // ─ │ ┴ ├ ┤: up 1 right 2 down 4 left 8
        if (arms != 0)
        {
            const int top = r.getY();
            if (arms & 8) g.fillRect(r.getX(), hy, hx + t - r.getX(), t);
            if (arms & 2) g.fillRect(hx, hy, r.getRight() - hx, t);
            if (arms & 1) g.fillRect(hx, top, t, hy + t - top);
            if (arms & 4) g.fillRect(hx, hy, t, r.getBottom() - hy);
        }
        else if (c.c >= 0x2800 && c.c <= 0x28ff) // braille
        {
            for (int dx = 0; dx < 2; ++dx)
                for (int dy = 0; dy < 4; ++dy)
                    if ((c.c - 0x2800) & brailleBit(dx, dy))
                    {
                        const Point<float> centre {x+w*(.3f+.4f*(float)dx),y+h*(.125f+.25f*(float)dy)};
                        if(c.fg==Colour(0xffff5a33))
                        { // the newest grains glow (silt3.py's drop shadow), still clipped to the cell
                            g.setColour(red.withAlpha(.18f)),g.fillEllipse(Rectangle<float>(w*.95f,w*.95f).withCentre(centre));
                            g.setColour(red.withAlpha(.35f)),g.fillEllipse(Rectangle<float>(w*.55f,w*.55f).withCentre(centre)),g.setColour(c.fg);
                        }
                        g.fillEllipse(Rectangle<float>(.3f*w,.3f*w).withCentre(centre));
                    }
        }
        else if (c.c == 0x25cf) g.fillEllipse(Rectangle<float>(.72f*w,.72f*w).withCentre({cx,cy}));
        else if (c.c == 0x25a0) g.fillRect(Rectangle<float>(0.72f * w, 0.36f * h).withCentre({ cx, cy })); // ■
        else if (c.c == 0x25a1) g.drawRect(Rectangle<float>(0.72f * w, 0.36f * h).withCentre({ cx, cy }), lw); // □
        else if (c.c == 0x25b2 || c.c == 0x25c0) // ▲ ◀
        {
            const float s = 0.95f * 0.36f * w;
            Path p;
            if (c.c == 0x25b2) p.addTriangle(cx, cy - s, cx + s, cy + 0.8f * s, cx - s, cy + 0.8f * s);
            else p.addTriangle(cx - s, cy, cx + 0.8f * s, cy - s, cx + 0.8f * s, cy + s);
            g.fillPath(p);
            g.strokePath(p, PathStrokeType(0.8f * lw, PathStrokeType::curved, PathStrokeType::rounded));
        }
        else if (c.c == 0x2039 || c.c == 0x203a) // ‹ › (not in Geist Mono)
        {
            const float d = c.c == 0x2039 ? 1.0f : -1.0f;
            Path p;
            p.startNewSubPath(cx + d * 0.14f * w, cy - 0.14f * h), p.lineTo(cx - d * 0.14f * w, cy + 0.02f * h), p.lineTo(cx + d * 0.14f * w, cy + 0.18f * h);
            g.strokePath(p, PathStrokeType(lw));
        }
        else glyph(c.c);
    }
};
} // namespace
AudioProcessorEditor* SiltProcessor::createEditor() { return new Editor(*this); }
