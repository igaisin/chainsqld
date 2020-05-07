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

#include <peersafe/app/shard/Node.h>
#include <ripple/app/misc/NetworkOPs.h>
#include <ripple/app/misc/HashRouter.h>
#include <ripple/app/misc/LoadFeeTrack.h>
#include <ripple/app/consensus/RCLConsensus.h>
#include <ripple/app/consensus/RCLValidations.h>
#include <peersafe/app/shard/FinalLedger.h>
#include <peersafe/app/shard/ShardManager.h>
#include <ripple/app/ledger/LedgerMaster.h>
#include <ripple/app/misc/HashRouter.h>
#include <ripple/protocol/digest.h>

#include <ripple/overlay/Peer.h>
#include <ripple/overlay/impl/PeerImp.h>

namespace ripple {

Node::Node(ShardManager& m, Application& app, Config& cfg, beast::Journal journal)
    : mShardManager(m)
    , app_(app)
    , journal_(journal)
    , cfg_(cfg)
{
    // TODO
	if ( ShardManager::COMMITTEE == m.myShardRole()) {
		mShardID = CommitteeShardID;

	}else if (ShardManager::SHARD == m.myShardRole()){
		mShardID = cfg_.SHARD_INDEX;
	}

	std::vector< std::vector<std::string> >& shardValidators = cfg_.SHARD_VALIDATORS;
	for (size_t i = 0; i < shardValidators.size(); i++) {

		// shardIndex = index + 1
		mMapOfShardValidators[i+1] = std::make_unique<ValidatorList>(
			app_.validatorManifests(), app_.publisherManifests(), app_.timeKeeper(),
			journal_, cfg_.VALIDATION_QUORUM);

		std::vector<std::string>  publisherKeys;
		// Setup trusted validators
		if (!mMapOfShardValidators[i+1]->load(
			app_.getValidationPublicKey(),
			shardValidators[i],
			publisherKeys)){
			//JLOG(m_journal.fatal()) <<
			//	"Invalid entry in validator configuration.";
			//return false;
			mMapOfShardValidators.erase(i+1);
		}

	}

	



}

void Node::addActive(std::shared_ptr<PeerImp> const& peer)
{
	std::lock_guard <decltype(mPeersMutex)> lock(mPeersMutex);


	std::uint32_t index = peer->getShardIndex();

	auto iter = mMapOfShardPeers.find(index);
	if (iter == mMapOfShardPeers.end()) {

		std::vector<std::weak_ptr <PeerImp>>		peers;
		peers.emplace_back(std::move(peer));

		mMapOfShardPeers.emplace(
			std::piecewise_construct,
			std::make_tuple(index),
			std::make_tuple(peers));
	}
	else {

		iter->second.emplace_back(std::move(peer));
	}


}

void Node::eraseDeactivate(Peer::id_t id)
{
	std::lock_guard <decltype(mPeersMutex)> lock(mPeersMutex);

	bool bErase = false;

	for (auto item : mMapOfShardPeers) {

		if(bErase) break;
		
		// id is unique
		auto position = item.second.begin();
		while (position != item.second.end()) {

			auto spt = position->lock();
			if (spt->id() == id) {
				item.second.erase(position);
				bErase = true;
				break;
			}
			position++;
		}
	}

}

bool Node::isLeader(PublicKey const& pubkey, LedgerIndex curSeq, uint64 view)
{
    if (mMapOfShardValidators.find(mShardID) != mMapOfShardValidators.end())
    {
        auto const& validators = mMapOfShardValidators[mShardID]->validators();
        assert(validators.size());
        int index = (view + curSeq) % validators.size();
        return pubkey == validators[index];
    }

    return false;
}

inline auto Node::validatorsPtr()
	-> std::unique_ptr<ValidatorList>&
{
    assert(mMapOfShardValidators.find(mShardID) != mMapOfShardValidators.end());
    return mMapOfShardValidators[mShardID];
}

std::size_t Node::quorum()
{
    if (mMapOfShardValidators.find(mShardID) != mMapOfShardValidators.end())
    {
        return mMapOfShardValidators[mShardID]->quorum();
    }

    return std::numeric_limits<std::size_t>::max();
}

std::int32_t Node::getPubkeyIndex(PublicKey const& pubkey)
{
    if (mMapOfShardValidators.find(mShardID) != mMapOfShardValidators.end())
    {
        auto const & validators = mMapOfShardValidators[mShardID]->validators();
        for (std::int32_t idx = 0; idx < validators.size(); idx++)
        {
            if (validators[idx] == pubkey)
            {
                return idx;
            }
        }
    }

    return -1;
}

void Node::onConsensusStart(LedgerIndex seq, uint64 view, PublicKey const pubkey)
{
    assert(mShardID > CommitteeShardID && mShardID != InvalidShardID);

    mIsLeader = false;

    auto iter = mMapOfShardValidators.find(mShardID);
	if (iter == mMapOfShardValidators.end()) {
		assert(0);
		return ;
	}

	auto const& validators = iter->second->validators();
	assert(validators.size() > 0);
	int index = (view + seq) % validators.size();

	mIsLeader = (pubkey == validators[index]);
    

    mMicroLedger.reset();

    {
        std::lock_guard<std::recursive_mutex> lock(mSignsMutex);
        mSignatureBuffer.erase(seq - 1);
    }

    iter->second->onConsensusStart (
        app_.getValidations().getCurrentPublicKeys ());
}

void Node::doAccept(
    RCLTxSet const& set,
    RCLCxLedger const& previousLedger,
    NetClock::time_point closeTime)
{
    closeTime = std::max<NetClock::time_point>(closeTime, previousLedger.closeTime() + 1s);

    auto buildLCL =
        std::make_shared<Ledger>(*previousLedger.ledger_, closeTime);

    {
        OpenView accum(&*buildLCL);
        assert(!accum.open());

        // Normal case, we are not replaying a ledger close
        applyTransactions(
            app_, set, accum, [&buildLCL](uint256 const& txID) {
            return !buildLCL->txExists(txID);
        });

        mMicroLedger.emplace(mShardID, accum.info().seq, accum);
    }

    commitSignatureBuffer();

    JLOG(journal_.info()) << "MicroLedger: " << mMicroLedger->ledgerHash();

    validate(*mMicroLedger);

    // See if we can submit this micro ledger.
    checkAccept();
}

void Node::commitSignatureBuffer()
{
    assert(mMicroLedger);

    std::lock_guard<std::recursive_mutex> lock_(mSignsMutex);

    auto iter = mSignatureBuffer.find(mMicroLedger->seq());
    if (iter != mSignatureBuffer.end())
    {
        for (auto it = iter->second.begin(); it != iter->second.end(); ++it)
        {
            if (std::get<0>(*it) == mMicroLedger->ledgerHash())
            {
                mMicroLedger->addSignature(std::get<1>(*it), std::get<2>(*it));
            }
        }
    }
}

void Node::validate(MicroLedger &microLedger)
{
    RCLConsensus::Adaptor& adaptor = app_.getOPs().getConsensus().adaptor_;
    auto validationTime = app_.timeKeeper().closeTime();
    if (validationTime <= adaptor.lastValidationTime_)
        validationTime = adaptor.lastValidationTime_ + 1s;
    adaptor.lastValidationTime_ = validationTime;

    // Build validation
    auto v = std::make_shared<STValidation>(
        microLedger.ledgerHash(), validationTime, adaptor.valPublic_, true);

    v->setFieldU32(sfLedgerSequence, microLedger.seq());
    v->setFieldU32(sfShardID, microLedger.shardID());

    // Add our load fee to the validation
    auto const& feeTrack = app_.getFeeTrack();
    std::uint32_t fee =
        std::max(feeTrack.getLocalFee(), feeTrack.getClusterFee());

    if (fee > feeTrack.getLoadBase())
        v->setFieldU32(sfLoadFee, fee);

    auto const signingHash = v->sign(adaptor.valSecret_);
    v->setTrusted();

    // suppress it if we receive it - FIXME: wrong suppression
    app_.getHashRouter().addSuppression(signingHash);

    handleNewValidation(app_, v, "local");

    Blob validation = v->getSerialized();
    protocol::TMValidation val;
    val.set_validation(&validation[0], validation.size());

    // Send signed validation to all of our directly connected peers
    auto const m = std::make_shared<Message>(
        val, protocol::mtVALIDATION);
    sendMessage(mShardID, m);
}

void Node::recvValidation(PublicKey& pubKey, STValidation& val)
{
    LedgerIndex seq = val.getFieldU32(sfLedgerSequence);
    uint256 microLedgerHash = val.getFieldH256(sfLedgerHash);

    if (seq <= app_.getLedgerMaster().getValidLedgerIndex())
    {
        JLOG(journal_.warn()) << "Validation for ledger seq(seq) from "
            << toBase58(TokenType::TOKEN_NODE_PUBLIC, pubKey) << " is stale";
        return;
    }

    if (mMicroLedger)
    {
        if (mMicroLedger->seq() == seq &&
            mMicroLedger->ledgerHash() == microLedgerHash)
        {
            std::lock_guard<std::recursive_mutex> lock(mSignsMutex);

            mMicroLedger->addSignature(pubKey, val.getFieldVL(sfMicroLedgerSign));
            return;
        }
    }

    // Buffer it
    {
        std::lock_guard<std::recursive_mutex> lock(mSignsMutex);

        if (mSignatureBuffer.find(seq) != mSignatureBuffer.end())
        {
            mSignatureBuffer[seq].push_back(std::make_tuple(microLedgerHash, pubKey, val.getFieldVL(sfMicroLedgerSign)));
        }
        else
        {
            std::vector<std::tuple<uint256, PublicKey, Blob>> v;
            v.push_back(std::make_tuple(microLedgerHash, pubKey, val.getFieldVL(sfMicroLedgerSign)));
            mSignatureBuffer.emplace(seq, std::move(v));
        }
    }
}

void Node::checkAccept()
{
    assert(mMapOfShardValidators.find(mShardID) != mMapOfShardValidators.end());

    size_t signCount = 0;

    {
        std::lock_guard<std::recursive_mutex> lock(mSignsMutex);
        signCount = mMicroLedger->signatures().size();
    }

    if (signCount >= mMapOfShardValidators[mShardID]->quorum())
    {
        submitMicroLedger(false);

        app_.getOPs().getConsensus().consensus_->setPhase(ConsensusPhase::waitingFinalLedger);
    }
}

void Node::submitMicroLedger(bool withTxMeta)
{
    protocol::TMMicroLedgerSubmit ms;

    auto suppressionKey = sha512Half(mMicroLedger->ledgerHash(), withTxMeta);

    if (!app_.getHashRouter().shouldRelay(suppressionKey))
    {
        return;
    }

    mMicroLedger->compose(ms, withTxMeta);

    auto const m = std::make_shared<Message>(
        ms, protocol::mtMICROLEDGER_SUBMIT);

    if (withTxMeta)
    {
        mShardManager.lookup().sendMessage(m);
    }
    else
    {
        mShardManager.committee().sendMessage(m);
    }
}

void Node::sendMessage(uint32 shardID, std::shared_ptr<Message> const &m)
{
    std::lock_guard<std::recursive_mutex> _(mPeersMutex);

    if (mMapOfShardPeers.find(shardID) != mMapOfShardPeers.end())
    {
        for (auto w : mMapOfShardPeers[shardID])
        {
            if (auto p = w.lock())
                p->send(m);
        }
    }
}

void Node::sendMessage(std::shared_ptr<Message> const &m)
{
    std::lock_guard<std::recursive_mutex> _(mPeersMutex);

    for (auto const& it : mMapOfShardPeers)
    {
        sendMessage(it.first, m);
    }
}

Overlay::PeerSequence Node::getActivePeers(uint32 shardID)
{
    Overlay::PeerSequence ret;

    std::lock_guard<std::recursive_mutex> lock(mPeersMutex);

    if (mMapOfShardPeers.find(shardID) != mMapOfShardPeers.end())
    {
        ret.reserve(mMapOfShardPeers[shardID].size());

        for (auto w : mMapOfShardPeers[shardID])
        {
            if (auto p = w.lock())
            {
                ret.emplace_back(std::move(p));
            }
        }
    }

    return ret;
}

void Node::onMessage(protocol::TMFinalLedgerSubmit const& m)
{
	auto finalLedger = std::make_shared<FinalLedger>(m);

    if (!app_.getHashRouter().shouldRelay(finalLedger->ledgerHash()))
    {
        return;
    }

    auto previousLedger = app_.getLedgerMaster().getValidatedLedger();

    if (finalLedger->seq() != previousLedger->seq() + 1)
    {
        return;
    }

	if (!finalLedger->checkValidity(mShardManager.committee().validatorsPtr()))
	{
        JLOG(journal_.info()) << "FinalLeger signature verification failed";
		return;
	}

	// build new ledger
	auto ledgerInfo = finalLedger->getLedgerInfo();
	auto buildLCL = std::make_shared<Ledger>(*previousLedger, ledgerInfo.closeTime);
	finalLedger->apply(*buildLCL);

    buildLCL->updateSkipList();

    // Write the final version of all modified SHAMap
    // nodes to the node store to preserve the new Ledger
    // Note,didn't save tx-shamap,so don't load tx-shamap when load ledger.
    auto timeStart = utcTime();
    int asf = buildLCL->stateMap().flushDirty(
        hotACCOUNT_NODE, buildLCL->info().seq);
    int tmf = buildLCL->txMap().flushDirty(
        hotTRANSACTION_NODE, buildLCL->info().seq);
    JLOG(journal_.debug()) << "Flushed " << asf << " accounts and " << tmf
        << " transaction nodes";
    JLOG(journal_.info()) << "flushDirty time used:" << utcTime() - timeStart << "ms";

    // check hash
    if (ledgerInfo.accountHash != buildLCL->stateMap().getHash().as_uint256() ||
        ledgerInfo.txHash != buildLCL->txMap().getHash().as_uint256())
    {
        JLOG(journal_.warn()) << "Final ledger txs/accounts shamap root hash missmatch";
        return;
    }

    submitMicroLedger(true);

    buildLCL->unshare();
    buildLCL->setAccepted(ledgerInfo.closeTime, ledgerInfo.closeTimeResolution, true, app_.config());

    app_.getLedgerMaster().storeLedger(buildLCL);

    app_.getOPs().getConsensus().adaptor_.notify(protocol::neACCEPTED_LEDGER, RCLCxLedger{std::move(buildLCL)}, true);

    app_.getLedgerMaster().setBuildingLedger(0);
	//save ledger
	app_.getLedgerMaster().accept(buildLCL);

	//begin next round consensus
	app_.getOPs().endConsensus();
}

void Node::onMessage(protocol::TMTransactions const& m)
{
    using beast::hash_append;

    auto const publicKey = parseBase58<PublicKey>(
        TokenType::TOKEN_NODE_PUBLIC, m.nodepubkey());
    if (!publicKey)
    {
        JLOG(journal_.info()) << "Transactions package from lookup has illegal pubkey";
        return;
    }

    boost::optional<PublicKey> pubKey =
        mShardManager.lookup().validators().getTrustedKey(*publicKey);
    if (!pubKey)
    {
        JLOG(journal_.info()) << "Transactions package from untrusted lookup node";
        return;
    }

    sha512_half_hasher checkHash;
    std::vector<std::shared_ptr<Transaction>> txs;

    for (auto const& TMTransaction : m.transactions())
    {
        SerialIter sit(makeSlice(TMTransaction.rawtransaction()));
        auto stx = std::make_shared<STTx const>(sit);
        std::string reason;
        txs.emplace_back(std::make_shared<Transaction>(stx, reason, app_));
        hash_append(checkHash, stx->getTransactionID());
    }

    if (!verifyDigest(
        *pubKey,
        static_cast<typename sha512_half_hasher::result_type>(checkHash),
        makeSlice(m.signature())))
    {
        JLOG(journal_.info()) << "Transactions package signature verification failed";
        return;
    }

    for (auto tx : txs)
    {
        app_.getOPs().processTransaction(
            tx, false, false, NetworkOPs::FailHard::no);
    }
}


}