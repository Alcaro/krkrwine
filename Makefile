# SPDX-License-Identifier: LGPL-2.0-or-later

FLAGS := -Wall
ifeq ($(OPT),1)
  FLAGS := -Os -s -g0 -fno-rtti -Wall
endif

all: gstkrkr-x86_64.so gstkrkr-i386.so krkrwine-x86_64.dll krkrwine-i386.dll

clean:
	-rm gstkrkr-x86_64.so gstkrkr-i386.so krkrwine-x86_64.dll krkrwine-i386.dll
	-rm run32.exe run64.exe krmovie.dll

MINGW32 = i686-w64-mingw32-g++-win32 -std=c++20 -fno-exceptions -gdwarf-4 -static-libgcc -static-libstdc++ $(FLAGS)
MINGW64 = x86_64-w64-mingw32-g++-win32 -std=c++20 -fno-exceptions -gdwarf-4 -static-libgcc -static-libstdc++ $(FLAGS)

gstkrkr-x86_64.so: gstkrkr.c Makefile
	gcc gstkrkr.c -shared -fPIC -std=c99 -DPLUGINARCH=x86_64 -o gstkrkr-x86_64.so -g $(shell pkg-config --cflags --libs gstreamer-1.0) -fvisibility=hidden -Wl,--no-undefined $(FLAGS)

# must use -std=c##, the default (gnu##) predefines the symbol i386
gstkrkr-i386.so: gstkrkr.c Makefile
	gcc -m32 gstkrkr.c -shared -fPIC -std=c99 -DPLUGINARCH=i386 -o gstkrkr-i386.so $(shell pkg-config --personality=i386-linux-gnu --cflags --libs gstreamer-1.0) -fvisibility=hidden -Wl,--no-undefined $(FLAGS)

krkrwine-x86_64.dll: krkrwine.cpp Makefile
	$(MINGW64) krkrwine.cpp -shared -fPIC -o krkrwine-x86_64.dll -lole32

krkrwine-i386.dll: krkrwine.cpp Makefile
	$(MINGW32) krkrwine.cpp -shared -fPIC -o krkrwine-i386.dll -lole32

prepare:
	mkdir Glorious-Eggroll/
	cp $(EGGROLL)/files/lib64/gstreamer-1.0/libgstmpegpsdemux.so        Glorious-Eggroll/x86_64-libgstmpegpsdemux.so
	cp $(EGGROLL)/files/lib64/gstreamer-1.0/libgstasf.so                Glorious-Eggroll/x86_64-libgstasf.so
	cp $(EGGROLL)/files/lib64/gstreamer-1.0/libgstvideoparsersbad.so    Glorious-Eggroll/x86_64-libgstvideoparsersbad.so
	cp $(EGGROLL)/files/lib64/libgstcodecparsers-1.0.so.0               Glorious-Eggroll/x86_64-libgstcodecparsers-1.0.so.0
	cp $(EGGROLL)/files/lib64/libavcodec.so.58                          Glorious-Eggroll/x86_64-libavcodec.so.58
	cp $(EGGROLL)/files/lib64/libavutil.so.56                           Glorious-Eggroll/x86_64-libavutil.so.56
	cp $(EGGROLL)/files/lib64/libavfilter.so.7                          Glorious-Eggroll/x86_64-libavfilter.so.7
	cp $(EGGROLL)/files/lib64/libavformat.so.58                         Glorious-Eggroll/x86_64-libavformat.so.58
	cp $(EGGROLL)/files/lib64/libavdevice.so.58                         Glorious-Eggroll/x86_64-libavdevice.so.58
	cp $(EGGROLL)/files/lib64/libswresample.so.3                        Glorious-Eggroll/x86_64-libswresample.so.3
	cp $(EGGROLL)/files/lib64/libswscale.so.5                           Glorious-Eggroll/x86_64-libswscale.so.5
	cp $(EGGROLL)/files/lib/gstreamer-1.0/libgstmpegpsdemux.so          Glorious-Eggroll/i386-libgstmpegpsdemux.so
	cp $(EGGROLL)/files/lib/gstreamer-1.0/libgstasf.so                  Glorious-Eggroll/i386-libgstasf.so
	cp $(EGGROLL)/files/lib/lib/gstreamer-1.0/libgstvideoparsersbad.so  Glorious-Eggroll/i386-libgstvideoparsersbad.so
	cp $(EGGROLL)/files/lib/libgstcodecparsers-1.0.so.0                 Glorious-Eggroll/i386-libgstcodecparsers-1.0.so.0
	cp $(EGGROLL)/files/lib/libavcodec.so.58                            Glorious-Eggroll/i386-libavcodec.so.58
	cp $(EGGROLL)/files/lib/libavutil.so.56                             Glorious-Eggroll/i386-libavutil.so.56
	cp $(EGGROLL)/files/lib/libavfilter.so.7                            Glorious-Eggroll/i386-libavfilter.so.7
	cp $(EGGROLL)/files/lib/libavformat.so.58                           Glorious-Eggroll/i386-libavformat.so.58
	cp $(EGGROLL)/files/lib/libavdevice.so.58                           Glorious-Eggroll/i386-libavdevice.so.58
	cp $(EGGROLL)/files/lib/libswresample.so.3                          Glorious-Eggroll/i386-libswresample.so.3
	cp $(EGGROLL)/files/lib/libswscale.so.5                             Glorious-Eggroll/i386-libswscale.so.5


krkrwine.tar.gz: | Glorious-Eggroll/x86_64-libgstmpegpsdemux.so
	$(MAKE) clean
	$(MAKE) OPT=1 -j4
	-rm krkrwine.tar.gz
	tar -czvf krkrwine.tar.gz --transform='s%^%krkrwine/%' --show-transformed-names README.md install.py gstkrkr-i386.so gstkrkr-x86_64.so krkrwine-i386.dll krkrwine-x86_64.dll Glorious-Eggroll/


# The following is various debug tools. If you're just trying to compile krkrwine, you don't need them;
#  if you're trying to debug or patch this program, you may find some of them useful. (Some of them are hardcoded for my own use.)

run32.exe: runner.cpp Makefile
	$(MINGW32) runner.cpp -o run32.exe -lole32 -ld3d9

run64.exe: runner.cpp Makefile
	$(MINGW64) runner.cpp -o run64.exe -lole32 -ld3d9 -lgdi32

run: krkrwine-x86_64.dll gstkrkr-x86_64.so run64.exe
	GST_DEBUG_NO_COLOR=1 WINEDEBUG=+warn,+error,+fixme timeout 10 wine run64.exe 2>&1 | guidfilt

run32: krkrwine-i386.dll gstkrkr-i386.so run32.exe
	GST_DEBUG_NO_COLOR=1 timeout 10 wine run32.exe 2>&1 | guidfilt

runv: krkrwine-x86_64.dll gstkrkr-x86_64.so run64.exe
	GST_DEBUG_NO_COLOR=1 GST_DEBUG=6 WINEDEBUG=trace+quartz,warn+quartz timeout 10 wine run64.exe 2>&1 | guidfilt | head -n2000 | tee e.log

mk/microkiri.exe:
	echo 'Download and extract microkiri from https://bugs.winehq.org/show_bug.cgi?id=9127#c102 to the mk/ subdirectory'
	mkdir mk
	false
mk/_rmovie.dll: mk/microkiri.exe
	mv mk/krmovie.dll mk/_rmovie.dll
krmovie.dll: fake-krmovie.cpp Makefile
	$(MINGW32) fake-krmovie.cpp -shared -fPIC -o krmovie.dll -lole32 -ld3d9
rmk: krmovie.dll krkrwine-i386.dll mk/_rmovie.dll
	-rm mk/krmovie.dll
	cp krmovie.dll mk/krmovie.dll
	LC_ALL=ja_JP wine mk/microkiri.exe

rmkd: krmovie.dll krkrwine-i386.dll mk/_rmovie.dll
	-rm mk/krmovie.dll
	cp krmovie.dll mk/krmovie.dll
	LC_ALL=ja_JP wine ~/tools/mingw64-11.2.0/bin/gdb.exe -ex "set disassemble-next-line on" -ex "set disassembly-flavor intel" mk/microkiri.exe

rwa: krkrwine-i386.dll gstkrkr-i386.so krmovie.dll
	LC_ALL=ja_JP wine /games/wine/waga/waga/waga.exe
rwas: krkrwine-i386.dll gstkrkr-i386.so krmovie.dll
	LC_ALL=ja_JP timeout 7 wine /games/wine/waga/waga/waga.exe

wau:
	mv /games/wine/waga/waga/plugin/_rmovie.dll /games/wine/waga/waga/plugin/krmovie.dll
wai:
	mv -n /games/wine/waga/waga/plugin/krmovie.dll /games/wine/waga/waga/plugin/_rmovie.dll
	ln -s $(shell realpath .)/krmovie.dll /games/wine/waga/waga/plugin/krmovie.dll

X1_HOME = /home/walrus/mount/x1/home/x1/
X1_PROTON = $(X1_HOME)steam/steamapps/common/"Proton 9.0 (Beta)"/
X1_TARGET = $(X1_PROTON)krkrwine/
X1_DLL = $(X1_TARGET)krkrwine-i386.dll
X1_GST = $(X1_TARGET)gstkrkr-i386.so
X1_EXE = $(X1_HOME)Desktop/a.exe
ax1: run32.exe krkrwine-i386.dll gstkrkr-i386.so
	-rm $(X1_DLL) $(X1_EXE) $(X1_GST)
	cp krkrwine-i386.dll $(X1_DLL)
	cp run32.exe $(X1_EXE)
	cp gstkrkr-i386.so $(X1_GST)

installx1: all
	./install.py $(X1_PROTON)

X2_HOME = /home/walrus/mount/x2/home/x2/
X2_PROTON = $(X2_HOME)steam/steamapps/common/"Proton 9.0 (Beta)"/
X2_WAGA = $(X2_HOME)steam/steamapps/common/"WAGAMAMA HIGH SPEC"

installx2: all
	./install.py $(X2_PROTON)

waux2:
	mv $(X2_WAGA)/plugin/_rmovie.dll $(X2_WAGA)/plugin/krmovie.dll
waix2: krmovie.dll
	mv -n $(X2_WAGA)/plugin/krmovie.dll $(X2_WAGA)/plugin/_rmovie.dll
	cp krmovie.dll $(X2_WAGA)/plugin/krmovie.dll
