exec(open('/tmp/claude-1000/-home-dingo-Programming-TraceMaker/0eb5330d-7327-41fa-97b4-5a919efd08e3/scratchpad/n55.py').read().split("bx,by=")[0])
import numpy as np
from scipy import ndimage
h=0.01; x0,x1,y0,y1=-7,9,-4.5,12
X,Y=np.meshgrid(x0+h*np.arange(int((x1-x0)/h)),y0+h*np.arange(int((y1-y0)/h)))
def segmask(a,b,c,e,r):
    dx,dy=c-a,e-b;L=dx*dx+dy*dy or 1e-12
    u=np.clip(((X-a)*dx+(Y-b)*dy)/L,0,1); return np.hypot(X-a-u*dx,Y-b-u*dy)<r
def run(drop_tracks,extra):
    R=0.06+0.1+extra; b=(Y>11.9)|(X<-6.9)|(X>8.9)
    for s in segs:
        if s[5]=='B.Cu' and s[6]!='bga_conn_055' and s[6] not in drop_tracks: b|=segmask(*s[:4],s[4]/2+R)
    for v in vias:
        if v[2]!='bga_conn_055': b|=np.hypot(X-v[0],Y-v[1])<0.15+R
    lab,_=ndimage.label(~b); st=lab[int(round((-2.8-y0)/h)),int(round((0.4-x0)/h))]
    return 'site blocked' if st==0 else round(y0+h*np.where((lab==st).any(1))[0].max(),2)
print('site clearance with tracks only bent away:')
for drop,ex,label in [((),0,'as routed'),(('bga_conn_053','bga_conn_041'),0,'053+041 tracks moved'),
   (('bga_conn_053','bga_conn_041','bga_conn_043'),0,'+043 track moved (vias kept)'),
   (('bga_conn_053','bga_conn_041','bga_conn_043'),0.22,'+043 moved, room for 043 to run alongside')]:
    print(f'  {label:45s} reach y = {run(drop,ex)}')
def run2(drop,extra,ycut):
    b=(Y>11.9)|(X<-6.9)|(X>8.9)
    for s in segs:
        if s[5]=='B.Cu' and s[6]!='bga_conn_055' and s[6] not in drop:
            b|=segmask(*s[:4],s[4]/2+0.16+(extra*(Y>ycut)))
    for v in vias:
        if v[2]!='bga_conn_055': b|=np.hypot(X-v[0],Y-v[1])<0.15+0.16+extra*(Y>ycut)
    lab,_=ndimage.label(~b); st=lab[int(round((-2.8-y0)/h)),int(round((0.4-x0)/h))]
    return 'site blocked' if st==0 else round(y0+h*np.where((lab==st).any(1))[0].max(),2)
for yc in (-1.5,0,2):
    print('two-track room needed only south of y',yc,'-> reach',run2(('bga_conn_053','bga_conn_041','bga_conn_043'),0.22,yc))
