// Stands in for D3D9Ex -> D3D11 shared render targets, which Wine's d3d9 does not
// implement, by copying each video frame through system memory. Dead or Alive 6 needs
// this to show its movies under Wine. Opt in with TP_D3D9_SHARED_COPY.
#include <StdInc.h>
#include "Utility/InitFunction.h"
#include <d3d9.h>
#include <d3d11.h>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

#if _M_AMD64

// Vtable slots, counted from the COM interface declarations.
static const int kD3D9ExCreateDeviceEx = 20;
static const int kD3D9DeviceCreateRenderTarget = 28;
static const int kProcessorServiceCreateVideoProcessor = 11;
static const int kVideoProcessorVideoProcessBlt = 8;
static const int kD3D11DeviceOpenSharedResource = 28;
static const int kD3D11ContextPSSetShaderResources = 8;

// IID_IDirectXVideoProcessorService
static const GUID kProcessorServiceIid = { 0xfc51a552, 0xd5e7, 0x11d9, { 0xaf, 0x55, 0x00, 0x05, 0x4e, 0x43, 0xff, 0x02 } };

static const int kLoggedFrames = 3;

struct SharedSurface
{
	HANDLE handle;
	IDirect3DSurface9* surface;
	UINT width;
	UINT height;
	DXGI_FORMAT format;
	ID3D11Texture2D* texture;
	std::vector<uint8_t> frame;
	bool dirty;
};

static std::mutex g_lock;
static std::vector<std::unique_ptr<SharedSurface>> g_surfaces;
static std::atomic<bool> g_framePending{ false };
static uintptr_t g_nextHandle = 0x50000000;
static int g_framesCopied = 0;
static ID3D11DeviceContext* g_context = nullptr;

static void Log(const char* format, ...)
{
	static FILE* file = fopen("OpenParrotVideo.log", "w");
	if (file == nullptr)
		return;

	va_list args;
	va_start(args, format);
	vfprintf(file, format, args);
	va_end(args);

	fputc('\n', file);
	fflush(file);
}

template <typename T>
static void PatchVtable(void* object, int slot, T hook, T* original)
{
	void** vtable = *reinterpret_cast<void***>(object);
	if (vtable[slot] == reinterpret_cast<void*>(hook))
		return;

	// A second class would need its own original, and Wine shares these vtables.
	if (*original != nullptr)
	{
		Log("another vtable has slot %d unpatched", slot);
		return;
	}

	DWORD oldProtect;
	if (!VirtualProtect(&vtable[slot], sizeof(void*), PAGE_READWRITE, &oldProtect))
	{
		Log("could not patch vtable slot %d (0x%X)", slot, GetLastError());
		return;
	}

	*original = reinterpret_cast<T>(vtable[slot]);
	vtable[slot] = reinterpret_cast<void*>(hook);
	VirtualProtect(&vtable[slot], sizeof(void*), oldProtect, &oldProtect);
}

static SharedSurface* FindByHandle(HANDLE handle)
{
	for (auto& entry : g_surfaces)
	{
		if (entry->handle == handle)
			return entry.get();
	}
	return nullptr;
}

static SharedSurface* FindBySurface(IDirect3DSurface9* surface)
{
	for (auto& entry : g_surfaces)
	{
		if (entry->surface == surface)
			return entry.get();
	}
	return nullptr;
}

static void ApplyPendingFrames(ID3D11DeviceContext* context)
{
	// The video thread holds this while it copies, so draw on and catch up next time.
	std::unique_lock<std::mutex> lock(g_lock, std::try_to_lock);
	if (!lock.owns_lock())
		return;

	g_framePending = false;

	for (size_t i = 0; i < g_surfaces.size();)
	{
		SharedSurface* entry = g_surfaces[i].get();
		if (entry->texture != nullptr)
		{
			// Once only this file's reference is left, the game has finished with it.
			entry->texture->AddRef();
			if (entry->texture->Release() == 1)
			{
				entry->texture->Release();
				g_surfaces.erase(g_surfaces.begin() + i);
				continue;
			}

			if (entry->dirty)
			{
				context->UpdateSubresource(entry->texture, 0, nullptr, entry->frame.data(), entry->width * 4, 0);
				entry->dirty = false;
			}
		}
		i++;
	}
}

typedef void(STDMETHODCALLTYPE* PSSetShaderResourcesFn)(ID3D11DeviceContext*, UINT, UINT, ID3D11ShaderResourceView* const*);
static PSSetShaderResourcesFn g_origPSSetShaderResources;

// Runs on the game's render thread just before a texture is bound, the one place the
// immediate context is safe to use.
static void STDMETHODCALLTYPE PSSetShaderResourcesHook(ID3D11DeviceContext* This, UINT start, UINT count, ID3D11ShaderResourceView* const* views)
{
	if (This == g_context && g_framePending.load(std::memory_order_relaxed))
		ApplyPendingFrames(This);

	g_origPSSetShaderResources(This, start, count, views);
}

typedef HRESULT(STDMETHODCALLTYPE* OpenSharedResourceFn)(ID3D11Device*, HANDLE, REFIID, void**);
static OpenSharedResourceFn g_origOpenSharedResource;

static HRESULT STDMETHODCALLTYPE OpenSharedResourceHook(ID3D11Device* This, HANDLE handle, REFIID riid, void** resource)
{
	std::lock_guard<std::mutex> lock(g_lock);

	SharedSurface* entry = FindByHandle(handle);
	if (entry == nullptr)
		return g_origOpenSharedResource(This, handle, riid, resource);

	if (g_context == nullptr)
	{
		This->GetImmediateContext(&g_context);
		PatchVtable(g_context, kD3D11ContextPSSetShaderResources, &PSSetShaderResourcesHook, &g_origPSSetShaderResources);
	}

	if (entry->texture == nullptr)
	{
		D3D11_TEXTURE2D_DESC desc = {};
		desc.Width = entry->width;
		desc.Height = entry->height;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = entry->format;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

		HRESULT hr = This->CreateTexture2D(&desc, nullptr, &entry->texture);
		if (FAILED(hr))
		{
			Log("CreateTexture2D %ux%u format %d failed (0x%X)", entry->width, entry->height, entry->format, hr);
			return hr;
		}
		Log("opened handle %p as a %ux%u texture", handle, entry->width, entry->height);
	}

	return entry->texture->QueryInterface(riid, resource);
}

typedef HRESULT(WINAPI* D3D11CreateDeviceFn)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
static D3D11CreateDeviceFn g_origD3D11CreateDevice;

static HRESULT WINAPI D3D11CreateDeviceHook(IDXGIAdapter* adapter, D3D_DRIVER_TYPE driverType, HMODULE software, UINT flags, const D3D_FEATURE_LEVEL* levels, UINT levelCount, UINT sdkVersion, ID3D11Device** device, D3D_FEATURE_LEVEL* level, ID3D11DeviceContext** context)
{
	HRESULT hr = g_origD3D11CreateDevice(adapter, driverType, software, flags, levels, levelCount, sdkVersion, device, level, context);
	if (SUCCEEDED(hr) && device != nullptr && *device != nullptr)
		PatchVtable(*device, kD3D11DeviceOpenSharedResource, &OpenSharedResourceHook, &g_origOpenSharedResource);
	return hr;
}

typedef HRESULT(STDMETHODCALLTYPE* CreateRenderTargetFn)(IDirect3DDevice9*, UINT, UINT, D3DFORMAT, D3DMULTISAMPLE_TYPE, DWORD, BOOL, IDirect3DSurface9**, HANDLE*);
static CreateRenderTargetFn g_origCreateRenderTarget;

static HRESULT STDMETHODCALLTYPE CreateRenderTargetHook(IDirect3DDevice9* This, UINT width, UINT height, D3DFORMAT format, D3DMULTISAMPLE_TYPE multisample, DWORD quality, BOOL lockable, IDirect3DSurface9** surface, HANDLE* shared)
{
	if (shared == nullptr || *shared != nullptr)
		return g_origCreateRenderTarget(This, width, height, format, multisample, quality, lockable, surface, shared);

	DXGI_FORMAT dxgiFormat;
	if (format == D3DFMT_X8R8G8B8)
		dxgiFormat = DXGI_FORMAT_B8G8R8X8_UNORM;
	else if (format == D3DFMT_A8R8G8B8)
		dxgiFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
	else
	{
		Log("shared render target has unsupported format %d", format);
		return g_origCreateRenderTarget(This, width, height, format, multisample, quality, lockable, surface, shared);
	}

	// Lockable, because the frames are read back from it.
	HRESULT hr = g_origCreateRenderTarget(This, width, height, format, multisample, quality, TRUE, surface, nullptr);
	if (FAILED(hr))
	{
		Log("CreateRenderTarget %ux%u failed (0x%X)", width, height, hr);
		return hr;
	}

	std::lock_guard<std::mutex> lock(g_lock);

	auto entry = std::make_unique<SharedSurface>();
	entry->handle = reinterpret_cast<HANDLE>(g_nextHandle);
	g_nextHandle += 4;
	entry->surface = *surface;
	entry->width = width;
	entry->height = height;
	entry->format = dxgiFormat;
	entry->texture = nullptr;
	entry->dirty = false;

	*shared = entry->handle;
	Log("render target %ux%u format %d given handle %p", width, height, format, entry->handle);
	g_surfaces.push_back(std::move(entry));
	return hr;
}

typedef HRESULT(STDMETHODCALLTYPE* CreateDeviceExFn)(IDirect3D9Ex*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, D3DDISPLAYMODEEX*, IDirect3DDevice9Ex**);
static CreateDeviceExFn g_origCreateDeviceEx;

static HRESULT STDMETHODCALLTYPE CreateDeviceExHook(IDirect3D9Ex* This, UINT adapter, D3DDEVTYPE type, HWND window, DWORD flags, D3DPRESENT_PARAMETERS* params, D3DDISPLAYMODEEX* mode, IDirect3DDevice9Ex** device)
{
	HRESULT hr = g_origCreateDeviceEx(This, adapter, type, window, flags, params, mode, device);
	if (SUCCEEDED(hr) && device != nullptr && *device != nullptr)
		PatchVtable(*device, kD3D9DeviceCreateRenderTarget, &CreateRenderTargetHook, &g_origCreateRenderTarget);
	Log("CreateDeviceEx returned 0x%X", hr);
	return hr;
}

typedef HRESULT(WINAPI* Direct3DCreate9ExFn)(UINT, IDirect3D9Ex**);
static Direct3DCreate9ExFn g_origDirect3DCreate9Ex;

static HRESULT WINAPI Direct3DCreate9ExHook(UINT sdkVersion, IDirect3D9Ex** d3d)
{
	HRESULT hr = g_origDirect3DCreate9Ex(sdkVersion, d3d);
	if (SUCCEEDED(hr) && d3d != nullptr && *d3d != nullptr)
		PatchVtable(*d3d, kD3D9ExCreateDeviceEx, &CreateDeviceExHook, &g_origCreateDeviceEx);
	return hr;
}

typedef HRESULT(STDMETHODCALLTYPE* VideoProcessBltFn)(void*, IDirect3DSurface9*, const void*, const void*, UINT, HANDLE*);
static VideoProcessBltFn g_origVideoProcessBlt;

static HRESULT STDMETHODCALLTYPE VideoProcessBltHook(void* This, IDirect3DSurface9* target, const void* params, const void* samples, UINT sampleCount, HANDLE* complete)
{
	HRESULT hr = g_origVideoProcessBlt(This, target, params, samples, sampleCount, complete);

	std::lock_guard<std::mutex> lock(g_lock);

	SharedSurface* entry = FindBySurface(target);
	if (entry == nullptr)
		return hr;

	if (FAILED(hr))
	{
		Log("VideoProcessBlt failed (0x%X)", hr);
		return hr;
	}

	D3DLOCKED_RECT locked;
	HRESULT lockResult = target->LockRect(&locked, nullptr, D3DLOCK_READONLY);
	if (FAILED(lockResult))
	{
		Log("LockRect failed (0x%X)", lockResult);
		return hr;
	}

	const UINT rowBytes = entry->width * 4;
	entry->frame.resize(static_cast<size_t>(rowBytes) * entry->height);
	const uint8_t* source = static_cast<const uint8_t*>(locked.pBits);
	for (UINT row = 0; row < entry->height; row++)
		memcpy(&entry->frame[static_cast<size_t>(row) * rowBytes], source + static_cast<size_t>(row) * locked.Pitch, rowBytes);

	target->UnlockRect();
	entry->dirty = true;
	g_framePending = true;

	if (g_framesCopied < kLoggedFrames)
	{
		g_framesCopied++;
		Log("copied frame %d, first pixel 0x%08X", g_framesCopied, *reinterpret_cast<const uint32_t*>(entry->frame.data()));
	}

	return hr;
}

typedef HRESULT(STDMETHODCALLTYPE* CreateVideoProcessorFn)(void*, REFGUID, const void*, D3DFORMAT, UINT, void**);
static CreateVideoProcessorFn g_origCreateVideoProcessor;

static HRESULT STDMETHODCALLTYPE CreateVideoProcessorHook(void* This, REFGUID device, const void* description, D3DFORMAT format, UINT maxSubStreams, void** processor)
{
	HRESULT hr = g_origCreateVideoProcessor(This, device, description, format, maxSubStreams, processor);
	if (SUCCEEDED(hr) && processor != nullptr && *processor != nullptr)
		PatchVtable(*processor, kVideoProcessorVideoProcessBlt, &VideoProcessBltHook, &g_origVideoProcessBlt);
	Log("CreateVideoProcessor format %d returned 0x%X", format, hr);
	return hr;
}

typedef HRESULT(WINAPI* DXVA2CreateVideoServiceFn)(IDirect3DDevice9*, REFIID, void**);
static DXVA2CreateVideoServiceFn g_origDXVA2CreateVideoService;

static HRESULT WINAPI DXVA2CreateVideoServiceHook(IDirect3DDevice9* device, REFIID riid, void** service)
{
	HRESULT hr = g_origDXVA2CreateVideoService(device, riid, service);
	if (SUCCEEDED(hr) && service != nullptr && *service != nullptr && IsEqualGUID(riid, kProcessorServiceIid))
		PatchVtable(*service, kProcessorServiceCreateVideoProcessor, &CreateVideoProcessorHook, &g_origCreateVideoProcessor);
	return hr;
}

static void HookExport(const wchar_t* module, const char* name, void* hook, void** original)
{
	MH_STATUS status = MH_CreateHookApi(module, name, hook, original);
	Log("hook %s: %s", name, status == MH_OK ? "ok" : "failed");
}

static InitFunction initFunc([]()
{
	if (getenv("TP_D3D9_SHARED_COPY") == nullptr)
		return;

	MH_Initialize();
	HookExport(L"d3d9.dll", "Direct3DCreate9Ex", reinterpret_cast<void*>(Direct3DCreate9ExHook), reinterpret_cast<void**>(&g_origDirect3DCreate9Ex));
	HookExport(L"dxva2.dll", "DXVA2CreateVideoService", reinterpret_cast<void*>(DXVA2CreateVideoServiceHook), reinterpret_cast<void**>(&g_origDXVA2CreateVideoService));
	HookExport(L"d3d11.dll", "D3D11CreateDevice", reinterpret_cast<void*>(D3D11CreateDeviceHook), reinterpret_cast<void**>(&g_origD3D11CreateDevice));
	MH_EnableHook(MH_ALL_HOOKS);
});

#endif
