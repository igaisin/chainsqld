//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2012-2014 Ripple Labs Inc.

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

#include <ripple/app/ledger/AcceptedLedger.h>
#include <ripple/app/ledger/InboundLedgers.h>
#include <ripple/app/ledger/LedgerMaster.h>
#include <peersafe/schema/Schema.h>
#include <ripple/app/misc/NetworkOPs.h>
#include <ripple/basics/UptimeClock.h>
#include <ripple/core/DatabaseCon.h>
#include <ripple/json/json_value.h>
#include <ripple/ledger/CachedSLEs.h>
#include <ripple/net/RPCErr.h>
#include <ripple/nodestore/Database.h>
#include <ripple/nodestore/DatabaseShard.h>
#include <ripple/protocol/ErrorCodes.h>
#include <ripple/protocol/jss.h>
#include <ripple/rpc/Context.h>
#include <ripple/shamap/ShardFamily.h>
#include <peersafe/app/misc/ConnectionPool.h>
#include <peersafe/app/sql/TxnDBConn.h>

namespace ripple {

static void
textTime(
    std::string& text,
    UptimeClock::time_point& seconds,
    const char* unitName,
    std::chrono::seconds unitVal)
{
    auto i = seconds.time_since_epoch() / unitVal;

    if (i == 0)
        return;

    seconds -= unitVal * i;

    if (!text.empty())
        text += ", ";

    text += std::to_string(i);
    text += " ";
    text += unitName;

    if (i > 1)
        text += "s";
}

Json::Value
getCountsJson(Schema& app, int minObjectCount)
{
    auto objectCounts = CountedObjects::getInstance().getCounts(minObjectCount);

    Json::Value ret(Json::objectValue);

    for (auto const& [k, v] : objectCounts)
    {
        ret[k] = v;
    }

    int dbKB = getKBUsedAll(app.getLedgerDB().getSession());

    if (dbKB > 0)
        ret[jss::dbKBTotal] = dbKB;

    dbKB = getKBUsedDB(app.getLedgerDB().getSession());

    if (dbKB > 0)
        ret[jss::dbKBLedger] = dbKB;

    if (app.config().useTxTables())
        dbKB = getKBUsedDB (app.getTxnDB ().getSession ());

    if (dbKB > 0)
        ret[jss::dbKBTransaction] = dbKB;

    {
        std::size_t c = app.getOPs().getLocalTxCount();
        if (c > 0)
            ret[jss::local_txs] = static_cast<Json::UInt>(c);
    }

    ret[jss::write_load] = app.getNodeStore().getWriteLoad();

    ret[jss::historical_perminute] =
        static_cast<int>(app.getInboundLedgers().fetchRate());
    ret[jss::SLE_hit_rate] = app.cachedSLEs().rate();
    ret[jss::node_hit_rate] = app.getNodeStore().getCacheHitRate();
    ret[jss::ledger_hit_rate] = app.getLedgerMaster().getCacheHitRate();
    ret[jss::AL_hit_rate] = app.getAcceptedLedgerCache().getHitRate();
    ret["Connection_Count_In_Pool"] = app.getConnectionPool().count();
    ret["AcceptedLedgerCacheSize"] =
        app.getAcceptedLedgerCache().getCacheSize();
    ret["LedgerHistorySize"] =
        app.getLedgerMaster().getLedgerHistory().getCacheSize();
    ret["HeldTransactionSize"] = app.getLedgerMaster().heldTransactionSize();

    ret["state_leafset_cache_size"] =
        static_cast<int> (app.getNodeFamily().getStateNodeHashSet()->size());
    ret[jss::fullbelow_size] =
        static_cast<int>(app.getNodeFamily().getFullBelowCache(0)->size());
    ret[jss::treenode_cache_size] =
        app.getNodeFamily().getTreeNodeCache(0)->getCacheSize();
    ret[jss::treenode_track_size] =
        app.getNodeFamily().getTreeNodeCache(0)->getTrackSize();
    // ret[jss::cachedSLE_size] = static_cast<unsigned int>(app.cachedSLEs().size());

    std::string uptime;
    auto s = UptimeClock::now();
    using namespace std::chrono_literals;
    textTime(uptime, s, "year", 365 * 24h);
    textTime(uptime, s, "day", 24h);
    textTime(uptime, s, "hour", 1h);
    textTime(uptime, s, "minute", 1min);
    textTime(uptime, s, "second", 1s);
    ret[jss::uptime] = uptime;

    ret[jss::node_writes] = app.getNodeStore().getStoreCount();
    ret[jss::node_reads_total] = app.getNodeStore().getFetchTotalCount();
    ret[jss::node_reads_hit] = app.getNodeStore().getFetchHitCount();
    ret[jss::node_written_bytes] = app.getNodeStore().getStoreSize();
    ret[jss::node_read_bytes] = app.getNodeStore().getFetchSize();

    if (auto shardStore = app.getShardStore())
    {
        auto getShardFamily{dynamic_cast<ShardFamily*>(app.getShardFamily())};
        auto const [cacheSz, trackSz] = getShardFamily->getTreeNodeCacheSize();
        Json::Value& jv = (ret[jss::shards] = Json::objectValue);

        jv[jss::fullbelow_size] = getShardFamily->getFullBelowCacheSize();
        jv[jss::treenode_cache_size] = cacheSz;
        jv[jss::treenode_track_size] = trackSz;
        ret[jss::write_load] = shardStore->getWriteLoad();
        ret[jss::node_hit_rate] = shardStore->getCacheHitRate();
        jv[jss::node_writes] = shardStore->getStoreCount();
        jv[jss::node_reads_total] = shardStore->getFetchTotalCount();
        jv[jss::node_reads_hit] = shardStore->getFetchHitCount();
        jv[jss::node_written_bytes] = shardStore->getStoreSize();
        jv[jss::node_read_bytes] = shardStore->getFetchSize();
    }

    return ret;
}

// {
//   min_count: <number>  // optional, defaults to 10
// }
Json::Value
doGetCounts(RPC::JsonContext& context)
{
    int minCount = 10;

    if (context.params.isMember(jss::min_count))
        minCount = context.params[jss::min_count].asUInt();

    return getCountsJson(context.app, minCount);
}

}  // namespace ripple
