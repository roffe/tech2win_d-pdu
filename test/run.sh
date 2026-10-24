#!/bin/sh
# End-to-end test under wine: setup.exe /install, then pduhost.exe drives the
# installed DLL like Tech2Win against the fake J2534 adapter. Throwaway prefix.
set -e
cd "$(dirname "$0")/.."
B=$PWD/build
export WINEPREFIX=$B/wine WINEDEBUG=-all WINEDLLOVERRIDES="mscoree,mshtml="
[ -d "$WINEPREFIX" ] || wineboot -i >/dev/null 2>&1

reg() { wine reg add "$@" /f /reg:32 >/dev/null; }
reg 'HKLM\SOFTWARE\PassThruSupport.04.04\Fake' /v Name /d 'Fake Mongoose'
reg 'HKLM\SOFTWARE\PassThruSupport.04.04\Fake' /v Vendor /d 'Test'
reg 'HKLM\SOFTWARE\PassThruSupport.04.04\Fake' /v FunctionLibrary /d "$(winepath -w "$B/fakej2534.dll")"

wine "$B/t2wdpdu-setup.exe" /install
echo "install ok"

PUB=$(winepath -u 'C:\users\Public')
ini() { # pin7 pin8 pin78 [FastInitPins, no 4th arg = DLL default 24:8 28:8 70:8]
    printf '[settings]\nAdapter=Fake Mongoose\nLog=1\n[kline]\nPin7=%s\nPin8=%s\nPin7and8=%s\n' "$1" "$2" "$3" >"$PUB/t2wdpdu.ini"
    [ $# -lt 4 ] || printf 'FastInitPins=%s\n' "$4" >>"$PUB/t2wdpdu.ini"
}
fail=0
check() { # name, env..., -- pduhost args
    name=$1; shift
    rm -f "$B/fake.log"
    if env FAKE_LOG="$B/fake.log" "$@" >"$B/host.log" 2>&1; then echo "PASS $name"; else
        echo "FAIL $name"; cat "$B/host.log"; echo "-- fake:"; cat "$B/fake.log" 2>/dev/null; fail=1; fi
}
H="wine $B/pduhost.exe"

ini 7 8 7
check "7+8 asked, DICE on 7, ABS forced to 8, back to 7 for DICE" $H pins=7,8 ecu=41,28,41 expect=ok j1962=0x0800
ini 7 8 7 ""
check "7+8 asked, default override -> pin 7, DICE answers" $H pins=7,8 ecu=41 expect=ok j1962=none
check "7+8 asked, unbridged ABS is silent" $H pins=7,8 ecu=28 expect=timeout
check "7+8 asked, bridged ABS answers on pin 7" FAKE_BRIDGED=1 $H pins=7,8 ecu=28 expect=ok j1962=none
check "8 asked -> pin 8 via ISO14230_PS" $H pins=8 ecu=28 expect=ok j1962=0x0800
check "CAN: GMLAN session on SW + MS + HS (ISO-TP, UUDT, HV wakeup, re-arm)" $H proto=can
check "KW82 SRS: 5-baud init, keywords, 8192 baud" $H proto=kw82 pins=7 ecu=41 expect=ok j1962=none
check "ECU ignores TesterPresent: empty tag-2 RESULT (also when cancelled), no ERROR" FAKE_NO_TP=1 $H pins=7 ecu=41 expect=ok tp=silent
ini 7 8 78
check "7+8 as asked -> GM II rejects K+L" $H pins=7,8 ecu=41 expect=connectfail
ini 7 7 7 ""
check "8 asked, override to 7 (bridged)" FAKE_BRIDGED=1 $H pins=8 ecu=28 expect=ok j1962=none

wine "$B/t2wdpdu-setup.exe" /uninstall
echo "uninstall ok"
exit $fail
