#include "SafeTimespanGuarantor.h"

namespace Babylon
{
    SafeTimespanGuarantor::SafeTimespanGuarantor()
        : m_inSafeTimespan{false}
    {
    }

    void SafeTimespanGuarantor::BeginSafeTimespan()
    {
        {
            std::scoped_lock lock{m_mutex};
            m_inSafeTimespan = true;
        }
        m_safetyCondition.notify_all();
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

    SafeTimespanGuarantor::SafetyGuarantee SafeTimespanGuarantor::BlockingGetSafetyGuarantee()
    {
        std::unique_lock lock{m_mutex};
        return InternalGetSafetyGuarantee(lock);
    }

    std::optional<SafeTimespanGuarantor::SafetyGuarantee> SafeTimespanGuarantor::TryGetSafetyGuarantee()
    {
        std::unique_lock lock{m_mutex};
        if (m_inSafeTimespan)
        {
            return InternalGetSafetyGuarantee(lock);
        }
        else
        {
            return{};
        }
    }

    SafeTimespanGuarantor::SafetyGuarantee SafeTimespanGuarantor::InternalGetSafetyGuarantee(std::unique_lock<std::mutex>& lock)
    {
        // First lock on the underlying mutex and wait until we're in a safe timespan.
        if (!m_inSafeTimespan)
        {
            m_safetyCondition.wait(lock, [this] { return m_inSafeTimespan; });
        }

        // Increment the outstanding SafetyGuarantee count.
        m_count++;

        // Then return a SafeteyGuarantee that should be held until caller operations are complete.
        return gsl::finally(std::function<void()>{ [this]
        {
            // First lock the underlying mutex and decrement the outstanding SafeteyGuarantee count.
            {
                std::scoped_lock lock{m_mutex};
                m_count--;
            }

            // Then signal the condition variable to recheck the condition.
            m_endCondition.notify_one();
        }});
    }
}
