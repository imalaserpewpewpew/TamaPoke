import math, os, sys
sys.path.insert(0,os.path.dirname(os.path.abspath(__file__)))
from PIL import Image
from bb_render import grime, sparkle
W,H=47,61
OUT=(32,26,22)
TOP=[(232,216,180),(222,204,166)]; FRONT=[(200,180,142),(188,168,132),(176,156,122)]; SIDE=[(150,132,104),(138,120,94)]
EDGE=(112,96,76); EDGE_HI=(240,228,198)
BLUE=[(112,142,166),(84,112,138),(64,88,112)]
SCR=(26,30,34); SCR2=(40,46,52)
def clamp(v): return max(0,min(255,int(v)))
class Canvas:
    def __init__(s): s.px={} ; s.tag={}
    def put(s,x,y,c,t='b'):
        x,y=int(x),int(y)
        if 0<=x<W and 0<=y<H: s.px[(x,y)]=c; s.tag[(x,y)]=t
    def get(s,x,y): return s.px.get((int(x),int(y)))
def tex(x,y,c,amt=6):
    # subtle weathering texture
    h=((x*928371+y*123457)^(x*y*31))&255
    d=(h%5-2)*amt/2
    return tuple(clamp(v+d) for v in c)
def box(cv,fx0,fx1,y0,y1,d,r,front=FRONT,side=SIDE,top=TOP,toprow=True,ti=0,bi=0):
    """ti/bi: inset per side at the top/bottom row -> tapered, angular box"""
    def ins(y): 
        t=(y-y0)/max(1,y1-1-y0); return int(round(ti*(1-t)+bi*t))
    a0,a1=fx0+ti,fx1-ti
    for k in range(1,r+1):
        y=y0-k; sh=int(round(k*d/r))
        for x in range(a0+sh,a1+sh): cv.put(x,y,tex(x,y,top[0] if k<r else top[1]))
    for y in range(y0,y1):
        t=(y-y0)/max(1,y1-y0-1); c=front[0] if t<0.33 else front[1] if t<0.7 else front[2]
        i=ins(y)
        for x in range(fx0+i,fx1-i): cv.put(x,y,tex(x,y,c))
        # side face for this row
        for x in range(fx1-i,fx1-i+d):
            off=int(round((x-(fx1-i)+0.5)*r/d))
            cv.put(x,y-off,tex(x,y-off,side[0] if x<fx1-i+d-2 else side[1]))
        # fill side gap above (top part of side face)
    for x in range(a1,a1+d):
        off=int(round((x-a1+0.5)*r/d))
        for y in range(y0-off,y0): cv.put(x,y,tex(x,y,side[0] if x<a1+d-2 else side[1]))
    for x in range(a0,a1): cv.put(x,y0,EDGE_HI)
    if d>0:
        for y in range(y0,y1): cv.put(fx1-ins(y),y,EDGE)
def outline(cv):
    keys=set(cv.px)
    for (x,y) in list(keys):
        for dx,dy in ((1,0),(-1,0),(0,1),(0,-1)):
            if (x+dx,y+dy) not in keys and 0<=x+dx<W and 0<=y+dy<H:
                cv.px[(x+dx,y+dy)]=OUT; cv.tag[(x+dx,y+dy)]='o'
def render(stage=2, bob=0, stepL=0, stepR=0, leds=(1,1,1), screen_on=True, tint=False, mirror=False, ant2=True):
    cv=Canvas(); G=60  # ground row (outline goes to 60)
    b=bob
    # feet (back foot first)
    fy=G-5
    for (fx,lift) in ((25,stepR),(12,stepL)):
        box(cv,fx,fx+10,fy-lift,G-lift,0,0,front=[(176,156,122)]*3,side=SIDE,top=TOP)
        for x in range(fx+1,fx+9,3): cv.put(x,G-1-lift,EDGE)
    # bellows legs
    for (lx,lift) in ((27,stepR),(14,stepL)):
        for y in range(49+b,fy-lift):
            c=(96,88,78) if (y%2==0) else (140,128,112)
            for x in range(lx+1,lx+5): cv.put(x,y,c)
    # lower box (slight taper)
    ly0,ly1=30+b,49+b
    box(cv,9,38,ly0,ly1,0,0,ti=0,bi=1)
    # upper box
    uy0,uy1=8+b,ly0-3
    box(cv,9,38,uy0,uy1,0,0,ti=1,bi=0)
    # belt ledge
    box(cv,7,40,ly0-3,ly0,0,0,front=[(212,194,156)]*3,side=[(176,156,124),(160,142,112)],top=TOP)
    for x in range(7,40): cv.put(x,ly0-1,EDGE)
    # --- details: upper front screen
    sx0,sx1,sy0,sy1=15,33,uy0+3,uy0+15
    for y in range(sy0,sy1):
        for x in range(sx0,sx1):
            edge = x in (sx0,sx1-1) or y in (sy0,sy1-1)
            corner=(x in (sx0,sx1-1)) and (y in (sy0,sy1-1))
            if corner: continue
            cv.put(x,y,(150,132,104) if edge else SCR)
    for x in range(sx0-1,sx1+1):
        cv.put(x,sy0-1,EDGE)
    for y in range(sy0,sy1): cv.put(sx0-1,y,EDGE)
    # frame highlight bottom
    for x in range(sx0+1,sx1-1): cv.put(x,sy1,EDGE_HI)
    # screen interior: LEDs + bars
    if screen_on:
        ledc=[(250,190,70),(230,70,50),(250,190,70)]
        for i,on in enumerate(leds):
            c=ledc[i] if on else (70,56,40)
            cv.put(sx0+3,sy0+2+i*2,c); cv.put(sx0+4,sy0+2+i*2,c)
        for y in range(sy0+2,sy1-2): cv.put(sx0+8,y,(120,124,128))
        for x in range(sx0+9,sx1-3): cv.put(x,sy0+5,(120,124,128))
        cv.put(sx0+11,sy0+3,(90,150,200)); cv.put(sx0+12,sy0+3,(70,120,170))
        for y in range(sy0+7,sy1-2,2):
            for x in range(sx0+10,sx1-3): cv.put(x,y,SCR2)
    # top: small cap + panel
    pass
    # lower front: vent lines, seam, port, blue mark
    for y in range(ly0+3,ly0+8,2):
        for x in range(23,31): cv.put(x,y,EDGE)
    for y in range(ly0+1,ly1-1): cv.put(16,y,EDGE)
    for x,y in ((19,ly0+5),(20,ly0+5),(19,ly0+6),(20,ly0+6)): cv.put(x,y,(60,52,44))
    for y in range(ly0+10,ly0+17):
        for x in range(23,26): cv.put(x,y,(178,158,124) if x>23 else EDGE)
    for x in range(23,30): cv.put(x,ly0+17,EDGE)
    for x in range(10,14): cv.put(x,ly0+2,BLUE[1]); cv.put(x,ly0+3,BLUE[1])
    for (x,y) in ((10,uy0),(36,uy0),(7,ly0-3),(39,ly0-3),(10,ly1-1),(36,ly1-1)):
        cv.px.pop((x,y),None); cv.tag.pop((x,y),None)
    outline(cv)
    # antennas (after outline so they stay thin)
    for (axx,ln,on) in ((14,3,True),(17,5,ant2)):
        if not on: continue
        top=uy0-1-ln
        for y in range(top,uy0-1): cv.put(axx,y,(70,64,56),'o')
        cv.put(axx,top-1,(200,190,170),'o')
    # stage weathering + build image
    im=Image.new('RGBA',(W,H),(0,0,0,0)); p=im.load()
    for (x,y),c in cv.px.items():
        t=cv.tag[(x,y)]
        if t=='b' and stage<2 and c not in (SCR,SCR2) and sum(c)>200:
            c=grime(c,(x/16.0,y/16.0,0.4),stage,dark=sum(c)<400)
        p[x,y]=c+(255,)
    if stage==0:  # cracked screen, one antenna gone handled by caller
        for i,(x,y) in enumerate(((sx0+6,sy0+2),(sx0+7,sy0+3),(sx0+7,sy0+4),(sx0+8,sy0+5),(sx0+9,sy0+6),(sx0+9,sy0+7),(sx0+10,sy0+8))):
            p[x,y]=(150,150,150,255)
    if tint:
        for y in range(H):
            for x in range(W):
                c=p[x,y]
                if c[3]: p[x,y]=(clamp(c[0]*0.55+230*0.45),clamp(c[1]*0.55+90*0.45),clamp(c[2]*0.55+90*0.45),255)
    if mirror: im=im.transpose(Image.FLIP_LEFT_RIGHT)
    return im
if __name__=='__main__':
    from tpk import load_builtin, parse, frame_img
    pal,acts=parse(load_builtin('/home/claude/work/TamaPoke/builtin_sprites.h','BUILTIN_P003'))
    w,h,ms,frs=acts[0]; r2=frame_img(pal,w,h,frs[0])
    pal6,acts6=parse(load_builtin('/home/claude/work/TamaPoke/builtin_sprites.h','BUILTIN_P006'))
    w6,h6,_,f6=acts6[0]; bb=frame_img(pal6,w6,h6,f6[0])
    cells=[r2,bb,render(stage=0,ant2=False),render(stage=1),render(stage=2)]
    S=5; sh=Image.new('RGBA',(len(cells)*(47*S+12)+12,61*S+24),(226,188,130,255))
    for i,c in enumerate(cells): sh.alpha_composite(c.resize((c.width*S,c.height*S),Image.NEAREST),(12+i*(47*S+12),12))
    sh.save('/home/claude/work/gonk/gonk_try1.png')
