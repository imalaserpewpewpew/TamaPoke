"""Builds the TamaDroid BB unit sprites (dex 4-6, TPK2) at the astromech's canvas size.
Canvas stays 47x61 so the game draws it at 2x like the astromech (the BB is drawn smaller inside)."""
import math, sys, os
sys.path.insert(0,os.path.dirname(os.path.abspath(__file__)))
from bb_render2 import render
from bb_render import sparkle
from tpk import pack
from PIL import Image
OUTDIR=os.environ.get('OUT','.')
SCHEME=([(90,78,64),(162,142,116),(214,196,166),(238,226,204)],[(30,40,56),(52,68,92),(80,102,130),(116,138,164)])  # tan + steel blue
def R(**k): return render(scheme=SCHEME,**k)
def frames_for(st):
    A={}
    A[0]=([R(stage=st),R(stage=st,hdy=-1),R(stage=st,hdx=-1),R(stage=st,hdx=1)],[500,250,350,500])
    step=math.radians(45)
    A[1]=([R(rot=-step*k,hdx=-1,stage=st) for k in range(8)],[110]*8)
    A[2]=([R(rot=step*k,hdx=1,stage=st) for k in range(8)],[110]*8)
    A[3]=([R(stage=st,hdy=1,eye=False),R(stage=st,hdy=1,eye=False,light=False)],[900,900])
    A[4]=([R(stage=st),sparkle(R(stage=st),8,46,1),sparkle(R(stage=st),8,44,2)],[250]*3)
    A[5]=([R(stage=st,tint=True),sparkle(R(stage=st,hdx=1),33,36,2)],[90,90])
    return A
for st,dex in ((0,4),(1,5),(2,6)):
    A=frames_for(st)
    acts=[(aid,frs,ms) for aid,(frs,ms) in sorted(A.items())]
    blob=pack(acts)
    open(os.path.join(OUTDIR,'p%03d.bin'%dex),'wb').write(blob); print(dex,len(blob))
    S=4; W,H=acts[0][1][0].size
    sh=Image.new('RGBA',(8*(W*S+4),6*(H*S+4)),(226,188,130,255))
    for r,(aid,frs,ms) in enumerate(acts):
        for c,im in enumerate(frs): sh.alpha_composite(im.resize((W*S,H*S),Image.NEAREST),(c*(W*S+4),r*(H*S+4)))
    sh.save(os.path.join(OUTDIR,'sheet_%d.png'%dex))
