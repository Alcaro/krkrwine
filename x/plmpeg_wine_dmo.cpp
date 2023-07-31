// SPDX-License-Identifier: LGPL-2.0-or-later

#ifdef __MINGW32__
#  define _FILE_OFFSET_BITS 64
// mingw *really* wants to define its own printf/scanf, which adds ~20KB random stuff to the binary
// (on some 32bit mingw versions, it also adds a dependency on libgcc_s_sjlj-1.dll)
// extra kilobytes and dlls is the opposite of what I want, and my want is stronger, so here's some shenanigans
// comments say libstdc++ demands a POSIX printf, but I don't use libstdc++'s text functions, so I don't care
#  define __USE_MINGW_ANSI_STDIO 0 // trigger a warning if it's enabled already - probably wrong include order
#  include <cstdbool>              // include some random c++ header; they all include <bits/c++config.h>,
#  undef __USE_MINGW_ANSI_STDIO    // which ignores my #define above and sets this flag; re-clear it before including <stdio.h>
#  define __USE_MINGW_ANSI_STDIO 0 // (subsequent includes of c++config.h are harmless, there's an include guard)
#endif

#define INITGUID
#define STRSAFE_NO_DEPRECATE
#include <typeinfo>
#include <windows.h>
#include <dshow.h>
#include <mmreg.h>
#include <dmodshow.h>
#include <dmoreg.h>
#include <mediaerr.h>
#define PL_MPEG_IMPLEMENTATION
#include "pl_mpeg.h"

static const GUID GUID_NULL = {}; // not defined in my headers, how lovely
DEFINE_GUID(IID_IUnknown, 0x00000000, 0x0000, 0x0000, 0xc0,0x00, 0x00,0x00,0x00,0x00,0x00,0x46);


// some of these aren't defined in my headers, or not where I want them
//DEFINE_GUID(MEDIASUBTYPE_I420,0x30323449,0x0000,0x0010,0x80,0x00,0x00,0xaa,0x00,0x38,0x9b,0x71);


// private guids, only meaningful in this dll
// there is a CLSID_MPEG1Splitter in Wine, but it's incomplete - it only supports audio, not video
DEFINE_GUID(CLSID_MPEG1Splitter_PlmpegDmo,0xd3d07daf,0x8ba2,0x49fd,0x9f,0xb4,0x47,0x8a,0xa2,0xe4,0x80,0xb1);
DEFINE_GUID(CLSID_CMpegVideoCodec_PlmpegDmo,0x64170df4,0x31c6,0x44e1,0x8d,0xfe,0xee,0x8e,0x59,0x13,0x04,0xcd);


#ifdef __i386__
// needs some extra shenanigans to kill the stdcall @12 suffix
#define EXPORT(ret, name, args) \
	__asm__(".section .drectve; .ascii \" -export:" #name "\"; .text"); \
	extern "C" __stdcall ret name args __asm__("_" #name); \
	extern "C" __stdcall ret name args
#else
#define EXPORT(ret, name, args) \
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

template<typename T> T min(T a, T b) { return a < b ? a : b; }
template<typename T> T max(T a, T b) { return a > b ? a : b; }


template<typename T> class CComPtr {
	void assign(T* ptr)
	{
		p = ptr;
	}
	void release()
	{
		if (p)
			p->Release();
		p = nullptr;
	}
public:
	T* p;
	
	CComPtr() { p = nullptr; }
	~CComPtr() { release(); }
	CComPtr(const CComPtr&) = delete;
	CComPtr(CComPtr&&) = delete;
	void operator=(const CComPtr&) = delete;
	void operator=(CComPtr&&) = delete;
	
	CComPtr& operator=(T* ptr)
	{
		release();
		assign(ptr);
		return *this;
	}
	T** operator&()
	{
		release();
		return &p;
	}
	T* operator->() { return p; }
	operator T*() { return p; }
	
	HRESULT CoCreateInstance(REFCLSID rclsid, LPUNKNOWN pUnkOuter, DWORD dwClsContext)
	{
		release();
		return ::CoCreateInstance(rclsid, pUnkOuter, dwClsContext, IID_PPV_ARGS(&p));
	}
	template<typename T2>
	HRESULT QueryInterface(T2** other)
	{
		return p->QueryInterface(IID_PPV_ARGS(other));
	}
};

template<typename... Tis>
class com_base_embedded : public Tis... {
private:
	template<typename T, typename... Ts> T first_helper();
	template<typename Ti> bool QueryInterfaceSingle(REFIID riid, void** ppvObject)
	{
		if (riid == __uuidof(Ti))
		{
			Ti* ret = this;
			ret->AddRef();
			*ppvObject = (void*)ret;
			return true;
		}
		else return false;
	}
public:
	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override
	{
printf("plmpeg QI %s\n", guid_to_str(riid));
		*ppvObject = nullptr;
		if (riid == __uuidof(IUnknown))
		{
			IUnknown* ret = (decltype(first_helper<Tis...>())*)this;
			ret->AddRef();
			*ppvObject = (void*)ret;
			return S_OK;
		}
		return (QueryInterfaceSingle<Tis>(riid, ppvObject) || ...) ? S_OK : E_NOINTERFACE;
	}
};

template<typename... Tis>
class com_base : public com_base_embedded<Tis...> {
	uint32_t refcount = 1;
public:
	ULONG STDMETHODCALLTYPE AddRef() override { return ++refcount; }
	ULONG STDMETHODCALLTYPE Release() override
	{
		uint32_t new_refcount = --refcount;
		if (!new_refcount)
			delete this;
		return new_refcount;
	}
};

template<typename... Tis>
class com_base_aggregate : public Tis... {
	// Microsoft docs imply that the outer object should have the local object's refcount and
	//    the inner object should implement all applicable interfaces, but that just sounds like a pain.
	// (this kinda contradicts the rule that x->QI(IUnknown) == x->QI(IOuter)->QI(IUnknown), doesn't it?
	//    But only the parent object can access the secret IUnknown anyways.)
private:
	class real_iunk : public IUnknown {
		uint32_t refcount = 1;
		com_base_aggregate* parent() { return container_of<&com_base_aggregate::iunk>(this); }
	public:
		ULONG STDMETHODCALLTYPE AddRef() override { return ++refcount; }
		ULONG STDMETHODCALLTYPE Release() override
		{
			uint32_t new_refcount = --refcount;
			if (!new_refcount)
				delete parent();
			return new_refcount;
		}
		HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override
		{
			return parent()->NonDelegatingQueryInterface(riid, ppvObject);
		}
	};
	
	real_iunk iunk;
	IUnknown* outer;
	
	template<typename Ti> bool QueryInterfaceSingle(REFIID riid, void** ppvObject)
	{
		if (riid == __uuidof(Ti))
		{
			Ti* ret = this;
			ret->AddRef();
			*ppvObject = (void*)ret;
			return true;
		}
		else return false;
	}
	HRESULT NonDelegatingQueryInterface(REFIID riid, void** ppvObject)
	{
printf("plmpeg NDQI %s\n", guid_to_str(riid));
		if (riid == __uuidof(IUnknown))
		{
			iunk.AddRef();
			*ppvObject = (void*)(IUnknown*)&iunk;
			return S_OK;
		}
		*ppvObject = nullptr;
		return (QueryInterfaceSingle<Tis>(riid, ppvObject) || ...) ? S_OK : E_NOINTERFACE;
	}
	
public:
	com_base_aggregate(IUnknown* pUnkOuter) : outer(pUnkOuter) {}
	
	IUnknown* real_iunknown() { return &iunk; }
	
	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override
	{
printf("plmpeg DQI %s\n", guid_to_str(riid));
		if (riid == __uuidof(IUnknown))
		{
			iunk.AddRef();
			*ppvObject = (void*)&iunk;
			return S_OK;
		}
		return outer->QueryInterface(riid, ppvObject);
	}
	
	ULONG STDMETHODCALLTYPE AddRef() override { return outer->AddRef(); }
	ULONG STDMETHODCALLTYPE Release() override { return outer->Release(); }
};

template<typename Touter, typename Tinner>
Tinner com_enum_helper(HRESULT STDMETHODCALLTYPE (Touter::*)(ULONG, Tinner**, ULONG*));

// don't inline this lambda into the template's default argument, gcc bug 105667
static const auto addref_ptr = []<typename T>(T* obj) { obj->AddRef(); return obj; };
template<typename Timpl, auto clone = addref_ptr>
class com_enum : public com_base<Timpl> {
	using Tret = decltype(com_enum_helper(&Timpl::Next));
	
	Tret** items;
	size_t pos = 0;
	size_t len;
	
	com_enum(Tret** items, size_t pos, size_t len) : items(items), pos(pos), len(len) {}
public:
	com_enum(Tret** items, size_t len) : items(items), len(len) {}
	
	HRESULT STDMETHODCALLTYPE Clone(Timpl** ppEnum) override
	{
		*ppEnum = new com_enum(items, pos, len);
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE Next(ULONG celt, Tret** rgelt, ULONG* pceltFetched) override
	{
		size_t remaining = len - pos;
		size_t ret = min((size_t)celt, remaining);
		for (size_t n=0;n<ret;n++)
			rgelt[n] = clone(items[pos++]);
		if (pceltFetched)
			*pceltFetched = ret;
		if (remaining >= celt)
			return S_OK;
		else
			return S_FALSE;
	}
	HRESULT STDMETHODCALLTYPE Reset() override
	{
		pos = 0;
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE Skip(ULONG celt) override
	{
		size_t remaining = len - pos;
		if (remaining >= celt)
		{
			pos += celt;
			return S_OK;
		}
		else
		{
			pos = len;
			return S_FALSE;
		}
	}
};


// inspired by the Linux kernel macro, but using a member pointer looks cleaner; non-expressions (like member names) in macros look wrong
template<typename Tc, typename Ti> Tc* container_of(Ti* ptr, Ti Tc:: * memb)
{
	// https://wg21.link/P0908 proposes a better implementation, but it was forgotten and not accepted
	Tc* fake_object = (Tc*)0x12345678;  // doing math on a fake pointer is UB, but good luck proving it's bogus
	size_t offset = (uintptr_t)&(fake_object->*memb) - (uintptr_t)fake_object;
	return (Tc*)((uint8_t*)ptr - offset);
}
template<typename Tc, typename Ti> const Tc* container_of(const Ti* ptr, Ti Tc:: * memb)
{
	return container_of<Tc, Ti>((Ti*)ptr, memb);
}
template<auto memb, typename Ti> auto container_of(Ti* ptr) { return container_of(ptr, memb); }


HRESULT qi_release(IUnknown* obj, REFIID riid, void** ppvObj)
{
	HRESULT hr = obj->QueryInterface(riid, ppvObj); 
	obj->Release(); 
	return hr;
}


static HRESULT CopyMediaType(AM_MEDIA_TYPE * pmtTarget, const AM_MEDIA_TYPE * pmtSource)
{
	*pmtTarget = *pmtSource;
	if (pmtSource->pbFormat != nullptr)
	{
		pmtTarget->pbFormat = (uint8_t*)CoTaskMemAlloc(pmtSource->cbFormat);
		memcpy(pmtTarget->pbFormat, pmtSource->pbFormat, pmtSource->cbFormat);
	}
	return S_OK;
}

static AM_MEDIA_TYPE* CreateMediaType(const AM_MEDIA_TYPE * pSrc)
{
	AM_MEDIA_TYPE* ret = (AM_MEDIA_TYPE*)CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE));
	CopyMediaType(ret, pSrc);
	return ret;
}


class scoped_lock {
	SRWLOCK* lock;
public:
	scoped_lock(SRWLOCK* lock) : lock(lock) { AcquireSRWLockExclusive(lock); }
	~scoped_lock() { ReleaseSRWLockExclusive(lock); }
};
class scoped_unlock {
	SRWLOCK* lock;
public:
	scoped_unlock(SRWLOCK* lock) : lock(lock) { ReleaseSRWLockExclusive(lock); }
	~scoped_unlock() { AcquireSRWLockExclusive(lock); }
};

template<typename Touter>
class base_dmo : public com_base_aggregate<IMediaObject> {
	Touter* parent() { return (Touter*)this; }
	
public:
	
	void debug(const char * fmt, ...)
	{
		char buf[1024];
		
		va_list args;
		va_start(args, fmt);
		vsnprintf(buf, 1024, fmt, args);
		va_end(args);
		
		fprintf(stdout, "%s %s\n", typeid(Touter).name(), buf);
		fflush(stdout);
	}
	
	using com_base_aggregate::com_base_aggregate;
	
	HRESULT STDMETHODCALLTYPE AllocateStreamingResources() override
	{
		debug("IMediaObject AllocateStreamingResources");
		return E_OUTOFMEMORY;
	}
	
	HRESULT STDMETHODCALLTYPE Discontinuity(DWORD dwInputStreamIndex) override
	{
		debug("IMediaObject Discontinuity");
		return E_OUTOFMEMORY;
	}
	
	HRESULT STDMETHODCALLTYPE Flush() override
	{
		debug("IMediaObject Flush");
		return E_OUTOFMEMORY;
	}
	
	HRESULT STDMETHODCALLTYPE FreeStreamingResources() override
	{
		debug("IMediaObject FreeStreamingResources");
		return E_OUTOFMEMORY;
	}
	
	HRESULT STDMETHODCALLTYPE GetInputCurrentType(DWORD dwInputStreamIndex, DMO_MEDIA_TYPE* pmt) override
	{
		debug("IMediaObject GetInputCurrentType");
		return E_OUTOFMEMORY;
	}
	
	HRESULT STDMETHODCALLTYPE GetInputMaxLatency(DWORD dwInputStreamIndex, REFERENCE_TIME* prtMaxLatency) override
	{
		debug("IMediaObject GetInputMaxLatency");
		return E_OUTOFMEMORY;
	}
	
	HRESULT STDMETHODCALLTYPE GetInputSizeInfo(DWORD dwInputStreamIndex, DWORD* pcbSize, DWORD* pcbMaxLookahead, DWORD* pcbAlignment) override
	{
		debug("IMediaObject GetInputSizeInfo");
		return E_OUTOFMEMORY;
	}
	
	HRESULT STDMETHODCALLTYPE GetInputStatus(DWORD dwInputStreamIndex, DWORD* dwFlags) override
	{
		debug("IMediaObject GetInputStatus");
		return E_OUTOFMEMORY;
	}
	
	HRESULT STDMETHODCALLTYPE GetInputStreamInfo(DWORD dwInputStreamIndex, DWORD* pdwFlags) override
	{
		debug("IMediaObject GetInputStreamInfo");
		return E_OUTOFMEMORY;
	}
	
	HRESULT STDMETHODCALLTYPE GetInputType(DWORD dwInputStreamIndex, DWORD dwTypeIndex, DMO_MEDIA_TYPE* pmt) override
	{
		debug("IMediaObject GetInputType");
		return E_OUTOFMEMORY;
	}
	
	HRESULT STDMETHODCALLTYPE GetOutputCurrentType(DWORD dwOutputStreamIndex, DMO_MEDIA_TYPE* pmt) override
	{
		debug("IMediaObject GetOutputCurrentType");
		return E_OUTOFMEMORY;
	}
	
	HRESULT STDMETHODCALLTYPE GetOutputSizeInfo(DWORD dwOutputStreamIndex, DWORD* pcbSize, DWORD* pcbAlignment) override
	{
		debug("IMediaObject GetOutputSizeInfo");
		return E_OUTOFMEMORY;
	}
	
	HRESULT STDMETHODCALLTYPE GetOutputStreamInfo(DWORD dwOutputStreamIndex, DWORD* pdwFlags) override
	{
		debug("IMediaObject GetOutputStreamInfo");
		return E_OUTOFMEMORY;
	}
	
	HRESULT STDMETHODCALLTYPE GetOutputType(DWORD dwOutputStreamIndex, DWORD dwTypeIndex, DMO_MEDIA_TYPE* pmt) override
	{
		debug("IMediaObject GetOutputType");
		return E_OUTOFMEMORY;
	}
	
	HRESULT STDMETHODCALLTYPE Lock(LONG bLock) override
	{
		debug("IMediaObject Lock");
		return E_OUTOFMEMORY;
	}
	
	HRESULT STDMETHODCALLTYPE ProcessInput(DWORD dwInputStreamIndex, IMediaBuffer* pBuffer, DWORD dwFlags,
	                                       REFERENCE_TIME rtTimestamp, REFERENCE_TIME rtTimelength) override
	{
		debug("IMediaObject ProcessInput");
		return E_OUTOFMEMORY;
	}
	
	HRESULT STDMETHODCALLTYPE ProcessOutput(DWORD dwFlags, DWORD cOutputBufferCount,
	                                        DMO_OUTPUT_DATA_BUFFER* pOutputBuffers, DWORD* pdwStatus) override
	{
		debug("IMediaObject ProcessOutput");
		return E_OUTOFMEMORY;
	}
	
	HRESULT STDMETHODCALLTYPE SetInputMaxLatency(DWORD dwInputStreamIndex, REFERENCE_TIME rtMaxLatency) override
	{
		debug("IMediaObject SetInputMaxLatency");
		return E_OUTOFMEMORY;
	}
	
	HRESULT STDMETHODCALLTYPE SetOutputType(DWORD dwOutputStreamIndex, const DMO_MEDIA_TYPE* pmt, DWORD dwFlags) override
	{
		debug("IMediaObject SetOutputType");
		return E_OUTOFMEMORY;
	}
};





class CMPEG1Splitter : public base_dmo<CMPEG1Splitter> {
public:
	using base_dmo::base_dmo;
	
	HRESULT STDMETHODCALLTYPE GetStreamCount(DWORD* pcInputStreams, DWORD* pcOutputStreams) override
	{
		debug("IMediaObject GetStreamCount");
		*pcInputStreams = 1;
		*pcOutputStreams = 2;
		return S_OK;
	}
	
	HRESULT STDMETHODCALLTYPE SetInputType(DWORD dwInputStreamIndex, const DMO_MEDIA_TYPE* pmt, DWORD dwFlags) override
	{
		debug("IMediaObject SetInputType %s %s %s", guid_to_str(pmt->majortype), guid_to_str(pmt->subtype), guid_to_str(pmt->formattype));
		if (pmt->majortype == MEDIATYPE_Stream && pmt->subtype == MEDIASUBTYPE_MPEG1System)
			return S_OK;
		return DMO_E_TYPE_NOT_ACCEPTED;
	}
	
	HRESULT STDMETHODCALLTYPE GetOutputType(DWORD dwOutputStreamIndex, DWORD dwTypeIndex, DMO_MEDIA_TYPE* pmt) override
	{
		debug("IMediaObject GetOutputType %lu %lu", dwOutputStreamIndex, dwTypeIndex);
		if (dwTypeIndex > 0)
			return DMO_E_NO_MORE_ITEMS;
		if (dwOutputStreamIndex == 0)
		{
			int width = 640;
			int height = 480;
			double fps = 60.0;
			
			MPEG1VIDEOINFO mediatype_head = {
				.hdr = {
					.rcSource = { 0, 0, width, height },
					.rcTarget = { 0, 0, width, height },
					.dwBitRate = 0,
					.dwBitErrorRate = 0,
					.AvgTimePerFrame = (REFERENCE_TIME)(10000000/fps),
					.bmiHeader = {
						.biSize = sizeof(BITMAPINFOHEADER),
						.biWidth = width,
						.biHeight = height,
						.biPlanes = 0,
						.biBitCount = 0,
						.biCompression = 0,
						.biSizeImage = 0,
					},
				},
				.dwStartTimeCode = 0,
				.cbSequenceHeader = 0,
				.bSequenceHeader = {},
			};
			AM_MEDIA_TYPE media_type = {
				.majortype = MEDIATYPE_Video,
				.subtype = MEDIASUBTYPE_MPEG1Packet,
				.bFixedSizeSamples = false,
				.bTemporalCompression = true,
				.lSampleSize = 0,
				.formattype = FORMAT_MPEGVideo,
				.pUnk = nullptr,
				.cbFormat = sizeof(mediatype_head),
				.pbFormat = (BYTE*)&mediatype_head,
			};
			
			CopyMediaType((AM_MEDIA_TYPE*)pmt, &media_type);
		}
		else
		{
			int rate = 48000;
			
			MPEG1WAVEFORMAT mediatype_head = {
				.wfx = {
					.wFormatTag = WAVE_FORMAT_MPEG,
					.nChannels = 2,
					.nSamplesPerSec = (DWORD)rate,
					.nAvgBytesPerSec = 4000,
					.nBlockAlign = 48,
					.wBitsPerSample = 0,
					.cbSize = sizeof(mediatype_head),
				},
				.fwHeadLayer = ACM_MPEG_LAYER2,
				.dwHeadBitrate = (DWORD)rate,
				.fwHeadMode = ACM_MPEG_STEREO,
				.fwHeadModeExt = 0,
				.wHeadEmphasis = 0,
				.fwHeadFlags = ACM_MPEG_ID_MPEG1,
				.dwPTSLow = 0,
				.dwPTSHigh = 0,
			};
			AM_MEDIA_TYPE media_type = {
				.majortype = MEDIATYPE_Audio,
				.subtype = MEDIASUBTYPE_MPEG1AudioPayload, // wine understands only MPEG1AudioPayload, not MPEG1Packet or MPEG1Payload
				.bFixedSizeSamples = false,
				.bTemporalCompression = true,
				.lSampleSize = 0,
				.formattype = FORMAT_WaveFormatEx,
				.pUnk = nullptr,
				.cbFormat = sizeof(mediatype_head),
				.pbFormat = (BYTE*)&mediatype_head,
			};
			
			CopyMediaType((AM_MEDIA_TYPE*)pmt, &media_type);
		}
		return S_OK;
	}
	
	HRESULT STDMETHODCALLTYPE SetOutputType(DWORD dwOutputStreamIndex, const DMO_MEDIA_TYPE* pmt, DWORD dwFlags) override
	{
		debug("IMediaObject SetOutputType");
		if (pmt->formattype == FORMAT_MPEGVideo)
		{
			MPEG1VIDEOINFO* mt_body = (MPEG1VIDEOINFO*)pmt->pbFormat;
			debug("video %d\n", mt_body->hdr.rcSource.bottom);
		}
		return S_OK;
	}
	
	HRESULT STDMETHODCALLTYPE GetOutputSizeInfo(DWORD dwOutputStreamIndex, DWORD* pcbSize, DWORD* pcbAlignment) override
	{
		debug("IMediaObject GetOutputSizeInfo");
		if (dwOutputStreamIndex == 0)
		{
			// don't care, just pick some random numbers
			*pcbSize = 8192;
			*pcbAlignment = 1;
		}
		else
		{
			*pcbSize = 8192;
			*pcbAlignment = 1;
		}
		return S_OK;
	}
};

class CMpegVideoCodec : public base_dmo<CMpegVideoCodec> {
public:
	using base_dmo::base_dmo;
	
	HRESULT STDMETHODCALLTYPE GetStreamCount(DWORD* pcInputStreams, DWORD* pcOutputStreams) override
	{
		debug("IMediaObject GetStreamCount");
		*pcInputStreams = 1;
		*pcOutputStreams = 1;
		return S_OK;
	}
	
	HRESULT STDMETHODCALLTYPE SetInputType(DWORD dwInputStreamIndex, const DMO_MEDIA_TYPE* pmt, DWORD dwFlags) override
	{
		debug("IMediaObject SetInputType");
		return E_OUTOFMEMORY;
	}
};





template<typename T, bool aggregation = false>
class ClassFactory : public com_base<IClassFactory> {
public:
	HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* pUnkOuter, REFIID riid, void** ppvObject)
	{
		if constexpr (aggregation)
		{
			if (pUnkOuter != nullptr && riid != IID_IUnknown)
				return E_NOINTERFACE;
			*ppvObject = (void*)(new T(pUnkOuter))->real_iunknown();
			return S_OK;
		}
		else
		{
			if (pUnkOuter != nullptr)
				return CLASS_E_NOAGGREGATION;
			return qi_release(new T(), riid, ppvObject);
		}
	}
	HRESULT STDMETHODCALLTYPE LockServer(BOOL lock) { return S_OK; } // don't care
};

template<const GUID * guid>
class DmoClassFactory : public com_base<IClassFactory> {
public:
	HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* pUnkOuter, REFIID riid, void** ppvObject)
	{
		CComPtr<IDMOWrapperFilter> dmo;
		HRESULT hr = dmo.CoCreateInstance(CLSID_DMOWrapperFilter, pUnkOuter, CLSCTX_INPROC);
		if (FAILED(hr))
			return hr;
		hr = dmo->Init(*guid, DMOCATEGORY_VIDEO_DECODER);
		if (FAILED(hr))
			return hr;
		
		return dmo->QueryInterface(riid, ppvObject);
	}
	HRESULT STDMETHODCALLTYPE LockServer(BOOL lock) { return S_OK; } // don't care
};

static bool initialized = false;
EXPORT(HRESULT, DllGetClassObject, (REFCLSID rclsid, REFIID riid, void** ppvObj))
{
	if (!initialized)
	{
		setvbuf(stdout, nullptr, _IONBF, 0);
		initialized = true;
		DWORD dummy;
		CoRegisterClassObject(CLSID_MPEG1Splitter_PlmpegDmo, new ClassFactory<CMPEG1Splitter, true>,
		                      CLSCTX_INPROC_SERVER, REGCLS_MULTIPLEUSE, &dummy);
		CoRegisterClassObject(CLSID_CMpegVideoCodec_PlmpegDmo, new ClassFactory<CMpegVideoCodec, true>,
		                      CLSCTX_INPROC_SERVER, REGCLS_MULTIPLEUSE, &dummy);
	}
fprintf(stdout, "DllGetClassObject %s\n", guid_to_str(rclsid)); fflush(stdout);
//CloseHandle(CreateFileA("Z:\\home\\walrus\\Desktop\\DllGetClassObject.txt", GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
	if (rclsid == CLSID_MPEG1Splitter)
		return qi_release(new DmoClassFactory<&CLSID_MPEG1Splitter_PlmpegDmo>(), riid, ppvObj);
	if (rclsid == CLSID_CMpegVideoCodec)
		return qi_release(new DmoClassFactory<&CLSID_CMpegVideoCodec_PlmpegDmo>(), riid, ppvObj);
	return CLASS_E_CLASSNOTAVAILABLE;
}

EXPORT(HRESULT, DllCanUnloadNow, ())
{
	return S_FALSE; // just don't bother
}

/*

[Software\\Classes\\CLSID\\{FEB50740-7BEF-11CE-9BD9-0000E202599C}] 1641316191
#time=1d8018de2a218a4
@="MPEG-I Video Decoder"

[Software\\Classes\\CLSID\\{FEB50740-7BEF-11CE-9BD9-0000E202599C}\\InProcServer32] 1641316191
#time=1d8018de2a21926
@="Z:\\home\\walrus\\x\\plmpeg_gst\\plmpeg_wine.dll"
"ThreadingModel"="Both"

[Software\\Classes\\CLSID\\{336475D0-942A-11CE-A870-00AA002FEAB5}] 1641316191
#time=1d8018de2a218a4
@="MPEG-I Stream Splitter"

[Software\\Classes\\CLSID\\{336475D0-942A-11CE-A870-00AA002FEAB5}\\InProcServer32] 1641316191
#time=1d8018de2a21926
@="Z:\\home\\walrus\\x\\plmpeg_gst\\plmpeg_wine.dll"
"ThreadingModel"="Both"

[Software\\Classes\\Wow6432Node\\CLSID\\{FEB50740-7BEF-11CE-9BD9-0000E202599C}] 1641316191
#time=1d8018de2a218a4
@="MPEG-I Video Decoder"

[Software\\Classes\\Wow6432Node\\CLSID\\{FEB50740-7BEF-11CE-9BD9-0000E202599C}\\InProcServer32] 1641316191
#time=1d8018de2a21926
@="Z:\\home\\walrus\\x\\plmpeg_gst\\plmpeg_wine32.dll"
"ThreadingModel"="Both"

[Software\\Classes\\Wow6432Node\\CLSID\\{336475D0-942A-11CE-A870-00AA002FEAB5}] 1641316191
#time=1d8018de2a218a4
@="MPEG-I Stream Splitter"

[Software\\Classes\\Wow6432Node\\CLSID\\{336475D0-942A-11CE-A870-00AA002FEAB5}\\InProcServer32] 1641316191
#time=1d8018de2a21926
@="Z:\\home\\walrus\\x\\plmpeg_gst\\plmpeg_wine32.dll"
"ThreadingModel"="Both"





original


[Software\\Classes\\CLSID\\{336475D0-942A-11CE-A870-00AA002FEAB5}] 1641316191
#time=1d8018de2a218a4
@="MPEG-I Stream Splitter"

[Software\\Classes\\CLSID\\{336475D0-942A-11CE-A870-00AA002FEAB5}\\InProcServer32] 1641316191
#time=1d8018de2a21926
@="C:\\windows\\system32\\winegstreamer.dll"
"ThreadingModel"="Both"

[Software\\Classes\\Wow6432Node\\CLSID\\{336475D0-942A-11CE-A870-00AA002FEAB5}] 1641316197
#time=1d8018de622ae30
@="MPEG-I Stream Splitter"

[Software\\Classes\\Wow6432Node\\CLSID\\{336475D0-942A-11CE-A870-00AA002FEAB5}\\InProcServer32] 1641316197
#time=1d8018de622ae80
@="C:\\windows\\system32\\winegstreamer.dll"
"ThreadingModel"="Both"


*/

#ifdef __MINGW32__
// deleting these things removes a few kilobytes of binary and a dependency on libstdc++-6.dll
void* operator new(std::size_t n) _GLIBCXX_THROW(std::bad_alloc) { return malloc(n); }
void operator delete(void* p) noexcept { free(p); }
void operator delete(void* p, std::size_t n) noexcept { operator delete(p); }
extern "C" void __cxa_pure_virtual() { __builtin_trap(); }
extern "C" void _pei386_runtime_relocator() {}
#endif
