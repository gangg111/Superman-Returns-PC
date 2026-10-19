### Plan: natywny silnik dźwięku (przepisanie EA SndPlayer + RenderWare Audio na XAudio2)

Cel: dźwięk 1:1 z konsolą bez emulacji sprzętu XMA i bez haków na mikser gry. Kod gry decydujący,
co i kiedy zagrać (warstwa dźwięku gry, sterowana plikami XML `SoundDefFile`), zostaje
przekompilowany; przepisujemy wszystko pod nim. Po osiągnięciu 1:1 przychodzi remaster.

#### Stan wyjściowy (zmierzony)

Trzy warstwy w kodzie gry:
1. Warstwa dźwięku gry, 0x8231xxxx-0x8236xxxx: `FMSoundSystem`, `FSGameSound`, definicje
   z XML (`audiolist`, `soundlist`, `filterlist`, `controllist`, `playtypelist`, magistrale
   `MIXER_BUS_*`), muzyka (`Superman_Load_Music`, `streamFiles`). ZOSTAJE.
2. EA SndPlayer, okolice 0x8265xxxx: rejestr kodeków `Xas0 EL30 P6B0 EXm0`, `Snd::SubMix`,
   strumienie `%s_sns`. PRZEPISUJEMY.
3. RenderWare Audio Core (`rw::audio::core`), 0x8264xxxx-0x8267xxxx: głosy, mikser,
   opóźnienie, filtr dolnoprzepustowy, rozciąganie czasu, wyjście `Dac` przez
   `XAudioSubmitRenderDriverFrame`; pod spodem konteksty XMA. PRZEPISUJEMY.

Granica: około 116 funkcji z zakresu 0x8264-0x8268 jest wołanych z zewnątrz; główni klienci
to warstwa gry (0x8232-0x8233, np. `sub_8264C3D8` z 30 miejsc) i moduł 0x8235xxxx.

Dane: `sound.AST` (1917 wpisów), `voice.AST` (6358), archiwa poziomów z zagnieżdżonymi BGFA.
Rekordy `sabr` z parametrami i nagłówkiem EAAudioCore SNR (EA-XMA, 44,1 kHz, 1/2/4 kanały),
strumienie w `NAZWA_SNS`. Dekodowanie jednego dźwięku przez FFmpeg sprawdzone.

Wzorce: andrewnakas/skate3-audio (RenderWare Audio w Skate 3: kolejka poleceń, struktury,
tryb cienia), vgmstream (EAAC, EA-XMA), QuickBMS nbajamfire.bms (BGFA).

#### Definicja „1:1” (mierzalna)

1. **Zdarzenia:** ten sam scenariusz gry daje tę samą sekwencję wywołań API silnika
   (dźwięk, start/stop, głośność, wysokość, pozycja 3D, magistrala) w oryginale i w nowej
   wersji. Porównanie dziennika wywołań, nie słuchu.
2. **Próbki:** każdy przekonwertowany dźwięk ma dokładnie tyle próbek i te same punkty pętli,
   co zdekodowany przez oryginalny tor (porównanie z nagraniem kontekstu XMA).
3. **Miks:** nagranie wyjścia (`audio_dump_wav`) z oryginału i z nowego silnika w tym samym
   scenariuszu: obwiednia głośności per pasmo w oknach 100 ms w granicy ustalonej po
   pomiarze szumu między dwoma przebiegami oryginału. Bit w bit nie jest celem, bo miksowanie
   XAudio2 liczy inaczej niż DSP RenderWare.
4. **Odsłuch:** pary klipów przed/po dla kluczowych scen (muzyka, dialogi, walka, lot).

**Zmierzony margines miksu (2026-10-19).** Dwa identyczne przebiegi ORYGINAŁU (Vulkan, port
natywny, 30 FPS, start nowej gry i trzy wzbicia), nagrane `--audio_dump_raw`, porównane
`tools/audio/mixdiff.py`: ekran tytułowy i menu (0-60 s) mediana różnicy 1,4-3,8 dB,
95. percentyl 6-19 dB; rozgrywka ma okna z dźwiękiem w jednym przebiegu i ciszą w drugim
(p95 do 79 dB), bo przechodnie, ruch i wczytywanie nie są deterministyczne. Kryterium 3 jest
więc tylko zgrubne: nowy silnik ma mieścić się w tym rozrzucie. O 1:1 rozstrzygają kryteria
1 (dziennik wywołań API) i 2 (każdy dźwięk osobno, co do próbki).

#### Etapy

1. **Mapa API** (bez zmian w zachowaniu gry): lista funkcji granicznych z argumentami,
   nazwy z napisów i metadanych wtyczek, struktury (głos, gracz, system, kolejka poleceń).
   Narzędzia: graf wywołań, odnośniki do napisów, mapa linkera, próbnik stosu.
2. **Rejestrator API:** hooki na funkcjach granicznych logują wywołania z argumentami
   (włączane przełącznikiem). Daje referencję zdarzeń do kryterium 1 i pokazuje, które
   funkcje są naprawdę używane.
3. **Konwerter danych w instalatorze:** BGFA → rekordy `sabr` → SNR/SNS → PCM (WAV) +
   manifest JSON (parametry, kanały, pętle). Weryfikacja na wszystkich wpisach, nie na próbce.
4. **Silnik natywny:** głosy, magistrale `MIXER_BUS_*`, filtry, panorama 3D, pogłos i
   resampling przepisane w C++ jako wierne odpowiedniki funkcji RenderWare (każda funkcja
   najpierw w trybie cienia przeciw oryginałowi, jak w skate3-audio). XAudio2 służy WYŁĄCZNIE
   do wyjścia (głos główny z naszym miksem 48 kHz). Wbudowanych filtrów, pogłosu i X3DAudio
   NIE używamy: filtr XAudio2 to filtr state-variable z odcięciem najwyżej fs/6, czyli 8 kHz
   przy 48 kHz (learn.microsoft.com, XAUDIO2_FILTER_PARAMETERS), pogłos to Princeton Digital
   Reverb (XAudio2CreateReverb), więc 1:1 z IIR i ReverbModel1 RenderWare nimi się nie da.
5. **Tryb cienia:** oryginał gra, nowy silnik dostaje te same wywołania i porównuje stan
   (aktywne głosy, parametry). Przełączenie dopiero przy zerowych rozbieżnościach
   w pełnych sesjach.
6. **Przełączenie i usunięcie** emulacji XMA, haka `XmaKickStarvedContext` i ścieżki
   `XAudioSubmitRenderDriverFrame`.

#### Stan etapów

- **Etap 1 (2026-10-19):** mapa API w `docs/natywny-dzwiek-api.md`, rejestrator
  `SR_LOG_AUDIO_API`, surowe nagranie miksu `--audio_dump_raw`, `tools/audio/mixdiff.py`.
- **Etap 3, konwerter (2026-10-19):** `tools/audio/convert_audio.py` + `tools/audio/ea_ast.py`.
  Czyta wszystkie archiwa BGFA (także zagnieżdżone i skompresowane RefPack), rekordy `sabr`
  według reguły z kodu gry (`sub_8232DD10`: `numc` = liczba wpisów, SNR na granicy 16 bajtów),
  dekoduje EA-XMA przez FFmpeg (4 kanały = dwa strumienie stereo). Wynik: 4648 dźwięków
  (1616 z `sound.AST`, 3032 głosy z `voice.AST`), **4648/4648 z dokładną liczbą próbek
  z nagłówka**, 0 błędów, 3,0 GB WAV z pętlami w `smpl` + `manifest.json`. Pozostałe archiwa
  zawierają wyłącznie banki wariantów (781), które wskazują te dźwięki po nazwie.
  Otwarte: pełny układ nagłówka banku (lista właściwości całego banku przed wariantami).

- **Etap 4, początek (2026-10-20):** `SnP0` rozpoznany (żądanie startu, deskryptory wszystkich 14 klas
  w `.data`, `docs/natywny-dzwiek/deskryptory.txt`), bank próbek `port/src/native_audio/sample_bank`
  z indeksem `index.bin` i testami (`tests/native_audio`). Specyfikacje DSP wtyczek:
  `docs/natywny-dzwiek/spec-*.md`.

- **Etap 4, rdzeń gotowy (2026-10-20):** natywny silnik w `port/src/native_audio` jest **bit w bit
  zgodny z oryginałem** w samoteście `SR_AUDIO_SELFTEST=<raport>` (proces startuje bez rozgrywki,
  oryginały wywoływane jako zrekompilowane funkcje na tych samych danych, 0 rozbieżności):
  resampler (100 przypadków, 30 tys. wywołań), filtry LP/HP, Gain, PanSurround, Send/SubMix/Route,
  pogłos, opóźnienie, VuMeter, finalizacja Dac, cały graf (Voice::Create, kolejka poleceń,
  wykonawca, zniszczenie głosu z wygaszeniem resztki, tryb ogona; 4800 bloków) i SndPlayer
  (planer, polecenia, pętle, starty z opóźnieniem, zatrzymania, wysokość, IsDone, pozycja; 5089
  bloków). Wzorcem jest kod zrekompilowany: `vmaddfp` = mnożenie i dodawanie w FTZ/DAZ,
  `fmadds` = fma w double; przenikanie opóźnienia ma przestawione mnożenie w jednej kopii pętli.
  Bank próbek: indeks v2 z liczbą próbek w blokach EAAC (wszystkie dźwięki w pamięci mają 1 blok,
  pętle strumieni zaczynają się na granicy bloku). Dźwięk filmów przekonwertowany (34 filmy).
  Dalej: most hooków gra -> silnik natywny, tryb cienia w grze, warstwa EA SND filmów.

- **Pierwsze przebiegi w grze (2026-10-20, sterowanie bez obrazu, okno zapory Windows zasłaniało
  grę):** `SR_NATIVE_AUDIO=shadow`: 25 000 bloków (133 s, 1870 startów dźwięków) **bit w bit
  zgodnych** z oryginałem, 0 różnic zwracanych wartości, IsDone i identyfikatorów żądań.
  `shadow-bank`: bank rozpoznaje **1699/1699** (potem 1844/1844) dźwięków, w tym strumienie po
  pliku archiwum i przesunięciu `_SNS`. Po wyrównaniu bank zgadza się z dekoderem gry co do
  0,00003-0,00015 (pojedyncze LSB), ale **emulowany dekoder XMA opóźnia każdy dźwięk o 1536 klatek**
  (3 ramki XMA, 32 ms; strumienie 4-kanałowe 1773): na początku daje ciszę, a ponieważ odtwarzacz
  gra dokładnie tyle próbek, ile mówi nagłówek, w obecnym porcie każdy dźwięk startuje 32 ms za
  późno i traci ostatnie 32 ms. Bank (FFmpeg z pominięciem opóźnienia kodera) zaczyna od pierwszej
  prawdziwej próbki. Do rozstrzygnięcia, jak robi to sprzęt konsoli. `native`: 146 s bez awarii,
  dźwięk ciągły, poziom -24..-12 dBFS, 0 przesterowań.

- **Tryb `full` (2026-10-19):** oryginalny silnik nie renderuje ani nie dekoduje; wykonawca gościa
  pominięty dla głosu master, zapytania stanu (`GetParamF/D`, `IsReleaseDone`, latencja, `IsDone`)
  odpowiada silnik natywny. W trybie native każda z tych odpowiedzi jest porównywana z oryginałem
  (65 tys. `GetParamF`, ponad tysiąc odczytów latencji: 0 różnic). Dwie pułapki:
  1. Dac czyta bufor wyjścia, który przydzielał graf gościa: bez niego odczyt spod adresu 0.
  2. **Gra czyta stan odtwarzacza wprost z jego pamięci**, nie przez API: `sub_82333608`
     przełącza kanał gry ze stanu 1 w 2 („gra”), gdy `SnP0+40` (stan planera) = 2. Dopiero w
     stanie 2 działa wyciszanie dźwięków poza zasięgiem (`sub_82332138`: odległość od słuchacza
     większa niż zasięg z obiektu dźwięku, wtedy `Channel::Stop` i znacznik `+417`). Bez lustra
     tego pola odległe źródła grały do końca (453 zamiast 11 głosów dłuższych niż 1 s), pula głosów
     się zapychała i gra zabierała głos muzyce (15 s ciszy). Most przepisuje teraz `+40` z
     natywnego odtwarzacza po każdym bloku i zapomina wtyczki zwolnionego głosu.
  Wynik: poziomy w każdym oknie 5 s identyczne z trybem native, 2274 przejść do stanu 2 (native
  2548). Krótkie efekty żyjące 30 ms w trybie native to zamierzone wyciszanie odległych źródeł, nie
  błąd dekodera. Dźwięk filmów idzie osobną warstwą i w trybie full gra bez zmian.

#### Metoda weryfikacji DSP (po red-teamie agy, 2026-10-20)

1. **Test różnicowy wtyczki przeciw oryginałowi w tym samym procesie.** Oryginalny DSP RenderWare
   istnieje u nas jako zrekompilowany kod x86 (`sub_XXXXXXXX`), więc da się go wywołać na tych samych
   danych wejściowych w pamięci gościa co natywną wersję i porównać bloki 256 próbek. Zastrzeżenie
   agy, że bit w bit odpada przez różnice FPU PPC i x86, tu nie obowiązuje: obie strony liczą na
   x86. Rozbieżność, jeśli wystąpi, wynika z kolejności działań albo z emulacji `fmadd`/VMX128
   w kodzie wygenerowanym, i to trzeba odwzorować.
2. **Tryb cienia liczony wewnątrz wywołania `Dac` gościa** (te same granice bloków i ten sam stan
   kolejki poleceń po opróżnieniu), nie na osobnym wątku: bez dryfu zegarów.
3. **Wyrównanie próbek:** zanim przyjmiemy WAV z konwertera za wzorzec, porównać korelacją jeden
   dźwięk w nagraniu wyjścia oryginału z plikiem (ryzyko przesunięcia startu dekodera XMA).
4. **Strumienie filmów** (0x82656-0x8265F) korzystają z miksera rdzenia z pominięciem
   `Voice::Create` (wywołania alokatora i `sub_826502A8`, `sub_826505E8`). Rozpracować przed
   przełączeniem, inaczej filmy zostaną bez dźwięku. To osobny silnik EA SND (0x82655FC8..0x8266CA20,
   ponad 60 funkcji na globalach 0x8299FED8/0x8299FDF0, rekordy głosów po 132 B). Filmy:
   `fmv.AST` (28) i `fmvlegal.AST` (5 logo) to kontenery ASF, obraz WMV3 1280x720 30 kl./s, dźwięk
   **WMA Pro 5.1 (28), WMA Pro stereo (2), WMA v2 stereo (4), 44,1 kHz**. FFmpeg dekoduje
   oba, `tools/audio/convert_movies.py` daje 34/34 filmów (23,8 min, 613 MB). Tor natywny: PCM plus zegar z
   `Stream::GetRequestInfo` (ms odtworzone), który moduł filmów odpytuje.

#### Pierwszy eksperyment (najtańszy, najwięcej rozstrzyga)

Surowe nagranie ramek miksu, które gra wysyła przez `XAudioSubmitRenderDriverFrame`
(256 próbek x 6 kanałów, float, big-endian, planarnie, 48 kHz), bez miksowania w dół. To punkt
odniesienia 1:1 dla wszystkich kolejnych porównań (tzw. oracle tap w skate3-audio). Obecny
`audio_dump_wav` składa do stereo 16-bit, więc do porównań się nie nadaje.

#### Ryzyka

Źródła: research 2026-10-18 (subagent z WebFetch, kluczowe twierdzenie o filtrze sprawdzone
osobno na learn.microsoft.com).
- Gra zależy od stanu silnika: odpytywanie „czy dźwięk się skończył”, `EVENT_ISPACKETDONE`,
  zegar filmów i dialogów liczony z odtworzonego dźwięku. SpeedBreaker (NFS MW): przy zastoju
  wyjścia gra zawieszała się przy następnym czekaniu na koniec dźwięku
  (SpeedBreakerProject/speedbreaker, runtime/apu/audio_cadence.h).
- Rytm wywołań callbacku miksera: wywoływany seriami zostawiał puste głosy i dźwięk „pocięty
  o 50%” (SpeedBreaker, runtime/apu/audio.cpp). Callback ma tykać równo co 256/48000 s.
- Kwant XAudio2 na Windows to 10 ms, na Xboksie 360 było 5 1/3 ms (learn.microsoft.com,
  XAudio2 audio graph): przesuwa krawędzie zmian parametrów, jeśli miksowanie oprzemy na nim.
- Dokładność DSP: kernele VMX128 z FMA i flush-to-zero zależnym od klasy instrukcji
  (skate3-audio, docs/PLAN.md); odtworzenie bit w bit wymaga tej samej kolejności działań.
- Konwersja danych: EA przepakowuje bloki XMA i usuwa wypełnienie pakietów; dekoder ma być
  jeden na strumień, inaczej giną próbki nakładki MDCT (skate3-audio, docs/rust-port.md).

- Funkcje graniczne mogą przekazywać wskaźniki do struktur silnika, które gra czyta
  bezpośrednio (np. stan „czy dialog się skończył”); wtedy nowy silnik musi utrzymywać te
  pola w pamięci gościa.
- Kolejka poleceń jest wypełniana z wielu wątków (w Skate 3 bez blokady, błąd wyścigu).
- Efekty DSP (opóźnienie z przenikaniem, rozciąganie czasu, pogłos) trzeba odtworzyć
  w XAudio2 albo własnym APO.
- Synchronizacja przerywników z dźwiękiem (dialogi wyznaczają tempo scen).
