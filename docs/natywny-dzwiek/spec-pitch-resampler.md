# RenderWare Audio Core: Pitch, SndPlayer resampler, voice graph execution

Scope: class `Pitch` ("Pit0"), the resampler owned by `SndPlayer` ("SnP0"), the SndPlayer
block pipeline that feeds it, the channel format converter used on decoder output, and the
voice graph executor that calls `Process`. Goal: a native C++ rewrite with bit identical output.

Tags: **[V]** = VERIFIED (read in the disassembly, addresses given), **[I]** = INFERRED
(naming, purpose or behaviour not proven by code read in this pass).

All float constants are single precision unless stated. Addresses are absolute (lis/lfs pairs
already resolved).

### 0. Constants used in this document

| Address | Hex | Value | Used by |
|---|---|---|---|
| 0x820013B8 | 0x00000000 | 0.0f | many |
| 0x820013C0 | 0x3F800000 | 1.0f | Pitch init, io+12 init, unity test, converter gain |
| 0x82005B10 | 0x47800000 | 65536.0f | SetRate |
| 0x820037C8 | 0x3F000000 | 0.5f | SetRate rounding |
| 0x820BD0D0 | 0xBF800000 | -1.0f | resampler init rate |
| 0x820BD208 | 0x40800000 | 4.0f | resampler rate cap |
| 0x82082274 | 0x377FFC9C | 1.5258e-05f (exactly 1.52579996e-05, NOT 1/65536 = 1.52587891e-05) | interpolation fraction scale |
| 0x8208228C | 0x3F34FDF4 | 0.7071f (0.70709997) | center/mono spread in converter |
| 0x82082070..0x820820AC | 0x3F70F0F1 .. 0x3D70F0F1 | (16-k)/17, k=0..15 | declick ramp |
| 0x82036EB8 | 0x43800000 | 256.0f | voice silent-frame counter step |
| 0x82042FD8 | 0x4070000000000000 (double) | 256.0 | SndPlayer start scheduling |
| 0x820BD200 | 0x0000000000000000 (double) | 0.0 | SndPlayer start scheduling |
| 0x82006BA4 | 0x3EAAAAAB | 1/3 | voice profiling average only |

Interpolation scale note [V]: the fraction is a 16 bit integer `frac` (0..65535) converted as
`(float)frac * 1.5258e-05f`, so the largest weight is 0.99992... and frac=0x8000 gives
0.49994..., not 0.5. A native port MUST use the same constant bits 0x377FFC9C.

## 1. Voice graph execution (pull model)

### 1.1 Who drives it [V]

Mixer thread loop `sub_82650B78` (Dac object `dac`, r31):
`sub_8265A6C0(dac+64)` (pending command queue, [I]), then
`ret = sub_8265A148(voice = dac+8, io = dac+56, stamp = System(dac+4)+20)`,
then `System+0 (double) += System+8 (float)` (stream time, block duration [I]).
If `ret == 1`: `sub_82650810(dac)` then `memcpy(packet, io->buf[io->cur], 6144)`; else
`memset(packet, 0, 6144)`. 6144 bytes = 6 channels x 256 frames x float, planar [V].
`sub_82650798` is a second (single shot) caller with identical argument shape [V].

### 1.2 io descriptor ("BufferSet", passed as r4 to every Process) [V]

| Off | Type | Meaning |
|---|---|---|
| +0 | ptr | allocator / System context passed to alloc/free (`sub_8264CBB8`, `sub_8264C8F0`) |
| +4 | float* | buffer[0] |
| +8 | float* | buffer[1] |
| +12 | float | **rate multiplier** requested from the current plug-in's output. Set to 1.0f at graph entry. NOT a sample rate. |
| +16 | float | set to 1.0f at the start of every voice [V]; consumer unknown [I] |
| +20 | u32 | capacity in bytes of buffer[0] |
| +24 | u32 | capacity in bytes of buffer[1] |
| +28 | u8 | index of the current buffer (0/1) |

Correction to the brief: buffer pointers are at **io+4 / io+8** (`lwzx r, (cur+1)*4, io`), not
io+32. Channel count is not in io: it is **u8 at plug-in instance +29** (`lbz 29(instance)`).

`sub_826581A8(io, bytes)` = get the *other* buffer [V]:
```c
float* io_get_next(IO* io, uint32_t bytes) {
    int o = io->cur ^ 1;
    if (bytes > io->cap[o]) {
        if (io->buf[o]) free(io->ctx, io->buf[o]);      // sub_8264C8F0(ctx, p, 0)
        io->cap[o] = bytes;
        io->buf[o] = alloc(io->ctx, bytes, name=0x82081E74, align=128); // sub_8264CBB8
        if (!io->buf[o]) return NULL;
    }
    return io->buf[o];
}
```
It does not toggle. The writer toggles after filling: `io->cur = (io->cur == 0)` (cntlzw idiom) [V].
So the protocol is: read `io->buf[io->cur]` (input), write `io_get_next()`, toggle. A plug-in
that processes in place does not toggle. Buffers are 128 byte aligned [V].

Block layout: 256 frames, planar, channel c at `buf + c*256` floats (1024 bytes stride) [V]
(SndPlayer pad loop, executor zero fill `ch << 10` bytes).

### 1.3 Voice object (fields used here)

| Off | Type | Meaning | Tag |
|---|---|---|---|
| +8, +12, +16..+28 | | profiling (cycle counters, running averages with 1/3) | [V] content, irrelevant to audio |
| +32 | ptr | lock object / System (sub_8264C4C0 / sub_8264C4E8 = lock/unlock) | [V] use, [I] name |
| +36 | Voice* | parent voice during traversal (temporary stack link) | [V] |
| +40 | Entry* | per plug-in table, 16 bytes per entry: +0 = rate function (class desc +20) or flag, +4 = Process (class desc +24) | [V] offsets, [I] source of values |
| +44 | float | copy of io+12 when the voice starts processing | [V] |
| +48 | float | frames of silence produced since the source went quiet | [V] |
| +52 | float | tail length in frames (max of latencies) | [V] |
| +56 | u32 | stamp of the last block in which this voice ran | [V] |
| +68 | u8 | plug-in count n | [V] |
| +69 | u8 | how many plug-ins ran in the previous block | [V] |
| +70 | u8 | state: 0 playing, 1 stopping (tail), 2 stopped | [V] |
| +72 | Plugin*[n] | plug-in instances, index 0 = source | [V] |

Plug-in instance base: +0 vtable, +4 System, +8 Voice, +12 param table, +24 float latency in
frames (summed by `sub_82650018`), +29 u8 output channel count [V for +24, +29].

### 1.4 Executor `sub_8265A148(Voice* v, IO* io, uint32_t stamp)` [V]

```c
int run_graph(Voice* v, IO* io, uint32_t stamp) {
    io->rate = 1.0f;                     // stfs 1.0 -> io+12
    v->parent = NULL;                    // root v+36 = 0
    int ret = 0;
    Voice* cur = v;
  enter:                                 // loc_8265A19C
    if (cur->stamp == stamp || voice_is_stopped(cur)) goto leave_voice;  // sub_826500F8
    cur->prof_acc = 0; cur->prof_t0 = now();
    io->f16 = 1.0f;
    cur->rate_in = io->rate;             // v+44
    // 1) rate propagation, from the plug-in nearest the output back to index 1
    for (int i = cur->n - 1; i > 0; --i)
        if (cur->entry[i].rate_fn) cur->entry[i].rate_fn(cur->plugin[i], io);
    if (cur->entry[0].rate_fn == 0) goto run_chain;  // plain voice
  children:                              // loc_8265A24C, plug-in 0 is a mixing bus
    {
        Bus* b = (Bus*)cur->plugin[0];
        if ((int16_t)b->child_i < (int16_t)b->child_n) {        // +144 < +146
            if (!b->iter) b->iter = b->child_head;               // +48 from +36
            Voice* child = b->iter->voice;                       // node+12
            child->parent = cur;
            if (b->child_i == 0) b->saved_rate = io->rate;       // +76
            io->rate = b->saved_rate;    // every child starts from the bus rate
            b->child_i++;
            b->iter = b->iter->next;
            cur = child;
            goto enter;
        }
        b->child_i = 0;
    }
  run_chain:                             // loc_8265A26C
    int i;
    for (i = 0; i < cur->n; ++i) {
        ProcessFn p = cur->entry[i].process;
        if (!p) continue;                // e.g. Pitch: no Process, skipped
        int first = (i >= cur->ran_last) ? 1 : 0;   // r5
        ret = p(cur->plugin[i], io, first);
        if (ret) { if (i == 0) cur->silent = 0.0f; continue; }
        if (i != 0) break;               // a downstream plug-in produced nothing: stop chain
        // source produced nothing: ring out tails
        float t = voice_tail(cur);       // sub_82650018
        if (cur->tail < t) cur->tail = voice_tail(cur);
        if (!(cur->silent < cur->tail)) break;
        uint32_t bytes = cur->plugin[0]->channels << 10;     // instance+29 * 1024
        float* z = io_get_next(io, bytes);
        if (!z) return 0;
        memset(z, 0, bytes);
        io->cur = (io->cur == 0);
        ret = 1;
        cur->silent = cur->silent + 256.0f;
    }
    cur->ran_last = (uint8_t)i;          // v+69
    cur->stamp = stamp;
    /* profiling update omitted (no audio effect) */
  leave_voice:
    if (cur->parent) { cur = cur->parent; goto children; }   // loc_8265A24C via +36
    return ret;
}
```
Notes:
* `voice_tail()` = `0.0f + lat[1] + lat[2] + ... + lat[n-1]` summed left to right in float,
  plug-in 0 excluded [V].
* `voice_is_stopped()` (`sub_826500F8`): state 0 returns false; state 1 updates
  `tail = max(tail, voice_tail())` and, if `silent >= tail`, sets state 2 and zeroes v+16, v+20,
  v+24; returns `state == 2` [V].
* Order of operations per voice: **all rate functions first (n-1 down to 1), then children (if
  bus), then Process of index 0..n-1 in ascending order** [V]. The source is index 0; each
  following plug-in consumes `io->buf[io->cur]` produced by the previous one.
* Rate propagation is cumulative down the bus tree: a bus voice multiplies io+12 with its own
  rate functions, saves it, and every child voice starts from that saved value and multiplies
  further [V]. A child voice's result therefore runs at `bus_pitch * child_pitch`.
* How child outputs are summed into the bus is done by plug-ins (Route/Send/SubMix), out of
  scope [I].
* `sub_826502A8(voice, mode)` [V]: under lock, if state != 2: `mode == 0` stops immediately
  (state 2, zero +16/+20/+24, posts deferred record {fn 0x826501C8, voice} into the System
  queue at System(+32)->+32/+40); `mode != 0` and state 0 sets state 1 and `tail =
  voice_tail()`. SndPlayer uses mode -1 (graceful, tail rings out).
* `sub_82650190(rec)` [V]: deferred record handler: pushes node `rec->+4` at the head of the
  doubly linked list `node->+32->+96`, returns record size 8.
* `sub_8265C060(sys)` registers two codec classes (entries returned by `sub_826615D0`,
  0x8292F270, and `sub_82659AC8`, 0x8292F0C4; the latter also initialises the XMA hardware
  contexts) into the codec registry created lazily by `sub_8265C0A8` at System+56 [V].

## 2. Pitch ("Pit0")

Class descriptor 0x82937630 [V]: name 0x820825EC "Pitch", FourCC 0x50697430, GetSize
0x826625F0, Create 0x82662620, +16 0x8292F40C, **+20 rate fn 0x82662608**, **+24 Process = 0**,
+28 0x00010000 (1 parameter [I]).

Object (40 bytes, GetSize returns 40) [V]:

| Off | Type | Meaning |
|---|---|---|
| +0 | vtable 0x820825F4 | |
| +16 | 0x8292F430 | parameter descriptor table pointer (passed to base init) |
| +32 | float | parameter 0: pitch ratio, initial 1.0f |
| +36 | | rest of the 8 byte param slot |

Create (`sub_82662620(desc, a, b, mem)`): `obj->vtbl = 0x820825F4; obj+16 = 0x8292F430;
sub_8264CDB8(obj, 0x8292F430, a, b, 32 /*param area offset*/); obj->pitch = 1.0f` [V].

Rate function (`sub_82662608(Pitch* p, IO* io)`) [V]:
```c
void pitch_rate(Pitch* p, IO* io) { io->rate = p->pitch * io->rate; }  // fmuls, single
```
That is the whole DSP of Pitch. There is no Process, so the executor skips it in the chain and
Pitch acts only through io+12, which the upstream source (SndPlayer) turns into a resampling
ratio. No clamping in Pitch; the resampler caps the ratio at 4.0 (section 4.2). Values written
by SetParamF reach +32 through the mixer thread queue, so a change is visible at the next block
in which the rate functions run [I for queue timing].

## 3. SndPlayer block pipeline (what feeds the resampler)

### 3.1 Player fields used [V]

| Off | Type | Meaning |
|---|---|---|
| +4 | System* | (+12 float = mixer sample rate, +0 double = stream time) |
| +29 | u8 | output channel count of the player |
| +36 | u8 | error flag |
| +40 | u32 | planner state (2 running, 3 finished) |
| +48 | float | copied from sound+24 on position reset |
| +56 | double | play position in seconds = pos / sampleRate |
| +64 | double | sound length in seconds = sound+36 / sound+32 |
| +84 | Sound* | current sound node (linked list via +0) |
| +100 | s32 | play position in source frames |
| +104 | s32 | current stream slot (-1 none) |
| +108 | float* | last output sample per channel (declick memory) |
| +112 | u8 | "previous block exists" |
| +113 | u8 | declick request [I: set on stop/seek] |
| +124 | Resampler* | created by `sub_8265D208(?, channels)` in Create `sub_82654428` |
| +128 / +132 | u8* / u32 | command list base / write offset |
| +136 + 8*slot | Decoder* | decoder per slot; +141+8*slot codec index, +142+8*slot channels |
| +296 + 44*slot | | stream slot: +296 state (2 active, 3 ended), +300 read pos, +312 end pos |
| +1176 | s32 | frames planned for the next block |

Sound fields: +8 double start time, +24 float, +32 u32 sample rate, +36 s32 length in frames,
+40 s32 loop/stream boundary, +72 u8 codec index, +73 u8 channel count, +74 u8 stream type,
+76 u8 started, +77 u8 decoder slot, +78 u8 stream slot [V offsets, I names].

### 3.2 Process `sub_82655610(Player* pl, IO* io)` [V]

```c
int snp_process(Player* pl, IO* io) {
    sub_82653600(pl);                         // param/state update [I]
    int produced = 0;
    execute_commands(pl, io, &produced);      // sub_82654060: runs list planned LAST block
    Snapshot s;
    pl->cmd_len = 0;
    rs_save(pl->rs, &s);                      // sub_8265C3B0
    if (produced < 256) {                     // underrun/first block: plan+run now
        pl->planned = plan(pl, io, 0, 256 - produced, &produced);
        rs_restore(pl->rs, &s);               // sub_8265C3F8
        execute_commands(pl, io, &produced);
        pl->cmd_len = 0;
    }
    rs_save(pl->rs, &s);
    pl->planned = plan(pl, io, 1, 256, &produced);   // list for the NEXT block
    rs_restore(pl->rs, &s);
    if (produced == 0 && !pl->declick) return 0;
    if (pl->sound) pl->pos_sec = (double)(int32_t)pl->pos / (double)(uint32_t)pl->sound->rate;
    if (produced < 256) {                     // pad to planar 256 stride
        float* src = io->buf[io->cur];
        float* dst = io_get_next(io, pl->ch << 10); if (!dst) return 0;
        for (c = 0; c < pl->ch; ++c) {
            memcpy(dst + c*256, src + c*produced, produced*4);
            memset(dst + c*256 + produced, 0, (256 - produced)*4);
        }
        io->cur = (io->cur == 0);
    }
    if (pl->declick && pl->have_prev) {
        declick(io->buf[io->cur], pl->last, pl->ch, 256);   // sub_8265AD70
        pl->declick = 0;
    }
    float* b = io->buf[io->cur];
    for (c = 0; c < pl->ch; ++c) pl->last[c] = b[c*256 + 255];
    pl->have_prev = 1;
    return 1;
}
```
Important timing consequence [V]: the command list (including the resampling ratio, stored as a
float in each command) for block N is planned at the end of block N-1 with block N-1's io+12.
Pitch changes therefore take effect with one block (256 frames) of latency, except in the
underrun branch where planning and execution happen in the same block.

The planner simulates the resampler with `rs_skip` (section 4.6) to know how many source
frames each command consumes; the snapshot/restore pair guarantees that the real run during
execution starts from the same state. A native port can keep this structure or compute the
same counts directly, but the counts and the state transitions must match.

### 3.3 Declick `sub_8265AD70(float* out, float* last, int ch, int stride)` [V]
```c
static const float R[16] = { 16/17.f, 15/17.f, ..., 1/17.f };  // 0x82082070.., bits 0x3F70F0F1..0x3D70F0F1
for (c = 0; c < ch; ++c) {
    float* o = out + c*stride;
    for (k = 0; k < 16; ++k) o[k] = fmaf(last[c], R[k], o[k]);    // fmadds
    last[c] = 0.0f;
}
```
Exact table bits [V]: 3F70F0F1 3F61E1E2 3F52D2D3 3F43C3C4 3F34B4B5 3F25A5A6 3F169697
3F078788 3EF0F0F1 3ED2D2D3 3EB4B4B5 3E969697 3E70F0F1 3E34B4B5 3DF0F0F1 3D70F0F1.

### 3.4 Planner `sub_82655170(pl, io, mode, nFrames, int* out)` (resampling part) [V]

Ratio:
```c
float rate = (io->rate / pl->sys->rate)                        // fdivs, System+12
           * (float)(double)(uint64_t)(uint32_t)sound->sampleRate;   // fcfid of zero-extended word, frsp, fmuls
```
With io->rate = pitch product and System+12 = 48000 this is `srcRate/48000 * pitch` = source
frames per output frame.

Start of a sound (sound+76 == 0): mode 0 calls `sub_82653880` (start now); mode 1 computes a
sample accurate delay: `d = start - (sys->time + 256.0/sysRate)`, `d = max(d, 0.0)`,
`delayFrames = (int)(sysRate * d)` (double ops, truncation); if `delayFrames >= 256` nothing is
planned this block; else a type 3 (silence) command of `delayFrames - done` frames is emitted.
Then `sub_82654FF0` creates the decoder, sound+76 = 1, position reset command (type 4).

Main loop (done = frames already planned in this block):
```c
for (;;) {
    int rem  = nFrames - done;
    int need = rs_inputs_for(rs, rate, rem);           // sub_8265C970
    int avail = slot->end - slot->pos;                 // +312 - +300
    if (need < avail) {
        rs_skip(rs, rate, rem, need);                  // sub_8265CBD0
        emit_resample(last=0, slot, outOff=done, outCnt=rem, rate, inCnt=need);
        done += rem; break;
    }
    int out = rs_outputs_for(rs, avail, rate);         // sub_8265D350
    if (out > rem) out = rem;
    rs_skip(rs, rate, out, avail);
    emit_resample(last=1, slot, outOff=done, outCnt=out, rate, inCnt=avail);
    consumed += avail; done += out;
    pos = consumed + pl->pos;
    if (pos == sound->loopEnd /*+40*/)  sub_82654CB0(pl, sound);    // loop jump [I]
    else if (pos == sound->length /*+36*/) sub_82654DD0(...);       // end or next sound [I]
    else sub_82654BE0(pl, sound, 0);                               // refill stream [I]
    if (out == rem) break;
}
```
Command records (byte stream at pl+128, executed by `sub_82654060`, switch on byte 0) [V]:

| type | size | layout | action |
|---|---|---|---|
| 0 | 20 (byte 3) | +1 u8 last, +2 u8 stream slot, +4 u16 outOff, +6 u16 outCnt, +8 Sound*, +12 float rate, +16 u16 inCnt | decode+convert+resample, `sub_82653B20` |
| 1 | 8 (byte 1) | +4 Sound* | `sub_82653998` (sound finished) |
| 2 | 4 | | `sub_826502A8(voice, -1)` graceful stop |
| 3 | 12 | +2 u16 off, +4 u16 count, +8 Sound* | append `count` frames of silence |
| 4 | 12 | +4 s32 pos, +8 Sound* | set pl+100; if 0 also reset +48/+56/+64 |

Type 0 execution: if `pl->slot != cmd.slot` then `pl->slot = cmd.slot; stream[slot].state = 2`;
`sub_82653B20(...)`; `pl->pos += decoded; stream.pos += decoded; *produced += outProduced`;
if `cmd.last` then `stream.state = 3` [V].

Type 3 execution: buffer becomes `[off*ch floats copied from current][count*ch zeros]` as one
contiguous zero run (not re-strided) [V]; see open question 4.

### 3.5 Segment executor `sub_82653B20(pl, io, sound, outCnt, inCnt, outOff, int* decoded, int* produced, float rate)` [V]

```c
int pre = pl->ch * outOff;                 // floats already in the output of this block
float* s = io_get_next(io, sound->ch*inCnt*4 + pre*4);
if (!s) { *produced = 0; *decoded = 0; return -1; }
int got = dec->vtbl->decode(dec, s + pre, inCnt);   // decoder at pl+136+8*slot, vtable +8
*decoded = got;
*produced = (got != inCnt) ? rs_outputs_for(pl->rs, got, rate) : outCnt;
if (pre) memcpy(s, io->buf[io->cur], pre*4);
io->cur = (io->cur == 0);
float* in = io->buf[io->cur];
if (pl->ch != sound->ch && got) {          // channel conversion, gain 1.0f
    float* t = io_get_next(io, pl->ch*got*4 + pre*4); if (!t) goto fail;
    if (pre) memcpy(t, in, pre*4);
    convert(t + pre, in + pre, pl->ch, sound->ch, got, 1.0f);   // sub_8265DA58
    io->cur = (io->cur == 0); in = io->buf[io->cur];
}
float* o = NULL;
if (*produced + pre) { o = io_get_next(io, pl->ch * *produced * 4 + pre*4); if (!o) goto fail; }
if (rs_process(pl->rs, o + pre, in + pre, *produced, got, rate) && o) {  // sub_8265CCB0
    if (pre) memcpy(o, in, pre*4);
    io->cur = (io->cur == 0);
}
if (pre) {                                  // re-stride [ch x outOff][ch x produced] -> [ch x (outOff+produced)]
    float* m = io_get_next(io, pl->ch * *produced * 4 + pre*4); if (!m) return -1;
    float* cb = io->buf[io->cur]; int N = *produced + outOff;
    for (c = 0; c < pl->ch; ++c) {
        memcpy(m + c*N,          cb + c*outOff,               outOff*4);
        memcpy(m + c*N + outOff, cb + pre + c * *produced,     *produced*4);
    }
    io->cur = (io->cur == 0);
}
return 0;
```
Decoder output format [V]: **32 bit float, planar, channel stride = number of decoded frames**,
`sound->ch` channels (proven by the stride arithmetic of `convert` and `rs_process`, which read
`in[c*nIn + j]`). No int16 stage exists after the decoder.

At unity step `rs_process` returns 0 and the decoded buffer itself becomes the output (no copy,
no interpolation): exact passthrough [V].

Codec selection [V]: FourCC = table at 0x82081CDC indexed by sound+72:
0 "Xas0", 1 "EL30", 2 "P6B0", 3 "EXm0". `sub_8265C1B8(registry, fourcc)` walks the registry
list (entry node at +8, FourCC at entry+0) and returns the entry or 0; `sub_8265C258(reg, entry,
ch, sys)` calls `entry->create(ch, sys)` (entry+4) and stores `dec+4 = sys, dec+8 = (u8)ch`.
Only two codecs are registered at start (section 1.4). Decoder vtable: +8 decode(dec, float*
dstPlanar, frames) returns frames, +12 destroy [V].

### 3.6 Channel converter `sub_8265DA58(dst, src, unused, outCh, inCh, n, float g)` [V]

Planar, channel k of a buffer at `buf + k*n`. Primitive ops:
* `SCALE(d, s, g)` = `sub_8265B488`: `d[i] = s[i]*g`.
* `MAC(d, s, g)` = `sub_8265AF48`: scalar path `d[i] = fmaf(s[i], g, d[i])`; VMX path (taken when
  `((d & s) & 0x7F) == 0` and `n % 64 == 0`) does `vmulfp` then `vaddfp` (two roundings, VMX
  denormal flush). With g = 1.0f (the only gain SndPlayer passes) both paths give identical
  results except denormals.
* `ZERO(d)` = memset n floats.
Let `c = g * 0.7071f`. Channel order deduced from the matrices: 6ch = {0 FL, 1 C, 2 FR, 3 SL,
4 SR, 5 LFE}, 4ch = {0 FL, 1 FR, 2 SL, 3 SR} [I names, V indices].

| in -> out | function | operations in order |
|---|---|---|
| same | | SCALE all `ch*n` floats |
| 1->2 | D3C8 | d0=SCALE(s0,c); d1=SCALE(s0,c) |
| 1->4 | D420 | d0=SCALE(s0,c); d1=SCALE(s0,c); ZERO d2,d3 |
| 1->6 | D4A8 | d1=SCALE(s0,g); ZERO d0,d2,d3,d4,d5 |
| 2->1 | inline | d0=SCALE(s0,g); MAC(d0,s1,g) |
| 2->4 | D540 | d0=SCALE(s0,g); d1=SCALE(s1,g); ZERO d2,d3 |
| 2->6 | D5B8 | d0=SCALE(s0,g); d2=SCALE(s1,g); ZERO d1,d3,d4,d5 |
| 4->1 | D660 | d0=SCALE(s0,g); MAC s1; MAC s2; MAC s3 |
| 4->2 | D6E0 | d0=SCALE(s0,g); MAC(d0,s2); d1=SCALE(s1,g); MAC(d1,s3) |
| 4->6 | D768 | d0=SCALE(s0); d2=SCALE(s1); d3=SCALE(s2); d4=SCALE(s3); ZERO d1,d5 |
| 6->1 | D818 | d0=SCALE(s1,g); MAC s2; MAC s4; MAC s3; MAC s0 (s5 dropped) |
| 6->2 | D8B8 | d0=SCALE(s1,c); MAC(d0,s0,g); MAC(d0,s3,g); d1=SCALE(s1,c); MAC(d1,s2,g); MAC(d1,s4,g) |
| 6->4 | D988 | d0=SCALE(s1,c); MAC(d0,s0,g); d1=SCALE(s1,c); MAC(d1,s2,g); d2=SCALE(s3,g); d3=SCALE(s4,g) |
| other, in<out | | SCALE first `inCh*n`, ZERO remaining `(out-in)*n` |
| other, in>out | | SCALE first `outCh*n` |

## 4. Resampler (owned by SndPlayer, pl+124)

### 4.1 State layout (created by `sub_8265D208(unused, ch)`) [V]

| Off | Type | Init | Meaning |
|---|---|---|---|
| +0 | u32 | 0 | `step`, 16.16 fixed point source frames per output frame, max 0x40000 |
| +4 | u32 | 0 | `frac`, fractional position 0..0xFFFF (only low 16 bits ever stored) |
| +8 | float | -1.0f | last rate passed in (cache key) |
| +12 | float | -1.0f | effective rate (rate, or 4.0f when capped) |
| +16 | s32 | 0 | `off`: history offset; input frame j of the next call sits at history index `H + off + j`; clamped `>= -H` |
| +20 | float* | (p+35)&~7 = p+32 | history, `ch * 10` floats, channel c at `hist + c*10` |
| +24 | u8 | ch | channel count |
| +25 | u8 | 0 | primed |
| +26 | u8 | 0 | `lead` (never changed except by restore; always 0 in practice) |
| +27 | u8 | 2 | `H`: history taps kept between calls |

Allocation: `(ch*10 + 8) * 4` bytes, 16 aligned, through `sub_8264CBB8(sys, size, 0, 16, 0)`;
first 7 words zeroed; history filled with 0.0f [V]. Destroy `sub_8265C938` = free [V].
Window per channel = `H + 8` = 10 floats (`r21 = H + 8`).

Snapshot (24 bytes) [V]: `sub_8265C3B0(rs, snap)` copies {+0 step, +4 frac, +8 rate, +12 rateEff,
+16 off} to snap+0..+16 and bytes +25,+26,+27 to snap+20,+21,+22. `sub_8265C3F8(rs, snap)` is the
exact inverse. The history buffer is NOT saved (planning never touches it).

### 4.2 SetRate `sub_8265C4A8(rs, float rate)` [V]
```c
float t = rate * 65536.0f;                      // fmuls
t = (t < 0.0f) ? (t - 0.5f) : (t + 0.5f);       // fsubs/fadds single, round half away from zero
int32_t i = (int32_t)t;                         // fctiwz: truncate, saturating
if (i > 0x40000) { rs->rate = rate; rs->rateEff = 4.0f; rs->step = 0x40000; }
else             { rs->rateEff = rate; rs->step = (uint32_t)i; rs->rate = rate; }
```
Every public entry point calls SetRate first when `rate != rs->rate` (fcmpu, so NaN always
re-sets). No lower clamp: rate 0 gives step 0, negative rates give a negative step (signed
compare passes) [V].

Interpolation kind [V]: **2 point linear**, fixed point 16.16 phase accumulator, no filter, no
table. Upward pitch is limited to 4.0 (two octaves); no anti aliasing at any ratio.

### 4.3 Linear kernels [V]

Mono `sub_8265C520(rs, cnt, src, dst, int* idx, u32* fracWord, stepHi, stepLo)` (the frac is
passed as `frac << 16` in a word and read with lhz):
```c
uint32_t step = (stepHi << 16) | (stepLo & 0xFFFF), fr = *fracWord >> 16; int i0 = *idx;
for (k = 0; k < cnt; ++k) {
    float f  = (float)fr * 1.5258e-05f;           // u64 -> double -> float (exact), fmuls
    float s0 = src[i0], s1 = src[i0 + 1];
    dst[k] = fmaf(s1 - s0, f, s0);                // fsubs then fmadds (single rounding)
    uint32_t a = fr + step;
    i0 += a >> 16; fr = a & 0xFFFF;
}
*fracWord = fr << 16; *idx = i0;
```
Multi channel `sub_8265C700(..., ch, srcStride, dstStride)` (extra args on the stack at +84, +92,
+100): same per frame `f`, then for each channel c (unrolled by 4, order irrelevant):
`dst[c*dstStride + k] = fmaf(src[c*srcStride + i0 + 1] - src[c*srcStride + i0], f, src[c*srcStride + i0])`.
`rs_process` uses the mono kernel iff `ch == 1`.

A native port must use `fmaf` (or equivalent) for the final step and the exact constant; the
`(s1 - s0)` subtraction is rounded to float before the FMA.

### 4.4 rs_inputs_for `sub_8265C970(rs, float rate, int nOut)` (input frames needed) [V]
```c
if (rate != rs->rate) SetRate(rs, rate);
if (rate == 1.0f) return nOut;                       // float compare, not step compare
int extra = rs->primed ? 0 : (rs->H - rs->lead);
int v = (int)(((uint32_t)(rs->step*nOut + rs->frac)) >> 16) - rs->off + extra;
return v < 0 ? 0 : v;
```

### 4.5 rs_outputs_for `sub_8265D350(rs, int nIn, float rate)` and search `sub_8265CA08` [V]
```c
int rs_outputs_for(RS* rs, int nIn, float rate) {
    if (nIn <= 0) return 0;
    if (rate != rs->rate) SetRate(rs, rate);
    if (rate == 1.0f) return nIn;
    return rs_search(rs, nIn, rs->rateEff, /*withState*/1);
}
int rs_search(RS* rs, int n, float r, int withState) {     // sub_8265CA08
    if (!(r > 0.0f)) return 8192;
    int base = n;
    if (withState) { if (!rs->primed) base = rs->lead - rs->H + n; base += rs->off; }
    int k = (int)((float)base / r);                         // fcfid, frsp, fdivs, fctiwz
    if (k > 8192) k = 8192;
    #define F(k) (withState ? rs_inputs_for(rs, r, k) \
                            : (int)(((uint32_t)(rs->step*(k) + rs->frac)) >> 16))
    int v = F(k);
    if (v == n) return k;
    if (v > n) { while (k > 0) { --k; if (F(k) <= n) return k; } return 0; }
    for (;;) { ++k; if (k > 8192) return k - 1; if (F(k) >= n) return k - 1; }
}
```
Note: `rs_inputs_for` inside `F` is called with `r = rateEff`; when the rate was capped
(rateEff 4.0 != rate) this triggers SetRate(4.0) as a side effect, changing `rs->rate` to 4.0
[V by code path, I that it matters]. The upward search returns `k-1` even when `F(k) == n`
(asymmetric), keep it.

### 4.6 rs_skip `sub_8265CBD0(rs, float rate, int nOut, int nIn)` (planner bookkeeping) [V]
```c
if (rate != rs->rate) SetRate(rs, rate);
if (rs->step == 0x10000) { rs->off = 0; rs->primed = 1; return; }
uint32_t pos = rs->step*nOut + rs->frac;
int extra = 0;
if (!rs->primed) { extra = rs->H - rs->lead; rs->primed = 1; }
int adv = (int)(pos >> 16) + extra; if (adv < 0) adv = 0;
rs->off = rs->off - adv + nIn;  if (rs->off < -rs->H) rs->off = -rs->H;
rs->frac = pos & 0xFFFF;
```

### 4.7 rs_process `sub_8265CCB0(rs, float* out, float* in, int nOut, int nIn, float rate)` [V]

Returns 1 when it wrote `out`, 0 when the caller must use `in` unchanged (unity) or nOut < 0.
`W = H + 8` (=10). `hist(c, j)` = `rs->hist[c*W + j]`; `IN(c, j)` = `in[c*nIn + j]`.
```c
if (nOut < 0) return 0;
if (rate != rs->rate) SetRate(rs, rate);
if (rs->step == 0x10000) {                          // unity passthrough
    rs->off = 0;
    int m = min(rs->H, nIn);
    for (j = 0; j < m; ++j) for (c) hist(c, rs->H - m + j) = IN(c, nIn - m + j);
    if (!rs->primed) rs->primed = 1;
    return 0;
}
// (1) append the head of the new input to the history window
if (!rs->primed) {
    for (k = 0; k < rs->lead; ++k) for (c) hist(c, k) = 0.0f;
    int m = min(W - rs->lead, nIn);
    for (j = 0; j < m; ++j) for (c) hist(c, rs->lead + j) = IN(c, j);
} else {
    int m = min(8 - rs->off, nIn);
    for (j = 0; j < m; ++j) for (c) hist(c, rs->H + rs->off + j) = IN(c, j);
}
// (2) interpolate
uint32_t step = rs->step;
int consumedInt = (int)(((uint32_t)(step*nOut + rs->frac)) >> 16);     // r26
float* hsrc = rs->hist + rs->lead;
int idx = 0; uint32_t frw = rs->frac << 16;
int nH = rs_search(rs, 8 /* W-2 */, rs->rateEff, 0);                    // outputs from history
int keep = 0;                                                           // r25
if (nOut <= nH) {
    nH = nOut;
    int top = rs->off + rs->H;
    if (consumedInt < top) keep = top - consumedInt;
}
kernel(hsrc, out, nH, &idx, &frw, srcStride=W, dstStride=nOut);
int rest = nOut - nH;
float* tailSrc;                                                         // r23
if (rest > 0) {
    float* isrc = in + (idx - rs->off - rs->H);        // history index -> input index
    int idx2 = 0;
    kernel(isrc, out + nH, rest, &idx2, &frw, srcStride=nIn, dstStride=nOut);
    tailSrc = isrc + (idx2 - rs->lead);
} else if (keep == 0) {
    tailSrc = in + (idx - rs->off - rs->H);
} else {
    for (j = 0; j < keep; ++j) for (c) hist(c, j) = hist(c, consumedInt + j);   // slide window
    tailSrc = in;
}
// (3) bookkeeping, identical to rs_skip
int extra = 0;
if (!rs->primed) { rs->primed = 1; extra = rs->H - rs->lead; }
int adv = (int)(((uint32_t)(step*nOut + rs->frac)) >> 16) + extra;   // rs->frac = value at call start
if (adv < 0) adv = 0;
rs->off = rs->off + nIn - adv;  if (rs->off < -rs->H) rs->off = -rs->H;
// (4) refill the history tail for the next call
int m = min(rs->H - keep + rs->off, nIn);
for (j = 0; j < m; ++j) for (c) hist(c, keep + j) = tailSrc[c*nIn + j];
rs->frac = frw >> 16;
return 1;
```
All pointer offsets in (2) are in frames (scaled by 4 bytes); `kernel` = mono version when
`ch == 1`, else the strided version. In step (3) `rs->frac` still holds the value from the
start of the call (it is only written at the very end), so `adv == consumedInt + extra` [V].

Edge handling summary [V]:
* Buffer boundary: the interpolator reads `src[idx+1]`, so the last H=2 source frames of each
  call are carried in the history window and the next call starts reading from the window
  (up to `W-2 = 8` frames ahead) before switching to the fresh input buffer.
* Loop points / stream ends are not handled inside the resampler: the planner splits a block
  into several type 0 commands at stream boundaries, and each command calls `rs_process` with
  the frames available. Continuity across a loop comes from the history window, so a loop
  seam is interpolated across (last frame before the seam with first frame after it).
* A decoder short read (`got < inCnt`) shrinks `nOut` to `rs_outputs_for(got)`; the remaining
  output frames of the block are zero padded by Process (section 3.2).

## 5. Open questions

1. First (unprimed) call: history is filled as `hist[j] = in[j]` (lead = 0), but the second
   kernel maps history index to input with `- off - H` (off = 0), i.e. `in[idx - 2]`, while the
   bookkeeping adds `H - lead = 2` extra consumed frames. Possible 2 frame skew on the very
   first block of a sound; reproduce literally or verify with a capture.
2. Unity detection differs between functions: `rs_process`/`rs_skip` test `step == 0x10000`
   (any rate within ~7.6e-6 of 1.0), while `rs_inputs_for`/`rs_outputs_for` test
   `rate == 1.0f` exactly. For rates like 1.000005f the planner counts via fixed point while
   processing is passthrough. Replicate as is.
3. Capped rates: `rs_search` passes `rateEff` (4.0) into `rs_inputs_for`, which re-runs
   SetRate(4.0) and overwrites the cached `rate`; next call with the original rate re-sets
   again. Harmless to output but must be replicated if state is compared.
4. Type 3 (silence) command appends zeros as one block without re-striding; correct only when
   `outOff == 0` (the normal case, silence before a sound start). Unknown whether `outOff > 0`
   ever happens.
5. Who sets pl+113 (declick request): not traced (expected: stop, seek, sound change).
6. Exact meaning of io+16 (set to 1.0 per voice) and of the 16 byte entry table at voice+40
   (only +0 and +4 read here; for SubMix ("Sub0", desc +20 = 0x1) entry +0 acts as a flag
   meaning "plug-in 0 is a bus with child voices").
7. FourCCs of the two codec classes actually registered (entries live in BSS, filled at run
   time); XMA ("EXm0" or "Xas0") is the one tied to `sub_82659AC8`.
8. `sub_82653600` (start of Process) and the stream refill functions (`sub_82654BE0`,
   `sub_82654CB0`, `sub_82654DD0`) were not read; they decide loop behaviour and where
   `slot->end` comes from.
