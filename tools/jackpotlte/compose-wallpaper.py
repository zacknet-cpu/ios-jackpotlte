#!/usr/bin/env python3
# tools/jackpotlte/compose-wallpaper.py PNGDIR OUT.png WIDTH HEIGHT
#
# The A8 shows the iOS 16 default wallpaper as one still picture (fix library
# v13: SpringBoard puts /private/var/mobile/Library/a8wall.png behind the lock
# and home screens), because PosterBoard does not run and the wallpaper's HEIC
# layers decode to zeros without a hardware HEVC decoder.  This composes that
# still from the layers of
#   /Library/Wallpaper/Collections/iOS_16~iphone.wallpaperCollection/Wallpapers/
#   7400.WWDC_2022-375w-812h@3x~iphone.wallpaper/*.ca
# decoded to PNG off the phone (Leg, Head, HeadMask, Body, Arm .png in PNGDIR;
# "Head Mask.HEIC" has a space in its path, so the fix library's a8copy switch
# copies it out first).  The geometry below is read from the three main.caml
# files ("Locked" state).  Apple's pixels stay on the phone and the PC: never
# commit the PNGs or the result.  On the A8: 540 1110 (half the panel) costs
# ~4% on home-screen page swipes; 1080 2220 ~10%.
import sys
from PIL import Image

S = 3.0                    # pixels per point
W, H = 390, 844            # the "Overall Scale" layer, points

def px(v):
    return int(round(v * S))

canvas = Image.new('RGBA', (px(W), px(H)))

# 1. gradient: 520x975 centred at (195,422); stops 0, 0.22, 1
c = [(0.00479, 0.4334, 0.7858), (0.06029, 0.5409, 0.7977), (0.09558, 0.2828, 0.6444)]
top, height = 422 - 487.5, 975.0
grad = Image.new('RGBA', (1, px(H)))
for y in range(px(H)):
    t = min(1.0, max(0.0, (y / S - top) / height))
    if t <= 0.22:
        u = t / 0.22
        u = 1 - (1 - u) * (1 - u)          # easeOut
        a, b = c[0], c[1]
    else:
        u = (t - 0.22) / 0.78
        u = u * u * (3 - 2 * u)            # easeInEaseOut
        a, b = c[1], c[2]
    grad.putpixel((0, y), tuple(int(round(255 * (a[i] + (b[i] - a[i]) * u))) for i in range(3)) + (255,))
canvas.alpha_composite(grad.resize((px(W), px(H))))

def put(name, x, y, w, h, mask=None):
    im = Image.open(name).convert('RGBA').resize((px(w), px(h)), Image.LANCZOS)
    if mask is not None:
        m = Image.open(mask).convert('RGBA').resize((px(w), px(h)), Image.LANCZOS).getchannel('A')
        im.putalpha(m)          # sourceAtop over the mask: Head's colours, the mask's shape
    layer = Image.new('RGBA', canvas.size)
    layer.paste(im, (px(x), px(y)))
    canvas.alpha_composite(layer)

d = sys.argv[1]
put(d + '/Leg.png', -65, 454.83, 520, 455.33)                        # Background: Leg
put(d + '/Head.png', 21.33, -57.17, 433.33, 455.33, d + '/HeadMask.png')  # Background: Head in its mask
put(d + '/Body.png', -44.33, 183.83, 498.67, 715.33)                 # Foreground: Body
put(d + '/Arm.png', -65, 94.83, 520, 520.33)                         # Floating: Arm

# Overall Scale 0.961538 about the centre -> the 375x812 root
root = Image.new('RGBA', (px(375), px(812)))
scaled = canvas.resize((int(round(canvas.width * 0.961538)), int(round(canvas.height * 0.961538))), Image.LANCZOS)
root.alpha_composite(scaled, (int(round(px(187.5) - scaled.width / 2)), int(round(px(406) - scaled.height / 2))))

# aspect-fill into the panel, centred
tw, th = int(sys.argv[3]), int(sys.argv[4])
k = max(tw / root.width, th / root.height)
r = root.resize((int(round(root.width * k)), int(round(root.height * k))), Image.LANCZOS)
l, t = (r.width - tw) // 2, (r.height - th) // 2
r.crop((l, t, l + tw, t + th)).convert('RGB').save(sys.argv[2], optimize=True)
