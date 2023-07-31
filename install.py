#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.0-or-later

import argparse, subprocess, os, shutil

CLSID_FilterGraph         = "{E436EBB3-524F-11CE-9F53-0020AF0BA770}"
CLSID_FilterGraphNoThread = "{E436EBB8-524F-11CE-9F53-0020AF0BA770}"

def key_clsid(clsid, suffix="\\InprocServer32"):
	return "Software\\Classes\\CLSID\\"+clsid+suffix
def key_clsid_wow64(clsid, suffix="\\InprocServer32"):
	return "Software\\Classes\\Wow6432Node\\CLSID\\"+clsid+suffix

def escape_regstr(key):
	return key.replace("\\","\\\\")
def unescape_regstr(key):
	return key.replace("\\\\","\\")
def find_regkey(registry, key):
	idx1 = registry.index("["+escape_regstr(key)+"]")
	idx2 = registry.index("\n@=", idx1)+3
	idx3 = registry.index("\n", idx2)
	return idx2, idx3
def get_regkey(registry, key):
	idx2, idx3 = find_regkey(registry, key)
	val = registry[idx2:idx3]
	if val.startswith('"') and val.endswith('"'):
		return unescape_regstr(val[1:-1])
	else:
		1/0
def set_regkey(registry, key, value):
	idx2, idx3 = find_regkey(registry, key)
	return registry[:idx2] + '"'+escape_regstr(value)+'"' + registry[idx3:]

def silent_unlink(path):
	try:
		os.unlink(path)
	except FileNotFoundError:
		pass

def in_this_dir(name):
	path = os.path.realpath(os.path.dirname(__file__)+"/"+name)
	assert os.path.isfile(path)
	return path

def set_regkeys(wineprefix, dllpaths):
	registry = open(wineprefix+"/system.reg","rt").read()
	registry = set_regkey(registry, key_clsid(CLSID_FilterGraph), dllpaths["x86_64"])
	registry = set_regkey(registry, key_clsid(CLSID_FilterGraphNoThread), dllpaths["x86_64"])
	registry = set_regkey(registry, key_clsid_wow64(CLSID_FilterGraph), dllpaths["i386"])
	registry = set_regkey(registry, key_clsid_wow64(CLSID_FilterGraphNoThread), dllpaths["i386"])
	open(wineprefix+"/system.reg","wt").write(registry)


def gstreamer_install(install, devel):
	gst_dir = os.getenv("HOME")+"/.local/share/gstreamer-1.0/plugins"
	os.makedirs(gst_dir, exist_ok=True)
	silent_unlink(gst_dir+"/gstkrkr-i386.so")
	silent_unlink(gst_dir+"/gstkrkr-x86_64.so")
	if uninstall:
		pass
	elif devel:
		os.symlink(in_this_dir("gstkrkr-i386.so"), gst_dir + "/gstkrkr-i386.so")
		os.symlink(in_this_dir("gstkrkr-x86_64.so"), gst_dir + "/gstkrkr-x86_64.so")
	else:
		shutil.copy(in_this_dir("gstkrkr-i386.so"), gst_dir + "/gstkrkr-i386.so")
		shutil.copy(in_this_dir("gstkrkr-x86_64.so"), gst_dir + "/gstkrkr-x86_64.so")


def wine_install(wineprefix, unixpaths, unsafe, mode):
	if not unsafe:
		procs = subprocess.run(["ps", "-ef"], stdout=subprocess.PIPE).stdout
		if b'\\windows' in procs or b'wineserver' in procs or b'winedevice' in procs:
			print("Wine is currently running, close all Wine programs then run  wineserver -k  (if you did that already, kill them manually)")
			exit(1)
	
	if mode == "z":
		set_regkeys(wineprefix, { "x86_64": "Z:"+unixpaths["x86_64"].replace("/","\\"), "i386": "Z:"+unixpaths["i386"].replace("/","\\") })
	elif mode == "symlink":
		set_regkeys(wineprefix, { "x86_64": "C:\\windows\\system32\\krkrwine.dll", "i386": "C:\\windows\\system32\\krkrwine.dll" })
		silent_unlink(wineprefix + "/drive_c/windows/system32/krkrwine.dll")
		silent_unlink(wineprefix + "/drive_c/windows/syswow64/krkrwine.dll")
		os.symlink(unixpaths["x86_64"], wineprefix + "/drive_c/windows/system32/krkrwine.dll")
		os.symlink(unixpaths["i386"],   wineprefix + "/drive_c/windows/syswow64/krkrwine.dll")
	elif mode == "copy":
		set_regkeys(wineprefix, { "x86_64": "C:\\windows\\system32\\krkrwine.dll", "i386": "C:\\windows\\system32\\krkrwine.dll" })
		shutil.copy(unixpaths["x86_64"], wineprefix + "/drive_c/windows/system32/krkrwine.dll")
		shutil.copy(unixpaths["i386"],   wineprefix + "/drive_c/windows/syswow64/krkrwine.dll")
	else:
		1/0

def wine_uninstall(wineprefix):
	set_regkeys(wineprefix, { "x86_64": "C:\\windows\\system32\\quartz.dll", "i386": "C:\\windows\\system32\\quartz.dll" })
	silent_unlink(wineprefix + "/drive_c/windows/system32/krkrwine.dll")
	silent_unlink(wineprefix + "/drive_c/windows/syswow64/krkrwine.dll")


if __name__ == "__main__":
	parser = argparse.ArgumentParser()
	parser.add_argument('path', type=str, nargs='?', action="store", help="Path to Proton installation")
	parser.add_argument('--wine', action="store_true", help="Install to non-Protom Wine at ~/.wine or WINEPREFIX")
	parser.add_argument('--unsafe', action="store_true", help="Don't check if Wine is running before installing")
	parser.add_argument('--with-gstreamer', action="store_true", help="Install GStreamer plugins as well (not recommended, unless for development purposes; Wine only, not Proton)")
	parser.add_argument('--uninstall', action="store_true", help="Uninstall instead (Wine only, not Proton - sorry about that)")
	parser.add_argument('--devel', action="store_true", help="Point Wine to the DLLs' current location, instead of copying them into Wine's system32; allows easier updates of them, but you can't remove the krkrwine download directory (Wine only, not Proton)")
	
	args = parser.parse_args()
	
	if args.path is None and not args.wine:
		print("""
Usage:
./install.py ~/steam/steamapps/common/Proton\ 8.0/  # install krkrwine into every current and future game installed into that Proton version
./install.py --wine  # install into a non-Proton Wine
		""".strip())
		exit(0)
	
	if args.path is not None and args.wine:
		print("Can't specify both Proton path and installing to Wine; to specify the Wine path, set WINEPREFIX")
		exit(1)
	
	unixpaths = { "x86_64": in_this_dir("krkrwine-x86_64.dll"), "i386": in_this_dir("krkrwine-i386.dll") }
	
	if args.wine:
		wineprefix = os.getenv("WINEPREFIX")
		if not wineprefix:
			wineprefix = os.getenv("HOME")+"/.wine"
		
		if not os.path.isfile(wineprefix + "/system.reg"):
			print("That doesn't look like a Wine prefix")
			exit(1)
		
		if not args.uninstall:
			dllpaths = {}
			wine_install(wineprefix, unixpaths=unixpaths, unsafe=args.unsafe, mode=["copy", "z"][args.devel])
			if args.with_gstreamer:
				gstreamer_install(True, args.devel)
		else:
			wine_uninstall(wineprefix)
			gstreamer_install(False, False)
	else:
		if os.path.isfile(args.path + "/dist/share/default_pfx/system.reg"):
			# normal Proton
			proton_files = args.path + "/dist"
		elif os.path.isfile(args.path + "/files/share/default_pfx/system.reg"):
			# Glorious Eggroll (I don't know if the community Proton uses dist or files)
			proton_files = args.path + "/files"
		elif os.path.isfile(args.path + "/pfx/system.reg"):
			# it's a Proton prefix
			print("Don't do that, install to the Proton installation instead")
			exit(1)
		else:
			print("That doesn't look like Proton")
			exit(1)
		
		if args.uninstall:
			print("sorry, unimplemented - krkrwine installs itself to every Proton prefix where it's used, it's hard to identify which prefixes to uninstall from")
			exit(1)
		
		print("Installing krkrwine...")
		
		# it's a Proton installation
		wine_install(proton_files + "/share/default_pfx", unixpaths=unixpaths, unsafe=True, mode="copy")
		
		import_me = "import krkrwine\nkrkrwine.install_within_proton()\n"
		shutil.copy(__file__, args.path + "/krkrwine.py")
		try:
			prev_user_settings = open(args.path + "/user_settings.py", "rt").read()
		except FileNotFoundError:
			prev_user_settings = "user_settings = {}\n"
		if import_me not in prev_user_settings:
			open(args.path + "/user_settings.py", "wt").write(import_me + prev_user_settings)
		
		shutil.copy(in_this_dir("gstkrkr-x86_64.so"), proton_files + "/lib64/gstreamer-1.0/gstkrkr-x86_64.so")
		shutil.copy(in_this_dir("gstkrkr-i386.so"),   proton_files +   "/lib/gstreamer-1.0/gstkrkr-i386.so")
		
		eggroll_files = [
			"lib64/gstreamer-1.0/libgstmpegpsdemux.so", "lib/gstreamer-1.0/libgstmpegpsdemux.so",
			"lib64/gstreamer-1.0/libgstasf.so",         "lib/gstreamer-1.0/libgstasf.so",
			"lib64/libgstcodecparsers-1.0.so.0",        "lib/libgstcodecparsers-1.0.so.0",
			"lib64/libavcodec.so.58",                   "lib/libavcodec.so.58",
			"lib64/libavutil.so.56",                    "lib/libavutil.so.56",
			"lib64/libavfilter.so.7",                   "lib/libavfilter.so.7",
			"lib64/libavformat.so.58",                  "lib/libavformat.so.58",
			"lib64/libavdevice.so.58",                  "lib/libavdevice.so.58",
			"lib64/libswresample.so.3",                 "lib/libswresample.so.3",
			"lib64/libswscale.so.5",                    "lib/libswscale.so.5",
		]
		if all([os.path.isfile(proton_files+"/"+f) for f in eggroll_files]):
			print("Skipping Glorious Eggroll components, they're already in place")
			pass  # if the Glorious Eggroll components are already in place (for example because this is a GE, or it's installed already), just leave them
		elif any([os.path.isfile(proton_files+"/"+f) for f in eggroll_files]):
			print("Partial Glorious Eggroll components installation? Either Proton updated in a way krkrwine doesn't recognize, or your installation is damaged")
			exit(1)
		else:
			print("Installing Glorious Eggroll components...")
			for dst_name in eggroll_files:
				src_name = "Glorious-Eggroll/"+dst_name.replace("gstreamer-1.0/","").replace("lib/","i386-").replace("lib64/","x86_64-")
				shutil.copy(in_this_dir(src_name), proton_files+"/"+dst_name)
	
	print("The operation completed successfully")

def install_within_proton():
	wineprefix = os.environ["STEAM_COMPAT_DATA_PATH"] + "/pfx"
	unixpaths = {}
	unixpaths["x86_64"] = in_this_dir("dist/share/default_pfx/drive_c/windows/system32/krkrwine.dll")
	unixpaths["i386"]   = in_this_dir("dist/share/default_pfx/drive_c/windows/syswow64/krkrwine.dll")
	if os.path.isdir(wineprefix):
		# if not, the registry and DLLs will be copied from the upstream Proton installation
		wine_install(wineprefix, unixpaths=unixpaths, unsafe=True, mode="symlink")
	os.environ["PROTON_NO_STEAM_FFMPEG"] = "1"
