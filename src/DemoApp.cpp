#include "DemoApp.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace
{
    struct DemoVertex
    {
        float px, py, pz;
        float nx, ny, nz;
        float u, v;
    };

    constexpr DemoVertex kVertices[] = {
        {-0.78f, -0.72f, 0.50f, 0.0f, 0.0f, -1.0f, 0.00f, 1.00f},
        { 0.00f,  0.78f, 0.50f, 0.0f, 0.0f, -1.0f, 0.50f, 0.00f},
        { 0.70f, -0.70f, 0.50f, 0.0f, 0.0f, -1.0f, 1.00f, 1.00f},

        { 0.10f, -0.20f, 0.35f, 0.0f, 0.0f, -1.0f, 0.10f, 0.90f},
        { 0.85f,  0.30f, 0.35f, 0.0f, 0.0f, -1.0f, 0.95f, 0.20f},
        { 0.92f, -0.80f, 0.35f, 0.0f, 0.0f, -1.0f, 0.85f, 0.98f},

        {-0.94f,  0.88f, 0.20f, 0.0f, 0.0f, -1.0f, 0.00f, 0.00f},
        {-0.938f, 0.882f, 0.20f, 0.0f, 0.0f, -1.0f, 0.50f, 0.00f},
        {-0.936f, 0.878f, 0.20f, 0.0f, 0.0f, -1.0f, 0.25f, 0.50f},
    };

    constexpr uint32_t kIndices[] = {
        0, 1, 2,
        3, 4, 5,
        6, 7, 8,
    };
    constexpr uint32_t kStaticTriangleCount = static_cast<uint32_t>(std::size(kIndices) / 3u);
    constexpr uint32_t kProceduralTriangleCapacity = 96u;
    constexpr uint32_t kMaxTriangleCapacity = 128u;
    constexpr uint32_t kTextureWidth = 2;
    constexpr uint32_t kTextureHeight = 2;
    constexpr uint32_t kTexturePixels[] = {
        0xffff4040u, 0xff40ff40u,
        0xff4040ffu, 0xffffffffu,
    };
}

namespace woah
{
    LRESULT CALLBACK DemoApp::WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (message == WM_DESTROY)
        {
            PostQuitMessage(0);
            return 0;
        }

        return DefWindowProc(hwnd, message, wParam, lParam);
    }

    bool DemoApp::Initialize(HINSTANCE instance, int showCommand)
    {
        if (!CreateWindowClass(instance, showCommand)) return false;
        if (!InitializeD3D12()) return false;
        if (!CreateCommandObjects()) return false;
        if (!CreateSwapchain()) return false;
        if (!CreateDescriptorHeaps()) return false;
        if (!CreateGeometry()) return false;
        if (!CreateTexture()) return false;
        if (!CreateRasterizer()) return false;
        if (!InitializeProfiling()) return false;
        return true;
    }

    bool DemoApp::CreateWindowClass(HINSTANCE instance, int showCommand)
    {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = WindowProc;
        wc.hInstance = instance;
        wc.lpszClassName = L"WoahManRasterizerWindow";
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        if (!RegisterClassW(&wc))
        {
            return false;
        }

        RECT rect = {0, 0, static_cast<LONG>(m_width), static_cast<LONG>(m_height)};
        AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);

        m_window = CreateWindowExW(
            0,
            wc.lpszClassName,
            L"Compute Software Rasterizer",
            WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT, CW_USEDEFAULT,
            rect.right - rect.left,
            rect.bottom - rect.top,
            nullptr,
            nullptr,
            instance,
            nullptr);

        if (!m_window)
        {
            return false;
        }

        ShowWindow(m_window, showCommand);
        return true;
    }

    bool DemoApp::InitializeD3D12()
    {
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&m_factory))))
        {
            return false;
        }

        ComPtr<IDXGIAdapter1> adapter;
        for (UINT adapterIndex = 0;
             m_factory->EnumAdapters1(adapterIndex, &adapter) != DXGI_ERROR_NOT_FOUND;
             ++adapterIndex)
        {
            DXGI_ADAPTER_DESC1 desc = {};
            adapter->GetDesc1(&desc);
            if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
            {
                continue;
            }

            if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&m_device))))
            {
                return true;
            }
        }

        return SUCCEEDED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&m_device)));
    }

    bool DemoApp::CreateCommandObjects()
    {
        D3D12_COMMAND_QUEUE_DESC queueDesc = {};
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(m_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_commandQueue))))
        {
            return false;
        }

        for (uint32_t i = 0; i < kFrameCount; ++i)
        {
            if (FAILED(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_allocators[i]))))
            {
                return false;
            }
        }

        if (FAILED(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_allocators[0].Get(), nullptr, IID_PPV_ARGS(&m_commandList))))
        {
            return false;
        }

        if (FAILED(m_commandList->Close()))
        {
            return false;
        }

        if (FAILED(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence))))
        {
            return false;
        }

        m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        return m_fenceEvent != nullptr;
    }

    bool DemoApp::CreateSwapchain()
    {
        DXGI_SWAP_CHAIN_DESC1 desc = {};
        desc.BufferCount = kFrameCount;
        desc.Width = m_width;
        desc.Height = m_height;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        desc.SampleDesc.Count = 1;

        ComPtr<IDXGISwapChain1> swapchain1;
        if (FAILED(m_factory->CreateSwapChainForHwnd(m_commandQueue.Get(), m_window, &desc, nullptr, nullptr, &swapchain1)))
        {
            return false;
        }

        if (FAILED(swapchain1.As(&m_swapchain)))
        {
            return false;
        }

        m_frameIndex = m_swapchain->GetCurrentBackBufferIndex();
        return true;
    }

    bool DemoApp::CreateDescriptorHeaps()
    {
        D3D12_DESCRIPTOR_HEAP_DESC rtvDesc = {};
        rtvDesc.NumDescriptors = kFrameCount;
        rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        if (FAILED(m_device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&m_rtvHeap))))
        {
            return false;
        }

        m_rtvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
        for (uint32_t i = 0; i < kFrameCount; ++i)
        {
            if (FAILED(m_swapchain->GetBuffer(i, IID_PPV_ARGS(&m_backbuffers[i]))))
            {
                return false;
            }
            m_device->CreateRenderTargetView(m_backbuffers[i].Get(), nullptr, rtvHandle);
            rtvHandle.ptr += m_rtvDescriptorSize;
        }

        D3D12_DESCRIPTOR_HEAP_DESC srvUavDesc = {};
        srvUavDesc.NumDescriptors = ComputeSoftwareRasterizer::kDescriptorCount;
        srvUavDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        srvUavDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(m_device->CreateDescriptorHeap(&srvUavDesc, IID_PPV_ARGS(&m_srvUavHeap))))
        {
            return false;
        }

        m_srvUavDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        m_srvTable = m_srvUavHeap->GetGPUDescriptorHandleForHeapStart();
        m_uavTable = m_srvTable;
        m_uavTable.ptr += SIZE_T(m_srvUavDescriptorSize) * ComputeSoftwareRasterizer::kSrvDescriptorCount;
        return true;
    }

    ComPtr<ID3D12Resource> DemoApp::CreateUploadBuffer(const void* data, uint64_t byteSize)
    {
        D3D12_HEAP_PROPERTIES heap = {};
        heap.Type = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = byteSize;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        ComPtr<ID3D12Resource> resource;
        if (FAILED(m_device->CreateCommittedResource(
                &heap,
                D3D12_HEAP_FLAG_NONE,
                &desc,
                D3D12_RESOURCE_STATE_GENERIC_READ,
                nullptr,
                IID_PPV_ARGS(&resource))))
        {
            return {};
        }

        void* mapped = nullptr;
        D3D12_RANGE range = {0, 0};
        if (FAILED(resource->Map(0, &range, &mapped)))
        {
            return {};
        }

        std::memcpy(mapped, data, static_cast<size_t>(byteSize));
        resource->Unmap(0, nullptr);
        return resource;
    }

    bool DemoApp::CreateGeometry()
    {
        m_vertexBuffer = CreateUploadBuffer(kVertices, sizeof(kVertices));
        std::vector<uint32_t> indexData(kMaxTriangleCapacity * 3u, 0u);
        std::memcpy(indexData.data(), kIndices, sizeof(kIndices));
        m_indexBuffer = CreateUploadBuffer(indexData.data(), uint64_t(indexData.size()) * sizeof(uint32_t));
        return m_vertexBuffer && m_indexBuffer;
    }

    bool DemoApp::UploadTexture2D(ID3D12Resource* texture, const void* pixels, uint32_t width, uint32_t height, DXGI_FORMAT)
    {
        uint64_t uploadBytes = 0;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
        uint32_t rowCount = 0;
        uint64_t rowSize = 0;
        D3D12_RESOURCE_DESC textureDesc = texture->GetDesc();
        m_device->GetCopyableFootprints(&textureDesc, 0, 1, 0, &footprint, &rowCount, &rowSize, &uploadBytes);

        D3D12_HEAP_PROPERTIES heap = {};
        heap.Type = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC uploadDesc = {};
        uploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        uploadDesc.Width = uploadBytes;
        uploadDesc.Height = 1;
        uploadDesc.DepthOrArraySize = 1;
        uploadDesc.MipLevels = 1;
        uploadDesc.SampleDesc.Count = 1;
        uploadDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        if (FAILED(m_device->CreateCommittedResource(
                &heap,
                D3D12_HEAP_FLAG_NONE,
                &uploadDesc,
                D3D12_RESOURCE_STATE_GENERIC_READ,
                nullptr,
                IID_PPV_ARGS(&m_textureUpload))))
        {
            return false;
        }

        uint8_t* mapped = nullptr;
        D3D12_RANGE range = {0, 0};
        if (FAILED(m_textureUpload->Map(0, &range, reinterpret_cast<void**>(&mapped))))
        {
            return false;
        }

        const uint8_t* src = reinterpret_cast<const uint8_t*>(pixels);
        const uint32_t srcRowPitch = width * sizeof(uint32_t);
        for (uint32_t row = 0; row < height; ++row)
        {
            std::memcpy(mapped + footprint.Offset + row * footprint.Footprint.RowPitch,
                        src + row * srcRowPitch,
                        srcRowPitch);
        }
        m_textureUpload->Unmap(0, nullptr);

        if (FAILED(m_allocators[m_frameIndex]->Reset())) return false;
        if (FAILED(m_commandList->Reset(m_allocators[m_frameIndex].Get(), nullptr))) return false;

        D3D12_TEXTURE_COPY_LOCATION dst = {};
        dst.pResource = texture;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = 0;

        D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
        srcLoc.pResource = m_textureUpload.Get();
        srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        srcLoc.PlacedFootprint = footprint;

        m_commandList->CopyTextureRegion(&dst, 0, 0, 0, &srcLoc, nullptr);
        Transition(m_commandList.Get(), texture, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        if (FAILED(m_commandList->Close())) return false;

        ID3D12CommandList* lists[] = {m_commandList.Get()};
        m_commandQueue->ExecuteCommandLists(1, lists);
        return WaitForGpu();
    }

    bool DemoApp::CreateTexture()
    {
        D3D12_HEAP_PROPERTIES heap = {};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = kTextureWidth;
        desc.Height = kTextureHeight;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

        if (FAILED(m_device->CreateCommittedResource(
                &heap,
                D3D12_HEAP_FLAG_NONE,
                &desc,
                D3D12_RESOURCE_STATE_COPY_DEST,
                nullptr,
                IID_PPV_ARGS(&m_texture))))
        {
            return false;
        }

        return UploadTexture2D(m_texture.Get(), kTexturePixels, kTextureWidth, kTextureHeight, DXGI_FORMAT_R8G8B8A8_UNORM);
    }

    bool DemoApp::CreateRasterizer()
    {
        RasterizerConfig config = {};
        config.viewportWidth = m_width;
        config.viewportHeight = m_height;
        config.maxVertices = static_cast<uint32_t>(std::size(kVertices));
        config.maxTriangles = kMaxTriangleCapacity;
        config.maxVisibleTriangles = config.maxTriangles * 2u;
        config.maxTrianglesPerTile = 64;

        if (!m_rasterizer.Initialize(m_device.Get(), config))
        {
            return false;
        }

        return m_rasterizer.WriteDescriptors(
            m_device.Get(),
            m_srvUavHeap->GetCPUDescriptorHandleForHeapStart(),
            m_srvUavDescriptorSize,
            m_vertexBuffer.Get(),
            m_indexBuffer.Get(),
            m_texture.Get());
    }

    bool DemoApp::InitializeProfiling()
    {
        D3D12_QUERY_HEAP_DESC queryDesc = {};
        queryDesc.Count = 2;
        queryDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        if (FAILED(m_device->CreateQueryHeap(&queryDesc, IID_PPV_ARGS(&m_timestampQueryHeap))))
        {
            return false;
        }

        D3D12_HEAP_PROPERTIES heap = {};
        heap.Type = D3D12_HEAP_TYPE_READBACK;

        D3D12_RESOURCE_DESC bufferDesc = {};
        bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufferDesc.Width = sizeof(uint64_t) * 2;
        bufferDesc.Height = 1;
        bufferDesc.DepthOrArraySize = 1;
        bufferDesc.MipLevels = 1;
        bufferDesc.SampleDesc.Count = 1;
        bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(m_device->CreateCommittedResource(
                &heap,
                D3D12_HEAP_FLAG_NONE,
                &bufferDesc,
                D3D12_RESOURCE_STATE_COPY_DEST,
                nullptr,
                IID_PPV_ARGS(&m_timestampReadback))))
        {
            return false;
        }

        UINT64 copyBytes = 0;
        D3D12_RESOURCE_DESC presentDesc = m_rasterizer.GetPresentTexture()->GetDesc();
        m_device->GetCopyableFootprints(&presentDesc, 0, 1, 0, nullptr, nullptr, nullptr, &copyBytes);
        bufferDesc.Width = copyBytes;
        if (FAILED(m_device->CreateCommittedResource(
                &heap,
                D3D12_HEAP_FLAG_NONE,
                &bufferDesc,
                D3D12_RESOURCE_STATE_COPY_DEST,
                nullptr,
                IID_PPV_ARGS(&m_presentReadback))))
        {
            return false;
        }

        bufferDesc.Width = uint64_t(ComputeSoftwareRasterizer::kProfilingCounterCount) * sizeof(uint32_t);
        if (FAILED(m_device->CreateCommittedResource(
                &heap,
                D3D12_HEAP_FLAG_NONE,
                &bufferDesc,
                D3D12_RESOURCE_STATE_COPY_DEST,
                nullptr,
                IID_PPV_ARGS(&m_profilingReadback))))
        {
            return false;
        }

        const uint32_t tileCount = ((m_width + 15u) / 16u) * ((m_height + 15u) / 16u);
        bufferDesc.Width = uint64_t(tileCount) * sizeof(uint32_t);
        if (FAILED(m_device->CreateCommittedResource(
                &heap,
                D3D12_HEAP_FLAG_NONE,
                &bufferDesc,
                D3D12_RESOURCE_STATE_COPY_DEST,
                nullptr,
                IID_PPV_ARGS(&m_acousticOcclusionReadback))))
        {
            return false;
        }

        if (FAILED(m_device->CreateCommittedResource(
                &heap,
                D3D12_HEAP_FLAG_NONE,
                &bufferDesc,
                D3D12_RESOURCE_STATE_COPY_DEST,
                nullptr,
                IID_PPV_ARGS(&m_acousticDiffractionReadback))))
        {
            return false;
        }

        return SUCCEEDED(m_commandQueue->GetTimestampFrequency(&m_timestampFrequency));
    }

    void DemoApp::Transition(ID3D12GraphicsCommandList* commandList,
                             ID3D12Resource* resource,
                             D3D12_RESOURCE_STATES before,
                             D3D12_RESOURCE_STATES after)
    {
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = resource;
        barrier.Transition.StateBefore = before;
        barrier.Transition.StateAfter = after;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        commandList->ResourceBarrier(1, &barrier);
    }

    uint32_t DemoApp::ComputeCRC32(const uint8_t* data, size_t size) const
    {
        uint32_t crc = 0xffffffffu;
        for (size_t i = 0; i < size; ++i)
        {
            crc ^= data[i];
            for (uint32_t bit = 0; bit < 8; ++bit)
            {
                uint32_t mask = -(crc & 1u);
                crc = (crc >> 1) ^ (0xedb88320u & mask);
            }
        }
        return ~crc;
    }

    bool DemoApp::CapturePresentTexture(std::vector<uint8_t>& outBytes)
    {
        D3D12_RESOURCE_DESC presentDesc = m_rasterizer.GetPresentTexture()->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
        UINT rowCount = 0;
        UINT64 rowBytes = 0;
        UINT64 totalBytes = 0;
        m_device->GetCopyableFootprints(&presentDesc, 0, 1, 0, &footprint, &rowCount, &rowBytes, &totalBytes);

        if (FAILED(m_allocators[m_frameIndex]->Reset())) return false;
        if (FAILED(m_commandList->Reset(m_allocators[m_frameIndex].Get(), nullptr))) return false;

        Transition(m_commandList.Get(), m_rasterizer.GetPresentTexture(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);

        D3D12_TEXTURE_COPY_LOCATION src = {};
        src.pResource = m_rasterizer.GetPresentTexture();
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;

        D3D12_TEXTURE_COPY_LOCATION dst = {};
        dst.pResource = m_presentReadback.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = footprint;

        m_commandList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        Transition(m_commandList.Get(), m_rasterizer.GetPresentTexture(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        if (FAILED(m_commandList->Close())) return false;
        ID3D12CommandList* lists[] = {m_commandList.Get()};
        m_commandQueue->ExecuteCommandLists(1, lists);
        if (!WaitForGpu()) return false;

        uint8_t* mapped = nullptr;
        D3D12_RANGE range = {0, static_cast<SIZE_T>(totalBytes)};
        if (FAILED(m_presentReadback->Map(0, &range, reinterpret_cast<void**>(&mapped))))
        {
            return false;
        }

        outBytes.resize(size_t(m_width) * size_t(m_height) * 4u);
        for (uint32_t row = 0; row < m_height; ++row)
        {
            std::memcpy(outBytes.data() + size_t(row) * size_t(m_width) * 4u,
                        mapped + footprint.Offset + size_t(row) * footprint.Footprint.RowPitch,
                        size_t(m_width) * 4u);
        }
        m_presentReadback->Unmap(0, nullptr);
        return true;
    }

    void DemoApp::UpdateProfilingAndValidation()
    {
        if (m_timestampReadback && m_timestampFrequency != 0 && m_profilingReadback &&
            m_acousticOcclusionReadback && m_acousticDiffractionReadback && (m_frameCounter % 60u) == 0u)
        {
            uint64_t* timestamps = nullptr;
            D3D12_RANGE range = {0, sizeof(uint64_t) * 2};
            if (SUCCEEDED(m_timestampReadback->Map(0, &range, reinterpret_cast<void**>(&timestamps))))
            {
                if (timestamps[1] > timestamps[0])
                {
                    double gpuMs = double(timestamps[1] - timestamps[0]) * 1000.0 / double(m_timestampFrequency);
                    std::wostringstream stream;
                    stream << L"[woah-man] GPU frame time: " << gpuMs << L" ms\n";
                    OutputDebugStringW(stream.str().c_str());
                }
                m_timestampReadback->Unmap(0, nullptr);
            }

            if (SUCCEEDED(m_allocators[m_frameIndex]->Reset()) &&
                SUCCEEDED(m_commandList->Reset(m_allocators[m_frameIndex].Get(), nullptr)))
            {
                Transition(m_commandList.Get(),
                           m_rasterizer.GetProfilingCountersBuffer(),
                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                           D3D12_RESOURCE_STATE_COPY_SOURCE);
                m_commandList->CopyResource(m_profilingReadback.Get(), m_rasterizer.GetProfilingCountersBuffer());
                Transition(m_commandList.Get(),
                           m_rasterizer.GetProfilingCountersBuffer(),
                           D3D12_RESOURCE_STATE_COPY_SOURCE,
                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                Transition(m_commandList.Get(),
                           m_rasterizer.GetTileAcousticOcclusionBuffer(),
                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                           D3D12_RESOURCE_STATE_COPY_SOURCE);
                Transition(m_commandList.Get(),
                           m_rasterizer.GetTileAcousticDiffractionBuffer(),
                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                           D3D12_RESOURCE_STATE_COPY_SOURCE);
                m_commandList->CopyResource(m_acousticOcclusionReadback.Get(), m_rasterizer.GetTileAcousticOcclusionBuffer());
                m_commandList->CopyResource(m_acousticDiffractionReadback.Get(), m_rasterizer.GetTileAcousticDiffractionBuffer());
                Transition(m_commandList.Get(),
                           m_rasterizer.GetTileAcousticOcclusionBuffer(),
                           D3D12_RESOURCE_STATE_COPY_SOURCE,
                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                Transition(m_commandList.Get(),
                           m_rasterizer.GetTileAcousticDiffractionBuffer(),
                           D3D12_RESOURCE_STATE_COPY_SOURCE,
                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

                if (SUCCEEDED(m_commandList->Close()))
                {
                    ID3D12CommandList* lists[] = {m_commandList.Get()};
                    m_commandQueue->ExecuteCommandLists(1, lists);
                    if (WaitForGpu())
                    {
                        uint32_t* counters = nullptr;
                        D3D12_RANGE counterRange = {0, ComputeSoftwareRasterizer::kProfilingCounterCount * sizeof(uint32_t)};
                        if (SUCCEEDED(m_profilingReadback->Map(0, &counterRange, reinterpret_cast<void**>(&counters))))
                        {
                            const uint32_t tiles = counters[0];
                            const uint32_t tileTriangleRefs = counters[1];
                            const uint32_t maxTileTriangles = counters[2];
                            const uint32_t waveTests = counters[3];
                            const uint32_t waveCovered = counters[4];
                            const uint32_t commitAttempts = counters[5];
                            const uint32_t commitSuccesses = counters[6];
                            const uint32_t tileOverflows = counters[7];
                            const uint32_t tinyTriangles = counters[8];
                            const uint32_t portalPixels = counters[9];
                            const uint32_t topologyRemaps = counters[10];
                            const uint32_t coveredSamples = counters[11];
                            const uint32_t portalTriangles = counters[12];
                            const uint32_t hizSkips = counters[13];
                            const uint32_t microSplats = counters[14];
                            const uint32_t proceduralTriangles = counters[15];
                            const uint32_t stochasticSamples = counters[16];
                            const uint32_t audioEdgeHits = counters[17];
                            const uint32_t materialWrites = counters[18];
                            const uint32_t taaHistorySamples = counters[19];
                            const uint32_t taaRejections = counters[20];
                            const uint32_t taaAccepts = counters[21];

                            const double avgTrianglesPerTile = (tiles > 0u) ? (double(tileTriangleRefs) / double(tiles)) : 0.0;
                            const double waveCoverage = (waveTests > 0u) ? (100.0 * double(waveCovered) / double(waveTests)) : 0.0;
                            const double contention = (commitAttempts > 0u)
                                ? (100.0 * double(commitAttempts - commitSuccesses) / double(commitAttempts))
                                : 0.0;

                            std::wostringstream stream;
                            stream << L"[woah-man] Raster profile:"
                                   << L" avgTileRefs=" << avgTrianglesPerTile
                                   << L" maxTileRefs=" << maxTileTriangles
                                   << L" waveCoverage=" << waveCoverage << L"%"
                                   << L" atomicContentionProxy=" << contention << L"%"
                                   << L" overflows=" << tileOverflows
                                   << L" hiZSkips=" << hizSkips
                                   << L" tinyHits=" << tinyTriangles
                                   << L" microSplats=" << microSplats
                                   << L" portalTris=" << portalTriangles
                                   << L" portalPixels=" << portalPixels
                                   << L" topologyRemaps=" << topologyRemaps
                                   << L" coveredSamples=" << coveredSamples
                                   << L" proceduralTris=" << proceduralTriangles
                                   << L" stochasticSamples=" << stochasticSamples
                                   << L" audioEdges=" << audioEdgeHits
                                   << L" materialWrites=" << materialWrites
                                   << L" taaHistory=" << taaHistorySamples
                                   << L" taaRejects=" << taaRejections
                                   << L" taaAccepts=" << taaAccepts
                                   << L"\n";
                            OutputDebugStringW(stream.str().c_str());
                            m_profilingReadback->Unmap(0, nullptr);
                        }

                        const uint32_t tileCount = ((m_width + 15u) / 16u) * ((m_height + 15u) / 16u);
                        uint32_t* acousticOcclusion = nullptr;
                        uint32_t* acousticDiffraction = nullptr;
                        D3D12_RANGE acousticRange = {0, SIZE_T(tileCount) * sizeof(uint32_t)};
                        if (SUCCEEDED(m_acousticOcclusionReadback->Map(0, &acousticRange, reinterpret_cast<void**>(&acousticOcclusion))) &&
                            SUCCEEDED(m_acousticDiffractionReadback->Map(0, &acousticRange, reinterpret_cast<void**>(&acousticDiffraction))))
                        {
                            uint64_t sumOcclusion = 0;
                            uint64_t sumDiffraction = 0;
                            uint32_t maxOcclusion = 0;
                            uint32_t maxDiffraction = 0;
                            for (uint32_t tileIndex = 0; tileIndex < tileCount; ++tileIndex)
                            {
                                sumOcclusion += acousticOcclusion[tileIndex];
                                sumDiffraction += acousticDiffraction[tileIndex];
                                maxOcclusion = (std::max)(maxOcclusion, acousticOcclusion[tileIndex]);
                                maxDiffraction = (std::max)(maxDiffraction, acousticDiffraction[tileIndex]);
                            }

                            std::wostringstream stream;
                            stream << L"[woah-man] Acoustic field:"
                                   << L" avgOcclusion=" << (tileCount ? double(sumOcclusion) / double(tileCount) : 0.0)
                                   << L" maxOcclusion=" << maxOcclusion
                                   << L" avgDiffraction=" << (tileCount ? double(sumDiffraction) / double(tileCount) : 0.0)
                                   << L" maxDiffraction=" << maxDiffraction
                                   << L"\n";
                            OutputDebugStringW(stream.str().c_str());

                            m_acousticDiffractionReadback->Unmap(0, nullptr);
                            m_acousticOcclusionReadback->Unmap(0, nullptr);
                        }
                    }
                }
            }
        }

        if (!m_saveReferenceRequested && !m_validateRequested)
        {
            return;
        }

        std::vector<uint8_t> bytes;
        if (!CapturePresentTexture(bytes))
        {
            return;
        }

        uint32_t crc = ComputeCRC32(bytes.data(), bytes.size());
        if (m_saveReferenceRequested)
        {
            std::ofstream file(m_referenceCRCPath);
            if (file.is_open())
            {
                file << std::hex << crc;
                OutputDebugStringW(L"[woah-man] Saved reference frame CRC.\n");
            }
            m_saveReferenceRequested = false;
        }

        if (m_validateRequested)
        {
            std::ifstream file(m_referenceCRCPath);
            uint32_t referenceCRC = 0;
            if (file.is_open())
            {
                file >> std::hex >> referenceCRC;
                std::wostringstream stream;
                stream << L"[woah-man] Validation "
                       << ((referenceCRC == crc) ? L"passed" : L"FAILED")
                       << L". expected=0x" << std::hex << referenceCRC
                       << L" actual=0x" << crc << L"\n";
                OutputDebugStringW(stream.str().c_str());
            }
            m_validateRequested = false;
        }
    }

    bool DemoApp::RecordAndSubmitFrame()
    {
        if (!m_rasterizer.ReloadShadersIfChanged(m_device.Get()))
        {
            return false;
        }

        bool f1Down = (GetAsyncKeyState(VK_F1) & 0x8000) != 0;
        bool f2Down = (GetAsyncKeyState(VK_F2) & 0x8000) != 0;
        bool f3Down = (GetAsyncKeyState(VK_F3) & 0x8000) != 0;
        bool f4Down = (GetAsyncKeyState(VK_F4) & 0x8000) != 0;
        bool f5Down = (GetAsyncKeyState(VK_F5) & 0x8000) != 0;
        bool f6Down = (GetAsyncKeyState(VK_F6) & 0x8000) != 0;
        bool f7Down = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
        bool f8Down = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
        bool f9Down = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
        bool f10Down = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
        bool f11Down = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
        bool f12Down = (GetAsyncKeyState(VK_F12) & 0x8000) != 0;
        if (f1Down && !m_prevF1Down)
        {
            m_showHeatmap = !m_showHeatmap;
        }
        if (f2Down && !m_prevF2Down)
        {
            m_usePacked32Path = !m_usePacked32Path;
        }
        if (f3Down && !m_prevF3Down)
        {
            m_msaaSampleCount = (m_msaaSampleCount == 1u) ? 2u : (m_msaaSampleCount == 2u) ? 4u : 1u;
        }
        if (f4Down && !m_prevF4Down)
        {
            m_projectionMode = (m_projectionMode + 1u) % 4u;
        }
        if (f5Down && !m_prevF5Down)
        {
            m_saveReferenceRequested = true;
        }
        if (f6Down && !m_prevF6Down)
        {
            m_validateRequested = true;
        }
        if (f7Down && !m_prevF7Down)
        {
            m_enablePortalTraversal = !m_enablePortalTraversal;
        }
        if (f8Down && !m_prevF8Down)
        {
            m_enableTopologyRemap = !m_enableTopologyRemap;
        }
        if (f9Down && !m_prevF9Down)
        {
            m_enableProceduralClusters = !m_enableProceduralClusters;
        }
        if (f10Down && !m_prevF10Down)
        {
            m_enableStochasticSampling = !m_enableStochasticSampling;
        }
        if (f11Down && !m_prevF11Down)
        {
            m_audioDebugMode = (m_audioDebugMode + 1u) % 3u;
        }
        if (f12Down && !m_prevF12Down)
        {
            m_enableTemporalReprojection = !m_enableTemporalReprojection;
        }
        m_prevF1Down = f1Down;
        m_prevF2Down = f2Down;
        m_prevF3Down = f3Down;
        m_prevF4Down = f4Down;
        m_prevF5Down = f5Down;
        m_prevF6Down = f6Down;
        m_prevF7Down = f7Down;
        m_prevF8Down = f8Down;
        m_prevF9Down = f9Down;
        m_prevF10Down = f10Down;
        m_prevF11Down = f11Down;
        m_prevF12Down = f12Down;

        if (FAILED(m_allocators[m_frameIndex]->Reset())) return false;
        if (FAILED(m_commandList->Reset(m_allocators[m_frameIndex].Get(), nullptr))) return false;

        ID3D12DescriptorHeap* heaps[] = {m_srvUavHeap.Get()};
        m_commandList->SetDescriptorHeaps(1, heaps);
        if (m_timestampQueryHeap)
        {
            m_commandList->EndQuery(m_timestampQueryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
        }

        const uint32_t triangleCount = m_enableProceduralClusters ? kProceduralTriangleCapacity : kStaticTriangleCount;
        RasterizerConstants constants = {};
        const float identity[16] = {
            1.0f, 0.0f, 0.0f, 0.0f,
            0.0f, 1.0f, 0.0f, 0.0f,
            0.0f, 0.0f, 1.0f, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f,
        };
        std::memcpy(constants.mvp, identity, sizeof(identity));
        constants.viewportWidth = m_width;
        constants.viewportHeight = m_height;
        constants.tileCountX = (m_width + 15) / 16;
        constants.tileCountY = (m_height + 15) / 16;
        constants.vertexCount = static_cast<uint32_t>(std::size(kVertices));
        constants.triangleCount = triangleCount;
        constants.maxVisibleTriangles = constants.triangleCount * 2u;
        constants.maxTrianglesPerTile = 64;
        constants.invViewportWidth = 1.0f / float(m_width);
        constants.invViewportHeight = 1.0f / float(m_height);
        constants.guardBandPixelsX = 4.0f;
        constants.guardBandPixelsY = 4.0f;
        constants.lightDirection[0] = 0.0f;
        constants.lightDirection[1] = 0.0f;
        constants.lightDirection[2] = -1.0f;
        constants.alphaBlendEnable = 0.0f;
        constants.usePacked32Path = m_usePacked32Path ? 1.0f : 0.0f;
        constants.debugHeatmapMode = m_showHeatmap ? 1.0f : 0.0f;
        constants.heatmapScale = 1.0f / 32.0f;
        constants.projectionMode = m_projectionMode;
        constants.msaaSampleCount = m_msaaSampleCount;
        constants.projectionStrength = m_projectionStrength;
        constants.mipBias = m_mipBias;
        constants.pad0 = 0.0f;
        constants.microTriangleAreaThreshold = m_microTriangleAreaThreshold;
        constants.portalTriangleIndex = 1u;
        constants.topologyTriangleIndex = 0u;
        constants.enablePortalTraversal = m_enablePortalTraversal ? 1u : 0u;
        constants.enableTopologyRemap = m_enableTopologyRemap ? 1u : 0u;
        constants.portalViewShift[0] = 0.18f;
        constants.portalViewShift[1] = -0.11f;
        constants.topologyFoldStrength = 2.5f;
        constants.frameIndex = static_cast<uint32_t>(m_frameCounter);
        constants.enableProceduralClusters = m_enableProceduralClusters ? 1u : 0u;
        constants.enableStochasticSampling = m_enableStochasticSampling ? 1u : 0u;
        constants.audioDebugMode = m_audioDebugMode;
        constants.stochasticJitterStrength = 0.38f;
        constants.acousticOcclusionScale = 72.0f;
        constants.acousticDiffractionScale = 3.0f;
        constants.proceduralClusterSeed = 17u;
        constants.enableTemporalReprojection = m_enableTemporalReprojection ? 1u : 0u;
        constants.temporalBlendWeight = 0.82f;
        constants.temporalDepthThreshold = 850.0f;
        constants.temporalMotionScale = 2.4f;

        m_rasterizer.Dispatch(
            m_commandList.Get(),
            m_srvTable,
            m_uavTable,
            static_cast<uint32_t>(std::size(kVertices)),
            triangleCount,
            constants);

        Transition(m_commandList.Get(), m_rasterizer.GetPresentTexture(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Transition(m_commandList.Get(), m_backbuffers[m_frameIndex].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
        m_commandList->CopyResource(m_backbuffers[m_frameIndex].Get(), m_rasterizer.GetPresentTexture());
        Transition(m_commandList.Get(), m_backbuffers[m_frameIndex].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
        Transition(m_commandList.Get(), m_rasterizer.GetPresentTexture(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if (m_timestampQueryHeap)
        {
            m_commandList->EndQuery(m_timestampQueryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
            m_commandList->ResolveQueryData(m_timestampQueryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, m_timestampReadback.Get(), 0);
        }

        if (FAILED(m_commandList->Close())) return false;

        ID3D12CommandList* lists[] = {m_commandList.Get()};
        m_commandQueue->ExecuteCommandLists(1, lists);
        if (FAILED(m_swapchain->Present(1, 0))) return false;
        ++m_frameCounter;
        MoveToNextFrame();
        return true;
    }

    bool DemoApp::WaitForGpu()
    {
        const uint64_t value = ++m_fenceValue;
        if (FAILED(m_commandQueue->Signal(m_fence.Get(), value)))
        {
            return false;
        }

        if (m_fence->GetCompletedValue() < value)
        {
            if (FAILED(m_fence->SetEventOnCompletion(value, m_fenceEvent)))
            {
                return false;
            }
            WaitForSingleObject(m_fenceEvent, INFINITE);
        }

        return true;
    }

    void DemoApp::MoveToNextFrame()
    {
        WaitForGpu();
        UpdateProfilingAndValidation();
        m_frameIndex = m_swapchain->GetCurrentBackBufferIndex();
    }

    int DemoApp::Run()
    {
        MSG msg = {};
        while (msg.message != WM_QUIT)
        {
            if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
            {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }
            else if (!RecordAndSubmitFrame())
            {
                return 1;
            }
        }

        WaitForGpu();
        return static_cast<int>(msg.wParam);
    }
}
