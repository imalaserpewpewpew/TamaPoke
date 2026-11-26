"""Builds the TamaDroid Gonk power droid sprites (dex 7-9, TPK2), canvas 47x61 like the astromech."""
import sys, os
sys.path.insert(0,os.path.dirname(os.path.abspath(__file__)))
from gonk_render import render
from bb_render import sparkle
from tpk import pack
from PIL import Image
OUTDIR=os.environ.get('OUT','.')
def frames_for(st):
    a2=(st>0)
    def R(**k): return render(stage=st,ant2=a2,**k)
    A={}
    A[0]=([R(leds=(1,1,1)),R(leds=(1,0,1)),R(leds=(0,1,1)),R(leds=(1,1,0))],[500,250,350,500])
    walk=[dict(stepL=1,bob=-1),dict(),dict(stepR=1,bob=-1),dict()]
    A[1]=([R(**w) for w in walk],[150]*4)
    A[2]=([R(**w) for w in walk[2:]+walk[:2]],[150]*4)
    A[3]=([R(bob=1,leds=(0,0,0)),R(bob=1,leds=(0,1,0))],[900,900])
    A[4]=([R(leds=(1,0,0)),sparkle(R(leds=(1,1,0)),6,44,1),sparkle(R(leds=(1,1,1)),6,42,2)],[250]*3)
    A[5]=([R(tint=True),sparkle(R(leds=(0,1,0)),36,20,2)],[90,90])
    return A
for st,dex in ((0,7),(1,8),(2,9)):
    acts=[(aid,frs,ms) for aid,(frs,ms) in sorted(frames_for(st).items())]
    blob=pack(acts); open(os.path.join(OUTDIR,'p%03d.bin'%dex),'wb').write(blob); print(dex,len(blob))
    S=4; W,H=acts[0][1][0].size
    sh=Image.new('RGBA',(4*(W*S+4),6*(H*S+4)),(226,188,130,255))
    for r,(aid,frs,ms) in enumerate(acts):
        for c,im in enumerate(frs): sh.alpha_composite(im.resize((W*S,H*S),Image.NEAREST),(c*(W*S+4),r*(H*S+4)))
    sh.save(os.path.join(OUTDIR,'sheet_%d.png'%dex))
