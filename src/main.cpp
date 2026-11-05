// ClearMic: main window + system tray front end.
//
// The window is native Win32 (no web view) to keep memory use low.
// Closing the window hides it; ClearMic keeps running in the tray so the
// virtual microphone stays live. Quit from the window or the tray menu.

#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <commctrl.h>
#include <string.h>
#include <string>
#include <vector>

#include "engine.h"

namespace {

const wchar_t kAppName[] = L"ClearMic";
const wchar_t kWndClass[] = L"ClearMicMainWindow";
const wchar_t kRegKey[] = L"Software\\ClearMic";
const wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const wchar_t kCableUrl[] = L"https://vb-audio.com/Cable/";
const wchar_t kCableMicName[] = L"CABLE Output (VB-Audio Virtual Cable)";

constexpr UINT WM_TRAY = WM_APP + 1;
constexpr UINT WM_ENGINE = WM_APP + 2;
constexpr UINT WM_SHOWME = WM_APP + 3;     // sent by a second launch
constexpr UINT_PTR kMeterTimer = 1;

enum : UINT {
    IDC_TOGGLE = 10,
    IDC_MIC,
    IDC_FOCUS,
    IDC_OUT,
    IDC_AUTOSTART,
    IDC_INSTALL,
    IDC_QUIT,
    IDC_LEVEL,
    IDC_VOLUME,
    IDC_MODEL,
    IDC_TEACH,
    IDC_FORGET,
    IDC_ENHANCE,
    ID_TRAY_OPEN = 200,
    ID_TRAY_TOGGLE,
    ID_TRAY_EXIT,
};

constexpr int kFocusCount = 6;
const wchar_t* const kFocusNames[kFocusCount] = {
    L"Off - keep all voices",
    L"Normal - block voices a few metres away",
    L"Strict - block voices nearby",
    L"Very strict - mic close to your mouth",
    L"Manual - I choose the level",
    L"My voice only - AI voice match",
};

// Manual gate level slider range (dBFS) and volume range (%).
constexpr int kLevelMin = -60, kLevelMax = -5;
constexpr int kVolMax = 200;

// Layout (in 96-dpi pixels; scaled with S()).
constexpr int kW = 460, kH = 704, kM = 24;
constexpr int yToggle = 84, hToggle = 60;
constexpr int yMicLbl = 156, yMic = 176;
constexpr int yModel = 216, kModelLabelW = 96;
constexpr int yEnhance = 248;
constexpr int yFocLbl = 286, yFoc = 306;
constexpr int yMeter = 346, hMeter = 104;
constexpr int yLevel = 462, yVolume = 496, hRow = 30, kRowLabelW = 168;
constexpr int yOutLbl = 536, yOut = 556;
constexpr int yAuto = 596;
constexpr int yInfo = 632, hInfo = 56;

const wchar_t* const kModelNames[2] = {
    L"Standard - RNNoise (lightest)",
    L"High quality - DeepFilterNet3",
};
// Clean-signal bar inside the meter card (click/drag here to set the manual level).
constexpr int yCleanBar = yMeter + 64, hBar = 10;

// Colours
const COLORREF cBg = RGB(246, 247, 249);
const COLORREF cCard = RGB(255, 255, 255);
const COLORREF cBorder = RGB(224, 227, 232);
const COLORREF cText = RGB(28, 30, 33);
const COLORREF cMuted = RGB(102, 109, 119);
const COLORREF cGreen = RGB(30, 158, 90);
const COLORREF cGreenDark = RGB(20, 118, 66);
const COLORREF cGrey = RGB(140, 146, 154);
const COLORREF cGreyDark = RGB(112, 118, 126);
const COLORREF cAmber = RGB(204, 112, 0);
const COLORREF cTrack = RGB(230, 233, 237);
const COLORREF cBlue = RGB(66, 118, 204);
const COLORREF cInfo = RGB(234, 242, 255);
const COLORREF cWarn = RGB(255, 243, 226);

// State
Engine g_engine;
HINSTANCE g_inst = nullptr;
HWND g_wnd = nullptr;
HWND g_toggle, g_mic, g_focusCb, g_out, g_auto, g_install, g_quit, g_level, g_volume, g_model, g_teach, g_forget;
HWND g_enhance;
bool g_enhanceOn = false;
bool g_enrolling = false;  // UI is showing an enrollment in progress
int g_matchPct = -1;       // voice-match threshold set by the user (%), -1 = use the calibrated value
UINT g_taskbarCreated = 0;
int g_dpi = 96;
HFONT g_fText, g_fBold, g_fTitle, g_fSmall, g_fBig;
HBRUSH g_bgBrush, g_infoBrush, g_warnBrush;
HICON g_iconOn, g_iconOff, g_iconErr, g_iconBig, g_iconSmall;

std::wstring g_inputId, g_outputId;
bool g_denoise = true;
int g_focus = 1;
int g_modelSel = 0;        // 0 = RNNoise, 1 = DeepFilterNet3
int g_manualDb = -35;      // manual gate level, dBFS
int g_volumePct = 100;     // output volume, %
bool g_dragging = false;   // dragging the level marker on the meter
std::vector<AudioDevice> g_micList, g_outList;
bool g_cablePresent = false;
bool g_hideHintShown = false;
EngineState g_lastState = EngineState::Stopped;

float g_dispIn = -100, g_dispOut = -100, g_gateDb = -100;
bool g_gateOpen = false;

int S(int v) { return MulDiv(v, g_dpi, 96); }

// ---------------------------------------------------------------------------
// Settings (HKCU\Software\ClearMic)

std::wstring RegReadString(const wchar_t* name) {
    DWORD bytes = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, kRegKey, name, RRF_RT_REG_SZ, nullptr, nullptr, &bytes) != ERROR_SUCCESS)
        return L"";
    std::wstring s(bytes / sizeof(wchar_t), L'\0');
    if (RegGetValueW(HKEY_CURRENT_USER, kRegKey, name, RRF_RT_REG_SZ, nullptr, &s[0], &bytes) != ERROR_SUCCESS)
        return L"";
    s.resize(lstrlenW(s.c_str()));
    return s;
}

DWORD RegReadDword(const wchar_t* name, DWORD def) {
    DWORD v = 0, bytes = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, kRegKey, name, RRF_RT_REG_DWORD, nullptr, &v, &bytes) != ERROR_SUCCESS)
        return def;
    return v;
}

void SaveSettings() {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS)
        return;
    RegSetValueExW(k, L"InputId", 0, REG_SZ, reinterpret_cast<const BYTE*>(g_inputId.c_str()),
                   DWORD((g_inputId.size() + 1) * sizeof(wchar_t)));
    RegSetValueExW(k, L"OutputId", 0, REG_SZ, reinterpret_cast<const BYTE*>(g_outputId.c_str()),
                   DWORD((g_outputId.size() + 1) * sizeof(wchar_t)));
    DWORD d = g_denoise ? 1 : 0;
    RegSetValueExW(k, L"Denoise", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&d), sizeof(d));
    DWORD f = DWORD(g_focus);
    RegSetValueExW(k, L"VoiceFocus", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&f), sizeof(f));
    DWORD lv = DWORD(-g_manualDb);   // stored as a positive number
    RegSetValueExW(k, L"ManualLevelNeg", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&lv), sizeof(lv));
    DWORD mdl = DWORD(g_modelSel);
    RegSetValueExW(k, L"Model", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&mdl), sizeof(mdl));
    DWORD en = g_enhanceOn ? 1 : 0;
    RegSetValueExW(k, L"Enhance", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&en), sizeof(en));
    DWORD mp = DWORD(g_matchPct);   // 0xFFFFFFFF = automatic
    RegSetValueExW(k, L"MatchPercent", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&mp), sizeof(mp));
    DWORD vol = DWORD(g_volumePct);
    RegSetValueExW(k, L"VolumePercent", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&vol), sizeof(vol));
    RegCloseKey(k);
}

bool IsAutostart() {
    return RegGetValueW(HKEY_CURRENT_USER, kRunKey, kAppName, RRF_RT_REG_SZ, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
}

void SetAutostart(bool on) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    if (on) {
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        std::wstring cmd = L"\"" + std::wstring(path) + L"\" --tray";
        RegSetValueExW(k, kAppName, 0, REG_SZ, reinterpret_cast<const BYTE*>(cmd.c_str()),
                       DWORD((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, kAppName);
    }
    RegCloseKey(k);
}

// ---------------------------------------------------------------------------
// Icons (drawn at runtime: no resource files needed)

HICON MakeIcon(COLORREF bg, bool slash, int s) {
    auto v = [s](int n) { return n * s / 16; };

    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    HBITMAP color = CreateCompatibleBitmap(screen, s, s);
    HBITMAP mask = CreateBitmap(s, s, 1, 1, nullptr);
    ReleaseDC(nullptr, screen);

    // Mask: white = transparent, black = opaque disc.
    HGDIOBJ oldBmp = SelectObject(dc, mask);
    PatBlt(dc, 0, 0, s, s, WHITENESS);
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(BLACK_BRUSH));
    HGDIOBJ oldPen = SelectObject(dc, GetStockObject(BLACK_PEN));
    Ellipse(dc, 0, 0, s, s);

    // Colour: black outside the disc so the XOR leaves the background untouched.
    SelectObject(dc, color);
    PatBlt(dc, 0, 0, s, s, BLACKNESS);
    HBRUSH bgBrush = CreateSolidBrush(bg);
    HPEN bgPen = CreatePen(PS_SOLID, 1, bg);
    SelectObject(dc, bgBrush);
    SelectObject(dc, bgPen);
    Ellipse(dc, 0, 0, s, s);

    // Microphone glyph.
    SelectObject(dc, GetStockObject(WHITE_BRUSH));
    SelectObject(dc, GetStockObject(WHITE_PEN));
    RoundRect(dc, v(6), v(3), v(10), v(10), v(4), v(4));
    Rectangle(dc, v(7) + v(1) / 2, v(10), v(9) - v(1) / 2, v(12));
    Rectangle(dc, v(5), v(12), v(11), v(13));

    HPEN slashPen = nullptr;
    if (slash) {
        slashPen = CreatePen(PS_SOLID, v(2) > 1 ? v(2) : 2, RGB(255, 255, 255));
        SelectObject(dc, slashPen);
        MoveToEx(dc, v(3), v(3), nullptr);
        LineTo(dc, v(13), v(13));
    }

    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldBmp);
    DeleteObject(bgBrush);
    DeleteObject(bgPen);
    if (slashPen) DeleteObject(slashPen);
    DeleteDC(dc);

    ICONINFO ii = {};
    ii.fIcon = TRUE;
    ii.hbmMask = mask;
    ii.hbmColor = color;
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(mask);
    DeleteObject(color);
    return icon;
}

// ---------------------------------------------------------------------------
// Tray

std::wstring TrayText(const EngineStatus& s) {
    switch (s.state) {
    case EngineState::Running: return g_denoise ? L"Noise cancellation ON" : L"Noise cancellation OFF";
    case EngineState::Error: return s.message;
    case EngineState::Starting: return L"Starting...";
    case EngineState::Stopped: return L"Stopped";
    }
    return L"";
}

void FillTray(NOTIFYICONDATAW& nid) {
    EngineStatus s = g_engine.Status();
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_wnd;
    nid.uID = 1;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon = s.state == EngineState::Error ? g_iconErr : (g_denoise ? g_iconOn : g_iconOff);
    std::wstring tip = std::wstring(kAppName) + L" - " + TrayText(s);
    lstrcpynW(nid.szTip, tip.c_str(), ARRAYSIZE(nid.szTip));
}

void TrayAdd() { NOTIFYICONDATAW nid = {}; FillTray(nid); Shell_NotifyIconW(NIM_ADD, &nid); }
void TrayUpdate() { NOTIFYICONDATAW nid = {}; FillTray(nid); Shell_NotifyIconW(NIM_MODIFY, &nid); }

void TrayRemove() {
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_wnd;
    nid.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

void TrayBalloon(const std::wstring& title, const std::wstring& text, DWORD kind) {
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_wnd;
    nid.uID = 1;
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = kind;
    lstrcpynW(nid.szInfoTitle, title.c_str(), ARRAYSIZE(nid.szInfoTitle));
    lstrcpynW(nid.szInfo, text.c_str(), ARRAYSIZE(nid.szInfo));
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

// ---------------------------------------------------------------------------
// Actions

void ApplyDevices() {
    SaveSettings();
    g_engine.SetDevices(g_inputId, g_outputId);
}

bool ManualMode() { return g_denoise && g_focus == Engine::kFocusManual; }
bool MatchMode() { return g_denoise && g_focus == Engine::kFocusVoiceMatch; }

int MatchPct() {
    return g_matchPct >= 0 ? g_matchPct : int(g_engine.SuggestedThreshold() * 100.0f + 0.5f);
}

// The level slider doubles as the voice-match threshold (0-100 %) in voice-match mode.
void UpdateLevelUi() {
    EnableWindow(g_level, ManualMode() || MatchMode());
    if (MatchMode()) {
        SendMessageW(g_level, TBM_SETRANGEMAX, FALSE, 100);
        SendMessageW(g_level, TBM_SETPOS, TRUE, MatchPct());
    } else {
        SendMessageW(g_level, TBM_SETRANGEMAX, FALSE, kLevelMax - kLevelMin);
        if (ManualMode()) SendMessageW(g_level, TBM_SETPOS, TRUE, g_manualDb - kLevelMin);
    }
    InvalidateRect(g_wnd, nullptr, FALSE);
}

void UpdateVoiceButtons() {
    const bool installed = Engine::VoiceMatchInstalled();
    ShowWindow(g_teach, installed ? SW_SHOW : SW_HIDE);
    ShowWindow(g_forget, installed && g_engine.HasVoiceprint() && !g_enrolling ? SW_SHOW : SW_HIDE);
    SetWindowTextW(g_teach, g_enrolling ? L"Cancel recording"
                                        : (g_engine.HasVoiceprint() ? L"Re-teach my voice" : L"Teach my voice"));
}

void SetManualDb(int db) {
    db = db < kLevelMin ? kLevelMin : (db > kLevelMax ? kLevelMax : db);
    g_manualDb = db;
    g_engine.SetManualLevel(float(db));
    SendMessageW(g_level, TBM_SETPOS, TRUE, db - kLevelMin);
    InvalidateRect(g_wnd, nullptr, FALSE);
}

void SetDenoiseUi(bool on) {
    g_denoise = on;
    g_engine.SetDenoise(on);
    SaveSettings();
    TrayUpdate();
    EnableWindow(g_focusCb, on);
    EnableWindow(g_model, on);
    UpdateLevelUi();
    InvalidateRect(g_toggle, nullptr, FALSE);
}

// Clickable area around the "clean" meter bar (manual level is set by clicking/dragging here).
RECT CleanBarRect() {
    return {S(kM) + S(16), S(yCleanBar) - S(8), S(kW - kM) - S(16), S(yCleanBar + hBar) + S(8)};
}

int DbFromX(int x) {
    RECT r = CleanBarRect();
    float f = float(x - r.left) / float(r.right - r.left);
    return int(-60.0f + 60.0f * f + (f >= 0 ? 0.5f : -0.5f));
}

void FillMicCombo() {
    g_micList.clear();
    for (auto& d : ListDevices(true))
        if (!IsVirtualCableCapture(d.name)) g_micList.push_back(d);
    SendMessageW(g_mic, CB_RESETCONTENT, 0, 0);
    SendMessageW(g_mic, CB_ADDSTRING, 0, (LPARAM)L"Windows default microphone");
    int sel = 0;
    for (size_t i = 0; i < g_micList.size(); ++i) {
        SendMessageW(g_mic, CB_ADDSTRING, 0, (LPARAM)g_micList[i].name.c_str());
        if (g_micList[i].id == g_inputId) sel = int(i) + 1;
    }
    SendMessageW(g_mic, CB_SETCURSEL, sel, 0);
}

void FillOutCombo() {
    g_outList = ListDevices(false);
    SendMessageW(g_out, CB_RESETCONTENT, 0, 0);
    SendMessageW(g_out, CB_ADDSTRING, 0, (LPARAM)L"Auto - VB-CABLE (recommended)");
    int sel = 0;
    for (size_t i = 0; i < g_outList.size(); ++i) {
        SendMessageW(g_out, CB_ADDSTRING, 0, (LPARAM)g_outList[i].name.c_str());
        if (g_outList[i].id == g_outputId) sel = int(i) + 1;
    }
    SendMessageW(g_out, CB_SETCURSEL, sel, 0);
}

void RefreshCable() {
    g_cablePresent = !FindVirtualCableRender().id.empty();
    ShowWindow(g_install, g_cablePresent ? SW_HIDE : SW_SHOW);
}

void ShowMain() {
    if (IsIconic(g_wnd)) ShowWindow(g_wnd, SW_RESTORE);
    ShowWindow(g_wnd, SW_SHOW);
    SetForegroundWindow(g_wnd);
    SetTimer(g_wnd, kMeterTimer, 50, nullptr);
}

void HideMain() {
    ShowWindow(g_wnd, SW_HIDE);
    KillTimer(g_wnd, kMeterTimer);
    if (!g_hideHintShown) {
        g_hideHintShown = true;
        TrayBalloon(L"ClearMic is still running",
                    L"The virtual microphone keeps working. Click the tray icon to open ClearMic again.", NIIF_INFO);
    }
}

void ShowTrayMenu() {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, ID_TRAY_OPEN, L"Open ClearMic");
    AppendMenuW(m, MF_STRING | (g_denoise ? MF_CHECKED : 0), ID_TRAY_TOGGLE, L"Noise cancellation");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_TRAY_EXIT, L"Quit ClearMic");
    SetMenuDefaultItem(m, ID_TRAY_OPEN, FALSE);

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_wnd);
    UINT cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, g_wnd, nullptr);
    PostMessageW(g_wnd, WM_NULL, 0, 0);
    DestroyMenu(m);

    if (cmd == ID_TRAY_OPEN) ShowMain();
    else if (cmd == ID_TRAY_TOGGLE) SetDenoiseUi(!g_denoise);
    else if (cmd == ID_TRAY_EXIT) DestroyWindow(g_wnd);
}

// ---------------------------------------------------------------------------
// Fonts and layout

HFONT MakeFont(int pt, int weight) {
    return CreateFontW(-MulDiv(pt, g_dpi, 72), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
}

void CreateFonts() {
    for (HFONT f : {g_fText, g_fBold, g_fTitle, g_fSmall, g_fBig})
        if (f) DeleteObject(f);
    g_fText = MakeFont(10, FW_NORMAL);
    g_fBold = MakeFont(10, FW_SEMIBOLD);
    g_fTitle = MakeFont(18, FW_SEMIBOLD);
    g_fSmall = MakeFont(9, FW_NORMAL);
    g_fBig = MakeFont(13, FW_SEMIBOLD);
}

void Layout() {
    const int w = S(kW) - 2 * S(kM);
    const int dropH = S(320);   // height including the open drop-down list
    MoveWindow(g_toggle, S(kM), S(yToggle), w, S(hToggle), TRUE);
    MoveWindow(g_mic, S(kM), S(yMic), w, dropH, TRUE);
    MoveWindow(g_model, S(kM + kModelLabelW), S(yModel), S(kW - kM) - S(kM + kModelLabelW), dropH, TRUE);
    MoveWindow(g_enhance, S(kM + kModelLabelW), S(yEnhance), S(kW - kM) - S(kM + kModelLabelW), S(26), TRUE);
    MoveWindow(g_focusCb, S(kM), S(yFoc), w, dropH, TRUE);
    MoveWindow(g_teach, S(kW - kM - 70 - 6 - 130), S(yFocLbl - 4), S(130), S(24), TRUE);
    MoveWindow(g_forget, S(kW - kM - 70), S(yFocLbl - 4), S(70), S(24), TRUE);
    MoveWindow(g_out, S(kM), S(yOut), w, dropH, TRUE);
    const int tl = S(kM + kRowLabelW), tw = S(kW - kM) - tl;
    MoveWindow(g_level, tl, S(yLevel), tw, S(hRow), TRUE);
    MoveWindow(g_volume, tl, S(yVolume), tw, S(hRow), TRUE);
    MoveWindow(g_auto, S(kM), S(yAuto), w - S(130), S(28), TRUE);
    MoveWindow(g_quit, S(kW - kM - 120), S(yAuto), S(120), S(28), TRUE);
    MoveWindow(g_install, S(kW - kM - 12 - 150), S(yInfo + 13), S(150), S(30), TRUE);
    for (HWND h : {g_mic, g_model, g_focusCb, g_out, g_auto, g_install, g_quit, g_teach, g_forget, g_enhance})
        SendMessageW(h, WM_SETFONT, (WPARAM)g_fText, TRUE);
}

void SizeWindow(bool center) {
    RECT r = {0, 0, S(kW), S(kH)};
    const DWORD style = DWORD(GetWindowLongPtrW(g_wnd, GWL_STYLE));
    AdjustWindowRectExForDpi(&r, style, FALSE, 0, g_dpi);
    int w = r.right - r.left, h = r.bottom - r.top;
    if (center) {
        MONITORINFO mi = {sizeof(mi)};
        GetMonitorInfoW(MonitorFromWindow(g_wnd, MONITOR_DEFAULTTOPRIMARY), &mi);
        int x = mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - w) / 2;
        int y = mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - h) / 2;
        SetWindowPos(g_wnd, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    } else {
        SetWindowPos(g_wnd, nullptr, 0, 0, w, h, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE);
    }
}

// ---------------------------------------------------------------------------
// Painting

void RoundFill(HDC dc, int l, int t, int r, int b, int rad, COLORREF fill, COLORREF border) {
    HBRUSH br = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ ob = SelectObject(dc, br), op = SelectObject(dc, pen);
    RoundRect(dc, l, t, r, b, rad, rad);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(br);
    DeleteObject(pen);
}

void Text(HDC dc, HFONT f, COLORREF c, const std::wstring& s, int l, int t, int r, int b,
          UINT flags = DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX) {
    HGDIOBJ of = SelectObject(dc, f);
    SetTextColor(dc, c);
    RECT rc = {l, t, r, b};
    DrawTextW(dc, s.c_str(), -1, &rc, flags);
    SelectObject(dc, of);
}

float Frac(float db) {
    float f = (db + 60.0f) / 60.0f;   // show -60 .. 0 dBFS
    return f < 0 ? 0 : (f > 1 ? 1 : f);
}

void DrawBar(HDC dc, int l, int t, int r, int h, float db, COLORREF color, bool marker, float markerDb) {
    RoundFill(dc, l, t, r, t + h, h, cTrack, cTrack);
    int x = l + int((r - l) * Frac(db));
    if (x > l + h / 2) RoundFill(dc, l, t, x, t + h, h, color, color);
    if (marker) {
        int mx = l + int((r - l) * Frac(markerDb));
        RoundFill(dc, mx - S(1), t - S(4), mx + S(1), t + h + S(4), 0, cText, cText);
    }
}

void Paint(HDC dc) {
    const EngineStatus s = g_engine.Status();
    const bool running = s.state == EngineState::Running;
    const int L = S(kM), R = S(kW - kM);
    SetBkMode(dc, TRANSPARENT);

    // Header: title + status pill
    Text(dc, g_fTitle, cText, kAppName, L, S(14), R, S(50));
    std::wstring pill;
    COLORREF pc;
    if (s.state == EngineState::Error) { pill = L"Needs attention"; pc = cAmber; }
    else if (running) { pill = g_denoise ? L"Active" : L"Pass-through"; pc = g_denoise ? cGreen : cGrey; }
    else { pill = L"Starting"; pc = cGrey; }
    HGDIOBJ of = SelectObject(dc, g_fBold);
    SIZE ts;
    GetTextExtentPoint32W(dc, pill.c_str(), int(pill.size()), &ts);
    SelectObject(dc, of);
    const int pw = ts.cx + S(24);
    RoundFill(dc, R - pw, S(20), R, S(46), S(26), pc, pc);
    Text(dc, g_fBold, RGB(255, 255, 255), pill, R - pw, S(20), R, S(46), DT_CENTER | DT_SINGLELINE | DT_VCENTER);

    // Status line (warnings such as a model that failed to load are shown in amber)
    if (s.state == EngineState::Error || (running && s.message != L"Running"))
        Text(dc, g_fSmall, cAmber, s.message, L, S(52), R, S(76));
    else if (running)
        Text(dc, g_fSmall, cMuted, L"Listening to: " + s.inputName, L, S(52), R, S(76));
    else
        Text(dc, g_fSmall, cMuted, s.message, L, S(52), R, S(76));

    // Section labels
    Text(dc, g_fBold, cText, L"Microphone", L, S(yMicLbl), R, S(yMicLbl + 20));
    Text(dc, g_fBold, g_denoise ? cText : cGrey, L"AI model", L, S(yModel), S(kM + kModelLabelW - 6),
         S(yModel + 26));
    Text(dc, g_fBold, cText, L"Quality", L, S(yEnhance), S(kM + kModelLabelW - 6), S(yEnhance + 26));
    Text(dc, g_fBold, g_denoise ? cText : cGrey,
         Engine::VoiceMatchInstalled() ? L"Voice focus" : L"Voice focus (block other people's voices)", L,
         S(yFocLbl), Engine::VoiceMatchInstalled() ? S(kW - kM - 212) : R, S(yFocLbl + 20));
    Text(dc, g_fBold, cText, L"Send clean audio to", L, S(yOutLbl), R, S(yOutLbl + 20));

    // Meter card
    const int mt = S(yMeter), mb = S(yMeter + hMeter);
    RoundFill(dc, L, mt, R, mb, S(12), cCard, cBorder);
    const int il = L + S(16), ir = R - S(16);
    const bool gating = running && g_denoise && g_focus > 0;
    Text(dc, g_fSmall, cMuted, L"Microphone (raw)", il, mt + S(8), ir, mt + S(26));
    DrawBar(dc, il, mt + S(28), ir, S(10), running ? g_dispIn : -100, cBlue, false, 0);
    Text(dc, g_fSmall, cMuted, L"Sent to the meeting (clean)", il, mt + S(44), ir, mt + S(62));
    DrawBar(dc, il, mt + S(64), ir, S(10), running ? g_dispOut : -100,
            gating && !g_gateOpen ? cGrey : cGreen, gating || ManualMode(),
            ManualMode() ? float(g_manualDb) : g_gateDb);

    std::wstring hint;
    COLORREF hc = cMuted;
    const bool manual = ManualMode();
    const float ep = g_engine.EnrollProgress();
    wchar_t mbuf[160];
    if (g_enrolling && ep >= 0.0f && ep < 1.0f) {
        wsprintfW(mbuf, L"Recording your voice: %d%%. Keep talking normally (pauses don't count).", int(ep * 100));
        hint = mbuf;
        hc = cBlue;
    } else if (!running) hint = L"Waiting for audio...";
    else if (g_denoise && MatchMode() && !g_engine.HasVoiceprint()) {
        hint = L"No voiceprint yet: click \"Teach my voice\" first.";
        hc = cAmber;
    } else if (g_denoise && MatchMode()) {
        const float sc = g_engine.MatchScore();
        if (sc < -0.5f) hint = L"Voice match: waiting for speech...";
        else {
            const bool me = g_engine.MatchIsUser();
            wsprintfW(mbuf, L"Voice match %d%%: %s", int(sc * 100), me ? L"you - passing" : L"not you - muted");
            hint = mbuf;
            if (me && g_gateOpen) hc = cGreenDark;
        }
    } else if (!running) hint = L"Waiting for audio...";
    else if (!g_denoise) hint = L"Noise cancellation is off: raw audio is sent.";
    else if (g_focus == 0) hint = L"Background noise removed. All voices pass (voice focus off).";
    else if (manual && g_gateOpen) { hint = L"Louder than your level: passing."; hc = cGreenDark; }
    else if (manual) hint = L"Quieter than your level: muted. Drag the black line to change.";
    else if (g_gateOpen) { hint = L"Your voice is passing through."; hc = cGreenDark; }
    else hint = L"Muted: silence, noise or a distant voice.";
    Text(dc, g_fSmall, hc, hint, il, mt + S(82), ir, mt + S(102));

    // Slider rows: gate level and output volume
    wchar_t buf[64];
    COLORREF lc = cText;
    if (MatchMode()) wsprintfW(buf, L"Match above: %d%%", MatchPct());
    else if (manual) wsprintfW(buf, L"Pass above: %d dB", g_manualDb);
    else if (gating) wsprintfW(buf, L"Level: auto (%d dB)", int(g_gateDb));
    else { lstrcpyW(buf, L"Level: off"); lc = cGrey; }
    Text(dc, g_fBold, lc, buf, L, S(yLevel), S(kM + kRowLabelW - 6), S(yLevel + hRow));
    wsprintfW(buf, L"Volume: %d%%", g_volumePct);
    Text(dc, g_fBold, cText, buf, L, S(yVolume), S(kM + kRowLabelW - 6), S(yVolume + hRow));

    // Info card: what to select in meeting apps, or how to fix a missing cable
    const int it = S(yInfo), ib = S(yInfo + hInfo);
    if (g_cablePresent) {
        RoundFill(dc, L, it, R, ib, S(12), cInfo, cInfo);
        Text(dc, g_fSmall, cMuted, L"In Google Meet, Zoom, Teams or OBS, choose this microphone:", L + S(14),
             it + S(8), R - S(14), it + S(28));
        Text(dc, g_fBold, cText, kCableMicName, L + S(14), it + S(28), R - S(14), it + S(50));
    } else {
        RoundFill(dc, L, it, R, ib, S(12), cWarn, cWarn);
        const int tr = S(kW - kM - 12 - 150 - 8);
        Text(dc, g_fBold, cAmber, L"Virtual mic driver missing", L + S(14), it + S(8), tr, it + S(28));
        Text(dc, g_fSmall, cMuted, L"Install VB-CABLE, then restart Windows.", L + S(14), it + S(28), tr,
             it + S(50));
    }
}

void DrawToggle(const DRAWITEMSTRUCT* d) {
    HDC dc = d->hDC;
    RECT r = d->rcItem;
    FillRect(dc, &r, g_bgBrush);
    const bool pressed = (d->itemState & ODS_SELECTED) != 0;
    COLORREF c = g_denoise ? (pressed ? cGreenDark : cGreen) : (pressed ? cGreyDark : cGrey);
    RoundFill(dc, r.left, r.top, r.right, r.bottom, S(16), c, c);
    SetBkMode(dc, TRANSPARENT);

    const int pad = S(18);
    Text(dc, g_fBig, RGB(255, 255, 255), g_denoise ? L"Noise cancellation: ON" : L"Noise cancellation: OFF",
         r.left + pad, r.top + S(10), r.right - S(90), r.top + S(36));
    Text(dc, g_fSmall, RGB(255, 255, 255),
         g_denoise ? L"Background noise is being removed. Click to turn off." : L"Your mic is sent unchanged. Click to turn on.",
         r.left + pad, r.top + S(36), r.right - S(90), r.top + S(56));

    // Switch graphic
    const int sw = S(52), sh = S(28);
    const int sx = r.right - pad - sw, sy = (r.top + r.bottom - sh) / 2;
    COLORREF track = g_denoise ? cGreenDark : cGreyDark;
    RoundFill(dc, sx, sy, sx + sw, sy + sh, sh, track, RGB(255, 255, 255));
    const int k = sh - S(8);
    const int kx = g_denoise ? sx + sw - S(4) - k : sx + S(4);
    RoundFill(dc, kx, sy + S(4), kx + k, sy + S(4) + k, k, RGB(255, 255, 255), RGB(255, 255, 255));

    if ((d->itemState & ODS_FOCUS) && !(d->itemState & ODS_NOFOCUSRECT)) {
        RECT f = {r.left + S(4), r.top + S(4), r.right - S(4), r.bottom - S(4)};
        DrawFocusRect(dc, &f);
    }
}

// ---------------------------------------------------------------------------
// Window procedure

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g_taskbarCreated && g_taskbarCreated != 0) {   // Explorer restarted
        TrayAdd();
        return 0;
    }
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        HDC mem = CreateCompatibleDC(hdc);
        HBITMAP bmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
        HGDIOBJ old = SelectObject(mem, bmp);
        FillRect(mem, &rc, g_bgBrush);
        Paint(mem);
        BitBlt(hdc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_DRAWITEM:
        if (wp == IDC_TOGGLE) { DrawToggle(reinterpret_cast<DRAWITEMSTRUCT*>(lp)); return TRUE; }
        break;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
        HDC dc = reinterpret_cast<HDC>(wp);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, cText);
        if (reinterpret_cast<HWND>(lp) == g_install) return reinterpret_cast<LRESULT>(g_warnBrush);
        return reinterpret_cast<LRESULT>(g_bgBrush);
    }
    case WM_COMMAND: {
        const UINT id = LOWORD(wp), code = HIWORD(wp);
        if (id == IDC_TOGGLE && code == BN_CLICKED) {
            SetDenoiseUi(!g_denoise);
        } else if (id == IDC_MIC && code == CBN_DROPDOWN) {
            FillMicCombo();
        } else if (id == IDC_MIC && code == CBN_SELCHANGE) {
            int i = int(SendMessageW(g_mic, CB_GETCURSEL, 0, 0));
            g_inputId = (i <= 0 || size_t(i) > g_micList.size()) ? L"" : g_micList[i - 1].id;
            ApplyDevices();
        } else if (id == IDC_OUT && code == CBN_DROPDOWN) {
            FillOutCombo();
        } else if (id == IDC_OUT && code == CBN_SELCHANGE) {
            int i = int(SendMessageW(g_out, CB_GETCURSEL, 0, 0));
            g_outputId = (i <= 0 || size_t(i) > g_outList.size()) ? L"" : g_outList[i - 1].id;
            ApplyDevices();
        } else if (id == IDC_FOCUS && code == CBN_SELCHANGE) {
            int i = int(SendMessageW(g_focusCb, CB_GETCURSEL, 0, 0));
            g_focus = i < 0 ? 0 : (i >= kFocusCount ? kFocusCount - 1 : i);
            g_engine.SetFocus(g_focus);
            SaveSettings();
            UpdateLevelUi();
        } else if (id == IDC_MODEL && code == CBN_SELCHANGE) {
            int i = int(SendMessageW(g_model, CB_GETCURSEL, 0, 0));
            if (i == 1 && !Engine::HighQualityInstalled()) {
                SendMessageW(g_model, CB_SETCURSEL, g_modelSel, 0);   // deepfilter.dll missing
                MessageBoxW(hwnd, L"deepfilter.dll was not found next to ClearMic.exe. Run build.bat to copy it.",
                            kAppName, MB_ICONWARNING);
            } else if (i >= 0 && i != g_modelSel) {
                g_modelSel = i;
                g_engine.SetModel(i);
                SaveSettings();
            }
        } else if (id == IDC_TEACH && code == BN_CLICKED) {
            if (g_enrolling) {
                g_engine.CancelEnrollment();
                g_enrolling = false;
            } else if (!g_denoise || g_engine.Status().state != EngineState::Running) {
                MessageBoxW(hwnd, L"Turn noise cancellation on and make sure ClearMic is Active first.", kAppName,
                            MB_ICONINFORMATION);
            } else if (MessageBoxW(hwnd,
                                   L"ClearMic will listen to about 20 seconds of your speech and save a voiceprint "
                                   L"on this PC only (nothing is uploaded).\n\nUse the microphone you will use in "
                                   L"calls. Talk normally, alone in the room if possible. Start now?",
                                   kAppName, MB_OKCANCEL | MB_ICONINFORMATION) == IDOK) {
                g_engine.StartEnrollment();
                g_enrolling = true;
            }
            UpdateVoiceButtons();
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (id == IDC_FORGET && code == BN_CLICKED) {
            if (MessageBoxW(hwnd, L"Delete your saved voiceprint from this PC?", kAppName,
                            MB_YESNO | MB_ICONQUESTION) == IDYES) {
                g_engine.ForgetVoice();
                UpdateVoiceButtons();
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        } else if (id == IDC_ENHANCE && code == BN_CLICKED) {
            g_enhanceOn = SendMessageW(g_enhance, BM_GETCHECK, 0, 0) == BST_CHECKED;
            g_engine.SetEnhance(g_enhanceOn);
            SaveSettings();
        } else if (id == IDC_AUTOSTART && code == BN_CLICKED) {
            SetAutostart(SendMessageW(g_auto, BM_GETCHECK, 0, 0) == BST_CHECKED);
        } else if (id == IDC_INSTALL && code == BN_CLICKED) {
            ShellExecuteW(nullptr, L"open", kCableUrl, nullptr, nullptr, SW_SHOWNORMAL);
        } else if (id == IDC_QUIT && code == BN_CLICKED) {
            DestroyWindow(hwnd);
        }
        return 0;
    }
    case WM_TIMER:
        if (wp == kMeterTimer) {
            const bool running = g_engine.Status().state == EngineState::Running;
            Engine::Meter m = g_engine.GetMeter();
            const float in = running ? m.inDb : -100.0f, out = running ? m.outDb : -100.0f;
            // Fast rise, slow fall so the bars are readable.
            g_dispIn = in > g_dispIn ? in : g_dispIn - 1.5f;
            g_dispOut = out > g_dispOut ? out : g_dispOut - 1.5f;
            g_gateDb = m.gateDb;
            g_gateOpen = m.open;
            // In automatic modes the (disabled) level slider follows the learned threshold.
            if (!ManualMode() && running && g_denoise && g_focus > 0) {
                int p = int(m.gateDb) - kLevelMin;
                p = p < 0 ? 0 : (p > kLevelMax - kLevelMin ? kLevelMax - kLevelMin : p);
                if (p != int(SendMessageW(g_level, TBM_GETPOS, 0, 0))) SendMessageW(g_level, TBM_SETPOS, TRUE, p);
            }
            RECT r = {S(kM), S(yMeter), S(kW - kM), S(yLevel + hRow)};   // meter card + level label
            InvalidateRect(hwnd, &r, FALSE);

            // Enrollment finished?
            if (g_enrolling) {
                const float ep = g_engine.EnrollProgress();
                if (ep >= 2.99f) {
                    g_enrolling = false;
                    UpdateVoiceButtons();
                    MessageBoxW(hwnd, (L"Could not save your voice: " + g_engine.EnrollError()).c_str(), kAppName,
                                MB_ICONWARNING);
                } else if (ep >= 2.0f) {
                    g_enrolling = false;
                    g_matchPct = -1;   // use the freshly calibrated threshold
                    g_focus = Engine::kFocusVoiceMatch;
                    g_engine.SetFocus(g_focus);
                    g_engine.SetMatchThreshold(MatchPct() / 100.0f);
                    SendMessageW(g_focusCb, CB_SETCURSEL, g_focus, 0);
                    SaveSettings();
                    UpdateLevelUi();
                    UpdateVoiceButtons();
                    MessageBoxW(hwnd, L"Your voice is saved. Voice focus is now set to \"My voice only\".",
                                kAppName, MB_ICONINFORMATION);
                }
            }
        }
        return 0;
    case WM_HSCROLL: {
        HWND bar = reinterpret_cast<HWND>(lp);
        const int pos = int(SendMessageW(bar, TBM_GETPOS, 0, 0));
        if (bar == g_level && MatchMode()) {
            g_matchPct = pos;
            g_engine.SetMatchThreshold(pos / 100.0f);
            SaveSettings();
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (bar == g_level && ManualMode()) {
            SetManualDb(pos + kLevelMin);
            SaveSettings();
        } else if (bar == g_volume) {
            g_volumePct = pos;
            g_engine.SetVolume(pos / 100.0f);
            SaveSettings();
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }
    case WM_SETCURSOR:
        if (reinterpret_cast<HWND>(wp) == hwnd && LOWORD(lp) == HTCLIENT && ManualMode()) {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            RECT r = CleanBarRect();
            if (PtInRect(&r, pt)) { SetCursor(LoadCursorW(nullptr, IDC_HAND)); return TRUE; }
        }
        break;
    case WM_LBUTTONDOWN: {
        POINT pt = {short(LOWORD(lp)), short(HIWORD(lp))};
        RECT r = CleanBarRect();
        if (ManualMode() && PtInRect(&r, pt)) {
            g_dragging = true;
            SetCapture(hwnd);
            SetManualDb(DbFromX(pt.x));
        }
        return 0;
    }
    case WM_MOUSEMOVE:
        if (g_dragging) SetManualDb(DbFromX(short(LOWORD(lp))));
        return 0;
    case WM_LBUTTONUP:
    case WM_CAPTURECHANGED:
        if (g_dragging) {
            g_dragging = false;
            if (msg == WM_LBUTTONUP) ReleaseCapture();
            SaveSettings();
        }
        return 0;
    case WM_SIZE:
        if (wp == SIZE_MINIMIZED) KillTimer(hwnd, kMeterTimer);
        else if (IsWindowVisible(hwnd)) SetTimer(hwnd, kMeterTimer, 50, nullptr);
        break;
    case WM_DPICHANGED: {
        g_dpi = HIWORD(wp);
        CreateFonts();
        Layout();
        const RECT* r = reinterpret_cast<const RECT*>(lp);
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;
    }
    case WM_TRAY:
        if (LOWORD(lp) == WM_LBUTTONUP) ShowMain();
        else if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) ShowTrayMenu();
        return 0;
    case WM_SHOWME:
        ShowMain();
        return 0;
    case WM_ENGINE: {
        EngineStatus s = g_engine.Status();
        RefreshCable();
        TrayUpdate();
        if (s.state == EngineState::Error && g_lastState != EngineState::Error && !IsWindowVisible(hwnd))
            TrayBalloon(L"ClearMic needs attention", s.message, NIIF_WARNING);
        g_lastState = s.state;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_CLOSE:
        HideMain();
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, kMeterTimer);
        TrayRemove();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

HWND Child(const wchar_t* cls, const wchar_t* text, DWORD style, UINT id) {
    return CreateWindowExW(0, cls, text, WS_CHILD | WS_TABSTOP | style, 0, 0, 10, 10, g_wnd,
                           reinterpret_cast<HMENU>(UINT_PTR(id)), g_inst, nullptr);
}

} // namespace

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR cmdLine, int) {
    HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\ClearMic.SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND other = FindWindowW(kWndClass, nullptr)) PostMessageW(other, WM_SHOWME, 0, 0);
        return 0;
    }
    const bool startHidden = cmdLine && strstr(cmdLine, "--tray") != nullptr;

    g_inst = inst;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_STANDARD_CLASSES | ICC_BAR_CLASSES};
    InitCommonControlsEx(&icc);

    g_inputId = RegReadString(L"InputId");
    g_outputId = RegReadString(L"OutputId");
    g_denoise = RegReadDword(L"Denoise", 1) != 0;
    g_focus = int(RegReadDword(L"VoiceFocus", 1));
    if (g_focus < 0 || g_focus >= kFocusCount) g_focus = 1;
    g_manualDb = -int(RegReadDword(L"ManualLevelNeg", 35));
    if (g_manualDb < kLevelMin || g_manualDb > kLevelMax) g_manualDb = -35;
    g_enhanceOn = RegReadDword(L"Enhance", 0) == 1;
    g_modelSel = RegReadDword(L"Model", 0) == 1 ? 1 : 0;
    if (g_modelSel == 1 && !Engine::HighQualityInstalled()) g_modelSel = 0;
    {
        DWORD mp = RegReadDword(L"MatchPercent", 0xFFFFFFFF);
        g_matchPct = mp <= 100 ? int(mp) : -1;
    }
    g_volumePct = int(RegReadDword(L"VolumePercent", 100));
    if (g_volumePct < 0 || g_volumePct > kVolMax) g_volumePct = 100;

    const int small = GetSystemMetrics(SM_CXSMICON);
    g_iconOn = MakeIcon(cGreen, false, small);
    g_iconOff = MakeIcon(cGrey, true, small);
    g_iconErr = MakeIcon(cAmber, false, small);
    g_iconSmall = MakeIcon(cGreen, false, small);
    g_iconBig = MakeIcon(cGreen, false, GetSystemMetrics(SM_CXICON));
    g_bgBrush = CreateSolidBrush(cBg);
    g_infoBrush = CreateSolidBrush(cInfo);
    g_warnBrush = CreateSolidBrush(cWarn);

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = g_bgBrush;
    wc.hIcon = g_iconBig;
    wc.hIconSm = g_iconSmall;
    wc.lpszClassName = kWndClass;
    RegisterClassExW(&wc);

    g_wnd = CreateWindowExW(0, kWndClass, kAppName,
                            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN,
                            CW_USEDEFAULT, CW_USEDEFAULT, 400, 600, nullptr, nullptr, inst, nullptr);
    g_dpi = int(GetDpiForWindow(g_wnd));
    if (g_dpi <= 0) g_dpi = 96;
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    CreateFonts();

    g_toggle = Child(L"BUTTON", L"Noise cancellation", WS_VISIBLE | BS_OWNERDRAW, IDC_TOGGLE);
    g_mic = Child(L"COMBOBOX", L"", WS_VISIBLE | WS_VSCROLL | CBS_DROPDOWNLIST, IDC_MIC);
    g_model = Child(L"COMBOBOX", L"", WS_VISIBLE | WS_VSCROLL | CBS_DROPDOWNLIST, IDC_MODEL);
    g_focusCb = Child(L"COMBOBOX", L"", WS_VISIBLE | WS_VSCROLL | CBS_DROPDOWNLIST, IDC_FOCUS);
    g_out = Child(L"COMBOBOX", L"", WS_VISIBLE | WS_VSCROLL | CBS_DROPDOWNLIST, IDC_OUT);
    g_auto = Child(L"BUTTON", L"Start ClearMic when Windows starts", WS_VISIBLE | BS_AUTOCHECKBOX, IDC_AUTOSTART);
    g_install = Child(L"BUTTON", L"Get VB-CABLE...", BS_PUSHBUTTON, IDC_INSTALL);
    g_quit = Child(L"BUTTON", L"Quit ClearMic", WS_VISIBLE | BS_PUSHBUTTON, IDC_QUIT);
    g_enhance = Child(L"BUTTON", L"Enhance voice (HD): clearer tone, even loudness", WS_VISIBLE | BS_AUTOCHECKBOX,
                      IDC_ENHANCE);
    SendMessageW(g_enhance, BM_SETCHECK, g_enhanceOn ? BST_CHECKED : BST_UNCHECKED, 0);
    g_teach = Child(L"BUTTON", L"Teach my voice", BS_PUSHBUTTON, IDC_TEACH);
    g_forget = Child(L"BUTTON", L"Forget", BS_PUSHBUTTON, IDC_FORGET);
    g_level = Child(TRACKBAR_CLASSW, L"", WS_VISIBLE | TBS_HORZ | TBS_NOTICKS, IDC_LEVEL);
    g_volume = Child(TRACKBAR_CLASSW, L"", WS_VISIBLE | TBS_HORZ | TBS_NOTICKS, IDC_VOLUME);
    SendMessageW(g_level, TBM_SETRANGEMIN, FALSE, 0);
    SendMessageW(g_level, TBM_SETRANGEMAX, FALSE, kLevelMax - kLevelMin);
    SendMessageW(g_level, TBM_SETPAGESIZE, 0, 5);
    SendMessageW(g_level, TBM_SETPOS, TRUE, g_manualDb - kLevelMin);
    SendMessageW(g_volume, TBM_SETRANGEMIN, FALSE, 0);
    SendMessageW(g_volume, TBM_SETRANGEMAX, FALSE, kVolMax);
    SendMessageW(g_volume, TBM_SETPAGESIZE, 0, 10);
    SendMessageW(g_volume, TBM_SETPOS, TRUE, g_volumePct);
    Layout();
    SizeWindow(true);

    SendMessageW(g_model, CB_ADDSTRING, 0, (LPARAM)kModelNames[0]);
    SendMessageW(g_model, CB_ADDSTRING, 0,
                 (LPARAM)(Engine::HighQualityInstalled() ? std::wstring(kModelNames[1])
                                                         : std::wstring(kModelNames[1]) + L" (not installed)")
                     .c_str());
    SendMessageW(g_model, CB_SETCURSEL, g_modelSel, 0);
    EnableWindow(g_model, g_denoise);
    for (const wchar_t* n : kFocusNames) SendMessageW(g_focusCb, CB_ADDSTRING, 0, (LPARAM)n);
    SendMessageW(g_focusCb, CB_SETCURSEL, g_focus, 0);
    EnableWindow(g_focusCb, g_denoise);
    EnableWindow(g_level, ManualMode());
    SendMessageW(g_auto, BM_SETCHECK, IsAutostart() ? BST_CHECKED : BST_UNCHECKED, 0);
    FillMicCombo();
    FillOutCombo();
    RefreshCable();

    g_engine.SetDenoise(g_denoise);
    g_engine.SetFocus(g_focus);
    g_engine.SetModel(g_modelSel);
    g_engine.SetEnhance(g_enhanceOn);
    g_engine.Start(g_wnd, WM_ENGINE);                     // loads the saved voiceprint
    g_engine.SetMatchThreshold(MatchPct() / 100.0f);
    UpdateVoiceButtons();
    UpdateLevelUi();
    g_engine.SetManualLevel(float(g_manualDb));
    g_engine.SetVolume(g_volumePct / 100.0f);
    g_engine.SetDevices(g_inputId, g_outputId);
    TrayAdd();
    if (!startHidden) ShowMain();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (IsDialogMessageW(g_wnd, &msg)) continue;   // Tab key moves between controls
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    g_engine.Stop();
    for (HICON i : {g_iconOn, g_iconOff, g_iconErr, g_iconBig, g_iconSmall}) DestroyIcon(i);
    for (HFONT f : {g_fText, g_fBold, g_fTitle, g_fSmall, g_fBig}) DeleteObject(f);
    for (HBRUSH b : {g_bgBrush, g_infoBrush, g_warnBrush}) DeleteObject(b);
    CoUninitialize();
    if (single) CloseHandle(single);
    return 0;
}
