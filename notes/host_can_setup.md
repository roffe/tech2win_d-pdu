# Tech2Win host side of the D-PDU API: pdu_can.c, CAN link SETUP / CONFIG

What GM Tech2Win (`emulator.exe`, 32-bit, build 2019-12-22) does with a D-PDU API DLL to set up
CAN, read out of the Ghidra decomp and the disassembly. Scope here is setup/config: lifecycle,
resources, ComParams, UniqueRespIdTable, filters, SW-CAN wakeup, status waits. The TX/RX data path
(CoP payloads, RESULT -> firmware, TX confirms) is covered in a separate note; where the two touch,
this note only says what the setup side hands over.

Sources:
- `RE/tech2win/out/decomp_all.c`, line numbers below are lines in that file. pdu_can.c is
  64674-67923, the CANdi ICD front end (`CANdi\candi_common.c`) is around 54600-56500.
- `RE/tech2win/emulator.exe`, objdump disassembly for the bits Ghidra garbled (return values,
  the PCAN worker thread at `0x468c30`, the drain's match logic). Marked "asm".
- Object id table dumped straight from .data (same method as `host_other_protocols.md` section 0).
- `dpdu-passthru/driver/pdu_api.h` for struct layouts and enum values.
- Real firmware ICD traffic: `t2emu/tech2/testdata/candi_golden.json` (118 request/reply pairs the
  real v148 firmware sent to the real candiapp, 2003 9-3 T8 session up to the Engine menu), plus
  `SOFTCANDI.md` for the module side.

Read `host_api_kw2000.md` sections 0, 4, 10, 11 first. Everything there about handles, item
layouts, the 0x801 callback and "STATUS item or 10 s stall" applies here too. Differences are
called out below.

Conventions:
- hMod = `*(DAT_00d5dca0 + 0x208)` (= `DAT_005236f0`, same module handle as K-line).
- "ch" = Tech2Win's CAN channel index 0/1/2 (see 1.3). Per-channel state is a 0x768-byte struct at
  `0xA48390 + ch*0x768` (layout in 1.4). Field names below are offsets into it.
- CLL[0] / CLL[1] = the two ComLogicalLinks of a channel (handles at struct +0x00 / +0x04).
- UNDEF = 0xFFFFFFFF, ID_UNDEF = 0xFFFFFFFE.
- Wrapper names: `FUN_004614d0` Cancel, `FUN_00461520` Connect, `FUN_004615c0` CreateCLL,
  `FUN_00461620` DestroyCLL, `FUN_00461670` DestroyItem, `FUN_00461700` Disconnect,
  `FUN_004617f0` GetEventItem, `FUN_00461b10` IoCtl, `FUN_00461c60` RegisterEventCallback,
  `FUN_00461cb0` SetComParam, `FUN_00461d00` SetUniqueRespIdTable, `FUN_00461d50`
  StartComPrimitive, `FUN_00461e70` = log a PDU error.

Object ids used by the CAN code (variable -> shortname, from the .data name tables):

| kind | variable | shortname |
|---|---|---|
| bus | `0x51F278` | SAE_J2411_SWCAN |
| bus | `0x51F27C` | ISO_11898_2_DWCAN |
| bus | `0x51F28C` | ISO_11898_3_DWFTCAN |
| bus | `0x51F280` | SAE_J1939_11_DWCAN |
| protocol | `0x5216B0` | ISO_15765_3_on_ISO_15765_2 |
| protocol | `0x5216B8` | ISO_15031_5_on_ISO_15765_4 |
| protocol | `0x5216BC` | SAE_J1939_73_on_SAE_J1939_21 |
| protocol | `0x5216C8` | ISO_11898_RAW |
| pin type | `0x522230` / `0x522234` / `0x522244` | HI / LOW / SINGLE |
| ioctl | `0x5234DC` / `0x5234E0` | PDU_IOCTL_START_MSG_FILTER / PDU_IOCTL_CLEAR_MSG_FILTER |
| comparam | `0x51E010` | CP_CanFillerByteHandling |
| comparam | `0x51E064` | CP_TerminationType |
| comparam | `0x51E000` | CP_Baudrate |
| comparam | `0x51E004` | CP_BitSamplePoint |
| comparam | `0x51E054` | CP_SamplesPerBit |
| comparam | `0x51E060` | CP_SyncJumpWidth |
| comparam | `0x51E0C4` | CP_P3Phys |
| comparam | `0x51E050` | CP_RequestAddrMode |
| comparam | `0x51E05C` | CP_SwCan_HighVoltage |
| comparam | `0x51E01C`,`018`,`014` | CP_CanPhysReqId / Format / ExtAddr |
| comparam | `0x51E028`,`024`,`020` | CP_CanRespUSDTId / Format / ExtAddr |
| comparam | `0x51E034`,`030`,`02C` | CP_CanRespUUDTId / Format / ExtAddr |

----------------------------------------------------------------------------------------------

## 0. Hard requirements (CAN on top of the K-line list)

1. **Every setup call must return 0.** The create chain aborts on the first non-zero return of:
   CreateComLogicalLink, SetComParam(CP_CanFillerByteHandling) **on CLL[1]** (its result is ORed
   with the RegisterEventCallback results, asm `0x465105`), RegisterEventCallback, any of the 11
   bus SetComParams (TerminationType, Baudrate, BitSamplePoint, SamplesPerBit, SyncJumpWidth,
   P3Phys), IoCtl CLEAR/START_MSG_FILTER, SetUniqueRespIdTable, SetComParam(CP_RequestAddrMode),
   Connect, STARTCOMM, the receive SENDRECV. Abort = the CLLs stay created but never connected and
   the firmware gets "create failed" (0xD0). Unknown ComParam? Store it and return 0.
2. **SetComParam before PDUConnect, no UPDATEPARAM before STARTCOMM.** All bus ComParams and the
   UniqueRespIdTable are set between Create and Connect and never committed with UPDATEPARAM at
   create time. Apply the working buffer at Connect (or treat SetComParam as immediate). Later
   changes do come with UPDATEPARAM (5.6, 4.4).
3. **TempParamUpdate = 1 must restore working := active after the call returns** (ISO note in
   `PDU_COP_CTRL_DATA`). Tech2Win sets CP_SwCan_HighVoltage=1 and CP_CanFillerByteHandling=0 for the
   wakeup, and RequestAddrMode / FillerByteHandling per TX, and never sets them back. If the DLL
   leaves them in the working buffer, the next UPDATEPARAM (any mailbox write) commits HV mode and
   no-padding permanently.
4. **Status items, per CLL, fast**: ONLINE after Connect, FINISHED after STARTCOMM / STOPCOMM /
   UPDATEPARAM / the HV wakeup SENDRECV, EXECUTING after the receive SENDRECV and after every TX
   SENDRECV, CANCELLED after Cancel, OFFLINE after Disconnect. Each wait is 10 s on that CLL's own
   callback event. A mailbox write costs 1 SetUniqueRespIdTable + 2 UPDATEPARAM + 1 Cancel + 1
   SENDRECV, and the real session does ~80 of them back to back while the firmware waits for each
   ICD reply (8.3). One missing status = 10 s per command.
5. **Disconnect waits OFFLINE even when Disconnect failed** (`FUN_00465270`, 65304-65308).
   **Teardown Cancel waits CANCELLED even when Cancel failed** (65293-65297). So: always queue the
   status, even for a CLL that was never connected / a CoP that already ended.
6. **Cancel of the permanent receive CoP must return 0.** If it returns non-zero in a rebuild
   (`FUN_00465d90` 65855), Tech2Win logs, returns, keeps the stale handle and never starts a new
   receive on that channel again (every later rebuild tries to cancel the same dead handle). This
   is the opposite of the K-line rule "Cancel on a non-running CoP must return non-zero".
7. **Never finish the receive CoP (NumReceiveCycles -1) on your own**, no RX_TIMEOUT errors on it.
   Tech2Win does not notice it ended; RX just stops until the next rebuild.
8. **Up to 6 CLLs at once**, 2 per channel, all three channels connected at the same time in a
   normal SAAB session (8). A channel the J2534 device can't do (MS-CAN) still has to be created
   successfully (virtual link, see 9).
9. CAN callbacks are `__stdcall`, 5 args (`ret 0x14`, asm `0x468efd`), only 0x801 does anything.
10. hCLL 0 must never be valid: after a teardown Tech2Win zeroes the handle slots, and a late
    callback makes it call `PDUGetEventItem(hMod, 0, ..)`, which must return non-zero.

----------------------------------------------------------------------------------------------

## 1. Architecture

### 1.1 Where CAN comes from

The Tech2 firmware never talks CAN to Tech2Win directly. It talks the **CANdi ICD** (the CS9
protocol of the CANdi pod, see `SOFTCANDI.md`) over the 16550 UART. In protocol mode 0 Tech2Win's
CANdi model (`CANdi\candi_common.c`) answers the pod-level commands itself and forwards the CAN
ones to the backend, which for the D-PDU build is pdu_can.c.

- Mode 0 is selected by the trap spy on firmware `L0 OP` with link type **0x1A00** (CANdi, switches
  immediately, `FUN_00420800` 17477, `FUN_00459d40(0)`), and also by 0x4001 HS-CAN / 0x4002 MS-CAN /
  0x4004 SW-CAN / 0x4000 (switch on EN). The link type does not pick a bus: the bus comes from the
  ICD command's channel bits.
- Mode 0 open = `FUN_00457000` (54751) -> backend init `FUN_00468170` (67353).
- Mode 0 close = asm `0x4573f0` -> `0x468df0`: SetEvent("PCAN_Event_Shutdown"), Sleep(1) until the
  PCAN thread has exited (`DAT_00a48380`).
- Mode 0 event handler = asm `0x468d90`: `return 0x14`. Link EN/DI/RS/CL do nothing for CAN; the
  whole lifecycle is driven by ICD commands.
- Mode switch (`FUN_00459d40`) closes the old protocol only when the mode actually changes. So the
  CAN stack lives as long as the firmware stays on CAN/CANdi links; opening a K-line link (e.g.
  0x1308) closes mode 0 and tears down all CAN CLLs. Emulator exit switches to mode 0xD, same thing.

### 1.2 Threads and events (`FUN_00468170` 67353, thread asm `0x468c30`)

Init creates 8 auto-reset events, in this order (array at `0xA499C8`):

| idx | name | signalled by |
|---|---|---|
| 0 | PCAN_CallBackEvent_SW | CLL cb ch0 CLL[0] (`FUN_00468f60`) |
| 1 | PCAN_CallBackEvent_SW_RAW | ch0 CLL[1] (`FUN_00468f80`) |
| 2 | PCAN_CallBackEvent_MS | ch1 CLL[0] (`FUN_00468f20`) |
| 3 | PCAN_CallBackEvent_MS_RAW | ch1 CLL[1] (`FUN_00468f40`) |
| 4 | PCAN_CallBackEvent_HS | ch2 CLL[0] (`FUN_00468ee0`) |
| 5 | PCAN_CallBackEvent_HS_RAW | ch2 CLL[1] (`FUN_00468f00`) |
| 6 | PCAN_Event_ICD_Message | ICD command from the firmware (`FUN_004646f0` 64656) |
| 7 | PCAN_Event_Shutdown | mode 0 close |

Then: flush all RX queues (`FUN_00466630(3)`), clear the per-channel "set by firmware" flags
(+0x30..+0x40), `FUN_00468460(ch, 2)` = load GMLAN defaults for all three channels (5.x), fetch the
module struct (`FUN_00461e30`), start the **PCAN thread** (`_beginthread(0x468c30)`).
**No D-PDU call happens at mode 0 open.** Nothing exists until the firmware sends a create.

PCAN thread (asm `0x468c30`): loop while hMod != -1:
`WaitForMultipleObjects(8, events, FALSE, 5000)`:
- 0..5 -> drain that CLL (`FUN_004682b0(ch, cll, 0, 0)`, section 7)
- 6 -> `FUN_004668f0(0xA45C48 /*cmd*/, 0xA46FE0 /*reply*/)`, the ICD dispatcher (1.3)
- 7 -> teardown ch2, ch1, ch0 (`FUN_00465270`), close the 8 handles, exit.

**Every CAN D-PDU call runs on this one thread**, including the status waits. The module-level
event drain thread from `host_api_kw2000.md` 4.1 still runs concurrently. The emulated CPU thread
never calls the DLL.

ICD hand-off: `FUN_004646f0` copies the firmware command into `0xA45C48` only when the backend is
idle (`DAT_00a4837c == 1`) and sets event 6. The handler sets idle again when it returns. While a
handler is blocked in a status wait, the next forwarded ICD command is refused (returns 10) and the
firmware gets no reply for it. So slow statuses directly become firmware ICD timeouts.

### 1.3 ICD command dispatch

Command byte = `[family 0xF8][channel bits 0x06][reply bit 0x01]`. Tech2Win's channel index is
`(cmd - family) / 2`: **0 = SW-CAN, 1 = MS-CAN, 2 = HS-CAN, 3 = all three**. Same mapping as the
real module (`SOFTCANDI.md` section 2: `cmd&6` 0/2/4/6). Every command ends in a two's-complement
checksum byte (`FUN_00468e30`); bad checksum = reply status 0x81, bad length = 0x83.

`FUN_00456bb0` (54628, candi_common) handles families 0x08/0x18/0x20/0x30/0x38/0x80/0x90/0xA0
itself, pre-filters 0x88 and 0x98, and forwards 0x00 (TX) and 0x28 (`2E` poll) straight to the
backend. Link Control (0x88) esids handled locally and **ignored** (ack 0x00, log "NOT
IMPLEMENTED"): 1 (config store), 2 Termination Resistor, **4 Switch To High Speed**, 7 Block Counter
Override, 8 Set Link Padding, 9 Listen Only, 0xA Link Reset, 0xD Min Separation Time, 0x10 Receive
First Frame Control (`FUN_00458650` 55994 and 56052-56180). Forwarded to pdu_can:

| ICD | Tech2Win handler | what it does on the D-PDU side |
|---|---|---|
| `8x 03 <variant>` | `FUN_004671b0` 66670 | **create** both CLLs for the channel(s) (2.2). Reply 0x40 at once, result later via `80 05` |
| `8x 05` | `FUN_00466d80` 66492 | **destroy** both CLLs (2.3). Reply 0x40, result via `80 05` (always OK) |
| `8x 06` | `FUN_00466e50` 66531 | flush the channel's RX queue. No D-PDU call |
| `8x 0B 00` / `8x 0B 01 b0 b1 b2` | `FUN_00466c00` 66420 | read / write bit timing (4.2). Write -> bus ComParams + UPDATEPARAM + receive restart, only if changed |
| `8x 0C 00` / `8x 0C 01 w0..w3 [w4]` | `FUN_00466ee0` 66556 | read / write 5 u16 timing words. **Stored and read back only, never reaches D-PDU** |
| `88 0E` | `FUN_004670e0` 66636 | **SW-CAN high-voltage wakeup** (6). ch0 only, else 0x83; no RAW CLL = 0xD0. Reply 0x40, result via `80 05` (0x87 on failure) |
| `8x 0F <x>` | `FUN_00467290` 66706 | CP_RequestAddrMode = 2 on both CLLs + UPDATEPARAM both. `x != 0` stored at +0x16 |
| `9x 01 00` / `9x 01 01 <20 B>` / `9x 01 11` | `FUN_00467390` 66748 | read / write / auto the "primary filter" -> IoCtl filters on CLL[0] (5.5) |
| `9x 02 <sub> <idx> ...` | `FUN_00467670` 66879 | the 64-entry mailbox table -> SetUniqueRespIdTable + UPDATEPARAM + receive restart (5.1-5.4) |
| `0x <flags> <id> <data>` | `FUN_00467c20` 67147 | TX (data path note). Sets temp ComParams (4.5) |
| `2E` | `FUN_00467aa0` 67092 | firmware RX poll (data path note) |

Async completion: create / destroy / wakeup reply `[cmd|1, esid, 0x40]` immediately, set the
pending state with `FUN_004573d0(1,0)`, do the D-PDU work, then `FUN_004573d0(2,0)` (ok) or
`FUN_004573d0(3, code)`. The firmware polls `80 05` (`FUN_004579d0` 55302) which answers 0x40 while
busy, 0x00 when done, `code|0x80` on failure (0xD0 create failed, 0x87 wakeup failed). The real
module also answers 0x40 to `88 03` and `8E 05`, so this is the expected shape. Everything else
(filters, mailboxes, bit timing) is **synchronous**: the ICD reply goes out only after all D-PDU
calls and status waits are done.

### 1.4 Per-channel struct (`0xA48390 + ch*0x768`)

| off | addr (ch0) | meaning |
|---|---|---|
| +0x00 | 0xA48390 | hCLL[0] (protocol CLL) |
| +0x04 | 0xA48394 | hCLL[1] (ISO_11898_RAW CLL) |
| +0x08 | 0xA48398 | hCoP of the permanent receive SENDRECV on CLL[0] |
| +0x0C / +0x10 | 0xA4839C / A0 | int tag[0], tag[1], value 0. `pCllTag` / `pCoPTag` point here |
| +0x14 | 0xA483A4 | u8 variant (protocol) from `8x 03` |
| +0x15 | 0xA483A5 | u8 set 1 at create, 0 on TX_ERROR, never read |
| +0x16 | 0xA483A6 | u8 from `8x 0F` (x != 0), only used on TX_ERROR |
| +0x17..0x1C | | last TX header / TX-done pending (data path) |
| +0x20,21,22 | 0xA483B0 | bit timing bytes b0 = SJA1000 BTR0, b1 = BTR1, b2 (read-back only) |
| +0x24..0x2D | 0xA483B4 | 5 u16 timing words from `8x 0C` (read-back only) |
| +0x30 | 0xA483C0 | flag: primary filter written by firmware |
| +0x34 | 0xA483C4 | flag: mailbox table written by firmware |
| +0x38 | 0xA483C8 | flag: bit timing written by firmware |
| +0x3C | 0xA483CC | flag: timing words written by firmware |
| +0x40 | 0xA483D0 | flag: `8x 0F` received |
| +0x44..0x53 | 0xA483D4 | primary filter as sent: code0, mask0, code1, mask1 (u32) |
| +0x54, +0x56 | 0xA483E4 | 2 more u16 from the primary filter (unused by D-PDU) |
| +0x58..0x67 | 0xA483E8 | D-PDU filter bytes: pattern0[4], mask0[4], pattern1[4], mask1[4] (big endian) |
| +0x68..0x767 | 0xA483F8 | 64 mailbox entries x 0x1C (5.2) |

The "written by firmware" flags (+0x30..+0x40) are only cleared at mode 0 open (and +0x34 by
`9x 02 05`, +0x40 by destroy). While a flag is set, create does not reload the default for that
group.

----------------------------------------------------------------------------------------------

## 2. Lifecycle (item 1)

### 2.1 Variant byte

`8x 03 <variant>`, byte 2 picks the protocol:

| variant | meaning | CLL[0] protocol | notes |
|---|---|---|---|
| 1 | OBD-II on CAN | ISO_15031_5_on_ISO_15765_4 | |
| 2 | **GMLAN** / ISO 15765-3 | ISO_15765_3_on_ISO_15765_2 | **the only one the SAAB card sends** (8) |
| 3 | J1939 | SAE_J1939_73_on_SAE_J1939_21 on SAE_J1939_11_DWCAN | MS/HS only; on SW-CAN logs "creating J1939 link for SW-CAN" and fails |
| 5 | "UDS" (log text for MS) | ISO_15765_3_on_ISO_15765_2 | ch0 becomes ISO_11898_3_DWFTCAN pins 1+9, no padding |
| other | | | create fails (0xD0) |

### 2.2 Create: `FUN_004671b0` -> `FUN_00464c20` (65016) per channel

`8x 03 v` (length 4). For each selected channel (all three for `8E 03`, stops at the first
failure), on the PCAN thread:

```
FUN_00465270(ch)            teardown of whatever exists on this channel (2.3)
FUN_00468460(ch, v)         "ResetBus": variant = v, reload defaults for groups not set by firmware (4.2, 5.4, 5.5)
FUN_00466630(ch)            flush the channel's firmware RX queue
FUN_004572c0(ch)            CANdi status block init (ICD side)
for i in 0,1:
    build PDU_RSC_DATA (section 3), CLL[1] gets protocol ISO_11898_RAW
    PDUCreateComLogicalLink(hMod, &rsc, 0xFFFFFFFE, &tag[ch][i], &hCLL[i], &{4, 80 00 00 00})
    SetComParam(hCLL[i], CP_CanFillerByteHandling, class 3, UNUM32, v != 5 ? 1 : 0)
RegisterEventCallback(hMod, hCLL[0], cb_ch_0); RegisterEventCallback(hMod, hCLL[1], cb_ch_raw)
FUN_00465540(ch)            bus ComParams on both CLLs + P3Phys on CLL[0] (4.1)
FUN_00465bf0(ch)            IoCtl CLEAR_MSG_FILTER + START_MSG_FILTER on CLL[0] (5.5)
FUN_00466070(ch)            SetUniqueRespIdTable on CLL[0] (5.3)
FUN_00465810(ch)            SetComParam CP_RequestAddrMode = 2 on CLL[0], CLL[1]
for i in 0,1: PDUConnect(hMod, hCLL[i]); wait STATUS ONLINE 0x8051 on CLL i
FUN_00465900(ch)            for i in 0,1: STARTCOMM on CLL i; wait FINISHED 0x8012 on CLL i
FUN_00465d90(ch)            permanent receive SENDRECV on CLL[0] (5.4); wait EXECUTING 0x8011
+0x15 = 1; return 0
```
Any non-zero step: log, return 0xA, firmware gets 0xD0 via `80 05`. Note the callbacks are
registered after both CLLs exist and after the filler ComParam, not right after each create.

STARTCOMM and STOPCOMM on CAN (`FUN_00465900` 65575, `FUN_00465a00` 65632):
`PDUStartComPrimitive(hMod, hCLL, 0x8001 / 0x8002, CoPDataSize 0, pCoPData -> one 00 byte,
&{Time 100, NumSendCycles 0, NumReceiveCycles 0, TempParamUpdate 0, TxFlag {0, NULL}, 0, NULL},
pCoPTag NULL, &h)`. No payload, no expected response. Just move the CLL to COMM_STARTED / back and
post FINISHED. Tech2Win never calls PDUGetStatus on CAN, so the CLL state is not checked.

### 2.3 Teardown: `FUN_00465270` (65278)

```
if (hCoP_rx != 0 && hCLL[0] != 0) { Cancel(hCLL[0], hCoP_rx) /*error only logged*/; wait CANCELLED on CLL 0; hCoP_rx = 0 }
FUN_00465a00(ch): for i in 0,1 while hCLL[i] != 0: STOPCOMM; if it returned 0, wait FINISHED on CLL i
for i in 0,1: if hCLL[i]:
    PDUDisconnect   (error only logged)
    wait OFFLINE 0x8050 on CLL i     <- also when Disconnect failed
    PDURegisterEventCallback(hMod, hCLL[i], NULL)
    PDUDestroyComLogicalLink         (error only logged)
hCLL[0] = hCLL[1] = tag = 0, reset +0x14.. fields
```
STOPCOMM is sent whatever the CLL state is (no GetStatus), so STOPCOMM on an ONLINE-only CLL must
also just post FINISHED. Unregister happens **before** Destroy here (K-line does it after).

### 2.4 When things are created and destroyed

- Created: only by `8x 03`. A second `8x 03` for a channel that already exists = full teardown +
  create (the DLL sees Destroy/Create of both CLLs mid-session).
- Destroyed: `8x 05` / `8E 05` (all), a re-create, mode 0 close (switch to a non-CAN protocol,
  emulator exit). Nothing else: no idle timeout, ERROR items don't destroy anything.
- How many: 2 per created channel, each channel independent. The SAAB session creates all three
  channels = **6 CLLs alive at once**, all connected and COMM_STARTED.

----------------------------------------------------------------------------------------------

## 3. Resources per channel / variant (item 2)

`FUN_00464c20` 65067-65190. Pins come from `FUN_00459270(pinA, pinB, linkType, ..)` (56637),
which is a plain copy plus the "COMM: Determine Pin K:%d L:%d" log; the link type argument is only
for the "SAEJ1962 pins %s & %s" status text.

| ch | variant | BusTypeId | CLL[0] ProtocolId | NumPinData | pins {DLCPinNumber, DLCPinTypeId} |
|---|---|---|---|---|---|
| 0 SW | 1 | SAE_J2411_SWCAN | ISO_15031_5_on_ISO_15765_4 | 1 | {1, SINGLE} |
| 0 SW | 2 | SAE_J2411_SWCAN | ISO_15765_3_on_ISO_15765_2 | 1 | {1, SINGLE} |
| 0 SW | 3 | (fails) | | | |
| 0 SW | 5 | ISO_11898_3_DWFTCAN | ISO_15765_3_on_ISO_15765_2 | 2 | {1, HI}, {9, LOW} |
| 1 MS | 1 | ISO_11898_2_DWCAN | ISO_15031_5_on_ISO_15765_4 | 2 | {3, HI}, {11, LOW} |
| 1 MS | 2, 5 | ISO_11898_2_DWCAN | ISO_15765_3_on_ISO_15765_2 | 2 | {3, HI}, {11, LOW} |
| 1 MS | 3 | SAE_J1939_11_DWCAN | SAE_J1939_73_on_SAE_J1939_21 | 2 | {3, HI}, {11, LOW} |
| 2 HS | 1 | ISO_11898_2_DWCAN | ISO_15031_5_on_ISO_15765_4 | 2 | {6, HI}, {14, LOW} |
| 2 HS | 2 | ISO_11898_2_DWCAN | ISO_15765_3_on_ISO_15765_2 | 2 | {6, HI}, {14, LOW}, or {12, HI}, {13, LOW} (below) |
| 2 HS | 3 | SAE_J1939_11_DWCAN | SAE_J1939_73_on_SAE_J1939_21 | 2 | {6, HI}, {14, LOW} |
| 2 HS | 5 | ISO_11898_2_DWCAN | ISO_15765_3_on_ISO_15765_2 | 2 | {6, HI}, {14, LOW} |

CLL[1] is identical (same bus, same pins) except **ProtocolId = ISO_11898_RAW**.

Other create arguments, both CLLs:
- resourceId = 0xFFFFFFFE (PDU_ID_UNDEF)
- CllCreateFlag = `{NumFlagBytes 4, -> 80 00 00 00}` = RawMode (bit 31), checksum bit clear
- pCllTag = `0xA4839C + ch*0x768 + i*4` (int, value 0). Tech2Win's CAN code never dereferences
  pCllTag or pCoPTag (unlike KW2000), but pass them through as given.
- HS-CAN pins 12/13: only for variant 2, when `FUN_0041c8a0` (14935) is true, i.e. the firmware
  executed the Tech2Win SIGNAL 0x5300 (`sf $5300.w`, `CPU\signals.c` `FUN_0041c8f0` 14959) less
  than 5 s (0x4FFFBC8 cycles) ago; 0x5301 clears it. **The SAAB v148 card never does this**
  (byte search of every `.dwn` and the card image: no `51 F8 53 00`), so SAAB HS-CAN is always
  6/14.

What the two CLLs are for:
- **CLL[0] (ISO 15765 / OBD / J1939)**: all reception (USDT and UUDT, through the permanent
  receive CoP and the UniqueRespIdTable), the IoCtl filters, and TX of ICD frames whose id field
  has bit 15 set (GMLAN length-framed / ISO-TP payloads).
- **CLL[1] (ISO_11898_RAW)**: TX of raw ICD frames (id bit 15 clear) and the SW-CAN HV wakeup.
  No filters, no table, **no receive CoP is ever started on it**. Its event queue is drained and
  any RESULT on it would still be forwarded, but Tech2Win never asks for one.

----------------------------------------------------------------------------------------------

## 4. ComParams (item 3)

All `PDUSetComParam(hMod, hCLL, &{ItemType 0x1200, id, DataType 0x105 UNUM32, class, -> u32})`.
Class numbers: 1 TIMING, 3 COM, 5 BUSTYPE, 6 UNIQUE_ID.

### 4.1 Inventory

| ComParam | class | CLL | value | when |
|---|---|---|---|---|
| CP_CanFillerByteHandling | 3 | 0 and 1 | `variant != 5` -> 1, variant 5 -> 0 | create, before Connect |
| CP_TerminationType | 5 | 0 and 1 | ch0: variant 5 -> 0, else `baud > 33333 ? 4 : 0`; ch1/ch2: 2 | create + bit-timing write |
| CP_Baudrate | 5 | 0 and 1 | from BTR0/BTR1 (4.2) | create + bit-timing write |
| CP_BitSamplePoint | 5 | 0 and 1 | 0x50 (80) constant | create + bit-timing write |
| CP_SamplesPerBit | 5 | 0 and 1 | BTR1 bit 7 (SJA1000 SAM): 0 = 1 sample, 1 = 3 | create + bit-timing write |
| CP_SyncJumpWidth | 5 | 0 and 1 | 0x0F (15) constant, BTR0 SJW bits ignored | create + bit-timing write |
| CP_P3Phys | 1 | 0 only | 10000 (us) | create + bit-timing write |
| CP_RequestAddrMode | 3 | 0 and 1 | **2** constant (`FUN_00465810` ignores its 2nd arg, asm `0x46585b`) | create (before Connect), `8x 0F` (+UPDATEPARAM) |
| CP_SwCan_HighVoltage | 3 | 1 (ch0 only) | 1, used with TempParamUpdate 1 | `88 0E` (6) |
| CP_CanFillerByteHandling | 3 | 1 (ch0) | 0, with TempParamUpdate 1 | `88 0E` |
| CP_RequestAddrMode | 3 | TX CLL | `extAddrFlag + 1`, only if != 1, TempParamUpdate 1 | per TX (4.5) |
| CP_CanFillerByteHandling | 3 | TX CLL | ICD TX byte1 bit 7, only if != create value, TempParamUpdate 1 | per TX (4.5) |
| 9 x CP_Can*Id/Format/ExtAddr | 6 | 0 | inside SetUniqueRespIdTable only (5.3) | create, every mailbox change |

Order at create (asm `0x465540..0x4657aa`): TerminationType CLL0, CLL1; Baudrate CLL0, CLL1;
BitSamplePoint x2; SamplesPerBit x2; SyncJumpWidth x2; P3Phys CLL0. First failure aborts.

**Never set by the CAN code** (verified on the disassembly: no reference to these ids in
`0x464650..0x468fa0`): CP_As, CP_Bs, CP_Cr, CP_Cs, CP_CanFillerByte (the value), CP_P2Max/Min,
CP_TesterPresentHandling, CP_RepeatReqCountApp, CP_StartMsgIndEnable, CP_TransmitIndEnable,
CP_Loopback. CP_CanFuncReqId / Format / ExtAddr are not even looked up with PDUGetObjectId. Pick
defaults that make sense: no tester present from the DLL (the firmware sends its own `3E`), no
loopback, no TX indications, no P2 timeout errors, ISO-TP timing per ISO 15765-2 defaults,
filler byte 0x00 (the real CANdi pads with zeros, `SOFTCANDI.md` 7).

The `8x 0C` timing words (defaults: variant 1 = 100/100/100/150/0, variant 2 and 5 =
250/150/150/150/0, variant 3 = 100/1250/1250/250/550) are stored for read-back only. They are NOT
mapped to CP_As/Bs/Cr/Cs.

### 4.2 Bit timing -> CP_Baudrate

`FUN_00465540` 65432-65436, with b0 = BTR0 (+0x20), b1 = BTR1 (+0x21), SJA1000 layout and the
CANdi's 24 MHz clock:
```
baud = 24000000 / ( ((b1>>4 & 7) + 1) + ((b1 & 0xF) + 1) + 1 ) / ( (b0 & 0x3F) + 1 ) / 2   (integer)
```
Source of b0/b1: the firmware's `8x 0B 01 b0 b1 b2` (`FUN_00466c00`), else the ResetBus default at
create (`FUN_00468460` 67471-67512), only while the "bit timing set" flag (+0x38) is clear:

| variant | ch | b0 b1 b2 | CP_Baudrate | TerminationType |
|---|---|---|---|---|
| 1, 3 | any | C2 3A 01 | 250000 | ch0: 4 (250000 > 33333; variant 3 on ch0 fails anyway), else 2 |
| 2 | 0 SW | DD 36 01 | 33333 | 0 |
| 2, 5 | 1 MS | C6 5A 00 | 95238 | 2 |
| 2, 5 | 2 HS | C1 36 00 | 500000 | 2 |
| 5 | 0 | 57 16 00 | 50000 (DWFTCAN) | 0 |

A bit-timing write that changes b0/b1/b2 on an existing channel re-sends the whole 4.1 bus block
(11 SetComParams), then UPDATEPARAM on CLL[0] and CLL[1] (each waits FINISHED), then restarts the
receive CoP (Cancel + wait CANCELLED + SENDRECV + wait EXECUTING). So **CP_Baudrate can change on a
connected, COMM_STARTED CLL via UPDATEPARAM**; the DLL has to re-open/re-rate the physical channel.
Unchanged values or no CLLs: only stored. 83333 (e.g. b0 = 0xCB, b1 = 0x36) would give
TerminationType 4 on SW-CAN.

### 4.3 What the Format values mean

Set only inside the UniqueRespIdTable. Built from the mailbox entry (5.2):
```
CanPhysReqFormat  (FUN_00464ba0 64972) = 0x20 | (flags & 0x10) | (req & 0x40000000 ? 0x08 : 0) | (flags & 0x80 ? 0x05 : 0) | (req & 0x20000000 ? 0x02 : 0)
CanResp*Format    (FUN_00464be0 64995) =        (flags & 0x40 ? 0x08 : 0) | (flags & 0x80 ? 0x05 : 0) | (req & 0x20000000 ? 0x02 : 0)
```
(`flags` = entry byte +0x0A, `req` = entry +0x10, the request id word.) Bits 0-3 follow the ISO
22900-2 CAN format table as I remember it (not on disk to check): bit 0 segmented (ISO-TP),
bit 1 29-bit id, bit 2 flow control, bit 3 extended addressing. So 0x05 = normal segmented 11-bit
with FC, 0x07 = same 29-bit, 0x0D/0x0F = extended addressing, 0x00 = unsegmented 11-bit (UUDT).
Bits 4 (0x10, copied from mailbox flag bit 4) and 5 (0x20, always set on requests) are not in that
base table: treat as MDI vendor bits and ignore them. Seen values: **requests 0x35** (firmware
GMLAN pairs, flags 0x92) or 0x25 (Tech2Win defaults, flags 0x81/0x82), **USDT responses 0x05**,
**UUDT responses 0x00**. ExtAddr values are always 0: GMLAN's functional ext address (FE) is not a
ComParam, it travels as the first payload byte of the TX (4.5).

### 4.4 UPDATEPARAM (`FUN_00465b00` 65689)

`PDUStartComPrimitive(hMod, hCLL[i], 0x8003, 0, NULL, pCopCtrlData NULL, &tag[ch][i], &h)` then
wait FINISHED on CLL i, for i = 0 then 1. Called after: a changed bit-timing write, every mailbox
table change (5.1), `8x 0F`. **Not** after IoCtl filter changes and **not** at create. Must be
cheap: the real session does ~160 of them in a row (8.3); only touch the hardware if baud/pins
actually changed.

### 4.5 Per-TX temporary ComParams (touches the data path)

`FUN_00467c20` 67228-67324, on the CLL the TX goes to (CLL[0] if ICD id bit 15 set, else CLL[1]):
- ICD id bit 14 (ext address byte present, GMLAN functional `C1 01` + `FE ..`): SetComParam
  CP_RequestAddrMode = 2. Compared against a global that create sets to 1 (`DAT_00d5dc9c`), so
  without the bit nothing is set (the CLL keeps the 2 from create).
- ICD TX byte 1 bit 7 vs the create value of CP_CanFillerByteHandling (`DAT_00d5dc98`): if
  different, SetComParam CP_CanFillerByteHandling = that bit.
- If either was set, the SENDRECV goes with **TempParamUpdate 1**, else 0. Ctrl is
  `{Time 500, NumSendCycles 1, NumReceiveCycles 0, Temp, TxFlag {4, ..}, 0, NULL}` -> wait
  EXECUTING on that CLL.
- Net effect: CP_RequestAddrMode is effectively always 2 ("functional") on CAN. It carries no
  useful information for a J2534 backend; the framing comes from the TX data and TxFlag
  (TxFlag byte 3 0x80 = ext address byte present, 0x40 = pad, byte 2 0x01 = 29-bit, i.e. the
  J2534 ISO15765_ADDR_TYPE / ISO15765_FRAME_PAD / CAN_29BIT_ID bit positions).

----------------------------------------------------------------------------------------------

## 5. UniqueRespIdTable, filters and the permanent receive (item 4)

### 5.1 Where the entries come from: ICD `9x 02` (`FUN_00467670` 66879)

| ICD | length | effect on entry `idx` (byte 3) | rebuild? |
|---|---|---|---|
| `9x 02 00 idx` | 5 | read entry back | no |
| `9x 02 01 idx <22 B>` | 27 | write entry (layout 5.2), set in-use = 1 | if newly in use or any field changed |
| `9x 02 02 idx` | 5 | in-use = 1 | if it was 0 |
| `9x 02 03 idx` | 5 | in-use = 0 | if it was 1 |
| `9x 02 04 idx` | 5 | zero the entry | always |
| `9x 02 05 xx` | 5 | zero all 64 entries, clear the +0x34 flag | always |

Sub 1/4 set the "table written by firmware" flag (+0x34). `9E 02 ..` applies to all three
channels, each rebuilt separately. Rebuild = in this order, on the PCAN thread, before the ICD
reply:
1. `FUN_00466070`: **PDUSetUniqueRespIdTable on CLL[0] with the WHOLE table** (every in-use
   entry, in index order).
2. `FUN_00465b00`: UPDATEPARAM CLL[0], wait FINISHED; UPDATEPARAM CLL[1], wait FINISHED.
3. `FUN_00465d90`: Cancel the receive CoP (wait CANCELLED), start a new one with one expected
   response per in-use entry (wait EXECUTING). 0 entries = no new receive CoP.

All three steps are skipped (silently) while the channel has no CLLs; the table is then just
stored and sent at the next create.

### 5.2 Mailbox entry (0x1C bytes at +0x68 + idx*0x1C) and the ICD record

ICD `9x 02 01` bytes (big endian words), matching `SOFTCANDI.md` 6:
```
[0]cmd [1]02 [2]01 [3]idx [4..7]respId [8..11]mask [12..15]flags4 [16..17]u16 [18..21]reqId [22..25]x4 [26]ck
```
Stored as: +0x00 respId (u32; bits 0-28 id, bit 29 = 29-bit), +0x04 mask (1 = must match), +0x08..
+0x0B flags4 (byte +0x0A is the flag byte below), +0x0C u16, +0x10 reqId (bit 31 = has a paired
request, bit 30 = request uses extended addressing, bit 29 = 29-bit), +0x14 4 bytes (not used by
D-PDU), +0x18 in-use (u32).

Flag byte +0x0A: bit 7 = **USDT pair** (ISO-TP response, paired with reqId), bit 6 = response uses
extended addressing, bit 5 = apparently 29-bit (only the AUTO filter looks at it; the table uses
the id words' bit 29), bit 4 = copied into request format bit 4, low bits = apparently the
protocol (1 OBD, 2 GMLAN, 3 J1939 in the defaults). Firmware GMLAN records: **0x92** (pair) and
**0x02** (plain UUDT). A USDT entry's mask must be full (0x7FF / 0x1FFFFFFF); otherwise Tech2Win
logs "PCAN: found invalid mask for 11Bit/29Bit USDT CAN-ID" and forces it.

### 5.3 PDUSetUniqueRespIdTable structure (`FUN_00466070` 65945)

```
PDU_UNIQUE_RESP_ID_TABLE_ITEM { ItemType 0x1700, NumEntries N, pUniqueData -> PDU_ECU_UNIQUE_RESP_DATA[N] }
PDU_ECU_UNIQUE_RESP_DATA      { UniqueRespIdentifier = mailbox idx (0..63), NumParamItems 9, pParams -> PDU_PARAM_ITEM[9] }
PDU_PARAM_ITEM[k]             { 0x1200, id_k, 0x105 UNUM32, class 6 PDU_PC_UNIQUE_ID, -> u32 }
```
Param order (always all 9):

| k | ComParam | USDT entry (flag bit 7) | UUDT entry |
|---|---|---|---|
| 0 | CP_CanPhysReqId | reqId & 0x1FFFFFFF | 0 |
| 1 | CP_CanPhysReqFormat | 4.3 (0x35 / 0x25) | 0 |
| 2 | CP_CanPhysReqExtAddr | 0 | 0 |
| 3 | CP_CanRespUSDTId | respId & 0x1FFFFFFF | 0 |
| 4 | CP_CanRespUSDTFormat | 4.3 (0x05) | 0 |
| 5 | CP_CanRespUSDTExtAddr | 0 | 0 |
| 6 | CP_CanRespUUDTId | 0 | one matching id |
| 7 | CP_CanRespUUDTFormat | 0 | 4.3 (0x00) |
| 8 | CP_CanRespUUDTExtAddr | 0 | 0 |

- A UUDT entry with don't-care bits in its mask is **expanded**: one table row per matching id
  (2^n rows, `FUN_00466510`/`FUN_00466570`), all with the same UniqueRespIdentifier. With the
  firmware's mask 0x0FFF there is nothing to expand, so it is 1 row per in-use entry.
- So N = in-use entries (64 max in practice), UniqueRespIdentifier = mailbox index, unique per
  entry, a UUDT/USDT pair uses two consecutive indexes.
- Variant 0, 4 or > 5: "CANDI: Protocol not supported", nothing sent, returns 0xA (fails a create).
- Set on **CLL[0] only**. Before Connect at create, otherwise followed by UPDATEPARAM.
- Memory is freed right after the call: copy everything.
- **Functional addressing is not in the table.** GMLAN AllNodes (0x101 + ext addr 0xFE) only exists
  as a TX (4.5). The answers come back on the physical response ids that are in the table.

### 5.4 The permanent receive CoP on CLL[0] (`FUN_00465d90` 65816)

```
PDUStartComPrimitive(hMod, hCLL[0], SENDRECV 0x8004, 0, NULL,
    &{Time 0, NumSendCycles 0, NumReceiveCycles -1, TempParamUpdate 0, TxFlag {0, NULL}, N, ER[]},
    pCoPTag &tag[ch][0], &hCoP_rx)                                    -> wait EXECUTING
ER[j] = {ResponseType 0, AcceptanceId = mailbox idx, NumMaskPatternBytes 4,
         mask    = BE32( 11-bit: (entry.mask & 0x7FF) | 0x1FFFF800 ; 29-bit: entry.mask & 0x1FFFFFFF ),
         pattern = BE32( entry.respId & 0x1FFFFFFF ), NumUniqueRespIds 0, NULL}
```
One ER per in-use entry (USDT and UUDT alike), in index order. Mask/pattern compare the first 4
RESULT bytes = the RawMode CAN id (big endian). With mask 0x0FFF the mask is `1F FF FF FF`, i.e.
exact id. Link to the data path: the firmware's mailbox index for a received frame is taken from
**`PDU_RESULT_DATA.AcceptanceId`** (offset +0x0C, read in `FUN_00464750` 64691), not from
UniqueRespIdentifier. When two entries match the same id (the tool reprograms index 0/1 while 0x3C..
for the same ids is still loaded), the **lowest index must win** = first matching ER in array
order, which is also the ISO rule.

Defaults (ResetBus `FUN_00468460` 67586-67766, only while the +0x34 flag is clear, i.e. before the
firmware's first `9x 02 01/04`):

| variant | entries |
|---|---|
| 1 OBD | idx 0-7: USDT req 0x7E0+i / resp 0x7E8+i, flags 0x81 (ReqFormat 0x25) |
| 2 GMLAN | idx 0-31: UUDT 0x540-0x55F (flags 0x02); idx 32-63: USDT req 0x240-0x25F / resp 0x640-0x65F (flags 0x82, ReqFormat 0x25) |
| 3 J1939 | 6 entries around 0x18ECxxxx / 0x18FExxxx (flags 0xA3/0x23), not relevant for SAAB |
| 5 | none |

So the first create of a session sends a 64-entry GMLAN table and a 64-ER receive, which the
firmware then clears with `9E 02 05` (8.1).

### 5.5 IoCtl filters on CLL[0] (`FUN_00465bf0` 65729), from ICD `9x 01`

Called at create and when the firmware's `9x 01 01` changed the values (no UPDATEPARAM after it):
```
PDUIoCtl(hMod, hCLL[0], PDU_IOCTL_CLEAR_MSG_FILTER, NULL, NULL)
PDUIoCtl(hMod, hCLL[0], PDU_IOCTL_START_MSG_FILTER, &{0x1003, -> {NumFilterEntries n, -> f[]}}, NULL)
f = {FilterType, FilterNumber, FilterCompareSize 4, Mask[12], Pattern[12]}  (bytes 4..11 zero)
```
- Pair 0 used if `(code0 | mask0) & 0x1FFFFFFF`, pair 1 if `(code1 | mask1) & 0x1FFFFFFF`:
  each becomes `{PASS 1, FilterNumber 1/2, 4, Mask = mask bytes, Pattern = pattern bytes}`.
- Neither used: one `{BLOCK 2, FilterNumber 1, 4, Mask 00 00 00 00, Pattern 00 00 00 00}` =
  **block everything**.
- ICD `9x 01 01` body (BE words at byte 3): code0, mask0, code1, mask1, u16, u16. Conversion
  (`FUN_00467390` 66830-66853): pattern = code with byte 0 `& 0x1F`; D-PDU mask = **inverted**
  firmware mask (firmware 1 = don't care), byte 0 = `~(b0 & 0x1F)`. Example: code `40 00 00 00`,
  mask `C0 00 07 FF` -> Pattern `00 00 00 00`, Mask `FF FF F8 00` = pass every 11-bit id.
- `9x 01 11` = AUTO (`FUN_00464940` 64859): derive two code/mask pairs from the mailbox table,
  logs "AUTO NOT TESTED!". Not seen in the SAAB session.
- Defaults while +0x30 is clear: variant 1 = pass 0x7E8-0x7EF (Mask `FF FF FF F8`, Pattern
  `00 00 07 E8`); variant 2 = pass all 11-bit (`FF FF F8 00` / 0); variant 3 = mask `FF FF FF FF`,
  pattern 0 (only id 0, looks like a Tech2Win slip, J1939 only); variant 5 = block all.
- Never on CLL[1].

----------------------------------------------------------------------------------------------

## 6. SW-CAN (item 5)

### 6.1 High-voltage wakeup: ICD `88 0E` -> `FUN_004670e0` (66636) -> `FUN_004653c0` (65329)

ch0 only (`8A/8C/8E 0E` = 0x83), length 3, needs CLL[1] (else 0xD0). Reply 0x40, then on CLL[1]:
```
SetComParam(hCLL[1], CP_SwCan_HighVoltage,     class 3, UNUM32, 1)
SetComParam(hCLL[1], CP_CanFillerByteHandling, class 3, UNUM32, 0)
PDUStartComPrimitive(hMod, hCLL[1], SENDRECV, 4, -> 00 00 01 00,
    &{Time 500, NumSendCycles 1, NumReceiveCycles 0, TempParamUpdate 1, TxFlag {4, -> 00 00 00 00}, 0, NULL},
    pCoPTag &tag[0][1], &h)                 -> wait FINISHED on CLL 1
```
Payload = RawMode CAN id 0x100 (big endian), zero data bytes: the GMLAN HV wakeup frame, DLC 0
(that's why padding is switched off for it). Result: 0x00 via `80 05`, or 0x87 if a call failed.
TxFlag has no SW_CAN_HV_TX-style bit; HV is signalled only by the temporary ComParam.

### 6.2 Normal vs HV mode

There is no explicit "back to normal" call. HV and no-padding apply to that one CoP only, through
TempParamUpdate 1 (requirement 0.3). After it the RAW CLL's active buffer is still HV 0 /
padding 1. The firmware waits 500 ms itself after the wakeup (the scenario's `delay(500)` after
"SCEN: HVW").

### 6.3 33.3k vs 83.3k

- ICD `88 04` "Switch To High Speed" is acked and **ignored** by Tech2Win (`FUN_00458940` 56158).
- The only way the D-PDU side changes speed is a bit-timing write `88 0B 01` with new BTRs:
  CP_Baudrate (83333) + CP_TerminationType 4 + the rest of 4.1, UPDATEPARAM x2, receive restart.
- The captured SAAB session never switches. SAAB SPS uses `A5 01` / `A5 03` (not the GMLAN
  high-speed `A5 02`), so 83.3k is not expected on SAAB; if it ever comes it will arrive as 4.2.

----------------------------------------------------------------------------------------------

## 7. Status waits, events, errors (item 6)

### 7.1 Wait primitive: `FUN_00468e60(ch, cll, 0x1301, status)` (67810)

```
loop:
  WaitForSingleObject(callbackEvent[ch*2 + cll], 10000)
  timeout -> log "PCAN: Waiting for special event failed!  bus:%02X group:%08X status:%08X", give up
  drain that CLL (FUN_004682b0), until it reports a match
```
Drain (`FUN_004682b0` 67394, match logic from asm `0x46830a..0x4683f6`):
`PDUGetEventItem(hMod, hCLL[cll], &it)` until non-zero, per item then `PDUDestroyItem`:

| ItemType | action | matches a wait for |
|---|---|---|
| 0x1300 RESULT | `FUN_00464750(ch, pData)` -> firmware RX queue (data path) | type 0x1300 (never used) |
| 0x1301 STATUS | nothing | 0x1301 with `*pData == status` (hCop not checked) |
| 0x1302 ERROR | `ErrorCodeId == 0x101` (TX_ERROR): CANdi status byte = 0x60, +0x15 = 0, if +0x16 set synthesize a TX-done record; else log "PDU_IT_ERROR: bus 0x%02X codeId 0x%08X info 0x%08X" | 0x1302 + code (never used) |
| 0x1303 INFO | nothing | 0x1303 (never used) |

Differences from K-line:
- Per CLL: each CLL has its own event and is drained separately. A STATUS for CLL[1] does not
  satisfy a wait on CLL[0].
- Stale statuses still match (value only), same as K-line.
- RESULTs arriving during a wait are forwarded, not lost.
- The thread is the same one that executes ICD commands, so a 10 s timeout blocks all CAN.

### 7.2 Every wait in the CAN code

| after | CLL | status |
|---|---|---|
| PDUConnect | each | ONLINE 0x8051 |
| PDUDisconnect (even if it failed) | each | OFFLINE 0x8050 |
| STARTCOMM, STOPCOMM (only if the call returned 0) | each | FINISHED 0x8012 |
| UPDATEPARAM | 0 then 1 | FINISHED 0x8012 |
| HV wakeup SENDRECV | 1 | FINISHED 0x8012 |
| receive SENDRECV | 0 | EXECUTING 0x8011 |
| each TX SENDRECV | the TX CLL | EXECUTING 0x8011 |
| Cancel receive CoP (rebuild: only if Cancel returned 0; teardown: always) | 0 | CANCELLED 0x8013 |

Not used on CAN: PDUGetStatus, PDUGetComParam, PDUGetUniqueRespIdTable, IOCTL READ_VBATT (that
one is module level at startup only).

### 7.3 Errors

- **No ERROR item kills a CAN link.** Nothing changes link state; only TX_ERROR (0x101) has a side
  effect (TX-done synthesis towards the firmware, data path). Everything else is a log line.
- ERRORs don't complete waits. Don't send ERROR instead of the expected STATUS.
- Return codes: every failure is logged with `FUN_00461e70`; aborts as in 0.1. Codes 0x10/0x11/
  0x106 also clear the VCI status display (module-level handler, `host_api_kw2000.md` 4.1).

----------------------------------------------------------------------------------------------

## 8. What the SAAB card actually does (item 7)

### 8.1 Captured session (real v148 firmware, `candi_golden.json`, 2003 9-3 Sport T8, pre-flight
to the Engine menu)

After the pod-level `90 03` / `80 09` / `80 0C` / `80 01` / `80 08` (handled by candi_common, no
D-PDU):

| # | ICD | Tech2Win D-PDU effect |
|---|---|---|
| 07-09 | `8A 0B 00`, `8A 0C 00`, `8A 0B 01 C6 5A 00` | MS bit timing = default, only stored (no CLLs yet) |
| 10 | `88 03 02` (+ `80 05` polls) | create SW: SAE_J2411_SWCAN pin 1, GMLAN, 33333, default table (64) |
| 12 | `8A 03 02` | create MS: DWCAN 3/11, 95238 |
| 14 | `8C 03 02` | create HS: DWCAN 6/14, 500000 |
| 17 | `9E 01 01 40000000 C0000000 0 0 0000 FFFF` | all 3: filters -> BLOCK all |
| 18 | `9E 02 05 00` | all 3: empty table, UPDATEPARAM x2, receive cancelled |
| 19, 20 | `98 01 01 40000000 C00007FF ..`, `9C 01 01` same | SW, HS: PASS all 11-bit ids. MS stays BLOCK |
| 21-100 | 50 x `98 02 01`, 30 x `9C 02 01` | 80 rebuilds (5.1) |
| 101 | `88 0E` | SW-CAN HV wakeup |

Resulting state (simulated from the corpus with the rules above):
- **SW-CAN (ch0)**: 50 entries, UniqueRespIdentifier = idx: even idx = UUDT 0x541..0x54F, 0x553,
  0x557..0x55F; odd idx+1 = USDT req 0x24x / resp 0x64x for the same x (formats 0x35 / 0x05).
- **MS-CAN (ch1)**: created, connected, COMM_STARTED, BLOCK-all filter, no table, no receive CoP.
  Never used by the NG 9-3.
- **HS-CAN (ch2)**: 30 entries: pairs for 0x43, 0x44, 0x45, 0x47, 0x49, 0x4F, 0x51..0x57 (UUDT 0x5xx
  + USDT 0x2xx/0x6xx), plus idx 60/61 = UUDT 0x5E8 + USDT 0x7E0/0x7E8 and 62/63 = UUDT 0x5E9 +
  USDT 0x7E1/0x7E9 (T8).
- All 11-bit, variant 2 everywhere, no extended addressing, no 29-bit, HS pins 6/14.

### 8.2 Per vehicle

- **9-3 NG (2003+)**: GMLAN on SW-CAN (I-bus, 33.3k, pin 1) and HS-CAN (P-bus, 500k, 6/14), both
  live at once. MS-CAN created but blocked. This is the session above.
- **9-5 NG (2010+)**: no capture. Same Tech2Win code path (variant 2, GMLAN); the card never
  raises SIGNAL 0x5300, so HS stays 6/14. Expect the same three creates with a different mailbox
  set. Unverified.
- **OG 9-3 / OG 9-5**: K-line (KW2000 / KW82). The CANdi pod link (0x1A00) may still be opened
  for identify/POST, but without `8x 03` no CLL is ever created.

### 8.3 Load

That one pre-flight is ~500 setup calls (not counting GetEventItem/DestroyItem): 3 creates
(6 Create, 45 SetComParam, 6 IoCtl, 3 SetUniqueRespIdTable, 6 Connect, 6 STARTCOMM, 3 receive),
10 more filter IoCtls, then ~83 rebuilds of (SetUniqueRespIdTable + 2 UPDATEPARAM + Cancel +
SENDRECV). About 340 status waits in total, each ICD reply held until its waits are done. Data screens add more `9x 02 01`
rewrites (the tool reprograms idx 0/1 for Read Values).

----------------------------------------------------------------------------------------------

## 9. Notes for a J2534 v04.04 (+J2534-2) backend (advice, not decomp)

- **One physical J2534 channel per D-PDU channel (bus), shared by both CLLs.** Raw CAN on it,
  ISO-TP done in the DLL using the table pairs (on a First Frame from CanRespUSDTId send FC to
  CanPhysReqId; segment TX on CLL[0]). Reason: the session needs 25 ISO-TP pairs on SW-CAN alone,
  most adapters cap FLOW_CONTROL_FILTERs at ~10 per channel.
  - HS (6/14): `CAN` (5), or `CAN_PS` (0x8004) + `J1962_PINS` 0x060E. DATA_RATE = CP_Baudrate.
  - SW (1): `SW_CAN_PS` (0x8008) + `J1962_PINS` 0x0100, 33333. Wakeup frame with TxFlags
    `SW_CAN_HV_TX` (0x400), id 0x100, 4 data bytes (id only). 83333 -> IOCTL `SW_CAN_HS` (0x8000),
    back -> `SW_CAN_NS` (0x8001).
  - MS (3/11): `CAN_PS` + `J1962_PINS` 0x030B at 95238 if the adapter can. If it can't, make it a
    **virtual link**: succeed every call, post every status, never deliver anything. SAAB never
    puts a mailbox on it, and a failed create is reported to the firmware as an error (0xD0).
- One J2534 PASS filter mask 0 / pattern 0 on the raw channel; do the D-PDU filter (5.5), the
  ER matching (5.4) and the table lookup in software. CLEAR/START_MSG_FILTER then never touch the
  adapter.
- Keep the physical channel open across D-PDU Destroy/Create when baud and pins stay the same; the
  firmware re-creates channels.
- Ignore CP_BitSamplePoint 80 / CP_SyncJumpWidth 15 (or map to `BIT_SAMPLE_POINT` 0x17 /
  `SYNC_JUMP_WIDTH` 0x18), CP_SamplesPerBit, CP_P3Phys, CP_TerminationType (except: 4 on SW-CAN
  goes with 83.3k), CP_RequestAddrMode.
- Padding: CP_CanFillerByteHandling 1 = pad to DLC 8 with 0x00 (the real CANdi does exactly that).
- `Time` (500 on TX and wakeup, 100 on STARTCOMM/STOPCOMM) is the ISO "cycle time" field and
  meaningless with NumSendCycles 1. If you want a TX timeout, 500 ms is the obvious reading, and a
  frame that can't go out is reported as ERROR 0x101 (Tech2Win turns that into a TX-done for the
  firmware).

----------------------------------------------------------------------------------------------

## 10. Open points

- CP_Can*Format bits 4/5 (0x10, 0x20) and CP_TerminationType values 2/4: MDI MDF meaning not
  verified (no MDF or ISO text on disk). Decoding bits 0-3 is enough for a J2534 bridge.
- 9-5 NG and SPS ICD sequences are not captured. In particular what SPS does after `8E 05`
  (Tech2Win destroys all 6 CLLs; an all-channel TX with no CLLs left gets 0xD0, so SPS must
  re-create with `8x 03` or Tech2Win would fail there too).
- `8x 0F` (RequestAddrMode) and `88 04` were not seen in the capture.
- Whether the firmware's `80 05` polling gives up before a slow create finishes: unknown, keep
  create well under a second.
