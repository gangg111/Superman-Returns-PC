# RenderWare Audio Core: effects and output plug-ins (RM10, Del0, VuM0, AiW0, Dac0)

Scope: native C++ rewrite spec for the reverb, delay, meter, debug writer and the output stage (Dac)
of RWAudioCore in Superman Returns (X360). Source of truth: PPC disassembly of the recompiled
functions (`tools/re/dump_asm.py`) and constants read from `logs/default_image.bin`.

Tags: **VERIFIED** = read in the asm (and constants decoded). **INFERRED** = deduced from layout,
naming or call context, not proven instruction by instruction.

### 0. Conventions and shared facts

* All arithmetic is IEEE single precision unless noted. PPC fused ops matter for bit exactness:
  `fmadds d=a*c+b`, `fnmsubs d=b-a*c`, both with ONE rounding. In C++ use `std::fma(a,c,b)` and
  `std::fma(-a,c,b)` (float overloads), never `a*c+b`. Everything called `fmuls/fadds/fsubs/fdivs`
  is an ordinary single rounded op.
* `fctiwz` = truncate toward zero to int32. "round" in this document always means the explicit
  sequence `x >= 0 ? x + 0.5f : x - 0.5f` followed by `fctiwz`.
* `log10(x)` (sub_827A3EC0) = `ln(x)` in double (sub_827A3D40) times double `0x3FDBCB7B1526E50E`
  (= 0.4342944819032518 = 1/ln 10), then rounded to float by the caller (`frsp`). VERIFIED.
* Plug-in header (VERIFIED, sub_8264CDB8 called from every Create):
  `+0 vtable, +4 System*, +8 Voice*, +12 paramTable = this + paramOffset, +16 param descriptor ptr,
  +20 float = 0, +24 float tail (samples) = 0, +28 u8 = Create arg r5 (channel count of the plug-in
  input, INFERRED), +29 u8 = Voice->+8 (voice channel count)`.
  Param slot k lives at `paramTable + 8*k` (float at +0 of the slot).
* Descriptor field +28 "packed counts" decodes as `(params << 16) | (controls << 8) | flags`
  (VERIFIED by consistency: RM10 0x30000 = 3 params, Del0 0x20000 = 2 params, VuM0 0x120100 =
  18 params + 1 control, AiW0 0x200 = 2 controls, Dac0 0x30500 = 3 params + 5 controls; the control
  counts match the Control implementations below).
* io block passed to Process: `buf = *(float**)(io + 4 + 4*io->cur)` with `io->cur = u8 at io+28`.
  The buffer is planar, channel c at `buf + 256*c`, 256 frames. `sub_826581A8(io, bytes)` returns
  the scratch buffer (the other half of the double buffer, INFERRED). A plug-in that writes into the
  scratch must flip `io->cur = (io->cur == 0)` (cntlzw idiom). A plug-in that works in place does
  not flip. VERIFIED for every plug-in below.
* Process return value: 1 = block valid, 0 = "nothing produced". In the chain walker
  sub_8265A148 a 0 from a plug-in at chain index > 0 stops the chain for this block and the voice
  returns 0 (VERIFIED: `bne cr6,0x8265a370`). The Dac zero fills its packet when the render returns
  something other than 1 (VERIFIED). So RM10 returning 0 silences the reverb bus.
* Channel order inside RW buses (INFERRED, but consistent across VuM0 slot mapping, RM10 output
  copies and the Dac reorder): `0 = L, 1 = C, 2 = R, 3 = Ls, 4 = Rs, 5 = LFE` for 6 ch;
  stereo = `L, R`; quad = `L, R, Ls, Rs`; mono = C.
* Mixer rate: `System+12` = 48000.0f (set by the Dac Create), block period `System+8` = 256/48000
  = 0.0053333333f (`0x43800000 / rate`). VERIFIED.

### 1. ReverbModel1 "RM10"

GetSize sub_8264DF18, Create sub_8264E668, Process sub_8264E398, vtable 0x82081994
(`[0] dtor 0x8264D7A0, [1] Control = sub_8231C140 (just blr, no controls), [2] 0x8264E930`).
VERIFIED.

#### 1.1 Topology (VERIFIED)

Mono in (channel 0 of the current io buffer), 6 parallel lowpass-feedback combs (Moorer type)
summed with gain 1/6, then 1 to 3 allpass filters in PARALLEL (one per distinct output signal),
then copies into the output channels. Output channel count `nch = Voice->+8` (stored at +60).

```
x ──┬─ comb0 ─┐
    ├─ comb1 ─┤
    ├─  ...   ├─(sum, each *1/6)── s ──┬─ AP0 ─> out ch A
    └─ comb5 ─┘                       ├─ AP1 ─> out ch B
                                      └─ AP2 ─> out ch C
```

#### 1.2 Object layout (VERIFIED offsets, names INFERRED)

| off | type | meaning | Create value |
|---|---|---|---|
| +32 | f32 | **P0** decay control "T" (param slot 0) | 0.0 |
| +40 | f32 | **P1** room size in metres (slot 1) | 15.0 (`0x41700000`) |
| +48 | f32 | **P2** damping divisor (slot 2) | 1.0 |
| +56 | f32 | cached System rate (slot 3 low word) | 0.0 |
| +60 | i32 | nch (= Voice+8) | |
| +64 | f32 | tail copy (same value as +24) | |
| +68 | f32 | previous P0 | 0.0 |
| +72 | f32 | previous P1 | NOT initialised by Create (see open questions) |
| +76 | f32 | previous P2 | 1.0 |
| +80 + 32*i | Comb[6] | comb i, i = 0..5 | zeroed by sub_8264E808 |
| +272 | f32[6] | comb distances in metres t_i | |
| +296 | i32[6] | comb delay lengths L_i (samples) | |
| +320..+343 | | unused by these functions | |
| +344 | f32[6] | lowpass coefficient a_i | |
| +368 | f32[6] | feedback b_i | |
| +392 | i32 | 0 | 0 |
| +396 | i32 | allpass initialised flag | 0 |
| +400 | AP*[3] | allpass pointers (null if unused) | |
| +412 | i32 | allpass count | |
| +416 | f32[3] | allpass gains | |
| +428 | i32[3] | allpass lengths | |
| +440.. | AP structs | each `align8(p)`, 24 bytes | |

GetSize: `size = 440; repeat apCount times { size = align8(size) + 24 }` with apCount from Voice+8:
1 ch -> 1, 2 ch -> 2, 4 ch -> 2, anything else -> 3. So 464 / 488 / 512 bytes. VERIFIED.

Comb struct (32 bytes): `+0 i32 L, +4 f32 c4 = -a, +8 f32 c8 = -b, +12 f32 c12 = -a,
+16 float* buf, +20 i32 size = L+2, +24 i32 writePos, +28 i32 nch (unused in DSP)`.
Allpass struct (24 bytes): `+0 i32 len, +4 f32 g, +8 float* buf, +12 i32 size = len+1,
+16 i32 writePos, +20 i32 nch` (Create sets +4 = 0.0, others 0, +20 = Voice+8).

#### 1.3 Parameters

| idx | off | meaning | unit | range enforced in code | default |
|---|---|---|---|---|---|
| 0 | +32 | decay control T. `<= 0` = reverb OFF | none (behaves like a time) | values in (0, 0.366) are clamped to 0.366 and WRITTEN BACK into the slot | 0.0 (off) |
| 1 | +40 | room size | metres (distance / speed of sound) | clamped to [2.0, 83.3], written back | 15.0 |
| 2 | +48 | damping divisor | none | clamped to >= a_5 + 0.001 when it changes, written back | 1.0 |

The descriptor ranges (min/max/default tables) could not be read from the image: the descriptor
data at 0x8292E7xx is runtime initialised (the image holds a fill pattern 0x02020202).

#### 1.4 Process (sub_8264E398) pseudo C, VERIFIED

```c
int RM10_Process(RM10* o, IO* io) {
    if (!(o->P0 > 0.0f)) {                       // fcmpu P0, 0.0 ; bgt
        if (o->prevP0 > 0.0f) {                  // transition on -> off: flush state
            for (i = 0..5) if (comb[i].buf) { memset(comb[i].buf, 0, comb[i].size*4); comb[i].pos = 0; }
            for (i = 0..apCount-1) if (ap[i]->buf) { memset(ap[i]->buf, 0, ap[i]->size*4); ap[i]->pos = 0; }
        }
        o->prevP0 = o->P0;
        return 0;                                // bus goes silent
    }
    if (o->P0 != o->prevP0 || o->P1 != o->prevP1 || o->P2 != o->prevP2 || System->rate != o->cachedRate)
        if (!RM10_Update(o)) return 0;           // sub_8264DF68, 0 only on alloc failure
    float* in  = io_cur_buffer(io);              // channel 0 only
    float* s   = scratch(io, nch * 1024);        // sub_826581A8, return 0 if null
    comb_block(&comb[0], 256, in, s, /*overwrite*/1);
    for (i = 1..5) comb_block(&comb[i], 256, in, s, /*accumulate*/0);
    switch (nch) {
    case 1:  ap_block(ap[0], 256, s, in);  return 1;          // in place, NO flip
    case 2:  ap_block(ap[1], 256, s, s+256);                  // R
             ap_block(ap[0], 256, s, s);  break;              // L (in place on the sum)
    case 4:  ap_block(ap[0], 256, s, s+512);                  // Ls
             ap_block(ap[1], 256, s, s+768);                  // Rs
             memcpy(s, s+512, 1024); memcpy(s+256, s+768, 1024); break;   // L=Ls, R=Rs
    default: ap_block(ap[0], 256, s, s+768);                  // ch3
             ap_block(ap[1], 256, s, s+256);                  // ch1
             ap_block(ap[2], 256, s, s+1024);                 // ch4
             memcpy(s, s+768, 1024);                          // ch0 = AP0
             memcpy(s+512, s+1024, 1024);                     // ch2 = AP2
             memset(s+1280, 0, 1024); break;                  // ch5 = 0
    }
    io->cur = (io->cur == 0);
    return 1;
}
```

6 ch result (INFERRED channel names): L = AP0, C = AP1, R = AP2, Ls = AP0 (copy of L),
Rs = AP2 (copy of R), LFE = 0. The 6-ch branch is also taken for nch = 3 or 5, where it writes past
a 3 or 5 channel scratch allocation (original bug, irrelevant for the game which uses 1/2/4/6).

Note the order of allpass calls matters only for which buffer is read: every allpass reads the
comb sum `s` (channel 0 region) and in the 6-ch and 4-ch paths channel 0 is overwritten only after
all allpasses ran; in the stereo path AP0 runs last and in place.

#### 1.5 Comb filter (sub_8264DE20 segmenting + sub_8264D480 kernel), VERIFIED

State `w[]` = ring buffer of size L+2 (zeroed on allocation). Per sample, with `w[n-k]` meaning
the ring entry k positions behind the write position:

```c
float t  = fnmsubs(w[n-1], c4, x);        // = x + a*w[n-1]      (single rounding)
float wn = fnmsubs(w[n-L], c8, t);        // = t + b*w[n-L]      (single rounding)
w[n] = wn;                                // stored before the output reads
float y  = fmadds(w[n-L-1], c12, w[n-L]); // = w[n-L] - a*w[n-L-1]
out[n] = overwrite ? y * K : fmadds(y, K, out[n]);   // K = 0x3E2AAAAB = 0.16666667
```

`c4 = c12 = -a_i`, `c8 = -b_i` (the code stores negated gains and uses fnmsubs/fmadds, reproduce
exactly that: negated constants, fused ops). Transfer function
`H(z) = z^-L (1 - a z^-1) / (1 - a z^-1 - b z^-L)`, a lowpass-feedback comb whose loop DC gain is
`b/(1-a) = 1 - 0.366/T`.

The segmenting in DE20 only splits the 256-sample block so that no index wraps inside a call to
the kernel; it is mathematically a plain ring buffer (read positions `pos-1`, `pos-L`, `pos-L-1`
modulo size, write at `pos`, then `pos++`, `pos = 0` when `pos >= size`). Any ring with the same
logical history and the same clearing behaviour is bit identical.

#### 1.6 Allpass (sub_8264DD70 segmenting + sub_8264D098 kernel), VERIFIED

Ring `v[]` of size len+1. Output scale computed per block:
`chanF = (float)ap->nch; if (ap->nch > 4) chanF = chanF - 1.0f; scale = 2.0f / chanF`
(`0x40000000`). So mono 2.0, stereo 1.0, quad 0.5, 6 ch 0.4 (LFE excluded).

```c
float vn = fnmsubs(v[n-len], g, x);       // = x - g*v[n-len]
v[n] = vn;
float y  = fmadds(vn, g, v[n-len]);       // = g*v[n] + v[n-len]  (v[n-len] re-read, same value)
out[n] = y * scale;
```

#### 1.7 Update (sub_8264DF68), VERIFIED

```c
int RM10_Update(o) {
    float rate = System->rate;
    bool roomChanged = false, rateChanged = false;
    if (o->P1 != o->prevP1) {                    // room
        rooms(o, &o->P1, o->t);                  // D7F8, may clamp P1 in place
        lengths(rate, o->t, o->L);               // D8A8
        damping(o, rate, o->a);                  // D9A0
        feedback(o, o->b);                       // DBB8
        roomChanged = true;
    } else if (rate != o->cachedRate) {
        lengths(rate, o->t, o->L); damping(o, rate, o->a); feedback(o, o->b);
        rateChanged = true;
    } else {
        if (o->P2 != o->prevP2) damping(o, rate, o->a);
        feedback(o, o->b);
    }
    for (i = 0..5)                               // D378: set gains, realloc only if size changes
        if (!comb_set(&comb[i], o->L[i], -o->a[i], -o->b[i], -o->a[i], o->L[i] + 2)) return 0;
    if (o->apInit && !rateChanged) {
        if (roomChanged) for (each ap) { memset(buf,0,size*4); pos = 0; }
    } else {
        configure allpasses (table below) and call ap_set(ap[i], len_i, g_i) for each  // CF80
        o->apInit = 1;
    }
    o->prevP0 = o->P0; o->prevP1 = o->P1; o->prevP2 = o->P2; o->cachedRate = rate;
    tail(o);                                     // DC50
    return 1;
}
```

`comb_set` (D378): always writes `L, c4, c8, c12`; if `size != L+2` it frees, allocates
`(L+2)*4` bytes, zeroes them, `pos = 0`. Same size keeps the history (no click on a P0 change).
`ap_set` (CF80): writes len, g, size = len+1, ALWAYS frees and reallocates a zeroed buffer,
`pos = 0`.

Allpass configuration (`len = round(rate * c)`):

| nch | AP0 (c, g) | AP1 (c, g) | AP2 (c, g) |
|---|---|---|---|
| 1 | 0.006 `0x3BC49BA6`, 0.7 `0x3F333333` | | |
| 2 or 4 | 0.0066666668 `0x3BDA740E`, 0.63 `0x3F2147AE` | 0.0054 `0x3BB0F27B`, 0.7777778 `0x3F471C72` | |
| other | 0.0066666668, 0.63 | 0.006, 0.7 | 0.0054, 0.7777778 |

At 48 kHz: 0.006 -> 288, 0.0066667 -> 320, 0.0054 -> 259 (259.20001 rounds to 259). Computed.

#### 1.8 Room to distances (sub_8264D7F8), VERIFIED

```c
float x = P1;
if (x > 83.3f /*0x42A6999A*/ || x < 2.0f) { x = (x > 83.3f) ? 83.3f : 2.0f; P1 = x; }  // written back
float mn = x * 0.8f;                 // 0x3F4CCCCD
float mx = mn * 1.5f;                // 0x3FC00000
if (mx > 100.0f) { P1 = 83.333328f /*0x42A6AAAA*/; mx = 100.0f; mn = 66.666664f /*0x42855555*/; }  // unreachable after the clamp
float st = (mx - mn) * 0.2f;         // 0x3E4CCCCD
t[0] = mn; t[1] = mn + st; t[2] = t[1] + st; t[3] = t[2] + st; t[4] = t[3] + st; t[5] = mx;
```

#### 1.9 Distances to prime lengths (sub_8264D8A8), VERIFIED

Table: 1652 floats at 0x8207FF90, all primes from 2 to 13999 in ascending order (checked
programmatically: integral, prime, sorted).

```c
L[5] = 0; int k = 0;                           // k persists across i
for (i = 0..5) {
    float f11 = t[i] * 0.0029002321f;          // 0x3B3E11D2 (1/344.8 m/s)
    float r, scale;
    if (rate > 48000.0f) { r = 48000.0f; scale = rate * 2.0833333e-5f; }   // 0x37AEC33E = 1/48000
    else                 { r = rate;     scale = 1.0f; }
    float s = r * f11;
    while (k < 1652 && !(prime[k] > s)) k++;
    if (k < 1652) { L[i] = (int)prime[k]; k++; }  // else L[i] keeps its old value
    if (scale > 1.0f) L[i] = fctiwz((float)L[i] * scale);
}
```

So each length is the smallest prime strictly greater than the delay in samples AND strictly
greater than the previous comb's prime. At 48 kHz the largest request is 99.96 m -> 13914.9 so the
search never runs off the table.

Precomputed at 48 kHz (float emulation of the exact op sequence):
P1 = 15 -> t = 12, 13.2, 14.4, 15.6, 16.8, 18 m -> **L = 1693, 1847, 2011, 2179, 2339, 2521**;
P1 = 2 -> L = 223, 251, 269, 293, 313, 337; P1 = 83.3 -> L = 9281, 10211, 11149, 12071, 13001, 13921.

#### 1.10 Damping coefficients (sub_8264D9A0), VERIFIED

Table at 0x8207FE5C: rates `{10000, 25000, 50000}`. Then 3 records of 72 bytes at 0x8207FE68,
each `{float x[9]; float y[9];}`; x is the same in all three: `0, 12.5, 25, ..., 100` (metres).

| record (rate) | y[0..8] |
|---|---|
| 0 (10 kHz) | 0, 0.03, 0.07, 0.11, 0.14, 0.16, 0.18, 0.22, 0.24 |
| 1 (25 kHz) | 0, 0.12, 0.23, 0.30, 0.35, 0.40, 0.43, 0.46, 0.50 |
| 2 (50 kHz) | 0, 0.31, 0.45, 0.53, 0.57, 0.61, 0.64, 0.67, 0.70 |

(Values printed rounded, use the exact float bit patterns from the image.)

```c
int seg; float f;
if (rate >= 50000) { seg = 1; f = (50000 - 50000) * 4.0e-5f; }          // f = 0
else if (rate > 25000) { seg = 1; f = (50000 - rate) * 3.9999999e-5f; } // 0x3827C5AC
else { float r0 = max(rate, 10000); seg = 0; f = (25000 - r0) * 6.6666667e-5f; } // 0x388BCF65
float g = 1.0f - f;
for (k = 0..8) Y[k] = fmadds(rec[seg+1].y[k], g, rec[seg].y[k] * f);
float inv = 1.0f / (x[1] - x[0]);                                       // 0.08
for (i = 0..5) {
    int idx = 1; while (t[i] > x[idx]) idx++;                          // no bound check
    float u = (x[idx] - t[i]) * inv;
    float v = 1.0f - u;
    a[i] = fmadds(Y[idx], v, Y[idx-1] * u);
}
if (P2 != prevP2) {                                                     // only on a P2 change
    float lim = a[5] + 0.001f;                                          // 0x3A83126F
    if (P2 < lim) P2 = lim;                                             // written back
    for (i = 0..5) { memset(comb[i].buf, 0, size*4); comb[i].pos = 0;   // all combs flushed
                     a[i] = a[i] / P2; }
}
```

At 48 kHz, Y = 0, 0.2948, 0.4324, 0.5116, 0.5524, 0.5932, 0.6232, 0.6532, 0.6840 and for
P1 = 15: a = 0.283008, 0.302506, 0.315715, 0.328925, 0.342134, 0.355344.

Quirk (VERIFIED, reproduce it): the division by P2 is applied only during the update in which P2
itself changed. A later room or rate change recomputes `a` WITHOUT the P2 division, so the P2
effect is silently lost until P2 changes again. A P2 change also flushes all comb histories.

#### 1.11 Feedback (sub_8264DBB8), VERIFIED

```c
if (P0 < 0.366f /*0x3EBB645A*/) P0 = 0.366f;            // written back into the param slot
float d = 1.0f - 0.366f / P0;
for (i = 0..5) b[i] = (1.0f - a[i]) * d;
```

Example P1 = 15, 48 kHz: T = 1 -> b = 0.45457 .. 0.40871; T = 2 -> 0.58578 .. 0.52668;
T = 5 -> 0.66451 .. 0.59747.

#### 1.12 Tail (sub_8264DC50), VERIFIED

```c
float L5 = (float)L[5];
float lc = (float)log10(b[0]);                // note: comb 0 feedback, comb 5 length
float p  = L5 / lc;
float tc = fnmsubs(p, 10.0f, L5);             // L5 - 10*L5/log10(b0)
float gm = 0; for (i < apCount) if (apG[i] > gm) gm = apG[i];
int   lm = 0; for (i < apCount) if (apL[i] > lm) lm = apL[i];
float Lm = (float)lm;
float ta = fnmsubs(Lm / (float)log10(gm), 10.0f, Lm);
o->tail24 = o->tail64 = ta + tc;              // samples
```

### 2. Delay "Del0"

GetSize sub_8264E980 = 128, Create sub_8264F848, Process sub_8264F458, vtable 0x82081A0C
(`[1] Control = sub_8231C140 = no-op`). VERIFIED. Works on `nch = +29` (Voice channel count),
every channel independently, 100 % wet output.

#### 2.1 Layout (VERIFIED)

| off | meaning | Create |
|---|---|---|
| +32 | **P0** delay time, seconds (slot 0) | 0.0 |
| +40 | **P1** feedback, clamped to [-0.99, 0.99] and written back (slot 1) | 0.0 |
| +48 | state (0 idle, 1 run, 2 wait after growth, 3 crossfade) | 0 |
| +52 | cached rate | 0.0 |
| +60 | i32 crossfade countdown / offset | |
| +64 / +68 / +72 | current time / samples (i32) / fbNeg | |
| +76 / +80 / +84 | pending time / samples / fbNeg | |
| +88 / +92 / +96 | old time / samples / fbNeg | |
| +100 | float* buffer, planar, channel c at `buf + c*size` | null |
| +104 | i32 size (frames per channel) | 0 |
| +108 | float* ramp[256] (allocated on first crossfade) | null |
| +112 | i32 write pos (shared by all channels) | 0 |
| +116 / +120 | tail bookkeeping: samples / fbNeg | 0 / 0.0 |

`fbNeg = -P1` (clamped). The tail is published in `+20` (float delay samples) and `+24`.

#### 2.2 Process (sub_8264F458), VERIFIED

```c
int Del_Process(D* o, IO* io) {
    float rate = System->rate;
    if (rate != o->cachedRate) {
        if (o->buf) { memset(o->buf, 0, o->size * nch * 4); o->pos = 0; }
        o->cachedRate = rate; o->state = 0;
    }
    float T = o->P0;
    int   n = fctiwz(T * o->cachedRate);
    float fb = -o->P1;
    if (fabsf(fb) > 0.99f) {                     // 0x3F7D70A4
        if (fb > 0.0f) { fb = 0.99f;  o->P1 = -0.99f; }
        else           { o->P1 = 0.99f; fb = -0.99f; }
    }
    switch (o->state) {
    case 0:
        if (!(T > 0.0f)) return 1;               // dry passthrough: io untouched, no flip
        o->curT = T; o->curFb = fb; o->curN = n;
        if (n > o->size - 1) grow(o, n);         // F280
        o->state = 1; break;
    case 1:
        if (T == o->curT && fb == o->curFb) break;
        o->oldT = o->curT; o->newT = T; o->newFb = fb; o->oldFb = o->curFb;
        o->newN = n; o->oldN = o->curN;
        if (n > o->size - 1) {
            grow(o, n);
            o->cnt = n - o->oldN + 1;
            if (o->cnt < 256) { o->curT = T; o->curFb = fb; o->curN = n; o->state = 3; }
            else o->state = 2;
        } else { o->curT = T; o->curFb = fb; o->cnt = 0; o->curN = n; o->state = 3; }
        break;
    case 2:
        if (o->cnt < 256) { o->curT = o->newT; o->curFb = o->newFb; o->curN = o->newN; o->state = 3; }
        break;
    case 3: break;                               // parameter changes are ignored until state 1
    }
    float* in = io_cur_buffer(io); float* out = scratch(io, nch * 1024); if (!out) return 0;
    if (o->state == 1 || o->state == 2) {
        steady(o, in, out);                      // EC80/E9F0, uses curN/curFb (old values in state 2)
        if (o->state == 2) o->cnt -= 256;
    } else {                                     // state 3
        if (!o->ramp) o->ramp = alloc(1024);
        make_ramp(o->ramp, 256, 1.0f, 0.0f, -o->cnt, 128);   // 82659F98
        crossfade(o, in, out, 256);              // F140/ED28
        o->cnt -= 256;
        if (-o->cnt >= 128) o->state = 1;
    }
    io->cur = (io->cur == 0);
    if (o->state == 1 && !(o->curT > 0.0f)) { clear buffer, pos = 0 (F228); o->state = 0; }
    update_tail(o);                              // see 2.6
    return 1;
}
```

Turning the delay off from state 1 therefore crossfades to delay 0 for 128 samples, then clears
the line and goes idle (passthrough).

#### 2.3 Steady kernel (sub_8264E9F0), VERIFIED

Per channel c, per sample, `rd = pos - curN` (mod size), `wr = pos`:
`buf[c][wr] = fnmsubs(buf[c][rd], curFbNeg, x)` (= x + P1*buf[rd]), then
`out = buf[c][rd]` (read AFTER the write, so for curN = 0 the output is the sample just written).

#### 2.4 Crossfade kernel (sub_8264ED28), VERIFIED exact op order

`rN = pos - curN`, `rO = pos - oldN` (both mod size), `r = ramp[j]`:

```c
float om = 1.0f - r;
float a  = buf[rO] * oldFbNeg;
float bn = buf[rN] * om;
float c  = a * r;
float d  = fmadds(bn, curFbNeg, c);
buf[pos] = x - d;
float e  = buf[rO] * r;                       // re-read after the write
out      = fmadds(buf[rN], om, e);
```

#### 2.5 Ramp generator (sub_82659F98, shared), VERIFIED

`make_ramp(dst, n, start, end, off, len)`: sample i corresponds to t = off + i.
t < 0 -> start. 0 <= t <= len-1 -> if end >= start: `(float)(t+1) * ((end-start)/len) + start`,
else `fnmsubs((float)len - (float)(t+1), (end-start)/len, end)` which for (1, 0, 128) is exactly
`(127 - t) / 128`. t >= len -> end. So the delay crossfade weight on the OLD tap is 127/128 at
t = 0 and reaches 0 at t = 127.

#### 2.6 Buffer growth (sub_8264F280) and tail (sub_8264F3C0), VERIFIED

`grow(o, n)`: new size `n+1`, allocate `nch*(n+1)` zeroed floats. If an old buffer exists and
state != 0, for each channel copy `old[pos+1 .. oldSize-1]` to `new[0..]` followed by
`old[0 .. pos]`, set `pos = oldSize - 1`, free old. If state == 0: no copy, `pos = 0`. No old
buffer: `pos = 0`. The line never shrinks.

Tail update at the end of Process: in states 0, 1, 2 compare `(curN, curFbNeg)` with
`(+116, +120)`; in state 3 compare `(max(curN, oldN), the one of curFbNeg/oldFbNeg with the larger
magnitude, ties -> oldFbNeg)`. On a difference store them and compute:
`+20 = D = (float)N; +24 = (fb == 0) ? D : fnmsubs(D / (float)log10(|fb|), 5.0f, D)`
(`0x40A00000`).

### 3. VuMeter "VuM0"

GetSize sub_8264F950 = 304, Create sub_8264FD88, Process sub_8264FD20, vtable 0x82081A38.
**Does not modify audio**: Process calls sub_8264FB78(o, curBuffer, 256) and returns 1, no flip.
VERIFIED. It writes NO global: the value 0x8299FDCC is the descriptor's last field (a runtime
initialised table pointer, INFERRED to describe the 18 output params), not something Process
touches (VERIFIED: no store outside the object in FB78/FA08).

Layout: `+32 window = rate * 0.01f` (= 480.0 at 48 kHz), `+36 counter`, params (18 slots, +40 ..
+176, output only), `+184 sumSq[6]`, `+208 rms[6]`, `+232 winPeak[6]`, `+256 lastPeak[6]`,
`+280 hold[6]`.

Per frame (frames outer, channels inner, nch = +29), VERIFIED:

```c
for (c) { float v = x[c]; if (v <= 0.0f) v = v * -1.0f;
          if (winPeak[c] < v) winPeak[c] = v;
          if (hold[c] < v) { hold[c] = v; publish(); }          // FA08
          sumSq[c] = fmadds(x[c], x[c], sumSq[c]); }
cnt += 1.0f;
if (cnt >= window) { for (c) { lastPeak[c] = winPeak[c]; sumSq[c] = sumSq[c] / cnt; rms[c] = sqrtf(sumSq[c]); }
                     publish(); cnt = 0; sumSq[0..5] = 0; winPeak[0..5] = 0; }
```

`publish` (FA08) copies into param slots: RMS -> slots 0..5, lastPeak -> 6..11, hold -> 12..17,
channel c mapped to RW slot: mono -> 1; stereo -> 0, 2; quad -> 0, 2, 3, 4; other -> 0..5.
(Mono writes only slots 1, 7, 13; for nch >= 2 slot 12 gets hold[0].)
Hold never decays. Control (any index, sub_8264FD58) queues sub_8264FCF8 -> sub_8264F958 which
zeroes all 18 slots, rms, lastPeak, hold (NOT sumSq, winPeak, cnt). VERIFIED.
Native rewrite: only needed if something reads the meters; it has no audible effect.

### 4. AiffWriter "AiW0"

GetSize sub_82651880 = 44, Create sub_82651990 (`+32 FILE* = null`), Process sub_82651C98.
Debug capture only, never alters audio, never flips. VERIFIED. If `+32 == null` Process returns 1
immediately. Otherwise: `+40 = (int)rate` once; converts the current block, channel count `+28`,
to interleaved 16-bit big-endian PCM: `clamp(x, -1, 1) * 32767.0f` (`0x46FFFE00`), `fctidz`
(truncate), low 16 bits; `fwrite(buf, 1, nch*512, file)` (sub_827A4F68); `+36 += 256` frames.
Controls (sub_82651F30): 0 = open file named by a string (queued fn 0x82651908), other = close
(0x82651A08, INFERRED: finalises the AIFF header). The game does not open a file (INFERRED from the
master voice init, which only sends controls to the Dac). Native rewrite: drop it.

### 5. Dac "Dac0" (the output stage)

GetSize sub_826505F0 = 12640, Create sub_82650E20, Process = none, vtable 0x82081AE8
(`[0] dtor/shutdown sub_82650A30, [1] Control sub_82651218, [2] delete sub_82651360`).

#### 5.1 Create (VERIFIED)

* Params (slot table at +32): `+32 = 3.0 (0x40400000)`, `+40 = 48000.0` (first entry of the rate
  table 0x82081A88 = {48000, 0, ...}), `+48 = 0.0`. Supported-rate list built at +96 (only 48000),
  `+141` = 1 entry, `+124 = 3`, `+143` = 6 output channels, `+140/+142/+144 = 0`.
* `+56 = sub_8265AD30(System)` (the io block the Dac renders into), `+60 = sub_8265ACF0(System)`
  (profiling timer).
* `System->rate (+12) = +40 = 48000.0`; `System->period (+8) = 256.0f / 48000.0f`.
* Global `u8 0x8299FDD4 = 1` ("running").
* Creates the XAudio render driver (sub_821168A8, handle at +148) with: 6 channels, (int)48000,
  callbacks `0x826509E8` and `0x82650A08`, context = Dac object. INFERRED field meanings
  (`sp+148 = 6, sp+152 = rate, sp+220/224 = callbacks, sp+232 = this`).
* Two packets: buffers `+152` and `+6296` (6144 bytes each = 256 x 6 x 4), packet descriptors
  88 bytes at `+12452 + 88*i`: `+0 buffer, +4 6144, +84 &state[i]`; `state[i]` at `+12628 + 4*i`
  (0 free, 1 filled, 2 submitted). `+12440` submit index, `+12444` fill index, `+12448` event
  (sub_82116908(0, 1)).
* Starts a worker thread, entry 0x82650DF8 -> sub_82650B78(dac).

#### 5.2 Controls (sub_82651218), VERIFIED

| idx | behaviour |
|---|---|
| 0 | synchronous query: `data[0]` float in; `data[1] = 0.0`; `data[2] = table[(int)data[0]]` (table 0x8292EA94, runtime); `data[1] = 1.0` if `data[0]` equals one of the +141 ints at +124 (holds 3). INFERRED: "is this speaker config supported" |
| 1 | queues fn 0x82650AB0 with the 4-byte value: the fn is `return 12`, a **no-op** |
| 2 | queues fn 0x82650AB8: also a **no-op** |
| 3 | queues fn 0x82650AC0 = **Start**: if not running (+142 == 0): `+144 = 1` (fade-in pending), driver SetVolume-like call sub_82116698(driver, 1.0), `+142 = 1`; start profiling timer; `+72 = mftb` |
| >= 4 | queues fn 0x82650B18 = **Stop**: `+142 = 0`, sub_82116698(driver, 0.0) |

So the game's init `Control(1, &3.0f)` has NO effect in this build, and `Control(3, NULL)` starts
output. The param `+32 = 3.0` is never read by the render path (VERIFIED in B78, 0810, AC0).

#### 5.3 Worker thread (sub_82650B78), VERIFIED

```c
lock(System+84);                                   // held for the thread's lifetime (8264C400)
while (running) {
    KeWaitForSingleObject(event, INFINITE);        // 821169A8(event, -1, 0)
    if (state[fill] != 0) continue;
    while (running) {
        timer_mark(+60);
        float* pkt = packet[fill].buffer;
        if (!started /*+142*/) { drain_commands(System); if (!running) goto out; }   // 8264C540
        else {
            +72 = mftb; drain_commands(System);    // param/control queue applied HERE, once per block
            if (!running) goto out;
            if (started) {
                int ok = render(dac->Voice, io=+56, System+20);    // 8265A148: whole graph
                System->time(double +0) += System->period;
                if (ok == 1) { finalize(dac); memcpy(pkt, io_cur_buffer(+56), 6144); }   // 82650810
                else memset(pkt, 0, 6144);
                state[fill] = 1;
                while (state[submit] == 1) {
                    driver_get_status(driver, &st);                // 821163B8 (vtbl+48)
                    if (!(st & 0x20)) break;                      // driver cannot take a frame
                    state[submit] = 2;
                    submit_frame(driver, &packet[submit]);        // 821164B8 (vtbl+28), XAudioSubmitRenderDriverFrame
                    submit = (submit + 1) & 1 (as 0/1 toggle);
                }
                fill = (fill + 1) toggle;
            }
        }
        if (started) { +68 += mftb - +72; }
        timer_stop(+60);
        if (state[fill] != 0) break;               // both packets busy: wait for the driver
    }
}
out: unlock(System+84);
```

`drain_commands` (8264C540) under System mutex +88: runs the per-block callback list at +100
with `f1 = period`, executes every queued command record (fn at record+0 returns its size),
records the high water mark at +44, resets the queue, `System+20++` (block counter).

Driver callbacks: 0x82650A08 (packet done): `*packet->statePtr = 0; KeSetEvent(event)`.
0x826509E8: `if (running) KeSetEvent(event)`. So the whole mixer runs on the Dac thread, paced by
the render driver, with at most 2 packets (2 x 5.333 ms) in flight.

#### 5.4 Finalize (sub_82650810), VERIFIED

1. `s = scratch(io, 6*1024)`; **interleave**: `s[f*6 + c] = cur[c*256 + f]` for c < 6, f < 256.
2. Flip `io->cur`; from now on the current buffer is the interleaved one.
3. Channel reorder per 6-float frame (sub_8265A850, 6-ch path; 2/4-ch paths are no-ops):
   `n0 = o0, n1 = o2, n2 = o1, n3 = o5, n4 = o3, n5 = o4`, i.e. RW `L C R Ls Rs LFE` ->
   XAudio `FL FR FC LFE RL RR` (names INFERRED, permutation VERIFIED).
4. If `+144` (first block after Start): fade in, sub_826505F8(dac, buf, 128, 1): frame k
   (k = 0..127) of all 6 channels multiplied by `(float)k * (1.0f/128.0f)` (the factor
   accumulates as `f0 += 1.0f; f13 = f0 * (1/128)` starting from 0, so frame 0 is muted and frame
   127 gets 127/128); frames 128..255 unchanged. Then `+144 = 0`.
5. Clamp all 1536 samples to [-1.0, 1.0] (sub_8265A810: `x < -1 -> -1`, `x > 1 -> 1`, NaN passes
   through unchanged).

**CORRECTION (measured 2026-10-20, main session):** at the kernel import the frame is
**channel-sequential (planar)**, as the SDK assumes. Test on `--audio_dump_raw` (SDK
`AudioSystem::SubmitFrame`, ref4.raw, 300 loudest blocks): median autocorrelation of 256-sample
runs is 0.98 at lag 1 and 0.79 at lag 6, of the whole 1536 floats 0.988 / 0.883; interleaved data
would peak at lag 6. So something between `sub_82650810` and the import (likely the XDK wrapper
`sub_821164B8`, vtbl+28) puts the data back channel by channel. The finalize steps below
(interleave, reorder, fade, clamp) are still what the samples go through; the native output must
match the import's planar frame. To be checked: whether the reorder survives the wrapper
(channel order at the import).

No gain, no dither, no other scaling. **The frame handed to XAudioSubmitRenderDriverFrame is
INTERLEAVED (frame-major, 6 floats per frame, XAudio channel order), not planar**, native
big-endian float, 256 frames, 6144 bytes. VERIFIED (interleave loop and the 24-byte-stride reorder
both prove it). This contradicts the "planar" assumption in the task brief; the host side shim
must treat the packet as interleaved.

### 6. Native rewrite checklist

* RM10: P0 <= 0 means silent bus (return 0, flush on the on->off edge). Reproduce the write-backs
  of clamped P0/P1/P2 into the param slots (the game may read them back).
* RM10: keep comb histories across P0 changes, reallocate (zero) only on length change; flush all
  combs on a P2 change; zero allpasses on a room change; reallocate allpasses on rate change.
* Use fused multiply-adds exactly where listed, and the negated gain constants.
* Del0: state machine including "changes ignored while crossfading" and dry passthrough when idle.
* Dac: interleave, reorder `[0,2,1,5,3,4]`, fade-in 128 frames after Start, clamp, double buffer.
  Controls 1 and 2 are no-ops.

### 7. Open questions

1. RM10 `+72` (previous room) is not written by Create or sub_8264E808. If the allocator returns
   zeroed memory the first update always takes the "room changed" path (the expected behaviour);
   otherwise the first update could skip `rooms()` and run with zero lengths. Check the allocator
   used by the plug-in factory.
2. The values the game actually sets for RM10 P0/P1/P2 and Del0 P0/P1 (environment presets) were
   not traced: sub_82325510 only registers the classes. Find the SetParamF callers for these
   voices to know the operating points.
3. Param descriptor tables (names, min/max/default) at 0x8292E7xx are runtime data; dump them from
   a live process to confirm units.
4. Exact semantics of the render driver status bit 0x20 and of sub_82116698 (vtable slot 23 of the
   driver, stores the float at driver+140) are INFERRED (ready flag, volume).
5. Dac Control 0's table at 0x8292EA94 and the meaning of `+124 = 3` (speaker configuration id?)
   are unresolved; not needed for audio output.
6. Channel names (L C R Ls Rs LFE inside RW) are INFERRED from three consistent sources; confirm
   with a panned test sound if the native mixer relies on them.

### 8. Corrections found by the differential self-test (2026-10-20)

* **Del0 crossfade kernel sub_8264ED28 is NOT uniform across samples.** The loop is unrolled by 4
  (while `i + 3 < count`, then a scalar remainder loop) and the original compiler reassociated the
  third unrolled copy: lane 2 computes `c = (buf[ro] * r) * oldFbNeg`, lanes 0, 1, 3 and the
  remainder compute `c = (buf[ro] * oldFbNeg) * r`. The lane is counted from the start of each
  segment cut by sub_8264F140 (segments end where the write or either read position wraps; the
  write position is reset to 0 at the START of a segment when `pos >= size`, so after a block it
  can be left equal to `size`). Ignoring this gives 1 ulp errors in about a quarter of crossfade
  samples, which the feedback then carries forward.
* The steady kernel is uniform (its lanes differ only in operand order of a product).
* Native implementation: `port/src/native_audio/effects.cpp`, verified bit for bit (40 random
  cases per class, 4090 delay blocks, 2734 reverb blocks, line contents, positions, state, tail and
  written-back parameters compared after every block).
