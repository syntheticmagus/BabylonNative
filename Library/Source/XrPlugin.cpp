#include "XrPlugin.h"

#include "NativeEngineImpl.h"

#include <XR.h>

namespace
{
    bgfx::TextureFormat::Enum XrTextureFormatToBgfxFormat(xr::TextureFormat format)
    {
        switch (format)
        {
        case xr::TextureFormat::RGBA8:
            return bgfx::TextureFormat::RGBA8;
        case xr::TextureFormat::D24S8:
            return bgfx::TextureFormat::D24S8;
        default:
            throw std::exception{ /* Unsupported texture format */ };
        }
    }
}

namespace babylon
{
    class XrPlugin : public Napi::ObjectWrap<XrPlugin>
    {
    public:

        static void Initialize(Napi::Env&);

        XrPlugin::XrPlugin(const Napi::CallbackInfo& info);
        ~XrPlugin();

    private:
        static Napi::FunctionReference constructor;

        void SetEngine(const Napi::CallbackInfo& info);
        void BeginSession(const Napi::CallbackInfo&); // TODO: Make this asynchronous.
        void EndSession(const Napi::CallbackInfo&); // TODO: Make this asynchronous.
        void EndSession();
        bool IsSessionActive() const;
        void BeginFrame(const Napi::CallbackInfo&);
        void EndFrame(const Napi::CallbackInfo&);
        void EndFrame();
        Napi::Value GetActiveFrameBuffers(const Napi::CallbackInfo& info);

        xr::System m_hmd{};
        std::unique_ptr<xr::System::Session> m_session{};
        std::unique_ptr<xr::System::Session::Frame> m_frame{};
        FrameBufferManager* m_frameBufferManagerPtr{};
        std::vector<FrameBufferData*> m_activeFrameBuffers{};

        std::map<uintptr_t, std::unique_ptr<FrameBufferData>> m_texturesToFrameBuffers{};
    };

    Napi::FunctionReference XrPlugin::constructor;

    void XrPlugin::Initialize(Napi::Env& env)
    {
        Napi::HandleScope scope{ env };

        Napi::Function func = DefineClass(
            env,
            "XrPlugin",
            {
                InstanceMethod("setEngine", &XrPlugin::SetEngine),
                InstanceMethod("beginSession", &XrPlugin::BeginSession),
                InstanceMethod("endSession", &XrPlugin::EndSession),
                InstanceMethod("beginFrame", &XrPlugin::BeginFrame),
                InstanceMethod("endFrame", &XrPlugin::EndFrame),
                InstanceAccessor("getActiveFrameBuffers", &XrPlugin::GetActiveFrameBuffers, nullptr)
            },
            nullptr);

        constructor = Napi::Persistent(func);
        constructor.SuppressDestruct();

        env.Global().Set("XrPlugin", func);
    }

    XrPlugin::XrPlugin(const Napi::CallbackInfo& info)
        : Napi::ObjectWrap<XrPlugin>{ info }
    {}

    XrPlugin::~XrPlugin()
    {
        if (IsSessionActive())
        {
            if (m_frame != nullptr)
            {
                EndFrame();
            }

            EndSession();
        }
    }

    void XrPlugin::SetEngine(const Napi::CallbackInfo& info)
    {
        m_frameBufferManagerPtr = &(info[0].As<Napi::External<NativeEngine>>().Data()->m_impl->GetFrameBufferManager());
    }

    // TODO: Make this asynchronous.
    void XrPlugin::BeginSession(const Napi::CallbackInfo&)
    {
        assert(!IsSessionActive());
        assert(m_frame == nullptr);

        if (!m_hmd.IsInitialized())
        {
            while (!m_hmd.TryInitialize());
        }

        m_session = m_hmd.CreateSession(bgfx::getInternalData()->context);
    }

    // TODO: Make this asynchronous.
    void XrPlugin::EndSession(const Napi::CallbackInfo&)
    {
        EndSession();
    }

    void XrPlugin::EndSession()
    {
        assert(IsSessionActive());
        assert(m_frame == nullptr);

        m_session->RequestEndSession();

        while (m_session->GetNextFrame() != nullptr);
        m_session.reset();
    }

    bool XrPlugin::IsSessionActive() const
    {
        return m_session != nullptr;
    }

    void XrPlugin::BeginFrame(const Napi::CallbackInfo&)
    {
        assert(IsSessionActive());
        assert(m_frame == nullptr);

        m_frame = m_session->GetNextFrame();

        m_activeFrameBuffers.reserve(m_frame->Views.size());
        for (const auto& view : m_frame->Views)
        {
            auto colorTexPtr = reinterpret_cast<uintptr_t>(view.ColorTexturePointer);

            auto it = m_texturesToFrameBuffers.find(colorTexPtr);
            if (it == m_texturesToFrameBuffers.end())
            {
                assert(view.ColorTextureSize.Width == view.DepthTextureSize.Width);
                assert(view.ColorTextureSize.Height == view.DepthTextureSize.Height);

                auto colorTextureFormat = XrTextureFormatToBgfxFormat(view.ColorTextureFormat);
                auto depthTextureFormat = XrTextureFormatToBgfxFormat(view.DepthTextureFormat);

                assert(bgfx::isTextureValid(0, false, 1, colorTextureFormat, BGFX_TEXTURE_RT));
                assert(bgfx::isTextureValid(0, false, 1, depthTextureFormat, BGFX_TEXTURE_RT));

                auto colorTex = bgfx::createTexture2D(1, 1, false, 1, colorTextureFormat, BGFX_TEXTURE_RT);
                auto depthTex = bgfx::createTexture2D(1, 1, false, 1, depthTextureFormat, BGFX_TEXTURE_RT);

                // Force BGFX to create the texture now, which is necessary in order to use overrideInternal.
                bgfx::frame();

                bgfx::overrideInternal(colorTex, colorTexPtr);
                bgfx::overrideInternal(depthTex, reinterpret_cast<uintptr_t>(view.DepthTexturePointer));

                std::array<bgfx::Attachment, 2> attachments{};
                attachments[0].init(colorTex);
                attachments[1].init(depthTex);
                auto frameBuffer = bgfx::createFrameBuffer(static_cast<uint8_t>(attachments.size()), attachments.data(), false);

                auto fbPtr = m_frameBufferManagerPtr->CreateNew(
                    frameBuffer,
                    static_cast<uint16_t>(view.ColorTextureSize.Width),
                    static_cast<uint16_t>(view.ColorTextureSize.Height));
                m_texturesToFrameBuffers[colorTexPtr] = std::unique_ptr<FrameBufferData>{ fbPtr };

                m_activeFrameBuffers.push_back(fbPtr);
            }
            else
            {
                m_activeFrameBuffers.push_back(it->second.get());
            }
        }
    }

    void XrPlugin::EndFrame(const Napi::CallbackInfo&)
    {
        EndFrame();
    }

    void XrPlugin::EndFrame()
    {
        assert(IsSessionActive());
        assert(m_frame != nullptr);

        m_activeFrameBuffers.clear();

        m_frame.reset();
    }

    Napi::Value XrPlugin::GetActiveFrameBuffers(const Napi::CallbackInfo& info)
    {
        auto activeFrameBuffers = Napi::Array::New(info.Env(), m_activeFrameBuffers.size());
        for (size_t idx = 0; idx < m_activeFrameBuffers.size(); ++idx)
        {
            activeFrameBuffers[idx] = Napi::External<FrameBufferData>::New(info.Env(), m_activeFrameBuffers[idx]);
        }

        return activeFrameBuffers;
    }
}