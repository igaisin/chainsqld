//------------------------------------------------------------------------------
/*
This file is part of chainsqld: https://github.com/chainsql/chainsqld
Copyright (c) 2016-2018 Peersafe Technology Co., Ltd.

chainsqld is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

chainsqld is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
You should have received a copy of the GNU General Public License
along with cpp-ethereum.  If not, see <http://www.gnu.org/licenses/>.
*/
//==============================================================================

#ifndef PEERSAFE_APP_SHARD_LOOKUP_H_INCLUDED
#define PEERSAFE_APP_SHARD_LOOKUP_H_INCLUDED

#include "ripple.pb.h"
#include <ripple/basics/UnorderedContainers.h>
#include <ripple/beast/utility/Journal.h>
#include <ripple/overlay/impl/PeerImp.h>
#include <ripple/app/misc/ValidatorList.h>
#include <peersafe/app/shard/MicroLedger.h>
#include <peersafe/app/shard/FinalLedger.h>

#include <mutex>

namespace ripple {

class Application;
class Config;
class ShardManager;

class PeerImp;
class Peer;

class Lookup {

private:

    // Hold all lookup peers
	hash_map<Peer::id_t, std::weak_ptr<PeerImp>>		mPeers;
	std::mutex											mPeersMutex;


    // Hold all Lookup validators
    std::unique_ptr <ValidatorList>                     mValidators;


    ShardManager&                                       mShardManager;

    Application&                                        app_;
    beast::Journal                                      journal_;
    Config&                                             cfg_;

	std::recursive_mutex								mutex_;

	using MapFinalLedger = std::map<LedgerIndex, std::shared_ptr<FinalLedger>>;
	MapFinalLedger										mMapFinalLedger;

	using MapMicroLedger = std::map<uint32, std::shared_ptr<MicroLedger>>;
	std::map<LedgerIndex, MapMicroLedger>				mMapMicroLedgers;
public:

    Lookup(ShardManager& m, Application& app, Config& cfg, beast::Journal journal);
    ~Lookup() {}

    inline hash_map<Peer::id_t, std::weak_ptr<PeerImp>>& peers()
    {
        return mPeers;
    }

    inline ValidatorList& validators()
    {
        return *mValidators;
    }

	void addActive(std::shared_ptr<PeerImp> const& peer);

	void eraseDeactivate(Peer::id_t id);

    void onMessage(protocol::TMMicroLedgerSubmit const& m);
    void onMessage(protocol::TMFinalLedgerSubmit const& m);

	void checkSaveLedger();
	void resetMetaIndex(LedgerIndex seq);
	void saveLedger(LedgerIndex seq);
};

}

#endif
