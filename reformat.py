#!/usr/bin/env python3

# This script takes a bunch of function prototypes copypasted from MSDN (or whatever it's called these days),
# or function pointers copied from a Wine vtable struct, and reformats them to valid C++.
# The functions will either call the same function on a parent object, or just return an error.

parent_name = "the_real_one"
debug_name = "pin"

text = """


    HRESULT (STDMETHODCALLTYPE *GetAllocator)(
        IMemInputPin *This,
        IMemAllocator **ppAllocator);

    HRESULT (STDMETHODCALLTYPE *NotifyAllocator)(
        IMemInputPin *This,
        IMemAllocator *pAllocator,
        BOOL bReadOnly);

    HRESULT (STDMETHODCALLTYPE *GetAllocatorRequirements)(
        IMemInputPin *This,
        ALLOCATOR_PROPERTIES *pProps);

    HRESULT (STDMETHODCALLTYPE *Receive)(
        IMemInputPin *This,
        IMediaSample *pSample);

    HRESULT (STDMETHODCALLTYPE *ReceiveMultiple)(
        IMemInputPin *This,
        IMediaSample **pSamples,
        LONG nSamples,
        LONG *nSamplesProcessed);

    HRESULT (STDMETHODCALLTYPE *ReceiveCanBlock)(
        IMemInputPin *This);



"""

if debug_name:
	debug_name += " "

import re
for fn in text.replace("\n", "").split(";"):
	if not fn:
		continue
	fn = fn.replace("STDMETHODCALLTYPE","").replace("virtual "," ")
	try:
		ret,name,args = re.match(r" *(.*) \( *\*([^ ]+)\)\([^,]+This(?:,|(?=\)))(.*)\)", fn).groups()
	except Exception:
		ret,name,args = re.match(r" *(.*) ([^ ]+)\((.*)\)", fn).groups()
	args = re.sub(r"\[[^\]]+\]", "", args)
	args = args.split(',')
	for n,arg in enumerate(args):
		arg = arg.strip()
		args[n] = arg
	# print("------")
	# print(ret,name,args)
	if args != ['']:
		args_untyped = ', '.join(re.search("[A-Za-z0-9_a+]+$", a)[0] for a in args)
	else:
		args_untyped = ''
	# print(args_untyped)
	if parent_name:
		process = 'return '+parent_name+'->'+name+'('+args_untyped+');'
	else:
		process = 'return E_OUTOFMEMORY;'
	debugprint = ""
	if debug_name:
		debugprint = 'puts("'+debug_name+name+'"); '
	fn = ret+' STDMETHODCALLTYPE '+name+'('+', '.join(args)+') override\n\t{ '+debugprint+process+' }'
	fn = re.sub(" +", " ", fn).replace("( ", "(").replace(" )", ")").replace(" *", "* ").replace(" *", "* ")
	print(fn)
