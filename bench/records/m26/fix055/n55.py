import re,math,sys
t=open('bench/results/srj20-again/sample152/r1/a1/out.kicad_pcb').read()
m=re.search(r'\(footprint "SRJ:bga_component".*?\(at ([-\d.]+) ([-\d.]+)',t,re.S); cx,cy=float(m[1]),float(m[2])
nets=dict((int(a),b) for a,b in re.findall(r'\(net (\d+) "([^"]*)"\)',t))
def netname(s):
    m=re.search(r'\(net (\d+)\)',s) or re.search(r'\(net "([^"]*)"\)',s)
    if not m: return '?'
    return nets.get(int(m[1]),m[1]) if m[1].isdigit() else m[1]
segs=[];vias=[]
for s in re.findall(r'\t\(segment\n.*?\n\t\)',t,re.S):
    a=re.search(r'\(start ([-\d.]+) ([-\d.]+)\)',s);b=re.search(r'\(end ([-\d.]+) ([-\d.]+)\)',s)
    w=float(re.search(r'\(width ([\d.]+)',s)[1]);L=re.search(r'\(layer "([^"]+)"',s)[1]
    segs.append((float(a[1])-cx,float(a[2])-cy,float(b[1])-cx,float(b[2])-cy,w,L,netname(s)))
for s in re.findall(r'\t\(via\n.*?\n\t\)',t,re.S):
    a=re.search(r'\(at ([-\d.]+) ([-\d.]+)',s);vias.append((float(a[1])-cx,float(a[2])-cy,netname(s)))
print('centre',cx,cy,len(segs),'segs',len(vias),'vias')
def dseg(px,py,s):
    x1,y1,x2,y2=s[:4];dx,dy=x2-x1,y2-y1;L=dx*dx+dy*dy
    u=0 if L==0 else max(0,min(1,((px-x1)*dx+(py-y1)*dy)/L));return math.hypot(px-x1-u*dx,py-y1-u*dy)
bx,by=0.8,-3.2
for sx in(-.4,.4):
  for sy in(-.4,.4):
    px,py=bx+sx,by+sy;print(f'site ({px:+.1f},{py:+.1f})')
    for s in segs:
      d=dseg(px,py,s)
      if d<0.15+0.1+s[4]/2-1e-6 and s[6]!='bga_conn_055': print('   track',s[5],s[6],f'dist {d:.3f} need {0.25+s[4]/2:.3f}',[round(v,2) for v in s[:4]])
    for v in vias:
      d=math.hypot(px-v[0],py-v[1])
      if d<0.3+0.1-1e-6: print('   via',v[2],f'dist {d:.3f}',(round(v[0],2),round(v[1],2)))
