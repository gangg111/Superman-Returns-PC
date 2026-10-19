### Tekstury w remasterze: zrzut, poprawa, podmiana

Gra zostaje 1:1, zmieniamy tylko to, co renderer wgrywa na kartę. Każda tekstura 2D ma stałą
nazwę z zawartości: `<skrót XXH3 bajtów gry>_<szer>x<wys>_f<format Xenos>`. Ten sam skrót renderer
liczy i tak do wykrywania zmian tekstur (`CreateTexture`), więc nazwa jest ta sama w każdym
uruchomieniu i na każdym procesorze.

#### Kroki

1. **Zrzut** (gra): `--sr_texture_dump_dir=<katalog>` zapisuje każdą teksturę raz jako `<nazwa>.dds`
   (dane po dekodowaniu, nagłówek DX10) i `<nazwa>.json` (format, wymiary, mipmapy, gamma,
   przestawienie kanałów). Tekstury, których zawartość pod tym samym adresem zmienia się szybciej niż
   co 2 s (klatki filmów, tabele pisane przez procesor), zapisują się raz i dalej są pomijane.
2. **PNG** (`tools/textures/texture_png.py to-png <zrzut> <png>`): RGBA tak, jak widzi je gracz
   (przestawienie kanałów zastosowane), plus `kind`: `color`, `mono` (jeden kanał jako szarość) albo
   `normal` (mapa normalnych BC5, nie przez model AI).
3. **Poprawa**: dowolnym narzędziem, z zachowaniem nazwy pliku (AI Image Studio, ręcznie, inny model).
4. **DDS** (`texture_png.py to-dds <png> <podmiany>`): R8G8B8A8, pełny łańcuch mipmap.
5. **Podmiana** (gra): katalog `textures` obok exe albo `--sr_texture_replace_dir=<katalog>`
   (`off` wyłącza). Plik o nazwie tekstury jest wgrywany zamiast niej, z własnymi wymiarami,
   formatem (RGBA8, BC1 do BC7) i mipmapami, ze zwykłym mapowaniem kanałów.

#### Jak to sprawdzono

`--sr_texture_replace_from_swap=N` włącza podmiany dopiero od klatki N i wtedy czyści pamięć
podręczną tekstur, więc w jednym przebiegu klatki przed N są z teksturami gry, a po N z podmianami.
W menu pauzy (świat stoi: klatki 1650 i 1690 oraz 1720 i 1760 identyczne) podmiana wszystkich
809 tekstur kolorowych tymi samymi teksturami powiększonymi 2× (Lanczos): średni kolor klatki zmienił
się o mniej niż 0,4/255, żaden piksel nie różni się o więcej niż 48, różnicę powyżej 16 ma 0,77%
pikseli. Wniosek: kanały, orientacja i mipmapy są poprawne.

#### Pierwszy wsad AI i wniosek

597 tekstur-obrazów (filtr: co najmniej 256 kolorów, nie mapa normalnych, lista wykluczeń) przez
Real-ESRGAN x4 z AI Image Studio (kafelki 192 px, 658 s na RTX 3080), BC7: 1,1 GB. W grze, pas
budynków nieruchomy (kontrola: dwie klatki z AI różnią się o 0,01/255): tekstury AI zmieniają go
średnio o 1,42/255, ostrość 10,75 i 10,66 (`docs/tekstury-ai-porownanie.png`). Przyczyna: gra
renderuje wewnętrznie 1280x720, fasada zajmuje na ekranie mniej pikseli niż oryginalna tekstura,
więc karta i tak próbkuje pomniejszoną mipmapę. **Tekstury wyższej rozdzielczości mają sens dopiero
po podniesieniu rozdzielczości wewnętrznej** (`sr_native_render_scale`).

#### Ograniczenia (stan na 2026-10-19)

- Tylko renderer D3D12 (domyślny). Vulkan jeszcze nie podmienia.
- Podmiany RGBA8 są duże (809 tekstur 2× to 1,5 GB) i wczytują się synchronicznie przy tworzeniu
  tekstury. Docelowo BC7 (`texconv` z DirectXTex) i wczytywanie w tle.
- Powiększone tekstury to pochodne zasobów EA: narzędzie działa na kopii gry gracza, paczek nie
  publikujemy.
