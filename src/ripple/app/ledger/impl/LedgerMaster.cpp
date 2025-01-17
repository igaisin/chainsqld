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

#include <ripple/app/consensus/RCLValidations.h>
#include <ripple/app/ledger/Ledger.h>
#include <ripple/app/ledger/LedgerMaster.h>
#include <ripple/app/ledger/TransactionMaster.h>
#include <ripple/app/ledger/InboundLedgers.h>
#include <ripple/app/ledger/OpenLedger.h>
#include <ripple/app/ledger/OrderBookDB.h>
#include <ripple/app/ledger/PendingSaves.h>
#include <ripple/app/main/Application.h>
#include <ripple/app/misc/AmendmentTable.h>
#include <ripple/app/misc/HashRouter.h>
#include <ripple/app/misc/LoadFeeTrack.h>
#include <ripple/app/misc/NetworkOPs.h>
#include <ripple/app/misc/SHAMapStore.h>
#include <ripple/app/misc/Transaction.h>
#include <ripple/app/misc/TxQ.h>
#include <ripple/app/misc/ValidatorList.h>
#include <ripple/app/paths/PathRequests.h>
#include <ripple/app/tx/apply.h>
#include <ripple/basics/Log.h>
#include <ripple/basics/MathUtilities.h>
#include <ripple/basics/TaggedCache.h>
#include <ripple/basics/UptimeClock.h>
#include <ripple/basics/contract.h>
#include <ripple/basics/safe_cast.h>
#include <ripple/core/DatabaseCon.h>
#include <ripple/core/TimeKeeper.h>
#include <ripple/nodestore/DatabaseShard.h>
#include <ripple/overlay/Overlay.h>
#include <ripple/overlay/Peer.h>
#include <ripple/protocol/BuildInfo.h>
#include <ripple/protocol/HashPrefix.h>
#include <ripple/protocol/digest.h>
#include <ripple/resource/Fees.h>
#include <peersafe/app/table/TableSync.h>
#include <peersafe/app/storage/TableStorage.h>
#include <peersafe/rpc/impl/TableAssistant.h>
#include <peersafe/app/table/TableTxAccumulator.h>
#include <peersafe/protocol/STEntry.h>
#include <peersafe/app/sql/TxStore.h>
#include <peersafe/app/misc/TxPool.h>
#include <peersafe/schema/Schema.h>
#include <peersafe/schema/PeerManager.h>
#include <peersafe/schema/SchemaManager.h>
#include <peersafe/gmencrypt/GmCheck.h>
#include <peersafe/consensus/ConsensusBase.h>
#include <peersafe/rpc/TableUtils.h>
#include <algorithm>
#include <cassert>
#include <limits>
#include <memory>
#include <vector>

namespace ripple {

namespace {

//==============================================================================
/**
    Automatically unlocks and re-locks a unique_lock object.

    This is the reverse of a std::unique_lock object - instead of locking the
   mutex for the lifetime of this object, it unlocks it.

    Make sure you don't try to unlock mutexes that aren't actually locked!

    This is essentially a less-versatile boost::reverse_lock.

    e.g. @code

    std::mutex mut;

    for (;;)
    {
        std::unique_lock myScopedLock{mut};
        // mut is now locked

        ... do some stuff with it locked ..

        while (xyz)
        {
            ... do some stuff with it locked ..

            ScopedUnlock unlocker{myScopedLock};

            // mut is now unlocked for the remainder of this block,
            // and re-locked at the end.

            ...do some stuff with it unlocked ...
        }  // mut gets locked here.

    }  // mut gets unlocked here
    @endcode
*/
template <class MutexType>
class ScopedUnlock
{
    std::unique_lock<MutexType>& lock_;

public:
    /** Creates a ScopedUnlock.

        As soon as it is created, this will unlock the unique_lock, and
        when the ScopedLock object is deleted, the unique_lock will
        be re-locked.

        Make sure this object is created and deleted by the same thread,
        otherwise there are no guarantees what will happen! Best just to use it
        as a local stack object, rather than creating on the heap.
    */
    explicit ScopedUnlock(std::unique_lock<MutexType>& lock) : lock_(lock)
    {
        assert(lock_.owns_lock());
        lock_.unlock();
    }

    ScopedUnlock(ScopedUnlock const&) = delete;
    ScopedUnlock&
    operator=(ScopedUnlock const&) = delete;

    /** Destructor.

        The unique_lock will be locked after the destructor is called.

        Make sure this object is created and deleted by the same thread,
        otherwise there are no guarantees what will happen!
    */
    ~ScopedUnlock() noexcept(false)
    {
        lock_.lock();
    }
};

}  // namespace

// Don't catch up more than 100 ledgers (cannot exceed 256)
static constexpr int MAX_LEDGER_GAP{100};

// Don't acquire history if ledger is too old
static constexpr std::chrono::minutes MAX_LEDGER_AGE_ACQUIRE{1};

// Don't acquire history if write load is too high
static constexpr int MAX_WRITE_LOAD_ACQUIRE{8192};

// Helper function for LedgerMaster::doAdvance()
// Return true if candidateLedger should be fetched from the network.
static bool
shouldAcquire(
    std::uint32_t const currentLedger,
    std::uint32_t const ledgerHistory,
    boost::optional<LedgerIndex> const minimumOnline,
    std::uint32_t const candidateLedger,
    beast::Journal j)
{
    bool const ret = [&]() {
        // Fetch ledger if it may be the current ledger
        if (candidateLedger >= currentLedger)
            return true;

        // Or if it is within our configured history range:
        if (currentLedger - candidateLedger <= ledgerHistory)
            return true;

        // Or if greater than or equal to a specific minimum ledger.
        // Do nothing if the minimum ledger to keep online is unknown.
        return minimumOnline.has_value() && candidateLedger >= *minimumOnline;
    }();

    JLOG(j.trace()) << "Missing ledger " << candidateLedger
                    << (ret ? " should" : " should NOT") << " be acquired";
    return ret;
}

LedgerMaster::LedgerMaster(
    Schema& app,
    Stopwatch& stopwatch,
    Stoppable& parent,
    beast::insight::Collector::ptr const& collector,
    beast::Journal journal)
    : Stoppable("LedgerMaster", parent)
    , app_(app)
    , m_journal(journal)
    , mLedgerHistory(collector, app)
    , mLedgerCleaner(
          detail::make_LedgerCleaner(app, *this, app_.journal("LedgerCleaner")))
    , standalone_(app_.config().standalone())
    , fetch_depth_(
          app_.getSHAMapStore().clampFetchDepth(app_.config().FETCH_DEPTH))
    , ledger_history_(app_.config().LEDGER_HISTORY)
    , ledger_fetch_size_(app_.config().getValueFor(SizedItem::ledgerFetch))
    , fetch_packs_(
          "FetchPack",
          65536,
          std::chrono::seconds{45},
          stopwatch,
          app_.journal("TaggedCache"))
    , m_stats(std::bind(&LedgerMaster::collect_metrics, this), collector)
{
}

void
LedgerMaster::setLastValidLedger(uint256 const& hash, std::uint32_t seq)
{
    std::lock_guard ml(m_mutex);
    if (seq > mLastValidLedger.second)
        mLastValidLedger = std::make_pair(hash, seq);
}

bool
LedgerMaster::isCompatible(
    ReadView const& view,
    beast::Journal::Stream s,
    char const* reason)
{
    auto validLedger = getValidatedLedger();

    if (validLedger && !areCompatible(*validLedger, view, s, reason))
    {
        return false;
    }

    {
        std::lock_guard sl(m_mutex);

        if ((mLastValidLedger.second != 0) &&
            !areCompatible(
                mLastValidLedger.first,
                mLastValidLedger.second,
                view,
                s,
                reason))
        {
            return false;
        }
    }

    return true;
}

std::chrono::seconds
LedgerMaster::getPublishedLedgerAge()
{
    using namespace std::chrono_literals;
    std::chrono::seconds pubClose{mPubLedgerClose.load()};
    if (pubClose == 0s)
    {
        JLOG(m_journal.debug()) << "No published ledger";
        return weeks{2};
    }

    std::chrono::seconds ret = app_.timeKeeper().closeTime().time_since_epoch();
    ret -= pubClose;
    ret = (ret > 0s) ? ret : 0s;

    JLOG(m_journal.trace()) << "Published ledger age is " << ret.count();
    return ret;
}

void
LedgerMaster::onConsensusReached(
    bool waitingConsensusReach,
    std::shared_ptr<Ledger const> previousLedger)
{
    updateConsensusTime();

    if (waitingConsensusReach &&
        previousLedger &&
        previousLedger->info().seq != mValidLedgerSeq)
    {
        setFullLedger(previousLedger, false, true);
        setPubLedger(previousLedger);
        {
            std::lock_guard ml(mCompleteLock);
            mCompleteLedgers.insert(previousLedger->info().seq);
        }
    }
    checkSubChains();
    checkLoadLedger();
    app_.getTableSync().TryTableSync();
    app_.getTableSync().InitTableItems();
    tryAdvance();
}

std::chrono::seconds
LedgerMaster::getValidatedLedgerAge()
{
    using namespace std::chrono_literals;
    std::chrono::seconds valClose = std::chrono::seconds(
        std::max(mValidLedgerSign.load(), mLastConsensusTime.load()));
    if (valClose == 0s)
    {
        JLOG(m_journal.debug()) << "No validated ledger";
        return NO_VALIDATED_LEDGER_AGE;
    }

    std::chrono::seconds ret = app_.timeKeeper().closeTime().time_since_epoch();
    ret -= valClose;
    ret = (ret > 0s) ? ret : 0s;

    JLOG(m_journal.trace()) << "Validated ledger age is " << ret.count();
    return ret;
}

bool
LedgerMaster::isCaughtUp(std::string& reason)
{
    using namespace std::chrono_literals;
    if (getPublishedLedgerAge() > 3min)
    {
        reason = "No recently-published ledger";
        return false;
    }
    std::uint32_t validClose = mValidLedgerSign.load();
    std::uint32_t pubClose = mPubLedgerClose.load();
    if (!validClose || !pubClose)
    {
        reason = "No published ledger";
        return false;
    }
    if (validClose > (pubClose + 90))
    {
        reason = "Published ledger lags validated ledger";
        return false;
    }
    return true;
}

void
LedgerMaster::setValidLedger(std::shared_ptr<Ledger const> const& l)
{
    std::vector<NetClock::time_point> times;
    boost::optional<uint256> consensusHash;

    if (!standalone_)
    {
        auto validations = app_.validators().negativeUNLFilter(
            app_.getValidations().getTrustedForLedger(l->info().hash));
        times.reserve(validations.size());
        for (auto const& val : validations)
            times.push_back(val->getSignTime());

        if (!validations.empty())
            consensusHash = validations.front()->getConsensusHash();
    }

    NetClock::time_point signTime;

    if (!times.empty() && times.size() >= app_.validators().quorum())
    {
        // Calculate the sample median
        std::sort(times.begin(), times.end());
        auto const t0 = times[(times.size() - 1) / 2];
        auto const t1 = times[times.size() / 2];
        signTime = t0 + (t1 - t0) / 2;
    }
    else
    {
        signTime = l->info().closeTime.time_since_epoch().count() == 0
            ? app_.timeKeeper().closeTime()
            : l->info().closeTime;
    }

    mValidLedger.set(l);
    mValidLedgerSign = signTime.time_since_epoch().count();
    assert(
        mValidLedgerSeq || !app_.getMaxDisallowedLedger() ||
        l->info().seq + max_ledger_difference_ > app_.getMaxDisallowedLedger());
    (void)max_ledger_difference_;
    mValidLedgerSeq = l->info().seq;

    app_.getOPs().updateLocalTx(*l);
    app_.getSHAMapStore().onLedgerClosed(getValidatedLedger());
    mLedgerHistory.validatedLedger(l, consensusHash);
    app_.getAmendmentTable().doValidatedLedger(l);
    if (!app_.getOPs().isAmendmentBlocked())
    {
        if (app_.getAmendmentTable().hasUnsupportedEnabled())
        {
            JLOG(m_journal.error()) << "One or more unsupported amendments "
                                       "activated: server blocked.";
            app_.getOPs().setAmendmentBlocked();
        }
        else if (!app_.getOPs().isAmendmentWarned() || l->isFlagLedger())
        {
            // Amendments can lose majority, so re-check periodically (every
            // flag ledger), and clear the flag if appropriate. If an unknown
            // amendment gains majority log a warning as soon as it's
            // discovered, then again every flag ledger until the operator
            // upgrades, the amendment loses majority, or the amendment goes
            // live and the node gets blocked. Unlike being amendment blocked,
            // this message may be logged more than once per session, because
            // the node will otherwise function normally, and this gives
            // operators an opportunity to see and resolve the warning.
            if (auto const first =
                    app_.getAmendmentTable().firstUnsupportedExpected())
            {
                JLOG(m_journal.error()) << "One or more unsupported amendments "
                                           "reached majority. Upgrade before "
                                        << to_string(*first)
                                        << " to prevent your server from "
                                           "becoming amendment blocked.";
                app_.getOPs().setAmendmentWarned();
            }
            else
                app_.getOPs().clearAmendmentWarned();
        }
    }
}

void
LedgerMaster::setPubLedger(std::shared_ptr<Ledger const> const& l)
{
    mPubLedger = l;
    mPubLedgerClose = l->info().closeTime.time_since_epoch().count();
    mPubLedgerSeq = l->info().seq;
}

bool
LedgerMaster::addHeldTransaction(
    std::shared_ptr<Transaction> const& transaction,
    bool bForceAdd)
{
    std::lock_guard ml(m_mutex);
    return mHeldTransactions.insert(transaction,bForceAdd);
}

// Validate a ledger's close time and sequence number if we're considering
// jumping to that ledger. This helps defend agains some rare hostile or
// insane majority scenarios.
bool
LedgerMaster::canBeCurrent(
    std::shared_ptr<Ledger const> const& ledger,
    ConsensusType type)
{
    assert(ledger);

    // Never jump to a candidate ledger that precedes our
    // last validated ledger

    std::shared_ptr<Ledger const> validLedger = nullptr;
    if (type == ConsensusType::HOTSTUFF)
    {
        validLedger = getClosedLedger();
    }
    else
    {
        validLedger = getValidatedLedger();
    }

    if (validLedger && (ledger->info().seq < validLedger->info().seq))
    {
        JLOG(m_journal.trace())
            << "Candidate for current ledger has low seq " << ledger->info().seq
            << " < " << validLedger->info().seq;
        return false;
    }

    // Ensure this ledger's parent close time is within five minutes of
    // our current time. If we already have a known fully-valid ledger
    // we perform this check. Otherwise, we only do it if we've built a
    // few ledgers as our clock can be off when we first start up

    auto closeTime = app_.timeKeeper().closeTime();
    auto ledgerClose = ledger->info().parentCloseTime;

    using namespace std::chrono_literals;
    if ((validLedger || (ledger->info().seq > 10)) &&
        ((std::max(closeTime, ledgerClose) - std::min(closeTime, ledgerClose)) >
         5min))
    {
        JLOG(m_journal.warn())
            << "Candidate for current ledger has close time "
            << to_string(ledgerClose) << " at network time "
            << to_string(closeTime) << " seq " << ledger->info().seq;
        return false;
    }

    if (validLedger)
    {
        // Sequence number must not be too high. We allow ten ledgers
        // for time inaccuracies plus a maximum run rate of one ledger
        // every two seconds. The goal is to prevent a malicious ledger
        // from increasing our sequence unreasonably high

        LedgerIndex maxSeq = validLedger->info().seq + 10;

        if (closeTime > validLedger->info().parentCloseTime)
            maxSeq += std::chrono::duration_cast<std::chrono::seconds>(
                          closeTime - validLedger->info().parentCloseTime)
                          .count() /
                2;

        if (ledger->info().seq > maxSeq)
        {
            JLOG(m_journal.warn())
                << "Candidate for current ledger has high seq "
                << ledger->info().seq << " > " << maxSeq;
            return false;
        }

        JLOG(m_journal.trace())
            << "Acceptable seq range: " << validLedger->info().seq
            << " <= " << ledger->info().seq << " <= " << maxSeq;
    }

    return true;
}

void
LedgerMaster::switchLCL(std::shared_ptr<Ledger const> const& lastClosed)
{
    assert(lastClosed);
    if (!lastClosed->isImmutable())
        LogicError("mutable ledger in switchLCL");

    if (lastClosed->open())
        LogicError("The new last closed ledger is open!");

    {
        std::lock_guard ml(m_mutex);
        mClosedLedger.set(lastClosed);
    }

    if (standalone_)
    {
        setFullLedger (lastClosed, true, false);
		tryAdvance();
		app_.getTableSync().TryTableSync();
		app_.getTableStorage().TryTableStorage();
    }
    else
    {
#ifdef HARD_GM
		LedgerInfo ledgerInfo = lastClosed->info();
		if (nullptr != GmEncryptObj::getInstance() && GmEncryptObj::hEType_ == GmEncryptObj::gmAlgType::sjkCardType)
		{
			GMCheck* gmCheckObj = GMCheck::getInstance();
			gmCheckObj->tryRandomCycleCheck(ledgerInfo.seq);
		}
#endif
    }
}

bool
LedgerMaster::fixIndex(LedgerIndex ledgerIndex, LedgerHash const& ledgerHash)
{
    return mLedgerHistory.fixIndex(ledgerIndex, ledgerHash);
}

bool
LedgerMaster::storeLedger(std::shared_ptr<Ledger const> ledger)
{
    // Returns true if we already had the ledger
    return mLedgerHistory.insert(std::move(ledger), false);
}

std::vector<std::shared_ptr<Transaction>>
LedgerMaster::pruneHeldTransactions(
    AccountID const& account,
    std::uint32_t const seq)
{
    std::lock_guard sl(m_mutex);

    return mHeldTransactions.prune(account, seq);
}

bool
LedgerMaster::haveLedger(std::uint32_t seq)
{
    std::lock_guard sl(mCompleteLock);
    return boost::icl::contains(mCompleteLedgers, seq);
}

bool
LedgerMaster::haveLedger(std::uint32_t seqMin, std::uint32_t seqMax)
{
    std::lock_guard sl(mCompleteLock);
    for (RangeSet<std::uint32_t>::iterator it = mCompleteLedgers.begin();
         it != mCompleteLedgers.end();
         it++)
    {
        if (it->lower() <= seqMin && it->upper() >= seqMax)
            return true;
    }
    return false;
}

std::uint32_t
LedgerMaster::lastCompleteIndex()
{
    std::lock_guard sl(mCompleteLock);
    return boost::icl::last(mCompleteLedgers);
}

void
LedgerMaster::clearLedger(std::uint32_t seq)
{
    std::lock_guard sl(mCompleteLock);
    mCompleteLedgers.erase(seq);
}

// returns Ledgers we have all the nodes for
bool
LedgerMaster::getFullValidatedRange(
    std::uint32_t& minVal,
    std::uint32_t& maxVal)
{
    // Validated ledger is likely not stored in the DB yet so we use the
    // published ledger which is.
    maxVal = mPubLedgerSeq.load();

    if (!maxVal)
        return false;

    boost::optional<std::uint32_t> maybeMin;
    {
        std::lock_guard sl(mCompleteLock);
        maybeMin = prevMissing(mCompleteLedgers, maxVal);
    }

    if (maybeMin == boost::none)
        minVal = maxVal;
    else
        minVal = 1 + *maybeMin;

    return true;
}

// Returns Ledgers we have all the nodes for and are indexed
bool
LedgerMaster::getValidatedRange(std::uint32_t& minVal, std::uint32_t& maxVal)
{
    if (!getFullValidatedRange(minVal, maxVal))
        return false;

    // Remove from the validated range any ledger sequences that may not be
    // fully updated in the database yet

    auto const pendingSaves = app_.pendingSaves().getSnapshot();

    if (!pendingSaves.empty() && ((minVal != 0) || (maxVal != 0)))
    {
        // Ensure we shrink the tips as much as possible. If we have 7-9 and
        // 8,9 are invalid, we don't want to see the 8 and shrink to just 9
        // because then we'll have nothing when we could have 7.
        while (pendingSaves.count(maxVal) > 0)
            --maxVal;
        while (pendingSaves.count(minVal) > 0)
            ++minVal;

        // Best effort for remaining exclusions
        for (auto v : pendingSaves)
        {
            if ((v.first >= minVal) && (v.first <= maxVal))
            {
                if (v.first > ((minVal + maxVal) / 2))
                    maxVal = v.first - 1;
                else
                    minVal = v.first + 1;
            }
        }

        if (minVal > maxVal)
            minVal = maxVal = 0;
    }

    return true;
}

// Get the earliest ledger we will let peers fetch
std::uint32_t
LedgerMaster::getEarliestFetch()
{
    // The earliest ledger we will let people fetch is ledger zero,
    // unless that creates a larger range than allowed
    std::uint32_t e = getClosedLedger()->info().seq;

    if (e > fetch_depth_)
        e -= fetch_depth_;
    else
        e = 0;
    return e;
}

void
LedgerMaster::tryFill(Job& job, std::shared_ptr<Ledger const> ledger)
{
	JLOG(m_journal.info()) << "tryFill:" << ledger->info().seq;
    std::uint32_t seq = ledger->info().seq;
    uint256 prevHash = ledger->info().parentHash;

    std::map<std::uint32_t, std::pair<uint256, uint256>> ledgerHashes;

    std::uint32_t minHas = seq;
    std::uint32_t maxHas = seq;

    NodeStore::Database& nodeStore{app_.getNodeStore()};
    while (!job.shouldCancel() && seq > 0)
    {
        {
            std::lock_guard ml(m_mutex);
            minHas = seq;
            --seq;

            if (haveLedger(seq))
                break;
        }

        auto it(ledgerHashes.find(seq));

        if (it == ledgerHashes.end())
        {
            if (app_.app().isShutdown())
                return;

            {
                std::lock_guard ml(mCompleteLock);
                mCompleteLedgers.insert(range(minHas, maxHas));
            }
            maxHas = minHas;
            ledgerHashes =
                getHashesByIndex((seq < 500) ? 0 : (seq - 499), seq, app_);
            it = ledgerHashes.find(seq);

            if (it == ledgerHashes.end())
                break;

            if (!nodeStore.fetch(
                    ledgerHashes.begin()->second.first,
                    ledgerHashes.begin()->first))
            {
                // The ledger is not backed by the node store
                JLOG(m_journal.warn()) << "SQL DB ledger sequence " << seq
                                       << " mismatches node store";
                break;
            }
        }

        if (it->second.first != prevHash)
            break;

        prevHash = it->second.second;
    }

    {
        std::lock_guard ml(mCompleteLock);
        mCompleteLedgers.insert(range(minHas, maxHas));
    }
    {
        std::lock_guard ml(m_mutex);
        mFillInProgress = 0;
        tryAdvance();
    }
}

/** Request a fetch pack to get to the specified ledger
 */
void
LedgerMaster::getFetchPack(LedgerIndex missing, InboundLedger::Reason reason)
{
    LedgerIndex const ledgerIndex([&]() {
        if (reason == InboundLedger::Reason::SHARD)
        {
            // Do not acquire a ledger sequence greater
            // than the last ledger in the shard
            auto const shardStore{app_.getShardStore()};
            auto const shardIndex{shardStore->seqToShardIndex(missing)};
            return std::min(missing + 1, shardStore->lastLedgerSeq(shardIndex));
        }
        return missing + 1;
    }());

    auto const haveHash{getLedgerHashForHistory(ledgerIndex, reason)};
    if (!haveHash || haveHash->isZero())
    {
        if (reason == InboundLedger::Reason::SHARD)
        {
            auto const shardStore{app_.getShardStore()};
            auto const shardIndex{shardStore->seqToShardIndex(missing)};
            if (missing < shardStore->lastLedgerSeq(shardIndex))
            {
                JLOG(m_journal.error())
                    << "No hash for fetch pack. "
                    << "Missing ledger sequence " << missing
                    << " while acquiring shard " << shardIndex;
            }
        }
        else
        {
            JLOG(m_journal.error())
                << "No hash for fetch pack. Missing Index " << missing;
        }
        return;
    }

    // Select target Peer based on highest score.  The score is randomized
    // but biased in favor of Peers with low latency.
    std::shared_ptr<Peer> target;
    {
        int maxScore = 0;
        auto peerList = app_.peerManager().getActivePeers();
        for (auto const& peer : peerList)
        {
            if (peer->hasRange(app_.schemaId(), missing, missing + 1))
            {
                int score = peer->getScore(true);
                if (!target || (score > maxScore))
                {
                    target = peer;
                    maxScore = score;
                }
            }
        }
    }

    if (target)
    {
        protocol::TMGetObjectByHash tmBH;
        tmBH.set_query(true);
        tmBH.set_type(protocol::TMGetObjectByHash::otFETCH_PACK);
        tmBH.set_ledgerhash(haveHash->begin(), 32);
        tmBH.set_schemaid(app_.schemaId().begin(), uint256::size());
        auto packet = std::make_shared<Message>(tmBH, protocol::mtGET_OBJECTS);

        target->send(packet);
        JLOG(m_journal.trace()) << "Requested fetch pack for " << missing;
    }
    else
        JLOG(m_journal.debug()) << "No peer for fetch pack";
}

void
LedgerMaster::fixMismatch(ReadView const& ledger)
{
    int invalidate = 0;
    boost::optional<uint256> hash;

    for (std::uint32_t lSeq = ledger.info().seq - 1; lSeq > 0; --lSeq)
    {
        if (haveLedger(lSeq))
        {
            try
            {
                hash = hashOfSeq(ledger, lSeq, m_journal);
            }
            catch (std::exception const&)
            {
                JLOG(m_journal.warn())
                    << "fixMismatch encounters partial ledger";
                clearLedger(lSeq);
                return;
            }

            if (hash)
            {
                // try to close the seam
                auto otherLedger = getLedgerBySeq(lSeq);

                if (otherLedger && (otherLedger->info().hash == *hash))
                {
                    // we closed the seam
                    if (invalidate != 0)
                    {
                        JLOG(m_journal.warn())
                            << "Match at " << lSeq << ", " << invalidate
                            << " prior ledgers invalidated";
                    }

                    return;
                }
            }

            clearLedger(lSeq);
            ++invalidate;
        }
    }

    // all prior ledgers invalidated
    if (invalidate != 0)
    {
        JLOG(m_journal.warn())
            << "All " << invalidate << " prior ledgers invalidated";
    }
}

void
LedgerMaster::onLastFullLedgerLoaded(
    std::shared_ptr<Ledger const> const& ledger)
{
    setLedgerRangePresent(ledger->info().seq, ledger->info().seq);
    switchLCL(ledger);
    ledger->setValidated();
    ledger->setFull();
    mLedgerHistory.insert(ledger, true);

    if (ledger->info().seq > mValidLedgerSeq)
        setValidLedger(ledger);
    if (!mPubLedger)
    {
        setPubLedger(ledger);
        //app_.getOrderBookDB().setup(ledger);
    }
    app_.getJobQueue().addJob(
    jtADVANCE, "tryFill", [this, ledger](Job& j) {
        tryFill(j, ledger);
    });
}
    
void LedgerMaster::setFullLedger(
    std::shared_ptr<Ledger const> const& ledger,
    bool isSynchronous,
    bool isCurrent)
{
    // A new ledger has been accepted as part of the trusted chain
    JLOG(m_journal.debug()) << "Ledger " << ledger->info().seq
                            << " accepted :" << ledger->info().hash;
    assert(ledger->stateMap().getHash().isNonZero());

    ledger->setValidated();
    ledger->setFull();

    if (isCurrent)
        mLedgerHistory.insert(ledger, true);

    {
        // Check the SQL database's entry for the sequence before this
        // ledger, if it's not this ledger's parent, invalidate it
        uint256 prevHash = getHashByIndex(ledger->info().seq - 1, app_);
        if (prevHash.isNonZero() && prevHash != ledger->info().parentHash)
            clearLedger(ledger->info().seq - 1);
    }

    pendSaveValidated(app_, ledger, isSynchronous, isCurrent);

    {
        std::lock_guard ml(mCompleteLock);
        mCompleteLedgers.insert(ledger->info().seq);
    }

    {
        std::lock_guard ml(m_mutex);

        if (ledger->info().seq > mValidLedgerSeq)
            setValidLedger(ledger);
        if (!mPubLedger)
        {
            setPubLedger(ledger);
            //app_.getOrderBookDB().setup(ledger);
        }

        if (ledger->info().seq != 0 && haveLedger(ledger->info().seq - 1))
        {
            // we think we have the previous ledger, double check
            auto prevLedger = getLedgerBySeq(ledger->info().seq - 1);

            if (!prevLedger ||
                (prevLedger->info().hash != ledger->info().parentHash))
            {
                JLOG(m_journal.warn())
                    << "Acquired ledger invalidates previous ledger: "
                    << (prevLedger ? "hashMismatch" : "missingLedger");
                fixMismatch(*ledger);
            }
        }
    }
}

void
LedgerMaster::failedSave(std::uint32_t seq, uint256 const& hash)
{
    clearLedger(seq);
    app_.getInboundLedgers().acquire(hash, seq, InboundLedger::Reason::GENERIC);
}

ripple::uint160
LedgerMaster::getNameInDB(
    LedgerIndex index,
    AccountID accountID,
    std::string sTableName)
{
    ripple::uint160 name;
    assert(accountID.isZero() == false);
    auto ledger = getLedgerBySeq(index);
    if (ledger)
    {
        auto tup = getTableEntry(*ledger, accountID, sTableName);
        auto pEntry = std::get<1>(tup);
        if (pEntry)
        {
            name = pEntry->getFieldH160(sfNameInDB);
        }
    }
    return name;
}

table_BaseInfo
LedgerMaster::getTableBaseInfo(
    LedgerIndex index,
    AccountID accountID,
    std::string sTableName)
{
    table_BaseInfo ret_baseInfo;
    assert(accountID.isZero() == false);
    auto ledger = getLedgerBySeq(index);
    if (ledger)
    {
        auto tup = getTableEntry(*ledger, accountID, sTableName);
        auto pEntry = std::get<1>(tup);
        if (pEntry != nullptr)
        {
            auto& table = *pEntry;
            if (table.isFieldPresent(sfNameInDB))
                ret_baseInfo.nameInDB = table.getFieldH160(sfNameInDB);
            if (table.isFieldPresent(sfCreateLgrSeq))
                ret_baseInfo.createLgrSeq =
                    table.getFieldU32(sfCreateLgrSeq);
            if (table.isFieldPresent(sfCreatedLedgerHash))
                ret_baseInfo.createdLedgerHash =
                    table.getFieldH256(sfCreatedLedgerHash);
            if (table.isFieldPresent(sfCreatedTxnHash))
                ret_baseInfo.createdTxnHash =
                    table.getFieldH256(sfCreatedTxnHash);
            if (table.isFieldPresent(sfPreviousTxnLgrSeq))
                ret_baseInfo.previousTxnLgrSeq =
                    table.getFieldU32(sfPreviousTxnLgrSeq);
            if (table.isFieldPresent(sfPrevTxnLedgerHash))
                ret_baseInfo.prevTxnLedgerHash =
                    table.getFieldH256(sfPrevTxnLedgerHash);

        }
    }
    return ret_baseInfo;
}

std::pair<ripple::uint256, error_code_i>
LedgerMaster::getLatestTxCheckHash(AccountID accountID, std::string sTableName)
{
    ripple::uint256 uTxCheckHash;
    error_code_i errCode = rpcUNKNOWN;
    // assert(accountID);
    auto ledger = getValidatedLedger();
    if (ledger)
    {
        auto tup = getTableEntry(*ledger, accountID, sTableName);
        auto pEntry = std::get<1>(tup);
        if (pEntry != nullptr)
        {
            uTxCheckHash = pEntry->getFieldH256(sfTxCheckHash);
        }
        if (uTxCheckHash.isZero())
        {
            errCode = rpcTAB_NOT_EXIST;  // Can't find the table in the chain.
        }
    }
    else
    {
        uTxCheckHash = beast::zero;
        errCode = rpcGET_LGR_FAILED;
    }

    return std::make_pair(uTxCheckHash, errCode);
}

std::pair<bool, error_code_i>
LedgerMaster::isAuthorityValid(
    AccountID accountID,
    AccountID ownerID,
    std::list<std::string> aTableName,
    TableRoleFlags roles)
{
    if (accountID.isZero() || ownerID.isZero() || aTableName.size() <= 0)
    {
        return std::make_pair(false, rpcINVALID_PARAMS);
    }

    // std::string errMsg = " does not have the right authority.";
    auto ledger = getValidatedLedger();
    if (ledger)
    {
        for (auto const& sCheckName : aTableName)
        {
            auto tup = getTableEntry(*ledger, ownerID, sCheckName);
            auto pEntry = std::get<1>(tup);
            if (pEntry != nullptr)
            {
                if (!hasAuthority(*ledger, ownerID, sCheckName, accountID, roles))
                    return std::make_pair(false, rpcTAB_UNAUTHORIZED);
            }
            else
                return std::make_pair(false, rpcTAB_NOT_EXIST);
        }
    }
    return std::make_pair(true, rpcSUCCESS);
}
std::tuple<bool, ripple::Blob, error_code_i>
LedgerMaster::getUserToken(
    std::shared_ptr<ReadView const> ledger,
    AccountID accountID,
    AccountID ownerID,
    std::string sTableName)
{
    std::string sToken, errMsg;

    assert(accountID.isZero() == false);
    if (ledger)
    {
        auto tupTable = getTableEntry(*ledger, ownerID, sTableName);
        if (std::get<1>(tupTable) == nullptr)
            return std::make_tuple(false, Blob(), rpcTAB_NOT_EXIST);

        bool bNeedToken = ripple::isConfidential(*ledger, ownerID, sTableName);
        if (!bNeedToken)
            return std::make_tuple(true, Blob(), rpcSUCCESS);

        auto tup = getUserAuthAndToken(*ledger, ownerID, sTableName, accountID);
        if (!std::get<0>(tup))
            return std::make_tuple(false, Blob(), rpcTAB_UNAUTHORIZED);

        auto token = std::get<2>(tup);
        if (token.size() > 0)
            return std::make_tuple(true, token, rpcSUCCESS);
        else
            return std::make_tuple(false, Blob(), rpcSLE_TOKEN_MISSING);
    }
    else
    {
        return std::make_tuple(false, Blob(), rpcGET_LGR_FAILED);
    }

    return std::make_tuple(false, Blob(), rpcUNKNOWN);
}

std::tuple<bool, ripple::uint256, error_code_i>
LedgerMaster::getUserFutureHash(AccountID accountID, const Json::Value& tx_json)
{
    auto ledger = getValidatedLedger();
    if (ledger)
    {
        ripple::uint256 futureHash;
        std::shared_ptr<SLE const> tableSleExist = nullptr;
        auto sTableName = tx_json[jss::Tables][0u][jss::Table][jss::TableName].asString();
        Keylet key = keylet::table(accountID, sTableName);
        if (auto sle = ledger->read(key))
        {
            tableSleExist = sle;
        }
        else
        {
            key = keylet::tablelist(accountID);
            tableSleExist = ledger->read(key);
        }
        if (tableSleExist && tableSleExist->isFieldPresent(sfFutureTxHash))
        {
            futureHash = tableSleExist->getFieldH256(sfFutureTxHash);
        }
        return std::make_tuple(true, futureHash, rpcSUCCESS);
    }
    else
    {
        return std::make_tuple(false, uint256(), rpcGET_LGR_FAILED);
    }

    return std::make_tuple(false, uint256(), rpcUNKNOWN);
}

bool
LedgerMaster::isConfidential(const STTx& tx)
{
    if (tx.getFieldU16(sfTransactionType) == ttSQLTRANSACTION)
    {
        auto vecTxs = app_.getMasterTransaction().getTxs(tx);
        for (auto& tx : vecTxs)
        {
            if (isConfidentialUnit(tx))
                return true;
        }
        return false;
    }
    else
    {
        return isConfidentialUnit(tx);
    }
}

std::uint32_t
LedgerMaster::getLastConsensusTime()
{
	return mLastConsensusTime.load();
}

void
LedgerMaster::processFullLedgerTask(std::shared_ptr<Ledger const> const& ledger)
{
    app_.getTableSync().CheckSyncTableTxs(ledger);
    app_.getTableStorage().TryTableStorage();
    app_.getTableAssistant().TryTableCheckHash();
    // app_.getOPs().TryCheckSubTx();
    app_.getTableTxAccumulator().trySweepCache();
}

bool
LedgerMaster::isConfidentialUnit(const STTx& tx)
{
    int opType = tx.getFieldU16(sfOpType);
    if (opType == T_CREATE)
    {
        if (tx.isFieldPresent(sfToken))
            return true;
        else
            return false;
    }
    else
    {
        AccountID owner = beast::zero;
        if (isSqlStatementOpType((TableOpType)opType))
        {
            owner = tx.getAccountID(sfOwner);
        }
        else
        {
            owner = tx.getAccountID(sfAccount);
        }

        auto const& sTxTables = tx.getFieldArray(sfTables);
        std::string sTxTableName =
            strCopy(sTxTables[0].getFieldVL(sfTableName));

        auto ledger = getValidatedLedger();
        if (ledger == NULL)
            return false;

        return ripple::isConfidential(*ledger, owner, sTxTableName);
    }

    return false;
}

void
LedgerMaster::checkSubChains()
{
    if (subChainInited_.exchange(true))
        return;

    if (app_.schemaId() != beast::zero)
        return;

    auto ledger = getValidatedLedger();
    auto sleIndex = ledger->read(keylet::schema_index());
    if (sleIndex)
    {
        auto& schemaIndexes = sleIndex->getFieldV256(sfSchemaIndexes);
        for (auto const& index : schemaIndexes)
        {
            auto key = Keylet(ltSCHEMA, index);
            auto sle = ledger->read(key);
            if (sle)
            {
                uint256 schemaId = sle->key();
                SchemaParams params{};
                params.readFromSle(sle);
                bool bShouldCreate = false;
                for (auto const& validator : params.validator_list)
                {
                    if (validator.first == app_.getValidationPublicKey())
                    {
                        bShouldCreate = true;
                        break;
                    }
                }
                if (bShouldCreate)
                {
                    if (!app_.getSchemaManager().contains(schemaId))
                    {
                        JLOG(m_journal.info())
                            << "Creating schema when checkSubChains:"
                            << schemaId;
                        auto schemaPath =
                            boost::filesystem::path(app_.config().SCHEMA_PATH) /
                            to_string(schemaId);
                        bool bForceCreate =
                            boost::filesystem::exists(schemaPath);
                        app_.getOPs().createSchema(sle, bForceCreate, true);
                    }
                }
                else
                {
                    if (app_.getSchemaManager().contains(schemaId))
                    {
                        JLOG(m_journal.info())
                            << "Removing schema when checkSubChains:"
                            << schemaId;
                        app_.app().getJobQueue().addJob(
                            jtSTOP_SCHEMA,
                            "StopSchema",
                            [this, schemaId](Job&) {
                                app_.app().doStopSchema(schemaId);
                            });
                    }
                }
            }
        }
    }
}

void
LedgerMaster::setLoadLedger(LedgerIndex const index)
{
    load_ledger_index_ = index;
}

void
LedgerMaster::checkLoadLedger()
{
    if (ledgerLoadInited.exchange(true))
        return;
    if (app_.getInboundLedgers().getCount() <= 0)
    {
        app_.getJobQueue().addJob(
            jtCheckLoadLedger, "LedgerMaster.checkLoadLedger", [this](Job&) {
                if (getValidLedgerIndex() > 1)
                {
                    JLOG(m_journal.warn()) << "checkLoadLedger load for :"
                                           << getValidLedgerIndex();
                    app_.getInboundLedgers().acquire(
                        mValidLedger.get()->info().hash,
                        mValidLedger.get()->seq(),
                        InboundLedger::Reason::GENERIC);
                }
                JLOG(m_journal.warn()) << "checkLoadLedger complete!";
            }, app_.doJobCounter());
    }
    else
    {
        app_.getJobQueue().addJob(
            jtCheckLoadLedger, "LedgerMaster::checkLoadLedger", [this](Job&) {
                if (load_ledger_index_ > 1)
                {
                    auto loadLedger = getLedgerBySeq(load_ledger_index_);
                    try
                    {
                        if (loadLedger && !loadLedger->walkLedger(m_journal))
                        {
                            JLOG(m_journal.fatal())
                                << "Ledger " << loadLedger->info().seq
                                << " is missing nodes.";
                            app_.getInboundLedgers().acquire(
                                loadLedger->info().hash,
                                loadLedger->info().seq,
                                InboundLedger::Reason::GENERIC);
                        }
                    }
                    catch (SHAMapMissingNode const& mn)
                    {
                        JLOG(m_journal.warn())
                            << "walkLedger exception for " << loadLedger->info().seq << ":"
                            << mn.what();
                        app_.getInboundLedgers().acquire(
                            loadLedger->info().hash,
                            loadLedger->info().seq,
                            InboundLedger::Reason::GENERIC);
                    }                    
                }
            }, app_.doJobCounter());
    }
}

int
LedgerMaster::heldTransactionSize()
{
    return mHeldTransactions.size();
}

void
LedgerMaster::checkUpdateOpenLedger()
{
    if (app_.openLedger().current()->seq() <= mValidLedgerSeq)
    {
        JLOG(m_journal.warn()) << "checkUpdateOpenLedger openLedger seq:"
                               << app_.openLedger().current()->seq()
                               << "<= mValidLedgerSeq:" << mValidLedgerSeq;
        auto const lastVal = getValidatedLedger();
        boost::optional<Rules> rules;
        if (lastVal)
            rules.emplace(*lastVal, app_.config().features);
        else
            rules.emplace(app_.config().features);

        auto retries = CanonicalTXSet({});
        app_.openLedger().accept(
            app_,
            *rules,
            lastVal,
            OrderedTxs({}),
            false,
            retries,
            tapNONE,
            "checkUpdate",
            nullptr);
    }
}

void
LedgerMaster::initGenesisLedger(std::shared_ptr<Ledger> const genesis)
{
    genesis->setValidated();
    setValidLedger(genesis);
    pendSaveValidated(app_, genesis, true, true);

    //tryAdvance();
}

void
LedgerMaster::advanceThread()
{
    std::unique_lock sl(m_mutex);
    assert(!mValidLedger.empty() && mAdvanceThread.load());

    JLOG(m_journal.trace()) << "advanceThread<";

    try
    {
        doAdvance(sl);
    }
    catch (std::exception const& e)
    {
        JLOG(m_journal.fatal()) << "doAdvance throws an exception:" << e.what();
    }

    mAdvanceThread.store(false);
    JLOG(m_journal.trace()) << "advanceThread>";
}

boost::optional<LedgerHash>
LedgerMaster::getLedgerHashForHistory(
    LedgerIndex index,
    InboundLedger::Reason reason)
{
    // Try to get the hash of a ledger we need to fetch for history
    boost::optional<LedgerHash> ret;
    auto const& l{reason == InboundLedger::Reason::SHARD ? mShardLedger
                                                         : mHistLedger};

    if (l && l->info().seq >= index)
    {
        ret = hashOfSeq(*l, index, m_journal);
        if (!ret)
            ret = walkHashBySeq(index, l, reason);
    }

    if (!ret)
        ret = walkHashBySeq(index, reason);

    return ret;
}

std::vector<std::shared_ptr<Ledger const>>
LedgerMaster::findNewLedgersToPublish(
    std::unique_lock<std::recursive_mutex>& sl)
{
    std::vector<std::shared_ptr<Ledger const>> ret;

    JLOG(m_journal.trace()) << "findNewLedgersToPublish<";

    // No valid ledger, nothing to do
    if (mValidLedger.empty())
    {
        JLOG(m_journal.trace()) << "No valid journal, nothing to publish.";
        return {};
    }

    if (!mPubLedger)
    {
        JLOG(m_journal.info())
            << "First published ledger will be " << mValidLedgerSeq;
        return {mValidLedger.get()};
    }

    if (mValidLedgerSeq > (mPubLedgerSeq + MAX_LEDGER_GAP))
    {
        JLOG(m_journal.warn()) << "Gap in validated ledger stream "
                               << mPubLedgerSeq << " - " << mValidLedgerSeq - 1;

        auto valLedger = mValidLedger.get();
        ret.push_back(valLedger);
        setPubLedger(valLedger);
        //app_.getOrderBookDB().setup(valLedger);

        return {valLedger};
    }

    if (mValidLedgerSeq <= mPubLedgerSeq)
    {
        JLOG(m_journal.trace()) << "No valid journal, nothing to publish.";
        return {};
    }

    int acqCount = 0;

    auto pubSeq = mPubLedgerSeq + 1;  // Next sequence to publish
    auto valLedger = mValidLedger.get();
    std::uint32_t valSeq = valLedger->info().seq;

    ScopedUnlock sul{sl};
    try
    {
        for (std::uint32_t seq = pubSeq; seq <= valSeq; ++seq)
        {
            JLOG(m_journal.trace())
                << "Trying to fetch/publish valid ledger " << seq;

            std::shared_ptr<Ledger const> ledger;
            // This can throw
            auto hash = hashOfSeq(*valLedger, seq, m_journal);
            // VFALCO TODO Restructure this code so that zero is not
            // used.
            if (!hash)
                hash = beast::zero;  // kludge
            if (seq == valSeq)
            {
                // We need to publish the ledger we just fully validated
                ledger = valLedger;
            }
            else if (hash->isZero())
            {
                JLOG(m_journal.fatal()) << "Ledger: " << valSeq
                                        << " does not have hash for " << seq;
                assert(false);
            }
            else
            {
                ledger = mLedgerHistory.getLedgerByHash(*hash);
            }

            // Can we try to acquire the ledger we need?
            if (!ledger && (++acqCount < ledger_fetch_size_))
                ledger = app_.getInboundLedgers().acquire(
                    *hash, seq, InboundLedger::Reason::GENERIC);

            // Did we acquire the next ledger we need to publish?
            if (ledger && (ledger->info().seq == pubSeq))
            {
                ledger->setValidated();
                ret.push_back(ledger);
                ++pubSeq;
            }
        }

        JLOG(m_journal.trace())
            << "ready to publish " << ret.size() << " ledgers.";
    }
    catch (std::exception const&)
    {
        JLOG(m_journal.error())
            << "Exception while trying to find ledgers to publish.";
    }

    return ret;
}

void
LedgerMaster::tryAdvance()
{
    std::lock_guard ml(m_mutex);

    // Can't advance without at least one fully-valid ledger
    mAdvanceWork = true;
    if (!mAdvanceThread.exchange(true) && !mValidLedger.empty())
    {
        app_.getJobQueue().addJob(
            jtADVANCE, "advanceLedger", [this](Job&) { advanceThread(); }, app_.doJobCounter());
    }
}

void
LedgerMaster::updatePaths(Job& job)
{
    {
        std::lock_guard ml(m_mutex);
        if (app_.getOPs().isNeedNetworkLedger())
        {
            --mPathFindThread;
            return;
        }
    }

    while (!job.shouldCancel())
    {
        std::shared_ptr<ReadView const> lastLedger;
        {
            std::lock_guard ml(m_mutex);

            if (!mValidLedger.empty() &&
                (!mPathLedger || (mPathLedger->info().seq != mValidLedgerSeq)))
            {  // We have a new valid ledger since the last full pathfinding
                mPathLedger = mValidLedger.get();
                lastLedger = mPathLedger;
            }
            else if (mPathFindNewRequest)
            {  // We have a new request but no new ledger
                lastLedger = app_.openLedger().current();
            }
            else
            {  // Nothing to do
                --mPathFindThread;
                return;
            }
        }

        if (!standalone_)
        {  // don't pathfind with a ledger that's more than 60 seconds old
            using namespace std::chrono;
            auto age = time_point_cast<seconds>(app_.timeKeeper().closeTime()) -
                lastLedger->info().closeTime;
            if (age > 1min)
            {
                JLOG(m_journal.debug())
                    << "Published ledger too old for updating paths";
                std::lock_guard ml(m_mutex);
                --mPathFindThread;
                return;
            }
        }

        try
        {
            app_.getPathRequests().updateAll(
                lastLedger, job.getCancelCallback());
        }
        catch (SHAMapMissingNode const& mn)
        {
            JLOG(m_journal.info()) << "During pathfinding: " << mn.what();
            if (lastLedger->open())
            {
                // our parent is the problem
                app_.getInboundLedgers().acquire(
                    lastLedger->info().parentHash,
                    lastLedger->info().seq - 1,
                    InboundLedger::Reason::GENERIC);
            }
            else
            {
                // this ledger is the problem
                app_.getInboundLedgers().acquire(
                    lastLedger->info().hash,
                    lastLedger->info().seq,
                    InboundLedger::Reason::GENERIC);
            }
        }
    }
}

bool
LedgerMaster::newPathRequest()
{
    std::unique_lock ml(m_mutex);
    mPathFindNewRequest = newPFWork("pf:newRequest", ml);
    return mPathFindNewRequest;
}

bool
LedgerMaster::isNewPathRequest()
{
    std::lock_guard ml(m_mutex);
    bool const ret = mPathFindNewRequest;
    mPathFindNewRequest = false;
    return ret;
}

// If the order book is radically updated, we need to reprocess all
// pathfinding requests.
bool
LedgerMaster::newOrderBookDB()
{
    std::unique_lock ml(m_mutex);
    mPathLedger.reset();

    return newPFWork("pf:newOBDB", ml);
}

/** A thread needs to be dispatched to handle pathfinding work of some kind.
 */
bool
LedgerMaster::newPFWork(
    const char* name,
    std::unique_lock<std::recursive_mutex>&)
{
    if (mPathFindThread < 2)
    {
        if (app_.getJobQueue().addJob(
                jtUPDATE_PF, name, [this](Job& j) { updatePaths(j); }, app_.doJobCounter()))
        {
            ++mPathFindThread;
        }
    }
    // If we're stopping don't give callers the expectation that their
    // request will be fulfilled, even if it may be serviced.
    return mPathFindThread > 0 && !isStopping();
}

Rules
LedgerMaster::getValidatedRules()
{
    // Once we have a guarantee that there's always a last validated
    // ledger then we can dispense with the if.

    // Return the Rules from the last validated ledger.
    if (auto const ledger = getValidatedLedger())
        return ledger->rules();

    return Rules(app_.config().features);
}

// This is the last ledger we published to clients and can lag the validated
// ledger.
std::shared_ptr<ReadView const>
LedgerMaster::getPublishedLedger()
{
    std::lock_guard lock(m_mutex);
    return mPubLedger;
}

std::string
LedgerMaster::getCompleteLedgers()
{
    std::lock_guard sl(mCompleteLock);
    return to_string(mCompleteLedgers);
}

boost::optional<NetClock::time_point>
LedgerMaster::getCloseTimeBySeq(LedgerIndex ledgerIndex)
{
    uint256 hash = getHashBySeq(ledgerIndex);
    return hash.isNonZero() ? getCloseTimeByHash(hash, ledgerIndex)
                            : boost::none;
}

boost::optional<NetClock::time_point>
LedgerMaster::getCloseTimeByHash(
    LedgerHash const& ledgerHash,
    LedgerIndex index)
{
    auto node = app_.getNodeStore().fetch(ledgerHash, index);
    if (node && (node->getData().size() >= 120))
    {
        SerialIter it(node->getData().data(), node->getData().size());
        if (safe_cast<HashPrefix>(it.get32()) == HashPrefix::ledgerMaster)
        {
            it.skip(
                4 + 8 + 32 +   // seq drops parentHash
                32 + 32 + 4);  // txHash acctHash parentClose
            return NetClock::time_point{NetClock::duration{it.get32()}};
        }
    }

    return boost::none;
}

uint256
LedgerMaster::getHashBySeq(std::uint32_t index)
{
    uint256 hash = mLedgerHistory.getLedgerHash(index);

    if (hash.isNonZero())
        return hash;

    return getHashByIndex(index, app_);
}

uint256
LedgerMaster::getHashBySeqEx(std::uint32_t index)
{
    boost::optional<LedgerHash> ledgerHash;

    if (auto referenceLedger = mValidLedger.get())
        ledgerHash = walkHashBySeq(
            index, referenceLedger, InboundLedger::Reason::GENERIC);

    if (ledgerHash == boost::none)
        return beast::zero;
    return ledgerHash.value();
}

boost::optional<LedgerHash>
LedgerMaster::walkHashBySeq(std::uint32_t index, InboundLedger::Reason reason)
{
    boost::optional<LedgerHash> ledgerHash;

    if (auto referenceLedger = mValidLedger.get())
        ledgerHash = walkHashBySeq(index, referenceLedger, reason);

    return ledgerHash;
}

boost::optional<LedgerHash>
LedgerMaster::walkHashBySeq(
    std::uint32_t index,
    std::shared_ptr<ReadView const> const& referenceLedger,
    InboundLedger::Reason reason)
{
    if (!referenceLedger || (referenceLedger->info().seq < index))
    {
        // Nothing we can do. No validated ledger.
        return boost::none;
    }

    // See if the hash for the ledger we need is in the reference ledger
    auto ledgerHash = hashOfSeq(*referenceLedger, index, m_journal);
    if (ledgerHash)
        return ledgerHash;

    // The hash is not in the reference ledger. Get another ledger which can
    // be located easily and should contain the hash.
    LedgerIndex refIndex = getCandidateLedger(index);
    auto const refHash = hashOfSeq(*referenceLedger, refIndex, m_journal);
    assert(refHash);
    if (refHash)
    {
        // Try the hash and sequence of a better reference ledger just found
        auto ledger = mLedgerHistory.getLedgerByHash(*refHash);

        if (ledger)
        {
            try
            {
                ledgerHash = hashOfSeq(*ledger, index, m_journal);
            }
            catch (SHAMapMissingNode const&)
            {
                ledger.reset();
            }
        }

        // Try to acquire the complete ledger
        if (!ledger)
        {
            if (auto const l = app_.getInboundLedgers().acquire(
                    *refHash, refIndex, reason))
            {
                ledgerHash = hashOfSeq(*l, index, m_journal);
                assert(ledgerHash);
            }
        }
    }
    return ledgerHash;
}

std::shared_ptr<Ledger const>
LedgerMaster::getLedgerBySeq(std::uint32_t index)
{
    if (index <= mValidLedgerSeq)
    {
        // Always prefer a validated ledger
        if (auto valid = mValidLedger.get())
        {
            if (valid->info().seq == index)
                return valid;

            try
            {
                auto const hash = hashOfSeq(*valid, index, m_journal);

                if (hash)
                    return mLedgerHistory.getLedgerByHash(*hash);
            }
            catch (std::exception const&)
            {
                // Missing nodes are already handled
            }
        }
    }

    if (auto ret = mLedgerHistory.getLedgerBySeq(index))
        return ret;

    auto ret = mClosedLedger.get();
    if (ret && (ret->info().seq == index))
        return ret;

    clearLedger(index);
    return {};
}

std::shared_ptr<Ledger const>
LedgerMaster::getLedgerByHash(uint256 const& hash)
{
    if (auto ret = mLedgerHistory.getLedgerByHash(hash))
        return ret;

    auto ret = mClosedLedger.get();
    if (ret && (ret->info().hash == hash))
        return ret;

    return {};
}

void
LedgerMaster::doLedgerCleaner(Json::Value const& parameters)
{
    mLedgerCleaner->doClean(parameters);
}

void
LedgerMaster::setLedgerRangePresent(std::uint32_t minV, std::uint32_t maxV)
{
    std::lock_guard sl(mCompleteLock);
    mCompleteLedgers.insert(range(minV, maxV));
}

void
LedgerMaster::tune(int size, std::chrono::seconds age)
{
    mLedgerHistory.tune(size, age);
}

void
LedgerMaster::sweep()
{
    mLedgerHistory.sweep();
    fetch_packs_.sweep();
}

float
LedgerMaster::getCacheHitRate()
{
    return mLedgerHistory.getCacheHitRate();
}

beast::PropertyStream::Source&
LedgerMaster::getPropertySource()
{
    return *mLedgerCleaner;
}

void
LedgerMaster::clearPriorLedgers(LedgerIndex seq)
{
    std::lock_guard sl(mCompleteLock);
    if (seq > 0)
        mCompleteLedgers.erase(range(0u, seq - 1));
}

void
LedgerMaster::clearLedgerCachePrior(LedgerIndex seq)
{
    mLedgerHistory.clearLedgerCachePrior(seq);
}

void
LedgerMaster::takeReplay(std::unique_ptr<LedgerReplay> replay)
{
    replayData = std::move(replay);
}

std::unique_ptr<LedgerReplay>
LedgerMaster::releaseReplay()
{
    return std::move(replayData);
}

void
LedgerMaster::fetchForHistory(
    std::uint32_t missing,
    bool& progress,
    InboundLedger::Reason reason,
    std::unique_lock<std::recursive_mutex>& sl)
{
    ScopedUnlock sul{sl};
    if (auto hash = getLedgerHashForHistory(missing, reason))
    {
        assert(hash->isNonZero());
        auto ledger = getLedgerByHash(*hash);
        if (!ledger)
        {
            if (!app_.getInboundLedgers().isFailure(*hash))
            {
                ledger =
                    app_.getInboundLedgers().acquire(*hash, missing, reason);
                if (!ledger && missing != fetch_seq_ &&
                    missing > app_.getNodeStore().earliestLedgerSeq())
                {
                    JLOG(m_journal.trace())
                        << "fetchForHistory want fetch pack " << missing;
                    fetch_seq_ = missing;
                    getFetchPack(missing, reason);
                }
                else
                    JLOG(m_journal.trace())
                        << "fetchForHistory no fetch pack for " << missing;
            }
            else
                JLOG(m_journal.debug())
                    << "fetchForHistory found failed acquire";
        }
        if (ledger)
        {
            auto seq = ledger->info().seq;
            assert(seq == missing);
            JLOG(m_journal.trace()) << "fetchForHistory acquired " << seq;
            if (reason == InboundLedger::Reason::SHARD)
            {
                ledger->setFull();
                {
                    std::lock_guard lock(m_mutex);
                    mShardLedger = ledger;
                }
                if (!ledger->stateMap().family().isShardBacked())
                    app_.getShardStore()->storeLedger(ledger);
            }
            else
            {
                setFullLedger(ledger, false, false);
                int fillInProgress;
                {
                    std::lock_guard lock(m_mutex);
                    mHistLedger = ledger;
                    fillInProgress = mFillInProgress;
                }
                if (fillInProgress == 0 &&
                    getHashByIndex(seq - 1, app_) == ledger->info().parentHash)
                {
                    {
                        // Previous ledger is in DB
                        std::lock_guard lock(m_mutex);
                        mFillInProgress = seq;
                    }
                    app_.getJobQueue().addJob(
                        jtADVANCE, "tryFill", [this, ledger](Job& j) {
                            tryFill(j, ledger);
                        }, app_.doJobCounter());
                }
            }
            progress = true;
        }
        else
        {
            std::uint32_t fetchSz;
            if (reason == InboundLedger::Reason::SHARD)
                // Do not fetch ledger sequences lower
                // than the shard's first ledger sequence
                fetchSz = app_.getShardStore()->firstLedgerSeq(
                    app_.getShardStore()->seqToShardIndex(missing));
            else
                // Do not fetch ledger sequences lower
                // than the earliest ledger sequence
                fetchSz = app_.getNodeStore().earliestLedgerSeq();
            fetchSz = missing >= fetchSz
                ? std::min(ledger_fetch_size_, (missing - fetchSz) + 1)
                : 0;
            try
            {
                for (std::uint32_t i = 0; i < fetchSz; ++i)
                {
                    std::uint32_t seq = missing - i;
                    if (auto h = getLedgerHashForHistory(seq, reason))
                    {
                        assert(h->isNonZero());
                        app_.getInboundLedgers().acquire(*h, seq, reason);
                    }
                }
            }
            catch (std::exception const&)
            {
                JLOG(m_journal.warn()) << "Threw while prefetching";
            }
        }
    }
    else
    {
        JLOG(m_journal.fatal())
            << "Can't find ledger following prevMissing " << missing;
        JLOG(m_journal.fatal())
            << "Pub:" << mPubLedgerSeq << " Val:" << mValidLedgerSeq;
        JLOG(m_journal.fatal())
            << "Ledgers: " << app_.getLedgerMaster().getCompleteLedgers();
        JLOG(m_journal.fatal())
            << "Acquire reason: "
            << (reason == InboundLedger::Reason::HISTORY ? "HISTORY" : "SHARD");
        clearLedger(missing + 1);
        progress = true;
    }
}

void
LedgerMaster::doValid(std::shared_ptr<Ledger const> const& ledger)
{
    ledger->setValidated();
    ledger->setFull();
    setValidLedger(ledger);

    checkSubChains();

    app_.getTxPool().removeTxs(
        ledger->txMap(), ledger->info().seq, ledger->info().parentHash);

    if (!mPubLedger)
    {
        pendSaveValidated(app_, ledger, true, true);
        setPubLedger(ledger);
        //app_.getOrderBookDB().setup(ledger);
    }

    std::uint32_t const base = app_.getFeeTrack().getLoadBase();
    auto fees = app_.getValidations().fees(ledger->info().hash, base);
    {
        auto fees2 =
            app_.getValidations().fees(ledger->info().parentHash, base);
        fees.reserve(fees.size() + fees2.size());
        std::copy(fees2.begin(), fees2.end(), std::back_inserter(fees));
    }
    std::uint32_t fee;
    if (!fees.empty())
    {
        std::sort(fees.begin(), fees.end());
        fee = fees[fees.size() / 2];  // median
    }
    else
    {
        fee = base;
    }

    app_.getFeeTrack().setRemoteFee(fee);

    tryAdvance();

    if (ledger->seq() % 256 == 0)
    {
        // Check if the majority of validators run a higher version rippled
        // software. If so print a warning.
        //
        // Once the HardenedValidations amendment is enabled, validators include
        // their rippled software version in the validation messages of every
        // (flag - 1) ledger. We wait for one ledger time before checking the
        // version information to accumulate more validation messages.

        auto currentTime = app_.timeKeeper().now();
        bool needPrint = false;

        // The variable upgradeWarningPrevTime_ will be set when and only when
        // the warning is printed.
        if (upgradeWarningPrevTime_ == TimeKeeper::time_point())
        {
            // Have not printed the warning before, check if need to print.
            auto const vals = app_.getValidations().getTrustedForLedger(
                ledger->info().parentHash);
            std::size_t higherVersionCount = 0;
            std::size_t rippledCount = 0;
            for (auto const& v : vals)
            {
                if (v->isFieldPresent(sfServerVersion))
                {
                    auto version = v->getFieldU64(sfServerVersion);
                    higherVersionCount +=
                        BuildInfo::isNewerVersion(version) ? 1 : 0;
                    rippledCount +=
                        BuildInfo::isRippledVersion(version) ? 1 : 0;
                }
            }
            // We report only if (1) we have accumulated validation messages
            // from 90% validators from the UNL, (2) 60% of validators
            // running the rippled implementation have higher version numbers,
            // and (3) the calculation won't cause divide-by-zero.
            if (higherVersionCount > 0 && rippledCount > 0)
            {
                constexpr std::size_t reportingPercent = 90;
                constexpr std::size_t cutoffPercent = 60;
                auto const unlSize{
                    app_.validators().getQuorumKeys().second.size()};
                needPrint = unlSize > 0 &&
                    calculatePercent(vals.size(), unlSize) >=
                        reportingPercent &&
                    calculatePercent(higherVersionCount, rippledCount) >=
                        cutoffPercent;
            }
        }
        // To throttle the warning messages, instead of printing a warning
        // every flag ledger, we print every week.
        else if (currentTime - upgradeWarningPrevTime_ >= weeks{1})
        {
            // Printed the warning before, and assuming most validators
            // do not downgrade, we keep printing the warning
            // until the local server is restarted.
            needPrint = true;
        }

        if (needPrint)
        {
            upgradeWarningPrevTime_ = currentTime;
            auto const upgradeMsg =
                "Check for upgrade: "
                "A majority of trusted validators are "
                "running a newer version.";
            std::cerr << upgradeMsg << std::endl;
            JLOG(m_journal.error()) << upgradeMsg;
        }
    }
}

// Try to publish ledgers, acquire missing ledgers
void
LedgerMaster::doAdvance(std::unique_lock<std::recursive_mutex>& sl)
{
    do
    {
        mAdvanceWork = false;  // If there's work to do, we'll make progress
        bool progress = false;

        auto const pubLedgers = findNewLedgersToPublish(sl);
        if (pubLedgers.empty())
        {
            if (!standalone_ && !app_.getFeeTrack().isLoadedLocal() &&
                (app_.getJobQueue().getJobCount(jtPUBOLDLEDGER) < 10) &&
                (mValidLedgerSeq == mPubLedgerSeq) &&
                (getValidatedLedgerAge() < MAX_LEDGER_AGE_ACQUIRE) &&
                (app_.getNodeStore().getWriteLoad() < MAX_WRITE_LOAD_ACQUIRE))
            {
                // We are in sync, so can acquire
                InboundLedger::Reason reason = InboundLedger::Reason::HISTORY;
                boost::optional<std::uint32_t> missing;
                {
                    std::lock_guard sll(mCompleteLock);
                    missing = prevMissing(
                        mCompleteLedgers,
                        mPubLedger->info().seq,
                        app_.getNodeStore().earliestLedgerSeq());
                }
                if (missing)
                {
                    JLOG(m_journal.trace())
                        << "tryAdvance discovered missing " << *missing;
                    if ((mFillInProgress == 0 || *missing > mFillInProgress) &&
                        shouldAcquire(
                            mValidLedgerSeq,
                            ledger_history_,
                            app_.getSHAMapStore().minimumOnline(),
                            *missing,
                            m_journal))
                    {
                        JLOG(m_journal.trace())
                            << "advanceThread should acquire";
                    }
                    else
                        missing = boost::none;
                }
                if (!missing && mFillInProgress == 0)
                {
                    if (auto shardStore = app_.getShardStore())
                    {
                        missing = shardStore->prepareLedger(mValidLedgerSeq);
                        if (missing)
                            reason = InboundLedger::Reason::SHARD;
                    }
                }
                if (missing)
                {
                    fetchForHistory(*missing, progress, reason, sl);
                    if (mValidLedgerSeq != mPubLedgerSeq)
                    {
                        JLOG(m_journal.debug())
                            << "tryAdvance found last valid changed";
                        progress = true;
                    }
                }
            }
            else
            {
                mHistLedger.reset();
                mShardLedger.reset();
                JLOG(m_journal.trace()) << "tryAdvance not fetching history";
            }
        }
        else
        {
            JLOG(m_journal.trace()) << "tryAdvance found " << pubLedgers.size()
                                    << " ledgers to publish";
            for (auto ledger : pubLedgers)
            {
                {
                    ScopedUnlock sul{sl};
                    JLOG(m_journal.info())
                        << "tryAdvance publishing seq " << ledger->info().seq;
                    setFullLedger(ledger, true, true);
                }

                setPubLedger(ledger);

                {
                    ScopedUnlock sul{sl};
                    app_.getOPs().pubLedger(ledger);
                }

                processFullLedgerTask(ledger);
            }
            // move table_sync here,cause it used pub_ledger
            app_.getTableSync().TryTableSync();

            app_.getOPs().clearNeedNetworkLedger();
            progress = newPFWork("pf:newLedger", sl);
        }
        if (progress)
            mAdvanceWork = true;
    } while (mAdvanceWork);
}

void
LedgerMaster::addFetchPack(uint256 const& hash, std::shared_ptr<Blob> data)
{
    fetch_packs_.canonicalize_replace_client(hash, data);
}

boost::optional<Blob>
LedgerMaster::getFetchPack(uint256 const& hash)
{
    Blob data;
    if (fetch_packs_.retrieve(hash, data))
    {
        fetch_packs_.del(hash, false);
        if (hash == sha512Half(makeSlice(data)))
            return data;
    }
    return boost::none;
}

void
LedgerMaster::gotFetchPack(bool progress, std::uint32_t seq)
{
    if (!mGotFetchPackThread.test_and_set(std::memory_order_acquire))
    {
        app_.getJobQueue().addJob(jtLEDGER_DATA, "gotFetchPack", [&](Job&) {
            app_.getInboundLedgers().gotFetchPack();
            mGotFetchPackThread.clear(std::memory_order_release);
        }, app_.doJobCounter());
    }
}

void
LedgerMaster::makeFetchPack(
    std::weak_ptr<Peer> const& wPeer,
    std::shared_ptr<protocol::TMGetObjectByHash> const& request,
    uint256 haveLedgerHash,
    UptimeClock::time_point uptime)
{
    using namespace std::chrono_literals;
    if (UptimeClock::now() > uptime + 1s)
    {
        JLOG(m_journal.info()) << "Fetch pack request got stale";
        return;
    }

    if (app_.getFeeTrack().isLoadedLocal() || (getValidatedLedgerAge() > 40s))
    {
        JLOG(m_journal.info()) << "Too busy to make fetch pack";
        return;
    }

    auto peer = wPeer.lock();

    if (!peer)
        return;

    auto haveLedger = getLedgerByHash(haveLedgerHash);

    if (!haveLedger)
    {
        JLOG(m_journal.info())
            << "Peer requests fetch pack for ledger we don't have: "
            << haveLedger;
        peer->charge(Resource::feeRequestNoReply);
        return;
    }

    if (haveLedger->open())
    {
        JLOG(m_journal.warn())
            << "Peer requests fetch pack from open ledger: " << haveLedger;
        peer->charge(Resource::feeInvalidRequest);
        return;
    }

    if (haveLedger->info().seq < getEarliestFetch())
    {
        JLOG(m_journal.debug()) << "Peer requests fetch pack that is too early";
        peer->charge(Resource::feeInvalidRequest);
        return;
    }

    auto wantLedger = getLedgerByHash(haveLedger->info().parentHash);

    if (!wantLedger)
    {
        JLOG(m_journal.info())
            << "Peer requests fetch pack for ledger whose predecessor we "
            << "don't have: " << haveLedger;
        peer->charge(Resource::feeRequestNoReply);
        return;
    }

    auto fpAppender = [](protocol::TMGetObjectByHash* reply,
                         std::uint32_t ledgerSeq,
                         SHAMapHash const& hash,
                         const Blob& blob) {
        protocol::TMIndexedObject& newObj = *(reply->add_objects());
        newObj.set_ledgerseq(ledgerSeq);
        newObj.set_hash(hash.as_uint256().begin(), 256 / 8);
        newObj.set_data(&blob[0], blob.size());
    };

    try
    {
        protocol::TMGetObjectByHash reply;
        reply.set_query(false);
        reply.set_schemaid(app_.schemaId().begin(), uint256::size());
        if (request->has_seq())
            reply.set_seq(request->seq());

        reply.set_ledgerhash(request->ledgerhash());
        reply.set_type(protocol::TMGetObjectByHash::otFETCH_PACK);

        // Building a fetch pack:
        //  1. Add the header for the requested ledger.
        //  2. Add the nodes for the AccountStateMap of that ledger.
        //  3. If there are transactions, add the nodes for the
        //     transactions of the ledger.
        //  4. If the FetchPack now contains greater than or equal to
        //     256 entries then stop.
        //  5. If not very much time has elapsed, then loop back and repeat
        //     the same process adding the previous ledger to the FetchPack.
        do
        {
            std::uint32_t lSeq = wantLedger->info().seq;

            protocol::TMIndexedObject& newObj = *reply.add_objects();
            newObj.set_hash(wantLedger->info().hash.data(), 256 / 8);
            Serializer s(256);
            s.add32(HashPrefix::ledgerMaster);
            addRaw(wantLedger->info(), s);
            newObj.set_data(s.getDataPtr(), s.getLength());
            newObj.set_ledgerseq(lSeq);

            wantLedger->stateMap().getFetchPack(
                &haveLedger->stateMap(),
                true,
                16384,
                std::bind(
                    fpAppender,
                    &reply,
                    lSeq,
                    std::placeholders::_1,
                    std::placeholders::_2));

            if (wantLedger->info().txHash.isNonZero())
                wantLedger->txMap().getFetchPack(
                    nullptr,
                    true,
                    512,
                    std::bind(
                        fpAppender,
                        &reply,
                        lSeq,
                        std::placeholders::_1,
                        std::placeholders::_2));

            if (reply.objects().size() >= 512)
                break;

            // move may save a ref/unref
            haveLedger = std::move(wantLedger);
            wantLedger = getLedgerByHash(haveLedger->info().parentHash);

            if (!wantLedger)
            {
                JLOG(m_journal.warn()) << "Cannot read ledger when building "
                                          "fetch patch, LedgerSeq="
                                       << haveLedger->info().seq - 1;
            }

        } while (wantLedger && UptimeClock::now() <= uptime + 1s);

        JLOG(m_journal.info())
            << "Built fetch pack with " << reply.objects().size() << " nodes";
        auto msg = std::make_shared<Message>(reply, protocol::mtGET_OBJECTS);
        peer->send(msg);
    }
    catch (std::exception const& e)
    {
        JLOG(m_journal.warn())
            << "Exception building fetch patch :" << e.what();
    }
}

std::size_t
LedgerMaster::getFetchPackCacheSize() const
{
    return fetch_packs_.getCacheSize();
}

// Returns the minimum ledger sequence in SQL database, if any.
boost::optional<LedgerIndex>
LedgerMaster::minSqlSeq()
{
    boost::optional<LedgerIndex> seq;
    auto db = app_.getLedgerDB().checkoutDb();
    *db << "SELECT MIN(LedgerSeq) FROM Ledgers WHERE LedgerSeq > 1", soci::into(seq);
    return seq;
}

}  // namespace ripple
