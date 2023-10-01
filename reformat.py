#!/usr/bin/env python3

# This script takes a bunch of function prototypes copypasted from MSDN (or whatever it's called these days), and reformats them to valid C++.
# The functions will either call the same function on a parent object, or just return an error.

parent_name = ""

text = """


HRESULT CheckCapabilities(
  [in, out] DWORD *pCapabilities
);HRESULT ConvertTimeFormat(
  [out] LONGLONG   *pTarget,
  [in]  const GUID *pTargetFormat,
  [in]  LONGLONG   Source,
  [in]  const GUID *pSourceFormat
);

HRESULT GetAvailable(
  [out] LONGLONG *pEarliest,
  [out] LONGLONG *pLatest
);

HRESULT GetCapabilities(
  [out] DWORD *pCapabilities
);HRESULT GetCurrentPosition(
  [out] LONGLONG *pCurrent
);
HRESULT GetDuration(
  [out] LONGLONG *pDuration
);HRESULT GetPositions(
  [out] LONGLONG *pCurrent,
  [out] LONGLONG *pStop
);HRESULT GetPreroll(
  [out] LONGLONG *pllPreroll
);HRESULT GetRate(
  [out] double *pdRate
);HRESULT GetStopPosition(
  [out] LONGLONG *pStop
);

HRESULT GetTimeFormat(
  [out] GUID *pFormat
);HRESULT IsFormatSupported(
  [in] const GUID *pFormat
);HRESULT IsUsingTimeFormat(
  [in] const GUID *pFormat
);HRESULT QueryPreferredFormat(
  [out] GUID *pFormat
);

HRESULT SetPositions(
  [in, out] LONGLONG *pCurrent,
  [in]      DWORD    dwCurrentFlags,
  [in, out] LONGLONG *pStop,
  [in]      DWORD    dwStopFlags
);HRESULT SetRate(
  [in] double dRate
);HRESULT SetTimeFormat(
  [in] const GUID *pFormat
);


"""

import re
for fn in text.replace("\n", "").split(";"):
	if not fn:
		continue
	ret,name,args = re.match(r" *(.*) ([^ ]+)\((.*)\)", fn).groups()
	args = re.sub(r"\[[^\]]+\]", "", args)
	args = args.split(',')
	for n,arg in enumerate(args):
		arg = arg.strip()
		args[n] = arg
	# print("------")
	# print(ret,name,args)
	if args != ['']:
		args_untyped = ','.join(re.search("[A-Za-z0-9_a+]+$", a)[0] for a in args)
	else:
		args_untyped = ''
	# print(args_untyped)
	if parent_name:
		process = 'return '+parent_name+'->'+name+'('+args_untyped+');'
	else:
		process = 'return E_OUTOFMEMORY;'
	fn = ret+' STDMETHODCALLTYPE '+name+'('+', '.join(args)+') override\n\t{ puts("'+name+'"); '+process+' }'
	fn = re.sub(" +", " ", fn).replace("( ", "(").replace(" )", ")").replace(" *", "* ").replace(" *", "* ")
	print(fn)
