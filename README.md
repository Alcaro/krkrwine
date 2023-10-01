krkrwine
========

This program implements various missing functionality in Wine / Proton, such that videos work properly in Kirikiri-based visual novels. (Other games are not targetted by krkrwine, but they may be improved as well.)

Installation - Linux/Proton
---------------------------

Simply download and extract [a release](https://github.com/Alcaro/krkrwine/releases), run ./install.py ~/steam/steamapps/common/Proton\ 8.0/, and it will install into every current and future game using that Proton. Installing krkrwine into Glorious Eggroll should work too, though this is untested.

krkrwine does not replace any existing files, and Steam's updater ignores files it doesn't recognize; therefore, you don't need to reinstall krkrwine if Proton updates. (However, if you reinstall Proton, or download a new Proton version, you obviously need to reinstall krkrwine too.)

krkrwine is only tested in Proton 8.0; I don't think it'll break too hard in anything else, but it's untested.

Installation - Linux/Wine
-------------------------

On Debian,

- Download and extract [a release](https://github.com/Alcaro/krkrwine/releases)
- sudo apt install gstreamer1.0-plugins-good gstreamer1.0-plugins-bad gstreamer1.0-plugins-ugly gstreamer1.0-libav gstreamer1.0-plugins-good:i386 gstreamer1.0-plugins-bad:i386 gstreamer1.0-plugins-ugly:i386 gstreamer1.0-libav:i386
- ./install.py --wine

On other distros, the package names are different.

Note that on Debian, gstreamer1.0-libav is not part of, or a dependency of, gstreamer1.0-plugins-good, -bad, nor -ugly; it must be installed separately. I haven't checked other distros.

install.py will obey the WINEPREFIX environment variable, if set.

krkrwine's GStreamer component will not be installed under Wine; it does nothing useful if you have the above plugins and don't have protonmediaconverter. If you want them anyways for development purposes, you can copy or link them to ~/.local/share/gstreamer-1.0/plugins/.

Installation - macOS
--------------------

See the Linux/Wine steps, it'll probably work. However, it's untested, and I can't help you if it breaks.

Installation - Windows
----------------------

Don't. All relevant functionality is already in place on every Windows edition I'm aware of, and some parts of this program depend on Wine internals that are different on Windows.

Compilation
-----------

- Download and extract your favorite release of Glorious Eggroll (I use 8.4; no real reason, I just picked one)
- sudo apt install make gcc libgstreamer1.0-dev gcc-multilib libgstreamer1.0-dev:i386 g++-mingw-w64-i686-win32 g++-mingw-w64-x86-64-win32
- make prepare EGGROLL=/home/user/steam/compatibilitytools.d/GE-Proton8-4/
- make
- ./install.py /home/user/steam/steamapps/common/Proton\ 8.0/
If desired, you may acquire i686-w64-mingw32-g++-win32 and x86_64-w64-mingw32-g++-win32 programs from elsewhere. I use https://winlibs.com/ in Wine. (Compiling with *-mingw32-g++-posix is not recommended, since it adds dependencies on libwinpthread-1.dll, and possibly others; I don't know if installing that lib into system32 breaks anything, and I don't feel like finding out.)

License
-------

krkrwine itself is LGPL-2.0, same as Wine and GStreamer. Some test programs are GPL-2.0, since they contain code copied from Kirikiri.

The Glorious-Eggroll subdirectory of the releases is, as the name implies, copied from a Glorious Eggroll release. They're either LGPL-2.0 or GPL-2.0, depending on how they were compiled; I didn't check.

Upstreaming
-----------

Ideally, this project would be unnecessary (other than the codec installation). As such, I'm offering bounties on fixing the Windows-side issues in upstream Wine.

- $250 - CLSID_MPEG1Splitter video output, and CLSID_CMpegVideoCodec (these objects may use GStreamer, of course)
- $25 - CLSID_MPEG1Splitter IAMStreamSelect (doesn't need to be fully implemented, just needs the parts Kirikiri uses)
- ~~$25 - CLSID_VideoMixingRenderer9 ChangeD3DDevice and NotifyEvent~~ Resolved and claimed
- $500 - WMCreateSyncReader compressed output, CLSID_CWMADecMediaObject, and CLSID_CWMVDecMediaObject
- $100 - Direct3D 9 on WS_CHILD windows under wined3d in the Debian package - the bug doesn't reproduce if I compile Wine from source
- $25 - make WMSyncReader resize its allocator, so it can output RGB32 properly
- $100 - figure out what's going on with the memory allocator and VFW_E_NOT_COMMITTED, and solve it
- $0 - anything involving gstkrkr and Proton's GStreamer. That's a patent issue; it's a question for lawyers, not programmers. It's only needed in Proton, not vanilla Wine.
- Anything that Kirikiri needs but isn't in the above list - that's a bug in this readme, contact me

though I'm working on them myself, so fair chance I'll finish first.

You are allowed, but not required, to base such efforts on this project. My architecture is very different from Wine's existing objects, and many of them are implemented in an awful way, so most of my code is unusable; but you're welcome to look for hints on the objects' expected behavior, or otherwise use them to help implement yours. You may also use the Kirikiri source code, <https://github.com/krkrz/krkr2/tree/master/kirikiri2/trunk>

To claim a bounty, post at <https://github.com/Alcaro/krkrwine/discussions/1>, or email me at sir@walrus.se. These bounties can be combined with similar offers made by others.

If I see any of the above implemented, but nobody claims the bounty, I will wait one month, then donate it to Wine.
