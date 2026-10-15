# Tech2Win host side of the D-PDU API: pdu_api.c + pdu_kw2000.c

What GM Tech2Win (`emulator.exe`, 32-bit, build 2019-12-22) does with a D-PDU API (ISO 22900-2)
DLL, read out of the Ghidra decomp. Goal: enough detail to write a D-PDU DLL (J2534 backed) that
Tech2Win drives correctly for KWP2000 on K-line, without opening the decomp.

Sources:
- `RE/tech2win/out/decomp_all.c` (line numbers below are lines in this file)
- `RE/tech2win/emulator.exe` (disassembled with objdump/capstone where Ghidra missed code or the
  decomp was unclear; string resources read with pefile)
- `dpdu/dpdu-passthru/driver/pdu_api.h` (Softing, ISO 22900-2 2.2.0 layout)
- firmware cross-check: `scantool/asm/api.asm` (v148 api.dwn disassembly)

Conventions:
- "cyc" = Tech2Win emulated CPU cycles, 16777 cyc/ms (0x4189). Timing globals are 64-bit cyc.
- UNDEF = 0xFFFFFFFF (PDU_HANDLE_UNDEF), ID_UNDEF = 0xFFFFFFFE (PDU_ID_UNDEF).
- hMod lives at `DAT_005236f0` (= `DAT_005234e8 + 0x208`), the KW2000 code reads it as
  `*(DAT_00d817a0 + 0x208)`. hCLL = `DAT_00d816bc`.
- "dead copy" = a pdu_kw2000.c function with zero references in the binary (verified on the
  disassembly). The worker thread `FUN_00474a00` has the same code inlined with the same
  constants, the dead copies are just easier to read.

Scope: link type 0x1308 (KW2000, Tech2Win mode 1) only. SAAB links on 0x1305 (KW94) go through
`pdu_kw08.c`, 0x1301/0x1302 through `pdu_kw81.c`/`pdu_kw82.c`, CANdi/CAN through `pdu_can.c`.

----------------------------------------------------------------------------------------------

## 0. Hard requirements (break any of these and Tech2Win misbehaves)

1. Exports must be **__stdcall with undecorated names** (`PDUConstruct`, ...). The wrappers
   (62412-62920) push args and `call [ptr]` with no stack cleanup. Resolved by name with
   GetProcAddress in `FUN_00463330` (63895). Missing export = wrapper logs "PDU: X not loaded!"
   and returns 0x12345600.
2. Struct layouts must be the **2.2.0** ones (as in `pdu_api.h`): `PDU_RESULT_DATA` has
   `TimestampFlags` so `NumDataBytes` is at +0x24 and `pDataBytes` at +0x28;
   `PDU_EXP_RESP_DATA` has `NumUniqueRespIds`/`pUniqueRespIds` (7 dwords). Tech2Win reads those
   exact offsets.
3. `PDUGetModuleIds`: every module that should be selectable needs `ModuleStatus ==
   PDU_MODST_AVAIL (0x8063)`, a non-NULL `pVendorAdditionalInfo` (it is `strstr`'d, NULL = crash),
   a **stable `pVendorModuleName` across Construct/Destruct/FreeLibrary cycles** (exact `strcmp`
   is used to find it again) and preferably no spaces in the name (see 1.6).
4. The DLL is loaded, constructed, enumerated, destructed and **FreeLibrary'd** during selection
   (and on every dialog refresh), then loaded again for real. `PDUDestruct` must stop all DLL
   threads and the DLL must survive repeated load/unload in one process.
5. `PDUGetVersion` must succeed. If `HwName` is exactly `"MDI"` or `"MDI 2"`, `FwVersion` and
   `PDUApiSwVersion` must both be >= 0x070E2500, otherwise Tech2Win unloads the DLL and then
   keeps calling through stale pointers (crash). Any other HwName just gives a one-time warning.
6. `pCoPTag` in every event item must be the **exact pointer** passed to `PDUStartComPrimitive`
   (or NULL). Tech2Win dereferences it (`*(int*)pCoPTag`) to tell its own TesterPresent results
   (value 2) from real traffic.
7. RESULT data = **exact wire frame, header + data + checksum**. TX payloads arrive the same way
   (header + data + checksum). Nothing is added or stripped on the Tech2Win side.
8. Never report TX echo / TX indications / start-of-message indications as RESULT items. The
   echo is generated locally by Tech2Win's 16550 model; RxFlag is never looked at.
9. Every wait in the KW2000 thread is "wait for a STATUS item with value X". The DLL must queue
   and signal (callback with 0x801) these:
   - `PDUConnect` -> STATUS `PDU_CLLST_ONLINE` (0x8051)
   - `PDUDisconnect` -> STATUS `PDU_CLLST_OFFLINE` (0x8050)
   - UPDATEPARAM, STARTCOMM, STOPCOMM, SENDRECV with NumReceiveCycles 1 -> STATUS
     `PDU_COPST_FINISHED` (0x8012)
   - SENDRECV with NumReceiveCycles -1 or -2 -> STATUS `PDU_COPST_EXECUTING` (0x8011)
   - successful `PDUCancelComPrimitive` -> STATUS `PDU_COPST_CANCELLED` (0x8013)
   A missing status stalls the worker thread up to 10 s (or forever while other items keep
   arriving).
10. CLL callback must be called with `eventType = PDU_EVT_DATA_AVAILABLE (0x801)` **after** the
    item is queued. 0x802 is ignored. Never call a CLL callback after
    `PDUDestroyComLogicalLink` returned (Tech2Win closes the event handle right after and the
    stored handle value is not cleared).
11. Handles: never hand out hCLL 0 (0 means "no CLL" to Tech2Win) and never hCoP 0 or UNDEF.
    `PDUGetStatus` on an unknown CoP handle (Tech2Win routinely passes hCoP 0) must return
    exactly `PDU_ERR_INVALID_HANDLE` (0x60). `PDUCancelComPrimitive` on a CoP that is not
    running must return non-zero (if it returns 0, Tech2Win waits for a CANCELLED status).
12. Never emit an ERROR item for silence on a SENDRECV with NumReceiveCycles -1/-2. Any ERROR
    item on the CLL makes Tech2Win mark the link dead until the firmware disables and re-enables
    it (see 11.6).
13. A failed STARTCOMM must produce an ERROR item (then FINISHED). Without it Tech2Win is stuck
    in its init state and drops every further firmware frame (see 11.6).
14. Accept `pCopCtrlData == NULL` for UPDATEPARAM, `TxFlag.NumFlagBytes == 0` with NULL pointer,
    `pOutputData == NULL` for IOCTLs, `PDURegisterEventCallback(hMod, <destroyed hCLL>, NULL)`.
15. `PDUSetComParam` must return 0 for `CP_TIdle`, `CP_TWup`, `CP_TInil`,
    `CP_InitializationSettings` (fast init) and `CP_TIdle` (5-baud): a failure there aborts the
    init and no STARTCOMM is ever sent. All other SetComParam/IoCtl failures are only logged.
16. Registry value under HKLM is read by a 32-bit process: on x64 it is
    `HKLM\SOFTWARE\WOW6432Node\D-PDU API` value `Root File`, max 129 characters.

----------------------------------------------------------------------------------------------

## 1. Startup and module selection (pdu_api.c)

All of this runs once at emulator start: `FUN_00404f10` (emulator.c) -> `FUN_004595f0` (56851,
comm init) -> `FUN_00461e30(0)` (62936) -> `FUN_004642d0` (64482). It is not lazy. If the user
cancels, `DAT_00e2e980 = 1`, hMod = -1, every protocol maps to mode 0xd (none) and the PDU DLL is
never used.

### 1.1 Root file location: `FUN_004631a0` (63845)

```
RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software", 0, KEY_READ(0x20019))
RegOpenKeyExW(.., L"D-PDU API", 0, KEY_READ)
RegQueryValueExW(.., L"Root File", NULL, &type, buf, &cb)   cb = 0x104 BYTES (buffer is 520 B)
```
- No KEY_WOW64_64KEY, so a 32-bit process on x64 reads `SOFTWARE\WOW6432Node\D-PDU API`.
- `cb = 0x104` bytes = 130 wchar incl NUL, so the path must be <= 129 chars or the query fails.
- Returns 0 ok, 4 = file does not exist (`FUN_00405e40`), 10 = key/value missing.
- Caller `FUN_00462dc0` (63723): 4 -> MessageBox title "W A R N I N G", text
  "E669265: Tech2Win could not locate the PDU-API dll (MDI communication software).\n
  E669266: A necessary registry key seems to be invalid.\n\nPlease contact dealer support to fix
  this problem." + log "Registry points to non existing file."; 10 -> log
  "No pdu_api_root.xml found." only. Either way the module list ends up empty.

### 1.2 Root XML parse: `FUN_00462dc0` (63723) with AddOns\xml.c helpers

- MSXML DOM: `CoCreateInstance(F6D90F11-9C73-11D3-B32E-00C04F990BB4, IID
  2933BF81-7B36-11D2-B20E-00C04F983E60)` (DOMDocument60 / IXMLDOMDocument), `load(path)`
  (`FUN_004a36a0`, 100436).
- Root element found by scanning the document's child nodes for nodeName
  `MVCI_PDU_API_ROOT` (case-insensitive, `FUN_004a3400`, 100308). Missing -> log
  "PDU: no root element found inside xml file."
- Root attribute `MVCI_PART2_STANDARD_VERSION` must be BSTR "2.2.0" (case-insensitive) or it
  logs "PDU: unsupported PDU-API version '%s'." and carries on.
- Child count capped at 100 ("PDU: more entries in root file (%d) than space available.").
- **Every** child element named `MVCI_PDU_API` (case-insensitive, `FUN_004a32e0` 100264 returns
  index+1 so the scan continues after each hit) becomes an entry, up to 100:
  - child `LIBRARY_FILE`, attribute `URI` -> path (1.3), stored at `DAT_00a39128[i*0x208]`
  - child `SHORT_NAME`, element text (`get_text`, max 260 wchar) -> `DAT_00e41a60[i*0x208]`
  - nothing else is read (no MDF/CDF, no DESCRIPTION, no SUPPLIER_NAME).
- It does **not** pick the first entry or match a SHORT_NAME from config. All entries are
  enumerated and all their AVAIL modules go into one list (1.5). SHORT_NAME is only a display
  label, with two special cases (1.5).

### 1.3 LIBRARY_FILE URI: `FUN_004a30d0` (100168)

- Attribute value must be a BSTR (`vt == 8`) else "XML DOM no URI found".
- Prefix handling, tested in this order with `wcsstr` (case-sensitive):
  `file:///`, `FILE:///`, `File:///` -> path starts 8 chars after the match;
  `file://`, `FILE://`, `File://` -> 7 chars after the match.
- No prefix: the string must contain `C:\`, `C:/`, `D:\` or `D:/` (uppercase drive, only C/D)
  and the path starts at that drive letter. Anything else -> "XML DOM malformed URI" -> entry has
  no library.
- Then `%20` -> space, every `/` -> `\`, max 0x104 chars. Log "XML DOM found VCI DLL: '%s'".
- So `file:///C:/x/y.dll`, `file://C:\x\y.dll` and plain `C:\x\y.dll` all work; relative paths,
  UNC and drives other than C/D (without a file:// prefix) do not.

### 1.4 PDUConstruct

`FUN_00461570(&DAT_004f6634, 0)` = `PDUConstruct(OptionStr = "" (pointer to a 0 byte), pAPITag =
NULL)`. Called at 64333 (enumeration) and 63937 (real load). Return value is only logged.

### 1.5 Enumeration pass: `FUN_00463bb0` (64242)

For each root entry i:
1. Label: SHORT_NAME `D_PDU_API_Bosch_MDI` (wcsicmp) -> `"MDI  "` (two spaces);
   `D_PDU_API_Bosch_MDI_2` -> `"MDI 2"` and flag `isMDI2 = 1`; anything else -> SHORT_NAME.
2. `LoadLibraryW(path)`; only `PDUConstruct`, `PDUDestruct`, `PDUGetModuleIds`,
   `PDUDestroyItem` are resolved here.
3. `PDUConstruct("", NULL)`, `PDUGetModuleIds(&item)`.
4. For each `PDU_MODULE_DATA` (stride 0x14):
   - if `strncmp(pVendorModuleName, <-MS default name, narrowed>, 260) == 0` -> remember the
     current list position as the default (`DAT_005237fc`). Note this happens before the
     AVAIL check, so a non-AVAIL default module points at the wrong row.
   - `strstr(pVendorAdditionalInfo, "USB")` -> listed only if status 0x8063, text
     `"%hs - %hs - %hs"` = `VendorModuleName - label - USB`
   - else `strstr(.., "Ethernet")` -> same with `Ethernet`, only if 0x8063
   - else `strstr(.., "Wireless")` -> text `Name - label - Wireless`; if not MDI 2 or status !=
     0x8062: listed only if 0x8063. MDI 2 with status 0x8062 (NOT_AVAIL): parse
     AdditionalInfo as `key=value` tokens (`FUN_004612a0`), if `WifiFlags` exists and bit0 == 0,
     append `" - Connect USB"` and list it **disabled**.
   - none of the three: listed only if 0x8063, text = `VendorModuleName` alone.
   - stored per row: narrow VendorModuleName (max 100 chars) at `DAT_00e3b4c0[n*0x104]`, the
     library path at `DAT_00e2e9a0[n*0x208]`, display text, enabled flag. Max 100 rows.
5. `PDUDestroyItem(item)` (also called when GetModuleIds failed, with whatever is in the
   uninitialised local), `PDUDestruct()`, `FreeLibrary()`; on success the 4 pointers are NULLed,
   else log "A dynamic library is not released correct!".

ModuleStatus values that hide a module: anything but 0x8063 (READY 0x8060, NOT_READY 0x8061,
NOT_AVAIL 0x8062) except the MDI 2 wireless case.

### 1.6 Selection: `FUN_004642d0` (64482)

- `.conf` option `-MS <name>` (also `-mS -Ms -ms`, `emulator.c` ~2132) fills the default name
  `DAT_00fbeb28` (wide).
- First pass, default set: if a row matched -> copy its name to `DAT_005236f4` (narrow) and its
  library path to `DAT_005234e8` and connect with no dialog ("predefined"). If not found ->
  MessageBox "W A R N I N G" / "E668559: Default Vehicle Communication Interface %s not found",
  log "Default VCI not found", then the dialog.
- Dialog (`FUN_00437d00` -> `FUN_0043f7a0`): calls the enumeration again as refresh callback, then
  shows title "Select Vehicle Communication Interface", text "Select the interface you want to
  use:", the rows from 1.5. Returns row index or -1. -1 = no VCI for this session.
- After a successful connect `FUN_00464270` copies the chosen row's display text, **cut at the
  first space**, into `DAT_00fbeb28` (status bar "Vehicle Communication Interface: %s", also
  logged as "Serial"). It is not written back to the .conf by the emulator. If the configurator
  persists it as `-MS`, a VendorModuleName with a space will never match again. Keep it
  space-free.
- Connect failure -> loop back to the dialog.

### 1.7 Real load + connect: `FUN_00462c70` (63681) -> `FUN_00463330` (63895) -> `FUN_004639c0` (64144)

1. `LoadLibraryW(DAT_005234e8)`; failure -> MessageBox "E668561: Vehicle Communication
   Interface %s (e.g. MDI) not found!\n\nIs it possibly not installed / not turned on?" + log
   "Load MVCI DLL failed. Path to DLL: %s".
2. GetProcAddress of all 29 functions (table in the task brief, `DAT_00a390b4`..`DAT_00a39124`).
3. `PDUConstruct("", NULL)` (error logged, line 0xb86).
4. `PDUGetModuleIds(&item)`. Error -> log "Error when getting the modules from API", returned.
   NumEntries 0 -> returns 0xA3.
5. Walk entries, `strcmp(pVendorModuleName, DAT_005236f4)` (exact, case-sensitive). First match:
   hMod = that entry's `hMod`, return `PDUModuleConnect(hMod)`. No match -> 0xA3.
   **The module list item from this call is never destroyed.** ModuleStatus is not checked here.
6. Result 0xA3 or 1 -> MessageBox E668561 (see 1) + log "No VCI found", `PDUModuleDisconnect(hMod)`,
   `PDUDestruct()`, fail. Other non-zero -> logged, same cleanup, fail. No FreeLibrary on these
   paths (DLL stays loaded, next attempt loads it again).
7. On failure `FUN_00462c70` logs "VCI connection failed. MDI could not be connected or refresh
   might have been too long ago!".

### 1.8 After ModuleConnect: `FUN_00462c70` (63681)

In this order:
1. `PDURegisterEventCallback(UNDEF, UNDEF, FUN_00461e00)` (system) and
   `PDURegisterEventCallback(hMod, UNDEF, FUN_00461e00)` (module). Errors logged only. The event
   handles do not exist yet at this point, so a callback fired during registration does nothing.
2. `FUN_00462a60` (63575) version check: `PDUGetVersion(hMod, &v)`.
   - error -> logged + full shutdown `FUN_004629b0` (DLL unloaded), then execution continues
     into GetObjectId through stale pointers. Do not fail this call.
   - `HwName == "MDI 2"` or `"MDI"` (exact): `isMDI = 1` (enables PDU_XTRA_ERR text logging),
     then if `FwVersion < 0x070E2500 || PDUApiSwVersion < 0x070E2500` -> log "Wrong VCI version:
     Firmware is:0x%08X min:0x%08X PDU-API is:0x%08X min:0x%08X" + `FUN_004629b0` (same crash
     risk).
   - other HwName: one-time popup "Attention!" / "E675228: Tech2Win is operating with an
     unvalidated vehicle communication device. In case of problems please contact the
     manufacturer of the device and not the GM help desk."
   - logs "HwName:'%hs' Firmware:0x%08X PDU-API:0x%08X" and "Vendor: '%hs' Serial: '%s' PDU
     Name: '%hs'" (VendorName, DAT_00fbeb28, PDUApiSwName).
3. `FUN_00464270`: remember display name (1.6).
4. `FUN_00463620` -> `FUN_00463750`: all PDUGetObjectId calls (section 2).
5. `FUN_00463ac0`: PDU_IOCTL_READ_VBATT (section 3).
6. Create auto-reset events "PduCallBackEvent - common events" / "- vci events" / "- shutdown",
   `_beginthread(FUN_00462850)` (event drain thread, 4.1).

----------------------------------------------------------------------------------------------

## 2. Every PDUGetObjectId call: `FUN_00463750` (64025)

Called once after connect, `PDUGetObjectId(objType, char *shortname, UNUM32 *id)`, in table
order. Tables in .data, entry = {UNUM32 *dest, char name[]}; entries with dest NULL are skipped.
Initial value of every dest is 0xFFFFFFFE.

Result handling: 0 + valid id = ok. 0 + id 0xFFFFFFFE -> log "PDU: %hs not supported but
required." and flag 0x10. Return 0x50 -> id set to 0xFFFFFFFE, flag 0x10. Any other error ->
stop the whole mapping (remaining ids stay 0xFFFFFFFE), log "Mapping of PDU API objects failed".
Flag 0x10 -> popup "W A R N I N G": ("E668859: You are not using a GM-MDI." if not isMDI) +
"E668860: Your VCI does not support all the parameters required for vehicle communication."
Nothing aborts. Return a unique valid id for all 88 names to get no popup.

PDU_OBJT_COMPARAM (0x8024), 59 calls, order and storage:
```
CP_Loopback 51e0dc            CP_P2Max 51e0b0               CP_P2Min 51e0ac
CP_P3Min 51e0b4               CP_P3Phys 51e0c4              CP_RepeatReqCountApp 51e04c
CP_StartMsgIndEnable 51e058   CP_SwCan_HighVoltage 51e05c   CP_TesterPresentHandling 51e068
CP_TransmitIndEnable 51e06c   CP_5BaudAddressPhys 51e07c    CP_5BaudMode 51e088
CP_As 51dffc                  CP_Bs 51e008                  CP_CanFillerByte 51e00c
CP_CanFillerByteHandling 51e010 CP_CanPhysReqExtAddr 51e014 CP_CanPhysReqFormat 51e018
CP_CanPhysReqId 51e01c        CP_CanRespUSDTExtAddr 51e020  CP_CanRespUSDTFormat 51e024
CP_CanRespUSDTId 51e028       CP_CanRespUUDTExtAddr 51e02c  CP_CanRespUUDTFormat 51e030
CP_CanRespUUDTId 51e034       CP_Cr 51e038                  CP_Cs 51e03c
CP_HeaderFormatKW 51e040      CP_InitializationSettings 51e044 CP_P4Max 51e0c0
CP_P4Min 51e0bc               CP_RequestAddrMode 51e050     CP_TIdle 51e070
CP_TInil 51e074               CP_TWup 51e078                CP_W1Max 51e090
CP_W1Min 51e08c               CP_W2Max 51e098               CP_W2Min 51e094
CP_W3Max 51e0a0               CP_W3Min 51e09c               CP_W4Max 51e0a8
CP_W4Min 51e0a4               CP_Baudrate 51e000            CP_BitSamplePoint 51e004
CP_K_L_LineInit 51e048        CP_SamplesPerBit 51e054       CP_SyncJumpWidth 51e060
CP_TerminationType 51e064     CP_5BaudInitBaudrate 51e084   CP_5BaudCommBaudrateOverride 51e080
CP_BecomeMaster 51e0c8        CP_BecomeSlave 51e0cc         CP_ByteCountOffset 51e0e4
CP_MessageScheduler 51e0d8    CP_P3Max 51e0b8               CP_P3Min_J2740 51e0e0
CP_SetPollResponse 51e0d0     CP_SetRelinquishMastership 51e0d4
```
PDU_OBJT_BUSTYPE (0x8022), 9 calls:
`ISO_11898_2_DWCAN, ISO_11898_3_DWFTCAN, ISO_9141_2_UART, ISO_14230_1_UART (51f270),
SAE_J1939_11_DWCAN, SAE_J1850_VPW, SAE_J2411_SWCAN, SAE_J2740_UART, KW_UART`

PDU_OBJT_PROTOCOL (0x8021), 11 calls:
`ISO_14230_3_on_ISO_14230_2 (51f294), ISO_15765_3_on_ISO_15765_2, SAE_J2190_on_ISO_9141_2,
SAE_J2190_on_SAE_J1850_VPW, ISO_15031_5_on_ISO_15765_4, SAE_J1939_73_on_SAE_J1939_21,
ISO_11898_RAW, XDE_5024_on_XDE_5024_UART, KW82_on_KW_UART, KW81_on_KW_UART, KW309_on_KW_UART`

PDU_OBJT_PINTYPE (0x8025), 6 calls: `HI, LOW, K (522238), L (52223c), PLUS, SINGLE`

PDU_OBJT_IO_CTRL (0x8023), 3 calls: `PDU_IOCTL_READ_VBATT (5234d8),
PDU_IOCTL_START_MSG_FILTER (5234dc), PDU_IOCTL_CLEAR_MSG_FILTER (5234e0)`

KW2000 actually uses: bus ISO_14230_1_UART, protocol ISO_14230_3_on_ISO_14230_2, pins K/L,
IOCTLs START/CLEAR_MSG_FILTER, ComParams Baudrate, TesterPresentHandling, RepeatReqCountApp,
K_L_LineInit, StartMsgIndEnable, HeaderFormatKW, P2Min, P2Max, P4Min, P4Max, TIdle, TWup, TInil,
InitializationSettings, 5BaudMode, 5BaudInitBaudrate, 5BaudAddressPhys, W4Min.

----------------------------------------------------------------------------------------------

## 3. PDUIoCtl usage

| IOCTL | where | hMod, hCLL | input | output | on failure |
|---|---|---|---|---|---|
| READ_VBATT | `FUN_00463ac0` (64209), once after connect, line 64230 | hMod, UNDEF | NULL | `PDU_DATA_ITEM **` | log "VBATT-Error: %lu" |
| CLEAR_MSG_FILTER | every KW2000 init (fast and 5-baud) | hMod, hCLL | NULL | NULL | logged |
| START_MSG_FILTER | right after CLEAR | hMod, hCLL | item, see below | NULL | logged |

READ_VBATT (64230): `(*PDUIoCtl)(hMod, 0xFFFFFFFF, id, NULL, &p)` where `p` is pre-set to a local
item `{0x1000, &DAT_006fde84}`. After a 0 return it logs `"VBATT: %lu mV"` with
`*(UNUM32*)p->pData`. No scaling, value is millivolts, only logged, never `PDUDestroyItem`ed, not
mandatory. Return `{PDU_IT_IO_UNUM32, &mV}` allocated by the DLL.

START_MSG_FILTER input (pass-all):
```
PDU_DATA_ITEM      { ItemType 0x1003 (PDU_IT_IO_FILTER), pData -> list }
PDU_IO_FILTER_LIST { NumFilterEntries 1, pFilterData -> f }
PDU_IO_FILTER_DATA f { FilterType 1 (PDU_FLT_PASS), FilterNumber 1, FilterCompareSize 1,
                       FilterMaskMessage[12] = 00.., FilterPatternMessage[12] = 00.. }
```
None of the IOCTLs is mandatory.

----------------------------------------------------------------------------------------------

## 4. Callbacks and event queues

### 4.1 System + module callback: `FUN_00461e00` (62922), drain thread `FUN_00462850` (63495)

- `void __stdcall cb(eventType, hMod, hCLL, pCllTag, pAPITag)` (`ret 0x14`). eventType is not
  checked (0x801 and 0x802 both signal).
- `if (hMod == 0xFFFFFFFE) SetEvent("common events") else SetEvent("vci events")`. Note it
  compares with **0xFFFFFFFE**, while Tech2Win itself uses 0xFFFFFFFF as the undefined handle in
  every call. A DLL that follows ISO (hMod = 0xFFFFFFFF for the system callback) gets its system
  items drained only if it happens to also signal with 0xFFFFFFFE. Easiest: never queue
  system-level items, put module errors on the module queue.
- Thread: `WaitForMultipleObjects({common, vci, shutdown}, any, 5000)`:
  0 -> drain `PDUGetEventItem(UNDEF, UNDEF, &it)`; 1 -> drain `PDUGetEventItem(hMod, UNDEF, &it)`;
  2 -> exit; timeout -> loop (no polling, items are only drained after a signal).
- Drain = loop until GetEventItem returns non-zero. Per item: if `ItemType == PDU_IT_ERROR`
  (0x1302): log `ExtraErrorInfoId` text (`FUN_004621b0` 63117, only when isMDI) and `ErrorCodeId`
  text (`FUN_00461e70` 62957). Codes 0x10, 0x11, 0x106 also call `FUN_00459490` which clears the
  VCI status bar fields and `DAT_00fbeb28`. Nothing else. Every item is `PDUDestroyItem`ed.

### 4.2 CLL callback: `FUN_00472340` (72432)

- `__stdcall`, 5 args. `if (eventType == 0x801 && hCallbackEvent) SetEvent(hCallbackEvent)`.
  hCallbackEvent = `DAT_00d816a4`, auto-reset, named "Callback Event".
- Registered per CLL: `PDURegisterEventCallback(hMod, hCLL, FUN_00472340)` right after
  `PDUCreateComLogicalLink`, before `PDUConnect`. Unregistered with NULL **after**
  `PDUDestroyComLogicalLink` (return ignored).
- Calling it from any DLL thread is fine, it only does SetEvent and never calls back into the
  DLL. Calling it synchronously from inside PDUStartComPrimitive etc. is also fine.

### 4.3 Who calls what, which thread

- Startup/main thread: enumeration, connect, version, GetObjectId, READ_VBATT, and at exit the
  shutdown.
- Event drain thread (`FUN_00462850`): GetEventItem/DestroyItem on system/module queues.
- KW2000 worker thread (`FUN_00474a00`): every CLL call (create, connect, SetComParam, IoCtl,
  StartComPrimitive, Cancel, GetStatus, GetEventItem, DestroyItem, GetComParam, disconnect,
  destroy).
- The emulated CPU thread never calls the DLL. It only exchanges data with the worker through
  globals, a 5-slot RX ring and SetEvent.
So expect concurrent calls from the worker and the drain thread.

### 4.4 CLL event drain in the worker (thread case 0, 73721-73998; same code in `FUN_00479430`)

`PDUGetEventItem(hMod, hCLL, &it)` until non-zero. For each item, then `PDUDestroyItem(it)`:

| ItemType | read | action |
|---|---|---|
| 0x1300 RESULT | `pCoPTag`, `pData->NumDataBytes` (+0x24), `pData->pDataBytes` (+0x28) | if `pCoPTag == NULL || *(int*)pCoPTag != 2`: copy bytes into the RX ring (section 11). Else (own TesterPresent): set `DAT_00d81678 = 1`, drop. |
| 0x1301 STATUS | `*(UNUM32*)pData` | only used by waits, no state change |
| 0x1302 ERROR | (pCoPTag only in a wait branch) | link reset, see 11.6 |
| 0x1303 INFO | nothing | ignored |
| other | | log "PKW: ProcessEvent() - unknown event" |

Never read: `hCop`, `Timestamp`, `RxFlag`, `UniqueRespIdentifier`, `AcceptanceId`,
`TimestampFlags`, `TxMsgDoneTimestamp`, `StartMsgTimestamp`, `pExtraInfo`.

### 4.5 The wait primitive: `FUN_00479430(itemType, value)` (75768)

```
do {
  if (WaitForSingleObject(hCallbackEvent, 10000) != 0) return;   // 10 s with no signal: give up
  drain all items (same handling as 4.4);
  matched if: STATUS item with *pData == value          (hCop NOT checked)
          or: ERROR while waiting for 0x1301 and pCoPTag && *pCoPTag == -1 (never true, tags are 0..4)
} while (!matched);
```
Every call in pdu_kw2000.c is `FUN_00479430(0x1301, X)`. A stale STATUS with the same value from
an older CoP satisfies a later wait.

----------------------------------------------------------------------------------------------

## 5. Return codes and error handling

- Every PDU call result goes to `FUN_00461e70` (62957), which logs "PDU error: %s" (level 6,
  0x63 COMPARAM_NOT_SUPPORTED only level 2). No retries, no PDUGetLastError (never called
  anywhere), no aborts except:
  - module connect path (1.7): GetModuleIds error, 0xA3 or 1 from ModuleConnect -> no VCI.
  - GetVersion error / wrong MDI version -> unload (crash risk).
  - fast init: SetComParam failure on CP_TIdle/CP_TWup/CP_TInil/CP_InitializationSettings,
    UPDATEPARAM failure, STOPCOMM failure -> init attempt abandoned, STARTCOMM not sent.
  - 5-baud: CP_TIdle failure, UPDATEPARAM failure, STOPCOMM failure -> abandoned.
- Codes 0x10 / 0x11 / 0x106 from any call or event also clear the VCI status display.
- `PDUGetStatus`: for the STARTCOMM handle only 0x60 means "gone"; any other non-zero return
  leaves status 0 and Tech2Win calls Cancel. For the SENDRECV handle any non-zero means gone.
- `PDUGetEventItem`: any non-zero ends the drain loop (expect PDU_ERR_EVENT_QUEUE_EMPTY 0x71).
- Never called anywhere in emulator.exe: GetLastError, GetTimestamp, GetUniqueRespIdTable,
  LockResource, UnlockResource, GetResourceIds, GetConflictingResources, GetResourceStatus.
  SetUniqueRespIdTable is used once (CAN code, not KW2000). Export them anyway.
- "No communication" is the firmware's own screen. From the host side it happens when: the
  STARTCOMM RESULT misses the P2 window (11.3), an ERROR item kills the link (11.6), the
  TesterPresent CoP never gets a RESULT (11.5), or a required STATUS item is missing (10 s stalls).

----------------------------------------------------------------------------------------------

## 6. Lifetime

- DLL load/unload: during enumeration (each root entry: Load, Construct, GetModuleIds,
  DestroyItem, Destruct, FreeLibrary; repeated on dialog refresh), then one real load for the
  whole emulator run.
- Exit: `FUN_00404690` -> `FUN_00459d10` (57119): switch protocol mode to 0xd (closes the KW2000
  instance, see 7.4), then `FUN_004629b0` (63546): stop drain thread, `RegisterEventCallback(hMod,
  UNDEF, NULL)`, `PDUModuleDisconnect(hMod)`, `RegisterEventCallback(UNDEF, UNDEF, NULL)`,
  `PDUDestruct()`, close the 3 events, hMod = -1, `FreeLibrary`.
- Same `FUN_004629b0` on GetVersion failure / wrong MDI version.
- CLL: one per KW2000 protocol instance. Created when the worker thread starts (first link
  enable of a 0x1308 link after a different protocol mode was active), destroyed when the mode
  changes to another protocol or at exit. Not per ECU session. Connect right after create,
  Disconnect right before destroy.
- STARTCOMM/STOPCOMM: per ECU init attempt; STOPCOMM also on link disable/reset/close.

----------------------------------------------------------------------------------------------

## 7. KW2000 protocol instance (pdu_kw2000.c)

### 7.1 Live code

Protocol table set at `0x4596f5..` (comm.c), PDU column:

| role | address | notes |
|---|---|---|
| Init | `0x4738e0` (not a Ghidra function, read from disasm) | defaults, 6 handles, attributes, starts worker |
| Send (UART frame complete) | `FUN_00471430` (71864) | runs on the CPU thread |
| GetMessage | `FUN_00473200` (73076) | hands the staged RX message to the UART |
| Poll | `0x473250` (disasm only) | decides when an RX message is staged (P2 gating) |
| Event | `FUN_00477bf0` (75008) | link enable/disable, UART/TPU events |
| Close | `0x478b60` | SetEvent(shutdown), wait for the worker to exit |
| Worker thread | `FUN_00474a00` (73469) | all D-PDU calls |
| Wait | `FUN_00479430` (75768) | 4.5 |
| CLL callback | `FUN_00472340` (72432) | 4.2 |

Dead copies (same constants, easier to read): `FUN_00470a50` 5-baud init (71558), `FUN_004716e0`
CLL create (71974), `FUN_00471a80` CLL destroy (72115), `FUN_00471d10` common ComParams (72205),
`FUN_00471f90` timing ComParams (72304), `FUN_00472130` stop comm (72358), `FUN_00472360` fast
init (72444), `FUN_00472d40` fake 7E (72777), `FUN_00472e70`/`FUN_00472ff0` TesterPresent
builders (72846/72946), `FUN_00474170` ProcessEvent (73138), `FUN_00478370` internal
TesterPresent (75260), `FUN_00478890` SendMessage (75456), `FUN_00478ba0` receive-only (75589),
`FUN_00478d20`/`FUN_00478df0` cancels (75665/75697).

### 7.2 Firmware link attributes -> globals

The firmware talks to its VCI task through the pSOS queue `QVCI`. Tech2Win's trap spy
(`FUN_00420e70`, 17711-17860, q_send hook) decodes `'L' x 'O' 'P'` = Link_Open -> `FUN_00420800` (17477) and
the other `'L'` codes as link attributes -> `FUN_00420010` (17097). Attribute block base
`0xEFE080` (`FUN_00420690`), each slot {value, valid}:

| offset | source | KW2000 global | meaning |
|---|---|---|---|
| +0x00 | OP msg[2] (link type) | | |
| +0x08 | OP msg[3] bits 8-11 | `DAT_00d816ec` | **K pin** (= firmware PIN_A) |
| +0x10 | OP msg[3] bits 0-3 | `DAT_00d816f0` | **L pin** (= firmware PIN_B) |
| +0x28/+0x30 | 'p2'/'P2' ISO_P2MIN/MAX (us) | `DAT_00d81758`/`60` | |
| +0x38/+0x40 | 'p3'/'P3' ISO_P3MIN/MAX | `DAT_00d81770`/`78` | |
| +0x48/+0x50 | 'p4'/'P4' ISO_P4MIN/MAX | `DAT_00d81780`/`88` | |
| +0x60 | 'IS' ISO_SADDRESS | `DAT_00d81701` | tester address |
| +0x68 | 'IA' ISO_ADDRESS or 'IR' ISO_TADDRESS | `DAT_00d81700` | ECU address |
| +0x88 | 'IB' ISO_INIT_BAUDRATE | `DAT_00d816f8` | 5 or 200 = 5-baud init |
| +0xa0 | 'IN' ISO_FUNC_ADDR | `DAT_00d816fc` | non-zero = functional |
| +0xb8 | link type (0x1308) | `DAT_00d816a0` | |

Also decoded but not used by KW2000: 'p1'/'P1', 'IK', 'BF', 'BT', 'FM', 'TS', 'LG', 'BR' SPEED,
'MS'. `'RA'` PIN_A and `'RB'` PIN_B attributes are **not handled** (logged "TRAP: unhandled link
attribute"). 'IF' ISO_5BAUD_ATTEMPTS is forced to >= 3 in guest memory. Attribute values are in
microseconds, converted `us/1000*16777` cyc (so ms resolution). Each Link_Open clears the block.

Defaults set by Init (`0x4738e0`) before the attributes are applied:
```
K pin 7, L pin 15, IN 0, init mode fast (DAT_00d8169c = 1), baud DAT_00d816f4 = 10400,
IB DAT_00d816f8 = 10400
P1 0 / 20 ms   P2 25 / 50 ms   P3 55 / 5000 ms   P4 5 / 20 ms
W1 60 / 300 ms  W2 5 / 20 ms  W3 10 / 19 ms  W4 25 / 50 ms   (W* are never overridden)
```
Validation when applying attributes: min must be < max (else "PKW: values for P2 are incorrect"
etc. and the pair is ignored), P2MAX clamped to 1500 ms (0x17ffebc cyc).

Derived:
- `P2MAX_CP` (`DAT_00d81768`, what goes into CP_P2Max) = P2MAX if functional, else
  max(P2MAX, 300 ms).
- `P2_FAKE` (`DAT_00d81790`) = P2MAX - (P2MAX - P2MIN)/4.
- `FAKE_LIMIT` (`DAT_00d81798`) = 6250 ms.

Init also sets the CoP tag variables: `DAT_00d816c8 = 0` (CLL tag), `DAT_00d816cc = 1` (normal
SENDRECV), `DAT_00d816d0 = 2` (internal TesterPresent), `DAT_00d816d4 = 3` (fast init),
`DAT_00d816d8 = 4` (5-baud). pCoPTag/pCllTag are pointers to these.

### 7.3 Worker handles (`WaitForMultipleObjects(6, ..., 5000)`, index order)

0 "Callback Event" (CLL data), 1 "New Transmit Message", 2 "Tester Present Timer" (waitable
timer), 3 "Shutdown Thread", 4 "Start Rx After 5 Baud Init" (never signalled anywhere),
5 "Stop Comm When Link Is Disabled". All auto-reset.

### 7.4 When the instance starts and stops

Firmware `'L0EN'` -> `FUN_00459d40(mode)` (57136): only if the mode differs from the current one,
Close old protocol, Init new one. Then event 9 is sent to the current protocol. So a second 0x1308
link with different pins while KW2000 is still the active mode does **not** recreate the CLL.
Event 9 re-reads the pins into the globals but nothing uses them until the next thread start.

----------------------------------------------------------------------------------------------

## 8. CLL creation and pins (B.1)

Worker start, 73640-73718 (dead copy `FUN_004716e0`):
```
if (hCLL != 0) { cancel/stop as needed, PDUDisconnect, wait OFFLINE, PDUDestroyComLogicalLink,
                 RegisterEventCallback(hMod, hCLL, NULL), hCLL = 0 }
FUN_00459270(Kpin, Lpin, 0x1308, &pin[0].DLCPinNumber, &pin[1].DLCPinNumber, ..)  // plain copy
PDU_PIN_DATA pin[2] = { { Kpin, id("K") }, { Lpin, id("L") } };
PDU_RSC_DATA rsc   = { id("ISO_14230_1_UART"), id("ISO_14230_3_on_ISO_14230_2"),
                       NumPinData = (Lpin == 0) ? 1 : 2, pin };
UNUM32 flag = 0x80;   PDU_FLAG_DATA cllFlag = { 4, (UNUM8*)&flag };   // bytes 80 00 00 00
PDUCreateComLogicalLink(hMod, &rsc, resourceId = 0xFFFFFFFE, pCllTag = &DAT_00d816c8 (=0),
                        &hCLL, &cllFlag);
PDURegisterEventCallback(hMod, hCLL, FUN_00472340);
PDUConnect(hMod, hCLL);
FUN_00479430(0x1301, 0x8051);       // wait ONLINE
```
Each step only runs if the previous returned 0 (errors logged at 0x3d9/0x3e1/0x3e9). No
ComParams are set before Connect, the DLL has to come up with defaults (10400 baud).

CllCreateFlag byte 0 = 0x80. In ISO 22900-2 terms that is the RAWMODE bit (bit 31) with
CHECKSUMMODE (bit 30) clear. Whatever the standard says about checksums in that mode, the
observed contract is: TX payload already contains header and checksum, RX must contain header
and checksum (11.1).

### Where the pin numbers come from

- Only from the Link_Open message, word msg[3]: `K = (w >> 8) & 0xF`, `L = w & 0xF`
  (`FUN_00420800`). Link_Open always marks both valid, so the 7/15 defaults never survive.
- On the firmware side (api.dwn `0x1ecfb6`) that word is built as `(link.pinA << 8) |
  link.pinB` from the link's PIN_A / PIN_B fields (setters `0x1ecf7e` / `0x1ecf9a`, filled from
  the scenario's link-open record bytes 8 and 9, `0x1c72ee`). So **K = PIN_A, L = PIN_B**.
  The `'RA'`/`'RB'` attribute messages and Tech2Win's MT8816 crosspoint model (`t2e_mux.c`,
  register level only) play no part.
- Nibble decode: pin 16 would come out as 0.

### When pin 7 and pin 8 both show up

Exactly when the firmware link has PIN_A and PIN_B both set to 7/8 (or 8/7). Then the CLL gets
`{7, K}, {8, L}` (or `{8, K}, {7, L}`): **always one K and one L pin, never two K pins**. Any
non-zero PIN_B produces a second entry with type L (e.g. PIN_B 15 = the standard L line).
PIN_B 0 -> only the K entry.

Context from the firmware: its ISO router for 0x1300-0x130b (`0x1d1812`) only connects PIN_A to
the K-line UART row of the crosspoint. PIN_B is validated (1..16) but only routed for link type
0x1306. So on a real Tech2 a non-zero PIN_B on a 0x1308 link is **not** an L line. Tech2Win
turns it into one anyway, and with `CP_K_L_LineInit = 0` (K and L used for the init address) a
spec-following MVCI would drive that pin during fast init / 5-baud. On a SAAB, if PIN_B were 8
that would hit the second K-line bus (ABS/TC etc.). Recommendation: treat the L entry as
informational, init on K only (J2534 `ISO9141_K_LINE_ONLY`).

Unverified at runtime: which PIN_A/PIN_B values the v148 SAAB scenarios actually send for 0x1308
links. Capture the QVCI `L0OP` msg[3] (d5 at q_send) with the t2emu queue tap. Also note 7.4:
the t2emu route capture saw a pin-8 probe before the pin-7 DICE session; if both are 0x1308 with
no other protocol in between, Tech2Win keeps the CLL of the first one.

----------------------------------------------------------------------------------------------

## 9. ComParams (B.2)

All `PDUSetComParam(hMod, hCLL, &item)` with `item = { ItemType 0x1200, ComParamId,
ComParamDataType 0x105 (UNUM32), ComParamClass, pComParamData -> UNUM32 }`. Set into the working
buffer, then committed with an UPDATEPARAM CoP. TempParamUpdate is always 0. Nothing is set
before PDUConnect and nothing is ever set via TempParamUpdate.

### 9.1 Common block (every init, fast and 5-baud), in order

| ComParam | class | value |
|---|---|---|
| CP_Baudrate | 5 BUSTYPE | `DAT_00d816f4`: 10400, or the last 5-baud result. **Fast init only** |
| CP_TesterPresentHandling | 7 TESTER_PRESENT | 0 (Tech2Win does its own) |
| CP_RepeatReqCountApp | 4 ERRHDL | 2 |
| CP_K_L_LineInit | 5 BUSTYPE | 0 |
| CP_StartMsgIndEnable | 3 COM | 0 |
| CP_HeaderFormatKW | 3 COM | 5 (only KW2000 sets it; literal) |
| IOCTL CLEAR_MSG_FILTER, START_MSG_FILTER | | section 3 |

Note: `DAT_00d816f4` is never loaded from the 'BR' SPEED attribute. Fast init always runs at
10400 unless a 5-baud init earlier in the instance read back another rate (or failed: then 9600).

### 9.2 Timing block (every init)

| ComParam | class | value (us) | default |
|---|---|---|---|
| CP_P2Min | 1 TIMING | P2MIN | 25000 |
| CP_P2Max | 1 | P2MAX_CP (>= 300 ms when physical) | 300000 |
| CP_P4Min | 1 | P4MIN | 5000 |
| CP_P4Max | 1 | P4MAX | 20000 |

Computed as `cyc / 16777 * 1000` (integer ms * 1000). P1/P3/W1-W3 are never sent.

### 9.3 Fast init block (after 9.1 + 9.2)

| ComParam | class | value | failure |
|---|---|---|---|
| CP_TIdle | 2 INIT | 1000 | abort init |
| CP_TWup | 2 | 50000 | abort |
| CP_TInil | 2 | 25000 | abort |
| CP_InitializationSettings | 2 | 2 (fast init) | abort |

### 9.4 5-baud block (after 9.1 without Baudrate + 9.2)

| ComParam | class | value | failure |
|---|---|---|---|
| CP_InitializationSettings | 2 | 1 (5-baud) | logged |
| CP_5BaudMode | 2 | 0 | logged |
| CP_5BaudInitBaudrate | 1 | `10000 / IB` (IB=5 -> 2000, IB=200 -> 50) | logged |
| CP_5BaudAddressPhys | 3 | address byte (see 13) | logged |
| CP_TIdle | 3 | 2100000 | abort |
| CP_W4Min | 2 | 25000 (W4MIN, never overridden) | logged |

CP_5BaudInitBaudrate units are whatever the MDI MDF says; Tech2Win only takes this path when IB is
5 or 200, so a DLL can treat both resulting values as "5 baud".

### 9.5 PDUGetComParam

Only one: after a 5-baud STARTCOMM finished, `PDUGetComParam(hMod, hCLL, id(CP_Baudrate), &it)`.
Accepted if return 0 and `it->ComParamId == id(CP_Baudrate)` and `it->ComParamDataType ==
0x105`; then `DAT_00d816f4 = *(UNUM32*)it->pComParamData` and `PDUDestroyItem(it)`. Otherwise
9600. That baud drives Tech2Win's fake sync-byte for the firmware's baud measurement
(`FUN_0041f630` -> TPU fn 0xC). Report the detected rate here.

----------------------------------------------------------------------------------------------

## 10. ComPrimitives (B.3)

`PDUStartComPrimitive(hMod, hCLL, CoPType, CoPDataSize, pCoPData, pCopCtrlData, pCoPTag,
&hCoP)`. Expected-response entry used everywhere it has one:
`ER = { ResponseType R, AcceptanceId 1, NumMaskPatternBytes 1, pMask -> {0x00},
pPattern -> {0x00}, NumUniqueRespIds 0, pUniqueRespIds NULL }` (mask 0 = matches anything).
`TXF4` = `TxFlag { 4, -> 00 00 00 00 }`.

| CoP | Type | data | Time | NumSend | NumRecv | TxFlag | NumExp / ER | pCoPTag | hCoP kept in | then waits |
|---|---|---|---|---|---|---|---|---|---|---|
| UPDATEPARAM fast | 0x8003 | none | pCopCtrlData = **NULL** | | | | | ->3 | local | FINISHED |
| UPDATEPARAM 5-baud | 0x8003 | none | NULL | | | | | ->4 | `DAT_00d816c0` | FINISHED |
| STARTCOMM fast | 0x8001 | StartComm frame, hdr+cs | 0 | 1 | 1 | TXF4 | 1 / R=0 | ->3 | local | FINISHED |
| STARTCOMM 5-baud | 0x8001 | none | 0 | 1 | 1 | {0, NULL} | 1 / **R=1** | ->4 | local | FINISHED |
| SENDRECV physical | 0x8004 | request frame, hdr+cs | 0 | 1 | **-1** | TXF4 | 1 / R=0 | ->1 | `DAT_00d816c4` | EXECUTING |
| SENDRECV functional ('IN') | 0x8004 | same | 0 | 1 | **-2** | TXF4 | 1 / R=0 | ->1 | `DAT_00d816c4` | EXECUTING |
| internal TP physical | 0x8004 | TP frame, hdr+cs | 0 | 1 | 1 | TXF4 | 1 / R=0 | ->2 | `DAT_00d816c4` | FINISHED |
| internal TP functional | 0x8004 | same | 0 | 1 | -2 | TXF4 | 1 / R=0 | ->2 | `DAT_00d816c4` | EXECUTING |
| STOPCOMM | 0x8002 | none | **100** | 0 | 0 | {0, NULL} | 0 / NULL | NULL | local | FINISHED |
| receive-only (dead, event never set) | 0x8004 | none | 0 | 0 | -1 | TXF4 | 1 / R=0 | ->1 | `DAT_00d816c4` | EXECUTING |

TempParamUpdate is 0 in all of them. Payloads are malloc'd copies freed right after the call
returns (copy them in the DLL).

Before sending anything the frame's KWP checksum is verified (sum of all bytes but the last,
mod 256, equals the last byte); a bad frame is logged ("PKW: SendMessage() - message has an
incorrect checksum." / "PKW: FastInit() - the message has an incorrect checksum!!!") and not sent.

### 10.1 Cancel/stop prologue (used before every init, stop and shutdown)

```
st = 0; r = PDUGetStatus(hMod, hCLL, DAT_00d816c0 /*5-baud UPDATEPARAM, normally 0*/, &st, &ts, &x);
if (r != 0x60 && st != FINISHED && st != CANCELLED) { if (PDUCancelComPrimitive(..)==0) wait CANCELLED; }
st = 0; r = PDUGetStatus(hMod, hCLL, DAT_00d816c4 /*SENDRECV*/, ..);
if (r == 0 && st == EXECUTING) { if (PDUCancelComPrimitive(..)==0) wait CANCELLED; }
PDUGetStatus(hMod, hCLL, UNDEF, &cll, ..);        // CLL state
if (cll == PDU_CLLST_COMM_STARTED) { STOPCOMM; wait FINISHED; }   // failure aborts the caller
```
The SENDRECV-only variant (before each request and each internal TP) skips the first and last
parts. Functional mode adds: if the SENDRECV is EXECUTING, log "artifical delay for MDI", wait
FINISHED (up to 10 s idle) first, then cancel if still running.

### 10.2 Sequences

Thread start: section 8 (create, callback, connect, wait ONLINE).

Fast init, triggered by the **first frame the firmware transmits** after link enable (normally
its StartCommunication request, e.g. `81 41 F1 81 34`), thread case 1 74268-74498:
```
verify checksum
10.1 prologue (cancel, STOPCOMM if COMM_STARTED)
if RX-ring read slot == 0xFFFF -> arm ring reset
9.1 common (incl. CP_Baudrate) + filters
DAT_00d8166c = 1 (build TP template from next result)
9.2 timing
9.3 fast init params (abort on failure)
UPDATEPARAM (ctrl NULL, tag ->3), wait FINISHED
DAT_00d81674 = 1 (first TP timer 100 ms)
STARTCOMM (payload = whole frame incl. checksum, tag ->3), wait FINISHED
```

5-baud init, triggered at link enable (event 9) when 'IB' is 5 or 200 and 'IA' is set:
Tech2Win fakes a 1-byte firmware TX `[IA]`, which lands in thread case 1 74008-74267:
```
10.1 prologue
9.1 common (no Baudrate) + filters
DAT_00d8166c = 1
9.2 timing
9.4 5-baud params (CP_5BaudAddressPhys = that byte)
UPDATEPARAM (ctrl NULL, tag ->4, hCoP -> DAT_00d816c0), wait FINISHED
DAT_00d81674 = 1
STARTCOMM (no data, tag ->4), wait FINISHED
PDUGetComParam(CP_Baudrate) (9.5)
```
No 'IA' -> log "PKW: 5BaudInit - no valid address found" and nothing happens.

Request in session (state 2), thread case 1 74500-74625: firmware frame -> SENDRECV-only
prologue -> SENDRECV (table) -> wait EXECUTING. So each request is Cancel(previous SENDRECV) ->
wait CANCELLED -> Start -> wait EXECUTING. Make cancel and the status items fast.

If the internal TP CoP is still waiting for its RESULT (`DAT_00d81678 == 0`), a new firmware
request is deferred: `Sleep(5); SetEvent(New Transmit Message)` in a loop until it arrives.

Internal TesterPresent, thread case 2 74626-74837: see 11.5.

Link disable / reset / close ('L0DI'/'L0RS'/'L0CL' = events 10/11/12) while in session: event
handler sets "Stop Comm When Link Is Disabled" and resets its state; thread case 5 runs the full
10.1 prologue (cancel + STOPCOMM if COMM_STARTED). The CLL stays connected.

Shutdown (Close, thread case 3 74937-75000): cancel timer, 10.1 prologue (STOPCOMM failure only
logs "PKW: CllDestroy() - stopping comm failed"), `PDUDisconnect`, wait OFFLINE,
`PDUDestroyComLogicalLink`, `PDURegisterEventCallback(hMod, hCLL, NULL)`, hCLL = 0, close the 6
handles, exit.

----------------------------------------------------------------------------------------------

## 11. Results back into the firmware (B.4)

### 11.1 RX ring

5 slots x 0x1398 bytes at `0xd77998` (data, u64 timestamp @+0x1388, len u16 @+0x1394), write
index `DAT_00d8167c`, read index `DAT_00d8167e`. A RESULT that passes the tag test is
`memcpy`'d as is (`NumDataBytes` bytes, no bounds check, no checksum strip/add, no header change)
into the write slot, stamped, len = NumDataBytes, write index++ mod 5. Slot busy -> log "PKW: rx
queue overflow" and the result is lost. The poll later copies a whole slot into the UART RX
staging buffer and the firmware reads it byte by byte (one byte time apart, baud floor 12300).

So the firmware sees exactly the bytes in `pDataBytes`. They must be the complete KWP frame:
format byte, optional target/source, optional length byte, service bytes, **checksum**. Tech2Win's
own locally generated frames (fake 7E, DEADBEEF filler) all carry a correct checksum too.

Header length rule Tech2Win uses everywhere: `hdr = ((fmt & 0xC0) ? 3 : 1) + ((fmt & 0x3F) == 0
? 1 : 0)`, SID = `data[hdr]`.

On each accepted RESULT also:
- First result after a STARTCOMM (`DAT_00d8166c`):
  - 5-baud: if `NumDataBytes == 2` build the TP template from `data[0]` = KB1, `data[1]` = KB2.
  - fast: if `data[hdr] == 0xC1` build the TP template from KB1 = `data[hdr+1]`, KB2 =
    `data[hdr+2]`. ECU/tester addresses default to `data[2]`/`data[1]` when 'IA'/'IS' are not set.
- If `data[hdr] == 0xC2` (StopCommunication positive response): cancel the TP timer, else
  (re)arm it: 100 ms for the first result after STARTCOMM, otherwise 0.8 x P3MAX (4 s default).

### 11.2 Echo

Tech2Win's 16550 model echoes every transmitted byte locally and the firmware consumes that echo
before any reply is offered. The DLL must not loop back TX (CP_Loopback and CP_TransmitIndEnable
are looked up but never set, leave them off) and must not report the request as a RESULT.

### 11.3 When a result is handed to the firmware (Poll `0x473250`)

`last` (`DAT_00d81688`) = time of the last firmware TX, the last hand-over, or UART/TPU events
0xd/0xe/0xf/0x10 (0x10 = firmware finished reading an RX message, i.e. also the end of the echo).
Poll runs only when no message is staged, the UART RX side is idle and the link is up.

- State 1, fast init: slot delivered only if **last + P2MIN < now < last + P2MAX**, where P2MAX
  is the raw firmware ISO_P2MAX (default 50 ms, max 1500 ms), not the >= 300 ms CP_P2Max. Late =
  never delivered (the firmware times out and its retries are dropped, see 11.6). Then state 2.
  Fast init on the wire (TIdle 1 + TInil 25 + TWup 25 + StartComm + ECU P2) takes 80-130 ms after
  the frame, so this only works if the firmware's ISO_P2MAX for the link is big enough. Nothing the
  DLL can change; deliver the STARTCOMM RESULT the moment the response is complete.
- State 1, 5-baud: sub-state machine emulating the handshake to the firmware:
  0: when last + W1min(60) < now < last + W1max(300): fake the 0x55 sync at the reported baud;
  1: after W2min(5): deliver KB1 as a 1-byte message; 2: after W3min(10): deliver KB2;
  3: wait until the firmware sends ~KB2 (UART event 0xf); 4: after W4min(25): deliver ~address
  as a 1-byte message, free the slot, state 2. That is why the 5-baud RESULT must be exactly
  `[KB1, KB2]`.
- State 2: if a fake 7E is pending (11.4) deliver it after last + P2MIN. Else if the ring has a
  slot: deliver after last + P2MIN (**no upper bound**). If the delivered frame is `7F xx 78`
  remember "response pending" (suppresses the internal TP). Else, while the firmware waits for an
  answer (set on each forwarded request): once now > last + P2_FAKE, hand the firmware the canned
  frame `80 F1 FF 05 FF DE AD BE EF AC` every time the window comes round, for at most 6.25 s
  after the first one. That keeps the firmware's P2 timer from expiring while the PC side is slow.
  The firmware's link layer accepts it and posts `F1 FF FF DE AD BE EF` to its `Rcv0`/`Rcv1`
  queue; the trap spy (event 0x12 -> `FUN_00477bf0` returns 0x15) swallows that q_send so the
  application never sees it.

### 11.4 Fake $7E for the firmware's own TesterPresent

In state 2, a firmware frame with SID 0x3E is **never sent to the DLL**. `FUN_00471430` builds the
positive response locally (swapped addresses, correct checksum) and hands it over after P2MIN:
`80 <req.src> <req.tgt> 01 7E cs`, `81 <req.src> <req.tgt> 7E cs`, `00 01 7E 7F` or `01 7E 7F`
depending on the request's header type (functional mode always `01 7E 7F`).

### 11.5 Tech2Win's own TesterPresent

Timer "Tester Present Timer", armed per 11.1. On expiry (thread case 2):
- if now > last + P3MAX: P3 expired -> 10.1 prologue incl. STOPCOMM; if still in state 1 log
  "PKW: internal tester present timeout during init" and force state 2.
- else if no "response pending": `DAT_00d81678 = 0`, re-arm at 0.8 x P3MAX, log
  "PKW2000_SendInternalTesterPresent()", cancel an executing SENDRECV, send the template as a
  SENDRECV with tag ->2 (table in 10).
Template (header type from KB1/KB2: `KB1 == 0xD0 && KB2 == 0x8F` -> 3; else t = 1, `+1` if
`(KB1&1)==0 || (KB1&2)`, `+2` if `(KB1&4)==0 || (KB1&8)`; f = 0xC0 if functional else 0):
type 4 `80|f ECU TESTER 01 3E cs`, type 3 `81|f ECU TESTER 3E cs`, type 2 `f 01 3E cs`, type 1
`f|1 3E cs`. ECU = 'IA'/'IR' (else source byte of the StartComm response), TESTER = 'IS' (else its
target byte).
The RESULT of this CoP (tag value 2) is dropped (never reaches the firmware) but it is what
clears `DAT_00d81678`; until then firmware requests are deferred (10.2). An ECU that ignores the
$3E therefore blocks Tech2Win until an ERROR item or a link re-enable.

### 11.6 STATUS / ERROR / timeouts

- STATUS items (FINISHED, CANCELLED, EXECUTING, IDLE, WAITING, CLL states): only matched by
  waits, no side effects.
- ERROR item on the CLL, any code, any CoP: cancel TP timer, stop the DEADBEEF filler, drop any
  staged/fake message, mark TX ready, and **set link-up = 0** (`DAT_00d816e8`); state 1 -> 2.
  With link-up 0 every firmware frame is dropped by `FUN_00471430` and nothing is delivered.
  Recovery only via firmware link disable/reset/close (state 2 -> 0) followed by link enable
  (event 9 sets link-up again and re-runs the init on the next firmware frame).
- No ERROR on a failed STARTCOMM is worse: Tech2Win stays in state 1 with "TX ready" cleared, so
  every firmware retry is dropped, and link disable does nothing in state 1. Emit an ERROR (e.g.
  PDU_ERR_EVT_INIT_ERROR 0x108 or RX_TIMEOUT 0x103) then FINISHED.
- For the internal TP (NumRecv 1) with no answer: ERROR + FINISHED is what a standard MVCI does
  and leads to the link reset above.
- For -1/-2 SENDRECVs: never ERROR on silence, stay EXECUTING until cancelled (for -2 a FINISHED
  after P2max is harmless).
- Tech2Win itself has no P2 timeout towards the DLL; the firmware owns all protocol timeouts.
  Its only timeouts are the 10 s idle in waits and the 6.25 s DEADBEEF limit.

----------------------------------------------------------------------------------------------

## 12. CLL status (B.5)

- Waits for STATUS `PDU_CLLST_ONLINE` (0x8051) after PDUConnect and `PDU_CLLST_OFFLINE` (0x8050)
  after PDUDisconnect (10 s idle limit each).
- Never waits for COMM_STARTED. Instead `PDUGetStatus(hMod, hCLL, UNDEF, &st, ..)` is polled
  before every init/stop and STOPCOMM is only sent when `st == PDU_CLLST_COMM_STARTED` (0x8052).
  So GetStatus on the CLL must report COMM_STARTED after a successful STARTCOMM and ONLINE after
  STOPCOMM, otherwise re-inits start without a STOPCOMM (harmless if the DLL accepts STARTCOMM in
  COMM_STARTED).
- Extra CLL status items (COMM_STARTED etc.) are harmless.

----------------------------------------------------------------------------------------------

## 13. Fast init vs 5-baud, and the break (B.6)

- Choice in event 9 (`FUN_00477bf0`, 75008) and Init: 5-baud iff link type 0x1308 and attribute
  'IB' ISO_INIT_BAUDRATE is valid and equals 5 or 200. Otherwise fast init.
- Conveyed only through `CP_InitializationSettings`: 2 = fast init (with CP_TIdle 1 ms, CP_TInil
  25 ms, CP_TWup 50 ms), 1 = 5-baud (with CP_5BaudMode 0, CP_5BaudInitBaudrate 10000/IB,
  CP_5BaudAddressPhys = 'IA', CP_TIdle 2.1 s, CP_W4Min 25 ms). Matches the ISO 22900-2 meaning
  (1 = 5 baud, 2 = fast).
- Break: the 16550 model measures the firmware's LCR set-break (20-30 ms window) and raises
  protocol event 1. The KW2000 event handler has no case for event 1, so **the break is ignored**.
  The wake-up pattern is entirely the MVCI's job, from the ComParams above, and the StartComm
  frame comes in as the STARTCOMM payload. The firmware's own retries (break + StartComm again)
  only matter if Tech2Win is ready to accept a frame again (state 1 "TX ready", see 11.6).
- 5-baud handshake bytes after the keybytes (inverted KB2 from the tester, inverted address from
  the ECU) are the MVCI's job on the wire; towards the firmware Tech2Win fakes them (11.3).

----------------------------------------------------------------------------------------------

## 14. Notes for a J2534 v04.04 backend (advice, not decomp facts)

- ISO14230 channel at CP_Baudrate (10400 until told otherwise), connect flags
  `ISO9141_K_LINE_ONLY` (see 8) and `ISO9141_NO_CHECKSUM` so frames go out exactly as Tech2Win
  built them (checksum included) and come back with the checksum byte.
- STARTCOMM fast: `SET_CONFIG` TIDLE/TINIL/TWUP from the ComParams (or TIDLE 0, see the goj2534
  bench notes: a default 300 ms TIDLE blows the window), `FAST_INIT` with the payload, RESULT =
  the response frame incl. header and checksum. Check whether your adapter wants the checksum
  in the FAST_INIT input when NO_CHECKSUM is set.
- STARTCOMM 5-baud: `FIVE_BAUD_INIT` with CP_5BaudAddressPhys, RESULT = `[KB1, KB2]`, keep the
  detected baud for `PDUGetComParam(CP_Baudrate)`.
- SENDRECV -1: write the frame, then post every received frame as a RESULT until cancelled. No
  loopback, no TX-done results.
- P2/P4 ComParams map to J2534 P4_MIN etc.; P3 is never sent, use the adapter default.
- CP_RepeatReqCountApp = 2: repeating a STARTCOMM costs time inside the firmware's window and
  repeating requests duplicates them on the bus; the firmware already retries. Probably ignore.

----------------------------------------------------------------------------------------------

## 15. Open points

- Runtime PIN_A/PIN_B values for SAAB 0x1308 links (8): capture `L0OP` msg[3].
- Whether the SAAB firmware's ISO_P2MAX for StartComm is wide enough for an MVCI fast init
  (11.3). If it is 50 ms, Tech2Win + any real MVCI can never deliver the StartComm reply.
- Whether a pin-8 probe and a pin-7 session share one KW2000 instance (7.4).
- `CP_HeaderFormatKW = 5`: value is literal, meaning depends on the MDI MDF; with raw frames it
  can be ignored.
