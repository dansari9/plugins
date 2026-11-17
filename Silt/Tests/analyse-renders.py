#!/usr/bin/env python3
"""Measure 24-bit offline exports, input-relative isolated impulses, and tracked-sweep alias energy."""
import argparse,json
from pathlib import Path
import numpy as np
from scipy.io import wavfile
from scipy import signal,ndimage

def db(x):return float(20*np.log10(max(float(x),1e-15)))
def read(path):
    fs,x=wavfile.read(path)
    assert fs==48000 and x.ndim==2 and x.shape[1]==2
    assert x.dtype==np.int32, (path,x.dtype)
    return x.astype(np.float64)/2**31

def metrics(x,source):
    jump=np.abs(np.diff(x,axis=0))
    source_jump=float(np.max(np.abs(np.diff(source,axis=0))))
    # A click is an isolated broadband spike, not an ordinary pitched waveform's large derivative.
    # Compare against 10 ms local derivative energy and the strongest attack in the source.
    local=np.sqrt(ndimage.uniform_filter1d(jump*jump,481,axis=0,mode='nearest'))
    threshold=np.maximum(.035,np.maximum(12*local,source_jump*1.5))
    high=signal.sosfilt(signal.butter(3,10000,fs=48000,btype='highpass',output='sos'),x,axis=0)
    high_local=np.sqrt(ndimage.uniform_filter1d(high*high,481,axis=0,mode='nearest'))
    spikes=(np.abs(high)>np.maximum(.015,12*high_local))
    candidates=np.any(jump>threshold,axis=1)
    # Both a sample step and a coincident high-pass impulse are required; coalesce 1 ms neighbours.
    coincident=ndimage.maximum_filter1d(np.any(spikes,axis=1),9)[:-1]
    indices=np.flatnonzero(candidates & coincident)
    count=int(np.sum(np.diff(np.r_[-1000,indices])>48))
    rms=float(np.sqrt(np.mean(x*x)));peak=float(np.max(np.abs(x)))
    dc=float(np.max(np.abs(np.mean(x,axis=0))))
    return dict(peak=db(peak),rms=db(rms),dc=db(dc),crest=db(peak/rms),max_step=float(jump.max()),
                hp_peak=db(np.max(np.abs(high))),clicks=count,click_times=(indices[::max(1,len(indices)//10)]/48000).tolist(),
                finite=bool(np.isfinite(x).all()))

def alias(path):
    mode=path.stem.split('-')[1];pitch=int(path.stem.split('-')[-1]);ratio=2**(pitch/12)
    x=read(path).mean(axis=1)
    # Analyse the first fired grain, before any delayed feedback can return. Window support and the
    # pitch-scaled shift taps define an expected frequency interval. Bin padding covers window main lobes.
    n=512;hop=64;f,t,z=signal.stft(x,48000,nperseg=n,noverlap=n-hop,boundary=None,padded=False)
    k=np.log(20000/30)/4
    start=4.06 if mode=='shift' else 4+.02/ratio
    end=7.9 if mode=='shift' else 4+3.98/ratio
    use=(t>start)&(t<end)
    t=t[use];power=np.abs(z[:,use])**2
    centre=t-4 if mode=='shift' else (t-4)*ratio
    half=n/2/48000*(1 if mode=='shift' else ratio)
    low=ratio*30*np.exp(k*np.maximum(0,centre-half-(.04*max(1,ratio-1) if mode=='shift' else 0)))
    high=ratio*30*np.exp(k*(centre+half))
    padding=4*48000/n
    expected=(f[:,None]>=np.maximum(0,low-padding))&(f[:,None]<=high+padding)
    above=f[:,None]>high+padding
    valid=high<20000
    stop=low>26000
    reference=np.mean(np.sum(power[:,valid],axis=0)) if valid.any() else 1e-30
    total=power.sum()
    return dict(mode=mode,pitch=pitch,nonharmonic_db=float(10*np.log10(max(power[~expected].sum()/total,1e-30))),
                above_band_db=float(10*np.log10(max(power[above].sum()/total,1e-30))),
                folded_stop_db=float(10*np.log10(max(np.mean(np.sum(power[:,stop],axis=0))/reference,1e-30))) if stop.any() else None)

def main():
    parser=argparse.ArgumentParser();parser.add_argument('folder',type=Path);parser.add_argument('--output',type=Path,required=True);args=parser.parse_args()
    rows=[];fail=[]
    for path in sorted(args.folder.glob('*.wav')):
        if path.stem.startswith(('input-','sweep-')):continue
        source=read(args.folder/('input-'+path.stem.split('-')[-1]+'.wav'))
        m=metrics(read(path),source);m['file']=path.name;rows.append(m)
        if not m['finite'] or m['peak']>-.5 or m['rms']>-14 or m['dc']>=-60 or m['clicks']:fail.append(m)
    aliases=[alias(path) for path in sorted(args.folder.glob('sweep-*.wav'))]
    alias_fail=[m for m in aliases if m['nonharmonic_db']>-45 or m['above_band_db']>-50 or m['folded_stop_db'] is None or m['folded_stop_db']>-80]
    report='''# Offline render measurements

48 kHz stereo PCM24, 12 seconds, starting from an empty capture. Inputs are normalised to −18 dBFS RMS. Peak/RMS/DC cover the whole file; DC is the larger absolute channel mean. Crest = peak/RMS. No limiter. Click flags require a sample jump exceeding both the source attack and 12× local derivative RMS, plus an isolated >10 kHz impulse; large periodic pitched waveforms alone do not count. HP peak is diagnostic, not a click count.

| Render | Peak dBFS | RMS dBFS | DC dBFS | Crest dB | Max step | HP peak dBFS | Clicks |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
'''
    for m in rows:report+=f"| {m['file']} | {m['peak']:.3f} | {m['rms']:.3f} | {m['dc']:.3f} | {m['crest']:.3f} | {m['max_step']:.6f} | {m['hp_peak']:.3f} | {m['clicks']} |\n"
    report+='\n## Pitched sine sweeps\n\n30 Hz–20 kHz logarithmic source sweep over 4 seconds, captured then read at +24/+48 st in both modes. The first grain is analysed before delayed feedback returns. STFT expected bands include the exact sweep slope, frame support, pitch-scaled tap support, and four Hann main-lobe bins. Nonharmonic includes all energy outside that interval; above-band counts energy above its upper edge. Folded stop compares output when the expected band is wholly above 26 kHz with the mean in-band output below 20 kHz. These metrics distinguish alias foldback from intended tap/window sidebands.\n\n| Mode | Pitch st | Nonharmonic dBc | Above band dBc | Folded stop dB |\n| --- | ---: | ---: | ---: | ---: |\n'
    for m in aliases:report+=f"| {m['mode']} | {m['pitch']} | {m['nonharmonic_db']:.2f} | {m['above_band_db']:.2f} | {m['folded_stop_db']:.2f} |\n"
    report+=f'\n{len(rows)} program renders; {len(fail)} level/DC/click failures. {len(alias_fail)} sweep failures (bounds: nonharmonic ≤ −45 dBc, above-band ≤ −50 dBc, folded stop ≤ −80 dB).\n'
    args.output.write_text(report);args.output.with_suffix('.json').write_text(json.dumps(dict(renders=rows,aliases=aliases,failures=fail,alias_failures=alias_fail),indent=2)+'\n')
    print(report[-1300:]);print('Failures:',[(m['file'],m['clicks'],round(m['dc'],2)) for m in fail])
    return bool(fail or alias_fail)
if __name__=='__main__':raise SystemExit(main())
