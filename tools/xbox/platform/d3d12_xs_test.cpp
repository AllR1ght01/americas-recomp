#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <cstdio>
#include <cstring>

typedef HRESULT (WINAPI* PcCreateDeviceFn)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
typedef HRESULT (WINAPI* XboxCreateDeviceFn)(void*, const void*, REFIID, void**);

static const D3D_FEATURE_LEVEL kLevels[] = {
    D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0
};

static void AdapterName(LUID luid, char* out, size_t size) {
    snprintf(out, size, "(unknown)");
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || !factory) return;
    IDXGIAdapter1* adapter = nullptr;
    IDXGIFactory4* factory4 = nullptr;
    if (SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(&factory4))) && factory4) {
        factory4->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter));
        factory4->Release();
    }
    if (!adapter && factory->EnumAdapters1(0, &adapter) != S_OK) adapter = nullptr;
    if (adapter) {
        DXGI_ADAPTER_DESC1 desc = {};
        if (SUCCEEDED(adapter->GetDesc1(&desc)))
            WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, out, (int)size, nullptr, nullptr);
        adapter->Release();
    }
    factory->Release();
}

static bool Check(HRESULT hr, const char* what) {
    if (SUCCEEDED(hr)) return true;
    printf("%s: hr 0x%08lx\n", what, (unsigned long)hr);
    return false;
}

static int Fail(const char* step) {
    printf("failed at: %s\n", step);
    printf("RESULT: FAIL\n");
    return 1;
}

static int Skip(const char* reason) {
    printf("skipped: %s\n", reason);
    printf("RESULT: SKIP\n");
    return 0;
}

int main() {
    HMODULE pc = LoadLibraryW(L"d3d12.dll");
    if (!pc) return Skip("d3d12.dll is not available");
    auto pcCreate = (PcCreateDeviceFn)GetProcAddress(pc, "D3D12CreateDevice");
    if (!pcCreate) return Skip("d3d12.dll has no D3D12CreateDevice");

    D3D_FEATURE_LEVEL level = (D3D_FEATURE_LEVEL)0;
    ID3D12Device* probe = nullptr;
    HRESULT last = E_FAIL;
    for (D3D_FEATURE_LEVEL candidate : kLevels) {
        probe = nullptr;
        last = pcCreate(nullptr, candidate, __uuidof(ID3D12Device), (void**)&probe);
        if (SUCCEEDED(last) && probe) {
            level = candidate;
            break;
        }
    }
    if (!level || !probe) {
        char reason[128];
        snprintf(reason, sizeof(reason), "D3D12CreateDevice did not return a device (last hr 0x%08lx)", (unsigned long)last);
        return Skip(reason);
    }
    char adapter[256];
    AdapterName(probe->GetAdapterLuid(), adapter, sizeof(adapter));
    probe->Release();
    printf("adapter: %s\n", adapter);
    printf("feature level: 0x%x\n", (unsigned)level);

    HMODULE xs = LoadLibraryW(L"d3d12_xs.dll");
    auto xboxCreate = xs ? (XboxCreateDeviceFn)GetProcAddress(xs, "D3D12XboxCreateDevice") : nullptr;
    if (!xboxCreate) return Skip("d3d12_xs.dll has no D3D12XboxCreateDevice");

    void* proxy = nullptr;
    HRESULT hr = xboxCreate(nullptr, nullptr, __uuidof(ID3D12Device), &proxy);
    if (FAILED(hr) || !proxy) {
        char reason[128];
        snprintf(reason, sizeof(reason), "D3D12XboxCreateDevice returned hr 0x%08lx", (unsigned long)hr);
        return Skip(reason);
    }
    printf("device: D3D12XboxCreateDevice\n");

    ID3D12Device* dev = nullptr;
    hr = ((IUnknown*)proxy)->QueryInterface(__uuidof(ID3D12Device), (void**)&dev);
    if (!Check(hr, "QueryInterface(ID3D12Device)") || !dev) return Fail("QueryInterface");

    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* queue = nullptr;
    if (!Check(dev->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)), "CreateCommandQueue")) return Fail("CreateCommandQueue");

    ID3D12CommandAllocator* allocator = nullptr;
    if (!Check(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "CreateCommandAllocator"))
        return Fail("CreateCommandAllocator");

    ID3D12GraphicsCommandList* list = nullptr;
    if (!Check(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, IID_PPV_ARGS(&list)), "CreateCommandList"))
        return Fail("CreateCommandList");
    list->Close();

    const DXGI_FORMAT format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    const UINT width = 4, height = 4;
    const float clear[4] = { 0.25f, 0.5f, 0.75f, 1.0f };

    D3D12_HEAP_PROPERTIES defaultHeap = {};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC textureDesc = {};
    textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    textureDesc.Width = width;
    textureDesc.Height = height;
    textureDesc.DepthOrArraySize = 1;
    textureDesc.MipLevels = 1;
    textureDesc.Format = format;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE clearValue = {};
    clearValue.Format = format;
    memcpy(clearValue.Color, clear, sizeof(clear));
    ID3D12Resource* texture = nullptr;
    if (!Check(dev->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &textureDesc, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                            &clearValue, IID_PPV_ARGS(&texture)), "CreateCommittedResource(texture)"))
        return Fail("CreateCommittedResource(texture)");

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heapDesc.NumDescriptors = 1;
    ID3D12DescriptorHeap* heap = nullptr;
    if (!Check(dev->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&heap)), "CreateDescriptorHeap")) return Fail("CreateDescriptorHeap");
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = heap->GetCPUDescriptorHandleForHeapStart();
    dev->CreateRenderTargetView(texture, nullptr, rtv);

    D3D12_RESOURCE_DESC bufferDesc = {};
    bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Width = width * height * 16;
    bufferDesc.Height = 1;
    bufferDesc.DepthOrArraySize = 1;
    bufferDesc.MipLevels = 1;
    bufferDesc.SampleDesc.Count = 1;
    bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES readbackHeap = {};
    readbackHeap.Type = D3D12_HEAP_TYPE_READBACK;
    ID3D12Resource* readback = nullptr;
    if (!Check(dev->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &bufferDesc, D3D12_RESOURCE_STATE_COPY_DEST,
                                            nullptr, IID_PPV_ARGS(&readback)), "CreateCommittedResource(readback)"))
        return Fail("CreateCommittedResource(readback)");

    if (!Check(list->Reset(allocator, nullptr), "Reset")) return Fail("Reset");
    list->ClearRenderTargetView(rtv, clear, 0, nullptr);
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = texture;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list->ResourceBarrier(1, &barrier);
    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = readback;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Offset = 0;
    dst.PlacedFootprint.Footprint.Format = format;
    dst.PlacedFootprint.Footprint.Width = width;
    dst.PlacedFootprint.Footprint.Height = height;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = width * 16;
    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource = texture;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    if (!Check(list->Close(), "Close")) return Fail("Close");

    ID3D12CommandList* lists[] = { list };
    queue->ExecuteCommandLists(1, lists);
    ID3D12Fence* fence = nullptr;
    if (!Check(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "CreateFence")) return Fail("CreateFence");
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!Check(queue->Signal(fence, 1), "Signal")) return Fail("Signal");
    if (fence->GetCompletedValue() < 1) {
        if (!Check(fence->SetEventOnCompletion(1, event), "SetEventOnCompletion")) return Fail("SetEventOnCompletion");
        if (WaitForSingleObject(event, 20000) != WAIT_OBJECT_0) {
            printf("fence wait timed out\n");
            return Fail("fence wait");
        }
    }

    void* mapped = nullptr;
    D3D12_RANGE range = { 0, (SIZE_T)bufferDesc.Width };
    if (!Check(readback->Map(0, &range, &mapped), "Map")) return Fail("Map");
    bool ok = true;
    for (UINT y = 0; y < height; ++y) {
        for (UINT x = 0; x < width; ++x) {
            const unsigned* texel = (const unsigned*)((const unsigned char*)mapped + y * width * 16 + x * 16);
            for (int channel = 0; channel < 4; ++channel) {
                unsigned want = 0;
                memcpy(&want, &clear[channel], 4);
                if (texel[channel] != want) ok = false;
            }
        }
    }
    const unsigned* words = (const unsigned*)mapped;
    printf("cleared words: %08x %08x %08x %08x\n", words[0], words[1], words[2], words[3]);
    unsigned expected = 0;
    memcpy(&expected, &clear[0], 4);
    printf("clear: 0x%08x expected 0x%08x\n", words[0], expected);
    readback->Unmap(0, nullptr);
    printf("RESULT: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
