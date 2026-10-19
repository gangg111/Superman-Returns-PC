# RenderWare Audio Core: simple DSP plug-ins (native rewrite spec)

Scope: `LI20` LowPassIir2, `HI20` HighPassIir2, `Gai0` Gain, `PnS0` PanSurroundPlugIn, `Sen0` Send,
`Sub0` SubMix, `Rou0` Route. Source: PPC disassembly of the static recompilation
(`tools/re/dump_asm.py`) and static data from `logs/default_image.bin`.

Tags: **VERIFIED** = read directly in asm or static data. **INFERRED** = deduced, not proven.

Notation: all arithmetic is IEEE single precision unless stated. `fmaf(a,b,c)` = PPC `fmadds`
(one rounding). `fnmsubs d,a,c,b` = `b - a*c` with one rounding, written `fms(b,a,c)` below
(C++: `std::fmaf(-a, c, b)`; differs from PPC only in the sign of an exact zero result).
`fmuls`/`fadds`/`fsubs`/`fdivs` = plain float ops. VMX ops (`vmulfp`, `vaddfp`, `vmaddfp`) are
per-lane float ops; `vmaddfp` is fused (one rounding).


## 0. Framework facts these plug-ins rely on

### 0.1 Buffers (`io` object passed to Process) VERIFIED

* `io+4`, `io+8`: two planar buffers (double buffering). `io+28` (u8) selects the current one:
  current = `*(io + 4 + 4*io[28])`. Channel `c` of a buffer = `base + c*1024` bytes (256 floats).
* `io+20`, `io+24`: capacity in bytes of buffer 0 and 1.
* `sub_826581A8(io, bytes)`: returns the OTHER buffer (`idx ^ 1`), regrowing it (free + alloc,
  align 128) when `bytes > capacity`; returns 0 on allocation failure. It does not toggle.
* A plug-in that writes a new output calls `581A8`, writes into it and then toggles
  `io[28] = (io[28] == 0)` (`cntlzw; rlwinm >>5`). A plug-in that returns without toggling is a
  pass-through: the next plug-in sees the same buffer.
* `io+12` float: pitch/rate product (set to 1.0f once per mix call, multiplied by Pit0's hook).
  `io+16` float: set to 1.0f per voice before the hooks run (see 0.3).

### 0.2 Plug-in base (`sub_8264CDB8`, called by every Create) VERIFIED

```
plugin+4  = System*              (voice+32)
plugin+12 = plugin + paramOffset  (param slot table, 8 bytes per slot, float in the first 4)
plugin+16 = pointer to a per-class table in .data (param descriptors, filled at runtime)
plugin+20 = 0.0f
plugin+24 = 0.0f                  (tail in samples; filters overwrite with 450.0f)
plugin+28 = u8 input channel count  (previous plug-in's output count)
plugin+29 = u8 output channel count (entry+8)
```
`plugin+8` (Voice*) is set by the framework, not by the base ctor.

Create signature: `Create(voice, entry, prevChannels, mem)`; `entry+0` = pointer to a creation
argument array (or a name string for Sub0), `entry+8` = u8 output channel count.

### 0.3 Per-block driver (`sub_8265A148`) VERIFIED

Per voice, before Process: `io+16 = 1.0f`, `voice+44 = io+12`, then the descriptor `+20` hooks
are called as `hook(plugin[i], io)` for `i = count-1` down to `1` (index 0 is never called as a
hook; for Sub0 the `+20` field holds the constant `1`, used as an "this voice is a bus" flag).
Then Process is called for `i = 0..count-1` as `Process(plugin[i], io, reset)` with
`reset = (i >= voice+69) ? 1 : 0` where `voice+69` = number of plug-ins that ran in the
previous block. So a plug-in gets `reset = 1` on its first block and after any block where the
chain stopped before it.
* Process returns 0 at index > 0: the chain stops for this block (`voice+69 = i`).
* Process returns 0 at index 0: tail mode. `voice+52 = max(voice+52, Σ plugin+24)`; if
  `voice+48 < voice+52` a zeroed buffer of `plugin0+29` channels is produced (memset, toggle),
  the rest of the chain runs on silence and `voice+48 += 256.0f` (0x82036EB8); otherwise the
  chain stops. A non-zero return at index 0 sets `voice+48 = 0.0f`.
* Bus voices (plug-in 0 is Sub0) are deferred until every attached sender voice has been
  processed: the driver walks `Sub0+36` (sender links), processes each owner voice first
  (depth first), saving `io+12` in `Sub0+76` and restoring it for each sender. Counters
  `Sub0+144` (senders visited this pass) and `Sub0+146` (attached senders). VERIFIED in
  8265A24C..8265A464 at the level of control flow; exact pass-id handling (`voice+56`) INFERRED.

### 0.4 Parameter writes VERIFIED

`SetParamF(plugin, idx, f)` (`sub_8264CE48`) queues a 16-byte record
`{0x8264CE28, plugin, idx, f}`. The handler (`vt_8264CE28`, drained on the mixer thread before
mixing) does `*(float*)(plugin[12] + idx*8) = f` and nothing else. No smoothing at write time;
every smoothing below happens inside Process. `GetParam` (`sub_8264CE00`) copies the 8-byte slot.

### 0.5 Shared gain kernels VERIFIED (constants VERIFIED)

All take `dst, src` float pointers. N = 256 and R (ramp length) = 64 in every call seen.

`Set(dst, src, g, N)` `sub_8265B488`: `dst[n] = g * src[n]`. (VMX path when
`((dst & src) & 0x7F) == 0 && (N % 64) == 0`, else scalar `fmuls`; same result except FTZ, 0.6.)

`Add(dst, src, g, N)` `sub_8265AF48`: VMX path (same condition): `dst[n] = (g*src[n]) + dst[n]`
(two roundings). Scalar fallback: `dst[n] = fmaf(src[n], g, dst[n])` (ONE rounding). With
256-sample, 128-aligned buffers the VMX path is taken; the scalar path occurs only for N = 1
(Send release residual, 4.6).

`RampSet(dst, src, g, s, N, R)` `sub_8265B708`, `RampAdd` `sub_8265B168`:
```
s4 = s * 4.0f                                   // 0x820BD208 = 4.0
base[0] = g
base[1] = g + s                                 // fadds
base[2] = fmaf(s, 2.0f, g)                       // 0x820BD0EC = 2.0
base[3] = fmaf(s, 3.0f, g)                       // 0x820BD210 = 3.0
for (blk = 0; blk < R/32; ++blk) {               // R = 64: two blocks
  for (k = 0; k < 8; ++k) for (l = 0; l < 4; ++l) {
    n = blk*32 + k*4 + l;
    gain = (k == 0) ? base[l] : fmaf((float)k, s4, base[l]);   // vmaddfp, k from vector consts
    y = gain * src[n];                            // vmulfp
    dst[n] = Set ? y : y + dst[n];                // RampAdd: vaddfp, two roundings
  }
  for (l = 0; l < 4; ++l) base[l] = fmaf(8.0f, s4, base[l]);
}
gEnd = fmaf((float)R, s, g);                     // fcfid(R)=64.0f, fmadds
for (n = R; n < N; ++n) dst[n] = Set ? gEnd*src[n] : gEnd*src[n] + dst[n];
```
Vector constants VERIFIED: RampSet uses 0x820821A0..E0 = {1,2,3,4,5} and 0x82082190 = 6,
0x82082180 = 7, 0x82082170 = 8; RampAdd uses 0x820820D0 = 1, E0 = 2, F0 = 3, 0x82082120 = 4,
0x82082140 = 5, 0x82082110 = 6, 0x82082130 = 7, 0x82082100 = 8 (all splatted).
Note: `gEnd` is not the target gain in general; the jump to the exact target happens at the
start of the next block.

### 0.6 Denormals

* Scalar FPU paths (biquad, coefficient math): no flush code anywhere. VERIFIED (absence).
* VMX paths: Xenon VMX runs in non-Java mode, denormal inputs and results flush to zero.
  INFERRED (hardware property); bit-exact port should use FTZ/DAZ for the VMX kernels only,
  or check what the recompiler's `vmulfp` helper does.


## 1. LowPassIir2 `LI20` and HighPassIir2 `HI20`

Functions VERIFIED: LP GetSize 0x826633F8, Create 0x82663508, hook 0x82663320, Process
0x82663420, coefficients 0x82663338. HP: 0x826636E8, 0x826637F8, 0x82663610, 0x82663710,
coefficients 0x82663628. Shared biquad: init 0x826690E8, size 0x826690D0, run 0x82668E18,
clear 0x82669088. Vtables LP 0x82082654, HP 0x82082694: `[0]` and `[1]` = default stub
0x8231C140 (no Control), `[2]` = destructor 0x826635B8 / 0x826638A8.

### 1.1 Layout VERIFIED
```
+32 float  p0 cutoff in Hz (param slot 0; paramOffset = 32)
+40 Biquad* = align8(this+79)  (== this+72 for an 8-aligned object)
+44 float  a1      +48 a2      +52 b0      +56 b1      +60 b2
+64 float  last w (radians/sample) used for coefficients
+68 float  rate ratio (written by the hook)
+72 Biquad:
   +0  float* xh   = align8(B+27)          (== B+24)
   +4  float* yh   = align8(B+24+8n+7)     (== B+24+8n)
   +8  float* coef = this+44
   +12 float  pi   (0x82082644 LP, 0x82082680 HP, both 0x40490FDB = 3.14159274f)
   +16 u16    8n (bytes of one history array), cleared at init
   +18 u8     n  = entry+8 (channel count)
   +24 xh[n][2] = {x[-1], x[-2]} per channel, then yh[n][2] = {y[-1], y[-2]}
GetSize = 72 + (8n + ((8n+31) & ~7)) = 72 + 16n + 24
+24 (tail) = 450.0f (0x82082668)
```

### 1.2 Params and defaults VERIFIED
| idx | meaning | unit | default |
|---|---|---|---|
| 0 | cutoff | Hz | LP 1000000.0f (0x8200E680, means bypass); HP 0.0f (0x820013B8, means bypass) |

Create also sets `+64 = default cutoff value` (raw Hz value stored in the w slot; harmless, see
1.6). Coefficients are NOT computed at Create.

### 1.3 Hook (`+20`) VERIFIED
```
plugin[68] = io[12] / voice[44];   // fdivs, voice = plugin+8
```
In every measured chain the filters sit after Pit0, hooks run from last to index 1, so the filter
hooks run before Pit0's hook (`io[12] *= pit[32]`, 0x82662608) and `voice[44]` was just set to
`io[12]`: the ratio is exactly 1.0f for any finite non-zero `io[12]`. INFERRED consequence; if
`io[12]` is 0 or inf the ratio is NaN and the filter falls into bypass (1.5).

### 1.4 Coefficients (RBJ biquad, Q = 1) VERIFIED
`sin` = `sub_827A1B18`, `cos` = `sub_827A1BF0`, double-precision CRT calls on the float `w`,
results rounded with `frsp`. (Which is sin and which is cos INFERRED from the formula: only this
assignment gives a valid LP/HP; called sin first, then cos.)
```
float s = (float)sin((double)w), c = (float)cos((double)w);
float alpha = s * 0.5f;              // 0x820037C8 = 0.5  -> Q = 1.0
float t     = c * -2.0f;             // 0x820173C8 = -2.0
float opa   = alpha + 1.0f;          // 0x820013C0 = 1.0
float oma   = 1.0f - alpha;
float inv   = 1.0f / opa;
float den   = opa * 2.0f;            // 0x820BD0EC = 2.0
a1 = t * inv;   a2 = oma * inv;
LP: float u = 1.0f - c;  b1 = u * inv;  b0 = u / den;  b2 = u / den;
HP: float u = c + 1.0f;  b1 = -(u * inv); b0 = u / den;  b2 = u / den;
```
Stores: `+44 a1, +48 a2, +52 b0, +56 b1, +60 b2` (LP stores b1 before b0; order irrelevant).

### 1.5 Process VERIFIED
```
int Process(LP* p, IO* io, u8 reset /*ignored*/) {
  float w = ((B->pi / sys[12]) * p[68]) * p[32];   // fdivs, fmuls, fmuls; sys[12] = 48000
  w = w * 2.0f;                                     // 0x820BD0EC
  const float WMIN = 0.0031415930f;   // 0x82082660 = 0x3B4DE32F (0.001*pi, 24 Hz @48k)
  const float WMAX = 3.1384511f;      // 0x82082664 = 0x4048DC62 (0.999*pi, 23976 Hz @48k)
  // LP
  if (!(w < WMAX)) {                       // includes NaN
    if (p[64] < WMAX) BiquadClear(B);      // only on the active->bypass transition
    p[64] = w; return 1;                   // pass-through, no toggle
  }
  if (w < WMIN) w = WMIN;
  // HP mirror: if (!(w > WMIN)) { if (p[64] > WMIN) BiquadClear(B); p[64] = w; return 1; }
  //            if (w > WMAX) w = WMAX;
  if (w != p[64]) { ComputeCoefs(p, w); p[64] = w; }   // fcmpu, NaN never reaches here
  return BiquadRun(B, io) == 0 ? 1 : 0;
}
```
`BiquadClear` = memset(xh, 0, 8n) and memset(yh, 0, 8n). No coefficient ramping, coefficients
change at block boundaries only, filter state is kept across coefficient changes.

### 1.6 BiquadRun (`sub_82668E18`) VERIFIED
Direct form I, coefficients re-read per sample from `coef[]`:
`a1=coef[0] a2=coef[1] b0=coef[2] b1=coef[3] b2=coef[4]`.
```
in  = current buffer; out = sub_826581A8(io, n*1024); if (!out) return -1;
for ch: x = in + ch*256, y = out + ch*256, X1 = xh[ch][0], X2 = xh[ch][1], Y1 = yh[ch][0], Y2 = yh[ch][1]
  // n = 0
  t = b1*X1;  t = fmaf(b2, X2, t);  t = fmaf(b0, x[0], t);  t = fms(t, a1, Y1);  t = fms(t, a2, Y2);  y[0] = t;
  // n = 1 (different term order!)
  t = b2*X1;  t = fmaf(x[1], b0, t); t = fmaf(b1, x[0], t); t = fms(t, y[0], a1); t = fms(t, a2, Y1); y[1] = t;
for ch: for n = 2..255:
  t = b1*x[n-1]; t = fmaf(b2, x[n-2], t); t = fmaf(b0, x[n], t); t = fms(t, a1, y[n-1]); t = fms(t, a2, y[n-2]); y[n] = t;
for ch: xh[ch] = {x[255], x[254]}; yh[ch] = {y[255], y[254]};
io[28] ^= 1 (toggle); return 0;
```
The three loops are separate passes over all channels (all n = 0,1 first, then the 2..255 loop
per channel, then history). The result is identical to a per-channel loop because channels are
independent.

Edge cases: n = 0 channels produces a zero-byte output and still toggles. Initial `+64` is the
default cutoff in Hz, never equal to a valid w in the active branch, so coefficients are always
computed before the first active block (LP default 1e6 Hz and HP default 0 Hz are both bypass).


## 2. Gain `Gai0`

GetSize 0x8266BE78 = 48, Create 0x826639D8, no hook, Process 0x82663900. Vtable 0x820826BC
(`[0]`,`[1]` default stub, `[2]` 0x82663A58). VERIFIED.

Layout: `+32` p0 gain (linear factor, default 1.0f 0x820013C0), `+40` previous gain (init 1.0f).

Process VERIFIED:
```
int Process(Gain* p, IO* io, u8 reset) {
  in = current; out = sub_826581A8(io, p[29]*1024);   // NO null check (crash on failure)
  if (reset) p[40] = p[32];                           // snap, no ramp on first block
  float s = (p[32] - p[40]) * 0.015625f;              // 0x820BD330 = 1/64
  for (c = 0; c < p[29]; ++c) RampSet(out + 256c, in + 256c, p[40], s, 256, 64);
  toggle; p[40] = p[32]; return 1;
}
```
Linear gain, ramp over the first 64 samples of the block (0.5), constant `fmaf(64, s, prev)` for
samples 64..255. RampSet is used even when `s == 0` (gain then equals `prev` exactly).
Channel count used is `+29` (output), input channel c read at the same index.


## 3. PanSurroundPlugIn `PnS0`

GetSize 0x82652028 = 592, Create 0x826529B0, no hook, Process 0x82652690, init 0x826521D0,
speaker vectors 0x82652060, gain matrix 0x82652248, input count 0x826521A0. Vtable
0x82081C0C (`[0]`,`[1]` default stub, `[2]` 0x82652A48). VERIFIED.

### 3.1 Layout VERIFIED
```
+32 p0 distance   +40 p1 azimuth (degrees, INFERRED unit)   +48 p2 source half-width
+56 prev p1   +60 prev p0   +64 prev p2
+68 u8  = (entry+0 != 0)  (creation-args pointer present; not read by Process)
+80 + 16*i  input position vector i (x, y, 0, 0), i < 6
row o = 0..4 at R = +176 + 80*o:
   R+0        row sum of current gains (written by 82652248, not read by Process)
   R+4 +4i    cur gain[o][i], i < 6
   R+28 +4i   prev gain[o][i]
   R+64       speaker direction (x, y, 0, 0), unit length
+576 u8 force recompute (1 at create, 1 after a failed allocation)
```

### 3.2 Params and defaults VERIFIED
| idx | slot | meaning (INFERRED) | default |
|---|---|---|---|
| 0 | +32 | distance of the source centre from the listener | 0.0f |
| 1 | +40 | azimuth in degrees, 0 = front, positive = toward left | 0.0f |
| 2 | +48 | half-width of the source (spacing of virtual inputs) | 5.123f (0x82081C08 = 0x40A3EF9E) |

### 3.3 Speakers and output channel order VERIFIED (positions), INFERRED (names)
Initial vectors (0x820BD0D0 = -1.0f, 1.0f), normalized at create:

| row / output channel | raw vector | speaker |
|---|---|---|
| 0 | (-1, 1) | front left |
| 1 | ( 0, 1) | centre |
| 2 | ( 1, 1) | front right |
| 3 | (-1,-1) | rear left |
| 4 | ( 1,-1) | rear right |
| 5 | none, always zero | LFE |

This order (L, C, R, Ls, Rs, LFE) is also the order used by the Send channel-mapping kernels
(4.4: mono goes to channel 1, stereo to 0 and 2), so it is the engine's internal 6-channel
order; `Dac0` remaps it for the hardware. INFERRED.

Normalization (VMX, 0x82652130): `len2 = x*x + y*y` (vmulfp then vaddfp lane0 + lane1);
`e0 = vrsqrtefp(len2)`; two Newton steps
`e = e + (e*0.5)*(1 - len2*(e*e))` (each op rounded separately); `v = v * e`.
Result for (+-1, +-1) is about 0.70710677f; exact bits depend on the `vrsqrtefp` estimate
(open question 2).

### 3.4 Gain matrix (`sub_82652248`) VERIFIED
```
n = inputs = (inCh == 1 || inCh == 4) ? 4 : (inCh == 2) ? 2 : inCh;   // +28
zero all 6 position vectors; zero cur[o][i] for o<5, i<n
float A  = X * 0.0027777778f;          // X = float at 0x8292ED74 (runtime-filled, see Q1), 0x82021BD4 = 1/360
float th = (p1 + 90.0f) * A;           // 0x82001618 = 90.0
float c = (float)cos(th), s = (float)sin(th);   // cos called first
float cx = c*p0, sy = s*p0;            // f13, f11
if (n == 2) {
  v0 = ( fmaf(-s, p2, cx),  fmaf(c, p2, sy) );        // left
  v1 = ( fmaf(-s,-p2, cx),  fmaf(c,-p2, sy) );        // right
} else if (n == 4) {
  xl = fmaf(-s, p2, cx); yl = fmaf(c, p2, sy);
  xr = fmaf(-s,-p2, cx); yr = fmaf(c,-p2, sy);
  cp = c*p2; sp = s*p2;
  v0 = ( fmaf(p2, c, xl), fmaf(p2, s, yl) );  // left front  (+80)
  v1 = ( fmaf(p2, c, xr), fmaf(p2, s, yr) );  // right front (+96)
  v2 = ( xl - cp, yl - sp );                  // left rear   (+112)
  v3 = ( xr - cp, yr - sp );                  // right rear  (+128)
}
for (i = 0; i < n; ++i) {
  for (o = 0; o < 5; ++o) g[o] = (spk[o].x * v[i].x) + (spk[o].y * v[i].y);  // vmulfp, vaddfp
  g[1] = g[1] * 0.5f;                         // centre at half weight (0x820037C8)
  sum = 0; for (o = 0; o < 5; ++o) { if (g[o] < 0) g[o] = 0; sum = fmaf(g[o], g[o], sum); }
  if (sum > 0) { k = 1.0f / sqrtf(sum); k = k / (float)n; for o: g[o] = g[o]*k; }
  cur[o][i] = g[o];
}
rowSum[o] = cur[o][0] + cur[o][1] + ...   (sequential fadds from 0)
```
So each virtual input gets a power-normalized vector of the speakers it faces (dot product of
directions, negative lobes clipped), scaled by 1/n. Distance only matters relative to p2 (wider
image when the source is close). With p0 = 0 and p2 = 0 every gain is 0 (silence).

### 3.5 Process VERIFIED
```
int Process(PnS* p, IO* io, u8 reset /*ignored*/) {
  in = current; out = sub_826581A8(io, p[29]*1024);
  if (*(u8*)0x8299FDE0) { p[32] = 10.0f; p[40] = p[40] + 0.2f; }   // debug orbit (0x820BD22C, 0x82006BA0)
  if (!out) { p[576] = 1; return 0; }
  changed = p[576] == 1 || p[40] != p[56] || p[32] != p[60] || p[48] != p[64];
  if (changed) {
    p[576] = 0;
    for o<5: prev[o][0..5] = cur[o][0..5];
    ComputeMatrix(p);  p[56] = p[40]; p[60] = p[32]; p[64] = p[48];
  }
  src[0..3]: inCh 1 -> {in0,in0,in0,in0}, n=4;  inCh 2 -> {in0,in1}, n=2;
             inCh 4 -> {in0,in1,in2,in3}, n=4;  other -> n = 0 (rows 0..4 left unwritten!)
  for (o = 0; o < 5; ++o, outp += 256)
    for (i = 0; i < n; ++i) {
      g0 = prev[o][i]; s = (cur[o][i] - g0) * 0.015625f;    // 0x820BD330 = 1/64
      if (i == 0) changed ? RampSet(outp, src[i], g0, s, 256, 64) : Set(outp, src[i], g0, 256);
      else        changed ? RampAdd(outp, src[i], g0, s, 256, 64) : Add(outp, src[i], g0, 256);
    }
  memset(outp, 0, (p[29]-5)*1024);      // channel 5 (and above) zero
  for o<5: prev[o][0..5] = cur[o][0..5];
  toggle; return 1;
}
```
Ramps only in blocks where a parameter changed, otherwise constant gains (no ramp kernel).
First block after create: `prev = 0`, so the sound fades in over 64 samples.


## 4. Send `Sen0`

GetSize 0x82662058 = 104, Create 0x82662428, hook 0x826620D8, Process 0x82662100. Vtable
0x820825CC: `[0]` release 0x82662378, `[1]` Control 0x826624F0, `[2]` 0x826625A8. VERIFIED.

### 4.1 Layout VERIFIED
```
+40 p0 level (linear, default 1.0f 0x820013C0; paramOffset = 40)
+48 link { +48 next, +52 prev, +56 Sub0* bus, +60 owner Voice* }
+64 float last[6]   last input sample per channel times target level (declick residual)
+88 prev level   (init 1.0f)
+92 target level (computed by the hook)
+96 u8 ignoreVoiceGain = (u8)(s64)trunc(args[0]) if entry+0 != 0, else 0
+97 u8 needSnap (set while unattached)
```

### 4.2 Hook VERIFIED
`p[92] = p[40]; if (!p[96]) p[92] = io[16] * p[40];` (`io[16]` is 1.0f in all measured chains,
INFERRED that nothing between Sen0 and the voice start changes it).

### 4.3 Process VERIFIED
```
int Process(Send* p, IO* io, u8 reset) {
  if (reset || p[97]) { p[88] = p[92]; p[97] = 0; }
  Sub0* b = p[56];
  if (!b) { p[97] = 1; return 1; }                 // pass-through
  in = current; inCh = p[28];
  bus = b->buf(+32); busCh = b[29]; first = (b[148]++ == 0);   // sub_826514B0
  if (first) (p[92] != p[88]) ? MapRampSet(bus, in, new=p[92], old=p[88], busCh, inCh)
                              : MapSet(bus, in, p[92], busCh, inCh);
  else       (p[92] != p[88]) ? MapRampAdd(...) : MapAdd(...);
  p[88] = p[92];
  for c < inCh: p[64+4c] = in[c][255] * p[92];
  return 1;                                          // no toggle: input passes on unchanged
}
```
Ramp variants (`sub_82668CE8` set, `sub_826682C8` add): `s = (new - old) * 0.015384615f`
(0x82082A18 = 0x3C7C0FC1, **1/65**, not 1/64), start gain `old`, R = 64, N = 256. VERIFIED.
After 64 samples the gain sits at `fmaf(64, s, old)` (about old + 0.985*(new-old)) until the
next block, which starts from `new`.

### 4.4 Channel mapping (inCh -> busCh) VERIFIED for set and add, ramp 6->2 and n->n VERIFIED
`K = 0.707f` (0x8208228C = 0x3F34FDF4, note: not 1/sqrt(2)). `gc = g*K` (ramp: `gc = old*K`,
`sc = s*K`, each rounded). Channel order L, C, R, Ls, Rs, LFE (3.3). Order of terms is the order
of kernel calls; the first call of a "set" mapping is a Set, later ones Add.

| in -> out | set (sub) | result |
|---|---|---|
| n -> n | Set N*n (B488) | identity |
| 1 -> 2 | D3C8 | o0 = gc*i0; o1 = gc*i0 |
| 1 -> 4 | D420 | o0 = o1 = gc*i0; o2 = o3 = 0 |
| 1 -> 6 | D4A8 | o1 = g*i0; o0 = o2 = o3 = o4 = o5 = 0 |
| 2 -> 1 | DA58 inline | o0 = g*i0; o0 += g*i1 |
| 2 -> 4 | D540 | o0 = g*i0; o1 = g*i1; o2 = o3 = 0 |
| 2 -> 6 | D5B8 | o0 = g*i0; o2 = g*i1; o1 = o3 = o4 = o5 = 0 |
| 4 -> 1 | D660 | o0 = g*i0 +g*i1 +g*i2 +g*i3 |
| 4 -> 2 | D6E0 | o0 = g*i0 + g*i2; o1 = g*i1 + g*i3 |
| 4 -> 6 | D768 | o0 = g*i0; o2 = g*i1; o3 = g*i2; o4 = g*i3; o1 = o5 = 0 |
| 6 -> 1 | D818 | o0 = g*i1 +g*i2 +g*i4 +g*i3 +g*i0 (LFE dropped) |
| 6 -> 2 | D8B8 | o0 = gc*i1 + g*i0 + g*i3; o1 = gc*i1 + g*i2 + g*i4 |
| 6 -> 4 | D988 | o0 = gc*i1 + g*i0; o1 = gc*i1 + g*i2; o2 = g*i3; o3 = g*i4 |
| in < out, other | DA58 | Set first in*N samples, zero the rest |
| in > out, other | DA58 | Set first out*N samples (extra channels dropped) |

Add variants (`sub_826678F8`) use the same matrix but only add, never zero (1->6 adds into
channel 1 only, 2->6 into 0 and 2). Ramp variants are dispatched the same way (8CE8/82C8 tables:
1->2 8400/7B60, 1->4 8470/7BD0, 1->6 8510/B168 into ch1, 2->1 85B0/7C40, 2->4 8610/7CA0,
2->6 86A0/7D00, 4->1 8760/7D68, 4->2 8808/7E10, 4->6 88B8/7EC0, 6->1 8990/7F70, 6->2 8A60/8040,
6->4 8B68/8148); only 6->2 and n->n were read, the rest INFERRED to mirror the set matrix.

### 4.5 Control VERIFIED
* `Control(p, 0, data)`: queues `{0x826623E0, p, data[0]}` (12 bytes). Handler: detach from the
  current bus with residual `p+64` (4.6), then if `data[0]` (Sub0*) != 0 attach:
  link `+56 = bus`, `+60 = p->voice (plugin+8)`, insert at head of `bus+36`, `bus[146]++`.
* `Control(p, idx != 0, data)`: queues `{0x826622C8, p, len, string}`; handler detaches the same
  way, then searches the global bus list (head 0x8299FDDC, iterator 0x8299FDD8, next = 0x826513C0)
  for a Sub0 whose name (`Sub0+80`) equals the string and attaches to it.

### 4.6 Detach and release residual VERIFIED
`sub_82651518(link, residual)`: unlink, `bus[146]--`; if `residual`: `bus[151] = 1` and
`bus[52+4c] += residual[c]` for `c < bus[29]`; clear link bus and owner.
Release (`vtbl[0]`): if attached, `tmp = MapSet(tmp, p+64, 1.0f, busCh, inCh, N=1)` (scalar paths,
the Add steps are fused here) and detach with `tmp`. The Control handlers pass `p+64` raw
(indexed by the send's input channel, not mapped; identical when inCh == busCh).
Sub0 then fades each residual out over 16 samples (5.3).


## 5. SubMix `Sub0`

GetSize 0x826513A8 = 152, Create 0x826516C8, `+20` = constant 1 (bus flag), Process 0x826513E8.
Vtable 0x82081B2C: `[0]` release 0x826515E0, `[1]` default stub (no Control), `[2]` 0x82651838.
VERIFIED.

### 5.1 Layout VERIFIED
```
+32 float* bus buffer, busCh*1024 bytes, align 128 (sub_8264CBB8)
+36 sender link list head
+40 global bus list node {next +40, prev +44}, head 0x8299FDDC
+48 sender cursor (driver)        +52 float residual[6]
+76 saved io+12 (driver)          +80 char name[] (copied from *(char**)entry+0)
+144 u16 senders visited this pass   +146 u16 attached senders
+148 u16 contributions this block    +150 u8 in global list   +151 u8 residual pending
+29 busCh = entry+8; paramOffset 0 (no params)
```
Create failure (buffer alloc) calls `vtbl[0]` and `vtbl[2](obj, 0)` and returns 0.

### 5.2 Accumulation protocol VERIFIED
Senders (Sen0, Rou0) call `sub_826514B0(bus, &busCh, &count)` which returns `bus+32`, the
current count and post-increments `+148`. The first contributor of the block overwrites the
bus (Send: Set/RampSet mapping; Route: memset then Add), later contributors add. Sub0 itself
never clears the bus at the end of a block; `+148 == 0` is the "empty" marker.

### 5.3 Process VERIFIED
```
int Process(Sub0* p, IO* io, u8 reset /*ignored*/) {
  if (p[148] == 0 && p[151] == 0) return 0;          // nothing this block -> chain stops / tail mode
  out = sub_826581A8(io, p[29]*1024); if (!out) return 0;
  toggle;
  if (p[148]) { memcpy(out, p[32], p[29]*1024); p[148] = 0; }
  else        memset(out, 0, p[29]*1024);
  if (p[151]) { ResidualFade(out, p+52, p[29], 256); p[151] = 0; }
  return 1;
}
ResidualFade (sub_8265AD70): for c < ch: r = res[c];
  for k = 0..15: out[c][k] = fmaf(r, F[k], out[c][k]);  res[c] = 0;
F[k] = (16-k)/17 as floats at 0x82082070..0x820820AC:
 3F70F0F1 3F61E1E2 3F52D2D3 3F43C3C4 3F34B4B5 3F25A5A6 3F169697 3F078788
 3EF0F0F1 3ED2D2D3 3EB4B4B5 3E969697 3E70F0F1 3E34B4B5 3DF0F0F1 3D70F0F1
```
Release (`vtbl[0]`): detach every sender link (no residual), unlink from the global list if
`+150`, free the bus buffer.


## 6. Route `Rou0`

GetSize 0x82661D30 = 80, Create 0x82661F28, no hook, Process 0x82661D58. Vtable 0x820825A4:
`[0]` release 0x82661E90 (detach with residual `p+56`), `[1]` Control 0x82661FC0, `[2]`
0x82662010. VERIFIED.

Layout: `+36` link {next, prev, `+44` Sub0* bus, `+48` owner voice}, `+52` u8 srcStart,
`+53` u8 dstStart, `+54` u8 count, `+56` float last[6] (indexed by bus channel). No params.

Control (any idx) VERIFIED: queues `{0x82661EA0, p, data[0..3]}` (24 bytes),
`data = {Sub0* bus, float srcStart, float dstStart, float count}`. Handler: detach with residual
`p+56`; if `bus != 0`: attach (owner = voice) and set `+52/+53/+54 = (u8)(s64)trunc(data[1..3])`
(`fctidz`, low byte). With `bus == 0` the old channel fields stay.

Process VERIFIED:
```
int Process(Route* p, IO* io, u8 reset /*ignored*/) {
  if (!p[44]) return 1;
  in = current; bus = buf(p[44], &busCh, &count);      // count post-incremented
  if (count == 0) memset(bus, 0, busCh*1024);
  for (i = 0; i < p[54]; ++i) Add(bus + (p[53]+i)*256, in + (p[52]+i)*256, 1.0f, 256);
  last[0..5] = 0; for i < p[54]: last[p[53]+i] = in[p[52]+i][255];
  return 1;                                            // pass-through, no toggle
}
```
Music chain `SnP0(4) > Pit0 > Rou0 > Rou0`: each Rou0 copies a channel range of the 4-channel
stream into a bus (INFERRED: front pair and rear pair to different buses or channel offsets).


## 7. Open questions

1. **PnS0 angle scale**: the float at 0x8292ED74 (inside the PnS0 descriptor table 0x8292ED30,
   filled at runtime; the static image holds the filler 0x02010201). Almost certainly 2*pi
   (6.2831855f) so that `th = (p1 + 90) * 2pi/360` is radians from degrees. Read it from guest
   memory in a running build before relying on it. No writer was found by grep.
2. **vrsqrtefp bits**: the PnS0 speaker vectors depend on the Xenon `vrsqrtefp` estimate plus two
   Newton steps. Dump `plugin+240+80*o` (x, y) from a live PnS0 instance to get the exact 10 floats
   and hard-code them natively.
3. **VMX denormal mode** (0.6): confirm what the recompiler does for `vmulfp/vaddfp/vmaddfp`
   (FTZ or not) so the native kernels match it.
4. **io+12 carry-over**: `io+12` is reset to 1.0f once per mix call, not per voice, and Pit0's
   hook multiplies it. The filters only see `io12/voice44` (1.0f) so they are unaffected, but the
   SnP0/Pit0 spec should confirm who resets it (Sub0+76 save/restore covers bus senders).
5. **io+16 writers**: Send's target level is `level * io+16`; only the per-voice reset to 1.0f
   was found. Check SnP0/Pit0 Process for writes to `io+16` (voice volume?).
6. **PnS0 inputs not in {1,2,4}** leave rows 0..4 of the output unwritten (stale scratch). Never
   hit by the measured chains (PnS0 always follows a 1/2/4-channel source), port may zero them.
7. **PnS0 `+68` flag and row sums** (`R+0`) are written but never read by these functions; probably
   read by a getter or the VU meter. Not needed for audio output.
8. **Send ramp mapping kernels** other than 6->2 and n->n were not read instruction by instruction
   (4.4); confirm term order if bit exactness matters for 1/2/4-channel sends.
9. **Sub0 scheduling** (`voice+56` pass id, `+144/+146/+48`) is described at control-flow level;
   a native mixer can replace it with "process all sender voices of a bus before the bus voice".
