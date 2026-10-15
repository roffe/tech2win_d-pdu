// Drives t2wdpdu.dll the way Tech2Win does (call order, ctrl data, status waits from
// notes/host_api_kw2000.md + host_other_protocols.md). Exit code 0 = expectation met.
//
// pduhost pins=7,8 ecu=41 expect=ok|timeout|connectfail [j1962=none|0x0800] [proto=kw82]
// pduhost proto=can   (Tech2Win's CANdi session: SW + MS + HS, 2 CLLs each, notes/host_can_*.md)
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include "pdu.h"

#define FAIL(...) (printf("FAIL: " __VA_ARGS__), printf("\n"), exit(1))

static HMODULE dll;
#define FNS(X)                                                                                                          \
    X(PDUConstruct, CHAR8 *, void *)                                                                                   \
    X(PDUDestruct)                                                                                                     \
    X(PDUGetModuleIds, PDU_MODULE_ITEM **)                                                                             \
    X(PDUModuleConnect, UNUM32)                                                                                        \
    X(PDUModuleDisconnect, UNUM32)                                                                                     \
    X(PDURegisterEventCallback, UNUM32, UNUM32, CALLBACKFNC)                                                           \
    X(PDUGetVersion, UNUM32, PDU_VERSION_DATA *)                                                                       \
    X(PDUIoCtl, UNUM32, UNUM32, UNUM32, PDU_DATA_ITEM *, PDU_DATA_ITEM **)                                             \
    X(PDUGetObjectId, T_PDU_OBJT, const CHAR8 *, UNUM32 *)                                                            \
    X(PDUCreateComLogicalLink, UNUM32, PDU_RSC_DATA *, UNUM32, void *, UNUM32 *, PDU_FLAG_DATA *)                     \
    X(PDUDestroyComLogicalLink, UNUM32, UNUM32)                                                                        \
    X(PDUConnect, UNUM32, UNUM32)                                                                                      \
    X(PDUDisconnect, UNUM32, UNUM32)                                                                                   \
    X(PDUSetComParam, UNUM32, UNUM32, PDU_PARAM_ITEM *)                                                                \
    X(PDUGetComParam, UNUM32, UNUM32, UNUM32, PDU_PARAM_ITEM **)                                                       \
    X(PDUStartComPrimitive, UNUM32, UNUM32, UNUM32, UNUM32, UNUM8 *, PDU_COP_CTRL_DATA *, void *, UNUM32 *)            \
    X(PDUCancelComPrimitive, UNUM32, UNUM32, UNUM32)                                                                   \
    X(PDUGetEventItem, UNUM32, UNUM32, PDU_EVENT_ITEM **)                                                              \
    X(PDUDestroyItem, void *)                                                                                          \
    X(PDUGetStatus, UNUM32, UNUM32, UNUM32, T_PDU_STATUS *, UNUM32 *, UNUM32 *)
#define DECL(n, ...) static T_PDU_ERROR(__stdcall *n)(__VA_ARGS__);
FNS(DECL)
static void resolve() {
#define RES(n, ...)                                \
    if (!(*(FARPROC *)&n = GetProcAddress(dll, #n))) \
        FAIL("missing export %s", #n);
    FNS(RES)
}
#define CALL(n, ...) n(__VA_ARGS__)

static HANDLE ev;
static void __stdcall cb(T_PDU_EVT_DATA, UNUM32, UNUM32, void *, void *) { SetEvent(ev); }

static UNUM32 hMod, hCLL;
static std::vector<std::vector<UNUM8>> results;
static int errors;

struct Res {
    UNUM32 acc, flag;
    std::vector<UNUM8> d;
};
static std::map<UNUM32, std::vector<Res>> rx; // per CLL, with AcceptanceId and RxFlag

// Tech2Win's wait: drain everything, stop at a STATUS item with this value (any hCoP)
static bool waitStatus(UNUM32 want, DWORD ms = 3000, UNUM32 cll = 0) {
    if (!cll)
        cll = hCLL;
    for (;;) {
        bool hit = false;
        PDU_EVENT_ITEM *e;
        while (CALL(PDUGetEventItem, hMod, cll, &e) == PDU_STATUS_NOERROR) {
            if (e->ItemType == PDU_IT_RESULT) {
                auto *r = (PDU_RESULT_DATA *)e->pData;
                if (!r->RxFlag.pFlagData || r->RxFlag.NumFlagBytes < 4)
                    FAIL("RESULT without 4 RxFlag bytes (Tech2Win's CAN code crashes on that)");
                UNUM8 *f = r->RxFlag.pFlagData;
                results.emplace_back(r->pDataBytes, r->pDataBytes + r->NumDataBytes);
                rx[cll].push_back({r->AcceptanceId, (UNUM32)f[0] << 24 | f[1] << 16 | f[2] << 8 | f[3], results.back()});
            } else if (e->ItemType == PDU_IT_ERROR) {
                errors++;
            } else if (e->ItemType == PDU_IT_STATUS && *(UNUM32 *)e->pData == want) {
                hit = true;
            }
            CALL(PDUDestroyItem, e);
        }
        if (hit)
            return true;
        if (WaitForSingleObject(ev, ms) != WAIT_OBJECT_0)
            return false;
    }
}

static UNUM32 id(T_PDU_OBJT t, const char *n) {
    UNUM32 v = PDU_ID_UNDEF;
    if (CALL(PDUGetObjectId, t, n, &v) || v == PDU_ID_UNDEF)
        FAIL("GetObjectId %s", n);
    return v;
}

static void setParam(const char *name, UNUM32 v, UNUM32 cll = 0) {
    UNUM32 val = v;
    PDU_PARAM_ITEM p{PDU_IT_PARAM, id(PDU_OBJT_COMPARAM, name), PDU_PT_UNUM32, 1, &val};
    if (CALL(PDUSetComParam, hMod, cll ? cll : hCLL, &p))
        FAIL("SetComParam %s", name);
}

static UNUM8 one = 0, zeros[4] = {};
static PDU_EXP_RESP_DATA er{0, 1, 1, &one, &one, 0, nullptr};

static UNUM32 cop(UNUM32 type, std::vector<UNUM8> data, PDU_COP_CTRL_DATA *ctrl, int tag, UNUM32 cll = 0) {
    static int tags[5] = {0, 1, 2, 3, 4};
    UNUM32 h = 0;
    if (CALL(PDUStartComPrimitive, hMod, cll ? cll : hCLL, type, (UNUM32)data.size(), data.empty() ? nullptr : data.data(), ctrl, &tags[tag], &h) || !h)
        FAIL("StartComPrimitive 0x%X", (unsigned)type);
    return h;
}

static std::vector<UNUM8> kwp(std::vector<UNUM8> b) {
    UNUM8 cs = 0;
    for (auto x : b)
        cs += x;
    b.push_back(cs);
    return b;
}

static std::string arg(int argc, char **argv, const char *k, const char *def) {
    for (int i = 1; i < argc; i++)
        if (!strncmp(argv[i], k, strlen(k)) && argv[i][strlen(k)] == '=')
            return argv[i] + strlen(k) + 1;
    return def;
}

static void loadFromRootFile() {
    char root[MAX_PATH] = "";
    DWORD n = sizeof(root);
    if (RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\D-PDU API", "Root File", RRF_RT_REG_SZ, nullptr, root, &n))
        FAIL("no D-PDU API Root File in the registry");
    FILE *f = fopen(root, "rb");
    if (!f)
        FAIL("root file %s missing", root);
    char xml[4096] = "";
    fread(xml, 1, sizeof(xml) - 1, f);
    fclose(f);
    // same URI rule as Tech2Win: file:// prefix, then the path up to the quote
    const char *u = strstr(xml, "URI=\"file://");
    if (!u)
        FAIL("no LIBRARY_FILE URI in %s", root);
    std::string path(u + 12, (const char *)strchr(u + 12, '"'));
    if (path[0] == '/')
        path.erase(0, 1);
    if (!(dll = LoadLibraryA(path.c_str())))
        FAIL("LoadLibrary %s (%lu)", path.c_str(), GetLastError());
    resolve();
}

// ---------------------------------------------------------------- CAN session

struct Mailbox {
    UNUM32 idx, req, resp; // req 0 = plain UUDT mailbox
};

struct Chan {
    const char *name, *bus;
    std::vector<PDU_PIN_DATA> pins;
    UNUM32 baud;
    std::vector<Mailbox> mb;
    UNUM32 cll[2] = {0, 0}, rxCop = 0;
};

static void canStatus(UNUM32 cll, UNUM32 want, const char *what) {
    if (!waitStatus(want, 3000, cll))
        FAIL("%s: no status 0x%X", what, (unsigned)want);
}

static void canTable(Chan &c) {
    static const char *names[9] = {"CP_CanPhysReqId", "CP_CanPhysReqFormat", "CP_CanPhysReqExtAddr", "CP_CanRespUSDTId", "CP_CanRespUSDTFormat",
                                   "CP_CanRespUSDTExtAddr", "CP_CanRespUUDTId", "CP_CanRespUUDTFormat", "CP_CanRespUUDTExtAddr"};
    std::vector<UNUM32> vals(c.mb.size() * 9);
    std::vector<PDU_PARAM_ITEM> items(c.mb.size() * 9);
    std::vector<PDU_ECU_UNIQUE_RESP_DATA> rows(c.mb.size());
    for (size_t i = 0; i < c.mb.size(); i++) {
        Mailbox &m = c.mb[i];
        UNUM32 v[9] = {m.req, m.req ? 0x35u : 0, 0, m.req ? m.resp : 0, m.req ? 0x05u : 0, 0, m.req ? 0 : m.resp, 0, 0};
        for (int k = 0; k < 9; k++) {
            vals[i * 9 + k] = v[k];
            items[i * 9 + k] = {PDU_IT_PARAM, id(PDU_OBJT_COMPARAM, names[k]), PDU_PT_UNUM32, 6, &vals[i * 9 + k]};
        }
        rows[i] = {m.idx, 9, &items[i * 9]};
    }
    static T_PDU_ERROR(__stdcall * setTable)(UNUM32, UNUM32, PDU_UNIQUE_RESP_ID_TABLE_ITEM *);
    if (!setTable && !(*(FARPROC *)&setTable = GetProcAddress(dll, "PDUSetUniqueRespIdTable")))
        FAIL("missing PDUSetUniqueRespIdTable");
    PDU_UNIQUE_RESP_ID_TABLE_ITEM t{PDU_IT_UNIQUE_RESP_ID_TABLE, (UNUM32)rows.size(), rows.data()};
    if (setTable(hMod, c.cll[0], &t))
        FAIL("%s SetUniqueRespIdTable", c.name);
}

// the permanent receiver: one expected response per mailbox, AcceptanceId = mailbox index
static void canReceiver(Chan &c) {
    std::vector<std::vector<UNUM8>> pat(c.mb.size());
    std::vector<PDU_EXP_RESP_DATA> ers(c.mb.size());
    static UNUM8 mask[4] = {0x1F, 0xFF, 0xFF, 0xFF};
    for (size_t i = 0; i < c.mb.size(); i++) {
        UNUM32 r = c.mb[i].resp;
        pat[i] = {(UNUM8)(r >> 24), (UNUM8)(r >> 16), (UNUM8)(r >> 8), (UNUM8)r};
        ers[i] = {0, c.mb[i].idx, 4, mask, pat[i].data(), 0, nullptr};
    }
    PDU_COP_CTRL_DATA ctrl{0, 0, -1, 0, {0, nullptr}, (UNUM32)ers.size(), ers.data()};
    c.rxCop = cop(PDU_COPT_SENDRECV, {}, &ctrl, 0, c.cll[0]);
    canStatus(c.cll[0], PDU_COPST_EXECUTING, "receiver");
}

static void canCreate(Chan &c) {
    UNUM8 raw[4] = {0x80, 0, 0, 0};
    PDU_FLAG_DATA flag{4, raw};
    static int tag = 0;
    for (int i = 0; i < 2; i++) {
        PDU_RSC_DATA rsc{id(PDU_OBJT_BUSTYPE, c.bus), id(PDU_OBJT_PROTOCOL, i ? "ISO_11898_RAW" : "ISO_15765_3_on_ISO_15765_2"),
                         (UNUM32)c.pins.size(), c.pins.data()};
        if (CALL(PDUCreateComLogicalLink, hMod, &rsc, PDU_ID_UNDEF, &tag, &c.cll[i], &flag) || !c.cll[i])
            FAIL("%s CreateComLogicalLink %d", c.name, i);
        setParam("CP_CanFillerByteHandling", 1, c.cll[i]);
    }
    for (int i = 0; i < 2; i++)
        CALL(PDURegisterEventCallback, hMod, c.cll[i], cb);
    for (int i = 0; i < 2; i++) {
        setParam("CP_TerminationType", c.baud == 33333 ? 0 : 2, c.cll[i]);
        setParam("CP_Baudrate", c.baud, c.cll[i]);
        setParam("CP_BitSamplePoint", 80, c.cll[i]);
        setParam("CP_SamplesPerBit", 0, c.cll[i]);
        setParam("CP_SyncJumpWidth", 15, c.cll[i]);
    }
    setParam("CP_P3Phys", 10000, c.cll[0]);
    CALL(PDUIoCtl, hMod, c.cll[0], id(PDU_OBJT_IO_CTRL, "PDU_IOCTL_CLEAR_MSG_FILTER"), nullptr, nullptr);
    canTable(c);
    for (int i = 0; i < 2; i++)
        setParam("CP_RequestAddrMode", 2, c.cll[i]);
    for (int i = 0; i < 2; i++) {
        if (CALL(PDUConnect, hMod, c.cll[i]))
            FAIL("%s Connect", c.name);
        canStatus(c.cll[i], PDU_CLLST_ONLINE, "connect");
    }
    static UNUM8 nul = 0;
    PDU_COP_CTRL_DATA sc{100, 0, 0, 0, {0, nullptr}, 0, nullptr};
    for (int i = 0; i < 2; i++) {
        if (CALL(PDUStartComPrimitive, hMod, c.cll[i], PDU_COPT_STARTCOMM, 0, &nul, &sc, nullptr, &c.rxCop))
            FAIL("%s STARTCOMM", c.name);
        canStatus(c.cll[i], PDU_COPST_FINISHED, "startcomm");
    }
    c.rxCop = 0;
    if (!c.mb.empty())
        canReceiver(c);
}

static void canTeardown(Chan &c) {
    if (c.rxCop) {
        if (CALL(PDUCancelComPrimitive, hMod, c.cll[0], c.rxCop))
            FAIL("%s cancel receiver", c.name);
        canStatus(c.cll[0], PDU_COPST_CANCELLED, "teardown cancel");
    }
    static UNUM8 nul = 0;
    PDU_COP_CTRL_DATA sc{100, 0, 0, 0, {0, nullptr}, 0, nullptr};
    UNUM32 h;
    for (int i = 0; i < 2; i++) {
        if (CALL(PDUStartComPrimitive, hMod, c.cll[i], PDU_COPT_STOPCOMM, 0, &nul, &sc, nullptr, &h))
            FAIL("%s STOPCOMM", c.name);
        canStatus(c.cll[i], PDU_COPST_FINISHED, "stopcomm");
    }
    for (int i = 0; i < 2; i++) {
        CALL(PDUDisconnect, hMod, c.cll[i]);
        canStatus(c.cll[i], PDU_CLLST_OFFLINE, "disconnect");
        CALL(PDURegisterEventCallback, hMod, c.cll[i], nullptr);
        if (CALL(PDUDestroyComLogicalLink, hMod, c.cll[i]))
            FAIL("%s Destroy", c.name);
    }
}

// TX the way Tech2Win does: SENDRECV send-only, wait EXECUTING (not FINISHED)
static void canSend(UNUM32 cll, std::vector<UNUM8> data, UNUM32 txflag, bool temp = false) {
    UNUM8 tf[4] = {(UNUM8)(txflag >> 24), (UNUM8)(txflag >> 16), (UNUM8)(txflag >> 8), (UNUM8)txflag};
    PDU_COP_CTRL_DATA ctrl{500, 1, 0, temp, {4, tf}, 0, nullptr};
    cop(PDU_COPT_SENDRECV, data, &ctrl, 0, cll);
    canStatus(cll, PDU_COPST_EXECUTING, "tx");
}

static bool hasRes(UNUM32 cll, UNUM32 acc, UNUM32 flag, const std::vector<UNUM8> &d) {
    for (auto &r : rx[cll])
        if (r.acc == acc && r.flag == flag && r.d == d)
            return true;
    return false;
}

static bool hasTxDone(UNUM32 cll) {
    for (auto &r : rx[cll])
        if (r.flag & 0x08)
            return true;
    return false;
}

// wait until the CLL got a result like that
static void expectRes(UNUM32 cll, UNUM32 acc, UNUM32 flag, const std::vector<UNUM8> &d, const char *what) {
    for (int i = 0; i < 100 && !hasRes(cll, acc, flag, d); i++)
        waitStatus(0xFFFFFFFF, 20, cll);
    if (!hasRes(cll, acc, flag, d)) {
        printf("got on CLL %u:\n", (unsigned)cll);
        for (auto &r : rx[cll]) {
            printf("  acc %u flag 0x%X:", (unsigned)r.acc, (unsigned)r.flag);
            for (auto b : r.d)
                printf(" %02X", b);
            printf("\n");
        }
        FAIL("%s", what);
    }
    printf("ok: %s\n", what);
}

static std::vector<UNUM8> vin(UNUM32 id) {
    std::vector<UNUM8> d{(UNUM8)(id >> 24), (UNUM8)(id >> 16), (UNUM8)(id >> 8), (UNUM8)id, 0x5A, 0x90};
    for (const char *v = "YS3FD49Y241012345"; *v; v++)
        d.push_back(*v);
    return d;
}

static int canSession() {
    Chan sw{"SW-CAN", "SAE_J2411_SWCAN", {{1, id(PDU_OBJT_PINTYPE, "SINGLE")}}, 33333, {{32, 0, 0x545}, {33, 0x245, 0x645}}};
    Chan ms{"MS-CAN", "ISO_11898_2_DWCAN", {{3, id(PDU_OBJT_PINTYPE, "HI")}, {11, id(PDU_OBJT_PINTYPE, "LOW")}}, 95238, {}};
    Chan hs{"HS-CAN", "ISO_11898_2_DWCAN", {{6, id(PDU_OBJT_PINTYPE, "HI")}, {14, id(PDU_OBJT_PINTYPE, "LOW")}}, 500000,
            {{60, 0, 0x5E8}, {61, 0x7E0, 0x7E8}}};
    canCreate(sw);
    canCreate(ms);
    canCreate(hs);
    UNUM32 H0 = hs.cll[0], H1 = hs.cll[1], S0 = sw.cll[0], S1 = sw.cll[1];

    // SW-CAN high voltage wakeup: temp params, RAW CLL, DLC 0, waits FINISHED, no TX confirm
    setParam("CP_SwCan_HighVoltage", 1, S1);
    setParam("CP_CanFillerByteHandling", 0, S1);
    {
        PDU_COP_CTRL_DATA ctrl{500, 1, 0, 1, {4, zeros}, 0, nullptr};
        cop(PDU_COPT_SENDRECV, {0, 0, 1, 0}, &ctrl, 0, S1);
        canStatus(S1, PDU_COPST_FINISHED, "HV wakeup");
        if (hasTxDone(S1))
            FAIL("HV wakeup must not produce a TX confirm");
        printf("ok: SW-CAN HV wakeup\n");
    }
    // a mailbox write later commits the working buffer: HV must not stick
    for (UNUM32 c : {S0, S1}) {
        cop(PDU_COPT_UPDATEPARAM, {}, nullptr, 0, c);
        canStatus(c, PDU_COPST_FINISHED, "updateparam");
    }
    canSend(S1, {0, 0, 1, 1, 0xFE, 0x01, 0x3E}, 0x40);
    canSend(S0, {0, 0, 2, 0x45, 0x1A, 0x90}, 0x40);
    expectRes(S0, 33, 0x40, vin(0x645), "SW-CAN VIN from 0x645 (multi-frame RX, FC to 0x245)");

    // HS-CAN, T8-ish ECU on 0x7E0/0x7E8 + DPIDs on 0x5E8
    canSend(H0, {0, 0, 7, 0xE0, 0x1A, 0x90}, 0x40);
    expectRes(H0, 61, 0x40, vin(0x7E8), "HS-CAN VIN (USDT, PCI stripped, AcceptanceId = mailbox)");
    if (!hasTxDone(H0))
        FAIL("no TX indication for the HS request");
    printf("ok: TX indication\n");

    std::vector<UNUM8> big{0, 0, 7, 0xE0, 0x3B, 0x01};
    for (int i = 0; i < 18; i++)
        big.push_back(i);
    canSend(H0, big, 0x40);
    expectRes(H0, 61, 0x40, {0, 0, 7, 0xE8, 0x7B, 0x01, 20}, "segmented TX (FF, wait FC, CFs)");

    canSend(H0, {0, 0, 7, 0xE0, 0x27, 0x01}, 0x40);
    expectRes(H0, 61, 0x40, {0, 0, 7, 0xE8, 0x7F, 0x27, 0x78}, "response pending passed through");
    expectRes(H0, 61, 0x40, {0, 0, 7, 0xE8, 0x67, 0x01, 0x12, 0x34}, "answer after response pending");

    canSend(H0, {0, 0, 7, 0xE0, 0xAA, 0x01, 0x02, 0x03}, 0x40);
    expectRes(H0, 60, 0x00, {0, 0, 5, 0xE8, 0x02, 1, 2, 3, 4, 5, 6, 7}, "UUDT DPID 02 (all bytes, no USDT flag)");
    expectRes(H0, 60, 0x00, {0, 0, 5, 0xE8, 0x03, 1, 2, 3, 4, 5, 6, 7}, "UUDT DPID 03");

    // functional AllNodes request with extended address FE, temp RequestAddrMode
    rx[H0].clear();
    setParam("CP_RequestAddrMode", 2, H0);
    canSend(H0, {0, 0, 1, 1, 0xFE, 0x1A, 0x90}, 0xC0, true);
    expectRes(H0, 61, 0x40, vin(0x7E8), "functional 0x101 FE request answered on 0x7E8");

    // the firmware's own TesterPresent on the RAW CLL
    canSend(H1, {0, 0, 1, 1, 0xFE, 0x01, 0x3E}, 0x40);
    for (int i = 0; i < 20 && !hasTxDone(H1); i++)
        waitStatus(0xFFFFFFFF, 20, H1);
    if (!hasTxDone(H1))
        FAIL("no TX indication on the RAW CLL");
    printf("ok: raw TesterPresent + TX indication\n");

    // re-arm gap: receiver cancelled, answer arrives, new receiver gets it
    rx[H0].clear();
    if (CALL(PDUCancelComPrimitive, hMod, H0, hs.rxCop))
        FAIL("cancel receiver must return 0");
    canStatus(H0, PDU_COPST_CANCELLED, "re-arm cancel");
    canSend(H0, {0, 0, 7, 0xE0, 0x1A, 0x90}, 0x40);
    Sleep(200);
    canReceiver(hs);
    expectRes(H0, 61, 0x40, vin(0x7E8), "answer from the re-arm gap delivered to the new receiver");

    for (auto &kv : rx)
        for (auto &r : kv.second)
            if (r.d.size() >= 4 && r.d[2] == 0x00 && r.d[3] == 0xC9)
                FAIL("unrelated frame 0x0C9 was delivered");
    printf("ok: unrelated traffic dropped\n");

    canTeardown(hs);
    canTeardown(ms);
    canTeardown(sw);

    // what the adapter saw
    FILE *f = fopen(getenv("FAKE_LOG"), "r");
    std::string log;
    char line[512];
    while (f && fgets(line, sizeof(line), f))
        log += line;
    if (f)
        fclose(f);
    struct {
        const char *what;
        bool want;
    } checks[] = {
        {"Connect proto=0x8008", true},                                // SW-CAN
        {"SetConfig 0x8001=256", true},                                // pin 1
        {"Connect proto=0x5 ", true},                                  // HS-CAN plain
        {"Connect proto=0x8004", false},                               // MS-CAN never used, never opened
        {"Write flags=1024 00 00 01 00\n", true},                      // HV wakeup, DLC 0
        {"Write 00 00 01 01 FE 02 1A 90 00 00 00 00\n", true},         // functional, ext addr FE, padded
        {"Write 00 00 07 E0 30 00 00 00 00 00 00 00\n", true},         // our flow control
        {"Write flags=1024 00 00 01 01 FE 01 3E", false},              // HV did not stick
    };
    for (auto &c : checks)
        if ((log.find(c.what) != std::string::npos) != c.want)
            FAIL("adapter log %s '%s'", c.want ? "lacks" : "has", c.what);
    printf("ok: adapter saw the right channels and frames\n");
    return 0;
}

int main(int argc, char **argv) {
    std::string pins = arg(argc, argv, "pins", "7"), expect = arg(argc, argv, "expect", "ok"), j1962 = arg(argc, argv, "j1962", "");
    bool kw82 = arg(argc, argv, "proto", "") == "kw82";
    UNUM8 ecu = strtoul(arg(argc, argv, "ecu", "41").c_str(), nullptr, 16);
    ev = CreateEventA(nullptr, FALSE, FALSE, nullptr);

    // enumeration pass: load, list, unload (Tech2Win does this for its VCI dialog)
    loadFromRootFile();
    PDU_MODULE_ITEM *mods = nullptr;
    CALL(PDUConstruct, (CHAR8 *)"", nullptr);
    if (CALL(PDUGetModuleIds, &mods) || !mods || mods->NumEntries < 1)
        FAIL("no modules");
    std::string name = mods->pModuleData[0].pVendorModuleName;
    if (name.find(' ') != std::string::npos || mods->pModuleData[0].ModuleStatus != PDU_MODST_AVAIL || !mods->pModuleData[0].pVendorAdditionalInfo)
        FAIL("module 0 '%s' not usable by Tech2Win", name.c_str());
    CALL(PDUDestroyItem, mods);
    CALL(PDUDestruct);
    FreeLibrary(dll);

    // real load
    loadFromRootFile();
    CALL(PDUConstruct, (CHAR8 *)"", nullptr);
    CALL(PDUGetModuleIds, &mods);
    for (UNUM32 i = 0; i < mods->NumEntries; i++)
        if (name == mods->pModuleData[i].pVendorModuleName)
            hMod = mods->pModuleData[i].hMod;
    if (CALL(PDUModuleConnect, hMod))
        FAIL("ModuleConnect");
    CALL(PDURegisterEventCallback, PDU_HANDLE_UNDEF, PDU_HANDLE_UNDEF, cb);
    PDU_VERSION_DATA ver;
    if (CALL(PDUGetVersion, hMod, &ver))
        FAIL("GetVersion");
    PDU_DATA_ITEM in{0x1000, nullptr}, *vb = &in;
    if (CALL(PDUIoCtl, hMod, PDU_HANDLE_UNDEF, id(PDU_OBJT_IO_CTRL, "PDU_IOCTL_READ_VBATT"), nullptr, &vb) || *(UNUM32 *)vb->pData != 12600)
        FAIL("READ_VBATT");
    if (arg(argc, argv, "proto", "") == "can") {
        int rc = canSession();
        CALL(PDUModuleDisconnect, hMod);
        CALL(PDUDestruct);
        FreeLibrary(dll);
        printf("ok\n");
        return rc;
    }

    // link: K = first pin, L = second (Tech2Win: {PIN_A, K}, {PIN_B, L} when PIN_B != 0)
    std::vector<PDU_PIN_DATA> pd;
    for (const char *p = pins.c_str(); *p; p += strspn(p, ","))
        pd.push_back({(UNUM32)strtoul(p, (char **)&p, 10), id(PDU_OBJT_PINTYPE, pd.empty() ? "K" : "L")});
    PDU_RSC_DATA rsc{kw82 ? id(PDU_OBJT_BUSTYPE, "KW_UART") : id(PDU_OBJT_BUSTYPE, "ISO_14230_1_UART"),
                     kw82 ? id(PDU_OBJT_PROTOCOL, "KW82_on_KW_UART") : id(PDU_OBJT_PROTOCOL, "ISO_14230_3_on_ISO_14230_2"),
                     (UNUM32)pd.size(), pd.data()};
    UNUM8 rawFlag[4] = {0x80, 0, 0, 0};
    PDU_FLAG_DATA cllFlag{4, rawFlag};
    static int cllTag = 0;
    if (CALL(PDUCreateComLogicalLink, hMod, &rsc, PDU_ID_UNDEF, &cllTag, &hCLL, &cllFlag) || !hCLL)
        FAIL("CreateComLogicalLink");
    CALL(PDURegisterEventCallback, hMod, hCLL, cb);
    T_PDU_ERROR rc = CALL(PDUConnect, hMod, hCLL);
    if (expect == "connectfail") {
        if (!rc)
            FAIL("Connect should have failed");
        printf("connect failed as expected\n");
        return 0;
    }
    if (rc || !waitStatus(PDU_CLLST_ONLINE))
        FAIL("Connect / ONLINE");

    T_PDU_STATUS st;
    UNUM32 ts, x;
    if (CALL(PDUGetStatus, hMod, hCLL, 0, &st, &ts, &x) != PDU_ERR_INVALID_HANDLE)
        FAIL("GetStatus(hCoP 0) must be INVALID_HANDLE");

    // init, in Tech2Win's ComParam order
    if (!kw82)
        setParam("CP_Baudrate", 10400);
    setParam("CP_TesterPresentHandling", 0);
    setParam("CP_RepeatReqCountApp", kw82 ? 0 : 2);
    setParam("CP_StartMsgIndEnable", 0);
    CALL(PDUIoCtl, hMod, hCLL, id(PDU_OBJT_IO_CTRL, "PDU_IOCTL_CLEAR_MSG_FILTER"), nullptr, nullptr);
    setParam("CP_P2Min", kw82 ? 8000 : 25000);
    setParam("CP_P2Max", 300000);
    setParam("CP_P4Min", kw82 ? 6000 : 5000);
    if (kw82) {
        setParam("CP_5BaudInitBaudrate", 2000);
        setParam("CP_5BaudAddressPhys", ecu);
        setParam("CP_TIdle", 2100000);
        setParam("CP_W4Min", 25000);
    } else {
        setParam("CP_TIdle", 1000);
        setParam("CP_TWup", 50000);
        setParam("CP_TInil", 25000);
        setParam("CP_InitializationSettings", 2);
    }
    cop(PDU_COPT_UPDATEPARAM, {}, nullptr, kw82 ? 4 : 3);
    if (!waitStatus(PDU_COPST_FINISHED))
        FAIL("UPDATEPARAM FINISHED");

    PDU_COP_CTRL_DATA start{0, 1, 1, 0, {kw82 ? 0u : 4u, kw82 ? nullptr : zeros}, 1, &er};
    results.clear();
    errors = 0;
    cop(PDU_COPT_STARTCOMM, kw82 ? std::vector<UNUM8>{} : kwp({0x81, ecu, 0xF1, 0x81}), &start, kw82 ? 4 : 3);
    if (!waitStatus(PDU_COPST_FINISHED, 5000))
        FAIL("STARTCOMM FINISHED");
    if (expect == "timeout") {
        if (errors != 1 || !results.empty())
            FAIL("silent ECU: want 1 ERROR and no RESULT, got %d / %d", errors, (int)results.size());
        printf("init timed out as expected\n");
        return 0;
    }
    if (errors || results.size() != 1)
        FAIL("STARTCOMM: %d errors, %d results", errors, (int)results.size());
    if (kw82) {
        if (results[0].size() != 2)
            FAIL("5-baud RESULT must be [KB1 KB2], got %d bytes", (int)results[0].size());
        PDU_PARAM_ITEM *p;
        if (CALL(PDUGetComParam, hMod, hCLL, id(PDU_OBJT_COMPARAM, "CP_Baudrate"), &p) || p->ComParamDataType != PDU_PT_UNUM32 || *(UNUM32 *)p->pComParamData != 8192)
            FAIL("CP_Baudrate readback");
        CALL(PDUDestroyItem, p);
    } else if (results[0] != kwp({0x83, 0xF1, ecu, 0xC1, 0xEF, 0x8F})) {
        FAIL("StartComm reply");
    }
    if (CALL(PDUGetStatus, hMod, hCLL, PDU_HANDLE_UNDEF, &st, &ts, &x) || st != PDU_CLLST_COMM_STARTED)
        FAIL("CLL not COMM_STARTED");

    // requests: SENDRECV NumRecv -1, wait EXECUTING, each new one cancels the previous
    PDU_COP_CTRL_DATA req{0, 1, -1, 0, {4, zeros}, 1, &er};
    UNUM32 prev = 0;
    for (int i = 0; i < 3; i++) {
        if (prev) {
            if (CALL(PDUCancelComPrimitive, hMod, hCLL, prev) || !waitStatus(PDU_COPST_CANCELLED))
                FAIL("cancel request %d", i);
            if (CALL(PDUCancelComPrimitive, hMod, hCLL, prev) == 0)
                FAIL("second cancel must fail");
        }
        results.clear();
        auto frame = kw82 ? std::vector<UNUM8>{0x03, 0x1A, (UNUM8)(0x90 + i), 0x00, (UNUM8)(0xAD + i)} : kwp({0x82, ecu, 0xF1, 0x1A, (UNUM8)(0x90 + i)});
        DWORD t0 = GetTickCount();
        prev = cop(PDU_COPT_SENDRECV, frame, &req, 1);
        if (!waitStatus(PDU_COPST_EXECUTING))
            FAIL("request %d EXECUTING", i);
        for (int w = 0; w < 100 && results.empty(); w++)
            waitStatus(0xFFFFFFFF, 10);
        if (results.size() != 1)
            FAIL("request %d: %d results", i, (int)results.size());
        printf("request %d answered in %lu ms: %02X %02X %02X\n", i, GetTickCount() - t0, results[0][0], results[0][1], results[0][2]);
    }
    if (CALL(PDUCancelComPrimitive, hMod, hCLL, prev) || !waitStatus(PDU_COPST_CANCELLED))
        FAIL("last cancel");

    if (!kw82) { // Tech2Win's own TesterPresent: NumRecv 1, waits FINISHED
        PDU_COP_CTRL_DATA tp{0, 1, 1, 0, {4, zeros}, 1, &er};
        results.clear();
        cop(PDU_COPT_SENDRECV, kwp({0x81, ecu, 0xF1, 0x3E}), &tp, 2);
        if (!waitStatus(PDU_COPST_FINISHED) || results.size() != 1 || errors)
            FAIL("TesterPresent");
    }

    PDU_COP_CTRL_DATA stop{100, 0, 0, 0, {0, nullptr}, 0, nullptr};
    cop(PDU_COPT_STOPCOMM, {}, &stop, 0);
    if (!waitStatus(PDU_COPST_FINISHED))
        FAIL("STOPCOMM");
    if (CALL(PDUGetStatus, hMod, hCLL, PDU_HANDLE_UNDEF, &st, &ts, &x) || st != PDU_CLLST_ONLINE)
        FAIL("CLL not ONLINE after STOPCOMM");

    if (CALL(PDUDisconnect, hMod, hCLL) || !waitStatus(PDU_CLLST_OFFLINE))
        FAIL("Disconnect / OFFLINE");
    if (CALL(PDUDestroyComLogicalLink, hMod, hCLL))
        FAIL("Destroy");
    CALL(PDURegisterEventCallback, hMod, hCLL, nullptr);
    CALL(PDUModuleDisconnect, hMod);
    CALL(PDUDestruct);
    FreeLibrary(dll);

    // what the adapter saw
    if (!j1962.empty()) {
        FILE *f = fopen(getenv("FAKE_LOG"), "r");
        char line[512];
        bool seen = false;
        while (f && fgets(line, sizeof(line), f))
            if (strstr(line, "SetConfig 0x8001="))
                seen = true, printf("adapter: %s", line);
        if (f)
            fclose(f);
        if (j1962 == "none" && seen)
            FAIL("J1962_PINS should not be set for pin 7");
        if (j1962 != "none" && !seen)
            FAIL("J1962_PINS %s not set", j1962.c_str());
    }
    printf("ok\n");
    return 0;
}
