#include "SafeTimespanGuarantor.h"

namespace Babylon
{
    SafeTimespanGuarantor::SafetyGuarantee::SafetyGuarantee(SafetyGuarantee&& other) noexcept
        : m_finalAction{}
    {
        *this = std::move(other);
    }

    SafeTimespanGuarantor::SafetyGuarantee& SafeTimespanGuarantor::SafetyGuarantee::operator=(SafetyGuarantee&& other) noexcept
    {
        m_finalAction.reset();
        if (other.m_finalAction)
        {
            m_finalAction.emplace(std::move(other.m_finalAction.value()));
        }
        return *this;
    }

    SafeTimespanGuarantor::SafeTimespanGuarantor()
        : m_inSafeTimespan{false}
    {
    }

    void SafeTimespanGuarantor::BeginSafeTimespan()
    {
        std::scoped_lock lock{m_mutex};
        m_inSafeTimespan = true;

        for (auto& promise : m_promises)
        {
            promise.set_value(InternalGetSafetyGuarantee());
        }
        m_promises.clear();
    }

    void SafeTimespanGuarantor::EndSafeTimespan()
    {
        // First lock on the underlying mutex.
        std::unique_lock lock{m_mutex};

        // Then wait for the count of outstanding SafeteyGuarentees to reach zero.
        // If the condition is not met, the underlying mutex is unlocked, but we still block on the condition variable, waiting to be signated to recheck the condition.
        // Once the condition is met, then the condition variable unblocks, but the lock on the underlying mutex (re-acquired when checking the condition) is retained.
        m_endCondition.wait(lock, [this] { return m_count == 0; });

        // Document the end of the safe timespan.
        m_inSafeTimespan = false;
    }

    std::future<SafeTimespanGuarantor::SafetyGuarantee> SafeTimespanGuarantor::GetSafetyGuarantee()
    {
        std::scoped_lock lock{m_mutex};
        std::promise<SafetyGuarantee> promise{};
        auto future{promise.get_future()};
        if (m_inSafeTimespan)
        {
            promise.set_value(InternalGetSafetyGuarantee());
        }
        else
        {
            m_promises.push_back(std::move(promise));
        }
        return future;
    }

    bool SafeTimespanGuarantor::IsCurrentTimespanSafe()
    {
        std::scoped_lock lock{m_mutex};
        return m_inSafeTimespan;
    }

    SafeTimespanGuarantor::SafetyGuarantee SafeTimespanGuarantor::InternalGetSafetyGuarantee()
    {
        // Increment the outstanding SafetyGuarantee count.
        m_count++;

        // Then return a SafeteyGuarantee that should be held until caller operations are complete.
        return {
            std::function<void()>{[this]() {
                // First lock the underlying mutex and decrement the outstanding SafeteyGuarantee count.
                {
                    std::scoped_lock lock{m_mutex};
                    m_count--;
                }

                // Then signal the condition variable to recheck the condition.
                m_endCondition.notify_one();
            }}};
    }
}
