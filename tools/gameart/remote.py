from PIL import Image
import math
N=26; c=(N-1)/2; R=11.2
L=(-0.55,-0.65,0.52); ln=math.sqrt(sum(v*v for v in L)); L=tuple(v/ln for v in L)
OUT=(0x1b,0x1b,0x25)
def remote(glow=0.0):
    im=Image.new('RGBA',(N,N),(0,0,0,0)); p=im.load()
    inside=[[False]*N for _ in range(N)]
    for y in range(N):
        for x in range(N):
            dx,dy=(x-c)/R,(y-c)/R; d2=dx*dx+dy*dy
            if d2>1: continue
            inside[y][x]=True
            nz=math.sqrt(1-d2); n=(dx,dy,nz)
            diff=max(0,sum(a*b for a,b in zip(n,L)))
            base=(150,156,168)
            if abs(y-c)<1.2: base=(88,92,104)          # equator seam
            sh=0.35+0.75*diff
            col=[min(255,int(v*sh)) for v in base]
            # specular
            rx=2*diff*n[2]-L[2]
            spec=max(0,rx)**18
            col=[min(255,int(v+255*spec*0.9)) for v in col]
            p[x,y]=tuple(col)+(255,)
    # emitter ports (dark rings, red centres); the front one glows before firing
    ports=[(c-6,c-6),(c+3.5,c-7.5),(c+7.5,c+3.5),(c-4,c+6),(c+2,c+8.6)]
    for i,(px,py) in enumerate(ports):
        for y in range(N):
            for x in range(N):
                d=math.hypot(x-px,y-py)
                big=(i==4)
                if inside[y][x] and d<(2.2 if big else 1.6): p[x,y]=(40,42,50,255)
                if inside[y][x] and d<(1.2 if big else 0.75):
                    g=glow if big else 0.3
                    p[x,y]=(int(140+115*g),int(30+60*g),int(30+40*g),255)
    # small fins on top
    for x0 in (0,N-2):   # emitter nubs on the sides, on the seam
        for y in range(int(c)-1,int(c)+2):
            for x in range(x0,x0+2):
                if not inside[y][x]: p[x,y]=(110,114,126,255); inside[y][x]=True
    # outline
    q=im.copy().load()
    for y in range(N):
        for x in range(N):
            if q[x,y][3]: continue
            if any(0<=x+a<N and 0<=y+b<N and q[x+a,y+b][3] for a,b in ((1,0),(-1,0),(0,1),(0,-1))): p[x,y]=OUT+(255,)
    return im
if __name__=='__main__':
    a=remote(0); b=remote(1)
    big=Image.new('RGBA',(N*8*2+20,N*8),(118,118,118,255))
    big.alpha_composite(a.resize((N*8,N*8),Image.NEAREST),(0,0)); big.alpha_composite(b.resize((N*8,N*8),Image.NEAREST),(N*8+20,0))
    big.save('remote_preview.png')
    print(len(set(a.getdata())))
