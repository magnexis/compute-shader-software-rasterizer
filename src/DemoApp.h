#pragma once

#include "SoftwareRasterizerD3D12.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <vector>
#include <windows.h>
#include <wrl/client.h>
#include <d3d12.h>
#include <dxgi1_6.h>

namespace woah
{
    class DemoApp
    {
    public:
        bool Initialize(HINSTANCE instance, int showCommand);
        int Run();

    private:
        static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
        bool CreateWindowClass(HINSTANCE instance, int showCommand);
        bool InitializeD3D12();
        bool CreateSwapchain();
        bool CreateCommandObjects();
        bool CreateDescriptorHeaps();
        bool CreateGeometry();
        bool CreateTexture();
        bool CreateRasterizer();
        bool RecordAndSubmitFrame();
        bool WaitForGpu();
        void MoveToNextFrame();
        bool InitializeProfiling();
        bool CapturePresentTexture(std::vector<uint8_t>& outBytes);
        uint32_t ComputeCRC32(const uint8_t* data, size_t size) const;
        void UpdateProfilingAndValidation();

        Microsoft::WRL::ComPtr<ID3D12Resource> CreateUploadBuffer(const void* data, uint64_t byteSize);
        bool UploadTexture2D(ID3D12Resource* texture, const void* pixels, uint32_t width, uint32_t height, DXGI_FORMAT format);
        void Transition(ID3D12GraphicsCommandList* commandList,
                        ID3D12Resource* resource,
                        D3D12_RESOURCE_STATES before,
                        D3D12_RESOURCE_STATES after);

        HWND m_window = nullptr;
        uint32_t m_width = 1280;
        uint32_t m_height = 720;

        static constexpr uint32_t kFrameCount = 2;

        Microsoft::WRL::ComPtr<IDXGIFactory6> m_factory;
        Microsoft::WRL::ComPtr<ID3D12Device> m_device;
        Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_commandQueue;
        Microsoft::WRL::ComPtr<IDXGISwapChain3> m_swapchain;
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_srvUavHeap;
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_commandList;
        Microsoft::WRL::ComPtr<ID3D12Fence> m_fence;
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> m_allocators[kFrameCount];
        Microsoft::WRL::ComPtr<ID3D12Resource> m_backbuffers[kFrameCount];
        Microsoft::WRL::ComPtr<ID3D12Resource> m_vertexBuffer;
        Microsoft::WRL::ComPtr<ID3D12Resource> m_indexBuffer;
        Microsoft::WRL::ComPtr<ID3D12Resource> m_texture;
        Microsoft::WRL::ComPtr<ID3D12Resource> m_textureUpload;

        D3D12_GPU_DESCRIPTOR_HANDLE m_srvTable = {};
        D3D12_GPU_DESCRIPTOR_HANDLE m_uavTable = {};
        uint32_t m_rtvDescriptorSize = 0;
        uint32_t m_srvUavDescriptorSize = 0;
        uint32_t m_frameIndex = 0;
        uint64_t m_fenceValue = 0;
        HANDLE m_fenceEvent = nullptr;
        bool m_usePacked32Path = false;
        bool m_showHeatmap = false;
        uint32_t m_msaaSampleCount = 4;
        float m_mipBias = 0.0f;
        uint32_t m_projectionMode = 0;
        float m_projectionStrength = 1.5f;
        bool m_enablePortalTraversal = false;
        bool m_enableTopologyRemap = false;
        bool m_enableProceduralClusters = false;
        bool m_enableStochasticSampling = false;
        bool m_enableTemporalReprojection = true;
        float m_microTriangleAreaThreshold = 1.5f;
        uint32_t m_audioDebugMode = 0;
        bool m_prevF1Down = false;
        bool m_prevF2Down = false;
        bool m_prevF3Down = false;
        bool m_prevF4Down = false;
        bool m_prevF5Down = false;
        bool m_prevF6Down = false;
        bool m_prevF7Down = false;
        bool m_prevF8Down = false;
        bool m_prevF9Down = false;
        bool m_prevF10Down = false;
        bool m_prevF11Down = false;
        bool m_prevF12Down = false;
        bool m_saveReferenceRequested = false;
        bool m_validateRequested = false;
        uint64_t m_frameCounter = 0;
        uint64_t m_timestampFrequency = 0;
        std::filesystem::path m_referenceCRCPath = L"reference_frame_crc.txt";

        Microsoft::WRL::ComPtr<ID3D12QueryHeap> m_timestampQueryHeap;
        Microsoft::WRL::ComPtr<ID3D12Resource> m_timestampReadback;
        Microsoft::WRL::ComPtr<ID3D12Resource> m_presentReadback;
        Microsoft::WRL::ComPtr<ID3D12Resource> m_profilingReadback;
        Microsoft::WRL::ComPtr<ID3D12Resource> m_acousticOcclusionReadback;
        Microsoft::WRL::ComPtr<ID3D12Resource> m_acousticDiffractionReadback;

        ComputeSoftwareRasterizer m_rasterizer;
    };
}
