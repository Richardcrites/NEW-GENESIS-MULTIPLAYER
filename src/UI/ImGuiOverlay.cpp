#include "UI/ImGuiOverlay.h"
#include "UI/ConnectScreen.h"

#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <MinHook.h>

#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx12.h>

#include <atomic>

// Forward declare ImGui Win32 message handler
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

using namespace std::string_view_literals;

namespace Pulsar::UI
{
    // ─── State ───
    static std::atomic<bool> s_visible{false};
    static bool s_initialized = false;
    static std::atomic<int> s_renderCount{0};

    static ID3D12Device*              s_device = nullptr;
    static ID3D12DescriptorHeap*      s_srvHeap = nullptr;
    static ID3D12CommandAllocator*    s_cmdAlloc = nullptr;
    static ID3D12GraphicsCommandList* s_cmdList = nullptr;
    static ID3D12CommandQueue*        s_cmdQueue = nullptr;
    static ID3D12Fence*               s_fence = nullptr;
    static HANDLE                     s_fenceEvent = nullptr;
    static UINT64                     s_fenceValue = 0;
    static IDXGISwapChain3*           s_swapChain = nullptr;
    static HWND                       s_hwnd = nullptr;
    static WNDPROC                    s_origWndProc = nullptr;

    static constexpr int MAX_BACK_BUFFERS = 4;
    static UINT                       s_bufferCount = 0;
    static DXGI_FORMAT                s_bufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    static ID3D12Resource*            s_backBuffers[MAX_BACK_BUFFERS] = {};
    static D3D12_CPU_DESCRIPTOR_HANDLE s_rtvHandles[MAX_BACK_BUFFERS] = {};
    static ID3D12DescriptorHeap*      s_rtvHeap = nullptr;

    // ─── Present Hook ───
    using PresentFn = HRESULT(WINAPI*)(IDXGISwapChain*, UINT, UINT);
    static PresentFn s_origPresent = nullptr;

    using ResizeBuffersFn = HRESULT(WINAPI*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
    static ResizeBuffersFn s_origResizeBuffers = nullptr;

    using ExecuteCommandListsFn = void(WINAPI*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
    static ExecuteCommandListsFn s_origExecuteCommandLists = nullptr;

    // ─── WndProc Hook ───
    static LRESULT CALLBACK HookedWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        // F2 toggles overlay
        if (msg == WM_KEYDOWN && wParam == VK_F2) {
            s_visible = !s_visible;
            PLOG_INFO("[PULSAR] ImGui: F2 pressed, visible={}"sv, s_visible.load());
            return 0;
        }

        // When overlay is visible, let ImGui handle input
        // (skip ImGui input handler until first frame has rendered to avoid crash)
        if (s_visible && s_renderCount > 0) {
            if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
                return 0;

            if (msg >= WM_KEYFIRST && msg <= WM_KEYLAST)
                return 0;
            if (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST)
                return 0;
        }

        return CallWindowProcW(s_origWndProc, hWnd, msg, wParam, lParam);
    }

    // ─── ImGui Init (called once on first Present) ───
    static void InitImGui(IDXGISwapChain* a_swapChain)
    {
        if (s_initialized) return;
        // Only try once
        static bool s_tried = false;
        if (s_tried) return;
        s_tried = true;

        // Need command queue first (captured from ExecuteCommandLists hook)
        if (!s_cmdQueue) {
            s_tried = false; // retry next frame
            return;
        }

        // Get D3D12 device from swap chain
        if (FAILED(a_swapChain->GetDevice(IID_PPV_ARGS(&s_device)))) {
            PLOG_WARN("[PULSAR] ImGui: GetDevice failed"sv);
            return;
        }

        // Query SwapChain3
        if (FAILED(a_swapChain->QueryInterface(IID_PPV_ARGS(&s_swapChain)))) {
            PLOG_WARN("[PULSAR] ImGui: QueryInterface SwapChain3 failed"sv);
            return;
        }

        // Get swap chain desc for HWND and format
        DXGI_SWAP_CHAIN_DESC desc{};
        a_swapChain->GetDesc(&desc);
        s_hwnd = desc.OutputWindow;
        UINT bufferCount = desc.BufferCount;
        DXGI_FORMAT format = desc.BufferDesc.Format;

        PLOG_INFO("[PULSAR] ImGui: SwapChain buffers={} format={} hwnd={}"sv,
            bufferCount, static_cast<int>(format), (uintptr_t)s_hwnd);

        if (bufferCount > MAX_BACK_BUFFERS) bufferCount = MAX_BACK_BUFFERS;
        s_bufferCount = bufferCount;
        s_bufferFormat = format;

        // Create SRV descriptor heap for ImGui fonts
        {
            D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
            heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            heapDesc.NumDescriptors = 1;
            heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            if (FAILED(s_device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&s_srvHeap)))) {
                PLOG_WARN("[PULSAR] ImGui: SRV heap failed"sv);
                return;
            }
        }

        // Create RTV descriptor heap
        {
            D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
            heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            heapDesc.NumDescriptors = bufferCount;
            if (FAILED(s_device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&s_rtvHeap)))) {
                PLOG_WARN("[PULSAR] ImGui: RTV heap failed"sv);
                return;
            }

            auto rtvSize = s_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
            auto rtvHandle = s_rtvHeap->GetCPUDescriptorHandleForHeapStart();

            for (UINT i = 0; i < bufferCount; i++) {
                s_rtvHandles[i] = rtvHandle;
                rtvHandle.ptr += rtvSize;
                if (FAILED(a_swapChain->GetBuffer(i, IID_PPV_ARGS(&s_backBuffers[i])))) {
                    PLOG_WARN("[PULSAR] ImGui: GetBuffer {} failed"sv, i);
                    return;
                }
                s_device->CreateRenderTargetView(s_backBuffers[i], nullptr, s_rtvHandles[i]);
            }
        }

        // Create command allocator + command list
        if (FAILED(s_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&s_cmdAlloc)))) {
            PLOG_WARN("[PULSAR] ImGui: CreateCommandAllocator failed"sv);
            return;
        }
        if (FAILED(s_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, s_cmdAlloc, nullptr, IID_PPV_ARGS(&s_cmdList)))) {
            PLOG_WARN("[PULSAR] ImGui: CreateCommandList failed"sv);
            return;
        }
        s_cmdList->Close();

        // Init ImGui
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        ImGui::StyleColorsDark();

        ImGui_ImplWin32_Init(s_hwnd);
        ImGui_ImplDX12_Init(s_device, bufferCount, format,
            s_srvHeap,
            s_srvHeap->GetCPUDescriptorHandleForHeapStart(),
            s_srvHeap->GetGPUDescriptorHandleForHeapStart());

        // Create fence for GPU synchronization
        s_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&s_fence));
        s_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        s_fenceValue = 0;

        // Upload font texture to GPU
        s_cmdAlloc->Reset();
        s_cmdList->Reset(s_cmdAlloc, nullptr);
        ImGui_ImplDX12_CreateDeviceObjects();
        s_cmdList->Close();
        ID3D12CommandList* initCmds[] = { s_cmdList };
        s_cmdQueue->ExecuteCommandLists(1, initCmds);

        // Wait for font upload to complete
        s_fenceValue++;
        s_cmdQueue->Signal(s_fence, s_fenceValue);
        if (s_fence->GetCompletedValue() < s_fenceValue) {
            s_fence->SetEventOnCompletion(s_fenceValue, s_fenceEvent);
            WaitForSingleObject(s_fenceEvent, 5000);
        }

        // Hook WndProc for input
        s_origWndProc = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrW(s_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(HookedWndProc)));

        s_initialized = true;
        PLOG_INFO("[PULSAR] ImGui overlay initialized (F2 to toggle)"sv);
    }

    // ─── Hooked Present ───
    static int DoRender()
    {
        __try {
            ImGui_ImplDX12_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();

            DrawConnectScreen();

            ImGui::EndFrame();
            ImGui::Render();

            UINT backIdx = s_swapChain->GetCurrentBackBufferIndex();
            if (backIdx >= s_bufferCount || !s_backBuffers[backIdx])
                return 0;

            // Hard fence wait - GPU must be completely idle before we reuse allocator
            s_fenceValue++;
            s_cmdQueue->Signal(s_fence, s_fenceValue);
            if (s_fence->GetCompletedValue() < s_fenceValue) {
                s_fence->SetEventOnCompletion(s_fenceValue, s_fenceEvent);
                WaitForSingleObject(s_fenceEvent, 5000);
            }

            s_cmdAlloc->Reset();
            s_cmdList->Reset(s_cmdAlloc, nullptr);

            // Barrier: PRESENT → RENDER_TARGET
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = s_backBuffers[backIdx];
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            s_cmdList->ResourceBarrier(1, &barrier);

            s_cmdList->OMSetRenderTargets(1, &s_rtvHandles[backIdx], FALSE, nullptr);
            ID3D12DescriptorHeap* heaps[] = { s_srvHeap };
            s_cmdList->SetDescriptorHeaps(1, heaps);

            ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), s_cmdList);

            // Barrier: RENDER_TARGET → PRESENT
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
            s_cmdList->ResourceBarrier(1, &barrier);

            s_cmdList->Close();

            ID3D12CommandList* cmdLists[] = { s_cmdList };
            s_cmdQueue->ExecuteCommandLists(1, cmdLists);

            s_renderCount++;
            return 1;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return -1;
        }
    }

    static HRESULT WINAPI HookedPresent(IDXGISwapChain* a_swapChain, UINT a_syncInterval, UINT a_flags)
    {
        InitImGui(a_swapChain);

        if (s_initialized && s_visible && s_cmdQueue) {
            DoRender();
        }

        return s_origPresent(a_swapChain, a_syncInterval, a_flags);
    }

    // ─── Hooked ExecuteCommandLists (to capture command queue) ───
    static void WINAPI HookedExecuteCommandLists(ID3D12CommandQueue* a_queue, UINT a_numCmdLists,
        ID3D12CommandList* const* a_cmdLists)
    {
        if (!s_cmdQueue) {
            D3D12_COMMAND_QUEUE_DESC desc = a_queue->GetDesc();
            if (desc.Type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
                s_cmdQueue = a_queue;
            }
        }
        s_origExecuteCommandLists(a_queue, a_numCmdLists, a_cmdLists);
    }

    // ─── Hooked ResizeBuffers (handle window resize) ───
    static HRESULT WINAPI HookedResizeBuffers(IDXGISwapChain* a_swapChain, UINT a_bufCount,
        UINT a_width, UINT a_height, DXGI_FORMAT a_format, UINT a_flags)
    {
        // Release back buffer references before resize
        for (UINT i = 0; i < MAX_BACK_BUFFERS; i++) {
            if (s_backBuffers[i]) {
                s_backBuffers[i]->Release();
                s_backBuffers[i] = nullptr;
            }
        }

        HRESULT hr = s_origResizeBuffers(a_swapChain, a_bufCount, a_width, a_height, a_format, a_flags);

        // Re-acquire back buffers
        if (SUCCEEDED(hr) && s_device && s_rtvHeap) {
            UINT count = a_bufCount ? a_bufCount : s_bufferCount;
            if (count > MAX_BACK_BUFFERS) count = MAX_BACK_BUFFERS;
            s_bufferCount = count;
            if (a_format != DXGI_FORMAT_UNKNOWN) s_bufferFormat = a_format;
            for (UINT i = 0; i < count; i++) {
                a_swapChain->GetBuffer(i, IID_PPV_ARGS(&s_backBuffers[i]));
                s_device->CreateRenderTargetView(s_backBuffers[i], nullptr, s_rtvHandles[i]);
            }
        }

        return hr;
    }

    // ─── Public API ───

    void InitOverlay()
    {
        // Create a temporary D3D12 device + swap chain to get the vtable pointers
        // Then hook Present via MinHook

        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"PulsarDummy";
        RegisterClassExW(&wc);

        HWND dummyHwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW,
            0, 0, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);

        IDXGIFactory4* factory = nullptr;
        CreateDXGIFactory1(IID_PPV_ARGS(&factory));

        ID3D12Device* dummyDevice = nullptr;
        D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dummyDevice));

        D3D12_COMMAND_QUEUE_DESC queueDesc{};
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        ID3D12CommandQueue* dummyQueue = nullptr;
        dummyDevice->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&dummyQueue));

        DXGI_SWAP_CHAIN_DESC1 swapDesc{};
        swapDesc.BufferCount = 2;
        swapDesc.Width = 100;
        swapDesc.Height = 100;
        swapDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        swapDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swapDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        swapDesc.SampleDesc.Count = 1;

        IDXGISwapChain1* dummySwap1 = nullptr;
        factory->CreateSwapChainForHwnd(dummyQueue, dummyHwnd, &swapDesc, nullptr, nullptr, &dummySwap1);

        // Get vtable pointers
        void** swapVtable = *reinterpret_cast<void***>(dummySwap1);
        void** queueVtable = *reinterpret_cast<void***>(dummyQueue);

        void* presentTarget = swapVtable[8];   // IDXGISwapChain::Present
        void* resizeTarget = swapVtable[13];    // IDXGISwapChain::ResizeBuffers
        void* executeTarget = queueVtable[10];  // ID3D12CommandQueue::ExecuteCommandLists

        // Cleanup dummy objects
        dummySwap1->Release();
        dummyQueue->Release();
        dummyDevice->Release();
        factory->Release();
        DestroyWindow(dummyHwnd);
        UnregisterClassW(wc.lpszClassName, wc.hInstance);

        // Hook Present
        MH_STATUS st = MH_CreateHook(presentTarget, &HookedPresent,
            reinterpret_cast<LPVOID*>(&s_origPresent));
        if (st == MH_OK) MH_EnableHook(presentTarget);
        PLOG_INFO("[PULSAR] Present hook: {}"sv, st == MH_OK ? "OK" : "FAILED");

        // Hook ResizeBuffers
        st = MH_CreateHook(resizeTarget, &HookedResizeBuffers,
            reinterpret_cast<LPVOID*>(&s_origResizeBuffers));
        if (st == MH_OK) MH_EnableHook(resizeTarget);

        // Hook ExecuteCommandLists (to capture the real command queue)
        st = MH_CreateHook(executeTarget, &HookedExecuteCommandLists,
            reinterpret_cast<LPVOID*>(&s_origExecuteCommandLists));
        if (st == MH_OK) MH_EnableHook(executeTarget);
    }

    void ToggleOverlay()
    {
        s_visible = !s_visible;
    }

    bool IsOverlayVisible()
    {
        return s_visible;
    }
}
