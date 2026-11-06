/*
 * csdup.dll - adds IDXGIOutput1::DuplicateOutput / IDXGIOutput5::DuplicateOutput1
 * to Wine's dxgi (which only has stubs) so that the ClickShare app can capture
 * the desktop. Frames come from GDI BitBlt on X11, or on Wayland from the portal
 * frames that helper/clickshare-capture.py writes, and are uploaded into a D3D11
 * texture on the caller's device. It also fixes the app's screen thumbnail and
 * window list under Wayland.
 *
 * The DLL patches the (shared) IDXGIOutput vtable of Wine's dxgi when loaded.
 */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <initguid.h>
#include <d3d11.h>
#include <dxgi1_5.h>
#include <stdio.h>
#include <string.h>
#include "pacing.h"

static HRESULT (STDMETHODCALLTYPE *orig_output_GetDesc)(IDXGIOutput1 *, DXGI_OUTPUT_DESC *);

static void logmsg(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    SYSTEMTIME now;
    int len;
    GetLocalTime(&now);
    len = snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%03d ", now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);
    va_start(ap, fmt);
    vsnprintf(buf + len, sizeof(buf) - len, fmt, ap);
    va_end(ap);
    OutputDebugStringA(buf);
    FILE *f = fopen("C:\\csdup.log", "a");
    if (f) { fputs(buf, f); fputc('\n', f); fclose(f); }
}

/* ---------------------------------------------------------------- */

typedef struct dup
{
    IDXGIOutputDuplication iface;
    LONG ref;
    IDXGIOutput1 *output;
    ID3D11Device *device;
    ID3D11DeviceContext *context;
    ID3D11Texture2D *texture;
    RECT rect;
    int width, height;
    HDC screen_dc, mem_dc;
    HBITMAP dib;
    void *bits;
    LARGE_INTEGER last, freq;
    BOOL frame_held;
    HANDLE frames_mapping;
    const BYTE *frames;
    SIZE_T frames_size;
    DWORD frame_width, frame_height;
    LONG frame_sequence;
    LONGLONG min_frame_interval, next_frame;
    BOOL portal;
} dup;

/* Written by helper/clickshare-capture.py; sequence is odd while a frame is being written. */
typedef struct frame_header
{
    DWORD magic, width, height, stride;
    volatile LONG sequence;
} frame_header;

#define FRAME_MAGIC 0x52465343
#define FRAME_HEADER_SIZE 64

/* CLICKSHARE_FRAMES plus a suffix: the helper's frame file or one of its request files. */
static BOOL frames_path(WCHAR *path, const WCHAR *suffix)
{
    DWORD length = GetEnvironmentVariableW(L"CLICKSHARE_FRAMES", path, MAX_PATH);
    if (!length || length + lstrlenW(suffix) >= MAX_PATH) return FALSE;
    lstrcatW(path, suffix);
    return TRUE;
}

/* Maps one of the helper's frame files; NULL until the helper has written it. */
static const BYTE *map_frames(const WCHAR *suffix, HANDLE *mapping, SIZE_T *size)
{
    WCHAR path[MAX_PATH];
    MEMORY_BASIC_INFORMATION info;
    HANDLE file;
    const BYTE *view;

    if (!frames_path(path, suffix)) return NULL;
    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            NULL, OPEN_EXISTING, 0, NULL);
    if (file == INVALID_HANDLE_VALUE) return NULL;
    *mapping = CreateFileMappingW(file, NULL, PAGE_READONLY, 0, 0, NULL);
    CloseHandle(file);
    if (!*mapping) return NULL;
    view = MapViewOfFile(*mapping, FILE_MAP_READ, 0, 0, 0);
    if (!view || ((const frame_header *)view)->magic != FRAME_MAGIC)
    {
        if (view) UnmapViewOfFile(view);
        CloseHandle(*mapping);
        *mapping = NULL;
        return NULL;
    }
    VirtualQuery(view, &info, sizeof(info));
    *size = info.RegionSize;
    return view;
}

/* The helper creates the frame file once the user has picked a screen or window. */
static const BYTE *open_frames(HANDLE *mapping, SIZE_T *size)
{
    const BYTE *view = map_frames(L"", mapping, size);
    if (view) logmsg("csdup: reading portal frames");
    return view;
}

static void write_request(const WCHAR *suffix, const char *text)
{
    WCHAR path[MAX_PATH];
    HANDLE file;
    DWORD written;

    if (!frames_path(path, suffix)) return;
    file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, CREATE_ALWAYS, 0, NULL);
    if (file == INVALID_HANDLE_VALUE) return;
    WriteFile(file, text, strlen(text), &written, NULL);
    CloseHandle(file);
}

/* Tells the helper to start a capture; the output size sets the size of a virtual screen. */
static void request_capture(int width, int height)
{
    char size[32];
    snprintf(size, sizeof(size), "%dx%d", width, height);
    write_request(L".request", size);
    logmsg("csdup: capture requested");
}

/* Live portal duplications; the lock keeps the count and the request file in step. */
static LONG portal_dups;
static SRWLOCK portal_lock = SRWLOCK_INIT;

static void end_capture(void)
{
    WCHAR path[MAX_PATH];
    if (frames_path(path, L".request")) DeleteFileW(path);
}

static void reopen_frames(dup *d)
{
    UnmapViewOfFile(d->frames);
    CloseHandle(d->frames_mapping);
    d->frames_mapping = NULL;
    d->frame_width = d->frame_height = 0;
    d->frames = open_frames(&d->frames_mapping, &d->frames_size);
}

/* Copies a frame into the output buffer, centred; a frame larger than the output (a bigger
 * monitor, or HiDPI scaling) is shrunk to fit, keeping its aspect ratio. */
static void copy_fitted(dup *d, const BYTE *src, DWORD width, DWORD height)
{
    DWORD fit_width = width, fit_height = height;
    BYTE *dst;

    if (width > (DWORD)d->width || height > (DWORD)d->height)
    {
        if ((ULONGLONG)width * d->height > (ULONGLONG)height * d->width)
        {
            fit_width = d->width;
            fit_height = (DWORD)((ULONGLONG)height * d->width / width);
        }
        else
        {
            fit_height = d->height;
            fit_width = (DWORD)((ULONGLONG)width * d->height / height);
        }
    }
    dst = (BYTE *)d->bits + ((size_t)(d->height - fit_height) / 2 * d->width + (d->width - fit_width) / 2) * 4;
    if (fit_width == width && fit_height == height)
    {
        if (width == (DWORD)d->width)
            memcpy(dst, src, (size_t)width * height * 4);
        else
            for (DWORD y = 0; y < height; ++y)
                memcpy(dst + (size_t)y * d->width * 4, src + (size_t)y * width * 4, (size_t)width * 4);
        return;
    }
    for (DWORD y = 0; y < fit_height; ++y)
    {
        const DWORD *row = (const DWORD *)(src + (size_t)(y * height / fit_height) * width * 4);
        DWORD *out = (DWORD *)(dst + (size_t)y * d->width * 4);
        for (DWORD x = 0; x < fit_width; ++x) out[x] = row[(ULONGLONG)x * width / fit_width];
    }
}

/* Returns TRUE once a new frame is in the texture. A window smaller than the output is
 * centred on black; the helper's file grows when the window does. After three torn reads
 * the last copy is used anyway, so a fast-changing screen still gets frames. */
static BOOL upload_portal_frame(dup *d)
{
    const frame_header *header;
    LONG sequence = 0;

    for (int attempt = 0; attempt < 3; ++attempt)
    {
        DWORD width, height;

        if (!d->frames && !(d->frames = open_frames(&d->frames_mapping, &d->frames_size))) return FALSE;
        header = (const frame_header *)d->frames;
        sequence = header->sequence;
        if (sequence == d->frame_sequence) return FALSE;
        if (sequence & 1) { Sleep(1); continue; }
        width = header->width;
        height = header->height;
        if (FRAME_HEADER_SIZE + (SIZE_T)width * height * 4 > d->frames_size)
        {
            reopen_frames(d);
            continue;
        }
        if (width != d->frame_width || height != d->frame_height)
        {
            DWORD *p = d->bits, *end = p + d->width * d->height;
            for (; p < end; ++p) *p = 0xff000000;
            d->frame_width = width;
            d->frame_height = height;
            logmsg("csdup: portal frames %lux%lu into %dx%d", width, height, d->width, d->height);
        }
        MemoryBarrier();
        copy_fitted(d, d->frames + FRAME_HEADER_SIZE, width, height);
        MemoryBarrier();
        if (header->sequence == sequence) break;
    }
    if ((sequence & 1) || !d->frame_width) return FALSE;
    d->frame_sequence = sequence;
    ID3D11DeviceContext_UpdateSubresource(d->context, (ID3D11Resource *)d->texture, 0, NULL,
            d->bits, d->width * 4, 0);
    return TRUE;
}

static inline dup *impl(IDXGIOutputDuplication *iface)
{
    return CONTAINING_RECORD(iface, dup, iface);
}

static HRESULT STDMETHODCALLTYPE d_QueryInterface(IDXGIOutputDuplication *iface, REFIID riid, void **out)
{
    if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IDXGIObject)
            || IsEqualGUID(riid, &IID_IDXGIOutputDuplication))
    {
        IDXGIOutputDuplication_AddRef(iface);
        *out = iface;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE d_AddRef(IDXGIOutputDuplication *iface)
{
    return InterlockedIncrement(&impl(iface)->ref);
}

static ULONG STDMETHODCALLTYPE d_Release(IDXGIOutputDuplication *iface)
{
    dup *d = impl(iface);
    ULONG ref = InterlockedDecrement(&d->ref);
    if (!ref)
    {
        if (d->texture) ID3D11Texture2D_Release(d->texture);
        if (d->context) ID3D11DeviceContext_Release(d->context);
        if (d->device) ID3D11Device_Release(d->device);
        if (d->output) IDXGIOutput1_Release(d->output);
        if (d->dib) DeleteObject(d->dib);
        if (d->mem_dc) DeleteDC(d->mem_dc);
        if (d->screen_dc) ReleaseDC(NULL, d->screen_dc);
        if (d->frames) UnmapViewOfFile(d->frames);
        if (d->frames_mapping) CloseHandle(d->frames_mapping);
        if (d->portal)
        {
            AcquireSRWLockExclusive(&portal_lock);
            if (!--portal_dups) end_capture();
            ReleaseSRWLockExclusive(&portal_lock);
        }
        HeapFree(GetProcessHeap(), 0, d);
        logmsg("csdup: duplication released");
    }
    return ref;
}

static HRESULT STDMETHODCALLTYPE d_SetPrivateData(IDXGIOutputDuplication *iface, REFGUID g, UINT s, const void *p)
{ return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE d_SetPrivateDataInterface(IDXGIOutputDuplication *iface, REFGUID g, const IUnknown *p)
{ return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE d_GetPrivateData(IDXGIOutputDuplication *iface, REFGUID g, UINT *s, void *p)
{ return E_NOTIMPL; }

static HRESULT STDMETHODCALLTYPE d_GetParent(IDXGIOutputDuplication *iface, REFIID riid, void **parent)
{
    return IDXGIOutput1_QueryInterface(impl(iface)->output, riid, parent);
}

static void STDMETHODCALLTYPE d_GetDesc(IDXGIOutputDuplication *iface, DXGI_OUTDUPL_DESC *desc)
{
    dup *d = impl(iface);
    memset(desc, 0, sizeof(*desc));
    desc->ModeDesc.Width = d->width;
    desc->ModeDesc.Height = d->height;
    desc->ModeDesc.RefreshRate.Numerator = 60;
    desc->ModeDesc.RefreshRate.Denominator = 1;
    desc->ModeDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc->Rotation = DXGI_MODE_ROTATION_IDENTITY;
    desc->DesktopImageInSystemMemory = FALSE;
}

static void grab(dup *d)
{
    CURSORINFO ci = { sizeof(ci) };

    BitBlt(d->mem_dc, 0, 0, d->width, d->height, d->screen_dc, d->rect.left, d->rect.top, SRCCOPY | CAPTUREBLT);
    if (GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING))
    {
        ICONINFO ii;
        if (GetIconInfo(ci.hCursor, &ii))
        {
            DrawIconEx(d->mem_dc, ci.ptScreenPos.x - d->rect.left - (int)ii.xHotspot,
                    ci.ptScreenPos.y - d->rect.top - (int)ii.yHotspot, ci.hCursor, 0, 0, 0, NULL, DI_NORMAL);
            if (ii.hbmMask) DeleteObject(ii.hbmMask);
            if (ii.hbmColor) DeleteObject(ii.hbmColor);
        }
    }
    GdiFlush();
    /* GDI leaves alpha at 0; make the image opaque. */
    {
        DWORD *p = d->bits, *end = p + d->width * d->height;
        for (; p < end; ++p) *p |= 0xff000000;
    }
    ID3D11DeviceContext_UpdateSubresource(d->context, (ID3D11Resource *)d->texture, 0, NULL,
            d->bits, d->width * 4, 0);
}

static HRESULT STDMETHODCALLTYPE d_AcquireNextFrame(IDXGIOutputDuplication *iface, UINT timeout,
        DXGI_OUTDUPL_FRAME_INFO *info, IDXGIResource **resource)
{
    dup *d = impl(iface);
    const LONGLONG interval = d->freq.QuadPart / 30; /* ~30 fps */
    LARGE_INTEGER now;
    LONGLONG wait_ms;

    QueryPerformanceCounter(&now);
    if (d->portal)
    {
        const LONGLONG deadline = now.QuadPart + (LONGLONG)timeout * d->freq.QuadPart / 1000;
        wait_ms = (d->next_frame - now.QuadPart) * 1000 / d->freq.QuadPart;
        if (wait_ms > 0)
        {
            if ((UINT)wait_ms > timeout)
            {
                Sleep(timeout);
                return DXGI_ERROR_WAIT_TIMEOUT;
            }
            Sleep((DWORD)wait_ms);
        }
        /* Like real duplication: hand over each new frame at once, time out while nothing changes. */
        while (!upload_portal_frame(d))
        {
            QueryPerformanceCounter(&now);
            if (now.QuadPart >= deadline) return DXGI_ERROR_WAIT_TIMEOUT;
            Sleep(1);
        }
        QueryPerformanceCounter(&now);
        d->next_frame = next_frame_due(d->next_frame, now.QuadPart, d->min_frame_interval);
    }
    else
    {
        wait_ms = (d->last.QuadPart + interval - now.QuadPart) * 1000 / d->freq.QuadPart;
        if (wait_ms > 0)
        {
            if ((UINT)wait_ms > timeout)
            {
                Sleep(timeout);
                return DXGI_ERROR_WAIT_TIMEOUT;
            }
            Sleep((DWORD)wait_ms);
            QueryPerformanceCounter(&now);
        }
        d->last = now;
        grab(d);
    }

    memset(info, 0, sizeof(*info));
    info->LastPresentTime = now;
    info->AccumulatedFrames = 1;
    info->TotalMetadataBufferSize = sizeof(RECT);
    info->PointerPosition.Visible = FALSE;

    d->frame_held = TRUE;
    return ID3D11Texture2D_QueryInterface(d->texture, &IID_IDXGIResource, (void **)resource);
}

static HRESULT STDMETHODCALLTYPE d_GetFrameDirtyRects(IDXGIOutputDuplication *iface, UINT size,
        RECT *rects, UINT *required)
{
    dup *d = impl(iface);
    *required = sizeof(RECT);
    if (size < sizeof(RECT)) return DXGI_ERROR_MORE_DATA;
    SetRect(rects, 0, 0, d->width, d->height);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE d_GetFrameMoveRects(IDXGIOutputDuplication *iface, UINT size,
        DXGI_OUTDUPL_MOVE_RECT *rects, UINT *required)
{
    *required = 0;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE d_GetFramePointerShape(IDXGIOutputDuplication *iface, UINT size,
        void *buffer, UINT *required, DXGI_OUTDUPL_POINTER_SHAPE_INFO *info)
{
    *required = 0;
    if (info) memset(info, 0, sizeof(*info));
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE d_MapDesktopSurface(IDXGIOutputDuplication *iface, DXGI_MAPPED_RECT *rect)
{ return DXGI_ERROR_UNSUPPORTED; }
static HRESULT STDMETHODCALLTYPE d_UnMapDesktopSurface(IDXGIOutputDuplication *iface)
{ return DXGI_ERROR_INVALID_CALL; }

static HRESULT STDMETHODCALLTYPE d_ReleaseFrame(IDXGIOutputDuplication *iface)
{
    impl(iface)->frame_held = FALSE;
    return S_OK;
}

static IDXGIOutputDuplicationVtbl dup_vtbl =
{
    d_QueryInterface, d_AddRef, d_Release,
    d_SetPrivateData, d_SetPrivateDataInterface, d_GetPrivateData, d_GetParent,
    d_GetDesc, d_AcquireNextFrame, d_GetFrameDirtyRects, d_GetFrameMoveRects,
    d_GetFramePointerShape, d_MapDesktopSurface, d_UnMapDesktopSurface, d_ReleaseFrame,
};

static HRESULT create_dup(IDXGIOutput1 *output, IUnknown *device, IDXGIOutputDuplication **out)
{
    DXGI_OUTPUT_DESC odesc;
    D3D11_TEXTURE2D_DESC tdesc = {0};
    BITMAPINFO bmi = {0};
    HRESULT hr;
    dup *d;

    *out = NULL;
    if (!device) return E_INVALIDARG;
    if (FAILED(hr = orig_output_GetDesc(output, &odesc))) return hr;

    d = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*d));
    d->iface.lpVtbl = &dup_vtbl;
    d->ref = 1;
    d->rect = odesc.DesktopCoordinates;
    d->width = d->rect.right - d->rect.left;
    d->height = d->rect.bottom - d->rect.top;
    if (GetEnvironmentVariableW(L"CLICKSHARE_FRAMES", NULL, 0))
    {
        d->portal = TRUE;
        AcquireSRWLockExclusive(&portal_lock);
        ++portal_dups;
        request_capture(d->width, d->height);
        ReleaseSRWLockExclusive(&portal_lock);
    }
    IDXGIOutput1_AddRef(output);
    d->output = output;
    QueryPerformanceFrequency(&d->freq);
    {
        WCHAR max_fps[16];
        if (GetEnvironmentVariableW(L"CLICKSHARE_MAX_FPS", max_fps, 16) && _wtoi(max_fps) > 0)
            d->min_frame_interval = d->freq.QuadPart / _wtoi(max_fps);
    }

    if (FAILED(hr = IUnknown_QueryInterface(device, &IID_ID3D11Device, (void **)&d->device)))
    {
        logmsg("csdup: device is not a D3D11 device (hr %#lx)", hr);
        d_Release(&d->iface);
        return hr;
    }
    ID3D11Device_GetImmediateContext(d->device, &d->context);

    tdesc.Width = d->width;
    tdesc.Height = d->height;
    tdesc.MipLevels = 1;
    tdesc.ArraySize = 1;
    tdesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    tdesc.SampleDesc.Count = 1;
    tdesc.Usage = D3D11_USAGE_DEFAULT;
    tdesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    if (FAILED(hr = ID3D11Device_CreateTexture2D(d->device, &tdesc, NULL, &d->texture)))
    {
        logmsg("csdup: CreateTexture2D failed (hr %#lx)", hr);
        d_Release(&d->iface);
        return hr;
    }

    d->screen_dc = GetDC(NULL);
    d->mem_dc = CreateCompatibleDC(d->screen_dc);
    bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
    bmi.bmiHeader.biWidth = d->width;
    bmi.bmiHeader.biHeight = -d->height; /* top-down */
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    d->dib = CreateDIBSection(d->mem_dc, &bmi, DIB_RGB_COLORS, &d->bits, NULL, 0);
    SelectObject(d->mem_dc, d->dib);

    logmsg("csdup: duplication created %dx%d at %ld,%ld", d->width, d->height, d->rect.left, d->rect.top);
    *out = &d->iface;
    return S_OK;
}

/* ---------------------------------------------------------------- */

static HRESULT STDMETHODCALLTYPE hook_DuplicateOutput(IDXGIOutput1 *iface, IUnknown *device,
        IDXGIOutputDuplication **dup)
{
    logmsg("csdup: DuplicateOutput called");
    return create_dup(iface, device, dup);
}

static HRESULT STDMETHODCALLTYPE hook_DuplicateOutput1(IDXGIOutput5 *iface, IUnknown *device, UINT flags,
        UINT format_count, const DXGI_FORMAT *formats, IDXGIOutputDuplication **dup)
{
    logmsg("csdup: DuplicateOutput1 called");
    return create_dup((IDXGIOutput1 *)iface, device, dup);
}

static void patch(void **slot, void *fn)
{
    DWORD old;
    VirtualProtect(slot, sizeof(void *), PAGE_EXECUTE_READWRITE, &old);
    *slot = fn;
    VirtualProtect(slot, sizeof(void *), old, &old);
}

static DWORD WINAPI install(void *arg)
{
    IDXGIFactory1 *factory;
    IDXGIAdapter1 *adapter;
    IDXGIOutput *output;
    IDXGIOutput1 *output1;
    IDXGIOutput5 *output5;
    HRESULT hr;

    if (FAILED(hr = CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&factory)))
    {
        logmsg("csdup: CreateDXGIFactory1 failed %#lx", hr);
        return 1;
    }
    if (FAILED(hr = IDXGIFactory1_EnumAdapters1(factory, 0, &adapter)))
    {
        logmsg("csdup: EnumAdapters1 failed %#lx", hr);
        IDXGIFactory1_Release(factory);
        return 1;
    }
    if (FAILED(hr = IDXGIAdapter1_EnumOutputs(adapter, 0, &output)))
    {
        logmsg("csdup: EnumOutputs failed %#lx", hr);
        IDXGIAdapter1_Release(adapter);
        IDXGIFactory1_Release(factory);
        return 1;
    }
    if (SUCCEEDED(IDXGIOutput_QueryInterface(output, &IID_IDXGIOutput1, (void **)&output1)))
    {
        orig_output_GetDesc = output1->lpVtbl->GetDesc;
        patch((void **)&output1->lpVtbl->DuplicateOutput, hook_DuplicateOutput);
        IDXGIOutput1_Release(output1);
        logmsg("csdup: patched DuplicateOutput");
    }
    if (SUCCEEDED(IDXGIOutput_QueryInterface(output, &IID_IDXGIOutput5, (void **)&output5)))
    {
        patch((void **)&output5->lpVtbl->DuplicateOutput1, hook_DuplicateOutput1);
        IDXGIOutput5_Release(output5);
        logmsg("csdup: patched DuplicateOutput1");
    }
    IDXGIOutput_Release(output);
    IDXGIAdapter1_Release(adapter);
    IDXGIFactory1_Release(factory);
    return 0;
}

static FARPROC (WINAPI *orig_GetProcAddress)(HMODULE, LPCSTR);

/* Touch from the room screen is dropped: Wine has no touch injection. */
static BOOL WINAPI stub_InitializeTouchInjection(UINT32 max_count, DWORD mode)
{
    logmsg("csdup: InitializeTouchInjection(%u) ignored", max_count);
    return TRUE;
}

static BOOL WINAPI stub_InjectTouchInput(UINT32 count, const void *contacts)
{
    return TRUE;
}

static FARPROC WINAPI hook_GetProcAddress(HMODULE module, LPCSTR name)
{
    FARPROC proc = orig_GetProcAddress(module, name);
    if (proc || !HIWORD(name)) return proc;
    if (!strcmp(name, "InitializeTouchInjection")) return (FARPROC)stub_InitializeTouchInjection;
    if (!strcmp(name, "InjectTouchInput")) return (FARPROC)stub_InjectTouchInput;
    return NULL;
}

/* Returns the import address slot for name in a pair of name/address thunk tables, or NULL. */
static void **find_import_slot(BYTE *base, IMAGE_THUNK_DATA *names, IMAGE_THUNK_DATA *slots, const char *name)
{
    for (; names->u1.AddressOfData; ++names, ++slots)
    {
        if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
        if (!strcmp((char *)((IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData))->Name, name))
            return (void **)&slots->u1.Function;
    }
    return NULL;
}

static IMAGE_DATA_DIRECTORY app_directory(BYTE *base, int index)
{
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    return nt->OptionalHeader.DataDirectory[index];
}

/* The app's delay-load helper resolves imports through its own GetProcAddress import. */
static void hook_app_get_proc_address(void)
{
    BYTE *base = (BYTE *)GetModuleHandleW(NULL);
    IMAGE_DATA_DIRECTORY dir = app_directory(base, IMAGE_DIRECTORY_ENTRY_IMPORT);

    for (IMAGE_IMPORT_DESCRIPTOR *desc = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); desc->Name; desc++)
    {
        void **slot;
        if (lstrcmpiA((char *)base + desc->Name, "kernel32.dll") || !desc->OriginalFirstThunk) continue;
        slot = find_import_slot(base, (IMAGE_THUNK_DATA *)(base + desc->OriginalFirstThunk),
                (IMAGE_THUNK_DATA *)(base + desc->FirstThunk), "GetProcAddress");
        if (!slot) continue;
        orig_GetProcAddress = *slot;
        patch(slot, hook_GetProcAddress);
        logmsg("csdup: hooked GetProcAddress for touch injection");
        return;
    }
    logmsg("csdup: GetProcAddress import not found");
}

typedef struct
{
    DWORD attributes, dll_name, module_handle, iat, int_table, bound_iat, unload_iat, timestamp;
} delay_descriptor;

/* Points the app's delay-import slot for dll!name at hook, bound or not. */
static void hook_delay_import(const char *dll, const char *name, void *hook, void **orig)
{
    BYTE *base = (BYTE *)GetModuleHandleW(NULL);
    IMAGE_DATA_DIRECTORY dir = app_directory(base, IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT);

    for (delay_descriptor *d = (delay_descriptor *)(base + dir.VirtualAddress); dir.VirtualAddress && d->dll_name; ++d)
    {
        void **slot;
        if (lstrcmpiA((char *)base + d->dll_name, dll)) continue;
        slot = find_import_slot(base, (IMAGE_THUNK_DATA *)(base + d->int_table), (IMAGE_THUNK_DATA *)(base + d->iat), name);
        if (!slot) continue;
        *orig = (void *)GetProcAddress(LoadLibraryA(dll), name);
        patch(slot, hook);
        logmsg("csdup: hooked %s", name);
        return;
    }
    logmsg("csdup: delay import %s not found", name);
}

/* Draws the helper's latest monitor snapshot; returns FALSE if there is none. */
static BOOL draw_snapshot(HDC dst, int x, int y, int width, int height, int src_x, int src_y)
{
    HANDLE mapping;
    SIZE_T size;
    const BYTE *view = map_frames(L".thumbnail", &mapping, &size);
    const frame_header *header = (const frame_header *)view;
    BITMAPINFO info = {{sizeof(BITMAPINFOHEADER)}};
    int screen_width = GetSystemMetrics(SM_CXSCREEN), screen_height = GetSystemMetrics(SM_CYSCREEN);
    BOOL drawn = FALSE;

    if (!view) return FALSE;
    if (!(header->sequence & 1) && header->width && header->height
            && FRAME_HEADER_SIZE + (SIZE_T)header->width * header->height * 4 <= size)
    {
        info.bmiHeader.biWidth = header->width;
        info.bmiHeader.biHeight = -(LONG)header->height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        SetStretchBltMode(dst, HALFTONE);
        drawn = StretchDIBits(dst, x, y, width, height,
                MulDiv(src_x, header->width, screen_width), MulDiv(src_y, header->height, screen_height),
                MulDiv(width, header->width, screen_width), MulDiv(height, header->height, screen_height),
                view + FRAME_HEADER_SIZE, &info, DIB_RGB_COLORS, SRCCOPY) != 0;
    }
    UnmapViewOfFile(view);
    CloseHandle(mapping);
    return drawn;
}

static BOOL (WINAPI *orig_BitBlt)(HDC, int, int, int, int, HDC, int, int, DWORD);

/* Under Wayland the screen DC is black, so screen thumbnails come from a portal snapshot.
 * A cached snapshot is drawn at once and a fresh one requested for the next thumbnail. */
static BOOL WINAPI hook_BitBlt(HDC dst, int x, int y, int width, int height, HDC src, int src_x, int src_y, DWORD rop)
{
    WCHAR request[MAX_PATH];
    BOOL drawn;

    if (!src || WindowFromDC(src) != GetDesktopWindow())
        return orig_BitBlt(dst, x, y, width, height, src, src_x, src_y, rop);
    drawn = draw_snapshot(dst, x, y, width, height, src_x, src_y);
    write_request(L".thumbnail.request", "");
    if (!drawn && frames_path(request, L".thumbnail.request"))
        for (int waited = 0; !drawn && waited < 1500; waited += 50)
        {
            /* The helper deletes the request after a snapshot, or at once if the user refused. */
            BOOL pending = GetFileAttributesW(request) != INVALID_FILE_ATTRIBUTES;
            Sleep(50);
            drawn = draw_snapshot(dst, x, y, width, height, src_x, src_y);
            if (!pending) break;
        }
    return drawn || orig_BitBlt(dst, x, y, width, height, src, src_x, src_y, rop);
}

typedef struct
{
    WNDENUMPROC callback;
    LPARAM param;
} window_filter;

static BOOL (WINAPI *orig_EnumChildWindows)(HWND, WNDENUMPROC, LPARAM);

static BOOL CALLBACK pass_own_window(HWND hwnd, LPARAM filter_param)
{
    window_filter *filter = (window_filter *)filter_param;
    DWORD pid;
    GetWindowThreadProcessId(hwnd, &pid);
    return pid != GetCurrentProcessId() || filter->callback(hwnd, filter->param);
}

/* The app lists top-level windows as children of the desktop. Other processes' windows under
 * Wine are Wine's own (explorer), never worth sharing; GNOME's picker offers the real ones. */
static BOOL WINAPI hook_EnumChildWindows(HWND parent, WNDENUMPROC callback, LPARAM param)
{
    window_filter filter = {callback, param};
    if (parent && parent != GetDesktopWindow()) return orig_EnumChildWindows(parent, callback, param);
    return orig_EnumChildWindows(parent, pass_own_window, (LPARAM)&filter);
}

static BOOL CALLBACK hide_transparent_window(HWND hwnd, LPARAM hidden_list)
{
    HWND *hidden = (HWND *)hidden_list;
    DWORD pid, flags = 0;
    BYTE alpha = 255;
    COLORREF key;
    int slot = -1;

    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId() || !(GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_LAYERED)) return TRUE;
    if (!GetLayeredWindowAttributes(hwnd, &key, &alpha, &flags) || !(flags & LWA_ALPHA)) return TRUE;
    for (int i = 0; i < 8; ++i) if (hidden[i] == hwnd) slot = i;
    if (alpha == 0 && slot < 0 && IsWindowVisible(hwnd))
    {
        for (int i = 0; i < 8 && slot < 0; ++i) if (!hidden[i]) hidden[slot = i] = hwnd;
        if (slot >= 0) ShowWindowAsync(hwnd, SW_HIDE);
    }
    else if (alpha > 0 && slot >= 0)
    {
        hidden[slot] = NULL;
        ShowWindowAsync(hwnd, SW_SHOWNA);
    }
    return TRUE;
}

/* Windows ignores a fully transparent layered window, but under XWayland Wine maps it as an
 * invisible window that covers others and takes their clicks. Keep such windows hidden. */
static DWORD WINAPI hide_transparent_windows(void *arg)
{
    HWND hidden[8] = {0};
    for (;;)
    {
        for (int i = 0; i < 8; ++i) if (hidden[i] && !IsWindow(hidden[i])) hidden[i] = NULL;
        EnumWindows(hide_transparent_window, (LPARAM)hidden);
        Sleep(500);
    }
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, void *reserved)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        HANDLE t;
        DisableThreadLibraryCalls(inst);
        hook_app_get_proc_address();
        /* Pin ourselves: the vtable will point into this module. */
        {
            HMODULE self;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                    (LPCWSTR)DllMain, &self);
        }
        if ((t = CreateThread(NULL, 0, install, NULL, 0, NULL))) CloseHandle(t);
        if (GetEnvironmentVariableW(L"CLICKSHARE_FRAMES", NULL, 0))
        {
            hook_delay_import("GDI32.dll", "BitBlt", hook_BitBlt, (void **)&orig_BitBlt);
            hook_delay_import("USER32.dll", "EnumChildWindows", hook_EnumChildWindows, (void **)&orig_EnumChildWindows);
            if ((t = CreateThread(NULL, 0, hide_transparent_windows, NULL, 0, NULL))) CloseHandle(t);
        }
    }
    return TRUE;
}
