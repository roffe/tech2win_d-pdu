// Fake J2534 v04.04 adapter with SAAB ECUs behind it, for testing t2wdpdu under
// wine. Behaves like a Mongoose Pro GM II: one K-line, no L-line, so J1962_PINS
// with a secondary K-line pin is rejected.
//
// K-line: 0x41 (DICE) sits on pin 7, 0x28 (ABS/TC) on pin 8. FAKE_BRIDGED=1
// simulates a 7+8 bridged OBD outlet (everything answers on either pin).
// CAN: one GMLAN ECU per bus doing ISO-TP (multi-frame both ways, flow control,
// padding, response pending, UUDT), plus unrelated bus traffic.
// Every call is logged to FAKE_LOG so the test can check what the DLL did.
#include "j2534.h"
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#define ERR_NOT_SUPPORTED       0x01
#define ERR_INVALID_CHANNEL_ID  0x02
#define ERR_INVALID_PROTOCOL_ID 0x05
#define ERR_INVALID_IOCTL_VALUE 0x1A
#define CAN         0x05
#define CAN_PS      0x8004
#define SW_CAN_PS   0x8008
#define SW_CAN_HV_TX 0x0400
#define SW_CAN_HS    0x8000
#define SW_CAN_NS    0x8001

typedef std::vector<unsigned char> Bytes;

struct Ecu {
    unsigned req, resp, uudt;
};

struct Chan {
    unsigned long proto, pins;
    bool filter = false;
    std::deque<PASSTHRU_MSG> rx;
    unsigned char kwAddr = 0; // ECU woken by the last 5-baud init
    // ISO-TP state of the bus ECU
    Bytes in;                 // segmented request being received
    size_t inLen = 0;
    Bytes out;                // segmented response still to send after FC
    size_t outPos = 0;
    unsigned char outSn = 0;
};

static std::mutex mu;
static std::map<unsigned long, Chan> chans;
static unsigned long nextChan = 1;
static bool open_;
static std::string lastErr;

static void logf(const char *fmt, ...) {
    const char *p = getenv("FAKE_LOG");
    FILE *f = p ? fopen(p, "a") : nullptr;
    if (!f)
        return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

static std::string hex(const unsigned char *b, unsigned long n) {
    std::string s;
    char t[4];
    for (unsigned long i = 0; i < n; i++) {
        snprintf(t, sizeof(t), i ? " %02X" : "%02X", b[i]);
        s += t;
    }
    return s;
}

static bool isCan(unsigned long p) { return p == CAN || p == CAN_PS || p == SW_CAN_PS; }

// ---------------------------------------------------------------- K-line

static bool ecuReachable(const Chan &c, unsigned char addr) {
    if (getenv("FAKE_BRIDGED"))
        return true;
    int pin = c.pins >> 8;
    return (addr == 0x41 && pin == 7) || (addr == 0x28 && pin == 8);
}

static unsigned char cs(const unsigned char *b, int n) {
    unsigned char c = 0;
    while (n--)
        c += *b++;
    return c;
}

static void pushSom(Chan &c) {
    PASSTHRU_MSG som{};
    som.ProtocolID = c.proto;
    som.RxStatus = START_OF_MESSAGE;
    c.rx.push_back(som);
}

// Build "<fmt> F1 <src> <data...> <cs>" with the length in the format byte.
static PASSTHRU_MSG kwpReply(const Chan &c, unsigned char src, const unsigned char *data, int n) {
    PASSTHRU_MSG m{};
    m.ProtocolID = c.proto;
    m.Data[0] = 0x80 | n;
    m.Data[1] = 0xF1;
    m.Data[2] = src;
    memcpy(m.Data + 3, data, n);
    m.Data[3 + n] = cs(m.Data, 3 + n);
    m.DataSize = 4 + n;
    return m;
}

static void klineWrite(Chan &c, const PASSTHRU_MSG &m) {
    if (c.proto == ISO9141 || c.proto == ISO9141_PS) { // KW82: [N][..][ckH][ckL], answer SID+0x40
        int n = m.Data[0];
        if (!ecuReachable(c, c.kwAddr) || n < 2 || m.DataSize != (unsigned long)n + 2)
            return;
        PASSTHRU_MSG r{};
        r.ProtocolID = c.proto;
        memcpy(r.Data, m.Data, n);
        r.Data[1] += 0x40;
        unsigned ck = 0;
        for (int j = 0; j < n; j++)
            ck += r.Data[j];
        r.Data[n] = ck >> 8;
        r.Data[n + 1] = ck;
        r.DataSize = n + 2;
        pushSom(c);
        c.rx.push_back(r);
        return;
    }
    unsigned char tgt = m.Data[1];
    int hdr = (m.Data[0] & 0x3F) ? 3 : 4;
    int len = (m.Data[0] & 0x3F) ? (m.Data[0] & 0x3F) : m.Data[3];
    if (!ecuReachable(c, tgt) || len < 1)
        return;
    unsigned char resp[260];
    resp[0] = m.Data[hdr] + 0x40; // positive response, echo the rest
    memcpy(resp + 1, m.Data + hdr + 1, len - 1);
    pushSom(c);
    c.rx.push_back(kwpReply(c, tgt, resp, len));
}

// ---------------------------------------------------------------- CAN / GMLAN

static Ecu busEcu(const Chan &c) {
    if (c.proto == SW_CAN_PS)
        return {0x245, 0x645, 0x545}; // CIM-ish, single wire
    if (c.proto == CAN_PS && c.pins == 0x030B)
        return {0x241, 0x641, 0x541}; // body module, mid speed
    return {0x7E0, 0x7E8, 0x5E8};     // ECM, high speed
}

static void canFrame(Chan &c, unsigned id, Bytes d, bool pad = true) {
    PASSTHRU_MSG m{};
    m.ProtocolID = c.proto;
    m.Data[0] = id >> 24, m.Data[1] = id >> 16, m.Data[2] = id >> 8, m.Data[3] = id;
    while (pad && d.size() < 8)
        d.push_back(0xAA); // GMLAN pads; the DLL must go by the PCI length
    memcpy(m.Data + 4, d.data(), d.size());
    m.DataSize = 4 + d.size();
    c.rx.push_back(m);
}

static void sendConsecutive(Chan &c, Ecu e) {
    while (c.outPos < c.out.size()) {
        Bytes f{(unsigned char)(0x20 | (c.outSn++ & 0x0F))};
        for (int i = 0; i < 7 && c.outPos < c.out.size(); i++)
            f.push_back(c.out[c.outPos++]);
        canFrame(c, e.resp, f);
    }
    c.out.clear();
}

// ISO-TP send from the ECU: single frame, or first frame and the rest after the tester's FC
static void ecuSend(Chan &c, Ecu e, const Bytes &p) {
    if (p.size() <= 7) {
        Bytes f{(unsigned char)p.size()};
        f.insert(f.end(), p.begin(), p.end());
        canFrame(c, e.resp, f);
        return;
    }
    Bytes f{(unsigned char)(0x10 | (p.size() >> 8)), (unsigned char)p.size()};
    f.insert(f.end(), p.begin(), p.begin() + 6);
    canFrame(c, e.resp, f);
    c.out = p;
    c.outPos = 6;
    c.outSn = 1;
}

static void ecuRequest(Chan &c, Ecu e, const Bytes &req) {
    logf("ECU 0x%03X request %s", e.req, hex(req.data(), req.size()).c_str());
    unsigned char sid = req[0];
    if (sid == 0x3E && req.size() == 1)
        return; // functional TesterPresent: GMLAN ECUs keep quiet
    if (sid == 0x1A && req.size() == 2 && req[1] == 0x90) {
        Bytes r{0x5A, 0x90};
        for (const char *v = "YS3FD49Y241012345"; *v; v++)
            r.push_back(*v);
        ecuSend(c, e, r);
        return;
    }
    if (sid == 0x27) { // seed after a response pending
        ecuSend(c, e, {0x7F, 0x27, 0x78});
        ecuSend(c, e, {0x67, req[1], 0x12, 0x34});
        return;
    }
    if (sid == 0xAA) { // read DPIDs: one UUDT frame per DPID, no PCI
        for (size_t i = 2; i < req.size(); i++)
            canFrame(c, e.uudt, {req[i], 1, 2, 3, 4, 5, 6, 7}, false);
        return;
    }
    if (sid == 0x3B) { // long write came in segmented: answer with its length
        ecuSend(c, e, {0x7B, req[1], (unsigned char)req.size()});
        return;
    }
    Bytes r(req);
    r[0] += 0x40;
    ecuSend(c, e, r);
}

static void canWrite(Chan &c, const PASSTHRU_MSG &m) {
    unsigned id = m.Data[0] << 24 | m.Data[1] << 16 | m.Data[2] << 8 | m.Data[3];
    const unsigned char *d = m.Data + 4;
    unsigned n = m.DataSize - 4;
    if (m.TxFlags & SW_CAN_HV_TX)
        logf("HV wakeup id 0x%X", id);
    if (!c.filter || n == 0)
        return;
    canFrame(c, 0x0C9, {1, 2, 3, 4, 5, 6, 7, 8}, false); // unrelated traffic
    Ecu e = busEcu(c);
    if (id == 0x101 && d[0] == 0xFE) { // functional, extended address FE = AllNodes
        if ((d[1] & 0xF0) == 0 && d[1] >= 1)
            ecuRequest(c, e, Bytes(d + 2, d + 2 + (d[1] & 0x0F)));
        return;
    }
    if (id != e.req)
        return;
    switch (d[0] >> 4) {
    case 0: // single frame
        if ((d[0] & 0x0F) >= 1 && (d[0] & 0x0F) < n)
            ecuRequest(c, e, Bytes(d + 1, d + 1 + (d[0] & 0x0F)));
        break;
    case 1: // first frame: flow control, then collect
        c.inLen = (d[0] & 0x0F) << 8 | d[1];
        c.in.assign(d + 2, d + n);
        canFrame(c, e.resp, {0x30, 0x00, 0x00});
        break;
    case 2: // consecutive frame
        c.in.insert(c.in.end(), d + 1, d + n);
        if (c.inLen && c.in.size() >= c.inLen) {
            c.in.resize(c.inLen);
            c.inLen = 0;
            ecuRequest(c, e, c.in);
        }
        break;
    case 3: // flow control from the tester
        if (d[0] == 0x30)
            sendConsecutive(c, e);
        break;
    }
}

// ---------------------------------------------------------------- J2534

extern "C" {

long WINAPI PassThruOpen(void *, unsigned long *dev) {
    std::lock_guard<std::mutex> l(mu);
    logf("Open");
    if (open_)
        return 0x0E; // ERR_DEVICE_IN_USE
    open_ = true;
    *dev = 1;
    return STATUS_NOERROR;
}

long WINAPI PassThruClose(unsigned long) {
    std::lock_guard<std::mutex> l(mu);
    logf("Close");
    open_ = false;
    chans.clear();
    return STATUS_NOERROR;
}

long WINAPI PassThruConnect(unsigned long, unsigned long p, unsigned long flags, unsigned long baud, unsigned long *ch) {
    std::lock_guard<std::mutex> l(mu);
    logf("Connect proto=0x%lX flags=0x%lX baud=%lu", p, flags, baud);
    if (p != ISO14230 && p != ISO14230_PS && p != ISO9141 && p != ISO9141_PS && !isCan(p))
        return ERR_INVALID_PROTOCOL_ID;
    for (auto &kv : chans) // one K-line, one channel per CAN bus kind
        if (kv.second.proto == p || (!isCan(p) && !isCan(kv.second.proto)))
            return 0x0E;
    Chan c;
    c.proto = p;
    c.pins = isCan(p) ? 0 : 0x0700;
    *ch = nextChan++;
    chans[*ch] = c;
    return STATUS_NOERROR;
}

long WINAPI PassThruDisconnect(unsigned long ch) {
    std::lock_guard<std::mutex> l(mu);
    logf("Disconnect");
    return chans.erase(ch) ? STATUS_NOERROR : ERR_INVALID_CHANNEL_ID;
}

long WINAPI PassThruReadMsgs(unsigned long ch, PASSTHRU_MSG *msgs, unsigned long *num, unsigned long timeout) {
    for (unsigned long waited = 0;; waited += 5) {
        {
            std::lock_guard<std::mutex> l(mu);
            auto it = chans.find(ch);
            if (it == chans.end())
                return ERR_INVALID_CHANNEL_ID;
            auto &rx = it->second.rx;
            if (!rx.empty()) {
                unsigned long n = 0;
                while (n < *num && !rx.empty()) {
                    msgs[n++] = rx.front();
                    rx.pop_front();
                }
                *num = n;
                return STATUS_NOERROR;
            }
        }
        if (waited >= timeout) {
            *num = 0;
            return timeout ? ERR_TIMEOUT : ERR_BUFFER_EMPTY;
        }
        Sleep(5);
    }
}

long WINAPI PassThruWriteMsgs(unsigned long ch, PASSTHRU_MSG *msgs, unsigned long *num, unsigned long) {
    std::lock_guard<std::mutex> l(mu);
    auto it = chans.find(ch);
    if (it == chans.end())
        return ERR_INVALID_CHANNEL_ID;
    Chan &c = it->second;
    for (unsigned long i = 0; i < *num; i++) {
        const PASSTHRU_MSG &m = msgs[i];
        logf("Write%s %s", m.TxFlags ? (" flags=" + std::to_string(m.TxFlags)).c_str() : "", hex(m.Data, m.DataSize).c_str());
        if (isCan(c.proto))
            canWrite(c, m);
        else if (c.filter)
            klineWrite(c, m);
    }
    return STATUS_NOERROR;
}

long WINAPI PassThruStartMsgFilter(unsigned long ch, unsigned long type, PASSTHRU_MSG *, PASSTHRU_MSG *, PASSTHRU_MSG *, unsigned long *id) {
    std::lock_guard<std::mutex> l(mu);
    logf("StartMsgFilter type=%lu", type);
    auto it = chans.find(ch);
    if (it == chans.end())
        return ERR_INVALID_CHANNEL_ID;
    it->second.filter = true;
    *id = 1;
    return STATUS_NOERROR;
}

long WINAPI PassThruStopMsgFilter(unsigned long, unsigned long) { return STATUS_NOERROR; }
long WINAPI PassThruStartPeriodicMsg(unsigned long, PASSTHRU_MSG *, unsigned long *, unsigned long) { return ERR_NOT_SUPPORTED; }
long WINAPI PassThruStopPeriodicMsg(unsigned long, unsigned long) { return ERR_NOT_SUPPORTED; }
long WINAPI PassThruSetProgrammingVoltage(unsigned long, unsigned long, unsigned long) { return ERR_NOT_SUPPORTED; }

long WINAPI PassThruReadVersion(unsigned long, char *fw, char *dll, char *api) {
    strcpy(fw, "1.0");
    strcpy(dll, "1.0");
    strcpy(api, "04.04");
    return STATUS_NOERROR;
}

long WINAPI PassThruGetLastError(char *buf) {
    std::lock_guard<std::mutex> l(mu);
    strncpy(buf, lastErr.c_str(), 79);
    buf[79] = 0;
    return STATUS_NOERROR;
}

long WINAPI PassThruIoctl(unsigned long ch, unsigned long id, void *in, void *out) {
    std::lock_guard<std::mutex> l(mu);
    if (id == READ_VBATT) {
        *(unsigned long *)out = 12600;
        return STATUS_NOERROR;
    }
    auto it = chans.find(ch);
    if (it == chans.end())
        return ERR_INVALID_CHANNEL_ID;
    Chan &c = it->second;
    switch (id) {
    case SET_CONFIG: {
        SCONFIG_LIST *cl = (SCONFIG_LIST *)in;
        for (unsigned long i = 0; i < cl->NumOfParams; i++) {
            SCONFIG s = cl->ConfigPtr[i];
            logf("SetConfig 0x%lX=%lu", s.Parameter, s.Value);
            if (s.Parameter == J1962_PINS) {
                if (c.proto == SW_CAN_PS) {
                    if (s.Value != 0x0100)
                        return ERR_INVALID_IOCTL_VALUE;
                } else if (c.proto == CAN_PS) {
                    if (s.Value != 0x030B && s.Value != 0x060E)
                        return ERR_INVALID_IOCTL_VALUE;
                } else if (c.proto == ISO14230_PS || c.proto == ISO9141_PS) {
                    int k = s.Value >> 8, l2 = s.Value & 0xFF;
                    if ((k != 7 && k != 8) || l2 != 0) {
                        lastErr = "pin configuration not supported";
                        return ERR_INVALID_IOCTL_VALUE;
                    }
                } else {
                    return ERR_NOT_SUPPORTED;
                }
                c.pins = s.Value;
            }
        }
        return STATUS_NOERROR;
    }
    case FAST_INIT: {
        PASSTHRU_MSG *req = (PASSTHRU_MSG *)in, *resp = (PASSTHRU_MSG *)out;
        logf("FastInit %s pins=0x%04lX", hex(req->Data, req->DataSize).c_str(), c.pins);
        if (!ecuReachable(c, req->Data[1])) {
            lastErr = "no response to fast init";
            return ERR_TIMEOUT;
        }
        const unsigned char ok[] = {0xC1, 0xEF, 0x8F};
        *resp = kwpReply(c, req->Data[1], ok, 3);
        return STATUS_NOERROR;
    }
    case FIVE_BAUD_INIT: {
        SBYTE_ARRAY *a = (SBYTE_ARRAY *)in, *kb = (SBYTE_ARRAY *)out;
        logf("FiveBaudInit 0x%02X pins=0x%04lX", a->BytePtr[0], c.pins);
        c.kwAddr = a->BytePtr[0];
        if (!ecuReachable(c, c.kwAddr)) {
            lastErr = "no sync byte";
            return ERR_TIMEOUT;
        }
        kb->BytePtr[0] = 0x08;
        kb->BytePtr[1] = 0x82;
        kb->NumOfBytes = 2;
        return STATUS_NOERROR;
    }
    case SW_CAN_HS:
    case SW_CAN_NS:
        logf("SwCan %s", id == SW_CAN_HS ? "high speed" : "normal speed");
        return c.proto == SW_CAN_PS ? STATUS_NOERROR : ERR_NOT_SUPPORTED;
    case CLEAR_RX_BUFFER:
        c.rx.clear();
        return STATUS_NOERROR;
    case CLEAR_TX_BUFFER:
    case GET_CONFIG:
        return STATUS_NOERROR;
    }
    return ERR_INVALID_IOCTL_VALUE;
}
}
