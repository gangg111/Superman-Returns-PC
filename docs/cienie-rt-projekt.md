# Cienie słońca liczone promieniami (projekt)

Stan: projekt, przed implementacją. Data: 2026-10-19.

### Problem

Cienie słońca w grze to 3 kaskady (30 / 150 / 750 jednostek, stosunek 5) wokół kamery,
policzone w teksturze warstwowej 640x640. Odbiorca liczy cień tylko do 750 jednostek od
kamery (literał w 37 shaderach), od 562,5 zanika. Z wysoka ulica jest dalej, więc cienie
znikają. Poszerzenie kaskad (`sr_shadow_range`) zostawiało pustą daleką kaskadę, bo obiekty
rzucające cień wybiera gra pod swoje małe kaskady. Szczegóły i adresy: pamięć projektu
`shadow-pipeline-map`.

### Cel

Cienie budynków widoczne z każdej wysokości i odległości, bez skoków między kaskadami,
liczone natywnie na PC (RTX 3080, DXR), bez zmian w logice gry.

### Pomysł

Budynki są statyczne. Kiedy gra raz narysuje budynek, renderer zapamiętuje jego geometrię
w przestrzeni świata. Z tej pamięci buduje strukturę do śledzenia promieni (BLAS/TLAS).
Co klatkę przebieg ekranowy wysyła z każdego piksela promień w stronę słońca i zapisuje
maskę cienia. Shadery odbiorców gry biorą cień z maski zamiast z kaskad (dla statycznej
geometrii). Ruchome obiekty (Superman, auta, ludzie) zostają z kaskadami gry z bliska,
gdzie działają poprawnie; wynik to minimum obu.

### Zmiany po red-teamie (agy, 2026-10-19)

- **Bez maski ekranowej.** Gra rysuje oświetlenie forward razem z głębią, więc pełnej
  głębi sceny przed odbiorcami nie ma (maska z poprzedniej klatki dałaby smużenie).
  Zamiast tego **RayQuery wprost w shaderze odbiorcy**: każdy z 37 odbiorców ma
  `iTexCoord7` (pozycja w przestrzeni światła kaskady 0), a renderer zna macierz tej
  kaskady (obiekt cieni +288), więc pozycja świata = odwrotność macierzy kaskady 0 razy
  `iTexCoord7`, jednakowo we wszystkich odbiorcach. Promień startuje z małym t_min.
- **Geometria w przestrzeni obiektu zamiast stream output.** Odwracanie view-projection
  po VS jest niestabilne numerycznie i psuje trójkąty przy płaszczyźnie bliskiej. BLAS z
  buforów wierzchołków i indeksów gry (pozycje przekonwertowane z big-endian do float3),
  instancja w TLAS z macierzą `gWorldXform` danego rysowania.
- **Model shaderów:** odbiorcy kompilowani jako `ps_6_5` / `lib_6_5` (dziś `ps_6_0` /
  `lib_6_3`).
- **Pierwszy dowód działania na jednym sztucznym sześcianie** w TLAS, zanim powstanie
  pamięć miasta.

### Elementy (wersja pierwotna; punkty 2 i 6 zastąpione powyżej)

1. **Kamera.** Renderer nie zna jeszcze kamery tej gry (`kInverseViewProjectionName =
   nullptr`). Katalog shaderów ma nazwane macierze: `gViewProjectionXform` (13 VS),
   `gViewInverse` (50 VS, 35 PS), `gWorldXform` (45 VS), `gWorldViewProjectionXform` (38 VS).
   Krok: odczyt `gViewProjectionXform` z rejestrów stałych rysowania sceny, odwrotność
   co klatkę. Kontrola: rekonstrukcja pozycji świata z głębi daje stałe współrzędne
   punktu na ziemi przy ruchu kamery.
2. **Przechwyt geometrii.** Dla rysowań nieprzezroczystej sceny dodatkowy przebieg z
   wyjściem strumieniowym (stream output) vertex shadera gry: pozycje w przestrzeni
   przycięcia razy odwrotność view-projection dają świat. Działa dla każdego VS (skinning,
   billboardy), bez znajomości jego stałych. Alternatywa tańsza: pozycje obiektu razy
   `gWorldXform`, gdy shader go ma.
3. **Statyczność i pamięć podręczna.** Klucz: adres bufora wierzchołków i indeksów,
   zakres rysowania, macierz świata. Geometria uznana za statyczną, gdy w kolejnych
   klatkach daje te same współrzędne świata. Poziomy szczegółowości (LOD) budynku to
   osobne wpisy; trzymamy najdokładniejszy widziany. Eksmisja po odległości (np. ponad
   3 km od kamery) i po czasie.
4. **Struktura promieni.** BLAS na wpis (współrzędne świata, transformacja jednostkowa),
   TLAS przebudowywany przy zmianie zbioru, nie co klatkę. Tylko geometria nieprzezroczysta;
   test alfa (drzewa, kraty) zostaje kaskadom gry.
5. **Kierunek słońca.** Z macierzy światła, którą gra liczy co klatkę (`sub_823028A8`,
   deskryptor +96), albo ze stałej kierunku światła w shaderach oświetlenia.
6. **Maska cienia.** Compute z inline `RayQuery` (DXR 1.1, SM 6.5): pozycja świata z
   głębi sceny, przesunięcie wzdłuż normalnej, jeden promień do słońca (później kilka z
   rozrzutem dla półcienia + filtr), wynik R8 w rozdzielczości renderu.
7. **Wpięcie w odbiorców.** Łatka generatora dla 37 shaderów ze `shadowMapOffsetScales`:
   człon cienia = min(cień z kaskad gry, maska RT w pozycji piksela); odcięcie 750
   dotyczy już tylko kaskad. Przełącznik cvar `sr_rt_shadows` do porównań A/B.

### Ryzyka

- Kolejność przebiegów: maska potrzebuje głębi sceny przed rysowaniem odbiorców; jeśli
  gra rysuje oświetlenie w przebiegu forward razem z głębią, maskę liczymy z głębi
  poprzedniej klatki albo z wcześniejszego przebiegu z samą głębią.
- Pamięć: miasto ma ok. 9000 budynków; limit wpisów i eksmisja po odległości.
- Obiekty spoza kadru nie są w pamięci, dopóki gra ich nie narysuje (budynek za kamerą,
  zanim się obrócimy). Złagodzenie: pamięć trzyma też to, co już raz było widać.
- Przezroczystość i cząsteczki nie dostają maski (jak w grze).

### Kolejność i kryteria (każdy krok kończy się pomiarem)

| Krok | Co | Dowód |
|---|---|---|
| 0 | Poziom DXR urządzenia, kompilacja odbiorców jako SM 6.5 | log: RaytracingTier >= 1.1; DXIL odbiorców przechodzi walidację |
| 1 | Pozycja świata w odbiorcy z `iTexCoord7` i macierzy kaskady 0 | podgląd pozycji świata kolorem: wzór stoi w miejscu przy ruchu kamery |
| 2 | TLAS z jednym sztucznym sześcianem, RayQuery w odbiorcach, min z kaskadami | cień sześcianu na ulicy w grze |
| 3 | Pamięć statycznej geometrii (BLAS z VB/IB gry + `gWorldXform`), eksport .obj | .obj pokazuje miasto w poprawnych proporcjach |
| 4 | Cienie miasta z pamięci, eksmisja, LOD | film z lotu: brak znikania i skoków, koszt w ms zmierzony |
| 5 | Półcień, filtr, stabilność | brak migotania przy ruchu kamery |

#### Stan kroku 3 (2026-10-20)

Budynki miasta nie idą przez `DrawIndexed` z `gWorldXform`, tylko przez draw bez indeksów
w czterech shaderach instancjonowanych (`1C89B1A6`, `2C45BFFE`, `5F7299BB`, `B27A827D`):
numer wierzchołka dzieli się na kopię `i = floor((id + 0,5) / instance_data.x)` i wierzchołek
kopii, indeksy siatki leżą w strumieniu `POSITION1` (`R32_UINT`, pamięć gry), a transformacja
kopii w `instance_transforms + i * k`: `k = 2` to przesunięcie `c0.xyz` i obrót wokół pionu
`c1`, `k = 4` to pełna macierz (`świat = x*c0 + y*c1 + z*c2 + c3`). Tabela shaderów:
`rt_instanced_regs.inc` z `tools/shaders/gen_shadow_regs.py`, przechwytywanie:
`Renderer::RtCaptureInstanced`.

Dowód: zrzut `sr_rt_dump_obj_swap=5000` (1,24 mln wierzchołków, 289 tys. trójkątów, 1676 grup
instancji) daje kwartały, ulice, estakadę i drapacze chmur we właściwym układzie
(`docs/rt-budynki-krok3.png`). W grze widok widoczności RT dzieli fasady na oświetlone
i zasłonięte, przesunięcie startu promienia 0,5 i 20 daje prawie ten sam obraz (brak
trafień we własną powierzchnię). Otwarte: RT zaciemnia więcej niż mapa cieni gry; trzeba
porównać oba wyniki piksel w piksel w zasięgu kaskady 0, zanim cień RT zastąpi kaskady.
Liczniki pokazują też dużo nowych siatek na sekundę i pominięć z budżetu 64 BLAS na klatkę.

#### Pomiar RT kontra mapa cieni gry (2026-10-20)

Metoda, bez dotykania pulpitu: `sr_input_script` wczytuje zapis i pauzuje grę w samym
sterowniku wejścia, `sr_rt_shadow_debug_cycle=60` przełącza co 60 klatek tryby 0 (gra),
2 (widoczność RT), 3 (gra bez geometrii w mapie cieni), 4 (odległość do przeszkody RT
i flaga kaskady 0), a `sr_rt_measure_at_s` zrzuca jedną klatkę każdego trybu. Cień gry =
jasność(0) / jasność(3) na pikselach odbiorców. Zrzut to około 2 GB surowych plików:
tylko na D, kasowane po analizie.

| Obszar | Gra cień, RT cień | Gra cień, RT światło | Gra światło, RT cień |
|---|---|---|---|
| Cały kadr | 223 175 | 529 | 887 606 |
| Kaskada 0 | 0 | 0 | 105 132 (obie postacie) |

- Każdy cień gry RT odtwarza (99,8%); przeszkody 92 do 360 jednostek dalej.
- Nadmiar RT poza kaskadą 0 to dalekie fasady, na których gra nie rysuje cieni wcale.
  Rzucają go te same budynki, które gra sama rysuje do mapy cieni (wyłączenie przejścia
  cieni albo instancji z osobna nie zmienia wyniku, wyłączenie obu zeruje cień RT).
- W kaskadzie 0 rozbieżność to postacie: promień trafia w budynek `mesh_131` (152 jednostki
  wysokości) 65 jednostek w stronę słońca (sprawdzone poza grą na zrzucie OBJ). Kaskada 0
  ma 30 x 30 jednostek i tylko 69 jednostek głębi (±34,5), ten budynek leży na głębokości
  światła -0,451, więc gra wycina go z mapy (brak spłaszczania rzucających przed bliską
  płaszczyzną). To błąd gry, nie RT: dokładnie znikające cienie budynków.
- Wniosek dla kroku 4: cień RT zastępuje kaskady, a nie wchodzi przez min z nimi (min
  zachowałby, ale nie naprawił, a kaskady nie mają niczego, czego nie ma RT).
- Ten sam budynek trafia do sceny RT dwa razy (przejście cieni i instancje): do odchudzenia.

#### Krok 4: cień RT zamiast kaskad (2026-10-20)

Łatki XenosRecomp 0021 i 0022. Odbiorca liczy raz na piksel widoczność słońca
(`g_srRtShadowVis`), a każda jego próbka mapy cieni (`tfetch3D` z `samp_depth`) przechodzi
przez `srRtShadowDepth`: głębia 1 tam, gdzie słońce widać, 0 tam, gdzie promień trafia.
Porównanie gry, gęstość (`shadow_dens_vis`), kolor i miękkie przejścia zostają grze.
Zasięg: odcięcie 750 jednostek i wygaszanie (FalloffStart/Rate) mnożone przez 4000/750,
czyli do zasięgu sceny RT. Włącznik: `sr_rt_shadows_replace` (domyślnie tak, przy
`sr_rt_shadows`), bajt 80 danych klatki; w widokach diagnostycznych wyłączony.

| Pomiar (ten sam kadr) | Cień w obrazie i w RT | Cień w obrazie, RT światło | RT cień, obraz światło |
|---|---|---|---|
| Kaskady gry | 218 454 | 486 | 810 386 |
| Cień RT, zasięg gry | 559 670 | 0 | 468 279 |
| Cień RT, zasięg 4000 | 589 800 | 0 | 471 798 |
| Kaskada 0, cień RT | 53 175 | 0 | 13 870 |

Zero fałszywych cieni. Zostaje: Superman (jego shader nie wykonuje gałęzi cienia, gra nie
cieniuje bohatera) i pasy dalekich fasad, gdzie gałąź cienia gry też się nie wykonuje.
Koszt (czas GPU z logu renderera, ten sam zapauzowany kadr, wewnętrznie 2560x1440):
7,32 ms bez RT, 7,77 ms z RT, czyli około 0,45 ms na klatkę (budowa TLAS i promień na
piksel odbiorcy). W locie z budową nowych BLAS: 9,9 do 10,6 ms na klatkę.
Film z lotu: `sr_record_path` (obraz wyjściowy, 30 kl./s, połowa rozdzielczości, ffmpeg
bez okna) i trasa z `sr_input_script`; nagranie spowalnia grę (odczyt synchroniczny).
Do zrobienia: dwa razy ta sama geometria w scenie RT, budżet BLAS.

#### Krok 5: półcień (2026-10-20, łatka 0023)

Odbiorca zamienia próbkę w cień liniowo przed nasyceniem: `saturate((odbiorca - próbka) * K)`,
`K = depthBias_FadeRange_FalloffStart_FalloffRate.y` (we wszystkich 37 odbiorcach tylko
w tym miejscu). Przy podmianie `srRtFade` daje `K = 0,001`, a próbka `-1000 * (1 - v)`, więc
cień = `1 - v` (+ głębia odbiorcy / 1000), ułamek widoczności tarczy słońca `v` przechodzi
przez kolor i gęstość cienia gry bez zmian. `v` z N promieni w stożku tarczy
(`sr_rt_sun_angle`, domyślnie 0,53°), obrót wzoru z pozycji w świecie (szum nie pływa
z kamerą). Najpierw 2 promienie z przeciwnych brzegów tarczy; gdy się zgadzają, piksel
jest cały w świetle albo w cieniu, resztę promieni liczy tylko półcień.

| Promienie (`sr_rt_shadow_rays`) | Wszystkie | Adaptacyjnie |
|---|---|---|
| 1 (twardy) | 7,84 ms | |
| 4 | 10,10 ms | 8,65 ms |
| 8 | 13,38 ms | 8,84 ms |
| 16 (domyślnie) | | 9,41 ms |

Bez RT ten kadr kosztuje 7,32 ms. Kaskada 0 przy półcieniu: 78 683 piksele cienia w obrazie
i w RT, 0 fałszywych. Przy 4 promieniach krawędź ma widoczne ziarno, przy 16 prawie znika.
Brakuje jeszcze cieni od postaci (skinning) i świateł innych niż słońce.

#### Cienie postaci (2026-10-20)

Postacie są rysowane jako paski trójkątów shaderami ze skinningiem (C687EB65D635C5D7,
C20921D0DBBD2F1F, 57A00D4EB3BF4C31). Każdy taki draw odbiorcy jest powtarzany przez PSO
ze stream output (ten sam VS, bez rasteryzacji), który zapisuje SRLIGHT0.xyz, czyli pozycję
po skinningu w przestrzeni światła; liczba trójkątów paska liczona na CPU z przerwami
restartu. Na początku następnej klatki wszystkie te zupy trójkątów idą do jednego BLAS-a,
instancjonowanego odwrotnością macierzy światła (jedna klatka opóźnienia). Zmierzone:
8 drawów, 10 252 trójkąty, 546 KB; w locie do 398 drawów i 62 652 trójkątów. Cień Supermana
na fasadzie i na pasach potwierdzony różnicą widoczności z postaciami i bez. Włącznik
`sr_rt_characters`, wykluczenie `sr_rt_exclude_sources=8`.

#### Odbicia RT, etap 1 (2026-10-20, łatka 0024)

37 odbiorców z `env_map` (kostka nieba budynków, szyby): próbka kostki zastąpiona tym, co
trafia promień z piksela w tym samym kierunku (`cubeMapData.cubeMapDirections`, przestrzeń
świata). Pudło: zostaje kostka gry. Maska szyb, fresnel i odcień zostają grze. Kolor
trafienia: najmniejszy mip tekstury `*col_map` drawa, który dał tę instancję (indeks SRV
w InstanceID, generacja SRV chroni przed zwolnioną teksturą), razy słońce i cień w punkcie
trafienia. Pomiar (tryby 0 i 5 w jednym przebiegu): zmieniają się tylko szyby, dolne
odbijają budynki naprzeciwko, górne niebo. Koszt: 9,9 ms bez odbić, 15,9 do 22,3 ms z nimi.
Brakuje: detalu tekstur (UV liczone inaczej w każdej parze shaderów) i taniej wersji.

#### Odbicia RT, etap 2: UV trafienia (2026-10-20, łatka 0025)

- `gen_shadow_regs.py` liczy symbolicznie (`analyze_albedo_uv`) UV pobrania `*col_map`
  w każdym pixel shaderze: `iTexCoordK.c * skala + przesunięcie`, składniki zależne od piksela
  (paralaksa) odpadają. 81 z 87 shaderów (`rt_albedo_regs.inc`, testy w
  `tests/shaders/test_gen_shadow_regs.py`).
- Siatka dostaje UV raz, przy pierwszym drawie z teksturą koloru: draw powtórzony przez PSO
  ze stream output zapisuje TEXCOORDK (4 składowe) jako zupę trójkątów w puli 192 MB
  (`kRtUvSrv`, kolejność prymitywów BLAS). Także drawy instancjonowane (pierwsza pełna kopia).
  Zmierzone: 11 657 siatek, 2,6 mln wierzchołków.
- Tabela materiałów per instancja TLAS (`kRtMaterialSrv`, InstanceID = indeks): tekstura,
  sampler, składowe i skala UV ze stałych drawu. Materiał przeżywa drawy bez tekstury
  (przejście cieni rysuje też to, co poza kadrem) i jest też pamiętany na siatce.
  Zmierzone: 81% instancji z teksturą i UV.
- Widok diagnostyczny UV (tryb 7) potwierdza spójne UV, ale tryb 6 (kolor odbicia) pokazuje
  szum: w shaderach budynków (np. 0D62162D76A532D1) kolor fasady to maska `L1_mat_map`
  mieszająca odcienie `L1_R/G/B_tint`, a `L1_col_map` jest mnożoną warstwą detalu. Jedna
  tekstura nie oddaje wyglądu materiału. Dlatego `sr_rt_reflection_detail` (bajt 96) jest
  domyślnie wyłączony i trafienia mają średni kolor tekstury jak w etapie 1.
- Dalej: albo kolor z poprzedniej klatki tam, gdzie trafienie jest na ekranie (dokładny
  wygląd), albo odtworzenie mieszania odcieni dla głównych rodzin shaderów budynków.

#### Odbicia RT, etap 3: mieszanie odcieni (2026-10-20, łatki 0026, 0027)

- `analyze_tint_blend` śledzi kanały maski `*mat_map` (zamiany składowych, normalizacja przez
  odwrotność sumy, tor skalarny, bloki `if (p0)` nie kasują znanych wartości) aż do mnożenia
  przez `R/G/B_tint`: 16 shaderów (`rt_tint_regs.inc`).
- `analyze_palette_blend` dla rodziny 8A6DD1D52752FAD6: reszta `1 - sum` waży `A_tint`, kolor
  miesza się z paletą budynku z wagą `saturate(dot(kanały i reszta, gBuildingTint))`, numer
  palety `floor(TEXCOORD5.w * 8)`, paletę 8 barw liczy symulacja łańcucha `select` na stałych
  (`rt_palette_regs.inc`; test z ręcznie przeliczoną paletą).
- Pomiar instancji: 99% budynków w scenie RT pochodzi z 8A6DD1D52752FAD6, a nie z 0D62162D.
  Jego odcienie są w grze białe (1, 1, 1), a `gBuildingTint` zerowe, więc kolor fasady to
  wprost `col_map` (wzór okien i gzymsów); różowo-beżowy ton dodaje dalej oświetlenie gry
  (`ramp_col_exp`, `gShadCol`, `ao_col_exp`), którego trafienie nie odtwarza.
- Ta sama geometria jest w scenie dwa razy (przejście cieni bez materiału, zwykły draw z nim):
  maska instancji 1 = z materiałem, 2 = bez, promień odbicia najpierw 1, przy pudle 2.
  Bez tego trafienia losowo wybierały kopię bez materiału.
- `RayQuery` zadeklarowany w pętli (`[unroll] for`) zawieszał GPU na pierwszej klatce
  z odbiciami (bisekcja przełącznikami); dwa jawne zapytania działają.
- Funkcja wołana poza gałęzią SM 6.5 bez zaślepki zabiła 204 shadery, a `build_corpus.ps1`
  skończył się kodem 0; gra tłumaczyła je w locie i chodziła w 28 kl./s.
- Ton oświetlenia (łatka 0028): stałe shadera miasta z gry, `gShadCol` (0,60 0,42 0,32),
  `ramp_col_exp` (0,60 0,50 0,59) i `Kramp_lo_hi_Kao.x` (0,97), idą co klatkę do bajtów 112..143
  danych klatki; trafienie = albedo · 1,35 · lerp(1, rampa, Kramp) · lerp(gShadCol, 1, słońce).
  Pomiar proporcji RGB: fasady 1,16 / 0,95 / 0,89, odbicia 1,28 / 0,94 / 0,78 (wcześniej szare).
  Połowa siły koloru cienia prawie nic nie zmieniła (1,26 / 0,95 / 0,79), więc zostaje wierny wzór.
- Postacie w odbiciach (łatka 0029): miały maskę 2 i promień, który trafił budynek za nimi,
  w ogóle ich nie widział. Teraz maska 1, a stream output postaci zapisuje 32 bajty na
  wierzchołek (interpolant UV shadera, potem SRLIGHT0; BLAS czyta pozycję od +16) do jednego
  z dwóch buforów na zmianę: z poprzedniej klatki budowany jest BLAS i z niego czytają UV
  trafienia (kRtSkinSrv0/1, bajt 144). Materiał na każdy draw postaci, wybierany jako
  InstanceID + GeometryIndex. Pomiar: 16 427 pikseli trafień w postać (tryb 7); w widoku
  lustra Superman z teksturą kostiumu, w klatce gry jego odbicie w szybie obok.
- Wynik w grze: szyby odbijają fasady budynków z naprzeciwka z ich wzorem zamiast nieba
  z kostki; zmienia się wyłącznie szkło. Czas GPU z odbiciami około 12,5 ms na kadr
  testowy (bez odbić wcześniej około 9,9 ms, inny przebieg).
