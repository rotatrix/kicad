// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <openaxis/diagnostics.hpp>
#include <openaxis/scheduler.hpp>
#include <wx/event.h>
#include <wx/timer.h>
#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <vector>

// Thread-safe ingress, UI-thread execution. One queued wakeup per burst and one
// one-shot wx timer service the SDK's monotonic deadlines; there is no polling.
class OPENAXIS_WX_SCHEDULER final : public openaxis::Scheduler
{
    struct STATE final : wxEvtHandler, std::enable_shared_from_this<STATE>
    {
        struct TIMER final : wxTimer
        {
            STATE& state;
            explicit TIMER( STATE& aState ) : state( aState ) {}
            void Notify() override { state.Dispatch(); }
        } timer{ *this };

        struct WORK { double deadline; Callback callback; };
        std::mutex mutex;
        std::vector<WORK> work;
        Callback beforeDispatch;
        bool wakePending = false;
        bool alive = true;

        void Enqueue( double deadline, Callback callback )
        {
            std::lock_guard<std::mutex> lock( mutex );
            if( !alive ) return;
            work.push_back( { deadline, std::move( callback ) } );
            if( wakePending ) return;
            wakePending = true;
            std::weak_ptr<STATE> weak = shared_from_this();
            CallAfter( [weak] { if( auto state = weak.lock() ) state->Dispatch(); } );
        }

        void Dispatch()
        {
            // Keep state valid even if a callback closes its owning canvas.
            const auto keepAlive = shared_from_this();
            timer.Stop();
            std::vector<WORK> ready;
            {
                std::lock_guard<std::mutex> lock( mutex );
                wakePending = false;
                if( !alive ) return;
                const double now = openaxis::diagnostic_time();
                std::stable_sort( work.begin(), work.end(),
                        []( const WORK& a, const WORK& b ) { return a.deadline < b.deadline; } );
                auto next = std::find_if( work.begin(), work.end(),
                        [now]( const WORK& item ) { return item.deadline > now; } );
                std::move( work.begin(), next, std::back_inserter( ready ) );
                work.erase( work.begin(), next );
                if( !work.empty() )
                {
                    const double delay = std::ceil( ( work.front().deadline - now ) * 1000 );
                    timer.StartOnce( int( std::clamp( delay, 1.0,
                                            double( std::numeric_limits<int>::max() ) ) ) );
                }
            }
            for( auto& item : ready )
            {
                if( !alive ) break; // Shutdown and dispatch both execute on the UI thread.
                const auto before = beforeDispatch;
                if( before ) before();
                if( alive ) item.callback();
            }
        }

        void Shutdown()
        {
            timer.Stop();
            std::lock_guard<std::mutex> lock( mutex );
            alive = false;
            work.clear();
            beforeDispatch = {};
        }
    };

    std::shared_ptr<STATE> m_state = std::make_shared<STATE>();

public:
    ~OPENAXIS_WX_SCHEDULER() override { m_state->Shutdown(); }
    void SetBeforeDispatch( Callback callback ) { m_state->beforeDispatch = std::move( callback ); }
    void post( Callback callback ) override { m_state->Enqueue( 0, std::move( callback ) ); }
    void post_at( double deadline, Callback callback ) override
    {
        m_state->Enqueue( deadline, std::move( callback ) );
    }
};
