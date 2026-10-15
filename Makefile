# Cross-build with mingw-w64 (i686: Tech2Win and most J2534 DLLs are 32-bit).
CROSS   ?= i686-w64-mingw32-
CXX     := $(CROSS)g++
WINDRES := $(CROSS)windres
VERSION ?= $(shell git describe --tags --match "v*" --always --dirty 2>/dev/null || date +%Y%m%d)

CXXFLAGS := -std=c++17 -O2 -Wall -DAPP_VERSION=\"$(VERSION)\" -Isrc
# static runtime: no VC++/mingw redistributables needed on the target
LDFLAGS  := -static -s

OUT := build

all: $(OUT)/t2wdpdu.dll $(OUT)/t2wdpdu-setup.exe

$(OUT):
	mkdir -p $@

$(OUT)/t2wdpdu.dll: src/dll.cpp src/common.cpp src/dll.def src/*.h | $(OUT)
	$(CXX) $(CXXFLAGS) -shared -o $@ src/dll.cpp src/common.cpp src/dll.def $(LDFLAGS) -Wl,--enable-stdcall-fixup -lwinmm

$(OUT)/setup_res.o: src/setup.rc src/setup_res.h src/setup.manifest | $(OUT)
	$(WINDRES) -Isrc -o $@ src/setup.rc

$(OUT)/t2wdpdu-setup.exe: src/setup.cpp src/common.cpp $(OUT)/setup_res.o src/*.h | $(OUT)
	$(CXX) $(CXXFLAGS) -mwindows -o $@ src/setup.cpp src/common.cpp $(OUT)/setup_res.o $(LDFLAGS) -lcomctl32 -lshell32

# fake J2534 adapter + D-PDU host that drives the DLL the way Tech2Win does
$(OUT)/fakej2534.dll: test/fakej2534.cpp test/fakej2534.def src/j2534.h | $(OUT)
	$(CXX) $(CXXFLAGS) -shared -o $@ test/fakej2534.cpp test/fakej2534.def $(LDFLAGS) -Wl,--enable-stdcall-fixup

$(OUT)/pduhost.exe: test/pduhost.cpp src/pdu.h | $(OUT)
	$(CXX) $(CXXFLAGS) -o $@ test/pduhost.cpp $(LDFLAGS)

# runs under wine in a throwaway prefix
test: all $(OUT)/fakej2534.dll $(OUT)/pduhost.exe
	test/run.sh

dist: all
	cd $(OUT) && rm -f t2wdpdu-$(VERSION).zip && zip -j t2wdpdu-$(VERSION).zip t2wdpdu.dll t2wdpdu-setup.exe ../README.md

clean:
	rm -rf $(OUT)

.PHONY: all test dist clean
