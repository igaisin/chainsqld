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

#include <peersafe/app/shard/Committee.h>

#include <ripple/core/Config.h>
#include <ripple/core/ConfigSections.h>

#include <ripple/overlay/Peer.h>
#include <ripple/overlay/impl/PeerImp.h>

namespace ripple {

Committee::Committee(ShardManager& m, Application& app, Config& cfg, beast::Journal journal)
    : mShardManager(m)
    , app_(app)
    , journal_(journal)
    , cfg_(cfg)
{
    // TODO


	mValidators = std::make_unique<ValidatorList>(
		app_.validatorManifests(), app_.publisherManifests(), app_.timeKeeper(),
		journal_, cfg_.VALIDATION_QUORUM);


	std::vector<std::string>  publisherKeys;
	// Setup trusted validators
	if (!mValidators->load(
		app_.getValidationPublicKey(),
		cfg_.section(SECTION_COMMITTEE_VALIDATORS).values(),
		publisherKeys))
	{
		//JLOG(m_journal.fatal()) <<
		//	"Invalid entry in validator configuration.";
		//return false;
	}
}

void Committee::addActive(std::shared_ptr<PeerImp> const& peer)
{
	std::lock_guard <decltype(mPeersMutex)> lock(mPeersMutex);
	auto const result = mPeers.emplace(
		std::piecewise_construct,
		std::make_tuple(peer->id()),
		std::make_tuple(peer));
	assert(result.second);
	(void)result.second;
}

void Committee::eraseDeactivate(Peer::id_t id)
{
	std::lock_guard <decltype(mPeersMutex)> lock(mPeersMutex);
	mPeers.erase(id);
}

void Committee::onConsensusStart(LedgerIndex seq, uint64 view, PublicKey const pubkey)
{
    //mMicroLedger.reset();

    auto const& validators = mValidators->validators();
    assert(validators.size() > 0);
    int index = (view + seq) % validators.size();

    mIsLeader = (pubkey == validators[index]);

}

void Committee::sendMessage(std::shared_ptr<Message> const &m)
{
    std::lock_guard<std::mutex> lock(mPeersMutex);

    for (auto w : mPeers)
    {
        if (auto p = w.second.lock())
            p->send(m);
    }
}

void Committee::onMessage(protocol::TMMicroLedgerSubmit const& m)
{

}

}
