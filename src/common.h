// Shared by the D-PDU DLL and the setup GUI.
#pragma once
#include <string>
#include <vector>

#define APP_NAME   "t2wdpdu"
#define APP_TITLE  "Tech2Win D-PDU J2534 bridge"
#define SHORT_NAME "T2WDPDU_J2534"
#ifndef APP_VERSION
#define APP_VERSION "dev"
#endif

struct Adapter {
    std::string name, vendor, dll;
};

// J2534 v04.04 interfaces from HKLM\SOFTWARE\PassThruSupport.04.04 (32-bit view).
std::vector<Adapter> listAdapters();

// K-line pin override. Values are 7, 8 or 78 (= pins 7 and 8 together).
struct Config {
    int pin7 = 7;        // Tech2Win asks for pin 7
    int pin8 = 8;        // Tech2Win asks for pin 8
    int pin78 = 7;       // Tech2Win asks for pins 7 and 8 at once (bridged outlet -> pin 7)
    bool log = true;
};

std::string dataDir(); // %PUBLIC%, writable by Tech2Win running as a normal user
std::string iniPath();
std::string logPath();
Config loadConfig();
bool saveConfig(const Config &c);
