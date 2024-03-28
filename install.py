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
	assert os.path.exists(path)
	return path

def set_regkeys(wineprefix, dllpaths):
	registry = open(wineprefix+"/system.reg","rt").read()
	orig_registry = registry
	registry = set_regkey(registry, key_clsid(CLSID_FilterGraph), dllpaths["x86_64"])
	registry = set_regkey(registry, key_clsid(CLSID_FilterGraphNoThread), dllpaths["x86_64"])
	registry = set_regkey(registry, key_clsid_wow64(CLSID_FilterGraph), dllpaths["i386"])
	registry = set_regkey(registry, key_clsid_wow64(CLSID_FilterGraphNoThread), dllpaths["i386"])
	if registry != orig_registry:
		open(wineprefix+"/system.reg","wt").write(registry)


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
	elif mode == "reg_only":
		set_regkeys(wineprefix, { "x86_64": "C:\\windows\\system32\\krkrwine.dll", "i386": "C:\\windows\\system32\\krkrwine.dll" })
	else:
		1/0

def wine_uninstall(wineprefix):
	set_regkeys(wineprefix, { "x86_64": "C:\\windows\\system32\\quartz.dll", "i386": "C:\\windows\\system32\\quartz.dll" })
	silent_unlink(wineprefix + "/drive_c/windows/system32/krkrwine.dll")
	silent_unlink(wineprefix + "/drive_c/windows/syswow64/krkrwine.dll")


eggroll_files = [
	"lib64/gstreamer-1.0/libgstmpegpsdemux.so",     "lib/gstreamer-1.0/libgstmpegpsdemux.so",
	"lib64/gstreamer-1.0/libgstasf.so",             "lib/gstreamer-1.0/libgstasf.so",
	"lib64/gstreamer-1.0/libgstvideoparsersbad.so", "lib/gstreamer-1.0/libgstvideoparsersbad.so",
	"lib64/libgstcodecparsers-1.0.so.0",            "lib/libgstcodecparsers-1.0.so.0",
	"lib64/libavcodec.so.58",                       "lib/libavcodec.so.58",
	"lib64/libavutil.so.56",                        "lib/libavutil.so.56",
	"lib64/libavfilter.so.7",                       "lib/libavfilter.so.7",
	"lib64/libavformat.so.58",                      "lib/libavformat.so.58",
	"lib64/libavdevice.so.58",                      "lib/libavdevice.so.58",
	"lib64/libswresample.so.3",                     "lib/libswresample.so.3",
	"lib64/libswscale.so.5",                        "lib/libswscale.so.5",
]
eggroll_files = [ ( dst.replace("gstreamer-1.0/","").replace("lib/","i386-").replace("lib64/","x86_64-"), dst ) for dst in eggroll_files ]

if __name__ == "__main__":
	parser = argparse.ArgumentParser()
	parser.add_argument('path', type=str, nargs='?', action="store", help="Path to Proton installation")
	parser.add_argument('--wine', action="store_true", help="Install to non-Protom Wine at ~/.wine or WINEPREFIX")
	parser.add_argument('--unsafe', action="store_true", help="With --wine, don't check if Wine is running before installing; with --uninstall, don't look for affected Proton prefixes")
	parser.add_argument('--uninstall', action="store_true", help="Uninstall instead")
	parser.add_argument('--devel', action="store_true", help="Point Wine to the DLLs' current location, instead of copying them into Wine's system32; allows easier updates of them, but you can't remove the krkrwine download directory (Wine only, not Proton)")
	parser.add_argument('--reinstall', action="store_true", help="Force install GStreamer components into Proton, even if they seem to be present already")
	
	args = parser.parse_args()
	
	if args.path is None and not args.wine:
		print("""
krkrwine v9.0 installer
Fixes some Wine bugs, making Kirikiri visual novels work under Wine and Proton 8.0
(Those bugs are fixed upstream in Wine 9.0)
Also installs some necessary codecs to Proton, such that mediaconverter will never be chosen

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
		
		print("Installing krkrwine to Wine...")
		if not args.uninstall:
			wine_install(wineprefix, unixpaths=unixpaths, unsafe=args.unsafe, mode=["copy", "z"][args.devel])
		else:
			wine_uninstall(wineprefix)
	else:
		if os.path.isfile(args.path + "/dist/share/default_pfx/system.reg"):
			# older Proton (8.0)
			proton_files = args.path + "/dist"
		elif os.path.isfile(args.path + "/files/share/default_pfx/system.reg"):
			# newer Proton (Experimental, Glorious Eggroll)
			proton_files = args.path + "/files"
		elif os.path.isfile(args.path + "/pfx/system.reg"):
			# it's a Proton prefix
			print("Don't do that, install to the Proton installation instead")
			exit(1)
		else:
			print("That doesn't look like Proton")
			exit(1)
		
		empty_settings = "user_settings = {}\n"
		import_me = "import krkrwine\nkrkrwine.install_within_proton()\n"
		
		if args.uninstall:
			if not args.unsafe:
				if not os.path.isdir(args.path+"/../../compatdata/"):
					print("No compatdata found")
					exit(1)
				first = True
				for appid in os.listdir(args.path+"/../../compatdata/"):
					compatdata = args.path+"/../../compatdata/"+appid
					if os.path.isfile(compatdata+"/pfx/drive_c/windows/system32/krkrwine.dll"):
						print("Uninstalling from appid "+appid)
						if first:
							print("(Don't worry if that's in another Proton version, it'll get reinstalled on next launch)")
							first = False
						wine_uninstall(compatdata+"/pfx")
			
			if os.path.isdir(args.path + "/krkrwine"):
				shutil.rmtree(args.path + "/krkrwine")
			wine_uninstall(proton_files + "/share/default_pfx")
			
			for src,dst in eggroll_files:
				fn = proton_files + "/" + dst
				if os.path.islink(fn) and not os.path.exists(fn):  # dead symlink
					os.unlink(fn)
			
			user_settings = open(args.path + "/user_settings.py", "rt").read()
			user_settings = user_settings.replace(import_me, "")
			if user_settings == empty_settings:
				os.unlink(args.path + "/user_settings.py")
			else:
				open(args.path + "/user_settings.py", "wt").write(user_settings)
		
		else:
			print("Installing krkrwine to Proton...")
			
			if not os.path.isdir(args.path + "/krkrwine"):
				os.mkdir(args.path + "/krkrwine")
			for fn in [ "krkrwine-x86_64.dll", "krkrwine-i386.dll", "gstkrkr-x86_64.so", "gstkrkr-i386.so" ]:
				shutil.copy(in_this_dir(fn), args.path + "/krkrwine/" + fn)
			
			shutil.copy(__file__, args.path + "/krkrwine/__init__.py")
			try:
				prev_user_settings = open(args.path + "/user_settings.py", "rt").read()
			except FileNotFoundError:
				prev_user_settings = empty_settings
			if import_me not in prev_user_settings:
				open(args.path + "/user_settings.py", "wt").write(import_me + prev_user_settings)
			
			eggroll_exists = [ os.path.isfile(args.path+"/krkrwine/"+src) for src,dst in eggroll_files ]
			if not args.reinstall and all(eggroll_exists):
				print("Not reinstalling Glorious Eggroll components, they're already in place")
				pass  # they can be in place because this is a GE, or because it's installed already; if so, just leave them
			else:
				print("Installing Glorious Eggroll components...")
				for src,dst in eggroll_files:
					shutil.copy(in_this_dir("Glorious-Eggroll/" + src), args.path + "/krkrwine/" + src)
	
	print("The operation completed successfully")

# This function is called from Proton user_settings.py
def install_within_proton():
	wineprefix = os.environ["STEAM_COMPAT_DATA_PATH"] + "/pfx"
	proton_dir = os.path.dirname(os.path.dirname(__file__))
	
	try:
		proton_dist = in_this_dir("../dist")
	except:
		# in case it's a Glorious Eggroll
		proton_dist = in_this_dir("../files")
	
	unixpaths = {}
	unixpaths["i386"]   = in_this_dir("krkrwine-i386.dll")
	unixpaths["x86_64"] = in_this_dir("krkrwine-x86_64.dll")
	if os.path.isdir(wineprefix):
		# game has been run already; install to the existing prefix
		wine_install(wineprefix, unixpaths=unixpaths, unsafe=True, mode="symlink")
	else:
		# game is being run for the first time; default_pfx will soon be copied (with all files being symlinks), install to there instead
		# (must check here, not in the main installer; the dlls occasionally get uninstalled on update)
		# (if the dll is uninstalled after this stage, that's fine - the wine_install above will redirect the symlink on next launch)
		default_pfx = proton_dist + "/share/default_pfx"
		wine_install(default_pfx, unixpaths=unixpaths, unsafe=True, mode="copy")
	
	os.environ["PROTON_NO_STEAM_FFMPEG"] = "1"
	gstkrkr_files = [
		("gstkrkr-i386.so",   "lib/gstreamer-1.0/gstkrkr-i386.so"),
		("gstkrkr-x86_64.so", "lib64/gstreamer-1.0/gstkrkr-x86_64.so")
	]
	for src,dst in eggroll_files + gstkrkr_files:
		if not os.path.isfile(proton_dist+"/"+dst):
			os.symlink(in_this_dir(src), proton_dist+"/"+dst)
