#pragma once
#include <windows.h>
#include <d3d12.h>
#include <vector>
#include <algorithm>

namespace xbox_address {
constexpr UINT kLog2 = 12;
constexpr UINT kEntries = 1u << kLog2;
constexpr UINT kReserved = 4096;
constexpr UINT kTableSlot = 0;
constexpr UINT kNullSlot = 1;
constexpr UINT kFirstResourceSlot = 2;
constexpr UINT64 kTableBytes = 16 + 16ull * kEntries;
constexpr UINT64 kNoBase = ~0ull;
static const GUID kTrackerGuid = { 0x6b0e1f52, 0x8c3d, 0x4a7e, { 0x9b, 0x21, 0x5e, 0x3f, 0x70, 0x1d, 0xa4, 0xc8 } };

struct Entry { UINT64 base; UINT size; UINT slot; };

struct Range { UINT64 base; UINT64 size; UINT slot; ID3D12Resource* resource; };

static std::vector<Entry> BuildEntries(std::vector<Range> ranges, UINT* dropped) {
    std::stable_sort(ranges.begin(), ranges.end(), [](const Range& a, const Range& b) { return a.base < b.base; });
    std::vector<Entry> out;
    out.push_back({ 0, 0, kNullSlot });
    UINT64 coverEnd = 0;
    for (size_t i = 0; i < ranges.size(); ++i) {
        const Range& r = ranges[i];
        UINT size = r.size > 0xFFFFFFFFull ? 0xFFFFFFFFu : (UINT)r.size;
        if (!out.empty() && out.back().base == r.base) out.back() = { r.base, size, r.slot };
        else out.push_back({ r.base, size, r.slot });
        UINT64 end = r.base + r.size;
        if (end < r.base) end = kNoBase;
        coverEnd = (std::max)(coverEnd, end);
        bool last = i + 1 == ranges.size();
        if (coverEnd != kNoBase && (last || ranges[i + 1].base > coverEnd)) out.push_back({ coverEnd, 0, kNullSlot });
    }
    UINT drop = 0;
    if (out.size() > kEntries) { drop = (UINT)(out.size() - kEntries); out.resize(kEntries); }
    if (dropped) *dropped = drop;
    return out;
}

static void WriteTable(const std::vector<Entry>& entries, unsigned char* dst) {
    UINT header[4] = { (UINT)entries.size(), kEntries, kReserved, 0 };
    memcpy(dst, header, 16);
    for (UINT i = 0; i < kEntries; ++i) {
        UINT w[4];
        if (i < entries.size()) { w[0] = (UINT)entries[i].base; w[1] = (UINT)(entries[i].base >> 32); w[2] = entries[i].size; w[3] = entries[i].slot; }
        else { w[0] = 0xFFFFFFFFu; w[1] = 0xFFFFFFFFu; w[2] = 0; w[3] = kNullSlot; }
        memcpy(dst + 16 + 16 * (size_t)i, w, 16);
    }
}

class Table;

class Tracker : public IUnknown {
public:
    Tracker(Table* t, bool heap, UINT slot, ID3D12DescriptorHeap* h) : table(t), isHeap(heap), slot(slot), heapPtr(h) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        if (riid == __uuidof(IUnknown)) { *out = this; AddRef(); return S_OK; }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&refs); }
    ULONG STDMETHODCALLTYPE Release() override;
private:
    LONG refs = 1;
    Table* table;
    bool isHeap;
    UINT slot;
    ID3D12DescriptorHeap* heapPtr;
};

class Table {
public:
    static Table* Create(ID3D12Device* dev) {
        if (GetEnvironmentVariableA("XS_NO_ADDRESS_TABLE", nullptr, 0)) return nullptr;
        D3D12_FEATURE_DATA_D3D12_OPTIONS o = {};
        if (FAILED(dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &o, sizeof(o))) || o.ResourceBindingTier < D3D12_RESOURCE_BINDING_TIER_3) return nullptr;
        D3D12_FEATURE_DATA_SHADER_MODEL sm = { D3D_SHADER_MODEL_6_6 };
        if (FAILED(dev->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm))) || sm.HighestShaderModel < D3D_SHADER_MODEL_6_6) return nullptr;
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_CUSTOM;
        hp.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE;
        hp.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = (kTableBytes + 255) & ~255ull;
        rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        ID3D12Resource* buffer = nullptr;
        if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&buffer)))) return nullptr;
        void* mapped = nullptr;
        D3D12_RANGE none = { 0, 0 };
        if (FAILED(buffer->Map(0, &none, &mapped))) { buffer->Release(); return nullptr; }
        auto* t = new Table();
        t->dev = dev;
        t->buffer = buffer;
        t->mapped = (unsigned char*)mapped;
        t->inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        t->Publish();
        return t;
    }

    UINT Increment() const { return inc; }
    ID3D12Resource* Buffer() const { return buffer; }

    void ReadTable(void* dst) {
        AcquireSRWLockShared(&lock);
        memcpy(dst, shadow.data(), shadow.size());
        ReleaseSRWLockShared(&lock);
    }

    size_t RangeCount() {
        AcquireSRWLockShared(&lock);
        size_t n = ranges.size();
        ReleaseSRWLockShared(&lock);
        return n;
    }

    void AddHeap(ID3D12DescriptorHeap* heap) {
        AcquireSRWLockExclusive(&lock);
        heaps.push_back(heap);
        WriteFixed(heap);
        for (const Range& r : ranges) WriteResource(heap, r.slot, r.resource, r.size);
        ReleaseSRWLockExclusive(&lock);
        auto* tr = new Tracker(this, true, 0, heap);
        heap->SetPrivateDataInterface(kTrackerGuid, tr);
        tr->Release();
    }

    void Record(ID3D12Resource* r) {
        if (!r) return;
        D3D12_RESOURCE_DESC d = r->GetDesc();
        if (d.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || !(d.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)) return;
        UINT64 base = r->GetGPUVirtualAddress();
        if (!base || d.Width < 4) return;
        AcquireSRWLockExclusive(&lock);
        UINT slot;
        if (!freeSlots.empty()) { slot = freeSlots.back(); freeSlots.pop_back(); }
        else if (nextSlot < kReserved) slot = nextSlot++;
        else { overflow++; ReleaseSRWLockExclusive(&lock); return; }
        ranges.push_back({ base, d.Width, slot, r });
        for (ID3D12DescriptorHeap* h : heaps) WriteResource(h, slot, r, d.Width);
        Publish();
        ReleaseSRWLockExclusive(&lock);
        auto* tr = new Tracker(this, false, slot, nullptr);
        r->SetPrivateDataInterface(kTrackerGuid, tr);
        tr->Release();
    }

    void Forget(UINT slot) {
        AcquireSRWLockExclusive(&lock);
        auto it = std::find_if(ranges.begin(), ranges.end(), [slot](const Range& r) { return r.slot == slot; });
        if (it != ranges.end()) {
            ranges.erase(it);
            Publish();
            for (ID3D12DescriptorHeap* h : heaps) WriteNull(h, slot);
            freeSlots.push_back(slot);
        }
        ReleaseSRWLockExclusive(&lock);
    }

    void ForgetHeap(ID3D12DescriptorHeap* heap) {
        AcquireSRWLockExclusive(&lock);
        heaps.erase(std::remove(heaps.begin(), heaps.end(), heap), heaps.end());
        ReleaseSRWLockExclusive(&lock);
    }

    UINT Overflow() const { return overflow; }
    UINT Dropped() const { return dropped; }

private:
    void Publish() {
        std::vector<Entry> e = BuildEntries(ranges, &dropped);
        shadow.resize((size_t)kTableBytes);
        WriteTable(e, shadow.data());
        memcpy(mapped, shadow.data(), shadow.size());
    }
    D3D12_CPU_DESCRIPTOR_HANDLE Handle(ID3D12DescriptorHeap* heap, UINT slot) {
        D3D12_CPU_DESCRIPTOR_HANDLE h = heap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += (SIZE_T)slot * inc;
        return h;
    }
    static D3D12_UNORDERED_ACCESS_VIEW_DESC RawView(UINT64 bytes) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC v = {};
        v.Format = DXGI_FORMAT_R32_TYPELESS;
        v.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        UINT64 n = bytes / 4;
        v.Buffer.NumElements = (UINT)(std::min<UINT64>)(n ? n : 1, 0x3FFFFFFFull);
        v.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        return v;
    }
    void WriteNull(ID3D12DescriptorHeap* heap, UINT slot) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC v = RawView(4);
        dev->CreateUnorderedAccessView(nullptr, nullptr, &v, Handle(heap, slot));
    }
    void WriteResource(ID3D12DescriptorHeap* heap, UINT slot, ID3D12Resource* r, UINT64 bytes) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC v = RawView(bytes);
        dev->CreateUnorderedAccessView(r, nullptr, &v, Handle(heap, slot));
    }
    void WriteFixed(ID3D12DescriptorHeap* heap) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC v = RawView(kTableBytes);
        dev->CreateUnorderedAccessView(buffer, nullptr, &v, Handle(heap, kTableSlot));
        for (UINT s = kNullSlot; s < kReserved; ++s) WriteNull(heap, s);
    }

    SRWLOCK lock = SRWLOCK_INIT;
    ID3D12Device* dev = nullptr;
    ID3D12Resource* buffer = nullptr;
    unsigned char* mapped = nullptr;
    UINT inc = 0;
    UINT nextSlot = kFirstResourceSlot;
    UINT overflow = 0;
    UINT dropped = 0;
    std::vector<Range> ranges;
    std::vector<UINT> freeSlots;
    std::vector<ID3D12DescriptorHeap*> heaps;
    std::vector<unsigned char> shadow;
};

inline ULONG STDMETHODCALLTYPE Tracker::Release() {
    LONG n = InterlockedDecrement(&refs);
    if (n == 0) {
        if (isHeap) table->ForgetHeap(heapPtr);
        else table->Forget(slot);
        delete this;
    }
    return (ULONG)n;
}

static HMODULE PcD3d12() {
    static HMODULE m = LoadLibraryW(L"d3d12.dll");
    return m;
}

static HRESULT AddHeapIndexingFlag(const void* blob, SIZE_T size, ID3DBlob** out) {
    *out = nullptr;
    HMODULE m = PcD3d12();
    if (!m) return E_NOTIMPL;
    auto deser = (PFN_D3D12_CREATE_VERSIONED_ROOT_SIGNATURE_DESERIALIZER)GetProcAddress(m, "D3D12CreateVersionedRootSignatureDeserializer");
    auto ser = (PFN_D3D12_SERIALIZE_VERSIONED_ROOT_SIGNATURE)GetProcAddress(m, "D3D12SerializeVersionedRootSignature");
    if (!deser || !ser) return E_NOTIMPL;
    ID3D12VersionedRootSignatureDeserializer* d = nullptr;
    HRESULT hr = deser(blob, size, IID_PPV_ARGS(&d));
    if (FAILED(hr)) return hr;
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* src = d->GetUnconvertedRootSignatureDesc();
    hr = src ? S_OK : E_FAIL;
    if (src) {
        D3D12_VERSIONED_ROOT_SIGNATURE_DESC copy = *src;
        D3D12_ROOT_SIGNATURE_FLAGS* flags = copy.Version == D3D_ROOT_SIGNATURE_VERSION_1_0 ? &copy.Desc_1_0.Flags
                                          : copy.Version == D3D_ROOT_SIGNATURE_VERSION_1_1 ? &copy.Desc_1_1.Flags : &copy.Desc_1_2.Flags;
        if (*flags & D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED) hr = S_FALSE;
        else {
            *flags |= D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;
            ID3DBlob* err = nullptr;
            hr = ser(&copy, out, &err);
            if (err) err->Release();
            if (FAILED(hr) && copy.Version == D3D_ROOT_SIGNATURE_VERSION_1_0) {
                const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* up = nullptr;
                if (SUCCEEDED(d->GetRootSignatureDescAtVersion(D3D_ROOT_SIGNATURE_VERSION_1_1, &up))) {
                    D3D12_VERSIONED_ROOT_SIGNATURE_DESC c11 = *up;
                    c11.Desc_1_1.Flags |= D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;
                    err = nullptr;
                    hr = ser(&c11, out, &err);
                    if (err) err->Release();
                }
            }
        }
    }
    d->Release();
    return hr;
}
}
