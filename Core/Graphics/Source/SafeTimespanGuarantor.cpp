#include "SafeTimespanGuarantor.h"

namespace Babylon
{
    SafeTimespanGuarantor::SafeTimespanGuarantor()
        : m_lock{m_mutex}
    {
    }

    void SafeTimespanGuarantor::BeginSafeTimespan()
    {
        m_postCount = 0;
        m_lock.reset();
    }

    bool SafeTimespanGuarantor::TryEndSafeTimespan(int32_t milliseconds)
    {
        m_lock.emplace(m_mutex);
        if (m_postCount == 0)
        {
            return true;
        }
        m_lock.reset();

        auto start = std::chrono::steady_clock::now();
        bool waitSucceed = m_semaphore.wait(milliseconds);
        auto stop = std::chrono::steady_clock::now();
        
        if (waitSucceed)
        {
            // TODO: There are tricks we can use to avoid this excess lock.
            m_lock.emplace(m_mutex);
            --m_postCount;
            m_lock.reset();

            // If milliseconds is nonnegative, reduce it by the time already waited, being careful 
            // not to go negative because that will cause bx::semaphore::wait() to become indefinitely
            // blocking.
            if (milliseconds >= 0)
            {
                auto waitTimeMillis = static_cast<int32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(stop - start).count());
                milliseconds = std::max(milliseconds - waitTimeMillis, 0);
            }
            return TryEndSafeTimespan(milliseconds);
        }
        else
        {
            return false;
        }
    }

    SafeTimespanGuarantor::SafetyGuarantee SafeTimespanGuarantor::GetSafetyGuarantee()
    {
        std::scoped_lock lock{m_mutex};
        ++m_postCount;
        return m_semaphore.GetPostFinalAction();
    }
}
