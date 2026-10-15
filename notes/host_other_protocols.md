# Tech2Win D-PDU host side: KW82, KW81, KW08, SDL_ISO (+ KW886, XDE, Class2, CAN)

What emulator.exe (Tech2Win build 2019-12-22) actually calls on the D-PDU API for every protocol
except KWP2000 (pdu_kw2000.c) and the generic pdu_api.c, which are covered elsewhere.

Source: `RE/tech2win/out/decomp_all.c` (line numbers below are lines in that file) plus capstone
disassembly of the bits Ghidra never turned into functions (tick/open/close handlers, marked "asm").
Annotated copy with object names and the disassembler used: scratch only, not kept.

Times in Tech2Win are emulated CPU cycles at 16 777 cycles/ms (0x4189). Every value below is
already converted to ms. "Emulated time" is slaved to wall clock by the PI pacer, but stops while
the CPU is frozen (see KW08).

## 0. Common plumbing (all K-line handlers)

### Name -> id tables (PDUGetObjectId)

At startup `FUN_00463750` (64025) resolves EVERY name below with PDUGetObjectId. Return 0 = ok,
0x50 (PDU_ERR_INVALID_PARAMETERS) = id set to PDU_ID_UNDEF + "PDU: %hs not supported but
required." warning; any other error = "Mapping of PDU API objects failed" (fatal). Tables dumped
straight out of the .data section (`{u32 *idvar; char name[]}` records):

| obj type | names (id variable) |
|---|---|
| COMPARAM 0x8024 | CP_Loopback(51e0dc) CP_P2Max(51e0b0) CP_P2Min(51e0ac) CP_P3Min(51e0b4) CP_P3Phys(51e0c4) CP_RepeatReqCountApp(51e04c) CP_StartMsgIndEnable(51e058) CP_SwCan_HighVoltage(51e05c) CP_TesterPresentHandling(51e068) CP_TransmitIndEnable(51e06c) CP_5BaudAddressPhys(51e07c) CP_5BaudMode(51e088) CP_As(51dffc) CP_Bs(51e008) CP_CanFillerByte(51e00c) CP_CanFillerByteHandling(51e010) CP_CanPhysReqExtAddr(51e014) CP_CanPhysReqFormat(51e018) CP_CanPhysReqId(51e01c) CP_CanRespUSDTExtAddr(51e020) CP_CanRespUSDTFormat(51e024) CP_CanRespUSDTId(51e028) CP_CanRespUUDTExtAddr(51e02c) CP_CanRespUUDTFormat(51e030) CP_CanRespUUDTId(51e034) CP_Cr(51e038) CP_Cs(51e03c) CP_HeaderFormatKW(51e040) CP_InitializationSettings(51e044) CP_P4Max(51e0c0) CP_P4Min(51e0bc) CP_RequestAddrMode(51e050) CP_TIdle(51e070) CP_TInil(51e074) CP_TWup(51e078) CP_W1Max(51e090) CP_W1Min(51e08c) CP_W2Max(51e098) CP_W2Min(51e094) CP_W3Max(51e0a0) CP_W3Min(51e09c) CP_W4Max(51e0a8) CP_W4Min(51e0a4) CP_Baudrate(51e000) CP_BitSamplePoint(51e004) CP_K_L_LineInit(51e048) CP_SamplesPerBit(51e054) CP_SyncJumpWidth(51e060) CP_TerminationType(51e064) CP_5BaudInitBaudrate(51e084) CP_5BaudCommBaudrateOverride(51e080) CP_BecomeMaster(51e0c8) CP_BecomeSlave(51e0cc) CP_ByteCountOffset(51e0e4) CP_MessageScheduler(51e0d8) CP_P3Max(51e0b8) CP_P3Min_J2740(51e0e0) CP_SetPollResponse(51e0d0) CP_SetRelinquishMastership(51e0d4) |
| BUSTYPE 0x8022 | ISO_11898_2_DWCAN ISO_11898_3_DWFTCAN ISO_9141_2_UART ISO_14230_1_UART SAE_J1939_11_DWCAN SAE_J1850_VPW SAE_J2411_SWCAN SAE_J2740_UART KW_UART |
| PROTOCOL 0x8021 | ISO_14230_3_on_ISO_14230_2 ISO_15765_3_on_ISO_15765_2 SAE_J2190_on_ISO_9141_2 SAE_J2190_on_SAE_J1850_VPW ISO_15031_5_on_ISO_15765_4 SAE_J1939_73_on_SAE_J1939_21 ISO_11898_RAW XDE_5024_on_XDE_5024_UART KW82_on_KW_UART KW81_on_KW_UART KW309_on_KW_UART |
| PINTYPE 0x8025 | HI LOW K L PLUS SINGLE |
| IO_CTRL 0x8023 | PDU_IOCTL_READ_VBATT PDU_IOCTL_START_MSG_FILTER PDU_IOCTL_CLEAR_MSG_FILTER |

So the DLL must answer all of these (at least with 0x50), and every one of the CP_ names above is
something Tech2Win may SetComParam/GetComParam on.

### Protocol dispatch

The firmware opens a link with a QVCI `L0 OP` message; trap_spy `FUN_00420800` (17477) maps the
link type to a mode and the pins word to pinA/pinB, `EN` then switches mode
(`FUN_00459d40` 57136: close old handler, open new one) and raises event 9.

| link type | name | mode | PDU handler file |
|---|---|---|---|
| 0x1301 | KW81 | 3 | pdu_kw81.c |
| 0x1302 | KW82 | 2 | pdu_kw82.c |
| 0x1304 / 0x1305 | KW08 / KW94 (both run as 0x1304) | 6 | pdu_kw08.c |
| 0x1308 | KW2000 | 1 | pdu_kw2000.c (not here) |
| 0x1309 | KW886/ISO8 | 4 | pdu_kw886.c |
| 0x1801 | SDL_ISO | 9 | pdu_sdl_iso.c |
| 0x1000 | XDE GM UART | 8 | pdu_xde.c |
| 0x2000 | CLASS2 | 7 | pdu_class2.c |
| 0x1a00 / 0x4000-0x4004 | CANdi / HS/MS/SW CAN | 0 | CANdi ICD -> pdu_can.c |
| 0x1303 KW71, 0x1200 J1708, 0x1b00 HeavyDuty, 0x1306/7/a/b ... | | 0xa/0xc/0xb/none | no PDU handler (J2534-only or nothing) |

Per-mode function tables are filled in `FUN_004595f0` (56851), 14 entries per backend
(PDU = row 0). Live PDU handlers:

| mode | send (UART frame done) | get (reply to UART) | tick/poll | open | close | event |
|---|---|---|---|---|---|---|
| 2 KW82 | FUN_0047f580 (78550) | FUN_004802e0 (79078) | 0x480330 asm | FUN_00480830 (79163) | 0x4835e0 asm | FUN_00482d00 (80284) |
| 3 KW81 | FUN_0047a590 (76402) | FUN_0047b270 (76892) | 0x47b2c0 asm | 0x47b700 asm | 0x47e1e0 asm | FUN_0047d9a0 (77693) |
| 6 KW08 | FUN_0046c610 (69712) | FUN_0046d5b0 (70260) | 0x46d600 asm | 0x46d9d0 asm | 0x470280 asm | FUN_0046fbe0 (71086) |
| 9 SDL | FUN_00489990 (83416) | FUN_0048ab20 (84043) | 0x48aba0 asm | 0x48abf0 asm | 0x48c370 asm | FUN_0048c100 (84645) |
| 4 KW886 | FUN_00484b10 (81219) | FUN_00485820 (81736) | 0x485870 | 0x485d10 | 0x488e20 | FUN_00488600 (82783) |
| 7 CLASS2 | FUN_00468fa0 (67904) | FUN_00469f10 (68517) | 0x469f60 | 0x46a030 | 0x46b550 | FUN_0046b380 (69079) |
| 8 XDE | FUN_0048c900 (85017) | FUN_0048da40 (85639) | 0x48dac0 | 0x48db10 | 0x48f330 | FUN_0048f020 (86243) |

Events into the handler (`FUN_00459cc0`, 57093): 9 = link EN, 10 = DI, 11 = RS, 12 = CL,
0xd = firmware armed TPU fn 0xC (sync-byte baud measurement, `FUN_0041e380` 16120),
0xe = Tech2Win's fed baud got delivered to the firmware, 0xf = firmware wrote the inverted key
byte (~KB2) to the UART (echoed locally, never sent on), 0x10 = firmware read the last byte of a
reply, 0x11 = first TX byte, 0x12 = firmware q_send on Rcv0/Rcv1 (only modes 1,2,4: lets the
handler swallow its own fake frames, return 0x15).

Every protocol file has the SAME skeleton. Most named functions in each file are dead stand-alone
copies (zero callers) of code that is inlined into the worker thread; the live path is always
open -> worker thread -> tick/send/get/event. The dead copies are still handy because each one is
one "step", e.g. for KW82: FUN_0047eca0 InitLink, FUN_0047f620 CllCreate, FUN_0047f9a0 CllDestroy,
FUN_0047fc30 filters, FUN_0047fda0 timing, FUN_0047ffe0 StopComm, FUN_00483320 SendMessage,
FUN_00483620 StartReceive, FUN_004837a0/FUN_00483870 cancel, FUN_00483e80 WaitForEvent.

### Things every K-line handler does the same way

* `PDURegisterEventCallback(hMod, hCLL, cb)` per CLL. cb (e.g. FUN_004801f0 79013, stdcall 5 args)
  only does `if (eventType == 0x801) SetEvent("Callback Event")`. 0x802 is ignored.
* Worker drains with `PDUGetEventItem(hMod, hCLL, &item)` until it returns non-zero, then
  `PDUDestroyItem` each item.
  * PDU_IT_RESULT 0x1300: copies `NumDataBytes` (+0x24) bytes from `pDataBytes` (+0x28) verbatim
    into an RX ring (5 slots, SDL 10). RxFlag, AcceptanceId, timestamps, hCop, pCoPTag are all
    ignored, so a RESULT from ANY CoP (STARTCOMM, SENDRECV, receive-only) lands in the same queue.
    Full ring = "PKW: rx queue overflow", frame dropped.
  * PDU_IT_STATUS 0x1301: only used by the WaitForEvent helper.
  * PDU_IT_ERROR 0x1302: kills the link, see per protocol. Treat ERROR as fatal-ish.
  * PDU_IT_INFO 0x1303: ignored.
* WaitForEvent helper (KW82 FUN_00483e80 80802, KW81 FUN_0047ea20 78188, KW08 FUN_004707a0 71460,
  SDL FUN_0048c680 84925): `WaitForSingleObject(callbackEvent, 10000)`, drain everything, return
  when a PDU_IT_STATUS with `*(UNUM32*)pData == wanted` shows up (any hCop!). Timeout = silently
  give up after 10 s. Used after every call:
  * PDUConnect -> waits 0x8051 CLLST_ONLINE
  * PDUDisconnect -> waits 0x8050 CLLST_OFFLINE
  * UPDATEPARAM / STARTCOMM / STOPCOMM / send-only SENDRECV -> waits 0x8012 COPST_FINISHED
  * infinite SENDRECV / receive-only -> waits 0x8011 COPST_EXECUTING
  * PDUCancelComPrimitive -> waits 0x8013 COPST_CANCELLED
  
  If the DLL doesn't emit these STATUS items (plus the 0x801 callback), every step stalls 10 s.
* PDUGetStatus usage: `GetStatus(hMod, hCLL, hCoP_init, ...)`: if it does NOT return 0x60 and the
  status is not FINISHED/CANCELLED, Tech2Win cancels it and waits CANCELLED. hCoP_init is 0 before
  the first init, so GetStatus on CoP handle 0 must return 0x60 (PDU_ERR_INVALID_HANDLE), and never
  hand out CoP handle 0. `GetStatus(hCoP_rx)`: if it returns 0 with 0x8011 -> cancel.
  `GetStatus(hMod, hCLL, 0xFFFFFFFF, ...)` = CLL status; 0x8052 COMM_STARTED -> it sends STOPCOMM
  first.
* CLL create (identical for all K-line files except the pin list):
  `PDUCreateComLogicalLink(hMod, &rsc, 0xFFFFFFFE /*PDU_ID_UNDEF resource*/, pCllTag, &hCLL,
  &cllCreateFlag)` with `cllCreateFlag = {NumFlagBytes 4, pFlagData -> 80 00 00 00}` = bit 31
  RawMode. So every link is RAW: TX payload = the complete frame incl. length byte and checksum,
  RESULT data must be the complete frame exactly as received (no echo).
  Order: Create -> RegisterEventCallback -> Connect (-> wait ONLINE). Teardown: cancel CoPs,
  STOPCOMM if COMM_STARTED, Disconnect (wait OFFLINE), DestroyComLogicalLink,
  RegisterEventCallback(NULL). A mode switch always destroys and recreates the CLL.
* Pins come from the `OP` link attribute: `FUN_00420800`: pinA = `(pins>>8)&0xF` (attrs+0x08),
  pinB = `pins&0xF` (attrs+0x10); `FUN_00459270` (56637, "COMM: Determine Pin K:%d L:%d") just
  copies them into the PDU_PIN_DATA. They are passed straight through as DLCPinNumber (logged as
  "SAEJ1962 pin %d"), so 7 or 8 for SAAB K-line.
* Filters (before every init): `PDUIoCtl(hMod, hCLL, PDU_IOCTL_CLEAR_MSG_FILTER, NULL, NULL)` then
  `PDUIoCtl(hMod, hCLL, PDU_IOCTL_START_MSG_FILTER, &{ItemType 0x1003, pData -> {NumFilterEntries
  1, -> {FilterType 1 PASS, FilterNumber 1, FilterCompareSize 1, Mask[0]=0, Pattern[0]=0}}}, NULL)`
  = pass everything.
* Every SetComParam is `PDU_PARAM_ITEM {0x1200, id, 0x105 UNUM32, class, -> value}` (only XDE uses
  0x107 BYTEFIELD). Timing values are passed in microseconds but always whole ms * 1000.
* TempParamUpdate is 0 everywhere. No CoP ever has NumSendCycles > 1 or -1 (no periodic CoPs) in
  KW81/KW82/KW08/SDL. Tester present is never delegated to the API (CP_TesterPresentHandling = 0
  where it is set at all).
* Frame cutting on the firmware side (UART model `FUN_004510f0` 49652, decides when a TX frame is
  complete and goes to `send`): KW82 total = b0+2, KW81/KW886 total = b0+1, KW08 = lengths learned
  from the Xmt0 q_send, SDL total = b1, XDE total = b1-0x52.
* RX side (`FUN_00451bf0` 50115): a queued frame is only pulled when the UART RX buffer is empty;
  after a 1-byte reply on modes 1-6 the UART arms "inverted key byte" detection (that's how the
  keyword exchange works, see below). The firmware parses the byte stream itself, so frame
  chunking mostly doesn't matter, BUT KW82 rewrites frames by offset and the init steps take
  bytes [0]/[1] of the first RESULT, so deliver one frame per RESULT.

## 1. KW82 (pdu_kw82.c), link 0x1302, mode 2

Bus/protocol: `KW_UART` / `KW82_on_KW_UART`. 5-baud init, ECU baud read back from the API.
SAAB: SRS (airbag) on OG9-3 and early OG9-5 per the JohnJocke dpdu-passthru README (which runs it
as J2534 ISO9141 @ 8192, no checksum, FIVE_BAUD_MOD 1). The card has a "Programming Error KW82"
string, so it's real SAAB content.

Frame format (from the checksum code, live in thread 80053, dead copy FUN_00483dd0 80764):
`[N][b1 .. b(N-1)][ckH][ckL]`, total N+2 bytes, ck = 16-bit sum of bytes 0..N-1, big endian.
Bad checksum on TX = "PKW: SendMessage() - message has an incorrect checksum.", frame not sent.

### PDU_RSC_DATA (thread FUN_00481470 79543, at 79707-79716)
`{BusTypeId = id(KW_UART), ProtocolId = id(KW82_on_KW_UART), NumPinData = 1,
pDLCPinData -> [{DLCPinNumber = pinA (default 7), DLCPinTypeId = id(K)}]}`, CllTag -> 0,
CllCreateFlag RawMode.

### Open (FUN_00480830 79163)
Defaults (ms): P2min 8, P2max 50, P3min 1, P3max 8, P4min 6, P4max 10, W4min (DAT_00d97a30) 25,
baud-feed window 25..50, KB1 delay 50, KB2 delay 5, keep-alive cutoff 1500. pinA default 7, IB
(5-baud "baud") default 5, baud fallback 9600. Overrides from link attributes (all in µs in the
firmware): p2/P2 (+0x28/+0x30), p3/P3 (+0x38/+0x40), p4/P4 (+0x48/+0x50), IA/IR = 5-baud address
(+0x68), IS (+0x60, stored, unused), IB (+0x88), pinA (+0x08). Then
`CP_P2Max value = max(P2max, 300 ms)` and keep-alive start = `P2max - (P2max-P2min)/4` (39.5 ms with
defaults). Builds the fake frame (see 1.4), creates 5 auto-reset events
("Callback Event", "New Transmit Message", "Shutdown Thread", "Start Rx After 5 Baud Init",
"Stop Comm When Link Is Disabled") and starts the worker thread, which immediately does
Create/RegisterEventCallback/Connect.

### Init (event 9 EN -> FUN_00482d00 80331-80494, then thread case 1 with state 1, 79826-80031)
EN with state 0: re-read link attributes, state = 1, queue a 1-byte "message" `[IA]` through
`send`, which wakes the thread. Thread:

1. Cancel the old init CoP / rx CoP if needed, STOPCOMM if CLL is COMM_STARTED.
2. SetComParam (hMod, hCLL), in this order:

| ComParam | class | value |
|---|---|---|
| CP_RepeatReqCountApp | 4 ERRHDL | 0 |
| CP_StartMsgIndEnable | 3 COM | 0 |
| (IoCtl CLEAR_MSG_FILTER, START_MSG_FILTER pass-all) | | |
| CP_P2Min | 1 TIMING | P2min, default 8000 µs |
| CP_P2Max | 1 | max(P2max,300 ms), default 300000 |
| CP_P3Min | 1 | default 1000 |
| CP_P3Max | 1 | default 8000 |
| CP_P4Min | 1 | default 6000 |
| CP_P4Max | 1 | default 10000 |
| CP_5BaudInitBaudrate | 1 | 10000 / IB (IB default 5 -> 2000; looks like bit time in 100 µs units, i.e. 200 ms = 5 baud) |
| CP_5BaudAddressPhys | 3 | IA (first byte of the `[IA]` message) |
| CP_TIdle | 3 | 2100000 (2.1 s) |
| CP_W4Min | 2 INIT | default 25000 |

   CP_5BaudMode is NOT set for KW82 (MDI default). CP_Baudrate is NOT set either.
3. `PDUStartComPrimitive(hMod, hCLL, PDU_COPT_UPDATEPARAM, 0, NULL, NULL /*no ctrl*/, tag->2,
   &hCoP_init)` -> wait FINISHED.
4. `PDUStartComPrimitive(hMod, hCLL, PDU_COPT_STARTCOMM, 0, NULL, &ctrl, tag->2, &tmp)` with
   `ctrl = {Time 0, NumSendCycles 1, NumReceiveCycles 1, TempParamUpdate 0, TxFlag {0, NULL},
   NumPossibleExpectedResponses 1, -> {ResponseType 1, AcceptanceId 1, NumMaskPatternBytes 1,
   mask {00}, pattern {00}, 0, NULL}}` -> wait FINISHED. (ResponseType 1 = "negative" on STARTCOMM
   is what the code does; mask 0 matches anything anyway.)
5. `PDUGetComParam(hMod, hCLL, CP_Baudrate, &item)`: if ok and item->ComParamId == CP_Baudrate and
   DataType == 0x105, baud = `*(UNUM32*)item->pComParamData` (then DestroyItem), else 9600. This is
   the baud the MDI detected from the sync byte. It gets fed to the firmware, so return the REAL
   bus baud (8192 for SAAB SRS), not a default.

What the firmware gets back (tick 0x480330 asm, state 1, waits until the RX ring has a frame):

| step | condition | action |
|---|---|---|
| 0 | t0+25 ms < now < t0+50 ms (t0 = last 0xd/0xe/0xf/0x10/send/get event) | `FUN_0041f630(baud)`: fake the TPU sync-byte measurement |
| 1 | firmware read it (event 0xe), now >= t0+50 ms | deliver 1 byte = RESULT[0] (KB1) |
| 2 | byte consumed (0x10), now >= t0+5 ms | deliver 1 byte = RESULT[1] (KB2) |
| 3 | firmware wrote ~KB2 (event 0xf, echoed locally, never sent) | drop that RESULT slot, state = 2, arm keep-alive, SetEvent("Start Rx After 5 Baud Init") |

So the STARTCOMM RESULT must carry KB1 KB2 in bytes 0 and 1. Anything after byte 1 of that frame is
thrown away. No ~address is synthesized for KW82 (contrast KW08), i.e. Tech2Win assumes "tool
sends ~KB2, ECU does not answer with ~address" (FIVE_BAUD_MOD / CP_5BaudMode 1 behaviour); since
it leaves CP_5BaudMode at the MDI default, the DLL has to pick that itself.

Timing trap: step 0 has a 25-50 ms window after the firmware arms its sync measurement (event
0xd). There is no CPU freeze for KW81/KW82, so the PC-side 5-baud init (2 s address + W1..W4) has
to finish before the firmware's own (emulated, wall-clock-paced) address phase does. If the
RESULT is late the window is missed and init only recovers on the firmware's next attempt. That's
why trap_spy forces link attribute `IF` (ISO_5BAUD_ATTEMPTS) to >= 3 (`FUN_00420010` 17097,
"TRAP: ISO_5BAUD_ATTEMPTS got increased to %d"). Make STARTCOMM return as fast as possible.

Unsolicited frames: the receive CoP is only started after step 3 (~100+ ms after the RESULT). If
the ECU starts talking right after init, the DLL has to buffer those frames and hand them to the
receive-only CoP when it starts, or they're gone.

### Receive-only CoP (thread case 3, 80119-80157)
`PDUStartComPrimitive(hMod, hCLL, PDU_COPT_SENDRECV, 0, NULL, &ctrl, tag->1, &hCoP_rx)`,
`ctrl = {0, NumSendCycles 0, NumReceiveCycles -1, 0, TxFlag {4, -> 00 00 00 00}, 1,
-> {ResponseType 0, AcceptanceId 1, 1, {00}, {00}, 0, NULL}}` -> wait EXECUTING. Infinite listener.

### Send (firmware frame complete -> FUN_0047f580 78550 -> thread case 1 with state 2, 80037-80116)
`send` only works when the link is enabled (e8) and the previous send was taken (7c); it stamps
t0, copies the frame, marks the read slot 0xFFFF ("waiting"), and with state 2 arms the
keep-alive. Thread: verify 16-bit sum; if the firmware sends exactly `02 B2 00 B4` it sets a flag
that stops the fake keep-alive at the next RESULT (looks like "end session"); cancel hCoP_rx if
EXECUTING (wait CANCELLED); then
`PDUStartComPrimitive(hMod, hCLL, PDU_COPT_SENDRECV, len, rawFrame, &ctrl, tag->1, &hCoP_rx)` with
`ctrl = {Time 0, NumSendCycles 1, NumReceiveCycles -1, TempParamUpdate 0, TxFlag {4, -> 00 00 00
00}, 1, -> {ResponseType 0, AcceptanceId 1, 1, {00}, {00}, 0, NULL}}` -> wait EXECUTING. Payload =
the whole frame `[N]..[ckH][ckL]` (RAW). So: every request is one infinite-receive SENDRECV, and
the next request cancels the previous one.

### RESULT -> firmware (tick state 2 + thread case 0 79743-79800)
* Each RESULT frame goes into the ring. If `byte1 in {F0, FA, FB} && byte2 == F0`, bytes 4 and 5
  are OVERWRITTEN with `ceil(P4min_ms/10)` and `ceil(P2max_ms/10)` and the 16-bit checksum is
  recomputed. (ECU-announced timing replaced by Tech2Win's own.) Needs frame-aligned RESULTs.
* A slot is released to the UART only when the previous one was fully read and
  `now >= t0 + P2min` (t0 = last send / last get).
* Keep-alive fake: if a reply is expected (armed after init and after each send) and nothing is in
  the ring for `P2max - (P2max-P2min)/4` (39.5 ms default), it pushes the prebuilt frame
  `0A F0 F0 10 t4 t2 DE AD BE EF ckH ckL` (t4 = ceil(P4min/10), t2 = ceil(P2max/10), defaults
  `0A F0 F0 10 01 05 DE AD BE EF 05 38`) into the UART, again every ~39.5 ms, until a RESULT
  arrives or 1500 ms since the first fake. When the firmware later posts that frame to Rcv0,
  event 0x12 compares it (`FUN_00459130`) and returns 0x15 so trap_spy drops it. Internal to
  Tech2Win, but it means slow RESULTs are papered over, not fatal.

### ERROR item (any)
`e8 = 0` (send() now rejects every frame), 7c = 1, and state 1 -> 2. The link stays dead until
the firmware does DI/CL (events 10/12 with state 2 -> state 0, SetEvent("Stop Comm ..."))
followed by EN. So don't emit ERROR for anything recoverable (and never a P2 timeout on a
NumReceiveCycles -1 CoP).

### Stop (event 10/11/12 -> thread case 4, 80158-80207) and close (0x4835e0)
Cancel init CoP if running, cancel rx CoP if EXECUTING, then if CLL is COMM_STARTED:
`PDUStartComPrimitive(hMod, hCLL, PDU_COPT_STOPCOMM, 0, NULL, &{Time 100, all other fields 0},
NULL, &h)` -> wait FINISHED. No payload, so `02 B2 00 B4` is NOT sent by STOPCOMM (the firmware
sends it itself as a normal frame if it wants to). Close = SetEvent("Shutdown Thread"), thread
does stop + Disconnect + Destroy (80212-80281).

## 2. KW81 (pdu_kw81.c), link 0x1301, mode 3

Near copy-paste of KW82. Bus/protocol `KW_UART` / `KW81_on_KW_UART`, 1 pin `{pinA (default 7), K}`
(thread FUN_0047c1c0 76989, create at 77148-77157). No known SAAB user.

Differences vs KW82:
* Frame: `[N][...N-1 bytes][ck]`, total N+1, ck = two's complement (all bytes sum to 0 mod 256)
  (77472-77490, FUN_0047e9d0 78158).
* Init SetComParam adds `CP_5BaudMode = 1` (class 2) first, then the same list as KW82:
  RepeatReqCountApp 0, StartMsgIndEnable 0, filters, P2Min/P2Max/P3Min/P3Max/P4Min/P4Max,
  5BaudInitBaudrate 10000/IB, 5BaudAddressPhys IA, TIdle 2100000, W4Min 25000, UPDATEPARAM,
  STARTCOMM (same ctrl), GetComParam(CP_Baudrate) fallback 9600 (77287-77439).
* Defaults (open 0x47b700 asm): P2min 5, P2max 15 (CP_P2Max still forced to 300 ms), P3min 1,
  P3max 4, P4min 6, P4max 10 ms, W4min 25 ms; keep-alive start 12.5 ms.
* Tick (0x47b2c0 asm): same 4 steps (baud window 25-50 ms, KB1 +50 ms, KB2 +5 ms, wait ~KB2),
  then state 2, keep-alive armed, "Start Rx After 5 Baud Init" -> receive-only CoP (77529-77568,
  same ctrl as KW82).
* No F0 rewrite on RX.
* Keep-alive fake is `02 FB 03` (FUN_0047b230 76877 / built in open), and it is NOT swallowed
  (mode 3 has no event 0x12 handling), so the firmware app sees it. Sending a frame that starts
  `02 06` does not arm the keep-alive (FUN_0047a590 76420).
* ERROR: same as KW82 (send disabled until DI/EN).

## 3. KW08 (pdu_kw08.c), link 0x1304 (and 0x1305 KW94), mode 6

ISO 9141-2 with 5-baud init. Bus/protocol `ISO_9141_2_UART` / `SAE_J2190_on_ISO_9141_2`, 1 pin
`{pinA (default 7), K}` (thread FUN_0046e240 70347, create 70502-70511). Frame = raw ISO 9141-2
frame incl. checksum, checksum = 8-bit sum of all bytes before it (70899-70916). Firmware-side
framing uses the Xmt0 length queue, not a length byte.

### Init (event 9 -> FUN_0046fbe0 71086, thread case 1 state 1, 70589-70881)
SetComParam (all set on the live thread; dead copy FUN_0046b9c0 69354 is identical):

| ComParam | class | value |
|---|---|---|
| CP_TesterPresentHandling | 7 | 0 |
| CP_RepeatReqCountApp | 4 | 2 |
| CP_K_L_LineInit | 5 BUSTYPE | 0 |
| CP_StartMsgIndEnable | 3 | 0 |
| CP_TransmitIndEnable | 3 | 0 |
| CP_RequestAddrMode | 3 | 1 (physical) |
| (filters, pass-all) | | |
| CP_W1Min / CP_W1Max | 1 | 60000 / 300000 |
| CP_W2Min / CP_W2Max | 1 | 5000 / 20000 |
| CP_W3Min / CP_W3Max | 1 | 5000 / 20000 |
| CP_W4Min / CP_W4Max | 1 | 25000 / 500000 |
| CP_P2Min / CP_P2Max | 1 | 25000 / 50000 (NOT bumped to 300 ms) |
| CP_P3Min | 1 | 55000 (P3Max never set) |
| CP_P4Min / CP_P4Max | 1 | 5000 / 20000 |
| CP_InitializationSettings | 2 | 1 (5-baud) |
| CP_5BaudMode | 2 | 0 (full ISO 9141-2: tool sends ~KB2, ECU sends ~address) |
| CP_5BaudInitBaudrate | 1 | 10000/IB (IB default 5) |
| CP_5BaudAddressPhys | 3 | IA |
| CP_TIdle | 3 | 7000000 (7 s) |

Only P2/P3min/P4 are overridable from link attributes; W values are fixed. Then UPDATEPARAM ->
STARTCOMM (same ctrl as KW82) -> GetComParam(CP_Baudrate), fallback 10400 (0x28a0).

Big difference: the emulated CPU is FROZEN during the init. Event 0xd (firmware arms sync
measurement) with state 1 calls `FUN_00418550(26500)` = sleep the CPU for up to 26.5 s; the
first RESULT or ERROR resumes it (`FUN_00418510`, 70562/70573). So KW08 has no race, the init
just has to finish inside 26.5 s.

Tick (0x46d600 asm), 5 steps: baud fed in the W1min..W1max window (60-300 ms), KB1 after W2min
(5 ms), KB2 after W3min (5 ms), wait for ~KB2 from the firmware, then after W4min (25 ms)
Tech2Win delivers `~IA` (inverted address byte) itself, state 2. So again only RESULT[0..1] =
KB1 KB2 are used; the ECU's real ~address is not needed in the RESULT.

### Send/receive
No receive-only CoP, no keep-alive fake, no RX rewrite. Each firmware frame (8-bit sum checked) ->
cancel rx CoP if EXECUTING -> `SENDRECV(len, raw, {0, 1, -1, 0, TxFlag {4, 00000000}, 1,
{0,1,1,{00},{00},0,NULL}}, tag->1, &hCoP_rx)` -> wait EXECUTING (70883-70947). RESULT slots
released after P2min.

ERROR: during init (state 1) -> resume CPU, disable send (DAT_00d751cc = 0), state 2; in state 2
it only clears the "send pending" flags (DAT_00d75174 = 1), link stays up. Softer than KW81/82.

## 4. SDL_ISO (pdu_sdl_iso.c), link 0x1801, mode 9

No-init ISO 9141 UART link. Bus/protocol `ISO_9141_2_UART` / `SAE_J2190_on_ISO_9141_2`, 1 pin
`{DLCPinNumber = pinA if non-zero else 7, K}` (FUN_0048c5f0 84877, create in thread FUN_0048b0e0
84142 at 84253-84263). Frame: total length = byte 1, last byte = two's complement checksum
(84468-84488, FUN_0048c610 84892). No known SAAB user.

### Setup (thread start, BEFORE PDUConnect, no UPDATEPARAM ever)

| ComParam | class | value |
|---|---|---|
| CP_TesterPresentHandling | 7 | 0 |
| CP_RepeatReqCountApp | 4 | 0 |
| CP_StartMsgIndEnable | 3 | 0 |
| CP_InitializationSettings | 2 | 3 (no init) |
| CP_Baudrate | 5 | 10400 (hard-coded, no link attribute) |
| (filters, pass-all) | | |
| CP_P2Min / P2Max | 1 | 50000 / 100000 |
| CP_P3Min / P3Max | 1 | 50000 / 100000 |
| CP_P4Min / P4Max | 1 | 0 / 5000 |

Then PDUConnect (wait ONLINE), then `STARTCOMM(CoPDataSize 0, pCoPData -> one 00 byte, ctrl =
{Time 100, everything else 0, NumPossibleExpectedResponses 0}, tag NULL)` -> wait FINISHED.
Since params are set between Create and Connect and never followed by UPDATEPARAM, the DLL has
to apply the working buffer at Connect (or treat SetComParam as immediate).

### Runtime
* EN (event 9) -> enable + SetEvent("Start Rx") -> thread case 3 (84584-84624): permanent
  listener `SENDRECV(0, NULL, {0, NumSendCycles 0, NumReceiveCycles -1, 0, TxFlag {4, 0}, 1,
  {0, AcceptanceId 1, 1, {00}, {00}, 0, NULL}}, tag->1, &hCoP_rx)` -> wait EXECUTING.
* DI/CL (10/12) -> disable + SetEvent("Stop Rx") (case 4: cancel the listener).
* Firmware frame -> checksum -> `SENDRECV(len, raw, {Time 0, NumSendCycles 1,
  NumReceiveCycles 0, 0, TxFlag {4, 0}, 1, {0, AcceptanceId 0, 1, {00}, {00}, 0, NULL}}, tag->2,
  &hCoP_tx)` -> wait FINISHED (84468-84527). Send-only; replies come in on the listener. Only one
  send in flight (next send waits for FINISHED).
* RX ring is 10 slots, delivered immediately (no P2min hold).
* ERROR: logs ErrorCodeId/ExtraErrorInfoId, disables the link until next EN.

## 5. Short summaries

**KW886 / ISO8 (pdu_kw886.c, 0x1309, mode 4).** `KW_UART` / `KW81_on_KW_UART`, pins
`[{pinA, K}, {pinB, L}]`, NumPinData 2, or 1 when pinB == 0 (81358-81371). Init:
RepeatReqCountApp 0, CP_K_L_LineInit 0 (class 5), StartMsgIndEnable 0, filters, P2Min/P2Max/
P4Min/P4Max, CP_5BaudMode 3, 5BaudInitBaudrate 10000/IB, 5BaudAddressPhys IA, CP_W1Max 1000000,
CP_TIdle 4500000, CP_W4Min, UPDATEPARAM, STARTCOMM, GetComParam(CP_Baudrate). Has its own
"Tester Present Timer" event. Not SAAB.

**XDE (pdu_xde.c, 0x1000, mode 8).** GM 8192 UART. `SAE_J2740_UART` /
`XDE_5024_on_XDE_5024_UART`, 1 pin `{pinA default 9, SINGLE}`. CP_BecomeMaster, CP_BecomeSlave,
CP_Loopback (class 3), CP_P3Min_J2740 3800 µs, CP_RepeatReqCountApp, CP_ByteCountOffset 0x55,
CP_Baudrate 8192 (class 5), CP_TerminationType 2 (class 5), CP_SetRelinquishMastership /
CP_SetPollResponse (BYTEFIELD 0x107), filters, Connect, STARTCOMM. Not SAAB.

**Class 2 (pdu_class2.c, 0x2000, mode 7).** J1850 VPW. `SAE_J1850_VPW` /
`SAE_J2190_on_SAE_J1850_VPW`, 1 pin `{pin default 2, PLUS}` (68706-68715). CP_Baudrate 10400
(class 5, switched to 41600 and back with UPDATEPARAM for 4x mode), CP_TesterPresentHandling 0,
CP_RepeatReqCountApp 0, CP_P2Max, filters, STARTCOMM, SENDRECV. GM's 600->1000 ms param RAM
stretch (0x7FFF74) is CLASS2 only. No evidence the SAAB card uses it.

**CAN (pdu_can.c, CANdi ICD mode 0).** Firmware talks CANdi ICD, Tech2Win maps CAN commands to
PDU per CANdi channel (`FUN_00464c20` 65016). Two CLLs per channel: [0] ISO 15765 protocol,
[1] `ISO_11898_RAW` for raw/UUDT frames.
* ch0 SW-CAN: `SAE_J2411_SWCAN`, 1 pin `{1, SINGLE}`, protocol ISO_15031_5_on_ISO_15765_4 or
  ISO_15765_3_on_ISO_15765_2; variant 5 = `ISO_11898_3_DWFTCAN` pins 1(HI)+9(LOW).
* ch1 MS-CAN: `ISO_11898_2_DWCAN`, pins 3 HI + 11 LOW.
* ch2 HS-CAN: `ISO_11898_2_DWCAN`, pins 6 HI + 14 LOW (or 12/13 alternate); J1939 variants too.
* Comparams: CP_CanFillerByteHandling (3) = 1, CP_TerminationType, CP_Baudrate, CP_BitSamplePoint
  0x50, CP_SamplesPerBit, CP_SyncJumpWidth 0xF, CP_P3Phys 10000 µs, CP_RequestAddrMode,
  CP_SwCan_HighVoltage; SetUniqueRespIdTable with up to 64 entries of 9 class-6 params
  (CanPhysReqId/Format/ExtAddr, CanRespUSDTId/Format/ExtAddr, CanRespUUDTId/Format/ExtAddr);
  a permanent receive SENDRECV with per-ID expected responses; SW-CAN wake-up = SENDRECV of 4
  bytes `00 00 01 00` (CAN id 0x100, no data) with Time 500, TempParamUpdate 1.
* SAAB GMLAN (NG9-3 etc.) goes this way. Out of scope until the CAN phase.

## 6. Two K pins? pin 7 AND pin 8?

Never. Every K-line handler here passes exactly one K pin (pinA). The only two-pin K-line
cases are K + L (pin type `L`, pinB) in KW886 and KW2000, with NumPinData 2 only when pinB != 0.
SAAB modules sit on pin 7 or pin 8 depending on the module (memory: DICE pin 7, ABS pin 8), and
Tech2Win hands that pin straight through as DLCPinNumber. A J2534 v04.04 device only does pin 7
unless it supports J2534-2 pin switching (J1962_PINS), which is why the dpdu-passthru README tells
people to short 7 and 8.

## 7. Config files

* `RE/tech2win/Saab.conf` and `t2w_installer/extracted/default.conf` / `quickstart.conf` /
  `Saab NAO.conf`: the only D-PDU relevant switch is `-Pp` (D-PDU / Bosch MDI backend;
  `-PJ` also lands on D-PDU in this build). `-B0`, `-P0` card image, `-PC`, `-WR`, `-ATA`, `-W pfrc`,
  `-CB/-CT` colours, `-S` sound, `-KP*` keypad are not PDU related. No comparam/pin overrides.
* `CONF.DTA` is binary: `10 00 "CAPTURE0.BIN" 00 ...` + flag bytes = the ATA data-capture config
  (`$$CONF$$.DTA`), nothing D-PDU.
* MDI selection is the registry `Software\D-PDU API\Root File` -> root XML, short name
  `D_PDU_API_Bosch_MDI`, version 2.2.0 (pdu_api.c, other notes).

## 8. What a SAAB session can actually reach

* KW2000 (0x1308) for nearly all OG9-5 / OG9-3 K-line modules (other notes).
* KW82 (0x1302) for SRS on OG9-3 and early OG9-5 (third-party confirmation + card string).
* CAN through CANdi ICD -> pdu_can.c for the GMLAN cars (card scens opens 0x4001/0x4002/0x4004
  for HS/MS/SW in the high-voltage-wakeup code).
* KW81, KW08/KW94, KW886, SDL_ISO, XDE, Class2: handlers exist but nothing points at SAAB using
  them. KW08 only matters if some generic OBD path opens 0x1304.

## 9. What this means for the J2534 DLL (my take, not decomp)

* Implement RawMode only; payloads in and out are whole frames; never return the TX echo.
* Emit STATUS items for ONLINE/OFFLINE, EXECUTING/FINISHED/CANCELLED and call the callback with
  0x801 every time, or each step costs Tech2Win 10 s.
* GetStatus(CoP handle 0 or unknown) -> 0x60. CLL status must say COMM_STARTED after STARTCOMM.
* KW82: ISO9141 channel, 8192 baud, ISO9141_NO_CHECKSUM (16-bit sum is in the payload),
  FIVE_BAUD_MOD 1, address from CP_5BaudAddressPhys (don't hard-code 0x64), STARTCOMM RESULT =
  the 2 keyword bytes, GetComParam(CP_Baudrate) = the real baud, buffer anything the ECU sends
  between init and the receive-only CoP, keep every SENDRECV receiving until it's cancelled.
* KW81: same with FIVE_BAUD_MOD 1 (Tech2Win sets it explicitly), 8-bit two's complement checksum
  in the payload.
* KW08: ISO9141, FIVE_BAUD_MOD 0, 10400, NO_CHECKSUM (checksum already in the payload), W/P values
  from the comparams above, J2534 W5 <- CP_TIdle.
* SDL: ISO9141 no init, 10400, NO_CHECKSUM, send-only SENDRECV + one permanent listener.
* ERROR events break KW81/KW82/SDL sessions until the firmware does DI+EN. Only send them for real
  failures (failed 5-baud init), and still finish the CoP with COPST_FINISHED afterwards.
