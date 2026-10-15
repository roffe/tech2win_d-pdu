# Tech2Win host side of the D-PDU API: CAN data path (pdu_can.c)

What GM Tech2Win (`emulator.exe`, 32-bit, build 2019-12-22) does with a D-PDU API DLL to move CAN
traffic between the emulated Tech2 firmware (which talks CANdi ICD) and the vehicle. Scope here is
the DATA PATH: transmit, receive, ISO-TP, TX confirmations, statuses, errors, concurrency, timing.
Link setup (CLL creation, pins, bus ComParams, IOCTL filters, UniqueRespIdTable, SW-CAN wakeup
details) is covered separately; it is only touched here where the data path depends on it.

Sources:
- `RE/tech2win/out/decomp_all.c` (line numbers below are lines in this file)
- `RE/tech2win/emulator.exe`, disassembled with capstone where Ghidra lost parameters or never made
  a function (worker thread `0x468c30`, drain `0x4682b0`, wait `0x468e60`, poll `0x456ed0`)
- `dpdu/dpdu-passthru/driver/pdu_api.h` (2.2.0 layouts)
- CANdi ICD cross-check: `t2emu/softcandi/softcandi.go`, `Tech3/SOFTCANDI.md`, bench corpus
  `t2emu/tech2/testdata/candi_golden.json` (bytes captured from the real candiapp firmware)

Conventions as in `host_api_kw2000.md` (cyc = 16777 cyc/ms, UNDEF = 0xFFFFFFFF, hMod =
`*(DAT_00d5dca0 + 0x208)` in this file). "ch" = CANdi channel index 0/1/2. Flag words
(TxFlag/RxFlag) are written as the 32-bit value with **byte 0 = most significant**, i.e.
`0x00000040` = bytes `00 00 00 40`. ICD frames are written with their trailing checksum byte (two's
complement, whole frame sums to 0 mod 256).

----------------------------------------------------------------------------------------------

## 0. Hard requirements (break any of these and Tech2Win misbehaves)

1. **Every CAN RESULT needs `RxFlag.pFlagData` pointing at >= 4 bytes.** `FUN_00464750` reads
   `pFlagData[2]` and `pFlagData[3]` unconditionally (asm `0x46475a`, `0x4647c3`, `0x4647f3`,
   `0x464800`), never `NumFlagBytes`. NULL = access violation in Tech2Win. (Current `dll.cpp`
   `postResult` leaves RxFlag zeroed: fine for K-line, crashes CAN.)
2. **`NumDataBytes >= 4`** for every RESULT that is not a TX indication: Tech2Win copies
   `NumDataBytes - 4` bytes from `pDataBytes + 4`, no check.
3. **`pDataBytes[0..3]` = CAN id, big endian, no flag bits in it.** Then the optional extended
   address byte, then the payload.
4. **`AcceptanceId` = the AcceptanceId of the matched expected response** (= firmware mailbox
   index 0..63, see 3.1). Tech2Win forwards its low byte to the firmware as the record tag.
5. **TX confirmations only come from RESULTs with RxFlag bit `0x00000008` (TX indication).** Tech2Win
   never sets `CP_TransmitIndEnable` on CAN links, so the DLL must report TX indications on CAN CLLs
   by default. Without them SPS programming never gets its `0x77` confirms.
6. **Never emit other indication RESULTs:** no TX echo/loopback (RxFlag bit 0), no first-frame /
   start-of-message (bit 1) results. Tech2Win checks only bits 3/6/7/8, so anything without bit 3 is
   forwarded to the firmware as a received message.
7. **Every TX SENDRECV must produce STATUS `PDU_COPST_EXECUTING` (0x8011)** on its CLL. Tech2Win
   waits for exactly that value (not FINISHED). The wait only gives up after 10 s with no item on
   that CLL, so with RX traffic flowing a missing EXECUTING stalls forever.
8. **The permanent receive CoP (NumReceiveCycles -1) must never finish, time out or error on its
   own**, and `PDUCancelComPrimitive` on it must return 0 and post `PDU_COPST_CANCELLED` (0x8013).
   If Cancel returns non-zero during a re-arm, Tech2Win does not start a new receiver (no RX until
   the next filter change).
9. No P2/P2*/response-pending handling on CAN: the firmware owns all timeouts.
10. RX RESULTs for USDT (ISO-TP) messages: PCI removed, multi-frame reassembled, RxFlag bit
    `0x00000040` set. UUDT frames: all DLC bytes verbatim, bit `0x40` clear (section 3.4).

----------------------------------------------------------------------------------------------

## 1. Architecture

### 1.1 Who talks to whom

```
firmware (api.dwn)                         Tech2Win                                  D-PDU DLL
16550 UART, CANdi mode 0  --ICD cmd-->  FUN_004510f0 frame cut (uart2candi)
                                        FUN_00456bb0 HandleICDCommand (candi_common.c, 54628)
                                          local: 0x08/0x18/0x20/0x30/0x38/0x80/0x90/0xA0 families
                                          sid&0xF8 0x00, 0x28, 0x88 esid 3/5/6/B/C/E/F,
                                          0x98 esid 1/2 -> FUN_004646f0 (64656): copy to
                                          DAT_00a45c48, SetEvent("PCAN_Event_ICD_Message")
                                        PCAN worker thread 0x468c30 ------------------>  all CAN calls
                                          FUN_004668f0 (66334) ICD dispatcher
                                          reply -> DAT_00a46fe0, DAT_00a48378 = 1
                          <--ICD reply-- FUN_00456e10 GetMessage / FUN_004668b0 (66320)
TPU ch15 input <-- DAT_0050d3e8 "attention" line (FUN_00457410 / FUN_00457430)
```

The emulated CPU thread never calls the DLL. It only copies ICD frames in/out and sets events.
There is no CPU freeze anywhere in the CANdi/pdu_can path (no `FUN_00418550`).

### 1.2 PCAN worker thread `0x468c30` (asm only, Ghidra shows it as `LAB_00468c30`)

Started by `FUN_00468170` (67353, CANdi protocol open). Loop until hMod == -1:

```
WaitForMultipleObjects(8, {SW, SW_RAW, MS, MS_RAW, HS, HS_RAW, ICD_Message, Shutdown}, FALSE, 5000)
0..5 -> FUN_004682b0(ch = i/2, cll = i%2, 0, 0)     drain that CLL (no wait match)
6    -> FUN_004668f0(&DAT_00a45c48, &DAT_00a46fe0) handle one ICD command
7    -> FUN_00465270(2), (1), (0)                    close all channels, CloseHandle x8, exit
timeout -> loop
```

Event names (auto reset): `PCAN_CallBackEvent_SW`, `_SW_RAW`, `_MS`, `_MS_RAW`, `_HS`, `_HS_RAW`,
`PCAN_Event_ICD_Message`, `PCAN_Event_Shutdown` (handles `DAT_00a499c8..e4`).

CLL callbacks (`FUN_00468f60` SW, `f80` SW_RAW, `f20` MS, `f40` MS_RAW, `ee0` HS, `f00` HS_RAW,
67832-67901): `__stdcall`, 5 args (`ret 0x14`), `if (eventType == 0x801 && hEvent) SetEvent(hEvent)`.
0x802 ignored. Calling them from any DLL thread is fine.

So **every** D-PDU call for CAN (create, connect, SetComParam, IoCtl, SetUniqueRespIdTable,
StartComPrimitive, Cancel, GetEventItem, DestroyItem, disconnect, destroy) comes from this one
thread, and every ICD command (including the RX poll) is handled on it, one at a time.

### 1.3 Channels and CLLs

Per channel struct, stride 0x768: ch0 SW-CAN at `0xa48390`, ch1 MS-CAN at `0xa48af8`, ch2 HS-CAN at
`0xa49260`. Two CLLs per channel, both created with CllCreateFlag `{4, 80 00 00 00}` (raw mode):

| CLL | index | protocol (variant 2, GMLAN) | used for |
|---|---|---|---|
| `[0]` `+0x00` | 0 | `ISO_15765_3_on_ISO_15765_2` (variant 1: `ISO_15031_5_on_ISO_15765_4`, 3: J1939) | USDT TX, **all RX** (permanent receiver), UniqueRespIdTable, IOCTL filters |
| `[1]` `+0x04` | 1 | `ISO_11898_RAW` | raw single-frame TX (UUDT, the firmware's TesterPresent), SW-CAN wakeup |

Other fields used by the data path:

| offset | name | meaning |
|---|---|---|
| +0x08 | hCoP_rx | handle of the permanent receive SENDRECV (CLL0) |
| +0x0C / +0x10 | cllTag[0] / cllTag[1] | pCllTag / pCoPTag pointers, value 0 |
| +0x14 | variant | link variant from `8x 03 <v>` (1 OBD, 2 GMLAN, 3 J1939, 5 "UDS") |
| +0x16 | flag | `8x 0F <m>`: m != 0 -> fake TX confirm on TX error (5.3) |
| +0x17..+0x1B | txIdBytes | copy of the last TX command's id bytes (+ext addr), echoed in `0x77` |
| +0x1C | txDoneSent | latch: one `0x77` per TX command |
| +0x68.. | 64 x 0x1C | firmware mailbox table (`9x 02 01`), source of expected responses |

Global ComParam caches (not per channel!): `DAT_00d5dc98` = current CP_CanFillerByteHandling (set
to `variant != 5` at create), `DAT_00d5dc9c` = CP_RequestAddrMode as Tech2Win believes it (1, never
updated).

### 1.4 ICD families relevant to data (`FUN_00456bb0` 54628, `FUN_004668f0` 66334)

| ICD cmd (`sid & 0xF8`) | Tech2Win name | handled by | data-path effect |
|---|---|---|---|
| `00..07` | transmit message | `FUN_00467c20` (67147) | **section 2** |
| `08` | "Transmit Once" | `FUN_00458ea0` | NOT IMPLEMENTED, replies status 0, sends nothing |
| `18` | "Formatted Transmit" | `FUN_00458e70` | NOT IMPLEMENTED, status 0 |
| `20` | "ECU Reprogramming Transmit" | `FUN_00458e40` | NOT IMPLEMENTED, status 0 |
| `28..2F` | Link Receive Group | `FUN_00467aa0` (67092) | only `2E` valid = RX poll (**3.5**), others 0x83 |
| `30` | "Receive Message With TimeStamp" | `FUN_00458c20` | NOT IMPLEMENTED, replies `31 00` |
| `38` | "Resend Last Message" | `FUN_00458c50` | replays the last saved receive record locally, header `0x39 \| ch`, no DLL call |
| `88..8F` esid 3/5/6/B/C/E/F | Link Control Group | pdu_can | open/close/flush/bitrate/NL timing/wakeup/mode |
| `98..9F` esid 1/2 | Link Filter Group | pdu_can | acceptance filter / mailbox table -> re-arm RX |
| `A0..A7` | Link Scheduler Group (periodic) | `FUN_00458ce0` (56334) | NOT IMPLEMENTED (Read/Write Scheduler Entry, Start/Stop Periodic Entry all reply 0, do nothing) |

Low bits of the command byte: `cmd & 6` = channel (0 SW, 2 MS, 4 HS, 6 all), bit 0 = reply.
Channel index in Tech2Win = `cmd >> 1 & 3` (3 = all).

----------------------------------------------------------------------------------------------

## 2. TX

### 2.1 ICD transmit command (firmware -> Tech2Win)

```
[0x00 | ch<<1] [flags] [idHi] [idLo] ([id2] [id3] if 29-bit) [ext?] [payload ...] [cks]
```

| byte/bit | Tech2Win meaning (`FUN_00467c20`) | firmware/candiapp meaning |
|---|---|---|
| `flags` bit 7 | pad: TxFlag `0x40` + CP_CanFillerByteHandling | "send a transmit confirmation"; firmware sends `0xC0 \| mailboxIdx`, so always set in practice |
| `flags` bits 6..0 | ignored | `0x40` + mailbox index, informational |
| `idHi` bit 7 | **1 = USDT: CLL[0] ISO 15765, Tech2Win passes payload only; 0 = CLL[1] ISO_11898_RAW** | framing `10` = module adds PCI / segments; `00` = raw |
| `idHi` bit 6 | extended address byte present (first byte after the id) | framing `11` = length inserted after byte 0 |
| `idHi` bit 5 | 29-bit id, 4 id bytes follow | |
| `idHi` bits 4..0 | id bits 12..8 (11-bit) / 28..24 (29-bit) | |

Checks (67196, 67234-67239): checksum must be valid else reply status `0x81`; 11-bit needs total
length >= 6 (>= 7 with ext), 29-bit >= 8 (>= 9), i.e. at least 1 payload byte; max total length
`id-header + 0x1005`; else `0x83`. Channel without CLL[0] -> `0xD0`; CLL[0] but no CLL[1] -> `0xD0`.

### 2.2 What Tech2Win does with it (67228-67347)

For each channel that matches (`ch == sel || sel == 3`) and has the selected CLL:

1. `cll = (idHi & 0x80) ? 0 : 1`.
2. `memcpy(chan.txIdBytes, &cmd[2], (29bit ? 4 : 2) + ext)` (kept for the `0x77` record).
3. Build CoP data: `[id BE 4 bytes][ext?][payload]`. 11-bit: `00 00 (idHi & 0x1F) idLo`; 29-bit:
   `(idHi & 0x1F) id2 id3 id4`. `CoPDataSize = 4 + ext + payloadLen`.
4. If `ext + 1 != DAT_00d5dc9c (1)`, i.e. **ext addr present**: `PDUSetComParam(CLL,
   CP_RequestAddrMode = 2, class 3 COM)` (log line 0x819) and TempParamUpdate = 1.
5. If `(flags & 0x80 != 0) != DAT_00d5dc98`: `PDUSetComParam(CLL, CP_CanFillerByteHandling =
   that bit, class 3)` (0x823) and TempParamUpdate = 1. Neither cache is updated afterwards, so
   these temporary sets repeat on every such TX.
6. `chan.txDoneSent = 0`.
7. `PDUStartComPrimitive` (table 2.3); failure -> log 0x845, reply status `0xD0`, stop.
8. `FUN_00468e60(ch, cll, 0x1301, 0x8011)`: wait STATUS EXECUTING on that CLL.

After all channels: reply `[cmd+1] [00] [cks]` (e.g. `05 00 FB`). The reply is only built after the
DLL reported EXECUTING for every channel used.

### 2.3 PDUStartComPrimitive for a TX

| field | value |
|---|---|
| hCLL | `chan.cll[cll]` (CLL0 ISO 15765 if idHi bit 7, else CLL1 ISO_11898_RAW) |
| CoPType | `0x8004` SENDRECV |
| CoPDataSize / pCoPData | `4 + ext + n` / `[id 4 BE][ext?][payload]` (stack buffer, copy it) |
| Time | **500** (no meaning with NumSendCycles 1) |
| NumSendCycles | **1** |
| NumReceiveCycles | **0** (send only; answers come in on the permanent receiver) |
| TempParamUpdate | 1 if step 4 or 5 did a SetComParam just before, else 0 |
| TxFlag | `{4, -> 4 bytes}`, see 2.4 |
| NumPossibleExpectedResponses / pExpectedResponseArray | **0 / NULL** |
| pCoPTag | `&chan.cllTag[cll]` (`0xa4839c + ch*0x768 + cll*4`, value 0). Never read back in the CAN path |
| phCoP | stack local, discarded. The TX CoP is never cancelled or queried |

### 2.4 TxFlag bytes Tech2Win sets (67306-67315)

| bit | bytes | set when | meaning (same values as J2534 TxFlags) |
|---|---|---|---|
| `0x00000100` | `00 00 01 00` | idHi bit 5 | CAN_29BIT_ID |
| `0x00000080` | `00 00 00 80` | idHi bit 6 | ISO15765_ADDR_TYPE: byte after the 4-byte id is the extended address |
| `0x00000040` | `00 00 00 40` | `flags` bit 7 | ISO15765_FRAME_PAD: pad frames to 8 bytes |

Typical: `00 00 00 40` (11-bit physical, padded), `00 00 00 C0` (functional with ext addr FE,
padded). Nothing else is ever set (no ENABLE_EXTRA_INFO, no TX-indication request bit).

### 2.5 Cases

| case | ICD from firmware | CLL | PDU payload | TxFlag | extra |
|---|---|---|---|---|---|
| physical USDT (GMLAN/T8 diag) | `04 FD 87 E0 1A 90 EE` | HS CLL0 | `00 00 07 E0 1A 90` | `00 00 00 40` | DLL adds PCI, segments if > 7 bytes |
| functional USDT (GMLAN AllNodes, ext addr FE) | `06 80 C1 01 FE 1A B0 F0` | CLL0 of every open channel | `00 00 01 01 FE 1A B0` | `00 00 00 C0` | CP_RequestAddrMode = 2 + TempParamUpdate 1 each time |
| raw / UUDT single frame | `04 C0 01 01 FE 01 3E FD` (firmware's own TesterPresent) | HS CLL1 | `00 00 01 01 FE 01 3E` | `00 00 00 40` | frame data verbatim, firmware built PCI/ext itself |
| 29-bit | idHi bit 5 set, 4 id bytes | as above | `id BE` with top 3 bits cleared | `+ 00 00 01 00` | J1939 variant only, nothing SAAB |
| all channels (`06`) | | every channel with that CLL | same | same | sequential: StartComPrimitive + wait EXECUTING per channel |

Expected wire frames (from the real CANdi, which always sends DLC 8 zero padded): `7E0 02 1A 90 00
00 00 00 00`, `101 FE 02 1A B0 00 00 00 00`, `101 FE 01 3E 00 00 00 00 00`.

USDT payload is the bare service data, **no PCI byte**: the MVCI/DLL adds the length byte (or FF/CF
headers) and, with ext addressing, puts the ext addr before the PCI.

### 2.6 Periodic messages / TesterPresent

- No CoP in pdu_can ever has NumSendCycles > 1 or -1. Nothing cyclic is delegated to the DLL.
- `CP_TesterPresentHandling`, `CP_TesterPresent*` are never set on CAN CLLs (all hits for
  `DAT_0051e068` are K-line/Class2 files). The DLL default for CAN must be "no tester present".
- The CANdi scheduler family `A0..A7` (periodic entries) is acknowledged with status 0 and does
  nothing (`FUN_00458ce0` and helpers, 56334-56410).
- The firmware sends GMLAN TesterPresent itself as an ordinary raw transmit, `04 C0 01 01 FE 01 3E`
  -> id 0x101 `FE 01 3E` on CLL1, roughly every 2 s (t2emu vehicle sim observation), with a `0x77`
  confirm like any other TX. Modules do not answer the AllNodes `3E`.

### 2.7 SW-CAN high-voltage wakeup (only TX done outside the ICD transmit command)

`FUN_004653c0` (65329), ICD `88 0E` on ch0: SetComParam on ch0 CLL1 `CP_SwCan_HighVoltage = 1`,
`CP_CanFillerByteHandling = 0` (class 3), then SENDRECV CLL1, data `00 00 01 00` (id 0x100, no data
bytes), ctrl `{Time 500, NumSend 1, NumRecv 0, TempParamUpdate 1, TxFlag {4, 00 00 00 00}, 0,
NULL}`, pCoPTag `&cllTag[0][1]`, then **wait FINISHED** (0x8012) on ch0 CLL1. Setup notes cover
the meaning; data-path wise: it is a DLC-0 frame, and a TX indication for it would produce a stale
`0x77` (3.6), so don't report one for CoPs whose TxFlag has no `0x40`.

----------------------------------------------------------------------------------------------

## 3. RX

### 3.1 The permanent receive SENDRECV (`FUN_00465d90`, 65816)

Started at link open (`FUN_00464c20`) and re-started after every effective change of the mailbox
table (`9x 02`, `FUN_00467670` 67074-67077: SetUniqueRespIdTable, UPDATEPARAM x2, then this) and of
the bitrate (`8x 0B 01`, `FUN_00466c00`). The acceptance filter (`9x 01`) only redoes the IOCTL
filters (`FUN_00465bf0`), and `8x 0F` only does CP_RequestAddrMode + UPDATEPARAM; neither re-arms.

```
if (hCoP_rx) { r = PDUCancelComPrimitive(hMod, CLL0, hCoP_rx);
               if (r) { log 0xd0c; return; }          // no new receiver!
               wait STATUS 0x8013 CANCELLED on CLL0; hCoP_rx = 0; }
n = number of mailbox entries with state == 1 (64 max)
if (n == 0) return;                                   // nothing listening at all
PDUStartComPrimitive(hMod, CLL0, 0x8004, 0, NULL, &ctrl, &cllTag[0], &hCoP_rx)
wait STATUS 0x8011 EXECUTING on CLL0
```

| ctrl field | value |
|---|---|
| Time | 0 |
| NumSendCycles | 0 |
| NumReceiveCycles | **-1** |
| TempParamUpdate | 0 |
| TxFlag | `{0, NULL}` |
| NumPossibleExpectedResponses | n |
| pExpectedResponseArray | n entries of 0x1C bytes, in mailbox index order |

Each expected response (65889-65919), for mailbox entry `i` (`idA` = entry+0, `mask` = entry+4):

| field | value |
|---|---|
| ResponseType | 0 |
| AcceptanceId | **i** (the mailbox index, gaps kept: inactive slots are skipped but not renumbered) |
| NumMaskPatternBytes | 4 |
| pMaskData | 11-bit: BE32(`(mask & 0x7FF) \| 0x1FFFF800`); 29-bit (`idA & 0x20000000`): BE32(`mask & 0x1FFFFFFF`) |
| pPatternData | BE32(`idA & 0x1FFFFFFF`) |
| NumUniqueRespIds / pUniqueRespIds | 0 / NULL |

So the match is on the first 4 result bytes = the CAN id. With the usual mask `0x0FFF` the mask is
`1F FF FF FF`: exact id. ISO rule applies: first matching entry in array order wins, i.e. **lowest
mailbox index wins**. The firmware relies on that (Read Values re-programs indices 0/1 while the
pre-flight's 0x3C-0x3F entries for the same ids are still loaded and expects tag 00; see
SOFTCANDI.md section 6).

Bench mailbox record and what it becomes:
`9C 02 01 3D 00 00 07 E8 00 00 0F FF 00 00 92 FF 00 00 80 00 07 E0 00 00 00 00 2F` -> entry 0x3D,
idA 0x7E8, mask 0xFFF, flags 0x92 (bit 7 = USDT/paired), idB `0x800007E0` (paired request id) ->
expected response `{0, 0x3D, 4, 1F FF FF FF, 00 00 07 E8, 0, NULL}`. USDT vs UUDT per id and the
paired request id reach the DLL through `PDUSetUniqueRespIdTable` on CLL0 (`FUN_00466070`, 65945,
setup notes): USDT entries carry CanRespUSDTId = idA and CanPhysReqId = idB, UniqueRespIdentifier
= mailbox index; UUDT (plain) entries carry CanRespUUDTId.

Note the receiver only exists on CLL0. Nothing ever receives on CLL1, so the only RESULTs on CLL1
are TX indications.

### 3.2 Event drain and what is read (`FUN_004682b0`, 67394; real signature from asm)

`int drain(ch, cll, waitType, waitValue)`: `PDUGetEventItem(hMod, chan.cll[cll], &it)` until
non-zero, `PDUDestroyItem` each, returns 1 if an item matched the wait:

| ItemType | read | action | matches wait if |
|---|---|---|---|
| 0x1300 RESULT | `pData` (+0x10) | `FUN_00464750(ch, pData)` | waitType == 0x1300 |
| 0x1301 STATUS | `*(UNUM32*)pData` | nothing | waitType == 0x1301 && value == waitValue |
| 0x1302 ERROR | `ErrorCodeId`, `ExtraErrorInfoId` | 5.3 | waitType == 0x1302 && ErrorCodeId == waitValue (never used) |
| 0x1303 INFO | | nothing | waitType == 0x1303 |

Never read from the event item: `hCop`, `pCoPTag`, `Timestamp`. Never read from PDU_RESULT_DATA:
`RxFlag.NumFlagBytes` (+0x00), `UniqueRespIdentifier` (+0x08), `TimestampFlags` (+0x10/+0x14),
`TxMsgDoneTimestamp` (+0x18), `StartMsgTimestamp` (+0x1C), `pExtraInfo` (+0x20).
Read: `RxFlag.pFlagData` (+0x04), `AcceptanceId` (+0x0C, low byte), `NumDataBytes` (+0x24),
`pDataBytes` (+0x28).

### 3.3 RESULT -> CANdi record (`FUN_00464750`, 64674; asm 0x464750-0x464895)

```
f = RxFlag.pFlagData
if (f[3] & 0x08) { FUN_00466790(ch); return; }          // TX indication, data ignored (3.6)
slot = per-channel ring write slot; if busy: log "MDI queue full!!! Discarding message." (0xed6)
e[0] = (UNUM8)AcceptanceId
if (f[2] & 0x01) { e[1..4] = pDataBytes[0..3]; e[1] |= 0x20; hdr = 5; }   // 29-bit
else             { e[1..2] = pDataBytes[2..3];            hdr = 3; }      // 11-bit
if (f[3] & 0x40) e[1] |= 0x80                              // USDT marker
if (f[3] & 0x80) e[1] |= 0x40                              // ext addr present
memcpy(e + hdr, pDataBytes + 4, NumDataBytes - 4)          // ext addr (if any) + payload
len = hdr + NumDataBytes - 4; queue; raise attention line (FUN_00457410 -> DAT_0050d3e8 = 1)
```

RxFlag bits Tech2Win looks at, nothing else:

| bit | bytes | effect |
|---|---|---|
| `0x00000008` | `00 00 00 08` | TX indication -> `0x77` confirm, result data ignored |
| `0x00000040` | `00 00 00 40` | ICD id byte bit 7 (`0x80`) |
| `0x00000080` | `00 00 00 80` | ICD id byte bit 6 (`0x40`), ext addr byte is `pDataBytes[4]` |
| `0x00000100` | `00 00 01 00` | 29-bit: 4 id bytes, ICD id byte bit 5 (`0x20`) |

The ISO 22900-2 name of RxFlag bit 6 is not confirmed from the decomp. What the firmware needs in
that ICD bit is known from candiapp (bench, SOFTCANDI.md section 5): **`0x80` set = record from a
paired (USDT, ISO-TP) mailbox, PCI stripped / reassembled; clear = plain mailbox, raw frame bytes**.
So the DLL sets RxFlag `0x40` exactly on USDT results.

### 3.4 What the DLL must deliver per received message

| kind | how the DLL knows | RESULT pDataBytes | RxFlag | AcceptanceId |
|---|---|---|---|---|
| USDT (ISO-TP, e.g. 0x7E8, 0x6xx GMLAN) | id is a CanRespUSDTId (mailbox flags bit 7) | `id BE` + [ext] + reassembled payload, **no PCI** | `00 00 00 40` (+`80` ext, +`01 00` 29-bit) | first matching expected response |
| UUDT (e.g. 0x5E8 DPID stream, 0x5xx GMLAN) | id is a CanRespUUDTId | `id BE` + **all DLC data bytes** as on the wire | `00 00 00 00` (+29-bit) | first matching expected response |
| TX indication | one per TX SENDRECV, after it went out | anything (ignored) | `00 00 00 08` | ignored |

Frames that match no expected response (or are blocked by the IOCTL filters, setup notes) are
dropped. Frames are delivered only while a receive CoP is executing; see 6.3 for the re-arm gap.

### 3.5 Delivery to the firmware: attention line and the `2E` poll

- Ring: per channel 0x105 (261) slots x 0x101A bytes at `0xa499f0 + (ch*0x105 + i)*0x101A`, len u16
  at +0x1018; per-channel write/read index `DAT_00d5d97c[ch]` / `DAT_00d5d976[ch]`; a global
  arrival-order queue of channel numbers, 0x30F (783) bytes at `DAT_00d5d986`, write/read
  `DAT_00d5d984` / `DAT_00d5d982`. Records go out in arrival order across channels. Full = record
  dropped with the "MDI queue full" log.
- Queueing a record sets `DAT_0050d3e8 = 1` (`FUN_0041f560`), which Tech2Win feeds to the firmware
  as the CANdi attention input on TPU channel 15 (`FUN_0041df30`, 15922). The firmware only polls
  while it is set. Emptying the queue clears it (`FUN_00457430`).
- Poll `2E D2` -> `FUN_00467aa0` (67092): one record per poll, reply
  `[0x29 + 2*ch] [record...] [cks]`, e.g. `2D 3D 87 E8 7F 11 01 96`. Empty: `[hdr] D2 [cks]` with
  hdr `2D` if HS CLL0 exists, else `2B` if MS, else `29` (the real CANdi answers `2F D2 FF`).
- `8x 06` (`FUN_00466e50`) flushes that channel's queued records. `8x 03` (open) flushes too.

### 3.6 TX confirmation `0x77` (`FUN_00466790`, 66275)

Called on a TX-indication RESULT from either CLL of channel ch, or on ERROR 0x101 when the `8x 0F`
flag is set (5.3):

```
if (chan.txDoneSent) return;                      // only the first one after each TX command
n = (txIdBytes[0] & 0x20 ? 4 : 2) + (txIdBytes[0] & 0x40 ? 1 : 0)
record = [0x77] [txIdBytes[0..n-1]]               // id bytes exactly as the firmware sent them
queue it like an RX record; chan.txDoneSent = 1
```

To the firmware: `[0x29 + 2*ch] 77 <idHi as sent> <idLo> [ext] [cks]`, e.g. `2D 77 87 E0 F5`,
`2D 77 C1 01 FE 9C`. (The real candiapp sends `2D 77 87 E0 00 00 ..`; the firmware accepts
Tech2Win's shorter form.) An all-channel TX gets one confirm per channel, each with its own
header (SPS clears one subnet bit per confirm).

Consequences for the DLL:
- One TX indication per TX SENDRECV, as soon as the frame / last CF is on the bus. A late one
  lands after `txDoneSent` was reset by the next TX command and is reported with the NEXT command's
  id bytes, and the next command's own confirm is then swallowed.
- Not on FINISHED, not immediately at StartComPrimitive: STATUS items have no side effects.
- TX indications for the DLL's own flow-control frames must not be reported.
- Tech2Win never generates the candiapp TX-error codes (`F4`, `D5`, `E2`, `E4`).
- Suggested rule: report the indication when the CoP's TxFlag has `0x40` (= firmware flags bit 7,
  which candiapp treats as "confirm requested"). That reproduces candiapp exactly and keeps the
  wakeup frame (TxFlag 0) from creating a stale `0x77`. "Always" is what a real MDI does and is
  equivalent for normal traffic, since the firmware always sends flags `0xC0 | idx`.

----------------------------------------------------------------------------------------------

## 4. Multi-frame, ISO-TP, response pending

- **ISO-TP is the DLL's job** (the CLL0 ISO 15765 protocol). Tech2Win hands over complete payloads
  without PCI (up to 0x1005 bytes) and expects complete reassembled payloads without PCI back. It
  never segments, never sends flow control, never sees CAN frames of a USDT message.
- Flow control the DLL sends for a received FF goes to the paired request id: CanPhysReqId of the
  UniqueRespIdTable entry whose CanRespUSDTId matched (bench: 0x7E8 -> FC on 0x7E0, 0x641 -> 0x241).
  candiapp sends `30 00 00 00 00 00 00 00` (BS 0, STmin 0, zero padded).
- ComParams passed: **none of the ISO-TP timing/flow params.** `CP_As`, `CP_Bs`, `CP_Cr`, `CP_Cs` are
  resolved with GetObjectId but never set; `CP_BlockSize`, `CP_STmin`, `CP_CanFillerByte` are not
  even looked up. Only `CP_CanFillerByteHandling` (default `variant != 5`, temp per TX), and
  `CP_P3Phys = 10000` us on CLL0 (setup, `FUN_00465540` 65515) are set. Use DLL defaults: BS 0,
  STmin 0 for our FC, honour the ECU's FC on TX, filler 0x00 (candiapp pads with 0x00).
- `8x 0C` (`FUN_00466ee0`, 66556) stores 5 u16 values per channel (defaults: variant 1 `100 100 100
  150 0`, variant 2/5 `250 150 150 150 0`, variant 3 `100 1250 1250 250 550`, looks like
  network-layer timeouts in ms) and echoes them back on read. They are never passed to the DLL.
- Extended addressing: TX `00 00 01 01 FE 1A B0` with TxFlag `0x80` means wire `FE 02 1A B0 ..`.
  RX with ext addr: RESULT `[id][ext][payload]` + RxFlag `0x80`.
- candiapp does not wait for FC when it segments a TX (sends FF + CFs back to back). A normal ISO-TP
  sender that waits for FC works with real modules; functional requests are single frame.
- **Response pending (`7F xx 78`)**: not handled anywhere in Tech2Win's CAN path, no CP_P2Max /
  CP_P2Star is set on CAN CLLs, the receiver is NumReceiveCycles -1 with no timeout. The DLL just
  forwards `7F xx 78` like any other USDT result; the firmware runs its own P2/P2* timers.

----------------------------------------------------------------------------------------------

## 5. STATUS waits, cancel, errors

### 5.1 The wait (`FUN_00468e60`, 67810; params lost by Ghidra, from asm)

```
wait(ch, cll, type, value):
  loop: if (WaitForSingleObject(event[ch*2 + cll], 10000) != 0)
            { log "PCAN: Waiting for special event failed! bus:.. group:.. status:.."; return; }
        if (drain(ch, cll, type, value)) return;
```

Matching is per CLL queue and by value only (hCop not checked). Any item on that CLL restarts the
10 s window, so a missing STATUS with RX traffic flowing means waiting forever. A stale STATUS with
the same value from an older CoP on the same CLL satisfies a later wait.

### 5.2 STATUS each CoP is waited for

| CoP / call | CLL | waits for | where |
|---|---|---|---|
| TX SENDRECV (NumRecv 0) | the TX CLL | **EXECUTING 0x8011** | 67334 |
| permanent receive SENDRECV (NumRecv -1) | CLL0 | EXECUTING 0x8011 | 65938 |
| Cancel of the receiver | CLL0 | CANCELLED 0x8013 | 65861 (re-arm), 65297 (close) |
| SW-CAN wakeup SENDRECV | ch0 CLL1 | FINISHED 0x8012 | 65384 |
| UPDATEPARAM (ctrl NULL, tag cllTag[cll]) | CLL0 then CLL1 | FINISHED | 65718 |
| STARTCOMM / STOPCOMM (ctrl `{100,0,0,0,{0,NULL},0,NULL}`, 1 byte 00 data ptr, size 0, tag NULL) | CLL0 then CLL1 | FINISHED | 65622, 65679 |
| PDUConnect / PDUDisconnect | each CLL | ONLINE 0x8051 / OFFLINE 0x8050 | 65254, 65308 |

FINISHED after EXECUTING for TX CoPs is harmless. **`PDUGetStatus` is never called by pdu_can**,
`PDUGetComParam` neither.

### 5.3 ERROR items (`FUN_004682b0` 67415-67429)

- `ErrorCodeId == 0x101` (PDU_ERR_EVT_TX_ERROR), from either CLL of channel ch:
  `FUN_004573a0(ch, 2, 0x60)` (byte 2 of the per-channel CANdi status block, init `0x3C`, set to
  `0x60`; looks like an SJA1000 status register, TS|ES; the firmware can read it via `80 01` memory
  read at CANdi address 0x202000/0x201000/0x200000 for ch0/1/2, clear on read, `FUN_00458ed0`), clear
  a "link ok" byte (+0x15, never read), and **if the `8x 0F` flag is set, fake a `0x77` confirm**
  (`FUN_00466790`). Without the flag, the firmware gets no confirm and times out on its own.
- Any other code: log only, `"PDU_IT_ERROR: bus 0x%02X codeId 0x%08X info 0x%08X"` (0x3ca).
- **No ERROR kills a CAN link** (unlike K-line). No per-CoP effect, CoPs keep running.
- Errors do not satisfy any wait (no wait uses type 0x1302), so ERROR without the expected STATUS
  still stalls.

### 5.4 Cancel / stop patterns

- Only the permanent receiver is ever cancelled (re-arm 65855, close 65293). TX CoPs never.
- Re-arm: Cancel non-zero -> no new receiver (8). Close (`FUN_00465270`): result ignored, waits
  CANCELLED anyway (10 s if it never comes), then STOPCOMM both, Disconnect + OFFLINE, unregister
  callback, Destroy, per CLL.
- Link close `8x 05`, link re-open `8x 03` (always closes first), CANdi protocol close (mode switch,
  e.g. to a K-line protocol, thread case 7) all go through `FUN_00465270`.

### 5.5 TX failure

- `PDUStartComPrimitive` returns non-zero: log, ICD reply status `0xD0` (`05 D0 2B`), no wait, no
  confirm. Remaining channels of an all-channel TX are skipped.
- Async failure (no ACK, bus off, N_Bs timeout): emit ERROR 0x101 on that CLL, still after
  EXECUTING, then FINISHED. See 5.3 for what Tech2Win does with it.

----------------------------------------------------------------------------------------------

## 6. Concurrency

### 6.1 CoPs in flight per CLL

- CLL0: the permanent receiver is EXECUTING all the time, TX SENDRECVs are started on the same CLL
  while it runs, UPDATEPARAM / SetUniqueRespIdTable happen while it runs (then cancel + restart).
  **The receiver must not block the CoP queue.**
- Tech2Win never waits for a TX CoP to finish, so the next TX command can start a new TX CoP while
  the previous one is still sending CFs / waiting for FC. Keep them in order per CLL, but report
  EXECUTING right away (queue internally) or the ICD reply is held.
- CLL1: raw TX CoPs and the wakeup; no receiver.

### 6.2 Channels

Up to 3 channels (SW ch0, MS ch1, HS ch2) x 2 CLLs = 6 CLLs open at once. NG9-3 sessions use HS
(P-bus) and SW-CAN (I-bus) together; SPS uses the all-channel selector `06` / `9E` / `8E`. CAN and
K-line are not open together: CANdi is protocol mode 0, and a mode switch (`FUN_00459d40`) closes
the old handler, for CANdi `0x4573f0` -> `0x468df0` = shutdown event -> thread case 7 closes all
CAN CLLs.

### 6.3 Threads calling the DLL

- PCAN worker thread `0x468c30`: every CAN CLL call (1.2).
- pdu_api.c drain thread `FUN_00462850`: GetEventItem/DestroyItem on module/system queues at the
  same time (host_api_kw2000.md 4.1).
- CLL callbacks may be called from any DLL thread, they only SetEvent.
- Re-arm gap: between Cancel and the new receiver's EXECUTING nothing is listening on CLL0. Tech2Win
  does this on every changed `9x 02 01` record (the session start uploads ~64 records). Keep the CAN
  channel and ISO-TP reassembly alive across it and hand frames that arrive in the gap to the new
  receiver (or accept a tiny loss window).

----------------------------------------------------------------------------------------------

## 7. Timing

- No CPU freeze in the CAN path. The firmware's own ICD reply timeout (emulated time, wall-clock
  paced) applies to every command, value not determined here.
- TX reply latency = Tech2Win thread wake + SetComParam(s) + StartComPrimitive + time until the DLL
  posts EXECUTING. Post EXECUTING synchronously or near it.
- The RX poll is handled on the same worker thread, so while the worker sits in a wait (e.g. a
  missing STATUS, 10 s) polls are not answered at all.
- `9x 02 01` replies come after SetUniqueRespIdTable + UPDATEPARAM x2 (wait FINISHED each) +
  Cancel/CANCELLED + SENDRECV/EXECUTING. Make all of these fast; ~64 of them at session start.
- `8x 03` open, `8x 05` close, `8x 0E` wakeup reply `0x40` (pending) first and do the work after,
  the firmware polls the final result with `80 05` (setup notes). Those may be slow.
- TX confirm: no deadline on Tech2Win's side, SPS waits for it with its own timeout.
- RX latency = DLL + callback + worker drain + attention line + firmware poll. No P2MIN holding,
  no fake keep-alive frames (unlike K-line).
- Reply timestamp: `FUN_004665c0` stamps replies with command time + 0x8312 cyc (2 ms), only the
  locally built candi_common replies are held to it; pdu_can replies go out when ready.

----------------------------------------------------------------------------------------------

## 8. Worked examples (11-bit, HS, GMLAN variant 2)

### 8.1 VIN read `1A 90` to 0x7E0, response on 0x7E8 (mailbox 0x3D, bench bytes)

```
firmware -> TW   04 FD 87 E0 1A 90 EE
TW -> DLL        PDUStartComPrimitive(hMod, HS.CLL0, 0x8004, 6, 00 00 07 E0 1A 90,
                   {500, 1, 0, 0, TxFlag{4, 00 00 00 40}, 0, NULL}, &HS.cllTag[0], &h)
DLL -> TW        STATUS 0x8011 (hCop h) on HS.CLL0
TW -> firmware   05 00 FB
wire             7E0: 02 1A 90 00 00 00 00 00
DLL -> TW        RESULT RxFlag{4, 00 00 00 08}            (TX indication)
TW -> firmware   (attention) poll 2E D2 -> 2D 77 87 E0 F5
wire             7E8: 10 13 5A 90 59 53 33 46
DLL wire         7E0: 30 00 00 00 00 00 00 00             (FC to the paired request id)
wire             7E8: 21 48 34 36 59 58 33 31 / 22 30 34 30 33 36 30 xx
DLL -> TW        RESULT RxFlag{4, 00 00 00 40}, AcceptanceId 0x3D, NumDataBytes 23,
                   00 00 07 E8 5A 90 59 53 33 46 48 34 36 59 58 33 31 30 34 30 33 36 30
TW ring          3D 87 E8 5A 90 59 ... 30
TW -> firmware   poll 2E D2 -> 2D 3D 87 E8 5A 90 59 53 33 46 48 34 36 59 58 33 31 30 34 30 33 36 30 24
```

The last line is byte-identical to the real CANdi bench capture.

### 8.2 Functional `1A B0` to all nodes on all channels (SPS subnet scan)

```
firmware -> TW   06 80 C1 01 FE 1A B0 F0
TW -> DLL        per open channel (SW, MS, HS):
                   PDUSetComParam(CLL0, CP_RequestAddrMode = 2, class 3)
                   PDUStartComPrimitive(CLL0, 0x8004, 7, 00 00 01 01 FE 1A B0,
                     {500, 1, 0, 1, TxFlag{4, 00 00 00 C0}, 0, NULL}, &cllTag[0], &h)
                   wait EXECUTING
TW -> firmware   07 00 F9
wire (each bus)  101: FE 02 1A B0 00 00 00 00
confirms         29 77 C1 01 FE A0 / 2B 77 C1 01 FE 9E / 2D 77 C1 01 FE 9C
answers          each module on its own USDT response id (0x6xx), RESULT 00 00 06 xx 5A B0 ..
                 RxFlag 0x40, AcceptanceId = that id's mailbox -> 2D <idx> 86 xx 5A B0 .. <cks>
```

### 8.3 Firmware TesterPresent (raw)

```
firmware -> TW   04 C0 01 01 FE 01 3E FD
TW -> DLL        PDUStartComPrimitive(HS.CLL1 /*ISO_11898_RAW*/, 0x8004, 7, 00 00 01 01 FE 01 3E,
                   {500, 1, 0, 0, TxFlag{4, 00 00 00 40}, 0, NULL}, &HS.cllTag[1], &h)
                 wait EXECUTING on HS.CLL1
TW -> firmware   05 00 FB
wire             101: FE 01 3E 00 00 00 00 00
confirm          TX indication on CLL1 -> 2D 77 01 01 5A
```

### 8.4 UUDT stream (DPID data on 0x5E8, plain mailbox 0x00)

```
wire             5E8: 60 00 BC 00 65 00 63 00
DLL -> TW        RESULT RxFlag{4, 00 00 00 00}, AcceptanceId 0, 00 00 05 E8 60 00 BC 00 65 00 63 00
TW -> firmware   2D 00 05 E8 60 00 BC 00 65 00 63 00 02     (bench capture)
```

----------------------------------------------------------------------------------------------

## 9. Notes for a J2534 v04.04 backend (advice, not decomp facts)

- One physical CAN channel per CANdi channel (HS `CAN`/`ISO15765` 500k pins 6/14, MS 95.2k pins
  3/11 via J2534-2 pin select, SW-CAN `SW_CAN_PS`/`SW_ISO15765_PS` 33.3k pin 1). Both CLLs of a
  channel share it.
- Two ways to do ISO-TP:
  - DLL-side ISO-TP over a raw CAN channel with a pass filter: full control over padding (0x00),
    ext addressing, FC to the paired id like candiapp, USDT/UUDT split by id from the
    UniqueRespIdTable, no J2534 filter-count limits (the firmware arms up to 64 mailboxes per
    channel). TX indication = PassThruWriteMsgs returned OK for the last frame.
  - J2534 ISO15765 channel: needs a FLOW_CONTROL_FILTER per USDT pair (pattern idA, FC id idB,
    `ISO15765_ADDR_TYPE` for ext), also for the functional id 0x101 (ext FE) since v04.04 refuses
    writes without one; UUDT ids need a parallel CAN channel or CAN_MIXED_FORMAT; TX indication =
    the `TX_INDICATION` (0x08) message; drop `ISO15765_FIRST_FRAME` (0x02) indications; check the
    device's filter limits.
- Map RX: J2534 `CAN_29BIT_ID` -> RxFlag `0x100`, `ISO15765_ADDR_TYPE` -> `0x80`, ISO-TP message ->
  add `0x40`. Never pass `TX_MSG_TYPE` echoes up.
- Padding from TxFlag `0x40` per CoP, not from CP_CanFillerByteHandling (Tech2Win's temp sets of
  that param rely on the TempParamUpdate restore).
- Ignore CP_RequestAddrMode for framing; functional vs physical is visible in the id (0x101) and
  the ext flag.

## 10. What the current `dpdu/src/dll.cpp` lacks for CAN

- `postResult`: RxFlag `{0, NULL}` -> crash in `FUN_00464750` (0.1); AcceptanceId hard-coded 1.
- No TX-indication RESULTs.
- `PDUStartComPrimitive` ignores TempParamUpdate and snapshots the working buffer; Tech2Win's
  temporary `CP_CanFillerByteHandling = 0` would then stick for later CoPs (no restore).
- No CAN protocols in `PROTOS`, no expected-response matching, no USDT/UUDT split, no ISO-TP.
- Worker executes one CoP at a time and only reads frames for the listener when the CoP queue is
  empty: OK as long as TX CoPs post EXECUTING immediately and the listener keeps reading between
  them.

## 11. Open points

- ISO 22900-2 name of RxFlag bit 6 (`0x40`). The required behaviour is clear from candiapp (3.3),
  the standard's naming is not checked.
- The firmware's ICD reply timeout and its SPS `0x77` timeout (api.dwn / sps.dwn).
- Meaning of `8x 0F <m>` (sets the "confirm on TX error" flag and CP_RequestAddrMode 2). Setup.
- Whether the firmware treats an unrequested `0x77` (TX with flags bit 7 clear) as harmless. Only
  matters if the DLL confirms every TX instead of following TxFlag `0x40`.
