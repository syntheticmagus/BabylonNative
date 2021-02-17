#pragma once

#include <Babylon/JsRuntime.h>

#include <memory>

namespace Babylon
{
    class Graphics
    {
    public:
        class Impl;

        ~Graphics();

        template<typename... Ts>
        static std::unique_ptr<Graphics> CreateGraphics(Ts...);

        template<typename... Ts>
        void UpdateWindow(Ts...);

        void UpdateSize(size_t width, size_t height);

        void AddToJavaScript(Napi::Env);

        void EnableRendering();
        void DisableRendering();

        class CallbackToken;
        std::unique_ptr<CallbackToken> AddUpdateStartedCallback(std::function<void()> callback);
        void WaitForUpdateStarted();

        void StartRenderingCurrentFrame();
        bool TryFinishRenderingCurrentFrame(int32_t milliseconds = -1);

        void SetDiagnosticOutput(std::function<void(const char* output)> outputFunction);

        float GetHardwareScalingLevel();
        void SetHardwareScalingLevel(float level);

    private:
        Graphics();

        Graphics(const Graphics&) = delete;
        Graphics(Graphics&&) = delete;

        std::unique_ptr<Impl> m_impl{};
    };

    class GraphicsThread
    {
    public:
        template<typename... Ts>
        GraphicsThread(Ts... args)
            : GraphicsThread(Graphics::CreateGraphics(args...))
        {
        }

        GraphicsThread(std::unique_ptr<Graphics> graphics)
            : m_graphics{std::move(graphics)}
            , m_continue{true}
            , m_thread{[this]() { Run(); }}
        {
        }

        ~GraphicsThread()
        {
            m_continue = false;
            m_thread.join();
        }

        Graphics& GetGraphics()
        {
            return *m_graphics;
        }

    private:
        std::unique_ptr<Graphics> m_graphics;
        std::atomic<bool> m_continue{};
        std::thread m_thread{};

        void Run()
        {
            m_graphics->EnableRendering();

            while (m_continue)
            {
                m_graphics->StartRenderingCurrentFrame();
                m_graphics->WaitForUpdateStarted();
                m_graphics->TryFinishRenderingCurrentFrame();
            }

            m_graphics->DisableRendering();
        }
    };
}
