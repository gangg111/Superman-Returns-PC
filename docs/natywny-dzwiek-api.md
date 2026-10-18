> Mapa API granicy gra / silnik dźwięku. Analiza statyczna asemblera z wygenerowanego kodu
> (subagent, 2026-10-19), z poprawką własną po weryfikacji: rzekomy odczyt `sys+0` dotyczy
> menedżera dźwięku gry. Każda pozycja ma oznaczenie VERIFIED (odczytane w asm) albo INFERRED.

# Superman Returns: game and audio engine API map

Source: PPC asm in the generated C++ (`dump_asm.py`), cross-calls (`xcalls.py`), static data (`peek.py`),
all in the scratchpad. VERIFIED = read in asm. INFERRED = naming or meaning deduced from usage or from the
Skate 3 reference. Addresses are guest addresses.

### 0. Corrections to the starting assumptions

1. **Global System pointer is at 0x8299FDC8**, not 0x8199FDC8: `lis r11,-32102` = 0x829A0000, `lwz r3,-568(r11)` (VERIFIED, sub_8264C3D8; written by sub_8264C958 `stw r31,-568(r10)`, cleared by sub_8264C6D0).
2. **The class registry is not at sys+4.** sub_8264CD70(sys) lazily creates a 24-byte registry object stored at **sys+52** (sub_8264CCF0: vtable 0x8207FE0C, reg+20 = sys). sub_8264CC40/CC70 take that **registry** as r3; list head at reg+4, tail reg+8, count reg+12; node = desc+44, id = desc+4 (VERIFIED).
3. **The band 0x8264C000..0x82675000 is too wide at both ends.**
   * 0x8264C130/C198/C208/C2D8/C360 are generic allocator/resource helpers called by non-audio code (0x82641E70, 0x82649C70, 0x8264AE68, 0x8264B9D8, 0x828E4590). Not audio API (INFERRED from callers; C130/C198 memset 140 B, set type 27, call sub_82641168/sub_8263DA58; C2D8 drives an interface vtbl[0..3]; C360 is a ctor with vtable 0x8207FD68).
   * **0x82670610 and up is a video decoder**, not audio: sub_82670668 computes YUV420 plane sizes (w*h*3/2, w*h/4), sub_82670F28 fills plane pointers and strides, sub_82670B10 builds lookup tables; callers are the movie module (strings "YuvChromaShader", "RasterContext", "Stopwatch object"). The last audio-reached function is sub_82670340 (called from 8266D068/D228/D3C0). All 11 "callee" entries of the second list (826716D8, 82671430, ... called from 0x82675F78..0x8267CB38) are internal calls of that decoder. **Note:** sub_82670B88 returns 0x82082E68 = "6.1.0.5", so that version string belongs to the module at 0x8267xxxx (or to a shared allocator area, strings "main memory/disposable/physical" follow it), not necessarily to RW Audio. Open question.
4. **The game's movie module (0x8235C..0x82362) talks to a second, handle-based stream API** (0x82656298..0x82658xxx, error code -8 = bad handle), not to the plug-in/voice API. It is a separate layer on top of the same System lock.

### 1. System / lifetime

| func | sites | name (proposed) | args → ret | what it does | evidence |
|---|---|---|---|---|---|
| sub_8264C958 | 1 (82326F98) | `System::Create(alloc, heapSize)` | r3 = ICoreAllocator*, r4 = heap bytes (game: 0x100000) → System* or 0 | Allocates 228 B (align 16) via alloc->vtbl[1]; stores global 0x8299FDC8; builds 2 critical sections (sys+88 main lock, sys+84 drain lock); allocates the heap (r4 bytes) via alloc → sys+32 (command buffer and heap); sys+36 = heapSize; sys+28 = alloc; sys+20 = 1; sys+16 = 90.0f; sys+112 = 15; sys+118 = 4; on heap failure frees sys and returns 0. | VERIFIED |
| sub_8264C6D0 | 2 (82321780, 82335E28) | `System::Release(sys)` | r3 = sys | Locks, enqueues {0x8264C440, sys} (destroy all voices in sys+96 list), unlocks; then under sys+84 loops `while (sys+40 != 0 \|\| sys+24 > 0) { sub_8264C540(sys); sub_825F7168(1) }` (sleep); then sub_82659678 (mixer shutdown), destroys sys+52/56/60/68 via vtbl[0], sys+64 via sub_826582E8, frees heap and sys, clears the global. | VERIFIED |
| sub_8264C3D8 | 27 | `System::GetInstance()` | → System* | `lwz r3, 0x8299FDC8` | VERIFIED |
| sub_8264C4C0 | 34 | `System::Lock(sys)` | r3 = sys | sys+76 callback if set, else EnterCriticalSection(sys+88) via sub_825FC108 | VERIFIED |
| sub_8264C4E8 | 34 | `System::Unlock(sys)` | r3 = sys | sys+80 callback if set, else LeaveCriticalSection(sys+88) via sub_825F6B90 | VERIFIED |
| sub_82656298 / sub_826562C0 | 5 / 5 (movie module) | `LockGlobal()` / `UnlockGlobal()` | none | GetInstance + Lock / Unlock | VERIFIED |
| sub_8264C6C8 | 1 (82326F98) | `System::SetCallback(sys, fn)` | r3 = sys, r4 = fn | sys+104 = r4; game passes 0x823217C0 | VERIFIED store, purpose INFERRED (event or error callback) |
| sub_82653498 | 1 (82326F98) | `System::InitBuiltins(sys)` | r3 = sys | registry = CD70(sys); sub_8265C0F0(registry) registers 10 built-in classes (826506F0, 826638F0, 82663600, 82663310, 82662F10, 826625F8, 82662060, 82661D38, 82653558, 826513B0); sub_8265C0A8(sys) lazily creates sys+56 via sub_8265C2C0; then sub_8265C060 (mixer start, above the mixer per earlier notes) | VERIFIED calls, meaning INFERRED |
| sub_8264C528 | 1 (82326CE8) | `System::HasUpdatedSince(sys, stamp)` | r3 = sys, r4 = stamp → 1 if `stamp < sys+20` (unsigned) | Game stores sys+20 into sound+444 right after a stop command (82327988) and later polls this to know the mixer thread drained that command. | VERIFIED arithmetic (subfc/subfe/clrlwi) and usage |

### 2. Object registry (plug-in classes)

| func | sites | name | args → ret | what | evidence |
|---|---|---|---|---|---|
| sub_8264CD70 | 7 | `System::GetRegistry(sys)` | r3 = sys → registry* | returns sys+52, creating it with sub_8264CCF0 if 0 (24 B from sys heap via sub_8264CBB8) | VERIFIED |
| sub_8264CC70 | 11 (82325510, 82326F98) | `Registry::Register(reg, desc)` | r3 = reg, r4 = ClassDesc* → desc already registered with same id, else r4 | append desc+44 to the list, count++ | VERIFIED |
| sub_8264CC40 | 17 | `Registry::Find(reg, fourcc)` | r3 = reg, r4 = id → ClassDesc* or 0 | walk list, compare desc+4 | VERIFIED |
| sub_826531A0, sub_82652E88, sub_82652A90, sub_8264FCE8, sub_82652050, sub_826518F8, sub_826513B0, sub_826506F0, sub_8264E988, sub_8264D768 | 1-2 each | `Xxx::GetClassDesc()` | → static ClassDesc* (0x8292D7xx..0x8292EC28, BSS, runtime-initialised) | `lis/addi; blr` | VERIFIED; class identity per address only INFERRED from code position: 8264D768 after "Reverb Model 1" code, 8264FCE8 after "rw::audio::core::Delay", 826513B0 after "RWAudioCore Dac", 826518F8 after "Snd::SubMix". 82653558 (registered by InitBuiltins, right before "SndPlayer" code) is the likely SndPlayer class. |

FourCC ids the game looks up (VERIFIED constants in 82326F98, 82327650, 823276E0), with the game-object field that receives the ClassDesc*:
`SnP0`→+60, `Sen0`→+64, `Dac0`→+68, `Sub0`→+72, `Pit0`→+76, `Rou0`→+80, `Gai0`→+84, `LI20`→+88, `HI20`→+92, `PnS0`→+96, `AiW0`→+100, plus `VuM0` (local only). Names are INFERRED (SndPlayer, Send, Dac, SubMix, Pitch, Routing, Gain, LP IIR 2-pole, HP IIR 2-pole, Pan to speakers, AiW0 unknown, VU meter).

ClassDesc layout used by voice creation (VERIFIED in 82650398): +4 id, +8 `GetSize(entry)` fn, +12 `Create(voice, entry, prevChannels, mem)` fn, +20 / +24 copied into the voice's 16-byte per-plug-in table, +44 list node.

### 3. Voices, plug-ins, parameters and commands

| func | sites | name | args → ret | what | evidence |
|---|---|---|---|---|---|
| sub_82650398 | 4 (82326F98, 823277E0, 8232B4D8, 8232E058) | `Voice::Create(n, entries, outPlugins, sys)` | r3 = plug-in count, r4 = array of 12-B entries {void* init; ClassDesc* cls; u8 channels}, r5 = out `Plugin**` (receives &voice+72), r6 = sys → Voice* or 0 | Sums sizes, allocates from sys heap (sub_8264CBB8), creates each plug-in (plugin+8 = voice), fills voice fields, then **enqueues 8-B command {0x82650190, voice}** (link into sys+96 active-voice list on the mixer side). | VERIFIED |
| sub_826505B8 | 1 (82327908) | `Voice::Release(voice)` | r3 = voice | enqueues 8-B {0x82650368, voice}; handler calls sub_826501D0(voice, 0) = destroy | VERIFIED |
| sub_826500F8 | 1 (82327EA0) | `Voice::IsReleaseDone(voice)` → bool | r3 = voice | state voice+70: 0 → 0; 1 → tail = Σ plugin+24 (sub_82650018), keep max in voice+52, if voice+48 ≥ voice+52 set state 2 and zero +16/+20/+24; returns state == 2 | VERIFIED |
| sub_826500C8 | 1 (82327988) | `Voice::GetLatencySeconds(voice)` → f1 | r3 = voice | Σ plugin+24 / sys+12 (voice+32 = sys) | VERIFIED math, meaning INFERRED (sys+12 = sample rate) |
| sub_8264FF20 / sub_8264FF28 | 1 / 1 | `System::SetMasterSubmix(sys, p)` / `GetMasterSubmix(sys)` | sys+48 | store / load | VERIFIED; "master submix" INFERRED (game stores plug-in 0 of the master voice SubMix→VuM→AiW→Dac) |
| sub_8264CE48 | 3 (82324F60, 82327378, 8232B4D8) | `Plugin::SetParamF(p, idx, f1)` | r3 = plug-in, r4 = param index, f1 = value | enqueues 16-B {0x8264CE28, p, idx, value} into [p+4]+32 at offset [p+4]+40 (offset advanced BEFORE fill, as in Skate 3). p+4 = sys. Game wrapper 82324F60 takes the system lock around it. | VERIFIED |
| sub_8264CDE8 | 7 (82324FE0, 823250C8) | `Plugin::GetParamF(p, idx, &out)` | r3 = p, r4 = idx, r5 = float* | `*out = ((float*)(p+12))[idx*2]` (8-byte slots) | VERIFIED |
| sub_8264CE00 | 1 (82327D90) | `Plugin::GetParamD(p, idx, &out)` | r3 = p, r4 = idx, r5 = double* | `*out = ((double*)(p+12))[idx]` | VERIFIED; game reads param 1 of the SndPlayer (sound+440), default 0.0, returns it as the sound's position (INFERRED) |
| sub_8264CE18 | 13 (11 callers) | `Plugin::Control(p, idx, void* data)` | r3 = p, r4 = idx, r5 = pointer or NULL | tail call p->vtbl[1](p, idx, data) | VERIFIED dispatch; uses (VERIFIED args): Dac `(1, &3.0f)` and `(3, NULL)` at init; SndPlayer `(1, NULL)` on stop (82327988); `(0, &int)` from 82325060 / 823278A8 / 82327440 (start or select sample, INFERRED); routing plug-in sound+448 `(0, &masterSubmix)` (82327760); 82327B00 passes a float vector. Whether vtbl[1] enqueues or acts directly is not yet checked. |
| sub_82653E70 | 1 (82327EA0) | `SndPlayer::IsDone(p)` → u8 | r3 = player plug-in | 0 if f(p+32) < f(p+96); else (p+84 == 0) | VERIFIED; meaning (pos ≥ end and no pending requests) INFERRED |
| sub_82653568 | 1 (8233C068) | `ParseSnrHeader(hdr, out)` | r3 = header bytes, r4 = out {u8 ch; u32 rate; u32 samples} | bit reader: skip 4, skip 4, 6 bits+1 → channels, 18 → sample rate, skip 3, 29 → sample count | VERIFIED; matches EA SNR header (INFERRED) |
| sub_82655E40 | 1 (823362A0) | `BufferPool::Create(user, n, slotSize, kind, sys, tag)` | r3 user, r4 slot count, r5 slot bytes, r6 kind (+3 passed on), r7 sys, r8 tag → pool* | allocates n×32-B slots + n×slotSize from sys heap; per slot calls sub_82601EE0(kind+3, buf, slotSize, 0,0,0) → slot+20; links pool into list 0x8299FDE4; vtable 0x82081D50 at +8 | VERIFIED; what sub_82601EE0 creates (XMA context, file reader) is open |
| sub_82655E10 | 1 (823363B0) | `BufferPool::Release(pool)` | r3 = pool | enqueues {0x82655C80, pool}; handler unlinks, runs each active slot's slot+12(slot+16), sub_8265DD48(sys, pool+8) under lock, then sys+24++ | VERIFIED |

### 4. Movie / stream channel API (handle based)

Globals (VERIFIED): channel table 0x8299FED8 (+14 u8 = channel count, +136 = base of 132-B per-voice records), channel pointer array 0x8299FDF0[ch], active fade list 0x8299FDEC. Request handle = `(serial += 256) | channel`; low byte = channel index.

| func | sites | name | args → ret | what | evidence |
|---|---|---|---|---|---|
| sub_826580F8 | 1 (82361DF0) | `Stream::GetMemSize(nCh, nReq)` | r3, r4 (game: 1, 30) → bytes | `(((r3+13)*10 + [0x8292F254]) * 4) + sub_8265EF18(r4)` | VERIFIED |
| sub_826580B0 | 1 (82361DF0) | `Stream::InitOptions(opts)` | r3 = 16-B opts → 0 | b0 = 127, b1 = 60, b2 = 127, b3 = 127, b4 = 0, h6 = 0, h8 = 4096, h10 = 4096, h12 = -1, h14 = 0 | VERIFIED; field meanings INFERRED (vol, pitch 4096 = 1.0) |
| sub_82658088 | 1 (82361DF0) | `Stream::Open(fs, opts, nCh, nReq, mem, size)` → channel or <0 | r3 fs/owner, r4 opts, r5 1, r6 30, r7 mem, r8 size | reorders and calls sub_82657A50(opts, nCh, nReq, mem, size, fs, 1, 0) | VERIFIED wrapper |
| sub_82658078 | 1 (82361DF0) | `Stream::QueueHandle(ch, loop, src)` → request handle | r3 ch, r4 (game −1), r5 src | → sub_82657EA8(ch, r4, 0, src, mode 2) | VERIFIED |
| (sub_82657EA8) | internal | `Stream::Queue(ch, loop, name, arg, mode)` | mode 0: open file by name (sub_82607968), 1: sub_82607AA0, 2: r6 is an already-open source | allocates a 40-B request from ch+288, appends to ch+276, req+8 = source, req+12 = handle, req+32 = r4; errors −8 bad channel, −13 no buffer (ch+296 == 0), −1 open failed | VERIFIED |
| sub_82657930 | 1 (8235F0A8) | `Stream::Close(ch)` → 0 / −8 | r3 = channel | under global lock: stop, sub_8265F570(ch+12), clear slot in 0x8299FDF0, free ch+4 resource if ch+21 == 0 | VERIFIED |
| sub_82656558 | 1 (8235CF40) | `Stream::GetStatus(ch, out[3])` → 0 / −8 | out[0] = ch+284, out[1] = handle of queue head ([ch+276]+12), out[2] = played ms = sub_8265F548(ch+12)*1000 / u16 ch+24 (fallback via request rate, clamp 4 000 000) | VERIFIED |
| sub_82656378 | 1 (8235CF40) | `Stream::GetRequestInfo(handle, out[4])` → 0 / −8 | out[0] state: 2 = playing (head of queue), 1 = queued, 3 = unknown handle, 0 = inactive (req+36 == 0); out[1] = req+20, out[2] = req+24 − req+20, out[3] = req+28, each × 1000/rate (u16 ch+24 if playing, ch+28 otherwise) | VERIFIED; game returns float(out[1]) when state == 2, else −1.0 → **movie audio clock in ms** |
| sub_82656698 | 1 (8235CFE8) | `Stream::SetVolume(ch, f1, spk)` → 0 / −8 | r3 ch, f1 volume, r4 speaker (−1 = all six) | writes 6 faders at ch+304 + 36k (+8 target, cancels running ramp: +32 = 0, unlink +12), then per voice float +56 and sub_8265DDC0 | VERIFIED (first branch fully, single-speaker branch partly) |
| sub_82656A40 | 2 (8235D050 with 0, 8235D090 with 4096) | `Stream::SetPitch(ch, q12)` → 0 / −8 | r3 ch, r4 value clamped to ≤ 16384 | ch+240 = value; sub_8265FF28(ch+8, value) writes u16 +128 of each voice and refreshes | VERIFIED; pause = 0 / resume = 4096 (1.0 in Q12) INFERRED |
| sub_82658030 | 1 (8235FA18) | `Stream::SetRequestLoop(handle, n)` → 0 / −8 | r3 handle, r4 s16 | req+32 = n (queued with −1, game sets 0) | VERIFIED store, "loop count" INFERRED |

### 5. Mixer buses / effects

No dedicated boundary functions beyond the plug-in API: the game builds the master voice in 82326F98 as **SubMix(6 ch) → VuM0(6) → AiW0(6) → Dac0** with `Voice::Create(4, ...)` (VERIFIED entries on stack r1+96..+140), stores plug-ins at game+108 (master submix via sys+48), +120 (VU), +128 (AiW0), +116 (Dac), and sets Dac controls (1, 3.0f) and (3, NULL). Effects (Reverb, Delay, two more classes) are only registered (82325510). Per-sound effect chains are built by 823277E0 / 8232B4D8 / 8232E058 through `Voice::Create`.

### 6. 3D / listener

No boundary function is clearly 3D. Candidates: CE18 with a float vector from 82327B00 (pan or position, INFERRED), and `PnS0`/`AiW0` class ids. Spatialisation appears to be done game-side and fed as plug-in params.

### 7. Not audio (false boundary)

8264C130, 8264C198, 8264C208, 8264C2D8, 8264C360 (allocator/resource helpers); 82670610 (5-word struct init), 82670630 / 82670B80 (empty stubs), 82670668, 82670B10, 82670B98, 82670CA0, 82670F28, 82671178, 82671188, 82671198, 826711A0, 82671258 (video decoder: open, set planes, decode, close, global 0x829A05E0), and the 11 callees of the second list.

### (a) Struct offsets established

System (228 B, global 0x8299FDC8):
* +0: not read by the game. CORRECTED 2026-10-19: the `lfd f0,0(r11)` in 82327988 loads through the game sound manager global 0x82976060 (`lis r29,-32105; lwz r11,24672(r29)`), set in sub_82337020, so it is the game manager's own double, not the RW System's (VERIFIED by reading the asm).
* +8 float dt passed to per-update callbacks (8264C540) (VERIFIED)
* +12 float, divisor of plug-in latency (INFERRED sample rate); +16 = 90.0f at create
* +20 update counter, ++ per queue drain, read directly by the game (82327988) (VERIFIED)
* +24 pending async release count; Release waits for it to reach 0 (VERIFIED)
* +28 allocator; +32 heap / command buffer base; +36 heap size; +40 command write offset; +44 high-water mark of +40 (VERIFIED)
* +48 master submix plug-in; +52 registry; +56 mixer-side object (8265C0A8); +60, +68 objects with vtbl dtor; +64 object (sub_826582E8) and a gate for the update-callback list +100 (VERIFIED)
* +76/+80 optional lock/unlock callbacks; +84 drain critical section; +88 main critical section (VERIFIED)
* +96 active voice list head (mixer side); +104 game callback (VERIFIED)

Command records (VERIFIED): `{u32 handler; obj; ...}`, handler returns record size (8 or 16); drain = sub_8264C540 (walks sys+32..sys+32+[sys+40], resets +40, +20++). Handlers seen: 0x8264CE28 (param set, 16 B), 0x82650190 (add voice), 0x82650368 (destroy voice), 0x8264C440 (destroy all voices), 0x82655C80 (destroy pool).

Voice: +16/+20/+24 floats, +28, +32 sys, +40 per-plug-in table (16 B each), +48 elapsed-since-stop, +52 required tail, +56, +60 = sys+20 at creation, +64 float, +68 u8 plug-in count, +69, +70 u8 state (0 run, 1 releasing, 2 done), +72.. Plugin* array (VERIFIED). Plug-in: +4 sys, +8 voice, +12 param table (8-B slots), +24 float latency/tail (VERIFIED as read).

Game sound manager (pointer at 0x82976060): +52 init flag, +104 master voice, +108 master submix, +112 Plugin** of master voice, +116 Dac, +120 VU, +124 registry, +128 AiW0, +132 System*; +60..+100 ClassDesc* by FourCC (VERIFIED). Game sound instance: +396 parent, +404 state, +408 double end time, +417 flag, +440 SndPlayer plug-in, +444 stop stamp, +448 routing plug-in, +576 Voice* (VERIFIED offsets, roles INFERRED).

Stream channel / request: see section 4.

### (b) Functions whose result the game reads back (hard to replace natively)

1. Direct struct read, no function: **sys+20 (update counter)**, read in 82327988 through `game manager (0x82976060) +132 -> System* -> +20` and stored to sound+444, then polled with sub_8264C528. VERIFIED: sub_82326F98 stores the result of System::Create (sub_8264C958) to manager+132. A native replacement must keep sys+20 alive in guest memory. (The earlier "sys+0 clock" read is the game manager's own field, see (a).)
2. `GetParamF` / `GetParamD` (8264CDE8, 8264CE00): read the mixer-written param table at plugin+12; 82327D90 uses param 1 of the player as the sound position, 823250C8 reads several params (6..) of a plug-in.
3. `Voice::IsReleaseDone` (826500F8), `SndPlayer::IsDone` (82653E70), `Voice::GetLatencySeconds` (826500C8): the game's sound lifetime depends on them.
4. Stream API: `GetStatus` (82656558) and `GetRequestInfo` (82656378) give the movie its audio clock (ms); errors −8/−13/−1 from Open/Queue/Close; `Stream::Open` and `Queue` return handles kept by the game.
5. Creation results kept as opaque pointers: System*, registry, ClassDesc*, Voice* and the Plugin** array (the game dereferences plug-in pointers and passes them back).

### (c) Open questions

1. Which ClassDesc address carries which FourCC: descriptors are BSS (image bytes 0x02010201 / 0x02020202 filler), filled by code not found with constant-store scanning. A runtime dump of desc+4 for the 20 getters would settle it.
2. What plug-in vtbl[1] (`Control`) does per class: enqueue or act directly, and what indices 0..3 mean for SndPlayer, Dac and routing.
3. sub_82327440 calls `Control(NULL, 0, &x)` (`li r3,0` right before `bl 0x8264ce18`) when its slot is empty; this dereferences guest address 0. Either a dead path or an original bug; worth a guard if hit.
4. What sub_82601EE0 creates for each pool slot (8260 range: XMA context or file reader) and what decrements sys+24.
5. Version "6.1.0.5" is returned by sub_82670B88 inside the video-decoder region; confirm which library it identifies.
6. sub_82657A50 (stream channel create) and the per-voice 132-B records at [0x8299FED8+136] were only skimmed.
