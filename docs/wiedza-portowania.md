### Baza wiedzy portu: dźwięk XMA i odblokowanie klatek

Zebrane 2026-10-18 z kodu i historii xenia-canary, rexglue-sdk, innych statycznych rekompilacji
(Unleashed Recompiled, MarathonRecomp, AC6_recomp, reblue) oraz dokumentacji Microsoft SHAPE
(następca dekodera XMA z Xboxa 360). Każda lekcja ma źródło i odniesienie do kodu tego portu.
Pozycje oznaczone „niezweryfikowane” to wnioski, nie fakty.

#### 1. Dekoder XMA

**Jak gra korzysta z dźwięku.** Superman Returns woła bezpośrednio eksporty jądra `XMA*`
(`XMACreateContext`, `XMASetInputBuffer`, `XMAGet/SetOutputBuffer*Offset`, `XMAIsOutputBufferValid`)
i ma własny mikser EA (`sub_826592F0`). Nie używa biblioteki `XMAPlayback` z XDK, więc podmiana
dekodowania na poziomie API (jak w MarathonRecomp) tu nie pasuje. Dekoder kontekstów musi zachowywać
się jak sprzęt.

**Jednostki.** Ramka XMA to 512 próbek, podramka 128, blok wyjścia 256 bajtów (stereo 16-bit:
ramka to 8 bloków). Bufor wyjścia ma najwyżej 31 bloków. Offsety zapisu i odczytu są w blokach.
Źródło: `xma2defs.h` z XDK, `XMA_CONTEXT_DATA` w xenia-canary, `SHAPE_XMA_CONTEXT` na devdocs.xbox.com.

**Kto ustawia `output_buffer_valid`.** Gra ustawia go po zwolnieniu miejsca. Xenia canary zeruje go
w trzech sytuacjach: bufor pełny (zapis == odczyt), wejście zagłodzone już przy wyzwoleniu (wtedy
też zapis = odczyt, bo gry NFS używają offsetu zapisu jako detektora zastoju), kontekst bez wejścia.
Commity canary: 09dbe2cd3, 505697f98, PR #1027. Rzeczywiste zachowanie układu przy pełnym buforze:
niezweryfikowane (SHAPE ma osobny kod błędu 2, „brak miejsca na zapis”).

**Koniec strumienia.**
- rexglue-sdk (commit f24299f14) wyrównuje wyjście FFmpeg o 192 próbki: każdą klatkę dzieli,
  320 próbek trzyma w `carry_frame_` do następnej. Bez osobnego wypuszczenia ostatnia klatka nigdy
  nie trafia na wyjście, a dźwięk z jednej klatki nie daje nic. Mikser gry czeka wtedy w nieskończoność.
  Ten port: naprawione w SDK, commit 76b61d5 (`FlushCarry`), pomiar w commicie.
- xenia-canary 6bef1fbed („Drain the current frame before ending the XMA work loop”, zakleszczenie
  wątku audio w NBA Live 06): pętla dekodera ma kręcić się, dopóki bieżąca klatka nie zostanie
  wydana w całości. `Work()` w rexglue-sdk nadal kończy pętlę, gdy znika wejście. Ten port:
  DO SPRAWDZENIA, częściowo maskowane przez ciągłe dekodowanie (commit SDK 1a1469b).
- Niespójność jednostek (obserwacja researchu): `Consume()` liczy `subframe_decode_count` w blokach
  256-bajtowych, a klatkę jako `4 << is_stereo` bloków, więc w stereo jedno „sdc” to pół podramki.
  Do sprawdzenia, czy gra zakłada inną liczbę podramek na przebieg.

**Znane problemy tej gry w Xenii.** Zgłoszenia xenia-project/game-compatibility #1468 i
xenia-canary/game-compatibility #119: dźwięk umiera, dialogi się urywają, potem awaria gościa;
obejście w Xenii to pusty `sound.AST`. Awaria albo czarny ekran po War World / Battleworld,
raz przeszło bez pomijania przerywników. Ten port: hak `XmaKickStarvedContext` (`xma_fixes.cpp`)
przerywa nieskończone oczekiwanie miksera; strumienie stale pomijane od startu gry nadal są
(log `SR_LOG_XMA=2`): mikser przeczytał część bufora, a odczyt zgłoszony dekoderowi jest zaokrąglony
w dół, więc dostępne miejsce (4,1 do 4,7 KB) nie pokrywa żądania miksera. Przyczyna: niezweryfikowana.

#### 2. Odblokowanie klatek

**Co zwykle się psuje** (Unleashed Recompiled `fps_patches.cpp`, AC6_recomp, MarathonRecomp,
Glenn Fiedler „Fix Your Timestep”):
- stałe dodawane raz na klatkę (siły, rampy, zanikanie),
- współczynniki lerp liczone na klatkę (poprawnie: `1-(1-t)^(30/fps)` albo `1-exp(-k*dt)`),
- liczniki i zanikania liczone w klatkach (timery bossów, HUD),
- obsługa kolizji i komunikatów wywoływana częściej (np. nakładające się dźwięki),
- dryf liczb zmiennoprzecinkowych, gałęzie z epsilonem, które przy małym dt nigdy nie zachodzą,
- równowagi pętli sprzężenia zwrotnego (AC6: lot 2 stopnie bardziej stromy przy 60 FPS),
- całkowite dt zaokrąglane w górę (AC6: +2% szybciej przy 60 FPS),
- przerywniki tykające raz na klatkę względem zegara dźwięku, pętle wczytywania.

**Jak to robią inni.** Unleashed: podłoga dt = 1/60, przeskalowane stałe fizyki, akumulatory dla
timerów liczonych w klatkach, niektóre systemy na stałe 30 Hz z interpolacją wizualną, pętla
wczytywania ograniczona do 30 Hz, a i tak przyznają, że część błędów przy wysokich FPS zostaje.
reblue (Blue Dragon): logika stale 30 Hz, render interpolowany, bez interpolacji w filmach.
AC6: przerywniki w natywnym tempie, synchronizowane z zegarem dźwięku. Każda poprawka ma być
przy 30 FPS bit w bit przezroczysta.

**Ten port (commit 93532bb).** Skraca okres dwóch pętli 30-hercowych do 1/60 s. Sprawdzone tylko:
suma dt gry na 10 s zegara (10,016 s przy 60 FPS). NIE sprawdzone: fizyka i peleryna, animacje,
timery liczone w klatkach, przerywniki, wczytywanie. Sygnał ostrzegawczy z pomiaru HUD-u: przy
60 FPS HUD po starcie był widoczny krócej niż przy 30 FPS, co pasuje do zanikania liczonego
w klatkach (wcześniej błędnie uznane za usterkę detektora).

**Weryfikacja, której wymaga każdy tryb > 30 FPS** (ten sam skrypt przy 30, 60 i 120):
1. czas gry względem zegara (suma dt) i licznik klatek gościa, nie nakładka FPS,
2. czas zanikania HUD-u po bezczynności i czas trwania podpowiedzi,
3. pozycja Supermana po N sekundach biegu i lotu, wysokość skoku,
4. przerywnik z dialogiem: czy dźwięk i obraz kończą się razem,
5. walka: częstotliwość ataków przeciwników, cząsteczki i ich czas życia,
6. peleryna i ragdoll: brak drgań i rozciągania.

#### 3. Inne lekcje

- Vblank w rexglue-sdk przy `vsync=false` tyka 1000 Hz; ten port ma własny wątek vblank
  (`sr_native_fps_limit`), więc go to nie dotyczy, ale nie wolno go wyłączać bez zastępczego taktu.
- FPS mierzyć licznikiem klatek gościa (`guest fps`), nie nakładką liczącą prezentacje hosta.
- Podanie tej samej flagi dwa razy w linii poleceń ReXGlue gubi wszystkie flagi (rexglue-native-kit).
- Ten sam projekt na GitHubie: MrPowerUp82/superman_returns_recomp (źródło tego repo).
