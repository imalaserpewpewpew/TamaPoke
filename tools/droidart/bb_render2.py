import math
from PIL import Image
from bb_render import shade, grime, vnoise, n3, dot, cross, rotz, sparkle, HUBS
OUT=(20,19,21)
# small BB: canvas 47x61 (same as the astromech, so the game draws both at 2x)
W,H=47,61
CX=23.5; RX,RY=15.0,14.5; CY=H-2-RY-0.5
HRX,HRY=10.0,9.0           # head dome
def palettes(body,acc):
    def ramp(st,n):
        out=[]
        for i in range(n):
            t=i/(n-1); k=min(len(st)-2,int(t*(len(st)-1))); f=t*(len(st)-1)-k
            out.append(tuple(int(st[k][j]+(st[k+1][j]-st[k][j])*f) for j in range(3)))
        return out
    WH=list(reversed(ramp(body,7)))   # light -> dark
    TE=list(reversed(ramp(acc,5)))
    return WH,TE
def render(rot=0.0,hdx=0,hdy=0,stage=2,eye=True,light=True,tint=False,scheme=None):
    WH,TE=palettes(*scheme)
    im=Image.new('RGBA',(W,H),(0,0,0,0)); op=im.load()
    kind={}; info={}
    for y in range(H):
        for x in range(W):
            nx=(x+0.5-CX)/RX; ny=(y+0.5-CY)/RY; r2=nx*nx+ny*ny
            if r2>1:
                ox=(x+0.5-CX)/(RX+1); oy=(y+0.5-CY)/(RY+1)
                if ox*ox+oy*oy<=1: op[x,y]=OUT+(255,)
                continue
            n=(nx,ny,math.sqrt(1-r2)); b=rotz(n,-rot); k='w'
            for h,R in HUBS:
                a=math.degrees(math.acos(max(-1,min(1,dot(b,h)))))
                if a<R:
                    f=a/R
                    if f>0.78: k='ring'
                    elif f>0.68: k='ringedge'
                    else:
                        ux=n3(cross(h,(0,-1,0.0001))); uy=cross(h,ux)
                        lx=dot(b,ux); ly=dot(b,uy)
                        if f<0.2: k='core'
                        elif f<0.3: k='coreedge'
                        elif 0.42<f<0.52: k='vent'
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
        op[x,y]=grime(c,b,stage,dark=k in('vent','ringedge','coreedge'))+(255,)
    # ---- head (procedural half dome) ----
    hcx=CX+hdx; hby=CY-RY+1.5+hdy      # bottom edge of head
    hk={}
    for y in range(H):
        for x in range(W):
            dx=(x+0.5-hcx)/HRX; dy=(y+0.5-hby)/HRY
            if dy>0.0 or dx*dx+dy*dy>1: continue
            hk[(x,y)]=(dx,dy)
    def isH(x,y): return (x,y) in hk
    for (x,y),(dx,dy) in hk.items():
        nz=math.sqrt(max(0,1-dx*dx-dy*dy)); n=(dx,dy,nz)
        row=hby-(y+0.5)      # height above bottom (px)
        c=shade(n,WH)
        if row<2.2: c=shade(n,TE) if row>1 else TE[3]       # rim band
        if 3.6<row<4.6 and abs(dx)<0.9: c=shade(n,WH[2:])   # seam line
        if row>6.6 and -0.1<dx<0.35: c=TE[2]                # top plate
        p=(dx,dy,0.5)
        c=grime(c,(dx*1.5,dy*1.5,0.7),stage)
        if not all(isH(x+a,y+b) for a,b in ((1,0),(-1,0),(0,-1))): c=OUT
        op[x,y]=c+(255,)
    # neck shadow under head
    for x in range(int(hcx-5),int(hcx+5)):
        y=int(hby)
        if 0<=y<H and op[x,y][3]: op[x,y]=(50,46,44,255)
    # main lens (front-left), small lens, light, antenna
    ex,ey=int(round(hcx-3+hdx*0)),int(round(hby-5))
    for a,b in ((0,0),(1,0),(0,1),(1,1),(-1,0),(-1,1),(0,-1),(1,-1),(0,2),(1,2),(2,0),(2,1)):
        op[ex+a,ey+b]=OUT+(255,)
    op[ex,ey]=((140,210,200) if eye else (40,50,54))+(255,)
    op[ex+1,ey+1]=((40,70,80) if eye else (30,36,40))+(255,)
    sx,sy=ex+5,ey+1
    op[sx,sy]=OUT+(255,); op[sx,sy+1]=OUT+(255,)
    if eye: op[sx,sy]=(120,190,180,255)
    op[int(hcx+5),int(hby-6)]=((240,140,70) if light else (140,130,120))+(255,)
    ax=int(hcx+4); ddx=(ax+0.5-hcx)/HRX; ytop=int(hby-math.sqrt(1-ddx*ddx)*HRY)
    for yy in range(ytop-3,ytop+1): op[ax,yy]=OUT+(255,)
    op[ax,ytop-4]=(200,190,170,255)
    if tint:
        for y in range(H):
            for x in range(W):
                c=op[x,y]
                if c[3]: op[x,y]=(min(255,int(c[0]*0.55+230*0.45)),int(c[1]*0.55+90*0.45),int(c[2]*0.55+90*0.45),255)
    return im
