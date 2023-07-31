// SPDX-License-Identifier: GPL-2.0-only
// This file contains plenty of headers and function prototypes copypasted from Kirikiri <https://github.com/krkrz/krkr2/tree/master>.
// Kirikiri is dual licensed under GPLv2 and some homemade license. Since the homemade license is Japanese,
//    and I can't read that, I can't accept that license; therefore, this file is GPLv2 only.
// (I'm not sure if headers and other required interopability data is covered by copyright, but better safe than sorry.)
// (I also don't know if Kirikiri is 2.0 only or 2.0 or later, so I'll pick the safe choice.)

//#define DOIT

#define STRSAFE_NO_DEPRECATE
#define INITGUID
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <dshow.h>
#include <d3d9.h>
#include <vmr9.h>

#ifdef __i386__
// needs some extra shenanigans to kill the stdcall @12 suffix
#define EXPORT_STDCALL(ret, name, args) \
	__asm__(".section .drectve; .ascii \" -export:" #name "\"; .text"); \
	extern "C" __stdcall ret name args __asm__("_" #name); \
	extern "C" __stdcall ret name args
#else
#define EXPORT_STDCALL(ret, name, args) \
	extern "C" __attribute__((__visibility__("default"))) __stdcall ret name args; \
	extern "C" __attribute__((__visibility__("default"))) __stdcall ret name args
#endif

static char* guid_to_str(const GUID& guid)
{
	static char buf[8][64];
	static int n = 0;
	char* ret = buf[n++%8];
	sprintf(ret, "{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", guid.Data1, guid.Data2, guid.Data3, guid.Data4[0], guid.Data4[1], guid.Data4[2], guid.Data4[3], guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7]);
	return ret;
}

void* pe_get_section_body(HMODULE mod, int sec)
{
	uint8_t* base_addr = (uint8_t*)mod;
	IMAGE_DOS_HEADER* head_dos = (IMAGE_DOS_HEADER*)base_addr;
	IMAGE_NT_HEADERS* head_nt = (IMAGE_NT_HEADERS*)(base_addr + head_dos->e_lfanew);
	IMAGE_DATA_DIRECTORY section_dir = head_nt->OptionalHeader.DataDirectory[sec];
	return base_addr + section_dir.VirtualAddress;
}

typedef void(*fptr)();
void override_imports(HMODULE mod, fptr(*override_import)(const char * name))
{
	uint8_t* base_addr = (uint8_t*)mod;
	IMAGE_IMPORT_DESCRIPTOR* imports = (IMAGE_IMPORT_DESCRIPTOR*)pe_get_section_body(mod, IMAGE_DIRECTORY_ENTRY_IMPORT);
	
	while (imports->Name)
	{
		//const char * libname = (char*)(base_addr + imports->Name);
		//HMODULE mod_src = GetModuleHandle(libname);
//say(libname);
//say("\n");
		
		void* * out = (void**)(base_addr + imports->FirstThunk);
		// scan the library's exports and see what this one is named
		// if OriginalFirstThunk, the names are still available, but FirstThunk-only has to work anyways, no point not using it everywhere
		
		while (*out)
		{
			// forwarder RVAs mean the imported function may end up pointing to a completely different dll
			// for example, kernel32!HeapAlloc is actually ntdll!RtlAllocateHeap
			HMODULE mod_src;
			GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (char*)*out, &mod_src);
			
			uint8_t* src_base_addr = (uint8_t*)mod_src;
			IMAGE_EXPORT_DIRECTORY* exports = (IMAGE_EXPORT_DIRECTORY*)pe_get_section_body(mod_src, IMAGE_DIRECTORY_ENTRY_EXPORT);
			
			DWORD * addr_off = (DWORD*)(src_base_addr + exports->AddressOfFunctions);
			DWORD * name_off = (DWORD*)(src_base_addr + exports->AddressOfNames);
			WORD * ordinal = (WORD*)(src_base_addr + exports->AddressOfNameOrdinals);
			for (size_t i=0;i<exports->NumberOfFunctions;i++)
			{
				if (src_base_addr+addr_off[i] == (uint8_t*)*out)
				{
					for (size_t j=0;j<exports->NumberOfNames;j++)
					{
						if (ordinal[j] == i)
						{
							fptr newptr = override_import((const char*)(src_base_addr + name_off[j]));
							if (newptr)
							{
								// can't just *out = newptr, it'll blow up if the import table is in .rdata or otherwise readonly
								WriteProcessMemory(GetCurrentProcess(), out, &newptr, sizeof(newptr), NULL);
							}
							goto done;
						}
					}
puts("imported a nameless function\n");
goto done;
				}
			}
puts("imported a non-exported function\n");
		done:
			
			//for (size_t i=0;i<exports->NumberOfNames;i++)
			//{
			//	const char * exp_name = (const char*)(base_addr + name_off[i]);
			//	if (streq(name, exp_name))
			//		return base_addr + addr_off[ordinal[i]];
			//}
			
			out++;
		}
		imports++;
	}
}




typedef int8_t tjs_int8;
typedef uint8_t tjs_uint8;
typedef int16_t tjs_int16;
typedef uint16_t tjs_uint16;
typedef int32_t tjs_int32;
typedef uint32_t tjs_uint32;
typedef int64_t tjs_int64;
typedef uint64_t tjs_uint64;
typedef int tjs_int;    /* at least 32bits */
typedef unsigned int tjs_uint;    /* at least 32bits */
#define TJS_VS_SHORT_LEN 21

typedef wchar_t tjs_char;
typedef unsigned int tjs_uint;
#ifndef TJS_INTF_METHOD
#define TJS_INTF_METHOD __cdecl
#endif

enum tTVPVideoStatus { vsStopped, vsPlaying, vsPaused, vsProcessing };

struct iTVPFunctionExporter
{
	virtual bool TJS_INTF_METHOD QueryFunctions(const tjs_char ** name, void ** function, tjs_uint count) = 0;
	virtual bool TJS_INTF_METHOD QueryFunctionsByNarrowString(const char ** name, void ** function, tjs_uint count) = 0;
};

class iTVPVideoOverlay // this is not a COM object
{
public:
	virtual void __stdcall AddRef() = 0;
	virtual void __stdcall Release() = 0;

	virtual void __stdcall SetWindow(HWND window) = 0;
	virtual void __stdcall SetMessageDrainWindow(HWND window) = 0;
	virtual void __stdcall SetRect(RECT *rect) = 0;
	virtual void __stdcall SetVisible(bool b) = 0;
	virtual void __stdcall Play() = 0;
	virtual void __stdcall Stop() = 0;
	virtual void __stdcall Pause() = 0;
	virtual void __stdcall SetPosition(uint64_t tick) = 0;
	virtual void __stdcall GetPosition(uint64_t *tick) = 0;
	virtual void __stdcall GetStatus(tTVPVideoStatus* status) = 0;
	virtual void __stdcall GetEvent(long* evcode, long* param1, long* param2, bool* got) = 0;

// Start:	Add:	T.Imoto
	virtual void __stdcall FreeEventParams(long evcode, long param1, long param2) = 0;

	virtual void __stdcall Rewind() = 0;
	virtual void __stdcall SetFrame( int f ) = 0;
	virtual void __stdcall GetFrame( int* f ) = 0;
	virtual void __stdcall GetFPS( double* f ) = 0;
	virtual void __stdcall GetNumberOfFrame( int* f ) = 0;
	virtual void __stdcall GetTotalTime( int64_t* t ) = 0;
	
	virtual void __stdcall GetVideoSize( long* width, long* height ) = 0;
	virtual void __stdcall GetFrontBuffer( BYTE** buff ) = 0;
	virtual void __stdcall SetVideoBuffer( BYTE* buff1, BYTE* buff2, long size ) = 0;

	virtual void __stdcall SetStopFrame( int frame ) = 0;
	virtual void __stdcall GetStopFrame( int* frame ) = 0;
	virtual void __stdcall SetDefaultStopFrame() = 0;

	virtual void __stdcall SetPlayRate( double rate ) = 0;
	virtual void __stdcall GetPlayRate( double* rate ) = 0;

	virtual void __stdcall SetAudioBalance( long balance ) = 0;
	virtual void __stdcall GetAudioBalance( long* balance ) = 0;
	virtual void __stdcall SetAudioVolume( long volume ) = 0;
	virtual void __stdcall GetAudioVolume( long* volume ) = 0;

	virtual void __stdcall GetNumberOfAudioStream( unsigned long* streamCount ) = 0;
	virtual void __stdcall SelectAudioStream( unsigned long num ) = 0;
	virtual void __stdcall GetEnableAudioStreamNum( long* num ) = 0;
	virtual void __stdcall DisableAudioStream( void ) = 0;

	virtual void __stdcall GetNumberOfVideoStream( unsigned long* streamCount ) = 0;
	virtual void __stdcall SelectVideoStream( unsigned long num ) = 0;
	virtual void __stdcall GetEnableVideoStreamNum( long* num ) = 0;

	virtual void __stdcall SetMixingBitmap( HDC hdc, RECT* dest, float alpha ) = 0;
	virtual void __stdcall ResetMixingBitmap() = 0;

	virtual void __stdcall SetMixingMovieAlpha( float a ) = 0;
	virtual void __stdcall GetMixingMovieAlpha( float* a ) = 0;
	virtual void __stdcall SetMixingMovieBGColor( unsigned long col ) = 0;
	virtual void __stdcall GetMixingMovieBGColor( unsigned long *col ) = 0;

	virtual void __stdcall PresentVideoImage() = 0;

	virtual void __stdcall GetContrastRangeMin( float* v ) = 0;
	virtual void __stdcall GetContrastRangeMax( float* v ) = 0;
	virtual void __stdcall GetContrastDefaultValue( float* v ) = 0;
	virtual void __stdcall GetContrastStepSize( float* v ) = 0;
	virtual void __stdcall GetContrast( float* v ) = 0;
	virtual void __stdcall SetContrast( float v ) = 0;

	virtual void __stdcall GetBrightnessRangeMin( float* v ) = 0;
	virtual void __stdcall GetBrightnessRangeMax( float* v ) = 0;
	virtual void __stdcall GetBrightnessDefaultValue( float* v ) = 0;
	virtual void __stdcall GetBrightnessStepSize( float* v ) = 0;
	virtual void __stdcall GetBrightness( float* v ) = 0;
	virtual void __stdcall SetBrightness( float v ) = 0;

	virtual void __stdcall GetHueRangeMin( float* v ) = 0;
	virtual void __stdcall GetHueRangeMax( float* v ) = 0;
	virtual void __stdcall GetHueDefaultValue( float* v ) = 0;
	virtual void __stdcall GetHueStepSize( float* v ) = 0;
	virtual void __stdcall GetHue( float* v ) = 0;
	virtual void __stdcall SetHue( float v ) = 0;

	virtual void __stdcall GetSaturationRangeMin( float* v ) = 0;
	virtual void __stdcall GetSaturationRangeMax( float* v ) = 0;
	virtual void __stdcall GetSaturationDefaultValue( float* v ) = 0;
	virtual void __stdcall GetSaturationStepSize( float* v ) = 0;
	virtual void __stdcall GetSaturation( float* v ) = 0;
	virtual void __stdcall SetSaturation( float v ) = 0;

// End:	Add:	T.Imoto
};

HMODULE krmovie;
void(*o_GetAPIVersion)(DWORD* version);
void(*o_GetMixingVideoOverlayObject)(HWND callbackwin, IStream* stream, const wchar_t * streamname, const wchar_t* type, uint64_t size, iTVPVideoOverlay** out);
void(*o_GetVideoLayerObject)(HWND callbackwin, IStream* stream, const wchar_t * streamname, const wchar_t* type, uint64_t size, iTVPVideoOverlay **out);
void(*o_GetVideoOverlayObject)(HWND callbackwin, IStream* stream, const wchar_t * streamname, const wchar_t* type, uint64_t size, iTVPVideoOverlay** out);
HRESULT(*o_V2Link)(iTVPFunctionExporter* exporter);
void(*o_V2Unlink)();
const wchar_t *(*o_GetOptionDesc)();

ATOM __stdcall myRegisterClassExA(const WNDCLASSEXA* cls)
{
	puts("myRegisterClassExA");
	WNDCLASSEXA cls2 = *cls;
	//cls2.style &= ~CS_PARENTDC;
	ATOM reg = RegisterClassExA(&cls2);
	return reg;
}

//HWND theRealParent;

BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam)
{
	DWORD wnd_pid;
	DWORD my_pid = GetProcessId(GetCurrentProcess());
	GetWindowThreadProcessId(hwnd, &wnd_pid);
	if (wnd_pid != my_pid)
		return TRUE;
	char buf[256];
	char buf2[256];
	GetClassName(hwnd, buf, 256);
	GetWindowText(hwnd, buf2, 256);
	RECT rect;
	GetClientRect(hwnd, &rect);
	const char * indent = "        "+8-lParam;
	printf("%sEEE=%p :: %s :: %s :: %p :: %d :: %ldx%ld\n", indent, hwnd, buf, buf2, GetAncestor(hwnd, GA_PARENT), IsWindowVisible(hwnd), rect.right, rect.bottom);
	
	WNDCLASSEXA wc;
	GetClassInfoExA((HINSTANCE)GetWindowLongPtr(hwnd, GWLP_HINSTANCE), buf, &wc);
	printf("%swcstyle=%.8x wndstyle=%.8lx %.8lx\n", indent, wc.style, (DWORD)GetWindowLong(hwnd, GWL_STYLE), (DWORD)GetWindowLong(hwnd, GWL_EXSTYLE));
	
	HWND child = GetWindow(hwnd, GW_CHILD);
	while (child)
	{
		EnumWindowsProc(child, lParam+2);
		child = GetWindow(child, GW_HWNDNEXT);
	}
	return TRUE;
}
HWND __stdcall myCreateWindowExA(DWORD dwExStyle, LPCSTR lpClassName, LPCSTR lpWindowName, DWORD dwStyle,
                                 int X, int Y, int nWidth, int nHeight, HWND hWndParent, HMENU hMenu, HINSTANCE hInstance, LPVOID lpParam)
{
	
	//HWND theRealParentsChild = GetWindow(hWndParent, GW_CHILD);
	//SetWindowLong(hWndParent, GWL_STYLE, GetWindowLong(hWndParent, GWL_STYLE) & ~WS_CLIPCHILDREN);
	//printf("HIDE=%p\n", theRealParentsChild);
	//ShowWindow(theRealParentsChild, SW_HIDE);
	
	//theRealParent = hWndParent;
	//HWND ret = hWndParent;
	HWND ret = CreateWindowExA(dwExStyle, lpClassName, lpWindowName, dwStyle, X, Y, nWidth, nHeight, hWndParent, hMenu, hInstance, lpParam);
	//HWND ret = CreateWindowExA(dwExStyle, lpClassName, lpWindowName, dwStyle&~WS_CHILDWINDOW, X, Y, nWidth, nHeight, nullptr, hMenu, hInstance, lpParam);
	printf("myCreateWindowExA %s %s PARENT=%p NEWCHILD=%p\n", lpClassName, lpWindowName, hWndParent, ret);
	//EnumWindows(EnumWindowsProc, 0);
	
	return ret;
}
BOOL __stdcall myShowWindow(HWND hWnd, int nCmdShow)
{
	printf("myShowWindow %p %d\n", hWnd, nCmdShow);
	return ShowWindow(hWnd, nCmdShow);
	return TRUE;
}
BOOL __stdcall myDestroyWindow(HWND hWnd)
{
	printf("myDestroyWindow %p\n", hWnd);
	//return TRUE;
	BOOL g = DestroyWindow(hWnd);
	//EnumWindows(EnumWindowsProc, 0);
	return g;
}
BOOL __stdcall myMoveWindow(HWND hWnd, int X, int Y, int nWidth, int nHeight, BOOL bRepaint)
{
	printf("myMoveWindow %p\n", hWnd);
	return MoveWindow(hWnd, X, Y, nWidth, nHeight, bRepaint);
}

IDirect3DDevice9* g_d3ddev;
bool dump_texture(IDirect3DSurface9* renderTarget, const char * filename)
{
	HRESULT hr;
    IDirect3DDevice9* dev;
    //hr = dev->GetRenderTarget( 0, &renderTarget );
    hr = renderTarget->GetDevice(&dev);
    //hr = lpPresInfo->lpSurf->GetSurfaceLevel( 0, &renderTarget );
    if( !renderTarget || FAILED(hr) )
        return false;

    D3DSURFACE_DESC rtDesc;
    renderTarget->GetDesc( &rtDesc );

    IDirect3DSurface9* resolvedSurface;
    if( rtDesc.MultiSampleType != D3DMULTISAMPLE_NONE )
    {
        hr = dev->CreateRenderTarget( rtDesc.Width, rtDesc.Height, rtDesc.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &resolvedSurface, NULL );
        if( FAILED(hr) )
            return false;
        hr = dev->StretchRect( renderTarget, NULL, resolvedSurface, NULL, D3DTEXF_NONE );
        if( FAILED(hr) )
            return false;
        renderTarget = resolvedSurface;
    }

    IDirect3DSurface9* offscreenSurface;
    //hr = dev->CreateOffscreenPlainSurface( rtDesc.Width, rtDesc.Height, rtDesc.Format, D3DPOOL_SYSTEMMEM, &offscreenSurface, NULL );
    hr = dev->CreateOffscreenPlainSurface( rtDesc.Width, rtDesc.Height, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &offscreenSurface, NULL );
    if( FAILED(hr) )
        return false;

    hr = dev->GetRenderTargetData( renderTarget, offscreenSurface );
    bool ok = SUCCEEDED(hr);
    if( ok )
    {
        // Here we have data in offscreenSurface.
        D3DLOCKED_RECT lr;
        RECT rect;
        rect.left = 0;
        rect.right = rtDesc.Width;
        rect.top = 0;
        rect.bottom = rtDesc.Height;
        // Lock the surface to read pixels
        hr = offscreenSurface->LockRect( &lr, &rect, D3DLOCK_READONLY );
        if( SUCCEEDED(hr) )
        {
FILE* f=fopen(filename, "wb");
for (size_t i=0;i<rtDesc.Height;i++)
{
	fwrite((uint8_t*)lr.pBits + lr.Pitch*i, 1,rtDesc.Width*4, f);
}
fclose(f);
            // Pointer to data is lt.pBits, each row is
            // lr.Pitch bytes apart (often it is the same as width*bpp, but
            // can be larger if driver uses padding)

            // Read the data here!
            offscreenSurface->UnlockRect();
        }
        else
        {
            ok = false;
        }
    }
    return ok;
}




struct tTJSVariantString_S
{
	tjs_int RefCount; // reference count - 1
	tjs_char *LongString;
	tjs_char ShortString[TJS_VS_SHORT_LEN +1];
	tjs_int Length; // string length
	tjs_uint32 HeapFlag;
	tjs_uint32 Hint;
};
class tTJSVariantString : public tTJSVariantString_S {};
struct tTJSString_S
{
	tTJSVariantString *Ptr;
};
class tTJSString : public tTJSString_S {};

typedef tTJSString ttstr;




static void (*orig_TVPAddLog)(const ttstr & line);

static void STDMETHODCALLTYPE TVPAddLog(const ttstr & line)
{
	//DebugBreak();
	printf("TVPAddLog: ");
	wchar_t* x = line.Ptr->LongString ? line.Ptr->LongString : line.Ptr->ShortString;
	while (*x) putchar(*x++);
	puts("");
	//orig_TVPAddLog(line);
}
static void STDMETHODCALLTYPE TVPThrowExceptionMessage(const tjs_char * msg)
{
	printf("TVPThrowExceptionMessage: ");
	while (*msg)
	{
		putchar(*msg);
		msg++;
	}
	puts("");
	TerminateProcess(GetCurrentProcess(), 1);
}

iTVPFunctionExporter* orig_exporter;
struct fancy_iTVPFunctionExporter : public iTVPFunctionExporter
{
	bool TJS_INTF_METHOD QueryFunctions(const tjs_char ** name, void ** function, tjs_uint count) override
	{
puts("WIDEQUERY??");
		return orig_exporter->QueryFunctions(name, function, count);
	}
	bool TJS_INTF_METHOD QueryFunctionsByNarrowString(const char * * name, void* * function, tjs_uint count) override
	{
		bool ret = true;
		for (tjs_uint i=0;i<count;i++)
		{
printf("QUERY %p %s\n", name[i], name[i]);
			if (!strcmp(name[i], "void ::TVPAddLog(const ttstr &)"))
			{
				function[i] = (void*)&TVPAddLog;
				ret &= orig_exporter->QueryFunctionsByNarrowString(&name[i], (void**)&orig_TVPAddLog, 1);
				continue;
			}
			if (!strcmp(name[i], "void ::TVPThrowExceptionMessage(const tjs_char *)"))
			{
				function[i] = (void*)&TVPThrowExceptionMessage;
				continue;
			}
			ret &= orig_exporter->QueryFunctionsByNarrowString(&name[i], &function[i], 1);
		}
		return ret;
	}
};
fancy_iTVPFunctionExporter my_exporter;

iTVPVideoOverlay* orig_overlay;
class fancy_iTVPVideoOverlay : public iTVPVideoOverlay // this is not a COM object
{
public:
	virtual void __stdcall AddRef() { puts("fancy_iTVPVideoOverlay AddRef"); orig_overlay->AddRef(); }
	virtual void __stdcall Release() { puts("fancy_iTVPVideoOverlay Release"); orig_overlay->Release(); puts("fancy_iTVPVideoOverlay Release done"); }

	virtual void __stdcall SetWindow(HWND window)
	{
		char buf[256];
		GetWindowTextA(window, buf, 256);
		printf("fancy_iTVPVideoOverlay SetWindow %p %s\n", window, buf);
		orig_overlay->SetWindow(window);
	}
	virtual void __stdcall SetMessageDrainWindow(HWND window)
	{
		char buf[256];
		GetWindowTextA(window, buf, 256);
		printf("fancy_iTVPVideoOverlay SetMessageDrainWindow %p %s\n", window, buf);
		orig_overlay->SetMessageDrainWindow(window);
	}
	virtual void __stdcall SetRect(RECT* rect) { puts("fancy_iTVPVideoOverlay SetRect"); orig_overlay->SetRect(rect); }
	virtual void __stdcall SetVisible(bool b) { puts("fancy_iTVPVideoOverlay SetVisible"); orig_overlay->SetVisible(b); }
	virtual void __stdcall Play() { puts("fancy_iTVPVideoOverlay Play"); orig_overlay->Play(); puts("Play done"); }
	virtual void __stdcall Stop() { puts("fancy_iTVPVideoOverlay Stop"); orig_overlay->Stop(); puts("fancy_iTVPVideoOverlay Stop done"); }
	virtual void __stdcall Pause() { puts("fancy_iTVPVideoOverlay Pause"); orig_overlay->Pause(); }
	virtual void __stdcall SetPosition(uint64_t tick) { puts("fancy_iTVPVideoOverlay SetPosition"); orig_overlay->SetPosition(tick); }
	virtual void __stdcall GetPosition(uint64_t* tick) { puts("fancy_iTVPVideoOverlay GetPosition"); orig_overlay->GetPosition(tick); }
	virtual void __stdcall GetStatus(tTVPVideoStatus* status) { puts("fancy_iTVPVideoOverlay GetStatus"); orig_overlay->GetStatus(status); }
	virtual void __stdcall GetEvent(long* evcode, long* param1, long* param2, bool* got)
	{
		//puts("fancy_iTVPVideoOverlay GetEvent");
		orig_overlay->GetEvent(evcode, param1, param2, got);
	}

// Start:	Add:	T.Imoto
	virtual void __stdcall FreeEventParams(long evcode, long param1, long param2)
	{
		//puts("fancy_iTVPVideoOverlay FreeEventParams");
		orig_overlay->FreeEventParams(evcode, param1, param2);
	}

	virtual void __stdcall Rewind() { puts("fancy_iTVPVideoOverlay Rewind"); orig_overlay->Rewind(); }
	virtual void __stdcall SetFrame( int f ) { puts("fancy_iTVPVideoOverlay SetFrame"); orig_overlay->SetFrame(f); }
	virtual void __stdcall GetFrame( int* f )
	{
		//puts("fancy_iTVPVideoOverlay GetFrame");
		orig_overlay->GetFrame(f);
printf("frame=%d\n", *f);
	}
	virtual void __stdcall GetFPS( double* f ) { puts("fancy_iTVPVideoOverlay GetFPS"); orig_overlay->GetFPS(f); }
	virtual void __stdcall GetNumberOfFrame( int* f ) { puts("fancy_iTVPVideoOverlay GetNumberOfFrame"); orig_overlay->GetNumberOfFrame(f); }
	virtual void __stdcall GetTotalTime( int64_t* t ) { puts("fancy_iTVPVideoOverlay GetTotalTime"); orig_overlay->GetTotalTime(t); }
	
	virtual void __stdcall GetVideoSize( long* width, long* height ) { puts("fancy_iTVPVideoOverlay GetVideoSize"); orig_overlay->GetVideoSize(width, height); }
	virtual void __stdcall GetFrontBuffer( BYTE** buff ) { puts("fancy_iTVPVideoOverlay GetFrontBuffer"); orig_overlay->GetFrontBuffer(buff); }
	virtual void __stdcall SetVideoBuffer( BYTE* buff1, BYTE* buff2, long size ) { puts("fancy_iTVPVideoOverlay SetVideoBuffer"); orig_overlay->SetVideoBuffer(buff1, buff2, size); }

	virtual void __stdcall SetStopFrame( int frame ) { puts("fancy_iTVPVideoOverlay SetStopFrame"); orig_overlay->SetStopFrame(frame); }
	virtual void __stdcall GetStopFrame( int* frame ) { puts("fancy_iTVPVideoOverlay GetStopFrame"); orig_overlay->GetStopFrame(frame); }
	virtual void __stdcall SetDefaultStopFrame() { puts("fancy_iTVPVideoOverlay SetDefaultStopFrame"); orig_overlay->SetDefaultStopFrame(); }

	virtual void __stdcall SetPlayRate( double rate ) { puts("fancy_iTVPVideoOverlay SetPlayRate"); orig_overlay->SetPlayRate(rate); }
	virtual void __stdcall GetPlayRate( double* rate ) { puts("fancy_iTVPVideoOverlay GetPlayRate"); orig_overlay->GetPlayRate(rate); }

	virtual void __stdcall SetAudioBalance( long balance ) { puts("fancy_iTVPVideoOverlay SetAudioBalance"); orig_overlay->SetAudioBalance(balance); }
	virtual void __stdcall GetAudioBalance( long* balance ) { puts("fancy_iTVPVideoOverlay GetAudioBalance"); orig_overlay->GetAudioBalance(balance); }
	virtual void __stdcall SetAudioVolume( long volume ) { puts("fancy_iTVPVideoOverlay SetAudioVolume"); orig_overlay->SetAudioVolume(volume); }
	virtual void __stdcall GetAudioVolume( long* volume ) { puts("fancy_iTVPVideoOverlay GetAudioVolume"); orig_overlay->GetAudioVolume(volume); }

	virtual void __stdcall GetNumberOfAudioStream( unsigned long* streamCount ) { puts("fancy_iTVPVideoOverlay GetNumberOfAudioStream"); orig_overlay->GetNumberOfAudioStream(streamCount); }
	virtual void __stdcall SelectAudioStream( unsigned long num ) { puts("fancy_iTVPVideoOverlay SelectAudioStream"); orig_overlay->SelectAudioStream(num); }
	virtual void __stdcall GetEnableAudioStreamNum( long* num ) { puts("fancy_iTVPVideoOverlay GetEnableAudioStreamNum"); orig_overlay->GetEnableAudioStreamNum(num); }
	virtual void __stdcall DisableAudioStream( void ) { puts("fancy_iTVPVideoOverlay DisableAudioStream"); orig_overlay->DisableAudioStream(); }

	virtual void __stdcall GetNumberOfVideoStream( unsigned long* streamCount ) { puts("fancy_iTVPVideoOverlay GetNumberOfVideoStream"); orig_overlay->GetNumberOfVideoStream(streamCount); }
	virtual void __stdcall SelectVideoStream( unsigned long num ) { puts("fancy_iTVPVideoOverlay SelectVideoStream"); orig_overlay->SelectVideoStream(num); }
	virtual void __stdcall GetEnableVideoStreamNum( long* num ) { puts("fancy_iTVPVideoOverlay GetEnableVideoStreamNum"); orig_overlay->GetEnableVideoStreamNum(num); }

	virtual void __stdcall SetMixingBitmap( HDC hdc, RECT* dest, float alpha ) { puts("fancy_iTVPVideoOverlay SetMixingBitmap"); orig_overlay->SetMixingBitmap(hdc, dest, alpha); }
	virtual void __stdcall ResetMixingBitmap() { puts("fancy_iTVPVideoOverlay ResetMixingBitmap"); orig_overlay->ResetMixingBitmap(); }

	virtual void __stdcall SetMixingMovieAlpha( float a ) { puts("fancy_iTVPVideoOverlay SetMixingMovieAlpha"); orig_overlay->SetMixingMovieAlpha(a); }
	virtual void __stdcall GetMixingMovieAlpha( float* a ) { puts("fancy_iTVPVideoOverlay GetMixingMovieAlpha"); orig_overlay->GetMixingMovieAlpha(a); }
	virtual void __stdcall SetMixingMovieBGColor( unsigned long col ) { puts("fancy_iTVPVideoOverlay SetMixingMovieBGColor"); orig_overlay->SetMixingMovieBGColor(col); }
	virtual void __stdcall GetMixingMovieBGColor( unsigned long *col ) { puts("fancy_iTVPVideoOverlay GetMixingMovieBGColor"); orig_overlay->GetMixingMovieBGColor(col); }

	virtual void __stdcall PresentVideoImage()
	{
		orig_overlay->PresentVideoImage();
	}

	virtual void __stdcall GetContrastRangeMin( float* v ) { puts("fancy_iTVPVideoOverlay GetContrastRangeMin"); orig_overlay->GetContrastRangeMin(v); }
	virtual void __stdcall GetContrastRangeMax( float* v ) { puts("fancy_iTVPVideoOverlay GetContrastRangeMax"); orig_overlay->GetContrastRangeMax(v); }
	virtual void __stdcall GetContrastDefaultValue( float* v ) { puts("fancy_iTVPVideoOverlay GetContrastDefaultValue"); orig_overlay->GetContrastDefaultValue(v); }
	virtual void __stdcall GetContrastStepSize( float* v ) { puts("fancy_iTVPVideoOverlay GetContrastStepSize"); orig_overlay->GetContrastStepSize(v); }
	virtual void __stdcall GetContrast( float* v ) { puts("fancy_iTVPVideoOverlay GetContrast"); orig_overlay->GetContrast(v); }
	virtual void __stdcall SetContrast( float v ) { puts("fancy_iTVPVideoOverlay SetContrast"); orig_overlay->SetContrast(v); }

	virtual void __stdcall GetBrightnessRangeMin( float* v ) { puts("fancy_iTVPVideoOverlay GetBrightnessRangeMin"); orig_overlay->GetBrightnessRangeMin(v); }
	virtual void __stdcall GetBrightnessRangeMax( float* v ) { puts("fancy_iTVPVideoOverlay GetBrightnessRangeMax"); orig_overlay->GetBrightnessRangeMax(v); }
	virtual void __stdcall GetBrightnessDefaultValue( float* v ) { puts("fancy_iTVPVideoOverlay GetBrightnessDefaultValue"); orig_overlay->GetBrightnessDefaultValue(v); }
	virtual void __stdcall GetBrightnessStepSize( float* v ) { puts("fancy_iTVPVideoOverlay GetBrightnessStepSize"); orig_overlay->GetBrightnessStepSize(v); }
	virtual void __stdcall GetBrightness( float* v ) { puts("fancy_iTVPVideoOverlay GetBrightness"); orig_overlay->GetBrightness(v); }
	virtual void __stdcall SetBrightness( float v ) { puts("fancy_iTVPVideoOverlay SetBrightness"); orig_overlay->SetBrightness(v); }

	virtual void __stdcall GetHueRangeMin( float* v ) { puts("fancy_iTVPVideoOverlay GetHueRangeMin"); orig_overlay->GetHueRangeMin(v); }
	virtual void __stdcall GetHueRangeMax( float* v ) { puts("fancy_iTVPVideoOverlay GetHueRangeMax"); orig_overlay->GetHueRangeMax(v); }
	virtual void __stdcall GetHueDefaultValue( float* v ) { puts("fancy_iTVPVideoOverlay GetHueDefaultValue"); orig_overlay->GetHueDefaultValue(v); }
	virtual void __stdcall GetHueStepSize( float* v ) { puts("fancy_iTVPVideoOverlay GetHueStepSize"); orig_overlay->GetHueStepSize(v); }
	virtual void __stdcall GetHue( float* v ) { puts("fancy_iTVPVideoOverlay GetHue"); orig_overlay->GetHue(v); }
	virtual void __stdcall SetHue( float v ) { puts("fancy_iTVPVideoOverlay SetHue"); orig_overlay->SetHue(v); }

	virtual void __stdcall GetSaturationRangeMin( float* v ) { puts("fancy_iTVPVideoOverlay GetSaturationRangeMin"); orig_overlay->GetSaturationRangeMin(v); }
	virtual void __stdcall GetSaturationRangeMax( float* v ) { puts("fancy_iTVPVideoOverlay GetSaturationRangeMax"); orig_overlay->GetSaturationRangeMax(v); }
	virtual void __stdcall GetSaturationDefaultValue( float* v ) { puts("fancy_iTVPVideoOverlay GetSaturationDefaultValue"); orig_overlay->GetSaturationDefaultValue(v); }
	virtual void __stdcall GetSaturationStepSize( float* v ) { puts("fancy_iTVPVideoOverlay GetSaturationStepSize"); orig_overlay->GetSaturationStepSize(v); }
	virtual void __stdcall GetSaturation( float* v ) { puts("fancy_iTVPVideoOverlay GetSaturation"); orig_overlay->GetSaturation(v); }
	virtual void __stdcall SetSaturation( float v ) { puts("fancy_iTVPVideoOverlay SetSaturation"); orig_overlay->SetSaturation(v); }

// End:	Add:	T.Imoto
};
fancy_iTVPVideoOverlay my_overlay;

void my_init()
{
	if (!o_GetAPIVersion)
	{
		setvbuf(stdout, nullptr, _IONBF, 0);
		
		krmovie = LoadLibraryA("_rmovie.dll");
		if (!krmovie)
			krmovie = LoadLibraryA("plugin/_rmovie.dll");
		o_GetAPIVersion = (decltype(o_GetAPIVersion))GetProcAddress(krmovie, "GetAPIVersion");
		o_GetMixingVideoOverlayObject = (decltype(o_GetMixingVideoOverlayObject))GetProcAddress(krmovie, "GetMixingVideoOverlayObject");
		o_GetVideoLayerObject = (decltype(o_GetVideoLayerObject))GetProcAddress(krmovie, "GetVideoLayerObject");
		o_GetVideoOverlayObject = (decltype(o_GetVideoOverlayObject))GetProcAddress(krmovie, "GetVideoOverlayObject");
		o_V2Link = (decltype(o_V2Link))GetProcAddress(krmovie, "V2Link");
		o_V2Unlink = (decltype(o_V2Unlink))GetProcAddress(krmovie, "V2Unlink");
		o_GetOptionDesc = (decltype(o_GetOptionDesc))GetProcAddress(krmovie, "GetOptionDesc");
		
		if (false)
		override_imports(krmovie,
			[](const char * name) -> fptr {
				//printf("import %s\n", name);
				//if (!strcmp(name, "RegisterClassExA"))
					//return (fptr)&myRegisterClassExA;
				if (!strcmp(name, "CreateWindowExA"))
					return (fptr)&myCreateWindowExA;
				if (!strcmp(name, "DestroyWindow"))
					return (fptr)&myDestroyWindow;
				//if (!strcmp(name, "ShowWindow"))
					//return (fptr)&myShowWindow;
				if (!strcmp(name, "MoveWindow"))
					return (fptr)&myMoveWindow;
#ifdef DOIT
				//if (!strcmp(name, "CoCreateInstance"))
					//return (fptr)&myCoCreateInstance;
#endif
				return nullptr;
			});
	}
}

EXPORT_STDCALL(void, GetAPIVersion, (DWORD* version))
{
	puts("krmovie GetAPIVersion");
	my_init();
	o_GetAPIVersion(version);
}
EXPORT_STDCALL(void, GetMixingVideoOverlayObject, (HWND callbackwin, IStream* stream, const wchar_t * streamname,
                                                   const wchar_t* type, uint64_t size, iTVPVideoOverlay** out))
{
	printf("krmovie GetMixingVideoOverlayObject %ls %ls %p\n", streamname, type, callbackwin);
	my_init();
	*out = &my_overlay;
#ifdef DOIT
	o_GetMixingVideoOverlayObject(callbackwin, stream, streamname, type, size, &orig_overlay);
#else
	o_GetMixingVideoOverlayObject(callbackwin, stream, streamname, type, size, out);
#endif
}
EXPORT_STDCALL(void, GetVideoLayerObject, (HWND callbackwin, IStream* stream, const wchar_t * streamname,
                                           const wchar_t* type, uint64_t size, iTVPVideoOverlay **out))
{
	printf("krmovie GetVideoLayerObject %ls %ls %p\n", streamname, type, callbackwin);
	my_init();
	*out = &my_overlay;
#ifdef DOIT
	o_GetVideoLayerObject(callbackwin, stream, streamname, type, size, &orig_overlay);
#else
	o_GetVideoLayerObject(callbackwin, stream, streamname, type, size, out);
#endif
}
EXPORT_STDCALL(void, GetVideoOverlayObject, (HWND callbackwin, IStream* stream, const wchar_t * streamname,
                                             const wchar_t* type, uint64_t size, iTVPVideoOverlay** out))
{
	printf("krmovie GetVideoOverlayObject %ls %ls %p\n", streamname, type, callbackwin);
	my_init();
	*out = &my_overlay;
#ifdef DOIT
	o_GetVideoOverlayObject(callbackwin, stream, streamname, type, size, &orig_overlay);
#else
	o_GetVideoOverlayObject(callbackwin, stream, streamname, type, size, out);
#endif
}
EXPORT_STDCALL(HRESULT, V2Link, (iTVPFunctionExporter* exporter))
{
	puts("krmovie V2Link");
	my_init();
	orig_exporter = exporter;
//#ifdef DOIT
	return o_V2Link(&my_exporter);
//#else
	//return o_V2Link(exporter);
//#endif
}
EXPORT_STDCALL(void, V2Unlink, ())
{
	puts("krmovie V2Unlink");
	my_init();
	o_V2Unlink();
}
EXPORT_STDCALL(const wchar_t *, GetOptionDesc, ())
{
	puts("krmovie GetOptionDesc");
	my_init();
	return o_GetOptionDesc();
}
