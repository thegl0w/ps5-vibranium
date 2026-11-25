// Phase 4 fill-in (included by ps5d3d12.cpp): the remaining views, descriptor copies, queries, indirect execution,
// bundles, UAV clears, resolves and small list calls, each a direct forward to vkd3d-proton.
static D3D12_RESOURCE_DESC DescOf(D3D12DDI_HRESOURCE h) { ID3D12Resource* r = Res(h); return r ? r->GetDesc() : D3D12_RESOURCE_DESC{}; }
static void APIENTRY CreateShaderResourceView(D3D12DDI_HDEVICE hd, const D3D12DDIARG_CREATE_SHADER_RESOURCE_VIEW_0002* a, D3D12DDI_CPU_DESCRIPTOR_HANDLE dst)
{
    ID3D12Resource* r = Res(a->hDrvResource);
    D3D12_SHADER_RESOURCE_VIEW_DESC v = {};
    v.Format = a->Format;
    v.Shader4ComponentMapping = a->Shader4ComponentMapping;
    D3D12_RESOURCE_DESC rd = DescOf(a->hDrvResource);
    switch (a->ResourceDimension) {
    case D3D12DDI_RD_BUFFER:
        v.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        v.Buffer = {a->Buffer.FirstElement, a->Buffer.NumElements, a->Buffer.StructureByteStride, (D3D12_BUFFER_SRV_FLAGS)a->Buffer.Flags};
        break;
    case D3D12DDI_RD_TEXTURE1D:
        v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1DARRAY;
        v.Texture1DArray = {a->Tex1D.MostDetailedMip, a->Tex1D.MipLevels, a->Tex1D.FirstArraySlice, a->Tex1D.ArraySize, a->Tex1D.ResourceMinLODClamp};
        break;
    case D3D12DDI_RD_TEXTURE3D:
        v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
        v.Texture3D = {a->Tex3D.MostDetailedMip, a->Tex3D.MipLevels, a->Tex3D.ResourceMinLODClamp};
        break;
    case D3D12DDI_RD_TEXTURECUBE:
        v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
        v.TextureCubeArray = {a->TexCube.MostDetailedMip, a->TexCube.MipLevels, a->TexCube.First2DArrayFace, a->TexCube.NumCubes, a->TexCube.ResourceMinLODClamp};
        break;
    case D3D12DDI_RD_RAYTRACING_ACCELERATION_STRUCTURE_0042:
        v.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
        v.RaytracingAccelerationStructure.Location = a->RaytracingAccelerationStructure.Location;
        r = NULL;
        break;
    default:
        if (rd.SampleDesc.Count > 1) { v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY; v.Texture2DMSArray = {a->Tex2D.FirstArraySlice, a->Tex2D.ArraySize}; }
        else {
            v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            v.Texture2DArray = {a->Tex2D.MostDetailedMip, a->Tex2D.MipLevels, a->Tex2D.FirstArraySlice, a->Tex2D.ArraySize, a->Tex2D.PlaneSlice, a->Tex2D.ResourceMinLODClamp};
        }
        break;
    }
    Dev(hd)->dev->CreateShaderResourceView(r, &v, {dst.ptr});
}
static void APIENTRY CreateUnorderedAccessView(D3D12DDI_HDEVICE hd, const D3D12DDIARG_CREATE_UNORDERED_ACCESS_VIEW_0002* a, D3D12DDI_CPU_DESCRIPTOR_HANDLE dst)
{
    ID3D12Resource* r = Res(a->hDrvResource);
    D3D12_RESOURCE_DESC rd = DescOf(a->hDrvResource);
    D3D12_UNORDERED_ACCESS_VIEW_DESC v = {};
    v.Format = a->Format;
    ID3D12Resource* counter = NULL;
    switch (rd.Dimension) {               // the runtime never sets ResourceDimension for UAVs
    case D3D12_RESOURCE_DIMENSION_BUFFER:
        v.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        v.Buffer = {a->Buffer.FirstElement, a->Buffer.NumElements, a->Buffer.StructureByteStride, a->Buffer.CounterOffsetInBytes, (D3D12_BUFFER_UAV_FLAGS)a->Buffer.Flags};
        counter = Res(a->Buffer.hDrvCounterResource);
        break;
    case D3D12_RESOURCE_DIMENSION_TEXTURE1D:
        v.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1DARRAY;
        v.Texture1DArray = {a->Tex1D.MipSlice, a->Tex1D.FirstArraySlice, a->Tex1D.ArraySize};
        break;
    case D3D12_RESOURCE_DIMENSION_TEXTURE3D:
        v.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
        v.Texture3D = {a->Tex3D.MipSlice, a->Tex3D.FirstW, a->Tex3D.WSize};
        break;
    default:
        v.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
        v.Texture2DArray = {a->Tex2D.MipSlice, a->Tex2D.FirstArraySlice, a->Tex2D.ArraySize, a->Tex2D.PlaneSlice};
        break;
    }
    Dev(hd)->dev->CreateUnorderedAccessView(r, counter, &v, {dst.ptr});
}
static void APIENTRY CreateDepthStencilView(D3D12DDI_HDEVICE hd, const D3D12DDIARG_CREATE_DEPTH_STENCIL_VIEW* a, D3D12DDI_CPU_DESCRIPTOR_HANDLE dst)
{
    ID3D12Resource* r = Res(a->hDrvResource);
    D3D12_RESOURCE_DESC rd = DescOf(a->hDrvResource);
    D3D12_DEPTH_STENCIL_VIEW_DESC v = {};
    v.Format = a->Format;
    v.Flags = (D3D12_DSV_FLAGS)a->Flags;
    if (a->ResourceDimension == D3D12DDI_RD_TEXTURE1D) { v.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE1DARRAY; v.Texture1DArray = {a->Tex1D.MipSlice, a->Tex1D.FirstArraySlice, a->Tex1D.ArraySize}; }
    else if (rd.SampleDesc.Count > 1) { v.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY; v.Texture2DMSArray = {a->Tex2D.FirstArraySlice, a->Tex2D.ArraySize}; }
    else { v.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY; v.Texture2DArray = {a->Tex2D.MipSlice, a->Tex2D.FirstArraySlice, a->Tex2D.ArraySize}; }
    Dev(hd)->dev->CreateDepthStencilView(r, &v, {dst.ptr});
}
static void APIENTRY CopyDescriptors(D3D12DDI_HDEVICE hd, UINT nd, const D3D12DDI_CPU_DESCRIPTOR_HANDLE* ds, const UINT* dsz, UINT ns,
                                     const D3D12DDI_CPU_DESCRIPTOR_HANDLE* ss, const UINT* ssz, D3D12DDI_DESCRIPTOR_HEAP_TYPE t)
{
    Dev(hd)->dev->CopyDescriptors(nd, (const D3D12_CPU_DESCRIPTOR_HANDLE*)ds, dsz, ns, (const D3D12_CPU_DESCRIPTOR_HANDLE*)ss, ssz, (D3D12_DESCRIPTOR_HEAP_TYPE)t);
}
static void APIENTRY CopyDescriptorsSimple(D3D12DDI_HDEVICE hd, UINT n, D3D12DDI_CPU_DESCRIPTOR_HANDLE d, D3D12DDI_CPU_DESCRIPTOR_HANDLE s, D3D12DDI_DESCRIPTOR_HEAP_TYPE t)
{
    Dev(hd)->dev->CopyDescriptorsSimple(n, {d.ptr}, {s.ptr}, (D3D12_DESCRIPTOR_HEAP_TYPE)t);
}

// ---- queries ----
struct P5QueryHeap { ID3D12QueryHeap* h; };
static SIZE_T APIENTRY CalcPrivateQueryHeapSize(D3D12DDI_HDEVICE, const D3D12DDIARG_CREATE_QUERY_HEAP_0001*) { return sizeof(P5QueryHeap); }
static HRESULT APIENTRY CreateQueryHeap(D3D12DDI_HDEVICE hd, const D3D12DDIARG_CREATE_QUERY_HEAP_0001* a, D3D12DDI_HQUERYHEAP hq)
{
    P5QueryHeap* q = new (hq.pDrvPrivate) P5QueryHeap();
    D3D12_QUERY_HEAP_DESC d = {(D3D12_QUERY_HEAP_TYPE)a->Type, a->Count, 0};
    HRESULT hr = Dev(hd)->dev->CreateQueryHeap(&d, IID_ID3D12QueryHeap, (void**)&q->h);
    if (FAILED(hr)) P5Log("CreateQueryHeap type %u count %u: %08lX", (UINT)d.Type, d.Count, (unsigned long)hr);
    return hr;
}
static void APIENTRY DestroyQueryHeap(D3D12DDI_HDEVICE, D3D12DDI_HQUERYHEAP hq)
{
    P5QueryHeap* q = Obj<P5QueryHeap>(hq);
    if (q->h) q->h->Release();
    q->~P5QueryHeap();
}
static void APIENTRY BeginQuery(D3D12DDI_HCOMMANDLIST hl, D3D12DDI_HQUERYHEAP hq, D3D12DDI_QUERY_TYPE t, UINT i) { List(hl)->l->BeginQuery(Obj<P5QueryHeap>(hq)->h, (D3D12_QUERY_TYPE)t, i); }
static void APIENTRY EndQuery(D3D12DDI_HCOMMANDLIST hl, D3D12DDI_HQUERYHEAP hq, D3D12DDI_QUERY_TYPE t, UINT i) { List(hl)->l->EndQuery(Obj<P5QueryHeap>(hq)->h, (D3D12_QUERY_TYPE)t, i); }
static void APIENTRY ResolveQueryData(D3D12DDI_HCOMMANDLIST hl, D3D12DDI_HQUERYHEAP hq, D3D12DDI_QUERY_TYPE t, UINT s, UINT n, D3D12DDI_HRESOURCE dst, UINT64 off)
{
    List(hl)->l->ResolveQueryData(Obj<P5QueryHeap>(hq)->h, (D3D12_QUERY_TYPE)t, s, n, Res(dst), off);
}

// ---- indirect execution ----
struct P5CommandSignature { ID3D12CommandSignature* s; };
static SIZE_T APIENTRY CalcPrivateCommandSignatureSize(D3D12DDI_HDEVICE, const D3D12DDIARG_CREATE_COMMAND_SIGNATURE_0001*) { return sizeof(P5CommandSignature); }
static HRESULT APIENTRY CreateCommandSignature(D3D12DDI_HDEVICE hd, const D3D12DDIARG_CREATE_COMMAND_SIGNATURE_0001* a, D3D12DDI_HCOMMANDSIGNATURE hs)
{
    static_assert(sizeof(D3D12_INDIRECT_ARGUMENT_DESC) == sizeof(D3D12DDI_INDIRECT_ARGUMENT_DESC), "indirect argument layout");
    P5CommandSignature* c = new (hs.pDrvPrivate) P5CommandSignature();
    D3D12_COMMAND_SIGNATURE_DESC d = {a->ByteStride, a->NumArgumentDescs, (const D3D12_INDIRECT_ARGUMENT_DESC*)a->pArgumentDescs, 0};
    P5RootSignature* rs = Obj<P5RootSignature>(a->hRootSignature);
    HRESULT hr = Dev(hd)->dev->CreateCommandSignature(&d, rs ? rs->rs : NULL, IID_ID3D12CommandSignature, (void**)&c->s);
    if (FAILED(hr)) P5Log("CreateCommandSignature: %u arguments, stride %u: %08lX", d.NumArgumentDescs, d.ByteStride, (unsigned long)hr);
    return hr;
}
static void APIENTRY DestroyCommandSignature(D3D12DDI_HDEVICE, D3D12DDI_HCOMMANDSIGNATURE hs)
{
    P5CommandSignature* c = Obj<P5CommandSignature>(hs);
    if (c->s) c->s->Release();
    c->~P5CommandSignature();
}
static void APIENTRY ExecuteIndirect(D3D12DDI_HCOMMANDLIST hl, D3D12DDI_HCOMMANDSIGNATURE hs, UINT max, D3D12DDIARG_BUFFER_PLACEMENT arg, D3D12DDIARG_BUFFER_PLACEMENT cnt)
{
    List(hl)->l->ExecuteIndirect(Obj<P5CommandSignature>(hs)->s, max, Res(arg.BaseAddress.UMD.hResource), arg.BaseAddress.UMD.Offset,
                                 Res(cnt.BaseAddress.UMD.hResource), cnt.BaseAddress.UMD.Offset);
}

// ---- other list calls ----
static void APIENTRY ExecuteBundle(D3D12DDI_HCOMMANDLIST hl, D3D12DDI_HCOMMANDLIST hb) { if (List(hb)->l) List(hl)->l->ExecuteBundle(List(hb)->l); }
static void APIENTRY ClearUavUint(D3D12DDI_HCOMMANDLIST hl, D3D12DDI_GPU_DESCRIPTOR_HANDLE g, D3D12DDI_CPU_DESCRIPTOR_HANDLE c, D3D12DDI_HRESOURCE r, const UINT v[4], UINT n, const D3D12DDI_RECT* rc)
{
    List(hl)->l->ClearUnorderedAccessViewUint({g.ptr}, {c.ptr}, Res(r), v, n, (const D3D12_RECT*)rc);
}
static void APIENTRY ClearUavFloat(D3D12DDI_HCOMMANDLIST hl, D3D12DDI_GPU_DESCRIPTOR_HANDLE g, D3D12DDI_CPU_DESCRIPTOR_HANDLE c, D3D12DDI_HRESOURCE r, const FLOAT v[4], UINT n, const D3D12DDI_RECT* rc)
{
    List(hl)->l->ClearUnorderedAccessViewFloat({g.ptr}, {c.ptr}, Res(r), v, n, (const D3D12_RECT*)rc);
}
static void APIENTRY ResolveSubresource(D3D12DDI_HCOMMANDLIST hl, D3D12DDI_HRESOURCE d, UINT ds, D3D12DDI_HRESOURCE s, UINT ss, DXGI_FORMAT f)
{
    List(hl)->l->ResolveSubresource(Res(d), ds, Res(s), ss, f);
}
static void APIENTRY DiscardResource(D3D12DDI_HCOMMANDLIST hl, D3D12DDI_HRESOURCE r, const D3D12DDIARG_DISCARD_RESOURCE_0003*)
{
    List(hl)->l->DiscardResource(Res(r), NULL);
}
static void APIENTRY SetMarker(D3D12DDI_HCOMMANDLIST, UINT64) {}
static void APIENTRY WriteBufferImmediate(D3D12DDI_HCOMMANDLIST hl, UINT n, const D3D12DDI_WRITEBUFFERIMMEDIATE_PARAMETER_0032* p, const D3D12DDI_WRITEBUFFERIMMEDIATE_MODE_0032* m)
{
    static_assert(sizeof(D3D12_WRITEBUFFERIMMEDIATE_PARAMETER) == sizeof(D3D12DDI_WRITEBUFFERIMMEDIATE_PARAMETER_0032), "wbi layout");
    ID3D12GraphicsCommandList2* l2 = NULL;
    if (SUCCEEDED(List(hl)->l->QueryInterface(IID_ID3D12GraphicsCommandList2, (void**)&l2))) {
        l2->WriteBufferImmediate(n, (const D3D12_WRITEBUFFERIMMEDIATE_PARAMETER*)p, (const D3D12_WRITEBUFFERIMMEDIATE_MODE*)m);
        l2->Release();
    }
}
static UINT APIENTRY GetPresentPrivateDriverDataSize(D3D12DDI_HDEVICE, const D3D12DDIARG_PRESENT_0001*) { return 0; }
static void FillPhase4Core(D3D12DDI_DEVICE_FUNCS_CORE_0075* f)
{
    f->pfnCreateShaderResourceView = CreateShaderResourceView;
    f->pfnCreateUnorderedAccessView = CreateUnorderedAccessView;
    f->pfnCreateDepthStencilView = CreateDepthStencilView;
    f->pfnCopyDescriptors = CopyDescriptors;
    f->pfnCopyDescriptorsSimple = CopyDescriptorsSimple;
    f->pfnCalcPrivateQueryHeapSize = CalcPrivateQueryHeapSize;
    f->pfnCreateQueryHeap = CreateQueryHeap;
    f->pfnDestroyQueryHeap = DestroyQueryHeap;
    f->pfnCalcPrivateCommandSignatureSize = CalcPrivateCommandSignatureSize;
    f->pfnCreateCommandSignature = CreateCommandSignature;
    f->pfnDestroyCommandSignature = DestroyCommandSignature;
    f->pfnGetPresentPrivateDriverDataSize = GetPresentPrivateDriverDataSize;
}
static void FillPhase4List(D3D12DDI_COMMAND_LIST_FUNCS_3D_0074* f)
{
    f->pfnBeginQuery = BeginQuery;
    f->pfnEndQuery = EndQuery;
    f->pfnResolveQueryData = ResolveQueryData;
    f->pfnExecuteIndirect = ExecuteIndirect;
    f->pfnExecuteBundle = ExecuteBundle;
    f->pfnClearUnorderedAccessViewUint = ClearUavUint;
    f->pfnClearUnorderedAccessViewFloat = ClearUavFloat;
    f->pfnResourceResolveSubresource = ResolveSubresource;
    f->pfnDiscardResource = DiscardResource;
    f->pfnSetMarker = SetMarker;
    f->pfnWriteBufferImmediate = WriteBufferImmediate;
}

// ---- pipeline libraries: kept empty. A library load still comes with the application's whole description (the
// runtime checks it against the stored one), so CreatePipelineState builds the pipeline as usual and ignores the
// library reference; nothing is cached across runs. A blob that is not ours gets DRIVER_VERSION_MISMATCH, which
// programs answer by starting a new, empty library.
struct P5PipelineLibrary { UINT count; };
static const UINT64 kLibMagic = 0x3142494C35355350ull;   // "PS55LIB1"
static SIZE_T APIENTRY CalcPrivatePipelineLibrarySize(D3D12DDI_HDEVICE, const D3D12DDIARG_CREATE_PIPELINE_LIBRARY_0010*) { return sizeof(P5PipelineLibrary); }
static HRESULT APIENTRY CreatePipelineLibrary(D3D12DDI_HDEVICE, const D3D12DDIARG_CREATE_PIPELINE_LIBRARY_0010* a, D3D12DDI_HPIPELINELIBRARY h)
{
    new (h.pDrvPrivate) P5PipelineLibrary();
    if (a->InitialDataSize && (a->InitialDataSize < 16 || *(const UINT64*)a->pInitialData != kLibMagic)) {
        P5Log("CreatePipelineLibrary: %zu-byte blob not ours -> DRIVER_VERSION_MISMATCH", a->InitialDataSize);
        return D3D12_ERROR_DRIVER_VERSION_MISMATCH;
    }
    return S_OK;
}
static void APIENTRY DestroyPipelineLibrary(D3D12DDI_HDEVICE, D3D12DDI_HPIPELINELIBRARY h) { Obj<P5PipelineLibrary>(h)->~P5PipelineLibrary(); }
static HRESULT APIENTRY AddPipelineStateToLibrary(D3D12DDI_HDEVICE, D3D12DDI_HPIPELINELIBRARY h, D3D12DDI_HPIPELINESTATE, UINT) { Obj<P5PipelineLibrary>(h)->count++; return S_OK; }
static SIZE_T APIENTRY CalcSerializedLibrarySize(D3D12DDI_HDEVICE, D3D12DDI_HPIPELINELIBRARY) { return 16; }
static HRESULT APIENTRY SerializeLibrary(D3D12DDI_HDEVICE, D3D12DDI_HPIPELINELIBRARY h, void* blob)
{
    UINT64 v[2] = {kLibMagic, Obj<P5PipelineLibrary>(h)->count};
    memcpy(blob, v, 16);
    return S_OK;
}
static void FillPipelineLibrary(D3D12DDI_DEVICE_FUNCS_CORE_0075* f)
{
    f->pfnCalcPrivatePipelineLibrarySize = CalcPrivatePipelineLibrarySize;
    f->pfnCreatePipelineLibrary = CreatePipelineLibrary;
    f->pfnDestroyPipelineLibrary = DestroyPipelineLibrary;
    f->pfnAddPipelineStateToLibrary = AddPipelineStateToLibrary;
    f->pfnCalcSerializedLibrarySize = CalcSerializedLibrarySize;
    f->pfnSerializeLibrary = SerializeLibrary;
}

// ---- DDI 0088+: enhanced barriers (ID3D12GraphicsCommandList7::Barrier); the DDI structs match the API's ----
// 0088 and 0094 differ only in the ranged barrier (runtime-internal); either becomes a global barrier here.
static void RangedToGlobal(const D3D12DDI_RANGED_BARRIER_0088& s, std::vector<D3D12_GLOBAL_BARRIER>& g)
{
    g.push_back({(D3D12_BARRIER_SYNC)s.SyncBefore, (D3D12_BARRIER_SYNC)s.SyncAfter, (D3D12_BARRIER_ACCESS)s.AccessBefore, (D3D12_BARRIER_ACCESS)s.AccessAfter});
}
static void RangedToGlobal(const D3D12DDI_RANGED_BARRIER_0094&, std::vector<D3D12_GLOBAL_BARRIER>& g)
{
    g.push_back({D3D12_BARRIER_SYNC_ALL, D3D12_BARRIER_SYNC_ALL, D3D12_BARRIER_ACCESS_COMMON, D3D12_BARRIER_ACCESS_COMMON});
}
template <class T> static void BarrierT(D3D12DDI_HCOMMANDLIST hl, UINT32 n, const T* b)
{
    P5Trace("Barrier (enhanced)");
    ID3D12GraphicsCommandList7* l7 = NULL;
    if (FAILED(List(hl)->l->QueryInterface(IID_ID3D12GraphicsCommandList7, (void**)&l7))) { P5Log("Barrier: vkd3d list has no ID3D12GraphicsCommandList7"); return; }
    std::vector<D3D12_GLOBAL_BARRIER> g;
    std::vector<D3D12_TEXTURE_BARRIER> t;
    std::vector<D3D12_BUFFER_BARRIER> bu;
    for (UINT32 i = 0; i < n; ++i) {
        const T& x = b[i];
        if (x.Type == D3D12DDI_BARRIER_TYPE_GLOBAL) {
            const D3D12DDI_GLOBAL_BARRIER_0088& s = x.GlobalBarrier;
            g.push_back({(D3D12_BARRIER_SYNC)s.SyncBefore, (D3D12_BARRIER_SYNC)s.SyncAfter, (D3D12_BARRIER_ACCESS)s.AccessBefore, (D3D12_BARRIER_ACCESS)s.AccessAfter});
        } else if (x.Type == D3D12DDI_BARRIER_TYPE_TEXTURE) {
            const D3D12DDI_TEXTURE_BARRIER_0088& s = x.TextureBarrier;
            D3D12_TEXTURE_BARRIER o = {};
            o.SyncBefore = (D3D12_BARRIER_SYNC)s.SyncBefore; o.SyncAfter = (D3D12_BARRIER_SYNC)s.SyncAfter;
            o.AccessBefore = (D3D12_BARRIER_ACCESS)s.AccessBefore; o.AccessAfter = (D3D12_BARRIER_ACCESS)s.AccessAfter;
            o.LayoutBefore = (D3D12_BARRIER_LAYOUT)s.LayoutBefore; o.LayoutAfter = (D3D12_BARRIER_LAYOUT)s.LayoutAfter;
            o.pResource = Res(s.hResource);
            memcpy(&o.Subresources, &s.Subresources, sizeof o.Subresources);
            o.Flags = (D3D12_TEXTURE_BARRIER_FLAGS)s.Flags;
            if (o.pResource) t.push_back(o);
        } else if (x.Type == D3D12DDI_BARRIER_TYPE_BUFFER) {
            const D3D12DDI_BUFFER_BARRIER_0088& s = x.BufferBarrier;
            D3D12_BUFFER_BARRIER o = {(D3D12_BARRIER_SYNC)s.SyncBefore, (D3D12_BARRIER_SYNC)s.SyncAfter, (D3D12_BARRIER_ACCESS)s.AccessBefore,
                                      (D3D12_BARRIER_ACCESS)s.AccessAfter, Res(s.hResource), 0, UINT64_MAX};
            if (o.pResource) bu.push_back(o);
        } else RangedToGlobal(x.RangedBarrier, g);
    }
    D3D12_BARRIER_GROUP grp[3];
    UINT k = 0;
    if (!g.empty()) { grp[k].Type = D3D12_BARRIER_TYPE_GLOBAL; grp[k].NumBarriers = (UINT32)g.size(); grp[k].pGlobalBarriers = g.data(); ++k; }
    if (!t.empty()) { grp[k].Type = D3D12_BARRIER_TYPE_TEXTURE; grp[k].NumBarriers = (UINT32)t.size(); grp[k].pTextureBarriers = t.data(); ++k; }
    if (!bu.empty()) { grp[k].Type = D3D12_BARRIER_TYPE_BUFFER; grp[k].NumBarriers = (UINT32)bu.size(); grp[k].pBufferBarriers = bu.data(); ++k; }
    if (k) l7->Barrier(k, grp);
    l7->Release();
}
static void APIENTRY Barrier88(D3D12DDI_HCOMMANDLIST hl, UINT32 n, const D3D12DDIARG_BARRIER_0088* b) { BarrierT(hl, n, b); }
static void APIENTRY Barrier94(D3D12DDI_HCOMMANDLIST hl, UINT32 n, const D3D12DDIARG_BARRIER_0094* b) { BarrierT(hl, n, b); }
// DDI 0092: alpha blend factor (separate alpha in the blend factor). Not forwarded yet: logged.
static void APIENTRY OMSetAlphaBlendFactor92(D3D12DDI_HCOMMANDLIST, FLOAT a)
{
    static LONG said;
    if (InterlockedIncrement(&said) <= 3) P5Log("OMSetAlphaBlendFactor %f: not forwarded", a);
}
