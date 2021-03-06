//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#include "stdafx.h"
#include "TVRayTracer.h"
#include "XUSGObjLoader.h"
#define _INDEPENDENT_DDS_LOADER_
#include "Advanced/XUSGDDSLoader.h"
#undef _INDEPENDENT_DDS_LOADER_
#include "DirectXPackedVector.h"

using namespace std;
using namespace DirectX;
using namespace XUSG;
using namespace XUSG::RayTracing;

struct Vertex
{
    XMFLOAT3 Pos;
    XMFLOAT3 Norm;
};

struct RayGenConstants
{
    XMMATRIX ProjToWorld;
    XMVECTOR EyePt;
};

struct CBGlobal
{
    XMFLOAT3X4 WorldITs[TVRayTracer::NUM_MESH];
    XMFLOAT4X4 Worlds[TVRayTracer::NUM_MESH];
};

struct CBMaterial
{
    XMFLOAT4 BaseColors[TVRayTracer::NUM_MESH];
    XMFLOAT4 Albedos[TVRayTracer::NUM_MESH];
};

struct CBGraphics
{
    XMFLOAT4X4 WorldViewProj;
    XMFLOAT3X4 WorldIT;
    XMFLOAT2   ProjBias;
};

struct CBEnv
{
    XMMATRIX ProjToWorld;
    XMVECTOR EyePt;
    XMFLOAT2 Viewport;
};

struct CBTessellation
{
    uint32_t InstanceIdx;
    uint32_t TessFactor;
    uint32_t MaxVertPerPatch;
};

const wchar_t* TVRayTracer::HitGroupNames[] = { L"hitGroupRadiance", L"hitGroupShadow" };
const wchar_t* TVRayTracer::RaygenShaderName = L"raygenMain";
const wchar_t* TVRayTracer::ClosestHitShaderNames[] = { L"closestHitRadiance", L"closestHitShadow" };
const wchar_t* TVRayTracer::MissShaderNames[] = { L"missRadiance", L"missShadow" };

// isPrime for n >= 2
static inline bool isPrime(uint32_t n)
{
    for (auto i = 2u; i * i <= n; ++i)
    {
        if (n % i == 0) return false;
    }
    return true;
}

static inline uint32_t nextPrime(uint32_t n)
{
    while (!isPrime(n)) ++n;
    return n;
}

static inline uint32_t calcMaxVertPerPatch(uint32_t tessFactor)
{
    uint32_t k = tessFactor / 2 + 1;
    uint32_t from = (tessFactor & 1 ? 3 * k * k : 3 * k * (k - 1) + 1);
    return nextPrime(from);
};

TVRayTracer::TVRayTracer(const RayTracing::Device::sptr& device) :
    m_device(device),
    m_instances(),
    m_tessFactor(2)
{
    m_maxVertPerPatch = calcMaxVertPerPatch(m_tessFactor);
    m_shaderPool = ShaderPool::MakeUnique();
    m_rayTracingPipelineCache = RayTracing::PipelineCache::MakeUnique(device.get());
    m_graphicsPipelineCache = Graphics::PipelineCache::MakeUnique(device.get());
    m_pipelineLayoutCache = PipelineLayoutCache::MakeUnique(device.get());
    m_descriptorTableCache = DescriptorTableCache::MakeUnique(device.get(), L"RayTracerDescriptorTableCache");

    AccelerationStructure::SetUAVCount(NUM_MESH + NUM_HIT_GROUP + 1);
}

TVRayTracer::~TVRayTracer()
{
}

bool TVRayTracer::Init(
    RayTracing::CommandList* pCommandList,
    uint32_t                 width,
    uint32_t                 height,
    vector<Resource::uptr>&  uploaders,
    GeometryBuffer*          pGeometries,
    const char*              fileName,
    const wchar_t*           envFileName,
    Format                   rtFormat,
    const XMFLOAT4&          posScale)
{
    m_viewport = XMUINT2(width, height);
    m_posScale = posScale;

    // Load inputs
    ObjLoader objLoader;
    if (!objLoader.Import(fileName, true, true)) return false;
    auto numVertices = objLoader.GetNumVertices();
    auto numIndices = objLoader.GetNumIndices();
    N_RETURN(createVB(pCommandList, numVertices, objLoader.GetVertexStride(), objLoader.GetVertices(), uploaders), false);
    N_RETURN(createIB(pCommandList, numIndices, objLoader.GetIndices(), uploaders), false);

    N_RETURN(createGroundMesh(pCommandList, uploaders), false);

    // Create output view
    {
        m_outputView = Texture2D::MakeUnique();
        N_RETURN(m_outputView->Create(m_device.get(), width, height, Format::R11G11B10_FLOAT, 1,
            ResourceFlag::ALLOW_UNORDERED_ACCESS, 1, 1, false, MemoryFlag::NONE,
            L"GraphicsOut"), false);
    }

    for (auto i = 0u; i < NUM_MESH; ++i)
    {
        m_numMaxTessVerts[i] = m_numIndices[i] / 3u * m_maxVertPerPatch;
    }

    // Create tessellated vertex color buffer
    for (auto i = 0u; i < NUM_MESH; ++i)
    {
        const uint32_t MMVerts = m_numIndices[i] / 3u * calcMaxVertPerPatch(MaxTessFactor);

        m_tessColors[i] = StructuredBuffer::MakeUnique();
        N_RETURN(m_tessColors[i]->Create(m_device.get(), MMVerts, sizeof(XMFLOAT3), ResourceFlag::ALLOW_UNORDERED_ACCESS), false);

        m_tessDoms[i] = StructuredBuffer::MakeUnique();
        N_RETURN(m_tessDoms[i]->Create(m_device.get(), MMVerts, sizeof(XMFLOAT2), ResourceFlag::ALLOW_UNORDERED_ACCESS), false);
    }

    m_cbGlobal = ConstantBuffer::MakeUnique();
    N_RETURN(m_cbGlobal->Create(m_device.get(), sizeof(CBGlobal[FrameCount]), FrameCount,
        nullptr, MemoryType::UPLOAD, MemoryFlag::NONE, L"CBGlobal"), false);

    m_cbEnv = ConstantBuffer::MakeUnique();
    N_RETURN(m_cbEnv->Create(m_device.get(), sizeof(CBEnv[FrameCount]), FrameCount,
        nullptr, MemoryType::UPLOAD, MemoryFlag::NONE, L"CBEnv"), false);

    m_cbMaterials = ConstantBuffer::MakeUnique();
    N_RETURN(m_cbMaterials->Create(m_device.get(), sizeof(CBMaterial), 1,
        nullptr, MemoryType::UPLOAD, MemoryFlag::NONE, L"CBMaterial"), false);
    {
        const auto pCbData = reinterpret_cast<CBMaterial*>(m_cbMaterials->Map());
        pCbData->BaseColors[GROUND] = XMFLOAT4(0.3, 0.1, 0.1, 10);
        pCbData->Albedos[GROUND] = XMFLOAT4(0.9, 0.1, 0.0, 0);
        pCbData->BaseColors[MODEL_OBJ] = XMFLOAT4(1, 1, 1, 1425);
        pCbData->Albedos[MODEL_OBJ] = XMFLOAT4(0, 10, 0.8, 0);
    }

    for (auto i = 0u; i < NUM_MESH; ++i)
    {
        auto& cbGraphics = m_cbGraphics[i];
        cbGraphics = ConstantBuffer::MakeUnique();
        N_RETURN(cbGraphics->Create(m_device.get(), sizeof(CBGraphics[FrameCount]), FrameCount,
            nullptr, MemoryType::UPLOAD, MemoryFlag::NONE, L"CBGraphics"), false);
    }

    // Load input image
    {
        DDS::Loader textureLoader;
        DDS::AlphaMode alphaMode;

        uploaders.emplace_back(Resource::MakeUnique());
        N_RETURN(textureLoader.CreateTextureFromFile(m_device.get(), static_cast<XUSG::CommandList*>(pCommandList), envFileName,
            8192, false, m_lightProbe, uploaders.back().get(), &alphaMode), false);
    }

    const auto dsFormat = Format::D24_UNORM_S8_UINT;
    m_depth = DepthStencil::MakeUnique();
    N_RETURN(m_depth->Create(m_device.get(), width, height, dsFormat, ResourceFlag::NONE,
        1, 1, 1, 1.0f, 0, false, MemoryFlag::NONE, L"Depth"), false);

    // Create raytracing pipelines
    N_RETURN(createInputLayout(), false);
    N_RETURN(createPipelineLayouts(), false);
    N_RETURN(createPipelines(rtFormat, dsFormat), false);

    // Build acceleration structures
    N_RETURN(buildAccelerationStructures(pCommandList, pGeometries), false);
    N_RETURN(buildShaderTables(), false);

    return true;
}

static const XMFLOAT2& IncrementalHalton()
{
    static auto haltonBase = XMUINT2(0, 0);
    static auto halton = XMFLOAT2(0.0f, 0.0f);

    // Base 2
    {
        // Bottom bit always changes, higher bits
        // Change less frequently.
        auto change = 0.5f;
        auto oldBase = haltonBase.x++;
        auto diff = haltonBase.x ^ oldBase;

        // Diff will be of the form 0*1+, i.e. one bits up until the last carry.
        // Expected iterations = 1 + 0.5 + 0.25 + ... = 2
        do
        {
            halton.x += (oldBase & 1) ? -change : change;
            change *= 0.5f;

            diff = diff >> 1;
            oldBase = oldBase >> 1;
        } while (diff);
    }

    // Base 3
    {
        const auto oneThird = 1.0f / 3.0f;
        auto mask = 0x3u;	// Also the max base 3 digit
        auto add = 0x1u;	// Amount to add to force carry once digit == 3
        auto change = oneThird;
        ++haltonBase.y;

        // Expected iterations: 1.5
        while (true)
        {
            if ((haltonBase.y & mask) == mask)
            {
                haltonBase.y += add;	// Force carry into next 2-bit digit
                halton.y -= 2 * change;

                mask = mask << 2;
                add = add << 2;

                change *= oneThird;
            }
            else
            {
                halton.y += change;	// We know digit n has gone from a to a + 1
                break;
            }
        };
    }

    return halton;
}

void TVRayTracer::UpdateFrame(
    uint8_t   frameIndex,
    CXMVECTOR eyePt,
    CXMMATRIX viewProj,
    float     timeStep,
    uint32_t  tessFactor)
{
    const auto halton = IncrementalHalton();
    XMFLOAT2 projBias =
    {
        (halton.x * 2.0f - 1.0f) / m_viewport.x,
        (halton.y * 2.0f - 1.0f) / m_viewport.y
    };

    {
        const auto projToWorld = XMMatrixInverse(nullptr, viewProj);
        RayGenConstants cbRayGen = { XMMatrixTranspose(projToWorld), eyePt };

        m_rayGenShaderTables[frameIndex]->Reset();
        m_rayGenShaderTables[frameIndex]->AddShaderRecord(ShaderRecord::MakeUnique(m_device.get(),
            m_pipelines[RAY_TRACING], RaygenShaderName, &cbRayGen, sizeof(RayGenConstants)).get());
        m_hitGroupShaderTable->Reset();
        m_hitGroupShaderTable->AddShaderRecord(ShaderRecord::MakeUnique(m_device.get(),
            m_pipelines[RAY_TRACING], HitGroupNames[HIT_GROUP_RADIANCE], &cbRayGen, sizeof(RayGenConstants)).get());

        const auto pCbEnv = reinterpret_cast<CBEnv*>(m_cbEnv->Map(frameIndex));
        pCbEnv->ProjToWorld = XMMatrixTranspose(projToWorld);
        pCbEnv->EyePt = eyePt;
        pCbEnv->Viewport = XMFLOAT2(static_cast<float>(m_viewport.x), static_cast<float>(m_viewport.y));
    }

    {
        static auto angle = 0.0f;
        angle += 16.0f * timeStep * XM_PI / 180.0f;
        const auto rot = XMMatrixRotationY(angle);

        XMMATRIX worlds[NUM_MESH] =
        {
            XMMatrixScaling(10.0f, 0.5f, 10.0f) * XMMatrixTranslation(0.0f, -0.5f, 0.0f),
            XMMatrixScaling(m_posScale.w, m_posScale.w, m_posScale.w) * rot *
            XMMatrixTranslation(m_posScale.x, m_posScale.y, m_posScale.z)
        };

        for (auto i = 0u; i < NUM_MESH; ++i)
        {
            XMStoreFloat4x4(&m_worlds[i], XMMatrixTranspose(worlds[i]));
        }

        const auto pCbGlobal = reinterpret_cast<CBGlobal*>(m_cbGlobal->Map(frameIndex));
        for (auto i = 0u; i < NUM_MESH; ++i)
        {
            XMStoreFloat3x4(&pCbGlobal->WorldITs[i], i ? rot : XMMatrixIdentity());
            pCbGlobal->Worlds[i] = m_worlds[i];
        }

        for (auto i = 0u; i < NUM_MESH; ++i)
        {
            const auto pCbGraphics = reinterpret_cast<CBGraphics*>(m_cbGraphics[i]->Map(frameIndex));
            pCbGraphics->ProjBias = projBias;
            XMStoreFloat4x4(&pCbGraphics->WorldViewProj, XMMatrixTranspose(worlds[i] * viewProj));
            XMStoreFloat3x4(&pCbGraphics->WorldIT, i ? rot : XMMatrixIdentity());
        }
    }

    if (m_tessFactor != tessFactor)
    {
        m_tessFactor = tessFactor;
        m_maxVertPerPatch = calcMaxVertPerPatch(m_tessFactor);
        for (auto i = 0u; i < NUM_MESH; ++i)
        {
            m_numMaxTessVerts[i] = m_numIndices[i] / 3u * m_maxVertPerPatch;
        }
    }
}

void TVRayTracer::Render(
    const RayTracing::CommandList* pCommandList,
    uint8_t                        frameIndex,
    const Descriptor&              rtv,
    uint32_t                       numBarriers,
    ResourceBarrier*               pBarriers)
{
    // Bind the heaps
    const DescriptorPool descriptorPools[] =
    {
        m_descriptorTableCache->GetDescriptorPool(CBV_SRV_UAV_POOL),
        m_descriptorTableCache->GetDescriptorPool(SAMPLER_POOL)
    };
    pCommandList->SetDescriptorPools(static_cast<uint32_t>(size(descriptorPools)), descriptorPools);

    zPrepass(pCommandList, frameIndex);
    envPrepass(pCommandList, frameIndex);
    tessellate(pCommandList, frameIndex);
    raytrace(pCommandList, frameIndex);
    rasterize(pCommandList, frameIndex);
    toneMap(pCommandList, rtv, numBarriers, pBarriers);
}

void TVRayTracer::UpdateAccelerationStructures(
    const RayTracing::CommandList* pCommandList,
    uint8_t                        frameIndex)
{
    // Set instance
    float* const transforms[] =
    {
        reinterpret_cast<float*>(&m_worlds[GROUND]),
        reinterpret_cast<float*>(&m_worlds[MODEL_OBJ])
    };
    const BottomLevelAS* pBottomLevelASs[NUM_MESH];
    for (auto i = 0u; i < NUM_MESH; ++i) pBottomLevelASs[i] = m_bottomLevelASs[i].get();
    TopLevelAS::SetInstances(m_device.get(), m_instances[frameIndex].get(), NUM_MESH, pBottomLevelASs, transforms);

    // Update top level AS 
    const auto& descriptorPool = m_descriptorTableCache->GetDescriptorPool(CBV_SRV_UAV_POOL);
    m_topLevelAS->Build(pCommandList, m_scratch.get(), m_instances[frameIndex].get(), descriptorPool, true);
}

bool TVRayTracer::createVB(
    RayTracing::CommandList* pCommandList,
    uint32_t                 numVert,
    uint32_t                 stride,
    const uint8_t*           pData,
    vector<Resource::uptr>&  uploaders)
{
    m_numVerts[MODEL_OBJ] = numVert;
    auto& vertexBuffer = m_vertexBuffers[MODEL_OBJ];
    vertexBuffer = VertexBuffer::MakeUnique();
    N_RETURN(vertexBuffer->Create(m_device.get(), numVert, sizeof(Vertex),
        ResourceFlag::NONE, MemoryType::DEFAULT, 1, nullptr, 1,
        nullptr, 1, nullptr, MemoryFlag::NONE, L"MeshVB"), false);

    uploaders.emplace_back(Resource::MakeUnique());
    return vertexBuffer->Upload(pCommandList, uploaders.back().get(), pData,
        sizeof(Vertex) * numVert, 0, ResourceState::NON_PIXEL_SHADER_RESOURCE);
}

bool TVRayTracer::createIB(
    RayTracing::CommandList* pCommandList,
    uint32_t                 numIndices,
    const uint32_t*          pData,
    vector<Resource::uptr>&  uploaders)
{
    m_numIndices[MODEL_OBJ] = numIndices;

    auto& indexBuffer = m_indexBuffers[MODEL_OBJ];
    const uint32_t byteWidth = sizeof(uint32_t) * numIndices;
    indexBuffer = IndexBuffer::MakeUnique();
    N_RETURN(indexBuffer->Create(m_device.get(), byteWidth, Format::R32_UINT, ResourceFlag::NONE,
        MemoryType::DEFAULT, 1, nullptr, 1, nullptr, 1, nullptr, MemoryFlag::NONE, L"MeshIB"), false);

    uploaders.emplace_back(Resource::MakeUnique());
    return indexBuffer->Upload(pCommandList, uploaders.back().get(), pData,
        byteWidth, 0, ResourceState::NON_PIXEL_SHADER_RESOURCE);
}

bool TVRayTracer::createGroundMesh(
    RayTracing::CommandList* pCommandList,
    vector<Resource::uptr>& uploaders)
{
    const uint32_t N = 64;
    // Vertex buffer
    {
        // Cube vertices positions and corresponding triangle normals.
        static Vertex vertices[N * N * 6];
        for (auto i = 0u; i < N; ++i)
            for (auto j = 0u; j < N; ++j)
                vertices[N * i + j] = {
                    XMFLOAT3(-1.0f + 2.0f * j / (N - 1), 1.0f, 1.0f - 2.0f * i / (N - 1)),
                    XMFLOAT3(0.0f, 1.0f, 0.0f)
                };
        for (auto i = 0u; i < N; ++i)
            for (auto j = 0u; j < N; ++j)
                vertices[N * N + N * i + j] = {
                    XMFLOAT3(-1.0f + 2.0f * j / (N - 1), -1.0f, 1.0f - 2.0f * i / (N - 1)),
                    XMFLOAT3(0.0f, -1.0f, 0.0f)
                };
        for (auto i = 0u; i < N; ++i)
            for (auto j = 0u; j < N; ++j)
                vertices[2 * N * N + N * i + j] = {
                    XMFLOAT3(-1.0f, -1.0f + 2.0f * j / (N - 1), 1.0f - 2.0f * i / (N - 1)),
                    XMFLOAT3(-1.0f, 0.0f, 0.0f)
                };
        for (auto i = 0u; i < N; ++i)
            for (auto j = 0u; j < N; ++j)
                vertices[3 * N * N + N * i + j] = {
                    XMFLOAT3(1.0f, -1.0f + 2.0f * j / (N - 1), 1.0f - 2.0f * i / (N - 1)),
                    XMFLOAT3(1.0f, 0.0f, 0.0f)
                };
        for (auto i = 0u; i < N; ++i)
            for (auto j = 0u; j < N; ++j)
                vertices[4 * N * N + N * i + j] = {
                    XMFLOAT3(-1.0f + 2.0f * j / (N - 1), 1.0f - 2.0f * i / (N - 1), -1.0f),
                    XMFLOAT3(0.0f, 0.0f, -1.0f)
                };
        for (auto i = 0u; i < N; ++i)
            for (auto j = 0u; j < N; ++j)
                vertices[5 * N * N + N * i + j] = {
                    XMFLOAT3(-1.0f + 2.0f * j / (N - 1), 1.0f - 2.0f * i / (N - 1), 1.0f),
                    XMFLOAT3(0.0f, 0.0f,1.0f)
                };

        auto& vertexBuffer = m_vertexBuffers[GROUND];
        vertexBuffer = VertexBuffer::MakeUnique();
        auto numVert = static_cast<uint32_t>(size(vertices));
        m_numVerts[GROUND] = numVert;
        N_RETURN(vertexBuffer->Create(m_device.get(), numVert, sizeof(Vertex),
            ResourceFlag::NONE, MemoryType::DEFAULT, 1, nullptr, 1, nullptr,
            1, nullptr, MemoryFlag::NONE, L"GroundVB"), false);

        uploaders.push_back(Resource::MakeUnique());
        N_RETURN(vertexBuffer->Upload(pCommandList, uploaders.back().get(), vertices,
            sizeof(vertices), 0, ResourceState::NON_PIXEL_SHADER_RESOURCE), false);
    }

    // Index Buffer
    {
        // Cube indices.
        static uint32_t indices[6][N - 1][N - 1][2][3];
        for (auto s = 0u; s < 6u; s += 2)
        {
            for (auto i = 0u; i < N - 1; ++i)
            {
                for (auto j = 0u; j < N - 1; ++j)
                {
                    indices[s][i][j][0][0] = i * N + j + s * N * N;
                    indices[s][i][j][0][1] = i * N + j + 1 + s * N * N;
                    indices[s][i][j][0][2] = (i + 1) * N + j + 1 + s * N * N;
                    indices[s][i][j][1][0] = i * N + j + s * N * N;
                    indices[s][i][j][1][1] = (i + 1) * N + j + 1 + s * N * N;
                    indices[s][i][j][1][2] = (i + 1) * N + j + s * N * N;
                    indices[s + 1][i][j][0][0] = i * N + j + (s + 1) * N * N;
                    indices[s + 1][i][j][0][1] = (i + 1) * N + j + 1 + (s + 1) * N * N;
                    indices[s + 1][i][j][0][2] = i * N + j + 1 + (s + 1) * N * N;
                    indices[s + 1][i][j][1][0] = i * N + j + (s + 1) * N * N;
                    indices[s + 1][i][j][1][1] = (i + 1) * N + j + (s + 1) * N * N;
                    indices[s + 1][i][j][1][2] = (i + 1) * N + j + 1 + (s + 1) * N * N;
                }
            }
        }
        auto numIndices = 36 * (N - 1) * (N - 1);
        m_numIndices[GROUND] = numIndices;

        auto& indexBuffer = m_indexBuffers[GROUND];
        indexBuffer = IndexBuffer::MakeUnique();
        N_RETURN(indexBuffer->Create(m_device.get(), sizeof(indices), Format::R32_UINT, ResourceFlag::NONE,
            MemoryType::DEFAULT, 1, nullptr, 1, nullptr, 1, nullptr, MemoryFlag::NONE, L"GroundIB"), false);

        uploaders.push_back(Resource::MakeUnique());
        N_RETURN(indexBuffer->Upload(pCommandList, uploaders.back().get(), indices,
            sizeof(indices), 0, ResourceState::NON_PIXEL_SHADER_RESOURCE), false);
    }

    return true;
}

bool TVRayTracer::createInputLayout()
{
    // Define the vertex input layout.
    const InputElement inputElements[] =
    {
        { "POSITION",	0, Format::R32G32B32_FLOAT,	0, 0,								InputClassification::PER_VERTEX_DATA, 0 },
        { "NORMAL",		0, Format::R32G32B32_FLOAT,	0, D3D12_APPEND_ALIGNED_ELEMENT,	InputClassification::PER_VERTEX_DATA, 0 }
    };

    X_RETURN(m_pInputLayout, m_graphicsPipelineCache->CreateInputLayout(inputElements, static_cast<uint32_t>(size(inputElements))), false);

    return true;
}

bool TVRayTracer::createPipelineLayouts()
{
    // Z prepass pipeline layout
    {
        // Get pipeline layout
        const auto pipelineLayout = Util::PipelineLayout::MakeUnique();
        pipelineLayout->SetConstants(0, 1, 0, 0, Shader::Stage::HS);
        pipelineLayout->SetRootCBV(1, 0, 0, Shader::Stage::DS);
        X_RETURN(m_pipelineLayouts[Z_PRE_LAYOUT], pipelineLayout->GetPipelineLayout(m_pipelineLayoutCache.get(),
            PipelineLayoutFlag::ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT, L"ZPrepassLayout"), false);
    }

    // Env prepass pipeline layout
    {
        const auto pipelineLayout = Util::PipelineLayout::MakeUnique();
        pipelineLayout->SetRootCBV(0, 0, 0, Shader::Stage::PS);
        pipelineLayout->SetRange(1, DescriptorType::UAV, 1, 0);
        pipelineLayout->SetRange(2, DescriptorType::SRV, 1, 0);
        pipelineLayout->SetRange(3, DescriptorType::SAMPLER, 1, 0);
        pipelineLayout->SetShaderStage(0, Shader::Stage::PS);
        X_RETURN(m_pipelineLayouts[ENV_PRE_LAYOUT], pipelineLayout->GetPipelineLayout(m_pipelineLayoutCache.get(), PipelineLayoutFlag::NONE, L"EnvPrepassPipelineLayout"), false);
    }

    // Tessellation pass pipeline layout
    {
        const auto pipelineLayout = Util::PipelineLayout::MakeUnique();
        pipelineLayout->SetConstants(0, SizeOfInUint32(CBTessellation), 0);
        pipelineLayout->SetRange(1, DescriptorType::UAV, NUM_MESH, 0);
        X_RETURN(m_pipelineLayouts[TESSELLATION_LAYOUT], pipelineLayout->GetPipelineLayout(m_pipelineLayoutCache.get(), PipelineLayoutFlag::ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT, L"TessellationPipelineLayout"), false);
    }

    // Global pipeline layout
    // This is a pipeline layout that is shared across all raytracing shaders invoked during a DispatchRays() call.
    {
        const auto pipelineLayout = RayTracing::PipelineLayout::MakeUnique();
        pipelineLayout->SetRange(VERTEX_COLOR, DescriptorType::UAV, 2, 0);
        pipelineLayout->SetRootSRV(ACCELERATION_STRUCTURE, 0, 0, DescriptorFlag::DATA_STATIC);
        pipelineLayout->SetRange(SAMPLER, DescriptorType::SAMPLER, 1, 0);
        pipelineLayout->SetRange(INDEX_BUFFERS, DescriptorType::SRV, NUM_MESH, 0, 1);
        pipelineLayout->SetRange(VERTEX_BUFFERS, DescriptorType::SRV, NUM_MESH, 0, 2);
        pipelineLayout->SetRootCBV(MATERIALS, 0);
        pipelineLayout->SetRootCBV(CONSTANTS, 1);
        pipelineLayout->SetConstants(TESS_CONSTS, SizeOfInUint32(CBTessellation), 3);
        pipelineLayout->SetRange(TESS_DOMS, DescriptorType::SRV, NUM_MESH, 2);
        pipelineLayout->SetRange(ENV_TEXTURE, DescriptorType::SRV, 1, 1);
        X_RETURN(m_pipelineLayouts[RT_GLOBAL_LAYOUT], pipelineLayout->GetPipelineLayout(
            m_device.get(), m_pipelineLayoutCache.get(), PipelineLayoutFlag::NONE,
            L"RayTracerGlobalPipelineLayout"), false);
    }

    // Local pipeline layout for RayGen shader
    // This is a pipeline layout that enables a shader to have unique arguments that come from shader tables.
    {
        const auto pipelineLayout = RayTracing::PipelineLayout::MakeUnique();
        pipelineLayout->SetConstants(0, SizeOfInUint32(RayGenConstants), 2);
        X_RETURN(m_pipelineLayouts[RAY_GEN_LAYOUT], pipelineLayout->GetPipelineLayout(
            m_device.get(), m_pipelineLayoutCache.get(), PipelineLayoutFlag::LOCAL_PIPELINE_LAYOUT,
            L"RayTracerRayGenPipelineLayout"), false);
    }

    // Local pipeline layout for HitRadiance shader
    // This is a pipeline layout that enables a shader to have unique arguments that come from shader tables.
    {
        const auto pipelineLayout = RayTracing::PipelineLayout::MakeUnique();
        pipelineLayout->SetConstants(0, SizeOfInUint32(RayGenConstants), 2);
        X_RETURN(m_pipelineLayouts[HIT_RADIANCE_LAYOUT], pipelineLayout->GetPipelineLayout(
            m_device.get(), m_pipelineLayoutCache.get(), PipelineLayoutFlag::LOCAL_PIPELINE_LAYOUT,
            L"RayTracerHitRadiancePipelineLayout"), false);
    }

    // Pipeline layout for graphics pass
    {
        const auto pipelineLayout = Util::PipelineLayout::MakeUnique();
        pipelineLayout->SetConstants(0, SizeOfInUint32(CBTessellation), 0);
        pipelineLayout->SetRootCBV(1, 1, 0, Shader::Stage::DS);
        pipelineLayout->SetRange(2, DescriptorType::SRV, NUM_MESH, 0);
        pipelineLayout->SetRange(3, DescriptorType::UAV, 1, 0);

        X_RETURN(m_pipelineLayouts[GRAPHICS_LAYOUT], pipelineLayout->GetPipelineLayout(m_pipelineLayoutCache.get(), PipelineLayoutFlag::ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT, L"GraphicsPipelineLayout"), false);
    }

    // Pipeline layout for tone mapping
    {
        const auto pipelineLayout = Util::PipelineLayout::MakeUnique();
        pipelineLayout->SetRange(0, DescriptorType::SRV, 1, 0);
        pipelineLayout->SetShaderStage(0, Shader::Stage::PS);
        X_RETURN(m_pipelineLayouts[TONEMAP_LAYOUT], pipelineLayout->GetPipelineLayout(m_pipelineLayoutCache.get(), PipelineLayoutFlag::NONE, L"ToneMappingPipelineLayout"), false);
    }

    return true;
}

bool TVRayTracer::createPipelines(Format rtFormat, Format dsFormat)
{
    N_RETURN(m_shaderPool->CreateShader(Shader::Stage::VS, ShaderIndex::VS_IDENT, L"VSIdent.cso"), false);
    N_RETURN(m_shaderPool->CreateShader(Shader::Stage::HS, ShaderIndex::HS_DEPTH, L"HSDepth.cso"), false);
    N_RETURN(m_shaderPool->CreateShader(Shader::Stage::DS, ShaderIndex::DS_DEPTH, L"DSDepth.cso"), false);
    N_RETURN(m_shaderPool->CreateShader(Shader::Stage::VS, ShaderIndex::VS_SQUAD, L"VSScreenQuad.cso"), false);
    N_RETURN(m_shaderPool->CreateShader(Shader::Stage::PS, ShaderIndex::PS_ENV, L"PSEnv.cso"), false);
    N_RETURN(m_shaderPool->CreateShader(Shader::Stage::HS, ShaderIndex::HS_GRAPHICS, L"TVHullShader.cso"), false);
    N_RETURN(m_shaderPool->CreateShader(Shader::Stage::DS, ShaderIndex::DS_TESS, L"TVDSTess.cso"), false);
    N_RETURN(m_shaderPool->CreateShader(Shader::Stage::CS, ShaderIndex::CS_RT, L"TVRayTracing.cso"), false);
    N_RETURN(m_shaderPool->CreateShader(Shader::Stage::DS, ShaderIndex::DS_GRAPHICS, L"TVDSGraphics.cso"), false);
    N_RETURN(m_shaderPool->CreateShader(Shader::Stage::PS, ShaderIndex::PS_GRAPHICS, L"TVPixelShader.cso"), false);
    N_RETURN(m_shaderPool->CreateShader(Shader::Stage::PS, ShaderIndex::PS_TONEMAP, L"PSToneMap.cso"), false);

    // Z prepass
    {
        const auto state = Graphics::State::MakeUnique();
        state->SetPipelineLayout(m_pipelineLayouts[Z_PRE_LAYOUT]);
        state->SetShader(Shader::Stage::VS, m_shaderPool->GetShader(Shader::Stage::VS, ShaderIndex::VS_IDENT));
        state->SetShader(Shader::Stage::HS, m_shaderPool->GetShader(Shader::Stage::HS, ShaderIndex::HS_DEPTH));
        state->SetShader(Shader::Stage::DS, m_shaderPool->GetShader(Shader::Stage::DS, ShaderIndex::DS_DEPTH));
        state->IASetInputLayout(m_pInputLayout);
        state->IASetPrimitiveTopologyType(PrimitiveTopologyType::PATCH);
        state->OMSetDSVFormat(dsFormat);
        X_RETURN(m_pipelines[Z_PREPASS], state->GetPipeline(m_graphicsPipelineCache.get(), L"ZPrepass"), false);
    }

    // Env prepass
    {
        const auto state = Graphics::State::MakeUnique();
        state->SetPipelineLayout(m_pipelineLayouts[ENV_PRE_LAYOUT]);
        state->SetShader(Shader::Stage::VS, m_shaderPool->GetShader(Shader::Stage::VS, ShaderIndex::VS_SQUAD));
        state->SetShader(Shader::Stage::PS, m_shaderPool->GetShader(Shader::Stage::PS, ShaderIndex::PS_ENV));
        state->DSSetState(Graphics::DEPTH_STENCIL_NONE, m_graphicsPipelineCache.get());
        state->IASetPrimitiveTopologyType(PrimitiveTopologyType::TRIANGLE);
        X_RETURN(m_pipelines[ENV_PREPASS], state->GetPipeline(m_graphicsPipelineCache.get(), L"EnvPrepass"), false);
    }

    // Tessellation pass
    {
        const auto state = Graphics::State::MakeUnique();
        state->SetPipelineLayout(m_pipelineLayouts[TESSELLATION_LAYOUT]);
        state->SetShader(Shader::Stage::VS, m_shaderPool->GetShader(Shader::Stage::VS, ShaderIndex::VS_IDENT));
        state->SetShader(Shader::Stage::HS, m_shaderPool->GetShader(Shader::Stage::HS, ShaderIndex::HS_GRAPHICS));
        state->SetShader(Shader::Stage::DS, m_shaderPool->GetShader(Shader::Stage::DS, ShaderIndex::DS_TESS));
        state->DSSetState(Graphics::DEPTH_STENCIL_NONE, m_graphicsPipelineCache.get());
        state->IASetInputLayout(m_pInputLayout);
        state->IASetPrimitiveTopologyType(PrimitiveTopologyType::PATCH);
        state->OMSetNumRenderTargets(0);
        X_RETURN(m_pipelines[TESSELLATION], state->GetPipeline(m_graphicsPipelineCache.get(), L"TessellationPass"), false);
    }

    // Ray tracing pass
    {
        const auto state = RayTracing::State::MakeUnique();
        state->SetShaderLibrary(m_shaderPool->GetShader(Shader::Stage::CS, ShaderIndex::CS_RT));
        state->SetHitGroup(HIT_GROUP_RADIANCE, HitGroupNames[HIT_GROUP_RADIANCE], ClosestHitShaderNames[HIT_GROUP_RADIANCE]);
        state->SetHitGroup(HIT_GROUP_SHADOW, HitGroupNames[HIT_GROUP_SHADOW], ClosestHitShaderNames[HIT_GROUP_SHADOW]);
        state->SetShaderConfig(sizeof(XMFLOAT4), sizeof(XMFLOAT2));
        state->SetLocalPipelineLayout(0, m_pipelineLayouts[RAY_GEN_LAYOUT],
            1, reinterpret_cast<const void**>(&RaygenShaderName));
        state->SetLocalPipelineLayout(1, m_pipelineLayouts[HIT_RADIANCE_LAYOUT],
            1, reinterpret_cast<const void**>(&ClosestHitShaderNames[HIT_GROUP_RADIANCE]));
        state->SetGlobalPipelineLayout(m_pipelineLayouts[RT_GLOBAL_LAYOUT]);
        state->SetMaxRecursionDepth(2);
        X_RETURN(m_pipelines[RAY_TRACING], state->GetPipeline(m_rayTracingPipelineCache.get(), L"Raytracing"), false);
    }

    // Graphics pass
    {
        const auto state = Graphics::State::MakeUnique();
        state->SetPipelineLayout(m_pipelineLayouts[GRAPHICS_LAYOUT]);
        state->SetShader(Shader::Stage::VS, m_shaderPool->GetShader(Shader::Stage::VS, ShaderIndex::VS_IDENT));
        state->SetShader(Shader::Stage::HS, m_shaderPool->GetShader(Shader::Stage::HS, ShaderIndex::HS_GRAPHICS));
        state->SetShader(Shader::Stage::DS, m_shaderPool->GetShader(Shader::Stage::DS, ShaderIndex::DS_GRAPHICS));
        state->SetShader(Shader::Stage::PS, m_shaderPool->GetShader(Shader::Stage::PS, ShaderIndex::PS_GRAPHICS));
        state->DSSetState(Graphics::DEPTH_READ_EQUAL, m_graphicsPipelineCache.get());
        state->IASetInputLayout(m_pInputLayout);
        state->IASetPrimitiveTopologyType(PrimitiveTopologyType::PATCH);
        state->OMSetDSVFormat(m_depth->GetFormat());
        state->OMSetNumRenderTargets(0);
        X_RETURN(m_pipelines[GRAPHICS], state->GetPipeline(m_graphicsPipelineCache.get(), L"GraphicsPass"), false);
    }

    // Tone mapping
    {
        const auto state = Graphics::State::MakeUnique();
        state->SetPipelineLayout(m_pipelineLayouts[TONEMAP_LAYOUT]);
        state->SetShader(Shader::Stage::VS, m_shaderPool->GetShader(Shader::Stage::VS, ShaderIndex::VS_SQUAD));
        state->SetShader(Shader::Stage::PS, m_shaderPool->GetShader(Shader::Stage::PS, ShaderIndex::PS_TONEMAP));
        state->DSSetState(Graphics::DEPTH_STENCIL_NONE, m_graphicsPipelineCache.get());
        state->IASetPrimitiveTopologyType(PrimitiveTopologyType::TRIANGLE);
        state->OMSetNumRenderTargets(1);
        state->OMSetRTVFormat(0, rtFormat);
        X_RETURN(m_pipelines[TONEMAP], state->GetPipeline(m_graphicsPipelineCache.get(), L"ToneMapping"), false);
    }

    return true;
}

bool TVRayTracer::createDescriptorTables()
{
    // Output UAV
    {
        const auto descriptorTable = Util::DescriptorTable::MakeUnique();
        descriptorTable->SetDescriptors(0, 1, &m_outputView->GetUAV());
        X_RETURN(m_uavTables[UAV_TABLE_OUTPUT], descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false);
    }

    // Tessellation domains UAV
    {
        Descriptor descriptors[NUM_MESH];
        for (auto i = 0u; i < NUM_MESH; ++i) descriptors[i] = m_tessDoms[i]->GetUAV();
        const auto descriptorTable = Util::DescriptorTable::MakeUnique();
        descriptorTable->SetDescriptors(0, static_cast<uint32_t>(size(descriptors)), descriptors);
        X_RETURN(m_uavTables[UAV_TABLE_TESSDOMS], descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false);
    }

    // Tessellated vertex color UAV
    {
        Descriptor descriptors[NUM_MESH];
        for (auto i = 0u; i < NUM_MESH; ++i) descriptors[i] = m_tessColors[i]->GetUAV();
        const auto descriptorTable = Util::DescriptorTable::MakeUnique();
        descriptorTable->SetDescriptors(0, static_cast<uint32_t>(size(descriptors)), descriptors);
        X_RETURN(m_uavTables[UAV_TABLE_RT], descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false);
    }

    // Index buffer SRVs
    {
        Descriptor descriptors[NUM_MESH];
        for (auto i = 0u; i < NUM_MESH; ++i) descriptors[i] = m_indexBuffers[i]->GetSRV();
        const auto descriptorTable = Util::DescriptorTable::MakeUnique();
        descriptorTable->SetDescriptors(0, static_cast<uint32_t>(size(descriptors)), descriptors);
        X_RETURN(m_srvTables[SRV_TABLE_IB], descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false);
    }

    // Vertex buffer SRVs
    {
        Descriptor descriptors[NUM_MESH];
        for (auto i = 0u; i < NUM_MESH; ++i) descriptors[i] = m_vertexBuffers[i]->GetSRV();
        const auto descriptorTable = Util::DescriptorTable::MakeUnique();
        descriptorTable->SetDescriptors(0, static_cast<uint32_t>(size(descriptors)), descriptors);
        X_RETURN(m_srvTables[SRV_TABLE_VB], descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false);
    }

    // Environment texture SRV
    {
        const auto descriptorTable = Util::DescriptorTable::MakeUnique();
        descriptorTable->SetDescriptors(0, 1, &m_lightProbe->GetSRV());
        X_RETURN(m_srvTables[SRV_TABLE_ENV], descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false);
    }

    // Tessellated vertex color SRV
    {
        Descriptor descriptors[NUM_MESH];
        for (auto i = 0u; i < NUM_MESH; ++i) descriptors[i] = m_tessColors[i]->GetSRV();
        const auto descriptorTable = Util::DescriptorTable::MakeUnique();
        descriptorTable->SetDescriptors(0, static_cast<uint32_t>(size(descriptors)), descriptors);
        X_RETURN(m_srvTables[SRV_TABLE_VCOLOR], descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false)
    }

    // Tessellation domain SRV
    {
        Descriptor descriptors[NUM_MESH];
        for (auto i = 0u; i < NUM_MESH; ++i) descriptors[i] = m_tessDoms[i]->GetSRV();
        const auto descriptorTable = Util::DescriptorTable::MakeUnique();
        descriptorTable->SetDescriptors(0, static_cast<uint32_t>(size(descriptors)), descriptors);
        X_RETURN(m_srvTables[SRV_TABLE_TESSDOMS], descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false)
    }

    // Output SRV for tone mapping
    {
        const auto descriptorTable = Util::DescriptorTable::MakeUnique();
        descriptorTable->SetDescriptors(0, 1, &m_outputView->GetSRV());
        X_RETURN(m_srvTables[SRV_TABLE_OUTPUT], descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false);
    }

    // Create the sampler
    {
        const auto descriptorTable = Util::DescriptorTable::MakeUnique();
        const auto samplerAnisoWrap = SamplerPreset::ANISOTROPIC_WRAP;
        descriptorTable->SetSamplers(0, 1, &samplerAnisoWrap, m_descriptorTableCache.get());
        X_RETURN(m_samplerTable, descriptorTable->GetSamplerTable(m_descriptorTableCache.get()), false);
    }

    return true;
}

bool TVRayTracer::buildAccelerationStructures(
    const RayTracing::CommandList* pCommandList,
    GeometryBuffer* pGeometries)
{
    // Set geometries
    VertexBufferView vertexBufferViews[NUM_MESH];
    IndexBufferView indexBufferViews[NUM_MESH];
    for (auto i = 0u; i < NUM_MESH; ++i)
    {
        vertexBufferViews[i] = m_vertexBuffers[i]->GetVBV();
        indexBufferViews[i] = m_indexBuffers[i]->GetIBV();
        BottomLevelAS::SetTriangleGeometries(pGeometries[i], 1, Format::R32G32B32_FLOAT,
            &vertexBufferViews[i], &indexBufferViews[i]);
    }

    // Descriptor index in descriptor pool
    const auto bottomLevelASIndex = 0u;
    const auto topLevelASIndex = bottomLevelASIndex + NUM_MESH;

    // Prebuild
    for (auto i = 0u; i < NUM_MESH; ++i)
    {
        m_bottomLevelASs[i] = BottomLevelAS::MakeUnique();
        N_RETURN(m_bottomLevelASs[i]->PreBuild(m_device.get(), 1, pGeometries[i], bottomLevelASIndex + i, BuildFlag::NONE), false);
    }
    m_topLevelAS = TopLevelAS::MakeUnique();
    N_RETURN(m_topLevelAS->PreBuild(m_device.get(), NUM_MESH, topLevelASIndex,
        BuildFlag::ALLOW_UPDATE), false);

    // Create scratch buffer
    auto scratchSize = m_topLevelAS->GetScratchDataMaxSize();
    for (const auto& bottomLevelAS : m_bottomLevelASs)
        scratchSize = (max)(bottomLevelAS->GetScratchDataMaxSize(), scratchSize);
    m_scratch = Resource::MakeUnique();
    N_RETURN(AccelerationStructure::AllocateUAVBuffer(m_device.get(), m_scratch.get(), scratchSize), false);

    // Get descriptor pool and create descriptor tables
    N_RETURN(createDescriptorTables(), false);
    const auto& descriptorPool = m_descriptorTableCache->GetDescriptorPool(CBV_SRV_UAV_POOL);

    // Set instance
    XMFLOAT3X4 matrices[NUM_MESH];
    XMStoreFloat3x4(&matrices[GROUND], (XMMatrixScaling(8.0f, 0.5f, 8.0f) * XMMatrixTranslation(0.0f, -0.5f, 0.0f)));
    XMStoreFloat3x4(&matrices[MODEL_OBJ], (XMMatrixScaling(m_posScale.w, m_posScale.w, m_posScale.w) *
        XMMatrixTranslation(m_posScale.x, m_posScale.y, m_posScale.z)));
    float* const transforms[] =
    {
        reinterpret_cast<float*>(&matrices[GROUND]),
        reinterpret_cast<float*>(&matrices[MODEL_OBJ])
    };
    for (auto& instances : m_instances) instances = Resource::MakeUnique();
    auto& instances = m_instances[FrameCount - 1];
    const BottomLevelAS* pBottomLevelASs[NUM_MESH];
    for (auto i = 0u; i < NUM_MESH; ++i) pBottomLevelASs[i] = m_bottomLevelASs[i].get();
    TopLevelAS::SetInstances(m_device.get(), instances.get(), NUM_MESH, pBottomLevelASs, transforms);

    // Build bottom level ASs
    for (auto& bottomLevelAS : m_bottomLevelASs)
        bottomLevelAS->Build(pCommandList, m_scratch.get(), descriptorPool);

    // Build top level AS
    m_topLevelAS->Build(pCommandList, m_scratch.get(), instances.get(), descriptorPool);

    return true;
}

bool TVRayTracer::buildShaderTables()
{
    // Get shader identifiers.
    const auto shaderIDSize = ShaderRecord::GetShaderIDSize(m_device.get());
    const auto cbRayGen = RayGenConstants();

    // Raytracing shader tables
    for (uint8_t i = 0; i < FrameCount; ++i)
    {
        // Ray gen shader table
        m_rayGenShaderTables[i] = ShaderTable::MakeUnique();
        N_RETURN(m_rayGenShaderTables[i]->Create(m_device.get(), 1, shaderIDSize + sizeof(RayGenConstants),
            (L"RayGenShaderTable" + to_wstring(i)).c_str()), false);
        N_RETURN(m_rayGenShaderTables[i]->AddShaderRecord(ShaderRecord::MakeUnique(m_device.get(),
            m_pipelines[RAY_TRACING], RaygenShaderName, &cbRayGen, sizeof(RayGenConstants)).get()), false);
    }

    // Hit group shader table
    m_hitGroupShaderTable = ShaderTable::MakeUnique();
    N_RETURN(m_hitGroupShaderTable->Create(m_device.get(), 1, shaderIDSize, L"HitGroupShaderTable"), false);
    N_RETURN(m_hitGroupShaderTable->AddShaderRecord(ShaderRecord::MakeUnique(m_device.get(),
        m_pipelines[RAY_TRACING], HitGroupNames[HIT_GROUP_RADIANCE], &cbRayGen, sizeof(RayGenConstants)).get()), false);

    // Miss shader table
    m_missShaderTable = ShaderTable::MakeUnique();
    N_RETURN(m_missShaderTable->Create(m_device.get(), 1, shaderIDSize, L"MissShaderTable"), false);
    N_RETURN(m_missShaderTable->AddShaderRecord(ShaderRecord::MakeUnique(m_device.get(),
        m_pipelines[RAY_TRACING], MissShaderNames[HIT_GROUP_RADIANCE]).get()), false);
    N_RETURN(m_missShaderTable->AddShaderRecord(ShaderRecord::MakeUnique(m_device.get(),
        m_pipelines[RAY_TRACING], MissShaderNames[HIT_GROUP_SHADOW]).get()), false);

    return true;
}

void TVRayTracer::zPrepass(
    const XUSG::CommandList* pCommandList,
    uint8_t                  frameIndex)
{
    // Set depth barrier to write
    ResourceBarrier barrier;
    const auto numBarriers = m_depth->SetBarrier(&barrier, ResourceState::DEPTH_WRITE);
    pCommandList->Barrier(numBarriers, &barrier);

    // Clear depth
    pCommandList->OMSetRenderTargets(0, nullptr, &m_depth->GetDSV());
    pCommandList->ClearDepthStencilView(m_depth->GetDSV(), ClearFlag::DEPTH, 1.0f);

    // Set pipeline state
    pCommandList->SetGraphicsPipelineLayout(m_pipelineLayouts[Z_PRE_LAYOUT]);
    pCommandList->SetPipelineState(m_pipelines[Z_PREPASS]);

    // Set viewport
    Viewport viewport(0.0f, 0.0f, static_cast<float>(m_viewport.x), static_cast<float>(m_viewport.y));
    RectRange scissorRect(0, 0, m_viewport.x, m_viewport.y);
    pCommandList->RSSetViewports(1, &viewport);
    pCommandList->RSSetScissorRects(1, &scissorRect);

    // Record commands.
    pCommandList->IASetPrimitiveTopology(PrimitiveTopology::CONTROL_POINT3_PATCHLIST);

    for (auto i = 0u; i < NUM_MESH; ++i)
    {
        // Set descriptor tables
        pCommandList->SetGraphics32BitConstant(0, m_tessFactor);
        pCommandList->SetGraphicsRootConstantBufferView(1, m_cbGraphics[i].get(), m_cbGraphics[i]->GetCBVOffset(frameIndex));
        pCommandList->IASetVertexBuffers(0, 1, &m_vertexBuffers[i]->GetVBV());
        pCommandList->IASetIndexBuffer(m_indexBuffers[i]->GetIBV());
        pCommandList->DrawIndexed(m_numIndices[i], 1, 0, 0, 0);
    }
}

void TVRayTracer::envPrepass(
    const XUSG::CommandList* pCommandList,
    uint8_t                  frameIndex)
{
    pCommandList->OMSetRenderTargets(0, nullptr);
    pCommandList->SetGraphicsPipelineLayout(m_pipelineLayouts[ENV_PRE_LAYOUT]);
    pCommandList->SetPipelineState(m_pipelines[ENV_PREPASS]);

    pCommandList->SetGraphicsRootConstantBufferView(0, m_cbEnv.get(), m_cbEnv->GetCBVOffset(frameIndex));
    pCommandList->SetGraphicsDescriptorTable(1, m_uavTables[UAV_TABLE_OUTPUT]);
    pCommandList->SetGraphicsDescriptorTable(2, m_srvTables[SRV_TABLE_ENV]);
    pCommandList->SetGraphicsDescriptorTable(3, m_samplerTable);

    Viewport viewport(0.0f, 0.0f, static_cast<float>(m_viewport.x), static_cast<float>(m_viewport.y));
    RectRange scissorRect(0, 0, m_viewport.x, m_viewport.y);
    pCommandList->RSSetViewports(1, &viewport);
    pCommandList->RSSetScissorRects(1, &scissorRect);

    pCommandList->IASetPrimitiveTopology(PrimitiveTopology::TRIANGLELIST);
    pCommandList->Draw(3, 1, 0, 0);
}

void TVRayTracer::tessellate(
    const XUSG::CommandList* pCommandList,
    uint8_t                  frameIndex)
{    
    pCommandList->OMSetRenderTargets(0, nullptr);
    pCommandList->SetGraphicsPipelineLayout(m_pipelineLayouts[TESSELLATION_LAYOUT]);
    pCommandList->SetPipelineState(m_pipelines[TESSELLATION]);
    
    pCommandList->SetGraphicsDescriptorTable(1, m_uavTables[UAV_TABLE_TESSDOMS]);

    Viewport viewport(0.0f, 0.0f, static_cast<float>(m_viewport.x), static_cast<float>(m_viewport.y));
    RectRange scissorRect(0, 0, m_viewport.x, m_viewport.y);
    pCommandList->RSSetViewports(1, &viewport);
    pCommandList->RSSetScissorRects(1, &scissorRect);

    pCommandList->IASetPrimitiveTopology(PrimitiveTopology::CONTROL_POINT3_PATCHLIST);

    for (auto i = 0u; i < NUM_MESH; ++i)
    {
        CBTessellation tessConsts = { i, m_tessFactor, m_maxVertPerPatch };
        pCommandList->SetGraphics32BitConstants(0, SizeOfInUint32(tessConsts), &tessConsts);
        pCommandList->IASetVertexBuffers(0, 1, &m_vertexBuffers[i]->GetVBV());
        pCommandList->IASetIndexBuffer(m_indexBuffers[i]->GetIBV());
        pCommandList->DrawIndexed(m_numIndices[i], 1, 0, 0, 0);
    }
}

void TVRayTracer::raytrace(
    const RayTracing::CommandList* pCommandList,
    uint8_t                        frameIndex)
{
    ResourceBarrier barriers[2];
    auto numBarriers = 0u;
    for (auto i = 0u; i < NUM_MESH; ++i)
    {
        numBarriers = m_tessDoms[i]->SetBarrier(barriers, ResourceState::UNORDERED_ACCESS, numBarriers);
    }
    pCommandList->Barrier(numBarriers, barriers);

    // Bind the acceleration structure and dispatch rays.
    pCommandList->SetComputePipelineLayout(m_pipelineLayouts[RT_GLOBAL_LAYOUT]);
    pCommandList->SetComputeRootConstantBufferView(MATERIALS, m_cbMaterials.get());
    pCommandList->SetComputeRootConstantBufferView(CONSTANTS, m_cbGlobal.get(), m_cbGlobal->GetCBVOffset(frameIndex));
    pCommandList->SetTopLevelAccelerationStructure(ACCELERATION_STRUCTURE, m_topLevelAS.get());
    pCommandList->SetComputeDescriptorTable(INDEX_BUFFERS, m_srvTables[SRV_TABLE_IB]);
    pCommandList->SetComputeDescriptorTable(VERTEX_BUFFERS, m_srvTables[SRV_TABLE_VB]);
    pCommandList->SetComputeDescriptorTable(ENV_TEXTURE, m_srvTables[SRV_TABLE_ENV]);
    pCommandList->SetComputeDescriptorTable(SAMPLER, m_samplerTable);
    pCommandList->SetComputeDescriptorTable(VERTEX_COLOR, m_uavTables[UAV_TABLE_RT]);
    pCommandList->SetComputeDescriptorTable(TESS_DOMS, m_srvTables[SRV_TABLE_TESSDOMS]);

    for (auto i = 0u; i < NUM_MESH; ++i)
    {
        CBTessellation tessConsts = { i, m_tessFactor, m_maxVertPerPatch };
        pCommandList->SetCompute32BitConstants(TESS_CONSTS, SizeOfInUint32(tessConsts), &tessConsts);
        // Fallback layer has no depth
        pCommandList->DispatchRays(m_pipelines[RAY_TRACING], m_numMaxTessVerts[i], 1, 1,
            m_hitGroupShaderTable.get(), m_missShaderTable.get(), m_rayGenShaderTables[frameIndex].get());
    }
}

void TVRayTracer::rasterize(
    const XUSG::CommandList* pCommandList,
    uint8_t                  frameIndex)
{
    ResourceBarrier barrier;
    const auto depthState = ResourceState::DEPTH_READ | ResourceState::NON_PIXEL_SHADER_RESOURCE;
    auto numBarriers = m_depth->SetBarrier(&barrier, depthState);
    pCommandList->Barrier(numBarriers, &barrier);

    pCommandList->OMSetRenderTargets(0, nullptr, &m_depth->GetDSV());

    pCommandList->SetGraphicsPipelineLayout(m_pipelineLayouts[GRAPHICS_LAYOUT]);
    pCommandList->SetPipelineState(m_pipelines[GRAPHICS]);

    pCommandList->SetGraphicsDescriptorTable(2, m_srvTables[SRV_TABLE_VCOLOR]);
    pCommandList->SetGraphicsDescriptorTable(3, m_uavTables[UAV_TABLE_OUTPUT]);

    Viewport viewport(0.0f, 0.0f, static_cast<float>(m_viewport.x), static_cast<float>(m_viewport.y));
    RectRange scissorRect(0, 0, m_viewport.x, m_viewport.y);
    pCommandList->RSSetViewports(1, &viewport);
    pCommandList->RSSetScissorRects(1, &scissorRect);

    pCommandList->IASetPrimitiveTopology(PrimitiveTopology::CONTROL_POINT3_PATCHLIST);

    for (auto i = 0u; i < NUM_MESH; ++i)
    {
        CBTessellation tessConsts = { i, m_tessFactor, m_maxVertPerPatch };
        pCommandList->SetGraphics32BitConstants(0, SizeOfInUint32(tessConsts), &tessConsts);
        pCommandList->SetGraphicsRootConstantBufferView(1, m_cbGraphics[i].get(), m_cbGraphics[i]->GetCBVOffset(frameIndex));
        pCommandList->IASetVertexBuffers(0, 1, &m_vertexBuffers[i]->GetVBV());
        pCommandList->IASetIndexBuffer(m_indexBuffers[i]->GetIBV());
        pCommandList->DrawIndexed(m_numIndices[i], 1, 0, 0, 0);
    }
}

void TVRayTracer::toneMap(
    const XUSG::CommandList* pCommandList,
    const Descriptor&        rtv,
    uint32_t                 numBarriers,
    ResourceBarrier*         pBarriers)
{
    pCommandList->Barrier(numBarriers, pBarriers);

    ResourceBarrier barrier;
    numBarriers = m_outputView->SetBarrier(&barrier, ResourceState::UNORDERED_ACCESS, 0);
    pCommandList->Barrier(numBarriers, &barrier);

    pCommandList->OMSetRenderTargets(1, &rtv);

    pCommandList->SetGraphicsPipelineLayout(m_pipelineLayouts[TONEMAP_LAYOUT]);
    pCommandList->SetGraphicsDescriptorTable(0, m_srvTables[SRV_TABLE_OUTPUT]);

    pCommandList->SetPipelineState(m_pipelines[TONEMAP]);

    Viewport viewport(0.0f, 0.0f, static_cast<float>(m_viewport.x), static_cast<float>(m_viewport.y));
    RectRange scissorRect(0, 0, m_viewport.x, m_viewport.y);
    pCommandList->RSSetViewports(1, &viewport);
    pCommandList->RSSetScissorRects(1, &scissorRect);

    pCommandList->IASetPrimitiveTopology(PrimitiveTopology::TRIANGLELIST);
    pCommandList->Draw(3, 1, 0, 0);
}
