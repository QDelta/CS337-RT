//*********************************************************
//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//
//*********************************************************

#pragma once

#include "DXFramework.h"
#include "StepTimer.h"
#include "RayTracerSelection.h"

using namespace DirectX;

// Note that while ComPtr is used to manage the lifetime of resources on the CPU,
// it has no understanding of the lifetime of resources on the GPU. Apps must account
// for the GPU lifetime of resources to avoid destroying objects that may still be
// referenced by the GPU.
// An example of this can be found in the class method: OnDestroy().

class RTGranularity : public DXFramework
{
public:
    RTGranularity(uint32_t width, uint32_t height, std::wstring name);
    virtual ~RTGranularity();

    virtual void OnInit();
    virtual void OnUpdate();
    virtual void OnRender();
    virtual void OnDestroy();

    virtual void OnKeyUp(uint8_t key);
    virtual void OnLButtonDown(float posX, float posY);
    virtual void OnLButtonUp(float posX, float posY);
    virtual void OnMouseMove(float posX, float posY);
    virtual void OnMouseWheel(float deltaZ, float posX, float posY);
    virtual void OnMouseLeave();

    virtual void ParseCommandLineArgs(wchar_t* argv[], int argc);

private:
    enum CommandType : uint8_t
    {
        UNIVERSAL,
        COMPUTE,

        COMMAND_TYPE_COUNT
    };

    enum CommandAllocatorIndex : uint8_t
    {
        ALLOCATOR_GEOMETRY,
        ALLOCATOR_GRAPHICS,
        ALLOCATOR_COMPUTE,

        COMMAND_ALLOCATOR_COUNT
    };

    static const auto FrameCount = RayTracer::FrameCount;
    static const auto MinTessFactor = RayTracer::MinTessFactor;
    static const auto MaxTessFactor = RayTracer::MaxTessFactor;

    // Pipeline objects.
    XUSG::Viewport                  m_viewport;
    XUSG::RectRange                 m_scissorRect;

    XUSG::SwapChain::uptr           m_swapChain;
    XUSG::CommandAllocator::uptr    m_commandAllocators[COMMAND_ALLOCATOR_COUNT][FrameCount];
    XUSG::CommandQueue::uptr        m_commandQueues[COMMAND_TYPE_COUNT];

    bool m_isDxrSupported;

    XUSG::RayTracing::Device::sptr      m_device;
    XUSG::RenderTarget::uptr            m_renderTargets[FrameCount];
    XUSG::RayTracing::CommandList::uptr m_commandLists[COMMAND_TYPE_COUNT];

    // App resources.
    std::unique_ptr<RayTracer>  m_rayTracer;
    XMFLOAT4X4                  m_proj;
    XMFLOAT4X4                  m_view;
    XMFLOAT3                    m_focusPt;
    XMFLOAT3                    m_eyePt;
    uint32_t                    m_tessFactor;

    // Synchronization objects.
    uint8_t                     m_frameIndex;
    HANDLE                      m_fenceEvent;
    XUSG::Fence::uptr           m_fence;
    uint64_t                    m_fenceValues[FrameCount];

    XUSG::Semaphore             m_semaphore;

    // Application state
    bool        m_isPaused;
    StepTimer   m_timer;

    // User camera interactions
    bool        m_tracking;
    XMFLOAT2    m_mousePt;

    // User external settings
    std::wstring    m_envFileName;
    std::string     m_meshFileName;
    XMFLOAT4        m_meshPosScale;

    void LoadPipeline();
    void LoadAssets();
    void PopulateCommandList();
    void WaitForGpu();
    void MoveToNextFrame();
    double CalculateFrameStats(float* fTimeStep = nullptr);

    // Ray tracing
    void EnableDirectXRaytracing(IDXGIAdapter1* adapter);;
};
