from PIL import Image, ImageDraw
import math
from props import outline
def shade_disc(im,cx,cy,r,base,L=(-0.55,-0.65,0.52),spec=0.8):
    p=im.load()
    for y in range(im.height):
        for x in range(im.width):
            dx,dy=(x-cx)/r,(y-cy)/r; d2=dx*dx+dy*dy
            if d2>1: continue
            nz=math.sqrt(1-d2); diff=max(0,dx*L[0]+dy*L[1]+nz*L[2])
            col=[min(255,int(v*(0.4+0.75*diff))) for v in base]
            s=max(0,2*diff*nz-L[2])**16
            p[x,y]=tuple(min(255,int(v+255*s*spec)) for v in col)+(255,)
def sensor():
    im=Image.new('RGBA',(16,16),(0,0,0,0)); shade_disc(im,7.5,7.5,7,(150,156,170)); shade_disc(im,7.5,7.5,4.2,(70,150,240),spec=1.0); return outline(im)
def servo():
    im=Image.new('RGBA',(16,16),(0,0,0,0)); d=ImageDraw.Draw(im)
    for a in range(8):
        ang=a*math.pi/4; x,y=7.5+6.3*math.cos(ang),7.5+6.3*math.sin(ang); d.rectangle([x-1.3,y-1.3,x+1.3,y+1.3],fill=(130,134,146,255))
    shade_disc(im,7.5,7.5,5.4,(160,164,176)); d.ellipse([5.5,5.5,9.5,9.5],fill=(50,52,60,255)); return outline(im)
def plating():
    im=Image.new('RGBA',(16,16),(0,0,0,0)); d=ImageDraw.Draw(im)
    d.rounded_rectangle([1,2,14,13],2,fill=(110,130,160,255)); d.line([2,3,13,3],fill=(180,196,220,255)); d.line([2,3,2,12],fill=(180,196,220,255)); d.line([13,4,13,12],fill=(70,84,108,255)); d.line([3,12,13,12],fill=(70,84,108,255))
    for (x,y) in ((4,5),(11,5),(4,10),(11,10)): d.point((x,y),fill=(220,230,240,255))
    return outline(im)
def coupling():
    im=Image.new('RGBA',(16,16),(0,0,0,0)); d=ImageDraw.Draw(im)
    d.rectangle([4,1,5,4],fill=(200,204,212,255)); d.rectangle([10,1,11,4],fill=(200,204,212,255))
    d.rounded_rectangle([2,4,13,13],2,fill=(230,180,40,255)); d.line([3,5,3,12],fill=(255,226,120,255)); d.line([12,5,12,12],fill=(170,120,20,255))
    d.polygon([(8,6),(6,9),(8,9),(7,12),(10,8),(8,8)],fill=(60,50,30,255)); return outline(im)
def holo():
    im=Image.new('RGBA',(16,16),(0,0,0,0)); d=ImageDraw.Draw(im)
    d.polygon([(7.5,2),(2,11),(13,11)],fill=(120,200,255,160)); d.line([7,3,4,10],fill=(200,240,255,255)); d.line([8,3,11,10],fill=(160,220,255,255))
    d.ellipse([2,10,13,14],fill=(150,156,170,255)); d.ellipse([4,10,11,12],fill=(80,170,255,255))
    q=im.load()
    for y in range(16):
        for x in range(16):
            if q[x,y][3] and q[x,y][3]<255: q[x,y]=q[x,y][:3]+(255,)
    return outline(im)
def mission_icon():
    im=Image.new('RGBA',(16,16),(0,0,0,0)); d=ImageDraw.Draw(im)
    d.polygon([(1,3),(5,1),(10,3),(14,1),(14,13),(10,15),(5,13),(1,15)],fill=(232,208,160,255))
    d.line([5,1,5,13],fill=(196,170,120,255)); d.line([10,3,10,15],fill=(196,170,120,255))
    for (x,y) in ((3,11),(4,9),(6,8),(8,8),(9,6)): d.point((x,y),fill=(120,90,60,255))
    d.line([10,3,13,6],fill=(220,50,40,255),width=1); d.line([13,3,10,6],fill=(220,50,40,255),width=1)
    return outline(im)
if __name__=='__main__':
    parts=[sensor(),servo(),plating(),coupling(),holo(),mission_icon()]
    sheet=Image.new('RGBA',(6*90,80),(118,118,118,255))
    for i,p in enumerate(parts): sheet.alpha_composite(p.resize((80,80),Image.NEAREST),(i*90,0))
    sheet.save('parts_preview.png')
