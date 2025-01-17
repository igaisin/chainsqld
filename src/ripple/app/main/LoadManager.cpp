//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2012, 2013 Ripple Labs Inc.

    Permission to use, copy, modify, and/or distribute this software for any
    purpose  with  or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.

    THE  SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    WITH  REGARD  TO  THIS  SOFTWARE  INCLUDING  ALL  IMPLIED  WARRANTIES  OF
    MERCHANTABILITY  AND  FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
    ANY  SPECIAL ,  DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
    WHATSOEVER  RESULTING  FROM  LOSS  OF USE, DATA OR PROFITS, WHETHER IN AN
    ACTION  OF  CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
    OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
//==============================================================================

#include <ripple/app/main/Application.h>
#include <ripple/app/main/LoadManager.h>
#include <ripple/app/misc/LoadFeeTrack.h>
#include <ripple/app/misc/NetworkOPs.h>
#include <ripple/basics/UptimeClock.h>
#include <ripple/json/to_string.h>
#include <ripple/beast/core/CurrentThreadName.h>
#include <peersafe/schema/SchemaManager.h>
#include <memory>
#include <mutex>
#include <thread>
#include <signal.h>

namespace ripple {

LoadManager::LoadManager(
    Application& app,
    Stoppable& parent,
    beast::Journal journal)
    : Stoppable("LoadManager", parent)
    , app_(app)
    , journal_(journal)
    , deadLock_()
    , armed_(false)
    , stop_(false)
{
}

LoadManager::~LoadManager()
{
    try
    {
        onStop();
    }
    catch (std::exception const& ex)
    {
        // Swallow the exception in a destructor.
        JLOG(journal_.warn())
            << "std::exception in ~LoadManager.  " << ex.what();
    }
}

//------------------------------------------------------------------------------

void
LoadManager::activateDeadlockDetector()
{
    std::lock_guard sl(mutex_);
    armed_ = true;
    deadLock_ = std::chrono::steady_clock::now();
}

void
LoadManager::resetDeadlockDetector()
{
    auto const detector_start = std::chrono::steady_clock::now();
    std::lock_guard sl(mutex_);
    deadLock_ = detector_start;
}

//------------------------------------------------------------------------------

void
LoadManager::onPrepare()
{
}

void
LoadManager::onStart()
{
    JLOG(journal_.debug()) << "Starting";
    assert(!thread_.joinable());

    thread_ = std::thread{&LoadManager::run, this};
}

void
LoadManager::onStop()
{
    if (thread_.joinable())
    {
        JLOG(journal_.debug()) << "Stopping";
        {
            std::lock_guard sl(mutex_);
            stop_ = true;
        }
        thread_.join();
    }
    stopped();
}

//------------------------------------------------------------------------------

void
LoadManager::run()
{
    beast::setCurrentThreadName("LoadManager");

    using namespace std::chrono_literals;
    using clock_type = std::chrono::system_clock;

    auto t = clock_type::now();
    bool stop = false;

    while (!(stop || isStopping()))
    {
        {
            // Copy out shared data under a lock.  Use copies outside lock.
            std::unique_lock<std::mutex> sl(mutex_);
            auto const deadLock = deadLock_;
            auto const armed = armed_;
            stop = stop_;
            sl.unlock();

            // Measure the amount of time we have been deadlocked, in seconds.
            using namespace std::chrono;
            auto const timeSpentDeadlocked =
                duration_cast<seconds>(steady_clock::now() - deadLock);

            constexpr auto reportingIntervalSeconds = 10s;
            constexpr auto deadlockFatalLogMessageTimeLimit = 90s;
            constexpr auto deadlockLogicErrorTimeLimit = 600s;
            if (armed && (timeSpentDeadlocked >= reportingIntervalSeconds))
            {
                // Report the deadlocked condition every
                // reportingIntervalSeconds
                if ((timeSpentDeadlocked % reportingIntervalSeconds) == 0s)
                {
                    if (timeSpentDeadlocked < deadlockFatalLogMessageTimeLimit)
                    {
                        JLOG(journal_.warn())
                            << "Server stalled for "
                            << timeSpentDeadlocked.count() << " seconds.";
                    }
                    else
                    {
                        JLOG(journal_.fatal())
                            << "Deadlock detected. Deadlocked time: "
                            << timeSpentDeadlocked.count() << "s";
                        if (app_.getJobQueue().isOverloaded())
                        {
                            JLOG(journal_.fatal())
                                << app_.getJobQueue().getJson(0);
                        }

                        raise(6);
                    }
                }

                // If we go over the deadlockTimeLimit spent deadlocked, it
                // means that the deadlock resolution code has failed, which
                // qualifies as undefined behavior.
                //
                if (timeSpentDeadlocked >= deadlockLogicErrorTimeLimit)
                {
                    JLOG(journal_.fatal())
                        << "LogicError: Deadlock detected. Deadlocked time: "
                        << timeSpentDeadlocked.count() << "s";
                    if (app_.getJobQueue().isOverloaded())
                    {
                        JLOG(journal_.fatal()) << app_.getJobQueue().getJson(0);
                    }
                    LogicError("Deadlock detected");
                }
            }
        }

        bool change = false;
		bool bOverloaded = app_.getJobQueue().isOverloaded();

        app_.getSchemaManager().foreach([this, &bOverloaded, &change](std::shared_ptr<Schema> schema) {
            if (bOverloaded)
			{
				JLOG(journal_.info()) << schema->getJobQueue().getJson(0);

				change = schema->getFeeTrack().raiseLocalFee();
			}
			else
			{
				change = schema->getFeeTrack().lowerLocalFee();
			}

			if (change)
			{
				// VFALCO TODO replace this with a Listener / observer and
				// subscribe in NetworkOPs or Application.
				schema->getOPs().reportFeeChange();
			}
        });
 

        t += 1s;
        auto const duration = t - clock_type::now();

        if ((duration < 0s) || (duration > 1s))
        {
            JLOG(journal_.warn()) << "time jump";
            t = clock_type::now();
        }
        else
        {
            alertable_sleep_until(t);
        }
    }

    stopped();
}

//------------------------------------------------------------------------------

std::unique_ptr<LoadManager>
make_LoadManager(Application& app, Stoppable& parent, beast::Journal journal)
{
    return std::unique_ptr<LoadManager>{new LoadManager{app, parent, journal}};
}

}  // namespace ripple
