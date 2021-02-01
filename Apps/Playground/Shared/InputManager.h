#pragma once

#include <napi/env.h>

#include <functional>
#include <mutex>

template<typename RuntimeT>
class InputManager final : public Napi::ObjectWrap<InputManager<RuntimeT>>
{
public:
    class InputBuffer
    {
    public:
        InputBuffer(RuntimeT&)
        {}
        InputBuffer(const InputBuffer&) = delete;
        InputBuffer& operator=(const InputBuffer&) = delete;

        void SetPointerPosition(int x, int y)
        {
            std::scoped_lock lock{m_mutex};

            m_pointerX = x;
            m_pointerY = y;
        }

        void SetPointerDown(bool isPointerDown)
        {
            std::scoped_lock lock{m_mutex};
            m_isPointerDown = isPointerDown;
        }

        int GetPointerX()
        {
            std::scoped_lock lock{m_mutex};
            return m_pointerX;
        }

        int GetPointerY()
        {
            std::scoped_lock lock{m_mutex};
            return m_pointerY;
        }

        bool IsPointerDown()
        {
            std::scoped_lock lock{m_mutex};
            return m_isPointerDown;
        }

    private:
        std::mutex m_mutex{};

        int m_pointerX{};
        int m_pointerY{};
        bool m_isPointerDown{};
    };

    static void Initialize(Napi::Env env, InputBuffer& buffer)
    {
        Napi::HandleScope scope{ env };

        Napi::Function func = Napi::ObjectWrap<InputManager>::DefineClass(
            env,
            "InputManager",
            {
                Napi::ObjectWrap<InputManager>::InstanceAccessor("pointerX", &InputManager::PointerX, nullptr),
                Napi::ObjectWrap<InputManager>::InstanceAccessor("pointerY", &InputManager::PointerY, nullptr),
                Napi::ObjectWrap<InputManager>::InstanceAccessor("isPointerDown", &InputManager::IsPointerDown, nullptr),
            },
            &buffer);

        env.Global().Set("InputManager", func);
    }

    explicit InputManager(const Napi::CallbackInfo& info)
        : Napi::ObjectWrap<InputManager>{info}
        , m_buffer{static_cast<InputBuffer*>(info.Data())}
    {
    }

private:
    InputBuffer* m_buffer{};

    Napi::Value PointerX(const Napi::CallbackInfo& info)
    {
        return Napi::Value::From(info.Env(), m_buffer->GetPointerX());
    }

    Napi::Value PointerY(const Napi::CallbackInfo& info)
    {
        return Napi::Value::From(info.Env(), m_buffer->GetPointerY());
    }

    Napi::Value IsPointerDown(const Napi::CallbackInfo& info)
    {
        return Napi::Value::From(info.Env(), m_buffer->IsPointerDown());
    }
};
