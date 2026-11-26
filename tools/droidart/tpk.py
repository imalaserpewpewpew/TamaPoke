import re, struct
from PIL import Image
def load_builtin(path, name):
    s=open(path).read()
    i=s.index('static const uint8_t %s['%name); j=s.index('};',i)
    body=s[s.index('{',i)+1:j]
    return bytes(int(x) for x in re.findall(r'\d+',body))
def parse(b):
    assert b[:4]==b'TPK2'
    na=b[4]; pc=struct.unpack('<H',b[5:7])[0]
    pal=[struct.unpack('<H',b[7+2*i:9+2*i])[0] for i in range(pc)]
    p=7+2*pc; acts={}
    for _ in range(na):
        aid,w,h,nf=b[p:p+4]; p+=4
        ms=[struct.unpack('<H',b[p+2*k:p+2*k+2])[0] for k in range(nf)]; p+=2*nf
        frames=[b[p+f*w*h:p+(f+1)*w*h] for f in range(nf)]; p+=w*h*nf
        acts[aid]=(w,h,ms,frames)
    return pal,acts
def c565(v): return (((v>>11)&31)*255//31,((v>>5)&63)*255//63,(v&31)*255//31)
def to565(c): return ((c[0]*31//255)<<11)|((c[1]*63//255)<<5)|(c[2]*31//255)
def frame_img(pal,w,h,fr):
    im=Image.new('RGBA',(w,h)); px=im.load()
    for i,v in enumerate(fr):
        if v!=255: px[i%w,i//w]=c565(pal[v])+(255,)
    return im
def pack(acts_imgs):
    """acts_imgs: list of (aid, [PIL RGBA frames], [ms])"""
    cols=[]; idx={}
    for _,frs,_ in acts_imgs:
        for im in frs:
            for c in im.getdata():
                if c[3]>=128:
                    k=to565(c[:3])
                    if k not in idx: idx[k]=len(cols); cols.append(k)
    assert len(cols)<=255, len(cols)
    out=bytearray(b'TPK2'); out.append(len(acts_imgs)); out+=struct.pack('<H',len(cols))
    for k in cols: out+=struct.pack('<H',k)
    for aid,frs,ms in acts_imgs:
        w,h=frs[0].size; out+=bytes([aid,w,h,len(frs)])
        for m in ms: out+=struct.pack('<H',m)
        for im in frs:
            for c in im.getdata():
                out.append(255 if c[3]<128 else idx[to565(c[:3])])
    return bytes(out)
