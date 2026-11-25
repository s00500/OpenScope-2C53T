#!/usr/bin/env python3
import os, subprocess, json, time, statistics, cmath, math
OS=os.environ.get("OPENSCOPE", "openscope")
def sh(*a): return subprocess.run(a,capture_output=True,text=True,timeout=60,check=True).stdout
def wave(path):
    sh("rigol","waveform","-c","1","-o",path)
    t=[];v=[]
    for line in open(path).read().splitlines()[1:]:
        a,b=line.split(","); t.append(float(a)); v.append(float(b))
    return t,v
def spectrum_peak(t,v):
    n=len(v); dt=t[1]-t[0]; m=statistics.mean(v)
    best=(0,0)
    for k in range(1,n//2):
        s=sum((v[i]-m)*cmath.exp(-2j*math.pi*k*i/n) for i in range(n))
        if abs(s)>best[1]: best=(k/(n*dt),abs(s))
    return best[0]
sh("rigol","acquire","--type","normal")
rows=[]
for state,cmd,off in [("off","siggen off",-0.25),("dc15","siggen wave 9 amp 15 on",-1.80)]:
    sh(OS,"shell",cmd)
    for tb,bwl in [("1us","20M"),("1us","off"),("100us","20M")]:
        sh("rigol","channel","1","--scale","100mV","--offset=%.2fV"%off,"--coupling","dc","--bwl",bwl)
        sh("rigol","trigger","--source","1","--sweep","auto","--level","%.2fV"%(-off),"--coupling","hfreject")
        sh("rigol","timebase","--scale",tb); time.sleep(1.5)
        t,v=wave("noise-%s-%s-%s.csv"%(state,tb,bwl))
        sd=statistics.pstdev(v); pp=max(v)-min(v)
        pk=spectrum_peak(t,v) if tb=="1us" else None
        rows.append({"state":state,"tb":tb,"bwl":bwl,"std_v":sd,"pp_v":pp,"peak_hz":pk,"mean_v":statistics.mean(v)})
        print("%-5s tb %-5s bwl %-3s  mean %.3f  std %.1f mV  pp %.0f mV  peak %s"%(state,tb,bwl,statistics.mean(v),sd*1e3,pp*1e3,("%.2f MHz"%(pk/1e6)) if pk else "-"))
json.dump(rows,open("noise-%s.json"%time.strftime("%H%M%S"),"w"),indent=1)
