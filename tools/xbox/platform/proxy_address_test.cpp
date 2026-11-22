#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <fstream>
#include <iterator>
#include "xbox_device_overrides.h"
using Microsoft::WRL::ComPtr;
using namespace xbox_address;

static int g_fail = 0;
static void Check(bool ok, const char* what) {
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) g_fail++;
}

static UINT Lookup(const std::vector<unsigned char>& table, UINT64 addr) {
    auto base = [&](UINT i) { UINT w[2]; memcpy(w, table.data() + 16 + 16 * (size_t)i, 8); return (UINT64)w[0] | (UINT64)w[1] << 32; };
    UINT cur = 0;
    for (UINT step = kEntries / 2; step; step /= 2)
        if (base(cur + step) <= addr) cur += step;
    UINT slot;
    memcpy(&slot, table.data() + 16 + 16 * (size_t)cur + 12, 4);
    return slot;
}

static std::vector<unsigned char> Encode(const std::vector<Range>& ranges, UINT* dropped = nullptr) {
    std::vector<unsigned char> t((size_t)kTableBytes);
    WriteTable(BuildEntries(ranges, dropped), t.data());
    return t;
}

static void CpuChecks() {
    std::vector<Entry> e = BuildEntries({}, nullptr);
    Check(e.size() == 1 && e[0].base == 0 && e[0].slot == kNullSlot, "empty table holds only the null entry at address 0");
    e = BuildEntries({ { 0x30000, 0x1000, 5, nullptr }, { 0x10000, 0x2000, 4, nullptr } }, nullptr);
    Check(e.size() == 5 && e[1].base == 0x10000 && e[1].slot == 4 && e[2].base == 0x12000 && e[2].slot == kNullSlot && e[3].slot == 5 && e[4].base == 0x31000,
          "disjoint ranges are sorted and each end gets a null entry");
    e = BuildEntries({ { 0x10000, 0x1000, 4, nullptr }, { 0x11000, 0x1000, 5, nullptr } }, nullptr);
    Check(e.size() == 4 && e[2].base == 0x11000 && e[2].slot == 5 && e[3].base == 0x12000, "adjacent ranges get no null entry between them");
    e = BuildEntries({ { 100, 100, 4, nullptr }, { 150, 20, 5, nullptr } }, nullptr);
    Check(e.size() == 4 && e[2].base == 150 && e[3].base == 200 && e[3].slot == kNullSlot, "a range inside another ends at the outer range's end");
    e = BuildEntries({ { 0x10000, 0x1000, 4, nullptr }, { 0x10000, 0x2000, 7, nullptr } }, nullptr);
    Check(e.size() == 3 && e[1].slot == 7 && e[2].base == 0x12000, "two ranges at one base keep the later one");
    e = BuildEntries({ { 0x10000, 0x100000000ull + 64, 4, nullptr } }, nullptr);
    Check(e[1].size == 0xFFFFFFFFu && e[2].base == 0x10000 + 0x100000000ull + 64, "sizes over 4 GiB are clamped in the entry, not in the extent");
    std::vector<Range> many;
    for (UINT i = 0; i < 3000; ++i) many.push_back({ 0x100000ull * (i + 1), 0x1000, kFirstResourceSlot + (i % 4000), nullptr });
    UINT dropped = 0;
    e = BuildEntries(many, &dropped);
    Check(e.size() == kEntries && dropped == 1 + 2 * 3000 - kEntries, "more entries than the table holds are dropped and counted");

    std::vector<Range> rs = { { 0x123456780000ull, 4 << 20, 9, nullptr }, { 0x1FFFFF000ull, 8 << 20, 3, nullptr }, { 0x1000, 0x1000, 2, nullptr } };
    std::vector<unsigned char> t = Encode(rs);
    UINT h[4]; memcpy(h, t.data(), 16);
    Check(h[0] == 7 && h[1] == kEntries && h[2] == kReserved, "header: used entries, capacity, reserved slots");
    UINT unused[4]; memcpy(unused, t.data() + 16 + 16 * 7, 16);
    Check(unused[0] == 0xFFFFFFFFu && unused[1] == 0xFFFFFFFFu, "unused entries have base 0xFFFFFFFFFFFFFFFF");
    bool lookups = Lookup(t, 0) == kNullSlot && Lookup(t, 0xFFF) == kNullSlot && Lookup(t, 0x1000) == 2 && Lookup(t, 0x1FFF) == 2 && Lookup(t, 0x2000) == kNullSlot
        && Lookup(t, 0x1FFFFF000ull) == 3 && Lookup(t, 0x200000000ull) == 3 && Lookup(t, 0x1FFFFF000ull + (8 << 20)) == kNullSlot
        && Lookup(t, 0x123456780000ull + 12) == 9 && Lookup(t, 0x123456780000ull + (4 << 20)) == kNullSlot && Lookup(t, ~0ull - 1) == kNullSlot;
    Check(lookups, "the shader's binary search finds the owning range, and the null slot in gaps, below and above");
}

static SIZE_T ProxyCpuStart(void* heap) { return ((SIZE_T(__fastcall*)(void*))(*(void***)heap)[9])(heap); }
static UINT64 ProxyGpuStart(void* heap) { return ((UINT64(__fastcall*)(void*))(*(void***)heap)[10])(heap); }

static ComPtr<ID3D12Resource> Committed(ID3D12Device* dev, UINT64 bytes, D3D12_HEAP_TYPE type, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state) {
    D3D12_HEAP_PROPERTIES hp = { type };
    D3D12_RESOURCE_DESC rd = { D3D12_RESOURCE_DIMENSION_BUFFER, 0, bytes, 1, 1, 1, DXGI_FORMAT_UNKNOWN, { 1, 0 }, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, flags };
    ComPtr<ID3D12Resource> r;
    HRESULT hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr, IID_PPV_ARGS(&r));
    if (FAILED(hr)) printf("     resource creation failed 0x%08lx\n", (unsigned long)hr);
    return r;
}

static bool FindRange(Table* t, UINT64 base, UINT* slot) {
    std::vector<unsigned char> tb((size_t)kTableBytes);
    t->ReadTable(tb.data());
    UINT n; memcpy(&n, tb.data(), 4);
    for (UINT i = 0; i < n; ++i) {
        UINT w[4]; memcpy(w, tb.data() + 16 + 16 * (size_t)i, 16);
        if (((UINT64)w[1] << 32 | w[0]) == base) { if (slot) *slot = w[3]; return w[3] != kNullSlot; }
    }
    return false;
}

static int DeviceChecks(bool warp, const std::vector<char>& dxil, UINT groups, const char* dump) {
    ComPtr<IDXGIFactory4> fac;
    CreateDXGIFactory1(IID_PPV_ARGS(&fac));
    ComPtr<IDXGIAdapter> ad;
    if (warp) fac->EnumWarpAdapter(IID_PPV_ARGS(&ad)); else fac->EnumAdapters(0, &ad);
    DXGI_ADAPTER_DESC ad_desc; ad->GetDesc(&ad_desc);
    printf("adapter: %ls\n", ad_desc.Description);
    ComPtr<ID3D12Device> dev;
    if (FAILED(D3D12CreateDevice(ad.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev)))) { printf("no device\n"); return 1; }
    XboxObjectProxy* proxy = xbox_overrides::WrapDevice(dev.Get());
    auto* table = (Table*)proxy->context;
    Check(table != nullptr, "address table is on (resource binding tier 3, shader model 6.6)");
    if (!table) return 1;
    auto* pdev = (ID3D12Device*)proxy;
    UINT inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 16, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
    void* heapObj = nullptr;
    Check(SUCCEEDED(pdev->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap), &heapObj)), "shader-visible heap through the proxy");
    auto* heapProxy = (XboxObjectProxy*)heapObj;
    auto* realHeap = (ID3D12DescriptorHeap*)heapProxy->real;
    Check(realHeap->GetDesc().NumDescriptors == 16 + kReserved, "the real heap has the reserved slots added");
    Check(((ID3D12DescriptorHeap*)heapObj)->GetDesc().NumDescriptors == 16, "the proxy heap reports the requested size");
    Check(ProxyCpuStart(heapObj) == realHeap->GetCPUDescriptorHandleForHeapStart().ptr + (SIZE_T)kReserved * inc
          && ProxyGpuStart(heapObj) == realHeap->GetGPUDescriptorHandleForHeapStart().ptr + (UINT64)kReserved * inc,
          "the proxy heap starts after the reserved slots");
    D3D12_DESCRIPTOR_HEAP_DESC cpuOnly = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 16, D3D12_DESCRIPTOR_HEAP_FLAG_NONE };
    void* cpuHeap = nullptr;
    pdev->CreateDescriptorHeap(&cpuOnly, __uuidof(ID3D12DescriptorHeap), &cpuHeap);
    Check(cpuHeap && ((ID3D12DescriptorHeap*)((XboxObjectProxy*)cpuHeap)->real)->GetDesc().NumDescriptors == 16, "a non-shader-visible heap is not enlarged");

    D3D12_ROOT_PARAMETER1 rp = {};
    rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC vrs = {};
    vrs.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    vrs.Desc_1_1 = { 1, &rp, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };
    ComPtr<ID3DBlob> sb, eb;
    D3D12SerializeVersionedRootSignature(&vrs, &sb, &eb);
    ComPtr<ID3D12RootSignature> plainSig, sig;
    dev->CreateRootSignature(0, sb->GetBufferPointer(), sb->GetBufferSize(), IID_PPV_ARGS(&plainSig));
    Check(SUCCEEDED(pdev->CreateRootSignature(0, sb->GetBufferPointer(), sb->GetBufferSize(), IID_PPV_ARGS(&sig))), "root signature through the proxy");
    ComPtr<ID3DBlob> patched;
    Check(AddHeapIndexingFlag(sb->GetBufferPointer(), sb->GetBufferSize(), &patched) == S_OK && patched, "the flag is added to a signature that lacks it");
    ComPtr<ID3DBlob> twice;
    Check(AddHeapIndexingFlag(patched->GetBufferPointer(), patched->GetBufferSize(), &twice) == S_FALSE, "a signature that has the flag is left alone");
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
    pd.CS = { dxil.data(), dxil.size() };
    ComPtr<ID3D12PipelineState> pso, plainPso;
    pd.pRootSignature = plainSig.Get();
    HRESULT plainHr = dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&plainPso));
    Check(FAILED(plainHr), "control: the kernel is rejected with the unpatched signature");
    pd.pRootSignature = sig.Get();
    HRESULT hr = pdev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso));
    printf("     pipeline 0x%08lx\n", (unsigned long)hr);
    Check(SUCCEEDED(hr), "the kernel is accepted with the signature created through the proxy");
    if (FAILED(hr)) return 1;

    size_t before = table->RangeCount();
    ComPtr<ID3D12Resource> decoy;
    {
        D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
        D3D12_RESOURCE_DESC rd = { D3D12_RESOURCE_DIMENSION_BUFFER, 0, 1 << 16, 1, 1, 1, DXGI_FORMAT_UNKNOWN, { 1, 0 }, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE };
        pdev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&decoy));
    }
    Check(decoy && (decoy->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS), "a default-heap buffer gets the UAV flag");
    UINT decoySlot = 0;
    Check(table->RangeCount() == before + 1 && FindRange(table, decoy->GetGPUVirtualAddress(), &decoySlot), "a committed buffer is recorded at its GPU address");
    decoy.Reset();
    Check(table->RangeCount() == before, "a released buffer is removed");

    ComPtr<ID3D12Resource> upload;
    {
        D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_UPLOAD };
        D3D12_RESOURCE_DESC rd = { D3D12_RESOURCE_DIMENSION_BUFFER, 0, 1 << 16, 1, 1, 1, DXGI_FORMAT_UNKNOWN, { 1, 0 }, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE };
        pdev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload));
    }
    Check(upload && table->RangeCount() == before, "an upload-heap buffer is not recorded (it cannot have a UAV)");

    const UINT64 U0 = 4 << 20, U1 = 8 << 20;
    ComPtr<ID3D12Resource> u0, u1;
    {
        D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
        D3D12_RESOURCE_DESC rd = { D3D12_RESOURCE_DIMENSION_BUFFER, 0, U0, 1, 1, 1, DXGI_FORMAT_UNKNOWN, { 1, 0 }, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE };
        pdev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&u0));
    }
    ComPtr<ID3D12Heap> placedHeap;
    {
        D3D12_HEAP_DESC phd = { U1 + (1 << 16), { D3D12_HEAP_TYPE_DEFAULT }, 0, D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS };
        dev->CreateHeap(&phd, IID_PPV_ARGS(&placedHeap));
        D3D12_RESOURCE_DESC rd = { D3D12_RESOURCE_DIMENSION_BUFFER, 0, U1, 1, 1, 1, DXGI_FORMAT_UNKNOWN, { 1, 0 }, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS };
        pdev->CreatePlacedResource(placedHeap.Get(), 1 << 16, &rd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&u1));
    }
    UINT s0 = 0, s1 = 0;
    Check(u0 && FindRange(table, u0->GetGPUVirtualAddress(), &s0), "u0 (committed) is recorded");
    Check(u1 && FindRange(table, u1->GetGPUVirtualAddress(), &s1), "u1 (placed, at offset 64 KiB of its heap) is recorded");
    Check(s0 == decoySlot || s1 == decoySlot, "the released buffer's descriptor slot is reused");
    printf("     u0 %016llx slot %u, u1 %016llx slot %u\n", (unsigned long long)u0->GetGPUVirtualAddress(), s0, (unsigned long long)u1->GetGPUVirtualAddress(), s1);

    auto up = Committed(dev.Get(), U0, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
    auto cb = Committed(dev.Get(), 256, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
    auto rb0 = Committed(dev.Get(), U0, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    auto rb1 = Committed(dev.Get(), U1, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    {
        uint32_t* p; up->Map(0, nullptr, (void**)&p);
        uint32_t s = 1;
        for (UINT i = 0; i < U0 / 4; i++) { s = s * 1664525u + 1013904223u; uint32_t x = s * 2654435761u; x ^= x >> 15; x *= 2246822519u; x ^= x >> 13; p[i] = x; }
        up->Unmap(0, nullptr);
        UINT64* c; cb->Map(0, nullptr, (void**)&c);
        c[0] = u0->GetGPUVirtualAddress(); c[1] = u1->GetGPUVirtualAddress();
        cb->Unmap(0, nullptr);
    }
    ComPtr<ID3D12CommandAllocator> al; dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&al));
    ComPtr<ID3D12GraphicsCommandList> cl; dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, al.Get(), nullptr, IID_PPV_ARGS(&cl));
    auto zero = Committed(dev.Get(), U1, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
    { void* z; zero->Map(0, nullptr, &z); memset(z, 0, (size_t)U1); zero->Unmap(0, nullptr); }
    cl->CopyResource(u0.Get(), up.Get());
    D3D12_RESOURCE_BARRIER zb = {};
    zb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; zb.Transition.pResource = u1.Get(); zb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    zb.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS; zb.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    cl->ResourceBarrier(1, &zb);
    cl->CopyBufferRegion(u1.Get(), 0, zero.Get(), 0, U1);
    zb.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST; zb.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    cl->ResourceBarrier(1, &zb);
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.pResource = u0.Get(); b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST; b.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    cl->ResourceBarrier(1, &b);
    cl->SetComputeRootSignature(sig.Get());
    cl->SetPipelineState(pso.Get());
    ID3D12DescriptorHeap* heaps[] = { realHeap };
    cl->SetDescriptorHeaps(1, heaps);
    cl->SetComputeRootConstantBufferView(0, cb->GetGPUVirtualAddress());
    cl->Dispatch(groups, 1, 1);
    D3D12_RESOURCE_BARRIER b2[2] = { b, b };
    b2[0].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS; b2[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b2[1] = b2[0]; b2[1].Transition.pResource = u1.Get();
    cl->ResourceBarrier(2, b2);
    cl->CopyResource(rb0.Get(), u0.Get());
    cl->CopyBufferRegion(rb1.Get(), 0, u1.Get(), 0, U1);
    cl->Close();
    ComPtr<ID3D12CommandQueue> q; D3D12_COMMAND_QUEUE_DESC qd = {}; dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q));
    ID3D12CommandList* lists[] = { cl.Get() };
    q->ExecuteCommandLists(1, lists);
    ComPtr<ID3D12Fence> f; dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&f));
    q->Signal(f.Get(), 1);
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    f->SetEventOnCompletion(1, ev);
    bool done = WaitForSingleObject(ev, 60000) == WAIT_OBJECT_0 && SUCCEEDED(dev->GetDeviceRemovedReason());
    Check(done, "the dispatch completes");
    if (!done) return 1;
    std::vector<unsigned char> o0((size_t)U0), o1((size_t)U1);
    void* p;
    rb0->Map(0, nullptr, &p); memcpy(o0.data(), p, o0.size()); rb0->Unmap(0, nullptr);
    rb1->Map(0, nullptr, &p); memcpy(o1.data(), p, o1.size()); rb1->Unmap(0, nullptr);
    if (dump) {
        for (int i = 0; i < 2; i++) {
            char fn[MAX_PATH]; sprintf_s(fn, "%s_%s_u%d.bin", dump, warp ? "warp" : "gpu", i);
            FILE* out = nullptr;
            if (!fopen_s(&out, fn, "wb")) { fwrite(i ? o1.data() : o0.data(), 1, i ? o1.size() : o0.size(), out); fclose(out); printf("     wrote %s\n", fn); }
        }
    }
    uint32_t first[12]; memcpy(first, o1.data() + 64, sizeof(first));
    uint32_t w1; memcpy(&w1, o0.data() + 4, 4);
    Check(first[0] == w1, "thread 1's first loaded dword reached u1 through the table");

    u0.Reset(); u1.Reset(); upload.Reset();
    Check(table->RangeCount() == before, "all buffers created through the proxy are gone from the table after release");
    ((IUnknown*)heapObj)->Release();
    return 0;
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    CpuChecks();
    if (argc > 1) {
        std::ifstream in(argv[1], std::ios::binary);
        std::vector<char> dxil((std::istreambuf_iterator<char>(in)), {});
        UINT groups = argc > 2 ? (UINT)atoi(argv[2]) : 16;
        bool warp = argc > 3 && !strcmp(argv[3], "warp");
        if (dxil.empty()) { printf("FAIL cannot read %s\n", argv[1]); return 1; }
        if (DeviceChecks(warp, dxil, groups, getenv("PT_DUMP"))) g_fail++;
    }
    printf("%d failure(s)\n", g_fail);
    return g_fail ? 1 : 0;
}
