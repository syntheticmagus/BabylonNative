#pragma once

#include <napi/env.h>

#include <functional>
#include <string>

namespace Babylon
{
    class JsRuntime
    {
        using DispatchFunctionT = std::function<void(std::function<void()>)>;

    public:
        JsRuntime(Napi::Env env, DispatchFunctionT dispatchFunction)
            : m_env{ env }
            , m_dispatchFunction{ std::move(dispatchFunction) }
        {
        }

        template<typename CallableT>
        void Dispatch(CallableT callable)
        {
            m_dispatchFunction([this, callable = std::move(callable)]
            {
                callable(m_env);
            });
        }

        void Eval(std::string source, std::string sourceUrl)
        {
            m_dispatchFunction([this, source = std::move(source), sourceUrl = std::move(sourceUrl)]
            {
                Napi::Eval(m_env, source.c_str(), sourceUrl.c_str());
            });
        }

    private:
        Napi::Env m_env;
        DispatchFunctionT m_dispatchFunction{};
    };
}
