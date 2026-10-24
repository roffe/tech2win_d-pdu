# t2wdpdu

D-PDU API driver for GM Tech2Win that talks to any J2534 v04.04 adapter. Drop in, install with the setup tool, pick your adapter in Tech2Win and go.

Written against how Tech2Win actually drives its D-PDU interface (pulled out of emulator.exe), not guessed from logs. Statically linked, so no Visual C++ redistributable needed.

## What works

- KWP2000 on K-Line (ISO 14230 fast init). That's almost every module on the OG 9-5 and OG 9-3.
- KW82 (5-baud init, 8192 baud). SRS on the OG 9-3 and early OG 9-5.
- ISO 9141 / J2190 links (KW08, KW94). Not tested on a car yet.
- GMLAN on CAN: HS-CAN (pins 6/14, 500k) and SW-CAN (pin 1, 33.3k incl. the high voltage wakeup). That's the NG 9-3 and the 9-5 NG. ISO-TP is done in the driver, so it works with any adapter that can do raw CAN, no flow control filter limits.
- MS-CAN (pins 3/11) needs an adapter with J2534-2 pin switching. SAAB never actually talks on it, so it's only opened if something does.

## Install

1. Grab the zip from [releases](https://github.com/roffe/tech2win_d-pdu/releases) (Nightly is the newest build), unzip it to a folder and run `t2wdpdu-setup.exe` (it asks for admin).
2. Check that your adapter shows up in the J2534 adapters list. If it's not there, the vendor driver isn't installed.
3. Set the K-Line pins (see below) and press Install.
4. Start Tech2Win. Pick your adapter in "Select Vehicle Communication Interface".

Tech2Win will pop up a warning about an "unvalidated vehicle communication device" once. That's normal for anything that isn't a GM MDI, just click it away.

Uninstall puts back whatever D-PDU driver you had before, so an MDI setup keeps working.

For scripts: `t2wdpdu-setup.exe /install` or `/uninstall`, exit code 0 means ok.

## K-Line pins

On the older SAAB's the K-Line sits on two pins: pin 7 for most modules (engine, DICE, SID...) and pin 8 for ABS/TC and friends.

Some K-Line links in Tech2Win ask for pin 7 and pin 8 at the same time. In J2534 terms that's K-Line on pin 7 plus L-Line on pin 8, and adapters with only one K-Line (Mongoose Pro GM II for example) refuse that, so you get nothing.

The setup tool lets you decide what to use for each case:

- Tech2Win asks for pin 7: use pin 7.
- Tech2Win asks for pin 8: use pin 8, if your adapter can switch K-Line to pin 8 (J2534-2 pin switching).
- Tech2Win asks for pins 7 + 8: use pin 7. That's the default.

If your OBD outlet has pin 7 and 8 bridged, set all three to pin 7, empty the fast init list below and you reach everything including the brakes, on any adapter.

Pin 7 uses plain ISO14230/ISO9141 which every adapter supports. Pin 8 and "pin 7 + L-line on 8" need an adapter with J2534-2 pin switching.

### Fast init forced pins

The Tech2 firmware does pick the right pin per module, but Tech2Win only builds its K-Line link once (with the pins of the first module, usually DICE on 7) and drops every pin change after that. So ABS gets fast inits on pin 7 and never answers.

The fix is the "Fast init to these addresses forces the pin" list: when a fast init goes to one of these addresses the driver moves the K-Line to that pin first, and back to the normal pin for everything else. Format is hex address:pin, default `24:8 28:8 70:8`, which is what the SAAB card puts on pin 8 (0x28 = ABS/TC and ESP, 0x70 = AHL). In the ini it's `FastInitPins` under `[kline]`. Empty = off.

## Problems?

The log is in `C:\Users\Public\t2wdpdu.log` ("Open log" in the setup tool). Switch "Write debug log" on, reproduce it, and post the log along with what car and module you tried and which adapter you have.

Settings live in `C:\Users\Public\t2wdpdu.ini` if you want to edit them by hand.

## Build

Needs mingw-w64 (i686) and make, builds from Linux:

```
make          # build/t2wdpdu.dll + build/t2wdpdu-setup.exe
make test     # runs install + a Tech2Win-like host against a fake adapter under wine
make dist     # zip for release
```

`notes/` has the decompiled Tech2Win D-PDU host behaviour the driver is built from, if you want the internals.
