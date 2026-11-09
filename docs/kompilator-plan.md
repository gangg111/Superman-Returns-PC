### Kompilator Superman Returns (wzór: t3native/builder)

Cel: jeden samowystarczalny plik `Superman Returns Builder.exe` (C++, bez Pythona), który z płyty
gracza (obraz .iso Xbox 360 albo rozpakowany folder z `default.xex` i `DATA`) robi w wybranym
folderze czystą, grywalną grę: bez logów, kopii, skryptów pomiarowych i plików diagnostycznych.
W kompilatorze nie ma żadnych danych gry; wszystko z płyty powstaje u gracza.

#### Co trafia do output\SupermanReturns

| plik | skąd |
|---|---|
| superman_returns.exe, rexruntime.dll, rexgpu-xenos.dll, VC runtime, nvngx_dlss.dll | wbudowane (nasz port, DLSS redistributable) |
| superman_returns_pipelines.bin, _shaders.srsl, _vulkan.srvk, shader_tools\ (dxc) | wbudowane |
| licenses\, THIRD_PARTY_NOTICES.txt, README (PL/EN) | wbudowane |
| superman_returns.toml | wbudowany czysty domyślny (klawisze, grafika), bez ścieżek i LUID GPU |
| game\default.xex, game\DATA\*.AST | z płyty gracza (sprawdzone SHA-256 z version.json) |
| game\DATA\ui_sr.AST, text_sr.AST | łatka (COPY z oryginału / ADD naszych bajtów) nałożona na ui.AST / text.AST gracza |
| native_audio\ (wav + manifest.json + index.bin) | dekodowane u gracza: rekordy EA (sabr/SNR) z BGFA, XMA i WMA Pro przez libavcodec |
| textures\ (przełącznik w oknie) | nasze: księżyc, woda Crest, plaża Poly Haven, panoramy pokoi; z gry: mapy okien, niebo bez słońca |

#### Budowa kompilatora (po stronie dewelopera)

`tools/builder/make_builder.py`: zbiera ładunek z białej listy (nie z folderu gracza), robi łatki UI
(narzędzie Python z build_ui_mod), kompiluje `builder/` (C++, clang z toolchainu portu) i dokleja
archiwum (jak T3BPAK: nagłówek + wpisy, kompresja Windows Compression API, bez zewnętrznych bibliotek).
Kompilator przy starcie rozpakowuje ładunek do %LOCALAPPDATA% (sprawdza CRC), okno Win32 PL/EN.

#### Moduły C++ kompilatora

1. XISO (XDVDFS): nagłówek "MICROSOFT*XBOX*MEDIA" w sektorze 32 przy przesunięciach partycji 0,
   0xFD90000 (XGD2), 0x2080000 (XGD3), 0x18300000; drzewo katalogów (lewy/prawy).
2. Kopiowanie gry + weryfikacja SHA-256 (xex i AST, z listy w ładunku).
3. Łatki UI (format COPY/ADD, sprawdzony hash wejścia i wyjścia).
4. Dźwięk: czytnik BGFA (w tym zagnieżdżone), rekordy sabr/SNR, dekodowanie XMA (ffmpeg xma2 przez
   API libavcodec), filmy ASF (WMA Pro / WMA v2) przez libavformat; WAV z pętlą smpl, manifest.json,
   index.bin (format z build_index.py). Kryterium: liczba próbek = nagłówek SNR (jak convert_audio.py).
5. Tekstury (`builder/textures.cpp`): tekstury źródłowe leżą na płycie NIESKOMPRESOWANE, kafelkowane
   jak w pamięci gościa (Xenos 2D, bajty 8-in-16), więc kompilator czyta je wprost z AST pod
   przesunięciem z `tools/builder/textures.json` (znalezione raz po bajtach zrzutów renderera,
   sprawdzane SHA-256). Dekodowanie BC1/BC3 jak Pillow (na nim liczyły narzędzia), mapa okien
   jako port make_window_map.py 1:1. Niebo bez słońca nie jest liczone od nowa (nosun.py to
   inpainting OpenCV): w ładunku jest tylko nasza różnica `(nasze - gra) mod 256`, z której nieba
   nie da się odtworzyć bez płyty.
6. Okno: wybór źródła, folderu, przełącznik "Odświeżone tekstury", pasek postępu, log do pliku.

#### Etapy

- E1: ładunek + XISO/folder + kopia + łatki UI + czysty toml → grywalna gra (oryginalny dźwięk).
- E2: natywny dźwięk (FFmpeg minimalny: demux asf, dekodery xma1/xma2/wmapro/wmav2).
- E3: tekstury (przełącznik), generatory okien i nieba w C++. Wynik: 85 z 85 plików `textures\`
  identycznych bajt w bajt z folderem SupermanReturnsPC; `--no-textures` nie tworzy folderu.
- Dowód każdego etapu: zbudowanie gry kompilatorem z `Superman Returns (USA).iso` do pustego folderu,
  porównanie plików z referencją (SHA-256 AST, ui_sr = build_ui_mod), start gry z output.
