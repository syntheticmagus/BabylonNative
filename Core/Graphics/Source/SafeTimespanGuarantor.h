#pragma once

#include <gsl/gsl>

#include <condition_variable>
#include <mutex>

#include <arcana/threading/affinity.h>

namespace Babylon
{
    class SafeTimespanGuarantor
    {
    public:
        SafeTimespanGuarantor();

        void BeginSafeTimespan();
        void EndSafeTimespan();

        using SafetyGuarantee = gsl::final_action<std::function<void()>>;
        SafetyGuarantee GetSafetyGuarantee();

    private:
        uint32_t m_count{};
        std::mutex m_mutex{};
        std::condition_variable m_safetyCondition{};
        std::condition_variable m_endCondition{};
        bool m_inSafeTimespan{};
    };
}
