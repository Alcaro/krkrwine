<h1>Moved to https://git.disroot.org/Sir_Walrus/krkrwine</h1>

krkrwine
========

This program implements various missing functionality in Wine / Proton, such that videos work properly in Kirikiri- and BURIKO-based visual novels. (Other software is not targetted by krkrwine, but may be improved as well.)

While I am doing my best to upstream my fixes, Proton's release cycle is once a year; krkrwine allows me to run these VNs before Proton 10 releases in 2025.

Installation - Linux/Proton
---------------------------

Simply download and extract [a release](https://github.com/Alcaro/krkrwine/releases), run `./install.py ~/'steam/steamapps/common/Proton 9.0 (Beta)/'`, and it will install into every current and future game using that Proton. Installing krkrwine into Glorious Eggroll should work too, though this is untested.

krkrwine does not replace any existing files, and Steam's updater ignores files it doesn't recognize; therefore, you don't need to reinstall krkrwine if Proton updates. (However, if you reinstall Proton, or download a new Proton version, you obviously need to reinstall krkrwine too.)

krkrwine is only tested in Proton 9.0, and may misbehave in other Proton versions. For Proton 8.0, use an older release; for older Proton, upgrade.

Installation - Linux/Wine
-------------------------

Not recommended. I recommend using the latest Wine, such that krkrwine is unnecessary.

If you or your distro feel otherwise, you can use `./install.py --wine`. (It will obey WINEPREFIX, if set.)

Make sure to use the correct krkrwine version; krkrwine 9.0 needs Wine 9.0 or higher. For older Wine, use an older release.

Do not install krkrwine's codecs; instead (for Debian), `sudo apt install gstreamer1.0-plugins-good gstreamer1.0-plugins-bad gstreamer1.0-plugins-ugly gstreamer1.0-libav gstreamer1.0-plugins-good:i386 gstreamer1.0-plugins-bad:i386 gstreamer1.0-plugins-ugly:i386 gstreamer1.0-libav:i386`. For other distros, the packages have different names. (Note that on Debian, gstreamer1.0-libav is not part of, or a dependency of, gstreamer1.0-plugins-good, -bad, nor -ugly; it must be installed separately.)

Installation - macOS
--------------------

Not recommended. Feel free to try the Linux/Wine steps, but it's more untested than Linux/Wine; I can't help you if it breaks.

Installation - Windows
----------------------

Don't. All relevant functionality is already in place on every Windows edition I'm aware of, and some parts of this program depend on Wine internals that are different on Windows.

Compilation
-----------

- Download and extract your favorite release of Glorious Eggroll (I use 8.4; no real reason, I just picked one)
- sudo apt install make gcc libgstreamer1.0-dev gcc-multilib libgstreamer1.0-dev:i386 g++-mingw-w64-i686-win32 g++-mingw-w64-x86-64-win32
- make prepare EGGROLL=/home/user/steam/compatibilitytools.d/GE-Proton8-4/
- make
- ./install.py ~/'steam/steamapps/common/Proton 9.0 (Beta)/'
If desired, you may acquire i686-w64-mingw32-g++-win32 and x86_64-w64-mingw32-g++-win32 from elsewhere. I use https://winlibs.com/ in Wine. (Compiling with *-mingw32-g++-posix is not recommended, since it adds dependencies on libwinpthread-1.dll, and possibly others; I don't know if installing that lib into system32 breaks anything, and I don't feel like finding out.)

If you want to use krkrwine's GStreamer components under Wine for debugging purposes, copy or link them to ~/.local/share/gstreamer-1.0/plugins/. For anyone else, use gst-libav.

License
-------

krkrwine itself is LGPL-2.0, same as Wine and GStreamer. Some test programs are GPL-2.0, since they contain code copied from Kirikiri.

The Glorious-Eggroll subdirectory of the releases is, as the name implies, copied from a Glorious Eggroll release. They're either LGPL-2.0 or GPL-2.0, depending on how they were compiled; I didn't check.

Upstreaming
-----------

All relevant pieces of this project have been upstreamed. The bounties that were here have been claimed.
