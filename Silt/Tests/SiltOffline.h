// Offline measurement modes use the real processor; timing excludes input generation, allocation and file I/O.
static void dense(Proc& p, bool tiny=false)
{
    set(p,plain);set(p,{{"size",tiny?.5f:4000},{"sizeSync",1},{"density",64},{"scatter",100},{"pitch",24},
        {"pitchMode",0},{"motion",100},{"texture",tiny?0.0f:100.0f},{"chaos",100},{"bloom",100},
        {"reverbOn",1},{"reverb",100},{"oversample",3}});
}
static void bench(bool profile=false)
{
    std::cout<<"rate | sound | audio seconds | processing seconds | realtime factor | one core % | max voices\n";
    for(int fs:{44100,48000,96000}) for(int sound=-1;sound<29;++sound)
    {
        if(profile && (fs!=96000 || sound!=10))continue;
        Proc p;
        if(sound>=0 && sound<10)p.setCurrentProgram(sound);
        if(sound>=19)p.setCurrentProgram(sound-9); // 0.2's presets
        if(sound==10 || sound==11 || sound==18)dense(p,sound==11);
        // Also preserve the exact layout-stage 128-voice workload (2x, +12 st, unmodulated).
        if(sound==12)set(p,plain),set(p,{{"size",4000},{"pitch",12},{"pitchMode",0},{"density",64},
            {"scatter",100},{"oversample",1},{"reverbOn",1},{"reverb",100},{"bloom",100}});
        if(sound==13)set(p,{{"shape",70},{"tilt",-40},{"reverse",40},{"detune",5},{"scale",2},{"repeat",30},{"scan",160}}); // 0.2 grain controls on the default sound
        if(sound==14)set(p,{{"heads",3},{"harmony",3},{"detune",3},{"shape",50},{"scan",60}}); // 4 heads, PLAN 3 budget case
        if(sound==15)set(p,{{"lock",2},{"pitch",23.2f},{"sizeSync",1},{"size",50}}); // tone lock, f0 ~1 kHz, 50 ms
        if(sound==16 || sound==18)set(p,{{"band",60},{"centre",1500},{"spectra",50}}); // colour: every grain band-passed
        if(sound==17)set(p,{{"space",1},{"width",150},{"filter",-30},{"feedback",30}}); // binaural, width, filter, feedback
        prepare(p,fs,512,2,0);
        juce::AudioBuffer<float> input(2,fs), block(2,512);juce::MidiBuffer midi;
        for(int i=0;i<fs;++i)for(int c=0;c<2;++c)input.setSample(c,i,pad(i+c*47,fs));
        double seconds=0;int voices=0;
        const int duration=profile?5:28,warmup=profile?4:8;
        for(int start=0;start<duration*fs;start+=512)
        {
            const int n=std::min(512,duration*fs-start);block.setSize(2,n,false,false,true);
            for(int c=0;c<2;++c)for(int i=0;i<n;++i)block.setSample(c,i,input.getSample(c,(start+i)%fs));
            const double before=juce::Time::getHighResolutionTicks();
            p.processBlock(block,midi);
            if(start>=warmup*fs)seconds+=(juce::Time::getHighResolutionTicks()-before)/juce::Time::getHighResolutionTicksPerSecond();
            int active=0;for(auto& e:p.feed.env)active+=e.load()>0?1:0;voices=std::max(voices,active);
        }
        const juce::String name=sound<0?"default":sound<10?p.getProgramName(sound):sound>=19?p.getProgramName(sound-9):sound==10?"dense":sound==11?"dense tiny":sound==12?"dense legacy 2x":sound==13?"0.2 grain":sound==14?"0.2 heads":sound==15?"0.2 tone":sound==16?"0.2 colour":sound==17?"0.2 space":"dense colour";
        std::cout<<fs<<" | "<<name<<" | "<<(duration-warmup)<<" | "<<seconds<<" | "<<(duration-warmup)/seconds<<" | "<<seconds*100/(duration-warmup)<<" | "<<voices<<"\n";
        if(sound==10 || sound==12 || sound==18)CHECK(voices==128);
    }
}
static void writeWav(const juce::File& file,const Audio& a)
{
    file.deleteFile();std::unique_ptr<juce::OutputStream> stream=file.createOutputStream();juce::WavAudioFormat format;
    auto writer=format.createWriterFor(stream,juce::AudioFormatWriterOptions().withSampleRate(48000).withNumChannels(2).withBitsPerSample(24));
    CHECK(writer!=nullptr);if(!writer)return;
    juce::AudioBuffer<float> b(2,(int)a.l.size());
    b.copyFrom(0,0,a.l.data(),(int)a.l.size()),b.copyFrom(1,0,a.r.data(),(int)a.r.size());
    CHECK(writer->writeFromAudioSampleBuffer(b,0,b.getNumSamples()));
}
static void renderFiles(const juce::File& folder)
{
    folder.createDirectory();
    const char* names[]{"bass","pad","pluck"};
    for(int kind=0;kind<3;++kind)
    {
        Program program(kind,12*48000);
        // Continuous oscillator phase and short note envelopes keep source attacks distinct from effect clicks.
        if(kind==0)
        {
            double phase=0,energy=0;const double notes[]{55,65.4064,73.4162,82.4069};
            for(size_t i=0;i<program.data.size();++i)
            {
                phase+=notes[(i/24000)%4]/48000;double x=0;
                for(int h=1;h<=12;++h)x+=std::sin(2*pi*h*phase)/h;
                const double t=(double)(i%24000)/48000;
                x*=std::min(1.0,t/.004)*std::min(1.0,(.5-t)/.01);
                program.data[i]=(float)x;energy+=x*x;
            }
            const float gain=(float)(std::pow(10.0,-18/20.0)/std::sqrt(energy/program.data.size()));
            for(auto& x:program.data)x*=gain;
        }
        const auto material=program.input();
        Audio source{program.data,program.data};writeWav(folder.getChildFile(juce::String("input-")+names[kind]+".wav"),source);
        for(int sound=-1;sound<silt::numPresets+20;++sound)
        {
            Proc p;juce::String name="default";
            if(sound>=0 && sound<silt::numPresets)p.setCurrentProgram(sound),name=p.getProgramName(sound).replaceCharacter(' ','-');
            if(sound>=silt::numPresets)p.randomise(sound-silt::numPresets),name="seed-"+juce::String(sound-silt::numPresets).paddedLeft('0',2);
            prepare(p);writeWav(folder.getChildFile(name+"-"+names[kind]+".wav"),run(p,12*48000,512,material));
        }
    }
    // Sweep into a fixed grain, so the expected output frequency is known at every analysed time.
    for(int mode:{0,1})for(int pitch:{24,48})
    {
        Proc p;set(p,plain);set(p,{{"pitch",(float)pitch},{"pitchMode",(float)mode},{"size",4000},{"density",1},
            {"trigger",2},{"position",4000},{"reverbOn",0},{"oversample",0}});prepare(p);
        Run r;r.side=[](int,int){return 0.0f;};
        // Log sweep, 30 Hz..20 kHz over 4 s, with a calibrated source reference beside each result.
        r.input=[](int i,int){const double t=i/48000.0,k=std::log(20000.0/30)/4;
            return (float)(std::sqrt(2.0)*std::pow(10.0,-18/20.0)*std::sin(2*pi*30*(std::expm1(k*std::min(t,4.0))/k+20000.0/30*std::max(0.0,t-4))));};
        if(mode==0 && pitch==24){Proc dry;set(dry,"mix",0);set(dry,"oversample",0);prepare(dry);writeWav(folder.getChildFile("input-sweep.wav"),run(dry,12*48000,512,r));}
        for(int t=4*48000;t<12*48000;t+=48000)r.events.push_back({t,juce::MidiMessage::noteOn(1,60,(juce::uint8)100)});
        writeWav(folder.getChildFile("sweep-"+juce::String(mode==0?"shift":"tape")+"-"+juce::String(pitch)+".wav"),run(p,12*48000,512,r));
    }
}
