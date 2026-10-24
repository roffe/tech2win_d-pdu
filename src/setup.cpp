// Setup + config GUI: installs/uninstalls the D-PDU DLL and edits the shared ini.
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <string>
#include "common.h"
#include "setup_res.h"

static const char *PDU_KEY = "SOFTWARE\\D-PDU API";
static const char *OUR_KEY = "SOFTWARE\\" APP_NAME;

static std::vector<Adapter> g_adapters;
static Config g_cfg;

static std::string installDir() {
    char p[MAX_PATH] = "C:\\Program Files (x86)";
    SHGetFolderPathA(nullptr, CSIDL_PROGRAM_FILESX86, nullptr, 0, p);
    return std::string(p) + "\\" APP_NAME;
}
static std::string dllTarget() { return installDir() + "\\" APP_NAME ".dll"; }
static std::string rootTarget() { return installDir() + "\\pdu_api_root.xml"; }

static std::string exeDir() {
    char p[MAX_PATH];
    GetModuleFileNameA(nullptr, p, MAX_PATH);
    std::string s = p;
    return s.substr(0, s.find_last_of('\\'));
}

static std::string regGet(const char *key, const char *name) {
    char buf[1024];
    DWORD size = sizeof(buf);
    if (RegGetValueA(HKEY_LOCAL_MACHINE, key, name, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS)
        return "";
    return buf;
}

static bool regSet(const char *key, const char *name, const std::string &v) {
    HKEY k;
    if (RegCreateKeyExA(HKEY_LOCAL_MACHINE, key, 0, nullptr, 0, KEY_WRITE | KEY_WOW64_32KEY, nullptr, &k, nullptr) != ERROR_SUCCESS)
        return false;
    bool ok = RegSetValueExA(k, name, 0, REG_SZ, (const BYTE *)v.c_str(), v.size() + 1) == ERROR_SUCCESS;
    RegCloseKey(k);
    return ok;
}

static bool installed() {
    return GetFileAttributesA(dllTarget().c_str()) != INVALID_FILE_ATTRIBUTES &&
           _stricmp(regGet(PDU_KEY, "Root File").c_str(), rootTarget().c_str()) == 0;
}

static std::string lastErr() {
    char buf[256] = "";
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, GetLastError(), 0, buf, sizeof(buf), nullptr);
    return buf;
}

static bool writeFile(const std::string &path, const std::string &data) {
    HANDLE f = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return false;
    DWORD n;
    bool ok = WriteFile(f, data.data(), data.size(), &n, nullptr) && n == data.size();
    CloseHandle(f);
    return ok;
}

// ponytail: Tech2Win only ever loads one D-PDU API, so we point the registry at
// our own root file and put the previous one back on uninstall (MDI users).
// 32-bit process: HKLM\SOFTWARE is redirected to WOW6432Node like Tech2Win's own lookups.
static std::string install() {
    std::string dir = installDir();
    CreateDirectoryA(dir.c_str(), nullptr);
    std::string src = exeDir() + "\\" APP_NAME ".dll";
    if (GetFileAttributesA(src.c_str()) == INVALID_FILE_ATTRIBUTES)
        return "Can't find " APP_NAME ".dll next to the setup.\nUnzip the whole zip to a folder first and run the setup from there.";
    if (!CopyFileA(src.c_str(), dllTarget().c_str(), FALSE))
        return "Could not copy " APP_NAME ".dll (is Tech2Win running?)\n" + lastErr();

    std::string xml =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
        "<MVCI_PDU_API_ROOT MVCI_PART2_STANDARD_VERSION=\"2.2.0\">\r\n"
        "  <MVCI_PDU_API>\r\n"
        "    <SHORT_NAME>" SHORT_NAME "</SHORT_NAME>\r\n"
        "    <DESCRIPTION>" APP_TITLE "</DESCRIPTION>\r\n"
        "    <SUPPLIER_NAME>github.com/roffe</SUPPLIER_NAME>\r\n"
        "    <LIBRARY_FILE URI=\"file://" + dllTarget() + "\"/>\r\n"
        "  </MVCI_PDU_API>\r\n"
        "</MVCI_PDU_API_ROOT>\r\n";
    if (!writeFile(rootTarget(), xml))
        return "Could not write " + rootTarget() + "\n" + lastErr();

    std::string prev = regGet(PDU_KEY, "Root File");
    if (_stricmp(prev.c_str(), rootTarget().c_str()) != 0)
        regSet(OUR_KEY, "PrevRootFile", prev);
    if (!regSet(PDU_KEY, "Root File", rootTarget()))
        return "Could not write the D-PDU API registry key";
    saveConfig(g_cfg);
    return "";
}

static std::string uninstall() {
    std::string prev = regGet(OUR_KEY, "PrevRootFile");
    if (_stricmp(regGet(PDU_KEY, "Root File").c_str(), rootTarget().c_str()) == 0) {
        if (prev.empty())
            RegDeleteKeyValueA(HKEY_LOCAL_MACHINE, PDU_KEY, "Root File");
        else
            regSet(PDU_KEY, "Root File", prev);
    }
    RegDeleteKeyExA(HKEY_LOCAL_MACHINE, OUR_KEY, KEY_WOW64_32KEY, 0);
    DeleteFileA(rootTarget().c_str());
    if (!DeleteFileA(dllTarget().c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND)
        return "Could not delete " + dllTarget() + " (is Tech2Win running?)\n" + lastErr();
    RemoveDirectoryA(installDir().c_str());
    return "";
}

static void refreshStatus(HWND dlg) {
    bool inst = installed();
    std::string s = inst ? "Installed in " + installDir() : "Not installed";
    if (!inst && !regGet(PDU_KEY, "Root File").empty())
        s += "\nCurrent D-PDU API: " + regGet(PDU_KEY, "Root File");
    SetDlgItemTextA(dlg, IDC_STATUS, s.c_str());
    EnableWindow(GetDlgItem(dlg, IDC_UNINSTALL), inst);
    SetDlgItemTextA(dlg, IDC_INSTALL, inst ? "Reinstall" : "Install");
}

static void fillAdapters(HWND dlg) {
    HWND lv = GetDlgItem(dlg, IDC_LIST);
    ListView_DeleteAllItems(lv);
    g_adapters = listAdapters();
    for (size_t i = 0; i < g_adapters.size(); i++) {
        LVITEMA it{};
        it.mask = LVIF_TEXT;
        it.iItem = i;
        it.pszText = (char *)g_adapters[i].name.c_str();
        ListView_InsertItem(lv, &it);
        ListView_SetItemText(lv, i, 1, (char *)g_adapters[i].vendor.c_str());
        ListView_SetItemText(lv, i, 2, (char *)g_adapters[i].dll.c_str());
    }
    if (g_adapters.empty()) {
        LVITEMA it{};
        it.mask = LVIF_TEXT;
        it.pszText = (char *)"No J2534 v04.04 adapters installed";
        ListView_InsertItem(lv, &it);
    }
}

static const int PIN_VALUES[] = {7, 8, 78};

static void initPinCombo(HWND dlg, int id, int value) {
    HWND cb = GetDlgItem(dlg, id);
    for (const char *s : {"Pin 7", "Pin 8", "Pin 7 + L-line on 8"})
        SendMessageA(cb, CB_ADDSTRING, 0, (LPARAM)s);
    for (int i = 0; i < 3; i++)
        if (PIN_VALUES[i] == value)
            SendMessageA(cb, CB_SETCURSEL, i, 0);
}

static int pinCombo(HWND dlg, int id) {
    int i = SendDlgItemMessageA(dlg, id, CB_GETCURSEL, 0, 0);
    return i >= 0 && i < 3 ? PIN_VALUES[i] : 7;
}

static void readForm(HWND dlg) {
    g_cfg.pin7 = pinCombo(dlg, IDC_PIN7);
    g_cfg.pin8 = pinCombo(dlg, IDC_PIN8);
    g_cfg.pin78 = pinCombo(dlg, IDC_PIN78);
    char buf[512];
    GetDlgItemTextA(dlg, IDC_INITPINS, buf, sizeof(buf));
    g_cfg.initPins = parseInitPins(buf);
    SetDlgItemTextA(dlg, IDC_INITPINS, formatInitPins(g_cfg.initPins).c_str()); // shows what was understood
    g_cfg.log = IsDlgButtonChecked(dlg, IDC_LOG) == BST_CHECKED;
}

static void report(HWND dlg, const std::string &err, const char *ok) {
    if (err.empty())
        MessageBoxA(dlg, ok, APP_TITLE, MB_ICONINFORMATION);
    else
        MessageBoxA(dlg, err.c_str(), APP_TITLE, MB_ICONERROR);
    refreshStatus(dlg);
}

static INT_PTR CALLBACK dlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM) {
    switch (msg) {
    case WM_INITDIALOG: {
        HWND lv = GetDlgItem(dlg, IDC_LIST);
        ListView_SetExtendedListViewStyle(lv, LVS_EX_FULLROWSELECT);
        const char *cols[] = {"Adapter", "Vendor", "DLL"};
        int widths[] = {150, 110, 300};
        for (int i = 0; i < 3; i++) {
            LVCOLUMNA c{};
            c.mask = LVCF_TEXT | LVCF_WIDTH;
            c.pszText = (char *)cols[i];
            c.cx = MulDiv(widths[i], LOWORD(GetDialogBaseUnits()), 6);
            ListView_InsertColumn(lv, i, &c);
        }
        g_cfg = loadConfig();
        fillAdapters(dlg);
        initPinCombo(dlg, IDC_PIN7, g_cfg.pin7);
        initPinCombo(dlg, IDC_PIN8, g_cfg.pin8);
        initPinCombo(dlg, IDC_PIN78, g_cfg.pin78);
        SetDlgItemTextA(dlg, IDC_INITPINS, formatInitPins(g_cfg.initPins).c_str());
        CheckDlgButton(dlg, IDC_LOG, g_cfg.log ? BST_CHECKED : BST_UNCHECKED);
        SetDlgItemTextA(dlg, IDC_VERSION, APP_NAME " " APP_VERSION);
        refreshStatus(dlg);
        return TRUE;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_INSTALL:
            readForm(dlg);
            report(dlg, install(), "Installed. Start Tech2Win and pick your adapter in the Select Vehicle Communication Interface list.");
            return TRUE;
        case IDC_UNINSTALL:
            report(dlg, uninstall(), "Uninstalled.");
            return TRUE;
        case IDC_REFRESH:
            fillAdapters(dlg);
            return TRUE;
        case IDC_OPENLOG:
            if ((INT_PTR)ShellExecuteA(dlg, "open", logPath().c_str(), nullptr, nullptr, SW_SHOWNORMAL) <= 32)
                MessageBoxA(dlg, ("No log yet: " + logPath()).c_str(), APP_TITLE, MB_ICONINFORMATION);
            return TRUE;
        case IDOK:
            readForm(dlg);
            if (!saveConfig(g_cfg))
                MessageBoxA(dlg, ("Could not write " + iniPath()).c_str(), APP_TITLE, MB_ICONERROR);
            return TRUE;
        case IDCANCEL:
            EndDialog(dlg, 0);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR cmd, int) {
    // unattended: t2wdpdu-setup.exe /install | /uninstall, exit code 0 = ok
    if (!_stricmp(cmd, "/install") || !_stricmp(cmd, "/uninstall")) {
        g_cfg = loadConfig();
        std::string err = cmd[1] == 'i' || cmd[1] == 'I' ? install() : uninstall();
        if (!err.empty())
            OutputDebugStringA(err.c_str());
        return err.empty() ? 0 : 1;
    }
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_LISTVIEW_CLASSES};
    InitCommonControlsEx(&icc);
    return DialogBoxA(inst, MAKEINTRESOURCEA(IDD_MAIN), nullptr, dlgProc);
}
