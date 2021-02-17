#include "GraphicsImpl.h"

#include <JsRuntimeInternalState.h>

#include <cassert>

#if (ANDROID)
// MSAA is disabled on Android.
// See issue https://github.com/BabylonJS/BabylonNative/issues/494#issuecomment-731135918
// for explanation
#define BGFX_RESET_FLAGS (BGFX_RESET_VSYNC | BGFX_RESET_MAXANISOTROPY)
#elif __APPLE__
// On Apple devices we need to set BGFX_RESET_FLIP_AFTER_RENDER so that draw calls are immediately pushed
// to the Metal command buffer to avoid a one frame delay.
#define BGFX_RESET_FLAGS (BGFX_RESET_VSYNC | BGFX_RESET_MSAA_X4 | BGFX_RESET_MAXANISOTROPY | BGFX_RESET_FLIP_AFTER_RENDER)
#else
#define BGFX_RESET_FLAGS (BGFX_RESET_VSYNC | BGFX_RESET_MSAA_X4 | BGFX_RESET_MAXANISOTROPY)
#endif

namespace
{
    constexpr auto JS_GRAPHICS_NAME = "_Graphics";

    bool g_bgfxRenderFrameCalled{false};
}

namespace Babylon
{
    Graphics::Impl::UpdateToken::UpdateToken(Graphics::Impl& graphicsImpl)
        : m_graphicsImpl(graphicsImpl)
        , m_guarantee{m_graphicsImpl.m_safeTimespanGuarantor->GetSafetyGuarantee()}
    {
    }

    bgfx::Encoder* Graphics::Impl::UpdateToken::GetEncoder()
    {
        return m_graphicsImpl.GetEncoderForThread();
    }

    FrameBuffer& Graphics::Impl::UpdateToken::AddFrameBuffer(
        bgfx::FrameBufferHandle handle,
        uint16_t width,
        uint16_t height,
        bool backBuffer)
    {
        return m_graphicsImpl.m_frameBufferManager->AddFrameBuffer(handle, width, height, backBuffer);
    }

    void Graphics::Impl::UpdateToken::RemoveFrameBuffer(const FrameBuffer& frameBuffer)
    {
        m_graphicsImpl.m_frameBufferManager->RemoveFrameBuffer(frameBuffer);
    }

    FrameBuffer& Graphics::Impl::UpdateToken::DefaultFrameBuffer()
    {
        return m_graphicsImpl.m_frameBufferManager->DefaultFrameBuffer();
    }

    FrameBuffer& Graphics::Impl::UpdateToken::BoundFrameBuffer()
    {
        return m_graphicsImpl.m_frameBufferManager->BoundFrameBuffer();
    }

    Graphics::Impl::Impl()
        : m_bgfxCallback{[this](const auto& data) { CaptureCallback(data); }}
    {
        std::scoped_lock lock{m_state.Mutex};
        m_state.Bgfx.Initialized = false;

        auto& init = m_state.Bgfx.InitState;
#if (ANDROID)
        init.type = bgfx::RendererType::OpenGLES;
#else
        init.type = bgfx::RendererType::Direct3D11;
#endif
        init.resolution.reset = BGFX_RESET_FLAGS;
        init.callback = &m_bgfxCallback;
    }

    Graphics::Impl::~Impl()
    {
        DisableRendering();
    }

    void* Graphics::Impl::GetNativeWindow()
    {
        std::scoped_lock lock{m_state.Mutex};
        return m_state.Bgfx.InitState.platformData.nwh;
    }

    void Graphics::Impl::SetNativeWindow(void* nativeWindowPtr, void* windowTypePtr)
    {
        std::scoped_lock lock{m_state.Mutex};
        m_state.Bgfx.Dirty = true;

        auto& pd = m_state.Bgfx.InitState.platformData;
        pd.ndt = windowTypePtr;
        pd.nwh = nativeWindowPtr;
        pd.context = nullptr;
        pd.backBuffer = nullptr;
        pd.backBufferDS = nullptr;
    }

    void Graphics::Impl::Resize(size_t width, size_t height)
    {
        std::scoped_lock lock{m_state.Mutex};
        m_state.Resolution.Width = width;
        m_state.Resolution.Height = height;
        UpdateBgfxResolution();
    }

    void Graphics::Impl::AddToJavaScript(Napi::Env env)
    {
        JsRuntime::NativeObject::GetFromJavaScript(env)
            .Set(JS_GRAPHICS_NAME, Napi::External<Impl>::New(env, this));
    }

    Graphics::Impl& Graphics::Impl::GetFromJavaScript(Napi::Env env)
    {
        return *JsRuntime::NativeObject::GetFromJavaScript(env)
                    .Get(JS_GRAPHICS_NAME)
                    .As<Napi::External<Graphics::Impl>>()
                    .Data();
    }

    Graphics::Impl::RenderScheduler& Graphics::Impl::BeforeRenderScheduler()
    {
        TrySignalUpdateStarted();
        return m_beforeRenderScheduler;
    }

    Graphics::Impl::RenderScheduler& Graphics::Impl::AfterRenderScheduler()
    {
        return m_afterRenderScheduler;
    }

    void Graphics::Impl::EnableRendering()
    {
        // Set the thread affinity (all other rendering operations must happen on this thread).
        m_renderThreadAffinity = std::this_thread::get_id();

        if (!g_bgfxRenderFrameCalled)
        {
            bgfx::renderFrame();
            g_bgfxRenderFrameCalled = true;
        }

        std::scoped_lock lock{m_state.Mutex};

        if (!m_state.Bgfx.Initialized)
        {
            // Initialize bgfx.
            auto& init{m_state.Bgfx.InitState};
            bgfx::setPlatformData(init.platformData);
            bgfx::init(init);

            m_state.Bgfx.Initialized = true;
            m_state.Bgfx.Dirty = false;

            m_frameBufferManager = std::make_unique<FrameBufferManager>();
        }

        m_safeTimespanGuarantor = std::make_unique<SafeTimespanGuarantor>();
    }

    void Graphics::Impl::DisableRendering()
    {
        assert(m_renderThreadAffinity.check());

        std::scoped_lock lock{m_state.Mutex};

        if (m_state.Bgfx.Initialized)
        {
            m_cancellationSource.cancel();

            m_frameBufferManager.reset();

            bgfx::shutdown();
            m_state.Bgfx.Initialized = false;

            m_renderThreadAffinity = {};
        }

        m_safeTimespanGuarantor.reset();
    }

    arcana::ticketed_collection<std::function<void()>>::ticket Graphics::Impl::AddUpdateStartedCallback(std::function<void()> callback)
    {
        return m_updateStartedCallbacks.insert(std::move(callback), m_updateStartedCallbacksMutex);
    }

    void Graphics::Impl::WaitForUpdateStarted()
    {
        std::unique_lock lock{m_updateStartedMutex};
        if (!m_updateStarted)
        {
            m_updateStartedConditionVariable.wait(lock);
        }
    }

    void Graphics::Impl::StartRenderingCurrentFrame()
    {
        assert(m_renderThreadAffinity.check());

        // Ensure rendering is enabled.
        EnableRendering();

        // Update bgfx state if necessary.
        UpdateBgfxState();

        {
            std::scoped_lock lock{m_updateStartedMutex};
            m_updateStarted = false;
        }

        m_safeTimespanGuarantor->BeginSafeTimespan();

        m_beforeRenderScheduler.m_dispatcher.tick(m_cancellationSource);
    }

    bool Graphics::Impl::TryFinishRenderingCurrentFrame(uint32_t milliseconds)
    {
        assert(m_renderThreadAffinity.check());

        if (!m_safeTimespanGuarantor->TryEndSafeTimespan(milliseconds))
        {
            return false;
        }

        Frame();

        m_afterRenderScheduler.m_dispatcher.tick(m_cancellationSource);

        return true;
    }

    Graphics::Impl::UpdateToken Graphics::Impl::GetUpdateToken()
    {
        Graphics::Impl::UpdateToken token{*this};

        TrySignalUpdateStarted();

        return token;
    }

    void Graphics::Impl::SetDiagnosticOutput(std::function<void(const char* output)> diagnosticOutput)
    {
        assert(m_renderThreadAffinity.check());
        m_bgfxCallback.SetDiagnosticOutput(std::move(diagnosticOutput));
    }

    void Graphics::Impl::RequestScreenShot(std::function<void(std::vector<uint8_t>)> callback)
    {
        assert(m_renderThreadAffinity.check());
        m_bgfxCallback.AddScreenShotCallback(callback);
        bgfx::requestScreenShot(BGFX_INVALID_HANDLE, "Graphics::Impl::RequestScreenShot");
    }

    float Graphics::Impl::GetHardwareScalingLevel()
    {
        std::scoped_lock lock{m_state.Mutex};
        return m_state.Resolution.HardwareScalingLevel;
    }

    void Graphics::Impl::SetHardwareScalingLevel(float level)
    {
        if (level <= std::numeric_limits<float>::epsilon())
        {
            throw std::runtime_error{"HardwareScalingValue cannot be less than or equal to 0."};
        }
        {
            std::scoped_lock lock{m_state.Mutex};
            m_state.Resolution.HardwareScalingLevel = level;
        }

        UpdateBgfxResolution();
    }

    Graphics::Impl::CaptureCallbackTicketT Graphics::Impl::AddCaptureCallback(std::function<void(const BgfxCallback::CaptureData&)> callback)
    {
        // If we're not already capturing, start.
        {
            std::scoped_lock lock{m_state.Mutex};
            if ((m_state.Bgfx.InitState.resolution.reset & BGFX_RESET_CAPTURE) == 0)
            {
                m_state.Bgfx.Dirty = true;
                m_state.Bgfx.InitState.resolution.reset |= BGFX_RESET_CAPTURE;
            }
        }

        return m_captureCallbacks.insert(std::move(callback), m_captureCallbacksMutex);
    }

    void Graphics::Impl::UpdateBgfxState()
    {
        std::scoped_lock lock{m_state.Mutex};
        if (m_state.Bgfx.Dirty)
        {
            bgfx::setPlatformData(m_state.Bgfx.InitState.platformData);

            // Ensure bgfx rebinds all texture information.
            bgfx::discard(BGFX_DISCARD_ALL);

            auto& res = m_state.Bgfx.InitState.resolution;
            bgfx::reset(res.width, res.height, BGFX_RESET_FLAGS);
            bgfx::setViewRect(0, 0, 0, static_cast<uint16_t>(res.width), static_cast<uint16_t>(res.height));

            m_state.Bgfx.Dirty = false;
        }
    }

    void Graphics::Impl::UpdateBgfxResolution()
    {
        std::scoped_lock lock{m_state.Mutex};
        m_state.Bgfx.Dirty = true;
        auto& res = m_state.Bgfx.InitState.resolution;
        auto level = m_state.Resolution.HardwareScalingLevel;
        res.width = static_cast<uint32_t>(m_state.Resolution.Width / level);
        res.height = static_cast<uint32_t>(m_state.Resolution.Height / level);
    }

    void Graphics::Impl::DiscardIfDirty()
    {
        std::scoped_lock lock{m_state.Mutex};
        if (m_state.Bgfx.Dirty)
        {
            bgfx::discard();
        }
    }

    void Graphics::Impl::Frame()
    {
        // Automatically end bgfx encoders.
        EndEncoders();

        // Discard everything if the bgfx state is dirty.
        DiscardIfDirty();

        // Advance frame and render!
        bgfx::frame();

        // Reset the frame buffers.
        m_frameBufferManager->Reset();
    }

    bgfx::Encoder* Graphics::Impl::GetEncoderForThread()
    {
        assert(!m_renderThreadAffinity.check());
        std::scoped_lock lock{m_threadIdToEncoderMutex};

        const auto threadId{std::this_thread::get_id()};
        auto it{m_threadIdToEncoder.find(threadId)};
        if (it == m_threadIdToEncoder.end())
        {
            bgfx::Encoder* encoder{bgfx::begin(true)};
            it = m_threadIdToEncoder.emplace(threadId, encoder).first;
        }

        return it->second;
    }

    void Graphics::Impl::EndEncoders()
    {
        std::scoped_lock lock{m_threadIdToEncoderMutex};

        for (auto [threadId, encoder] : m_threadIdToEncoder)
        {
            bgfx::end(encoder);
        }

        m_threadIdToEncoder.clear();
    }

    void Graphics::Impl::CaptureCallback(const BgfxCallback::CaptureData& data)
    {
        std::scoped_lock callbackLock{m_captureCallbacksMutex};

        // If no one is listening anymore, stop capturing.
        if (m_captureCallbacks.empty())
        {
            std::scoped_lock stateLock{m_state.Mutex};
            m_state.Bgfx.Dirty = true;
            m_state.Bgfx.InitState.resolution.reset &= ~BGFX_RESET_CAPTURE;
            return;
        }

        for (const auto& callback : m_captureCallbacks)
        {
            callback(data);
        }
    }

    bool Graphics::Impl::TrySignalUpdateStarted()
    {
        std::scoped_lock updateStartedLock{m_updateStartedMutex};
        if (m_updateStarted)
        {
            return false;
        }

        {
            std::scoped_lock updateStartedCallbacksLock{m_updateStartedCallbacksMutex};
            for (const auto& callback : m_updateStartedCallbacks)
            {
                callback();
            }
        }

        m_updateStarted = true;
        m_updateStartedConditionVariable.notify_all();

        return true;
    }
}
