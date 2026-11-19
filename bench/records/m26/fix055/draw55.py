exec(open('/tmp/claude-1000/-home-dingo-Programming-TraceMaker/0eb5330d-7327-41fa-97b4-5a919efd08e3/scratchpad/n55.py').read().split("bx,by=")[0])
from PIL import Image,ImageDraw
X0,X1,Y0,Y1=-2.5,4.5,-4.6,6.6; K=110
W,H=int((X1-X0)*K),int((Y1-Y0)*K)
im=Image.new('RGB',(W,H),(13,16,32)); d=ImageDraw.Draw(im,'RGBA')
P=lambda x,y:((x-X0)*K,(y-Y0)*K)
HL={'bga_conn_053':(255,212,0),'bga_conn_041':(255,140,0),'bga_conn_043':(0,224,160)}
for i in range(13):
  for j in range(13):
    x,y=-4.8+0.8*i,-4.8+0.8*j; a=P(x-.18,y-.18);b=P(x+.18,y+.18); d.rectangle([a,b],fill=(200,50,50,110))
def line(s,col,w):
    d.line([P(s[0],s[1]),P(s[2],s[3])],fill=col,width=w)
    for x,y in(s[:2],s[2:4]): c=P(x,y); d.ellipse([c[0]-w/2,c[1]-w/2,c[0]+w/2,c[1]+w/2],fill=col)
for s in segs:
    if s[5]=='F.Cu': line(s,(200,50,50,90),int(s[4]*K))
for s in segs:
    if s[5]=='B.Cu': line(s,HL.get(s[6],(74,127,208)),int(s[4]*K))
for v in vias:
    c=P(v[0],v[1]); r=.15*K; d.ellipse([c[0]-r,c[1]-r,c[0]+r,c[1]+r],fill=HL.get(v[2],(220,220,220)))
c=P(0.4,-2.8); r=.15*K; d.ellipse([c[0]-r,c[1]-r,c[0]+r,c[1]+r],outline=(255,0,255),width=3)
c=P(0.8,-3.2); d.rectangle([c[0]-.2*K,c[1]-.2*K,c[0]+.2*K,c[1]+.2*K],outline=(255,0,255),width=3)
for k,(n,col) in enumerate(HL.items()): d.rectangle([10,H-25-20*k,30,H-15-20*k],fill=col); d.text((36,H-26-20*k),n,fill='white')
d.text((10,10),'B.Cu around ball 055 (magenta square); magenta circle = SW via site',fill='white')
im.save('/tmp/claude-1000/-home-dingo-Programming-TraceMaker/0eb5330d-7327-41fa-97b4-5a919efd08e3/scratchpad/n55.png')
