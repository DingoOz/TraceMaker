exec(open('/tmp/claude-1000/-home-dingo-Programming-TraceMaker/0eb5330d-7327-41fa-97b4-5a919efd08e3/scratchpad/n55.py').read().split("bx,by=")[0])
import numpy as np, sys
from scipy import ndimage
h=0.01; x0,x1,y0,y1=-3,5,-4.5,6.5
X,Y=np.meshgrid(x0+h*np.arange(int((x1-x0)/h)),y0+h*np.arange(int((y1-y0)/h)))
# distance field to the nearest fixed B.Cu obstacle edge (tracks and vias, all nets except the four stripped)
drop={'bga_conn_055','bga_conn_053','bga_conn_041','bga_conn_043'}
D=np.full(X.shape,9.0)
for s in segs:
    if s[5]=='B.Cu' and s[6] not in drop:
        a,b,c,e=s[:4];dx,dy=c-a,e-b;L=dx*dx+dy*dy or 1e-12
        u=np.clip(((X-a)*dx+(Y-b)*dy)/L,0,1); D=np.minimum(D,np.hypot(X-a-u*dx,Y-b-u*dy)-s[4]/2)
for v in vias:
    if v[2] not in drop: D=np.minimum(D,np.hypot(X-v[0],Y-v[1])-0.15)
for v in vias:   # keep the stripped nets' escape vias as obstacles
    if v[2] in ('bga_conn_053','bga_conn_041'): D=np.minimum(D,np.hypot(X-v[0],Y-v[1])-0.15)
st=(int(round((-2.8-y0)/h)),int(round((0.4-x0)/h)))
def reach(need, ycut=-1.5, need_south=None):
    ok=np.where(Y>ycut, D>=need_south, D>=need) if need_south else D>=need
    lab,_=ndimage.label(ok); l=lab[st]
    return 'blocked' if l==0 else round(y0+h*np.where((lab==l).any(1))[0].max(),2)
for w in (0.12,0.10,0.09,0.08,0.075,0.06):
    one=w/2+0.1; two=w/2+0.1+w+0.1   # one track / room for a second track of the same width beside it
    print(f'w={w}: one track reach {reach(one)}, two side by side (south of -1.5) reach {reach(one,-1.5,two)}')
lo,hi=0.0,0.5
for _ in range(20):
    m=(lo+hi)/2; r=reach(0.16,-1.5,m)
    (lo,hi)=(m,hi) if (r!='blocked' and r>5) else (lo,m)
print(f'widest way south: gap {2*lo:.3f} mm edge to edge')
ok=np.where(Y>-1.5,D>=lo+0.002,D>=0.16); lab,_=ndimage.label(np.where(Y>-1.5,D>=lo-0.002,D>=0.16)); l=lab[st]
# bottleneck cells: on the path region with D close to lo
cand=(lab==l)&(Y>-1.5)&(np.abs(D-lo)<0.003)
ys,xs=np.where(cand); print('bottleneck near', sorted({(round(x0+h*x,1),round(y0+h*y,1)) for y,x in zip(ys,xs)})[:8])
