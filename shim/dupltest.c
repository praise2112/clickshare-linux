/*
 * dupltest.exe - checks the csdup.dll capture path without the ClickShare app.
 * Usage: dupltest.exe <csdup.dll> [frame.bmp]
 * Saves one desktop frame and prints the share of non-black pixels.
 */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <initguid.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <stdio.h>

static int fail(const char *what, HRESULT hr)
{
    printf("dupltest: %s failed (hr %#lx)\n", what, hr);
    return 1;
}

static void write_bmp(const char *path, const BYTE *pixels, UINT pitch, int width, int height)
{
    BITMAPFILEHEADER fh = {0};
    BITMAPINFOHEADER ih = {0};
    FILE *f = fopen(path, "wb");
    if (!f) { printf("dupltest: cannot write %s\n", path); return; }
    fh.bfType = 0x4d42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + width * height * 4;
    ih.biSize = sizeof(ih);
    ih.biWidth = width;
    ih.biHeight = -height;
    ih.biPlanes = 1;
    ih.biBitCount = 32;
    ih.biCompression = BI_RGB;
    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&ih, sizeof(ih), 1, f);
    for (int y = 0; y < height; ++y) fwrite(pixels + y * pitch, 4, width, f);
    fclose(f);
}

int main(int argc, char **argv)
{
    const char *bmp_path = argc > 2 ? argv[2] : "frame.bmp";
    ID3D11Device *device;
    ID3D11DeviceContext *context;
    IDXGIDevice *dxgi_device;
    IDXGIAdapter *adapter;
    IDXGIOutput *output;
    IDXGIOutput1 *output1;
    IDXGIOutputDuplication *duplication;
    DXGI_OUTDUPL_FRAME_INFO info;
    IDXGIResource *resource;
    ID3D11Texture2D *frame, *staging;
    D3D11_TEXTURE2D_DESC desc;
    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr;

    if (argc < 2) { printf("usage: dupltest.exe <csdup.dll> [frame.bmp]\n"); return 2; }
    if (!LoadLibraryA(argv[1])) { printf("dupltest: cannot load %s\n", argv[1]); return 1; }
    /* csdup.dll patches the vtable from a thread started in DllMain. */
    Sleep(1000);

    if (FAILED(hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0,
            D3D11_SDK_VERSION, &device, NULL, &context)))
        return fail("D3D11CreateDevice", hr);
    if (FAILED(hr = ID3D11Device_QueryInterface(device, &IID_IDXGIDevice, (void **)&dxgi_device)))
        return fail("QueryInterface IDXGIDevice", hr);
    if (FAILED(hr = IDXGIDevice_GetAdapter(dxgi_device, &adapter))) return fail("GetAdapter", hr);
    if (FAILED(hr = IDXGIAdapter_EnumOutputs(adapter, 0, &output))) return fail("EnumOutputs", hr);
    if (FAILED(hr = IDXGIOutput_QueryInterface(output, &IID_IDXGIOutput1, (void **)&output1)))
        return fail("QueryInterface IDXGIOutput1", hr);
    if (FAILED(hr = IDXGIOutput1_DuplicateOutput(output1, (IUnknown *)device, &duplication)))
        return fail("DuplicateOutput", hr);
    if (FAILED(hr = IDXGIOutputDuplication_AcquireNextFrame(duplication, 1000, &info, &resource)))
        return fail("AcquireNextFrame", hr);
    if (FAILED(hr = IDXGIResource_QueryInterface(resource, &IID_ID3D11Texture2D, (void **)&frame)))
        return fail("QueryInterface ID3D11Texture2D", hr);

    ID3D11Texture2D_GetDesc(frame, &desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    if (FAILED(hr = ID3D11Device_CreateTexture2D(device, &desc, NULL, &staging)))
        return fail("CreateTexture2D staging", hr);
    ID3D11DeviceContext_CopyResource(context, (ID3D11Resource *)staging, (ID3D11Resource *)frame);
    if (FAILED(hr = ID3D11DeviceContext_Map(context, (ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &mapped)))
        return fail("Map", hr);

    {
        const BYTE *pixels = mapped.pData;
        long long lit = 0;
        for (UINT y = 0; y < desc.Height; ++y)
        {
            const DWORD *row = (const DWORD *)(pixels + y * mapped.RowPitch);
            for (UINT x = 0; x < desc.Width; ++x) lit += (row[x] & 0x00ffffff) != 0;
        }
        write_bmp(bmp_path, pixels, mapped.RowPitch, desc.Width, desc.Height);
        printf("dupltest: frame %ux%u, non-black pixels %.1f%%, saved %s\n", desc.Width, desc.Height,
                100.0 * lit / ((double)desc.Width * desc.Height), bmp_path);
    }
    return 0;
}
