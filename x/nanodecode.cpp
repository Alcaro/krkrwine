// SPDX-License-Identifier: LGPL-2.0-or-later

#define INITGUID
#define STRSAFE_NO_DEPRECATE
#include <windows.h>
#include <stdio.h>
#include <dshow.h>
#include <stdint.h>
#include <typeinfo>

static const GUID GUID_NULL = {}; // not defined in my headers, how lovely
DEFINE_GUID(CLSID_decodebin_parser, 0xf9d8d64e, 0xa144, 0x47dc, 0x8e, 0xe0, 0xf5, 0x34, 0x98, 0x37, 0x2c, 0x29);
DEFINE_GUID(CLSID_MPEG1Splitter_Alt,  0x731537a0,0xda85,0x4c5b,0xa4,0xc3,0x45,0x6c,0x49,0x45,0xf8,0xfa);
DEFINE_GUID(CLSID_CMpegVideoCodec_Alt,0xe688d538,0xe607,0x4169,0x86,0xde,0xef,0x08,0x21,0xf5,0xe7,0xa7);

static char* guid_to_str(const GUID& guid)
{
	static char buf[8][64];
	static int n = 0;
	char* ret = buf[n++%8];
	sprintf(ret, "{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", guid.Data1, guid.Data2, guid.Data3, guid.Data4[0], guid.Data4[1], guid.Data4[2], guid.Data4[3], guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7]);
	return ret;
}
static wchar_t* guid_to_wstr(const GUID& guid)
{
	static wchar_t buf[8][64] = {};
	char* guids = guid_to_str(guid);
	static int n = 0;
	wchar_t* ret = buf[n++%8];
	for (int i=0;guids[i];i++)
		ret[i] = guids[i];
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
			//HRESULT hr = graph->Connect(src_pin, dst_pin);
			HRESULT hr = graph->ConnectDirect(src_pin, dst_pin, nullptr);
			if (SUCCEEDED(hr))
			{
puts("match2.");
				return S_OK;
			}
printf("FAILCONNECTPAIR %.8lx\n", hr);
		}
	}
	return E_FAIL;
}

static void require(int seq, HRESULT hr, const char * text = "")
{
printf("%d.\n", seq);
	if (SUCCEEDED(hr))
		return;
	
	printf("fail: %d %.8lX%s%s\n", seq, hr, text?" ":"", text);
	exit(seq);
}



static CComPtr<IGraphBuilder> filterGraph;

IBaseFilter* try_make_filter(REFIID riid)
{
	IBaseFilter* filt = nullptr;
	CoCreateInstance(riid, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&filt));
	if (!filt)
		return nullptr;
	filterGraph->AddFilter(filt, guid_to_wstr(riid));
	return filt;
}
IBaseFilter* make_filter(IBaseFilter* filt) { return filt; }
IBaseFilter* make_filter(REFIID riid)
{
	IBaseFilter* filt = try_make_filter(riid);
	if (!filt)
	{
		printf("failed to create %s\n", guid_to_str(riid));
		exit(1);
	}
	return filt;
}
bool connect_chain(IBaseFilter** filts, size_t n, bool required)
{
	for (size_t i=0;i<n-1;i++)
	{
		IBaseFilter* first = filts[i];
		IBaseFilter* second = filts[i+1];
		if (FAILED(connect_filters(filterGraph, first, second)))
		{
			if (!required)
				return false;
			FILTER_INFO inf1;
			FILTER_INFO inf2;
			first->QueryFilterInfo(&inf1);
			second->QueryFilterInfo(&inf2);
			printf("failed to connect %ls to %ls\n", inf1.achName, inf2.achName);
			exit(1);
		}
	}
	return true;
}
template<typename... Ts>
IBaseFilter* chain_tail(Ts... args)
{
	IBaseFilter* filts[] = { make_filter(args)... };
	connect_chain(filts, sizeof...(Ts), true);
	return filts[sizeof...(Ts)-1];
}
template<typename... Ts>
void chain(Ts... args)
{
	IBaseFilter* filts[] = { make_filter(args)... };
	connect_chain(filts, sizeof...(Ts), true);
}
template<typename... Ts>
bool try_chain(Ts... args)
{
	IBaseFilter* filts[] = { make_filter(args)... };
	return connect_chain(filts, sizeof...(Ts), false);
}


int main()
{
	setvbuf(stdout, nullptr, _IONBF, 0);
	CoInitializeEx(NULL, COINIT_MULTITHREADED | COINIT_DISABLE_OLE1DDE);
	
	filterGraph.CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER);
	
	IBaseFilter* asyncReader = make_filter(CLSID_AsyncReader);
	CComPtr<IFileSourceFilter> asyncReaderFsf;
	require(20, asyncReader->QueryInterface(&asyncReaderFsf));
	require(21, asyncReaderFsf->Load(L"video.mpg", nullptr), "failed to open video.mpg, does the file exist?");
	
	IBaseFilter* mpegdec = try_make_filter(CLSID_CMpegVideoCodec);
	if (mpegdec)
	{
		IBaseFilter* demux = chain_tail(asyncReader, CLSID_MPEG1Splitter);
		chain(demux, mpegdec, CLSID_VideoMixingRenderer9);
		try_chain(demux, CLSID_CMpegAudioCodec, CLSID_DSoundRender); // don't worry too much if the file doesn't have sound
	}
	else
	{
		puts("CLSID_CMpegVideoCodec not available? Probably running on Wine, trying CLSID_decodebin_parser instead");
		IBaseFilter* demux = chain_tail(asyncReader, CLSID_decodebin_parser);
		chain(demux, CLSID_VideoMixingRenderer9);
		try_chain(demux, CLSID_DSoundRender);
	}
	
	puts("connected");
	
	CComPtr<IMediaControl> filterGraph_mc;
	require(40, filterGraph.QueryInterface(&filterGraph_mc));
	filterGraph_mc->Run();
	puts("running");
	SleepEx(15000, true);
	
	puts("exit");
}
