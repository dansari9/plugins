#include "Silt.h"
#include "BinaryData.h"
#include <juce_cryptography/juce_cryptography.h>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iomanip>
#include <limits>
#include <iostream>
#include <new>
#include <thread>

// Real-time check: count heap allocations made on a thread flagged as "audio" (as gulp).
static thread_local bool onAudioThread = false;
static std::atomic<int> audioAllocations { 0 };
static void countAllocation() { audioAllocations += onAudioThread ? 1 : 0; }
#if defined(__GLIBC__) && !defined(__SANITIZE_ADDRESS__)
#define SILT_COUNTS_MALLOC 1
extern "C" void* __libc_malloc(size_t);
extern "C" void* __libc_calloc(size_t, size_t);
extern "C" void* __libc_realloc(void*, size_t);
extern "C" void* malloc(size_t n) noexcept { return countAllocation(), __libc_malloc(n); }
extern "C" void* calloc(size_t n, size_t s) noexcept { return countAllocation(), __libc_calloc(n, s); }
extern "C" void* realloc(void* p, size_t n) noexcept { return countAllocation(), __libc_realloc(p, n); }
#endif
void* operator new(std::size_t n)
{
    countAllocation();
    if (void* p = std::malloc(n == 0 ? 1 : n))
        return p;
    throw std::bad_alloc();
}
void* operator new(std::size_t n, std::align_val_t a)
{
    countAllocation();
    void* p = nullptr;
    if (posix_memalign(&p, std::max(sizeof(void*), (std::size_t) a), n == 0 ? 1 : n) == 0)
        return p;
    throw std::bad_alloc();
}
void* operator new(std::size_t n, const std::nothrow_t&) noexcept { return countAllocation(), std::malloc(n == 0 ? 1 : n); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
static void escape(void* p) { asm volatile("" : : "r"(p) : "memory"); }

static int failures = 0;
#define CHECK(c) ((c) ? (void) 0 : (void) (++failures, std::cerr << "FAIL " << __LINE__ << ": " #c "\n"))

using Proc = SiltProcessor;
using Settings = std::vector<std::pair<const char*, float>>;
constexpr double pi = juce::MathConstants<double>::pi;
// Frozen independently from the processor: ID order, domains, defaults and exact choice strings.
struct Contract { const char* id; float lo, hi, def; const char* choices = nullptr; };
static const Contract contract[] {
    {"mix",0,100,100},{"output",-24,12,0},{"size",.5f,4000,51.78f},{"sizeSync",0,1,0,"sync,free"},
    {"density",1,64,24},{"position",0,4000,1200},{"scatter",0,100,45},{"pitch",-48,48,12},
    {"pitchMode",0,1,0,"shift,tape"},{"trigger",0,2,0,"clock,transient,sidechain"},{"reverbOn",0,1,1,""},
    {"reverb",0,100,30},{"oversample",0,3,1,"off,2x,4x,8x"},{"freeze",0,1,0,""},
    {"motion",0,100,0},{"texture",0,100,0},{"bloom",0,100,0},{"chaos",0,100,0},
    // 0.2, appended (version hint 2); every default is 0.1.0's sound.
    {"scan",-200,200,100},{"lock",0,2,0,"free,overlap,tone"},{"heads",0,3,0,"1,2,3,4"},
    {"harmony",0,5,0,"unison,fifth,octave,major,minor,held"},{"shape",0,100,0},{"tilt",-100,100,0},{"reverse",0,100,0},
    {"detune",0,24,0},{"scale",0,4,0,"free,chromatic,major,minor,pentatonic"},{"drop",0,100,0},{"repeat",0,100,0},
    {"band",0,100,0},{"centre",100,12000,1200},{"spectra",0,100,0},{"space",0,1,0,"pan,binaural"},{"width",0,200,100},
    {"feedback",0,95,0},{"filter",-100,100,0},{"sprayDir",0,2,0,"both,back,ahead"},{"onset",0,1,0,"ramp,full"}
};
static_assert(std::size(contract)==silt::numParams);
static void set(Proc& p, const char* id, float v) { auto* q = p.apvts.getParameter(id); q->setValueNotifyingHost(q->convertTo0to1(v)); }
static void set(Proc& p, const Settings& s) { for (auto& [id,v] : s) set(p,id,v); }
static float get(Proc& p, const char* id) { return p.apvts.getRawParameterValue(id)->load(); }
static std::vector<float> params(Proc& p)
{
    std::vector<float> out;
    for (const auto& c : contract) out.push_back(get(p,c.id));
    return out;
}
static void prepare(Proc& p, double fs = 48000, int block = 512, int in = 2, int side = 2, int out = 0)
{
    juce::AudioProcessor::BusesLayout l;
    l.inputBuses.add(juce::AudioChannelSet::canonicalChannelSet(in));
    l.inputBuses.add(side > 0 ? juce::AudioChannelSet::canonicalChannelSet(side) : juce::AudioChannelSet::disabled());
    l.outputBuses.add(juce::AudioChannelSet::canonicalChannelSet(out > 0 ? out : in));
    CHECK(p.setBusesLayout(l));
    p.setRateAndBufferSizeDetails(fs, block), p.prepareToPlay(fs, block);
}
struct Host : juce::AudioPlayHead
{
    double bpm = 120, ppq = 0;
    bool playing = true, known = true;
    juce::Optional<PositionInfo> getPosition() const override
    {
        if (!known) return {};
        PositionInfo pos;
        pos.setBpm(bpm), pos.setPpqPosition(ppq), pos.setIsPlaying(playing);
        return pos;
    }
};
static float noise(int i) { juce::uint32 h = (juce::uint32) i * 2654435761u; h = (h ^ (h >> 15)) * 0x2c1b3c6du, h ^= h >> 12; return (float) ((h >> 8) * 0x1p-23 - 1); }
static float pad(int i, double fs)
{
    const double t = i / fs;
    return (float) (0.1028 * (std::sin(2*pi*110*t) + std::sin(2*pi*138.5913*t+.4) + std::sin(2*pi*164.8138*t+.8)));
}
struct Event { int at; juce::MidiMessage m; };
struct Run
{
    std::function<float(int,int)> input = [](int i,int c) { return pad(i + c*47,48000); };
    std::function<float(int,int)> side = [](int i,int) { return i % 24000 < 128 ? .3f : 0.0f; };
    std::vector<Event> events;
    Host* host = nullptr;
    std::function<void(int)> perBlock;
    int offset = 0;
};
struct Audio
{
    std::vector<float> l, r;
    bool operator==(const Audio& other) const { return l == other.l && r == other.r; }
};
static Audio run(Proc& p, int samples, int block = 512, const Run& r = {})
{
    const int main = p.getMainBusNumInputChannels(), out = p.getMainBusNumOutputChannels();
    const int total = juce::jmax(p.getTotalNumInputChannels(),p.getTotalNumOutputChannels());
    juce::AudioBuffer<float> b(total,4096);
    juce::MidiBuffer midi;
    midi.ensureSize(65536);
    Audio a;
    a.l.reserve((size_t) samples), a.r.reserve((size_t) samples);
    juce::Random sizes(814);
    p.setPlayHead(r.host);
    for (int start=0,n=0; start<samples; start+=n)
    {
        n=juce::jmin(samples-start,block>0 ? block : 1+sizes.nextInt(4096));
        b.setSize(total,n,false,false,true);
        for (int c=0;c<total;++c) for (int i=0;i<n;++i)
            b.setSample(c,i,c<main ? r.input(start+r.offset+i,c) : r.side(start+r.offset+i,c-main));
        midi.clear();
        for (const auto& e:r.events) if (e.at>=start+r.offset && e.at<start+r.offset+n) midi.addEvent(e.m,e.at-start-r.offset);
        if(r.perBlock) r.perBlock(start+r.offset);
        onAudioThread=true;
        p.processBlock(b,midi);
        onAudioThread=false;
        if(r.host) r.host->ppq+=n*r.host->bpm/60/p.getSampleRate();
        for(int i=0;i<n;++i) a.l.push_back(b.getSample(0,i)), a.r.push_back(b.getSample(out-1,i));
    }
    p.setPlayHead(nullptr);
    return a;
}
static juce::MemoryBlock blob(const juce::String& xml)
{
    juce::MemoryOutputStream m;
    m.writeInt(0x21324356),m.writeInt((int) xml.getNumBytesAsUTF8()),m.write(xml.toRawUTF8(),xml.getNumBytesAsUTF8());
    return m.getMemoryBlock();
}
static void load(Proc& p,const juce::MemoryBlock& b) { p.setStateInformation(b.getData(),(int)b.getSize()); }
static juce::MemoryBlock save(Proc& p) { juce::MemoryBlock b; p.getStateInformation(b); return b; }
static void randomState(Proc& p,juce::Random& r)
{
    for(auto* param:p.getParameters()) param->setValueNotifyingHost(r.nextFloat());
}
static void testContract()
{
    CHECK(JucePlugin_PluginCode==0x53696c74 && JucePlugin_ManufacturerCode==0x44776b73);
    CHECK(juce::String(JUCE_STRINGIFY(JucePlugin_AUMainType))=="'aumf'");
    CHECK(juce::String(JUCE_STRINGIFY(JucePlugin_CFBundleIdentifier))=="com.dripworks.silt");
    CHECK(juce::String(JucePlugin_Name)=="silt" && juce::String(JucePlugin_Manufacturer)=="daveydidit");
    CHECK(juce::String(JucePlugin_VersionString)=="0.2.0" && juce::String(JucePlugin_Vst3Category)=="Fx");
    CHECK(!JucePlugin_IsSynth && JucePlugin_WantsMidiInput && !JucePlugin_ProducesMidiOutput);
    Proc p;
    CHECK(p.getParameters().size()==(int)std::size(contract) && p.stateVersion==2 && p.getNumPrograms()==20);
    for(int i=0;i<p.getParameters().size();++i)
    {
        const auto& c=contract[i];
        auto* q=dynamic_cast<juce::RangedAudioParameter*>(p.getParameters()[i]);
        CHECK(q && q->paramID==c.id && q->getVersionHint()==(i<silt::numParamsV1?1:2) && q->isAutomatable());
        CHECK(q->getNormalisableRange().start==c.lo && q->getNormalisableRange().end==c.hi && get(p,c.id)==c.def);
        if(auto* choice=dynamic_cast<juce::AudioParameterChoice*>(q)) CHECK(c.choices && choice->choices.joinIntoString(",")==c.choices);
        else if(dynamic_cast<juce::AudioParameterBool*>(q)) CHECK(c.choices && juce::String(c.choices).isEmpty());
        else CHECK(c.choices==nullptr);
    }
    // Hosts bind automation by ID, not index: VST3 and AU both use the 31-bit hash of the string ID, and AU lists
    // parameters by version hint, then ID, so appended (hint 2) parameters sort after all of these.
    constexpr juce::uint32 hostIds[]{108124,1141971201,3530753,846956092,1552717032,747804969,1911146174,106677056,544002307,
        1087591864,1882965841,1213131218,394773374,881080983,1079164854,729666843,93832707,94623616};
    static_assert(std::size(hostIds)==silt::numParamsV1);
    for(int i=0;i<(int)std::size(hostIds);++i)CHECK(juce::VST3ClientExtensions::convertJuceParameterId(contract[i].id,true)==hostIds[i]);
    // Mono -> stereo is intentional; stereo -> mono and surround are not supported.
    for(int in:{1,2,3}) for(int out:{1,2,3}) for(int side:{0,1,2,3})
    {
        juce::AudioProcessor::BusesLayout l;
        l.inputBuses.add(juce::AudioChannelSet::canonicalChannelSet(in));
        l.inputBuses.add(side ? juce::AudioChannelSet::canonicalChannelSet(side):juce::AudioChannelSet::disabled());
        l.outputBuses.add(juce::AudioChannelSet::canonicalChannelSet(out));
        CHECK(p.checkBusesLayoutSupported(l)==(in<=2 && out<=2 && out>=in && side<=2));
    }
    CHECK(!p.getBus(true,1)->isEnabled() && p.acceptsMidi() && p.hasEditor());
    CHECK(p.getTailLengthSeconds()>=8+4 && p.getName()=="silt"); // the capture span plus the longest free grain
}
static void testParameterText()
{
    Proc p;
    // The musical middle gets useful travel, while both size endpoints remain exact host values.
    auto* size=p.apvts.getParameter("size");
    CHECK(size->convertFrom0to1(0)==.5f && size->convertFrom0to1(1)==4000);
    CHECK(std::abs(size->convertFrom0to1(.5f)-100)<5);
    CHECK(p.apvts.getParameter("position")->convertFrom0to1(.5f)==2000);

    // Every parameter has finite, bounded host mappings and a useful text representation at its endpoints/middle.
    for(const auto& c:contract)
    {
        auto* q=p.apvts.getParameter(c.id);
        for(float n:{0.0f,.25f,.5f,.75f,1.0f})
        {
            const float plain=q->convertFrom0to1(n);
            CHECK(std::isfinite(plain) && plain>=c.lo && plain<=c.hi);
            CHECK(std::isfinite(q->convertTo0to1(plain)));
            CHECK(q->getText(n,0).isNotEmpty());
        }
        CHECK(q->getLabel().isEmpty()); // texts carry their units; VST3 hosts would append a label ("1/16 ms")
    }
    const auto parse=[&](const char* id,const juce::String& text)
    {
        auto* q=p.apvts.getParameter(id);
        q->setValueNotifyingHost(q->getValueForText(text));
    };
    // Sync shows what plays: whole grains per beat and a position in beats; typed values read back the same way.
    const auto shown=[&](const char* id){return p.apvts.getParameter(id)->getCurrentValueAsText();};
    CHECK(shown("density")=="12/beat" && shown("position")=="2.40 beats");
    parse("density","8/beat"),parse("position","3 beats");CHECK(get(p,"density")==16 && get(p,"position")==1500);
    set(p,"sizeSync",1);CHECK(shown("density")=="16/s" && shown("position")=="1.5 s");
    set(p,"sizeSync",1);parse("size","2.5 s");CHECK(get(p,"size")==2500);
    parse("size","0.5 ms");CHECK(get(p,"size")==.5f);
    parse("position","1.25 s");CHECK(get(p,"position")==1250);
    parse("pitch",juce::String::fromUTF8("−12 st"));CHECK(get(p,"pitch")==-12);
    parse("output","-3.5 dB");CHECK(get(p,"output")==-3.5f);
    set(p,"sizeSync",0);
    for(int k=0;k<silt::numNotes;++k)
    {
        const auto text=juce::String(silt::noteNames[k]);parse("size",text);CHECK(size->getCurrentValueAsText()==text);
    }
    parse("size","1/16");CHECK(size->getCurrentValueAsText()=="1/16");
}

static void testState()
{
    juce::Random rng(0x5117);
    for(int trial=0;trial<200;++trial)
    {
        Proc p,q;
        p.setCurrentProgram(trial%10),p.selectAb(1),randomState(p,rng),p.selectAb(0),randomState(p,rng);
        p.showMacros=trial%2!=0,p.editorWidth=512+(trial%3)*512;
        const auto original=params(p); const auto state=save(p);
        load(q,state);
        CHECK(params(q)==original && q.abSlot()==p.abSlot() && q.presetName()==p.presetName());
        CHECK(q.showMacros==p.showMacros && q.editorWidth==p.editorWidth);
        q.selectAb(1),p.selectAb(1);
        CHECK(params(q)==params(p) && q.presetName()==p.presetName());
        q.selectAb(0),p.selectAb(0);
        CHECK(params(q)==original && params(p)==original);
        prepare(p,48000,512,trial%2+1,trial%3,2),prepare(q,48000,512,trial%2+1,trial%3,2);
        Host a,b;
        a.bpm=b.bpm=137,a.ppq=b.ppq=2.131;
        Run r;
        r.events={{17,juce::MidiMessage::noteOn(1,67,(juce::uint8)100)},{701,juce::MidiMessage::controllerEvent(1,74,92)},
                  {1537,juce::MidiMessage::noteOff(1,67)}};
        r.host=&a;
        const auto x=run(p,4096,512,r);
        r.host=&b;
        CHECK(x==run(q,4096,512,r));
    }
    Proc p;
    set(p,"pitch",20);
    load(p,blob("<SILT stateVersion=\"1\"><PARAM id=\"size\" value=\"0.5\"/></SILT>"));
    for(const auto& c:contract) CHECK(get(p,c.id)==(juce::String(c.id)=="size"?.5f:c.def));
    load(p,blob("<SILT stateVersion=\"9000\"><PARAM id=\"pitch\" value=\"-7\"/><PARAM id=\"newThing\" value=\"8\"/></SILT>"));
    CHECK(get(p,"pitch")==-7 && get(p,"mix")==100);
    load(p,blob("<SILT><PARAM id=\"size\" value=\"nan\"/><PARAM id=\"output\" value=\"oops\"/></SILT>"));
    CHECK(get(p,"size")==51.78f && get(p,"output")==0);
    set(p,"pitch",11);
    for(const auto& xml:{"<OTHER/>","<SILT><PARAM id=\"pitch\" value=\"1\"/>","garbage"}) load(p,blob(xml)),CHECK(get(p,"pitch")==11);
    juce::MemoryBlock fixture;
    CHECK(juce::File(SILT_TEST_SOURCE_DIR "/state-v1.bin").loadFileAsData(fixture));
    load(p,fixture);
    CHECK(get(p,"size")==350 && get(p,"pitch")==-7 && p.presetName()=="v1 fixture" && p.abSlot()==1);
    const auto v1=params(p);
    Proc q;load(q,save(p));CHECK(params(q)==v1 && q.presetName()==p.presetName());
    CHECK(juce::String(static_cast<const char*>(save(p).getData())+8).contains("stateVersion=\"2\""));
    prepare(p),prepare(q);CHECK(run(p,48000)==run(q,48000)); // a v1 session saved as v2 sounds the same
    // A v1 A/B slot holds 18 values: the 0.2 parameters are padded with their defaults, never dropped.
    juce::StringArray slot;for(int k=0;k<silt::numParamsV1;++k)slot.add("0.25");
    Proc ab;load(ab,blob("<SILT stateVersion=\"1\" slotB=\"2;"+slot.joinIntoString(",")+"\"/>"));
    set(ab,{{"scan",-50},{"shape",80},{"centre",300}});ab.selectAb(1);
    CHECK(ab.apvts.getParameter("mix")->getValue()==.25f && ab.apvts.getParameter("chaos")->getValue()==.25f);
    for(int k=silt::numParamsV1;k<silt::numParams;++k)CHECK(get(ab,contract[k].id)==contract[k].def);
    ab.selectAb(0);CHECK(get(ab,"scan")==-50 && get(ab,"centre")==300);
}
static void testPresets()
{
    const auto folder=juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("silt-tests","",false);
    Proc p(folder),defaults;
    p.setCurrentProgram(4);CHECK(params(p)==params(defaults));
    const char* names[]{"sub dust","grit","glass","halo","tide","choir","tines","chime","rain","drift",
        "swarm","organ","cathedral","stutter","undertow","stretch","prism","orbit","snapshot","chorale"};
    static_assert(std::size(names)==silt::numPresets);
    for(int i=0;i<silt::numPresets;++i)
    {
        p.setCurrentProgram(i);CHECK(p.getProgramName(i)==names[i] && p.getCurrentProgram()==i);
        CHECK(get(p,"mix")==100 && get(p,"output")==(i==1?3:i==6 || i==16?6:i==11?2:i==17?-1.5f:0) && get(p,"freeze")==0);
    }
    p.setCurrentProgram(0),set(p,"pitch",7);
    const auto before=params(p);
    const auto file=p.saveUserPreset();CHECK(file.existsAsFile() && p.presetName()=="user 01");
    // A chosen user preset records like a factory one (gestures); a host restoring state sends none.
    struct Gestures : juce::AudioProcessorListener
    {
        int begun=0;
        void audioProcessorParameterChanged(juce::AudioProcessor*,int,float) override {}
        void audioProcessorChanged(juce::AudioProcessor*,const ChangeDetails&) override {}
        void audioProcessorParameterChangeGestureBegin(juce::AudioProcessor*,int) override {++begun;}
    } gestures;
    p.setCurrentProgram(silt::numPresets-1);p.addListener(&gestures);p.stepPreset(1);CHECK(params(p)==before && p.presetName()=="user 01" && gestures.begun>0);
    gestures.begun=0;{Proc r;r.addListener(&gestures);load(r,save(p));r.removeListener(&gestures);}CHECK(gestures.begun==0);
    p.removeListener(&gestures);
    p.stepPreset(1);CHECK(p.preset==0);p.stepPreset(-1);CHECK(params(p)==before);
    p.selectAb(1),set(p,"pitch",12),p.selectAb(0);CHECK(params(p)==before);
    Proc q(folder);load(q,save(p));q.selectAb(1);CHECK(get(q,"pitch")==12);
    p.setCurrentProgram(3),set(p,{{"mix",33},{"output",-6},{"texture",27},{"motion",9},{"bloom",19},{"chaos",11}});
    const auto old=params(p);
    p.randomise(42);const auto random=params(p);
    CHECK(get(p,"motion")==9 && get(p,"bloom")==19);
    CHECK(get(p,"mix")==33 && get(p,"output")==-6 && get(p,"texture")==27 && get(p,"chaos")==11 && old!=random);
    p.selectAb(1-p.abSlot());CHECK(params(p)==old);
    p.selectAb(1-p.abSlot());CHECK(params(p)==random);
    folder.deleteRecursively();
}
static Settings plain { {"trigger",0},{"sizeSync",1},{"pitch",0},{"motion",0},{"texture",0},{"bloom",0},{"chaos",0},
    {"scatter",0},{"reverb",0},{"reverbOn",0},{"density",2},{"position",180},{"oversample",0} };
static void testDryAndLifecycle()
{
    std::cout<<"Latency: rate | off | 2x | 4x | 8x\n";
    for(double fs:{44100.0,48000.0,96000.0})
    {
        std::cout<<fs;
        for(int quality=0;quality<4;++quality)
        {
            for(int block:{1,17,64,512,4096,0})
            {
                Proc p;set(p,{{"mix",0},{"output",12},{"oversample",(float)quality}});prepare(p,fs,512,1,2,2);
                Run r;r.input=[](int i,int){return i==37?1.0f:noise(i)*2;};
                const auto audio=run(p,8192,block,r);const int latency=p.getLatencySamples();
                CHECK(latency==0);
                for(size_t i=0;i<audio.l.size();++i)CHECK(audio.l[i]==r.input((int)i,0) && audio.r[i]==audio.l[i]);
                Run impulse;impulse.input=[](int i,int){return i==0?1.0f:0.0f;};
                prepare(p,fs,512,2,0);const auto y=run(p,512,17,impulse);
                CHECK(std::distance(y.l.begin(),std::max_element(y.l.begin(),y.l.end()))==latency);
                if(block==512)std::cout<<" | "<<latency;
                // Quality can change after prepare, with exact dry and no new storage.
                for(int next=0;next<4;++next)
                {
                    set(p,"oversample",(float)next);const auto changed=run(p,512,block,r);
                    for(size_t i=0;i<changed.l.size();++i)CHECK(changed.l[i]==r.input((int)i,0) && changed.r[i]==changed.l[i]);
                    CHECK(p.getLatencySamples()==0);
                }
            }
        }
        std::cout<<"\n";
    }
    // Live quality changes crossfade the feedback FIR paths while dry stays exact.
    Proc changing;set(changing,"mix",0);prepare(changing);
    Run sine;sine.input=[](int i,int){return (float)(.1*std::sin(2*pi*110*i/48000));};
    sine.perBlock=[&](int t){set(changing,"oversample",(float)((t/2400)%4));};
    const auto switched=run(changing,24000,64,sine);
    double step=0;for(size_t i=1;i<switched.l.size();++i)step=std::max(step,(double)std::abs(switched.l[i]-switched.l[i-1]));CHECK(step<.02);
    Proc p,q;set(p,plain),set(q,plain);prepare(p),prepare(q);
    const auto reference=run(p,48000,512);
    for(int block:{0,1,17,64,4096}){prepare(q);CHECK(reference==run(q,48000,block));}
    for(double fs:{96000.0,44100.0,48000.0})
    {
        prepare(p,fs,4096);Run silent;silent.input=[](int,int){return 0.0f;};silent.side=silent.input;
        for(float v:run(p,12000,0,silent).l)CHECK(v==0 && std::fpclassify(v)!=FP_SUBNORMAL);
    }
    p.setCurrentProgram(4),q.setCurrentProgram(4);prepare(p),prepare(q);
    Host ha,hb;ha.bpm=hb.bpm=127;
    Run performance;performance.events={{37,juce::MidiMessage::controllerEvent(1,1,93)},
        {701,juce::MidiMessage::controllerEvent(1,2,74)},{1350,juce::MidiMessage::controllerEvent(1,3,64)},
        {5000,juce::MidiMessage::controllerEvent(1,4,80)},{347,juce::MidiMessage::noteOn(1,64,(juce::uint8)100)}};
    performance.host=&ha;const auto fixed=run(p,48000,512,performance);
    performance.host=&hb;const auto variable=run(q,48000,0,performance);
    double error=0;for(size_t i=0;i<fixed.l.size();++i)error=std::max(error,(double)std::abs(fixed.l[i]-variable.l[i]));
    CHECK(error<1e-6); // host PPQ accumulated in different blocks may differ by an ulp at a grain boundary
    juce::Random rng(39);
    for(int in:{1,2})for(int sc:{0,1,2})
    {
        prepare(p,48000,512,in,sc,2);Run r;r.perBlock=[&](int){randomState(p,rng);};
        for(float v:run(p,16000,0,r).l)CHECK(std::isfinite(v) && std::fpclassify(v)!=FP_SUBNORMAL);
    }
    juce::AudioBuffer<float> buffer(4,512);buffer.clear();juce::MidiBuffer midi;
    char sysex[256]{};midi.addEvent(juce::MidiMessage::createSysExMessage(sysex,256),0);
    onAudioThread=true;p.processBlock(buffer,midi);onAudioThread=false;CHECK(audioAllocations==0);
}
static void testTimingAndMidi()
{
    for(double fs:{44100.0,48000.0,96000.0})
    {
        Proc p;set(p,plain),set(p,{{"sizeSync",0},{"density",2}}),prepare(p,fs);
        Host host;Run r;r.host=&host;
        std::vector<juce::int64> times;juce::int64 last=-1;
        r.perBlock=[&](int){const auto fire=p.feed.lastFire.load();if(fire!=last && fire>=0)times.push_back(fire),last=fire;};
        run(p,(int)fs+2,1,r);CHECK(times.size()==3);
        for(size_t k=0;k<times.size();++k)CHECK(std::abs(times[k]-(juce::int64)std::llround(k*fs/2))<=1);
        host.ppq=8;run(p,1,1,r);CHECK(std::abs(p.feed.lastFire.load()-(juce::int64)fs-2)<=1);
        for(float size:{.5f,4000.0f})
        {
            set(p,plain),set(p,{{"trigger",1},{"size",size}}),prepare(p,fs);
            Run note;note.input=[](int,int){return 0.0f;};note.events={{7,juce::MidiMessage::noteOn(1,60,(juce::uint8)127)}};run(p,8,8,note);
            CHECK(p.feed.lastFire==7 && std::abs(p.feed.lastLength-(int)(size*fs/1000))<=1);
        }
        for(int index=0;index<silt::numNotes;++index)
        {
            set(p,plain),set(p,{{"trigger",1},{"sizeSync",0}});
            auto* size=p.apvts.getParameter("size");size->setValueNotifyingHost(size->getValueForText(silt::noteNames[index]));
            prepare(p,fs);Run note;note.input=[](int,int){return 0.0f;};note.host=&host;note.events={{0,juce::MidiMessage::noteOn(1,60,(juce::uint8)127)}};run(p,1,1,note);
            const double seconds=2.0*silt::noteNum[index]/silt::noteDen[index];
            CHECK(std::abs(p.feed.lastLength-(int)(seconds*fs))<=1);
        }
        set(p,plain),set(p,{{"sizeSync",0},{"density",2}}),prepare(p,fs);
        host.playing=false;Run stopped;stopped.host=&host;run(p,(int)fs,512,stopped);CHECK(p.feed.fired==2);
        prepare(p,fs);host.known=false;run(p,(int)fs,512,stopped);CHECK(p.feed.fired==2);
    }
    Proc p;set(p,plain),set(p,"trigger",1),prepare(p);
    Run r;r.events={{37,juce::MidiMessage::noteOn(1,67,(juce::uint8)100)}};run(p,38,38,r);
    CHECK(p.feed.lastFire==37 && p.feed.lastPitch==7 && p.feed.note==67);
    r.offset=38;r.events={{50,juce::MidiMessage::noteOn(1,72,(juce::uint8)60)}};run(p,13,13,r);CHECK(p.feed.lastPitch==12);
    r.offset=51;r.events={{52,juce::MidiMessage::noteOff(1,72)}};run(p,3,3,r);
    set(p,"trigger",0),set(p,"density",64);run(p,1000,1);CHECK(p.feed.lastPitch==7 && p.feed.note==72);
    r.offset=0;r.events={{0,juce::MidiMessage::allNotesOff(1)}};run(p,1000,1,r);CHECK(p.feed.lastPitch==0);
    // MIDI always transposes clock grains and triggers immediately, with exact sample-offset velocity.
    Proc loud,soft;set(loud,plain),set(soft,plain);set(loud,"trigger",2),set(soft,"trigger",2);prepare(loud),prepare(soft);
    Run hi,lo;hi.side=lo.side=[](int,int){return 0.0f;};
    hi.events={{12000,juce::MidiMessage::noteOn(1,60,(juce::uint8)127)}};lo.events={{12000,juce::MidiMessage::noteOn(1,60,(juce::uint8)64)}};
    const auto a=run(loud,30000,512,hi),b=run(soft,30000,512,lo);
    double error=0;for(size_t i=0;i<a.l.size();++i)error=std::max(error,(double)std::abs(b.l[i]-a.l[i]*64/127));CHECK(error<1e-6);
    for(int cc:{1,2,3,4})
    {
        Proc base,mod;prepare(base),prepare(mod);const auto before=params(mod);
        Run changed;changed.events={{20000,juce::MidiMessage::controllerEvent(1,cc,127)}};
        CHECK(!(run(base,144000)==run(mod,144000,512,changed)) && params(mod)==before);
    }
    for(int mode:{0,1})
    {
        set(p,plain),set(p,{{"trigger",1},{"size",200},{"pitch",12},{"pitchMode",(float)mode}}),prepare(p);
        Run note;note.input=[](int,int){return 0.0f;};note.events={{0,juce::MidiMessage::noteOn(1,60,(juce::uint8)127)}};run(p,1,1,note);
        CHECK(p.feed.lastLength==(mode==1?4800:9600));
    }
    set(p,plain),set(p,{{"chaos",100},{"density",64}}),prepare(p);
    Run notes;notes.perBlock=[&](int){if(p.feed.fired>0){const float st=p.feed.lastPitch;CHECK(std::abs(st-std::round(st))<1e-6f);}};run(p,48000,64,notes);
    for(double fs:{44100.0,48000.0,96000.0})for(double bpm:{60.0,90.0,180.0})for(int hold:{0,1})
    {
        set(p,plain),set(p,{{"trigger",2},{"sizeSync",0},{"size",4000},{"freeze",(float)hold}}),prepare(p,fs);
        Host host;host.bpm=bpm;Run note;note.input=[](int,int){return 0.0f;};note.host=&host;note.side=[](int,int){return 0.0f;};note.events={{1,juce::MidiMessage::noteOn(1,60,(juce::uint8)127)}};
        run(p,2,2,note);CHECK(std::abs(p.feed.lastLength-(int)(480/bpm*fs))<=1);
    }
}
static void testRatios()
{
    Proc p;set(p,plain),set(p,{{"sizeSync",0},{"density",24},{"position",1200}});
    float centres[3]{};
    int k=0;for(double bpm:{60.0,120.0,240.0})
    {
        prepare(p);Host host;host.bpm=bpm;Run r;r.host=&host;run(p,1,1,r);centres[k++]=p.feed.centre;
    }
    CHECK(std::abs(centres[0]-2*centres[1])<1e-6 && std::abs(centres[1]-2*centres[2])<1e-6);
}
static void testOnsetsAndSource()
{
    for(int trigger:{1,2})
    {
        Proc p;set(p,plain),set(p,"trigger",(float)trigger),prepare(p);
        Run r;r.input=[](int i,int){return i%12000<100?.3f:0.0f;};r.side=[](int,int){return 0.0f;};
        run(p,36000,17,r);CHECK(p.feed.fired==(trigger==2?0:3));
        prepare(p);r.side=r.input;r.input=[](int,int){return 0.0f;};run(p,36000,17,r);CHECK(p.feed.fired==(trigger==2?3:0));
    }
    Proc p;set(p,plain),set(p,"trigger",1),prepare(p);
    Run steady;steady.input=[](int,int){return .2f;};run(p,48000,17,steady);CHECK(p.feed.fired==1);
    // Sidechain triggers main capture, never replaces the captured source or leaks into the output.
    for(int main:{1,2})for(int sc:{0,1,2})
    {
        set(p,plain),set(p,"trigger",2),prepare(p,48000,512,main,sc,2);
        Run side;side.input=[](int,int){return 0.0f;};side.side=[](int i,int){return i%12000<100?.3f:0.0f;};
        for(float v:run(p,48000,64,side).l)CHECK(v==0);
        CHECK(sc==0?p.feed.fired==0:p.feed.fired==4);
    }
}
static double maxStep(const Audio& a,size_t begin=1)
{double m=0;for(size_t i=juce::jmax<size_t>(1,begin);i<a.l.size();++i)m=juce::jmax(m,(double)std::abs(a.l[i]-a.l[i-1]));return m;}
static void testFreezeAndWindows()
{
    for(int shape=0;shape<silt::numWindows;++shape)for(double skew:{0.0,.5,1.0})
    {
        CHECK(std::abs(silt::window(shape,0,skew))<1e-12 && std::abs(silt::window(shape,1,skew))<1e-12);
        for(int k=0;k<=1000;++k)CHECK(silt::window(shape,k/1000.0,skew)>=0 && silt::window(shape,k/1000.0,skew)<=1);
    }
    Proc p,reference;
    for(auto* q:{&p,&reference})set(*q,plain),set(*q,{{"size",600},{"density",24},{"bloom",50},{"scatter",25}}),prepare(*q);
    Run r;r.input=[](int i,int){return (float)(.15*std::sin(2*pi*110*i/48000));};
    const int warm=201600; // freeze holds once 4 s are captured
    run(p,warm,64,r),run(reference,warm,64,r);
    r.offset=warm;const auto live=run(reference,72000,64,r);
    r.perBlock=[&](int){set(p,"freeze",1);};const auto held=run(p,24000,64,r);
    const double bound=2*maxStep(live);
    std::cout<<"Freeze steps: live "<<maxStep(live)<<" held "<<maxStep(held);
    CHECK(p.feed.frozen && maxStep(held)<bound);
    const int head=p.feed.at;r.offset=warm+24000;run(p,24000,64,r);CHECK(p.feed.at==head);
    r.offset=warm+48000;r.perBlock=[&](int){set(p,"freeze",0);};const auto released=run(p,24000,64,r);
    std::cout<<" released "<<maxStep(released)<<"\n";
    CHECK(maxStep(released)<bound && !p.feed.frozen);
    set(p,plain),set(p,{{"trigger",2},{"size",4000},{"pitch",-48},{"pitchMode",1}}),prepare(p,96000,4096);
    Run longGrain;longGrain.input=[](int,int){return 0.0f;};longGrain.side=longGrain.input;
    longGrain.events={{0,juce::MidiMessage::noteOn(1,60,(juce::uint8)127)}};
    run(p,16*96000,4096,longGrain);CHECK(std::abs(p.feed.env[0]-.5f)<2e-6f);
    longGrain.offset=16*96000;run(p,32*96000,4096,longGrain);CHECK(std::abs(p.feed.env[0]-.5f)<2e-6f);
    longGrain.offset=48*96000;run(p,16*96000,4096,longGrain);CHECK(p.feed.env[0]==0);
}

// 0.2 grain controls, each heard or seen in the grain events as designed. Their defaults are 0.1.0's (testGoldens).
static void testGrainFeatures()
{
    // One grain over a click a quarter into its 9600-sample read: forward it sounds a quarter in, reversed three quarters.
    for(float reverse:{0.0f,100.0f})
    {
        Proc p;set(p,plain),set(p,{{"trigger",2},{"pitchMode",1},{"size",200},{"position",500},{"reverse",reverse}}),prepare(p);
        Run r;r.input=[](int i,int){return i==26400?.5f:0.0f;};r.side=[](int,int){return 0.0f;};
        r.events={{48000,juce::MidiMessage::noteOn(1,60,(juce::uint8)127)}};
        const auto a=run(p,57600,512,r);
        size_t at=48000;for(size_t i=at;i<a.l.size();++i)if(std::abs(a.l[i])>std::abs(a.l[at]))at=i;
        CHECK(std::abs((int)at-48000-(reverse>0?7200:2400))<=2);
    }
    // The window of one 4800-sample grain: tilt moves its peak; shape goes bell -> flat top -> rectangle (1 ms edges).
    const auto window=[](const Settings& s)
    {
        Proc p;set(p,plain),set(p,{{"trigger",2},{"size",100}}),set(p,s),prepare(p);
        Run r;r.side=[](int,int){return 0.0f;};r.events={{0,juce::MidiMessage::noteOn(1,60,(juce::uint8)127)}};
        std::vector<float> w;r.perBlock=[&](int t){if(t>0)w.push_back(p.feed.env[0]);};
        run(p,4800,1,r);
        for(float x:w)CHECK(x>0 && x<=1);
        return w;
    };
    const auto peak=[](const std::vector<float>& w){return (int)std::distance(w.begin(),std::max_element(w.begin(),w.end()));};
    const auto flat=[](const std::vector<float>& w){return std::count_if(w.begin(),w.end(),[](float x){return x>.999f;});};
    const auto bell=window({}),pluck=window({{"tilt",-100}}),swell=window({{"tilt",100}}),top=window({{"shape",50}}),rect=window({{"shape",100}});
    std::cout<<"Window peaks: bell "<<peak(bell)<<" pluck "<<peak(pluck)<<" swell "<<peak(swell)<<"; flat samples "<<flat(bell)<<" / "<<flat(top)<<" / "<<flat(rect)<<"\n";
    CHECK(std::abs(peak(bell)-2399)<=2 && std::abs(peak(pluck)-95)<=2 && std::abs(peak(swell)-4703)<=2);
    CHECK(flat(bell)<200 && flat(top)>600 && flat(top)<1000 && flat(rect)>=4799-2*48-2 && rect.front()<.05f);
    // Grain events: pitch, fire time and where each grain starts reading (capture frames).
    struct Grain{float pitch;juce::int64 frame;double start,back;};
    const auto grains=[](const Settings& s,int samples=48000)
    {
        Proc p;set(p,plain),set(p,{{"density",64},{"size",20}}),set(p,s),prepare(p);run(p,samples);
        std::vector<Grain> out;
        for(juce::int64 i=0;i<p.feed.events;++i)
        {
            auto& e=p.feed.history[(size_t)i%silt::Feed::historySize];
            out.push_back({e.pitch,e.frame,(double)e.captureHead-(double)e.back*48000,(double)e.back});
        }
        return out;
    };
    const auto inScale=[](float st,int mask){const int n=(int)std::lround(st);return std::abs(st-(float)n)<1e-4f && ((mask>>((n%12+12)%12))&1);};
    for(auto [scale,mask]:{std::pair{0,0},{1,0xfff},{2,0xab5},{3,0x5ad},{4,0x295}})
    {
        float lo=0,hi=0;bool fractional=false;
        for(const auto& g:grains({{"detune",12},{"scale",(float)scale}}))
        {
            lo=std::min(lo,g.pitch),hi=std::max(hi,g.pitch),fractional|=std::abs(g.pitch-std::round(g.pitch))>.01f;
            CHECK(std::abs(g.pitch)<=12.0f+(scale?2:0));
            if(scale)CHECK(inScale(g.pitch,mask));
        }
        CHECK(hi-lo>12 && fractional==(scale==0));
        // Chaos alone keeps 0.1.0's semitone rounding in free, and snaps to the chosen scale otherwise.
        for(const auto& g:grains({{"chaos",40},{"scale",(float)scale}}))CHECK(inScale(g.pitch,scale?mask:0xfff));
    }
    for(const auto& g:grains({{"pitch",3.5f},{"scale",2}}))CHECK(g.pitch==3.5f); // no spread: no snap
    // Drop silences a share of the clock's grains; the rest keep their ticks.
    const auto all=grains({}),half=grains({{"drop",50}});
    CHECK(grains({{"drop",100}}).empty() && half.size()>all.size()/4 && half.size()<all.size()*3/4);
    for(const auto& g:half)CHECK(std::any_of(all.begin(),all.end(),[&](const Grain& a){return a.frame==g.frame;}));
    // Repeat re-reads the previous grain's audio: at 100% every grain starts where the first one did.
    const auto starts=[&](float repeat){std::vector<double> x;for(const auto& g:grains({{"scatter",45},{"position",1200},{"density",24},{"repeat",repeat}},72000))x.push_back(g.start);return x;};
    const auto fresh=starts(0),repeated=starts(100);
    CHECK(fresh.size()>30 && std::abs(*std::max_element(fresh.begin(),fresh.end())-*std::min_element(fresh.begin(),fresh.end())-72000)<48000);
    for(double x:repeated)CHECK(std::abs(x-repeated.front())<.5);
    // Spray direction: back only reaches into older audio, ahead only newer; both does each.
    for(int dir:{0,1,2})
    {
        int older=0,newer=0;
        for(const auto& g:grains({{"scatter",60},{"position",1000},{"sprayDir",(float)dir}}))older+=g.back>1.0001,newer+=g.back<.9999;
        CHECK(older+newer>50 && (dir==1?newer==0:dir==2?older==0:older>10 && newer>10));
    }
    // Scan moves the read anchor (the cloud's centre) through history; 100% holds 0.1.0's fixed lag.
    const auto centres=[](float scan,bool freeze)
    {
        Proc p;set(p,plain),set(p,{{"position",1000},{"scan",scan},{"freeze",freeze?1.0f:0.0f}}),prepare(p);
        std::vector<double> c;Run r;r.perBlock=[&](int t){if(t>0)c.push_back(p.feed.centre);};
        run(p,10*48000,4800,r);return c; // c[k]: after (k + 1) / 10 s
    };
    for(float scan:{100.0f,0.0f,-100.0f,200.0f})
    {
        const auto c=centres(scan,false);
        for(size_t k=0;k<c.size();++k)
        {
            const double t=(double)(k+1)/10,lag=1+(1-scan/100)*t,cycle=std::fmod(lag-1,7); // past 8 s: back to 1 s
            CHECK(std::abs(c[k]-(scan>100?std::max(.02,lag):1+cycle))<1e-3 || cycle<.01 || cycle>6.99);
        }
    }
    const auto held=centres(200,true); // live it rides 20 ms behind; frozen (after 4 s) it moves on, wrapping in the held 8 s
    CHECK(std::abs(held[30]-.02)<1e-3 && *std::max_element(held.begin()+41,held.end())>7.5);
    for(size_t k=41;k<held.size();++k)CHECK(held[k]>=.02-1e-6 && held[k]<=7.98+1e-6 && (held[k]<held[k-1] || held[k]>7.5));
}

// Three stationary, zero-mean musical programs, each measured and normalised to -18 dBFS RMS before use.
struct Program
{
    std::vector<float> data;
    explicit Program(int kind,int samples=192000)
    {
        data.resize((size_t)samples);double energy=0,mean=0;
        for(int i=0;i<samples;++i)
        {
            const double t=i/48000.0;
            const double bassNotes[]{55,65.4064,73.4162,82.4069};
            const double phase=t*bassNotes[(i/24000)%4];
            double x=0;
            if(kind==0) for(int h=1;h<=12;++h) x+=std::sin(2*pi*h*phase)/h;
            if(kind==1) x=pad(i,48000);
            if(kind==2) {const double u=std::fmod(t,.25);for(int h=1;h<=5;++h) x+=std::sin(2*pi*110*h*t)*std::exp(-u*(8+2*h))/h;}
            data[(size_t)i]=(float)x,mean+=x;
        }
        mean/=samples;
        for(float& x:data) x-=(float)mean,energy+=(double)x*x;
        const double gain=std::pow(10.0,-18/20.0)/std::sqrt(energy/samples);
        for(float& x:data) x*=(float)gain;
    }
    Run input() const
    {
        Run r;r.input=[this](int i,int){return data[(size_t)i%data.size()];};r.side=r.input;return r;
    }
};
struct Level
{
    double peak=0,rms=0,dc=0;
    explicit Level(const Audio& a)
    {
        double sum[2]{},energy=0;
        for(size_t i=0;i<a.l.size();++i)
        {
            CHECK(std::isfinite(a.l[i]) && std::isfinite(a.r[i]));
            peak=std::max({peak,(double)std::abs(a.l[i]),(double)std::abs(a.r[i])});
            energy+=(double)a.l[i]*a.l[i]+(double)a.r[i]*a.r[i],sum[0]+=a.l[i],sum[1]+=a.r[i];
        }
        rms=std::sqrt(energy/(2*a.l.size()));dc=std::max(std::abs(sum[0]),std::abs(sum[1]))/a.l.size();
    }
    double rmsDb() const {return 20*std::log10(rms+1e-30);}
    double peakDb() const {return 20*std::log10(peak+1e-30);}
    void check() const
    {
        if(rmsDb()>-14 || peakDb()>-.5 || dc>=.001) std::cerr<<"level failure rms "<<rmsDb()<<" peak "<<peakDb()<<" dc "<<dc<<"\n";
        CHECK(rmsDb()<=-14 && peakDb()<=-.5 && dc<.001);
    }
};
// Audible controls and capture behaviour, rather than tests of the implementation's arithmetic.
// 0.2 phase 2: heads and harmony, held-chord heads, the locked clocks, stealing and the full onset.
static void testHeadsAndLock()
{
    struct Fired{float pitch;juce::int64 frame;};
    const auto fired=[](Proc& p){std::vector<Fired> out;for(juce::int64 i=0;i<p.feed.events;++i){auto& e=p.feed.history[(size_t)i%silt::Feed::historySize];out.push_back({e.pitch,e.frame});}return out;};
    const auto voices=[](Proc& p){int n=0;for(auto& e:p.feed.env)n+=e.load()>0;return n;};
    // Heads deal the shared clock round-robin; each head adds its harmony interval (major: 0 4 7 12).
    {
        Proc p;set(p,plain),set(p,{{"density",64},{"size",20},{"heads",3},{"harmony",3}}),prepare(p);run(p,24000); // 4 heads
        const auto f=fired(p);CHECK(f.size()>20);
        const float major[]{0,4,7,12};int first=0;while(f[0].pitch!=major[first])++first;
        for(size_t k=0;k<f.size();++k)CHECK(f[k].pitch==major[((size_t)first+k)%4]);
    }
    // Held: each held note keeps its head; a middle note-off renumbers nothing; a new note takes the free head,
    // and with every head busy, the oldest note's.
    {
        Proc p;set(p,plain),set(p,{{"density",64},{"size",20},{"heads",2},{"harmony",5},{"trigger",2}}),prepare(p); // heads is a choice: index 2 = 3 heads
        Run r;r.side=[](int,int){return 0.0f;};
        r.events={{100,juce::MidiMessage::noteOn(1,60,(juce::uint8)100)},{200,juce::MidiMessage::noteOn(1,64,(juce::uint8)100)},
            {300,juce::MidiMessage::noteOn(1,67,(juce::uint8)100)},{400,juce::MidiMessage::noteOff(1,64)},{500,juce::MidiMessage::noteOn(1,72,(juce::uint8)100)},
            {600,juce::MidiMessage::noteOn(1,74,(juce::uint8)100)}};
        run(p,1000,64,r);
        std::vector<float> pitches;for(const auto& f:fired(p))pitches.push_back(f.pitch);
        CHECK((pitches==std::vector<float>{0,4,7,12,14})); // each note-on's grain lands on its own head
        set(p,"trigger",0);run(p,48000,64,{}); // the clock then cycles the heads in head order: 74 (was 60's), 72 (64's), 67
        const auto f=fired(p);CHECK(f.size()>30);
        int at=(int)f.size()-3;while(f[(size_t)at].pitch!=14)--at;
        const float order[]{14,12,7};
        for(int k=at;k<(int)f.size();++k)CHECK(f[(size_t)k].pitch==order[(k-at)%3]);
    }
    // Overlap reads density as x1-x16 grains per size: 64 -> 16 per 100 ms. Tone: one grain per period of the note.
    {
        Proc p;set(p,plain),set(p,{{"lock",1},{"density",64},{"size",100}}),prepare(p);run(p,48000);
        std::cout<<"Overlap x16 / 100 ms: "<<p.feed.fired.load()<<" grains/s\n";CHECK(std::abs(p.feed.fired-160)<=2);
    }
    for(int note:{-1,69})
    {
        Proc p;set(p,plain),set(p,{{"lock",2},{"size",4000},{"pitch",note<0?0.0f:-12.0f}}),prepare(p);
        Run r;if(note>0)r.events={{0,juce::MidiMessage::noteOn(1,note,(juce::uint8)100)}};
        run(p,48000,512,r);const double f0=note<0?261.6255653:440;
        std::cout<<"Tone lock "<<f0<<" Hz: "<<p.feed.fired.load()<<" grains/s, "<<voices(p)<<" voices\n";
        CHECK(std::abs(p.feed.fired-f0)<=2+(note>0) && voices(p)<=silt::maxGrains); // a 4 s size shrinks to 120 periods
    }
    // Stealing (locked clocks): a full onset over a busy tone cloud (120 voices) takes the 8 free voices, then steals the
    // 16 oldest into the tail voices; with those all releasing, the rest of the burst is skipped (never a click).
    {
        Proc p;set(p,plain),set(p,{{"lock",2},{"size",4000},{"pitch",12},{"onset",1}}),prepare(p);
        Run r;r.events={{24000,juce::MidiMessage::noteOn(1,72,(juce::uint8)100)}};
        const auto a=run(p,30000,512,r);
        const int ticks=(int)(30000*523.2511306/48000);
        std::cout<<"Stealing: "<<p.feed.fired.load()<<" grains for "<<ticks<<" ticks + a 64-grain onset\n";
        CHECK(std::abs(p.feed.fired-(ticks+8+16))<=1);
        float jump=0;for(size_t i=24001;i<a.l.size();++i)jump=std::max(jump,std::abs(a.l[i]-a.l[i-1]));CHECK(jump<.1f);
        Proc free;set(free,plain),set(free,{{"size",4000},{"density",64},{"onset",1},{"trigger",2}}),prepare(free);
        Run burst;burst.side=[](int,int){return 0.0f;};for(int k=0;k<3;++k)burst.events.push_back({100+k*100,juce::MidiMessage::noteOn(1,60+k,(juce::uint8)100)});
        run(free,1000,512,burst);CHECK(free.feed.fired==silt::maxGrains); // free lock keeps 0.1.0's skip when full
    }
    // Onset: ramp starts one grain per trigger; full back-dates a whole size of them (500 ms / 41.7 ms -> 12),
    // on a note-on and when freeze releases.
    for(int onset:{0,1})
    {
        Proc p;set(p,plain),set(p,{{"size",500},{"density",24},{"trigger",2},{"onset",(float)onset}}),prepare(p);
        Run r;r.side=[](int,int){return 0.0f;};r.events={{1000,juce::MidiMessage::noteOn(1,60,(juce::uint8)100)}};
        run(p,1512,512,r);CHECK(voices(p)==(onset?12:1));
        r.events.clear();run(p,48000*5,512,r);set(p,"freeze",1);run(p,48000,512,r);const int before=p.feed.fired;
        set(p,"freeze",0);run(p,512,512,r);CHECK(p.feed.fired-before==(onset?12:0));
    }
}
// 0.2 phase 3: colour (band, centre, spectra), binaural space, width, the one-knob filter and the feedback amount.
static void testColourAndSpace()
{
    const auto power=[](const std::vector<float>& x,double hz)
    {double re=0,im=0;for(size_t i=x.size()/2;i<x.size();++i)re+=x[i]*std::cos(2*pi*hz*(double)i/48000),im+=x[i]*std::sin(2*pi*hz*(double)i/48000);return re*re+im*im;};
    // 4 kHz against 500 Hz (dB) through the cloud: band narrows to one critical band at the centre; spectra spreads the centres.
    const auto tilt=[&](const Settings& s)
    {
        Proc p;set(p,plain),set(p,{{"density",24},{"size",200}}),set(p,s),prepare(p);
        Run r;r.input=[](int i,int){return (float)(.1*std::sin(2*pi*500*i/48000)+.1*std::sin(2*pi*4000*i/48000));};
        const auto a=run(p,96000,512,r);return 10*std::log10(power(a.l,4000)/power(a.l,500));
    };
    const double flat=tilt({}),low=tilt({{"band",100},{"centre",500}}),high=tilt({{"band",100},{"centre",4000}}),wide=tilt({{"band",1},{"centre",500}}),
        spread=tilt({{"band",100},{"centre",500},{"spectra",100}}),lp=tilt({{"filter",-100}}),hp=tilt({{"filter",100}}),half=tilt({{"filter",-50}});
    std::cout<<"4 kHz / 500 Hz: flat "<<flat<<" band@500 "<<low<<" band@4k "<<high<<" wide "<<wide<<" spectra "<<spread<<" filter -100 "<<lp<<" -50 "<<half<<" +100 "<<hp<<" dB\n";
    CHECK(std::abs(flat)<3 && low<flat-25 && high>flat+25 && std::abs(wide-flat)<4 && spread>low+10);
    CHECK(lp<flat-25 && hp>flat+25 && half<flat-6 && half>lp+6);
    // Binaural: the left lane's first grain reads one click. The right ear hears it later (up to 0.65 ms), quieter; pan has no delay.
    for(int space:{0,1})
    {
        Proc p;set(p,plain),set(p,{{"trigger",2},{"pitchMode",1},{"size",200},{"position",500},{"space",(float)space}}),prepare(p);
        Run r;r.input=[](int i,int){return i==26400?.5f:0.0f;};r.side=[](int,int){return 0.0f;};
        r.events={{48000,juce::MidiMessage::noteOn(1,60,(juce::uint8)127)}};
        const auto a=run(p,57600,512,r);
        double moment[2]{},energy[2]{};
        for(size_t i=50300;i<50500;++i)for(int c=0;c<2;++c){const double x=c?a.r[i]:a.l[i];moment[c]+=(double)i*x*x,energy[c]+=x*x;}
        const double lag=moment[1]/energy[1]-moment[0]/energy[0];
        std::cout<<(space?"Binaural":"Pan")<<" click: right - left "<<lag<<" samples, "<<10*std::log10(energy[1]/energy[0])<<" dB\n";
        CHECK(energy[1]<energy[0] && (space?lag>5 && lag<32:std::abs(lag)<.01));
    }
    // Width: 0 is mono (identical channels); 200% at least triples the side's share of the energy.
    const auto side=[](float width,bool& mono)
    {
        Proc p;set(p,plain),set(p,{{"density",24},{"size",200},{"scatter",40},{"width",width}}),prepare(p);
        Run r;r.input=[](int i,int c){return (float)(.15*std::sin(2*pi*(c?300:700)*i/48000));};
        const auto a=run(p,96000,512,r);mono=a.l==a.r;
        double m=0,s=0;for(size_t i=48000;i<a.l.size();++i)m+=std::pow(a.l[i]+a.r[i],2),s+=std::pow(a.l[i]-a.r[i],2);return s/m;
    };
    bool mono[3]{};const double narrow=side(0,mono[0]),normal=side(100,mono[1]),wider=side(200,mono[2]);
    std::cout<<"Side / mid energy: width 0 "<<narrow<<" 100 "<<normal<<" 200 "<<wider<<"\n";
    CHECK(mono[0] && !mono[1] && narrow==0 && wider>3*normal);
    // The filter engages, flips and bypasses mid-stream without a step beyond the cloud's own.
    {
        Proc p,q;for(auto* x:{&p,&q})set(*x,plain),set(*x,{{"density",24},{"size",200}}),prepare(*x);
        Run r;r.input=[](int i,int){return (float)(.2*std::sin(2*pi*110*i/48000));};
        r.perBlock=[&](int t){set(p,"filter",t<48000?0.0f:t<72000?-20.0f:t<96000?20.0f:0.0f);};
        const auto switched=run(p,120000,512,r),steady=run(q,120000,512,r);
        std::cout<<"Filter switching step "<<maxStep(switched,24000)<<" (steady "<<maxStep(steady,24000)<<")\n";
        CHECK(maxStep(switched,24000)<1.5*maxStep(steady,24000));
    }
    // Feedback adds to bloom's loop: a longer tail after the input stops, still bounded by the level guard.
    double tail[2]{};
    for(int k=0;k<2;++k)
    {
        Proc p;set(p,plain),set(p,{{"density",24},{"size",200},{"feedback",k?95.0f:0.0f}}),prepare(p);
        const Program pad(1);Run r;r.input=[&](int i,int){return i<96000?pad.data[(size_t)i]:0.0f;};r.side=r.input;
        const auto a=run(p,4*48000,512,r);Level(a).check();
        for(size_t i=120000;i<168000;++i)tail[k]+=(double)a.l[i]*a.l[i];
    }
    std::cout<<"Feedback tail 0.5-1.5 s after the input: "<<10*std::log10(tail[0]/48000+1e-30)<<" / "<<10*std::log10(tail[1]/48000+1e-30)<<" dBFS at 0 / 95%\n";
    CHECK(tail[1]>4*tail[0]);
}
static void testSound()
{
    const auto energy=[](const Audio& a,size_t from=24000)
    {
        double e=0;
        for(size_t i=from;i<a.l.size();++i)e+=(double)a.l[i]*a.l[i];
        return e/(double)(a.l.size()-from);
    };
    Proc low,high;
    for(auto* p:{&low,&high})set(*p,plain),set(*p,{{"pitch",24},{"pitchMode",1},{"size",100},{"density",24}}),prepare(*p);
    Run lo,hi;lo.input=[](int i,int){return (float)(.15*std::sin(2*pi*1000*i/48000));};hi.input=[](int i,int){return (float)(.15*std::sin(2*pi*10000*i/48000));};
    const double pass=energy(run(low,96000,512,lo)),stop=energy(run(high,96000,512,hi));
    std::cout<<"Pitch-up rejection: "<<10*std::log10((stop+1e-30)/pass)<<" dB\n";CHECK(pass>1e-5 && stop<pass*.01);
    for(int channels:{1,2})
    {Proc p;set(p,plain),set(p,"density",24),prepare(p,48000,512,channels,0,2);const auto a=run(p,48000);CHECK(a.l!=a.r && energy(a)>1e-5);}
    // Channel identity must survive stereo capture; random pan alone cannot satisfy this check.
    Proc stereo;set(stereo,plain),set(stereo,{{"density",24},{"size",500}}),prepare(stereo,48000,512,2,0,2);
    Run tones;tones.input=[](int i,int c){return (float)(.15*std::sin(2*pi*(c==0?300:700)*i/48000));};
    const auto separated=run(stereo,144000,512,tones);
    const auto band=[](const std::vector<float>& audio,double hz) {
        double re=0,im=0;
        for(size_t i=96000;i<audio.size();++i)re+=audio[i]*std::cos(2*pi*hz*(double)i/48000),im+=audio[i]*std::sin(2*pi*hz*(double)i/48000);
        return re*re+im*im;
    };
    CHECK(band(separated.l,300)>band(separated.l,700)*std::pow(10.0,1.5));
    CHECK(band(separated.r,700)>band(separated.r,300)*std::pow(10.0,1.5));
    // Small pitch-ups retain the top octave rather than jumping straight to the half-rate mip.
    double highBand[2]{};
    for(int pitch=0;pitch<=1;++pitch)
    {
        Proc p;set(p,plain),set(p,{{"pitch",(float)pitch},{"pitchMode",1},{"size",500},{"trigger",2}}),prepare(p);
        Run r;r.input=[](int i,int){return (float)(.15*std::sin(2*pi*13000*i/48000));};
        r.events={{96000,juce::MidiMessage::noteOn(1,60,(juce::uint8)127)}};
        run(p,96000,512,r);r.offset=96000;highBand[pitch]=std::pow(Level(run(p,48000,512,r)).rms,2);
    }
    const double bandwidthDb=10*std::log10(highBand[1]/highBand[0]);
    std::cout<<"+1 st / 13 kHz relative level: "<<bandwidthDb<<" dB\n";
    CHECK(std::abs(bandwidthDb)<3);
    // Pitched and held readers need power overlap compensation even without scatter or macros.
    double unpitched=0;
    for(int scenario=0;scenario<6;++scenario)
    {
        Proc p;set(p,plain),set(p,{{"size",1200},{"density",15},{"pitch",scenario==0 || scenario==5?0.0f:scenario%2?7.0f:-7.0f},{"pitchMode",scenario>=3?1.0f:0.0f}}),prepare(p);
        // Broad, low-passed noise avoids periodic comb cancellation and mip stop-band losses.
        Run r;r.input=[](int i,int){float x=0;for(int j=0;j<8;++j)x+=noise(i-j);return .053f*x;};
        run(p,192000,512,r);if(scenario==5)set(p,"freeze",1);
        r.offset=192000;run(p,96000,512,r);r.offset=288000;
        const double db=Level(run(p,96000,512,r)).rmsDb();
        if(scenario==0)unpitched=db;
        std::cout<<"Zero-scatter level "<<scenario<<": "<<db<<" dBFS\n";
        CHECK(std::abs(db-unpitched)<6);
    }
    for(const auto* macro:{"motion","texture","bloom","chaos"})
    {
        Proc a,b;prepare(a),prepare(b);const auto neutral=params(b);set(b,macro,75);const auto modified=params(b);
        CHECK(!(run(a,144000)==run(b,144000)));int changes=0;
        for(size_t k=0;k<neutral.size();++k)changes+=neutral[k]!=modified[k]?1:0;
        CHECK(changes==1 && params(b)==modified);
    }
    // Changing texture retains the window of an already sounding grain.
    Proc a,b;for(auto* p:{&a,&b})set(*p,plain),set(*p,{{"trigger",2},{"size",1000}}),prepare(*p);
    Run first;first.side=[](int,int){return 0.0f;};first.events={{24000,juce::MidiMessage::noteOn(1,60,(juce::uint8)127)}};
    CHECK(run(a,30000,512,first)==run(b,30000,512,first));set(b,"texture",100);first.offset=30000;CHECK(run(a,12000,512,first)==run(b,12000,512,first));
    // Capture history and fired events reflect actual audio, and are stable without processBlock.
    CHECK(a.feed.events>0 && a.feed.time==42000);const auto count=a.feed.events.load();CHECK(count==a.feed.events);
}

static void testLoudness(bool tableOnly=false)
{
    const char* const names[]{"bass","pad","pluck"};
    Program programs[]{Program(0),Program(1),Program(2)};
    std::cout<<"preset | program | peak dBFS | RMS dBFS | DC dBFS\n"<<std::fixed<<std::setprecision(3);
    Proc p;
    // 0.2's presets are level-matched: every program within 1 dB of the range 0.1.0's ten span here.
    double factoryLo=0,factoryHi=-300;
    for(int preset=0;preset<=silt::numPresets;++preset) for(int program=0;program<3;++program)
    {
        if(preset<silt::numPresets)p.setCurrentProgram(preset);else load(p,blob("<SILT/>"));
        prepare(p);
        Run material=programs[program].input();
        run(p,8*48000,512,material);material.offset=8*48000;
        const Level level(run(p,192000,512,material));level.check();
        std::cout<<(preset<silt::numPresets?p.getProgramName(preset):juce::String("default"))<<" | "<<names[program]<<" | "<<level.peakDb()<<" | "<<level.rmsDb()<<" | "<<20*std::log10(level.dc+1e-30)<<"\n";
        if(preset<10)factoryLo=std::min(factoryLo,level.rmsDb()),factoryHi=std::max(factoryHi,level.rmsDb());
        else if(preset<silt::numPresets && (level.rmsDb()<factoryLo-1 || level.rmsDb()>factoryHi+1))
            std::cerr<<"preset "<<preset<<" program "<<program<<" outside "<<factoryLo<<".."<<factoryHi<<" dB\n",CHECK(false);
    }
    if(tableOnly)return;
    double maxRms=-300,maxPeak=-300,maxDc=0,minRms=0;
    // Warm the selected history and grain overlaps before measuring each random sound.
    for(int seed=0;seed<500;++seed) for(int program=0;program<3;++program)
    {
        if(program==0 && seed%50==0)std::cout<<"Random loudness seed "<<seed<<" / 500\n";
        p.setCurrentProgram(0),p.randomise(seed),prepare(p);
        Run material=programs[program].input();
        run(p,96000,512,material);material.offset=96000;
        const Level level(run(p,96000,512,material));level.check();
        maxRms=std::max(maxRms,level.rmsDb()),minRms=std::min(minRms,level.rmsDb()),maxPeak=std::max(maxPeak,level.peakDb()),maxDc=std::max(maxDc,level.dc);
    }
    std::cout<<"500 randomise seeds x 3 programs: RMS "<<minRms<<" .. "<<maxRms<<" dBFS, peak "<<maxPeak<<" dBFS, DC "<<20*std::log10(maxDc+1e-30)<<" dBFS\n";
    CHECK(minRms>-40); // never a silent sound
    double cornerRms=-300,cornerPeak=-300,cornerDc=0;
    for(float pitch:{-48.0f,48.0f}) for(float size:{.5f,4000.0f}) for(int freeze:{0,1}) for(int mode:{0,1})
        for(int program=0;program<3;++program)
    {
        p.setCurrentProgram(0);set(p,{{"density",64},{"reverb",100},{"bloom",100},{"pitch",pitch},
            {"size",size},{"sizeSync",1},{"pitchMode",(float)mode},{"texture",100},{"motion",100},{"chaos",100}});prepare(p);
        Run r=programs[program].input();r.perBlock=[&](int t){if(freeze && t>=8*48000)set(p,"freeze",1);};
        const int warmup=(mode==1 && pitch<0 && size>1000?32:8)*48000;
        run(p,warmup,512,r);r.offset=warmup;
        const Level level(run(p,192000,512,r));if(level.dc>=.001)std::cerr<<"corner pitch "<<pitch<<" size "<<size<<" freeze "<<freeze<<" mode "<<mode<<" program "<<program<<"\n";
        level.check();
        cornerRms=std::max(cornerRms,level.rmsDb()),cornerPeak=std::max(cornerPeak,level.peakDb()),cornerDc=std::max(cornerDc,level.dc);
    }
    std::cout<<"Warmed corners: max RMS "<<cornerRms<<" dBFS, peak "<<cornerPeak<<" dBFS, DC "<<20*std::log10(cornerDc+1e-30)<<" dBFS\n";
    p.setCurrentProgram(0),set(p,{{"density",64},{"reverb",100},{"bloom",100}}),prepare(p);
    Run tail=programs[1].input();tail.input=[&](int i,int){return i<48000?programs[1].data[(size_t)i]:0.0f;};tail.side=tail.input;
    const auto y=run(p,60*48000,4096,tail);const Level longLevel(y);longLevel.check();
    double endEnergy=0;for(size_t i=y.l.size()-48000;i<y.l.size();++i)endEnergy+=(double)y.l[i]*y.l[i];
    CHECK(endEnergy/48000<1e-8);
    std::cout<<"60 s feedback: peak "<<longLevel.peakDb()<<" dBFS; final second RMS "<<10*std::log10(endEnergy/48000+1e-30)<<" dBFS\n";
}
// Edge cases from the design review: randomize spread, freeze across a re-prepare, cloud history depth, quiet onsets.
static void testEdges()
{
    {
        Proc p;set(p,{{"trigger",2},{"lock",2},{"onset",1}});float lo=1e9f,hi=0;int coloured=0,binaural=0,heads=0;
        for(int seed=0;seed<50;++seed)
        {
            p.randomise(seed);lo=std::min(lo,get(p,"size")),hi=std::max(hi,get(p,"size"));
            CHECK(get(p,"trigger")==2 && get(p,"onset")==1 && get(p,"size")*get(p,"density")>1490);
            CHECK(get(p,"lock")==0 && get(p,"harmony")<5 && get(p,"scan")>=100 && get(p,"band")==0); // see randomise()
            coloured+=get(p,"spectra")>0,binaural+=get(p,"space")==1,heads+=get(p,"heads")>0;
        }
        CHECK(coloured>5 && coloured<45 && binaural>5 && binaural<45 && heads>20);
        std::cout<<"Randomize sizes over 50 seeds: "<<lo<<" .. "<<hi<<" ms\n";CHECK(hi-lo>300);
    }
    {
        Proc p;prepare(p);run(p,5*48000);set(p,"freeze",1);Run r;r.offset=5*48000;run(p,48000,512,r);CHECK(p.feed.frozen);
        prepare(p,48000,256);r.offset=6*48000;run(p,5*48000,256,r);
        r.offset=11*48000;const Level held(run(p,48000,256,r));
        std::cout<<"Freeze after a re-prepare: "<<held.rmsDb()<<" dBFS\n";CHECK(p.feed.frozen && held.rmsDb()>-40);
    }
    for(int kind:{0,1})
    {
        Proc p;if(kind==0)p.setCurrentProgram(9);else set(p,plain),set(p,{{"size",4000},{"density",64},{"scatter",100},{"pitch",12}});
        prepare(p);run(p,230400);
        juce::int64 oldest=p.feed.time;const auto count=p.feed.events.load();
        for(auto i=std::max<juce::int64>(0,count-(juce::int64)silt::Feed::historySize);i<count;++i)oldest=std::min(oldest,p.feed.history[(size_t)i%silt::Feed::historySize].frame.load());
        const double span=(double)(p.feed.time-oldest)/48000;
        std::cout<<(kind==0?"drift":"dense")<<" cloud history: "<<span<<" s\n";CHECK(span>=3);
    }
    {
        // Meter ballistics fall at the same rate at 48 and 96 kHz.
        float level[2]{};
        for(int k=0;k<2;++k)
        {
            const double fs=48000*(k+1);Proc p;set(p,"mix",0),prepare(p,fs);
            Run r;r.input=[fs](int i,int){return i<fs/10?.3f:0.0f;};run(p,(int)(fs/5),512,r);level[k]=p.feed.meter[0];
        }
        std::cout<<"Meter after 100 ms: "<<level[0]<<" at 48 kHz, "<<level[1]<<" at 96 kHz\n";
        CHECK(level[0]>.01f && std::abs(level[0]-level[1])<.01f*level[0]);
    }
    for(float peak:{.01f,.001f})
    {
        Proc p;set(p,plain),set(p,"trigger",1),prepare(p);
        Run r;r.input=[peak](int i,int){const int k=i%12000;return peak==.01f?(float)(peak*std::exp(-k/2400.0)*std::sin(2*pi*220*k/48000)):peak*noise(i);};
        run(p,16*12000,64,r);CHECK(p.feed.fired==(peak==.01f?16:0)); // a -40 dBFS pluck fires; -60 dBFS noise never
    }
}
// A sustained full-voice workload at 96 kHz guards the cost behind the pluginval watchdog failure.
static void testPerformance()
{
    Proc p;
    for(int quality=0;quality<4;++quality)
    {
        set(p,plain),set(p,{{"size",4000},{"pitch",12},{"pitchMode",0},{"density",64},{"scatter",100},
            {"oversample",(float)quality},{"reverbOn",1},{"reverb",100},{"bloom",100}}),prepare(p,96000,512);
        run(p,3*96000,512);
        int voices=0;for(auto& e:p.feed.env)if(e.load()>0)++voices;CHECK(voices==silt::maxGrains);
        const double start=juce::Time::getMillisecondCounterHiRes();
        Run r;r.offset=3*96000;run(p,96000,512,r);
        const double elapsed=juce::Time::getMillisecondCounterHiRes()-start;
        std::cout<<"96 kHz / 128 shifted voices / "<<(1<<quality)<<"x: "<<elapsed<<" ms per audio second\n";
        CHECK(elapsed<10000); // generous on instrumented/contended CI; detects a processing-loop stall
    }
}
// Dense batch reads and lookahead must agree with single-sample processing, including a held capture.
static void testDenseBlocks()
{
    for (int variant : { 0, 1, 2, 3, 4, 5 }) for (bool frozen : { false, true })
    {
        Run r;const int warm=194000; // freeze holds once 4 s are captured
        for (int note=0;note<128;++note) r.events.push_back({warm,juce::MidiMessage::noteOn(1,note,(juce::uint8)100)});
        r.events.push_back({warm,juce::MidiMessage::allNotesOff(1)});
        r.events.push_back({warm+17001,juce::MidiMessage::controllerEvent(1,1,100)});
        if(variant>=2)for(auto [at,on,note]:{std::tuple{warm+3001,true,60},{warm+5000,true,64},{warm+9000,true,67},{warm+20000,false,64},{warm+26003,true,71}})
            r.events.push_back({at,on?juce::MidiMessage::noteOn(1,note,(juce::uint8)90):juce::MidiMessage::noteOff(1,note)});
        Audio reference;
        for(int block : {512,1,17,0})
        {
            Proc p;set(p,plain);set(p,{{"size",4000},{"pitch",12},{"pitchMode",0},{"density",64},{"bloom",100},{"oversample",3},{"position",0}});
            if(variant==2)set(p,{{"lock",2},{"heads",3},{"harmony",5},{"onset",1},{"scatter",30},{"pitch",-5}}); // tone: steals at the onsets
            if(variant==3)set(p,{{"lock",1},{"heads",2},{"harmony",1},{"onset",1},{"size",300},{"scatter",50}});
            if(variant==4)set(p,{{"band",60},{"centre",2000},{"spectra",50},{"space",1},{"width",160},{"filter",-30},{"feedback",50},{"scatter",40}});
            if(variant==5)set(p,{{"band",30},{"centre",800},{"width",40},{"filter",40},{"feedback",80},{"scatter",50}});
            if(variant==1)set(p,{{"shape",70},{"tilt",-40},{"reverse",40},{"detune",5},{"scale",2},{"drop",20},{"repeat",30},{"sprayDir",1},{"scan",160},{"scatter",30}});
            prepare(p);
            run(p,warm,block);
            if(frozen)set(p,"freeze",1);
            r.offset=warm;
            const auto output=run(p,36000,block,r);
            if(block==512)reference=output;
            else
            {
                if(!(output==reference))
                {
                    double difference=0;size_t first=output.l.size();
                    for(size_t j=0;j<output.l.size();++j) if(output.l[j]!=reference.l[j] || output.r[j]!=reference.r[j])
                    { first=std::min(first,j);difference=std::max({difference,std::abs((double)output.l[j]-reference.l[j]),std::abs((double)output.r[j]-reference.r[j])}); }
                    std::cerr<<"Dense block mismatch: variant "<<variant<<" freeze "<<frozen<<" block "<<block<<" first "<<first<<" max "<<difference<<"\n";
                }
                CHECK(output==reference);
            }
            CHECK(p.feed.frozen==frozen);
        }
    }
    for(int mode:{0,1})for(float pitch:{7.0f,24.0f,48.0f})
    {
        Proc low,high;const double ratio=std::exp2(pitch/12.0);
        for(auto* p:{&low,&high})set(*p,plain),set(*p,{{"pitch",pitch},{"pitchMode",(float)mode},{"size",1000},{"density",12}}),prepare(*p);
        Run pass,stop;pass.input=[=](int i,int){return (float)(.15*std::sin(2*pi*1000/ratio*i/48000));};
        stop.input=[=](int i,int){return (float)(.15*std::sin(2*pi*26000/ratio*i/48000));};
        run(low,96000,512,pass),run(high,96000,512,stop);pass.offset=stop.offset=96000;
        const double passEnergy=std::pow(Level(run(low,48000,512,pass)).rms,2),stopEnergy=std::pow(Level(run(high,48000,512,stop)).rms,2);
        std::cout<<"Near-fold rejection "<<mode<<" / "<<pitch<<": "<<10*std::log10((stopEnergy+1e-30)/passEnergy)<<" dB\n";
        CHECK(stopEnergy<passEnergy*1e-6);
    }
}
static void pump() { juce::MessageManager::getInstance()->runDispatchLoopUntil(5); }
template <typename Done> static void pumpUntil(Done done)
{
    for (auto end = juce::Time::getMillisecondCounter() + 2000; !done() && juce::Time::getMillisecondCounter() < end;)
        pump(), juce::Timer::callPendingTimersSynchronously();
}
static juce::Component* find(juce::Component& c, const juce::String& id)
{
    if (c.getComponentID() == id) return &c;
    for (auto* child : c.getChildren())
        if (auto* found = find(*child, id)) return found;
    return nullptr;
}
static juce::String sha(const juce::MemoryOutputStream& m) { return juce::SHA256(m.getData(), m.getDataSize()).toHexString(); }
static juce::String pixels(const juce::Image& image)
{
    const juce::Image argb = image.convertedToFormat(juce::Image::ARGB);
    const juce::Image::BitmapData d(argb, juce::Image::BitmapData::readOnly);
    juce::MemoryOutputStream m;
    for (int y = 0; y < d.height; ++y) m.write(d.getLinePointer(y), (size_t) (d.width * d.pixelStride));
    return sha(m);
}

// Observe real repaint invalidations without a window (as gulp).
struct RepaintCapture : juce::CachedComponentImage
{
    juce::Component& component;
    juce::Rectangle<int> pending;
    explicit RepaintCapture(juce::Component& c) : component(c), pending(c.getLocalBounds()) {}
    void paint(juce::Graphics& g) override { component.paint(g); }
    bool invalidateAll() override { pending = component.getLocalBounds(); return false; }
    bool invalidate(const juce::Rectangle<int>& r) override { pending = pending.getUnion(r); return false; }
    void releaseResources() override {}
    bool consume(juce::Image& image, float scale)
    {
        if (pending.isEmpty()) return false;
        juce::Graphics g(image);
        g.addTransform(juce::AffineTransform::scale(scale)), g.reduceClipRegion(pending);
        paint(g), pending = {};
        return true;
    }
};

static void testEditor()
{
    const auto folder=juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("silt-ui","",false);
    for(int percent:{50,100,200})
    {
        Proc p(folder);prepare(p);run(p,48000);
        std::unique_ptr<juce::AudioProcessorEditor> editor(p.createEditor());
        auto& e=*editor;auto& timer=dynamic_cast<juce::Timer&>(e);timer.stopTimer();
        CHECK(e.getWidth()==1024 && e.getHeight()==410);
        e.setSize(1024*percent/100,410*percent/100),e.setVisible(true);
        const auto point=[&](juce::Component& c){return e.getLocalPoint(&c,c.getLocalBounds().toFloat().getCentre()).roundToInt();};
        const auto press=[&](const juce::String& id)
        {
            auto* c=find(e,id);CHECK(c && c->isVisible());
            if(!c)return;
            auto* target=e.getComponentAt(point(*c));CHECK(target==c);
            auto* button=dynamic_cast<juce::Button*>(target);CHECK(button);
            if(button){bool done=false;button->triggerClick();juce::MessageManager::callAsync([&]{done=true;});pumpUntil([&]{return done;});}
            timer.timerCallback();
        };
        CHECK(!find(e,"group:macros")->isVisible());press("macro");CHECK(find(e,"group:macros")->isVisible() && !find(e,"group:presets")->isVisible());
        const auto time=juce::Time::getCurrentTime();
        const auto mouse=[&](juce::Component* c,juce::Point<float> pos,juce::Point<float> down,int clicks=1)
        {
            return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(),pos,juce::ModifierKeys::leftButtonModifier,
                1,0,0,0,0,c,c,time,down,time,clicks,pos!=down);
        };
        for(const auto& spec:contract)
        {
            if(&spec-contract>=silt::numParamsV1)break; // 0.2's parameters get controls with the new layout
            auto* param=p.apvts.getParameter(spec.id);
            if(dynamic_cast<juce::AudioParameterFloat*>(param))
            {
                auto* slider=dynamic_cast<juce::Slider*>(find(e,spec.id));CHECK(slider && slider->isVisible());
                if(!slider)continue;
                juce::Label* label=nullptr;for(auto* child:slider->getChildren())if(auto* l=dynamic_cast<juce::Label*>(child))label=l;
                CHECK(label && label->isEditableOnSingleClick());
                const bool fader=juce::String(spec.id)=="motion" || juce::String(spec.id)=="texture" || juce::String(spec.id)=="bloom" || juce::String(spec.id)=="chaos";
                const auto grab=fader?juce::Point<float>(20,8):slider->getLocalBounds().toFloat().getCentre();
                CHECK(e.getComponentAt(e.getLocalPoint(slider,grab).roundToInt())==slider);
                param->setValueNotifyingHost(.3f);
                if(fader)
                {
                    const float before=param->getValue();
                    const auto click=juce::Point<float>(100,8);
                    slider->mouseDown(mouse(slider,click,click));CHECK(param->getValue()==before);
                    slider->mouseUp(mouse(slider,click,click));CHECK(param->getValue()==before);
                }
                const auto to=fader?grab+juce::Point<float>(30,0):grab-juce::Point<float>(0,50);
                slider->mouseDown(mouse(slider,grab,grab)),slider->mouseDrag(mouse(slider,to,grab)),slider->mouseUp(mouse(slider,to,grab));
                CHECK(fader?param->getValue()>.3f:std::abs(param->getValue()-.5f)<.01f); // gulp's exact 250 design-pixel vertical sensitivity
                slider->mouseDoubleClick(mouse(slider,grab,grab,2));
                CHECK(std::abs(get(p,spec.id)-param->convertFrom0to1(param->getDefaultValue()))<.001f);
                if(label)
                {
                    CHECK(e.getComponentAt(point(*label))==label);
                    label->showEditor();CHECK(label->isBeingEdited());
                    const auto typed=param->getText(.7f,0);
                    if(auto* text=label->getCurrentTextEditor())text->setText(typed);
                    label->hideEditor(false);CHECK(param->getCurrentValueAsText()==typed);
                }
            }
            else if(dynamic_cast<juce::AudioParameterChoice*>(param))
            {
                set(p,spec.id,spec.lo);
                for(int k=0;k<=(int)spec.hi;++k)press(juce::String(spec.id)+":"+juce::String(k)),CHECK(get(p,spec.id)==(float)k);
            }
            else
            {
                set(p,spec.id,0);press(spec.id);CHECK(get(p,spec.id)==1);press(spec.id);CHECK(get(p,spec.id)==0);
            }
        }
        // All hit cells stay inside their group and never cover another control in that group.
        for(const auto* id:{"grain","pitch","trigger","space / output","macros","header","presets","freeze"})
        {
            auto* group=find(e,juce::String("group:")+id);CHECK(group);
            for(auto* a:group->getChildren()) if(a->isVisible())
            {
                CHECK(group->getLocalBounds().contains(a->getBounds()));
                for(auto* b:group->getChildren())if(b!=a && b->isVisible())CHECK(!a->getBounds().intersects(b->getBounds()));
            }
        }
        p.setCurrentProgram(0);press("next");CHECK(p.preset==1);press("prev");CHECK(p.preset==0);
        press("save");CHECK(p.presetName().startsWith("user "));
        press("B"),set(p,"pitch",12),press("A");CHECK(get(p,"pitch")==-12);press("B");CHECK(get(p,"pitch")==12);
        const auto before=params(p);press("rnd");CHECK(params(p)!=before && p.presetName()=="random");
        press("macro");CHECK(!find(e,"group:macros")->isVisible() && find(e,"group:presets")->isVisible());
        for(int i=0;i<10;++i){press("preset:"+juce::String(i));CHECK(p.preset==i);}
        press("next");CHECK(p.preset==10); // the row now shows presets 11-20
        for(int i=0;i<10;++i){press("preset:"+juce::String(i));CHECK(p.preset==10+i);}
        p.setCurrentProgram(4);
        // Dirty-cell redraw gives the same pixels as a full render; no repaint when audio and controls are idle.
        auto* capture=new RepaintCapture(e);e.setCachedComponentImage(capture);
        const float scale=(float)percent/50;
        juce::Image image(juce::Image::RGB,1024*percent/50,410*percent/50,true);
        capture->consume(image,scale);
        int paints=0;
        for(int frame=0;frame<35;++frame)
        {
            if(frame<25){Run r;r.offset=frame*1600;run(p,1600,512,r);}
            timer.timerCallback();const bool painted=capture->consume(image,scale);paints+=painted;
            if(frame>=26)CHECK(!painted);
        }
        CHECK(paints>=15);
        juce::Image fresh(juce::Image::RGB,image.getWidth(),image.getHeight(),true);
        {juce::Graphics g(fresh);g.addTransform(juce::AffineTransform::scale(scale));e.paint(g);}
        CHECK(pixels(image)==pixels(fresh));
        Proc restored;load(restored,save(p));std::unique_ptr<juce::AudioProcessorEditor> reopened(restored.createEditor());
        CHECK(reopened->getWidth()==1024*percent/100 && reopened->getHeight()==410*percent/100);
    }
    folder.deleteRecursively();
}
// 4.8 s of pad fills the cloud's 4 s span; freeze then holds it for one more second.
static juce::Image snapshot(int percent=100,bool macros=false,bool freeze=false,float scale=2)
{
    Proc p;prepare(p);run(p,230400);p.showMacros=macros;
    if(freeze){set(p,"freeze",1);Run r;r.offset=230400;run(p,48000,512,r);}
    std::unique_ptr<juce::AudioProcessorEditor> editor(p.createEditor());
    editor->setSize(1024*percent/100,410*percent/100);
    return editor->createComponentSnapshot(editor->getLocalBounds(),true,scale);
}
static juce::String soundHash()
{
    Proc p;prepare(p);Host h;Run r;r.host=&h;
    r.events={{1701,juce::MidiMessage::noteOn(1,64,(juce::uint8)100)},{12001,juce::MidiMessage::controllerEvent(1,1,64)},
        {30000,juce::MidiMessage::noteOff(1,64)}};
    const auto a=run(p,230400,512,r);CHECK(Level(a).rms>.001);juce::MemoryOutputStream bytes;
    bytes.write(a.l.data(),a.l.size()*sizeof(float)),bytes.write(a.r.data(),a.r.size()*sizeof(float));return sha(bytes);
}
// Old sessions sound bit-identical: v1 documents (the fixture; each factory preset as 0.1.0 saved it; one with an
// 18-value A/B slot, then selected) render the hashes recorded from the 0.1.0 build (ad32710).
static std::vector<juce::String> legacyHashes()
{
    Proc defaults;
    const auto doc=[&](int i,const juce::String& attributes) {
        juce::String xml="<SILT stateVersion=\"1\" preset=\""+juce::String(i)+"\""+attributes+">";
        for(size_t k=0;k<std::size(contract);++k)xml<<"<PARAM id=\""<<contract[k].id<<"\" value=\""<<juce::String((double)silt::presets[i].v[k],9)<<"\"/>";
        return blob(xml+"</SILT>");
    };
    juce::StringArray slot;
    for(size_t k=0;k<std::size(contract);++k)slot.add(juce::String((double)defaults.apvts.getParameter(contract[k].id)->convertTo0to1(silt::presets[7].v[k]),9));
    std::vector<juce::MemoryBlock> states{{},doc(3," ab=\"1\" slotA=\"7;"+slot.joinIntoString(",")+"\"")};
    CHECK(juce::File(SILT_TEST_SOURCE_DIR "/state-v1.bin").loadFileAsData(states[0]));
    for(int i=0;i<10;++i)states.push_back(doc(i,{}));
    std::vector<juce::String> hashes;
    for(size_t k=0;k<states.size();++k)
    {
        Proc p,v2;load(p,states[k]);
        if(k==1)p.selectAb(0);
        load(v2,save(p)); // the same session saved by 0.2 (stateVersion 2) and reopened
        const auto render=[](Proc& q){prepare(q);Host h;Run r;r.host=&h;
            r.events={{1701,juce::MidiMessage::noteOn(1,64,(juce::uint8)100)},{12001,juce::MidiMessage::controllerEvent(1,1,64)},
                {30000,juce::MidiMessage::noteOff(1,64)}};
            return run(q,144000,512,r);};
        const auto a=render(p);CHECK(render(v2)==a);juce::MemoryOutputStream bytes;
        bytes.write(a.l.data(),a.l.size()*sizeof(float)),bytes.write(a.r.data(),a.r.size()*sizeof(float));
        hashes.push_back(sha(bytes).substring(0,16));
    }
    return hashes;
}
static void testGoldens(bool record=false)
{
    juce::MemoryOutputStream math;
    for(int i=1;i<=256;++i)
    {
        volatile double input=i*.123456789;const double x=input;
        const double values[]{std::sin(x),std::cos(x),std::tan(x),std::exp(-x),std::exp2(x),std::log(x),std::log2(x),std::log1p(x),std::tanh(x),std::pow(x,.37)};
        math.write(values,sizeof(values));
    }
    juce::Image probe(juce::Image::ARGB,128,32,true);
    {juce::Graphics g(probe);g.setColour(juce::Colours::white);g.fillRoundedRectangle(.25f,.75f,127,31,4.5f);
        g.setColour(juce::Colours::black);g.setFont(juce::Font(juce::FontOptions(juce::Typeface::createSystemTypefaceFor(BinaryData::GeistMonoRegular_ttf,BinaryData::GeistMonoRegular_ttfSize)).withPointHeight(11)));
        g.drawText("silt 0123456789",probe.getBounds(),juce::Justification::centred,false);}
    const auto environment=juce::String(__VERSION__)+"/"+SILT_BUILD_TYPE+"/"+sha(math);
    const auto renderer=pixels(probe);
    const bool ci=std::getenv("GITHUB_ACTIONS")!=nullptr;
    const char* expectedEnvironment="14.2.0/Release/07adb83962bd7f372226ea055bca705c159534fa4a19d3279488b12cd95eea5c", *expectedRenderer="506141d22f79fb1886eb4dec6e8e40da3c3c48e3df63f1f3d60e979dd78361b5";
    const char* expectedSound="c84295f77b28dd4228a7fe471bf7e41937303f55ff5663e927482f9dca5907da";
    const char* expectedShots[]{"03c1267a53e020ac141dd43b3fc7f19032595a4cef35ba60a7392df930093463","16e2c80b54d29ef2a7b0987c4411b50ab275fb3e9ea404b7faf68cd31edcf415","c3eabceccba6d36a308e2afb1eccb026a3cd1b26845191afef88565950ce5399"};
    std::cout<<"Golden environment: "<<environment<<"\nGolden renderer: "<<renderer<<"\n";
    if(record || (!ci && environment==expectedEnvironment))
    {
        const auto audio=soundHash();std::cout<<"Golden sound: "<<audio<<"\n";
        if(!record)CHECK(audio==expectedSound);
        const char* expectedLegacy[]{"49a665f135552f91","4f990f341d3ef688","ad373836121b1f3c","ff7b604aba9a3ab8","41e0d592e0b2ab2a","63f176c6e5a5e2d5",
            "3a8e7ffa6fbe0cd3","88107176265b1e65","42faedd107cb9113","4f990f341d3ef688","99bfadade61751c0","cc09de93224ecb2d"}; // 0.1.0, ad32710
        const auto legacy=legacyHashes();CHECK(legacy.size()==std::size(expectedLegacy));
        for(size_t k=0;k<legacy.size();++k)
        {
            std::cout<<"Golden v1 "<<k<<": "<<legacy[k]<<"\n";
            if(!record)CHECK(legacy[k]==expectedLegacy[k]);
        }
        if(record || renderer==expectedRenderer)
        {
            int k=0;for(int percent:{50,100,200})
            {
                const auto hash=pixels(snapshot(percent));std::cout<<"Golden "<<percent<<": "<<hash<<"\n";
                if(!record)CHECK(hash==expectedShots[k]);
                ++k;
            }
        }
    }
    else std::cout<<"Goldens skipped ("<<(ci?"GITHUB_ACTIONS":"different environment")<<")\n";
    CHECK(soundHash()==soundHash() && pixels(snapshot())==pixels(snapshot()));
}
#if JUCE_LINUX
#include <dlfcn.h>
static void ignoreXErrors()
{
    if(auto* x11=dlopen("libX11.so.6",RTLD_NOW))
        if(auto handler=(void* (*)(int (*)(void*,void*)))dlsym(x11,"XSetErrorHandler"))handler([](void*,void*){return 0;});
}
#endif
#include "SiltOffline.h"
int main(int argc,char** argv)
{
#if JUCE_LINUX
    ignoreXErrors();
#endif
    std::cout<<std::unitbuf;
    const juce::ScopedJuceInitialiser_GUI init;
    if(const juce::String mode=argc>=3?argv[1]:"";mode=="--screenshot" || mode=="--screenshot-macros" || mode=="--screenshot-freeze")
    {
        // An optional scale: 1 = 1024 x 410 pixels at device scale 1, 2 (default) = 2048 x 820.
        const float scale=argc>=4?juce::jlimit(1.0f,4.0f,juce::String(argv[3]).getFloatValue()):2.0f;
        juce::File file=juce::File::getCurrentWorkingDirectory().getChildFile(argv[2]);file.deleteFile();
        juce::FileOutputStream output(file);
        return output.openedOk() && juce::PNGImageFormat().writeImageToStream(snapshot(100,mode=="--screenshot-macros",mode=="--screenshot-freeze",scale),output)?0:1;
    }
    if(argc>=2 && (juce::String(argv[1])=="--bench" || juce::String(argv[1])=="--profile")){bench(juce::String(argv[1])=="--profile");return failures?1:0;}
    if(argc>=3 && juce::String(argv[1])=="--renders"){renderFiles(juce::File(argv[2]));return failures?1:0;}
    if(argc>=2 && juce::String(argv[1])=="--benchmark"){testPerformance();return failures?1:0;}
    if(argc>=2 && juce::String(argv[1])=="--editor"){testEditor();return failures?1:0;}
    if(argc>=2 && juce::String(argv[1])=="--goldens"){testGoldens(true);return failures?1:0;}
    if(argc>=2 && juce::String(argv[1])=="--loudness"){testLoudness(true);return failures?1:0;}
    // Make sure the allocation probe itself works, then clear its calibration allocation.
    onAudioThread=true;void* probe=::operator new(64);escape(probe);::operator delete(probe);onAudioThread=false;
    CHECK(audioAllocations>0);audioAllocations=0;
    testContract();testParameterText();testState();testPresets();testDryAndLifecycle();testTimingAndMidi();testRatios();testOnsetsAndSource();testFreezeAndWindows();
    testGrainFeatures();testHeadsAndLock();testColourAndSpace();testSound();testEdges();testDenseBlocks();
    if(argc>=2 && juce::String(argv[1])=="--core")return failures?1:0;
    testEditor();testGoldens();testPerformance();testLoudness();
    CHECK(audioAllocations==0);
    std::cout<<"Audio allocations: "<<audioAllocations<<"\n"<<(failures?"Silt tests FAILED\n":"All Silt tests passed\n");
    return failures?1:0;
}
