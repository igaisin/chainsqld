#include <peersafe/app/tx/SchemaTx.h>
#include <peersafe/schema/SchemaParams.h>
#include <ripple/protocol/STTx.h>
#include <ripple/ledger/View.h>
#include <ripple/protocol/st.h>


namespace ripple {

	TER preclaimCommon(PreclaimContext const& ctx)
	{
		auto const& vals = ctx.tx.getFieldArray(sfValidators);
		for (auto val : vals)
		{
			// check the construct of the validtors object
			if (val.getCount() != 1 || !val.isFieldPresent(sfValidator))
			{
				return temBAD_VALIDATOR;
			}
			auto const& oVal = val.getFieldObject(sfValidator);
			if (oVal.getCount() != 1 || !oVal.isFieldPresent(sfPublicKey) || oVal.getFieldVL(sfPublicKey).size() == 0)
			{
				return temBAD_VALIDATOR;
			}
		}

		auto const& peers = ctx.tx.getFieldArray(sfPeerList);
		for (auto peer : peers)
		{
			// check the construct of the validtors object
			if (peer.getCount() != 1 || !peer.isFieldPresent(sfPeer))
			{
				return temBAD_PEERLIST;
			}

			auto const& oPeer = peer.getFieldObject(sfPeer);
			if (oPeer.getCount() != 1 || !oPeer.isFieldPresent(sfEndpoint) || oPeer.getFieldVL(sfEndpoint).size() == 0)
			{
				return temBAD_PEERLIST;
			}
		}

		if (ctx.tx.getSigningPubKey().empty())
		{
			STArray const & vals = ctx.tx.getFieldArray(sfValidators);
			STArray const& txSigners(ctx.tx.getFieldArray(sfSigners));

			for (auto const& txSigner : txSigners)
			{
				auto const &spk = txSigner.getFieldVL(sfSigningPubKey);

				auto iter(vals.end());
				iter = std::find_if(vals.begin(), vals.end(),
					[spk](STObject const &val) {
					auto const& oVal = val.getFieldObject(sfValidator);
					if (oVal.getFieldVL(sfPublicKey) == spk) return true;
					return false;
				});
				if (iter == vals.end())
				{
					return temBAD_SIGNERFORVAL;
				}
			}
		}

		return tesSUCCESS;
	}


	NotTEC SchemaCreate::preflight(PreflightContext const& ctx)
	{
		auto const ret = preflight1(ctx);
		if (!isTesSuccess(ret))
			return ret;

		if( !ctx.tx.isFieldPresent(sfSchemaName) ||
			!ctx.tx.isFieldPresent(sfSchemaStrategy) ||
			!ctx.tx.isFieldPresent(sfValidators) ||
			!ctx.tx.isFieldPresent(sfPeerList))
			return temMALFORMED;

		return preflight2(ctx);
	}

	TER SchemaCreate::preclaim(PreclaimContext const& ctx)
	{
		auto j = ctx.app.journal("preclaimSchema");

		if ((uint8_t)SchemaStragegy::with_state == ctx.tx.getFieldU8(sfSchemaStrategy) && !ctx.tx.isFieldPresent(sfAnchorLedgerHash))
		{
			JLOG(j.trace()) << "anchor ledger is not match the schema strategy.";
			return temBAD_ANCHORLEDGER;
		}

		if (ctx.tx.getFieldArray(sfValidators).size() <= 0 || ctx.tx.getFieldArray(sfPeerList).size() <= 0)
		{
			return temMALFORMED;
		}

		return preclaimCommon(ctx);
		
	}

	TER SchemaCreate::doApply()
	{
		auto j = ctx_.app.journal("schemaCreateApply");

		auto const account = ctx_.tx[sfAccount];
		auto const sle = ctx_.view().peek(keylet::account(account));
		// Create schema in ledger
		auto const slep = std::make_shared<SLE>(
			keylet::schema(account, (*sle)[sfSequence] - 1, ctx_.view().info().parentHash));
		(*slep)[sfAccount] = account;
		(*slep)[sfSchemaName] = ctx_.tx[sfSchemaName];
		(*slep)[sfSchemaStrategy] = ctx_.tx[sfSchemaStrategy];
		(*slep)[~sfSchemaAdmin] = ctx_.tx[~sfSchemaAdmin];
		(*slep)[~sfAnchorLedgerHash] = ctx_.tx[~sfAnchorLedgerHash];

		STArray vals = ctx_.tx.getFieldArray(sfValidators);
		
		// Multi-Sign
		if (ctx_.tx.getSigningPubKey().empty())
		{
			// Get the array of transaction signers.
			STArray const& txSigners(ctx_.tx.getFieldArray(sfSigners));
			
			for (auto& val : vals)
			{
				auto& oVal = val.peekFieldObject(sfValidator);
				oVal.setFieldU8(sfSigned, (uint8_t)0);

				auto const &spk = oVal.getFieldVL(sfPublicKey);
				for (auto const& txSigner : txSigners)
				{
					if (txSigner.getFieldVL(sfSigningPubKey) == spk)
					{
						oVal.setFieldU8(sfSigned, 1);
					}
				}
			}	
		}
		slep->setFieldArray(sfValidators, vals);

		STArray const& peerList = ctx_.tx.getFieldArray(sfPeerList);
		slep->setFieldArray(sfPeerList, peerList);
		
		ctx_.view().insert(slep);

		// Add schema to sender's owner directory
		{
			auto page = dirAdd(ctx_.view(), keylet::ownerDir(account), slep->key(),
				false, describeOwnerDir(account), ctx_.app.journal("View"));
			if (!page)
				return tecDIR_FULL;
			(*slep)[sfOwnerNode] = *page;
		}
		adjustOwnerCount(ctx_.view(), sle, 1, ctx_.journal);
		ctx_.view().update(sle);

		JLOG(j.trace()) << "schema sle is created.";

		return tesSUCCESS;
	}

	//------------------------------------------------------------------------------

	NotTEC SchemaModify::preflight(PreflightContext const& ctx)
	{
		auto const ret = preflight1(ctx);
		if (!isTesSuccess(ret))
			return ret;

		if (!ctx.tx.isFieldPresent(sfSchemaName) ||
			!ctx.tx.isFieldPresent(sfOpType)     ||
			!ctx.tx.isFieldPresent(sfValidators) ||
			!ctx.tx.isFieldPresent(sfPeerList)   ||
			!ctx.tx.isFieldPresent(sfSchemaID))
			return temMALFORMED;

		return preflight2(ctx);
	}

	TER SchemaModify::preclaim(PreclaimContext const& ctx)
	{
		auto j = ctx.app.journal("schemaModifyPreclaim");

		if (ctx.tx.getFieldU16(sfOpType) != (UINT16)SchemaModifyOp::add && ctx.tx.getFieldU16(sfOpType) != (UINT16)SchemaModifyOp::del)
		{
			JLOG(j.trace()) << "modify operator is not valid.";
			return temBAD_OPTYPE;
		}

		if (ctx.tx.getFieldArray(sfValidators).size() <= 0 && ctx.tx.getFieldArray(sfPeerList).size() <= 0)
		{
			return temMALFORMED;
		}

		return preclaimCommon(ctx);
	}

	TER SchemaModify::doApply()
	{
		auto j = ctx_.app.journal("schemaModifyApply");
		auto sleSchema = ctx_.view().peek(Keylet(ltSCHEMA, ctx_.tx.getFieldH256(sfSchemaID)));
		//for sle
		auto & peers = sleSchema->peekFieldArray(sfPeerList);
		auto & vals = sleSchema->peekFieldArray(sfValidators);

		//auto const account = ctx_.tx[sfAccount];

		//for tx
		STArray const & peersTx = ctx_.tx.getFieldArray(sfPeerList);
		STArray const & valsTx  = ctx_.tx.getFieldArray(sfValidators);

		for (auto const& valTx : valsTx)
		{
			auto iter(vals.end());
			iter = std::find_if(vals.begin(), vals.end(),
				[valTx](STObject const &val) {
				auto const& oVal = val.getFieldObject(sfValidator);
				auto const& spk = oVal.getFieldVL(sfPublicKey);

				auto const& oValTx = valTx.getFieldObject(sfValidator);
				auto const& spkTx = oValTx.getFieldVL(sfPublicKey);

				if (spk == spkTx) return true;
				return false;
			});
			if (ctx_.tx.getFieldU16(sfOpType) == (UINT16)SchemaModifyOp::add)
			{
				if (iter != vals.end())
				{
					return tefSCHEMA_VALIDATOREXIST;
				}
				vals.push_back(valTx);
			}
			else
			{
				if (iter == vals.end())
				{
					return tefSCHEMA_NOVALIDATOR;
				}
				vals.erase(iter);
			}
		}

		for (auto const& peerTx : peersTx)
		{
			auto iter(peers.end());
			iter = std::find_if(peers.begin(), peers.end(),
				[peerTx](STObject const &peer) {
				auto const& oPeer = peer.getFieldObject(sfPeer);
				auto const& sEndpoint = oPeer.getFieldVL(sfEndpoint);

				auto const& oPeerTx = peerTx.getFieldObject(sfPeer);
				auto const& sEndpointTx = oPeerTx.getFieldVL(sfEndpoint);

				if (sEndpoint == sEndpointTx) return true;
				return false;
			});
			if (ctx_.tx.getFieldU16(sfOpType) == (UINT16)SchemaModifyOp::add)
			{
				if (iter != vals.end())
				{
					return tefSCHEMA_PEEREXIST;
				}
				peers.push_back(peerTx);
			}
			else
			{
				if (iter == vals.end())
				{
					return tefSCHEMA_NOPEER;
				}
				peers.erase(iter);
			}
		}

		ctx_.view().update(sleSchema);
		return tesSUCCESS;
	}
}
