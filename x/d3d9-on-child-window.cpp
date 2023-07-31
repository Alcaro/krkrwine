#include <stdio.h>
#include <stdint.h>
#include <windows.h>
#include <d3d9.h>

HWND wnd_parent;
HWND wnd;
int f;

LRESULT myWindowProcA(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam)
{
printf("%d %u %d\n", hWnd==wnd, Msg, f++);
	if (Msg == WM_CLOSE)
		exit(0);
	return DefWindowProcA(hWnd, Msg, wParam, lParam);
}

int main()
{
	WNDCLASSEXA wcex = { sizeof(WNDCLASSEX), CS_DBLCLKS, myWindowProcA, 0L, 0L, nullptr, NULL, NULL, NULL, NULL, "windowclass", NULL };
	RegisterClassEx(&wcex);
	
	WNDCLASSEXA wcex2 = { sizeof(WNDCLASSEX), 0, myWindowProcA, 0L, 0L, nullptr, NULL, NULL, NULL, NULL, "windowclass_child", NULL };
	RegisterClassEx(&wcex2);
	
	uint32_t wstyle = WS_CAPTION|WS_CLIPSIBLINGS|WS_CLIPCHILDREN|WS_VISIBLE|WS_SYSMENU|WS_MINIMIZEBOX;
	uint32_t wstyleex = WS_EX_ACCEPTFILES|WS_EX_WINDOWEDGE|WS_EX_CONTROLPARENT;
	wnd_parent = CreateWindowExA(wstyleex, "windowclass", "parent", wstyle, 0, 0, 640, 480, nullptr, NULL, nullptr, NULL );
	ShowWindow(wnd_parent, SW_SHOWDEFAULT);
	UpdateWindow(wnd_parent);
	
	wnd = CreateWindowExA(0, "windowclass_child", "child", WS_CHILD, 0, 0, 320, 240, wnd_parent, NULL, nullptr, NULL );
	ShowWindow(wnd, SW_SHOWDEFAULT);
	UpdateWindow(wnd);
	
	IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
	DWORD	BehaviorFlags = D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED;
	
	D3DPRESENT_PARAMETERS d3dpp = {};
	D3DDISPLAYMODE	dm;
	d3d->GetAdapterDisplayMode(D3DADAPTER_DEFAULT, &dm);
	d3dpp.Windowed = TRUE;
	d3dpp.SwapEffect = D3DSWAPEFFECT_COPY;
	d3dpp.BackBufferFormat = dm.Format;
	d3dpp.BackBufferHeight = 640;
	d3dpp.BackBufferWidth = 480;
	d3dpp.hDeviceWindow = wnd_parent;
	
	IDirect3DDevice9* d3ddev;
	d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, NULL, BehaviorFlags, &d3dpp, &d3ddev);
	
	IDirect3D9* d3d2 = Direct3DCreate9(D3D_SDK_VERSION);
	d3dpp.hDeviceWindow = wnd;
	IDirect3DDevice9* d3ddev2;
	d3d2->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, NULL, BehaviorFlags, &d3dpp, &d3ddev2);
	
	for (int frame=0;frame<600;frame++)
	{
		d3ddev->Clear(0,NULL,D3DCLEAR_TARGET,D3DCOLOR_RGBA(255,0,frame*4,0),1.0f,0);
		d3ddev->Present( nullptr, NULL, nullptr, NULL );
		
		d3ddev2->Clear(0,NULL,D3DCLEAR_TARGET,D3DCOLOR_RGBA(0,255,frame*4,0),1.0f,0);
		d3ddev2->Present( nullptr, NULL, nullptr, NULL );
		
		MSG msg;
		while( (PeekMessage( &msg, NULL, 0, 0, PM_REMOVE )) != 0)
		{ 
			TranslateMessage(&msg); 
			DispatchMessage(&msg); 
		} 
	}
}
