#pragma once

#include <gsl/gsl>

#include <condition_variable>
#include <mutex>
#include <future>

#include <arcana/threading/affinity.h>

namespace Babylon
{
    class SafeTimespanGuarantor
    {
    public:
        SafeTimespanGuarantor();

        void BeginSafeTimespan();
        void EndSafeTimespan();

        class SafetyGuarantee
        {
        public:
            SafetyGuarantee(std::function<void()> callback)
                : m_finalAction{std::move(callback)}
            {
            }

            SafetyGuarantee() = default;
            SafetyGuarantee(const SafetyGuarantee&) = delete;
            SafetyGuarantee& operator=(const SafetyGuarantee&) = delete;
            SafetyGuarantee(SafetyGuarantee&&) noexcept;
            SafetyGuarantee& operator=(SafetyGuarantee&&) noexcept;
        
        private:
            std::optional<gsl::final_action<std::function<void()>>> m_finalAction{};
        };

        std::future<SafetyGuarantee> GetSafetyGuarantee();
        bool IsCurrentTimespanSafe();

    private:
        SafetyGuarantee InternalGetSafetyGuarantee();

        uint32_t m_count{};
        std::mutex m_mutex{};
        bool m_inSafeTimespan{};
        std::vector<std::promise<SafetyGuarantee>> m_promises{};
        std::condition_variable m_endCondition{};
    };
}
