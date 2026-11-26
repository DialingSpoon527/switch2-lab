"""Rasterize the authored SVG's simple geometry with Pillow, without extra DLLs."""
from pathlib import Path
from PIL import Image, ImageDraw

p = Path(__file__).resolve().parent
scale = 4
image = Image.new('RGB', (256 * scale, 256 * scale), '#25213e')
draw = ImageDraw.Draw(image)
def box(values): return tuple(int(v * scale) for v in values)
draw.rounded_rectangle(box((28, 28, 228, 228)), radius=28*scale, fill='#39315b')
def line(points, width):
    draw.line([(int(x*scale),int(y*scale)) for x,y in points], fill='#b7f579', width=width*scale)
    for x,y in [points[0],points[-1]]:
        draw.ellipse(box((x-width/2,y-width/2,x+width/2,y+width/2)),fill='#b7f579')
line([(98,51),(98,96)],17)
line([(158,51),(158,96)],17)
line([(128,179),(128,201)],18)
def curve(a,b,c,d):
    return [tuple((1-t)**3*a[j]+3*(1-t)**2*t*b[j]+3*(1-t)*t*t*c[j]+t**3*d[j] for j in (0,1))
            for t in [i/64 for i in range(65)]]
plug=[(78,91),(178,91),(178,134)]
plug+=curve((178,134),(178,160),(156,181),(128,181))
plug+=curve((128,181),(100,181),(78,160),(78,134))
draw.polygon([(int(x*scale),int(y*scale)) for x,y in plug],fill='#b7f579')
draw.polygon([(x*scale,y*scale) for x,y in [(119,110),(105,136),(124,136),(119,161),(149,126),(127,126),(136,110)]],fill='#39315b')
draw.ellipse(box((187,187,217,217)),fill='#b7f579')
draw.ellipse(box((196,196,208,208)),fill='#39315b')
image=image.resize((256,256),Image.Resampling.LANCZOS)
image.save(p/'icon.png')
image.save(p/'icon.jpg',quality=95,subsampling=0)
