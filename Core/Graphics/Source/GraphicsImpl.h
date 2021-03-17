#pragma once

#include <Babylon/Graphics.h>
#include "BgfxCallback.h"
#include "FrameBufferManager.h"
#include "SafeTimespanGuarantor.h"

#include <arcana/containers/ticketed_collection.h>
#include <arcana/threading/blocking_concurrent_queue.h>
#include <arcana/threading/dispatcher.h>
#include <arcana/threading/task.h>
#include <arcana/threading/affinity.h>

#include <bgfx/bgfx.h>
#include <bgfx/platform.h>

#include <memory>
#include <map>

namespace Babylon
{
    class Graphics::Impl
    {
    public:
        class UpdateToken final
        {
        public:
            UpdateToken(const UpdateToken& other) = delete;
            UpdateToken(UpdateToken&&) = default;

            bgfx::Encoder* GetEncoder();

        private:
            friend class Graphics::Impl;

            UpdateToken(Graphics::Impl&, SafeTimespanGuarantor::SafetyGuarantee);

            Impl& m_graphicsImpl;
            SafeTimespanGuarantor::SafetyGuarantee m_guarantee;
        };

        class RenderScheduler final
        {
        public:
            RenderScheduler(std::function<void()> workScheduledCallback)
                : m_workScheduledCallback{workScheduledCallback}
            {
            }

            template<typename CallableT>
            void operator()(CallableT&& callable)
            {
                m_dispatcher(callable);
                m_workScheduledCallback();
            }

        private:
            friend Impl;

            arcana::manual_dispatcher<128> m_dispatcher{};
            std::function<void()> m_workScheduledCallback{};
        };

        class AutoRenderThread
        {
        public:
            AutoRenderThread(Graphics::Impl&);
            ~AutoRenderThread();

        private:
            Graphics::Impl& m_graphicsImpl;
            std::thread m_thread{};
            arcana::cancellation_source m_cancelSource{};

            void Run();
        };

        Impl();
        ~Impl();

        void* GetNativeWindow();
        void SetNativeWindow(void* nativeWindowPtr, void* windowTypePtr);
        void Resize(size_t width, size_t height);

        void AddToJavaScript(Napi::Env);
        static Impl& GetFromJavaScript(Napi::Env);

        RenderScheduler& BeforeRenderScheduler();
        RenderScheduler& AfterRenderScheduler();

        void EnableRendering();
        void DisableRendering();

        void StartRenderingCurrentFrame();
        void FinishRenderingCurrentFrame();

        using RequestNextFrameCallbackTicketT = arcana::ticketed_collection<std::function<void()>>::ticket;
        RequestNextFrameCallbackTicketT AddRequestNextFrameCallback(std::function<void()>);

        void StartAutoRendering();
        void StopAutoRendering();

        UpdateToken GetUpdateToken();

        FrameBuffer& AddFrameBuffer(bgfx::FrameBufferHandle handle, uint16_t width, uint16_t height, bool backBuffer);
        void RemoveFrameBuffer(const FrameBuffer& frameBuffer);
        FrameBuffer& DefaultFrameBuffer();

        void SetDiagnosticOutput(std::function<void(const char* output)> diagnosticOutput);

        void RequestScreenShot(std::function<void(std::vector<uint8_t>)> callback);

        float GetHardwareScalingLevel();
        void SetHardwareScalingLevel(float level);

        using CaptureCallbackTicketT = arcana::ticketed_collection<std::function<void(const BgfxCallback::CaptureData&)>>::ticket;
        CaptureCallbackTicketT AddCaptureCallback(std::function<void(const BgfxCallback::CaptureData&)> callback);

    private:
        friend class UpdateToken;

        void UpdateBgfxState();
        void UpdateBgfxResolution();
        void DiscardIfDirty();
        void RequestScreenShots();
        void Frame();
        bgfx::Encoder* GetEncoderForThread();
        void EndEncoders();
        void CaptureCallback(const BgfxCallback::CaptureData&);

        void BeforeRenderWorkScheduled();
        void AfterRenderWorkScheduled();
        void RequestNextFrame();

        arcana::affinity m_renderThreadAffinity{};
        std::atomic_bool m_rendering{};

        std::unique_ptr<arcana::cancellation_source> m_cancellationSource{};

        struct
        {
            std::recursive_mutex Mutex{};

            struct
            {
                bgfx::Init InitState{};
                bool Initialized{};
                bool Dirty{};
            } Bgfx{};

            struct
            {
                size_t Width{};
                size_t Height{};
                float HardwareScalingLevel{1.0f};
            } Resolution{};
        } m_state;

        BgfxCallback m_bgfxCallback;

        SafeTimespanGuarantor m_safeTimespanGuarantor{};

        RenderScheduler m_beforeRenderScheduler;
        RenderScheduler m_afterRenderScheduler;

        std::unique_ptr<FrameBufferManager> m_frameBufferManager{};

        std::mutex m_captureCallbacksMutex{};
        arcana::ticketed_collection<std::function<void(const BgfxCallback::CaptureData&)>> m_captureCallbacks{};

        arcana::blocking_concurrent_queue<std::function<void(std::vector<uint8_t>)>> m_screenShotCallbacks{};

        std::map<std::thread::id, bgfx::Encoder*> m_threadIdToEncoder{};
        std::mutex m_threadIdToEncoderMutex{};

        bool m_nextFrameRequested{false};
        std::mutex m_nextFrameRequestedMutex{};

        arcana::ticketed_collection<std::function<void()>> m_nextFrameRequestCallbacks{};
        std::mutex m_nextFrameRequestCallbacksMutex{};

        std::optional<AutoRenderThread> m_autoRenderThread{};
    };
}
