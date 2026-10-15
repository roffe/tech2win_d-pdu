// t2wdpdu: D-PDU API (ISO 22900-2) for GM Tech2Win on top of any J2534 v04.04 adapter.
//
// Written against what Tech2Win's own D-PDU host does (decompiled, see notes/):
//  - every link is RAW. K-line: TX payload and RESULT data are whole frames incl. checksum.
//    CAN: [CAN id 4 bytes BE][ext addr?][data], ISO-TP is ours (USDT payloads without PCI)
//  - Tech2Win waits for a STATUS item after every call: Connect -> ONLINE, Disconnect -> OFFLINE,
//    UPDATEPARAM/STARTCOMM/STOPCOMM/finite SENDRECV -> FINISHED, NumRecv -1/-2 and every CAN
//    TX -> EXECUTING, Cancel -> CANCELLED. A missing one stalls it for 10 s.
//  - K-line: any ERROR item kills the link on Tech2Win's side until the firmware re-enables it,
//    so ERROR only for a failed init or a finite receive that got nothing
//  - K-line: each request cancels the previous NumRecv -1 SENDRECV, so cancel has to be quick
//  - CAN: one permanent receiver per bus that must never end on its own, TX confirmations for
//    the firmware come only from TX-indication RESULTs, every RESULT needs RxFlag bytes
#include <windows.h>
#include <mmsystem.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "common.h"
#include "j2534.h"
#include "pdu.h"

#define EXPORT extern "C" T_PDU_ERROR __stdcall

typedef std::vector<UNUM8> Bytes;

static Config g_cfg;

// ---------------------------------------------------------------- logging

static std::mutex g_logMu;
static FILE *g_log;

static void logf(const char *fmt, ...) {
    if (!g_cfg.log)
        return;
    char line[4096];
    SYSTEMTIME t;
    GetLocalTime(&t);
    int n = snprintf(line, sizeof(line), "%02d:%02d:%02d.%03d [%5lu] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, GetCurrentThreadId());
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line + n, sizeof(line) - n, fmt, ap);
    va_end(ap);
    std::lock_guard<std::mutex> l(g_logMu);
    if (!g_log) {
        // ponytail: start over past 20 MB instead of rotating
        WIN32_FILE_ATTRIBUTE_DATA fa;
        bool big = GetFileAttributesExA(logPath().c_str(), GetFileExInfoStandard, &fa) && (fa.nFileSizeHigh || fa.nFileSizeLow > (20u << 20));
        g_log = fopen(logPath().c_str(), big ? "w" : "a");
    }
    if (g_log) {
        fprintf(g_log, "%s\n", line);
        fflush(g_log);
    }
}

static std::string hex(const UNUM8 *p, size_t n) {
    std::string s;
    char b[4];
    for (size_t i = 0; i < n; i++) {
        snprintf(b, sizeof(b), i ? " %02X" : "%02X", p[i]);
        s += b;
    }
    return s;
}

static LARGE_INTEGER g_qpf, g_t0;

static UNUM32 nowUs() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (UNUM32)((t.QuadPart - g_t0.QuadPart) * 1000000 / g_qpf.QuadPart);
}

static UNUM32 be32(const UNUM8 *p) { return (UNUM32)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

// ---------------------------------------------------------------- J2534 library

struct PassThru {
    HMODULE h = nullptr;
    PTOPEN Open;
    PTCLOSE Close;
    PTCONNECT Connect;
    PTDISCONNECT Disconnect;
    PTREADMSGS ReadMsgs;
    PTWRITEMSGS WriteMsgs;
    PTSTARTMSGFILTER StartMsgFilter;
    PTGETLASTERROR GetLastError;
    PTIOCTL Ioctl;
    std::mutex mu; // one J2534 call at a time, most vendor DLLs are not reentrant

    // ponytail: never FreeLibrary'd, vendor DLLs don't all survive it and the process ends soon after
    bool load(const std::string &dll) {
        if (h)
            return true;
        if (!(h = LoadLibraryA(dll.c_str())))
            return false;
#define SYM(n) n = (decltype(n))GetProcAddress(h, "PassThru" #n)
        SYM(Open), SYM(Close), SYM(Connect), SYM(Disconnect), SYM(ReadMsgs), SYM(WriteMsgs), SYM(StartMsgFilter), SYM(GetLastError), SYM(Ioctl);
#undef SYM
        if (Open && Close && Connect && Disconnect && ReadMsgs && WriteMsgs && StartMsgFilter && GetLastError && Ioctl)
            return true;
        h = nullptr;
        return false;
    }

    std::string err(long rc) {
        char buf[256] = "";
        GetLastError(buf);
        return std::to_string(rc) + " " + buf;
    }

    long ioctl(unsigned long ch, unsigned long id, void *in, void *out) {
        std::lock_guard<std::mutex> g(mu);
        return Ioctl(ch, id, in, out);
    }

    long setConfig(unsigned long ch, unsigned long param, unsigned long value) {
        SCONFIG c{param, value};
        SCONFIG_LIST l{1, &c};
        return ioctl(ch, SET_CONFIG, &l, nullptr);
    }

    // pass-all filter, J2534 drops everything without one
    long passAll(unsigned long ch, unsigned long proto, unsigned long size) {
        PASSTHRU_MSG mask{}, pattern{};
        mask.ProtocolID = pattern.ProtocolID = proto;
        mask.DataSize = pattern.DataSize = size;
        unsigned long fid;
        std::lock_guard<std::mutex> g(mu);
        return StartMsgFilter(ch, PASS_FILTER, &mask, &pattern, nullptr, &fid);
    }
};

struct CanBus;

struct Module {
    Adapter a;
    std::string name; // VendorModuleName: Tech2Win finds the module again by strcmp, no spaces
    PassThru pt;
    unsigned long dev = 0;
    bool open = false;
    std::map<std::string, std::shared_ptr<CanBus>> buses; // CAN buses in use, guarded by g_mu
};

static std::mutex g_mu; // guards everything below
static std::vector<std::unique_ptr<Module>> g_mods; // hMod = index + 1
static std::map<std::string, UNUM32> g_ids;         // shortname -> object id

static Module *module(UNUM32 hMod) {
    return hMod >= 1 && hMod <= g_mods.size() ? g_mods[hMod - 1].get() : nullptr;
}

static std::string idName(UNUM32 id) {
    for (auto &kv : g_ids)
        if (kv.second == id)
            return kv.first;
    return "#" + std::to_string(id);
}

static void scanModules() {
    if (!g_mods.empty())
        return;
    for (auto &a : listAdapters()) {
        auto m = std::make_unique<Module>();
        m->a = a;
        m->name = a.name;
        std::replace(m->name.begin(), m->name.end(), ' ', '_');
        logf("module %u: %s (%s) %s", (unsigned)g_mods.size() + 1, m->name.c_str(), a.vendor.c_str(), a.dll.c_str());
        g_mods.push_back(std::move(m));
    }
}

// ---------------------------------------------------------------- ComParams

// Tech2Win only ever sets UNUM32 ComParams (times in us). Kept by shortname.
typedef std::map<std::string, UNUM32> Params;

static bool has(const Params &p, const char *name) { return p.count(name) != 0; }
static UNUM32 pget(const Params &p, const char *name, UNUM32 def) {
    auto it = p.find(name);
    return it == p.end() ? def : it->second;
}

// ---------------------------------------------------------------- protocols

#define INIT_5BAUD 1
#define INIT_FAST  2
#define INIT_NONE  3

struct Proto {
    const char *name;
    unsigned long j2534, j2534ps, baud;
    UNUM32 init;      // default CP_InitializationSettings
    UNUM32 fiveBaud;  // default CP_5BaudMode (same values as J2534 FIVE_BAUD_MOD)
};

static const Proto PROTOS[] = {
    {"ISO_14230_3_on_ISO_14230_2", ISO14230, ISO14230_PS, 10400, INIT_FAST, 0},  // KW2000, most OG 9-5/9-3 modules
    {"KW82_on_KW_UART", ISO9141, ISO9141_PS, 8192, INIT_5BAUD, 1},               // SRS OG 9-3 / early OG 9-5
    {"KW81_on_KW_UART", ISO9141, ISO9141_PS, 9600, INIT_5BAUD, 1},
    {"SAE_J2190_on_ISO_9141_2", ISO9141, ISO9141_PS, 10400, INIT_5BAUD, 0},      // KW08/KW94, SDL (no init)
};

// CAN links: GMLAN (NG 9-3, 9-5 NG) and OBD on CAN. J1939 and fault-tolerant CAN aren't SAAB.
static bool isCanProto(const std::string &p) {
    return p == "ISO_15765_3_on_ISO_15765_2" || p == "ISO_15031_5_on_ISO_15765_4" || p == "ISO_11898_RAW";
}

// ---------------------------------------------------------------- links

struct ER { // expected response of a receive CoP
    UNUM32 acc;
    Bytes mask, pattern;
};

struct CoP {
    UNUM32 h, type;
    Bytes data;
    SNUM32 sendCycles, recvCycles;
    UNUM32 time, txFlag = 0;
    void *tag;
    Params params;
    std::vector<ER> ers;
    bool cancel = false, done = false; // guarded by Link::mu
    bool execPosted = false;
};

struct UrEntry { // one UniqueRespIdTable row
    UNUM32 urid, reqId, reqFmt, reqExt, usdtId, usdtFmt, usdtExt, uudtId;
};

struct Pending { // RX for a CAN receiver that is being re-armed
    Bytes data;
    UNUM32 flag;
    DWORD t;
};

struct Link {
    UNUM32 h, hMod;
    Module *mod;
    void *tag;
    CALLBACKFNC cb = nullptr;
    std::string bus, protoName;
    const Proto *proto = nullptr; // K-line
    std::shared_ptr<CanBus> can;  // CAN
    bool canRaw = false;          // ISO_11898_RAW link (TX only)
    int askedK = 0, askedL = 0;   // K-line pins Tech2Win asked for
    int pins = 7;                 // what we use: 7, 8 or 78 (K on 7 + L on 8)
    unsigned long baud = 0;
    Params working, active;
    std::vector<UrEntry> table; // CAN UniqueRespIdTable, guarded by mu
    std::deque<Pending> pending;

    unsigned long ch = 0; // K-line J2534 channel
    bool chOpen = false, connected = false;
    UNUM32 status = PDU_CLLST_OFFLINE;

    std::mutex mu; // guards the queues, CoP flags and status
    std::condition_variable cv;
    std::deque<std::shared_ptr<CoP>> cops;
    std::shared_ptr<CoP> current;  // CoP being executed
    std::shared_ptr<CoP> listener; // NumRecv -1 SENDRECV, gets every frame until cancelled
    std::deque<PDU_EVENT_ITEM *> events;
    std::thread worker;
    bool quit = false;
};

static std::map<UNUM32, std::shared_ptr<Link>> g_links;
static UNUM32 g_nextLink = 1;
static std::atomic<UNUM32> g_nextCop{1};

static std::shared_ptr<Link> findLink(UNUM32 hMod, UNUM32 hCLL) {
    std::lock_guard<std::mutex> g(g_mu);
    auto it = g_links.find(hCLL);
    return it != g_links.end() && it->second->hMod == hMod ? it->second : nullptr;
}

// ---------------------------------------------------------------- events
// Every item is one calloc block (item + data + payload), PDUDestroyItem frees it.

static PDU_EVENT_ITEM *newEvent(T_PDU_IT type, UNUM32 hCop, void *tag, size_t extra) {
    auto *e = (PDU_EVENT_ITEM *)calloc(1, sizeof(PDU_EVENT_ITEM) + extra);
    e->ItemType = type;
    e->hCop = hCop;
    e->pCoPTag = tag;
    e->pData = e + 1;
    e->Timestamp = nowUs();
    return e;
}

static void post(Link &l, PDU_EVENT_ITEM *e) {
    CALLBACKFNC cb;
    {
        std::lock_guard<std::mutex> g(l.mu);
        if (l.quit) { // link is being destroyed, Tech2Win won't drain it again
            free(e);
            return;
        }
        l.events.push_back(e);
        cb = l.cb;
    }
    if (cb)
        cb(PDU_EVT_DATA_AVAILABLE, l.hMod, l.h, l.tag, nullptr);
}

static void postStatus(Link &l, UNUM32 hCop, void *tag, T_PDU_STATUS st) {
    auto *e = newEvent(PDU_IT_STATUS, hCop, tag, sizeof(T_PDU_STATUS));
    *(T_PDU_STATUS *)e->pData = st;
    logf("CLL %u status hCoP=%d 0x%X", (unsigned)l.h, (int)hCop, (unsigned)st);
    post(l, e);
}

static void setLinkStatus(Link &l, T_PDU_STATUS st, bool always = false) {
    {
        std::lock_guard<std::mutex> g(l.mu);
        if (l.status == st && !always)
            return;
        l.status = st;
    }
    postStatus(l, PDU_HANDLE_UNDEF, nullptr, st);
}

static void postError(Link &l, const CoP &c, T_PDU_ERR_EVT code) {
    auto *e = newEvent(PDU_IT_ERROR, c.h, c.tag, sizeof(PDU_ERROR_DATA));
    ((PDU_ERROR_DATA *)e->pData)->ErrorCodeId = code;
    logf("CLL %u error hCoP=%u 0x%X", (unsigned)l.h, (unsigned)c.h, (unsigned)code);
    post(l, e);
}

// RxFlag bits (byte 0 = MSB, same positions as the J2534 RxStatus bits)
#define RXF_TX_INDICATION 0x0008
#define RXF_USDT          0x0040 // ISO-TP message, PCI stripped (Tech2Win: CANdi "paired mailbox")
#define RXF_EXT_ADDR      0x0080
#define RXF_29BIT         0x0100

// Tech2Win reads RxFlag.pFlagData[2..3] on CAN without checking, so it is always 4 bytes.
static void postResult(Link &l, const CoP &c, const UNUM8 *data, size_t n, UNUM32 txDone, UNUM32 rxStart, UNUM32 rxFlag = 0, UNUM32 acc = 1) {
    auto *e = newEvent(PDU_IT_RESULT, c.h, c.tag, sizeof(PDU_RESULT_DATA) + 4 + n);
    auto *r = (PDU_RESULT_DATA *)e->pData;
    UNUM8 *flag = (UNUM8 *)(r + 1);
    flag[0] = rxFlag >> 24, flag[1] = rxFlag >> 16, flag[2] = rxFlag >> 8, flag[3] = rxFlag;
    r->RxFlag.NumFlagBytes = 4;
    r->RxFlag.pFlagData = flag;
    r->UniqueRespIdentifier = PDU_ID_UNDEF;
    r->AcceptanceId = acc;
    r->TxMsgDoneTimestamp = txDone;
    r->StartMsgTimestamp = rxStart;
    r->NumDataBytes = n;
    r->pDataBytes = flag + 4;
    memcpy(r->pDataBytes, data, n);
    logf("CLL %u RX  hCoP=%u%s %s", (unsigned)l.h, (unsigned)c.h, rxFlag & RXF_TX_INDICATION ? " TX done" : "", hex(data, n).c_str());
    post(l, e);
}

static bool cancelled(Link &l, const CoP &c) {
    std::lock_guard<std::mutex> g(l.mu);
    return c.cancel;
}

// ---------------------------------------------------------------- K-line over J2534

// Pins: Tech2Win passes the firmware's PIN_A as K and PIN_B (if set) as L. Pin 7 + 8 together
// is K on 7 and L on 8, which a K-only adapter like the Mongoose Pro GM II can't do. The user
// picks what to do with 7, 8 and 7+8 in the setup GUI.
static void pickPins(Link &l, const std::vector<PDU_PIN_DATA> &pins) {
    for (auto &p : pins) {
        std::string t = idName(p.DLCPinTypeId);
        if (t == "L")
            l.askedL = p.DLCPinNumber;
        else if (!l.askedK)
            l.askedK = p.DLCPinNumber;
    }
    int k = l.askedK ? l.askedK : 7, L = l.askedL;
    if ((k == 7 && L == 8) || (k == 8 && L == 7))
        l.pins = g_cfg.pin78;
    else if (k == 8)
        l.pins = g_cfg.pin8;
    else
        l.pins = g_cfg.pin7; // ponytail: other pins (not used by SAAB) end up on 7 too
}

static void klineClose(Link &l) {
    if (!l.chOpen)
        return;
    std::lock_guard<std::mutex> g(l.mod->pt.mu);
    l.mod->pt.Disconnect(l.ch);
    l.chOpen = false;
}

static bool klineOpen(Link &l) {
    PassThru &pt = l.mod->pt;
    // ponytail: plain ISO14230/9141 for pin 7, every adapter does that. _PS (J2534-2) only for 8 / 7+8.
    bool ps = l.pins != 7;
    unsigned long proto = ps ? l.proto->j2534ps : l.proto->j2534;
    unsigned long flags = ISO9141_NO_CHECKSUM | (l.pins == 78 ? 0 : ISO9141_K_LINE_ONLY);
    l.baud = pget(l.active, "CP_Baudrate", l.proto->baud);
    logf("CLL %u connect: asked K=%d L=%d, using %s -> proto 0x%lX flags 0x%lX baud %lu", (unsigned)l.h, l.askedK, l.askedL,
         l.pins == 78 ? "K7+L8" : l.pins == 8 ? "K8" : "K7", proto, flags, l.baud);
    long rc;
    {
        std::lock_guard<std::mutex> g(pt.mu);
        rc = pt.Connect(l.mod->dev, proto, flags, l.baud, &l.ch);
    }
    if (rc != STATUS_NOERROR) {
        logf("  PassThruConnect failed: %s", pt.err(rc).c_str());
        return false;
    }
    l.chOpen = true;
    if (ps) {
        unsigned long v = l.pins == 78 ? 0x0708 : 0x0800;
        if ((rc = pt.setConfig(l.ch, J1962_PINS, v)) != STATUS_NOERROR) {
            logf("  J1962_PINS 0x%04lX failed: %s (adapter can't do this pin, see the K-Line pins setup)", v, pt.err(rc).c_str());
            klineClose(l);
            return false;
        }
    }
    pt.setConfig(l.ch, LOOPBACK, 0);
    if ((rc = pt.passAll(l.ch, proto, 1)) != STATUS_NOERROR)
        logf("  PassThruStartMsgFilter failed: %s", pt.err(rc).c_str());
    return true;
}

// ComParams -> J2534 config, on every UPDATEPARAM. Times are us in D-PDU, ms or 0.5 ms in J2534.
static void klineParams(Link &l) {
    PassThru &pt = l.mod->pt;
    const Params &p = l.active;
    std::vector<SCONFIG> cfg;
    UNUM32 baud = pget(p, "CP_Baudrate", l.baud);
    if (baud != l.baud) {
        cfg.push_back({DATA_RATE, baud});
        l.baud = baud;
    }
    if (has(p, "CP_P3Min"))
        cfg.push_back({P3_MIN, pget(p, "CP_P3Min", 0) / 500});
    if (has(p, "CP_P4Min"))
        cfg.push_back({P4_MIN, pget(p, "CP_P4Min", 0) / 500});
    UNUM32 init = pget(p, "CP_InitializationSettings", l.proto->init);
    if (init == INIT_FAST) {
        // Tech2Win asks for TIdle 1 ms: the adapter default 300 ms makes the StartComm reply too late
        cfg.push_back({TIDLE, pget(p, "CP_TIdle", 300000) / 1000});
        cfg.push_back({TINIL, pget(p, "CP_TInil", 25000) / 1000});
        cfg.push_back({TWUP, pget(p, "CP_TWup", 50000) / 1000});
    } else if (init == INIT_5BAUD) {
        cfg.push_back({FIVE_BAUD_MOD, pget(p, "CP_5BaudMode", l.proto->fiveBaud)});
        // ponytail: CP_TIdle is 2.1 s (KW82) / 7 s (KW08) here, capped to the J2534 default so the
        // init still lands inside the firmware's window. Raise it if an ECU misses the address.
        unsigned long idle = std::min<UNUM32>(pget(p, "CP_TIdle", 300000) / 1000, 300);
        cfg.push_back({W0, idle});
        cfg.push_back({W5, idle});
        if (has(p, "CP_W1Max"))
            cfg.push_back({W1, pget(p, "CP_W1Max", 0) / 1000});
        if (has(p, "CP_W2Max"))
            cfg.push_back({W2, pget(p, "CP_W2Max", 0) / 1000});
        if (has(p, "CP_W3Max"))
            cfg.push_back({W3, pget(p, "CP_W3Max", 0) / 1000});
        if (has(p, "CP_W4Min"))
            cfg.push_back({W4, pget(p, "CP_W4Min", 0) / 1000});
    }
    for (auto &c : cfg) {
        long rc = pt.setConfig(l.ch, c.Parameter, c.Value);
        logf("  SET_CONFIG 0x%02lX = %lu%s", c.Parameter, c.Value, rc ? (" failed: " + pt.err(rc)).c_str() : "");
    }
}

// One frame from the adapter, skipping start-of-message indications and loopback.
// Returns ERR_TIMEOUT when nothing came in time or the CoP got cancelled.
static long readFrame(Link &l, const CoP &c, PASSTHRU_MSG &m, unsigned long timeoutMs, UNUM32 &startUs) {
    PassThru &pt = l.mod->pt;
    DWORD end = GetTickCount() + timeoutMs;
    startUs = 0;
    for (;;) {
        if (cancelled(l, c))
            return ERR_TIMEOUT;
        long left = (long)(end - GetTickCount());
        if (left < 0)
            return ERR_TIMEOUT;
        unsigned long n = 1;
        long rc;
        {
            std::lock_guard<std::mutex> g(pt.mu);
            rc = pt.ReadMsgs(l.ch, &m, &n, std::min(left, 10L));
        }
        if (rc == ERR_TIMEOUT || rc == ERR_BUFFER_EMPTY || (rc == STATUS_NOERROR && n == 0))
            continue;
        if (rc != STATUS_NOERROR)
            return rc;
        if (m.RxStatus & START_OF_MESSAGE) {
            startUs = nowUs();
            continue;
        }
        if ((m.RxStatus & TX_MSG_TYPE) || m.DataSize == 0)
            continue;
        if (!startUs)
            startUs = nowUs();
        return STATUS_NOERROR;
    }
}

// KWP2000 7F xx 78: the ECU asks for more time, the real answer follows
static bool responsePending(const Link &l, const PASSTHRU_MSG &m) {
    if (l.proto->j2534 != ISO14230)
        return false;
    unsigned hdr = ((m.Data[0] & 0xC0) ? 3 : 1) + ((m.Data[0] & 0x3F) == 0 ? 1 : 0);
    return m.DataSize >= hdr + 3 && m.Data[hdr] == 0x7F && m.Data[hdr + 2] == 0x78;
}

static void klineStartComm(Link &l, CoP &c) {
    PassThru &pt = l.mod->pt;
    UNUM32 init = pget(c.params, "CP_InitializationSettings", l.proto->init);
    long rc;
    if (init == INIT_5BAUD) {
        // RESULT must be exactly [KB1 KB2], Tech2Win replays the handshake to the firmware itself
        unsigned char addr = pget(c.params, "CP_5BaudAddressPhys", 0x33), kb[2] = {};
        SBYTE_ARRAY in{1, &addr}, out{2, kb};
        rc = pt.ioctl(l.ch, FIVE_BAUD_INIT, &in, &out);
        if (rc != STATUS_NOERROR) {
            logf("  FIVE_BAUD_INIT 0x%02X failed: %s", addr, pt.err(rc).c_str());
            postError(l, c, PDU_ERR_EVT_INIT_ERROR);
            return;
        }
        logf("  5-baud init 0x%02X ok", addr);
        postResult(l, c, kb, 2, 0, nowUs());
    } else if (init == INIT_FAST) {
        PASSTHRU_MSG req{}, resp{};
        req.ProtocolID = resp.ProtocolID = l.proto->j2534;
        req.DataSize = c.data.size();
        memcpy(req.Data, c.data.data(), c.data.size());
        pt.ioctl(l.ch, CLEAR_RX_BUFFER, nullptr, nullptr);
        UNUM32 t0 = nowUs();
        rc = pt.ioctl(l.ch, FAST_INIT, &req, &resp);
        if (rc != STATUS_NOERROR || resp.DataSize == 0) {
            logf("  FAST_INIT failed after %u ms: %s", (unsigned)((nowUs() - t0) / 1000), pt.err(rc).c_str());
            postError(l, c, PDU_ERR_EVT_INIT_ERROR);
            return;
        }
        logf("  fast init took %u ms", (unsigned)((nowUs() - t0) / 1000));
        postResult(l, c, resp.Data, resp.DataSize, t0, nowUs());
    }
    setLinkStatus(l, PDU_CLLST_COMM_STARTED);
}

// Returns false when the CoP turned into the link's listener (NumRecv -1, no FINISHED yet).
static bool klineSendRecv(Link &l, std::shared_ptr<CoP> cp) {
    CoP &c = *cp;
    PassThru &pt = l.mod->pt;
    UNUM32 txDone = 0;
    if (!c.data.empty() && c.sendCycles != 0) {
        PASSTHRU_MSG m{};
        m.ProtocolID = l.proto->j2534;
        m.DataSize = c.data.size();
        memcpy(m.Data, c.data.data(), c.data.size());
        unsigned long n = 1;
        bool listening;
        {
            std::lock_guard<std::mutex> g(l.mu);
            listening = l.listener != nullptr;
        }
        if (!listening) // stale frames would end up as the answer to this request
            pt.ioctl(l.ch, CLEAR_RX_BUFFER, nullptr, nullptr);
        long rc;
        {
            std::lock_guard<std::mutex> g(pt.mu);
            rc = pt.WriteMsgs(l.ch, &m, &n, 1000);
        }
        txDone = nowUs();
        logf("CLL %u TX  hCoP=%u %s", (unsigned)l.h, (unsigned)c.h, hex(m.Data, m.DataSize).c_str());
        if (rc != STATUS_NOERROR) {
            logf("  PassThruWriteMsgs failed: %s", pt.err(rc).c_str());
            postError(l, c, PDU_ERR_EVT_TX_ERROR);
            return true;
        }
    }
    if (c.recvCycles == -1) {
        std::lock_guard<std::mutex> g(l.mu);
        if (!c.cancel)
            l.listener = cp;
        return c.cancel;
    }
    // finite receive: n answers, -2 = whatever comes within P2max
    int got = 0;
    unsigned long p2 = pget(c.params, "CP_P2Max", 50000) / 1000 + 50, timeout = p2;
    while (c.recvCycles == -2 || got < c.recvCycles) {
        PASSTHRU_MSG m;
        UNUM32 start;
        long rc = readFrame(l, c, m, timeout, start);
        if (rc == ERR_TIMEOUT) {
            if (c.recvCycles > 0 && !cancelled(l, c))
                postError(l, c, PDU_ERR_EVT_RX_TIMEOUT);
            break;
        }
        if (rc != STATUS_NOERROR) {
            logf("  PassThruReadMsgs failed: %s", pt.err(rc).c_str());
            postError(l, c, PDU_ERR_EVT_RX_ERROR);
            break;
        }
        postResult(l, c, m.Data, m.DataSize, txDone, start);
        bool pending = responsePending(l, m);
        timeout = pending ? pget(c.params, "CP_P2Star", 5000000) / 1000 : p2;
        if (!pending)
            got++;
    }
    return true;
}

// ---------------------------------------------------------------- CAN over J2534
//
// One raw J2534 CAN channel per bus, shared by the two CLLs Tech2Win makes per CANdi channel
// (ISO 15765 + ISO_11898_RAW). ISO-TP is done here, paired by the UniqueRespIdTable: the
// session has 25+ USDT pairs on SW-CAN alone, more than adapters take as flow-control filters.
// The channel opens when a bus first gets a table or a TX: SAAB creates MS-CAN but never uses
// it, and it shouldn't take a CAN controller away from HS.

#define CAN_PROTO     0x05
#define CAN_PS        0x8004
#define SW_CAN_PS     0x8008
#define CAN_29BIT_ID  0x0100
#define CAN_ID_BOTH   0x0800
#define SW_CAN_HV_TX  0x0400
#define SW_CAN_HS     0x8000
#define SW_CAN_NS     0x8001
#define ERR_DEVICE_NOT_CONNECTED 0x08

struct CanBus {
    Module *mod;
    std::string name;
    unsigned long proto, pins, baud;
    unsigned long ch = 0;
    bool open = false, failed = false;
    std::mutex openMu; // open/close come from Tech2Win's thread and the link workers
    std::atomic<bool> quit{false};
    std::thread rx;

    std::mutex mu; // links, fc
    std::condition_variable fcCv;
    std::vector<std::shared_ptr<Link>> links;
    struct Fc {
        bool got = false;
        UNUM8 fs = 0, bs = 0, st = 0;
    };
    std::map<UNUM32, Fc> fc; // flow control we wait for, by the ECU's response id

    struct Rx { // ISO-TP reassembly, RX thread only
        Bytes buf;
        size_t want;
        UNUM8 sn;
        DWORD t;
    };
    std::map<UNUM32, Rx> reasm;
};

static void canFrameMsg(const CanBus &b, PASSTHRU_MSG &m, UNUM32 id, const Bytes &d, bool x29, bool hv) {
    memset(&m, 0, sizeof(m) - sizeof(m.Data));
    m.ProtocolID = b.proto;
    m.TxFlags = (x29 ? CAN_29BIT_ID : 0) | (hv ? SW_CAN_HV_TX : 0);
    m.Data[0] = id >> 24, m.Data[1] = id >> 16, m.Data[2] = id >> 8, m.Data[3] = id;
    memcpy(m.Data + 4, d.data(), d.size());
    m.DataSize = 4 + d.size();
}

static long canWrite(CanBus &b, UNUM32 id, const std::vector<Bytes> &frames, bool x29, bool hv = false) {
    std::vector<PASSTHRU_MSG> msgs(frames.size());
    for (size_t i = 0; i < frames.size(); i++)
        canFrameMsg(b, msgs[i], id, frames[i], x29, hv);
    unsigned long n = msgs.size();
    PassThru &pt = b.mod->pt;
    std::lock_guard<std::mutex> g(pt.mu);
    // ~130 bits per padded frame, twice that as margin: a 4 KB SPS block on 33.3k SW-CAN takes seconds
    long rc = pt.WriteMsgs(b.ch, msgs.data(), &n, 200 + n * 260000 / std::max(b.baud, 10000ul));
    return rc == STATUS_NOERROR && n != msgs.size() ? ERR_TIMEOUT : rc;
}

static Bytes padded(Bytes f, bool pad) {
    if (pad)
        f.resize(8, 0x00); // the real CANdi pads with zeros
    return f;
}

static bool erMatch(const ER &e, const Bytes &d) {
    for (size_t i = 0; i < e.mask.size(); i++)
        if (i >= d.size() || (d[i] & e.mask[i]) != (e.pattern[i] & e.mask[i]))
            return false;
    return true;
}

// Hand a received message to the link's permanent receiver, AcceptanceId from the first
// matching expected response (the firmware's mailbox index; lowest index wins).
static void canPost(Link &l, const CoP &lis, const Bytes &d, UNUM32 flag) {
    if (lis.ers.empty()) {
        postResult(l, lis, d.data(), d.size(), 0, nowUs(), flag, 0);
        return;
    }
    for (auto &e : lis.ers)
        if (erMatch(e, d)) {
            postResult(l, lis, d.data(), d.size(), 0, nowUs(), flag, e.acc);
            return;
        }
}

static void canDeliver(Link &l, Bytes d, UNUM32 flag) {
    std::shared_ptr<CoP> lis;
    {
        std::lock_guard<std::mutex> g(l.mu);
        lis = l.listener;
        if (!lis) { // receiver being re-armed (Tech2Win does that on every mailbox write): keep it
            DWORD now = GetTickCount();
            while (!l.pending.empty() && (now - l.pending.front().t > 1000 || l.pending.size() >= 256))
                l.pending.pop_front();
            l.pending.push_back({std::move(d), flag, now});
            return;
        }
    }
    canPost(l, *lis, d, flag);
}

static bool findUsdt(Link &l, UNUM32 id, UrEntry &out) {
    std::lock_guard<std::mutex> g(l.mu);
    for (auto &e : l.table)
        if (e.usdtId && e.usdtId == id) {
            out = e;
            return true;
        }
    return false;
}

static UNUM32 pairedResp(Link &l, UNUM32 reqId) {
    std::lock_guard<std::mutex> g(l.mu);
    for (auto &e : l.table)
        if (e.reqId == reqId && e.usdtId)
            return e.usdtId;
    return 0;
}

static void canRxFrame(CanBus &b, UNUM32 id, bool x29, const UNUM8 *d, size_t n) {
    std::vector<std::shared_ptr<Link>> links;
    {
        std::lock_guard<std::mutex> g(b.mu);
        links = b.links;
    }
    Bytes head{(UNUM8)(id >> 24), (UNUM8)(id >> 16), (UNUM8)(id >> 8), (UNUM8)id};
    UNUM32 base = x29 ? RXF_29BIT : 0;
    for (auto &lp : links) {
        Link &l = *lp;
        if (l.canRaw)
            continue; // nothing ever receives on the RAW CLL
        UrEntry e;
        if (!findUsdt(l, id, e)) { // UUDT / plain frame: all bytes as on the wire
            Bytes r(head);
            r.insert(r.end(), d, d + n);
            canDeliver(l, std::move(r), base);
            continue;
        }
        size_t o = (e.usdtFmt & 0x08) ? 1 : 0; // extended addressing: first byte is the address
        if (n < o + 1)
            continue;
        UNUM32 flag = base | RXF_USDT | (o ? RXF_EXT_ADDR : 0);
        Bytes r(head);
        if (o)
            r.push_back(d[0]);
        UNUM8 pci = d[o];
        switch (pci >> 4) {
        case 0: { // single frame
            size_t len = pci & 0x0F;
            if (len == 0 || len > n - o - 1)
                break;
            r.insert(r.end(), d + o + 1, d + o + 1 + len);
            canDeliver(l, std::move(r), flag);
            break;
        }
        case 1: { // first frame: flow control to the paired request id, BS 0 STmin 0
            if (n < o + 2)
                break;
            CanBus::Rx &s = b.reasm[id];
            s.want = (pci & 0x0F) << 8 | d[o + 1];
            s.buf = r;
            s.buf.insert(s.buf.end(), d + o + 2, d + n);
            s.sn = 1;
            s.t = GetTickCount();
            Bytes fc;
            if (e.reqFmt & 0x08)
                fc.push_back(e.reqExt);
            fc.insert(fc.end(), {0x30, 0x00, 0x00});
            long rc = canWrite(b, e.reqId, {padded(fc, true)}, (e.reqFmt & 0x02) != 0);
            if (rc != STATUS_NOERROR)
                logf("%s: flow control to 0x%X failed: %s", b.name.c_str(), (unsigned)e.reqId, b.mod->pt.err(rc).c_str());
            break;
        }
        case 2: { // consecutive frame
            auto it = b.reasm.find(id);
            if (it == b.reasm.end())
                break;
            CanBus::Rx &s = it->second;
            size_t have = s.buf.size() - r.size();
            if ((pci & 0x0F) != (s.sn & 0x0F) || GetTickCount() - s.t > 1000) { // lost frame or N_Cr timeout
                b.reasm.erase(it);
                break;
            }
            s.sn++;
            s.t = GetTickCount();
            s.buf.insert(s.buf.end(), d + o + 1, d + o + 1 + std::min(n - o - 1, s.want - have));
            if (s.buf.size() - r.size() >= s.want) {
                canDeliver(l, std::move(s.buf), flag);
                b.reasm.erase(it);
            }
            break;
        }
        case 3: { // flow control for one of our segmented TX
            std::lock_guard<std::mutex> g(b.mu);
            auto it = b.fc.find(id);
            if (it != b.fc.end() && n >= o + 3) {
                it->second = {true, (UNUM8)(pci & 0x0F), d[o + 1], d[o + 2]};
                b.fcCv.notify_all();
            }
            break;
        }
        }
    }
}

static void canRxThread(CanBus *b) {
    PASSTHRU_MSG msgs[16];
    while (!b->quit) {
        unsigned long n = 16;
        long rc;
        {
            std::lock_guard<std::mutex> g(b->mod->pt.mu);
            rc = b->mod->pt.ReadMsgs(b->ch, msgs, &n, 0); // non-blocking: TX must not wait behind us
        }
        if (rc != STATUS_NOERROR || n == 0) {
            if (rc == ERR_DEVICE_NOT_CONNECTED) {
                logf("%s: adapter gone", b->name.c_str());
                return;
            }
            Sleep(1);
            continue;
        }
        for (unsigned long i = 0; i < n; i++) {
            PASSTHRU_MSG &m = msgs[i];
            if ((m.RxStatus & TX_MSG_TYPE) || m.DataSize < 4)
                continue;
            canRxFrame(*b, be32(m.Data) & 0x1FFFFFFF, (m.RxStatus & CAN_29BIT_ID) != 0, m.Data + 4, m.DataSize - 4);
        }
    }
}

static bool canOpen(CanBus &b) {
    std::lock_guard<std::mutex> og(b.openMu);
    if (b.open || b.failed)
        return b.open;
    PassThru &pt = b.mod->pt;
    long rc;
    {
        std::lock_guard<std::mutex> g(pt.mu);
        rc = pt.Connect(b.mod->dev, b.proto, CAN_ID_BOTH, b.baud, &b.ch);
        if (rc != STATUS_NOERROR) // not every driver takes CAN_ID_BOTH; GMLAN is 11-bit
            rc = pt.Connect(b.mod->dev, b.proto, 0, b.baud, &b.ch);
    }
    logf("%s: connect proto 0x%lX pins 0x%04lX baud %lu%s", b.name.c_str(), b.proto, b.pins, b.baud, rc ? (" failed: " + pt.err(rc)).c_str() : "");
    if (rc == STATUS_NOERROR && b.pins && (rc = pt.setConfig(b.ch, J1962_PINS, b.pins)) != STATUS_NOERROR) {
        logf("  J1962_PINS 0x%04lX failed: %s", b.pins, pt.err(rc).c_str());
        std::lock_guard<std::mutex> g(pt.mu);
        pt.Disconnect(b.ch);
    }
    if (rc != STATUS_NOERROR) {
        // ponytail: the bus stays virtual (every call ok, nothing on the wire) until Tech2Win
        // re-creates the channel, as an adapter without that bus has to look to Tech2Win
        b.failed = true;
        return false;
    }
    pt.setConfig(b.ch, LOOPBACK, 0);
    if ((rc = pt.passAll(b.ch, b.proto, 4)) != STATUS_NOERROR)
        logf("  PassThruStartMsgFilter failed: %s", pt.err(rc).c_str());
    if (b.proto == SW_CAN_PS && b.baud > 33333)
        pt.ioctl(b.ch, SW_CAN_HS, nullptr, nullptr);
    timeBeginPeriod(1); // Sleep(1) in the RX poll should be 1 ms, not 15.6
    b.open = true;
    b.quit = false;
    b.rx = std::thread(canRxThread, &b);
    return true;
}

static void canClose(CanBus &b) {
    std::lock_guard<std::mutex> og(b.openMu);
    if (!b.open)
        return;
    b.quit = true;
    if (b.rx.joinable())
        b.rx.join();
    {
        std::lock_guard<std::mutex> g(b.mod->pt.mu);
        b.mod->pt.Disconnect(b.ch);
    }
    timeEndPeriod(1);
    b.open = false;
    logf("%s: closed", b.name.c_str());
}

// Bus from the CLL's bus type and pins. SAAB: SW-CAN pin 1, MS 3/11, HS 6/14.
static std::shared_ptr<CanBus> canAttach(std::shared_ptr<Link> l, const std::vector<PDU_PIN_DATA> &pins) {
    unsigned long proto, pinsCfg = 0, baud;
    std::string name;
    int hi = 0, lo = 0;
    for (auto &p : pins) {
        std::string t = idName(p.DLCPinTypeId);
        if (t == "LOW")
            lo = p.DLCPinNumber;
        else if (!hi)
            hi = p.DLCPinNumber;
    }
    if (l->bus == "SAE_J2411_SWCAN") {
        name = "SW-CAN", proto = SW_CAN_PS, pinsCfg = 0x0100, baud = 33333;
    } else if (l->bus == "ISO_11898_2_DWCAN" && hi == 6 && lo == 14) {
        name = "HS-CAN", proto = CAN_PROTO, baud = 500000;
    } else if (l->bus == "ISO_11898_2_DWCAN" && hi && lo) {
        name = hi == 3 && lo == 11 ? "MS-CAN" : "CAN " + std::to_string(hi) + "/" + std::to_string(lo);
        proto = CAN_PS, pinsCfg = hi << 8 | lo, baud = hi == 3 ? 95238 : 500000;
    } else {
        return nullptr;
    }
    auto &slot = l->mod->buses[name];
    if (!slot) {
        slot = std::make_shared<CanBus>();
        slot->mod = l->mod;
        slot->name = name;
        slot->proto = proto;
        slot->pins = pinsCfg;
        slot->baud = baud;
    }
    std::lock_guard<std::mutex> g(slot->mu);
    slot->links.push_back(l);
    return slot;
}

// Drop the link from its bus, close the bus with the last one (called without locks held)
static void canDetach(Link &l) {
    std::shared_ptr<CanBus> b = l.can;
    if (!b)
        return;
    bool last;
    {
        std::lock_guard<std::mutex> g(b->mu);
        b->links.erase(std::remove_if(b->links.begin(), b->links.end(), [&](auto &p) { return p.get() == &l; }), b->links.end());
        last = b->links.empty();
    }
    if (!last)
        return;
    canClose(*b);
    std::lock_guard<std::mutex> g(g_mu);
    auto it = l.mod->buses.find(b->name);
    if (it != l.mod->buses.end() && it->second == b)
        l.mod->buses.erase(it);
}

// CP_Baudrate can change on a live channel (bit timing write, 83.3k SW-CAN)
static void canParams(Link &l) {
    CanBus &b = *l.can;
    UNUM32 baud = pget(l.active, "CP_Baudrate", b.baud);
    if (baud == b.baud)
        return;
    logf("%s: baud %lu -> %u", b.name.c_str(), b.baud, (unsigned)baud);
    b.baud = baud;
    if (!b.open)
        return;
    PassThru &pt = b.mod->pt;
    long rc = b.proto == SW_CAN_PS && (baud == 83333 || baud == 33333) ? pt.ioctl(b.ch, baud > 33333 ? SW_CAN_HS : SW_CAN_NS, nullptr, nullptr)
                                                                       : pt.setConfig(b.ch, DATA_RATE, baud);
    if (rc != STATUS_NOERROR)
        logf("  baud change failed: %s", pt.err(rc).c_str());
}

// One TX CoP: [id BE][ext?][data]. ISO 15765 CLL: data is a USDT payload, segmented here.
// RAW CLL: one frame as is. TxFlag 0x40 = pad to 8, 0x80 = ext address, 0x100 = 29-bit id.
static bool canTx(Link &l, CoP &c) {
    CanBus &b = *l.can;
    if (c.data.size() < 4)
        return false;
    if (!canOpen(b)) {
        logf("%s: not available, TX dropped", b.name.c_str());
        return false;
    }
    UNUM32 id = be32(c.data.data()) & 0x1FFFFFFF;
    bool x29 = c.txFlag & 0x100, pad = c.txFlag & 0x40, hv = pget(c.params, "CP_SwCan_HighVoltage", 0) == 1;
    Bytes p(c.data.begin() + 4, c.data.end());
    logf("CLL %u TX  hCoP=%u id 0x%X%s %s", (unsigned)l.h, (unsigned)c.h, (unsigned)id, hv ? " HV" : "", hex(p.data(), p.size()).c_str());
    if (l.canRaw)
        return canWrite(b, id, {padded(p, pad && !p.empty())}, x29, hv) == STATUS_NOERROR;

    Bytes pre;
    if ((c.txFlag & 0x80) && !p.empty()) {
        pre.push_back(p[0]);
        p.erase(p.begin());
    }
    size_t room = 7 - pre.size();
    if (p.size() <= room) { // single frame
        Bytes f(pre);
        f.push_back(p.size());
        f.insert(f.end(), p.begin(), p.end());
        return canWrite(b, id, {padded(f, pad)}, x29, hv) == STATUS_NOERROR;
    }
    if (p.size() > 0xFFF)
        return false;
    UNUM32 resp = pairedResp(l, id); // ECU's FC comes on its response id; unknown = don't wait, like the CANdi
    if (resp) {
        std::lock_guard<std::mutex> g(b.mu);
        b.fc[resp] = {};
    }
    Bytes ff(pre);
    ff.push_back(0x10 | p.size() >> 8);
    ff.push_back(p.size() & 0xFF);
    size_t pos = room - 1;
    ff.insert(ff.end(), p.begin(), p.begin() + pos);
    bool ok = canWrite(b, id, {ff}, x29) == STATUS_NOERROR;
    UNUM8 sn = 1;
    while (ok && pos < p.size()) {
        UNUM8 bs = 0, st = 0;
        if (resp) {
            std::unique_lock<std::mutex> g(b.mu);
            for (;;) { // N_Bs 1 s, FC WAIT starts it over
                ok = b.fcCv.wait_for(g, std::chrono::seconds(1), [&] { return b.fc[resp].got || c.cancel; }) && !c.cancel;
                if (!ok || b.fc[resp].fs != 1)
                    break;
                b.fc[resp].got = false;
            }
            CanBus::Fc f = b.fc[resp];
            b.fc[resp].got = false;
            if (!ok || f.fs != 0) {
                logf("  no flow control from 0x%X (%s)", (unsigned)resp, ok ? "overflow" : "timeout");
                ok = false;
                break;
            }
            bs = f.bs, st = f.st;
        }
        std::vector<Bytes> burst;
        for (int i = 0; pos < p.size() && (bs == 0 || i < bs); i++) {
            Bytes cf(pre);
            cf.push_back(0x20 | (sn++ & 0x0F));
            size_t k = std::min(room, p.size() - pos);
            cf.insert(cf.end(), p.begin() + pos, p.begin() + pos + k);
            pos += k;
            burst.push_back(padded(cf, pad));
        }
        UNUM32 stUs = st <= 0x7F ? st * 1000u : (st >= 0xF1 && st <= 0xF9 ? (st - 0xF0) * 100u : 127000u);
        if (stUs < 1000) { // STmin under a ms: the adapter's own frame pacing is enough
            ok = canWrite(b, id, burst, x29) == STATUS_NOERROR;
        } else {
            for (size_t i = 0; ok && i < burst.size(); i++) {
                if (i)
                    Sleep(stUs / 1000);
                ok = canWrite(b, id, {burst[i]}, x29) == STATUS_NOERROR;
            }
        }
    }
    if (resp) {
        std::lock_guard<std::mutex> g(b.mu);
        b.fc.erase(resp);
    }
    return ok;
}

// ---------------------------------------------------------------- CoP execution

// Returns true when the CoP is done (FINISHED is posted by the worker).
static bool execCoP(Link &l, std::shared_ptr<CoP> cp) {
    CoP &c = *cp;
    switch (c.type) {
    case PDU_COPT_UPDATEPARAM: // active was committed in PDUStartComPrimitive
        if (l.can)
            canParams(l);
        else if (l.chOpen)
            klineParams(l);
        return true;
    case PDU_COPT_RESTORE_PARAM: {
        std::lock_guard<std::mutex> g(l.mu);
        l.working = l.active;
        return true;
    }
    case PDU_COPT_DELAY:
        for (UNUM32 t = 0; t < c.time && !cancelled(l, c); t += 10)
            Sleep(10);
        return true;
    case PDU_COPT_STARTCOMM:
        if (l.can)
            setLinkStatus(l, PDU_CLLST_COMM_STARTED);
        else
            klineStartComm(l, c);
        return true;
    case PDU_COPT_STOPCOMM:
        // raw mode: Tech2Win sends its own StopCommunication frame if it wants one
        if (!c.data.empty() && !l.can)
            klineSendRecv(l, cp);
        setLinkStatus(l, PDU_CLLST_ONLINE);
        return true;
    case PDU_COPT_SENDRECV:
        if (!l.can)
            return klineSendRecv(l, cp);
        if (canTx(l, c)) {
            // the firmware's TX confirm (0x77) comes from this; it asks for one with the pad bit,
            // and the SW-CAN wakeup (no pad bit) must not produce a stray one
            if (c.txFlag & 0x40)
                postResult(l, c, c.data.data(), std::min<size_t>(c.data.size(), 4 + ((c.txFlag & 0x80) ? 1 : 0)), nowUs(), 0, RXF_TX_INDICATION, 0);
        } else {
            postError(l, c, PDU_ERR_EVT_TX_ERROR);
        }
        return true;
    }
    return true;
}

static void worker(std::shared_ptr<Link> l) {
    for (;;) {
        std::shared_ptr<CoP> c, lis;
        {
            std::unique_lock<std::mutex> g(l->mu);
            bool listen = l->listener && l->chOpen && !l->can; // K-line listener reads here, CAN on the bus thread
            if (!listen) // no wait_for(0): on mingw that still sleeps a timer tick
                l->cv.wait_for(g, std::chrono::seconds(1), [&] { return l->quit || !l->cops.empty(); });
            if (l->quit)
                return;
            if (l->listener && l->listener->cancel)
                l->listener.reset();
            if (!l->cops.empty()) {
                c = l->current = l->cops.front();
                l->cops.pop_front();
            }
            lis = l->listener;
        }
        if (c) {
            if (!c->execPosted)
                postStatus(*l, c->h, c->tag, PDU_COPST_EXECUTING);
            bool finished = execCoP(*l, c);
            bool post;
            {
                std::lock_guard<std::mutex> g(l->mu);
                post = finished && !c->cancel; // a cancel already posted CANCELLED
                c->done = finished;
                l->current.reset();
            }
            if (post)
                postStatus(*l, c->h, c->tag, PDU_COPST_FINISHED);
        } else if (lis && l->chOpen && !l->can) {
            PASSTHRU_MSG m;
            UNUM32 start;
            long rc = readFrame(*l, *lis, m, 20, start);
            if (rc == STATUS_NOERROR && !cancelled(*l, *lis))
                postResult(*l, *lis, m.Data, m.DataSize, 0, start);
            else if (rc != ERR_TIMEOUT)
                Sleep(10); // adapter error: don't spin, no ERROR item for a listener (it kills the link)
        }
    }
}

static void stopCoPs(Link &l) {
    std::lock_guard<std::mutex> g(l.mu);
    for (auto &c : l.cops)
        c->cancel = true;
    if (l.current)
        l.current->cancel = true;
    if (l.listener)
        l.listener->cancel = true;
    if (l.can)
        l.can->fcCv.notify_all(); // a TX waiting for flow control
}

// ---------------------------------------------------------------- API

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        QueryPerformanceFrequency(&g_qpf);
        QueryPerformanceCounter(&g_t0);
        g_cfg = loadConfig();
    }
    return TRUE;
}

EXPORT PDUConstruct(CHAR8 *opt, void *) {
    std::lock_guard<std::mutex> g(g_mu);
    g_cfg = loadConfig();
    logf("---- " APP_NAME " " APP_VERSION " PDUConstruct opt='%s'", opt ? opt : "");
    logf("K-line pins: 7 -> %d, 8 -> %d, 7+8 -> %d", g_cfg.pin7, g_cfg.pin8, g_cfg.pin78);
    scanModules();
    return PDU_STATUS_NOERROR;
}

static void destroyLink(std::shared_ptr<Link> l) {
    stopCoPs(*l);
    {
        std::lock_guard<std::mutex> g(l->mu);
        l->quit = true;
        l->cb = nullptr; // never call back after Destroy returned
    }
    l->cv.notify_all();
    if (l->worker.get_id() == std::this_thread::get_id())
        l->worker.detach(); // host destroyed the link from inside our callback
    else if (l->worker.joinable())
        l->worker.join();
    klineClose(*l);
    canDetach(*l);
    std::lock_guard<std::mutex> g(l->mu);
    for (auto *e : l->events)
        free(e);
    l->events.clear();
}

// Drops links (all, or one module's) and closes their devices.
static void closeModules(UNUM32 hMod) {
    std::vector<std::shared_ptr<Link>> links;
    {
        std::lock_guard<std::mutex> g(g_mu);
        for (auto it = g_links.begin(); it != g_links.end();) {
            if (hMod == PDU_HANDLE_UNDEF || it->second->hMod == hMod) {
                links.push_back(it->second);
                it = g_links.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (auto &l : links)
        destroyLink(l);
    std::lock_guard<std::mutex> g(g_mu);
    for (size_t i = 0; i < g_mods.size(); i++) {
        Module &m = *g_mods[i];
        if ((hMod == PDU_HANDLE_UNDEF || hMod == i + 1) && m.open) {
            std::lock_guard<std::mutex> pg(m.pt.mu);
            m.pt.Close(m.dev);
            m.open = false;
        }
    }
}

EXPORT PDUDestruct() {
    logf("PDUDestruct");
    closeModules(PDU_HANDLE_UNDEF);
    std::lock_guard<std::mutex> g(g_mu);
    g_mods.clear(); // re-read the registry on the next PDUConstruct
    return PDU_STATUS_NOERROR;
}

EXPORT PDUGetModuleIds(PDU_MODULE_ITEM **out) {
    if (!out)
        return PDU_ERR_INVALID_PARAMETERS;
    std::lock_guard<std::mutex> g(g_mu);
    scanModules();
    static const char info[] = "J2534"; // no "USB"/"Ethernet"/"Wireless": Tech2Win shows the name alone
    size_t n = g_mods.size(), strs = 0;
    for (auto &m : g_mods)
        strs += m->name.size() + sizeof(info) + 1;
    auto *it = (PDU_MODULE_ITEM *)calloc(1, sizeof(PDU_MODULE_ITEM) + n * sizeof(PDU_MODULE_DATA) + strs);
    it->ItemType = PDU_IT_MODULE_ID;
    it->NumEntries = n;
    it->pModuleData = n ? (PDU_MODULE_DATA *)(it + 1) : nullptr;
    char *s = (char *)((PDU_MODULE_DATA *)(it + 1) + n);
    for (size_t i = 0; i < n; i++) {
        PDU_MODULE_DATA &d = it->pModuleData[i];
        d.ModuleTypeId = 1;
        d.hMod = i + 1;
        d.ModuleStatus = PDU_MODST_AVAIL; // Tech2Win only lists AVAIL modules
        d.pVendorModuleName = s;
        s += sprintf(s, "%s", g_mods[i]->name.c_str()) + 1;
        d.pVendorAdditionalInfo = s;
        s += sprintf(s, "%s", info) + 1;
    }
    logf("PDUGetModuleIds -> %u modules", (unsigned)n);
    *out = it;
    return PDU_STATUS_NOERROR;
}

EXPORT PDUModuleConnect(UNUM32 hMod) {
    std::lock_guard<std::mutex> g(g_mu);
    Module *m = module(hMod == PDU_HANDLE_UNDEF ? 1 : hMod);
    if (!m)
        return PDU_ERR_INVALID_HANDLE;
    logf("PDUModuleConnect %u %s -> %s", (unsigned)hMod, m->name.c_str(), m->a.dll.c_str());
    if (m->open)
        return PDU_STATUS_NOERROR;
    if (!m->pt.load(m->a.dll)) {
        logf("  could not load %s (error %lu)", m->a.dll.c_str(), GetLastError());
        return PDU_ERR_MODULE_NOT_CONNECTED;
    }
    long rc;
    {
        std::lock_guard<std::mutex> pg(m->pt.mu);
        rc = m->pt.Open(nullptr, &m->dev);
    }
    if (rc != STATUS_NOERROR) {
        logf("  PassThruOpen failed: %s", m->pt.err(rc).c_str());
        return PDU_ERR_MODULE_NOT_CONNECTED;
    }
    m->open = true;
    return PDU_STATUS_NOERROR;
}

EXPORT PDUModuleDisconnect(UNUM32 hMod) {
    logf("PDUModuleDisconnect %d", (int)hMod);
    closeModules(hMod);
    return PDU_STATUS_NOERROR;
}

EXPORT PDUGetTimestamp(UNUM32, UNUM32 *ts) {
    if (!ts)
        return PDU_ERR_INVALID_PARAMETERS;
    *ts = nowUs();
    return PDU_STATUS_NOERROR;
}

EXPORT PDUGetObjectId(T_PDU_OBJT type, CHAR8 *name, UNUM32 *id) {
    if (!name || !id)
        return PDU_ERR_INVALID_PARAMETERS;
    std::lock_guard<std::mutex> g(g_mu);
    auto it = g_ids.find(name);
    if (it == g_ids.end())
        it = g_ids.emplace(name, g_ids.size() + 1).first;
    *id = it->second;
    return PDU_STATUS_NOERROR;
}

EXPORT PDUGetVersion(UNUM32 hMod, PDU_VERSION_DATA *v) {
    if (!v)
        return PDU_ERR_INVALID_PARAMETERS;
    std::lock_guard<std::mutex> g(g_mu);
    memset(v, 0, sizeof(*v));
    v->MVCI_Part2StandardVersion = 0x02020000;
    Module *m = module(hMod);
    // never "MDI"/"MDI 2": Tech2Win version-checks those and unloads us
    snprintf(v->HwName, sizeof(v->HwName), "%s", m ? m->name.c_str() : "J2534");
    snprintf(v->VendorName, sizeof(v->VendorName), "%s", m ? m->a.vendor.c_str() : "");
    snprintf(v->PDUApiSwName, sizeof(v->PDUApiSwName), APP_NAME " " APP_VERSION);
    return PDU_STATUS_NOERROR;
}

EXPORT PDUGetStatus(UNUM32 hMod, UNUM32 hCLL, UNUM32 hCoP, T_PDU_STATUS *st, UNUM32 *ts, UNUM32 *extra) {
    if (!st)
        return PDU_ERR_INVALID_PARAMETERS;
    if (ts)
        *ts = nowUs();
    if (extra)
        *extra = 0;
    if (hCLL == PDU_HANDLE_UNDEF) {
        std::lock_guard<std::mutex> g(g_mu);
        Module *m = module(hMod);
        if (!m)
            return PDU_ERR_INVALID_HANDLE;
        *st = m->open ? PDU_MODST_READY : PDU_MODST_AVAIL;
        return PDU_STATUS_NOERROR;
    }
    auto l = findLink(hMod, hCLL);
    if (!l)
        return PDU_ERR_INVALID_HANDLE;
    std::lock_guard<std::mutex> g(l->mu);
    if (hCoP == PDU_HANDLE_UNDEF) {
        *st = l->status;
        return PDU_STATUS_NOERROR;
    }
    // unknown handles (Tech2Win polls handle 0) must give exactly INVALID_HANDLE
    if ((l->current && l->current->h == hCoP && !l->current->cancel) || (l->listener && l->listener->h == hCoP && !l->listener->cancel)) {
        *st = PDU_COPST_EXECUTING;
        return PDU_STATUS_NOERROR;
    }
    for (auto &c : l->cops)
        if (c->h == hCoP) {
            *st = PDU_COPST_IDLE;
            return PDU_STATUS_NOERROR;
        }
    return PDU_ERR_INVALID_HANDLE;
}

EXPORT PDUGetLastError(UNUM32, UNUM32, T_PDU_ERR_EVT *code, UNUM32 *hCoP, UNUM32 *ts, UNUM32 *extra) {
    if (code)
        *code = PDU_ERR_EVT_NOERROR;
    if (hCoP)
        *hCoP = PDU_HANDLE_UNDEF;
    if (ts)
        *ts = nowUs();
    if (extra)
        *extra = 0;
    return PDU_STATUS_NOERROR;
}

// never called by Tech2Win, exported because it resolves all 29 names
EXPORT PDUGetResourceStatus(PDU_RSC_STATUS_ITEM *) { return PDU_ERR_ID_NOT_SUPPORTED; }
EXPORT PDUGetResourceIds(UNUM32, PDU_RSC_DATA *, PDU_RSC_ID_ITEM **) { return PDU_ERR_ID_NOT_SUPPORTED; }
EXPORT PDUGetConflictingResources(UNUM32, PDU_MODULE_ITEM *, PDU_RSC_CONFLICT_ITEM **) { return PDU_ERR_ID_NOT_SUPPORTED; }
EXPORT PDULockResource(UNUM32, UNUM32, UNUM32) { return PDU_STATUS_NOERROR; }
EXPORT PDUUnlockResource(UNUM32, UNUM32, UNUM32) { return PDU_STATUS_NOERROR; }
EXPORT PDUGetUniqueRespIdTable(UNUM32, UNUM32, PDU_UNIQUE_RESP_ID_TABLE_ITEM **) { return PDU_ERR_ID_NOT_SUPPORTED; }

// CAN: which response ids are ISO-TP (and their request id for flow control) vs plain UUDT.
// Applied right away; Tech2Win follows it with UPDATEPARAM anyway.
EXPORT PDUSetUniqueRespIdTable(UNUM32 hMod, UNUM32 hCLL, PDU_UNIQUE_RESP_ID_TABLE_ITEM *t) {
    auto l = findLink(hMod, hCLL);
    if (!l)
        return PDU_ERR_INVALID_HANDLE;
    std::vector<UrEntry> table;
    for (UNUM32 i = 0; t && i < t->NumEntries; i++) {
        PDU_ECU_UNIQUE_RESP_DATA &d = t->pUniqueData[i];
        UrEntry e{d.UniqueRespIdentifier, 0, 0, 0, 0, 0, 0, 0};
        for (UNUM32 k = 0; k < d.NumParamItems; k++) {
            PDU_PARAM_ITEM &p = d.pParams[k];
            if (!p.pComParamData)
                continue;
            UNUM32 v = *(UNUM32 *)p.pComParamData;
            std::string n;
            {
                std::lock_guard<std::mutex> g(g_mu);
                n = idName(p.ComParamId);
            }
            if (n == "CP_CanPhysReqId") e.reqId = v & 0x1FFFFFFF;
            else if (n == "CP_CanPhysReqFormat") e.reqFmt = v;
            else if (n == "CP_CanPhysReqExtAddr") e.reqExt = v;
            else if (n == "CP_CanRespUSDTId") e.usdtId = v & 0x1FFFFFFF;
            else if (n == "CP_CanRespUSDTFormat") e.usdtFmt = v;
            else if (n == "CP_CanRespUSDTExtAddr") e.usdtExt = v;
            else if (n == "CP_CanRespUUDTId") e.uudtId = v & 0x1FFFFFFF;
        }
        table.push_back(e);
    }
    logf("PDUSetUniqueRespIdTable CLL %u: %u entries", (unsigned)hCLL, (unsigned)table.size());
    for (auto &e : table)
        if (e.usdtId)
            logf("  %3u USDT 0x%X -> 0x%X", (unsigned)e.urid, (unsigned)e.reqId, (unsigned)e.usdtId);
        else
            logf("  %3u UUDT 0x%X", (unsigned)e.urid, (unsigned)e.uudtId);
    bool any = !table.empty();
    {
        std::lock_guard<std::mutex> g(l->mu);
        l->table = std::move(table);
    }
    if (any && l->can)
        canOpen(*l->can); // the bus is in use from here on
    return PDU_STATUS_NOERROR;
}

EXPORT PDURegisterEventCallback(UNUM32 hMod, UNUM32 hCLL, CALLBACKFNC cb) {
    logf("PDURegisterEventCallback hMod=%d hCLL=%d %s", (int)hMod, (int)hCLL, cb ? "set" : "clear");
    if (hCLL == PDU_HANDLE_UNDEF)
        return PDU_STATUS_NOERROR; // we never queue system or module items
    auto l = findLink(hMod, hCLL);
    if (!l)
        return PDU_ERR_INVALID_HANDLE;
    std::lock_guard<std::mutex> g(l->mu);
    l->cb = cb;
    return PDU_STATUS_NOERROR;
}

EXPORT PDUCreateComLogicalLink(UNUM32 hMod, PDU_RSC_DATA *rsc, UNUM32, void *tag, UNUM32 *phCLL, PDU_FLAG_DATA *) {
    if (!phCLL || !rsc)
        return PDU_ERR_INVALID_PARAMETERS;
    std::lock_guard<std::mutex> g(g_mu);
    Module *m = module(hMod);
    if (!m || !m->open)
        return PDU_ERR_INVALID_HANDLE;
    auto l = std::make_shared<Link>();
    l->hMod = hMod;
    l->mod = m;
    l->tag = tag;
    l->bus = idName(rsc->BusTypeId);
    l->protoName = idName(rsc->ProtocolId);
    std::vector<PDU_PIN_DATA> pins(rsc->pDLCPinData, rsc->pDLCPinData + rsc->NumPinData);
    std::string ps;
    for (auto &p : pins)
        ps += " " + std::to_string(p.DLCPinNumber) + "=" + idName(p.DLCPinTypeId);
    for (auto &p : PROTOS)
        if (l->protoName == p.name)
            l->proto = &p;
    if (isCanProto(l->protoName)) {
        l->canRaw = l->protoName == "ISO_11898_RAW";
        l->can = canAttach(l, pins);
    }
    if (!l->proto && !l->can) {
        logf("PDUCreateComLogicalLink %s / %s pins:%s: not supported", l->bus.c_str(), l->protoName.c_str(), ps.c_str());
        return PDU_ERR_VALUE_NOT_SUPPORTED;
    }
    if (l->proto)
        pickPins(*l, pins);
    l->h = g_nextLink++;
    logf("PDUCreateComLogicalLink %s / %s pins:%s -> CLL %u%s", l->bus.c_str(), l->protoName.c_str(), ps.c_str(), (unsigned)l->h,
         l->can ? (" on " + l->can->name).c_str() : "");
    l->worker = std::thread(worker, l);
    g_links[l->h] = l;
    *phCLL = l->h;
    return PDU_STATUS_NOERROR;
}

EXPORT PDUDestroyComLogicalLink(UNUM32 hMod, UNUM32 hCLL) {
    logf("PDUDestroyComLogicalLink %u", (unsigned)hCLL);
    std::shared_ptr<Link> l = findLink(hMod, hCLL);
    if (!l)
        return PDU_ERR_INVALID_HANDLE;
    {
        std::lock_guard<std::mutex> g(g_mu);
        g_links.erase(hCLL);
    }
    destroyLink(l);
    return PDU_STATUS_NOERROR;
}

EXPORT PDUConnect(UNUM32 hMod, UNUM32 hCLL) {
    auto l = findLink(hMod, hCLL);
    if (!l)
        return PDU_ERR_INVALID_HANDLE;
    logf("PDUConnect CLL %u", (unsigned)hCLL);
    {
        std::lock_guard<std::mutex> g(l->mu);
        l->active = l->working; // ComParams set before Connect (SDL, all of CAN) apply now
    }
    if (l->proto && !l->chOpen && !klineOpen(*l))
        return PDU_ERR_FCT_FAILED;
    if (l->can && !l->canRaw)
        l->can->baud = pget(l->active, "CP_Baudrate", l->can->baud);
    l->connected = true;
    setLinkStatus(*l, PDU_CLLST_ONLINE, true);
    return PDU_STATUS_NOERROR;
}

EXPORT PDUDisconnect(UNUM32 hMod, UNUM32 hCLL) {
    auto l = findLink(hMod, hCLL);
    if (!l)
        return PDU_ERR_INVALID_HANDLE;
    logf("PDUDisconnect CLL %u", (unsigned)hCLL);
    stopCoPs(*l);
    for (int i = 0; i < 300; i++) { // the worker drops cancelled CoPs within a read poll
        {
            std::lock_guard<std::mutex> g(l->mu);
            if (!l->current && !l->listener && l->cops.empty())
                break;
            l->cops.clear();
            if (l->listener && l->listener->cancel)
                l->listener.reset();
        }
        Sleep(5);
    }
    klineClose(*l);
    l->connected = false;
    setLinkStatus(*l, PDU_CLLST_OFFLINE, true); // Tech2Win waits for it even when we fail
    return PDU_STATUS_NOERROR;
}

EXPORT PDUSetComParam(UNUM32 hMod, UNUM32 hCLL, PDU_PARAM_ITEM *p) {
    if (!p || !p->pComParamData)
        return PDU_ERR_INVALID_PARAMETERS;
    auto l = findLink(hMod, hCLL);
    if (!l)
        return PDU_ERR_INVALID_HANDLE;
    std::string name;
    {
        std::lock_guard<std::mutex> g(g_mu);
        name = idName(p->ComParamId);
    }
    UNUM32 v;
    switch (p->ComParamDataType) {
    case PDU_PT_UNUM8: v = *(UNUM8 *)p->pComParamData; break;
    case PDU_PT_SNUM8: v = *(signed char *)p->pComParamData; break;
    case PDU_PT_UNUM16: v = *(UNUM16 *)p->pComParamData; break;
    case PDU_PT_SNUM16: v = *(short *)p->pComParamData; break;
    case PDU_PT_UNUM32:
    case PDU_PT_SNUM32: v = *(UNUM32 *)p->pComParamData; break;
    default:
        logf("PDUSetComParam %s: type 0x%X ignored", name.c_str(), (unsigned)p->ComParamDataType);
        return PDU_STATUS_NOERROR;
    }
    logf("PDUSetComParam CLL %u %s = %u", (unsigned)hCLL, name.c_str(), (unsigned)v);
    std::lock_guard<std::mutex> g(l->mu);
    l->working[name] = v;
    return PDU_STATUS_NOERROR;
}

EXPORT PDUGetComParam(UNUM32 hMod, UNUM32 hCLL, UNUM32 id, PDU_PARAM_ITEM **out) {
    if (!out)
        return PDU_ERR_INVALID_PARAMETERS;
    auto l = findLink(hMod, hCLL);
    if (!l)
        return PDU_ERR_INVALID_HANDLE;
    std::string name;
    {
        std::lock_guard<std::mutex> g(g_mu);
        name = idName(id);
    }
    UNUM32 v;
    {
        std::lock_guard<std::mutex> g(l->mu);
        // CP_Baudrate is read back after a 5-baud init and fed to the firmware: report the real one
        v = name == "CP_Baudrate" && l->baud ? l->baud : pget(l->working, name.c_str(), 0);
    }
    auto *p = (PDU_PARAM_ITEM *)calloc(1, sizeof(PDU_PARAM_ITEM) + sizeof(UNUM32));
    p->ItemType = PDU_IT_PARAM;
    p->ComParamId = id;
    p->ComParamDataType = PDU_PT_UNUM32;
    p->pComParamData = p + 1;
    *(UNUM32 *)p->pComParamData = v;
    logf("PDUGetComParam CLL %u %s -> %u", (unsigned)hCLL, name.c_str(), (unsigned)v);
    *out = p;
    return PDU_STATUS_NOERROR;
}

EXPORT PDUStartComPrimitive(UNUM32 hMod, UNUM32 hCLL, UNUM32 type, UNUM32 size, UNUM8 *data, PDU_COP_CTRL_DATA *ctrl, void *tag, UNUM32 *phCoP) {
    auto l = findLink(hMod, hCLL);
    if (!l)
        return PDU_ERR_INVALID_HANDLE;
    if (!l->connected && type != PDU_COPT_UPDATEPARAM && type != PDU_COPT_RESTORE_PARAM && type != PDU_COPT_DELAY)
        return PDU_ERR_CLL_NOT_CONNECTED;
    auto c = std::make_shared<CoP>();
    c->h = g_nextCop++;
    c->type = type;
    if (data && size)
        c->data.assign(data, data + size);
    c->sendCycles = ctrl ? ctrl->NumSendCycles : 1;
    c->recvCycles = ctrl ? ctrl->NumReceiveCycles : 1;
    c->time = ctrl ? ctrl->Time : 0;
    c->tag = tag; // the exact pointer goes back in every item, Tech2Win dereferences it
    if (ctrl && ctrl->TxFlag.pFlagData)
        for (UNUM32 i = 0; i < ctrl->TxFlag.NumFlagBytes && i < 4; i++)
            c->txFlag |= (UNUM32)ctrl->TxFlag.pFlagData[i] << (8 * (ctrl->TxFlag.NumFlagBytes - 1 - i));
    for (UNUM32 i = 0; ctrl && i < ctrl->NumPossibleExpectedResponses; i++) {
        PDU_EXP_RESP_DATA &e = ctrl->pExpectedResponseArray[i];
        UNUM32 n = e.NumMaskPatternBytes;
        c->ers.push_back({e.AcceptanceId, Bytes(e.pMaskData, e.pMaskData + n), Bytes(e.pPatternData, e.pPatternData + n)});
    }
    logf("PDUStartComPrimitive CLL %u hCoP=%u type=0x%X send=%d recv=%d txflag=0x%X ers=%u [%s]", (unsigned)hCLL, (unsigned)c->h,
         (unsigned)type, (int)c->sendCycles, (int)c->recvCycles, (unsigned)c->txFlag, (unsigned)c->ers.size(),
         hex(c->data.data(), c->data.size()).c_str());
    bool canRx = l->can && type == PDU_COPT_SENDRECV && c->data.empty() && c->recvCycles == -1;
    std::deque<Pending> pending;
    {
        std::lock_guard<std::mutex> g(l->mu);
        if (type == PDU_COPT_UPDATEPARAM) // commit now, the worker applies it to the adapter
            l->active = l->working;
        c->params = l->working;
        if (ctrl && ctrl->TempParamUpdate) // the temporary values were for this CoP only
            l->working = l->active;
        if (canRx) { // CAN permanent receiver: live right away, never queued behind TX
            l->listener = c;
            pending.swap(l->pending);
        } else {
            l->cops.push_back(c);
        }
        c->execPosted = l->can && type == PDU_COPT_SENDRECV;
    }
    if (phCoP)
        *phCoP = c->h;
    // CAN: Tech2Win holds the firmware's reply until EXECUTING, so it goes out before any work
    if (c->execPosted)
        postStatus(*l, c->h, c->tag, PDU_COPST_EXECUTING);
    if (canRx) {
        DWORD now = GetTickCount();
        for (auto &p : pending) // what came in while the previous receiver was being replaced
            if (now - p.t < 1000)
                canPost(*l, *c, p.data, p.flag);
        if (!c->ers.empty())
            canOpen(*l->can);
    } else {
        l->cv.notify_one();
    }
    return PDU_STATUS_NOERROR;
}

EXPORT PDUCancelComPrimitive(UNUM32 hMod, UNUM32 hCLL, UNUM32 hCoP) {
    auto l = findLink(hMod, hCLL);
    if (!l)
        return PDU_ERR_INVALID_HANDLE;
    std::shared_ptr<CoP> c;
    {
        std::lock_guard<std::mutex> g(l->mu);
        if (l->listener && l->listener->h == hCoP && !l->listener->cancel) {
            c = l->listener;
            if (l->can)
                l->listener.reset(); // the bus thread buffers until the next receiver
        } else if (l->current && l->current->h == hCoP && !l->current->cancel && !l->current->done) {
            c = l->current;
        }
        for (auto it = l->cops.begin(); !c && it != l->cops.end(); ++it)
            if ((*it)->h == hCoP) {
                c = *it;
                l->cops.erase(it);
                break;
            }
        if (c)
            c->cancel = true;
    }
    logf("PDUCancelComPrimitive CLL %u hCoP=%u%s", (unsigned)hCLL, (unsigned)hCoP, c ? "" : " (not running)");
    if (!c && !l->can)
        return PDU_ERR_INVALID_HANDLE; // K-line: non-zero means Tech2Win doesn't wait for CANCELLED
    // CAN: Tech2Win waits for CANCELLED whatever we return, and a non-zero return during a
    // receiver re-arm means it never starts a new one. So always CANCELLED and 0 there.
    // Posted right away so the next request isn't held up by a read poll; the worker
    // stops touching the CoP as soon as it sees the flag.
    postStatus(*l, hCoP, c ? c->tag : nullptr, PDU_COPST_CANCELLED);
    l->cv.notify_one();
    if (l->can)
        l->can->fcCv.notify_all();
    return PDU_STATUS_NOERROR;
}

EXPORT PDUGetEventItem(UNUM32 hMod, UNUM32 hCLL, PDU_EVENT_ITEM **out) {
    if (!out)
        return PDU_ERR_INVALID_PARAMETERS;
    *out = nullptr;
    if (hCLL == PDU_HANDLE_UNDEF)
        return PDU_ERR_EVENT_QUEUE_EMPTY;
    auto l = findLink(hMod, hCLL);
    if (!l)
        return PDU_ERR_INVALID_HANDLE;
    std::lock_guard<std::mutex> g(l->mu);
    if (l->events.empty())
        return PDU_ERR_EVENT_QUEUE_EMPTY;
    *out = l->events.front();
    l->events.pop_front();
    return PDU_STATUS_NOERROR;
}

EXPORT PDUDestroyItem(PDU_ITEM *item) {
    free(item); // every item we hand out is a single calloc block
    return PDU_STATUS_NOERROR;
}

EXPORT PDUIoCtl(UNUM32 hMod, UNUM32 hCLL, UNUM32 id, PDU_DATA_ITEM *, PDU_DATA_ITEM **out) {
    std::string name;
    Module *m;
    {
        std::lock_guard<std::mutex> g(g_mu);
        name = idName(id);
        m = module(hMod);
    }
    logf("PDUIoCtl hMod=%d hCLL=%d %s", (int)hMod, (int)hCLL, name.c_str());
    // ponytail: msg filters are ignored, K-line passes everything and CAN only delivers what
    // matches the receiver's expected responses (= the firmware's mailboxes) anyway
    if (name != "PDU_IOCTL_READ_VBATT")
        return PDU_STATUS_NOERROR;
    if (!m || !m->open || !out)
        return PDU_ERR_MODULE_NOT_CONNECTED;
    unsigned long mv = 0;
    long rc = m->pt.ioctl(m->dev, READ_VBATT, nullptr, &mv);
    if (rc != STATUS_NOERROR) {
        logf("  READ_VBATT failed: %s", m->pt.err(rc).c_str());
        return PDU_ERR_FCT_FAILED;
    }
    auto *d = (PDU_DATA_ITEM *)calloc(1, sizeof(PDU_DATA_ITEM) + sizeof(UNUM32));
    d->ItemType = PDU_IT_IO_UNUM32;
    d->pData = d + 1;
    *(UNUM32 *)d->pData = mv;
    logf("  VBATT %lu mV", mv);
    *out = d;
    return PDU_STATUS_NOERROR;
}
