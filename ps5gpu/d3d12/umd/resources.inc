// Heaps, resources, views and the first command-list functions (included by ps5d3d12.cpp).
//
// Heaps come as {memory pool, CPU page property}: L1 = video memory (DEFAULT), L0 write-combined = UPLOAD, L0
// write-back = READBACK, anything else a CUSTOM heap. Committed resources arrive as heap + resource in one call and
// become vkd3d committed resources; a plain heap becomes an ID3D12Heap, and if the CPU may see it, MapHeap maps a
// buffer placed over the whole heap (the runtime maps heaps, not resources, and adds the offsets itself).
struct P5Resource {
    ID3D12Resource* r;
    D3DKMT_HANDLE alloc;                       // swap-chain buffer: its Windows allocation (A8R8G8B8, our segment)
    UINT width, height;
    DXGI_FORMAT format;
    ID3D12Resource* readback;                  // present: the picture comes back through this buffer
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
    P5Device* dev;
    D3D12DDI_HRTRESOURCE hrt;
    D3DKMT_HANDLE kmResource;
    UINT64 gpuva;                              // GPU VA of the Windows allocation (the runtime maps it)
    D3DKMT_HANDLE staging;                     // CPU-visible twin: the picture goes there, the kernel copies it over
    UINT64 stagingVa;
    ULONGLONG cookie, stagingCookie;
    ID3D12Heap* backing;                       // buffer that may back an app heap: made as heap + buffer placed at 0;
    UINT64 backingOffset;                      // placed resources: the heap they sit in (one reference) and where
};
struct P5Heap {
    ID3D12Heap* h;
    ID3D12Resource* committed;                 // committed resource: map this one
    ID3D12Resource* view;                      // plain CPU-visible heap: a buffer over all of it, made on first map
    ID3D12Heap* backing;                       // heap + heap-sized buffer: the vkd3d heap behind it (tile mappings)
    UINT64 bytes;
    D3D12_HEAP_PROPERTIES props;
};
static D3D12_HEAP_PROPERTIES HeapProps(const D3D12DDIARG_CREATEHEAP_0001* h)
{
    D3D12_HEAP_PROPERTIES p = {};
    if (h->MemoryPool == D3D12DDI_MEMORY_POOL_L1) p.Type = D3D12_HEAP_TYPE_DEFAULT;
    else if (h->CPUPageProperty == D3D12DDI_CPU_PAGE_PROPERTY_WRITE_COMBINE) p.Type = D3D12_HEAP_TYPE_UPLOAD;
    else if (h->CPUPageProperty == D3D12DDI_CPU_PAGE_PROPERTY_WRITE_BACK) p.Type = D3D12_HEAP_TYPE_READBACK;
    else {
        p.Type = D3D12_HEAP_TYPE_CUSTOM;
        p.CPUPageProperty = (D3D12_CPU_PAGE_PROPERTY)(h->CPUPageProperty + 1);   // DDI 0..2 -> API 1..3
        p.MemoryPoolPreference = (D3D12_MEMORY_POOL)(h->MemoryPool + 1);
    }
    return p;
}
static D3D12_RESOURCE_DESC ResourceDesc(const D3D12DDIARG_CREATERESOURCE_0075* a)
{
    D3D12_RESOURCE_DESC d = {};
    d.Dimension = (D3D12_RESOURCE_DIMENSION)a->ResourceType;
    d.Width = a->Width;
    d.Height = a->Height;
    d.DepthOrArraySize = a->DepthOrArraySize;
    d.MipLevels = a->MipLevels;
    d.Format = a->Format;
    d.SampleDesc = a->SampleDesc;
    d.Layout = (D3D12_TEXTURE_LAYOUT)a->Layout;
    UINT f = a->Flags, r = 0;
    if (f & D3D12DDI_RESOURCE_FLAG_0003_RENDER_TARGET) r |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    if (f & D3D12DDI_RESOURCE_FLAG_0003_DEPTH_STENCIL) {
        r |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        if (!(f & D3D12DDI_RESOURCE_FLAG_0003_SHADER_RESOURCE)) r |= D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
    }
    if (f & D3D12DDI_RESOURCE_FLAG_0022_UNORDERED_ACCESS) r |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (f & D3D12DDI_RESOURCE_FLAG_0003_CROSS_ADAPTER) r |= D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;
    if (f & D3D12DDI_RESOURCE_FLAG_0003_SIMULTANEOUS_ACCESS) r |= D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
    d.Flags = (D3D12_RESOURCE_FLAGS)r;
    return d;
}
static D3D12_RESOURCE_STATES InitialState(const D3D12_HEAP_PROPERTIES& p, UINT ddiState)
{
    if (p.Type == D3D12_HEAP_TYPE_UPLOAD) return D3D12_RESOURCE_STATE_GENERIC_READ;
    if (p.Type == D3D12_HEAP_TYPE_READBACK) return D3D12_RESOURCE_STATE_COPY_DEST;
    return (D3D12_RESOURCE_STATES)ddiState;
}
static D3DKMT_HANDLE MakeAllocation(P5Resource* res);
// DDI 0088+ (enhanced barriers): resources come with an initial barrier layout and castable formats instead of a
// state. The 0088 entry points convert to the 0075 struct and leave these here for the creation calls below.
struct P5Create88 { bool on; D3D12_BARRIER_LAYOUT layout; UINT32 nCast; const DXGI_FORMAT* casts; };
static thread_local P5Create88 g_c88;
static D3D12_RESOURCE_DESC1 Desc1(const D3D12_RESOURCE_DESC& d)
{
    D3D12_RESOURCE_DESC1 d1 = {};
    d1.Dimension = d.Dimension; d1.Alignment = d.Alignment; d1.Width = d.Width; d1.Height = d.Height;
    d1.DepthOrArraySize = d.DepthOrArraySize; d1.MipLevels = d.MipLevels; d1.Format = d.Format; d1.SampleDesc = d.SampleDesc;
    d1.Layout = d.Layout; d1.Flags = d.Flags;
    return d1;
}
static HRESULT CreateCommittedX(ID3D12Device* dev, const D3D12_HEAP_PROPERTIES* hp, const D3D12_RESOURCE_DESC& d, D3D12_RESOURCE_STATES st,
                                const D3D12_CLEAR_VALUE* pc, ID3D12Resource** out)
{
    if (!g_c88.on) return dev->CreateCommittedResource(hp, D3D12_HEAP_FLAG_NONE, &d, st, pc, IID_ID3D12Resource, (void**)out);
    ID3D12Device10* d10 = NULL;
    HRESULT hr = dev->QueryInterface(IID_ID3D12Device10, (void**)&d10);
    if (FAILED(hr)) return hr;
    D3D12_RESOURCE_DESC1 d1 = Desc1(d);
    D3D12_BARRIER_LAYOUT lay = d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ? D3D12_BARRIER_LAYOUT_UNDEFINED : g_c88.layout;
    hr = d10->CreateCommittedResource3(hp, D3D12_HEAP_FLAG_NONE, &d1, lay, pc, NULL, g_c88.nCast, (DXGI_FORMAT*)g_c88.casts, IID_ID3D12Resource, (void**)out);
    d10->Release();
    return hr;
}
static HRESULT CreatePlacedX(ID3D12Device* dev, ID3D12Heap* heap, UINT64 off, const D3D12_RESOURCE_DESC& d, D3D12_RESOURCE_STATES st,
                             const D3D12_CLEAR_VALUE* pc, ID3D12Resource** out)
{
    if (!g_c88.on) return dev->CreatePlacedResource(heap, off, &d, st, pc, IID_ID3D12Resource, (void**)out);
    ID3D12Device10* d10 = NULL;
    HRESULT hr = dev->QueryInterface(IID_ID3D12Device10, (void**)&d10);
    if (FAILED(hr)) return hr;
    D3D12_RESOURCE_DESC1 d1 = Desc1(d);
    D3D12_BARRIER_LAYOUT lay = d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ? D3D12_BARRIER_LAYOUT_UNDEFINED : g_c88.layout;
    hr = d10->CreatePlacedResource2(heap, off, &d1, lay, pc, g_c88.nCast, (DXGI_FORMAT*)g_c88.casts, IID_ID3D12Resource, (void**)out);
    d10->Release();
    return hr;
}
static D3D12DDI_HEAP_AND_RESOURCE_SIZES APIENTRY CalcPrivateHeapAndResourceSizes(D3D12DDI_HDEVICE, const D3D12DDIARG_CREATEHEAP_0001*,
                                                                                 const D3D12DDIARG_CREATERESOURCE_0075*, D3D12DDI_HPROTECTEDRESOURCESESSION_0030)
{
    P5Trace("CalcPrivateHeapAndResourceSizes");
    D3D12DDI_HEAP_AND_RESOURCE_SIZES s = {sizeof(P5Heap), sizeof(P5Resource)};
    return s;
}
static HRESULT APIENTRY CreateHeapAndResource(D3D12DDI_HDEVICE hd, const D3D12DDIARG_CREATEHEAP_0001* ha, D3D12DDI_HHEAP hh, D3D12DDI_HRTRESOURCE hrt,
                                              const D3D12DDIARG_CREATERESOURCE_0075* ra, const D3D12DDI_CLEAR_VALUES* cv,
                                              D3D12DDI_HPROTECTEDRESOURCESESSION_0030, D3D12DDI_HRESOURCE hr_)
{
    P5Trace("CreateHeapAndResource");
    ID3D12Device* dev = Dev(hd)->dev;
    P5Heap* heap = ha && hh.pDrvPrivate ? new (hh.pDrvPrivate) P5Heap() : NULL;
    P5Resource* res = ra && hr_.pDrvPrivate ? new (hr_.pDrvPrivate) P5Resource() : NULL;
    HRESULT hr = S_OK;
    if (heap) { heap->props = HeapProps(ha); heap->bytes = ha->ByteSize; }
    if (heap && res) {
        D3D12_RESOURCE_DESC d = ResourceDesc(ra);
        if (heap->props.Type == D3D12_HEAP_TYPE_UPLOAD || heap->props.Type == D3D12_HEAP_TYPE_READBACK)
            d.Flags &= ~D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;     // the runtime marks buffers UAV-capable; CPU heaps refuse it
        D3D12_CLEAR_VALUE c = {}, *pc = NULL;
        if (cv && (d.Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL))) {
            c.Format = cv->Format;
            memcpy(c.Color, cv->Color, sizeof c.Color);
            pc = &c;
        }
        // The runtime backs an application heap with one buffer over the whole heap and places resources by
        // {that buffer, offset}: heap-sized buffers (whole 64 KB units) become a vkd3d heap + a buffer placed at 0.
        if (d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER && d.Width >= 65536 && d.Width % 65536 == 0) {
            D3D12_HEAP_DESC hdsc = {d.Width, heap->props, 65536, D3D12_HEAP_FLAG_NONE};
            hr = dev->CreateHeap(&hdsc, IID_ID3D12Heap, (void**)&res->backing);
            if (SUCCEEDED(hr))
                hr = CreatePlacedX(dev, res->backing, 0, d, InitialState(heap->props, ra->InitialResourceState), NULL, &res->r);
            if (FAILED(hr)) {
                static LONG bsaid;
                if (InterlockedIncrement(&bsaid) <= 5) P5Log("  heap-backed buffer %llu bytes: %08lX, committed instead", d.Width, (unsigned long)hr);
                if (res->backing) { res->backing->Release(); res->backing = NULL; }
                hr = E_FAIL;
            }
        } else hr = E_FAIL;
        if (FAILED(hr))
            hr = CreateCommittedX(dev, &heap->props, d, InitialState(heap->props, ra->InitialResourceState), pc, &res->r);
        heap->committed = res->r;
        heap->backing = res->backing;                 // (no reference of its own: the resource holds it)
        res->dev = Dev(hd);
        res->hrt = hrt;
        // Possible swap-chain buffers (DXGI shares them through the kernel right after creating them, so a kernel
        // resource must exist now): every single-mip, single-sample 2D render target of a 32-bit format.
        bool fmt32 = d.Format == DXGI_FORMAT_R8G8B8A8_UNORM || d.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || d.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
                     d.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || d.Format == DXGI_FORMAT_R10G10B10A2_UNORM || d.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS ||
                     d.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS;
        if (SUCCEEDED(hr) && d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && (d.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) &&
            d.MipLevels == 1 && d.DepthOrArraySize == 1 && d.SampleDesc.Count == 1 && fmt32)
            MakeAllocation(res);
        static LONG said;
        if (FAILED(hr) || InterlockedIncrement(&said) <= 30)
            P5Log("committed resource: dim %u %llux%u x%u mips %u fmt %u flags %X (ddi %X) heap %u (ddi flags %X pool %u cpu %u) state %X -> %08lX", (UINT)d.Dimension, d.Width, d.Height,
                d.DepthOrArraySize, d.MipLevels, (UINT)d.Format, (UINT)d.Flags, (UINT)ra->Flags, (UINT)heap->props.Type, (UINT)ha->Flags, (UINT)ha->MemoryPool,
                (UINT)ha->CPUPageProperty, (UINT)ra->InitialResourceState, (unsigned long)hr);
    } else if (heap) {
        UINT f = ha->Flags, deny = 0;
        if (!(f & D3D12DDI_HEAP_FLAG_BUFFERS)) deny |= D3D12_HEAP_FLAG_DENY_BUFFERS;
        if (!(f & D3D12DDI_HEAP_FLAG_RT_DS_TEXTURES)) deny |= D3D12_HEAP_FLAG_DENY_RT_DS_TEXTURES;
        if (!(f & D3D12DDI_HEAP_FLAG_NON_RT_DS_TEXTURES)) deny |= D3D12_HEAP_FLAG_DENY_NON_RT_DS_TEXTURES;
        D3D12_HEAP_DESC d = {ha->ByteSize, heap->props, ha->Alignment, (D3D12_HEAP_FLAGS)deny};
        hr = dev->CreateHeap(&d, IID_ID3D12Heap, (void**)&heap->h);
        P5Log("heap: %llu bytes, type %u, flags %X -> %08lX", d.SizeInBytes, (UINT)heap->props.Type, f, (unsigned long)hr);
    } else if (res) {
        // Placed resource: inside the buffer that backs an application heap, at an offset
        const D3D12DDIARG_HRESOURCE_PLACEMENT& pl = ra->ReuseBufferGPUVA.BaseAddress.UMD;
        P5Resource* base = Obj<P5Resource>(pl.hResource);
        D3D12_RESOURCE_DESC d = ResourceDesc(ra);
        if (base && base->backing) {
            D3D12_HEAP_PROPERTIES hp = {};
            D3D12_HEAP_FLAGS hf;
            base->r->GetHeapProperties(&hp, &hf);
            if (hp.Type == D3D12_HEAP_TYPE_UPLOAD || hp.Type == D3D12_HEAP_TYPE_READBACK) d.Flags &= ~D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            D3D12_CLEAR_VALUE c = {}, *pc = NULL;
            if (cv && (d.Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL))) {
                c.Format = cv->Format;
                memcpy(c.Color, cv->Color, sizeof c.Color);
                pc = &c;
            }
            res->backing = base->backing;
            res->backing->AddRef();
            res->backingOffset = base->backingOffset + pl.Offset;
            hr = CreatePlacedX(dev, res->backing, res->backingOffset, d, InitialState(hp, ra->InitialResourceState), pc, &res->r);
            res->dev = Dev(hd);
            res->hrt = hrt;
        } else if (!pl.hResource.pDrvPrivate) {
            // Reserved (tiled) resource: no memory until UpdateTileMappings maps heap tiles into it
            D3D12_CLEAR_VALUE c = {}, *pc = NULL;
            if (cv && (d.Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL))) {
                c.Format = cv->Format;
                memcpy(c.Color, cv->Color, sizeof c.Color);
                pc = &c;
            }
            if (g_c88.on) {
                ID3D12Device10* d10 = NULL;
                hr = dev->QueryInterface(IID_ID3D12Device10, (void**)&d10);
                if (SUCCEEDED(hr)) {
                    D3D12_BARRIER_LAYOUT lay = d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ? D3D12_BARRIER_LAYOUT_UNDEFINED : g_c88.layout;
                    hr = d10->CreateReservedResource2(&d, lay, pc, NULL, g_c88.nCast, (DXGI_FORMAT*)g_c88.casts, IID_ID3D12Resource, (void**)&res->r);
                    d10->Release();
                }
            } else {
                D3D12_HEAP_PROPERTIES none = {D3D12_HEAP_TYPE_DEFAULT};
                hr = dev->CreateReservedResource(&d, InitialState(none, ra->InitialResourceState), pc, IID_ID3D12Resource, (void**)&res->r);
            }
            res->dev = Dev(hd);
            res->hrt = hrt;
            static LONG rsaid;
            if (FAILED(hr) || InterlockedIncrement(&rsaid) <= 10)
                P5Log("reserved resource: dim %u %llux%u x%u mips %u fmt %u flags %X -> %08lX", (UINT)d.Dimension, d.Width, d.Height, (UINT)d.DepthOrArraySize,
                      (UINT)d.MipLevels, (UINT)d.Format, (UINT)d.Flags, (unsigned long)hr);
            return hr;
        } else hr = E_NOTIMPL;
        static LONG psaid;
        if (FAILED(hr) || InterlockedIncrement(&psaid) <= 20)
            P5Log("placed resource: in %p (%s) at %llu, dim %u %llux%u x%u mips %u fmt %u flags %X state %X -> %08lX", pl.hResource.pDrvPrivate,
                  base && base->backing ? "heap" : "no heap behind it", pl.Offset, (UINT)d.Dimension, d.Width, d.Height, (UINT)d.DepthOrArraySize,
                  (UINT)d.MipLevels, (UINT)d.Format, (UINT)d.Flags, (UINT)ra->InitialResourceState, (unsigned long)hr);
    }
    return hr;
}
static void PresentWorkerIdle();
static void APIENTRY DestroyHeapAndResource(D3D12DDI_HDEVICE, D3D12DDI_HHEAP hh, D3D12DDI_HRESOURCE hr_)
{
    P5Trace("DestroyHeapAndResource");
    if (P5Resource* r = Obj<P5Resource>(hr_)) { if (r->readback) { PresentWorkerIdle(); r->readback->Release(); } if (r->r) r->r->Release(); if (r->backing) r->backing->Release(); r->~P5Resource(); }
    if (P5Heap* h = Obj<P5Heap>(hh)) { if (h->view) h->view->Release(); if (h->h) h->h->Release(); h->~P5Heap(); }
}
static HRESULT APIENTRY MapHeap(D3D12DDI_HDEVICE hd, D3D12DDI_HHEAP hh, void** p)
{
    P5Trace("MapHeap");
    P5Heap* h = Obj<P5Heap>(hh);
    *p = NULL;
    ID3D12Resource* r = h->committed;
    if (!r && h->h) {
        if (!h->view) {
            D3D12_RESOURCE_DESC d = {D3D12_RESOURCE_DIMENSION_BUFFER, 0, h->bytes, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE};
            HRESULT hr = Dev(hd)->dev->CreatePlacedResource(h->h, 0, &d, InitialState(h->props, 0), NULL, IID_ID3D12Resource, (void**)&h->view);
            if (FAILED(hr)) { P5Log("MapHeap: no buffer over the heap: %08lX", (unsigned long)hr); return hr; }
        }
        r = h->view;
    }
    if (!r) return E_INVALIDARG;
    D3D12_RANGE none = {0, 0};
    HRESULT hr = r->Map(0, h->props.Type == D3D12_HEAP_TYPE_READBACK ? NULL : &none, p);
    if (FAILED(hr)) P5Log("MapHeap: %08lX", (unsigned long)hr);
    return hr;
}
static void APIENTRY UnmapHeap(D3D12DDI_HDEVICE, D3D12DDI_HHEAP hh)
{
    P5Trace("UnmapHeap");
    P5Heap* h = Obj<P5Heap>(hh);
    if (ID3D12Resource* r = h->committed ? h->committed : h->view) r->Unmap(0, NULL);
}
static D3D12DDI_GPU_VIRTUAL_ADDRESS APIENTRY CheckResourceVirtualAddress(D3D12DDI_HDEVICE, D3D12DDI_HRESOURCE hr_)
{
    P5Trace("CheckResourceVirtualAddress");
    P5Resource* r = Obj<P5Resource>(hr_);
    return r && r->r && r->r->GetDesc().Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ? r->r->GetGPUVirtualAddress() : 0;
}
static void FillAllocationInfo(ID3D12Device* dev, const D3D12_RESOURCE_DESC& d, D3D12DDI_TEXTURE_LAYOUT layout, D3D12DDI_RESOURCE_ALLOCATION_INFO_0022* o)
{
    D3D12_RESOURCE_ALLOCATION_INFO i = dev->GetResourceAllocationInfo(0, 1, &d);
    memset(o, 0, sizeof *o);
    o->ResourceDataSize = i.SizeInBytes;
    o->ResourceDataAlignment = (UINT32)i.Alignment;
    o->AdditionalDataHeaderAlignment = 1;
    o->AdditionalDataAlignment = 1;
    o->Layout = layout;
}
static void APIENTRY CheckResourceAllocationInfo(D3D12DDI_HDEVICE hd, const D3D12DDIARG_CREATERESOURCE_0075* a, D3D12DDI_RESOURCE_OPTIMIZATION_FLAGS, UINT32, UINT,
                                                 D3D12DDI_RESOURCE_ALLOCATION_INFO_0022* o)
{
    P5Trace("CheckResourceAllocationInfo");
    FillAllocationInfo(Dev(hd)->dev, ResourceDesc(a), a->Layout, o);
}
// ---- DDI 0088 resource entry points: the 0075 code with the layout and castable formats passed on (g_c88) ----
static D3D12DDIARG_CREATERESOURCE_0075 To0075(const D3D12DDIARG_CREATERESOURCE_0088* a)
{
    D3D12DDIARG_CREATERESOURCE_0075 c = {};
    c.ReuseBufferGPUVA = a->ReuseBufferGPUVA;
    c.ResourceType = a->ResourceType;
    c.Width = a->Width; c.Height = a->Height; c.DepthOrArraySize = a->DepthOrArraySize; c.MipLevels = a->MipLevels;
    c.Format = a->Format; c.SampleDesc = a->SampleDesc; c.Layout = a->Layout; c.Flags = a->Flags;
    c.InitialResourceState = D3D12DDI_RESOURCE_STATE_COMMON;          // unused in layout mode
    c.pRowMajorLayout = a->pRowMajorLayout;
    c.SamplerFeedbackMipRegion = a->SamplerFeedbackMipRegion;
    return c;
}
static D3D12DDI_HEAP_AND_RESOURCE_SIZES APIENTRY CalcPrivateHeapAndResourceSizes88(D3D12DDI_HDEVICE hd, const D3D12DDIARG_CREATEHEAP_0001* ha,
                                                                                   const D3D12DDIARG_CREATERESOURCE_0088*, D3D12DDI_HPROTECTEDRESOURCESESSION_0030 ps)
{
    return CalcPrivateHeapAndResourceSizes(hd, ha, NULL, ps);
}
static HRESULT APIENTRY CreateHeapAndResource88(D3D12DDI_HDEVICE hd, const D3D12DDIARG_CREATEHEAP_0001* ha, D3D12DDI_HHEAP hh, D3D12DDI_HRTRESOURCE hrt,
                                                const D3D12DDIARG_CREATERESOURCE_0088* ra, const D3D12DDI_CLEAR_VALUES* cv,
                                                D3D12DDI_HPROTECTEDRESOURCESESSION_0030 ps, D3D12DDI_HRESOURCE hr_)
{
    if (!ra) return CreateHeapAndResource(hd, ha, hh, hrt, NULL, cv, ps, hr_);
    D3D12DDIARG_CREATERESOURCE_0075 c = To0075(ra);
    g_c88 = {true, (D3D12_BARRIER_LAYOUT)ra->InitialBarrierLayout, ra->NumCastableFormats, ra->pCastableFormats};
    HRESULT hr = CreateHeapAndResource(hd, ha, hh, hrt, &c, cv, ps, hr_);
    g_c88.on = false;
    static LONG said;
    if (InterlockedIncrement(&said) <= 3 || FAILED(hr)) P5Log("  (0088 resource: layout %u, %u castable formats -> %08lX)", (UINT)ra->InitialBarrierLayout, ra->NumCastableFormats, (unsigned long)hr);
    return hr;
}
static void APIENTRY CheckResourceAllocationInfo88(D3D12DDI_HDEVICE hd, const D3D12DDIARG_CREATERESOURCE_0088* a, D3D12DDI_RESOURCE_OPTIMIZATION_FLAGS f,
                                                   UINT32 al, UINT nm, D3D12DDI_RESOURCE_ALLOCATION_INFO_0022* o)
{
    D3D12DDIARG_CREATERESOURCE_0075 c = To0075(a);
    CheckResourceAllocationInfo(hd, &c, f, al, nm, o);
}
static void APIENTRY ImplicitShaderCacheControl(D3D12DDI_HDEVICE, D3D12DDI_IMPLICIT_SHADER_CACHE_CONTROL_FLAGS_0080) {}
static void APIENTRY CheckExistingResourceAllocationInfo(D3D12DDI_HDEVICE hd, D3D12DDI_HRESOURCE hr_, D3D12DDI_RESOURCE_ALLOCATION_INFO_0022* o)
{
    P5Trace("CheckExistingResourceAllocationInfo");
    D3D12_RESOURCE_DESC d = Obj<P5Resource>(hr_)->r->GetDesc();
    FillAllocationInfo(Dev(hd)->dev, d, (D3D12DDI_TEXTURE_LAYOUT)d.Layout, o);
}
static void APIENTRY CheckSubresourceInfo(D3D12DDI_HDEVICE hd, D3D12DDI_HRESOURCE hr_, UINT sub, D3D12DDI_SUBRESOURCE_INFO* o)
{
    P5Trace("CheckSubresourceInfo");
    memset(o, 0, sizeof *o);
    D3D12_RESOURCE_DESC d = Obj<P5Resource>(hr_)->r->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
    UINT rows = 0;
    Dev(hd)->dev->GetCopyableFootprints(&d, sub, 1, 0, &fp, &rows, NULL, NULL);
    o->Offset = fp.Offset;
    o->RowStride = fp.Footprint.RowPitch;
    o->DepthStride = (UINT64)fp.Footprint.RowPitch * rows;
}

// ---- views ----
static ID3D12Resource* Res(D3D12DDI_HRESOURCE h) { P5Resource* r = Obj<P5Resource>(h); return r ? r->r : NULL; }
static void APIENTRY CreateRenderTargetView(D3D12DDI_HDEVICE hd, const D3D12DDIARG_CREATE_RENDER_TARGET_VIEW_0002* a, D3D12DDI_CPU_DESCRIPTOR_HANDLE dst)
{
    P5Trace("CreateRenderTargetView");
    ID3D12Resource* r = Res(a->hDrvResource);
    D3D12_RENDER_TARGET_VIEW_DESC v = {};
    v.Format = a->Format;
    UINT samples = r ? r->GetDesc().SampleDesc.Count : 1;
    switch (a->ResourceDimension) {
    case D3D12DDI_RD_BUFFER: v.ViewDimension = D3D12_RTV_DIMENSION_BUFFER; v.Buffer.FirstElement = a->Buffer.FirstElement; v.Buffer.NumElements = a->Buffer.NumElements; break;
    case D3D12DDI_RD_TEXTURE1D: v.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE1DARRAY; v.Texture1DArray = {a->Tex1D.MipSlice, a->Tex1D.FirstArraySlice, a->Tex1D.ArraySize}; break;
    case D3D12DDI_RD_TEXTURE3D: v.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE3D; v.Texture3D = {a->Tex3D.MipSlice, a->Tex3D.FirstW, a->Tex3D.WSize}; break;
    case D3D12DDI_RD_TEXTURECUBE:
    case D3D12DDI_RD_TEXTURE2D:
    default:
        if (samples > 1) { v.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY; v.Texture2DMSArray = {a->Tex2D.FirstArraySlice, a->Tex2D.ArraySize}; }
        else { v.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY; v.Texture2DArray = {a->Tex2D.MipSlice, a->Tex2D.FirstArraySlice, a->Tex2D.ArraySize, a->Tex2D.PlaneSlice}; }
        break;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE h = {dst.ptr};
    Dev(hd)->dev->CreateRenderTargetView(r, &v, h);
}
static void APIENTRY CreateConstantBufferView(D3D12DDI_HDEVICE hd, const D3D12DDI_CONSTANT_BUFFER_VIEW_DESC* a, D3D12DDI_CPU_DESCRIPTOR_HANDLE dst)
{
    P5Trace("CreateConstantBufferView");
    D3D12_CONSTANT_BUFFER_VIEW_DESC v = {a->BufferLocation, a->SizeInBytes};
    D3D12_CPU_DESCRIPTOR_HANDLE h = {dst.ptr};
    Dev(hd)->dev->CreateConstantBufferView(&v, h);
}
static void APIENTRY CreateSampler(D3D12DDI_HDEVICE hd, const D3D12DDIARG_CREATE_SAMPLER* a, D3D12DDI_CPU_DESCRIPTOR_HANDLE dst)
{
    P5Trace("CreateSampler");
    static_assert(sizeof(D3D12_SAMPLER_DESC) == sizeof(D3D12DDI_SAMPLER_DESC), "sampler layout");
    D3D12_CPU_DESCRIPTOR_HANDLE h = {dst.ptr};
    Dev(hd)->dev->CreateSampler((const D3D12_SAMPLER_DESC*)a->pSamplerDesc, h);
}

// A Windows allocation for a resource the runtime wants to present or share (swap-chain buffers are ordinary
// committed resources at creation; DXGI asks for their allocation afterwards). Made on the first question.
static D3DKMT_HANDLE MakeAllocation(P5Resource* res)
{
    if (res->alloc || !res->r || !res->dev) return res->alloc;
    D3D12_RESOURCE_DESC d = res->r->GetDesc();
    if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) return 0;
    struct { ULONG magic, kind, width, height, pitch, format; ULONGLONG bytes, gpuPhys; } data = {};
    data.magic = 0x41355350u;          // P5A_MAGIC
    data.kind = 4 | 0x100;             // P5A_USER | P5A_SHARED: the runtime creates these as shared (DXGI shares them)
    data.width = (ULONG)d.Width;
    data.height = d.Height;
    data.pitch = data.width * 4;
    data.format = 21;                  // D3DDDIFMT_A8R8G8B8
    data.bytes = (ULONGLONG)data.pitch * data.height;
    static LONG64 cookies;
    // Low 32 bits: the DXGI format (DWM's driver reads it from here when it opens the shared buffer); high 32: our
    // cookie (0x5035 + 16 bits), by which ps5wddm finds the allocation for the present write escape.
    data.gpuPhys = (0x5035ull << 48) | ((ULONGLONG)((GetCurrentProcessId() * 131u + (UINT)InterlockedIncrement64(&cookies)) & 0xFFFF) << 32) | (ULONGLONG)d.Format;
    res->cookie = data.gpuPhys;
    D3D12DDI_ALLOCATION_INFO_0022 info = {};
    info.pPrivateDriverData = &data;
    info.PrivateDriverDataSize = sizeof data;
    // The runtime refuses its own resource handle here (E_INVALIDARG); with NULL and resource-level data the kernel
    // makes the allocation and a kernel resource of its own.
    D3D12DDICB_ALLOCATE_0022 al = {};
    al.pPrivateDriverData = &data;
    al.PrivateDriverDataSize = sizeof data;
    al.hResource = res->hrt.handle;
    al.NumAllocations = 1;
    al.pAllocationInfo = &info;
    const D3D12DDI_CORELAYER_DEVICECALLBACKS_0050* cb = (const D3D12DDI_CORELAYER_DEVICECALLBACKS_0050*)res->dev->um;
    HRESULT hr = cb->pfnAllocateCb(res->dev->rt, &al);
    if (FAILED(hr)) {
        P5Log("  AllocateCb shared with the runtime's resource: %08lX; trying a private allocation", (unsigned long)hr);
        data.kind = 4; al.hResource = NULL; info.hAllocation = 0;
        hr = cb->pfnAllocateCb(res->dev->rt, &al);
    }
    res->kmResource = al.hKMResource;
    res->gpuva = info.GpuVirtualAddress;
    if (false && SUCCEEDED(hr) && (data.kind & 0x100)) {    // (CPU-visible twin: unused, the picture goes through P5E_WRITE)
        data.kind = 4;
        data.gpuPhys = (0x5035ull << 48) | ((ULONGLONG)GetCurrentProcessId() << 24) | (ULONGLONG)InterlockedIncrement64(&cookies);
        res->stagingCookie = data.gpuPhys;
        D3D12DDI_ALLOCATION_INFO_0022 si = {};
        si.pPrivateDriverData = &data;
        si.PrivateDriverDataSize = sizeof data;
        D3D12DDICB_ALLOCATE_0022 sa = {};
        sa.NumAllocations = 1;
        sa.pAllocationInfo = &si;
        HRESULT h2 = cb->pfnAllocateCb(res->dev->rt, &sa);
        if (SUCCEEDED(h2)) { res->staging = si.hAllocation; res->stagingVa = si.GpuVirtualAddress; }
        P5Log("  staging allocation %X at GPU VA %llX (%08lX); shared one at %llX", res->staging, res->stagingVa, (unsigned long)h2, res->gpuva);
    }
    res->alloc = SUCCEEDED(hr) ? info.hAllocation : 0;
    res->width = data.width; res->height = data.height; res->format = d.Format;
    P5Log("Windows allocation for %ux%u fmt %u: %X, kernel resource %X (%08lX)", data.width, data.height, (UINT)d.Format, res->alloc, res->kmResource, (unsigned long)hr);
    return res->alloc;
}
static D3DKMT_HANDLE APIENTRY CheckResourceAllocationHandle(D3D12DDI_HDEVICE, D3D10DDI_HRESOURCE h)
{
    P5Resource* r = (P5Resource*)h.pDrvPrivate;
    return r ? MakeAllocation(r) : 0;
}

// ---- command-list functions ----
static void APIENTRY ClearRenderTargetView(D3D12DDI_HCOMMANDLIST hl, D3D12DDI_CPU_DESCRIPTOR_HANDLE v, const FLOAT c[4], UINT n, const D3D12DDI_RECT* rects)
{
    P5Trace("ClearRenderTargetView");
    D3D12_CPU_DESCRIPTOR_HANDLE h = {v.ptr};
    List(hl)->l->ClearRenderTargetView(h, c, n, (const D3D12_RECT*)rects);
}
static void APIENTRY ResourceBarrier(D3D12DDI_HCOMMANDLIST hl, UINT n, const D3D12DDIARG_RESOURCE_BARRIER_0022* b)
{
    P5Trace("ResourceBarrier");
    D3D12_RESOURCE_BARRIER v[32];
    UINT k = 0;
    for (UINT i = 0; i < n; ++i) {
        D3D12_RESOURCE_BARRIER& o = v[k];
        memset(&o, 0, sizeof o);
        o.Flags = (D3D12_RESOURCE_BARRIER_FLAGS)b[i].Flags;
        if (b[i].Type == D3D12DDI_RESOURCE_BARRIER_TYPE_TRANSITION) {
            o.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            o.Transition = {Res(b[i].Transition.hResource), b[i].Transition.Subresource, (D3D12_RESOURCE_STATES)b[i].Transition.StateBefore,
                            (D3D12_RESOURCE_STATES)b[i].Transition.StateAfter};
            if (!o.Transition.pResource) continue;
        } else if (b[i].Type == D3D12DDI_RESOURCE_BARRIER_TYPE_UAV) {
            o.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            o.UAV.pResource = Res(b[i].UAV.hResource);
        } else {
            o.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;     // ranged/aliasing: a full barrier is the safe superset
            o.UAV.pResource = NULL;
        }
        if (++k == 32) { List(hl)->l->ResourceBarrier(k, v); k = 0; }
    }
    if (k) List(hl)->l->ResourceBarrier(k, v);
}
static bool CopyLocation(const D3D12DDIARG_BUFFER_PLACEMENT* p, D3D12DDIARG_PLACED_RESOURCE l, D3D12_TEXTURE_COPY_LOCATION* o)
{
    memset(o, 0, sizeof *o);
    o->pResource = Res(p->BaseAddress.UMD.hResource);
    if (l.Layout == D3D12DDI_RL_SELECT_SUBRESOURCE) {
        o->Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        o->SubresourceIndex = (UINT)p->BaseAddress.UMD.Offset;
        return true;
    }
    o->Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    o->PlacedFootprint.Offset = p->BaseAddress.UMD.Offset;
    D3D12_SUBRESOURCE_FOOTPRINT& f = o->PlacedFootprint.Footprint;
    if (l.Layout == D3D12DDI_RL_PLACED_VIRTUAL_SUBRESOURCE_PITCHED) {
        const D3D12DDIARG_VIRTUAL_SUBRESOURCE_PITCHED_LAYOUT* v = (const D3D12DDIARG_VIRTUAL_SUBRESOURCE_PITCHED_LAYOUT*)l.pLayout;
        f = {v->Format, v->VirtualWidth, v->VirtualHeight, v->VirtualDepth, v->Pitch};
        return true;
    }
    if (l.Layout == D3D12DDI_RL_PLACED_PHYSICAL_SUBRESOURCE_PITCHED) {
        const D3D12DDIARG_PHYSICAL_SUBRESOURCE_PITCHED_LAYOUT* v = (const D3D12DDIARG_PHYSICAL_SUBRESOURCE_PITCHED_LAYOUT*)l.pLayout;
        UINT blk = (v->Format >= DXGI_FORMAT_BC1_TYPELESS && v->Format <= DXGI_FORMAT_BC5_SNORM) || (v->Format >= DXGI_FORMAT_BC6H_TYPELESS && v->Format <= DXGI_FORMAT_BC7_UNORM_SRGB) ? 4 : 1;
        f = {v->Format, v->PhysicalWidth * blk, v->PhysicalHeight * blk, v->PhysicalDepth, v->Pitch};
        return true;
    }
    P5Log("copy location layout %u not handled", (UINT)l.Layout);
    return false;
}
static void APIENTRY CopyTextureRegion(D3D12DDI_HCOMMANDLIST hl, const D3D12DDIARG_BUFFER_PLACEMENT* dp, D3D12DDIARG_PLACED_RESOURCE dl, UINT x, UINT y, UINT z,
                                       const D3D12DDIARG_BUFFER_PLACEMENT* sp, D3D12DDIARG_PLACED_RESOURCE sl, const D3D12DDI_BOX* box)
{
    P5Trace("CopyTextureRegion");
    D3D12_TEXTURE_COPY_LOCATION d, s;
    if (!CopyLocation(dp, dl, &d) || !CopyLocation(sp, sl, &s)) return;
    List(hl)->l->CopyTextureRegion(&d, x, y, z, &s, (const D3D12_BOX*)box);
}
static void APIENTRY CopyBufferRegion(D3D12DDI_HCOMMANDLIST hl, D3D12DDIARG_BUFFER_PLACEMENT d, D3D12DDIARG_BUFFER_PLACEMENT s, UINT64 bytes)
{
    P5Trace("CopyBufferRegion");
    List(hl)->l->CopyBufferRegion(Res(d.BaseAddress.UMD.hResource), d.BaseAddress.UMD.Offset, Res(s.BaseAddress.UMD.hResource), s.BaseAddress.UMD.Offset, bytes);
}
static void APIENTRY ResourceCopy(D3D12DDI_HCOMMANDLIST hl, D3D12DDI_HRESOURCE d, D3D12DDI_HRESOURCE s)
{
    P5Trace("ResourceCopy");
    List(hl)->l->CopyResource(Res(d), Res(s));
}
static void APIENTRY PresentList(D3D12DDI_HCOMMANDLIST, D3D12DDI_HCOMMANDQUEUE, const D3D12DDIARG_PRESENT_0001*, D3D12DDI_PRESENT_0051*,
                                 D3D12DDI_PRESENT_CONTEXTS_0051*, D3D12DDI_PRESENT_HWQUEUES_0051*);
static void FillListFuncs(D3D12DDI_COMMAND_LIST_FUNCS_3D_0074* f)
{
    f->pfnClearRenderTargetView = ClearRenderTargetView;
    f->pfnResourceBarrier = ResourceBarrier;
    f->pfnCopyTextureRegion = CopyTextureRegion;
    f->pfnCopyBufferRegion = CopyBufferRegion;
    f->pfnResourceCopy = ResourceCopy;
    f->pfnPresent = PresentList;
}

// Present (phase 3: CPU copy, like the Direct3D 10 driver): the back buffer is copied on the presenting queue into a
// readback buffer, then into the locked Windows allocation (R and B swapped for RGBA formats); the runtime presents
// that allocation through the queue's kernel context.
struct P5PresentCopier { ID3D12CommandAllocator* a; ID3D12GraphicsCommandList* l; };
// Copy n bytes out of write-combined memory with SSE4.1 streaming loads (MOVNTDQA), 16-byte aligned source assumed
// (readback footprints are 256-aligned per row); falls back to memcpy for the tail.
__attribute__((target("sse4.1"))) static void StreamRead(void* dst, const void* src, SIZE_T n)
{
    const __m128i* s = (const __m128i*)src;
    __m128i* d = (__m128i*)dst;
    SIZE_T i = 0, v = n / 64;
    for (; i < v; ++i, s += 4, d += 4) {
        __m128i a = _mm_stream_load_si128((__m128i*)s), b = _mm_stream_load_si128((__m128i*)s + 1);
        __m128i c = _mm_stream_load_si128((__m128i*)s + 2), e = _mm_stream_load_si128((__m128i*)s + 3);
        _mm_storeu_si128(d, a); _mm_storeu_si128(d + 1, b); _mm_storeu_si128(d + 2, c); _mm_storeu_si128(d + 3, e);
    }
    if (n % 64) memcpy(d, s, n % 64);
}
// Present worker: one CPU copy in flight. PresentCopy and resource destruction wait for it to be idle first, so a
// readback buffer is never reused or freed while its rows are being read.
static struct { CRITICAL_SECTION lock; HANDLE wake, idle, thread; P5Resource* job; LONG init; } g_pw;
static void PresentCpu(P5Resource* r);
static DWORD WINAPI PresentWorker(void*)
{
    for (;;) {
        WaitForSingleObject(g_pw.wake, INFINITE);
        EnterCriticalSection(&g_pw.lock);
        P5Resource* r = g_pw.job;
        LeaveCriticalSection(&g_pw.lock);
        if (r) PresentCpu(r);
        EnterCriticalSection(&g_pw.lock);
        g_pw.job = NULL;
        SetEvent(g_pw.idle);
        LeaveCriticalSection(&g_pw.lock);
    }
}
static void PresentWorkerIdle()
{
    if (InterlockedCompareExchange(&g_pw.init, 0, 0) != 2) return;
    WaitForSingleObject(g_pw.idle, INFINITE);  // manual-reset: stays signalled until the next job
}
static void PresentWorkerPost(P5Resource* r)
{
    if (InterlockedCompareExchange(&g_pw.init, 1, 0) == 0) {
        InitializeCriticalSection(&g_pw.lock);
        g_pw.wake = CreateEventA(NULL, FALSE, FALSE, NULL);
        g_pw.idle = CreateEventA(NULL, TRUE, TRUE, NULL);
        g_pw.thread = CreateThread(NULL, 0, PresentWorker, NULL, 0, NULL);
        InterlockedExchange(&g_pw.init, 2);
    }
    while (InterlockedCompareExchange(&g_pw.init, 0, 0) != 2) Sleep(0);
    WaitForSingleObject(g_pw.idle, INFINITE);
    EnterCriticalSection(&g_pw.lock);
    ResetEvent(g_pw.idle);
    g_pw.job = r;
    LeaveCriticalSection(&g_pw.lock);
    SetEvent(g_pw.wake);
}
static void PresentCopy(P5Queue* q, P5Resource* r)
{
    ID3D12Device* dev = q->d->dev;
    LARGE_INTEGER qf, t0, t1;                  // timing: submit + GPU wait
    QueryPerformanceFrequency(&qf);
    QueryPerformanceCounter(&t0);
    PresentWorkerIdle();                       // the last picture's rows are out of the readback buffers
    static P5PresentCopier pc;                 // one per process is enough for phase 3
    if (!pc.a) {
        dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_ID3D12CommandAllocator, (void**)&pc.a);
        dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, pc.a, NULL, IID_ID3D12GraphicsCommandList, (void**)&pc.l);
        pc.l->Close();
    }
    D3D12_RESOURCE_DESC d = r->r->GetDesc();
    if (!r->readback) {
        UINT64 total = 0;
        dev->GetCopyableFootprints(&d, 0, 1, 0, &r->fp, NULL, NULL, &total);
        D3D12_HEAP_PROPERTIES rb = {D3D12_HEAP_TYPE_READBACK};
        D3D12_RESOURCE_DESC bd = {D3D12_RESOURCE_DIMENSION_BUFFER, 0, total, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE};
        if (FAILED(dev->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, NULL, IID_ID3D12Resource, (void**)&r->readback))) return;
    }
    pc.a->Reset();
    pc.l->Reset(pc.a, NULL);
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = {r->r, 0, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE};
    pc.l->ResourceBarrier(1, &b);
    D3D12_TEXTURE_COPY_LOCATION src = {}, dst = {};
    src.pResource = r->r; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource = r->readback; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = r->fp;
    pc.l->CopyTextureRegion(&dst, 0, 0, 0, &src, NULL);
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    pc.l->ResourceBarrier(1, &b);
    pc.l->Close();
    ID3D12CommandList* ls[] = {pc.l};
    q->q->ExecuteCommandLists(1, ls);
    EnterCriticalSection(&q->lock);
    UINT64 t = ++q->ticks;
    q->q->Signal(q->f, t);
    LeaveCriticalSection(&q->lock);
    if (q->f->GetCompletedValue() < t) {
        HANDLE ev = CreateEventA(NULL, FALSE, FALSE, NULL);
        q->f->SetEventOnCompletion(t, ev);
        WaitForSingleObject(ev, 5000);
        CloseHandle(ev);
    }
    QueryPerformanceCounter(&t1);
    static LONG timedGpu;
    LONG tg = InterlockedIncrement(&timedGpu);
    if (tg <= 5 || tg % 30 == 0) P5Log("present copy: gpu part %.1f ms", (t1.QuadPart - t0.QuadPart) * 1000.0 / qf.QuadPart);
    PresentWorkerPost(r);                      // the CPU half (readback rows -> escapes) runs while the game goes on
}
// CPU half of a present: rows out of the readback buffer (streaming loads), R/B swapped for RGBA, to ps5wddm in
// pieces (P5E_WRITE). Runs on the present worker thread.
static void PresentCpu(P5Resource* r)
{
    LARGE_INTEGER qf, t1, t2, t3;
    QueryPerformanceFrequency(&qf);
    QueryPerformanceCounter(&t1);
    t2 = t1;
    const BYTE* px = NULL;
    D3D12_RANGE all = {0, (SIZE_T)(r->fp.Footprint.RowPitch * r->height)};
    if (FAILED(r->readback->Map(0, &all, (void**)&px))) return;
    // The shared swap-chain allocation is not CPU-visible: the rows go to ps5wddm in pieces (escape P5E_WRITE), which
    // writes them where the buffer is (it learns that from DWM's own copies of it).
    if (KmtOpen()) {
        bool swap = r->format == DXGI_FORMAT_R8G8B8A8_UNORM || r->format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || r->format == DXGI_FORMAT_R8G8B8A8_TYPELESS;
        UINT rowBytes = r->width * 4, rowsPer = (60 * 1024) / rowBytes;
        if (!rowsPer) rowsPer = 1;
        static std::vector<BYTE> buf;
        buf.resize(sizeof(P5KmtEscape) + 4 + (size_t)rowsPer * rowBytes);
        ULONGLONG written = 0;
        for (UINT y0 = 0; y0 < r->height; y0 += rowsPer) {
            UINT rows = r->height - y0 < rowsPer ? r->height - y0 : rowsPer;
            P5KmtEscape* e = (P5KmtEscape*)buf.data();
            *e = {0x45355350u, 3, r->cookie, (ULONGLONG)y0 * rowBytes};
            *(ULONG*)(e + 1) = rows * rowBytes;
            UINT32* o = (UINT32*)((BYTE*)(e + 1) + 4);
            static std::vector<UINT32> bounce;       // rows pulled out of the readback memory with streaming loads first:
            bounce.resize((size_t)rowBytes / 4 + 4);  // it is write-combined, plain loads ran at ~25 MB/s
            for (UINT y = 0; y < rows; ++y) {
                const BYTE* srow = px + r->fp.Offset + (UINT64)(y0 + y) * r->fp.Footprint.RowPitch;
                StreamRead(bounce.data(), srow, rowBytes);
                const UINT32* sr = bounce.data();
                UINT32* od = o + (size_t)y * r->width;
                if (swap) for (UINT x = 0; x < r->width; ++x) { UINT32 v = sr[x]; od[x] = (v & 0xFF00FF00u) | ((v & 0xFF) << 16) | ((v >> 16) & 0xFF); }
                else memcpy(od, sr, rowBytes);
            }
            if (y0 == 0) QueryPerformanceCounter(&t2);   // first piece swizzled: rows read from the readback memory
            D3DKMT_ESCAPE es = {};
            es.hAdapter = g_kmt.adapter;
            es.hDevice = g_kmt.device;
            es.Type = D3DKMT_ESCAPE_DRIVERPRIVATE;
            es.pPrivateDriverData = buf.data();
            es.PrivateDriverDataSize = (UINT)(sizeof(P5KmtEscape) + 4 + (size_t)rows * rowBytes);
            NTSTATUS st = D3DKMTEscape(&es);
            if (st) { P5Log("present write escape: status %08lX", (unsigned long)st); break; }
            written += (ULONGLONG)rows * rowBytes;
        }
        QueryPerformanceCounter(&t3);
        static LONG timed;
        LONG tn = InterlockedIncrement(&timed);
        if (tn <= 5 || tn % 30 == 0)
            P5Log("present copy %ux%u (worker): first piece %.1f ms, all pieces %.1f ms (%u rows per piece)", r->width, r->height,
                  (t2.QuadPart - t1.QuadPart) * 1000.0 / qf.QuadPart, (t3.QuadPart - t1.QuadPart) * 1000.0 / qf.QuadPart, rowsPer);
        static LONG said;
        if (InterlockedIncrement(&said) <= 6 || !written) {
            static LONG zero;
            if (written || InterlockedIncrement(&zero) <= 6) P5Log("present: %llu bytes written into the shared buffer", written);
        }
    }
    D3D12_RANGE none = {0, 0};
    r->readback->Unmap(0, &none);
}
static void APIENTRY PresentList(D3D12DDI_HCOMMANDLIST, D3D12DDI_HCOMMANDQUEUE hq, const D3D12DDIARG_PRESENT_0001* a, D3D12DDI_PRESENT_0051* out,
                                 D3D12DDI_PRESENT_CONTEXTS_0051* ctx, D3D12DDI_PRESENT_HWQUEUES_0051*)
{
    P5Queue* q = Obj<P5Queue>(hq);
    static LONG nth;
    LONG me = InterlockedIncrement(&nth);
    if (me <= 12) P5Log("present #%ld enter", me);
    P5Resource* src = a->SurfacesToPresent ? Obj<P5Resource>(a->phSurfacesToPresent[0].hSurface) : NULL;
    P5Resource* dst = Obj<P5Resource>(a->hDstResource);
    if (src && MakeAllocation(src)) PresentCopy(q, src);
    memset(out, 0, sizeof *out);
    out->BroadcastSrcAllocation[0] = src ? src->alloc : 0;   // the shared buffer: DWM opens it (picture copied in by ps5wddm)
    out->BroadcastDstAllocation[0] = dst ? MakeAllocation(dst) : 0;
    out->AddedGpuWork = FALSE;
    out->BackBufferMultiplicity = 1;
    if (ctx) { memset(ctx, 0, sizeof *ctx); ctx->hContext = q->pctx ? q->pctx : q->kctx; ctx->BroadcastContextCount = 0; }
    if (me <= 12)
        P5Log("present #%ld: %u surfaces, src alloc %X, dst alloc %X, flags %X, interval %u, context %p", me, a->SurfacesToPresent, src ? src->alloc : 0,
            dst ? dst->alloc : 0, a->Flags.Value, (UINT)a->FlipInterval, ctx ? ctx->hContext : NULL);
}

// ---- reserved (tiled) resources: tile mappings on the queue, tile copies in lists (DDI structs match the API's) ----
static ID3D12Heap* VkHeap(D3D12DDI_HHEAP hh)
{
    P5Heap* h = Obj<P5Heap>(hh);
    return h ? (h->h ? h->h : h->backing) : NULL;
}
static void APIENTRY UpdateTileMappings(D3D12DDI_HCOMMANDQUEUE hq, D3D12DDI_HRESOURCE hr_, UINT nRegions, const D3D12DDI_TILED_RESOURCE_COORDINATE* coords,
                                        const D3D12DDI_TILE_REGION_SIZE* sizes, D3D12DDI_HHEAP hh, UINT nRanges, const D3D12DDI_TILE_RANGE_FLAGS* rflags,
                                        const UINT* offsets, const UINT* counts, D3D12DDI_TILE_MAPPING_FLAGS flags)
{
    P5Trace("UpdateTileMappings");
    P5Queue* q = Obj<P5Queue>(hq);
    ID3D12Resource* r = Res(hr_);
    ID3D12Heap* heap = hh.pDrvPrivate ? VkHeap(hh) : NULL;
    if (!r || (hh.pDrvPrivate && !heap)) { P5Log("UpdateTileMappings: resource %p heap %p (no vkd3d heap behind it)", r, heap); return; }
    q->q->UpdateTileMappings(r, nRegions, (const D3D12_TILED_RESOURCE_COORDINATE*)coords, (const D3D12_TILE_REGION_SIZE*)sizes, heap, nRanges,
                             (const D3D12_TILE_RANGE_FLAGS*)rflags, offsets, counts, (D3D12_TILE_MAPPING_FLAGS)flags);
}
static void APIENTRY CopyTileMappings(D3D12DDI_HCOMMANDQUEUE hq, D3D12DDI_HRESOURCE hdst, const D3D12DDI_TILED_RESOURCE_COORDINATE* dc,
                                      D3D12DDI_HRESOURCE hsrc, const D3D12DDI_TILED_RESOURCE_COORDINATE* sc, const D3D12DDI_TILE_REGION_SIZE* size,
                                      D3D12DDI_TILE_MAPPING_FLAGS flags)
{
    P5Trace("CopyTileMappings");
    P5Queue* q = Obj<P5Queue>(hq);
    if (Res(hdst) && Res(hsrc))
        q->q->CopyTileMappings(Res(hdst), (const D3D12_TILED_RESOURCE_COORDINATE*)dc, Res(hsrc), (const D3D12_TILED_RESOURCE_COORDINATE*)sc,
                               (const D3D12_TILE_REGION_SIZE*)size, (D3D12_TILE_MAPPING_FLAGS)flags);
}
static void APIENTRY CopyTiles(D3D12DDI_HCOMMANDLIST hl, D3D12DDI_HRESOURCE hr_, const D3D12DDI_TILED_RESOURCE_COORDINATE* c, const D3D12DDI_TILE_REGION_SIZE* size,
                               D3D12DDI_HRESOURCE hbuf, UINT64 off, D3D12DDI_TILE_COPY_FLAGS flags)
{
    P5Trace("CopyTiles");
    if (Res(hr_) && Res(hbuf))
        List(hl)->l->CopyTiles(Res(hr_), (const D3D12_TILED_RESOURCE_COORDINATE*)c, (const D3D12_TILE_REGION_SIZE*)size, Res(hbuf), off, (D3D12_TILE_COPY_FLAGS)flags);
}
static void APIENTRY GetMipPacking(D3D12DDI_HDEVICE hd, D3D12DDI_HRESOURCE hr_, UINT* nPacked, UINT* nTiles)
{
    P5Trace("GetMipPacking");
    ID3D12Resource* r = Res(hr_);
    UINT total = 0;
    D3D12_PACKED_MIP_INFO pm = {};
    D3D12_TILE_SHAPE shape = {};
    if (r) Dev(hd)->dev->GetResourceTiling(r, &total, &pm, &shape, NULL, 0, NULL);
    if (nPacked) *nPacked = pm.NumPackedMips;
    if (nTiles) *nTiles = pm.NumTilesForPackedMips;
}
