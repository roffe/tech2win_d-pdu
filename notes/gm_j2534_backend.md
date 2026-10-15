# GM Tech2Win J2534 backend (dormant): what it actually does

Source: `RE/tech2win/out/decomp_all.c` (Ghidra decomp of `emulator.exe`, build 2019-12-22).
Line numbers below are in that file. A bunch of the per-protocol open/close/poll/thread
functions were never turned into functions by Ghidra (the dispatch table only points at
`LAB_xxxxxxxx`), so they are NOT in decomp_all.c. I decompiled those from a scratch copy of the
project and list them by address as `gap 0x....`.

Reminder: none of this runs in this build. `DAT_00fbf152` (backend selector) is only ever 0
(D-PDU), the help file says J2534 is "always grayed out" and `-PJ` ends up as D-PDU too. But the
logic is complete, and it's GM's own idea of how the Tech2 VCI link layer maps onto
SAE J2534 04.04 + J2534-2 (with a lot of MX2/MDI2 OEM extensions).

Emulated time: GM converts everything to 68332 cycles, 16.777216 MHz, so `x * 0x4189` = x ms.
Handy constants that show up below: `0x8312`=2 ms, `0x147ad`=5 ms, `0x28f5a`=10 ms,
`0x66661`=25 ms, `0xb8515`=45 ms, `0xcccc2`=50 ms, `0xf5c1c`=60 ms, `0x3fffca`=250 ms.

## Summary table

| Module (file) | Link type (Link_Open) | Internal proto idx | J2534 ProtocolID | Connect flags | Baud | Pins (J1962_PINS 0x8001) | Init |
|---|---|---|---|---|---|---|---|
| J2534_kw2000.c | 0x1308 KW2000 | 1 | **0x8003 ISO14230_PS** | **0x200 ISO9141_NO_CHECKSUM** | **10400 (0x28A0)** | (pinA<<8)\|pinB, no default | FAST_INIT with the firmware's StartComm frame, or OEM 0x10001 + FIVE_BAUD_INIT |
| J2534_kw82.c | 0x1302 KW82 | 2 | 0x10002 (OEM) | 0x200 | 9615 (0x258F) | (pinA<<8)\|pinB, default K=7 L=15 | OEM 0x10001 + FIVE_BAUD_INIT |
| J2534_kw81.c | 0x1301 KW81 | 3 | 0x10001 (OEM) | 0x200 | 9600 | (pinA<<8)\|15, default K=7 | OEM 0x10001 + FIVE_BAUD_INIT (IB=5) or OEM ioctl 0x10005 (IB=300) |
| J2534_kw309.c | 0x130B KW309 | 5 | **3 ISO9141** (not _PS!) | 0x200 | 4800 | (pinA<<8)\|15, default K=7 | OEM 0x10001 + FIVE_BAUD_INIT |
| J2534_wabco.c | 0x1303 KW71 | 10 | 0x10005 (OEM) | 0 | 9600 | (pinA<<8)\|pinB | OEM 0x10001 + FIVE_BAUD_INIT |
| J2534_iszhd.c | 0x1B00 HeavyDuty | 11 | 0x10004 (OEM) | 0 | 'BR' attr or 62500 | from 12V/24V hints, see ISZHD | none |
| J2534_J1708.c | 0x1200 J1708 | 12 | 0x800D (J1708_PS) | 0x200 | 9600 | 0x1A1B (side cable 26/27) | none |
| J2534_XDE.c | 0x1000 XDE/GM-UART | 8 | 0x10003 (OEM, not GM_UART_PS 0x8009) | 0 | 8192 | none | none |
| J2534_CLASS2.c | 0x2000 CLASS2 | 7 | 1 J1850VPW | 0 | 10400 | none (computed, never set) | none |
| J2534_can.c | 0x1A00/0x4001 CAN | 0 | 6 ISO15765 (HS-CAN only) | 0 | 500000 | none (logged as 6/14) | none |

`ISO9141_PS 0x8002` and `ISO9141_K_LINE_ONLY 0x1000` are never used anywhere in the backend
(checked the whole 0x420000-0x463000 range for the immediates).

## 0. Plumbing (J2534.c)

PassThru wrappers (each returns `0x12345600` if the export wasn't found, and pokes the UI
activity LED via `FUN_0043f280`):

| Wrapper | Call | Line |
|---|---|---|
| FUN_0045b400 | PassThruOpen(NULL, &devId) | 58027 |
| FUN_0045b280 | PassThruClose | 57938 |
| FUN_0045b2c0 | PassThruConnect(dev, proto, flags, baud, &ch) | 57956 |
| FUN_0045b310 | PassThruDisconnect | 57975 |
| FUN_0045b460 | PassThruReadMsgs | 58047 |
| FUN_0045b6a0 | PassThruWriteMsgs | 58170 |
| FUN_0045b550 | PassThruStartMsgFilter | 58099 |
| FUN_0045b600 | PassThruStopMsgFilter | 58135 |
| FUN_0045b3b0 | PassThruIoctl | 58009 |
| FUN_0045b230 | Ioctl(dev, READ_VBATT=3) | 57921 |
| FUN_0045b030 | Ioctl(dev, 0x10002 OEM "detect smart cable") | 57840 |
| FUN_0045b360 | PassThruGetLastError | 57992 |
| FUN_0045b4b0 | PassThruReadVersion | 58064 |

Error check `FUN_0045aee0(ret, file, line, nIgnore, ignoreList)` (57783, return value read from
the disasm at 0x45aee0):
- returns 1 ("ok") for `STATUS_NOERROR`, `ERR_TIMEOUT (9)` and `ERR_BUFFER_EMPTY (0x10)`. So an
  empty read or a write timeout is never an error for GM.
- anything else returns 0. If the code is in the caller's ignore list it returns 0 silently,
  otherwise it logs via `FUN_0045ab20` (57598, the code -> text table) and for `ERR_FAILED (7)`
  appends `PassThruGetLastError` text.
- `ERR_DEVICE_NOT_CONNECTED (8)` in the text logger also calls `FUN_00459490` (56741) which marks
  the comm backend dead (`DAT_006fe140=0`), so all protocols drop to "none" (0xd).

Silenced error lists: FAST_INIT ignores {7, 0x13} (`DAT_004f0718`), KW2000/KW82 FIVE_BAUD_INIT
ignore {7} (`DAT_004e5e48` / local). Ignored here just means "don't spam the log", the caller
still treats it as failed.

The device/channel handle is a shared 2-dword struct at `DAT_0050e390` = {devId, chanId}. Every
K-line/J1850 module gets a pointer to the same struct from `FUN_0045a490`, so there is exactly one
non-CAN channel at a time. CAN keeps its own channel in `DAT_0070174c`.

## 1. Pins: from firmware link attributes to J1962_PINS

### 1.1 Where K and L come from

The firmware opens a link with a QVCI `q_send` 'L'+'OP' message. trap_spy `FUN_00420e70` (17711)
catches it and calls `FUN_00420800(linkType, pins)` (17477):

```
pinA = (pins >> 8) & 0xF   -> attr+0x08  (used as K)
pinB =  pins       & 0xF   -> attr+0x10  (used as L)
```

So pins are 4-bit nibbles, 0..15. The attribute struct (`DAT_00efe080`, returned by
`FUN_00420690`, 17381) is zeroed on every Link_Open, each field is {value, valid}. The rest is
filled from 'L'+2-char link attributes in `FUN_00420010` (17097):

| Offset | Attr | Meaning as used by J2534 code |
|---|---|---|
| +0x00 | (OP) | link type, e.g. 0x1308 |
| +0x08 | (OP) | pinA = K pin |
| +0x10 | (OP) | pinB = L pin |
| +0x18 / +0x20 | 'p1' 0x7031 / 'P1' 0x5031 | P1min / P1max, microseconds |
| +0x28 / +0x30 | 'p2' / 'P2' | P2min / P2max, us |
| +0x38 / +0x40 | 'p3' / 'P3' | P3min / P3max, us |
| +0x48 / +0x50 | 'p4' / 'P4' | P4min / P4max, us |
| +0x58 | 'IK' | |
| +0x60 | 'IS' 0x4953 | tester (source) address |
| +0x68 | 'IA' 0x4941 or 'IR' 0x4952 | ECU/init target address (5-baud address) |
| +0x88 | 'IB' 0x4942 | init baud: 5 (or 200 in KW2000) = 5-baud init, 300 = 300-baud init (KW81) |
| +0xa0 | 'IN' 0x494E | KW2000 header flag, see 2.1 |
| +0xa8 | 'V'+'MP'/'SP' | voltage/mode 1/2 |
| +0xb8 | link id | |
| +0xc0 | 'BR' 0x4252 | baud (ISZHD) |
| +0xc8 | 'MS' 0x4D53 | ISZHD |

Lower case = min, upper case = max: confirmed by KW81 which feeds 'p1'->P1_MIN and 'P1'->P1_MAX.

### 1.2 The mapping

`FUN_00459270(K, L, linkType, &outK, &outL, &connector)` (56637, lives in CANdi\comm.c and is
shared with the live D-PDU path) does nothing clever: `*outK = K; *outL = L`, builds the
"SAEJ1962 pin %s & %s" status text and logs `COMM: Determine Pin K:%d L:%d`. No remap, no
validation.

Then every K-line module does (e.g. KW2000 61126-61131, KW82 20480-20487):

```
SCONFIG { Parameter = 0x8001 (J1962_PINS), Value = K * 0x100 + L }
PassThruIoctl(ch, SET_CONFIG=2, &SCONFIG_LIST{1, &cfg}, NULL)
```

PPSS = K<<8 | L, verbatim.

**K=7, L=8**: you get `0x0708`, i.e. K on 7 and L-line on 8. There is no special case anywhere
for "two K-lines" or for pin 8. GM's backend has no notion of driving two K pins, it's always a
K/L pair and pinB is the L-line. **K-only** (pinB=0): `0x0700`. The live D-PDU path does the same
thing with the same helper: `PDU_PIN_DATA[0]={K, K-line type}, [1]={L, L-line type}` and
`NumPinData = (L==0) ? 1 : 2` (pdu_kw2000.c, 72077-72090), so for a D-PDU->J2534 bridge,
`J1962_PINS = (K<<8)|L` from the two PDU_PIN_DATA entries reproduces GM's own mapping exactly.

Per-module defaults and quirks:

| Module | K | L | Notes |
|---|---|---|---|
| KW2000 | attr pinA | attr pinB | no defaults (0 if absent), pins re-read on every link open (`FUN_00460ef0`, 62119) |
| KW82 | pinA, default 7 | pinB, default 15 | defaults in open `gap 0x428540`; pins cached in `DAT_005545c8/cc` and only computed once per open |
| KW81 | pinA, default 7 | **always 15** | open `gap 0x424310` sets 7/15, pinB is never read |
| KW309 | pinA, default 7 | **always 15** | open `gap 0x422850` sets 7/15, pinB is never read |
| KW71/WABCO | pinA | pinB | `FUN_0042b7e0` |
| J1708 | - | - | hardcoded 0x1A1B |
| CLASS2 | computed | computed | never sent (J1850VPW is not a _PS protocol) |
| CAN | 6 | 14 | log only, no J1962_PINS (plain ISO15765) |

### 1.3 Pins 3 / 12 and the 12V/24V hints

The "hints" come from a firmware signal: `sf $1600.w` with D0 -> `DAT_006fde78` ("12V") and
D1 -> `DAT_006fde7c` ("24V") (CPU\signals.c, 15096-15101). Pick rule (e.g. 20497-20499):

```
h = (hint24V & 0x80000000) ? hint12V : hint24V;
```

KW81/KW82/KW309 (helpers `FUN_00423630` 19129, `FUN_00428190` 21838, `FUN_00422320` 18403):

- `(h & 0xFFFF) != 0` -> OEM config param **0x10001 = 2** (side cable / 24V connector)
- else -> **0x10001 = 1** (J1962 DLC), and if K is 3 or 12 it pops up string 0x36d/0x36e:
  "Unsupported Protocol / E668164: Tech2Win does not support this protocol: KW82/DLC Pin3"
  (or Pin12, KW81, KW309). It only warns, the connect carries on with that PPSS.

KW2000 does none of this: no 0x10001 param, no pin 3/12 check.

ISZHD (`FUN_00421e00`, 18167) decodes pins from the hint bits:
- DLC bits `h & 0x7FF00000`: 0x20000000->3, 0x08000000->7, 0x02000000->9, 0x00800000->11.
  Sets 0x10001=1 and OEM param **0x26** = pin.
- side-cable bits `h & 0x2FD8`: 0x2000->7, 0x400->15, 0x200->16, 0x80->18, 0x40->19.
  Sets 0x10001=2 and **J1962_PINS** = pin (just the pin number, PP=0).
- both present: goes through FUN_00459270, connector flag forced to 1, so DLC wins.

So OEM config **0x10001 is the MX2 connector select** (1 = DLC, 2 = side cable), not a voltage.

## 2. Per module

### 2.1 KW2000 (ISO 14230 K-line, link 0x1308)

This is the one that matters for Saab.

Attributes read at link open (`FUN_00460ef0`, 62119):
- K = attr+8, L = attr+0x10
- `DAT_00a39078` = 'p2' us / 1000 * 0x4189 (P2min in cycles, used to pace replies to the firmware)
- 'IB' == 5 or == 200 -> 5-baud mode (`DAT_00a25d38=2`), wait-timer `DAT_00a39080` = 250 ms;
  else fast-init mode (`=1`), wait-timer = 45 ms
- 'IA'/'IR' -> target address `DAT_00a25d39`, 'IS' -> tester address `DAT_00a25d3a`, 'IN' -> `DAT_00a25d3b`

**Connect sequence** `FUN_0045f920` (61069-61183), done fresh on every init:

1. `PassThruConnect(dev, 0x8003 ISO14230_PS, 0x200 ISO9141_NO_CHECKSUM, 10400, &ch)` (61121)
2. `SET_CONFIG { J1962_PINS 0x8001 = K<<8 | L }` (61126-61131)
3. `StartMsgFilter(ch, PASS_FILTER=1, mask, pattern, NULL, &id)` (61136-61151):
   mask = pattern = `{ProtocolID 0x8003, RxStatus 0, TxFlags 0, DataSize 1, Data[0]=0x00}` -> pass all
4. `SET_CONFIG` list of 9 (61156-61176):

| Param | ID | Value | Real |
|---|---|---|---|
| W1 | 0x0E | 300 | 300 ms |
| W2 | 0x0F | 20 | 20 ms |
| W3 | 0x10 | 20 | 20 ms |
| W4 | 0x11 | 25 | 25 ms (J2534 default is 50) |
| W5 | 0x12 | 300 | 300 ms |
| FIVE_BAUD_MOD | 0x21 | 0 | ISO 9141-2/14230: tester sends ~KB2, ECU sends ~addr |
| P1_MAX | 0x07 | 40 (0x28) | 20 ms |
| P3_MIN | 0x0A | 110 (0x6E) | 55 ms |
| P4_MIN | 0x0C | 10 | 5 ms |

All hardcoded. Nothing comes from the firmware's p1..P4 attributes. TIDLE/TINIL/TWUP/LOOPBACK/
PARITY/DATA_BITS/DATA_RATE are not touched (device defaults: 300/25/50 ms, loopback off).
Checksum: NO_CHECKSUM means the firmware builds the whole frame incl. checksum and gets raw
frames back incl. checksum. The backend never touches the checksum of firmware traffic.

Errors in any step are only logged ("JKW: Connect failed / Set Pins failed / Set filter failed /
Set timing failed"), the sequence continues regardless.

**Fast init** `FUN_0045fcb0` (61236-61330, args checked in disasm 0x45fcb0):
- trigger: the first frame the firmware sends after link up/teardown (state 0, event "new request",
  `FUN_00460d20` 62042-62048). Whatever that frame is (normally the StartCommunication request
  `81 xx F1 81 cs`) becomes the FAST_INIT payload. The emulated CPU is frozen up to 30 s
  (`FUN_00418550(30000)`) around connect + FAST_INIT, released after.
- `Ioctl(ch, FAST_INIT=5, &in, &out)`, in = `{0x8003, RxStatus 0, TxFlags 0, DataSize = firmware
  frame length, Data = firmware frame with checksum}`.
- on success the whole `out` PASSTHRU_MSG goes into the RX ring unchanged (with checksum) and is
  delivered to the firmware as the StartComm reply.
- the reply header and key bytes are parsed (`FUN_004600e0`, 61478): if fmt & 0xC0 the reply's
  target/source bytes fill in the tester/ECU addresses if unknown; if the SID is `C1`, KB1/KB2 are
  the next two bytes, else defaults KB1=0x5B KB2=0x8F. Those decide the header format of GM's own
  TesterPresent (below).
- failure: "JKW: fast init failed (Ioctl)", init-fail event, no retry. If there was no pending
  frame it logs and still calls the ioctl with DataSize 0 (bug).

**5-baud** `FUN_0045fe20` (61333-61376), triggered by the firmware's link enable ('EN', event 9)
when 'IB' says 5-baud:
1. OEM `Ioctl(ch, 0x10001, &SBYTE_ARRAY{1, [0x01]}, NULL)`: "MX2 OEM IOCTL auto baud" (error only logged)
2. `Ioctl(ch, FIVE_BAUD_INIT=4, &SBYTE_ARRAY{1, &targetAddr}, &SBYTE_ARRAY{2, &kb[2]})`
3. KB1 = kb[0], KB2 = kb[1] (stored as one word, low byte KB1).

The firmware's own 5-baud state machine is then fed the bytes it expects to see on the wire
(state 1, `FUN_004608c0`, 61873):
- firmware's TPU finishes clocking out the address -> 5 ms later: UART set to 10400, feed KB1
- +10 ms: feed KB2
- firmware transmits ~KB2 (UART TX byte compare, event 0xF) -> +25 ms: feed `~targetAddr`, link up.
The J2534 device does the real wire init (FIVE_BAUD_MOD 0), GM only replays it to keep the
firmware happy. The 0x55 sync is never fed.

**TX** `FUN_00461080` (62188-62229):
- `Ioctl(ch, CLEAR_RX_BUFFER=8)` before **every** write
- `PassThruWriteMsgs(ch, {0x8003, RxStatus 0, TxFlags 0, DataSize n, firmware bytes}, &num=1, timeout 0)`
- re-arms the 4.5 s idle timer (see TesterPresent)
- emulated-time "response wait" timer armed with `DAT_00a39080` (45 ms fast-init / 250 ms 5-baud)

**RX** `FUN_004603b0` (61656-61754, args from disasm 0x4603b0):
- `PassThruReadMsgs(ch, &msg, &num=1, timeout=1 ms)` on every 25 ms tick of the protocol thread
  (`WaitForMultipleObjects(20 handles, 25 ms)`, thread `gap 0x460540`), recursing until empty.
- RxStatus is **not looked at**. No TX_MSG_TYPE / START_OF_MESSAGE handling. It relies on
  LOOPBACK being off (default) and on the device returning whole frames.
- any frame received re-arms the 4.5 s idle timer.
- drop rule: if the service byte after the header == **0x7E** the frame is dropped (that's the
  answer to GM's own TesterPresent). Header length decoded from fmt: `(fmt&0xC0)==0 ? 1 : 3`,
  +1 if `(fmt&0x3F)==0` (length byte).
- everything else goes into an 8-slot ring and is handed to the firmware one frame at a time,
  each spaced by the 'p2' attribute (P2min) (`FUN_00460df0`, 62081).

**TesterPresent handling** (this is where GM diverges from "dumb pipe"):
- firmware's own `$3E` is **never sent to the ECU** (`FUN_0045f2f0` 60939 routes SID 0x3E to a
  separate event). `FUN_00460160` (61513) builds a fake positive reply after P2min, addresses
  swapped from the request (reply target = request source):
  `[80, reqSrc, reqTgt, 01, 7E, cs]` / `[81, reqSrc, reqTgt, 7E, cs]` / `[00, 01, 7E, 7F]` /
  `[01, 7E, 7F]` depending on the request's header type (fmt always 0x80/0x81, the request's
  A1A0 bits are dropped); with 'IN' set it's always `01 7E 7F`.
- GM sends its own TesterPresent after **4.5 s** of bus silence (waitable timer
  `0xFFFFFFFFFD515AC0` = -45,000,000 x 100 ns, `gap 0x45f400`), re-armed on every TX and RX.
  Frame built by `FUN_0045ff40` (61379) from KB1:
  - KB1=0xD0 and KB2=0x8F -> type 3
  - else base = (KB1 bit0 set and bit1 clear) ? 1 : 2, then +2 unless (bit2 set and bit3 clear)
  - type 4: `[0x80|x, tgt, src, 01, 3E, cs]`, type 3: `[0x81|x, tgt, src, 3E, cs]`,
    type 2: `[x, 01, 3E, cs]`, type 1: `[0x01|x, 3E, cs]`, x = 0xC0 if 'IN' else 0.
  Checksum computed by GM (sum of all bytes).

**Keeping the firmware's P2 alive** (state 3, `FUN_00460b50`, 61929, verified in disasm):
while waiting for the ECU, every 45 ms (250 ms in 5-baud mode) with nothing to deliver, GM
hands the firmware a fake frame `80 F1 FF 05 FF DE AD BE EF AC` (slot at `DAT_00a28480`).
trap_spy recognises it on Rcv0 (`F1 FF FF DE AD BE EF`, 7 bytes) and swallows it (`FUN_0045f660`
returns 0x15, 61041-61048). After 18 of those (`DAT_00a25d3e > 0x12`) it gives up and queues a
5-byte `FF FF FF FF FF` frame to the firmware. That's GM papering over PC/J2534 latency. A real
`7F xx 78` from the ECU is just delivered like any other frame.

**Teardown/re-init**: events 1 (firmware UART break of 20-30 ms, i.e. the firmware's own fast-init
wake-up pattern, `FUN_00451d20` 50160), 2, 10 ('DI'), 11 ('RS'), 12 ('CL') all end in
`FUN_00460370` (61643): CLEAR the RX ring, `PassThruDisconnect`, state 0. The next firmware frame
then does a brand new Connect + FAST_INIT. A StartComm sent while already connected is just
written as a normal message.

### 2.2 KW82 (link 0x1302)

Connect: `FUN_00425ae0` (20361), same code inlined in 20652, 21008, 21557 and the thread 22094.
- `PassThruConnect(dev, 0x10002, 0x200, 9615, &ch)` (values from `.data` 0x50D4D0 / 0x50D4F8)
- `SET_CONFIG {J1962_PINS = K<<8|L}` (20480-20487)
- `SET_CONFIG` list of 18 (20501-20558):

| Param | ID | Value |
|---|---|---|
| W1..W5 | 0x0E..0x12 | 300, 20, 20, 25, 300 |
| FIVE_BAUD_MOD | 0x21 | 1 (tester sends ~KB2, no ~addr from ECU) |
| P1_MAX | 0x07 | 10 |
| P1_MIN | 0x06 | 0 |
| P2_MAX | 0x09 | 50 |
| P2_MIN | 0x08 | 'p2' in ms if < 50, default 8 |
| P3_MAX | 0x0B | 8 |
| P3_MIN | 0x0A | 'p3' in ms if <= 7, default 1 |
| P4_MAX | 0x0D | 10 |
| P4_MIN | 0x0C | 6 |
| TIDLE | 0x13 | 2200 (0x898) |
| TINIL | 0x14 | 25 |
| TWUP | 0x15 | 50 |
| OEM | 0x10001 | 1 (DLC) / 2 (side cable) |

  Note the P values are fed in milliseconds into params whose J2534 unit is 0.5 ms (and most of
  them are the "don't use" ones). Either the MX2 reads them as ms or it's a GM bug. Don't copy.
- PASS_FILTER, ProtocolID 0x10002, 1-byte zero mask/pattern (20565-20579)
- init (20888-20911): OEM 0x10001 auto baud `{1,[1]}`, then FIVE_BAUD_INIT `{1,&addr}` -> `{2,&kb}`,
  ERR_FAILED silenced. KB1/KB2 fed to the firmware 10 ms apart, no inverted address.
- TX `FUN_004260f0` (20595): CLEAR_RX_BUFFER + WriteMsgs(timeout 0), firmware frame verbatim.
- RX `FUN_004269c0` (20918): ReadMsgs(1 msg, timeout 0). Response filter `FUN_00428bd0` (22046)
  matches Data[1] against the request's Data[1]: 0x10-0x15 -> +0x90, 0x20-0x23 -> +0x8A,
  0xB0-0xB2 -> +0x40, 0xBA/0xBB/0xCF -> 0xF0, 0xBC -> 0xF0/0xFA/0xFB, 0xFF -> anything. Mismatch
  = drop. RxStatus ignored.
- DiagnosticEnd (request Data[1]==0xB2): fake reply `03 F0 AA 01 9D` to the firmware (21523-21530),
  and on shutdown `Sleep(5000)` before letting go (21903-21905).

### 2.3 KW81 (link 0x1301)

Connect `FUN_00423750` (19174-19435), attrs `gap 0x4258b0`:
- `PassThruConnect(dev, 0x10001, 0x200, 9600, &ch)` (`.data` 0x50D480 / 0x50D4C4)
- `SET_CONFIG {J1962_PINS = K<<8|15}`
- 18-param list (19338-19401), same IDs as KW82 but values from attributes (us/1000, so ms) with
  these defaults: W1 300, W2 20, W3 20, W4 50, W5 300, FIVE_BAUD_MOD 1, P1_MAX 20, P1_MIN 'p1'
  (else 0), P2_MAX 50 (capped 1500), P2_MIN 25, P3_MAX 5000, P3_MIN 55, P4_MAX 20, P4_MIN 5,
  TIDLE 2200, TINIL 25, TWUP 50, 0x10001 1/2. Those are the ISO 9141 numbers in ms, again into
  0.5 ms params.
- PASS_FILTER ProtocolID 0x10001, 1 zero byte.
- init `FUN_004247e0` (19709): OEM 0x10001 auto baud, then 'IB'==5 -> FIVE_BAUD_INIT
  `{1,&addr}`->`{2,&kb}`, 'IB'==300 -> **OEM ioctl 0x10005** with the same in/out (300-baud init),
  anything else "invalid Baudrate".
- TX `FUN_00423ee0` (19456): CLEAR_RX + WriteMsgs(timeout 0). RX `FUN_00424040` (19506):
  ReadMsgs(1, timeout 0), no filtering. CPU held 50 ms (`DAT_0050d47c`) per request.

### 2.4 KW309 (link 0x130B)

Connect `FUN_00422f30` (18833-18971), attrs `FUN_00423310` (19015):
- `PassThruConnect(dev, 3 ISO9141, 0x200, 4800, &ch)` (`.data` 0x50D430 / 0x50D474)
- one `SET_CONFIG` list of 14 (18908-18937), and here J1962_PINS is **inside** the timing list
  (on a non-_PS protocol, which is not spec):
  `{0x10001:1/2}, {J1962_PINS: K<<8|15}, W1 200, W2 20, W3 20, W4 200, W5 25, FIVE_BAUD_MOD 1,
  P1_MAX 10, P2_MIN 8, P2_MAX 400, P3_MIN 2, P4_MIN 6, P1_MIN 0` (defaults from `.data`
  0x50D438-0x50D46C). Overrides in ms (attr/1000) when present: 'P1'->P1_MAX, 'p1'->P1_MIN,
  'p2'->P2_MIN, 'P2'->P2_MAX, 'p3'->P3_MIN, 'p4'->P4_MIN. 'P3'/'P4' are ignored.
- PASS_FILTER ProtocolID 3, 1 zero byte.
- init `FUN_00423220` (18974): OEM 0x10001 (result ignored), FIVE_BAUD_INIT `{1,&addr}`->`{2,&kb}`.
  Firmware fed KB1, +10 ms KB2, then after its ~KB2 +25 ms `~addr` (even though FIVE_BAUD_MOD 1
  means the device doesn't get one).
- TX `FUN_00422560` (18522) CLEAR_RX + WriteMsgs(0); AddMessage holds the CPU 200 ms
  (`FUN_00418580(200)`, 18583). RX `FUN_00422440` (18448) ReadMsgs(1, 0) drained recursively.

### 2.5 KW71 ("WABCO", link 0x1303)

`FUN_0042a7e0` (22937): `Connect(dev, 0x10005, 0, 9600)`, `SET_CONFIG {J1962_PINS K<<8|L}`, PASS
filter 1 zero byte. No timing config at all. Init `FUN_0042ac90` (23145): OEM 0x10001 then
FIVE_BAUD_INIT `{1,&addr}`->`{2,&kb}`. TX CLEAR_RX + WriteMsgs(0), RX ReadMsgs(1, 0).

### 2.6 ISZHD (heavy duty, link 0x1B00)

`FUN_004212c0` (17862): `Connect(dev, 0x10004, 0, baud)` with baud = 'BR' attr or 62500, then
SET_CONFIG list of 6 (initialised at 0x421B47, connector/pin patched by `FUN_00421e00`):
`{0x10001: 1|2}, {DATA_RATE: baud}, {0x28: 120}, {0x27: 10}, {0x26 or 0x8001: pin}, {0x10006: 1}`.
PASS filter ProtocolID 0x10004, DataSize 4 all zero. Write timeout 500 ms (18392), read timeout 0.
0x26/0x27/0x28/0x10006 are not 04.04 params, MX2 specific.

### 2.7 J1708 (link 0x1200)

`FUN_0042b820` (23534): `Connect(dev, 0x800D, 0x200, 9600)`, SET_CONFIG list of 3 (set at
0x42BF31): `{0x10001: 2}, {J1962_PINS: 0x1A1B}, {DATA_RATE: 9600}`, PASS filter 4 zero bytes.

### 2.8 XDE / GM-UART (link 0x1000)

`FUN_0042c3a0` (23888): `Connect(dev, 0x10003, 0, 8192)`, `SET_CONFIG {DATA_RATE(1): 8192}`
(0x42C1C5), PASS filter 4 zero bytes. Note: OEM 0x10003, not GM_UART_PS.

### 2.9 CLASS2 (J1850VPW, link 0x2000)

`FUN_0045e5d0` (60517): `Connect(dev, 1 J1850VPW, 0, 10400)`, pins computed and thrown away,
PASS filter ProtocolID 1 with 1 zero byte. TX `FUN_0045ede0` (60820): CLEAR_RX + WriteMsgs(0).
RX `FUN_0045e840` (60641): ReadMsgs(1, timeout 1), and **if the device is an MX2 it strips the
last byte** (`DataSize-1`, 60700-60702). CPU held 150 ms per request (60738).

### 2.10 CAN (link 0x1A00 CANdi / 0x4001 HS)

This module emulates the CANdi ICD command set on top of J2534 (`FUN_0045bf80` dispatch),
answering ICD commands locally after +0x8312 cycles (2 ms).
- only HS-CAN (CANdi bus 2) is real: `FUN_0045d5e0` (59635) `Connect(dev, 6 ISO15765, 0, 500000)`,
  then `SET_CONFIG {CAN_MIXED_FORMAT 0x8000 = 2 (ALL_FRAMES)}`. SW-CAN and MS-CAN just report the
  link closed (TX gets 0xD0 back).
- filters (`FUN_0045e230` 60240 / `FUN_0045d820` 59739): `CLEAR_MSG_FILTERS (0x0A)` then for each
  of the 64 CANdi secondary filter slots: CANdi flag bit7 -> `FLOW_CONTROL_FILTER (3)` with
  ProtocolID 6, mask FF FF FF FF; else `PASS_FILTER` with ProtocolID **5 (CAN)** (mixed format).
  Flag 0x10 -> TxFlags ISO15765_FRAME_PAD 0x40, 0x20 -> CAN_29BIT_ID 0x100, 0x40 ->
  ISO15765_ADDR_TYPE 0x80 (DataSize 5 instead of 4). After each: CLEAR_RX_BUFFER. CPU held 2 s
  per filter.
- TX `FUN_0045d2b0` (59504): WriteMsgs ProtocolID 6, timeout 0, TxFlags from the CANdi header
  (0x40->0x80, 0x20->0x100, pad bit->0x40).
- RX `FUN_0045d9d0` (59812) ReadMsgs(1, 0) every 10 ms; `FUN_0045b990` (58297):
  `RxStatus & TX_INDICATION(8)` -> synthesize the CANdi TX-done (0x77) to the firmware; otherwise
  ProtocolID 6 -> ISO-TP flag, RxStatus == 0x80 -> ext addr, RxStatus == 0x100 -> 29-bit
  (compared with ==, not as bit masks).

## 3. Error handling worth knowing

- No retries on any PassThru call. Every connect/config step just logs and carries on.
- ERR_TIMEOUT and ERR_BUFFER_EMPTY are success everywhere.
- ERR_FAILED gets the PassThruGetLastError string appended to the log.
- ERR_DEVICE_NOT_CONNECTED kills the backend (`FUN_00459490`). There is no reopen: the device
  is opened once at emulator start (`FUN_004595f0` -> `FUN_0045a490(0)`, 56888) and `FUN_0045a490`
  returns early afterwards because devId stays set.
- Link re-init = Disconnect + new Connect (KW2000 on every firmware wake-up break).
- Protocol threads are stopped with an event and 5 s (KW2000, KW82) or 11 s (KW81, KW309) wait,
  then TerminateThread.
- While blocking J2534 calls run, the emulated CPU is frozen (`FUN_00418550/80`, released by
  `FUN_00418510`, wall-clock timeout in ms): KW2000 open 10 s, connect+FAST_INIT 30 s, KW309 200 ms,
  KW81 50 ms, KW82 20/40 ms, CLASS2 150 ms, CAN filter 2 s.

## 4. Device enumeration / selection

`FUN_0045a0c0` (57272-57414) and `FUN_0045a490` (57417-57595):
- `RegOpenKeyExW(HKLM, "Software")` -> `"PassThruSupport.04.04"`, KEY_READ (0x20019). It's a
  32-bit exe so this lands in `WOW6432Node`.
- `RegEnumKeyExW` up to 10 vendor subkeys. 0 -> "J2534: No Device selected". 1 -> used directly.
  More than 1 -> "Select manufacturer / Select the manufacturer you want to use:" dialog
  (string 0x8e/0x8f, `FUN_00437d00` -> `FUN_0043f7a0`).
- values read: `Name` (REG_SZ into a 16-byte buffer), `FunctionLibrary`, `ConfigApplication`
  (only logged). No protocol capability values are checked.
- `Name` compared to `L"MX2"`. Not MX2 -> popup 0x395/0x396 "E675228: Tech2Win is operating with an
  unvalidated vehicle communication device..." and the MX2-only bits are skipped.
- `LoadLibraryW(FunctionLibrary)`, GetProcAddress of all 14 PassThru exports.
- `PassThruOpen(NULL, &devId)`; ERR_FAILED/ERR_DEVICE_NOT_CONNECTED -> "J2534: No Device connected".
- `PassThruReadVersion`; for MX2: firmware string `MX2_%d_%d_%d` must be >= `MX2_08_25_2014` and the
  DLL version >= 03.99, else popups M675225 / M675226.
- `Ioctl(dev, READ_VBATT)` -> `DAT_006fde84` (mV, shown in the log).
- MX2 only: `Ioctl(dev, 0x10002, NULL, &SBYTE_ARRAY{1,&b})`, b must be 2 ("Smart Cable detected"),
  else popup M675229 "The MX2 needs to have the smart cable attached."
- Close (`FUN_0045ad10`, 57716): PassThruClose + FreeLibrary.

## 5. MX2 OEM extensions used

| Kind | ID | Use |
|---|---|---|
| ProtocolID | 0x10001 | KW81 |
| ProtocolID | 0x10002 | KW82 |
| ProtocolID | 0x10003 | XDE / GM-UART 8192 |
| ProtocolID | 0x10004 | ISZHD heavy duty |
| ProtocolID | 0x10005 | KW71 (WABCO) |
| Ioctl | 0x10001 | "auto baud" before every FIVE_BAUD_INIT, in = SBYTE_ARRAY{1,[0x01]}, out NULL |
| Ioctl | 0x10002 | smart cable detect (on devId), out SBYTE_ARRAY{1}, 2 = present |
| Ioctl | 0x10005 | 300-baud init (KW81), same in/out as FIVE_BAUD_INIT |
| Config | 0x10001 | connector: 1 = J1962 DLC, 2 = side cable |
| Config | 0x10006 | ISZHD, = 1 |
| Config | 0x26, 0x27, 0x28 | ISZHD (DLC pin, 10, 120) |

## 6. What to copy into the D-PDU -> J2534 bridge

Worth copying for KWP2000 K-line:
- `Connect(0x8003 ISO14230_PS, 0x200 NO_CHECKSUM, 10400)`, then `J1962_PINS = (K<<8)|L` right
  after Connect, a pass-all filter, then the 9-param timing list from 2.1. Tech2Win already sends
  complete frames with checksum and expects them back with checksum.
- FAST_INIT with the full StartComm frame (incl. checksum) as input, and hand the output message
  back as the StartComm response.
- `CLEAR_RX_BUFFER` before each write, `ReadMsgs(1 msg, 1 ms)` drained in a loop.
- Treat ERR_TIMEOUT / ERR_BUFFER_EMPTY as fine.
- Disconnect + fresh Connect on every new wake-up/init instead of reusing the channel.
- For 5-baud: FIVE_BAUD_MOD 0 for KW2000, input `{1, addr}`, output `{2, KB1 KB2}`.

Don't copy:
- the OEM protocol IDs / ioctls / 0x10001 config (MX2 only, a generic J2534 box will reject them).
- P-timings in ms for KW81/KW82/KW309 (wrong unit per spec).
- J1962_PINS on plain ISO9141 (KW309 does it, spec says _PS only).
- the local $3E answering and the DEADBEEF keep-alives: that's Tech2Win compensating for its own
  firmware timers, the live D-PDU path already does its own version of that.
