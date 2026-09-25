// SPDX-License-Identifier: GPL-3.0-or-later
#include <openaxis/wx_scheduler.h>
#include <wx/app.h>
#include <wx/evtloop.h>
#include <wx/init.h>
#include <wx/thread.h>
#include <wx/utils.h>
#include <thread>
#include <iostream>
#include <stdexcept>

static void pump( wxEventLoop& loop, double seconds )
{
    const auto until = openaxis::diagnostic_time() + seconds;
    do
    {
        wxTheApp->ProcessPendingEvents();
        while( loop.Pending() ) loop.Dispatch();
        wxMilliSleep( 1 );
    } while( openaxis::diagnostic_time() < until );
    wxTheApp->ProcessPendingEvents();
    while( loop.Pending() ) loop.Dispatch();
}

int main()
{
    wxInitializer init;
    if( !init.IsOk() ) return 1;
    wxEventLoop loop;
    wxEventLoopActivator active( &loop );
    int count = 0;
    bool wrongThread = false;
    auto scheduler = std::make_unique<OPENAXIS_WX_SCHEDULER>();
    std::thread producer( [&] {
        for( int i = 0; i < 100; ++i )
            scheduler->post( [&] { ++count; wrongThread |= !wxIsMainThread(); } );
    } );
    producer.join();
    if( count ) return 2; // No inline callbacks on the producer thread.
    pump( loop, 0.05 );
    if( count != 100 || wrongThread ) return 3;
    std::vector<int> order;
    bool early = false;
    const auto deadline = openaxis::diagnostic_time() + 0.03;
    scheduler->post_at( deadline + 0.03, [&] {
        early |= openaxis::diagnostic_time() < deadline + 0.03;
        order.push_back( 2 );
    } );
    scheduler->post_at( deadline, [&] {
        early |= openaxis::diagnostic_time() < deadline;
        order.push_back( 1 );
    } );
    pump( loop, 0.1 );
    if( early ) return 4;
    if( order != std::vector<int>{ 1, 2 } ) return 5;
    // Closing a canvas from a callback must invalidate the rest of the batch.
    scheduler->post( [&] { scheduler.reset(); } );
    scheduler->post( [&] { ++count; } );
    pump( loop, 0.02 );
    if( count != 100 ) return 6;
    scheduler = std::make_unique<OPENAXIS_WX_SCHEDULER>();
    scheduler->post_at( openaxis::diagnostic_time() + 0.01, [&] { ++count; } );
    scheduler.reset();
    pump( loop, 0.03 );
    if( count != 100 ) return 7;
    std::cout << "wx scheduler thread dispatch, deadlines and shutdown passed\n";
}
