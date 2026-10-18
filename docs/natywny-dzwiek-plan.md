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
