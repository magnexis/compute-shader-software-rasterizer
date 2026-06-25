#pragma once

#include <cstdint>
#include <filesystem>
#include <wrl/client.h>
#include <d3d12.h>
#include <dxgi1_6.h>

namespace woah
{
    /**
     * Describes the capacity of the internal compute-rasterizer resources.
     * All counts are upper bounds used to size GPU buffers.
     */
    struct RasterizerConfig
    {
        uint32_t viewportWidth = 0;
        uint32_t viewportHeight = 0;
        uint32_t maxVertices = 0;
        uint32_t maxTriangles = 0;
        uint32_t maxVisibleTriangles = 0;
        uint32_t maxTrianglesPerTile = 0;
    };

    /**
     * Mirrored constant buffer shared between C++ and HLSL.
     * Keep field order and packing exactly synchronized with RasterizerConstants
     * in ComputeSoftwareRasterizer.hlsl.
     */
    struct RasterizerConstants
    {
        float mvp[16];
        uint32_t viewportWidth;
        uint32_t viewportHeight;
        uint32_t tileCountX;
        uint32_t tileCountY;
        uint32_t vertexCount;
        uint32_t triangleCount;
        uint32_t maxVisibleTriangles;
        uint32_t maxTrianglesPerTile;
        float invViewportWidth;
        float invViewportHeight;
        float guardBandPixelsX;
        float guardBandPixelsY;
        float lightDirection[3];
        float alphaBlendEnable;
        float usePacked32Path;
        float debugHeatmapMode;
        float heatmapScale;
        uint32_t msaaSampleCount;
        uint32_t projectionMode;
        float projectionStrength;
        float mipBias;
        float pad0;
        float microTriangleAreaThreshold;
        uint32_t portalTriangleIndex;
        uint32_t topologyTriangleIndex;
        uint32_t enablePortalTraversal;
        uint32_t enableTopologyRemap;
        float portalViewShift[2];
        float topologyFoldStrength;
        uint32_t frameIndex;
        uint32_t enableProceduralClusters;
        uint32_t enableStochasticSampling;
        uint32_t audioDebugMode;
        float stochasticJitterStrength;
        float acousticOcclusionScale;
        float acousticDiffractionScale;
        uint32_t proceduralClusterSeed;
        uint32_t enableTemporalReprojection;
        float temporalBlendWeight;
        float temporalDepthThreshold;
        float temporalMotionScale;
    };

    static_assert((sizeof(RasterizerConstants) % 16u) == 0u, "RasterizerConstants must stay 16-byte aligned.");

    /**
     * Minimal D3D12 wrapper that owns the internal UAV resources and dispatches
     * the compute-based rasterization pipeline.
     */
    class ComputeSoftwareRasterizer
    {
    public:
        static constexpr uint32_t kSrvDescriptorCount = 3;
        static constexpr uint32_t kUavDescriptorCount = 19;
        static constexpr uint32_t kDescriptorCount = kSrvDescriptorCount + kUavDescriptorCount;
        static constexpr uint32_t kProfilingCounterCount = 22;

        bool Initialize(ID3D12Device* device, const RasterizerConfig& config);
        void Resize(ID3D12Device* device, uint32_t width, uint32_t height);
        bool ReloadShadersIfChanged(ID3D12Device* device);
        bool WriteDescriptors(ID3D12Device* device,
                              D3D12_CPU_DESCRIPTOR_HANDLE cpuStart,
                              uint32_t descriptorSize,
                              ID3D12Resource* vertexBuffer,
                              ID3D12Resource* indexBuffer,
                              ID3D12Resource* texture);
        void Dispatch(ID3D12GraphicsCommandList* commandList,
                      D3D12_GPU_DESCRIPTOR_HANDLE srvTable,
                      D3D12_GPU_DESCRIPTOR_HANDLE uavTable,
                      uint32_t vertexCount,
                      uint32_t triangleCount,
                      const RasterizerConstants& constants);

        ID3D12Resource* GetPackedFramebuffer() const { return m_packedDepthColor.Get(); }
        ID3D12Resource* GetPresentTexture() const { return m_presentTexture.Get(); }
        ID3D12Resource* GetTileOverflowBuffer() const { return m_tileOverflow.Get(); }
        ID3D12Resource* GetProfilingCountersBuffer() const { return m_profilingCounters.Get(); }
        ID3D12Resource* GetTileAcousticOcclusionBuffer() const { return m_tileAcousticOcclusion.Get(); }
        ID3D12Resource* GetTileAcousticDiffractionBuffer() const { return m_tileAcousticDiffraction.Get(); }

    private:
        bool CreateBuffers(ID3D12Device* device);
        bool CreatePipeline(ID3D12Device* device);
        bool CreateRootSignature(ID3D12Device* device);
        void RewriteCachedDescriptors(ID3D12Device* device);

        RasterizerConfig m_config = {};
        uint32_t m_tileCountX = 0;
        uint32_t m_tileCountY = 0;
        std::filesystem::file_time_type m_shaderWriteTime = {};
        bool m_hasShaderWriteTime = false;

        Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSignature;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> m_clearPSO;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> m_transformPSO;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> m_setupPSO;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> m_binPSO;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> m_finePSO;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> m_presentPSO;

        Microsoft::WRL::ComPtr<ID3D12Resource> m_constantsUpload;
        Microsoft::WRL::ComPtr<ID3D12Resource> m_projectedVertices;
        Microsoft::WRL::ComPtr<ID3D12Resource> m_visibleTriangles;
        Microsoft::WRL::ComPtr<ID3D12Resource> m_visibleCounter;
        Microsoft::WRL::ComPtr<ID3D12Resource> m_tileCounters;
        Microsoft::WRL::ComPtr<ID3D12Resource> m_tileOverflow;
        Microsoft::WRL::ComPtr<ID3D12Resource> m_tileTriangleIndices;
        Microsoft::WRL::ComPtr<ID3D12Resource> m_tileDepthHint;
        Microsoft::WRL::ComPtr<ID3D12Resource> m_packedDepthColor;
        Microsoft::WRL::ComPtr<ID3D12Resource> m_packedDepthColor32;
        Microsoft::WRL::ComPtr<ID3D12Resource> m_tileDebugCounts;
        Microsoft::WRL::ComPtr<ID3D12Resource> m_portalMask;
        Microsoft::WRL::ComPtr<ID3D12Resource> m_profilingCounters;
        Microsoft::WRL::ComPtr<ID3D12Resource> m_materialDensity;
        Microsoft::WRL::ComPtr<ID3D12Resource> m_tileAcousticOcclusion;
        Microsoft::WRL::ComPtr<ID3D12Resource> m_tileAcousticDiffraction;
        Microsoft::WRL::ComPtr<ID3D12Resource> m_previousPackedTemporal;
        Microsoft::WRL::ComPtr<ID3D12Resource> m_historyTexture;
        Microsoft::WRL::ComPtr<ID3D12Resource> m_previousHistoryTexture;
        Microsoft::WRL::ComPtr<ID3D12Resource> m_presentTexture;

        // Cached descriptor heap parameters for re-binding after Resize()
        bool m_hasCachedDescriptors = false;
        D3D12_CPU_DESCRIPTOR_HANDLE m_cachedCpuStart = {};
        uint32_t m_cachedDescriptorSize = 0;
        ID3D12Resource* m_cachedVertexBuffer = nullptr;
        ID3D12Resource* m_cachedIndexBuffer = nullptr;
        ID3D12Resource* m_cachedTexture = nullptr;
    };
}
