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

#define DEBUG 1

#define INITGUID
#define STRSAFE_NO_DEPRECATE
#include <typeinfo>
#include <windows.h>
#include <dshow.h>
#include <mmreg.h>
#include <wmsdk.h>
#include <wmcodecdsp.h>
#include <dmo.h>
#include <dmodshow.h>
#include <nserror.h>
#include <stdint.h>

#if DEBUG >= 1
// can't use stdout from wine/proton since 8.12 https://gitlab.winehq.org/wine/wine/-/commit/dcf0bf1f383f8429136cb761f5170a04503a297b
static void my_puts(const char * a, bool linebreak=true)
{
	static int __cdecl (*__wine_dbg_output)( const char *str );
	if (!__wine_dbg_output)
		__wine_dbg_output = (decltype(__wine_dbg_output))GetProcAddress(GetModuleHandle("ntdll.dll"), "__wine_dbg_output");
	if (__wine_dbg_output)
	{
		__wine_dbg_output(a);
		if (linebreak)
			__wine_dbg_output("\n");
	}
	else
	{
		// probably shows up on windows. Maybe. Who cares.
		fputs(a, stdout);
		if (linebreak)
			fputs("\n", stdout);
		fflush(stdout);
	}
}
#define puts my_puts
#define printf(...) do { char my_buf[4096]; sprintf(my_buf, __VA_ARGS__); my_puts(my_buf, false); } while(0)
#else
#define puts(x) do{}while(0)
#define printf(...) do{}while(0)
#endif

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
	ULONG STDMETHODCALLTYPE AddRef() override
	{
		return ++refcount;
	}
	ULONG STDMETHODCALLTYPE Release() override
	{
		uint32_t new_refcount = --refcount;
		if (!new_refcount)
			delete this;
		return new_refcount;
	}
	com_base() {}
	virtual ~com_base() {}
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

template<typename T, const GUID& clsid>
class ClassFactory : public com_base<IClassFactory> {
public:
	HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* pUnkOuter, REFIID riid, void** ppvObject) override
	{
//printf("krkrwine: create %s (iface %s)\n", guid_to_str(clsid), guid_to_str(riid));
		if (pUnkOuter != nullptr)
			return CLASS_E_NOAGGREGATION;
		return qi_release(new T(), riid, ppvObject);
	}
	HRESULT STDMETHODCALLTYPE LockServer(BOOL lock) override { return S_OK; } // don't care
};

template<typename T>
class WrappingClassFactory : public com_base<IClassFactory> {
public:
	CComPtr<IClassFactory> parent;
	WrappingClassFactory(IUnknown* parent)
	{
		parent->QueryInterface(&this->parent);
	}
	HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* pUnkOuter, REFIID riid, void** ppvObject) override
	{
//printf("krkrwine: create %s (iface %s)\n", guid_to_str(clsid), guid_to_str(riid));
		if (pUnkOuter != nullptr)
			return CLASS_E_NOAGGREGATION;
		
		IUnknown* inner;
		HRESULT hr = parent->CreateInstance(pUnkOuter, __uuidof(IUnknown), (void**)&inner);
		if (FAILED(hr))
			return hr;
		IUnknown* ret = new T(inner);
		inner->Release();
		hr = ret->QueryInterface(riid, ppvObject);
		ret->Release();
		return hr;
	}
	HRESULT STDMETHODCALLTYPE LockServer(BOOL lock) override { return S_OK; } // don't care
};

template<typename T, const GUID& clsid>
class AggregatingClassFactory : public com_base<IClassFactory> {
public:
	HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* pUnkOuter, REFIID riid, void** ppvObject) override
	{
//printf("krkrwine: create %s (iface %s)\n", guid_to_str(clsid), guid_to_str(riid));
		if (pUnkOuter == nullptr)
			return VFW_E_NEED_OWNER; // wrong if this isn't a VFW object, but who cares, it's an error, good enough
		if (riid != __uuidof(IUnknown))
			return E_NOINTERFACE;
		T* ret = new T(pUnkOuter);
		*ppvObject = (void*)(IUnknown*)&ret->own_iunknown;
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE LockServer(BOOL lock) override { return S_OK; } // don't care
};

class wrap_DMOObject final : public IMediaObject {
public:
	class own_iunknown_t : public IUnknown {
	public:
		ULONG refcount = 1;
		wrap_DMOObject* parent() { return container_of<&wrap_DMOObject::own_iunknown>(this); }
		
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
	
	CComPtr<IUnknown> the_real_one_iunk;
	CComPtr<IMediaObject> the_real_one;
	REFERENCE_TIME AvgTimePerFrame;
	
	wrap_DMOObject(IUnknown* outer) : parent(outer)
	{
		auto* pDllGetClassObject = (decltype(DllGetClassObject)*)GetProcAddress(LoadLibrary("winegstreamer.dll"), "DllGetClassObject");
		CComPtr<IClassFactory> fac;
		pDllGetClassObject(CLSID_CWMVDecMediaObject, __uuidof(IClassFactory), (void**)&fac);
		fac->CreateInstance(outer, IID_PPV_ARGS(&the_real_one_iunk));
		the_real_one_iunk.QueryInterface(&the_real_one);
	}
	
	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override
		{ return parent->QueryInterface(riid, ppvObject); }
	ULONG STDMETHODCALLTYPE AddRef() override
		{ return parent->AddRef(); }
	ULONG STDMETHODCALLTYPE Release() override
		{ return parent->Release(); }
	
	HRESULT NonDelegatingQueryInterface(REFIID riid, void** ppvObject)
	{
		if (riid == __uuidof(IMediaObject))
		{
			parent->AddRef();
			*ppvObject = (void*)(IMediaObject*)this;
			return S_OK;
		}
		return E_NOINTERFACE;
	}
	
	HRESULT STDMETHODCALLTYPE AllocateStreamingResources() override
		{ return the_real_one->AllocateStreamingResources(); }
	HRESULT STDMETHODCALLTYPE Discontinuity(DWORD dwInputStreamIndex) override
		{ return the_real_one->Discontinuity(dwInputStreamIndex); }
	HRESULT STDMETHODCALLTYPE Flush() override
		{ return the_real_one->Flush(); }
	HRESULT STDMETHODCALLTYPE FreeStreamingResources() override
		{ return the_real_one->FreeStreamingResources(); }
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
		{ return the_real_one->GetOutputSizeInfo(dwOutputStreamIndex, pcbSize, pcbAlignment); }
	HRESULT STDMETHODCALLTYPE GetOutputStreamInfo(DWORD dwOutputStreamIndex, DWORD* pdwFlags) override
		{ return E_OUTOFMEMORY; }
	HRESULT STDMETHODCALLTYPE GetOutputType(DWORD dwOutputStreamIndex, DWORD dwTypeIndex, DMO_MEDIA_TYPE* pmt) override
	{
		HRESULT hr = the_real_one->GetOutputType(dwOutputStreamIndex, dwTypeIndex, pmt);
		if (SUCCEEDED(hr) && IsEqualGUID(pmt->formattype, FORMAT_VideoInfo))
		{
			VIDEOINFOHEADER* vih = (VIDEOINFOHEADER*)pmt->pbFormat;
			vih->AvgTimePerFrame = AvgTimePerFrame;
		}
		return hr;
	}
	HRESULT STDMETHODCALLTYPE GetStreamCount(DWORD* pcInputStreams, DWORD* pcOutputStreams) override
	{ return the_real_one->GetStreamCount(pcInputStreams, pcOutputStreams); }
	HRESULT STDMETHODCALLTYPE Lock(LONG bLock) override
		{ return E_OUTOFMEMORY; }
	HRESULT STDMETHODCALLTYPE ProcessInput(DWORD dwInputStreamIndex, IMediaBuffer* pBuffer, DWORD dwFlags,
	                                       REFERENCE_TIME rtTimestamp, REFERENCE_TIME rtTimelength) override
		{ return the_real_one->ProcessInput(dwInputStreamIndex, pBuffer, dwFlags, rtTimestamp, rtTimelength); }
	HRESULT STDMETHODCALLTYPE ProcessOutput(DWORD dwFlags, DWORD cOutputBufferCount,
	                                        DMO_OUTPUT_DATA_BUFFER* pOutputBuffers, DWORD* pdwStatus) override
	{ return the_real_one->ProcessOutput(dwFlags, cOutputBufferCount, pOutputBuffers, pdwStatus); }
	HRESULT STDMETHODCALLTYPE SetInputMaxLatency(DWORD dwInputStreamIndex, REFERENCE_TIME rtMaxLatency) override
		{ return E_OUTOFMEMORY; }
	HRESULT STDMETHODCALLTYPE SetInputType(DWORD dwInputStreamIndex, const DMO_MEDIA_TYPE* pmt, DWORD dwFlags) override
		{ return the_real_one->SetInputType(dwInputStreamIndex, pmt, dwFlags); }
	HRESULT STDMETHODCALLTYPE SetOutputType(DWORD dwOutputStreamIndex, const DMO_MEDIA_TYPE* pmt, DWORD dwFlags) override
	{
		HRESULT hr = the_real_one->SetOutputType(dwOutputStreamIndex, pmt, dwFlags);
		if (dwFlags == 0 && hr == S_OK)
		{
			VIDEOINFOHEADER* vih = (VIDEOINFOHEADER*)pmt->pbFormat;
			AvgTimePerFrame = vih->AvgTimePerFrame;
		}
		return hr;
	}
};

static void dump_graph(IGraphBuilder* graph)
{
	puts("dumping graph");
	char buf[2048];
	char* buf_out = buf;
	*buf_out = '\0';
	
	CComPtr<IEnumFilters> flts;
	graph->EnumFilters(&flts);
	
	CComPtr<IBaseFilter> flt;
	while (flts->Next(1, &flt, nullptr) == S_OK)
	{
		buf_out += sprintf(buf_out, "filter %p, vtbl=%p\n", (IBaseFilter*)flt, *(void**)(IBaseFilter*)flt);
		
		FILTER_INFO inf;
		flt->QueryFilterInfo(&inf);
		buf_out += sprintf(buf_out, " name=%ls\n", inf.achName);
		inf.pGraph->Release();
		
		CComPtr<IEnumPins> pins;
		flt->EnumPins(&pins);
		
		CComPtr<IPin> pin;
		while (pins->Next(1, &pin, nullptr) == S_OK)
		{
			buf_out += sprintf(buf_out, " pin %p, vtbl=%p\n", (IPin*)pin, *(void**)(IPin*)pin);
			
			PIN_INFO inf;
			pin->QueryPinInfo(&inf);
			buf_out += sprintf(buf_out, "  name=%ls, dir=%s\n", inf.achName, inf.dir==PINDIR_INPUT ? "sink" : "src");
			inf.pFilter->Release();
			
			CComPtr<IPin> pin2;
			pin->ConnectedTo(&pin2);
			buf_out += sprintf(buf_out, "  connected to %p\n", (IPin*)pin2);
		}
	}
	
	printf("graph:\n%s\nend of dump\n", buf);
}

class funny_BaseFilter final : public IBaseFilter {
	ULONG refcount = 1;
public:
	ULONG STDMETHODCALLTYPE AddRef() override
	{
		return ++refcount;
	}
	ULONG STDMETHODCALLTYPE Release() override
	{
		ULONG new_refcount = --refcount;
		if (!new_refcount)
			delete this;
		return new_refcount;
	}
	
	CComPtr<IBaseFilter> the_real_one;
	CComPtr<IPin> the_real_ones_pin;
	CComPtr<IMemInputPin> the_real_ones_meminput_pin;
	CComPtr<IPin> prev_pin;
	
	funny_BaseFilter(IBaseFilter* real)
	{
		real->QueryInterface(&the_real_one);
		CComPtr<IEnumPins> p_e;
		the_real_one->EnumPins(&p_e);
		p_e->Next(1, &the_real_ones_pin, NULL);
		the_real_ones_pin->QueryInterface(&the_real_ones_meminput_pin);
	}
	
	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override
	{
		*ppvObject = NULL;
		if (riid == __uuidof(IBaseFilter))
			*(IBaseFilter**)ppvObject = this;
		else if (riid == __uuidof(IUnknown))
			*(IUnknown**)ppvObject = this;
		else
			return E_NOINTERFACE;
		this->AddRef();
		return S_OK;
	}
	
	template<bool out>
	class my_Pin final : public IPin, public IMemInputPin {
		funny_BaseFilter* parent()
		{
			if constexpr (out)
				return container_of<&funny_BaseFilter::pin_out>(this);
			else
				return container_of<&funny_BaseFilter::pin_in>(this);
		}
	public:
		ULONG STDMETHODCALLTYPE AddRef() override
		{
			return parent()->AddRef();
		}
		ULONG STDMETHODCALLTYPE Release() override
		{
			return parent()->Release();
		}
		
		HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override
		{
			if (riid == __uuidof(IPin))
				*(IPin**)ppvObject = this;
			if (riid == __uuidof(IMemInputPin))
				*(IMemInputPin**)ppvObject = this;
			else if (riid == __uuidof(IUnknown))
				*(IUnknown**)ppvObject = (IPin*)this;
			else
				return E_NOINTERFACE;
			this->AddRef();
			return S_OK;
		}
		
		HRESULT STDMETHODCALLTYPE Connect(IPin* pReceivePin, const AM_MEDIA_TYPE* pmt) override
			{ return S_OK; } // out pin is already connected
		HRESULT STDMETHODCALLTYPE ReceiveConnection(IPin* pConnector, const AM_MEDIA_TYPE* pmt) override
		{
			if (out)
				return E_UNEXPECTED;
			if (IsEqualGUID(pmt->formattype, FORMAT_VideoInfo)) // it's enough to call ReceiveConnection once, but too many doesn't hurt
			{
				MPEG1VIDEOINFO fake_vi = { *(VIDEOINFOHEADER*)pmt->pbFormat };
				AM_MEDIA_TYPE fake_mt = *pmt;
				fake_mt.formattype = FORMAT_MPEGVideo;
				fake_mt.cbFormat = sizeof(MPEG1VIDEOINFO);
				fake_mt.pbFormat = (uint8_t*)&fake_vi;
				HRESULT hr = parent()->the_real_ones_pin->ReceiveConnection(&parent()->pin_out, &fake_mt);
				if (SUCCEEDED(hr)) // won't happen, but...
				{
					pConnector->QueryInterface(&parent()->prev_pin);
					return hr;
				}
			}
			HRESULT hr = parent()->the_real_ones_pin->ReceiveConnection(&parent()->pin_out, pmt);
			if (SUCCEEDED(hr))
				pConnector->QueryInterface(&parent()->prev_pin);
			return hr;
		}
		HRESULT STDMETHODCALLTYPE Disconnect() override
			{ return parent()->the_real_ones_pin->Disconnect(); }
		HRESULT STDMETHODCALLTYPE ConnectedTo(IPin** pPin) override
		{
			IPin* ret = out ? parent()->the_real_ones_pin : parent()->prev_pin;
			if (!ret)
				return VFW_E_NOT_CONNECTED;
			ret->AddRef();
			*pPin = ret;
			return S_OK;
		}
		HRESULT STDMETHODCALLTYPE ConnectionMediaType(AM_MEDIA_TYPE* pmt) override
			{ return E_OUTOFMEMORY; }
		HRESULT STDMETHODCALLTYPE QueryPinInfo(PIN_INFO* pInfo) override
		{
			pInfo->pFilter = parent();
			parent()->AddRef();
			this->QueryDirection(&pInfo->dir);
			wcscpy(pInfo->achName, out ? L"OUT" : L"IN");
			return S_OK;
		}
		HRESULT STDMETHODCALLTYPE QueryDirection(PIN_DIRECTION* pPinDir) override
			{ *pPinDir = out ? PINDIR_OUTPUT : PINDIR_INPUT; return S_OK; }
		HRESULT STDMETHODCALLTYPE QueryId(LPWSTR* Id) override
			{ return E_OUTOFMEMORY; }
		HRESULT STDMETHODCALLTYPE QueryAccept(const AM_MEDIA_TYPE* pmt) override
			{ return parent()->the_real_ones_pin->QueryAccept(pmt); }
		HRESULT STDMETHODCALLTYPE EnumMediaTypes(IEnumMediaTypes** ppEnum) override
			{ return parent()->the_real_ones_pin->EnumMediaTypes(ppEnum); }
		HRESULT STDMETHODCALLTYPE QueryInternalConnections(IPin** apPin, ULONG* nPin) override
			{ return E_OUTOFMEMORY; }
		HRESULT STDMETHODCALLTYPE EndOfStream() override
			{ return parent()->the_real_ones_pin->EndOfStream(); }
		HRESULT STDMETHODCALLTYPE BeginFlush() override
			{ return S_OK; }
		HRESULT STDMETHODCALLTYPE EndFlush() override
			{ return S_OK; }
		HRESULT STDMETHODCALLTYPE NewSegment(REFERENCE_TIME tStart, REFERENCE_TIME tStop, double dRate) override
			{ return S_OK; }
		
		// these will do something crazy if called on the out pin, but nobody will try that so who cares
		HRESULT STDMETHODCALLTYPE GetAllocator(IMemAllocator** ppAllocator) override
			{ return parent()->the_real_ones_meminput_pin->GetAllocator(ppAllocator); }
		HRESULT STDMETHODCALLTYPE NotifyAllocator(IMemAllocator* pAllocator, BOOL bReadOnly) override
			{ return parent()->the_real_ones_meminput_pin->NotifyAllocator(pAllocator, bReadOnly); }
		HRESULT STDMETHODCALLTYPE GetAllocatorRequirements(ALLOCATOR_PROPERTIES* pProps) override
			{ return parent()->the_real_ones_meminput_pin->GetAllocatorRequirements(pProps); }
		HRESULT STDMETHODCALLTYPE Receive(IMediaSample* pSample) override
			{ return parent()->the_real_ones_meminput_pin->Receive(pSample); }
		HRESULT STDMETHODCALLTYPE ReceiveMultiple(IMediaSample** pSamples, LONG nSamples, LONG* nSamplesProcessed) override
			{ return parent()->the_real_ones_meminput_pin->ReceiveMultiple(pSamples, nSamples, nSamplesProcessed); }
		HRESULT STDMETHODCALLTYPE ReceiveCanBlock() override
			{ return parent()->the_real_ones_meminput_pin->ReceiveCanBlock(); }
	};
	
	my_Pin<false> pin_in;
	my_Pin<true> pin_out;
	
	class my_EnumPins final : public IEnumPins {
		ULONG refcount = 1;
	public:
		ULONG STDMETHODCALLTYPE AddRef() override
		{
			return ++refcount;
		}
		ULONG STDMETHODCALLTYPE Release() override
		{
			ULONG new_refcount = --refcount;
			if (!new_refcount)
				delete this;
			return new_refcount;
		}
		
		CComPtr<funny_BaseFilter> parent;
		size_t pos = 0;
		
		my_EnumPins(funny_BaseFilter* parent)
		{
			parent->AddRef();
			this->parent.p = parent;
		}
		
		HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override
		{
			if (riid == __uuidof(IEnumPins))
				*(IEnumPins**)ppvObject = this;
			else if (riid == __uuidof(IUnknown))
				*(IUnknown**)ppvObject = this;
			else
				return E_NOINTERFACE;
			this->AddRef();
			return S_OK;
		}
		
		HRESULT STDMETHODCALLTYPE Next(ULONG cPins, IPin** ppPins, ULONG* pcFetched) override
		{
			if (cPins != 1)
				return E_UNEXPECTED;
			IPin* ret;
			if (pos == 0)
				ret = &parent->pin_in;
			else if (pos == 1)
				ret = &parent->pin_out;
			else
				return S_FALSE;
			*ppPins = ret;
			ret->AddRef();
			pos++;
			if (pcFetched) *pcFetched = 1;
			return S_OK;
		}
		HRESULT STDMETHODCALLTYPE Skip(ULONG cPins) override
			{ return E_OUTOFMEMORY; }
		HRESULT STDMETHODCALLTYPE Reset() override
			{ return E_OUTOFMEMORY; }
		HRESULT STDMETHODCALLTYPE Clone(IEnumPins** ppEnum) override
			{ return E_OUTOFMEMORY; }
	};
	
	
	HRESULT STDMETHODCALLTYPE GetClassID(CLSID* pClassID) override
		{ return the_real_one->GetClassID(pClassID); }
	HRESULT STDMETHODCALLTYPE Stop() override
		{ return S_OK; }
	HRESULT STDMETHODCALLTYPE Pause() override
		{ return S_OK; }
	HRESULT STDMETHODCALLTYPE Run(REFERENCE_TIME tStart) override
		{ return S_OK; }
	HRESULT STDMETHODCALLTYPE GetState(DWORD dwMilliSecsTimeout, FILTER_STATE* State) override
		{ return the_real_one->GetState(dwMilliSecsTimeout, State); }
	HRESULT STDMETHODCALLTYPE SetSyncSource(IReferenceClock* pClock) override
		{ return S_OK; }
	HRESULT STDMETHODCALLTYPE GetSyncSource(IReferenceClock** pClock) override
		{ return the_real_one->GetSyncSource(pClock); }
	HRESULT STDMETHODCALLTYPE EnumPins(IEnumPins** ppEnum) override
	{
		*ppEnum = new my_EnumPins(this);
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE FindPin(LPCWSTR Id, IPin** ppPin) override
		{ return E_OUTOFMEMORY; }
	HRESULT STDMETHODCALLTYPE QueryFilterInfo(FILTER_INFO* pInfo) override
	{
		the_real_one->QueryFilterInfo(pInfo);
		wcscpy(pInfo->achName, L"FAKE_TEXTURERENDERER_BURIKO"); // this line is just a debug aid
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE JoinFilterGraph(IFilterGraph* pGraph, LPCWSTR pName) override
		{ return S_OK; }
	HRESULT STDMETHODCALLTYPE QueryVendorInfo(LPWSTR* pVendorInfo) override
		{ return the_real_one->QueryVendorInfo(pVendorInfo); }
};

class wrap_FilterGraph final : public IGraphBuilder {
	ULONG refcount = 1;
public:
	ULONG STDMETHODCALLTYPE AddRef() override
	{
		return ++refcount;
	}
	ULONG STDMETHODCALLTYPE Release() override
	{
		ULONG new_refcount = --refcount;
		if (!new_refcount)
			delete this;
		return new_refcount;
	}
	
	CComPtr<IGraphBuilder> the_real_one;
	
	wrap_FilterGraph(IUnknown* outer)
	{
		outer->QueryInterface(&the_real_one);
	}
	
	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override
	{
		if (riid == __uuidof(IGraphBuilder))
			*(IGraphBuilder**)ppvObject = this;
		else if (riid == __uuidof(IUnknown))
			*(IUnknown**)ppvObject = this;
		else
			return the_real_one->QueryInterface(riid, ppvObject);
		this->AddRef();
		return S_OK;
	}
	
	HRESULT STDMETHODCALLTYPE AddFilter(IBaseFilter* pFilter, LPCWSTR pName) override
	{
		HRESULT hr = the_real_one->AddFilter(pFilter, pName);
		// must install myself AFTER the real one, Wine's FilterGraph2_Render() tries the last-added one first
		if (pName && !wcscmp(pName, L"TEXTURERENDERER_BURIKO"))
		{
			puts("krkrwine: interjecting in front of TEXTURERENDERER_BURIKO");
			the_real_one->AddFilter(new funny_BaseFilter(pFilter), L"FAKE_TEXTURERENDERER_BURIKO");
		}
		return hr;
	}
	HRESULT STDMETHODCALLTYPE RemoveFilter(IBaseFilter* pFilter) override
		{ return the_real_one->RemoveFilter(pFilter); }
	HRESULT STDMETHODCALLTYPE EnumFilters(IEnumFilters** ppEnum) override
		{ return the_real_one->EnumFilters(ppEnum); }
	HRESULT STDMETHODCALLTYPE FindFilterByName(LPCWSTR pName, IBaseFilter** ppFilter) override
		{ return the_real_one->FindFilterByName(pName, ppFilter); }
	HRESULT STDMETHODCALLTYPE ConnectDirect(IPin* ppinOut, IPin* ppinIn, const AM_MEDIA_TYPE* pmt) override
		{ return the_real_one->ConnectDirect(ppinOut, ppinIn, pmt); }
	HRESULT STDMETHODCALLTYPE Reconnect(IPin* ppin) override
		{ return the_real_one->Reconnect(ppin); }
	HRESULT STDMETHODCALLTYPE Disconnect(IPin* ppin) override
		{ return the_real_one->Disconnect(ppin); }
	HRESULT STDMETHODCALLTYPE SetDefaultSyncSource() override
		{ return the_real_one->SetDefaultSyncSource(); }
	HRESULT STDMETHODCALLTYPE Connect(IPin* ppinOut, IPin* ppinIn) override
		{ return the_real_one->Connect(ppinOut, ppinIn); }
	HRESULT STDMETHODCALLTYPE Render(IPin* ppinOut) override
		{ return the_real_one->Render(ppinOut); }
	HRESULT STDMETHODCALLTYPE RenderFile(LPCWSTR lpcwstrFile, LPCWSTR lpcwstrPlayList) override
	{
		HRESULT hr = the_real_one->RenderFile(lpcwstrFile, lpcwstrPlayList);
		if (FAILED(hr) && !*lpcwstrFile)
			hr = VFW_E_NOT_FOUND;
		return hr;
	}
	HRESULT STDMETHODCALLTYPE AddSourceFilter(LPCWSTR lpcwstrFileName, LPCWSTR lpcwstrFilterName, IBaseFilter** ppFilter) override
		{ return the_real_one->AddSourceFilter(lpcwstrFileName, lpcwstrFilterName, ppFilter); }
	HRESULT STDMETHODCALLTYPE SetLogFile(DWORD_PTR hFile) override
		{ return the_real_one->SetLogFile(hFile); }
	HRESULT STDMETHODCALLTYPE Abort() override
		{ return the_real_one->Abort(); }
	HRESULT STDMETHODCALLTYPE ShouldOperationContinue() override
		{ return the_real_one->ShouldOperationContinue(); }
};

static bool hijack_FilterGraph = false;

static void init()
{
	static bool initialized = false;
	if (initialized)
		return;
	initialized = true;
puts("krkrwine - hello world");
	
	auto wine_get_version = (const char*(*)())GetProcAddress(GetModuleHandle("ntdll.dll"), "wine_get_version");
	if (!wine_get_version)
	{
		puts("krkrwine: Don't install me on Windows!");
		return;
	}
	int major_ver = strtol(wine_get_version(), nullptr, 10);
	if (major_ver <= 8)
	{
		puts("krkrwine: your Wine is too old; use an older krkrwine or a newer Wine");
		return;
	}
	if (major_ver == 9)
	{
		// Kirikiri needs AvgTimePerFrame in WMV DMO output
		// fixed in 9.4 https://gitlab.winehq.org/wine/wine/-/merge_requests/5240
		// it's nontrivial to check if the hack is still needed, and krkrwine is only intended for Proton, so version check is good enough
		puts("krkrwine: patching CLSID_CWMVDecMediaObject");
		DWORD ignore;
		IClassFactory* fac_wmv = new AggregatingClassFactory<wrap_DMOObject, CLSID_CWMVDecMediaObject>();
		CoRegisterClassObject(CLSID_CWMVDecMediaObject, fac_wmv, CLSCTX_INPROC_SERVER, REGCLS_MULTIPLEUSE, &ignore);
	}
	if (major_ver == 9)
	{
		// BURIKO demands IGraphBuilder::RenderFile() with empty filename to fail with VFW_E_NOT_FOUND, other errors aren't accepted
		// BURIKO also demands that IGraphBuilder::Render() calls IPin::ReceiveConnection() with FORMAT_MPEGVideo or FORMAT_MPEG2Video;
		//    this connection will be rejected by the pin, but it takes the resolution from there and ignores the one in FORMAT_VideoInfo
		// https://bugs.winehq.org/show_bug.cgi?id=56491
		// fixed in (not yet)
		puts("krkrwine: patching CLSID_FilterGraph"); // those aren't wine bugs, it's workarounds for BURIKO nonsense
		hijack_FilterGraph = true;
	}
	
	
	
	//HMODULE fakelib = LoadLibrary("Z:/home/x2/steam/steamapps/common/MakingLovers/fakelib.dll");
	//printf("krkrwine=%p\n",fakelib);
	//auto* pInit = (void(*)())GetProcAddress(fakelib, "init");
	//printf("krkrwine=%p\n",pInit);
	//pInit();
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
	init();
	(void)dump_graph;
	(void)guid_to_str;
	
	HRESULT hr = ((decltype(DllGetClassObject)*)GetProcAddress(LoadLibrary("quartz.dll"), "DllGetClassObject"))(rclsid, riid, ppvObj);
	if (SUCCEEDED(hr) && hijack_FilterGraph)
	{
		IUnknown* fac = (IUnknown*)*ppvObj;
		IUnknown* ret = new WrappingClassFactory<wrap_FilterGraph>(fac);
		fac->Release();
		hr = ret->QueryInterface(riid, ppvObj);
		ret->Release();
	}
	return hr;
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
