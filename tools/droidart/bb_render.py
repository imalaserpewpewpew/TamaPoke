import math, os
from PIL import Image
WH=[(246,248,248),(234,237,236),(214,220,222),(192,200,204),(160,170,176),(134,146,153),(108,120,130)]
TE=[(74,174,163),(63,164,157),(43,126,133),(45,105,115),(36,84,83)]
OUT=(20,19,21)
CX,RX,RY=31.5,20.4,19.5; CY=18.6+19.8
def n3(v): l=math.sqrt(sum(c*c for c in v)); return tuple(c/l for c in v)
def dot(a,b): return sum(x*y for x,y in zip(a,b))
def cross(a,b): return (a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0])
HUBS=[(n3((-0.36,0.12,0.92)),35),(n3((0.93,0.2,0.3)),30),(n3((0.1,1.0,0.2)),34),(n3((0.4,-0.75,-0.5)),34),(n3((-0.2,-0.3,-0.93)),34),(n3((-0.95,-0.1,-0.2)),32)]
def shade(n,pal):
    L=(-0.55,-0.6,0.58); d=max(0,dot(n,L)); v=0.25+0.85*d-0.25*(1-n[2])
    return pal[int(round((1-min(1,max(0,v)))*(len(pal)-1)))]
def h3(ix,iy,iz,seed):
    h=(ix*73856093)^(iy*19349663)^(iz*83492791)^(seed*2654435761); h&=0xffffffff
    h=(h^(h>>13))*1274126177&0xffffffff; return (h&0xffff)/65535
def vnoise(p,sc,seed):
    x,y,z=(c*sc for c in p); ix,iy,iz=math.floor(x),math.floor(y),math.floor(z); fx,fy,fz=x-ix,y-iy,z-iz
    def s(t): return t*t*(3-2*t)
    fx,fy,fz=s(fx),s(fy),s(fz); r=0
    for dx in (0,1):
        for dy in (0,1):
            for dz in (0,1):
                w=(fx if dx else 1-fx)*(fy if dy else 1-fy)*(fz if dz else 1-fz)
                r+=w*h3(ix+dx,iy+dy,iz+dz,seed)
    return r
RUST=[(150,96,60),(122,74,46),(96,58,40),(176,120,72)]
DIRT=lambda c,t:tuple(int(a*(1-t)+b*t) for a,b in zip(c,(150,140,118)))
def grime(c,p,stage,dark=False):
    """stage 0 scrap, 1 repaired, 2 restored. p = body-frame coords"""
    if stage==2: return c
    if stage==0:
        c=DIRT(c,0.45 if not dark else 0.25)
        r=vnoise(p,5.0,1)*0.7+vnoise(p,12.0,2)*0.3
        if r>0.60: c=RUST[1] if r<0.66 else RUST[2] if r<0.72 else RUST[0]
        elif r>0.57: c=RUST[3]
        if vnoise(p,14,5)>0.86: c=(70,60,52)
    else:
        r=vnoise(p,5.0,1)*0.7+vnoise(p,12.0,2)*0.3
        if r>0.665: c=RUST[1] if r<0.71 else RUST[0]
        elif r>0.63: c=DIRT(c,0.5)
    return c
def rotz(v,a):
    c,s=math.cos(a),math.sin(a); return (v[0]*c-v[1]*s, v[0]*s+v[1]*c, v[2])
def render(rot=0.0, hdx=0, hdy=0, stage=2, eye=True, light=True, tint=None, size=64):
    im=Image.new('RGBA',(size,size),(0,0,0,0)); op=im.load()
    kind={}; info={}
    for y in range(size):
        for x in range(size):
            nx=(x+0.5-CX)/RX; ny=(y+0.5-CY)/RY; r2=nx*nx+ny*ny
            if r2>1:
                ox=(x+0.5-CX)/(RX+1); oy=(y+0.5-CY)/(RY+1)
                if ox*ox+oy*oy<=1: op[x,y]=OUT+(255,)
                continue
            n=(nx,ny,math.sqrt(1-r2)); b=rotz(n,-rot)   # body-frame coords
            k='w'
            for h,R in HUBS:
                a=math.degrees(math.acos(max(-1,min(1,dot(b,h)))))
                if a<R:
                    f=a/R
                    if f>0.80: k='ring'
                    elif f>0.72: k='ringedge'
                    else:
                        ux=n3(cross(h,(0,-1,0.0001))); uy=cross(h,ux)
                        lx=dot(b,ux); ly=dot(b,uy)
                        if f<0.18: k='core'
                        elif f<0.26: k='coreedge'
                        elif 0.3<f<0.64 and abs(ly)<0.26*(R/57.3)*2 and int((lx/(R/57.3))*10+40)%2==0: k='vent'
                        else: k='hub'
                    break
            kind[(x,y)]=k; info[(x,y)]=(n,b)
    for (x,y),k in kind.items():
        n,b=info[(x,y)]
        if k=='w': c=shade(n,WH)
        elif k=='hub': c=shade(n,WH[1:])
        elif k=='ring':
            c=shade(n,TE)
            if any(kind.get((x+dx,y+dy),'ring')=='w' for dx,dy in ((1,0),(-1,0),(0,1),(0,-1))): c=TE[3]
        elif k=='ringedge': c=TE[4]
        elif k=='vent': c=WH[5]
        elif k=='core': c=shade(n,WH[2:])
        else: c=TE[3]
        c=grime(c,b,stage,dark=k in('vent','ringedge','coreedge'))
        op[x,y]=c+(255,)
    # head
    hp=HEAD.load()
    for y in range(19):
        for x in range(64):
            c=hp[x,y]
            if c[3]<128: continue
            X,Y=x+hdx,y+2+hdy
            if not(0<=X<size and 0<=Y<size): continue
            col=c[:3]
            if not eye and (x,y) in ((34,12),): col=(36,60,66)
            if not eye and 26<=x<=30 and 10<=y<=14 and col[0]<130 and col[2]>col[0]+15: col=(30,40,44)
            if not light and (x,y) in ((37,9),(37,10)): col=(150,150,150)
            if stage<2 and col!=OUT and sum(col)>60:
                p=(x/9.0,y/9.0,0.3)
                col=grime(col,p,stage,dark=sum(col)<300)
            op[X,Y]=col+(255,)
    if tint:
        px=im.load()
        for y in range(size):
            for x in range(size):
                c=px[x,y]
                if c[3]: px[x,y]=(min(255,int(c[0]*0.55+230*0.45)),int(c[1]*0.55+90*0.45),int(c[2]*0.55+90*0.45),255)
    return im
def sparkle(im,x,y,big):
    p=im.load(); Y=(255,224,120); W=(255,250,220)
    pts=[(0,0)]+([(1,0),(-1,0),(0,1),(0,-1)] if big else [])+([(2,0),(-2,0),(0,2),(0,-2)] if big==2 else [])
    for dx,dy in pts: p[x+dx,y+dy]=(W if (dx,dy)==(0,0) else Y)+(255,)
    return im
