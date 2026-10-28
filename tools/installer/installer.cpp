// SCDA Recomp installer. Takes the user's own Splinter Cell: Double Agent (X360) ISO,
// extracts its XDVDFS volume into <dir>\game, verifies SC4_Offline.xex is the exact
// binary the recompiled code was generated from, and writes the embedded scda.exe.
// Ships no game data.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>
#include "resource.h"

// SHA-256 of the SC4_Offline.xex that generated/ was recompiled from.
static const char* kExpectedXexSha256 = "86d32713b57feb6631f0810a9416046beb3a2157084498e61f5b59b669e82a93";
// SHA-256 of the flat image xex_dump builds from it (config/expected_image.txt).
static const char* kExpectedImageSha256 = "01193b2556abc03506ad9c0d40f30b04ce27d027e27a59cf786eeb90b53aea4e";

constexpr uint32_t SECTOR = 2048;
constexpr uint64_t kXgdBases[] = { 0, 0x18300000, 0xFD90000, 0x2080000 }; // xiso, XGD3, XGD2, XGD1
constexpr int W = 960, H = 540;

enum { ID_ISO_EDIT = 100, ID_ISO_BROWSE, ID_DIR_EDIT, ID_DIR_BROWSE, ID_INSTALL, ID_PROGRESS, ID_STATUS, ID_SHORTCUT };
constexpr UINT WM_APP_STATUS = WM_APP + 1, WM_APP_PROGRESS = WM_APP + 2, WM_APP_DONE = WM_APP + 3;

static HWND g_wnd, g_isoEdit, g_dirEdit, g_install, g_progress, g_status, g_shortcut;
static HBITMAP g_background;
static HFONT g_font;

struct Entry { std::wstring path; uint32_t sector, size; bool dir; };

static bool g_headless, g_ok;

static void PostStatus(const std::wstring& s)
{
    if (g_headless)
        fwprintf(stdout, L"%ls\n", s.c_str());
    else
        PostMessageW(g_wnd, WM_APP_STATUS, 0, (LPARAM)new std::wstring(s));
}

// ---- XDVDFS (ported from tools/pe_ppc_inspect.py) ----

static bool ReadAt(HANDLE f, uint64_t off, void* buf, DWORD size)
{
    LARGE_INTEGER li; li.QuadPart = (LONGLONG)off;
    DWORD got = 0;
    return SetFilePointerEx(f, li, nullptr, FILE_BEGIN) && ReadFile(f, buf, size, &got, nullptr) && got == size;
}

static bool FindVolume(HANDLE f, uint64_t& base, uint32_t& rootSector, uint32_t& rootSize)
{
    for (uint64_t b : kXgdBases)
    {
        uint8_t vd[28];
        if (ReadAt(f, b + 32ull * SECTOR, vd, sizeof(vd)) && !memcmp(vd, "MICROSOFT*XBOX*MEDIA", 20))
        {
            base = b;
            memcpy(&rootSector, vd + 20, 4);
            memcpy(&rootSize, vd + 24, 4);
            return true;
        }
    }
    return false;
}

static void WalkDir(HANDLE f, uint64_t base, uint32_t sector, uint32_t size, const std::wstring& prefix, std::vector<Entry>& out, int depth = 0)
{
    if (depth > 32 || size == 0 || size > 64 * 1024 * 1024)
        return;
    std::vector<uint8_t> data(size);
    if (!ReadAt(f, base + uint64_t(sector) * SECTOR, data.data(), size))
        return;

    std::vector<uint32_t> stack{ 0 };
    std::vector<bool> seen(size / 4 + 1);
    while (!stack.empty())
    {
        uint32_t off = stack.back();
        stack.pop_back();
        if (off * 4 + 14 > size || seen[off])
            continue;
        seen[off] = true;
        const uint8_t* e = data.data() + off * 4;
        uint16_t left, right; uint32_t esec, esize;
        memcpy(&left, e, 2); memcpy(&right, e + 2, 2); memcpy(&esec, e + 4, 4); memcpy(&esize, e + 8, 4);
        uint8_t attr = e[12], nlen = e[13];
        if (left == 0xFFFF && right == 0xFFFF && esec == 0xFFFFFFFF)
            continue; // padding
        if (off * 4 + 14 + nlen > size)
            continue;
        std::wstring name;
        for (int i = 0; i < nlen; i++)
            name += wchar_t(e[14 + i]);
        std::wstring path = prefix + name;
        if (attr & 0x10)
        {
            out.push_back({ path, esec, esize, true });
            WalkDir(f, base, esec, esize, path + L"\\", out, depth + 1);
        }
        else
        {
            out.push_back({ path, esec, esize, false });
        }
        for (uint16_t next : { left, right })
            if (next != 0 && next != 0xFFFF)
                stack.push_back(next);
    }
}

// ---- Helpers ----

static std::string Sha256File(const std::wstring& path)
{
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return {};
    BCRYPT_ALG_HANDLE alg; BCRYPT_HASH_HANDLE hash;
    BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0);
    std::vector<uint8_t> buf(1 << 20);
    DWORD got;
    while (ReadFile(f, buf.data(), (DWORD)buf.size(), &got, nullptr) && got)
        BCryptHashData(hash, buf.data(), got, 0);
    uint8_t digest[32];
    BCryptFinishHash(hash, digest, 32, 0);
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(f);
    std::string hex;
    char tmp[3];
    for (uint8_t b : digest) { snprintf(tmp, 3, "%02x", b); hex += tmp; }
    return hex;
}

static bool WriteResource(int id, const std::wstring& path)
{
    HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!r) return false;
    HGLOBAL g = LoadResource(nullptr, r);
    DWORD size = SizeofResource(nullptr, r);
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD wrote;
    bool ok = WriteFile(f, LockResource(g), size, &wrote, nullptr) && wrote == size;
    CloseHandle(f);
    return ok;
}

// Runs the embedded xex_dump: SC4_Offline.xex -> flat image + XEX header (what scda.exe loads).
static bool BuildImage(const std::wstring& game)
{
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring tool = std::wstring(tmp) + L"scda_xex_dump.exe";
    if (!WriteResource(IDR_XEX_DUMP, tool))
        return false;
    std::wstring cmd = L"\"" + tool + L"\" \"" + game + L"\\SC4_Offline.xex\" --out-img \"" + game +
        L"\\SC4_Offline.img\" --out-header \"" + game + L"\\SC4_Offline.xexhdr\"";
    STARTUPINFOW si{ sizeof(si) };
    PROCESS_INFORMATION pi;
    DWORD code = 1;
    if (CreateProcessW(tool.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
    {
        WaitForSingleObject(pi.hProcess, INFINITE);
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    DeleteFileW(tool.c_str());
    return code == 0 && Sha256File(game + L"\\SC4_Offline.img") == kExpectedImageSha256;
}

static void CreateDesktopShortcut(const std::wstring& target, const std::wstring& workDir)
{
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IShellLinkW* link;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))))
    {
        link->SetPath(target.c_str());
        link->SetWorkingDirectory(workDir.c_str());
        link->SetIconLocation(target.c_str(), 0);
        link->SetDescription(L"Splinter Cell: Double Agent (Recomp)");
        IPersistFile* file;
        PWSTR desktop;
        if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&file))) && SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desktop)))
        {
            file->Save((std::wstring(desktop) + L"\\Splinter Cell Double Agent (Recomp).lnk").c_str(), TRUE);
            CoTaskMemFree(desktop);
            file->Release();
        }
        link->Release();
    }
    CoUninitialize();
}

// ---- Install worker ----

static void Install(std::wstring iso, std::wstring dir, bool shortcut)
{
    auto fail = [&](const std::wstring& msg) { PostStatus(msg); PostMessageW(g_wnd, WM_APP_DONE, 0, 0); };

    HANDLE f = CreateFileW(iso.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return fail(L"Cannot open the ISO.");

    uint64_t base; uint32_t rootSector, rootSize;
    if (!FindVolume(f, base, rootSector, rootSize))
    {
        CloseHandle(f);
        return fail(L"Not an Xbox 360 disc image (no XDVDFS volume).");
    }

    PostStatus(L"Reading disc layout...");
    std::vector<Entry> entries;
    WalkDir(f, base, rootSector, rootSize, L"", entries);

    bool hasXex = false;
    uint64_t total = 0;
    for (auto& e : entries)
    {
        if (!e.dir) total += e.size;
        if (!_wcsicmp(e.path.c_str(), L"SC4_Offline.xex")) hasXex = true;
    }
    if (!hasXex)
    {
        CloseHandle(f);
        return fail(L"SC4_Offline.xex not found: this is not Splinter Cell: Double Agent.");
    }

    std::wstring game = dir + L"\\game";
    SHCreateDirectoryExW(nullptr, game.c_str(), nullptr);

    // Verify the executable first so a wrong disc fails fast.
    std::vector<uint8_t> buf(4 << 20);
    uint64_t done = 0;
    auto extract = [&](const Entry& e) -> bool
    {
        std::wstring out = game + L"\\" + e.path;
        HANDLE o = CreateFileW(out.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        if (o == INVALID_HANDLE_VALUE) return false;
        uint64_t off = base + uint64_t(e.sector) * SECTOR;
        uint32_t left = e.size;
        while (left)
        {
            DWORD n = std::min<uint32_t>(left, (uint32_t)buf.size()), wrote;
            if (!ReadAt(f, off, buf.data(), n) || !WriteFile(o, buf.data(), n, &wrote, nullptr))
            {
                CloseHandle(o);
                return false;
            }
            off += n; left -= n; done += n;
            PostMessageW(g_wnd, WM_APP_PROGRESS, (WPARAM)(total ? done * 1000 / total : 0), 0);
        }
        CloseHandle(o);
        return true;
    };

    for (auto& e : entries)
    {
        if (!_wcsicmp(e.path.c_str(), L"SC4_Offline.xex"))
        {
            PostStatus(L"Verifying SC4_Offline.xex...");
            if (!extract(e)) { CloseHandle(f); return fail(L"Failed writing SC4_Offline.xex."); }
            if (Sha256File(game + L"\\SC4_Offline.xex") != kExpectedXexSha256)
            {
                CloseHandle(f);
                return fail(L"SC4_Offline.xex does not match the supported version (V1, Title 555307D7).");
            }
        }
    }

    for (auto& e : entries)
    {
        if (e.dir)
        {
            SHCreateDirectoryExW(nullptr, (game + L"\\" + e.path).c_str(), nullptr);
            continue;
        }
        if (!_wcsicmp(e.path.c_str(), L"SC4_Offline.xex"))
            continue;
        PostStatus(L"Extracting " + e.path);
        if (!extract(e)) { CloseHandle(f); return fail(L"Failed extracting " + e.path); }
    }
    CloseHandle(f);

    PostStatus(L"Building SC4_Offline.img...");
    if (!BuildImage(game))
        return fail(L"Failed building SC4_Offline.img from SC4_Offline.xex.");

    PostStatus(L"Installing scda.exe...");
    std::wstring exe = dir + L"\\scda.exe";
    if (!WriteResource(IDR_SCDA_EXE, exe))
        return fail(L"Failed writing scda.exe.");
    if (shortcut)
        CreateDesktopShortcut(exe, dir);

    PostMessageW(g_wnd, WM_APP_PROGRESS, 1000, 0);
    g_ok = true;
    PostStatus(L"Done. Installed to " + dir + L"  (developer preview)");
    PostMessageW(g_wnd, WM_APP_DONE, 1, 0);
}

// ---- UI ----

static std::wstring GetText(HWND h)
{
    std::wstring s(GetWindowTextLengthW(h), L'\0');
    GetWindowTextW(h, s.data(), (int)s.size() + 1);
    return s;
}

static void BrowseIso()
{
    wchar_t path[MAX_PATH] = L"";
    OPENFILENAMEW ofn{ sizeof(ofn) };
    ofn.hwndOwner = g_wnd;
    ofn.lpstrFilter = L"Xbox 360 disc image (*.iso)\0*.iso\0All files\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameW(&ofn))
        SetWindowTextW(g_isoEdit, path);
}

static void BrowseDir()
{
    IFileOpenDialog* dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))))
        return;
    DWORD opts;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS);
    IShellItem* item;
    if (SUCCEEDED(dlg->Show(g_wnd)) && SUCCEEDED(dlg->GetResult(&item)))
    {
        PWSTR p;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)))
        {
            SetWindowTextW(g_dirEdit, (std::wstring(p) + L"\\SCDA Recomp").c_str());
            CoTaskMemFree(p);
        }
        item->Release();
    }
    dlg->Release();
}

static HWND Make(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id)
{
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, g_wnd, (HMENU)(INT_PTR)id, nullptr, nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE);
    return c;
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_CREATE:
    {
        g_wnd = hwnd;
        g_font = CreateFontW(-15, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        PWSTR pf;
        std::wstring defDir = L"C:\\Games\\SCDA Recomp";
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramFilesX86, 0, nullptr, &pf)))
            CoTaskMemFree(pf); // default stays outside Program Files: no admin rights needed
        int y = H - 150;
        Make(L"STATIC", L"Disc image:", SS_LEFT, 40, y + 3, 110, 22, 0);
        g_isoEdit = Make(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, 150, y, 640, 24, ID_ISO_EDIT);
        Make(L"BUTTON", L"Browse...", BS_PUSHBUTTON, 800, y, 120, 26, ID_ISO_BROWSE);
        y += 34;
        Make(L"STATIC", L"Install to:", SS_LEFT, 40, y + 3, 110, 22, 0);
        g_dirEdit = Make(L"EDIT", defDir.c_str(), WS_BORDER | ES_AUTOHSCROLL, 150, y, 640, 24, ID_DIR_EDIT);
        Make(L"BUTTON", L"Browse...", BS_PUSHBUTTON, 800, y, 120, 26, ID_DIR_BROWSE);
        y += 36;
        g_progress = Make(PROGRESS_CLASSW, L"", 0, 40, y, 750, 20, ID_PROGRESS);
        SendMessageW(g_progress, PBM_SETRANGE, 0, MAKELPARAM(0, 1000));
        g_install = Make(L"BUTTON", L"Install", BS_DEFPUSHBUTTON, 800, y - 4, 120, 30, ID_INSTALL);
        y += 28;
        g_status = Make(L"STATIC", L"Select your own Splinter Cell: Double Agent (Xbox 360) disc image.", SS_LEFT, 40, y, 600, 22, ID_STATUS);
        g_shortcut = Make(L"BUTTON", L"Desktop shortcut", BS_AUTOCHECKBOX, 760, y, 160, 22, ID_SHORTCUT);
        SendMessageW(g_shortcut, BM_SETCHECK, BST_CHECKED, 0);
        return 0;
    }
    case WM_ERASEBKGND:
    {
        HDC dc = (HDC)wp, mem = CreateCompatibleDC(dc);
        HGDIOBJ old = SelectObject(mem, g_background);
        BitBlt(dc, 0, 0, W, H, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteDC(mem);
        return 1;
    }
    case WM_CTLCOLORSTATIC:
    {
        if ((HWND)lp == g_isoEdit || (HWND)lp == g_dirEdit)
            break;
        HDC dc = (HDC)wp;
        SetTextColor(dc, RGB(150, 255, 120)); // NVG green on the dark background
        SetBkMode(dc, TRANSPARENT);
        return (LRESULT)GetStockObject(NULL_BRUSH);
    }
    case WM_COMMAND:
        switch (LOWORD(wp))
        {
        case ID_ISO_BROWSE: BrowseIso(); break;
        case ID_DIR_BROWSE: BrowseDir(); break;
        case IDCANCEL: DestroyWindow(hwnd); break;
        case ID_INSTALL:
        {
            std::wstring iso = GetText(g_isoEdit), dir = GetText(g_dirEdit);
            if (iso.empty() || dir.empty())
            {
                SetWindowTextW(g_status, L"Pick a disc image and an install folder first.");
                break;
            }
            EnableWindow(g_install, FALSE);
            bool sc = SendMessageW(g_shortcut, BM_GETCHECK, 0, 0) == BST_CHECKED;
            std::thread(Install, iso, dir, sc).detach();
            break;
        }
        }
        return 0;
    case WM_APP_STATUS:
    {
        auto* s = (std::wstring*)lp;
        SetWindowTextW(g_status, s->c_str());
        delete s;
        // Static text is transparent: repaint the strip behind it.
        RECT r; GetWindowRect(g_status, &r); MapWindowPoints(nullptr, hwnd, (POINT*)&r, 2);
        InvalidateRect(hwnd, &r, TRUE);
        return 0;
    }
    case WM_APP_PROGRESS:
        SendMessageW(g_progress, PBM_SETPOS, wp, 0);
        return 0;
    case WM_APP_DONE:
        EnableWindow(g_install, TRUE);
        if (wp) SetWindowTextW(g_install, L"Close");
        if (wp) SetWindowLongPtrW(g_install, GWLP_ID, IDCANCEL);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show)
{
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // Headless: SCDA_Setup.exe --iso <image> --dir <folder> [--no-shortcut]
    int argc;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring iso, dir;
    bool shortcut = true;
    for (int i = 1; i < argc; i++)
    {
        if (!wcscmp(argv[i], L"--iso") && i + 1 < argc) iso = argv[++i];
        else if (!wcscmp(argv[i], L"--dir") && i + 1 < argc) dir = argv[++i];
        else if (!wcscmp(argv[i], L"--no-shortcut")) shortcut = false;
    }
    if (!iso.empty() && !dir.empty())
    {
        g_headless = true;
        if (AttachConsole(ATTACH_PARENT_PROCESS))
            freopen("CONOUT$", "w", stdout);
        Install(iso, dir, shortcut);
        return g_ok ? 0 : 1;
    }

    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);
    g_background = (HBITMAP)LoadImageW(inst, MAKEINTRESOURCEW(IDB_BACKGROUND), IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION);

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP));
    wc.hIconSm = wc.hIcon;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"SCDARecompSetup";
    RegisterClassExW(&wc);

    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    RECT r{ 0, 0, W, H };
    AdjustWindowRect(&r, style, FALSE);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"Splinter Cell: Double Agent Recomp - Setup", style,
        CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, nullptr);
    ShowWindow(hwnd, show);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0))
    {
        if (!IsDialogMessageW(hwnd, &m))
        {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    return 0;
}
