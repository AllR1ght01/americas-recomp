// The Xbox device deviations learned so far, applied to a proxy (xbox_device_proxy.h) around a PC D3D12 device:
//   slot 14 CreateDescriptorHeap: same arguments as PC; the heap returned is wrapped so GetCPU/GPUDescriptorHandleForHeapStart (slots 9, 10) return the
//           8-byte handle in RAX as on Xbox (the PC runtime wants a hidden return pointer in RDX and would write through garbage)
//   slot 76 resource creation: same argument order as the PC CreateCommittedResource (slot 27), but the description carries an Xbox texture layout (0x101)
//           and extension words; the layout becomes UNKNOWN (the PC driver chooses) and only the 56-byte PC struct is read
// Other slots are forwarded to the same PC slot (see PLATFORM_SURFACE.md for the ones confirmed to match: 1, 14, 15, 16, 18, 19, 27, 38; 62 is pipeline creation).
#pragma once
#include "xbox_device_proxy.h"
#include "xbox_address_table.h"

namespace xbox_overrides {
static SIZE_T __fastcall HeapCpuStart(XboxObjectProxy* self) { return ((ID3D12DescriptorHeap*)self->real)->GetCPUDescriptorHandleForHeapStart().ptr; }
static UINT64 __fastcall HeapGpuStart(XboxObjectProxy* self) { return ((ID3D12DescriptorHeap*)self->real)->GetGPUDescriptorHandleForHeapStart().ptr; }
static SIZE_T __fastcall ReservedHeapCpuStart(XboxObjectProxy* self) {
    auto* t = (xbox_address::Table*)self->context;
    return ((ID3D12DescriptorHeap*)self->real)->GetCPUDescriptorHandleForHeapStart().ptr + (SIZE_T)xbox_address::kReserved * t->Increment();
}
static UINT64 __fastcall ReservedHeapGpuStart(XboxObjectProxy* self) {
    auto* t = (xbox_address::Table*)self->context;
    return ((ID3D12DescriptorHeap*)self->real)->GetGPUDescriptorHandleForHeapStart().ptr + (UINT64)xbox_address::kReserved * t->Increment();
}
static D3D12_DESCRIPTOR_HEAP_DESC* __fastcall ReservedHeapDesc(XboxObjectProxy* self, D3D12_DESCRIPTOR_HEAP_DESC* out) {
    *out = ((ID3D12DescriptorHeap*)self->real)->GetDesc();
    out->NumDescriptors -= xbox_address::kReserved;
    return out;
}
static bool Reserves(const D3D12_DESCRIPTOR_HEAP_DESC* desc) {
    return desc && desc->Type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV && (desc->Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE);
}
static HRESULT __fastcall Slot14(XboxObjectProxy* self, const D3D12_DESCRIPTOR_HEAP_DESC* desc, REFIID riid, void** out) {
    auto* table = (xbox_address::Table*)self->context;
    bool reserve = table && Reserves(desc);
    D3D12_DESCRIPTOR_HEAP_DESC d;
    if (reserve) { d = *desc; d.NumDescriptors += xbox_address::kReserved; desc = &d; }
    HRESULT hr = ((ID3D12Device*)self->real)->CreateDescriptorHeap(desc, riid, out);
    if (SUCCEEDED(hr) && out && *out && riid == __uuidof(ID3D12DescriptorHeap)) {
        auto* real = (ID3D12DescriptorHeap*)*out;
        auto* h = XboxObjectProxy::Create((IUnknown*)real, "descriptor heap");
        if (reserve) {
            h->context = table;
            table->AddHeap(real);
            h->Override(8, (void*)ReservedHeapDesc);
            h->Override(9, (void*)ReservedHeapCpuStart);
            h->Override(10, (void*)ReservedHeapGpuStart);
        } else {
            h->Override(9, (void*)HeapCpuStart);
            h->Override(10, (void*)HeapGpuStart);
        }
        *out = h;
    }
    return hr;
}
static DXGI_FORMAT UavFallback(ID3D12Device* dev, const D3D12_RESOURCE_DESC& d) {
    if (d.Format != DXGI_FORMAT_R9G9B9E5_SHAREDEXP || !(d.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)) return DXGI_FORMAT_UNKNOWN;
    if (GetEnvironmentVariableA("XS_NO_FORMAT_FALLBACK", nullptr, 0)) return DXGI_FORMAT_UNKNOWN;
    D3D12_FEATURE_DATA_FORMAT_SUPPORT fs = { d.Format };
    if (SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof(fs))) && (fs.Support1 & D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW)) return DXGI_FORMAT_UNKNOWN;
    return DXGI_FORMAT_R16G16B16A16_FLOAT;
}
static void PrepareBuffer(xbox_address::Table* table, D3D12_RESOURCE_DESC& d, D3D12_HEAP_TYPE heapType) {
    if (table && d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER && heapType == D3D12_HEAP_TYPE_DEFAULT && !GetEnvironmentVariableA("XS_NO_BUFFER_UAV", nullptr, 0))
        d.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
}
static void RecordCreated(xbox_address::Table* table, HRESULT hr, void** out) {
    if (!table || FAILED(hr) || !out || !*out) return;
    ID3D12Resource* r = nullptr;
    if (SUCCEEDED(((IUnknown*)*out)->QueryInterface(IID_PPV_ARGS(&r)))) { table->Record(r); r->Release(); }
}
static HRESULT CreateResource(ID3D12Device* dev, const D3D12_HEAP_PROPERTIES* hp, UINT heapFlags, const unsigned char* desc, UINT state, const D3D12_CLEAR_VALUE* cv, REFIID riid, void** out, xbox_address::Table* table = nullptr) {
    D3D12_RESOURCE_DESC d;
    memcpy(&d, desc, sizeof(d));
    if ((unsigned)d.Layout == 0x101) d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    D3D12_CLEAR_VALUE c;
    if (DXGI_FORMAT f = UavFallback(dev, d)) {
        d.Format = f;
        if (cv) { c = *cv; if (c.Format == DXGI_FORMAT_R9G9B9E5_SHAREDEXP) c.Format = f; cv = &c; }
    }
    PrepareBuffer(table, d, hp ? hp->Type : D3D12_HEAP_TYPE_DEFAULT);
    HRESULT hr = dev->CreateCommittedResource(hp, (D3D12_HEAP_FLAGS)heapFlags, &d, (D3D12_RESOURCE_STATES)state, cv, riid, out);
    RecordCreated(table, hr, out);
    return hr;
}
static HRESULT __fastcall Slot76(XboxObjectProxy* self, const D3D12_HEAP_PROPERTIES* hp, UINT heapFlags, const unsigned char* desc, UINT state, const D3D12_CLEAR_VALUE* cv, REFIID riid, void** out) {
    return CreateResource((ID3D12Device*)self->real, hp, heapFlags, desc, state, cv, riid, out, (xbox_address::Table*)self->context);
}
static HRESULT __fastcall Slot27(XboxObjectProxy* self, const D3D12_HEAP_PROPERTIES* hp, D3D12_HEAP_FLAGS heapFlags, const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* cv, REFIID riid, void** out) {
    auto* table = (xbox_address::Table*)self->context;
    D3D12_RESOURCE_DESC d = *desc;
    PrepareBuffer(table, d, hp ? hp->Type : D3D12_HEAP_TYPE_DEFAULT);
    HRESULT hr = ((ID3D12Device*)self->real)->CreateCommittedResource(hp, heapFlags, &d, state, cv, riid, out);
    RecordCreated(table, hr, out);
    return hr;
}
static HRESULT __fastcall Slot29(XboxObjectProxy* self, ID3D12Heap* heap, UINT64 offset, const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* cv, REFIID riid, void** out) {
    auto* table = (xbox_address::Table*)self->context;
    D3D12_RESOURCE_DESC d = *desc;
    if (heap) {
        D3D12_HEAP_DESC hd = heap->GetDesc();
        if (!(hd.Flags & D3D12_HEAP_FLAG_DENY_BUFFERS)) PrepareBuffer(table, d, hd.Properties.Type);
    }
    HRESULT hr = ((ID3D12Device*)self->real)->CreatePlacedResource(heap, offset, &d, state, cv, riid, out);
    RecordCreated(table, hr, out);
    return hr;
}
static HRESULT __fastcall Slot16(XboxObjectProxy* self, UINT nodeMask, const void* blob, SIZE_T size, REFIID riid, void** out) {
    ID3DBlob* patched = nullptr;
    if (self->context && SUCCEEDED(xbox_address::AddHeapIndexingFlag(blob, size, &patched)) && patched) {
        HRESULT hr = ((ID3D12Device*)self->real)->CreateRootSignature(nodeMask, patched->GetBufferPointer(), patched->GetBufferSize(), riid, out);
        patched->Release();
        if (SUCCEEDED(hr)) return hr;
    }
    return ((ID3D12Device*)self->real)->CreateRootSignature(nodeMask, blob, size, riid, out);
}
static bool Substituted(ID3D12Resource* r, DXGI_FORMAT viewFormat) {
    return r && viewFormat == DXGI_FORMAT_R9G9B9E5_SHAREDEXP && r->GetDesc().Format == DXGI_FORMAT_R16G16B16A16_FLOAT;
}
static void __fastcall Slot18(XboxObjectProxy* self, ID3D12Resource* r, const D3D12_SHADER_RESOURCE_VIEW_DESC* desc, SIZE_T handle) {
    D3D12_SHADER_RESOURCE_VIEW_DESC v;
    if (desc && Substituted(r, desc->Format)) { v = *desc; v.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; desc = &v; }
    ((ID3D12Device*)self->real)->CreateShaderResourceView(r, desc, D3D12_CPU_DESCRIPTOR_HANDLE{ handle });
}
static void __fastcall Slot19(XboxObjectProxy* self, ID3D12Resource* r, ID3D12Resource* counter, const D3D12_UNORDERED_ACCESS_VIEW_DESC* desc, SIZE_T handle) {
    D3D12_UNORDERED_ACCESS_VIEW_DESC v;
    if (desc && Substituted(r, desc->Format)) { v = *desc; v.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; desc = &v; }
    ((ID3D12Device*)self->real)->CreateUnorderedAccessView(r, counter, desc, D3D12_CPU_DESCRIPTOR_HANDLE{ handle });
}
// Wrap a PC device as an Xbox device object.
static XboxObjectProxy* WrapDevice(ID3D12Device* dev) {
    auto* p = XboxObjectProxy::Create(dev, "device");
    p->context = xbox_address::Table::Create(dev);
    p->Override(14, (void*)Slot14);
    p->Override(16, (void*)Slot16);
    p->Override(18, (void*)Slot18);
    p->Override(19, (void*)Slot19);
    p->Override(27, (void*)Slot27);
    p->Override(29, (void*)Slot29);
    p->Override(76, (void*)Slot76);
    return p;
}
}  // namespace xbox_overrides
