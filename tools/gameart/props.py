from PIL import Image, ImageDraw
import math
OUT=(0x1b,0x1b,0x25)
def outline(im):
    p=im.load(); q=im.copy().load(); W,H=im.size
    for y in range(H):
        for x in range(W):
            if q[x,y][3]: continue
            if any(0<=x+a<W and 0<=y+b<H and q[x+a,y+b][3] for a,b in ((1,0),(-1,0),(0,1),(0,-1))): p[x,y]=OUT+(255,)
    return im
def crate():
    W,H=40,32; im=Image.new('RGBA',(W,H),(0,0,0,0)); d=ImageDraw.Draw(im)
    body=(150,112,70); hi=(196,154,98); dk=(104,74,44); metal=(150,156,168); mhi=(206,210,220); mdk=(86,90,102)
    d.rectangle([1,1,W-2,H-2],fill=body)
    for y in range(4,H-3,6): d.line([2,y,W-3,y],fill=dk)          # plank seams
    d.line([2,2,W-3,2],fill=hi); d.line([2,2,2,H-3],fill=hi)          # light edge
    d.line([W-3,2,W-3,H-3],fill=dk); d.line([2,H-3,W-3,H-3],fill=dk)
    for x0 in (8,W-12):                                               # metal straps
        d.rectangle([x0,1,x0+3,H-2],fill=metal); d.line([x0,1,x0,H-2],fill=mhi); d.line([x0+3,1,x0+3,H-2],fill=mdk)
        for y in (5,H-6): d.rectangle([x0+1,y,x0+2,y+1],fill=mdk)  # rivets
    for (x,y) in ((1,1),(W-4,1),(1,H-4),(W-4,H-4)):                  # corner caps
        d.rectangle([x,y,x+2,y+2],fill=metal); d.point((x,y),fill=mhi)
    d.rectangle([W//2-5,H//2-3,W//2+4,H//2+2],fill=(230,190,60)); d.rectangle([W//2-4,H//2-2,W//2+3,H//2+1],fill=(40,40,48))  # hazard label
    d.line([W//2-3,H//2-1,W//2+2,H//2-1],fill=(230,190,60))
    return outline(im)
def pulley():
    N=16; im=Image.new('RGBA',(N,N),(0,0,0,0)); p=im.load(); c=(N-1)/2
    for y in range(N):
        for x in range(N):
            r=math.hypot(x-c,y-c)
            if r<=6.8:
                v=170 if r>4.5 else (110 if r>2.2 else 200)
                shade=1.0-0.25*((x-c)+(y-c))/7
                p[x,y]=(int(v*shade),int((v+4)*shade),int((v+14)*shade),255)
    return outline(im)
LAMP=[((255,88,72),(110,34,32)),((255,222,88),(110,92,30)),((88,232,120),(30,92,44)),((104,184,255),(32,60,110))]
def lamp(i,on):
    N=30; im=Image.new('RGBA',(N,N),(0,0,0,0)); p=im.load(); c=(N-1)/2; R=13.2
    lit,dim=LAMP[i]; base=lit if on else dim
    L=(-0.5,-0.65,0.57)
    for y in range(N):
        for x in range(N):
            dx,dy=(x-c)/R,(y-c)/R; d2=dx*dx+dy*dy
            if d2>1: continue
            if d2>0.78:   # metal bezel ring
                v=int(150+60*(-dx*0.6-dy*0.7)); p[x,y]=(v,v+4,v+12,255); continue
            nz=math.sqrt(1-d2/0.78); n=(dx/0.88,dy/0.88,nz)
            diff=max(0,sum(a*b for a,b in zip(n,L)))
            k=(0.55+0.6*diff) if on else (0.45+0.45*diff)
            col=[min(255,int(v*k)) for v in base]
            spec=max(0,2*diff*n[2]-L[2])**14
            col=[min(255,int(v+255*spec*(0.95 if on else 0.4))) for v in col]
            if on: col=[min(255,v+30) for v in col]
            p[x,y]=tuple(col)+(255,)
    return outline(im)
def chip():
    W,H=28,28; im=Image.new('RGBA',(W,H),(0,0,0,0)); d=ImageDraw.Draw(im)
    for i in range(5):   # legs
        x=6+i*4
        d.rectangle([x,0,x+1,4],fill=(200,204,212)); d.rectangle([x,H-5,x+1,H-1],fill=(200,204,212))
        d.rectangle([0,x,4,x+1],fill=(200,204,212)); d.rectangle([W-5,x,W-1,x+1],fill=(200,204,212))
    d.rectangle([4,4,W-5,H-5],fill=(40,42,50)); d.line([5,5,W-6,5],fill=(80,84,96)); d.line([5,5,5,H-6],fill=(80,84,96))
    d.ellipse([7,7,9,9],fill=(90,94,106))
    return outline(im)
if __name__=='__main__':
    parts=[crate().resize((160,128),Image.NEAREST),pulley().resize((64,64),Image.NEAREST),chip().resize((112,112),Image.NEAREST)]+[lamp(i,o).resize((90,90),Image.NEAREST) for i in range(4) for o in (False,True)]
    Wt=sum(p.width for p in parts)+10*len(parts); sheet=Image.new('RGBA',(Wt,140),(118,118,118,255)); x=0
    for p in parts: sheet.alpha_composite(p,(x,0)); x+=p.width+10
    sheet.save('props_preview.png')
