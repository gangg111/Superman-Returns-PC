// Superman Returns Builder: the window (as t3native's builder: a dark panel, the
// player's game and the output folder, BUILD, progress and a log) and a window-less
// mode for scripts and tests:
//   "Superman Returns Builder.exe" --build --src <iso or folder> --out <folder> [--log <file>] [--no-textures]
// The payload is read from this .exe (pack.h); the work is build.cpp's.
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shobjidl.h>

#include <atomic>
#include <cstdio>
#include <fstream>
#include <array>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "build.h"
#include "pack.h"
#include "xiso.h"

using namespace srb;

namespace {

// --- words ------------------------------------------------------------------

int g_lang = 0;  // kLangs: 0 English, 1 Polish, 2 Spanish, 3 German, 4 Italian
constexpr const wchar_t* kLangs[] = {L"en", L"pl", L"es", L"de", L"it"};
constexpr const wchar_t* kLangLabels[] = {L"EN", L"PL", L"ES", L"DE", L"IT"};
constexpr int kLangCount = 5;

const std::map<std::string, std::array<const wchar_t*, 5>> kWords = {
    {"subtitle", {L"NATIVE PC PORT",
                 L"NATYWNY PORT NA PC",
                 L"PORT NATIVO PARA PC",
                 L"NATIVER PC-PORT",
                 L"PORT NATIVO PER PC"}},
    {"source", {L"Superman Returns (USA), Xbox 360: disc image (.iso) or game folder",
               L"Superman Returns (USA), Xbox 360: obraz płyty (.iso) albo folder gry",
               L"Superman Returns (USA), Xbox 360: imagen del disco (.iso) o carpeta del juego",
               L"Superman Returns (USA), Xbox 360: Disc-Image (.iso) oder Spielordner",
               L"Superman Returns (USA), Xbox 360: immagine del disco (.iso) o cartella del gioco"}},
    {"output", {L"Folder for the finished game",
               L"Folder na gotową grę",
               L"Carpeta para el juego terminado",
               L"Ordner für das fertige Spiel",
               L"Cartella per il gioco finito"}},
    {"choose", {L"Choose",
               L"Wybierz",
               L"Elegir",
               L"Auswählen",
               L"Scegli"}},
    {"folder", {L"Folder",
               L"Folder",
               L"Carpeta",
               L"Ordner",
               L"Cartella"}},
    {"build", {L"BUILD GAME",
              L"ZBUDUJ GRĘ",
              L"CREAR JUEGO",
              L"SPIEL ERSTELLEN",
              L"CREA GIOCO"}},
    {"cancel", {L"Cancel",
               L"Anuluj",
               L"Cancelar",
               L"Abbrechen",
               L"Annulla"}},
    {"open", {L"Open folder",
             L"Otwórz folder",
             L"Abrir carpeta",
             L"Ordner öffnen",
             L"Apri cartella"}},
    {"none", {L"not chosen",
             L"nie wybrano",
             L"sin elegir",
             L"nicht gewählt",
             L"non scelto"}},
    {"checking", {L"checking…",
                 L"sprawdzam…",
                 L"comprobando…",
                 L"wird geprüft…",
                 L"controllo…"}},
    // short: shown at the right of the source's label, which names the release
    {"recognized", {L"recognized",
                   L"rozpoznano",
                   L"reconocido",
                   L"erkannt",
                   L"riconosciuto"}},
    {"not_game", {L"not an Xbox 360 game",
                 L"to nie gra z Xbox 360",
                 L"no es un juego de Xbox 360",
                 L"kein Xbox-360-Spiel",
                 L"non è un gioco Xbox 360"}},
    {"wrong_release", {L"another release",
                      L"inne wydanie",
                      L"otra edición",
                      L"andere Version",
                      L"altra edizione"}},
    {"step_check", {L"Checking the game",
                   L"Sprawdzam grę",
                   L"Comprobando el juego",
                   L"Spiel wird geprüft",
                   L"Controllo del gioco"}},
    {"step_port", {L"Writing the port",
                  L"Zapisuję port",
                  L"Escribiendo el port",
                  L"Port wird geschrieben",
                  L"Scrittura del port"}},
    {"step_game", {L"Copying the game",
                  L"Kopiuję grę",
                  L"Copiando el juego",
                  L"Spiel wird kopiert",
                  L"Copia del gioco"}},
    {"step_patch", {L"Making the menus",
                   L"Tworzę menu",
                   L"Creando los menús",
                   L"Menüs werden erstellt",
                   L"Creazione dei menu"}},
    {"step_textures", {L"Making the textures",
                      L"Tworzę tekstury",
                      L"Creando las texturas",
                      L"Texturen werden erstellt",
                      L"Creazione delle texture"}},
    {"step_audio", {L"Converting the sounds",
                   L"Konwertuję dźwięki",
                   L"Convirtiendo los sonidos",
                   L"Sounds werden umgewandelt",
                   L"Conversione dei suoni"}},
    {"textures", {L"Remastered textures",
                 L"Odświeżone tekstury",
                 L"Texturas remasterizadas",
                 L"Überarbeitete Texturen",
                 L"Texture rimasterizzate"}},
    {"step_finish", {L"Finishing",
                    L"Kończę",
                    L"Terminando",
                    L"Abschluss",
                    L"Completamento"}},
    {"done", {L"Done: the game is in ",
             L"Gotowe: gra jest w ",
             L"Listo: el juego está en ",
             L"Fertig: das Spiel ist in ",
             L"Fatto: il gioco è in "}},
    {"failed", {L"The build stopped: ",
               L"Budowa przerwana: ",
               L"La creación se detuvo: ",
               L"Erstellung abgebrochen: ",
               L"Creazione interrotta: "}},
    {"cancelled", {L"cancelled",
                  L"anulowano",
                  L"cancelado",
                  L"abgebrochen",
                  L"annullato"}},
    {"space", {L"not enough free space in the output folder",
              L"za mało wolnego miejsca w folderze docelowym",
              L"no hay espacio libre suficiente en la carpeta de destino",
              L"nicht genug freier Speicher im Zielordner",
              L"spazio libero insufficiente nella cartella di destinazione"}},
    {"missing", {L"the game has no ",
                L"w grze brakuje ",
                L"al juego le falta ",
                L"im Spiel fehlt ",
                L"nel gioco manca "}},
    {"wrong_game", {L"a file differs from Superman Returns (USA): ",
                   L"plik różni się od Superman Returns (USA): ",
                   L"un archivo no coincide con Superman Returns (USA): ",
                   L"eine Datei weicht von Superman Returns (USA) ab: ",
                   L"un file non corrisponde a Superman Returns (USA): "}},
    {"no_payload", {L"This builder has no port inside (the developer's build).",
                   L"Ten kompilator nie ma w sobie portu (wersja deweloperska).",
                   L"Este creador no lleva el port dentro (versión de desarrollo).",
                   L"Dieser Builder enthält keinen Port (Entwicklerversion).",
                   L"Questo builder non contiene il port (versione di sviluppo)."}},
    {"note", {L"The finished game holds data from your disc: for your own private use only.",
             L"Gotowa gra zawiera dane z Twojej płyty: tylko do Twojego prywatnego użytku.",
             L"El juego terminado contiene datos de tu disco: solo para tu uso privado.",
             L"Das fertige Spiel enthält Daten von deiner Disc: nur für den privaten Gebrauch.",
             L"Il gioco finito contiene dati del tuo disco: solo per uso privato."}},
};


std::wstring W(const std::string& key) {
  auto it = kWords.find(key);
  if (it == kWords.end()) return Widen(key);
  return it->second[g_lang];
}

// A Failure's key ("missing:DATA/x.AST") in words.
std::wstring Explain(const std::string& what) {
  const size_t colon = what.find(':');
  if (colon != std::string::npos && kWords.count(what.substr(0, colon)))
    return W(what.substr(0, colon)) + Widen(what.substr(colon + 1));
  if (kWords.count(what)) return W(what);
  return Widen(what);
}

std::wstring StepName(Step s) {
  switch (s) {
    case Step::Check: return W("step_check");
    case Step::Port: return W("step_port");
    case Step::Game: return W("step_game");
    case Step::Patch: return W("step_patch");
    case Step::Textures: return W("step_textures");
    case Step::Audio: return W("step_audio");
    default: return W("step_finish");
  }
}

double Overall(Step s, double f) {
  switch (s) {
    case Step::Check: return 0.01 * f;
    case Step::Port: return 0.01 + 0.04 * f;
    case Step::Game: return 0.05 + 0.20 * f;
    case Step::Patch: return 0.25 + 0.01 * f;
    case Step::Textures: return 0.26 + 0.02 * f;
    case Step::Audio: return 0.28 + 0.71 * f;
    default: return 0.99 + 0.01 * f;
  }
}

fs::path ExePath() {
  wchar_t p[MAX_PATH];
  GetModuleFileNameW(nullptr, p, MAX_PATH);
  return p;
}

// The release the payload's plan names: default.xex's SHA-256.
std::string ExpectedXex(const Pack& pack) {
  const auto text = pack.Get("builder.txt");
  const std::string s(text.begin(), text.end());
  const size_t at = s.find("game default.xex ");
  return at == std::string::npos ? "" : s.substr(at + 17, 64);
}

// --- the window-less build ----------------------------------------------------

int RunBuild(const Pack& pack, const BuildOptions& options, const fs::path& log_path) {
  std::ofstream log(log_path.empty() ? fs::path(L"NUL") : log_path);
  std::atomic<bool> cancel{false};
  int last = -1;
  try {
    const fs::path dir = Build(pack, options, cancel, [&](Step s, double f, const std::string& name) {
      const int pct = int(Overall(s, f) * 100);
      if (pct != last) {
        last = pct;
        log << pct << "% " << Narrow(StepName(s)) << " " << name << std::endl;
      }
    });
    log << Narrow(W("done")) << Narrow(dir.wstring()) << std::endl;
    return 0;
  } catch (const std::exception& e) {
    log << Narrow(W("failed")) << Narrow(Explain(e.what())) << std::endl;
    return 1;
  }
}

// --- the window -------------------------------------------------------------------

constexpr COLORREF kBg = RGB(0x12, 0x12, 0x18), kPanel = RGB(0x1c, 0x1c, 0x26), kField = RGB(0x26, 0x26, 0x33),
                   kText = RGB(0xec, 0xec, 0xf2), kMuted = RGB(0x8a, 0x8a, 0x9e), kAccent = RGB(0xff, 0xce, 0x00),
                   kError = RGB(0xff, 0x5a, 0x5a), kOk = RGB(0x3d, 0xdc, 0x84), kRed = RGB(0xd8, 0x1e, 0x2c);

enum : int { kIdIso = 101, kIdFolder, kIdOut, kIdBuild, kIdCancel, kIdOpen, kIdLog };
enum : UINT { kMsgProgress = WM_APP + 1, kMsgDone, kMsgChecked };

struct App {
  Pack pack;
  bool has_pack = false;
  std::string expected_xex;
  HWND wnd = nullptr, log = nullptr;
  HWND btn_iso = nullptr, btn_folder = nullptr, btn_out = nullptr, btn_build = nullptr, btn_cancel = nullptr,
       btn_open = nullptr;
  HFONT title = nullptr, sub = nullptr, label = nullptr, note_font = nullptr, body = nullptr, big = nullptr, mono = nullptr;
  int dpi = 96;
  fs::path source, output, finished;
  std::wstring source_note;
  COLORREF source_color = kMuted;
  bool source_ok = false;
  bool textures = true;
  RECT textures_hit{};
  std::atomic<bool> building{false}, cancel{false};
  bool close_after = false;
  std::mutex mtx;
  Step step = Step::Check;
  double fraction = 0;
  std::wstring status;
  RECT langs[5]{};  // the language buttons, kLangs' order
};
App g;

int S(int v) { return MulDiv(v, g.dpi, 96); }

fs::path SettingsPath() { return ExePath().parent_path() / L"builder_settings.txt"; }

void LoadSettings() {
  std::wifstream in(SettingsPath());
  std::wstring line;
  while (std::getline(in, line)) {
    if (line.rfind(L"lang=", 0) == 0)
      for (int i = 0; i < kLangCount; ++i)
        if (line.substr(5) == kLangs[i]) g_lang = i;
    if (line.rfind(L"source=", 0) == 0) g.source = line.substr(7);
    if (line.rfind(L"output=", 0) == 0) g.output = line.substr(7);
    if (line.rfind(L"textures=", 0) == 0) g.textures = line.substr(9) != L"0";
  }
  if (!fs::exists(SettingsPath())) {
    const LANGID ui = PRIMARYLANGID(GetUserDefaultUILanguage());
    g_lang = ui == LANG_POLISH ? 1 : ui == LANG_SPANISH ? 2 : ui == LANG_GERMAN ? 3 : ui == LANG_ITALIAN ? 4 : 0;
  }
}

void SaveSettings() {
  std::wofstream out(SettingsPath());
  out << L"lang=" << kLangs[g_lang] << L"\nsource=" << g.source.wstring() << L"\noutput="
      << g.output.wstring() << L"\ntextures=" << (g.textures ? L"1" : L"0") << L"\n";
}

HFONT MakeFont(int size, int weight, const wchar_t* face = L"Bahnschrift") {
  return CreateFontW(-S(size), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, face);
}

void MakeFonts() {
  for (HFONT f : {g.title, g.sub, g.label, g.note_font, g.body, g.big, g.mono})
    if (f) DeleteObject(f);
  g.title = MakeFont(34, FW_BOLD);
  g.sub = MakeFont(18, FW_BOLD);
  g.label = MakeFont(16, FW_SEMIBOLD);
  g.note_font = MakeFont(13, FW_NORMAL);
  g.body = MakeFont(15, FW_NORMAL);
  g.big = MakeFont(20, FW_BOLD);
  g.mono = MakeFont(12, FW_NORMAL, L"Consolas");
}

void Text(HDC dc, const std::wstring& s, RECT r, HFONT font, COLORREF color, UINT fmt) {
  SelectObject(dc, font);
  SetTextColor(dc, color);
  SetBkMode(dc, TRANSPARENT);
  DrawTextW(dc, s.c_str(), int(s.size()), &r, fmt | DT_NOPREFIX);
}

void Fill(HDC dc, RECT r, COLORREF c) {
  HBRUSH b = CreateSolidBrush(c);
  FillRect(dc, &r, b);
  DeleteObject(b);
}

void Layout() {
  RECT c;
  GetClientRect(g.wnd, &c);
  const int w = c.right;
  const int pad = S(22), bw = S(92), bh = S(38);
  MoveWindow(g.btn_iso, w - pad - S(18) - bw * 2 - S(8), S(138), bw, bh, TRUE);
  MoveWindow(g.btn_folder, w - pad - S(18) - bw, S(138), bw, bh, TRUE);
  MoveWindow(g.btn_out, w - pad - S(18) - bw, S(219), bw, bh, TRUE);
  MoveWindow(g.btn_build, pad, S(342), S(220), S(56), TRUE);
  MoveWindow(g.btn_cancel, pad + S(236), S(350), S(120), S(40), TRUE);
  MoveWindow(g.btn_open, pad + S(236), S(350), S(160), S(40), TRUE);
  MoveWindow(g.log, pad, S(450), w - pad * 2, c.bottom - S(450) - pad, TRUE);
}

void Paint(HDC dc) {
  RECT c;
  GetClientRect(g.wnd, &c);
  Fill(dc, c, kBg);
  const int pad = S(22);
  // header
  Text(dc, L"SUPERMAN RETURNS", {pad, S(18), c.right, S(66)}, g.title, kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
  SIZE ts;
  SelectObject(dc, g.title);
  GetTextExtentPoint32W(dc, L"SUPERMAN RETURNS", 16, &ts);
  Text(dc, W("subtitle"), {pad + ts.cx + S(16), S(24), c.right, S(66)}, g.sub, kAccent,
       DT_LEFT | DT_VCENTER | DT_SINGLELINE);
  // the language buttons, the last at the right edge
  for (int i = kLangCount - 1; i >= 0; --i) {
    const LONG right = i == kLangCount - 1 ? c.right - pad : g.langs[i + 1].left - S(8);
    g.langs[i] = {right - S(28), S(28), right, S(56)};
    Text(dc, kLangLabels[i], g.langs[i], g.label, g_lang == i ? kAccent : kMuted, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  }
  // the panel with the two fields
  Fill(dc, {pad, S(80), c.right - pad, S(322)}, kPanel);
  auto field = [&](int y, const std::wstring& label, const std::wstring& note, COLORREF note_color,
                   const fs::path& value, int right) {
    Text(dc, label, {pad + S(20), y, c.right - pad - S(20), y + S(26)}, g.label, kText,
         DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    Text(dc, note, {pad + S(20), y, c.right - pad - S(20), y + S(26)}, g.note_font, note_color,
         DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    RECT box{pad + S(18), y + S(32), right - S(8), y + S(70)};
    Fill(dc, box, kField);
    box.left += S(12);
    Text(dc, value.empty() ? W("none") : value.wstring(), box, g.body, value.empty() ? kMuted : kText,
         DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_PATH_ELLIPSIS);
  };
  RECT b;
  GetWindowRect(g.btn_iso, &b);
  MapWindowPoints(nullptr, g.wnd, reinterpret_cast<POINT*>(&b), 2);
  field(S(106), W("source"), g.source_note, g.source_color, g.source, b.left);
  GetWindowRect(g.btn_out, &b);
  MapWindowPoints(nullptr, g.wnd, reinterpret_cast<POINT*>(&b), 2);
  field(S(187), W("output"), L"", kMuted, g.output, b.left);
  // the textures switch: the label and a flat track with its knob (accent when on)
  const int ty = S(278);
  Text(dc, W("textures"), {pad + S(20), ty, c.right - pad - S(20), ty + S(26)}, g.label, kText,
       DT_LEFT | DT_VCENTER | DT_SINGLELINE);
  const RECT track{c.right - pad - S(18) - S(52), ty + S(1), c.right - pad - S(18), ty + S(25)};
  Fill(dc, track, g.textures ? kAccent : kField);
  const int knob = S(18), inset = S(3);
  const int kx = g.textures ? track.right - inset - knob : track.left + inset;
  Fill(dc, {kx, track.top + inset, kx + knob, track.top + inset + knob}, g.textures ? kBg : kMuted);
  g.textures_hit = {pad + S(18), ty - S(4), c.right - pad - S(18), ty + S(30)};
  // progress and status
  RECT bar{pad, S(414), c.right - pad, S(424)};
  Fill(dc, bar, kPanel);
  double overall;
  std::wstring status;
  {
    std::lock_guard lock(g.mtx);
    overall = g.building ? Overall(g.step, g.fraction) : (g.finished.empty() ? 0 : 1);
    status = g.status;
  }
  RECT done = bar;
  done.right = bar.left + LONG((bar.right - bar.left) * overall);
  Fill(dc, done, kAccent);
  Text(dc, status, {pad, S(424), c.right - pad, S(448)}, g.body, kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

void DrawButton(const DRAWITEMSTRUCT* d) {
  const bool build = d->CtlID == kIdBuild, disabled = d->itemState & ODS_DISABLED;
  const bool pressed = d->itemState & ODS_SELECTED;
  COLORREF bg = build ? (pressed ? RGB(0xff, 0xe0, 0x66) : kAccent) : (pressed ? kAccent : kField);
  if (d->CtlID == kIdCancel) bg = pressed ? kError : kField;
  if (disabled) bg = kPanel;
  Fill(d->hDC, d->rcItem, bg);
  wchar_t text[64];
  GetWindowTextW(d->hwndItem, text, 64);
  Text(d->hDC, text, d->rcItem, build ? g.big : g.body, disabled ? kMuted : (build ? kBg : kText),
       DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void Relabel() {
  SetWindowTextW(g.btn_iso, L".iso");
  SetWindowTextW(g.btn_folder, W("folder").c_str());
  SetWindowTextW(g.btn_out, W("choose").c_str());
  SetWindowTextW(g.btn_build, W("build").c_str());
  SetWindowTextW(g.btn_cancel, W("cancel").c_str());
  SetWindowTextW(g.btn_open, W("open").c_str());
  static const wchar_t* const kTitles[] = {L"Superman Returns Builder", L"Kompilator Superman Returns", L"Creador de Superman Returns", L"Superman Returns Builder", L"Superman Returns Builder"};
  SetWindowTextW(g.wnd, kTitles[g_lang]);
  EnableWindow(g.btn_build, g.has_pack && g.source_ok && !g.output.empty() && !g.building);
  InvalidateRect(g.wnd, nullptr, TRUE);
}

void Log(const std::wstring& line) {
  const int n = GetWindowTextLengthW(g.log);
  SendMessageW(g.log, EM_SETSEL, n, n);
  SendMessageW(g.log, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>((line + L"\r\n").c_str()));
}

// Checks the chosen game in the background: the window says which release it is.
void CheckSource() {
  g.source_ok = false;
  g.source_note = g.source.empty() ? L"" : W("checking");
  g.source_color = kMuted;
  Relabel();
  if (g.source.empty()) return;
  std::thread([path = g.source] {
    std::string result = "not_game";
    try {
      GameSource game;
      game.Open(path);
      if (game.Find("default.xex", nullptr, nullptr)) {
        const auto xex = game.Read("default.xex");
        Sha256 h;
        h.Update(xex.data(), xex.size());
        result = h.Hex() == g.expected_xex ? "recognized" : "wrong_release";
      }
    } catch (...) {
    }
    PostMessageW(g.wnd, kMsgChecked, 0, reinterpret_cast<LPARAM>(new std::string(result)));
  }).detach();
}

fs::path Pick(bool folder, bool iso) {
  fs::path chosen;
  IFileOpenDialog* dlg = nullptr;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return {};
  DWORD opts = 0;
  dlg->GetOptions(&opts);
  dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | (folder ? FOS_PICKFOLDERS : 0));
  if (iso) {
    const COMDLG_FILTERSPEC spec[] = {{L"Xbox 360 (*.iso)", L"*.iso"}};
    dlg->SetFileTypes(1, spec);
  }
  if (SUCCEEDED(dlg->Show(g.wnd))) {
    IShellItem* item = nullptr;
    if (SUCCEEDED(dlg->GetResult(&item))) {
      PWSTR p = nullptr;
      if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
        chosen = p;
        CoTaskMemFree(p);
      }
      item->Release();
    }
  }
  dlg->Release();
  return chosen;
}

void StartBuild() {
  g.building = true;
  g.cancel = false;
  g.finished.clear();
  ShowWindow(g.btn_open, SW_HIDE);
  ShowWindow(g.btn_cancel, SW_SHOW);
  Relabel();
  SaveSettings();
  Log(W("build") + L": " + g.source.wstring() + L" -> " + g.output.wstring());
  std::thread([] {
    std::wstring result;
    bool ok = false;
    try {
      const fs::path dir = Build(g.pack, {g.source, g.output, g.textures}, g.cancel, [](Step s, double f, const std::string& name) {
        std::lock_guard lock(g.mtx);
        g.step = s;
        g.fraction = f;
        g.status = StepName(s) + (name.empty() ? L"" : L": " + Widen(name));
        PostMessageW(g.wnd, kMsgProgress, 0, 0);
      });
      g.finished = dir;
      result = W("done") + dir.wstring();
      ok = true;
    } catch (const std::exception& e) {
      result = W("failed") + Explain(e.what());
    }
    PostMessageW(g.wnd, kMsgDone, ok, reinterpret_cast<LPARAM>(new std::wstring(result)));
  }).detach();
}

HWND MakeButton(int id) {
  return CreateWindowW(L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 10, 10, g.wnd,
                       reinterpret_cast<HMENU>(INT_PTR(id)), nullptr, nullptr);
}

// The point is on a language button (the hand cursor).
bool OnLanguage(POINT p) {
  for (int i = 0; i < kLangCount; ++i)
    if (PtInRect(&g.langs[i], p)) return true;
  return false;
}

LRESULT CALLBACK WndProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_CREATE: {
      g.wnd = wnd;
      g.dpi = int(GetDpiForWindow(wnd));
      MakeFonts();
      g.btn_iso = MakeButton(kIdIso);
      g.btn_folder = MakeButton(kIdFolder);
      g.btn_out = MakeButton(kIdOut);
      g.btn_build = MakeButton(kIdBuild);
      g.btn_cancel = MakeButton(kIdCancel);
      g.btn_open = MakeButton(kIdOpen);
      ShowWindow(g.btn_cancel, SW_HIDE);
      ShowWindow(g.btn_open, SW_HIDE);
      g.log = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | WS_VSCROLL | ES_AUTOVSCROLL,
                            0, 0, 10, 10, wnd, reinterpret_cast<HMENU>(INT_PTR(kIdLog)), nullptr, nullptr);
      SendMessageW(g.log, WM_SETFONT, WPARAM(g.mono), TRUE);
      Layout();
      Relabel();
      Log(W("note"));
      if (!g.has_pack) Log(W("no_payload"));
      CheckSource();
      return 0;
    }
    case WM_SIZE:
      if (g.btn_iso) Layout();
      InvalidateRect(wnd, nullptr, TRUE);
      return 0;
    case WM_DPICHANGED: {
      g.dpi = HIWORD(wp);
      MakeFonts();
      SendMessageW(g.log, WM_SETFONT, WPARAM(g.mono), TRUE);
      const RECT* r = reinterpret_cast<RECT*>(lp);
      SetWindowPos(wnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER);
      return 0;
    }
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(wnd, &ps);
      RECT c;
      GetClientRect(wnd, &c);
      HDC mem = CreateCompatibleDC(dc);
      HBITMAP bmp = CreateCompatibleBitmap(dc, c.right, c.bottom);
      HGDIOBJ old = SelectObject(mem, bmp);
      Paint(mem);
      BitBlt(dc, 0, 0, c.right, c.bottom, mem, 0, 0, SRCCOPY);
      SelectObject(mem, old);
      DeleteObject(bmp);
      DeleteDC(mem);
      EndPaint(wnd, &ps);
      return 0;
    }
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC: {
      static HBRUSH panel = CreateSolidBrush(kPanel);
      SetTextColor(HDC(wp), kMuted);
      SetBkColor(HDC(wp), kPanel);
      return LRESULT(panel);
    }
    case WM_DRAWITEM:
      DrawButton(reinterpret_cast<const DRAWITEMSTRUCT*>(lp));
      return TRUE;
    case WM_LBUTTONDOWN: {
      const POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
      int lang = -1;
      for (int i = 0; i < kLangCount; ++i)
        if (PtInRect(&g.langs[i], p)) lang = i;
      if (!g.building && lang >= 0) {
        g_lang = lang;
        SaveSettings();
        Relabel();
        if (!g.source.empty()) CheckSource();
      } else if (!g.building && PtInRect(&g.textures_hit, p)) {
        g.textures = !g.textures;
        SaveSettings();
        InvalidateRect(wnd, nullptr, FALSE);
      }
      return 0;
    }
    case WM_SETCURSOR: {
      POINT p;
      GetCursorPos(&p);
      ScreenToClient(wnd, &p);
      if (LOWORD(lp) == HTCLIENT && (OnLanguage(p) || PtInRect(&g.textures_hit, p))) {
        SetCursor(LoadCursor(nullptr, IDC_HAND));
        return TRUE;
      }
      break;
    }
    case WM_COMMAND:
      switch (LOWORD(wp)) {
        case kIdIso:
        case kIdFolder: {
          fs::path p = Pick(LOWORD(wp) == kIdFolder, LOWORD(wp) == kIdIso);
          if (!p.empty()) {
            g.source = p;
            SaveSettings();
            CheckSource();
          }
          break;
        }
        case kIdOut: {
          fs::path p = Pick(true, false);
          if (!p.empty()) {
            g.output = p;
            SaveSettings();
            Relabel();
          }
          break;
        }
        case kIdBuild:
          StartBuild();
          break;
        case kIdCancel:
          g.cancel = true;
          break;
        case kIdOpen:
          ShellExecuteW(nullptr, L"open", g.finished.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
          break;
      }
      return 0;
    case kMsgProgress:
      InvalidateRect(wnd, nullptr, FALSE);
      return 0;
    case kMsgChecked: {
      std::unique_ptr<std::string> r(reinterpret_cast<std::string*>(lp));
      g.source_ok = *r == "recognized";
      g.source_note = W(*r);
      g.source_color = g.source_ok ? kOk : kError;
      Relabel();
      return 0;
    }
    case kMsgDone: {
      std::unique_ptr<std::wstring> r(reinterpret_cast<std::wstring*>(lp));
      g.building = false;
      {
        std::lock_guard lock(g.mtx);
        g.status = *r;
      }
      Log(*r);
      ShowWindow(g.btn_cancel, SW_HIDE);
      if (wp) ShowWindow(g.btn_open, SW_SHOW);
      Relabel();
      if (g.close_after) DestroyWindow(wnd);
      return 0;
    }
    case WM_CLOSE:
      if (g.building) {
        g.cancel = true;
        g.close_after = true;
        return 0;  // closes when the build has stopped
      }
      break;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(wnd, msg, wp, lp);
}

}  // namespace

int Run(HINSTANCE inst, int show);

// A program without a console dies silently: what stopped it goes to builder_crash.log.
int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
  try {
    return Run(inst, show);
  } catch (const std::exception& e) {
    std::ofstream(ExePath().parent_path() / L"builder_crash.log") << e.what() << std::endl;
    if (!wcsstr(GetCommandLineW(), L"--build"))  // no window to block a script
      MessageBoxW(nullptr, Widen(e.what()).c_str(), L"Superman Returns Builder", MB_ICONERROR);
    return 3;
  }
}

int Run(HINSTANCE inst, int show) {
  int argc = 0;
  wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  g.has_pack = g.pack.Open(ExePath());
  if (g.has_pack) g.expected_xex = ExpectedXex(g.pack);

  // window-less: --build --src X --out Y [--log F]
  bool build = false;
  BuildOptions options;
  fs::path log;
  for (int i = 1; i < argc; ++i) {
    const std::wstring a = argv[i];
    if (a == L"--build") build = true;
    if (a == L"--src" && i + 1 < argc) options.source = argv[++i];
    if (a == L"--out" && i + 1 < argc) options.output = argv[++i];
    if (a == L"--no-textures") options.textures = false;
    if (a == L"--log" && i + 1 < argc) log = argv[++i];
  }
  if (build) {
    if (!g.has_pack) return 2;
    return RunBuild(g.pack, options, log);
  }

  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  LoadSettings();
  WNDCLASSW wc{};
  wc.lpfnWndProc = WndProc;
  wc.hInstance = inst;
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.hIcon = LoadIconW(inst, L"APP");
  wc.lpszClassName = L"SupermanReturnsBuilder";
  RegisterClassW(&wc);
  const UINT dpi = GetDpiForSystem();
  HWND wnd = CreateWindowExW(0, wc.lpszClassName, L"Superman Returns Builder", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                             CW_USEDEFAULT, MulDiv(860, int(dpi), 96), MulDiv(710, int(dpi), 96), nullptr, nullptr,
                             inst, nullptr);
  ShowWindow(wnd, show);
  MSG m;
  while (GetMessageW(&m, nullptr, 0, 0)) {
    TranslateMessage(&m);
    DispatchMessageW(&m);
  }
  return 0;
}
