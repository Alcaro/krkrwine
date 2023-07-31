// SPDX-License-Identifier: LGPL-2.0-or-later

#define DOIT

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

// WARNING to anyone trying to use these objects to implement them for real in Wine:
// Wine's CLSID_CMpegAudioCodec demands packet boundaries to be in place. Therefore, this demuxer does that.
//    It's as if there's a builtin mpegaudioparse element.
// However, the demuxer does NOT do the same for video data!
// The demuxer does parse the video info somewhat, but only to discover the resolution. There is no builtin mpegvideoparse;
//    that functionality is instead part of the video decoder.
// This works fine for me, since this demuxer's video pin only needs to connect to the matching video decoder,
//    but it may look confusing to anyone investigating either object on its own.
// Additionally, WMCreateSyncReader, CLSID_CWMVDecMediaObject and CLSID_CWMADecMediaObject send very different data
//    between each other than the corresponding objects on Windows; you cannot use any of them to implement the others.
// Finally, these objects do not work on Windows; after attaching the output pin,
//    they segfault somewhere deep inside quartz.dll. I have not been able to determine why.

#define INITGUID
#define STRSAFE_NO_DEPRECATE
#include <typeinfo>
#include <windows.h>
#include <dshow.h>
#include <mmreg.h>
#include <d3d9.h>
#include <vmr9.h>
#include <wmsdk.h>
#include <wmcodecdsp.h>
#include <dmo.h>
#include <nserror.h>
#define PL_MPEG_IMPLEMENTATION
#include "pl_mpeg.h"

static const GUID GUID_NULL = {}; // not defined in my headers, how lovely

// some of these guids aren't defined in my headers, and some are only defined in some ways
// for example, __uuidof(IUnknown) is defined, but IID_IUnknown is not; IID_IAMStreamSelect is defined, but __uuidof(IAMStreamSelect) is not
DEFINE_GUID(IID_IUnknown, 0x00000000, 0x0000, 0x0000, 0xc0,0x00, 0x00,0x00,0x00,0x00,0x00,0x46);
DEFINE_GUID(IID_IAMOpenProgress,0x8E1C39A1, 0xDE53, 0x11cf, 0xAA, 0x63, 0x00, 0x80, 0xC7, 0x44, 0x52, 0x8D);
DEFINE_GUID(IID_IAMDeviceRemoval,0xf90a6130,0xb658,0x11d2,0xae,0x49,0x00,0x00,0xf8,0x75,0x4b,0x99);
__CRT_UUID_DECL(IAMStreamSelect, 0xc1960960, 0x17f5, 0x11d1, 0xab,0xe1, 0x00,0xa0,0xc9,0x05,0xf3,0x75)
// used only to detect DXVK
DEFINE_GUID(IID_ID3D9VkInteropDevice, 0x2eaa4b89, 0x0107, 0x4bdb, 0x87,0xf7, 0x0f,0x54,0x1c,0x49,0x3c,0xe0);

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
	HRESULT CopyTo(T** ppT)
	{
		p->AddRef();
		*ppT = p;
		return S_OK;
	}
};

template<typename T, typename... Ts> T first_helper();
template<typename... Tis>
class com_base_embedded : public Tis... {
private:
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
//printf("plm QI %s\n", guid_to_str(riid));
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

//static uint32_t n_combase_objects = 0;
template<typename... Tis>
class com_base : public com_base_embedded<Tis...> {
	uint32_t refcount = 1;
public:
	ULONG STDMETHODCALLTYPE AddRef() override
	{
//if (!strcmp(typeid(this).name(), "P8com_baseIJ11IBaseFilter27IVMRSurfaceAllocatorNotify9EE"))
//{
	//printf("addref, now %u\n", refcount+1);
//}
		return ++refcount;
	}
	ULONG STDMETHODCALLTYPE Release() override
	{
//if (!strcmp(typeid(this).name(), "P8com_baseIJ11IBaseFilter27IVMRSurfaceAllocatorNotify9EE"))
//{
	//printf("release, now %u\n", refcount-1);
//}
		uint32_t new_refcount = --refcount;
		if (!new_refcount)
			delete this;
		return new_refcount;
	}
	com_base()
	{
		// there's some weird race condition here... haven't bothered tracking it down
		// Wagamama High Spec Trial Edition segfaults on launch if I remove it
		Sleep(1);
		//DWORD n = InterlockedIncrement(&n_combase_objects);
		//printf("created a %s at %p, now %lu objects\n", typeid(this).name(), this, n);
	}
	virtual ~com_base()
	{
		//DWORD n = InterlockedDecrement(&n_combase_objects);
		//printf("deleted a %s at %p, now %lu objects\n", typeid(this).name(), this, n);
	}
};

template<typename Touter, typename Tinner>
Tinner com_enum_helper(HRESULT STDMETHODCALLTYPE (Touter::*)(ULONG, Tinner**, ULONG*));

// don't inline this lambda into the template's default argument, gcc bug 105667
static const auto addref_ptr = []<typename T>(T* obj) { obj->AddRef(); return obj; };
template<typename Tinterface, auto clone = addref_ptr>
class com_enum : public com_base<Tinterface> {
	using Tret = decltype(com_enum_helper(&Tinterface::Next));
	
	Tret** items;
	size_t pos = 0;
	size_t len;
	
	com_enum(Tret** items, size_t pos, size_t len) : items(items), pos(pos), len(len) {}
public:
	com_enum(Tret** items, size_t len) : items(items), len(len) {}
	
	HRESULT STDMETHODCALLTYPE Clone(Tinterface** ppEnum) override
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


template<typename T>
HRESULT qi_release(T* obj, REFIID riid, void** ppvObj)
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

static void convert_rgb24_to_rgb32(uint8_t * dst, const uint8_t * src, size_t n_pixels)
{
	for (size_t n=0;n<n_pixels-1;n++)
		memcpy(dst+n*4, src+n*3, 4);
	dst[(n_pixels-1)*4+0] = src[(n_pixels-1)*3+0];
	dst[(n_pixels-1)*4+1] = src[(n_pixels-1)*3+1];
	dst[(n_pixels-1)*4+2] = src[(n_pixels-1)*3+2];
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

template<typename Touter, typename... bases>
class base_filter : public com_base<IBaseFilter, bases...> {
public:
	void debug(const char * fmt, ...)
	{
		return;
		char buf[1024];
		
		va_list args;
		va_start(args, fmt);
		vsnprintf(buf, 1024, fmt, args);
		va_end(args);
		
#ifdef __cpp_rtti
		const char * my_typename = typeid(Touter).name();
#else
		const char * my_typename = "";
#endif
		fprintf(stdout, "plm %lu %s %s\n", GetCurrentThreadId(), my_typename, buf);
		fflush(stdout);
	}
	
	Touter* parent()
	{
		return (Touter*)this;
	}
	
	// several pointers in here aren't CComPtr, due to reference cycles
	IFilterGraph* graph = nullptr;
	
	FILTER_STATE state = State_Stopped;
	
	WCHAR filter_name[128];
	
	HRESULT STDMETHODCALLTYPE GetClassID(CLSID* pClassID) override
	{
		debug("IPersist GetClassID");
		*pClassID = {};
		return E_UNEXPECTED;
	}
	
	HRESULT STDMETHODCALLTYPE GetState(DWORD dwMilliSecsTimeout, FILTER_STATE* State) override
	{
		//debug("IMediaFilter GetState %lx", state);
		*State = state;
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE GetSyncSource(IReferenceClock** pClock) override
	{
		debug("IMediaFilter GetSyncSource");
		*pClock = nullptr;
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE Pause() override
	{
		debug("IMediaFilter Pause");
		state = State_Paused;
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE Run(REFERENCE_TIME tStart) override
	{
		debug("IMediaFilter Run");
		state = State_Running;
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE SetSyncSource(IReferenceClock* pClock) override
	{
		debug("IMediaFilter SetSyncSource");
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE Stop() override
	{
		debug("IMediaFilter Stop");
		state = State_Stopped;
		return S_OK;
	}
	
	IEnumPins* enum_pins() requires requires { sizeof(parent()->pins); }
	{
		return new com_enum<IEnumPins>(parent()->pins, sizeof(parent()->pins) / sizeof(parent()->pins[0]));
	}
	HRESULT STDMETHODCALLTYPE EnumPins(IEnumPins** ppEnum) override
	{
		debug("IBaseFilter EnumPins");
		*ppEnum = parent()->enum_pins();
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE FindPin(LPCWSTR Id, IPin** ppPin) override
	{
		debug("IBaseFilter FindPin");
		*ppPin = nullptr;
		return VFW_E_NOT_FOUND;
	}
	HRESULT STDMETHODCALLTYPE JoinFilterGraph(IFilterGraph* pGraph, LPCWSTR pName) override
	{
		debug("IBaseFilter JoinFilterGraph");
		if (pName)
			wcscpy(filter_name, pName);
		graph = pGraph;
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE QueryFilterInfo(FILTER_INFO* pInfo) override
	{
		debug("IBaseFilter QueryFilterInfo");
		wcscpy(pInfo->achName, filter_name);
		pInfo->pGraph = graph;
		graph->AddRef();
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE QueryVendorInfo(LPWSTR* pVendorInfo) override
	{
		debug("IBaseFilter QueryVendorInfo");
		return E_NOTIMPL;
	}
};

template<bool is_output, typename Tbase, typename Touter>
class base_pin : public Tbase {
public:
	IPin* peer = nullptr;
	
	void debug(const char * fmt, ...)
	{
		return;
		char buf[1024];
		
		va_list args;
		va_start(args, fmt);
		vsnprintf(buf, 1024, fmt, args);
		va_end(args);
		
#ifdef __cpp_rtti
		const char * my_typename = typeid(Touter).name();
#else
		const char * my_typename = "";
#endif
		fprintf(stdout, "plm %lu %s %s\n", GetCurrentThreadId(), my_typename, buf);
	}
	
	Touter* parent()
	{
		return (Touter*)this;
	}
	
	ULONG STDMETHODCALLTYPE AddRef() override { return parent()->parent()->AddRef(); }
	ULONG STDMETHODCALLTYPE Release() override { return parent()->parent()->Release(); }
	
	HRESULT STDMETHODCALLTYPE BeginFlush() override
	{
		debug("IPin BeginFlush");
		if (is_output)
			return E_UNEXPECTED;
		return E_OUTOFMEMORY;
	}
	HRESULT STDMETHODCALLTYPE Connect(IPin* pReceivePin, const AM_MEDIA_TYPE * pmt) override
	{
		debug("IPin Connect %p %p %d", pReceivePin, pmt, is_output);
		if constexpr (is_output)
		{
			if (true) // TODO: this is debug code, delete it
			{
				CComPtr<IEnumMediaTypes> p;
				debug("X=%.8lx", pReceivePin->EnumMediaTypes(&p));
				if (p)
				{
					AM_MEDIA_TYPE* pmt;
					while (p->Next(1, &pmt, nullptr) == S_OK)
					{
						debug("CANCONNECT %p %p %p\n", guid_to_str(pmt->majortype), guid_to_str(pmt->subtype), guid_to_str(pmt->formattype));
					}
				}
			}
			if (parent()->connect_output(pReceivePin))
			{
				debug("OUTPUT PIN CONNECTED");
				peer = pReceivePin;
				return S_OK;
			}
			return VFW_E_NO_ACCEPTABLE_TYPES;
		}
		else return E_UNEXPECTED;
	}
	HRESULT STDMETHODCALLTYPE ConnectedTo(IPin** pPin) override
	{
		debug("IPin ConnectedTo %p %p", pPin, peer);
		if (!peer)
			return VFW_E_NOT_CONNECTED;
		*pPin = peer;
		peer->AddRef();
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE ConnectionMediaType(AM_MEDIA_TYPE* pmt) override
	{
		debug("IPin ConnectionMediaType");
		return E_OUTOFMEMORY;
	}
	HRESULT STDMETHODCALLTYPE Disconnect() override
	{
		debug("IPin Disconnect");
		peer = nullptr;
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE EndFlush() override
	{
		if (is_output)
			return E_UNEXPECTED;
		debug("IPin EndFlush");
		return E_OUTOFMEMORY;
	}
	HRESULT STDMETHODCALLTYPE EndOfStream() override
	{
		debug("IPin EndOfStream");
		if constexpr (!is_output)
		{
			parent()->end_of_stream();
			return S_OK;
		}
		return E_UNEXPECTED;
	}
	HRESULT STDMETHODCALLTYPE EnumMediaTypes(IEnumMediaTypes** ppEnum) override
	{
		debug("IPin EnumMediaTypes");
		*ppEnum = nullptr;
		return E_OUTOFMEMORY;
	}
	HRESULT STDMETHODCALLTYPE NewSegment(REFERENCE_TIME tStart, REFERENCE_TIME tStop, double dRate) override
	{
		debug("IPin NewSegment");
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE QueryAccept(const AM_MEDIA_TYPE * pmt) override
	{
		debug("IPin QueryAccept");
		return E_OUTOFMEMORY;
	}
	HRESULT STDMETHODCALLTYPE QueryDirection(PIN_DIRECTION* pPinDir) override
	{
		debug("IPin QueryDirection");
		*pPinDir = is_output ? PINDIR_OUTPUT : PINDIR_INPUT;
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE QueryId(LPWSTR* Id) override
	{
		debug("IPin QueryId");
		return E_OUTOFMEMORY;
	}
	HRESULT STDMETHODCALLTYPE QueryInternalConnections(IPin** apPin, ULONG* nPin) override
	{
		debug("IPin QueryInternalConnections");
		return E_NOTIMPL;
	}
	HRESULT STDMETHODCALLTYPE QueryPinInfo(PIN_INFO* pInfo) override
	{
		debug("IPin QueryPinInfo");
		pInfo->pFilter = parent()->parent();
		pInfo->pFilter->AddRef();
		pInfo->dir = is_output ? PINDIR_OUTPUT : PINDIR_INPUT;
		wcscpy(pInfo->achName, is_output ? L"source" : L"sink");
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE ReceiveConnection(IPin* pConnector, const AM_MEDIA_TYPE * pmt) override
	{
		debug("IPin ReceiveConnection %s %s %s", guid_to_str(pmt->majortype), guid_to_str(pmt->subtype), guid_to_str(pmt->formattype));
		if constexpr (!is_output)
		{
			if (parent()->connect_input(pConnector, pmt))
			{
				debug("INPUT PIN CONNECTED");
				peer = pConnector;
				return S_OK;
			}
			return VFW_E_TYPE_NOT_ACCEPTED;
		}
		else return E_UNEXPECTED;
	}
	
	// IMemInputPin
	HRESULT STDMETHODCALLTYPE GetAllocator(IMemAllocator** ppAllocator)
	{
		debug("IMemInputPin GetAllocator");
		return VFW_E_NO_ALLOCATOR;
	}
	HRESULT STDMETHODCALLTYPE GetAllocatorRequirements(ALLOCATOR_PROPERTIES* pProps)
	{
		debug("IMemInputPin GetAllocatorRequirements");
		return E_NOTIMPL;
	}
	HRESULT STDMETHODCALLTYPE NotifyAllocator(IMemAllocator* pAllocator, BOOL bReadOnly)
	{
		debug("IMemInputPin NotifyAllocator, readonly=%u", bReadOnly);
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE Receive(IMediaSample* pSample)
	{
		BYTE* ptr;
		pSample->GetPointer(&ptr);
		size_t size = pSample->GetActualDataLength();
		//debug("IMemInputPin Receive %u", (unsigned)size);
		parent()->receive_input(ptr, size);
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE ReceiveCanBlock()
	{
		debug("IMemInputPin ReceiveCanBlock");
		return E_OUTOFMEMORY;
	}
	HRESULT STDMETHODCALLTYPE ReceiveMultiple(IMediaSample** pSamples, long nSamples, long* nSamplesProcessed)
	{
		debug("IMemInputPin ReceiveMultiple");
		return E_OUTOFMEMORY;
	}
	
	void send_packet(IMediaSample* samp, const uint8_t * ptr, size_t len)
	{
		uint8_t* ptr2;
		samp->GetPointer(&ptr2);
		samp->SetActualDataLength(len);
		memcpy(ptr2, ptr, len);
		
		CComPtr<IMemInputPin> peer_mem;
		peer->QueryInterface(IID_PPV_ARGS(&peer_mem));
		peer_mem->Receive(samp);
	}
};

static void sample_set_time(IMediaSample* samp, double pts, double duration)
{
	if (pts != -1.0)
	{
		REFERENCE_TIME t = pts * 10000000;
		if (duration != 0.0)
		{
			REFERENCE_TIME t2 = t + duration*10000000;
			samp->SetTime(&t, &t2);
			samp->SetMediaTime(&t, &t2);
		}
		else
		{
			samp->SetTime(&t, nullptr);
			samp->SetMediaTime(&t, nullptr);
		}
	}
	else
	{
		samp->SetTime(nullptr, nullptr);
		samp->SetMediaTime(nullptr, nullptr);
	}
}

// Given the start of an MP2 packet, returns some information about this packet.
// Returns 1 if ok, 0 if incomplete packet, -1 if corrupt packet.
#define MP2_PACKET_MAX_SIZE 1728
static inline int mp2_packet_parse(const uint8_t * ptr, size_t len, int* samplerate, int* n_channels, size_t* size)
{
	//size_t prefix_padding = 0;
	//while (len > 0 && *ptr == 0x00)
	//{
		//prefix_padding++;
		//ptr++;
		//len--;
	//}
	
	//header is 48 bits
	//11 bits sync, 0x7ff
	//2 bits version, must be 3
	//2 bits layer, must be 2
	//1 bit hasCRC, can be whatever
	
	//4 bits bitrate index
	//2 bits sample rate index
	//1 bit padding flag
	//1 bit private (ignore)
	
	//2 bits mode (stereo, joint stereo, dual channel, mono)
	//2 bits mode extension (used only for joint stereo)
	//4 bits copyright crap
	
	//16 bits checksum, if hasCRC
	
	if (len < 6)
		return 0;
	if (ptr[0] != 0xFF || (ptr[1]&0xFE) != 0xFC)
		return -1;
	
	static const uint16_t bitrates[16] = { 0xFFFF, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 0xFFFF };
	static const uint16_t samplerates[4] = { 44100, 48000, 32000, 0xFFFF };
	
	int bitrate = bitrates[(ptr[2]&0xF0)>>4];
	int sample_rate = samplerates[(ptr[2]&0x0C)>>2];
	if (bitrate == 0xFFFF || sample_rate == 0xFFFF)
		return -1;
	
	bool has_pad = (ptr[2]&0x02);
	
	if (samplerate)
		*samplerate = sample_rate;
	if (n_channels)
		*n_channels = ((ptr[3]&0xC0)>>6 == 3) ? 1 : 2;
	if (size)
		*size = (144000 * bitrate / sample_rate) + has_pad;
	return 1;
}

class CMPEG1Splitter : public base_filter<CMPEG1Splitter, IAMStreamSelect> {
public:
	SRWLOCK srw; // threading: safe
	CONDITION_VARIABLE wake_parent; // threading: safe
	CONDITION_VARIABLE wake_video; // threading: safe
	CONDITION_VARIABLE wake_audio; // threading: safe
	bool have_video = false; // threading: parent only
	bool have_audio = false; // threading: parent only
	plm_demux_t* demux_video = nullptr; // threading: lock
	plm_demux_t* demux_audio = nullptr; // threading: lock
	
	bool video_thread_exists = false; // threading: lock
	bool video_thread_stop = false; // threading: lock
	bool video_thread_active = false; // threading: lock; tells whether the child is currently writing anything
	bool audio_thread_exists = false; // threading: lock
	bool audio_thread_stop = false; // threading: lock
	bool audio_thread_active = false; // threading: lock; tells whether the child is currently writing anything
	
	// FILTER_STATE state; // (from parent) threading: lock
	
	~CMPEG1Splitter()
	{
		scoped_lock lock(&this->srw);
		video_thread_stop = true;
		audio_thread_stop = true;
		WakeConditionVariable(&this->wake_video);
		WakeConditionVariable(&this->wake_audio);
		while (video_thread_exists || audio_thread_exists)
			SleepConditionVariableSRW(&this->wake_parent, &this->srw, INFINITE, 0);
		if (demux_video)
			plm_demux_destroy(demux_video);
		if (demux_audio)
			plm_demux_destroy(demux_audio);
	}
	
	class in_pin : public base_pin<false, com_base_embedded<IPin>, in_pin> {
	public:
		CMPEG1Splitter* parent() { return container_of<&CMPEG1Splitter::pin_i>(this); }
		
		bool connect_input(IPin* pConnector, const AM_MEDIA_TYPE * pmt)
		{
			if (pmt->majortype == MEDIATYPE_Stream && pmt->subtype == MEDIASUBTYPE_MPEG1System)
			{
				CComPtr<IAsyncReader> ar;
				if (FAILED(pConnector->QueryInterface(&ar)))
					return false;
				
				parent()->start_threads(ar);
				return true;
			}
			
			return false;
		}
		
		void end_of_stream()
		{
			abort(); // we should get input only from IAsyncStream::SyncRead, EndOfStream() should be unreachable
		}
	};
	class out_pin_v : public base_pin<true, com_base_embedded<IPin>, out_pin_v> {
	public:
		CMPEG1Splitter* parent() { return container_of<&CMPEG1Splitter::pin_v>(this); }
		
		MPEG1VIDEOINFO mvi_mediatype;
		AM_MEDIA_TYPE am_mediatype;
		
		AM_MEDIA_TYPE* media_type()
		{
			int width = parent()->video_width;
			int height = parent()->video_height;
			double fps = parent()->video_fps;
			
			// why are some of those structs so obnoxiously deeply nested
			mvi_mediatype = {
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
			am_mediatype = {
				.majortype = MEDIATYPE_Video,
				.subtype = MEDIASUBTYPE_MPEG1Packet,
				.bFixedSizeSamples = false,
				.bTemporalCompression = true,
				.lSampleSize = 0,
				.formattype = FORMAT_MPEGVideo,
				.pUnk = nullptr,
				.cbFormat = sizeof(mvi_mediatype),
				.pbFormat = (BYTE*)&mvi_mediatype,
			};
			
			return &am_mediatype;
		}
		
		bool connect_output(IPin* pReceivePin)
		{
			if (!parent()->have_video)
				return false;
			return SUCCEEDED(pReceivePin->ReceiveConnection(this, media_type()));
		}
	};
	class out_pin_a : public base_pin<true, com_base_embedded<IPin>, out_pin_a> {
	public:
		CMPEG1Splitter* parent() { return container_of<&CMPEG1Splitter::pin_a>(this); }
		
		MPEG1WAVEFORMAT mwf_mediatype;
		AM_MEDIA_TYPE am_mediatype;
		
		AM_MEDIA_TYPE* media_type()
		{
			int rate = parent()->audio_rate;
			
			mwf_mediatype = {
				.wfx = {
					.wFormatTag = WAVE_FORMAT_MPEG,
					.nChannels = 2,
					.nSamplesPerSec = (DWORD)rate,
					.nAvgBytesPerSec = 4000,
					.nBlockAlign = 48,
					.wBitsPerSample = 0,
					.cbSize = sizeof(mwf_mediatype),
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
			am_mediatype = {
				.majortype = MEDIATYPE_Audio,
				.subtype = MEDIASUBTYPE_MPEG1AudioPayload, // wine understands only MPEG1AudioPayload, not MPEG1Packet or MPEG1Payload
				.bFixedSizeSamples = false,
				.bTemporalCompression = true,
				.lSampleSize = 0,
				.formattype = FORMAT_WaveFormatEx,
				.pUnk = nullptr,
				.cbFormat = sizeof(mwf_mediatype),
				.pbFormat = (BYTE*)&mwf_mediatype,
			};
			return &am_mediatype;
		}
		
		bool connect_output(IPin* pReceivePin)
		{
			if (!parent()->have_audio)
				return false;
			
			return SUCCEEDED(pReceivePin->ReceiveConnection(this, media_type()));
		}
	};
	in_pin pin_i;
	out_pin_v pin_v; // threading: parent only for most variables, read only while child exists for .peer
	out_pin_a pin_a; // threading: parent only for most variables, read only while child exists for .peer
	
	IPin* pins[3] = { &pin_i, &pin_v, &pin_a };
	
	int video_width;
	int video_height;
	double video_fps;
	int audio_rate;
	
	HRESULT STDMETHODCALLTYPE Run(REFERENCE_TIME tStart) override
	{
		debug("IMediaFilter Run but better");
		scoped_lock lock(&this->srw);
		state = State_Running;
		WakeConditionVariable(&this->wake_video);
		WakeConditionVariable(&this->wake_audio);
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE Pause() override
	{
		debug("IMediaFilter Pause but better");
		scoped_lock lock(&this->srw);
		state = State_Paused;
		WakeConditionVariable(&this->wake_video);
		WakeConditionVariable(&this->wake_audio);
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE Stop() override
	{
		debug("IMediaFilter Stop but better");
		scoped_lock lock(&this->srw);
		state = State_Stopped;
		while (video_thread_active || audio_thread_active)
			SleepConditionVariableSRW(&this->wake_parent, &this->srw, INFINITE, 0);
		return S_OK;
	}
	
	void start_threads(IAsyncReader* ar)
	{
		scoped_lock lock(&this->srw);
		
		// don't bother with async reading, just take the easiest solution
		int64_t ignore;
		int64_t size;
		ar->Length(&ignore, &size);
		uint8_t * ptr = (uint8_t*)malloc(size);
		ar->SyncRead(0, size, ptr);
		
		// just tell one to free it, and the other not
		demux_video = plm_demux_create(plm_buffer_create_with_memory(ptr, size, true), true);
		demux_audio = plm_demux_create(plm_buffer_create_with_memory(ptr, size, false), true);
		video_thread_active = false;
		audio_thread_active = false;
		
		// - all pins must be connected before any filter can move to Paused
		// - video decoder must know its output size before its pin can connect
		// - video decoder won't have any input data before moving to Paused
		// therefore, demuxer must provide this information to the decoder through another mechanism
		// therefore, demuxer must START UP AN ENTIRE DECODER to read the headers and extract the video size
		bool need_video = (plm_demux_get_num_video_streams(demux_video) > 0);
		bool need_audio = (plm_demux_get_num_audio_streams(demux_video) > 0);
		plm_buffer_t* video_buffer = plm_buffer_create_with_capacity(32768);
		plm_video_t* video_checker = plm_video_create_with_buffer(video_buffer, false);
		plm_buffer_t* audio_buffer = plm_buffer_create_with_capacity(32768);
		plm_audio_t* audio_checker = plm_audio_create_with_buffer(audio_buffer, false);
		
		while (need_video || need_audio)
		{
			plm_packet_t* pack = plm_demux_decode(demux_video);
			if (!pack)
				break;
			if (pack->type == PLM_DEMUX_PACKET_VIDEO_1 && need_video)
			{
				plm_buffer_write(video_buffer, pack->data, pack->length);
				if (plm_video_has_header(video_checker))
				{
					this->video_width = plm_video_get_width(video_checker);
					this->video_height = plm_video_get_height(video_checker);
					this->video_fps = plm_video_get_framerate(video_checker);
					have_video = true;
					need_video = false;
				}
			}
			if (pack->type == PLM_DEMUX_PACKET_AUDIO_1 && need_audio)
			{
				plm_buffer_write(audio_buffer, pack->data, pack->length);
				if (plm_audio_has_header(audio_checker))
				{
					this->audio_rate = plm_audio_get_samplerate(audio_checker);
					have_audio = true;
					need_audio = false;
				}
			}
		}
		
		plm_buffer_destroy(video_buffer);
		plm_video_destroy(video_checker);
		plm_buffer_destroy(audio_buffer);
		plm_audio_destroy(audio_checker);
		
		plm_demux_rewind(demux_video);
		
		// need two threads, or the audio pin blocks while the video pin demands more input
		// and I need two plm_demuxers, so they can go arbitrarily far out of sync
		CreateThread(nullptr, 0, &CMPEG1Splitter::video_thread_fn_wrap, this, 0, nullptr);
		CreateThread(nullptr, 0, &CMPEG1Splitter::audio_thread_fn_wrap, this, 0, nullptr);
	}
	
	static DWORD WINAPI video_thread_fn_wrap(void* lpParameter)
	{
		((CMPEG1Splitter*)lpParameter)->video_thread_fn();
		return 0;
	}
	void video_thread_fn()
	{
//printf("VIDEOTHREAD=%.8lx\n", GetCurrentThreadId());
		scoped_lock lock(&this->srw);
		bool first_packet = true;
		
		CComPtr<IMemAllocator> mem_alloc;
		mem_alloc.CoCreateInstance(CLSID_MemoryAllocator, NULL, CLSCTX_INPROC);
		ALLOCATOR_PROPERTIES props = { 1, 65536, 1, 0 };
		mem_alloc->SetProperties(&props, &props);
		mem_alloc->Commit();
		
		while (!this->video_thread_stop)
		{
//puts("vt1.");
			while (this->state == State_Stopped)
			{
//puts("vt2.");
				if (this->video_thread_stop)
					goto stop;
//puts("vt3.");
				if (video_thread_active)
				{
//puts("vt4.");
					video_thread_active = false;
					WakeConditionVariable(&this->wake_parent);
				}
//puts("vt5.");
				SleepConditionVariableSRW(&this->wake_video, &this->srw, INFINITE, 0);
//puts("vt6.");
			}
//puts("vt7.");
			video_thread_active = true;
			plm_packet_t* pack = plm_demux_decode(demux_video);
			if (!pack)
			{
				scoped_unlock unlock(&this->srw);
				if (pin_v.peer)
					pin_v.peer->EndOfStream();
				break;
			}
			if (pack->type == PLM_DEMUX_PACKET_VIDEO_1 && pin_v.peer)
			{
				scoped_unlock unlock(&this->srw);
				
				CComPtr<IMediaSample> samp;
				mem_alloc->GetBuffer(&samp, nullptr, nullptr, 0);
				
				samp->SetDiscontinuity(first_packet);
				samp->SetPreroll(false);
				samp->SetSyncPoint(false);
				sample_set_time(samp, pack->pts, 1.0 / video_fps);
				pin_v.send_packet(samp, pack->data, pack->length);
				
				first_packet = false;
			}
		}
		
	stop:
//puts("vt999.");
		this->video_thread_active = false;
		this->video_thread_exists = false;
		WakeConditionVariable(&this->wake_parent);
	}
	
	static DWORD WINAPI audio_thread_fn_wrap(void* lpParameter)
	{
		((CMPEG1Splitter*)lpParameter)->audio_thread_fn();
		return 0;
	}
	void audio_thread_fn()
	{
//printf("AUDIOTHREAD=%.8lx\n", GetCurrentThreadId());
		scoped_lock lock(&this->srw);
		bool first_packet = true;
		
		CComPtr<IMemAllocator> mem_alloc;
		mem_alloc.CoCreateInstance(CLSID_MemoryAllocator, NULL, CLSCTX_INPROC);
		ALLOCATOR_PROPERTIES props = { 16, MP2_PACKET_MAX_SIZE, 1, 0 };
		mem_alloc->SetProperties(&props, &props);
		mem_alloc->Commit();
		
		size_t audio_chunk_pos = 0;
		uint8_t audio_chunk[MP2_PACKET_MAX_SIZE];
		
		while (!this->audio_thread_stop)
		{
//puts("at1.");
			while (this->state == State_Stopped)
			{
//puts("at2.");
				if (this->audio_thread_stop)
					goto stop;
//puts("at3.");
				if (audio_thread_active)
				{
//puts("at4.");
					audio_thread_active = false;
					WakeConditionVariable(&this->wake_parent);
				}
//puts("at5.");
				SleepConditionVariableSRW(&this->wake_audio, &this->srw, INFINITE, 0);
//puts("at6.");
			}
//puts("at7.");
			audio_thread_active = true;
			plm_packet_t* pack = plm_demux_decode(demux_audio);
			if (!pack)
			{
				scoped_unlock unlock(&this->srw);
				if (pin_a.peer)
					pin_a.peer->EndOfStream();
				break;
			}
			if (pack->type == PLM_DEMUX_PACKET_AUDIO_1 && pin_a.peer)
			{
				scoped_unlock unlock(&this->srw);
				
				const uint8_t * new_buf = pack->data;
				size_t new_len = pack->length;
				
				while (new_len)
				{
					size_t claim = min(new_len, MP2_PACKET_MAX_SIZE - audio_chunk_pos);
					memcpy(audio_chunk + audio_chunk_pos, new_buf, claim);
					audio_chunk_pos += claim;
					new_buf += claim;
					new_len -= claim;
					
					size_t pack_size = SIZE_MAX;
					if (mp2_packet_parse(audio_chunk, audio_chunk_pos, NULL, NULL, &pack_size) < 0)
					{
						audio_chunk_pos = 0; // just discard it and see what happens
						break;
					}
					if (pack_size <= audio_chunk_pos)
					{
						CComPtr<IMediaSample> samp;
						mem_alloc->GetBuffer(&samp, nullptr, nullptr, 0);
						
						samp->SetDiscontinuity(first_packet);
						samp->SetPreroll(false);
						samp->SetSyncPoint(false);
						sample_set_time(samp, pack->pts, 0.0);
						pin_a.send_packet(samp, audio_chunk, pack_size);
						
						first_packet = false;
						
						memmove(audio_chunk, audio_chunk+pack_size, audio_chunk_pos-pack_size);
						audio_chunk_pos -= pack_size;
					}
				}
			}
		}
		
	stop:
		this->audio_thread_active = false;
		this->audio_thread_exists = false;
		WakeConditionVariable(&this->wake_parent);
	}
	
	HRESULT STDMETHODCALLTYPE Count(DWORD* pcStreams) override
	{
		*pcStreams = have_video + have_audio;
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE Enable(long lIndex, DWORD dwFlags) override
	{
		return S_OK; // just don't bother
	}
	HRESULT STDMETHODCALLTYPE Info(long lIndex, AM_MEDIA_TYPE** ppmt, DWORD* pdwFlags, LCID* plcid,
	                               DWORD* pdwGroup, LPWSTR* ppszName, IUnknown** ppObject, IUnknown** ppUnk) override
	{
		bool is_video = (have_video && lIndex == 0);
		if (have_video) lIndex--;
		bool is_audio = (have_audio && lIndex == 0);
		if (have_audio) lIndex--;
		
		if (!is_video && !is_audio)
			return S_FALSE;
		
		if (ppmt)
		{
			if (is_video)
				*ppmt = CreateMediaType(pin_v.media_type());
			if (is_audio)
				*ppmt = CreateMediaType(pin_a.media_type());
		}
		if (pdwFlags)
			*pdwFlags = AMSTREAMSELECTINFO_ENABLED; // just don't bother
		if (pdwGroup)
			*pdwGroup = is_audio;
		// ignore the others, Kirikiri just leaves them as null (and it never uses the group, it just stores it somewhere)
		return S_OK;
	}
};

class CMpegVideoCodec : public base_filter<CMpegVideoCodec> {
public:
	plm_buffer_t* buf;
	plm_video_t* decode;
	
	CMpegVideoCodec()
	{
		buf = plm_buffer_create_with_capacity(PLM_BUFFER_DEFAULT_SIZE);
		decode = plm_video_create_with_buffer(buf, false);
	}
	~CMpegVideoCodec()
	{
		plm_buffer_destroy(buf);
		plm_video_destroy(decode);
	}
	
	class in_pin : public base_pin<false, com_base_embedded<IPin, IMemInputPin>, in_pin> {
	public:
		CMpegVideoCodec* parent() { return container_of<&CMpegVideoCodec::pin_i>(this); }
		
		bool connect_input(IPin* pConnector, const AM_MEDIA_TYPE * pmt)
		{
			if (pmt->majortype == MEDIATYPE_Video && pmt->subtype == MEDIASUBTYPE_MPEG1Packet)
				return parent()->update_size_from_mediatype(pmt);
			return false;
		}
		
		void receive_input(uint8_t* ptr, size_t size)
		{
			plm_buffer_write(parent()->buf, ptr, size);
			
			while (true)
			{
				plm_frame_t* frame = plm_video_decode(parent()->decode);
				if (!frame)
					break;
				
				parent()->dispatch_frame(frame);
			}
		}
		
		void end_of_stream()
		{
			parent()->pin_o.peer->EndOfStream();
		}
	};
	class out_pin : public base_pin<true, com_base_embedded<IPin>, out_pin> {
	public:
		CMpegVideoCodec* parent() { return container_of<&CMpegVideoCodec::pin_o>(this); }
		
		bool connect_output(IPin* pReceivePin)
		{
			if (FAILED(pReceivePin->ReceiveConnection(this, parent()->media_type(MEDIASUBTYPE_YV12))) && 
			    FAILED(pReceivePin->ReceiveConnection(this, parent()->media_type(MEDIASUBTYPE_RGB24))) &&
			    FAILED(pReceivePin->ReceiveConnection(this, parent()->media_type(MEDIASUBTYPE_RGB32))))
			{
				return false;
			}
			
			parent()->commit_media_type();
			
			return true;
		}
	};
	in_pin pin_i;
	out_pin pin_o;
	
	IPin* pins[2] = { &pin_i, &pin_o };
	
	CComPtr<IMemAllocator> mem_alloc;
	
	VIDEOINFOHEADER vih_mediatype = {
		.rcSource = { 0, 0, -1, -1 },
		.rcTarget = { 0, 0, -1, -1 },
		.dwBitRate = 0,
		.dwBitErrorRate = 0,
		.AvgTimePerFrame = -1,
		.bmiHeader = {
			.biSize = sizeof(BITMAPINFOHEADER),
			.biWidth = -1,
			.biHeight = -1,
			.biPlanes = 1,
			.biBitCount = 0,
			.biCompression = 0,
			.biSizeImage = 0xFFFFFFFF,
		},
	};
	AM_MEDIA_TYPE am_mediatype = {
		.majortype = MEDIATYPE_Video,
		.subtype = {},
		.bFixedSizeSamples = false,
		.bTemporalCompression = true,
		.lSampleSize = 0,
		.formattype = FORMAT_VideoInfo,
		.pUnk = nullptr,
		.cbFormat = sizeof(vih_mediatype),
		.pbFormat = (BYTE*)&vih_mediatype,
	};
	
	bool update_size_from_mediatype(const AM_MEDIA_TYPE * pmt)
	{
		if (pmt->formattype != FORMAT_MPEGVideo)
			return false;
		
		const MPEG1VIDEOINFO * vi = (MPEG1VIDEOINFO*)pmt->pbFormat;
		vih_mediatype.rcSource = vi->hdr.rcSource;
		vih_mediatype.rcTarget = vi->hdr.rcTarget;
		vih_mediatype.AvgTimePerFrame = vi->hdr.AvgTimePerFrame;
		vih_mediatype.bmiHeader.biWidth = vi->hdr.bmiHeader.biWidth;
		vih_mediatype.bmiHeader.biHeight = vi->hdr.bmiHeader.biHeight;
		return true;
	}
	
	static void update_mediatype_from_subtype(AM_MEDIA_TYPE* pmt)
	{
		VIDEOINFOHEADER* vi = (VIDEOINFOHEADER*)pmt->pbFormat;
		BITMAPINFOHEADER* bmi = &vi->bmiHeader;
		if (pmt->subtype == MEDIASUBTYPE_YV12)
		{
			bmi->biBitCount = 12;
			bmi->biCompression = MAKEFOURCC('Y','V','1','2');
		}
		if (pmt->subtype == MEDIASUBTYPE_RGB24)
		{
			bmi->biBitCount = 24;
			bmi->biCompression = BI_RGB;
		}
		if (pmt->subtype == MEDIASUBTYPE_RGB32)
		{
			bmi->biBitCount = 32;
			bmi->biCompression = BI_RGB;
		}
		bmi->biSizeImage = bmi->biWidth * bmi->biHeight * bmi->biBitCount / 8;
	}
	
	AM_MEDIA_TYPE* media_type(GUID subtype)
	{
		am_mediatype.subtype = subtype;
		update_mediatype_from_subtype(&am_mediatype);
		return &am_mediatype;
	}
	
	void commit_media_type()
	{
		mem_alloc.CoCreateInstance(CLSID_MemoryAllocator, NULL, CLSCTX_INPROC);
		ALLOCATOR_PROPERTIES props = { 1, (int32_t)vih_mediatype.bmiHeader.biSizeImage, 1024, 0 };
		mem_alloc->SetProperties(&props, &props);
		mem_alloc->Commit();
	}
	
	void dispatch_frame(plm_frame_t* frame)
	{
		CComPtr<IMediaSample> samp;
		mem_alloc->GetBuffer(&samp, nullptr, nullptr, 0);
		
		samp->SetDiscontinuity(false);
		samp->SetPreroll(false);
		samp->SetSyncPoint(true);
		sample_set_time(samp, frame->time, 1.0 / plm_video_get_framerate(decode));
		
		uint8_t* ptr2;
		samp->GetPointer(&ptr2);
		samp->SetActualDataLength(frame->width * frame->height * vih_mediatype.bmiHeader.biBitCount / 8);
		
		if (vih_mediatype.bmiHeader.biBitCount == 12)
		{
			for (size_t y=0;y<frame->height;y++)
			{
				memcpy(ptr2, frame->y.data + frame->y.width*y, frame->width);
				ptr2 += frame->width;
			}
			for (size_t y=0;y<frame->height/2;y++)
			{
				memcpy(ptr2, frame->cr.data + frame->cr.width*y, frame->width/2);
				ptr2 += frame->width/2;
			}
			for (size_t y=0;y<frame->height/2;y++)
			{
				memcpy(ptr2, frame->cb.data + frame->cb.width*y, frame->width/2);
				ptr2 += frame->width/2;
			}
		}
		if (vih_mediatype.bmiHeader.biBitCount == 24)
		{
			plm_frame_to_bgr(frame, ptr2+(frame->height-1)*frame->width*3, -frame->width*3);
		}
		if (vih_mediatype.bmiHeader.biBitCount == 32)
		{
			plm_frame_to_bgra(frame, ptr2+(frame->height-1)*frame->width*4, -frame->width*4);
		}
		
		CComPtr<IMemInputPin> peer_mem;
		pin_o.peer->QueryInterface(IID_PPV_ARGS(&peer_mem));
		peer_mem->Receive(samp);
	}
};



// This class mostly represents functionality that exists in Wine, but works around a few Wine bugs.
// - IVMRSurfaceAllocatorNotify9::ChangeD3DDevice - semi-stub
//    once this one is fixed, delete members need_reinit, every the_*, and everything that uses them
// - IVMRSurfaceAllocatorNotify9::NotifyEvent - stub
//    once this one is fixed, simply delete the E_NOTIMPL check from NotifyEvent
// - Direct3D 9 can't draw on child windows
//    once this one is fixed, delete function is_window_visible, member parent_window, and everything that uses them
// The rest of the class is simply boilerplate to inject my hacks where I need them.
class CVideoMixingRenderer9 : public com_base<IBaseFilter> {
	static bool is_window_visible(HWND hwnd)
	{
		DWORD wnd_pid;
		DWORD my_pid = GetProcessId(GetCurrentProcess());
		GetWindowThreadProcessId(hwnd, &wnd_pid);
		if (wnd_pid != my_pid)
			return false;
		
		if (!IsWindowVisible(hwnd))
			return false;
		
		RECT rect;
		GetClientRect(hwnd, &rect);
		// I don't know what a visible 0x0 window means, but they exist sometimes
		if (rect.right == 0 || rect.bottom == 0)
			return false;
		
		return true;
	}
	static BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam)
	{
		HWND* ret = (HWND*)lParam;
		
		if (!is_window_visible(hwnd))
			return TRUE;
		if (*ret)
			return FALSE;
		*ret = hwnd;
		return TRUE;
	}
	static HWND find_the_only_visible_window()
	{
		HWND ret = nullptr;
		if (!EnumWindows(EnumWindowsProc, (LPARAM)&ret))
			return nullptr;
		return ret;
	}
	
	class fancy_surfalloc : public IVMRSurfaceAllocator9, IVMRImagePresenter9 {
	public:
		IVMRSurfaceAllocator9* parent_alloc;
		IVMRImagePresenter9* parent_pres;
		
		bool need_reinit = false;
		IDirect3DDevice9* the_d3ddevice;
		IDirect3DSurface9** the_surface;
		DWORD the_surfaceflags;
		DWORD_PTR the_userid;
		VMR9AllocationInfo the_allocinfo;
		
		bool need_move_window;
		//HWND parent_window = nullptr;
		//DWORD parent_orig_style;
		
		//CVideoMixingRenderer9* parent() { return container_of<&CVideoMixingRenderer9::surfalloc_wrapper>(this); }
		DWORD STDMETHODCALLTYPE AddRef() { return parent_alloc->AddRef(); }
		DWORD STDMETHODCALLTYPE Release() { return parent_alloc->Release(); }
		
		HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override
		{
//printf("QI(surfalloc)=%s\n", guid_to_str(riid));
			*ppvObject = nullptr;
			if (riid == __uuidof(IVMRImagePresenter9))
			{
				*ppvObject = (void*)(IVMRImagePresenter9*)this;
				this->AddRef();
				return S_OK;
			}
			return parent_alloc->QueryInterface(riid, ppvObject);
		}
		
		HRESULT STDMETHODCALLTYPE AdviseNotify(IVMRSurfaceAllocatorNotify9* lpIVMRSurfAllocNotify)
			{ return parent_alloc->AdviseNotify(lpIVMRSurfAllocNotify); }
		HRESULT STDMETHODCALLTYPE GetSurface(DWORD_PTR dwUserID, DWORD SurfaceIndex, DWORD SurfaceFlags, IDirect3DSurface9** lplpSurface)
		{
			the_surface = lplpSurface;
			the_surfaceflags = SurfaceFlags;
			return parent_alloc->GetSurface(dwUserID, SurfaceIndex, SurfaceFlags, lplpSurface);
		}
		HRESULT STDMETHODCALLTYPE InitializeDevice(DWORD_PTR dwUserID, VMR9AllocationInfo* lpAllocInfo, DWORD* lpNumBuffers)
		{
			need_reinit = false;
			need_move_window = true;
			// DXVK doesn't have this glitch
			// I'd rather detect wined3d, not DXVK, but those are the only two options, so good enough
			// (native windows shouldn't run this code at all)
			CComPtr<IUnknown> detect_dxvk;
			if (SUCCEEDED(the_d3ddevice->QueryInterface(IID_ID3D9VkInteropDevice, (void**)&detect_dxvk)))
				need_move_window = false;
			the_userid = dwUserID;
			the_allocinfo = *lpAllocInfo;
			return parent_alloc->InitializeDevice(dwUserID, lpAllocInfo, lpNumBuffers);
		}
		HRESULT STDMETHODCALLTYPE TerminateDevice(DWORD_PTR dwID)
		{
			return parent_alloc->TerminateDevice(dwID);
		}
		
		
		HRESULT STDMETHODCALLTYPE PresentImage(DWORD_PTR dwUserID, VMR9PresentationInfo* lpPresInfo)
		{
			if (need_move_window)
			{
				need_move_window = false;
				
				HWND parent = find_the_only_visible_window();
				HWND child = GetWindow(parent, GW_CHILD);
				while (child)
				{
					char buf[256];
					GetClassName(child, buf, 256);
					if (!strcmp(buf, "krmovie VMR9 Child Window Class"))
					{
						RECT rect;
						GetClientRect(child, &rect);
						POINT pt = { 0, 0 };
						ClientToScreen(parent, &pt);
						
						// need to hide the window before setting the parent
						// don't know if windows limitation, wine limitation, x11 limitation, window manager limitation, or whatever
						// don't really care either
						ShowWindow(child, SW_HIDE);
						
						// WS_POPUP seems to do pretty much nothing in this year (other than mess with WS_CHILD),
						// but it seems intended to be set on parented borderless windows like this, so let's do it
						SetWindowLong(child, GWL_STYLE, (GetWindowLong(child, GWL_STYLE) & ~WS_CHILD) | WS_POPUP);
						SetParent(child, nullptr);
						// no clue why owner is set by setting PARENT, but that's what it is
						SetWindowLongPtr(child, GWLP_HWNDPARENT, (LONG_PTR)parent);
						
						MoveWindow(child, pt.x, pt.y, rect.right, rect.bottom, FALSE);
						
						ShowWindow(child, SW_SHOW);
						
						//break;
					}
					child = GetWindow(child, GW_HWNDNEXT);
				}
			}
			return parent_pres->PresentImage(dwUserID, lpPresInfo);
		}
		HRESULT STDMETHODCALLTYPE StartPresenting(DWORD_PTR dwUserID)
		{
			return parent_pres->StartPresenting(dwUserID);
		}
		HRESULT STDMETHODCALLTYPE StopPresenting(DWORD_PTR dwUserID)
		{
			return parent_pres->StopPresenting(dwUserID);
		}
		
		void evil_reinit()
		{
			if (!need_reinit)
				return;
			
			need_reinit = false;
			the_surface[0]->Release();
			the_surface[0] = nullptr;
			this->TerminateDevice(the_userid);
			DWORD one = 1;
			this->InitializeDevice(the_userid, &the_allocinfo, &one);
			this->GetSurface(the_userid, 0, the_surfaceflags, the_surface);
		}
	};
	
	class fancy_san9 : public IVMRSurfaceAllocatorNotify9 {
	public:
		IVMRSurfaceAllocatorNotify9* parent;
		
		CVideoMixingRenderer9* container() { return container_of<&CVideoMixingRenderer9::san_wrapper>(this); }
		
		DWORD STDMETHODCALLTYPE AddRef() { return parent->AddRef(); }
		DWORD STDMETHODCALLTYPE Release() { return parent->Release(); }
		
		HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override
		{
//printf("QI(san)=%s\n", guid_to_str(riid));
			return parent->QueryInterface(riid, ppvObject);
		}
		
		HRESULT STDMETHODCALLTYPE AdviseSurfaceAllocator(DWORD_PTR dwUserID, IVMRSurfaceAllocator9* lpIVRMSurfaceAllocator)
		{
	//puts("FSAN9 AdviseSurfaceAllocator");
			container()->surfalloc_wrapper.parent_alloc = lpIVRMSurfaceAllocator;
			lpIVRMSurfaceAllocator->QueryInterface(&container()->surfalloc_wrapper.parent_pres);
			container()->surfalloc_wrapper.parent_pres->Release();
			return parent->AdviseSurfaceAllocator(dwUserID, &container()->surfalloc_wrapper);
		}
		HRESULT STDMETHODCALLTYPE AllocateSurfaceHelper(VMR9AllocationInfo* lpAllocInfo, DWORD* lpNumBuffers, IDirect3DSurface9** lplpSurface)
		{
	//puts("FSAN9 AllocateSurfaceHelper");
//DWORD gg=*lpNumBuffers;
HRESULT hr=parent->AllocateSurfaceHelper(lpAllocInfo, lpNumBuffers, lplpSurface);
//printf("FSAN9 AllocateSurfaceHelper %lu->%lu %.8lx\n", gg,*lpNumBuffers,hr);
return hr;
	
			return parent->AllocateSurfaceHelper(lpAllocInfo, lpNumBuffers, lplpSurface);
		}
		HRESULT STDMETHODCALLTYPE ChangeD3DDevice(IDirect3DDevice9* lpD3DDevice, HMONITOR hMonitor)
		{
	//puts("FSAN9 ChangeD3DDevice");
			container()->surfalloc_wrapper.need_reinit = true;
			container()->surfalloc_wrapper.the_d3ddevice = lpD3DDevice;
			HRESULT hr = parent->ChangeD3DDevice(lpD3DDevice, hMonitor);
			container()->surfalloc_wrapper.evil_reinit();
			return hr;
		}
		HRESULT STDMETHODCALLTYPE NotifyEvent(LONG EventCode, LONG_PTR Param1, LONG_PTR Param2)
		{
	//puts("FSAN9 NotifyEvent");
			HRESULT hr = parent->NotifyEvent(EventCode, Param1, Param2);
			if (hr == E_NOTIMPL)
				hr = container()->graph_mes->Notify(EventCode, Param1, Param2);
			return hr;
		}
		HRESULT STDMETHODCALLTYPE SetD3DDevice(IDirect3DDevice9* lpD3DDevice, HMONITOR hMonitor)
		{
	//puts("FSAN9 SetD3DDevice");
			container()->surfalloc_wrapper.the_d3ddevice = lpD3DDevice;
			return parent->SetD3DDevice(lpD3DDevice, hMonitor);
		}
	};
	
public:
	CComPtr<IBaseFilter> parent;
	CComPtr<IVMRMixerBitmap9> parent_bmp;
	IMediaEventSink* graph_mes;
	
	fancy_surfalloc surfalloc_wrapper;
	fancy_san9 san_wrapper;
	
	CVideoMixingRenderer9()
	{
//puts("CREATE VMR9");
		auto* pDllGetClassObject = (decltype(DllGetClassObject)*)GetProcAddress(GetModuleHandle("quartz.dll"), "DllGetClassObject");
		CComPtr<IClassFactory> fac;
		pDllGetClassObject(CLSID_VideoMixingRenderer9, IID_IClassFactory, (void**)&fac);
		fac->CreateInstance(nullptr, IID_IBaseFilter, (void**)&parent);
		parent->QueryInterface(&parent_bmp);
	}
	
	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override
	{
//printf("QI(vmr9)=%s\n", guid_to_str(riid));
		if (riid == __uuidof(IBaseFilter))
		{
			*ppvObject = (void*)(IBaseFilter*)this;
			this->AddRef();
			return S_OK;
		}
		if (riid == __uuidof(IVMRSurfaceAllocatorNotify9))
		{
			parent->QueryInterface(&san_wrapper.parent);
			// don't hold this ref, it's a ref cycle
			// I don't think COM objects are allowed to have different refcount for different interfaces, but Windows and Wine both do this
			san_wrapper.parent->Release();
			*ppvObject = (void*)(IVMRSurfaceAllocatorNotify9*)&san_wrapper;
			san_wrapper.AddRef();
			return S_OK;
		}
		return parent->QueryInterface(riid, ppvObject);
	}
	
	HRESULT STDMETHODCALLTYPE GetClassID(CLSID* pClassID) override
		{ return parent->GetClassID(pClassID); }
	HRESULT STDMETHODCALLTYPE GetState(DWORD dwMilliSecsTimeout, FILTER_STATE* State) override
		{ return parent->GetState(dwMilliSecsTimeout, State); }
	HRESULT STDMETHODCALLTYPE GetSyncSource(IReferenceClock** pClock) override
		{ return parent->GetSyncSource(pClock); }
	HRESULT STDMETHODCALLTYPE Pause() override
		{ return parent->Pause(); }
	HRESULT STDMETHODCALLTYPE Run(REFERENCE_TIME tStart) override
		{ return parent->Run(tStart); }
	HRESULT STDMETHODCALLTYPE SetSyncSource(IReferenceClock* pClock) override
		{ return parent->SetSyncSource(pClock); }
	HRESULT STDMETHODCALLTYPE Stop() override
		{ return parent->Stop(); }
	HRESULT STDMETHODCALLTYPE EnumPins(IEnumPins** ppEnum) override
		{ return parent->EnumPins(ppEnum); }
	HRESULT STDMETHODCALLTYPE FindPin(LPCWSTR Id, IPin** ppPin) override
		{ return parent->FindPin(Id, ppPin); }
	HRESULT STDMETHODCALLTYPE JoinFilterGraph(IFilterGraph* pGraph, LPCWSTR pName) override
	{
		graph_mes = nullptr;
		if (pGraph)
		{
			pGraph->QueryInterface(&graph_mes);
			graph_mes->Release(); // don't hold a reference to the graph, that's a ref cycle
		}
		return parent->JoinFilterGraph(pGraph, pName);
	}
	HRESULT STDMETHODCALLTYPE QueryFilterInfo(FILTER_INFO* pInfo) override
		{ return parent->QueryFilterInfo(pInfo); }
	HRESULT STDMETHODCALLTYPE QueryVendorInfo(LPWSTR* pVendorInfo) override
		{ return parent->QueryVendorInfo(pVendorInfo); }
};



class fancy_WMSyncReader : public com_base<IWMSyncReader, IWMProfile> {
public:
	CComPtr<IWMSyncReader> parent;
	CComPtr<IWMProfile> parent_prof;
	
	static const constexpr GUID expected_magic = { 0xe23d171f, 0x3df0, 0x4a57, 0x8d, 0xb5, 0x1b, 0xc3, 0x4c, 0xca, 0x97, 0x7a };
	struct sneaky_information {
		fancy_WMSyncReader* self;
		WORD streamnumber;
		GUID real_subtype;
		GUID magic;
	};
	
	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override
	{
		if (riid == IID_IWMProfile)
		{
			this->AddRef();
			*ppvObject = (void*)(IWMProfile*)this;
			return parent->QueryInterface(&parent_prof);
		}
		return parent->QueryInterface(riid, ppvObject);
	}
	
	HRESULT STDMETHODCALLTYPE Close() override
		{ return parent->Close(); }
	HRESULT STDMETHODCALLTYPE GetMaxOutputSampleSize(DWORD dwOutput, DWORD* pcbMax) override
		{ return parent->GetMaxOutputSampleSize(dwOutput, pcbMax); }
	HRESULT STDMETHODCALLTYPE GetMaxStreamSampleSize(WORD wStream, DWORD* pcbMax) override
		{ return parent->GetMaxStreamSampleSize(wStream, pcbMax); }
	HRESULT STDMETHODCALLTYPE GetNextSample(WORD wStreamNum, INSSBuffer** ppSample, QWORD* pcnsSampleTime, QWORD* pcnsDuration,
	                                        DWORD* pdwFlags, DWORD* pdwOutputNum, WORD* pwStreamNum) override
	{
		//printf("GETNEXTSAMPLE BEGIN %u\n", wStreamNum);
		HRESULT hr = parent->GetNextSample(wStreamNum, ppSample, pcnsSampleTime, pcnsDuration, pdwFlags, pdwOutputNum, pwStreamNum);
		//printf("GETNEXTSAMPLE END %.8lx\n", hr);
		if (hr == VFW_E_NOT_COMMITTED)
		{
			// this seems to happen while the filter graph is flushing during a seek
			// haven't investigated it very closely
			return NS_E_NO_MORE_SAMPLES;
		}
		return hr;
	}
	HRESULT STDMETHODCALLTYPE GetOutputCount(DWORD* pcOutputs) override
		{ return parent->GetOutputCount(pcOutputs); }
	HRESULT STDMETHODCALLTYPE GetOutputFormat(DWORD dwOutputNum, DWORD dwFormatNum, IWMOutputMediaProps** ppProps) override
		{ return parent->GetOutputFormat(dwOutputNum, dwFormatNum, ppProps); }
	HRESULT STDMETHODCALLTYPE GetOutputFormatCount(DWORD dwOutputNum, DWORD* pcFormats) override
		{ return parent->GetOutputFormatCount(dwOutputNum, pcFormats); }
	HRESULT STDMETHODCALLTYPE GetOutputNumberForStream(WORD wStreamNum, DWORD* pdwOutputNum) override
		{ return parent->GetOutputNumberForStream(wStreamNum, pdwOutputNum); }
	HRESULT STDMETHODCALLTYPE GetOutputProps(DWORD dwOutputNum, IWMOutputMediaProps** ppOutput) override
		{ return parent->GetOutputProps(dwOutputNum, ppOutput); }
	HRESULT STDMETHODCALLTYPE GetOutputSetting(DWORD dwOutputNum, LPCWSTR pszName,
	                                           WMT_ATTR_DATATYPE* pType, BYTE* pValue, WORD* pcbLength) override
		{ return parent->GetOutputSetting(dwOutputNum, pszName, pType, pValue, pcbLength); }
	HRESULT STDMETHODCALLTYPE GetReadStreamSamples(WORD wStreamNum, BOOL* pfCompressed) override
		{ return parent->GetReadStreamSamples(wStreamNum, pfCompressed); }
	HRESULT STDMETHODCALLTYPE GetStreamNumberForOutput(DWORD dwOutputNum, WORD* pwStreamNum) override
		{ return parent->GetStreamNumberForOutput(dwOutputNum, pwStreamNum); }
	HRESULT STDMETHODCALLTYPE GetStreamSelected(WORD wStreamNum, WMT_STREAM_SELECTION* pSelection) override
		{ return parent->GetStreamSelected(wStreamNum, pSelection); }
	HRESULT STDMETHODCALLTYPE Open(const WCHAR* pwszFilename) override
		{ return parent->Open(pwszFilename); }
	HRESULT STDMETHODCALLTYPE OpenStream(IStream* pStream) override
		{ return parent->OpenStream(pStream); }
	HRESULT STDMETHODCALLTYPE SetOutputProps(DWORD dwOutputNum, IWMOutputMediaProps* pOutput) override
		{ return parent->SetOutputProps(dwOutputNum, pOutput); }
	HRESULT STDMETHODCALLTYPE SetOutputSetting(DWORD dwOutputNum, LPCWSTR pszName,
	                                           WMT_ATTR_DATATYPE Type, const BYTE* pValue, WORD cbLength) override
		{ return parent->SetOutputSetting(dwOutputNum, pszName, Type, pValue, cbLength); }
	HRESULT STDMETHODCALLTYPE SetRange(QWORD cnsStartTime, LONGLONG cnsDuration) override
		{ return parent->SetRange(cnsStartTime, cnsDuration); }
	HRESULT STDMETHODCALLTYPE SetRangeByFrame(WORD wStreamNum, QWORD qwFrameNumber, LONGLONG cFramesToRead) override
		{ return parent->SetRangeByFrame(wStreamNum, qwFrameNumber, cFramesToRead); }
	HRESULT STDMETHODCALLTYPE SetReadStreamSamples(WORD wStreamNum, BOOL fCompressed) override
		{ return parent->SetReadStreamSamples(wStreamNum, fCompressed); }
	HRESULT STDMETHODCALLTYPE SetStreamsSelected(WORD cStreamCount, WORD* pwStreamNumbers, WMT_STREAM_SELECTION* pSelections) override
		{ return parent->SetStreamsSelected(cStreamCount, pwStreamNumbers, pSelections); }
	
	class fancy_WMStreamConfig : public com_base<IWMStreamConfig, IWMMediaProps> {
	public:
		fancy_WMSyncReader* owner;
		
		CComPtr<IWMStreamConfig> parent;
		CComPtr<IWMMediaProps> parent_props;
		
		HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override
		{
			if (riid == IID_IWMMediaProps)
			{
				this->AddRef();
				*ppvObject = (void*)(IWMMediaProps*)this;
				return parent->QueryInterface(&parent_props);
			}
			return parent->QueryInterface(riid, ppvObject);
		}
		
		HRESULT STDMETHODCALLTYPE GetBitrate(DWORD* pdwBitrate) override
			{ return parent->GetBitrate(pdwBitrate); }
		HRESULT STDMETHODCALLTYPE GetBufferWindow(DWORD* pmsBufferWindow) override
			{ return parent->GetBufferWindow(pmsBufferWindow); }
		HRESULT STDMETHODCALLTYPE GetConnectionName(WCHAR* pwszInputName, WORD* pcchInputName) override
			{ return parent->GetConnectionName(pwszInputName, pcchInputName); }
		HRESULT STDMETHODCALLTYPE GetStreamName(WCHAR* pwszStreamName, WORD* pcchStreamName) override
			{ return parent->GetStreamName(pwszStreamName, pcchStreamName); }
		HRESULT STDMETHODCALLTYPE GetStreamNumber(WORD* pwStreamNum) override
			{ return parent->GetStreamNumber(pwStreamNum); }
		HRESULT STDMETHODCALLTYPE GetStreamType(GUID* pguidStreamType) override
			{ return parent->GetStreamType(pguidStreamType); }
		HRESULT STDMETHODCALLTYPE SetBitrate(DWORD pdwBitrate) override
			{ return parent->SetBitrate(pdwBitrate); }
		HRESULT STDMETHODCALLTYPE SetBufferWindow(DWORD msBufferWindow) override
			{ return parent->SetBufferWindow(msBufferWindow); }
		HRESULT STDMETHODCALLTYPE SetConnectionName(LPCWSTR pwszInputName) override
			{ return parent->SetConnectionName(pwszInputName); }
		HRESULT STDMETHODCALLTYPE SetStreamName(LPCWSTR pwszStreamName) override
			{ return parent->SetStreamName(pwszStreamName); }
		HRESULT STDMETHODCALLTYPE SetStreamNumber(WORD wStreamNum) override
			{ return parent->SetStreamNumber(wStreamNum); }
		
		HRESULT STDMETHODCALLTYPE GetMediaType(WM_MEDIA_TYPE* pType, DWORD* pcbType) override
		{
			HRESULT hr = parent_props->GetMediaType(pType, pcbType);
			// subtype must be one of
			/*
			if( type == WMMEDIASUBTYPE_MP43 || type == WMMEDIASUBTYPE_MP4S || type == WMMEDIASUBTYPE_MPEG2_VIDEO ||
				type == WMMEDIASUBTYPE_MSS1 || type == WMMEDIASUBTYPE_MSS2 || type == WMMEDIASUBTYPE_WMVP ||
				type == WMMEDIASUBTYPE_WMAudio_Lossless || type == WMMEDIASUBTYPE_WMAudioV2 || type == WMMEDIASUBTYPE_WMAudioV7 ||
				type == WMMEDIASUBTYPE_WMAudioV8 || type == WMMEDIASUBTYPE_WMAudioV9 || type == WMMEDIASUBTYPE_WMSP1 ||
				type == WMMEDIASUBTYPE_WMV1 || type == WMMEDIASUBTYPE_WMV2 || type == WMMEDIASUBTYPE_WMV3 )
			*/
			// let's just take the first video and first audio format from the above
			
			// must also sneak in the real subtype somewhere
			*pcbType += sizeof(sneaky_information);
			
			if (pType)
			{
				uint8_t* after_end = (uint8_t*)pType->pbFormat + pType->cbFormat;
				sneaky_information sneak = { owner, 0, pType->subtype, expected_magic };
				parent->GetStreamNumber(&sneak.streamnumber);
				memcpy(after_end, &sneak, sizeof(sneaky_information));
				pType->cbFormat += sizeof(sneaky_information);
				
				if (pType->majortype == MEDIATYPE_Video)
					pType->subtype = WMMEDIASUBTYPE_MP43; // originally MEDIASUBTYPE_RGB24
				if (pType->majortype == MEDIATYPE_Audio)
					pType->subtype = WMMEDIASUBTYPE_WMAudio_Lossless; // originally WMMEDIASUBTYPE_PCM
			}
			return hr;
		}
		HRESULT STDMETHODCALLTYPE GetType(GUID* pguidType) override
			{ return parent_props->GetType(pguidType); }
		HRESULT STDMETHODCALLTYPE SetMediaType(WM_MEDIA_TYPE* pType) override
			{ return parent_props->SetMediaType(pType); }
	};
	
	HRESULT STDMETHODCALLTYPE AddMutualExclusion(IWMMutualExclusion* pME) override
		{ return parent_prof->AddMutualExclusion(pME); }
	HRESULT STDMETHODCALLTYPE AddStream(IWMStreamConfig* pConfig) override
		{ return parent_prof->AddStream(pConfig); }
	HRESULT STDMETHODCALLTYPE CreateNewMutualExclusion(IWMMutualExclusion** ppME) override
		{ return parent_prof->CreateNewMutualExclusion(ppME); }
	HRESULT STDMETHODCALLTYPE CreateNewStream(REFGUID guidStreamType, IWMStreamConfig** ppConfig) override
		{ return parent_prof->CreateNewStream(guidStreamType, ppConfig); }
	HRESULT STDMETHODCALLTYPE GetDescription(WCHAR* pwszDescription, DWORD* pcchDescription) override
		{ return parent_prof->GetDescription(pwszDescription, pcchDescription); }
	HRESULT STDMETHODCALLTYPE GetMutualExclusion(DWORD dwMEIndex, IWMMutualExclusion** ppME) override
		{ return parent_prof->GetMutualExclusion(dwMEIndex, ppME); }
	HRESULT STDMETHODCALLTYPE GetMutualExclusionCount(DWORD* pcMutexs) override
		{ return parent_prof->GetMutualExclusionCount(pcMutexs); }
	HRESULT STDMETHODCALLTYPE GetName(WCHAR* pwszName, DWORD* pcchName) override
		{ return parent_prof->GetName(pwszName, pcchName); }
	HRESULT STDMETHODCALLTYPE GetStream(DWORD dwStreamIndex, IWMStreamConfig** ppConfig) override
		{ return parent_prof->GetStream(dwStreamIndex, ppConfig); }
	HRESULT STDMETHODCALLTYPE GetStreamByNumber(WORD wStreamNumber, IWMStreamConfig** ppIStream) override
	{
		fancy_WMStreamConfig* ret = new fancy_WMStreamConfig();
		ret->owner = this;
		*ppIStream = ret;
		return parent_prof->GetStreamByNumber(wStreamNumber, &ret->parent);
	}
	HRESULT STDMETHODCALLTYPE GetStreamCount(DWORD* pcStreams) override
		{ return parent_prof->GetStreamCount(pcStreams); }
	HRESULT STDMETHODCALLTYPE GetVersion(WMT_VERSION* pdwVersion) override
		{ return parent_prof->GetVersion(pdwVersion); }
	HRESULT STDMETHODCALLTYPE ReconfigStream(IWMStreamConfig* pConfig) override
		{ return parent_prof->ReconfigStream(pConfig); }
	HRESULT STDMETHODCALLTYPE RemoveMutualExclusion(IWMMutualExclusion* pME) override
		{ return parent_prof->RemoveMutualExclusion(pME); }
	HRESULT STDMETHODCALLTYPE RemoveStream(IWMStreamConfig* pConfig) override
		{ return parent_prof->RemoveStream(pConfig); }
	HRESULT STDMETHODCALLTYPE RemoveStreamByNumber(WORD wStreamNum) override
		{ return parent_prof->RemoveStreamByNumber(wStreamNum); }
	HRESULT STDMETHODCALLTYPE SetDescription(const WCHAR* pwszDescription) override
		{ return parent_prof->SetDescription(pwszDescription); }
	HRESULT STDMETHODCALLTYPE SetName(const WCHAR* pwszName) override
		{ return parent_prof->SetName(pwszName); }
	
	void set_subtype(DWORD dwOutputNum, GUID subtype)
	{
		DWORD idx;
		parent->GetOutputNumberForStream(dwOutputNum, &idx);
		CComPtr<IWMOutputMediaProps> prop;
		parent->GetOutputProps(idx, &prop);
		char buf[256]; // needs 160 (or 90 for the audio channel), let's give it a little more
		WM_MEDIA_TYPE* mt = (WM_MEDIA_TYPE*)buf;
		DWORD n = sizeof(buf);
		prop->GetMediaType(mt, &n);
		if (mt->majortype == MEDIATYPE_Video && mt->subtype != subtype)
		{
			mt->subtype = subtype;
			prop->SetMediaType(mt);
			parent->SetOutputProps(idx, prop);
		}
	}
};

class fake_DMOObject final : public IMediaObject {
public:
	class own_iunknown_t : public IUnknown {
	public:
		ULONG refcount = 1;
		fake_DMOObject* parent() { return container_of<&fake_DMOObject::own_iunknown>(this); }
		
		HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override
		{
			return parent()->NonDelegatingQueryInterface(riid, ppvObject);
		}
		
		ULONG STDMETHODCALLTYPE AddRef() override
		{
			return ++refcount;
		}
		ULONG STDMETHODCALLTYPE Release() override
		{
			uint32_t new_refcount = --refcount;
			if (!new_refcount)
				delete parent();
			return new_refcount;
		}
	};
	own_iunknown_t own_iunknown;
	IUnknown* parent;
	
	AM_MEDIA_TYPE mt;
	BYTE mt_buf[128];
	bool convert_rgb24_rgb32 = false;
	
	CComPtr<IMediaBuffer> my_buffer;
	DWORD buf_dwFlags;
	REFERENCE_TIME buf_rtTimestamp;
	REFERENCE_TIME buf_rtTimelength;
	
	fancy_WMSyncReader* predecessor;
	WORD streamnumber;
	
	fake_DMOObject(IUnknown* outer) : parent(outer) {}
	
	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override
		{ return parent->QueryInterface(riid, ppvObject); }
	ULONG STDMETHODCALLTYPE AddRef() override
		{ return parent->AddRef(); }
	ULONG STDMETHODCALLTYPE Release() override
		{ return parent->Release(); }
	
	HRESULT NonDelegatingQueryInterface(REFIID riid, void** ppvObject)
	{
		if (riid == IID_IMediaObject)
		{
			parent->AddRef();
			*ppvObject = (void*)(IMediaObject*)this;
			return S_OK;
		}
		return E_NOINTERFACE;
	}
	
	HRESULT STDMETHODCALLTYPE AllocateStreamingResources() override
		{ return S_OK; }
	HRESULT STDMETHODCALLTYPE Discontinuity(DWORD dwInputStreamIndex) override
		{ return S_OK; }
	HRESULT STDMETHODCALLTYPE Flush() override
	{
		my_buffer = nullptr;
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE FreeStreamingResources() override
		{ return S_OK; }
	HRESULT STDMETHODCALLTYPE GetInputCurrentType(DWORD dwInputStreamIndex, DMO_MEDIA_TYPE* pmt) override
		{ return E_OUTOFMEMORY; }
	HRESULT STDMETHODCALLTYPE GetInputMaxLatency(DWORD dwInputStreamIndex, REFERENCE_TIME* prtMaxLatency) override
		{ return E_OUTOFMEMORY; }
	HRESULT STDMETHODCALLTYPE GetInputSizeInfo(DWORD dwInputStreamIndex, DWORD* pcbSize,
	                                           DWORD* pcbMaxLookahead, DWORD* pcbAlignment) override
		{ return E_OUTOFMEMORY; }
	HRESULT STDMETHODCALLTYPE GetInputStatus(DWORD dwInputStreamIndex, DWORD* dwFlags) override
		{ return E_OUTOFMEMORY; }
	HRESULT STDMETHODCALLTYPE GetInputStreamInfo(DWORD dwInputStreamIndex, DWORD* pdwFlags) override
		{ return E_OUTOFMEMORY; }
	HRESULT STDMETHODCALLTYPE GetInputType(DWORD dwInputStreamIndex, DWORD dwTypeIndex, DMO_MEDIA_TYPE* pmt) override
		{ return E_OUTOFMEMORY; }
	HRESULT STDMETHODCALLTYPE GetOutputCurrentType(DWORD dwOutputStreamIndex, DMO_MEDIA_TYPE* pmt) override
		{ return E_OUTOFMEMORY; }
	HRESULT STDMETHODCALLTYPE GetOutputSizeInfo(DWORD dwOutputStreamIndex, DWORD* pcbSize, DWORD* pcbAlignment) override
	{
		if (dwOutputStreamIndex != 0)
			return DMO_E_INVALIDSTREAMINDEX;
		if (mt.formattype == FORMAT_VideoInfo || mt.formattype == FORMAT_VideoInfo2)
		{
			VIDEOINFOHEADER* vi = (VIDEOINFOHEADER*)mt.pbFormat;
			*pcbSize = vi->bmiHeader.biBitCount * vi->bmiHeader.biWidth * vi->bmiHeader.biHeight / 8;
			*pcbAlignment = 1; // Kirikiri TBufferRendererAllocator::SetProperties accepts only 1, anything else returns VFW_E_BADALIGN
		}
		else if (mt.formattype == FORMAT_WaveFormatEx)
		{
			WAVEFORMATEX* wfx = (WAVEFORMATEX*)mt.pbFormat;
			*pcbSize = wfx->nAvgBytesPerSec * 4; // usual buffer size is 1 second, but let's give it some more
			*pcbAlignment = 8; // float32 * two channels, should be enough
		}
		else
		{
			*pcbSize = 1024*1024; // just pick something
			*pcbAlignment = 8;
		}
//printf("GETOUTPUTSIZEINFO %lu -> %lu\n", dwOutputStreamIndex, *pcbSize);
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE GetOutputStreamInfo(DWORD dwOutputStreamIndex, DWORD* pdwFlags) override
		{ return E_OUTOFMEMORY; }
	HRESULT STDMETHODCALLTYPE GetOutputType(DWORD dwOutputStreamIndex, DWORD dwTypeIndex, DMO_MEDIA_TYPE* pmt) override
	{
//printf("get output type %lu\n", dwTypeIndex);
		if (dwOutputStreamIndex != 0)
			return DMO_E_INVALIDSTREAMINDEX;
		if (dwTypeIndex >= 3 || (dwTypeIndex == 1 && mt.majortype != MEDIATYPE_Video))
			return DMO_E_NO_MORE_ITEMS;
		CopyMediaType((AM_MEDIA_TYPE*)pmt, &mt);
		if (pmt->majortype == MEDIATYPE_Video)
		{
			if (dwTypeIndex == 0)
			{
				pmt->subtype = MEDIASUBTYPE_YV12;
				CMpegVideoCodec::update_mediatype_from_subtype((AM_MEDIA_TYPE*)pmt);
			}
			if (dwTypeIndex == 1)
			{
				pmt->subtype = MEDIASUBTYPE_RGB24;
				CMpegVideoCodec::update_mediatype_from_subtype((AM_MEDIA_TYPE*)pmt);
			}
			if (dwTypeIndex == 2)
			{
				pmt->subtype = MEDIASUBTYPE_RGB32;
				CMpegVideoCodec::update_mediatype_from_subtype((AM_MEDIA_TYPE*)pmt);
			}
		}
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE GetStreamCount(DWORD* pcInputStreams, DWORD* pcOutputStreams) override
	{
		*pcInputStreams = 1;
		*pcOutputStreams = 1;
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE Lock(LONG bLock) override
		{ return E_OUTOFMEMORY; }
	HRESULT STDMETHODCALLTYPE ProcessInput(DWORD dwInputStreamIndex, IMediaBuffer* pBuffer, DWORD dwFlags,
	                                       REFERENCE_TIME rtTimestamp, REFERENCE_TIME rtTimelength) override
	{
//printf("ProcessInput %p\n", this);
		if (my_buffer)
			return E_OUTOFMEMORY;
		my_buffer = pBuffer;
		buf_dwFlags = dwFlags;
		buf_rtTimestamp = rtTimestamp;
		buf_rtTimelength = rtTimelength;
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE ProcessOutput(DWORD dwFlags, DWORD cOutputBufferCount,
	                                        DMO_OUTPUT_DATA_BUFFER* pOutputBuffers, DWORD* pdwStatus) override
	{
//printf("ProcessOutput %p\n", this);
		if (!my_buffer)
			return S_FALSE;
		if (cOutputBufferCount != 1)
			return E_FAIL;
		
		BYTE* bytes;
		DWORD len;
		my_buffer->GetBufferAndLength(&bytes, &len);
		if (convert_rgb24_rgb32)
		{
			pOutputBuffers[0].pBuffer->SetLength(len*4/3);
			BYTE* bytes2;
			DWORD len2;
			pOutputBuffers[0].pBuffer->GetBufferAndLength(&bytes2, &len2);
			convert_rgb24_to_rgb32(bytes2, bytes, len/3);
		}
		else
		{
			pOutputBuffers[0].pBuffer->SetLength(len);
			BYTE* bytes2;
			DWORD len2;
			pOutputBuffers[0].pBuffer->GetBufferAndLength(&bytes2, &len2);
			memcpy(bytes2, bytes, len2);
		}
		pOutputBuffers[0].dwStatus = buf_dwFlags & ~DMO_OUTPUT_DATA_BUFFERF_INCOMPLETE;
		pOutputBuffers[0].rtTimestamp = buf_rtTimestamp;
		pOutputBuffers[0].rtTimelength = buf_rtTimelength;
		my_buffer = nullptr;
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE SetInputMaxLatency(DWORD dwInputStreamIndex, REFERENCE_TIME rtMaxLatency) override
		{ return E_OUTOFMEMORY; }
	HRESULT STDMETHODCALLTYPE SetInputType(DWORD dwInputStreamIndex, const DMO_MEDIA_TYPE* pmt, DWORD dwFlags) override
	{
		if (dwInputStreamIndex != 0)
			return DMO_E_INVALIDSTREAMINDEX;
		if (!pmt && (dwFlags & DMO_SET_TYPEF_CLEAR))
			return S_OK;
		fancy_WMSyncReader::sneaky_information sneak;
		uint8_t* after_end = (uint8_t*)pmt->pbFormat + pmt->cbFormat - sizeof(sneak);
		memcpy(&sneak, after_end, sizeof(sneak));
		if (sneak.magic != fancy_WMSyncReader::expected_magic)
			return DMO_E_TYPE_NOT_ACCEPTED;
		if (!(dwFlags & DMO_SET_TYPEF_TEST_ONLY))
		{
			mt = *(AM_MEDIA_TYPE*)pmt;
			mt.subtype = sneak.real_subtype;
			mt.cbFormat -= sizeof(sneak);
			memcpy(mt_buf, mt.pbFormat, mt.cbFormat);
			mt.pbFormat = mt_buf;
			
			predecessor = sneak.self;
			streamnumber = sneak.streamnumber;
		}
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE SetOutputType(DWORD dwOutputStreamIndex, const DMO_MEDIA_TYPE* pmt, DWORD dwFlags) override
	{
		if (dwOutputStreamIndex != 0)
			return DMO_E_INVALIDSTREAMINDEX;
		if (dwFlags & DMO_SET_TYPEF_CLEAR)
			return S_OK;
//if (pmt->subtype == MEDIASUBTYPE_YV12)
	//printf("set subtype YV12 fl %lu\n", dwFlags);
//else if (pmt->subtype == MEDIASUBTYPE_RGB24)
	//printf("set subtype RGB24 fl %lu\n", dwFlags);
//else if (pmt->subtype == MEDIASUBTYPE_RGB32)
	//printf("set subtype RGB32 fl %lu\n", dwFlags);
//else if (pmt->subtype == WMMEDIASUBTYPE_PCM)
	//printf("set subtype PCM fl %lu\n", dwFlags);
//else
	//printf("set subtype %s fl %lu\n",guid_to_str(pmt->subtype), dwFlags);
		if (pmt->subtype != mt.subtype && !(dwFlags & DMO_SET_TYPEF_TEST_ONLY))
		{
			mt.subtype = pmt->subtype;
			convert_rgb24_rgb32 = false;
			if (mt.subtype == MEDIASUBTYPE_RGB32)
			{
				convert_rgb24_rgb32 = true;
				predecessor->set_subtype(streamnumber, MEDIASUBTYPE_RGB24);
			}
			else
				predecessor->set_subtype(streamnumber, mt.subtype);
			CMpegVideoCodec::update_mediatype_from_subtype(&mt);
		}
		return S_OK;
	}
};

template<typename T>
class ClassFactory : public com_base<IClassFactory> {
public:
	HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* pUnkOuter, REFIID riid, void** ppvObject)
	{
		if (pUnkOuter != nullptr)
			return CLASS_E_NOAGGREGATION;
		return qi_release(new T(), riid, ppvObject);
	}
	HRESULT STDMETHODCALLTYPE LockServer(BOOL lock) { return S_OK; } // don't care
};

template<typename T>
class AggregatingClassFactory : public com_base<IClassFactory> {
public:
	HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* pUnkOuter, REFIID riid, void** ppvObject)
	{
		if (pUnkOuter == nullptr)
			return VFW_E_NEED_OWNER; // wrong if this isn't a VFW object, but who cares, it's an error, good enough
		if (riid != IID_IUnknown)
			return E_NOINTERFACE;
		T* ret = new T(pUnkOuter);
		*ppvObject = (void*)(IUnknown*)&ret->own_iunknown;
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE LockServer(BOOL lock) { return S_OK; } // don't care
};

typedef HRESULT STDMETHODCALLTYPE (*WMCreateSyncReader_t)(IUnknown* pUnkCert, DWORD dwRights, IWMSyncReader** ppSyncReader);
static WMCreateSyncReader_t orig_WMCreateSyncReader;
static HRESULT STDMETHODCALLTYPE myWMCreateSyncReader(IUnknown* pUnkCert, DWORD dwRights, IWMSyncReader** ppSyncReader)
{
	fancy_WMSyncReader* ret = new fancy_WMSyncReader();
	*ppSyncReader = ret;
	//puts("krkrwine: creating sync reader");
	HRESULT hr = orig_WMCreateSyncReader(pUnkCert, dwRights, &ret->parent);
	//puts("krkrwine: created sync reader");
	return hr;
}
static FARPROC STDMETHODCALLTYPE myGetProcAddress(HMODULE hModule, LPCSTR lpProcName)
{
	if (!strcmp(lpProcName, "WMCreateSyncReader"))
	{
		orig_WMCreateSyncReader = (WMCreateSyncReader_t)GetProcAddress(hModule, "WMCreateSyncReader");
		return (FARPROC)myWMCreateSyncReader;
	}
	return GetProcAddress(hModule, lpProcName);
}

static void very_unsafe_init()
{
	static bool initialized = false;
	if (initialized)
		return;
	initialized = true;
	
	// refuse to operate on Windows
	if (!GetProcAddress(GetModuleHandle("ntdll.dll"), "wine_get_version"))
		return;
	
	DWORD ignore;
	
	IClassFactory* fac_mpegsplit = new ClassFactory<CMPEG1Splitter>();
	CoRegisterClassObject(CLSID_MPEG1Splitter, fac_mpegsplit, CLSCTX_INPROC_SERVER, REGCLS_MULTIPLEUSE, &ignore);
	
	IClassFactory* fac_mpegvideo = new ClassFactory<CMpegVideoCodec>();
	CoRegisterClassObject(CLSID_CMpegVideoCodec, fac_mpegvideo, CLSCTX_INPROC_SERVER, REGCLS_MULTIPLEUSE, &ignore);
	
	IClassFactory* fac_vmr9 = new ClassFactory<CVideoMixingRenderer9>();
	CoRegisterClassObject(CLSID_VideoMixingRenderer9, fac_vmr9, CLSCTX_INPROC_SERVER, REGCLS_MULTIPLEUSE, &ignore);
	
	// if all I needed was to swap/implement the above objects, I'd put that in install.py
	// unfortunately, I also need to hijack WMCreateSyncReader, which is loaded via GetProcAddress
	// I need some seriously deep shenanigans to be able to access that
	
	// _rmovie is just a debug aid
	// use LoadLibrary, not GetProcAddress, so my hacks aren't undone if the DLL is unloaded and reloaded
	HMODULE mod = LoadLibrary("_rmovie.dll");
	if (!mod)
		mod = LoadLibrary("krmovie.dll");
	if (!mod)
		return;
	
	uint8_t* base_addr = (uint8_t*)mod;
	IMAGE_DOS_HEADER* head_dos = (IMAGE_DOS_HEADER*)base_addr;
	IMAGE_NT_HEADERS* head_nt = (IMAGE_NT_HEADERS*)(base_addr + head_dos->e_lfanew);
	IMAGE_DATA_DIRECTORY section_dir = head_nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
	IMAGE_IMPORT_DESCRIPTOR* imports = (IMAGE_IMPORT_DESCRIPTOR*)(base_addr + section_dir.VirtualAddress);
	
	void* find_function = (void*)GetProcAddress;
	//void* find_function = (void*)GetProcAddress(GetModuleHandle("kernel32.dll"), "GetProcAddress"); // in case your compiler sucks
	void* replace_function = (void*)myGetProcAddress;
	
	while (imports->Name)
	{
		void* * out = (void**)(base_addr + imports->FirstThunk);
		while (*out)
		{
			if (*out == find_function)
			{
				// can't just *out = replace_function, import table is read only
				WriteProcessMemory(GetCurrentProcess(), out, &replace_function, sizeof(replace_function), NULL);
			}
			out++;
		}
		imports++;
	}
	
	// only register these if the fake WMCreateSyncReader can be injected
	IClassFactory* fac_dmo = new AggregatingClassFactory<fake_DMOObject>();
	CoRegisterClassObject(CLSID_CWMVDecMediaObject, fac_dmo, CLSCTX_INPROC_SERVER, REGCLS_MULTIPLEUSE, &ignore);
	CoRegisterClassObject(CLSID_CWMADecMediaObject, fac_dmo, CLSCTX_INPROC_SERVER, REGCLS_MULTIPLEUSE, &ignore);
}

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

EXPORT(HRESULT, DllGetClassObject, (REFCLSID rclsid, REFIID riid, void** ppvObj))
{
	//freopen("Z:\\dev\\stdout", "wt", stdout);
	//setvbuf(stdout, nullptr, _IONBF, 0);
	//puts("krkrwine - hello world");
	
	very_unsafe_init();
	
	// this DllGetClassObject doesn't actually implement anything, it just calls very_unsafe_init then defers to the original
	return ((decltype(DllGetClassObject)*)GetProcAddress(LoadLibrary("quartz.dll"), "DllGetClassObject"))(rclsid, riid, ppvObj);
}

EXPORT(HRESULT, DllCanUnloadNow, ())
{
	return S_FALSE; // just don't bother
}

#ifdef __MINGW32__
// deleting these things removes a few kilobytes of binary and a dependency on libstdc++-6.dll
void* operator new(std::size_t n) _GLIBCXX_THROW(std::bad_alloc) { return malloc(n); }
void operator delete(void* p) noexcept { free(p); }
void operator delete(void* p, std::size_t n) noexcept { operator delete(p); }
extern "C" void __cxa_pure_virtual() { __builtin_trap(); }
extern "C" void _pei386_runtime_relocator() {}
#endif
