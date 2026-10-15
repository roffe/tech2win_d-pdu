// ISO 22900-2 (D-PDU API) types and constants. Only what this project needs.
#pragma once
#include <windows.h>

typedef unsigned char UNUM8;
typedef unsigned short UNUM16;
typedef unsigned long UNUM32;
typedef signed long SNUM32;
typedef char CHAR8;

typedef UNUM32 T_PDU_ERROR;
typedef UNUM32 T_PDU_IT;
typedef UNUM32 T_PDU_STATUS;
typedef UNUM32 T_PDU_OBJT;
typedef UNUM32 T_PDU_EVT_DATA;
typedef UNUM32 T_PDU_ERR_EVT;

#define PDU_ID_UNDEF     0xFFFFFFFE
#define PDU_HANDLE_UNDEF 0xFFFFFFFF

// item types
#define PDU_IT_IO_UNUM32       0x1000
#define PDU_IT_IO_PROG_VOLTAGE 0x1001
#define PDU_IT_IO_BYTEARRAY    0x1002
#define PDU_IT_IO_FILTER       0x1003
#define PDU_IT_RSC_STATUS      0x1100
#define PDU_IT_PARAM           0x1200
#define PDU_IT_RESULT          0x1300
#define PDU_IT_STATUS          0x1301
#define PDU_IT_ERROR           0x1302
#define PDU_IT_INFO            0x1303
#define PDU_IT_RSC_ID          0x1400
#define PDU_IT_RSC_CONFLICT    0x1500
#define PDU_IT_MODULE_ID       0x1600
#define PDU_IT_UNIQUE_RESP_ID_TABLE 0x1700

// ComPrimitive types
#define PDU_COPT_STARTCOMM     0x8001
#define PDU_COPT_STOPCOMM      0x8002
#define PDU_COPT_UPDATEPARAM   0x8003
#define PDU_COPT_SENDRECV      0x8004
#define PDU_COPT_DELAY         0x8005
#define PDU_COPT_RESTORE_PARAM 0x8006

// object types
#define PDU_OBJT_PROTOCOL 0x8021
#define PDU_OBJT_BUSTYPE  0x8022
#define PDU_OBJT_IO_CTRL  0x8023
#define PDU_OBJT_COMPARAM 0x8024
#define PDU_OBJT_PINTYPE  0x8025
#define PDU_OBJT_RESOURCE 0x8026

// status
#define PDU_COPST_IDLE         0x8010
#define PDU_COPST_EXECUTING    0x8011
#define PDU_COPST_FINISHED     0x8012
#define PDU_COPST_CANCELLED    0x8013
#define PDU_COPST_WAITING      0x8014
#define PDU_CLLST_OFFLINE      0x8050
#define PDU_CLLST_ONLINE       0x8051
#define PDU_CLLST_COMM_STARTED 0x8052
#define PDU_MODST_READY        0x8060
#define PDU_MODST_NOT_READY    0x8061
#define PDU_MODST_NOT_AVAIL    0x8062
#define PDU_MODST_AVAIL        0x8063

#define PDU_EVT_DATA_AVAILABLE 0x0801
#define PDU_EVT_DATA_LOST      0x0802

// ComParam data types / classes
#define PDU_PT_UNUM8       0x101
#define PDU_PT_SNUM8       0x102
#define PDU_PT_UNUM16      0x103
#define PDU_PT_SNUM16      0x104
#define PDU_PT_UNUM32      0x105
#define PDU_PT_SNUM32      0x106
#define PDU_PT_BYTEFIELD   0x107
#define PDU_PT_STRUCTFIELD 0x108
#define PDU_PT_LONGFIELD   0x109
#define PDU_PC_TIMING  1
#define PDU_PC_INIT    2
#define PDU_PC_COM     3
#define PDU_PC_ERRHDL  4
#define PDU_PC_BUSTYPE 5

// function return codes
#define PDU_STATUS_NOERROR             0x00
#define PDU_ERR_FCT_FAILED             0x01
#define PDU_ERR_PDUAPI_NOT_CONSTRUCTED 0x20
#define PDU_ERR_SHARING_VIOLATION      0x21
#define PDU_ERR_CLL_NOT_CONNECTED      0x40
#define PDU_ERR_CLL_NOT_STARTED        0x41
#define PDU_ERR_INVALID_PARAMETERS     0x50
#define PDU_ERR_INVALID_HANDLE         0x60
#define PDU_ERR_VALUE_NOT_SUPPORTED    0x61
#define PDU_ERR_ID_NOT_SUPPORTED       0x62
#define PDU_ERR_COMPARAM_NOT_SUPPORTED 0x63
#define PDU_ERR_EVENT_QUEUE_EMPTY      0x71
#define PDU_ERR_MODULE_NOT_CONNECTED   0xA3

// error events
#define PDU_ERR_EVT_NOERROR          0x000
#define PDU_ERR_EVT_FRAME_STRUCT     0x100
#define PDU_ERR_EVT_TX_ERROR         0x101
#define PDU_ERR_EVT_RX_TIMEOUT       0x103
#define PDU_ERR_EVT_RX_ERROR         0x104
#define PDU_ERR_EVT_PROT_ERR         0x105
#define PDU_ERR_EVT_LOST_COMM_TO_VCI 0x106
#define PDU_ERR_EVT_INIT_ERROR       0x108

typedef struct { T_PDU_IT ItemType; } PDU_ITEM;
typedef struct { T_PDU_IT ItemType; void *pData; } PDU_DATA_ITEM;
typedef struct { UNUM32 DataSize; UNUM8 *pData; } PDU_IO_BYTEARRAY_DATA;

typedef struct { UNUM32 ParamMaxLen, ParamActLen; UNUM8 *pDataArray; } PDU_PARAM_BYTEFIELD_DATA;
typedef struct { UNUM32 ParamMaxLen, ParamActLen; UNUM32 *pDataArray; } PDU_PARAM_LONGFIELD_DATA;
typedef struct { UNUM32 ComParamStructType, ParamMaxEntries, ParamActEntries; void *pStructArray; } PDU_PARAM_STRUCTFIELD_DATA;

typedef struct {
    T_PDU_IT ItemType;
    UNUM32 ComParamId;
    UNUM32 ComParamDataType;
    UNUM32 ComParamClass;
    void *pComParamData;
} PDU_PARAM_ITEM;

typedef struct {
    UNUM32 ModuleTypeId;
    UNUM32 hMod;
    CHAR8 *pVendorModuleName;
    CHAR8 *pVendorAdditionalInfo;
    T_PDU_STATUS ModuleStatus;
} PDU_MODULE_DATA;
typedef struct { T_PDU_IT ItemType; UNUM32 NumEntries; PDU_MODULE_DATA *pModuleData; } PDU_MODULE_ITEM;

typedef struct { UNUM32 hMod, NumIds; UNUM32 *pResourceIdArray; } PDU_RSC_ID_ITEM_DATA;
typedef struct { T_PDU_IT ItemType; UNUM32 NumModules; PDU_RSC_ID_ITEM_DATA *pResourceIdDataArray; } PDU_RSC_ID_ITEM;
typedef struct { UNUM32 hMod, ResourceId, ResourceStatus; } PDU_RSC_STATUS_DATA;
typedef struct { T_PDU_IT ItemType; UNUM32 NumEntries; PDU_RSC_STATUS_DATA *pResourceStatusData; } PDU_RSC_STATUS_ITEM;
typedef struct { UNUM32 hMod, ResourceId; } PDU_RSC_CONFLICT_DATA;
typedef struct { T_PDU_IT ItemType; UNUM32 NumEntries; PDU_RSC_CONFLICT_DATA *pRscConflictData; } PDU_RSC_CONFLICT_ITEM;
typedef struct { UNUM32 UniqueRespIdentifier, NumParamItems; PDU_PARAM_ITEM *pParams; } PDU_ECU_UNIQUE_RESP_DATA;
typedef struct { T_PDU_IT ItemType; UNUM32 NumEntries; PDU_ECU_UNIQUE_RESP_DATA *pUniqueData; } PDU_UNIQUE_RESP_ID_TABLE_ITEM;

typedef struct { UNUM32 DLCPinNumber, DLCPinTypeId; } PDU_PIN_DATA;
typedef struct { UNUM32 BusTypeId, ProtocolId, NumPinData; PDU_PIN_DATA *pDLCPinData; } PDU_RSC_DATA;

typedef struct { UNUM32 NumFlagBytes; UNUM8 *pFlagData; } PDU_FLAG_DATA;

typedef struct {
    T_PDU_IT ItemType;
    UNUM32 hCop;
    void *pCoPTag;
    UNUM32 Timestamp;
    void *pData;
} PDU_EVENT_ITEM;

typedef struct { UNUM32 InfoCode, ExtraInfoData; } PDU_INFO_DATA;
typedef struct { T_PDU_ERR_EVT ErrorCodeId; UNUM32 ExtraErrorInfoId; } PDU_ERROR_DATA;
typedef struct { UNUM32 NumHeaderBytes, NumFooterBytes; UNUM8 *pHeaderBytes, *pFooterBytes; } PDU_EXTRA_INFO;

typedef struct {
    PDU_FLAG_DATA RxFlag;
    UNUM32 UniqueRespIdentifier;
    UNUM32 AcceptanceId;
    PDU_FLAG_DATA TimestampFlags;
    UNUM32 TxMsgDoneTimestamp;
    UNUM32 StartMsgTimestamp;
    PDU_EXTRA_INFO *pExtraInfo;
    UNUM32 NumDataBytes;
    UNUM8 *pDataBytes;
} PDU_RESULT_DATA;

typedef struct {
    UNUM32 MVCI_Part1StandardVersion;
    UNUM32 MVCI_Part2StandardVersion;
    UNUM32 HwSerialNumber;
    CHAR8 HwName[64];
    UNUM32 HwVersion;
    UNUM32 HwDate;
    UNUM32 HwInterface;
    CHAR8 FwName[64];
    UNUM32 FwVersion;
    UNUM32 FwDate;
    CHAR8 VendorName[64];
    CHAR8 PDUApiSwName[64];
    UNUM32 PDUApiSwVersion;
    UNUM32 PDUApiSwDate;
} PDU_VERSION_DATA;

typedef struct {
    UNUM32 ResponseType;
    UNUM32 AcceptanceId;
    UNUM32 NumMaskPatternBytes;
    UNUM8 *pMaskData;
    UNUM8 *pPatternData;
    UNUM32 NumUniqueRespIds;
    UNUM32 *pUniqueRespIds;
} PDU_EXP_RESP_DATA;

typedef struct {
    UNUM32 Time;
    SNUM32 NumSendCycles;
    SNUM32 NumReceiveCycles;
    UNUM32 TempParamUpdate;
    PDU_FLAG_DATA TxFlag;
    UNUM32 NumPossibleExpectedResponses;
    PDU_EXP_RESP_DATA *pExpectedResponseArray;
} PDU_COP_CTRL_DATA;

typedef void(__stdcall *CALLBACKFNC)(T_PDU_EVT_DATA eventType, UNUM32 hMod, UNUM32 hCLL, void *pCllTag, void *pAPITag);
