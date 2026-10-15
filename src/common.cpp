#include "common.h"
#include <windows.h>
#include <algorithm>

static std::string regString(HKEY k, const char *name) {
    char buf[1024];
    DWORD size = sizeof(buf) - 1, type = 0;
    if (RegQueryValueExA(k, name, nullptr, &type, (BYTE *)buf, &size) != ERROR_SUCCESS || type != REG_SZ)
        return "";
    buf[size] = 0;
    return buf;
}

std::vector<Adapter> listAdapters() {
    std::vector<Adapter> out;
    HKEY root;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\PassThruSupport.04.04", 0, KEY_READ | KEY_WOW64_32KEY, &root) != ERROR_SUCCESS)
        return out;
    char sub[256];
    for (DWORD i = 0;; i++) {
        DWORD len = sizeof(sub);
        if (RegEnumKeyExA(root, i, sub, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
            break;
        HKEY k;
        if (RegOpenKeyExA(root, sub, 0, KEY_READ | KEY_WOW64_32KEY, &k) != ERROR_SUCCESS)
            continue;
        Adapter a{regString(k, "Name"), regString(k, "Vendor"), regString(k, "FunctionLibrary")};
        RegCloseKey(k);
        if (a.name.empty())
            a.name = sub;
        if (!a.dll.empty())
            out.push_back(a);
    }
    RegCloseKey(root);
    std::sort(out.begin(), out.end(), [](const Adapter &a, const Adapter &b) { return a.name < b.name; });
    return out;
}

std::string dataDir() {
    char buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableA("PUBLIC", buf, sizeof(buf));
    return (n > 0 && n < sizeof(buf)) ? buf : "C:\\Users\\Public";
}

std::string iniPath() { return dataDir() + "\\" APP_NAME ".ini"; }
std::string logPath() { return dataDir() + "\\" APP_NAME ".log"; }

static int pinValue(int v, int def) { return (v == 7 || v == 8 || v == 78) ? v : def; }

Config loadConfig() {
    Config c;
    std::string ini = iniPath();
    c.pin7 = pinValue(GetPrivateProfileIntA("kline", "Pin7", c.pin7, ini.c_str()), c.pin7);
    c.pin8 = pinValue(GetPrivateProfileIntA("kline", "Pin8", c.pin8, ini.c_str()), c.pin8);
    c.pin78 = pinValue(GetPrivateProfileIntA("kline", "Pin7and8", c.pin78, ini.c_str()), c.pin78);
    c.log = GetPrivateProfileIntA("settings", "Log", c.log, ini.c_str()) != 0;
    return c;
}

bool saveConfig(const Config &c) {
    std::string ini = iniPath();
    const char *f = ini.c_str();
    return WritePrivateProfileStringA("settings", "Log", c.log ? "1" : "0", f) &&
           WritePrivateProfileStringA("kline", "Pin7", std::to_string(c.pin7).c_str(), f) &&
           WritePrivateProfileStringA("kline", "Pin8", std::to_string(c.pin8).c_str(), f) &&
           WritePrivateProfileStringA("kline", "Pin7and8", std::to_string(c.pin78).c_str(), f);
}
