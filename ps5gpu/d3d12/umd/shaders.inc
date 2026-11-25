// Shaders, fixed-function state objects and pipeline states (included by ps5d3d12.cpp).
//
// The DDI hands a shader over as its bare program (DXBC tokens: version, length in dwords, ...) plus input/output
// signatures as arrays of {system value, register, mask, component type} without semantic names. vkd3d-proton wants
// a whole DXBC container, so one is rebuilt here: ISGN/OSGN from the arrays (system values get their SV_ names,
// everything else the name "P5R" with semantic index = register * 4 + first component, which the element layout
// repeats for vertex inputs), the program as SHDR (4.x) or SHEX (5.x), and the container checksum (DXBC's MD5
// variant; checked against the four containers inside 19041's D3D12Core.dll).



// ---- DXBC checksum ----
static uint32_t Rol(uint32_t x, int c) { return (x << c) | (x >> (32 - c)); }
static void Md5Block(uint32_t st[4], const uint8_t* p)
{
    static const int S[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
                              4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
    static uint32_t K[64];
    if (!K[0]) for (int i = 0; i < 64; ++i) K[i] = (uint32_t)(fabs(sin((double)(i + 1))) * 4294967296.0);
    uint32_t M[16];
    memcpy(M, p, 64);
    uint32_t a = st[0], b = st[1], c = st[2], d = st[3];
    for (int i = 0; i < 64; ++i) {
        uint32_t f; int g;
        if (i < 16) { f = (b & c) | (~b & d); g = i; }
        else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) % 16; }
        else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) % 16; }
        else { f = c ^ (b | ~d); g = (7 * i) % 16; }
        f += a + K[i] + M[g];
        a = d; d = c; c = b; b += Rol(f, S[i]);
    }
    st[0] += a; st[1] += b; st[2] += c; st[3] += d;
}
static void DxbcChecksum(std::vector<uint8_t>& blob)
{
    const uint8_t* data = blob.data() + 20;
    uint32_t n = (uint32_t)blob.size() - 20, bits = n * 8, st[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    uint32_t full = n / 64 * 64, tail = n - full;
    for (uint32_t i = 0; i < full; i += 64) Md5Block(st, data + i);
    uint8_t blk[64];
    uint32_t last = (bits >> 2) | 1;
    if (tail >= 56) {
        memset(blk, 0, 64); memcpy(blk, data + full, tail); blk[tail] = 0x80; Md5Block(st, blk);
        memset(blk, 0, 64); memcpy(blk, &bits, 4); memcpy(blk + 60, &last, 4); Md5Block(st, blk);
    } else {
        memset(blk, 0, 64); memcpy(blk, &bits, 4); memcpy(blk + 4, data + full, tail); blk[4 + tail] = 0x80; memcpy(blk + 60, &last, 4); Md5Block(st, blk);
    }
    memcpy(blob.data() + 4, st, 16);
}

// ---- signatures ----
struct P5SigName { const char* name; UINT sysval; UINT index; };
static P5SigName SigName(UINT sb, UINT reg, BYTE mask, bool psOut)
{
    switch (sb) {
    case 1: return {"SV_Position", 1, 0};
    case 2: return {"SV_ClipDistance", 2, ~0u};
    case 3: return {"SV_CullDistance", 3, ~0u};
    case 4: return {"SV_RenderTargetArrayIndex", 4, 0};
    case 5: return {"SV_ViewportArrayIndex", 5, 0};
    case 6: return {"SV_VertexID", 6, 0};
    case 7: return {"SV_PrimitiveID", 7, 0};
    case 8: return {"SV_InstanceID", 8, 0};
    case 9: return {"SV_IsFrontFace", 9, 0};
    case 10: return {"SV_SampleIndex", 10, 0};
    case 11: case 12: case 13: case 14: return {"SV_TessFactor", 11, sb - 11};
    case 15: case 16: return {"SV_InsideTessFactor", 12, sb - 15};
    case 17: case 18: case 19: return {"SV_TessFactor", 13, sb - 17};
    case 20: return {"SV_InsideTessFactor", 14, 0};
    case 21: return {"SV_TessFactor", 15, 0};
    case 22: return {"SV_TessFactor", 16, 1};
    case 23: return {"SV_Barycentrics", 23, 0};
    case 24: return {"SV_ShadingRate", 24, 0};
    case 25: return {"SV_CullPrimitive", 25, 0};
    default: break;
    }
    if (psOut) {
        if (reg == 0xFFFFFFFF) return {"SV_Depth", 65, 0};
        return {"SV_Target", 64, reg};
    }
    UINT first = 0;
    while (first < 4 && !(mask & (1 << first))) ++first;
    return {"P5R", 0, reg * 4 + (first & 3)};
}
// One ISGN/OSGN chunk body (24-byte elements, names after them).
static std::vector<uint8_t> SigChunk(const D3D12DDIARG_SIGNATURE_ENTRY_0012* e, UINT n, bool output, bool psOut)
{
    struct Elem { uint32_t nameOff, index, sysval, compType, reg; uint8_t mask, rw, pad0, pad1; };
    static_assert(sizeof(Elem) == 24, "signature element");
    std::vector<Elem> el(n);
    std::string names;
    std::vector<std::pair<std::string, uint32_t>> nameOffs;
    UINT clipIndex = 0, cullIndex = 0;
    for (UINT i = 0; i < n; ++i) {
        P5SigName s = SigName(e[i].SystemValue, e[i].Register, e[i].Mask, psOut);
        UINT index = s.index;
        if (s.index == ~0u) index = s.sysval == 2 ? clipIndex++ : cullIndex++;
        uint32_t off = ~0u;
        for (auto& p : nameOffs) if (p.first == s.name) off = p.second;
        if (off == ~0u) {
            off = (uint32_t)names.size();
            nameOffs.push_back({s.name, off});
            names += s.name;
            names += '\0';
        }
        el[i] = {off, index, s.sysval, (uint32_t)e[i].RegisterComponentType, e[i].Register, (uint8_t)(e[i].Mask & 0xF),
                 (uint8_t)(output ? 0 : (e[i].Mask & 0xF)), 0, 0};
    }
    uint32_t base = 8 + 24 * n;
    for (auto& x : el) x.nameOff += base;
    std::vector<uint8_t> b(base + names.size());
    uint32_t head[2] = {n, 8};
    memcpy(b.data(), head, 8);
    if (n) memcpy(b.data() + 8, el.data(), 24 * n);
    memcpy(b.data() + base, names.data(), names.size());
    while (b.size() & 3) b.push_back(0xAB);
    return b;
}
static std::vector<uint8_t> BuildDxbc(const UINT* code, const D3D12DDIARG_STAGE_IO_SIGNATURES* sig)
{
    UINT version = code[0], dwords = code[1], type = version >> 16, major = (version >> 4) & 0xF;
    bool ps = type == 0;
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> chunks;
    chunks.push_back({0x4E475349 /* ISGN */, SigChunk(sig ? sig->pInputSignature : NULL, sig ? sig->NumInputSignatureEntries : 0, false, false)});
    chunks.push_back({0x4E47534F /* OSGN */, SigChunk(sig ? sig->pOutputSignature : NULL, sig ? sig->NumOutputSignatureEntries : 0, true, ps)});
    std::vector<uint8_t> prog(dwords * 4);
    memcpy(prog.data(), code, dwords * 4);
    chunks.push_back({major >= 5 ? 0x58454853u /* SHEX */ : 0x52444853u /* SHDR */, prog});
    uint32_t count = (uint32_t)chunks.size(), off = 32 + 4 * count;
    std::vector<uint8_t> blob(off);
    std::vector<uint32_t> offs;
    for (auto& c : chunks) {
        offs.push_back((uint32_t)blob.size());
        uint32_t hdr[2] = {c.first, (uint32_t)c.second.size()};
        blob.insert(blob.end(), (uint8_t*)hdr, (uint8_t*)hdr + 8);
        blob.insert(blob.end(), c.second.begin(), c.second.end());
    }
    uint32_t head[8] = {0x43425844 /* DXBC */, 0, 0, 0, 0, 1, (uint32_t)blob.size(), count};
    memcpy(blob.data(), head, 32);
    memcpy(blob.data() + 32, offs.data(), 4 * count);
    DxbcChecksum(blob);
    return blob;
}

// DXIL (shader model 6): the DDI program is the container's DXIL part as is (program header, bitcode header, module),
// plus ISGN/OSGN from the DDI signatures (same synthesized names as for DXBC) so vkd3d-proton can match the input layout.
static std::vector<uint8_t> BuildDxilContainer(const UINT* code, const D3D12DDIARG_STAGE_IO_SIGNATURES* sig)
{
    UINT dwords = code[1], type = code[0] >> 16;
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> chunks;
    // Only the vertex shader's input signature (for matching the input layout); synthesized signatures on other
    // stages made RADV's compiler crash on UE4 shaders (dxil-spirv reads the module's own metadata anyway).
    if (sig && type == 1)
        chunks.push_back({0x4E475349 /* ISGN */, SigChunk(sig->pInputSignature, sig->NumInputSignatureEntries, false, false)});
    std::vector<uint8_t> prog(dwords * 4);
    memcpy(prog.data(), code, dwords * 4);
    chunks.push_back({0x4C495844 /* DXIL */, prog});
    uint32_t count = (uint32_t)chunks.size(), off = 32 + 4 * count;
    std::vector<uint8_t> blob(off);
    std::vector<uint32_t> offs;
    for (auto& c : chunks) {
        offs.push_back((uint32_t)blob.size());
        uint32_t hdr[2] = {c.first, (uint32_t)c.second.size()};
        blob.insert(blob.end(), (uint8_t*)hdr, (uint8_t*)hdr + 8);
        blob.insert(blob.end(), c.second.begin(), c.second.end());
    }
    uint32_t head[8] = {0x43425844 /* DXBC */, 0, 0, 0, 0, 1, (uint32_t)blob.size(), count};
    memcpy(blob.data(), head, 32);
    memcpy(blob.data() + 32, offs.data(), 4 * count);
    DxbcChecksum(blob);
    return blob;
}

// ---- shader objects ----
struct P5Shader {
    std::vector<uint8_t> container;
    UINT version;
    bool dxil;
    uint32_t id;                               // FNV-1a of the container (dump file name)
};
// P5D3D12_DUMP=1: every container goes to C:\ps5\gpu\umdlog\sh\<id>.cso (finding shaders that break compilers).
static uint32_t Fnv(const std::vector<uint8_t>& b)
{
    uint32_t h = 2166136261u;
    for (uint8_t c : b) h = (h ^ c) * 16777619u;
    return h;
}
static bool DumpOn()
{
    static int on = -1;
    if (on < 0) { char v[8] = ""; on = GetEnvironmentVariableA("P5D3D12_DUMP", v, sizeof v) && v[0] == '1'; if (on) CreateDirectoryA("C:\\ps5\\gpu\\umdlog\\sh", NULL); }
    return on > 0;
}
static SIZE_T APIENTRY CalcPrivateShaderSize(D3D12DDI_HDEVICE, const D3D12DDIARG_CREATE_SHADER_0026*) { return sizeof(P5Shader); }
static void APIENTRY CreateShader(D3D12DDI_HDEVICE, const D3D12DDIARG_CREATE_SHADER_0026* a, D3D12DDI_HSHADER hs)
{
    P5Shader* s = new (hs.pDrvPrivate) P5Shader();
    const UINT* code = a->pShaderCode;
    s->version = code[0];
    UINT major = (code[0] >> 4) & 0xF;
    s->dxil = major >= 6;
    if (s->dxil) s->container = BuildDxilContainer(code, a->IOSignatures.Standard);       // signatures live in the module's metadata
    else s->container = BuildDxbc(code, a->IOSignatures.Standard);
    s->id = Fnv(s->container);
    if (DumpOn()) {
        char path[MAX_PATH];
        snprintf(path, sizeof path, "C:\\ps5\\gpu\\umdlog\\sh\\%08x.cso", s->id);
        if (FILE* f = fopen(path, "wb")) { fwrite(s->container.data(), 1, s->container.size(), f); fclose(f); }
    }
    static LONG said;
    if (InterlockedIncrement(&said) <= 40) {
        const D3D12DDIARG_STAGE_IO_SIGNATURES* g = a->IOSignatures.Standard;
        P5Log("CreateShader: program %08X, %u dwords, %u in / %u out -> container %zu bytes", code[0], code[1],
            g ? g->NumInputSignatureEntries : 0, g ? g->NumOutputSignatureEntries : 0, s->container.size());
    }
}
static void APIENTRY DestroyShader(D3D12DDI_HDEVICE, D3D12DDI_HSHADER h) { Obj<P5Shader>(h)->~P5Shader(); }

// ---- element layouts and fixed-function state (kept as the API's descriptions) ----
struct P5ElementLayout { std::vector<D3D12_INPUT_ELEMENT_DESC> el; };
static SIZE_T APIENTRY CalcPrivateElementLayoutSize(D3D12DDI_HDEVICE, const D3D12DDIARG_CREATEELEMENTLAYOUT_0010*) { return sizeof(P5ElementLayout); }
static void APIENTRY CreateElementLayout(D3D12DDI_HDEVICE, const D3D12DDIARG_CREATEELEMENTLAYOUT_0010* a, D3D12DDI_HELEMENTLAYOUT h)
{
    P5ElementLayout* l = new (h.pDrvPrivate) P5ElementLayout();
    for (UINT i = 0; i < a->NumElements; ++i) {
        const D3D12DDIARG_INPUT_ELEMENT_DESC& e = a->pVertexElements[i];
        D3D12_INPUT_ELEMENT_DESC d = {"P5R", e.InputRegister * 4, e.Format, e.InputSlot, e.AlignedByteOffset,
                                      (D3D12_INPUT_CLASSIFICATION)e.InputSlotClass, e.InstanceDataStepRate};
        l->el.push_back(d);
    }
}
static void APIENTRY DestroyElementLayout(D3D12DDI_HDEVICE, D3D12DDI_HELEMENTLAYOUT h) { Obj<P5ElementLayout>(h)->~P5ElementLayout(); }

struct P5Blend { D3D12_BLEND_DESC d; };
static SIZE_T APIENTRY CalcPrivateBlendStateSize(D3D12DDI_HDEVICE, const D3D12DDI_BLEND_DESC_0010*) { return sizeof(P5Blend); }
static void APIENTRY CreateBlendState(D3D12DDI_HDEVICE, const D3D12DDI_BLEND_DESC_0010* a, D3D12DDI_HBLENDSTATE h)
{
    P5Blend* b = new (h.pDrvPrivate) P5Blend();
    b->d.AlphaToCoverageEnable = a->AlphaToCoverageEnable;
    b->d.IndependentBlendEnable = a->IndependentBlendEnable;
    for (int i = 0; i < 8; ++i) {
        const D3D12DDI_RENDER_TARGET_BLEND_DESC& s = a->RenderTarget[i];
        D3D12_RENDER_TARGET_BLEND_DESC& d = b->d.RenderTarget[i];
        d.BlendEnable = s.BlendEnable; d.LogicOpEnable = s.LogicOpEnable;
        d.SrcBlend = (D3D12_BLEND)s.SrcBlend; d.DestBlend = (D3D12_BLEND)s.DestBlend; d.BlendOp = (D3D12_BLEND_OP)s.BlendOp;
        d.SrcBlendAlpha = (D3D12_BLEND)s.SrcBlendAlpha; d.DestBlendAlpha = (D3D12_BLEND)s.DestBlendAlpha; d.BlendOpAlpha = (D3D12_BLEND_OP)s.BlendOpAlpha;
        d.LogicOp = (D3D12_LOGIC_OP)s.LogicOp; d.RenderTargetWriteMask = s.RenderTargetWriteMask;
    }
}
static void APIENTRY DestroyBlendState(D3D12DDI_HDEVICE, D3D12DDI_HBLENDSTATE h) { Obj<P5Blend>(h)->~P5Blend(); }

struct P5Raster { D3D12_RASTERIZER_DESC d; };
static SIZE_T APIENTRY CalcPrivateRasterizerStateSize(D3D12DDI_HDEVICE, const D3D12DDI_RASTERIZER_DESC_0010*) { return sizeof(P5Raster); }
static void APIENTRY CreateRasterizerState(D3D12DDI_HDEVICE, const D3D12DDI_RASTERIZER_DESC_0010* a, D3D12DDI_HRASTERIZERSTATE h)
{
    P5Raster* r = new (h.pDrvPrivate) P5Raster();
    r->d = {(D3D12_FILL_MODE)a->FillMode, (D3D12_CULL_MODE)a->CullMode, a->FrontCounterClockwise, a->DepthBias, a->DepthBiasClamp,
            a->SlopeScaledDepthBias, a->DepthClipEnable, a->MultisampleEnable, a->AntialiasedLineEnable, a->ForcedSampleCount,
            (D3D12_CONSERVATIVE_RASTERIZATION_MODE)a->ConservativeRasterizationMode};
}
static void APIENTRY DestroyRasterizerState(D3D12DDI_HDEVICE, D3D12DDI_HRASTERIZERSTATE h) { Obj<P5Raster>(h)->~P5Raster(); }

struct P5DepthStencil { D3D12_DEPTH_STENCIL_DESC d; BOOL depthBounds; };
static SIZE_T APIENTRY CalcPrivateDepthStencilStateSize(D3D12DDI_HDEVICE, const D3D12DDI_DEPTH_STENCIL_DESC_0025*) { return sizeof(P5DepthStencil); }
static void APIENTRY CreateDepthStencilState(D3D12DDI_HDEVICE, const D3D12DDI_DEPTH_STENCIL_DESC_0025* a, D3D12DDI_HDEPTHSTENCILSTATE h)
{
    P5DepthStencil* s = new (h.pDrvPrivate) P5DepthStencil();
    auto op = [](const D3D12DDI_DEPTH_STENCILOP_DESC& o) {
        return D3D12_DEPTH_STENCILOP_DESC{(D3D12_STENCIL_OP)o.StencilFailOp, (D3D12_STENCIL_OP)o.StencilDepthFailOp, (D3D12_STENCIL_OP)o.StencilPassOp,
                                          (D3D12_COMPARISON_FUNC)o.StencilFunc};
    };
    s->d = {a->DepthEnable, (D3D12_DEPTH_WRITE_MASK)a->DepthWriteMask, (D3D12_COMPARISON_FUNC)a->DepthFunc, a->StencilEnable,
            a->StencilReadMask, a->StencilWriteMask, op(a->FrontFace), op(a->BackFace)};
    s->depthBounds = a->DepthBoundsTestEnable;
}
static void APIENTRY DestroyDepthStencilState(D3D12DDI_HDEVICE, D3D12DDI_HDEPTHSTENCILSTATE h) { Obj<P5DepthStencil>(h)->~P5DepthStencil(); }

// ---- pipeline states: the decomposed DDI description put back together ----
struct P5Pipeline { ID3D12PipelineState* pso; bool compute; ID3D12RootSignature* rs; };
static SIZE_T APIENTRY CalcPrivatePipelineStateSize(D3D12DDI_HDEVICE, const D3D12DDIARG_CREATE_PIPELINE_STATE_0075*) { return sizeof(P5Pipeline); }
static D3D12_SHADER_BYTECODE Bytecode(D3D12DDI_HSHADER h)
{
    P5Shader* s = Obj<P5Shader>(h);
    if (!s || s->container.empty()) return {NULL, 0};
    return {s->container.data(), s->container.size()};
}
static HRESULT APIENTRY CreatePipelineState(D3D12DDI_HDEVICE hd, const D3D12DDIARG_CREATE_PIPELINE_STATE_0075* a, D3D12DDI_HPIPELINESTATE hp, D3D12DDI_HRTPIPELINESTATE)
{
    P5Pipeline* p = new (hp.pDrvPrivate) P5Pipeline();
    P5RootSignature* rs = Obj<P5RootSignature>(a->hRootSignature);
    p->rs = rs ? rs->rs : NULL;
    HRESULT hr;
    if (a->hComputeShader.pDrvPrivate) {
        p->compute = true;
        D3D12_COMPUTE_PIPELINE_STATE_DESC c = {};
        c.pRootSignature = p->rs;
        c.CS = Bytecode(a->hComputeShader);
        if (DumpOn()) P5Log("PSO begin cs %08x rs %08x", Obj<P5Shader>(a->hComputeShader) ? Obj<P5Shader>(a->hComputeShader)->id : 0u, rs ? rs->id : 0u);
        hr = Dev(hd)->dev->CreateComputePipelineState(&c, IID_ID3D12PipelineState, (void**)&p->pso);
        P5Log("CreatePipelineState: compute, %zu-byte shader %08x -> %08lX", c.CS.BytecodeLength, Obj<P5Shader>(a->hComputeShader) ? Obj<P5Shader>(a->hComputeShader)->id : 0u, (unsigned long)hr);
        return hr;
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC g = {};
    g.pRootSignature = p->rs;
    g.VS = Bytecode(a->hVertexShader);
    g.PS = Bytecode(a->hPixelShader);
    g.DS = Bytecode(a->hDomainShader);
    g.HS = Bytecode(a->hHullShader);
    g.GS = Bytecode(a->hGeometryShader);
    if (P5Blend* b = Obj<P5Blend>(a->hBlendState)) g.BlendState = b->d;
    g.SampleMask = a->SampleMask;
    if (P5Raster* r = Obj<P5Raster>(a->hRasterizerState)) g.RasterizerState = r->d;
    if (P5DepthStencil* s = Obj<P5DepthStencil>(a->hDepthStencilState)) g.DepthStencilState = s->d;
    if (P5ElementLayout* l = Obj<P5ElementLayout>(a->hElementLayout)) g.InputLayout = {l->el.data(), (UINT)l->el.size()};
    g.IBStripCutValue = (D3D12_INDEX_BUFFER_STRIP_CUT_VALUE)a->IBStripCutValue;
    g.PrimitiveTopologyType = (D3D12_PRIMITIVE_TOPOLOGY_TYPE)a->PrimitiveTopologyType;
    g.NumRenderTargets = a->NumRenderTargets;
    for (int i = 0; i < 8; ++i) g.RTVFormats[i] = a->RTVFormats[i];
    g.DSVFormat = a->DSVFormat;
    g.SampleDesc = a->SampleDesc;
    if (!g.SampleDesc.Count) g.SampleDesc.Count = 1;
    if (DumpOn()) {
        auto id = [](D3D12DDI_HSHADER h) { P5Shader* x = Obj<P5Shader>(h); return x ? x->id : 0u; };
        char el[256] = "";
        for (UINT i = 0; i < g.InputLayout.NumElements && strlen(el) < 220; ++i)
            snprintf(el + strlen(el), sizeof el - strlen(el), "%s%u:%u:%u:%u", i ? "," : "", g.InputLayout.pInputElementDescs[i].SemanticIndex,
                     (UINT)g.InputLayout.pInputElementDescs[i].Format, g.InputLayout.pInputElementDescs[i].InputSlot, g.InputLayout.pInputElementDescs[i].AlignedByteOffset);
        char rts[64] = "";
        for (UINT i = 0; i < g.NumRenderTargets; ++i) snprintf(rts + strlen(rts), sizeof rts - strlen(rts), "%s%u", i ? "," : "", (UINT)g.RTVFormats[i]);
        P5Log("PSO begin vs %08x ps %08x gs %08x hs %08x ds %08x rs %08x rts %s dsv %u topo %u el %s", id(a->hVertexShader), id(a->hPixelShader), id(a->hGeometryShader),
              id(a->hHullShader), id(a->hDomainShader), rs ? rs->id : 0u, rts[0] ? rts : "-", (UINT)g.DSVFormat, (UINT)g.PrimitiveTopologyType, el[0] ? el : "-");
    }
    hr = Dev(hd)->dev->CreateGraphicsPipelineState(&g, IID_ID3D12PipelineState, (void**)&p->pso);
    static LONG said;
    if (DumpOn()) P5Log("PSO done vs %08x ps %08x -> %08lX", Obj<P5Shader>(a->hVertexShader) ? Obj<P5Shader>(a->hVertexShader)->id : 0u,
                         Obj<P5Shader>(a->hPixelShader) ? Obj<P5Shader>(a->hPixelShader)->id : 0u, (unsigned long)hr);
    if (FAILED(hr) || InterlockedIncrement(&said) <= 20)
        P5Log("CreatePipelineState: graphics VS %zu PS %zu GS %zu HS %zu DS %zu, %u RTs (fmt %u), DSV %u, topology %u, %u elements -> %08lX",
            g.VS.BytecodeLength, g.PS.BytecodeLength, g.GS.BytecodeLength, g.HS.BytecodeLength, g.DS.BytecodeLength, g.NumRenderTargets,
            (UINT)g.RTVFormats[0], (UINT)g.DSVFormat, (UINT)g.PrimitiveTopologyType, g.InputLayout.NumElements, (unsigned long)hr);
    return hr;
}
static void APIENTRY DestroyPipelineState(D3D12DDI_HDEVICE, D3D12DDI_HPIPELINESTATE h)
{
    P5Pipeline* p = Obj<P5Pipeline>(h);
    if (p->pso) p->pso->Release();
    p->~P5Pipeline();
}

// Residency: vkd3d-proton keeps its memory resident (it lives outside Windows' video memory manager).
static HRESULT APIENTRY MakeResident(D3D12DDI_HDEVICE, D3D12DDIARG_MAKERESIDENT_0001* a) { a->WaitMask = 0; return S_OK; }
static HRESULT APIENTRY Evict(D3D12DDI_HDEVICE, const D3D12DDIARG_EVICT*) { return S_OK; }
