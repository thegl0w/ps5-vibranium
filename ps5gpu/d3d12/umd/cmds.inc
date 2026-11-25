// Descriptor heaps, command pools/recorders/lists, command queues and fences (included by ps5d3d12.cpp).
//
// Fences: the runtime's fences are Windows monitored fences; the DDI hands over only their GPU virtual addresses. A
// queue's SignalFence puts a private vkd3d-proton fence signal on the vkd3d queue; the queue's worker thread waits for
// it and then writes the Windows value through ps5wddm (an OP_FENCE submission on a virtual kernel context: the driver's
// page-table walker writes the value and completes the submission, which wakes Windows' waiters; see
// ps5gpu/wddm/WDDM2-TABLE.md "Fences", tested by w2fence.exe).
#include <d3dkmthk.h>
#include <deque>

// ---- kernel side: our adapter, a device and a virtual context for fence writes ----
struct P5KmtOp {               // must match struct Op in ps5wddm.cpp (64-bit)
    ULONG kind, color, nrects, magic;
    void* src; void* dst;
    ULONGLONG srcAddr, dstAddr;
    ULONG srcSeg, dstSeg;
    RECT srcRect, dstRect;
    RECT rects[16];
    void* mdl;
    ULONG mdlOffset, ticket;
    ULONGLONG bytes;
};
struct P5KmtEscape { ULONG magic, code; ULONGLONG va, value; };
struct P5Kmt {
    D3DKMT_HANDLE adapter = 0, device = 0, context = 0;
    CRITICAL_SECTION lock;
    bool ok = false;
    UINT64 cmdVa = 0;          // a GPU VA of this process (our own monitored fence): the "command buffer" of wait ops
    bool tickOk = false;       // ps5wddm has the kernel wait op (OP_WAITTICK + escape P5E_TICK)
};
static P5Kmt g_kmt;
static bool KmtOpen()
{
    static LONG once;
    if (InterlockedExchange(&once, 1) == 0) {
        InitializeCriticalSection(&g_kmt.lock);
        D3DKMT_ADAPTERINFO list[16];
        D3DKMT_ENUMADAPTERS2 en = {};
        en.NumAdapters = 16;
        en.pAdapters = list;
        if (!D3DKMTEnumAdapters2(&en))
            for (ULONG i = 0; i < en.NumAdapters && !g_kmt.adapter; ++i) {
                struct { ULONG magic, version, width, height, pitch, reserved; ULONGLONG fbPhys, segPhys, segBytes; } info = {};
                D3DKMT_QUERYADAPTERINFO q = {};
                q.hAdapter = list[i].hAdapter;
                q.Type = KMTQAITYPE_UMDRIVERPRIVATE;
                q.pPrivateDriverData = &info;
                q.PrivateDriverDataSize = sizeof info;
                if (!D3DKMTQueryAdapterInfo(&q) && info.magic == 0x41355350u && info.reserved == 20) g_kmt.adapter = list[i].hAdapter;
            }
        D3DKMT_CREATEDEVICE cd = {};
        cd.hAdapter = g_kmt.adapter;
        NTSTATUS st = g_kmt.adapter ? D3DKMTCreateDevice(&cd) : -1;
        if (!st) {
            g_kmt.device = cd.hDevice;
            D3DKMT_CREATECONTEXTVIRTUAL cc = {};
            cc.hDevice = cd.hDevice;
            cc.NodeOrdinal = 1;          // two-node mode: node 1 runs virtual contexts
            cc.EngineAffinity = 1;
            cc.ClientHint = D3DKMT_CLIENTHINT_DX12;
            st = D3DKMTCreateContextVirtual(&cc);
            if (!st) { g_kmt.context = cc.hContext; g_kmt.ok = true; }
        }
        if (g_kmt.ok) {
            D3DKMT_CREATESYNCHRONIZATIONOBJECT2 so = {};
            so.hDevice = g_kmt.device;
            so.Info.Type = D3DDDI_MONITORED_FENCE;
            if (!D3DKMTCreateSynchronizationObject2(&so)) g_kmt.cmdVa = so.Info.MonitoredFence.FenceValueGPUVirtualAddress;
            P5KmtEscape e = {0x45355350u, 5, 0, 0};          // P5E_TICK with key 0: "is it there"
            D3DKMT_ESCAPE es = {};
            es.hAdapter = g_kmt.adapter;
            es.hDevice = g_kmt.device;
            es.Type = D3DKMT_ESCAPE_DRIVERPRIVATE;
            es.pPrivateDriverData = &e;
            es.PrivateDriverDataSize = sizeof e;
            g_kmt.tickOk = g_kmt.cmdVa && !D3DKMTEscape(&es) && e.value == 1;
            P5Log("kernel wait op: %s (command VA %llX)", g_kmt.tickOk ? "available" : "not available, CPU waits", g_kmt.cmdVa);
        }
        P5Log("kernel fence path: adapter %X device %X context %X status %08lX", g_kmt.adapter, g_kmt.device, g_kmt.context, (unsigned long)st);
    }
    return g_kmt.ok;
}
static void KmtFenceWrite(UINT64 va, UINT64 value)
{
    if (!KmtOpen()) return;
    P5KmtOp op;
    memset(&op, 0, sizeof op);
    op.magic = 0x3550504Fu;
    op.kind = 20;                    // OP_FENCE
    op.dstAddr = va;
    op.bytes = value;
    D3DKMT_SUBMITCOMMAND sc = {};
    sc.Commands = va;
    sc.CommandLength = 8;
    sc.BroadcastContextCount = 1;
    sc.BroadcastContext[0] = g_kmt.context;
    sc.pPrivateDriverData = &op;
    sc.PrivateDriverDataSize = sizeof op;
    EnterCriticalSection(&g_kmt.lock);
    NTSTATUS st = D3DKMTSubmitCommand(&sc);
    LeaveCriticalSection(&g_kmt.lock);
    if (st) P5Log("fence write %llX = %llu: status %08lX", va, value, (unsigned long)st);
}
// Kernel wait op: the queue's kernel context waits (in ps5wddm) until we report the tick done. The context is the
// runtime's (a runtime handle), so the submission goes through the runtime's kernel-thunk callback.
static bool KmtWaitTick(const D3DDDI_DEVICECALLBACKS* kt, HANDLE rtDevice, HANDLE kctx, ULONGLONG key, UINT64 tick)
{
    if (!kt || !kt->pfnSubmitCommandCb) return false;
    P5KmtOp op;
    memset(&op, 0, sizeof op);
    op.magic = 0x3550504Fu;
    op.kind = 22;                    // OP_WAITTICK
    op.dstAddr = key;
    op.bytes = tick;
    D3DDDICB_SUBMITCOMMAND sc = {};
    sc.Commands = g_kmt.cmdVa;
    sc.CommandLength = 8;
    sc.BroadcastContextCount = 1;
    sc.BroadcastContext[0] = kctx;
    sc.pPrivateDriverData = &op;
    sc.PrivateDriverDataSize = sizeof op;
    HRESULT hr = kt->pfnSubmitCommandCb(rtDevice, &sc);
    static LONG said;
    if (FAILED(hr) && InterlockedIncrement(&said) <= 10) P5Log("wait op key %llX tick %llu: %08lX", key, tick, (unsigned long)hr);
    return SUCCEEDED(hr);
}
static void KmtTickDone(ULONGLONG key, UINT64 tick)
{
    P5KmtEscape e = {0x45355350u, 5, key, tick};
    D3DKMT_ESCAPE es = {};
    es.hAdapter = g_kmt.adapter;
    es.hDevice = g_kmt.device;
    es.Type = D3DKMT_ESCAPE_DRIVERPRIVATE;
    es.pPrivateDriverData = &e;
    es.PrivateDriverDataSize = sizeof e;
    D3DKMTEscape(&es);
}
static UINT64 KmtFenceRead(UINT64 va)
{
    if (!KmtOpen()) return 0;
    P5KmtEscape e = {0x45355350u, 1, va, 0};
    D3DKMT_ESCAPE es = {};
    es.hAdapter = g_kmt.adapter;
    es.hDevice = g_kmt.device;
    es.hContext = g_kmt.context;
    es.Type = D3DKMT_ESCAPE_DRIVERPRIVATE;
    es.pPrivateDriverData = &e;
    es.PrivateDriverDataSize = sizeof e;
    return D3DKMTEscape(&es) ? 0 : e.value;
}

// ---- descriptor heaps: vkd3d-proton's handles pass straight through ----
struct P5DescHeap { ID3D12DescriptorHeap* h; };
static SIZE_T APIENTRY CalcPrivateDescriptorHeapSize(D3D12DDI_HDEVICE, const D3D12DDIARG_CREATE_DESCRIPTOR_HEAP_0001*) { return sizeof(P5DescHeap); }
static HRESULT APIENTRY CreateDescriptorHeap(D3D12DDI_HDEVICE hd, const D3D12DDIARG_CREATE_DESCRIPTOR_HEAP_0001* a, D3D12DDI_HDESCRIPTORHEAP hh)
{
    P5Trace("CreateDescriptorHeap");
    P5DescHeap* h = new (hh.pDrvPrivate) P5DescHeap();
    // DDI flags: 1 = CPU visible, 2 = shader visible (API: 1 = shader visible)
    D3D12_DESCRIPTOR_HEAP_DESC d = {(D3D12_DESCRIPTOR_HEAP_TYPE)a->Type, a->NumDescriptors,
                                    (a->Flags & D3D12DDI_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    HRESULT hr = Dev(hd)->dev->CreateDescriptorHeap(&d, IID_ID3D12DescriptorHeap, (void**)&h->h);
    if (FAILED(hr)) P5Log("CreateDescriptorHeap type %u n %u flags %X: %08lX", (UINT)d.Type, d.NumDescriptors, (UINT)d.Flags, (unsigned long)hr);
    return hr;
}
static void APIENTRY DestroyDescriptorHeap(D3D12DDI_HDEVICE, D3D12DDI_HDESCRIPTORHEAP hh)
{
    P5Trace("DestroyDescriptorHeap");
    P5DescHeap* h = Obj<P5DescHeap>(hh);
    if (h->h) h->h->Release();
    h->~P5DescHeap();
}
static UINT APIENTRY GetDescriptorSizeInBytes(D3D12DDI_HDEVICE hd, D3D12DDI_DESCRIPTOR_HEAP_TYPE t)
{
    P5Trace("GetDescriptorSizeInBytes");
    return Dev(hd)->dev->GetDescriptorHandleIncrementSize((D3D12_DESCRIPTOR_HEAP_TYPE)t);
}
static D3D12DDI_CPU_DESCRIPTOR_HANDLE APIENTRY GetCPUDescriptorHandleForHeapStart(D3D12DDI_HDEVICE, D3D12DDI_HDESCRIPTORHEAP hh)
{
    P5Trace("GetCPUDescriptorHandleForHeapStart");
    D3D12DDI_CPU_DESCRIPTOR_HANDLE r;
    r.ptr = Obj<P5DescHeap>(hh)->h->GetCPUDescriptorHandleForHeapStart().ptr;
    return r;
}
static D3D12DDI_GPU_DESCRIPTOR_HANDLE APIENTRY GetGPUDescriptorHandleForHeapStart(D3D12DDI_HDEVICE, D3D12DDI_HDESCRIPTORHEAP hh)
{
    P5Trace("GetGPUDescriptorHandleForHeapStart");
    D3D12DDI_GPU_DESCRIPTOR_HANDLE r;
    r.ptr = Obj<P5DescHeap>(hh)->h->GetGPUDescriptorHandleForHeapStart().ptr;
    return r;
}

// ---- command pools (allocators), recorders, lists ----
static D3D12_COMMAND_LIST_TYPE ListType(UINT queueFlags, UINT ddiType)
{
    if (ddiType == D3D12DDI_COMMAND_LIST_TYPE_BUNDLE) return D3D12_COMMAND_LIST_TYPE_BUNDLE;
    if (queueFlags & D3D12DDI_COMMAND_QUEUE_FLAG_3D) return D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (queueFlags & D3D12DDI_COMMAND_QUEUE_FLAG_COMPUTE) return D3D12_COMMAND_LIST_TYPE_COMPUTE;
    if (queueFlags & D3D12DDI_COMMAND_QUEUE_FLAG_COPY) return D3D12_COMMAND_LIST_TYPE_COPY;
    return D3D12_COMMAND_LIST_TYPE_DIRECT;
}
struct P5Pool {
    P5Device* d;
    ID3D12CommandAllocator* alloc[4];          // by list type (direct, bundle, compute, copy), made when first used
    ID3D12CommandAllocator* Get(D3D12_COMMAND_LIST_TYPE t)
    {
        if (!alloc[t] && FAILED(d->dev->CreateCommandAllocator(t, IID_ID3D12CommandAllocator, (void**)&alloc[t]))) alloc[t] = NULL;
        return alloc[t];
    }
};
static SIZE_T APIENTRY CalcPrivateCommandPoolSize(D3D12DDI_HDEVICE, const D3D12DDIARG_CREATE_COMMAND_POOL_0040*) { return sizeof(P5Pool); }
static HRESULT APIENTRY CreateCommandPool(D3D12DDI_HDEVICE hd, const D3D12DDIARG_CREATE_COMMAND_POOL_0040*, D3D12DDI_HCOMMANDPOOL_0040 hp)
{
    P5Trace("CreateCommandPool");
    P5Pool* p = new (hp.pDrvPrivate) P5Pool();
    p->d = Dev(hd);
    return S_OK;
}
static void APIENTRY DestroyCommandPool(D3D12DDI_HDEVICE, D3D12DDI_HCOMMANDPOOL_0040 hp)
{
    P5Trace("DestroyCommandPool");
    P5Pool* p = Obj<P5Pool>(hp);
    for (auto* a : p->alloc) if (a) a->Release();
    p->~P5Pool();
}
static void APIENTRY ResetCommandPool(D3D12DDI_HDEVICE, D3D12DDI_HCOMMANDPOOL_0040 hp)
{
    P5Trace("ResetCommandPool");
    for (auto* a : Obj<P5Pool>(hp)->alloc) if (a) a->Reset();
}
struct P5Recorder { UINT queueFlags; P5Pool* pool; };
static SIZE_T APIENTRY CalcPrivateCommandRecorderSize(D3D12DDI_HDEVICE, const D3D12DDIARG_CREATE_COMMAND_RECORDER_0040*) { return sizeof(P5Recorder); }
static HRESULT APIENTRY CreateCommandRecorder(D3D12DDI_HDEVICE, const D3D12DDIARG_CREATE_COMMAND_RECORDER_0040* a, D3D12DDI_HCOMMANDRECORDER_0040 hr)
{
    P5Trace("CreateCommandRecorder");
    P5Recorder* r = new (hr.pDrvPrivate) P5Recorder();
    r->queueFlags = a->QueueFlags;
    return S_OK;
}
static void APIENTRY DestroyCommandRecorder(D3D12DDI_HDEVICE, D3D12DDI_HCOMMANDRECORDER_0040 hr) { Obj<P5Recorder>(hr)->~P5Recorder(); }
static void APIENTRY CommandRecorderSetCommandPoolAsTarget(D3D12DDI_HDEVICE, D3D12DDI_HCOMMANDRECORDER_0040 hr, D3D12DDI_HCOMMANDPOOL_0040 hp)
{
    P5Trace("CommandRecorderSetCommandPoolAsTarget");
    Obj<P5Recorder>(hr)->pool = Obj<P5Pool>(hp);
}
struct P5List {
    P5Device* d;
    D3D12_COMMAND_LIST_TYPE type;
    ID3D12GraphicsCommandList* l;
    D3D12DDI_HRTCOMMANDLIST rt;
};
static P5List* List(D3D12DDI_HCOMMANDLIST h) { return (P5List*)h.pDrvPrivate; }
static SIZE_T APIENTRY CalcPrivateCommandListSize(D3D12DDI_HDEVICE, const D3D12DDIARG_CREATE_COMMAND_LIST_0040*) { return sizeof(P5List); }
static HRESULT APIENTRY CreateCommandList(D3D12DDI_HDEVICE hd, const D3D12DDIARG_CREATE_COMMAND_LIST_0040* a, D3D12DDI_HCOMMANDLIST hl, D3D12DDI_HRTCOMMANDLIST rt)
{
    P5Trace("CreateCommandList");
    P5List* l = new (hl.pDrvPrivate) P5List();
    l->d = Dev(hd);
    l->type = ListType(a->QueueFlags, a->Type);
    l->rt = rt;
    // Every new command list must be told which of our filled tables it uses, or the runtime falls back to stubs.
    const D3D12DDI_CORELAYER_DEVICECALLBACKS_0050* cb = (const D3D12DDI_CORELAYER_DEVICECALLBACKS_0050*)l->d->um;
    if (cb && cb->pfnSetCommandListDDITableCb) cb->pfnSetCommandListDDITableCb(rt, l->d->adapter->listTable[0]);
    return S_OK;
}
static void APIENTRY DestroyCommandList(D3D12DDI_HDEVICE, D3D12DDI_HCOMMANDLIST hl)
{
    P5Trace("DestroyCommandList");
    P5List* l = List(hl);
    if (l->l) l->l->Release();
    l->~P5List();
}
static void APIENTRY ResetCommandList(D3D12DDI_HCOMMANDLIST hl, const D3D12DDIARG_RESETCOMMANDLIST_0040* a)
{
    P5Trace("ResetCommandList");
    P5List* l = List(hl);
    P5Recorder* r = Obj<P5Recorder>(a->hDrvCommandRecorder);
    ID3D12CommandAllocator* alloc = r && r->pool ? r->pool->Get(l->type) : NULL;
    if (!alloc) { P5Log("ResetCommandList: no allocator (recorder %p)", (void*)r); return; }
    HRESULT hr;
    if (!l->l) hr = l->d->dev->CreateCommandList(0, l->type, alloc, NULL, IID_ID3D12GraphicsCommandList, (void**)&l->l);
    else hr = l->l->Reset(alloc, NULL);
    if (FAILED(hr)) P5Log("ResetCommandList type %u: %08lX", (UINT)l->type, (unsigned long)hr);
}
static void APIENTRY CloseCommandList(D3D12DDI_HCOMMANDLIST hl)
{
    P5Trace("CloseCommandList");
    P5List* l = List(hl);
    if (l->l) {
        HRESULT hr = l->l->Close();
        if (FAILED(hr)) P5Log("CloseCommandList: %08lX", (unsigned long)hr);
    }
}

// ---- fences ----
struct P5Fence { UINT64 va; UINT64 monitoredVa; };
static SIZE_T APIENTRY CalcPrivateFenceSize(D3D12DDI_HDEVICE, const D3D12DDIARG_CREATE_FENCE*) { return sizeof(P5Fence); }
static HRESULT APIENTRY CreateFence(D3D12DDI_HDEVICE, D3D12DDI_HFENCE hf, const D3D12DDIARG_CREATE_FENCE* a)
{
    P5Trace("CreateFence");
    P5Fence* f = new (hf.pDrvPrivate) P5Fence();
    if (a->FenceCount) { f->va = a->Fences[0].FenceValue.BaseAddress; f->monitoredVa = a->Fences[0].FenceMonitoredValue.BaseAddress; }
    static LONG said;
    if (InterlockedIncrement(&said) <= 10) P5Log("CreateFence: %u, value at GPU VA %llX, monitored %llX", a->FenceCount, f->va, f->monitoredVa);
    return S_OK;
}
static void APIENTRY DestroyFence(D3D12DDI_HDEVICE, D3D12DDI_HFENCE hf) { Obj<P5Fence>(hf)->~P5Fence(); }

// ---- command queues ----
struct P5Queue {
    P5Device* d;
    ID3D12CommandQueue* q;
    ID3D12Fence* f;                            // private vkd3d fence: one tick per Windows fence signal
    UINT64 ticks;
    struct Job { UINT64 tick, va, value; bool tickOnly; };   // tickOnly: tell ps5wddm the tick is done (wait op)
    std::deque<Job> jobs;
    CRITICAL_SECTION lock;
    HANDLE wake, thread, done;
    HANDLE kctx;                               // the runtime-visible kernel context of this queue (virtual: fence signals)
    HANDLE pctx;                               // physical context for presents (blt presents run there)
    ULONGLONG key;                             // kernel wait op: this queue's key (process id << 32 | serial)
    bool kwait;                                // ExecuteCommandLists uses the kernel wait op instead of a CPU wait
    bool stop;
};
static DWORD WINAPI QueueWorker(void* p)
{
    P5Queue* q = (P5Queue*)p;
    for (;;) {
        WaitForSingleObject(q->wake, INFINITE);
        for (;;) {
            EnterCriticalSection(&q->lock);
            if (q->jobs.empty()) { bool s = q->stop; LeaveCriticalSection(&q->lock); if (s) return 0; break; }
            P5Queue::Job j = q->jobs.front();
            q->jobs.pop_front();
            LeaveCriticalSection(&q->lock);
            if (q->f->GetCompletedValue() < j.tick) {
                q->f->SetEventOnCompletion(j.tick, q->done);
                WaitForSingleObject(q->done, INFINITE);
            }
            if (j.tickOnly) KmtTickDone(q->key, j.tick);
            else KmtFenceWrite(j.va, j.value);
        }
    }
}
static SIZE_T APIENTRY CalcPrivateCommandQueueSize(D3D12DDI_HDEVICE, const D3D12DDIARG_CREATECOMMANDQUEUE_0050*) { return sizeof(P5Queue); }
static HRESULT APIENTRY CreateCommandQueue(D3D12DDI_HDEVICE hd, const D3D12DDIARG_CREATECOMMANDQUEUE_0050* a, D3D12DDI_HCOMMANDQUEUE hq, D3D12DDI_HRTCOMMANDQUEUE rtq)
{
    P5Trace("CreateCommandQueue");
    P5Queue* q = new (hq.pDrvPrivate) P5Queue();
    q->d = Dev(hd);
    D3D12_COMMAND_QUEUE_DESC d = {ListType(a->QueueFlags, 0), 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0};
    HRESULT hr = q->d->dev->CreateCommandQueue(&d, IID_ID3D12CommandQueue, (void**)&q->q);
    if (SUCCEEDED(hr)) hr = q->d->dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_ID3D12Fence, (void**)&q->f);
    if (FAILED(hr)) { P5Log("CreateCommandQueue flags %X: %08lX", (UINT)a->QueueFlags, (unsigned long)hr); return hr; }
    // A kernel context for the runtime's queue (virtual, node 1): the runtime signals and waits on Windows fences there.
    const D3D12DDI_CORELAYER_DEVICECALLBACKS_0050* cb = (const D3D12DDI_CORELAYER_DEVICECALLBACKS_0050*)q->d->um;
    D3DDDICB_CREATECONTEXTVIRTUAL cv = {};
    cv.NodeOrdinal = 1;
    cv.EngineAffinity = 1;
    HRESULT hc = cb && cb->pfnCreateContextVirtualCb ? cb->pfnCreateContextVirtualCb(rtq, &cv) : E_NOTIMPL;
    P5Log("queue kernel context: %08lX handle %p", (unsigned long)hc, cv.hContext);
    q->kctx = cv.hContext;
    D3DDDICB_CREATECONTEXT cp = {};
    cp.NodeOrdinal = 0;
    cp.EngineAffinity = 1;
    HRESULT hp = cb && cb->pfnCreateContextCb ? cb->pfnCreateContextCb(rtq, &cp) : E_NOTIMPL;
    q->pctx = SUCCEEDED(hp) ? cp.hContext : NULL;
    P5Log("queue present context: %08lX handle %p", (unsigned long)hp, cp.hContext);
    InitializeCriticalSection(&q->lock);
    q->wake = CreateEventA(NULL, FALSE, FALSE, NULL);
    q->done = CreateEventA(NULL, FALSE, FALSE, NULL);
    q->thread = CreateThread(NULL, 0, QueueWorker, q, 0, NULL);
    // P5D3D12_KWAIT=0: CPU wait in ExecuteCommandLists as before (comparison switch)
    static LONG serial;
    char kw[8] = "";
    bool off = GetEnvironmentVariableA("P5D3D12_KWAIT", kw, sizeof kw) && kw[0] == '0';
    q->key = ((ULONGLONG)GetCurrentProcessId() << 32) | (ULONG)InterlockedIncrement(&serial);
    q->kwait = !off && q->kctx && KmtOpen() && g_kmt.tickOk;
    P5Log("queue key %llX: %s", q->key, q->kwait ? "kernel wait op" : "CPU wait in ExecuteCommandLists");
    P5Log("CreateCommandQueue flags %X -> type %u", (UINT)a->QueueFlags, (UINT)d.Type);
    return S_OK;
}
static void APIENTRY DestroyCommandQueue(D3D12DDI_HDEVICE, D3D12DDI_HCOMMANDQUEUE hq)
{
    P5Trace("DestroyCommandQueue");
    P5Queue* q = Obj<P5Queue>(hq);
    if (q->thread) {
        EnterCriticalSection(&q->lock); q->stop = true; LeaveCriticalSection(&q->lock);
        SetEvent(q->wake);
        WaitForSingleObject(q->thread, 10000);
        CloseHandle(q->thread); CloseHandle(q->wake); CloseHandle(q->done);
        DeleteCriticalSection(&q->lock);
    }
    if (q->f) q->f->Release();
    if (q->q) q->q->Release();
    q->~P5Queue();
}
static void APIENTRY ExecuteCommandLists(D3D12DDI_HCOMMANDQUEUE hq, UINT n, const D3D12DDI_HCOMMANDLIST* lists)
{
    P5Trace("ExecuteCommandLists");
    P5Queue* q = Obj<P5Queue>(hq);
    ID3D12CommandList* v[64];
    UINT k = 0;
    for (UINT i = 0; i < n && k < 64; ++i) if (List(lists[i])->l) v[k++] = List(lists[i])->l;
    if (!k) return;
    static LONG nth;
    LONG me = InterlockedIncrement(&nth);
    if (me <= 12) P5Log("execute #%ld: %u lists", me, k);
    q->q->ExecuteCommandLists(k, v);
    // The runtime signals its fences on the queue's kernel context, which knows nothing of vkd3d-proton's work: wait
    // here until the GPU has finished these lists, so a Signal that follows is never early. (Phase 4: a kernel-side
    // wait op on that context instead, so the CPU does not wait.)
    EnterCriticalSection(&q->lock);
    UINT64 t = ++q->ticks;
    q->q->Signal(q->f, t);
    LeaveCriticalSection(&q->lock);
    // P5D3D12_ECLWAIT=0 (test switch): no wait here; the runtime's kernel-context signals may then come early.
    static int noWait = -1;
    if (noWait < 0) { char e[8] = ""; noWait = GetEnvironmentVariableA("P5D3D12_ECLWAIT", e, sizeof e) && e[0] == '0'; if (noWait) P5Log("ExecuteCommandLists: no CPU wait (P5D3D12_ECLWAIT=0)"); }
    LARGE_INTEGER w0, w1, wf;
    QueryPerformanceCounter(&w0);
    if (q->kwait && KmtWaitTick(q->d->kt, q->d->rt.handle, q->kctx, q->key, t)) {
        // The kernel context now waits for tick t; the worker reports it done once vkd3d-proton's fence reaches it.
        EnterCriticalSection(&q->lock);
        q->jobs.push_back({t, 0, 0, true});
        LeaveCriticalSection(&q->lock);
        SetEvent(q->wake);
    } else if (!noWait && q->f->GetCompletedValue() < t) {
        HANDLE ev = CreateEventA(NULL, FALSE, FALSE, NULL);
        q->f->SetEventOnCompletion(t, ev);
        if (WaitForSingleObject(ev, 10000) != WAIT_OBJECT_0) P5Log("ExecuteCommandLists: GPU not done after 10 s");
        CloseHandle(ev);
    }
    QueryPerformanceCounter(&w1);
    QueryPerformanceFrequency(&wf);
    static double waited;                      // CPU time spent waiting here, logged every 600 executes
    waited += (w1.QuadPart - w0.QuadPart) * 1000.0 / wf.QuadPart;
    if (me % 600 == 0) { P5Log("execute #%ld: %.2f ms waited per execute (last 600)", me, waited / 600); waited = 0; }
    if (me <= 12) P5Log("execute #%ld done", me);
}
static void APIENTRY SignalFence(D3D12DDI_HCOMMANDQUEUE hq, D3D12DDIARG_FENCE_OPERATION* a)
{
    P5Trace("SignalFence");
    P5Queue* q = Obj<P5Queue>(hq);
    P5Fence* f = Obj<P5Fence>(a->Fence);
    a->PhysicalAdapterMask = 1;
    EnterCriticalSection(&q->lock);
    UINT64 t = ++q->ticks;
    q->q->Signal(q->f, t);
    q->jobs.push_back({t, f->va, a->Value, false});
    LeaveCriticalSection(&q->lock);
    SetEvent(q->wake);
}
static void APIENTRY WaitForFence(D3D12DDI_HCOMMANDQUEUE, D3D12DDIARG_FENCE_OPERATION* a)
{
    P5Trace("WaitForFence");
    // Simple form: the CPU waits here until the Windows fence has the value (GPU-side waits come later).
    P5Fence* f = Obj<P5Fence>(a->Fence);
    a->PhysicalAdapterMask = 1;
    for (DWORD t0 = GetTickCount(); KmtFenceRead(f->va) < a->Value; Sleep(1))
        if (GetTickCount() - t0 > 5000) { P5Log("WaitForFence %llX >= %llu: still waiting after 5 s, going on", f->va, a->Value); break; }
}
static void APIENTRY GetDebugAllocationInfo(D3D12DDI_HDEVICE, D3D12DDI_HANDLE_AND_TYPE, UINT* nva, D3D12DDI_DEBUG_VIRTUAL_ADDRESS_ALLOCATION_INFO_0012*, UINT* nkmt, D3D12DDI_DEBUG_KMT_ALLOCATION_INFO_0014*)
{
    P5Trace("GetDebugAllocationInfo");
    if (nva) *nva = 0;
    if (nkmt) *nkmt = 0;
}
