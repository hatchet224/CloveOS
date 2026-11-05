#!/usr/bin/env python3
"""Rebuild the original Clove skin and bounded runtime font atlases (Pillow)."""
from pathlib import Path
import argparse, json, shutil, struct
from PIL import Image, ImageDraw, ImageFont

def build(font_path, license_path, out):
    out.mkdir(parents=True, exist_ok=True)
    (out/'fonts').mkdir(exist_ok=True)
    (out/'skin').mkdir(exist_ok=True)
    font_name = 'AtkinsonHyperlegible-Regular.ttf'
    shutil.copyfile(font_path, out/'fonts'/font_name)
    shutil.copyfile(license_path, out/'fonts'/'OFL.txt')
    theme = {'name':'Clove','list':{'font':'fonts/'+font_name,'size':24,'color':'202124'},
        'title':{'font':'fonts/'+font_name,'size':32,'color':'202124'},
        'hint':{'font':'fonts/'+font_name,'size':16,'color':'68646e'},
        'frame':{'border-left':16,'border-right':16},
        'grid':{'grid1x4':24,'grid3x4':18,'selectedcolor':'ffffff'},
        'batteryPercentage':{'visible':True},'hideLabels':{'hints':False},
        'palette':{'background':'f4f2ee','panel':'e9e5df','selected':'69548f','selectedText':'ffffff','accent':'69548f','muted':'68646e','ready':'277751','warning':'9d5218'},
        'layout':{'mode':'list','listTop':80,'rowHeight':52,'rowGap':4,'radius':12,'gridColumns':3,'gridRows':2,'gridGap':12}}
    (out/'config.json').write_text(json.dumps(theme,indent=2)+'\n',encoding='utf-8',newline='\n')
    grid={'layout':{'mode':'grid'}}
    (out/'grid.override.json').write_text(json.dumps(grid,indent=2)+'\n',encoding='utf-8',newline='\n')
    # Atlas covers Latin-1 and replacement; each record keeps actual font advance.
    codes=list(range(32,127))+list(range(160,256))+[0xfffd]
    for size in (16,24,32):
        font=ImageFont.truetype(str(font_path),size)
        ascent, descent=font.getmetrics(); line_height=ascent+descent
        image=Image.new('RGBA',(512,512),(255,255,255,0)); draw=ImageDraw.Draw(image)
        records=[]; x=y=1; rowheight=0
        for code in codes:
            glyph=chr(code); left,top,right,bottom=font.getbbox(glyph)
            width=max(1,right-left); height=max(1,bottom-top)
            if x+width+1>512: x=1;y+=rowheight+2;rowheight=0
            assert y+height+1<=512
            draw.text((x-left,y-top),glyph,font=font,fill=(255,255,255,255))
            advance=max(1,round(font.getlength(glyph)))
            records.append(struct.pack('<IHHHHhhH',code,x,y,width,height,left,top,advance))
            x+=width+2;rowheight=max(rowheight,height)
        prefix=out/'fonts'/f'{font_name}.{size}'
        image.save(str(prefix)+'.png')
        Path(str(prefix)+'.metrics').write_bytes(struct.pack('<4sHHHH',b'MXFA',1,len(codes),line_height,0)+b''.join(records))
    background=Image.new('RGB',(640,480),'#f4f2ee'); d=ImageDraw.Draw(background)
    d.line((16,62,624,62),fill='#ded8e4',width=1)
    d.line((16,443,624,443),fill='#ded8e4',width=1)
    background.save(out/'skin'/'bg-gamelist.png')
    (out/'LICENSE.txt').write_text('Original Clove layout configuration and background artwork: CC0-1.0.\nAtkinson Hyperlegible font remains under the attached SIL Open Font License.\nNo Onion copyrighted skin images, logos, or proprietary MainUI binary are included.\n',encoding='utf-8')

if __name__=='__main__':
    p=argparse.ArgumentParser(); p.add_argument('--font',type=Path,required=True);p.add_argument('--license',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();build(a.font,a.license,a.output)
