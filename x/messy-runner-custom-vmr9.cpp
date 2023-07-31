// SPDX-License-Identifier: GPL-2.0-only
// This file contains some code copypasted from Kirikiri <https://github.com/krkrz/krkr2/tree/master>.
// Kirikiri is dual licensed under GPLv2 and some homemade license. Since the homemade license is Japanese,
//    and I can't read that, I can't accept that license; therefore, this file is GPLv2 only.
// (I'm not sure if headers and other required interopability data is covered by copyright, but better safe than sorry.)
// (I also don't know if Kirikiri is 2.0 only or 2.0 or later, so I'll pick the safe choice.)

#define INITGUID
#define STRSAFE_NO_DEPRECATE
#include <windows.h>
#include <stdio.h>
#include <dshow.h>
#include <stdint.h>
#include <d3d9.h>
#include <vmr9.h>
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
	HRESULT CopyTo(T** ppT)
	{
		p->AddRef();
		*ppT = p;
		return S_OK;
	}
};

static HRESULT connect_filters(IGraphBuilder* graph, IBaseFilter* src, IBaseFilter* dst)
{
puts("connect.");
	CComPtr<IEnumPins> src_enum;
	if (FAILED(src->EnumPins(&src_enum)))
		return E_FAIL;
	
	CComPtr<IPin> src_pin;
	while (src_enum->Next(1, &src_pin, nullptr) == S_OK)
	{
puts("src.");
		PIN_INFO src_info;
		if (FAILED(src_pin->QueryPinInfo(&src_info)))
			return E_FAIL;
		if (src_info.pFilter)
			src_info.pFilter->Release();
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
puts("dst.");
			PIN_INFO dst_info;
			if (FAILED(dst_pin->QueryPinInfo(&dst_info)))
				return E_FAIL;
			if (dst_info.pFilter)
				dst_info.pFilter->Release();
			if (dst_info.dir != PINDIR_INPUT)
				continue;
			
			dst_pin->ConnectedTo(&check_pin);
			if (check_pin != nullptr)
				continue;
			
puts("match.");
			//if (SUCCEEDED(graph->Connect(src_pin, dst_pin)))
			if (SUCCEEDED(graph->ConnectDirect(src_pin, dst_pin, nullptr)))
			{
puts("match2.");
				return S_OK;
			}
puts("match3.");
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

IBaseFilter* make_filter(IBaseFilter* filt) { return filt; }
IBaseFilter* make_filter(REFIID riid)
{
	IBaseFilter* filt = nullptr;
	CoCreateInstance(riid, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&filt));
	if (!filt)
	{
		printf("failed to create %s\n", guid_to_str(riid));
		exit(1);
	}
	filterGraph->AddFilter(filt, guid_to_wstr(riid));
	return filt;
}
void connect_chain(IBaseFilter** filts, size_t n)
{
	for (size_t i=0;i<n-1;i++)
	{
		IBaseFilter* first = filts[i];
		IBaseFilter* second = filts[i+1];
		if (FAILED(connect_filters(filterGraph, first, second)))
		{
			FILTER_INFO inf1;
			FILTER_INFO inf2;
			first->QueryFilterInfo(&inf1);
			second->QueryFilterInfo(&inf2);
			printf("failed to connect %ls to %ls\n", inf1.achName, inf2.achName);
			exit(1);
		}
	}
}
template<typename... Ts>
IBaseFilter* chain_tail(Ts... args)
{
	IBaseFilter* filts[] = { make_filter(args)... };
	connect_chain(filts, sizeof...(Ts));
	return filts[sizeof...(Ts)-1];
}
template<typename... Ts>
void chain(Ts... args)
{
	IBaseFilter* filts[] = { make_filter(args)... };
	connect_chain(filts, sizeof...(Ts));
}








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
printf("fancy_QI %s -> ", guid_to_str(riid));
		*ppvObject = nullptr;
		if (riid == __uuidof(IUnknown))
		{
			IUnknown* ret = (decltype(first_helper<Tis...>())*)this;
			ret->AddRef();
			*ppvObject = (void*)ret;
puts("ok");
			return S_OK;
		}
		bool ok = (QueryInterfaceSingle<Tis>(riid, ppvObject) || ...);
if (ok) puts("ok");
else puts("fail");
		return ok ? S_OK : E_NOINTERFACE;
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
	virtual ~com_base() {}
};

class my_vmr9_allocator : public com_base<IVMRSurfaceAllocator9, IVMRImagePresenter9> {
public:
	IDirect3D9* d3d;
	IDirect3DDevice9* d3ddev;
	IVMRSurfaceAllocatorNotify9* allocNotify;
	
	IDirect3DSurface9* surf[16];
	IDirect3DTexture9* tex;
	IDirect3DVertexBuffer9* vertbuf;
	CComPtr<IDirect3DSurface9> m_RenderTarget;
	HWND wnd;
	
	struct VideoVertex
	{
		float		x, y, z, w;
		float		tu, tv;
	};
	
	void debug(const char * fmt, ...)
	{
		char buf[1024];
		
		va_list args;
		va_start(args, fmt);
		vsnprintf(buf, 1024, fmt, args);
		va_end(args);
		
		fprintf(stdout, "my_vmr9_allocator %s\n", buf);
		fflush(stdout);
	}
	
	HRESULT STDMETHODCALLTYPE InitializeDevice(DWORD_PTR dwUserID, VMR9AllocationInfo* lpAllocInfo, DWORD* lpNumBuffers)
	{
		debug("IVMRSurfaceAllocator9 InitializeDevice %u %lu, %lux%lu, fmt %lx", (unsigned)dwUserID, *lpNumBuffers, lpAllocInfo->dwWidth, lpAllocInfo->dwHeight, lpAllocInfo->Format);
		
		D3DCAPS9	d3dcaps;
		d3ddev->GetDeviceCaps( &d3dcaps );
		if( d3dcaps.TextureCaps & D3DPTEXTURECAPS_POW2 )
		{	puts("walruses??");
			exit(1);
		}

		lpAllocInfo->dwFlags |= VMR9AllocFlag_TextureSurface;
		
		HRESULT hr = allocNotify->AllocateSurfaceHelper(lpAllocInfo, lpNumBuffers, surf);
printf("EXPECTERROR=%lx NBUF=%lu\n", hr, *lpNumBuffers);
		
		if( FAILED(hr) && !(lpAllocInfo->dwFlags & VMR9AllocFlag_3DRenderTarget) )
		{
			if( lpAllocInfo->Format > MAKEFOURCC('0','0','0','0') )
			{
				D3DDISPLAYMODE dm; 
				if( FAILED( hr = (d3ddev->GetDisplayMode( 0, &dm )) ) )
					return hr;

				if( D3D_OK != ( hr = d3ddev->CreateTexture(lpAllocInfo->dwWidth, lpAllocInfo->dwHeight, 1, D3DUSAGE_RENDERTARGET, dm.Format, 
									D3DPOOL_DEFAULT, &tex, NULL) ) )
					return hr;
				puts("krmovie : Use offscreen and YUV surface.");
			} else {
				puts("krmovie : Use offscreen surface.");
			}
			lpAllocInfo->dwFlags &= ~VMR9AllocFlag_TextureSurface;
			lpAllocInfo->dwFlags |= VMR9AllocFlag_OffscreenSurface;
			if( FAILED( hr = allocNotify->AllocateSurfaceHelper(lpAllocInfo, lpNumBuffers, surf ) ) )
				return hr;
		} else {
			puts("krmovie : Use texture surface.");
		}




	D3DDISPLAYMODE dm;
	if( FAILED(hr = d3d->GetAdapterDisplayMode( D3DADAPTER_DEFAULT, &dm )) )
		return hr;

	float	vtx_l = 0.0f;
	float	vtx_r = 0.0f;
	float	vtx_t = 0.0f;
	float	vtx_b = 0.0f;

	vtx_l = static_cast<float>(0-0);
	vtx_t = static_cast<float>(0-0);
	vtx_r = vtx_l + static_cast<float>(640);
	vtx_b = vtx_t + static_cast<float>(480);

	float	tex_w = static_cast<float>(lpAllocInfo->dwWidth);
	float	tex_h = static_cast<float>(lpAllocInfo->dwWidth);
	float	video_w = static_cast<float>(lpAllocInfo->dwWidth);
	float	video_h = static_cast<float>(lpAllocInfo->dwWidth);
	if( vtx_r == 0.0f || vtx_b == 0.0f ) {
		vtx_r = static_cast<float>(dm.Width);
		vtx_b = static_cast<float>(dm.Height);
	}

	VideoVertex		m_Vtx[4];

	m_Vtx[0].x = vtx_l - 0.5f;	// TL
	m_Vtx[0].y = vtx_t - 0.5f;
	m_Vtx[0].tu = 0.0f;
	m_Vtx[0].tv = 0.0f;

	m_Vtx[1].x = vtx_r - 0.5f;	// TR
	m_Vtx[1].y = vtx_t - 0.5f;
	m_Vtx[1].tu = video_w / tex_w;
	m_Vtx[1].tv = 0.0f;

	m_Vtx[2].x = vtx_r - 0.5f;	// BR
	m_Vtx[2].y = vtx_b - 0.5f;
	m_Vtx[2].tu = video_w / tex_w;
	m_Vtx[2].tv = video_h / tex_h;

	m_Vtx[3].x = vtx_l - 0.5f;	// BL
	m_Vtx[3].y = vtx_b - 0.5f;
	m_Vtx[3].tu = 0.0f;
	m_Vtx[3].tv = video_h / tex_h;

	if( FAILED( hr = d3ddev->CreateVertexBuffer( sizeof(m_Vtx) ,D3DUSAGE_WRITEONLY, D3DFVF_XYZRHW|D3DFVF_TEX1, D3DPOOL_MANAGED, &vertbuf, NULL ) ) )
		return hr;

	void* pData;
	if( FAILED( hr = vertbuf->Lock( 0, sizeof(pData), &pData, 0 ) ) )
		return hr;

	memcpy( pData, m_Vtx, sizeof(m_Vtx) );

	if( FAILED( hr = vertbuf->Unlock() ) )
		return hr;

	return S_OK;
	}
	
	HRESULT STDMETHODCALLTYPE TerminateDevice(DWORD_PTR dwID)
	{
		debug("IVMRSurfaceAllocator9 TerminateDevice");
		return E_OUTOFMEMORY;
	}
	HRESULT STDMETHODCALLTYPE GetSurface(DWORD_PTR dwUserID, DWORD SurfaceIndex, DWORD SurfaceFlags, IDirect3DSurface9** lplpSurface)
	{
		debug("IVMRSurfaceAllocator9 GetSurface");
		surf[SurfaceIndex]->AddRef();
		*lplpSurface = surf[SurfaceIndex];
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE AdviseNotify(IVMRSurfaceAllocatorNotify9* lpIVMRSurfAllocNotify)
	{
		debug("IVMRSurfaceAllocator9 AdviseNotify");
		return E_OUTOFMEMORY;
	}
	
	HRESULT STDMETHODCALLTYPE StartPresenting(DWORD_PTR dwUserID)
	{
		debug("IVMRImagePresenter9 StartPresenting");
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE StopPresenting(DWORD_PTR dwUserID)
	{
		debug("IVMRImagePresenter9 StopPresenting");
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE PresentImage(DWORD_PTR dwUserID, VMR9PresentationInfo* lpPresInfo)
	{
		debug("IVMRImagePresenter9 PresentImage");
		
		HRESULT hr;
		CComPtr<IDirect3DDevice9> device;
		if( FAILED(hr = lpPresInfo->lpSurf->GetDevice(&device.p )) )
			return hr;

		if( FAILED(hr = device->SetRenderTarget( 0, m_RenderTarget ) ) )
			return hr;
		if( tex != NULL )
		{
			puts("stretchy");
			CComPtr<IDirect3DSurface9> pSurf;
			if( SUCCEEDED(hr = tex->GetSurfaceLevel(0, &pSurf)) ) {
puts("stretchy2");
				if( FAILED(hr = device->StretchRect( lpPresInfo->lpSurf, NULL, pSurf, NULL, D3DTEXF_NONE )) ) {
					return hr;
				}
			} else {
				return hr;
			}
			if( FAILED(hr = DrawVideoPlane( device, tex ) ) )
				return hr;
		} else {
			CComPtr<IDirect3DTexture9> texture;
			if( FAILED(hr = lpPresInfo->lpSurf->GetContainer( IID_IDirect3DTexture9, (LPVOID*)&texture.p ) ) )
				return hr;
			if( FAILED(hr = DrawVideoPlane( device, texture.p ) ) )
				return hr;
		}
		RECT r = { 0, 0, 640, 480 };
		d3ddev->Present( &r, NULL, wnd, NULL );
		return S_OK;
	}
	
	HRESULT DrawVideoPlane( IDirect3DDevice9* device, IDirect3DTexture9* tex )
	{
	//	device->Clear(0,NULL,D3DCLEAR_TARGET,0,1.0f,0);
		if( SUCCEEDED(device->BeginScene()) )
		{
			struct CAutoEndSceneCall {
				IDirect3DDevice9*	m_Device;
				CAutoEndSceneCall( IDirect3DDevice9* device ) : m_Device(device) {}
				~CAutoEndSceneCall() { m_Device->EndScene(); }
			};
			{
				HRESULT				hr;
				CAutoEndSceneCall	autoEnd(device);
				if( FAILED( hr = device->SetTexture( 0, tex ) ) )
					return hr;
				if( FAILED( hr = device->SetStreamSource(0, vertbuf, 0, sizeof(VideoVertex) ) ) )
					return hr;
				if( FAILED( hr = device->SetFVF( D3DFVF_XYZRHW|D3DFVF_TEX1 ) ) )
					return hr;
				if( FAILED( hr = device->DrawPrimitive( D3DPT_TRIANGLEFAN, 0, 2 ) ) )
					return hr;
	//			device->DrawPrimitiveUP( D3DPT_TRIANGLEFAN, 2, reinterpret_cast<void*>(m_Vtx), sizeof(m_Vtx[0]) );
				if( FAILED( hr = device->SetTexture( 0, NULL) ) )
					return hr;
			}

			//allocNotify->NotifyEvent(EC_UPDATE,0,0);
		}
		return S_OK;
	}
};

my_vmr9_allocator vmr9_alloc;


int main()
{
	setvbuf(stdout, nullptr, _IONBF, 0);
	CoInitializeEx(NULL, COINIT_MULTITHREADED | COINIT_DISABLE_OLE1DDE);
	
	filterGraph.CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER);
	
	IBaseFilter* asyncReader = make_filter(CLSID_AsyncReader);
	CComPtr<IFileSourceFilter> asyncReaderFsf;
	require(20, asyncReader->QueryInterface(&asyncReaderFsf));
	
	require(21, asyncReaderFsf->Load(L"video.mpg", nullptr), "failed to open video.mpg, does the file exist?");
	IBaseFilter* demux = chain_tail(asyncReader, CLSID_MPEG1Splitter);
	
	
	WNDCLASSEXA wcex = { sizeof(WNDCLASSEX), CS_VREDRAW | CS_HREDRAW, DefWindowProcA, 0L, 0L, nullptr, NULL, NULL, NULL, NULL, "windowclass", NULL };
	RegisterClassEx(&wcex);
	
	HWND wnd = CreateWindowA("windowclass", "VMR9 child", 0, 0, 0, 640, 480, nullptr, NULL, nullptr, NULL );
	ShowWindow(wnd, SW_SHOWDEFAULT);
	UpdateWindow(wnd);
	
puts("a");
	IBaseFilter* vmr9 = make_filter(CLSID_VideoMixingRenderer9);
puts("c");
	CComPtr<IVMRFilterConfig9> pConfig;
puts("d");
	vmr9->QueryInterface(&pConfig);
puts("e");
	pConfig->SetNumberOfStreams(1);
puts("f");
	pConfig->SetRenderingMode(VMR9Mode_Renderless);
puts("g");
	
	CComPtr<IDirect3D9> d3d;
	d3d.p = Direct3DCreate9(D3D_SDK_VERSION);
printf("d3d=%p\n", d3d.p);
	
	D3DPRESENT_PARAMETERS d3dpp = {};
	D3DDISPLAYMODE	dm;
	require(5, d3d->GetAdapterDisplayMode(D3DADAPTER_DEFAULT, &dm));
	d3dpp.Windowed = TRUE;
	d3dpp.SwapEffect = D3DSWAPEFFECT_COPY;
	d3dpp.BackBufferFormat = dm.Format;
	d3dpp.BackBufferHeight = 640;
	d3dpp.BackBufferWidth = 480;
	d3dpp.hDeviceWindow = wnd;
	
puts("h");
	CComPtr<IDirect3DDevice9> d3ddev;
	DWORD	BehaviorFlags = D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED;
	require(1, d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, NULL, BehaviorFlags, &d3dpp, &d3ddev));
	
	vmr9_alloc.d3d = d3d;
	vmr9_alloc.d3ddev = d3ddev;
	vmr9_alloc.wnd = wnd;
	
	require(2, d3ddev->GetRenderTarget( 0, &vmr9_alloc.m_RenderTarget ) );

	D3DCAPS9	d3dcaps;
	require(1, d3ddev->GetDeviceCaps( &d3dcaps ) );
	if( d3dcaps.TextureFilterCaps & D3DPTFILTERCAPS_MAGFLINEAR ) {
		require(2, d3ddev->SetSamplerState( 0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR ) );
	} else {
		require(3, d3ddev->SetSamplerState( 0, D3DSAMP_MAGFILTER, D3DTEXF_POINT ) );
	}

	if( d3dcaps.TextureFilterCaps & D3DPTFILTERCAPS_MINFLINEAR ) {
		require(4, d3ddev->SetSamplerState( 0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR ) );
	} else {
		require(5, d3ddev->SetSamplerState( 0, D3DSAMP_MINFILTER, D3DTEXF_POINT ) );
	}

	require(6, d3ddev->SetSamplerState( 0, D3DSAMP_ADDRESSU,  D3DTADDRESS_CLAMP ) );
	require(7, d3ddev->SetSamplerState( 0, D3DSAMP_ADDRESSV,  D3DTADDRESS_CLAMP ) );
	require(8, d3ddev->SetRenderState( D3DRS_CULLMODE, D3DCULL_NONE ) );
	require(9, d3ddev->SetRenderState( D3DRS_LIGHTING, FALSE ) );
	require(10, d3ddev->SetRenderState( D3DRS_ZENABLE, FALSE ) );
	require(11, d3ddev->SetRenderState( D3DRS_ALPHABLENDENABLE, FALSE ) );
	require(12, d3ddev->SetTextureStageState( 0, D3DTSS_COLOROP,   D3DTOP_SELECTARG1 ) );
	require(13, d3ddev->SetTextureStageState( 0, D3DTSS_COLORARG1, D3DTA_TEXTURE ) );
	require(14, d3ddev->SetTextureStageState( 0, D3DTSS_ALPHAOP,   D3DTOP_DISABLE ) );
	
	HMONITOR hMonitor = d3d->GetAdapterMonitor(D3DADAPTER_DEFAULT);
	
	CComPtr<IVMRSurfaceAllocatorNotify9> allocNotify;
puts("h");
	vmr9->QueryInterface(&allocNotify);
	vmr9_alloc.allocNotify = allocNotify;
puts("i");
	allocNotify->AdviseSurfaceAllocator(12345678, &vmr9_alloc);
puts("j");
	allocNotify->SetD3DDevice(d3ddev, hMonitor);
puts("k");
	
	chain(demux, CLSID_CMpegVideoCodec, (IBaseFilter*)vmr9);
puts("k");
	chain(demux, CLSID_CMpegAudioCodec, CLSID_DSoundRender);
puts("l");
	
	//require(21, asyncReaderFsf->Load(L"waga.wmv", nullptr), "failed to open waga.wmv, does the file exist?");
	//IBaseFilter* demux = make_filter(CLSID_decodebin_parser);
	//chain(asyncReader, demux);
	//chain(demux, CLSID_VideoMixingRenderer9);
	//chain(demux, CLSID_DSoundRender);
	
	/*
	require(3, asyncReader.CoCreateInstance(CLSID_AsyncReader, nullptr, CLSCTX_INPROC_SERVER));
	require(4, mpegSplitter.CoCreateInstance(CLSID_MPEG1Splitter, nullptr, CLSCTX_INPROC_SERVER));
	require(5, mpegVideoCodec.CoCreateInstance(CLSID_CMpegVideoCodec, nullptr, CLSCTX_INPROC_SERVER));
	//require(4, mpegSplitter.CoCreateInstance(CLSID_MPEG1Splitter_Alt, nullptr, CLSCTX_INPROC_SERVER));
	//require(5, mpegVideoCodec.CoCreateInstance(CLSID_CMpegVideoCodec_Alt, nullptr, CLSCTX_INPROC_SERVER));
	require(6, mpegAudioCodec.CoCreateInstance(CLSID_CMpegAudioCodec, nullptr, CLSCTX_INPROC_SERVER));
	require(7, decodebin.CoCreateInstance(CLSID_decodebin_parser, nullptr, CLSCTX_INPROC_SERVER));
	require(8, dsound.CoCreateInstance(CLSID_DSoundRender, nullptr, CLSCTX_INPROC_SERVER));
	require(9, vmr.CoCreateInstance(CLSID_VideoMixingRenderer9, nullptr, CLSCTX_INPROC_SERVER));
	//require(9, vmr.CoCreateInstance(CLSID_VideoMixingRenderer, nullptr, CLSCTX_INPROC_SERVER));
	
	require(13, filterGraph->AddFilter(asyncReader, L"asyncReader"));
	//require(14, filterGraph->AddFilter(mpegSplitter, L"mpegSplitter"));
	//require(15, filterGraph->AddFilter(mpegVideoCodec, L"mpegVideoCodec"));
	//require(16, filterGraph->AddFilter(mpegAudioCodec, L"mpegAudioCodec"));
	require(17, filterGraph->AddFilter(decodebin, L"decodebin"));
	require(18, filterGraph->AddFilter(dsound, L"dsound"));
	require(19, filterGraph->AddFilter(vmr, L"vmr"));
	
	require(20, asyncReader.QueryInterface(&asyncReaderFsf));
	require(21, asyncReaderFsf->Load(L"waga.wmv", nullptr), "failed to open waga.wmv, does the file exist?");
	//require(21, asyncReaderFsf->Load(L"video.mpg", nullptr), "failed to open video.mpg, does the file exist?");
	//require(21, asyncReaderFsf->Load(L"data_video_sample_640x360.mpeg", nullptr), "failed to open data_video_sample_640x360.mpeg, does the file exist?");
	
	//require(31, connect_filters(filterGraph, asyncReader, mpegSplitter));
	//require(32, connect_filters(filterGraph, mpegSplitter, mpegVideoCodec));
	//require(33, connect_filters(filterGraph, mpegSplitter, mpegAudioCodec));
	//require(34, connect_filters(filterGraph, mpegVideoCodec, sink));
	//require(35, connect_filters(filterGraph, mpegAudioCodec, dsound));
	
	//require(31, connect_filters(filterGraph, asyncReader, decodebin));
	//require(32, connect_filters(filterGraph, decodebin, sink));
	//require(33, connect_filters(filterGraph, decodebin, dsound));
	
	//require(31, connect_filters(filterGraph, asyncReader, vmr));
	//require(31, connect_filters(filterGraph, asyncReader, mpegSplitter));
	//require(32, connect_filters(filterGraph, mpegSplitter, mpegVideoCodec));
	//require(34, connect_filters(filterGraph, mpegVideoCodec, vmr));
	//require(33, connect_filters(filterGraph, mpegSplitter, mpegAudioCodec));
	//require(35, connect_filters(filterGraph, mpegAudioCodec, dsound));
	
	require(31, connect_filters(filterGraph, asyncReader, decodebin));
	require(32, connect_filters(filterGraph, decodebin, vmr));
	require(33, connect_filters(filterGraph, decodebin, dsound));
	*/
	
	puts("connected");
	
	CComPtr<IMediaControl> filterGraph_mc;
	require(40, filterGraph.QueryInterface(&filterGraph_mc));
	filterGraph_mc->Run();
	puts("running");
	SleepEx(4000, true);
	
	puts("exit");
}
