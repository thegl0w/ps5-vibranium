// ps5d3d12.dll: the PS5 GPU's Direct3D 12 user-mode driver (phase 3 of windows-handoff/WDDM2-DX12-PLAN.md, design D2).
// Windows' own d3d12.dll loads it from the 4th UserModeDriverName entry (ps5wddm in WDDM 2.0 mode) and talks the
// Direct3D 12 driver interface, version 0076 (D3D12DDI_SUPPORTED_0076; tables: device core 0075, command list 0074,
// queue 0001, DXGI). Each call is forwarded to vkd3d-proton used as a library: its d3d12core.dll, shipped beside us as
// ps5vkd3d.dll (renamed: Windows' own D3D12Core.dll is already in the process) and loaded by full path, gives a real
// ID3D12Device on our Vulkan driver (RADV -> libps5gpu -> ps5gpu.sys). The mapping of every entry is in
// ps5gpu/d3d12/UMD-TABLE.md; entries not done yet are logging stubs (stubs.inc, tools/genstubs.py).
// Log: C:\ps5\gpu\umdlog\ps5d3d12-<exe>-<pid>.txt.
#include <vector>
#include <smmintrin.h>
#include <string>
#include <windows.h>
#include <initguid.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <winddk_compat.h>
#include <d3d10umddi.h>
#include <d3d12umddi.h>
#include <stdio.h>
#include <stdarg.h>
#include <new>
#include <math.h>
#include <stdint.h>

// ---- log ----
static FILE* g_log;
static CRITICAL_SECTION g_logLock;
static void P5Log(const char* f, ...)
{
    EnterCriticalSection(&g_logLock);
    if (!g_log) {
        char exe[MAX_PATH] = "", p[MAX_PATH];
        GetModuleFileNameA(NULL, exe, sizeof exe);
        const char* base = strrchr(exe, '\\');
        CreateDirectoryA("C:\\ps5\\gpu\\umdlog", NULL);
        snprintf(p, sizeof p, "C:\\ps5\\gpu\\umdlog\\ps5d3d12-%s-%lu.txt", base ? base + 1 : exe, GetCurrentProcessId());
        g_log = fopen(p, "a");
    }
    if (g_log) {
        va_list a;
        va_start(a, f);
        fprintf(g_log, "%6lu ", GetTickCount() % 1000000);
        vfprintf(g_log, f, a);
        va_end(a);
        fputc('\n', g_log);
        fflush(g_log);
    }
    LeaveCriticalSection(&g_logLock);
}
#define P5Trace(n) do { static LONG c_; if (InterlockedIncrement(&c_) <= 3) P5Log("call %s", n); } while (0)
struct P5StubEntry { const char* name; void* fn; };
static void P5Stub(const char* name)                       // first 3 calls of each stub are logged
{
    static struct { const char* n; LONG c; } seen[256];
    for (auto& s : seen) {
        if (s.n == name || !s.n) {
            s.n = name;
            if (InterlockedIncrement(&s.c) <= 3) P5Log("NOT IMPLEMENTED: %s", name);
            return;
        }
    }
}
#include "stubs.inc"

// ---- vkd3d-proton as a library ----
typedef HRESULT (WINAPI* PFN_GETINTERFACE)(REFCLSID, REFIID, void**);
static const GUID kClsidDeviceFactory = {0x114863bf, 0xc386, 0x4aee, {0xb3, 0x9d, 0x8f, 0x0b, 0xbb, 0x06, 0x29, 0x55}};   // CLSID_D3D12DeviceFactory
static HMODULE g_vkd3d;
static PFN_GETINTERFACE g_getInterface;
static BOOL LoadVkd3d()
{
    if (g_getInterface) return TRUE;
    wchar_t path[MAX_PATH];
    HMODULE self = NULL;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)&LoadVkd3d, &self);
    GetModuleFileNameW(self, path, MAX_PATH);
    wchar_t* slash = wcsrchr(path, L'\\');
    if (slash) wcscpy(slash + 1, L"ps5vkd3d.dll");
    g_vkd3d = LoadLibraryExW(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!g_vkd3d) { P5Log("ps5vkd3d.dll not loaded (%lu)", GetLastError()); return FALSE; }
    g_getInterface = (PFN_GETINTERFACE)GetProcAddress(g_vkd3d, "D3D12GetInterface");
    if (!g_getInterface) { P5Log("ps5vkd3d.dll has no D3D12GetInterface"); return FALSE; }
    return TRUE;
}
static HRESULT CreateVkd3dDevice(ID3D12Device** out)
{
    *out = NULL;
    if (!LoadVkd3d()) return E_FAIL;
    ID3D12DeviceFactory* factory = NULL;
    HRESULT hr = g_getInterface(kClsidDeviceFactory, IID_ID3D12DeviceFactory, (void**)&factory);
    if (FAILED(hr)) { P5Log("D3D12GetInterface(DeviceFactory): %08lX", (unsigned long)hr); return hr; }
    hr = factory->CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, IID_ID3D12Device, (void**)out);
    factory->Release();
    P5Log("vkd3d-proton device: %08lX %p", (unsigned long)hr, *out);
    return hr;
}

// ---- driver objects (live in memory the runtime sizes with our CalcPrivate*Size and hands back as handles) ----
struct P5Adapter {
    D3D12DDI_HRTADAPTER rt;
    const D3DDDI_ADAPTERCALLBACKS* cb;
    ID3D12Device* caps;                    // a vkd3d device kept for capability questions
    D3D12DDI_HRTTABLE listTable[2];        // the runtime's handles of the command-list tables we filled
};
static const ULONG kDeviceMagic = 0x44355350;   // "PS5D"
struct P5Device {
    ULONG magic;
    P5Adapter* adapter;
    D3D12DDI_HRTDEVICE rt;
    const D3DDDI_DEVICECALLBACKS* kt;
    const void* um;                        // D3D12DDI_CORELAYER_DEVICECALLBACKS_0062 for 0076
    ID3D12Device* dev;
};
static P5Device* Dev(D3D12DDI_HDEVICE h) { return (P5Device*)h.pDrvPrivate; }

// ---- adapter ----
static SIZE_T APIENTRY CalcPrivateDeviceSize(D3D12DDI_HADAPTER, const D3D12DDIARG_CALCPRIVATEDEVICESIZE* a)
{
    P5Log("CalcPrivateDeviceSize: interface %08X version %08X flags %X", a->Interface, a->Version, (UINT)a->Flags);
    return sizeof(P5Device);
}
static UINT g_ddiBuild;                     // the DDI build the runtime took (Version >> 16: 94 for 0094, 6 for 0076)
static HRESULT APIENTRY CreateDevice(D3D12DDI_HADAPTER ha, const D3D12DDIARG_CREATEDEVICE_0003* a)
{
    P5Log("CreateDevice: interface %08X version %08X flags %X", a->Interface, a->Version, (UINT)a->Flags);
    g_ddiBuild = a->Version >> 16;
    P5Device* d = new (a->hDrvDevice.pDrvPrivate) P5Device();
    d->magic = kDeviceMagic;
    d->adapter = (P5Adapter*)ha.pDrvPrivate;
    d->rt = a->hRTDevice;
    d->kt = a->pKTCallbacks;
    d->um = a->p12UMCallbacks;
    {
        HMODULE core = GetModuleHandleA("D3D12Core.dll");
        const D3D12DDI_CORELAYER_DEVICECALLBACKS_0050* cb = (const D3D12DDI_CORELAYER_DEVICECALLBACKS_0050*)d->um;
        P5Log("runtime callbacks: D3D12Core at %p, AllocateCb RVA %llX, CreateContextVirtualCb RVA %llX", (void*)core,
            (unsigned long long)((const BYTE*)cb->pfnAllocateCb - (const BYTE*)core), (unsigned long long)((const BYTE*)cb->pfnCreateContextVirtualCb - (const BYTE*)core));
    }
    HRESULT hr = CreateVkd3dDevice(&d->dev);
    if (FAILED(hr)) { d->magic = 0; return hr; }
    return S_OK;
}
static void APIENTRY DestroyDevice(D3D12DDI_HDEVICE h)
{
    P5Device* d = Dev(h);
    P5Log("DestroyDevice");
    if (d->dev) d->dev->Release();
    d->~P5Device();
}
static HRESULT APIENTRY CloseAdapter(D3D12DDI_HADAPTER h)
{
    P5Adapter* a = (P5Adapter*)h.pDrvPrivate;
    P5Log("CloseAdapter");
    if (a->caps) a->caps->Release();
    delete a;
    return S_OK;
}
static HRESULT APIENTRY GetSupportedVersions(D3D12DDI_HADAPTER, UINT32* n, UINT64* v)
{
    // 0094 (enhanced barriers: D3D12Core 1.618 turns them on only from 0094 on (CDevice::EnhancedBarriersSupported,
    // found by offering builds to p5opt12); 0090-0094 add only caps queries, one list entry and the ranged-barrier
    // struct) and 0076 (Windows 10's own runtime). P5D3D12_DDI=76:
    // offer 0076 only. The runtime picks; FillDDITable tells which by the size of the tables it hands over.
    char e[8] = "";
    bool only76 = GetEnvironmentVariableA("P5D3D12_DDI", e, sizeof e) && !strcmp(e, "76");
    UINT32 want = only76 ? 1 : 2;
    if (!v) { *n = want; return S_OK; }
    if (*n < want) return E_INVALIDARG;
    UINT32 k = 0;
    // P5D3D12_DDITOP=<build> (test only): offer R8 build <build> on top instead of 0094 (finding runtime thresholds)
    char tb[8] = "";
    UINT top = GetEnvironmentVariableA("P5D3D12_DDITOP", tb, sizeof tb) ? (UINT)atoi(tb) : 0;
    if (!only76) v[k++] = top >= 80 && top < 200 ? ((((UINT64)D3D12DDI_INTERFACE_VERSION_R8) << 32) | ((UINT64)top << 16)) : D3D12DDI_SUPPORTED_0094;
    v[k++] = D3D12DDI_SUPPORTED_0076;
    *n = k;
    return S_OK;
}
// Capability answers come from a vkd3d-proton device kept by the adapter (vkd3d's own CheckFeatureSupport), limited to
// what this layer forwards so far (no raytracing, mesh shaders, sampler feedback, shading rate, render passes yet).
static ID3D12Device* CapsDevice(P5Adapter* ad)
{
    if (!ad->caps && FAILED(CreateVkd3dDevice(&ad->caps))) ad->caps = NULL;
    return ad->caps;
}
template <class T> static bool Feature(ID3D12Device* d, D3D12_FEATURE f, T* out)
{
    memset(out, 0, sizeof *out);
    return d && SUCCEEDED(d->CheckFeatureSupport(f, out, sizeof *out));
}
static bool MaxSm51()
{
    char ms[16] = "";
    return GetEnvironmentVariableA("P5D3D12_MAXSM", ms, sizeof ms) && atoi(ms) == 51;
}
static void FillOptions(ID3D12Device* d, D3D12DDI_D3D12_OPTIONS_DATA_0073* o)
{
    D3D12_FEATURE_DATA_D3D12_OPTIONS o0; D3D12_FEATURE_DATA_D3D12_OPTIONS2 o2; D3D12_FEATURE_DATA_D3D12_OPTIONS3 o3;
    D3D12_FEATURE_DATA_D3D12_OPTIONS4 o4;
    Feature(d, D3D12_FEATURE_D3D12_OPTIONS, &o0);
    Feature(d, D3D12_FEATURE_D3D12_OPTIONS2, &o2);
    Feature(d, D3D12_FEATURE_D3D12_OPTIONS3, &o3);
    Feature(d, D3D12_FEATURE_D3D12_OPTIONS4, &o4);
    o->ResourceBindingTier = (D3D12DDI_RESOURCE_BINDING_TIER)o0.ResourceBindingTier;
    o->ConservativeRasterizationTier = (D3D12DDI_CONSERVATIVE_RASTERIZATION_TIER)o0.ConservativeRasterizationTier;
    o->TiledResourcesTier = (D3D12DDI_TILED_RESOURCES_TIER)(o0.TiledResourcesTier > 3 ? 3 : o0.TiledResourcesTier);   // tier 4 exists from 0117 only
    o->CrossNodeSharingTier = (D3D12DDI_CROSS_NODE_SHARING_TIER)0;
    o->VPAndRTArrayIndexFromAnyShaderFeedingRasterizerSupportedWithoutGSEmulation = o0.VPAndRTArrayIndexFromAnyShaderFeedingRasterizerSupportedWithoutGSEmulation;
    o->OutputMergerLogicOp = o0.OutputMergerLogicOp;
    o->ResourceHeapTier = (D3D12DDI_RESOURCE_HEAP_TIER)o0.ResourceHeapTier;
    o->DepthBoundsTestSupported = o2.DepthBoundsTestSupported;
    o->ProgrammableSamplePositionsTier = (D3D12DDI_PROGRAMMABLE_SAMPLE_POSITIONS_TIER)o2.ProgrammableSamplePositionsTier;
    o->CopyQueueTimestampQueriesSupported = o3.CopyQueueTimestampQueriesSupported;
    // API list types (direct 1, bundle 2, compute 4, copy 8) -> driver queue flags (3D 1, compute 2, copy 4)
    UINT wb = o3.WriteBufferImmediateSupportFlags, q = 0;
    if (wb & D3D12_COMMAND_LIST_SUPPORT_FLAG_DIRECT) q |= D3D12DDI_COMMAND_QUEUE_FLAG_3D;
    if (wb & D3D12_COMMAND_LIST_SUPPORT_FLAG_COMPUTE) q |= D3D12DDI_COMMAND_QUEUE_FLAG_COMPUTE;
    if (wb & D3D12_COMMAND_LIST_SUPPORT_FLAG_COPY) q |= D3D12DDI_COMMAND_QUEUE_FLAG_COPY;
    o->WriteBufferImmediateQueueFlags = (D3D12DDI_COMMAND_QUEUE_FLAGS)q;
    o->ViewInstancingTier = (D3D12DDI_VIEW_INSTANCING_TIER)o3.ViewInstancingTier;
    o->BarycentricsSupported = o3.BarycentricsSupported;
    o->ReservedBufferPlacementSupported = o4.MSAA64KBAlignedTextureSupported;
    o->Deterministic64KBUndefinedSwizzle = FALSE;
    o->SRVOnlyTiledResourceTier3 = FALSE;
    if (MaxSm51()) {                       // features the runtime ties to shader model 6.x
        o->ViewInstancingTier = (D3D12DDI_VIEW_INSTANCING_TIER)0;
        o->BarycentricsSupported = FALSE;
    }
}
static void FillShaderCaps(ID3D12Device* d, D3D12DDI_SHADER_CAPS_0042* c)
{
    D3D12_FEATURE_DATA_D3D12_OPTIONS o0; D3D12_FEATURE_DATA_D3D12_OPTIONS1 o1; D3D12_FEATURE_DATA_D3D12_OPTIONS4 o4;
    Feature(d, D3D12_FEATURE_D3D12_OPTIONS, &o0);
    Feature(d, D3D12_FEATURE_D3D12_OPTIONS1, &o1);
    Feature(d, D3D12_FEATURE_D3D12_OPTIONS4, &o4);
    c->MinPrecision = (D3D12DDI_SHADER_MIN_PRECISION)o0.MinPrecisionSupport;
    c->DoubleOps = o0.DoublePrecisionFloatShaderOps;
    c->ShaderSpecifiedStencilRef = o0.PSSpecifiedStencilRefSupported;
    c->TypedUAVLoadAdditionalFormats = o0.TypedUAVLoadAdditionalFormats;
    c->ROVs = o0.ROVsSupported;
    c->WaveOps = o1.WaveOps;
    c->WaveLaneCountMin = o1.WaveLaneCountMin;
    c->WaveLaneCountMax = o1.WaveLaneCountMax;
    c->TotalLaneCount = o1.TotalLaneCount;
    c->Int64Ops = o1.Int64ShaderOps;
    c->Native16BitOps = o4.Native16BitShaderOpsSupported;
    if (MaxSm51()) { c->Native16BitOps = FALSE; c->WaveOps = FALSE; c->Int64Ops = FALSE; }
}
static HRESULT APIENTRY GetCaps(D3D12DDI_HADAPTER ha, const D3D12DDIARG_GETCAPS* a)
{
    P5Adapter* ad = (P5Adapter*)ha.pDrvPrivate;
    static LONG said[2048];
    UINT t = (UINT)a->Type;
    if (t < 2048 && InterlockedIncrement(&said[t]) <= 2)
        P5Log("GetCaps: type %u info %p data %p size %u", t, a->pInfo, a->pData, a->DataSize);
    // Output-only caps structs are cleared before they are filled; others (e.g. the shader-model list) carry the
    // runtime's own pointers and must not be touched beyond what they ask for.
    bool outOnly = t == D3D12DDICAPS_TYPE_D3D12_OPTIONS || t == D3D12DDICAPS_TYPE_SHADER || t == D3D12DDICAPS_TYPE_3DPIPELINESUPPORT ||
                   t == D3D12DDICAPS_TYPE_ARCHITECTURE_INFO || t == D3D12DDICAPS_TYPE_MEMORY_ARCHITECTURE || t == D3D12DDICAPS_TYPE_0022_TEXTURE_LAYOUT;
    // (1062 queue priority, 1067 hardware scheduling: left as the runtime filled them; zeroing them failed device creation)
    if (outOnly && a->pData && a->DataSize) memset(a->pData, 0, a->DataSize);
    switch (t) {
    case D3D12DDICAPS_TYPE_0011_SHADER_MODELS: {
        static const D3D12DDI_SHADER_MODEL kModels[] = {
            D3D12DDI_SHADER_MODEL_5_1_RELEASE_0011, D3D12DDI_SHADER_MODEL_6_0_RELEASE_0011, D3D12DDI_SHADER_MODEL_6_1_RELEASE_0033,
            D3D12DDI_SHADER_MODEL_6_2_RELEASE_0042, D3D12DDI_SHADER_MODEL_6_3_RELEASE_0054, D3D12DDI_SHADER_MODEL_6_4_RELEASE_0062,
            D3D12DDI_SHADER_MODEL_6_5_RELEASE_0071 };
        D3D12DDI_D3D12_SHADER_MODELS_DATA_0011* m = (D3D12DDI_D3D12_SHADER_MODELS_DATA_0011*)a->pData;
        if (!m || a->DataSize < sizeof *m || !m->pNumShaderModelsSupported) return E_INVALIDARG;
        UINT have = sizeof kModels / sizeof kModels[0];
        // P5D3D12_MAXSM=51: report shader model 5.1 only. Then the runtime does not convert DXBC programs to DXIL
        // (it loads dxilconv when the highest model is 6.0 or more) and vkd3d-proton gets them as DXBC, as on its own
        // d3d12.dll path; programs that need shader model 6 are refused by the runtime.
        if (MaxSm51()) have = 1;
        if (!m->pShaderModelsSupported) { *m->pNumShaderModelsSupported = have; return S_OK; }
        UINT k = *m->pNumShaderModelsSupported < have ? *m->pNumShaderModelsSupported : have;
        memcpy(m->pShaderModelsSupported, kModels, k * sizeof kModels[0]);
        *m->pNumShaderModelsSupported = k;
        return S_OK;
    }
    case D3D12DDICAPS_TYPE_D3D12_OPTIONS:
        if (a->DataSize >= sizeof(D3D12DDI_D3D12_OPTIONS_DATA_0073)) {
            D3D12DDI_D3D12_OPTIONS_DATA_0073* o = (D3D12DDI_D3D12_OPTIONS_DATA_0073*)a->pData;
            FillOptions(CapsDevice(ad), o);
            if (a->DataSize >= sizeof(D3D12DDI_D3D12_OPTIONS_DATA_0089)) {           // DDI 0089: enhanced barriers
                ((D3D12DDI_D3D12_OPTIONS_DATA_0089*)a->pData)->EnhancedBarriersSupported = TRUE;
                P5Log("  options size %u (0089+): enhanced barriers on", a->DataSize);
            }
            // P5D3D12_OPTMASK (hex): bit i set = dword i of the answer forced to 0 (bisecting what the runtime refuses)
            char m[32] = "";
            if (GetEnvironmentVariableA("P5D3D12_OPTMASK", m, sizeof m)) {
                ULONG mask = strtoul(m, NULL, 16);
                for (UINT i = 0; i < 25; ++i) if (mask & (1u << i)) ((UINT*)o)[i] = 0;
            }
            char line[400] = "";
            for (UINT i = 0; i < 25; ++i) snprintf(line + strlen(line), sizeof line - strlen(line), " %X", ((UINT*)o)[i]);
            P5Log("  options:%s", line);
        }
        else P5Log("  options: size %u not handled", a->DataSize);
        return S_OK;
    case D3D12DDICAPS_TYPE_SHADER:
        if (a->DataSize >= sizeof(D3D12DDI_SHADER_CAPS_0042)) FillShaderCaps(CapsDevice(ad), (D3D12DDI_SHADER_CAPS_0042*)a->pData);   // newer (0084: 64 bytes) only append
        else P5Log("  shader caps: size %u not handled", a->DataSize);
        return S_OK;
    case D3D12DDICAPS_TYPE_MEMORY_ARCHITECTURE:
        if (a->DataSize >= sizeof(D3D12DDI_MEMORY_ARCHITECTURE_CAPS_0041)) {
            D3D12_FEATURE_DATA_ARCHITECTURE arch;
            Feature(CapsDevice(ad), D3D12_FEATURE_ARCHITECTURE, &arch);
            D3D12DDI_MEMORY_ARCHITECTURE_CAPS_0041* m = (D3D12DDI_MEMORY_ARCHITECTURE_CAPS_0041*)a->pData;
            m->UMA = arch.UMA;
            m->IOCoherent = TRUE;        // required on x86/amd64 (D3D12Core: PCIe supports I/O coherence)
            m->CacheCoherent = arch.UMA && arch.CacheCoherentUMA;
            P5Log("  memory architecture: UMA %d io-coherent 1 cache-coherent %d", arch.UMA, m->CacheCoherent);
        }
        return S_OK;
    case D3D12DDICAPS_TYPE_TEXTURE_LAYOUT_SETS:
        // *pInfo = {D3D12DDI_TL_ROW_MAJOR, functional unit}: row-major (linear) textures, the same for every unit.
        // Power-of-two alignments no larger than the API's own footprint rules (pitch 256, placement 512).
        if (a->pData && a->DataSize >= sizeof(D3D12DDI_ROW_MAJOR_LAYOUT_CAPS)) {
            D3D12DDI_ROW_MAJOR_LAYOUT_CAPS* r = (D3D12DDI_ROW_MAJOR_LAYOUT_CAPS*)a->pData;
            memset(r, 0, sizeof *r);
            for (auto& sc : r->SubCaps) { sc.MaxElementSize = 16; sc.BaseOffsetAlignment = 256; sc.PitchAlignment = 256; sc.DepthPitchAlignment = 256; }
            r->Flags = D3D12DDI_ROW_MAJOR_LAYOUT_FLAG_NONE;
            const UINT* in = (const UINT*)a->pInfo;
            P5Log("  row-major layout caps (layout %u, unit %u): element 16, alignments 256", in ? in[0] : 0, in ? in[1] : 0);
        }
        return S_OK;
    case D3D12DDICAPS_TYPE_GPUVA_CAPS:
        if (a->pData && a->DataSize >= sizeof(D3D12DDI_GPUVA_CAPS_0004)) {
            D3D12_FEATURE_DATA_GPU_VIRTUAL_ADDRESS_SUPPORT va;
            UINT bits = Feature(CapsDevice(ad), D3D12_FEATURE_GPU_VIRTUAL_ADDRESS_SUPPORT, &va) && va.MaxGPUVirtualAddressBitsPerResource ? va.MaxGPUVirtualAddressBitsPerResource : 40;
            ((D3D12DDI_GPUVA_CAPS_0004*)a->pData)->MaxGPUVirtualAddressBitsPerResource = bits;
            P5Log("  gpuva caps: %u bits per resource", bits);
        }
        return S_OK;
    case D3D12DDICAPS_TYPE_3DPIPELINESUPPORT:
        if (a->DataSize >= sizeof(UINT)) *(UINT*)a->pData = D3D12DDI_3DPIPELINELEVEL_12_1;
        return S_OK;
    case D3D12DDICAPS_TYPE_0081_3DPIPELINESUPPORT1:  // newer runtimes (Agility SDK) ask this one instead: in = runtime's highest
        if (a->pData && a->DataSize >= sizeof(D3D12DDI_3DPIPELINESUPPORT1_DATA_0081)) {
            auto* p = (D3D12DDI_3DPIPELINESUPPORT1_DATA_0081*)a->pData;
            p->MaximumDriverSupportedFeatureLevel = D3D12DDI_3DPIPELINELEVEL_12_1;
            P5Log("  3d pipeline support 1: runtime %u, driver 12_1", (UINT)p->HighestRuntimeSupportedFeatureLevel);
        }
        return S_OK;
    default:
        return S_OK;                     // zeros = "nothing beyond the minimum" while the real answers are written
    }
}
// ---- device: capability questions forwarded to vkd3d-proton ----
// DDI format-support bits for a format, from vkd3d-proton's API answer. `quiet`: no log line.
static UINT DdiFormatSupport(ID3D12Device* dev, DXGI_FORMAT format, bool quiet)
{
    D3D12_FEATURE_DATA_FORMAT_SUPPORT f = {};
    f.Format = format;
    HRESULT hr = dev->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &f, sizeof f);
    if (FAILED(hr)) { if (!quiet) P5Log("  format %u: vkd3d hr %08lX -> 0", (UINT)format, (unsigned long)hr); return 0; }
    UINT s1 = f.Support1, s2 = f.Support2, r = 0;
    if (s1 & D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE) r |= D3D12DDI_FORMAT_SUPPORT_SHADER_SAMPLE;
    if (s1 & (D3D12_FORMAT_SUPPORT1_RENDER_TARGET | D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL)) r |= D3D12DDI_FORMAT_SUPPORT_RENDERTARGET;
    if (s1 & D3D12_FORMAT_SUPPORT1_BLENDABLE) r |= D3D12DDI_FORMAT_SUPPORT_BLENDABLE;
    if (s1 & D3D12_FORMAT_SUPPORT1_MULTISAMPLE_RENDERTARGET) r |= D3D12DDI_FORMAT_SUPPORT_MULTISAMPLE_RENDERTARGET;
    if (s1 & D3D12_FORMAT_SUPPORT1_MULTISAMPLE_LOAD) r |= D3D12DDI_FORMAT_SUPPORT_MULTISAMPLE_LOAD;
    if (s1 & D3D12_FORMAT_SUPPORT1_IA_VERTEX_BUFFER) r |= D3D12DDI_FORMAT_SUPPORT_VERTEX_BUFFER;
    if (s1 & D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW) r |= D3D12DDI_FORMAT_SUPPORT_UAV_WRITES;
    if (s1 & D3D12_FORMAT_SUPPORT1_BUFFER) r |= D3D12DDI_FORMAT_SUPPORT_BUFFER;
    if (s1 & D3D12_FORMAT_SUPPORT1_SHADER_GATHER) r |= D3D12DDI_FORMAT_SUPPORT_SHADER_GATHER;
    if (s1 & D3D12_FORMAT_SUPPORT1_DISPLAY) r |= D3D12DDI_FORMAT_SUPPORT_DISPLAY;
    if (s2 & D3D12_FORMAT_SUPPORT2_OUTPUT_MERGER_LOGIC_OP) r |= D3D12DDI_FORMAT_SUPPORT_OUTPUT_MERGER_LOGIC_OP;
    if (s2 & D3D12_FORMAT_SUPPORT2_TILED) r |= D3D12DDI_FORMAT_SUPPORT_TILED;
    if (s2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD) r |= D3D12DDI_FORMAT_SUPPORT_UAV_READS;
    // Stencil views (X32_TYPELESS_G8X24_UINT, X24_TYPELESS_G8_UINT) are integer: no filtered sampling, no gather
    if (format == DXGI_FORMAT_X32_TYPELESS_G8X24_UINT || format == DXGI_FORMAT_X24_TYPELESS_G8_UINT)
        r &= ~(D3D12DDI_FORMAT_SUPPORT_SHADER_SAMPLE | D3D12DDI_FORMAT_SUPPORT_SHADER_GATHER);
    static LONG said;
    if (!quiet && InterlockedIncrement(&said) <= 1000) P5Log("  format %u: api %08X/%08X -> %08X", (UINT)format, s1, s2, r);
    return r;
}
static void APIENTRY CheckFormatSupport(D3D12DDI_HDEVICE h, DXGI_FORMAT format, UINT* out)
{
    *out = DdiFormatSupport(Dev(h)->dev, format, false);
}
// The typeless "parent" formats of DXGI's format families.
static bool TypelessParent(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: case DXGI_FORMAT_R32G32B32_TYPELESS: case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R32G32_TYPELESS: case DXGI_FORMAT_R32G8X24_TYPELESS: case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R16G16_TYPELESS: case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_R24G8_TYPELESS: case DXGI_FORMAT_R8G8_TYPELESS: case DXGI_FORMAT_R16_TYPELESS: case DXGI_FORMAT_R8_TYPELESS:
    case DXGI_FORMAT_BC1_TYPELESS: case DXGI_FORMAT_BC2_TYPELESS: case DXGI_FORMAT_BC3_TYPELESS: case DXGI_FORMAT_BC4_TYPELESS:
    case DXGI_FORMAT_BC5_TYPELESS: case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8X8_TYPELESS:
    case DXGI_FORMAT_BC6H_TYPELESS: case DXGI_FORMAT_BC7_TYPELESS:
        return true;
    default:
        return false;
    }
}
static void APIENTRY CheckMultisampleQualityLevels(D3D12DDI_HDEVICE h, DXGI_FORMAT format, UINT samples, D3D12DDI_MULTISAMPLE_QUALITY_LEVEL_FLAGS flags, UINT* out)
{
    D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS q = {};
    // D3D12Core (19041) checks every format's answers for 2..31 samples against its MULTISAMPLE_RENDERTARGET bit: a format
    // without it must answer 0 (else 887A0020 at device creation; vkd3d-proton answers 1 at 2x for
    // R32_FLOAT_X8X24_TYPELESS, which has MULTISAMPLE_LOAD only). Typeless parents keep vkd3d-proton's answer.
    q.Format = format;
    if (!TypelessParent(format) && !(DdiFormatSupport(Dev(h)->dev, format, true) & D3D12DDI_FORMAT_SUPPORT_MULTISAMPLE_RENDERTARGET)) {
        *out = 0;
        return;
    }
    q.SampleCount = samples;
    q.Flags = (D3D12_MULTISAMPLE_QUALITY_LEVEL_FLAGS)flags;
    HRESULT hr = Dev(h)->dev->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &q, sizeof q);
    *out = SUCCEEDED(hr) ? q.NumQualityLevels : 0;
    // Logged: failures, non-zero answers and the first question per format (the walk asks counts 1..32 per format).
    static UINT lastFormat = ~0u;
    static LONG said;
    if ((FAILED(hr) || *out || lastFormat != (UINT)format || samples == 2 || samples == 4 || samples == 8) && InterlockedIncrement(&said) <= 3000)
        P5Log("  msaa format %u x%u flags %u -> %u (hr %08lX)", (UINT)format, samples, (UINT)flags, *out, (unsigned long)hr);
    lastFormat = (UINT)format;
}
static void APIENTRY QueryNodeMap(D3D12DDI_HDEVICE, UINT n, UINT* map)
{
    for (UINT i = 0; i < n; ++i) map[i] = i;
}

// Extended features (table type 8, 0021): none offered.
static HRESULT APIENTRY GetSupportedExtendedFeatures(D3D12DDI_HDEVICE, UINT32* n, D3D12DDI_FEATURE_0020*) { if (n) *n = 0; return S_OK; }
static HRESULT APIENTRY GetSupportedExtendedFeatureVersions(D3D12DDI_HDEVICE, D3D12DDI_FEATURE_0020, UINT32* n, UINT32*) { if (n) *n = 0; return S_OK; }
static HRESULT APIENTRY EnableExtendedFeature(D3D12DDI_HDEVICE, D3D12DDI_FEATURE_0020 f, UINT32 v) { P5Log("EnableExtendedFeature %u %u: refused", (UINT)f, v); return E_INVALIDARG; }
static HRESULT APIENTRY SetExtendedFeatureCallbacks(D3D12DDI_HDEVICE, D3D12DDI_TABLE_TYPE t, const void*, SIZE_T s) { P5Log("SetExtendedFeatureCallbacks table %u size %zu", (UINT)t, s); return S_OK; }

// ---- device objects ----
template <class T, class H> static T* Obj(H h) { return (T*)h.pDrvPrivate; }

// Root signatures: the DDI hands over the parsed version 1.1 description (the same layout as the API's
// D3D12_ROOT_SIGNATURE_DESC1); it is serialized with Windows' own d3d12.dll (already in the process) and created on
// the vkd3d-proton device.
static_assert(sizeof(D3D12_ROOT_SIGNATURE_DESC1) == sizeof(D3D12DDI_ROOT_SIGNATURE_0013), "root signature layout");
static_assert(sizeof(D3D12_ROOT_PARAMETER1) == sizeof(D3D12DDI_ROOT_PARAMETER_0013), "root parameter layout");
static_assert(sizeof(D3D12_DESCRIPTOR_RANGE1) == sizeof(D3D12DDI_DESCRIPTOR_RANGE_0013), "descriptor range layout");
static_assert(sizeof(D3D12_STATIC_SAMPLER_DESC) == sizeof(D3D12DDI_STATIC_SAMPLER), "static sampler layout");
typedef HRESULT (WINAPI* PFN_SERIALIZE_VRS)(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC*, ID3DBlob**, ID3DBlob**);
struct P5RootSignature { ID3D12RootSignature* rs; uint32_t id; };
static SIZE_T APIENTRY CalcPrivateRootSignatureSize(D3D12DDI_HDEVICE, const D3D12DDIARG_CREATE_ROOT_SIGNATURE_0013*) { return sizeof(P5RootSignature); }
static HRESULT APIENTRY CreateRootSignature(D3D12DDI_HDEVICE h, const D3D12DDIARG_CREATE_ROOT_SIGNATURE_0013* a, D3D12DDI_HROOTSIGNATURE hrs)
{
    P5RootSignature* r = new (hrs.pDrvPrivate) P5RootSignature();
    static PFN_SERIALIZE_VRS serialize;
    if (!serialize) {
        HMODULE m = GetModuleHandleA("d3d12.dll");
        serialize = m ? (PFN_SERIALIZE_VRS)GetProcAddress(m, "D3D12SerializeVersionedRootSignature") : NULL;
    }
    if (!serialize || a->Version != D3D12DDI_ROOT_SIGNATURE_VERSION_1_1) {
        P5Log("CreateRootSignature: version %u, serializer %p: not handled", (UINT)a->Version, (void*)serialize);
        return E_INVALIDARG;
    }
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC v = {};
    v.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    memcpy(&v.Desc_1_1, a->pRootSignature_1_1, sizeof v.Desc_1_1);
    ID3DBlob *blob = NULL, *err = NULL;
    HRESULT hr = serialize(&v, &blob, &err);
    if (FAILED(hr)) {
        P5Log("CreateRootSignature: serialize %08lX %s", (unsigned long)hr, err ? (const char*)err->GetBufferPointer() : "");
        if (err) err->Release();
        return hr;
    }
    {
        uint32_t hsh = 2166136261u;
        for (SIZE_T i = 0; i < blob->GetBufferSize(); ++i) hsh = (hsh ^ ((const uint8_t*)blob->GetBufferPointer())[i]) * 16777619u;
        r->id = hsh;
        char v[8] = "";
        if (GetEnvironmentVariableA("P5D3D12_DUMP", v, sizeof v) && v[0] == '1') {
            char path[MAX_PATH];
            CreateDirectoryA("C:\\ps5\\gpu\\umdlog\\sh", NULL);
            snprintf(path, sizeof path, "C:\\ps5\\gpu\\umdlog\\sh\\rs-%08x.bin", hsh);
            if (FILE* f = fopen(path, "wb")) { fwrite(blob->GetBufferPointer(), 1, blob->GetBufferSize(), f); fclose(f); }
        }
    }
    hr = Dev(h)->dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_ID3D12RootSignature, (void**)&r->rs);
    static LONG said;
    if (FAILED(hr) || InterlockedIncrement(&said) <= 20)
        P5Log("CreateRootSignature: %u parameters, %u samplers, flags %X -> %08lX", v.Desc_1_1.NumParameters, v.Desc_1_1.NumStaticSamplers, (UINT)v.Desc_1_1.Flags, (unsigned long)hr);
    blob->Release();
    return hr;
}
static void APIENTRY DestroyRootSignature(D3D12DDI_HDEVICE, D3D12DDI_HROOTSIGNATURE h)
{
    P5RootSignature* r = Obj<P5RootSignature>(h);
    if (r->rs) r->rs->Release();
    r->~P5RootSignature();
}

#include "shaders.inc"
#include "cmds.inc"
#include "resources.inc"
#include "draw.inc"
#include "phase4.inc"

static HRESULT APIENTRY GetOptionalDDITables(D3D12DDI_HADAPTER, UINT32* n, D3D12DDI_TABLE_REQUEST*)
{
    if (n) *n = 0;
    return S_OK;
}
static void FillFrom(void* table, SIZE_T size, const P5StubEntry* stubs, SIZE_T count, const char* what)
{
    SIZE_T slots = size / sizeof(void*);
    if (slots > count) P5Log("%s table: runtime wants %zu entries, we know %zu", what, slots, count);
    for (SIZE_T i = 0; i < slots && i < count; ++i) ((void**)table)[i] = stubs[i].fn;
}
static HRESULT APIENTRY FillDDITable(D3D12DDI_HADAPTER ha, D3D12DDI_TABLE_TYPE type, void* table, SIZE_T size, UINT n, D3D12DDI_HRTTABLE rtt)
{
    P5Log("FillDDITable: type %u size %zu count %u", (UINT)type, size, n);
    switch (type) {
    case D3D12DDI_TABLE_TYPE_DEVICE_CORE:
        FillFrom(table, size, kStubs_core, sizeof kStubs_core / sizeof kStubs_core[0], "core");
        if (size >= sizeof(D3D12DDI_DEVICE_FUNCS_CORE_0075)) {
            D3D12DDI_DEVICE_FUNCS_CORE_0075* f = (D3D12DDI_DEVICE_FUNCS_CORE_0075*)table;
            f->pfnCheckFormatSupport = CheckFormatSupport;
            f->pfnGetMipPacking = GetMipPacking;
            f->pfnCheckMultisampleQualityLevels = CheckMultisampleQualityLevels;
            f->pfnQueryNodeMap = QueryNodeMap;
            f->pfnCalcPrivateRootSignatureSize = CalcPrivateRootSignatureSize;
            f->pfnCreateRootSignature = CreateRootSignature;
            f->pfnDestroyRootSignature = DestroyRootSignature;
            f->pfnCalcPrivateShaderSize = CalcPrivateShaderSize;
            f->pfnCreateVertexShader = CreateShader;
            f->pfnCreatePixelShader = CreateShader;
            f->pfnCreateGeometryShader = CreateShader;
            f->pfnCreateComputeShader = CreateShader;
            f->pfnCalcPrivateTessellationShaderSize = CalcPrivateShaderSize;
            f->pfnCreateHullShader = CreateShader;
            f->pfnCreateDomainShader = CreateShader;
            f->pfnDestroyShader = DestroyShader;
            f->pfnCalcPrivateElementLayoutSize = CalcPrivateElementLayoutSize;
            f->pfnCreateElementLayout = CreateElementLayout;
            f->pfnDestroyElementLayout = DestroyElementLayout;
            f->pfnCalcPrivateBlendStateSize = CalcPrivateBlendStateSize;
            f->pfnCreateBlendState = CreateBlendState;
            f->pfnDestroyBlendState = DestroyBlendState;
            f->pfnCalcPrivateRasterizerStateSize = CalcPrivateRasterizerStateSize;
            f->pfnCreateRasterizerState = CreateRasterizerState;
            f->pfnDestroyRasterizerState = DestroyRasterizerState;
            f->pfnCalcPrivateDepthStencilStateSize = CalcPrivateDepthStencilStateSize;
            f->pfnCreateDepthStencilState = CreateDepthStencilState;
            f->pfnDestroyDepthStencilState = DestroyDepthStencilState;
            f->pfnCalcPrivatePipelineStateSize = CalcPrivatePipelineStateSize;
            f->pfnCreatePipelineState = CreatePipelineState;
            f->pfnDestroyPipelineState = DestroyPipelineState;
            f->pfnMakeResident = MakeResident;
            f->pfnEvict = Evict;
            f->pfnGetDebugAllocationInfo = GetDebugAllocationInfo;
            f->pfnCalcPrivateDescriptorHeapSize = CalcPrivateDescriptorHeapSize;
            f->pfnCreateDescriptorHeap = CreateDescriptorHeap;
            f->pfnDestroyDescriptorHeap = DestroyDescriptorHeap;
            f->pfnGetDescriptorSizeInBytes = GetDescriptorSizeInBytes;
            f->pfnGetCPUDescriptorHandleForHeapStart = GetCPUDescriptorHandleForHeapStart;
            f->pfnGetGPUDescriptorHandleForHeapStart = GetGPUDescriptorHandleForHeapStart;
            f->pfnCalcPrivateCommandPoolSize = CalcPrivateCommandPoolSize;
            f->pfnCreateCommandPool = CreateCommandPool;
            f->pfnDestroyCommandPool = DestroyCommandPool;
            f->pfnResetCommandPool = ResetCommandPool;
            f->pfnCalcPrivateCommandRecorderSize = CalcPrivateCommandRecorderSize;
            f->pfnCreateCommandRecorder = CreateCommandRecorder;
            f->pfnDestroyCommandRecorder = DestroyCommandRecorder;
            f->pfnCommandRecorderSetCommandPoolAsTarget = CommandRecorderSetCommandPoolAsTarget;
            f->pfnCalcPrivateCommandListSize = CalcPrivateCommandListSize;
            f->pfnCreateCommandList = CreateCommandList;
            f->pfnDestroyCommandList = DestroyCommandList;
            f->pfnCalcPrivateFenceSize = CalcPrivateFenceSize;
            f->pfnCreateFence = CreateFence;
            f->pfnDestroyFence = DestroyFence;
            f->pfnCalcPrivateCommandQueueSize = CalcPrivateCommandQueueSize;
            f->pfnCreateCommandQueue = CreateCommandQueue;
            f->pfnDestroyCommandQueue = DestroyCommandQueue;
            f->pfnCalcPrivateHeapAndResourceSizes = CalcPrivateHeapAndResourceSizes;
            f->pfnCreateHeapAndResource = CreateHeapAndResource;
            f->pfnDestroyHeapAndResource = DestroyHeapAndResource;
            f->pfnMapHeap = MapHeap;
            f->pfnUnmapHeap = UnmapHeap;
            f->pfnCheckResourceVirtualAddress = CheckResourceVirtualAddress;
            f->pfnCheckResourceAllocationInfo = CheckResourceAllocationInfo;
            f->pfnCheckExistingResourceAllocationInfo = CheckExistingResourceAllocationInfo;
            f->pfnCheckSubresourceInfo = CheckSubresourceInfo;
            f->pfnCreateRenderTargetView = CreateRenderTargetView;
            f->pfnCreateConstantBufferView = CreateConstantBufferView;
            f->pfnCreateSampler = CreateSampler;
            f->pfnCheckResourceAllocationHandle = CheckResourceAllocationHandle;
            FillPhase4Core(f);
            FillPipelineLibrary(f);
        }
        if (size >= sizeof(D3D12DDI_DEVICE_FUNCS_CORE_0088)) {   // the runtime took 0088+: layouts and castable formats
            D3D12DDI_DEVICE_FUNCS_CORE_0088* f = (D3D12DDI_DEVICE_FUNCS_CORE_0088*)table;
            f->pfnCalcPrivateHeapAndResourceSizes = CalcPrivateHeapAndResourceSizes88;
            f->pfnCreateHeapAndResource = CreateHeapAndResource88;
            f->pfnCheckResourceAllocationInfo = CheckResourceAllocationInfo88;
            f->pfnImplicitShaderCacheControl = ImplicitShaderCacheControl;
            P5Log("device table: DDI 0088+ entry points");
        }
        return S_OK;
    case D3D12DDI_TABLE_TYPE_COMMAND_LIST_3D:
        FillFrom(table, size, kStubs_list, sizeof kStubs_list / sizeof kStubs_list[0], "list");
        if (n < 2) ((P5Adapter*)ha.pDrvPrivate)->listTable[n] = rtt;
        if (size >= sizeof(D3D12DDI_COMMAND_LIST_FUNCS_3D_0074)) {
            D3D12DDI_COMMAND_LIST_FUNCS_3D_0074* f = (D3D12DDI_COMMAND_LIST_FUNCS_3D_0074*)table;
            f->pfnCloseCommandList = CloseCommandList;
            f->pfnResetCommandList = ResetCommandList;
            FillListFuncs(f);
            FillDrawFuncs(f);
            FillPhase4List(f);
            f->pfnCopyTiles = CopyTiles;
        }
        if (size >= sizeof(D3D12DDI_COMMAND_LIST_FUNCS_3D_0088)) {      // 0092 and 0094 tables have the same size: the build decides
            if (g_ddiBuild >= 94) ((D3D12DDI_COMMAND_LIST_FUNCS_3D_0094*)table)->pfnBarrier = Barrier94;
            else ((D3D12DDI_COMMAND_LIST_FUNCS_3D_0088*)table)->pfnBarrier = Barrier88;
        }
        if (size >= sizeof(D3D12DDI_COMMAND_LIST_FUNCS_3D_0092)) ((D3D12DDI_COMMAND_LIST_FUNCS_3D_0092*)table)->pfnOmSetAlphaBlendFactor = OMSetAlphaBlendFactor92;
        return S_OK;
    case D3D12DDI_TABLE_TYPE_COMMAND_QUEUE_3D:
        FillFrom(table, size, kStubs_queue, sizeof kStubs_queue / sizeof kStubs_queue[0], "queue");
        if (size >= sizeof(D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001)) {
            D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001* f = (D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001*)table;
            f->pfnExecuteCommandLists = ExecuteCommandLists;
            f->pfnSignalFence = SignalFence;
            f->pfnWaitForFence = WaitForFence;
            f->pfnUpdateTileMappings = UpdateTileMappings;
            f->pfnCopyTileMappings = CopyTileMappings;
        }
        return S_OK;
    case D3D12DDI_TABLE_TYPE_0020_EXTENDED_FEATURES: {
        D3D12DDI_EXTENDED_FEATURES_FUNCS_0021 f = {GetSupportedExtendedFeatures, GetSupportedExtendedFeatureVersions, EnableExtendedFeature, SetExtendedFeatureCallbacks};
        memcpy(table, &f, size < sizeof f ? size : sizeof f);
        return S_OK;
    }
    case D3D12DDI_TABLE_TYPE_DXGI:
        FillFrom(table, size, kStubs_dxgi, sizeof kStubs_dxgi / sizeof kStubs_dxgi[0], "dxgi");
        return S_OK;
    default:
        return E_NOTIMPL;
    }
}

extern "C" __declspec(dllexport) HRESULT APIENTRY OpenAdapter12(D3D12DDIARG_OPENADAPTER* a)
{
    char exe[MAX_PATH] = "";
    GetModuleFileNameA(NULL, exe, sizeof exe);
    P5Log("OpenAdapter12 from %s", exe);
    P5Adapter* ad = new (std::nothrow) P5Adapter();
    if (!ad) return E_OUTOFMEMORY;
    ad->rt = a->hRTAdapter;
    ad->cb = a->pAdapterCallbacks;
    a->hAdapter.pDrvPrivate = ad;
    D3D12DDI_ADAPTERFUNCS* f = a->pAdapterFuncs;
    f->pfnCalcPrivateDeviceSize = CalcPrivateDeviceSize;
    f->pfnCreateDevice = CreateDevice;
    f->pfnCloseAdapter = CloseAdapter;
    f->pfnGetSupportedVersions = GetSupportedVersions;
    f->pfnGetCaps = GetCaps;
    f->pfnGetOptionalDDITables = GetOptionalDDITables;
    f->pfnFillDDITable = FillDDITable;
    f->pfnDestroyDevice = DestroyDevice;
    return S_OK;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) InitializeCriticalSection(&g_logLock);
    return TRUE;
}
