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
