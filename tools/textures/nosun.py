# The sky panorama's lower half (EA4A8BC906AA9D06, 1024x512) without its painted sun:
# the disc filled by inpainting, the glow around it taken off as a radial luminance profile.
# usage: py nosun.py <in.dds> <out.png> [cx cy]
import sys
import numpy as np
import cv2
from PIL import Image

src, dst = sys.argv[1], sys.argv[2]
im = Image.open(src).convert('RGBA')
a = np.asarray(im).astype(np.float32)
rgb, alpha = a[..., :3], a[..., 3:]
h, w = rgb.shape[:2]
lum = rgb @ np.array([0.2126, 0.7152, 0.0722], np.float32)
if len(sys.argv) > 4:
    cx, cy = float(sys.argv[3]), float(sys.argv[4])
else:  # the brightest blob
    b = cv2.GaussianBlur(lum, (0, 0), 3)
    cy, cx = np.unravel_index(np.argmax(b), b.shape)
yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
r = np.hypot(xx - cx, yy - cy)

# 1. the disc: inpaint everything markedly brighter than its ring within 22 px
ring = (r > 22) & (r < 30)
disc = ((r < 22) & (lum > np.percentile(lum[ring], 70) + 8)).astype(np.uint8) * 255
disc = cv2.dilate(disc, np.ones((5, 5), np.uint8))
fixed = cv2.inpaint(np.clip(rgb, 0, 255).astype(np.uint8), disc, 9, cv2.INPAINT_TELEA).astype(np.float32)

# 2. the glow: per radius, the lower quartile of luminance (the sky between clouds)
# minus the same far away; taken off along the glow's own colour
lum2 = fixed @ np.array([0.2126, 0.7152, 0.0722], np.float32)
edges = np.arange(0, 300, 1)
prof = []
for r0 in edges:
    m = (r >= r0) & (r < r0 + 4)
    prof.append(np.percentile(lum2[m], 25) if m.any() else 0)
prof = np.array(prof, np.float32)
base = np.median(prof[(edges > 220)])
excess = np.clip(prof - base, 0, None)
excess = cv2.GaussianBlur(excess.reshape(1, -1), (0, 0), 8).ravel()
g = np.interp(r, edges + 0.5, excess, right=0.0)
near = (r < 120)
tint = fixed[near].mean(0) / max(fixed[near].mean(), 1e-3)  # the glow is near white: scale per channel
out = fixed - g[..., None] * tint[None, None, :]
# 3. the disc's footprint: its large-scale colour levelled to a plane fitted on the ring
# around it (keeps the fine texture, takes off any leftover brighter or darker disc)
fit = (r > 45) & (r < 80)
A = np.stack([xx[fit] - cx, yy[fit] - cy, np.ones(fit.sum(), np.float32)], 1)
low = cv2.GaussianBlur(out, (0, 0), 10)
plane = np.zeros_like(out)
for ch in range(3):
    coef, *_ = np.linalg.lstsq(A, low[..., ch][fit], rcond=None)
    plane[..., ch] = coef[0] * (xx - cx) + coef[1] * (yy - cy) + coef[2]
wgt = (1.0 - np.clip((r - 35.0) / 15.0, 0.0, 1.0))[..., None]
out = out + (plane - low) * wgt
# 4. the grain: the fine detail (high-pass) of the sky 70 px to the left, faded in where
# the disc was filled
hp = out - cv2.GaussianBlur(out, (0, 0), 2)
grain = np.roll(hp, 70, axis=1)
gw = (1.0 - np.clip((r - 26.0) / 10.0, 0.0, 1.0))[..., None]
out = out + grain * gw
out = np.clip(out, 0, 255)
# 5. the alpha channel carries the sun too (78 at its centre against 27 around): its
# radial excess over the far ring taken off, then levelled like the colour
al = alpha[..., 0]
aprof = np.array([np.median(al[(r >= r0) & (r < r0 + 1)]) for r0 in edges], np.float32)
abase = np.median(aprof[(edges > 60) & (edges < 120)])
aex = cv2.GaussianBlur(np.clip(aprof - abase, 0, None).reshape(1, -1), (0, 0), 1.5).ravel()
al = al - np.interp(r, edges + 0.5, aex, right=0.0)
alow = cv2.GaussianBlur(al, (0, 0), 10)
acoef, *_ = np.linalg.lstsq(A, alow[fit], rcond=None)
aplane = acoef[0] * (xx - cx) + acoef[1] * (yy - cy) + acoef[2]
al = al + (aplane - alow) * wgt[..., 0]
alpha = np.clip(al, 0, 255)[..., None]
res = np.concatenate([out, alpha], -1).astype(np.uint8)
Image.fromarray(res, 'RGBA').save(dst)
print('sun at', cx, cy, 'glow peak', float(excess.max()), 'base', float(base))
