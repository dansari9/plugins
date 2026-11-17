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

// Real-time check: count heap allocations made on a thread flagged as "audio" (as crumb).
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
    {"density",1,64,24},{"position",0,4000,1200},{"scatter",0,100,45},{"pitch",-48,48,7},
    {"pitchMode",0,1,0,"shift,tape"},{"trigger",0,2,0,"clock,transient,sidechain"},{"reverbOn",0,1,1,""},
    {"reverb",0,100,30},{"oversample",0,3,1,"off,2x,4x,8x"},{"freeze",0,1,0,""},
    {"motion",0,100,0},{"texture",0,100,0},{"bloom",0,100,0},{"chaos",0,100,0}
};
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
    CHECK(juce::String(JucePlugin_VersionString)=="0.1.0" && juce::String(JucePlugin_Vst3Category)=="Fx");
    CHECK(!JucePlugin_IsSynth && JucePlugin_WantsMidiInput && !JucePlugin_ProducesMidiOutput);
    Proc p;
    CHECK(p.getParameters().size()==(int)std::size(contract) && p.stateVersion==1 && p.getNumPrograms()==10);
    for(int i=0;i<p.getParameters().size();++i)
    {
        const auto& c=contract[i];
        auto* q=dynamic_cast<juce::RangedAudioParameter*>(p.getParameters()[i]);
        CHECK(q && q->paramID==c.id && q->getVersionHint()==1 && q->isAutomatable());
        CHECK(q->getNormalisableRange().start==c.lo && q->getNormalisableRange().end==c.hi && get(p,c.id)==c.def);
        if(auto* choice=dynamic_cast<juce::AudioParameterChoice*>(q)) CHECK(c.choices && choice->choices.joinIntoString(",")==c.choices);
        else if(dynamic_cast<juce::AudioParameterBool*>(q)) CHECK(c.choices && juce::String(c.choices).isEmpty());
        else CHECK(c.choices==nullptr);
    }
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
    CHECK(p.getTailLengthSeconds()>=8 && p.getName()=="silt");
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
    }
    const auto parse=[&](const char* id,const juce::String& text)
    {
        auto* q=p.apvts.getParameter(id);
        q->setValueNotifyingHost(q->getValueForText(text));
    };
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
}
static void testPresets()
{
    const auto folder=juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("silt-tests","",false);
    Proc p(folder),defaults;
    p.setCurrentProgram(4);CHECK(params(p)==params(defaults));
    const char* names[]{"sub dust","grit","glass","halo","tide","choir","tines","chime","rain","drift"};
    for(int i=0;i<10;++i)
    {
        p.setCurrentProgram(i);CHECK(p.getProgramName(i)==names[i] && p.getCurrentProgram()==i);
        CHECK(get(p,"mix")==100 && get(p,"output")==0 && get(p,"freeze")==0);
    }
    p.setCurrentProgram(0),set(p,"pitch",7);
    const auto before=params(p);
    const auto file=p.saveUserPreset();CHECK(file.existsAsFile() && p.presetName()=="user 01");
    p.setCurrentProgram(9),p.stepPreset(1);CHECK(params(p)==before && p.presetName()=="user 01");
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
                CHECK(latency>=0 && latency<512 && (quality==0?latency==0:latency>0));
                for(size_t i=0;i<audio.l.size();++i)CHECK(audio.l[i]==((int)i<latency?0:r.input((int)i-latency,0)) && audio.r[i]==audio.l[i]);
                Run impulse;impulse.input=[](int i,int){return i==0?1.0f:0.0f;};
                prepare(p,fs,512,2,0);const auto y=run(p,512,17,impulse);
                CHECK(std::distance(y.l.begin(),std::max_element(y.l.begin(),y.l.end()))==latency);
                if(block==512)std::cout<<" | "<<latency;
                // Quality can change after prepare, with no new storage or stale dry data after its delay.
                for(int next=0;next<4;++next)
                {
                    set(p,"oversample",(float)next);run(p,512,block,r);
                    CHECK(next==0?p.getLatencySamples()==0:p.getLatencySamples()>0);
                }
            }
        }
        std::cout<<"\n";
    }
    // Live quality changes glide between delay taps and FIR paths instead of resetting into a step.
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
    for(int shape=0;shape<12;++shape)for(double skew:{0.0,.5,1.0})
    {
        CHECK(std::abs(silt::window(shape,0,skew))<1e-12 && std::abs(silt::window(shape,1,skew))<1e-12);
        for(int k=0;k<=1000;++k)CHECK(silt::window(shape,k/1000.0,skew)>=0 && silt::window(shape,k/1000.0,skew)<=1);
    }
    Proc p;set(p,plain),set(p,{{"size",600},{"density",24},{"bloom",50}}),prepare(p);
    Run r;r.input=[](int i,int){return (float)(.15*std::sin(2*pi*110*i/48000));};run(p,48000,64,r);
    r.offset=48000;r.perBlock=[&](int){set(p,"freeze",1);};const auto held=run(p,24000,64,r);
    CHECK(p.feed.frozen && maxStep(held)<.04);
    const int head=p.feed.at[0];r.offset=72000;run(p,24000,64,r);CHECK(p.feed.at[0]==head);
    r.offset=96000;r.perBlock=[&](int){set(p,"freeze",0);};CHECK(maxStep(run(p,24000,64,r))<.04 && !p.feed.frozen);
    set(p,plain),set(p,{{"trigger",2},{"size",4000},{"pitch",-48},{"pitchMode",1}}),prepare(p,96000,4096);
    Run longGrain;longGrain.input=[](int,int){return 0.0f;};longGrain.side=longGrain.input;
    longGrain.events={{0,juce::MidiMessage::noteOn(1,60,(juce::uint8)127)}};
    run(p,16*96000,4096,longGrain);CHECK(std::abs(p.feed.env[0]-.5f)<2e-6f);
    longGrain.offset=16*96000;run(p,32*96000,4096,longGrain);CHECK(std::abs(p.feed.env[0]-.5f)<2e-6f);
    longGrain.offset=48*96000;run(p,16*96000,4096,longGrain);CHECK(p.feed.env[0]==0);
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
    for(int preset=0;preset<11;++preset) for(int program=0;program<3;++program)
    {
        if(preset<10)p.setCurrentProgram(preset);else load(p,blob("<SILT/>"));
        prepare(p);
        Run material=programs[program].input();
        run(p,8*48000,512,material);material.offset=8*48000;
        const Level level(run(p,192000,512,material));level.check();
        std::cout<<(preset<10?p.getProgramName(preset):juce::String("default"))<<" | "<<names[program]<<" | "<<level.peakDb()<<" | "<<level.rmsDb()<<" | "<<20*std::log10(level.dc+1e-30)<<"\n";
    }
    if(tableOnly)return;
    double maxRms=-300,maxPeak=-300,maxDc=0;
    // Warm the selected history and grain overlaps before measuring each random sound.
    for(int seed=0;seed<500;++seed) for(int program=0;program<3;++program)
    {
        p.setCurrentProgram(0),p.randomise(seed),prepare(p);
        Run material=programs[program].input();
        run(p,96000,512,material);material.offset=96000;
        const Level level(run(p,96000,512,material));level.check();
        maxRms=std::max(maxRms,level.rmsDb()),maxPeak=std::max(maxPeak,level.peakDb()),maxDc=std::max(maxDc,level.dc);
    }
    std::cout<<"500 randomise seeds x 3 programs: max RMS "<<maxRms<<" dBFS, peak "<<maxPeak<<" dBFS, DC "<<20*std::log10(maxDc+1e-30)<<" dBFS\n";
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

// Observe real repaint invalidations without a window (as crumb).
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
static juce::Image snapshot(int percent=100,bool macros=false)
{
    Proc p;prepare(p);run(p,48000);p.showMacros=macros;
    std::unique_ptr<juce::AudioProcessorEditor> editor(p.createEditor());
    editor->setSize(1024*percent/100,410*percent/100);
    return editor->createComponentSnapshot(editor->getLocalBounds(),true,2.0f);
}
static juce::String soundHash()
{
    Proc p;prepare(p);Host h;Run r;r.host=&h;
    r.events={{1701,juce::MidiMessage::noteOn(1,64,(juce::uint8)100)},{12001,juce::MidiMessage::controllerEvent(1,1,64)},
        {30000,juce::MidiMessage::noteOff(1,64)}};
    const auto a=run(p,48000,512,r);juce::MemoryOutputStream bytes;
    bytes.write(a.l.data(),a.l.size()*sizeof(float)),bytes.write(a.r.data(),a.r.size()*sizeof(float));return sha(bytes);
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
    const char* expectedSound="fec9afb531a8e036eba1d81651896e1b2c1f78b0234dbadc8e7549563f09407b";
    const char* expectedShots[]{"9d7bece4635ab4f6212002a83e81edeb47b6c51dd0519dddac557fc49c8cadd5","4196a120f6368ce662e951a30d4f85bb8b5da35f597d9b0c7c55390579d74597","9fb6a34fd52acc3ef7cbd91b179931707117f91270cb6c43dc60e5b9877ef8eb"};
    std::cout<<"Golden environment: "<<environment<<"\nGolden renderer: "<<renderer<<"\n";
    if(record || (!ci && environment==expectedEnvironment))
    {
        const auto audio=soundHash();std::cout<<"Golden sound: "<<audio<<"\n";
        if(!record)CHECK(audio==expectedSound);
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
int main(int argc,char** argv)
{
#if JUCE_LINUX
    ignoreXErrors();
#endif
    std::cout<<std::unitbuf;
    const juce::ScopedJuceInitialiser_GUI init;
    if(argc>=3 && (juce::String(argv[1])=="--screenshot" || juce::String(argv[1])=="--screenshot-macros"))
    {
        juce::File file=juce::File::getCurrentWorkingDirectory().getChildFile(argv[2]);file.deleteFile();
        juce::FileOutputStream output(file);return output.openedOk() && juce::PNGImageFormat().writeImageToStream(snapshot(100,juce::String(argv[1])=="--screenshot-macros"),output)?0:1;
    }
    if(argc>=2 && juce::String(argv[1])=="--benchmark"){testPerformance();return failures?1:0;}
    if(argc>=2 && juce::String(argv[1])=="--editor"){testEditor();return failures?1:0;}
    if(argc>=2 && juce::String(argv[1])=="--goldens"){testGoldens(true);return failures?1:0;}
    if(argc>=2 && juce::String(argv[1])=="--loudness"){testLoudness(true);return failures?1:0;}
    // Make sure the allocation probe itself works, then clear its calibration allocation.
    onAudioThread=true;void* probe=::operator new(64);escape(probe);::operator delete(probe);onAudioThread=false;
    CHECK(audioAllocations>0);audioAllocations=0;
    testContract();testParameterText();testState();testPresets();testDryAndLifecycle();testTimingAndMidi();testRatios();testOnsetsAndSource();testFreezeAndWindows();
    testSound();
    if(argc>=2 && juce::String(argv[1])=="--core")return failures?1:0;
    testEditor();testGoldens();testPerformance();testLoudness();
    CHECK(audioAllocations==0);
    std::cout<<"Audio allocations: "<<audioAllocations<<"\n"<<(failures?"Silt tests FAILED\n":"All Silt tests passed\n");
    return failures?1:0;
}
