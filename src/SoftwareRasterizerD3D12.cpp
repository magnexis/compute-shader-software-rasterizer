#include "SoftwareRasterizerD3D12.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
#include <dxcapi.h>

using Microsoft::WRL::ComPtr;

namespace
{
    constexpr uint32_t kTileSize = 16;
    constexpr uint32_t kMaxTileTriangles = 256;
    constexpr uint32_t kClearGroupSize = 256;
    constexpr uint32_t kVertexGroupSize = 128;
    constexpr uint32_t kTriangleGroupSize = 128;
    constexpr uint32_t kBinGroupSize = 64;
    constexpr wchar_t kShaderPath[] = L"shaders/ComputeSoftwareRasterizer.hlsl";

    ComPtr<IDxcBlob> CompileShader(const wchar_t* path, const wchar_t* entryPoint)
    {
        ComPtr<IDxcUtils> utils;
        ComPtr<IDxcCompiler3> compiler;
        if (FAILED(DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&utils)))) return {};
        if (FAILED(DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler)))) return {};

        uint32_t codePage = DXC_CP_UTF8;
        ComPtr<IDxcBlobEncoding> source;
        if (FAILED(utils->LoadFile(path, &codePage, &source)))
        {
            return {};
        }

        DxcBuffer sourceBuffer = {};
        sourceBuffer.Ptr = source->GetBufferPointer();
        sourceBuffer.Size = source->GetBufferSize();
        sourceBuffer.Encoding = DXC_CP_UTF8;

        std::vector<LPCWSTR> args = {
            path,
            L"-E", entryPoint,
            L"-T", L"cs_6_6",
            L"-HV", L"2021"
        };

#if defined(_DEBUG)
        args.push_back(L"-Zi");
        args.push_back(L"-Qembed_debug");
#else
        args.push_back(L"-O3");
#endif

        ComPtr<IDxcResult> result;
        if (FAILED(compiler->Compile(&sourceBuffer, args.data(), static_cast<UINT32>(args.size()), nullptr, IID_PPV_ARGS(&result))))
        {
            return {};
        }

        HRESULT status = S_OK;
        if (FAILED(result->GetStatus(&status)) || FAILED(status))
        {
            return {};
        }

        ComPtr<IDxcBlob> shader;
        if (FAILED(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&shader), nullptr)))
        {
            return {};
        }
        return shader;
    }

    D3D12_RESOURCE_DESC MakeStructuredBufferDesc(uint64_t byteSize)
    {
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = byteSize;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        return desc;
    }

    D3D12_RESOURCE_DESC MakeTexture2DDesc(uint32_t width, uint32_t height, DXGI_FORMAT format)
    {
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        return desc;
    }

    bool CreateDefaultBuffer(ID3D12Device* device, uint64_t bytes, ID3D12Resource** outResource)
    {
        D3D12_HEAP_PROPERTIES heap = {};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC desc = MakeStructuredBufferDesc(bytes);
        return SUCCEEDED(device->CreateCommittedResource(
            &heap,
            D3D12_HEAP_FLAG_NONE,
            &desc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            nullptr,
            IID_PPV_ARGS(outResource)));
    }

    bool CreateDefaultTexture(ID3D12Device* device, uint32_t width, uint32_t height, DXGI_FORMAT format, ID3D12Resource** outResource)
    {
        D3D12_HEAP_PROPERTIES heap = {};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC desc = MakeTexture2DDesc(width, height, format);
        return SUCCEEDED(device->CreateCommittedResource(
            &heap,
            D3D12_HEAP_FLAG_NONE,
            &desc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            nullptr,
            IID_PPV_ARGS(outResource)));
    }

    bool CreateUploadBuffer(ID3D12Device* device, uint64_t bytes, ID3D12Resource** outResource)
    {
        D3D12_HEAP_PROPERTIES heap = {};
        heap.Type = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = bytes;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        return SUCCEEDED(device->CreateCommittedResource(
            &heap,
            D3D12_HEAP_FLAG_NONE,
            &desc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(outResource)));
    }

    void InsertUAVBarrier(ID3D12GraphicsCommandList* commandList, ID3D12Resource* resource)
    {
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barrier.UAV.pResource = resource;
        commandList->ResourceBarrier(1, &barrier);
    }
}

namespace woah
{
    bool ComputeSoftwareRasterizer::Initialize(ID3D12Device* device, const RasterizerConfig& config)
    {
        m_config = config;
        m_config.maxTrianglesPerTile = std::min<uint32_t>(std::max<uint32_t>(m_config.maxTrianglesPerTile, 1u), kMaxTileTriangles);
        m_tileCountX = (config.viewportWidth + kTileSize - 1) / kTileSize;
        m_tileCountY = (config.viewportHeight + kTileSize - 1) / kTileSize;

        if (!CreateBuffers(device))
        {
            return false;
        }

        return CreatePipeline(device);
    }

    void ComputeSoftwareRasterizer::Resize(ID3D12Device* device, uint32_t width, uint32_t height)
    {
        m_config.viewportWidth = width;
        m_config.viewportHeight = height;
        m_tileCountX = (width + kTileSize - 1) / kTileSize;
        m_tileCountY = (height + kTileSize - 1) / kTileSize;
        CreateBuffers(device);
    }

    bool ComputeSoftwareRasterizer::CreateBuffers(ID3D12Device* device)
    {
        const uint64_t vertexProjectedBytes = uint64_t(m_config.maxVertices) * sizeof(float) * 16;
        const uint64_t visibleTrianglesBytes = uint64_t(m_config.maxVisibleTriangles) * 128;
        const uint64_t tileCount = uint64_t(m_tileCountX) * uint64_t(m_tileCountY);
        const uint64_t tileCountersBytes = tileCount * sizeof(uint32_t);
        const uint64_t tileBinsBytes = tileCount * uint64_t(m_config.maxTrianglesPerTile) * sizeof(uint32_t);
        const uint64_t framebufferPixels = uint64_t(m_config.viewportWidth) * uint64_t(m_config.viewportHeight);
        const uint64_t framebufferBytes = framebufferPixels * sizeof(uint64_t);
        const uint64_t profilingBytes = uint64_t(ComputeSoftwareRasterizer::kProfilingCounterCount) * sizeof(uint32_t);
        const uint64_t acousticTileBytes = tileCount * sizeof(uint32_t);

        if (!CreateUploadBuffer(device, sizeof(RasterizerConstants), m_constantsUpload.ReleaseAndGetAddressOf())) return false;
        if (!CreateDefaultBuffer(device, vertexProjectedBytes, m_projectedVertices.ReleaseAndGetAddressOf())) return false;
        if (!CreateDefaultBuffer(device, visibleTrianglesBytes, m_visibleTriangles.ReleaseAndGetAddressOf())) return false;
        if (!CreateDefaultBuffer(device, sizeof(uint32_t), m_visibleCounter.ReleaseAndGetAddressOf())) return false;
        if (!CreateDefaultBuffer(device, tileCountersBytes, m_tileCounters.ReleaseAndGetAddressOf())) return false;
        if (!CreateDefaultBuffer(device, tileCountersBytes, m_tileOverflow.ReleaseAndGetAddressOf())) return false;
        if (!CreateDefaultBuffer(device, tileBinsBytes, m_tileTriangleIndices.ReleaseAndGetAddressOf())) return false;
        if (!CreateDefaultBuffer(device, tileCountersBytes, m_tileDepthHint.ReleaseAndGetAddressOf())) return false;
        if (!CreateDefaultBuffer(device, framebufferBytes, m_packedDepthColor.ReleaseAndGetAddressOf())) return false;
        if (!CreateDefaultBuffer(device, framebufferPixels * sizeof(uint32_t), m_packedDepthColor32.ReleaseAndGetAddressOf())) return false;
        if (!CreateDefaultBuffer(device, tileCountersBytes, m_tileDebugCounts.ReleaseAndGetAddressOf())) return false;
        if (!CreateDefaultBuffer(device, framebufferPixels * sizeof(uint32_t), m_portalMask.ReleaseAndGetAddressOf())) return false;
        if (!CreateDefaultBuffer(device, profilingBytes, m_profilingCounters.ReleaseAndGetAddressOf())) return false;
        if (!CreateDefaultBuffer(device, framebufferPixels * sizeof(uint32_t), m_materialDensity.ReleaseAndGetAddressOf())) return false;
        if (!CreateDefaultBuffer(device, acousticTileBytes, m_tileAcousticOcclusion.ReleaseAndGetAddressOf())) return false;
        if (!CreateDefaultBuffer(device, acousticTileBytes, m_tileAcousticDiffraction.ReleaseAndGetAddressOf())) return false;
        if (!CreateDefaultBuffer(device, framebufferPixels * sizeof(uint32_t), m_previousPackedTemporal.ReleaseAndGetAddressOf())) return false;
        if (!CreateDefaultTexture(device, m_config.viewportWidth, m_config.viewportHeight, DXGI_FORMAT_R8G8B8A8_UNORM, m_historyTexture.ReleaseAndGetAddressOf())) return false;
        if (!CreateDefaultTexture(device, m_config.viewportWidth, m_config.viewportHeight, DXGI_FORMAT_R8G8B8A8_UNORM, m_previousHistoryTexture.ReleaseAndGetAddressOf())) return false;
        if (!CreateDefaultTexture(device, m_config.viewportWidth, m_config.viewportHeight, DXGI_FORMAT_R8G8B8A8_UNORM, m_presentTexture.ReleaseAndGetAddressOf())) return false;
        return true;
    }

    bool ComputeSoftwareRasterizer::ReloadShadersIfChanged(ID3D12Device* device)
    {
        std::error_code error;
        const auto writeTime = std::filesystem::last_write_time(kShaderPath, error);
        if (error)
        {
            return false;
        }

        if (!m_hasShaderWriteTime || writeTime != m_shaderWriteTime)
        {
            return CreatePipeline(device);
        }

        return true;
    }

    bool ComputeSoftwareRasterizer::WriteDescriptors(ID3D12Device* device,
                                                     D3D12_CPU_DESCRIPTOR_HANDLE cpuStart,
                                                     uint32_t descriptorSize,
                                                     ID3D12Resource* vertexBuffer,
                                                     ID3D12Resource* indexBuffer,
                                                     ID3D12Resource* texture)
    {
        auto advance = [&](uint32_t index) -> D3D12_CPU_DESCRIPTOR_HANDLE
        {
            D3D12_CPU_DESCRIPTOR_HANDLE handle = cpuStart;
            handle.ptr += SIZE_T(descriptorSize) * index;
            return handle;
        };

        D3D12_SHADER_RESOURCE_VIEW_DESC vertexSrv = {};
        vertexSrv.Format = DXGI_FORMAT_UNKNOWN;
        vertexSrv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        vertexSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        vertexSrv.Buffer.FirstElement = 0;
        vertexSrv.Buffer.NumElements = m_config.maxVertices;
        vertexSrv.Buffer.StructureByteStride = sizeof(float) * 8;
        device->CreateShaderResourceView(vertexBuffer, &vertexSrv, advance(0));

        D3D12_SHADER_RESOURCE_VIEW_DESC indexSrv = {};
        indexSrv.Format = DXGI_FORMAT_UNKNOWN;
        indexSrv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        indexSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        indexSrv.Buffer.FirstElement = 0;
        indexSrv.Buffer.NumElements = m_config.maxTriangles * 3;
        indexSrv.Buffer.StructureByteStride = sizeof(uint32_t);
        device->CreateShaderResourceView(indexBuffer, &indexSrv, advance(1));

        D3D12_SHADER_RESOURCE_VIEW_DESC textureSrv = {};
        textureSrv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        textureSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        textureSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        textureSrv.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(texture, &textureSrv, advance(2));

        D3D12_UNORDERED_ACCESS_VIEW_DESC projectedUav = {};
        projectedUav.Format = DXGI_FORMAT_UNKNOWN;
        projectedUav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        projectedUav.Buffer.NumElements = m_config.maxVertices;
        projectedUav.Buffer.StructureByteStride = sizeof(float) * 16;
        device->CreateUnorderedAccessView(m_projectedVertices.Get(), nullptr, &projectedUav, advance(3));

        D3D12_UNORDERED_ACCESS_VIEW_DESC visibleUav = {};
        visibleUav.Format = DXGI_FORMAT_UNKNOWN;
        visibleUav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        visibleUav.Buffer.NumElements = m_config.maxVisibleTriangles;
        visibleUav.Buffer.StructureByteStride = 128;
        device->CreateUnorderedAccessView(m_visibleTriangles.Get(), nullptr, &visibleUav, advance(4));

        D3D12_UNORDERED_ACCESS_VIEW_DESC uintBufferUav = {};
        uintBufferUav.Format = DXGI_FORMAT_UNKNOWN;
        uintBufferUav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uintBufferUav.Buffer.StructureByteStride = sizeof(uint32_t);

        uintBufferUav.Buffer.NumElements = 1;
        device->CreateUnorderedAccessView(m_visibleCounter.Get(), nullptr, &uintBufferUav, advance(5));

        uintBufferUav.Buffer.NumElements = m_tileCountX * m_tileCountY;
        device->CreateUnorderedAccessView(m_tileCounters.Get(), nullptr, &uintBufferUav, advance(6));
        device->CreateUnorderedAccessView(m_tileOverflow.Get(), nullptr, &uintBufferUav, advance(7));

        uintBufferUav.Buffer.NumElements = m_tileCountX * m_tileCountY * m_config.maxTrianglesPerTile;
        device->CreateUnorderedAccessView(m_tileTriangleIndices.Get(), nullptr, &uintBufferUav, advance(8));

        uintBufferUav.Buffer.NumElements = m_tileCountX * m_tileCountY;
        device->CreateUnorderedAccessView(m_tileDepthHint.Get(), nullptr, &uintBufferUav, advance(9));

        D3D12_UNORDERED_ACCESS_VIEW_DESC packedUav = {};
        packedUav.Format = DXGI_FORMAT_UNKNOWN;
        packedUav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        packedUav.Buffer.StructureByteStride = sizeof(uint64_t);
        packedUav.Buffer.NumElements = m_config.viewportWidth * m_config.viewportHeight;
        device->CreateUnorderedAccessView(m_packedDepthColor.Get(), nullptr, &packedUav, advance(10));

        D3D12_UNORDERED_ACCESS_VIEW_DESC packed32Uav = {};
        packed32Uav.Format = DXGI_FORMAT_UNKNOWN;
        packed32Uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        packed32Uav.Buffer.StructureByteStride = sizeof(uint32_t);
        packed32Uav.Buffer.NumElements = m_config.viewportWidth * m_config.viewportHeight;
        device->CreateUnorderedAccessView(m_packedDepthColor32.Get(), nullptr, &packed32Uav, advance(11));

        uintBufferUav.Buffer.NumElements = m_tileCountX * m_tileCountY;
        device->CreateUnorderedAccessView(m_tileDebugCounts.Get(), nullptr, &uintBufferUav, advance(12));

        packed32Uav.Buffer.NumElements = m_config.viewportWidth * m_config.viewportHeight;
        device->CreateUnorderedAccessView(m_portalMask.Get(), nullptr, &packed32Uav, advance(13));

        uintBufferUav.Buffer.NumElements = ComputeSoftwareRasterizer::kProfilingCounterCount;
        device->CreateUnorderedAccessView(m_profilingCounters.Get(), nullptr, &uintBufferUav, advance(14));

        packed32Uav.Buffer.NumElements = m_config.viewportWidth * m_config.viewportHeight;
        device->CreateUnorderedAccessView(m_materialDensity.Get(), nullptr, &packed32Uav, advance(15));

        uintBufferUav.Buffer.NumElements = m_tileCountX * m_tileCountY;
        device->CreateUnorderedAccessView(m_tileAcousticOcclusion.Get(), nullptr, &uintBufferUav, advance(16));
        device->CreateUnorderedAccessView(m_tileAcousticDiffraction.Get(), nullptr, &uintBufferUav, advance(17));

        packed32Uav.Buffer.NumElements = m_config.viewportWidth * m_config.viewportHeight;
        device->CreateUnorderedAccessView(m_previousPackedTemporal.Get(), nullptr, &packed32Uav, advance(18));

        D3D12_UNORDERED_ACCESS_VIEW_DESC historyUav = {};
        historyUav.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        historyUav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(m_historyTexture.Get(), nullptr, &historyUav, advance(19));
        device->CreateUnorderedAccessView(m_previousHistoryTexture.Get(), nullptr, &historyUav, advance(20));

        D3D12_UNORDERED_ACCESS_VIEW_DESC presentUav = {};
        presentUav.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        presentUav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(m_presentTexture.Get(), nullptr, &presentUav, advance(21));

        return true;
    }

    bool ComputeSoftwareRasterizer::CreateRootSignature(ID3D12Device* device)
    {
        if (m_rootSignature)
        {
            return true;
        }

        D3D12_DESCRIPTOR_RANGE srvRange = {};
        srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srvRange.NumDescriptors = 3;
        srvRange.BaseShaderRegister = 0;
        srvRange.RegisterSpace = 0;
        srvRange.OffsetInDescriptorsFromTableStart = 0;

        D3D12_DESCRIPTOR_RANGE uavRange = {};
        uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        uavRange.NumDescriptors = 19;
        uavRange.BaseShaderRegister = 0;
        uavRange.RegisterSpace = 0;
        uavRange.OffsetInDescriptorsFromTableStart = 0;

        std::array<D3D12_ROOT_PARAMETER, 3> rootParameters = {};
        rootParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParameters[0].Descriptor.ShaderRegister = 0;
        rootParameters[0].Descriptor.RegisterSpace = 0;
        rootParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        rootParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParameters[1].DescriptorTable.NumDescriptorRanges = 1;
        rootParameters[1].DescriptorTable.pDescriptorRanges = &srvRange;
        rootParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        rootParameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParameters[2].DescriptorTable.NumDescriptorRanges = 1;
        rootParameters[2].DescriptorTable.pDescriptorRanges = &uavRange;
        rootParameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rootDesc = {};
        rootDesc.NumParameters = static_cast<UINT>(rootParameters.size());
        rootDesc.pParameters = rootParameters.data();
        rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

        ComPtr<ID3DBlob> serialized;
        ComPtr<ID3DBlob> errors;
        if (FAILED(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors)))
        {
            return false;
        }

        return SUCCEEDED(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&m_rootSignature)));
    }

    bool ComputeSoftwareRasterizer::CreatePipeline(ID3D12Device* device)
    {
        if (!CreateRootSignature(device))
        {
            return false;
        }

        auto createCS = [&](const wchar_t* entryPoint, ID3D12PipelineState** outPSO) -> bool
        {
            ComPtr<IDxcBlob> bytecode = CompileShader(kShaderPath, entryPoint);
            if (!bytecode)
            {
                return false;
            }

            D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
            desc.pRootSignature = m_rootSignature.Get();
            desc.CS.pShaderBytecode = bytecode->GetBufferPointer();
            desc.CS.BytecodeLength = bytecode->GetBufferSize();
            return SUCCEEDED(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(outPSO)));
        };

        ComPtr<ID3D12PipelineState> clearPSO;
        ComPtr<ID3D12PipelineState> transformPSO;
        ComPtr<ID3D12PipelineState> setupPSO;
        ComPtr<ID3D12PipelineState> binPSO;
        ComPtr<ID3D12PipelineState> finePSO;
        ComPtr<ID3D12PipelineState> presentPSO;

        if (!(createCS(L"CS_Clear", clearPSO.GetAddressOf()) &&
              createCS(L"CS_TransformVertices", transformPSO.GetAddressOf()) &&
              createCS(L"CS_SetupAndCull", setupPSO.GetAddressOf()) &&
              createCS(L"CS_BinTriangles", binPSO.GetAddressOf()) &&
              createCS(L"CS_FineRasterize", finePSO.GetAddressOf()) &&
              createCS(L"CS_Present", presentPSO.GetAddressOf())))
        {
            return false;
        }

        m_clearPSO = clearPSO;
        m_transformPSO = transformPSO;
        m_setupPSO = setupPSO;
        m_binPSO = binPSO;
        m_finePSO = finePSO;
        m_presentPSO = presentPSO;

        std::error_code error;
        m_shaderWriteTime = std::filesystem::last_write_time(kShaderPath, error);
        m_hasShaderWriteTime = !error;
        return true;
    }

    void ComputeSoftwareRasterizer::Dispatch(ID3D12GraphicsCommandList* commandList,
                                             D3D12_GPU_DESCRIPTOR_HANDLE srvTable,
                                             D3D12_GPU_DESCRIPTOR_HANDLE uavTable,
                                             uint32_t vertexCount,
                                             uint32_t triangleCount,
                                             const RasterizerConstants& constants)
    {
        void* mapped = nullptr;
        D3D12_RANGE emptyRange = {0, 0};
        if (SUCCEEDED(m_constantsUpload->Map(0, &emptyRange, &mapped)))
        {
            std::memcpy(mapped, &constants, sizeof(constants));
            m_constantsUpload->Unmap(0, nullptr);
        }

        commandList->SetComputeRootSignature(m_rootSignature.Get());
        commandList->SetComputeRootConstantBufferView(0, m_constantsUpload->GetGPUVirtualAddress());
        commandList->SetComputeRootDescriptorTable(1, srvTable);
        commandList->SetComputeRootDescriptorTable(2, uavTable);

        const uint32_t pixelCount = m_config.viewportWidth * m_config.viewportHeight;
        const uint32_t tileCount = m_tileCountX * m_tileCountY;
        const uint32_t clearCount = (std::max)(pixelCount, tileCount);

        commandList->SetPipelineState(m_clearPSO.Get());
        commandList->Dispatch((clearCount + kClearGroupSize - 1) / kClearGroupSize, 1, 1);
        InsertUAVBarrier(commandList, m_visibleCounter.Get());
        InsertUAVBarrier(commandList, m_tileCounters.Get());
        InsertUAVBarrier(commandList, m_tileDepthHint.Get());
        InsertUAVBarrier(commandList, m_packedDepthColor.Get());
        InsertUAVBarrier(commandList, m_packedDepthColor32.Get());
        InsertUAVBarrier(commandList, m_tileDebugCounts.Get());
        InsertUAVBarrier(commandList, m_portalMask.Get());
        InsertUAVBarrier(commandList, m_profilingCounters.Get());
        InsertUAVBarrier(commandList, m_materialDensity.Get());
        InsertUAVBarrier(commandList, m_tileAcousticOcclusion.Get());
        InsertUAVBarrier(commandList, m_tileAcousticDiffraction.Get());
        InsertUAVBarrier(commandList, m_previousPackedTemporal.Get());
        InsertUAVBarrier(commandList, m_historyTexture.Get());
        InsertUAVBarrier(commandList, m_previousHistoryTexture.Get());

        commandList->SetPipelineState(m_transformPSO.Get());
        commandList->Dispatch((vertexCount + kVertexGroupSize - 1) / kVertexGroupSize, 1, 1);
        InsertUAVBarrier(commandList, m_projectedVertices.Get());

        commandList->SetPipelineState(m_setupPSO.Get());
        commandList->Dispatch((triangleCount + kTriangleGroupSize - 1) / kTriangleGroupSize, 1, 1);
        InsertUAVBarrier(commandList, m_visibleTriangles.Get());
        InsertUAVBarrier(commandList, m_visibleCounter.Get());

        commandList->SetPipelineState(m_binPSO.Get());
        commandList->Dispatch((m_config.maxVisibleTriangles + kBinGroupSize - 1) / kBinGroupSize, 1, 1);
        InsertUAVBarrier(commandList, m_tileCounters.Get());
        InsertUAVBarrier(commandList, m_tileTriangleIndices.Get());

        commandList->SetPipelineState(m_finePSO.Get());
        commandList->Dispatch(m_tileCountX, m_tileCountY, 1);
        InsertUAVBarrier(commandList, m_packedDepthColor.Get());
        InsertUAVBarrier(commandList, m_packedDepthColor32.Get());
        InsertUAVBarrier(commandList, m_tileDebugCounts.Get());
        InsertUAVBarrier(commandList, m_portalMask.Get());
        InsertUAVBarrier(commandList, m_profilingCounters.Get());
        InsertUAVBarrier(commandList, m_materialDensity.Get());
        InsertUAVBarrier(commandList, m_tileAcousticOcclusion.Get());
        InsertUAVBarrier(commandList, m_tileAcousticDiffraction.Get());
        InsertUAVBarrier(commandList, m_historyTexture.Get());
        InsertUAVBarrier(commandList, m_presentTexture.Get());

        commandList->SetPipelineState(m_presentPSO.Get());
        commandList->Dispatch(m_tileCountX, m_tileCountY, 1);

        D3D12_RESOURCE_BARRIER historyToCopy[4] = {};
        historyToCopy[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        historyToCopy[0].Transition.pResource = m_historyTexture.Get();
        historyToCopy[0].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        historyToCopy[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        historyToCopy[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        historyToCopy[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        historyToCopy[1].Transition.pResource = m_previousHistoryTexture.Get();
        historyToCopy[1].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        historyToCopy[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        historyToCopy[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        historyToCopy[2].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        historyToCopy[2].Transition.pResource = m_packedDepthColor32.Get();
        historyToCopy[2].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        historyToCopy[2].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        historyToCopy[2].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        historyToCopy[3].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        historyToCopy[3].Transition.pResource = m_previousPackedTemporal.Get();
        historyToCopy[3].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        historyToCopy[3].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        historyToCopy[3].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        commandList->ResourceBarrier(4, historyToCopy);

        commandList->CopyResource(m_previousHistoryTexture.Get(), m_historyTexture.Get());
        commandList->CopyResource(m_previousPackedTemporal.Get(), m_packedDepthColor32.Get());

        D3D12_RESOURCE_BARRIER copyToHistory[4] = {};
        copyToHistory[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        copyToHistory[0].Transition.pResource = m_historyTexture.Get();
        copyToHistory[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        copyToHistory[0].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        copyToHistory[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        copyToHistory[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        copyToHistory[1].Transition.pResource = m_previousHistoryTexture.Get();
        copyToHistory[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        copyToHistory[1].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        copyToHistory[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        copyToHistory[2].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        copyToHistory[2].Transition.pResource = m_packedDepthColor32.Get();
        copyToHistory[2].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        copyToHistory[2].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        copyToHistory[2].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        copyToHistory[3].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        copyToHistory[3].Transition.pResource = m_previousPackedTemporal.Get();
        copyToHistory[3].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        copyToHistory[3].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        copyToHistory[3].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        commandList->ResourceBarrier(4, copyToHistory);
    }
}
