// Draw/dispatch state and commands (included by ps5d3d12.cpp): one-to-one with the API command list.
#define L(h) List(h)->l
static void APIENTRY SetPipelineState(D3D12DDI_HCOMMANDLIST hl, D3D12DDI_HPIPELINESTATE hp)
{
    P5Pipeline* p = Obj<P5Pipeline>(hp);
    if (L(hl) && p && p->pso) L(hl)->SetPipelineState(p->pso);
}
static void APIENTRY IaSetTopology(D3D12DDI_HCOMMANDLIST hl, D3D12DDI_PRIMITIVE_TOPOLOGY t) { L(hl)->IASetPrimitiveTopology((D3D12_PRIMITIVE_TOPOLOGY)t); }
static void APIENTRY SetDescriptorHeaps(D3D12DDI_HCOMMANDLIST hl, UINT n, D3D12DDI_HDESCRIPTORHEAP* h)
{
    ID3D12DescriptorHeap* v[4];
    UINT k = 0;
    for (UINT i = 0; i < n && k < 4; ++i) if (P5DescHeap* d = Obj<P5DescHeap>(h[i])) v[k++] = d->h;
    L(hl)->SetDescriptorHeaps(k, v);
}
static void APIENTRY SetComputeRootSignature(D3D12DDI_HCOMMANDLIST hl, D3D12DDI_HROOTSIGNATURE h)
{
    if (P5RootSignature* r = Obj<P5RootSignature>(h)) L(hl)->SetComputeRootSignature(r->rs);
}
static void APIENTRY SetGraphicsRootSignature(D3D12DDI_HCOMMANDLIST hl, D3D12DDI_HROOTSIGNATURE h)
{
    if (P5RootSignature* r = Obj<P5RootSignature>(h)) L(hl)->SetGraphicsRootSignature(r->rs);
}
static void APIENTRY SetComputeRootDescriptorTable(D3D12DDI_HCOMMANDLIST hl, UINT i, D3D12DDI_GPU_DESCRIPTOR_HANDLE b) { L(hl)->SetComputeRootDescriptorTable(i, {b.ptr}); }
static void APIENTRY SetGraphicsRootDescriptorTable(D3D12DDI_HCOMMANDLIST hl, UINT i, D3D12DDI_GPU_DESCRIPTOR_HANDLE b) { L(hl)->SetGraphicsRootDescriptorTable(i, {b.ptr}); }
static void APIENTRY SetComputeRoot32BitConstant(D3D12DDI_HCOMMANDLIST hl, UINT i, UINT v, UINT o) { L(hl)->SetComputeRoot32BitConstant(i, v, o); }
static void APIENTRY SetGraphicsRoot32BitConstant(D3D12DDI_HCOMMANDLIST hl, UINT i, UINT v, UINT o) { L(hl)->SetGraphicsRoot32BitConstant(i, v, o); }
static void APIENTRY SetComputeRoot32BitConstants(D3D12DDI_HCOMMANDLIST hl, UINT i, UINT n, const void* p, UINT o) { L(hl)->SetComputeRoot32BitConstants(i, n, p, o); }
static void APIENTRY SetGraphicsRoot32BitConstants(D3D12DDI_HCOMMANDLIST hl, UINT i, UINT n, const void* p, UINT o) { L(hl)->SetGraphicsRoot32BitConstants(i, n, p, o); }
static void APIENTRY SetComputeRootCbv(D3D12DDI_HCOMMANDLIST hl, UINT i, D3D12DDI_GPU_VIRTUAL_ADDRESS a) { L(hl)->SetComputeRootConstantBufferView(i, a); }
static void APIENTRY SetGraphicsRootCbv(D3D12DDI_HCOMMANDLIST hl, UINT i, D3D12DDI_GPU_VIRTUAL_ADDRESS a) { L(hl)->SetGraphicsRootConstantBufferView(i, a); }
static void APIENTRY SetComputeRootSrv(D3D12DDI_HCOMMANDLIST hl, UINT i, D3D12DDI_GPU_VIRTUAL_ADDRESS a) { L(hl)->SetComputeRootShaderResourceView(i, a); }
static void APIENTRY SetGraphicsRootSrv(D3D12DDI_HCOMMANDLIST hl, UINT i, D3D12DDI_GPU_VIRTUAL_ADDRESS a) { L(hl)->SetGraphicsRootShaderResourceView(i, a); }
static void APIENTRY SetComputeRootUav(D3D12DDI_HCOMMANDLIST hl, UINT i, D3D12DDI_GPU_VIRTUAL_ADDRESS a) { L(hl)->SetComputeRootUnorderedAccessView(i, a); }
static void APIENTRY SetGraphicsRootUav(D3D12DDI_HCOMMANDLIST hl, UINT i, D3D12DDI_GPU_VIRTUAL_ADDRESS a) { L(hl)->SetGraphicsRootUnorderedAccessView(i, a); }
static void APIENTRY IASetIndexBuffer(D3D12DDI_HCOMMANDLIST hl, const D3D12DDI_INDEX_BUFFER_VIEW* v)
{
    D3D12_INDEX_BUFFER_VIEW a = {};
    if (v) a = {v->BufferLocation, v->SizeInBytes, v->Format};
    L(hl)->IASetIndexBuffer(v ? &a : NULL);
}
static void APIENTRY IASetVertexBuffers(D3D12DDI_HCOMMANDLIST hl, UINT start, UINT n, const D3D12DDI_VERTEX_BUFFER_VIEW* v)
{
    static_assert(sizeof(D3D12_VERTEX_BUFFER_VIEW) == sizeof(D3D12DDI_VERTEX_BUFFER_VIEW), "vbv");
    L(hl)->IASetVertexBuffers(start, n, (const D3D12_VERTEX_BUFFER_VIEW*)v);
}
static void APIENTRY SOSetTargets(D3D12DDI_HCOMMANDLIST hl, UINT start, UINT n, const D3D12DDI_STREAM_OUTPUT_BUFFER_VIEW* v)
{
    static_assert(sizeof(D3D12_STREAM_OUTPUT_BUFFER_VIEW) == sizeof(D3D12DDI_STREAM_OUTPUT_BUFFER_VIEW), "sov");
    L(hl)->SOSetTargets(start, n, (const D3D12_STREAM_OUTPUT_BUFFER_VIEW*)v);
}
static void APIENTRY OMSetRenderTargets(D3D12DDI_HCOMMANDLIST hl, UINT n, const D3D12DDI_CPU_DESCRIPTOR_HANDLE* rt, BOOL single, const D3D12DDI_CPU_DESCRIPTOR_HANDLE* ds)
{
    L(hl)->OMSetRenderTargets(n, (const D3D12_CPU_DESCRIPTOR_HANDLE*)rt, single, (const D3D12_CPU_DESCRIPTOR_HANDLE*)ds);
}
static void APIENTRY RsSetViewports(D3D12DDI_HCOMMANDLIST hl, UINT n, const D3D12DDI_VIEWPORT* v) { L(hl)->RSSetViewports(n, (const D3D12_VIEWPORT*)v); }
static void APIENTRY RsSetScissorRects(D3D12DDI_HCOMMANDLIST hl, UINT n, const D3D12DDI_RECT* r) { L(hl)->RSSetScissorRects(n, (const D3D12_RECT*)r); }
static void APIENTRY OmSetBlendFactor(D3D12DDI_HCOMMANDLIST hl, const FLOAT f[4]) { L(hl)->OMSetBlendFactor(f); }
static void APIENTRY OmSetStencilRef(D3D12DDI_HCOMMANDLIST hl, UINT r) { L(hl)->OMSetStencilRef(r); }
static void APIENTRY OMSetDepthBounds(D3D12DDI_HCOMMANDLIST hl, FLOAT mn, FLOAT mx)
{
    ID3D12GraphicsCommandList1* l1 = NULL;
    if (SUCCEEDED(L(hl)->QueryInterface(IID_ID3D12GraphicsCommandList1, (void**)&l1))) { l1->OMSetDepthBounds(mn, mx); l1->Release(); }
}
static void APIENTRY DrawInstanced(D3D12DDI_HCOMMANDLIST hl, UINT a, UINT b, UINT c, UINT d) { L(hl)->DrawInstanced(a, b, c, d); }
static void APIENTRY DrawIndexedInstanced(D3D12DDI_HCOMMANDLIST hl, UINT a, UINT b, UINT c, INT d, UINT e) { L(hl)->DrawIndexedInstanced(a, b, c, d, e); }
static void APIENTRY Dispatch(D3D12DDI_HCOMMANDLIST hl, UINT x, UINT y, UINT z) { L(hl)->Dispatch(x, y, z); }
static void APIENTRY ClearDepthStencilView(D3D12DDI_HCOMMANDLIST hl, D3D12DDI_CPU_DESCRIPTOR_HANDLE v, UINT flags, FLOAT d, UINT8 s, UINT n, const D3D12DDI_RECT* r)
{
    L(hl)->ClearDepthStencilView({v.ptr}, (D3D12_CLEAR_FLAGS)flags, d, s, n, (const D3D12_RECT*)r);
}
static void APIENTRY ClearRootArguments(D3D12DDI_HCOMMANDLIST) {}
static void APIENTRY SetPredication(D3D12DDI_HCOMMANDLIST hl, D3D12DDI_HRESOURCE r, UINT64 off, D3D12DDI_PREDICATION_OP op)
{
    L(hl)->SetPredication(Res(r), off, (D3D12_PREDICATION_OP)op);
}
static void FillDrawFuncs(D3D12DDI_COMMAND_LIST_FUNCS_3D_0074* f)
{
    f->pfnSetPipelineState = SetPipelineState;
    f->pfnIaSetTopology = IaSetTopology;
    f->pfnSetDescriptorHeaps = SetDescriptorHeaps;
    f->pfnSetComputeRootSignature = SetComputeRootSignature;
    f->pfnSetGraphicsRootSignature = SetGraphicsRootSignature;
    f->pfnSetComputeRootDescriptorTable = SetComputeRootDescriptorTable;
    f->pfnSetGraphicsRootDescriptorTable = SetGraphicsRootDescriptorTable;
    f->pfnSetComputeRoot32BitConstant = SetComputeRoot32BitConstant;
    f->pfnSetGraphicsRoot32BitConstant = SetGraphicsRoot32BitConstant;
    f->pfnSetComputeRoot32BitConstants = SetComputeRoot32BitConstants;
    f->pfnSetGraphicsRoot32BitConstants = SetGraphicsRoot32BitConstants;
    f->pfnSetComputeRootConstantBufferView = SetComputeRootCbv;
    f->pfnSetGraphicsRootConstantBufferView = SetGraphicsRootCbv;
    f->pfnSetComputeRootShaderResourceView = SetComputeRootSrv;
    f->pfnSetGraphicsRootShaderResourceView = SetGraphicsRootSrv;
    f->pfnSetComputeRootUnorderedAccessView = SetComputeRootUav;
    f->pfnSetGraphicsRootUnorderedAccessView = SetGraphicsRootUav;
    f->pfnIASetIndexBuffer = IASetIndexBuffer;
    f->pfnIASetVertexBuffers = IASetVertexBuffers;
    f->pfnSOSetTargets = SOSetTargets;
    f->pfnOMSetRenderTargets = OMSetRenderTargets;
    f->pfnRsSetViewports = RsSetViewports;
    f->pfnRsSetScissorRects = RsSetScissorRects;
    f->pfnOmSetBlendFactor = OmSetBlendFactor;
    f->pfnOmSetStencilRef = OmSetStencilRef;
    f->pfnOMSetDepthBounds = OMSetDepthBounds;
    f->pfnDrawInstanced = DrawInstanced;
    f->pfnDrawIndexedInstanced = DrawIndexedInstanced;
    f->pfnDispatch = Dispatch;
    f->pfnClearDepthStencilView = ClearDepthStencilView;
    f->pfnClearRootArguments = ClearRootArguments;
    f->pfnSetPredication = SetPredication;
}
#undef L
