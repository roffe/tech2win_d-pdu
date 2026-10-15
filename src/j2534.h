// SAE J2534-1 v04.04 + the J2534-2 bits we use. Only what this project needs.
#pragma once
#include <windows.h>

typedef struct {
    unsigned long ProtocolID;
    unsigned long RxStatus;
    unsigned long TxFlags;
    unsigned long Timestamp;
    unsigned long DataSize;
    unsigned long ExtraDataIndex;
    unsigned char Data[4128];
} PASSTHRU_MSG;

typedef struct { unsigned long Parameter, Value; } SCONFIG;
typedef struct { unsigned long NumOfParams; SCONFIG *ConfigPtr; } SCONFIG_LIST;
typedef struct { unsigned long NumOfBytes; unsigned char *BytePtr; } SBYTE_ARRAY;

typedef long (WINAPI *PTOPEN)(void *, unsigned long *);
typedef long (WINAPI *PTCLOSE)(unsigned long);
typedef long (WINAPI *PTCONNECT)(unsigned long, unsigned long, unsigned long, unsigned long, unsigned long *);
typedef long (WINAPI *PTDISCONNECT)(unsigned long);
typedef long (WINAPI *PTREADMSGS)(unsigned long, PASSTHRU_MSG *, unsigned long *, unsigned long);
typedef long (WINAPI *PTWRITEMSGS)(unsigned long, PASSTHRU_MSG *, unsigned long *, unsigned long);
typedef long (WINAPI *PTSTARTMSGFILTER)(unsigned long, unsigned long, PASSTHRU_MSG *, PASSTHRU_MSG *, PASSTHRU_MSG *, unsigned long *);
typedef long (WINAPI *PTREADVERSION)(unsigned long, char *, char *, char *);
typedef long (WINAPI *PTGETLASTERROR)(char *);
typedef long (WINAPI *PTIOCTL)(unsigned long, unsigned long, void *, void *);

// protocols
#define ISO9141     0x03
#define ISO14230    0x04
#define ISO9141_PS  0x8002
#define ISO14230_PS 0x8003

// connect flags
#define ISO9141_NO_CHECKSUM 0x0200
#define ISO9141_K_LINE_ONLY 0x1000

// RxStatus
#define TX_MSG_TYPE      0x0001
#define START_OF_MESSAGE 0x0002

#define PASS_FILTER 1

// ioctls
#define GET_CONFIG      0x01
#define SET_CONFIG      0x02
#define READ_VBATT      0x03
#define FIVE_BAUD_INIT  0x04
#define FAST_INIT       0x05
#define CLEAR_TX_BUFFER 0x07
#define CLEAR_RX_BUFFER 0x08

// config params
#define DATA_RATE     0x01
#define LOOPBACK      0x03
#define P1_MAX        0x07
#define P3_MIN        0x0A
#define P4_MIN        0x0C
#define W0            0x19
#define W1            0x0E
#define W2            0x0F
#define W3            0x10
#define W4            0x11
#define W5            0x12
#define TIDLE         0x13
#define TINIL         0x14
#define TWUP          0x15
#define PARITY        0x16
#define DATA_BITS     0x20
#define FIVE_BAUD_MOD 0x21
#define J1962_PINS    0x8001

// return codes
#define STATUS_NOERROR   0x00
#define ERR_FAILED       0x07
#define ERR_TIMEOUT      0x09
#define ERR_BUFFER_EMPTY 0x10
