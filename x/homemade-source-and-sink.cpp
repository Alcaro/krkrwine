// SPDX-License-Identifier: LGPL-2.0-or-later

// can be compiled with
// wine g++ -std=c++20 nanodecode3.cpp -lole32 -fno-exceptions -fno-rtti
// (where g++ refers to, for example, https://winlibs.com/ )

// the resulting a.bin can be converted to png with imagemagick
// convert -depth 8 -size 640x360 bgr:a.bin -flip a.png
// (size may be different if you're using a video other than data_video_sample_640x360.mpeg)

#define INITGUID
#define STRSAFE_NO_DEPRECATE
#include <windows.h>
#include <stdio.h>
#include <dshow.h>
#include <stdint.h>
#include <typeinfo>

static const GUID GUID_NULL = {}; // not defined in my headers, how lovely
DEFINE_GUID(CLSID_decodebin_parser, 0xf9d8d64e, 0xa144, 0x47dc, 0x8e, 0xe0, 0xf5, 0x34, 0x98, 0x37, 0x2c, 0x29);

static char* guid_to_str(const GUID& guid)
{
	static char buf[8][64];
	static int n = 0;
	char* ret = buf[n++%8];
	sprintf(ret, "{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", guid.Data1, guid.Data2, guid.Data3, guid.Data4[0], guid.Data4[1], guid.Data4[2], guid.Data4[3], guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7]);
	return ret;
}


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
printf("plm QI %s\n", guid_to_str(riid));
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

template<typename Touter, typename Tinner>
Tinner com_enum_helper(HRESULT STDMETHODCALLTYPE (Touter::*)(ULONG, Tinner**, ULONG*));

template<typename Timpl>
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
		size_t ret = celt;
		if (ret > remaining)
			ret = remaining;
		for (size_t n=0;n<ret;n++)
		{
			rgelt[n] = items[pos++];
			rgelt[n]->AddRef();
		}
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
class base_filter : public com_base<IBaseFilter/*, ISpecifyPropertyPages*/> {
public:
	void debug(const char * fmt, ...)
	{
		char buf[1024];
		
		va_list args;
		va_start(args, fmt);
		vsnprintf(buf, 1024, fmt, args);
		va_end(args);
		
		fprintf(stdout, "%lu %s %s\n", GetCurrentThreadId(), typeid(Touter).name(), buf);
		fflush(stdout);
	}
	
	Touter* parent()
	{
		return (Touter*)this;
	}
	
	// several pointers in here aren't CComPtr, due to reference cycles
	IFilterGraph* graph = nullptr;
	
	FILTER_STATE state = State_Stopped;
	
	HRESULT STDMETHODCALLTYPE GetClassID(CLSID* pClassID) override
	{
		debug("IPersist GetClassID");
		*pClassID = {};
		return E_UNEXPECTED;
	}
	
	HRESULT STDMETHODCALLTYPE GetState(DWORD dwMilliSecsTimeout, FILTER_STATE* State) override
	{
		debug("IMediaFilter GetState %lx", state);
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
		graph = pGraph;
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE QueryFilterInfo(FILTER_INFO* pInfo) override
	{
		debug("IBaseFilter QueryFilterInfo");
		wcscpy(pInfo->achName, L"my object");
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
		char buf[1024];
		
		va_list args;
		va_start(args, fmt);
		vsnprintf(buf, 1024, fmt, args);
		va_end(args);
		
		fprintf(stdout, "%lu %s %s\n", GetCurrentThreadId(), typeid(Touter).name(), buf);
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
		debug("IPin Connect %p", pmt);
		if constexpr (is_output)
		{
			//debug("IPin Connect %s %s %s", guid_to_str(pmt->majortype), guid_to_str(pmt->subtype), guid_to_str(pmt->formattype));
			if (parent()->connect_output(pReceivePin))
			{
				puts("OUTPUT PIN CONNECTED");
				peer = pReceivePin;
				return S_OK;
			}
			return VFW_E_NO_ACCEPTABLE_TYPES;
		}
		else return E_UNEXPECTED;
	}
	HRESULT STDMETHODCALLTYPE ConnectedTo(IPin** pPin) override
	{
		debug("IPin ConnectedTo");
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
		//if constexpr (!is_output)
		//{
			//if (parent()->acceptable_input(nullptr, pmt))
				//return S_OK;
			//return VFW_E_TYPE_NOT_ACCEPTED;
		//}
		return E_UNEXPECTED;
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
			if (parent()->acceptable_input(pConnector, pmt))
			{
				puts("INPUT PIN CONNECTED");
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
REFERENCE_TIME time1;
REFERENCE_TIME time2;
pSample->GetTime(&time1, &time2);
		debug("IMemInputPin Receive %u at %u %u", (unsigned)size, (unsigned)time1, (unsigned)time2);
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
	
	// IAsyncReader
	// these two exist on IPin already
	//HRESULT WINAPI BeginFlush();
	//HRESULT WINAPI EndFlush();
	HRESULT WINAPI Length(int64_t* pTotal, int64_t* pAvailable)
	{
		debug("IAsyncReader Length");
		return E_OUTOFMEMORY;
	}
	HRESULT WINAPI Request(IMediaSample* pSample, uintptr_t dwUser)
	{
		debug("IAsyncReader Request");
		return E_OUTOFMEMORY;
	}
	HRESULT WINAPI RequestAllocator(IMemAllocator* pPreferred, ALLOCATOR_PROPERTIES* pProps, IMemAllocator** ppActual)
	{
		debug("IAsyncReader RequestAllocator");
		return E_OUTOFMEMORY;
	}
	HRESULT WINAPI SyncRead(int64_t llPosition, LONG lLength, uint8_t* pBuffer)
	{
		debug("IAsyncReader SyncRead %u %u", (unsigned)llPosition, (unsigned)lLength);
		return E_OUTOFMEMORY;
	}
	HRESULT WINAPI SyncReadAligned(IMediaSample* pSample)
	{
		debug("IAsyncReader SyncReadAligned");
		return E_OUTOFMEMORY;
	}
	HRESULT WINAPI WaitForNext(DWORD dwTimeout, IMediaSample** ppSample, uintptr_t* pdwUser)
	{
		debug("IAsyncReader WaitForNext");
		return E_OUTOFMEMORY;
	}
};

class customFilterSink : public base_filter<customFilterSink> {
public:
	CComPtr<IDirectDraw> dd;
	CComPtr<IDirectDrawSurface> dds;
	
	customFilterSink()
	{
		DirectDrawCreate(nullptr, &dd, nullptr);
		dd->SetCooperativeLevel(GetDesktopWindow(), DDSCL_NORMAL);
		DDSURFACEDESC ddsd;
		ddsd.dwSize = sizeof(ddsd);
		ddsd.dwFlags = DDSD_CAPS;// | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
		ddsd.dwWidth = 640;
		ddsd.dwHeight = 540;
		ddsd.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;
		ddsd.ddpfPixelFormat.dwSize = sizeof(ddsd.ddpfPixelFormat);
		ddsd.ddpfPixelFormat.dwFlags = DDPF_LUMINANCE;
		ddsd.ddpfPixelFormat.dwLuminanceBitCount = 8;
		ddsd.ddpfPixelFormat.dwLuminanceBitMask = 0xFF;
		dd->CreateSurface(&ddsd, &dds, NULL);
	}
	
	class in_pin : public base_pin<false, com_base_embedded<IPin, IMemInputPin>, in_pin> {
	public:
		customFilterSink* parent() { return container_of<&customFilterSink::pin>(this); }
		
		bool acceptable_input(IPin* pConnector, const AM_MEDIA_TYPE * pmt)
		{
			if (pmt->majortype == MEDIATYPE_Video && pmt->formattype == FORMAT_VideoInfo)
				return (pmt->subtype == MEDIASUBTYPE_RGB24 || pmt->subtype == MEDIASUBTYPE_YV12);
			return false;
		}
		
		void receive_input(uint8_t* ptr, size_t size)
		{
			IDirectDrawSurface* dds = parent()->dds;
			RECT rect = { 0, 0, 640, 540 };
			DDSURFACEDESC surf = { sizeof(DDSURFACEDESC) };
			dds->Lock(&rect, &surf, 0, nullptr);
			for (int y=0;y<540;y++)
			{
				for (int x=0;x<640;x++)
				{
					uint8_t* px = (uint8_t*)surf.lpSurface + surf.lPitch*y + 4*x;
					px[0] = ptr[640*y+x];
					px[1] = ptr[640*y+x];
					px[2] = ptr[640*y+x];
					px[3] = 0;
				}
			}
			dds->Unlock(&rect);
			dds->Flip(nullptr, DDFLIP_WAIT);
			//static bool first = true;
			//if (first)
			//{
				//FILE* f = fopen("a.bin", "wb");
				//fwrite(ptr, size,1, f);
				//fclose(f);
				//first = false;
			//}
		}
		void end_of_stream()
		{
			//exit(0);
		}
	};
	in_pin pin;
	
	IPin* pins[1] = { &pin };
};

class customFilterSource : public base_filter<customFilterSource> {
public:
	class out_pin : public base_pin<true, com_base_embedded<IPin, IAsyncReader>, out_pin> {
	public:
		customFilterSource* parent() { return container_of<&customFilterSource::pin>(this); }
		
		bool connect_output(IPin* pReceivePin)
		{
			/*
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
			*/
			AM_MEDIA_TYPE mt = {
				.majortype = MEDIATYPE_Stream,
				.subtype = MEDIASUBTYPE_MPEG1System,
				.bFixedSizeSamples = false,
				.bTemporalCompression = true,
				.lSampleSize = 0,
				.formattype = GUID_NULL,
				.pUnk = nullptr,
				.cbFormat = 0,
				.pbFormat = nullptr,
			};
			return SUCCEEDED(pReceivePin->ReceiveConnection(this, &mt));
		}
		
		HRESULT WINAPI Length(int64_t* pTotal, int64_t* pAvailable)
		{
			debug("IAsyncReader Length");
			*pTotal = parent()->len;
			*pAvailable = parent()->len;
			return S_OK;
		}
		HRESULT WINAPI SyncRead(int64_t llPosition, LONG lLength, uint8_t* pBuffer)
		{
			debug("IAsyncReader SyncRead %u %u", (unsigned)llPosition, (unsigned)lLength);
			memcpy(pBuffer, parent()->ptr+llPosition, lLength);
			return S_OK;
		}
	};
	out_pin pin;
	
	IPin* pins[1] = { &pin };
	
	uint8_t* ptr;
	size_t len;
};

static HRESULT connect_filters(IGraphBuilder* graph, IBaseFilter* src, IBaseFilter* dst)
{
puts("TRYCONNECTFILT");
	CComPtr<IEnumPins> src_enum;
	if (FAILED(src->EnumPins(&src_enum)))
		return E_FAIL;
	
	CComPtr<IPin> src_pin;
	while (src_enum->Next(1, &src_pin, nullptr) == S_OK)
	{
		PIN_INFO src_info;
		if (FAILED(src_pin->QueryPinInfo(&src_info)))
			return E_FAIL;
		if (src_info.pFilter)
			src_info.pFilter->Release();
printf("TRYCONNECTSRC ");
for (int i=0;src_info.achName[i];i++)putchar(src_info.achName[i]);
puts("");
		if (src_info.dir != PINDIR_OUTPUT)
			continue;
		
		CComPtr<IPin> check_pin;
		src_pin->ConnectedTo(&check_pin);
		if (check_pin != nullptr)
			continue;
		
		CComPtr<IEnumPins> dst_enum;
		dst->EnumPins(&dst_enum);
		CComPtr<IPin> dst_pin;
		while (dst_enum->Next(1, &dst_pin, nullptr) == S_OK)
		{
			PIN_INFO dst_info;
			if (FAILED(dst_pin->QueryPinInfo(&dst_info)))
				return E_FAIL;
printf("TRYCONNECTDST ");
for (int i=0;src_info.achName[i];i++)putchar(src_info.achName[i]);
puts("");
			if (dst_info.pFilter)
				dst_info.pFilter->Release();
			if (dst_info.dir != PINDIR_INPUT)
				continue;
			
			dst_pin->ConnectedTo(&check_pin);
			if (check_pin != nullptr)
				continue;
			
puts("TRYCONNECTPAIR");
			if (SUCCEEDED(graph->ConnectDirect(src_pin, dst_pin, nullptr)))
				return S_OK;
		}
	}
	return E_FAIL;
}

static void require(int seq, HRESULT hr, const char * text = "")
{
	if (SUCCEEDED(hr))
		return;
	
	printf("fail: %d %.8X%s%s\n", seq, hr, text?" ":"", text);
	exit(seq);
}

int main()
{
	setvbuf(stdout, nullptr, _IONBF, 0);
	
	if (FAILED(CoInitializeEx(NULL, COINIT_MULTITHREADED | COINIT_DISABLE_OLE1DDE)))
		exit(1);
	
	CComPtr<IGraphBuilder> filterGraph;
	//CComPtr<IBaseFilter> asyncReader;
	//CComPtr<IFileSourceFilter> asyncReaderFsf;
	CComPtr<IBaseFilter> mpegSplitter;
	CComPtr<IBaseFilter> mpegVideoCodec;
	CComPtr<IBaseFilter> mpegAudioCodec;
	CComPtr<IBaseFilter> decodebin;
	CComPtr<IBaseFilter> dsound;
	CComPtr<IBaseFilter> vmr9;
	customFilterSource* source = new customFilterSource();
	customFilterSink* sink = new customFilterSink();
	
	require(1, filterGraph.CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER));
	//require(3, asyncReader.CoCreateInstance(CLSID_AsyncReader, nullptr, CLSCTX_INPROC_SERVER));
	require(4, mpegSplitter.CoCreateInstance(CLSID_MPEG1Splitter, nullptr, CLSCTX_INPROC_SERVER));
	require(5, mpegVideoCodec.CoCreateInstance(CLSID_CMpegVideoCodec, nullptr, CLSCTX_INPROC_SERVER));
	require(6, mpegAudioCodec.CoCreateInstance(CLSID_CMpegAudioCodec, nullptr, CLSCTX_INPROC_SERVER));
	require(7, decodebin.CoCreateInstance(CLSID_decodebin_parser, nullptr, CLSCTX_INPROC_SERVER));
	require(8, dsound.CoCreateInstance(CLSID_DSoundRender, nullptr, CLSCTX_INPROC_SERVER));
	require(9, vmr9.CoCreateInstance(CLSID_VideoMixingRenderer9, nullptr, CLSCTX_INPROC_SERVER));
	
	require(11, filterGraph->AddFilter(source, L"source"));
	require(12, filterGraph->AddFilter(sink, L"sink"));
	//require(13, filterGraph->AddFilter(asyncReader, L"asyncReader"));
	require(14, filterGraph->AddFilter(mpegSplitter, L"mpegSplitter"));
	require(15, filterGraph->AddFilter(mpegVideoCodec, L"mpegVideoCodec"));
	require(16, filterGraph->AddFilter(mpegAudioCodec, L"mpegAudioCodec"));
	require(17, filterGraph->AddFilter(decodebin, L"decodebin"));
	require(18, filterGraph->AddFilter(dsound, L"dsound"));
	require(19, filterGraph->AddFilter(vmr9, L"vmr9"));
	
	//require(20, asyncReader.QueryInterface(&asyncReaderFsf));
	//require(21, asyncReaderFsf->Load(L"video.mpg", nullptr), "failed to open data_video_sample_640x360.mpeg, does the file exist?");
	
	FILE* f = fopen("video.mpg", "rb");
	fseek(f, 0, SEEK_END);
	size_t f_len = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t* f_ptr = (uint8_t*)malloc(f_len);
	fread(f_ptr, 1,f_len, f);
	fclose(f);
	source->ptr = f_ptr;
	source->len = f_len;
	
	//require(31, connect_filters(filterGraph, asyncReader, mpegSplitter));
	
	//require(31, connect_filters(filterGraph, source, mpegSplitter));
	//require(32, connect_filters(filterGraph, mpegSplitter, mpegVideoCodec));
	//require(33, connect_filters(filterGraph, mpegSplitter, mpegAudioCodec));
	//require(34, connect_filters(filterGraph, mpegVideoCodec, sink));
	//require(35, connect_filters(filterGraph, mpegAudioCodec, dsound));
	
	//require(31, connect_filters(filterGraph, source, decodebin));
	//require(32, connect_filters(filterGraph, decodebin, sink));
	//require(33, connect_filters(filterGraph, decodebin, dsound));
	
	require(31, connect_filters(filterGraph, source, mpegSplitter));
	require(32, connect_filters(filterGraph, mpegSplitter, mpegVideoCodec));
	require(34, connect_filters(filterGraph, mpegVideoCodec, vmr9));
	require(33, connect_filters(filterGraph, mpegSplitter, mpegAudioCodec));
	require(35, connect_filters(filterGraph, mpegAudioCodec, dsound));
	
	//require(31, connect_filters(filterGraph, source, decodebin));
	//require(32, connect_filters(filterGraph, decodebin, vmr9));
	//require(33, connect_filters(filterGraph, decodebin, dsound));
	
	puts("connected");
	
	//puts("CON1");
	//require(31, connect_filters(filterGraph, source, decodebin));
	//puts("CON2");
	//require(33, connect_filters(filterGraph, decodebin, sink));
	//puts("CON3");
	//require(34, connect_filters(filterGraph, decodebin, dsound));
	//puts("CON4");
	
	CComPtr<IMediaControl> filterGraph_mc;
	require(40, filterGraph.QueryInterface(&filterGraph_mc));
	filterGraph_mc->Run();
	puts("running");
	FILTER_STATE st;
	printf("WALRUS=%x\n", vmr9->GetState(0, &st));
	printf("WALRUS2=%x\n", st);
	SleepEx(8000, true);
	
	puts("exit");
}
