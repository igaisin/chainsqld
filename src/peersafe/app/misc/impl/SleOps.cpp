#include <peersafe/app/misc/SleOps.h>
#include <ripple/protocol/digest.h>
#include <ripple/protocol/TxFormats.h>
#include <ripple/protocol/Quality.h>
#include <ripple/app/tx/apply.h>
#include <ripple/ledger/ApplyViewImpl.h>
#include <ripple/app/misc/NetworkOPs.h>
#include <ripple/beast/core/LexicalCast.h>
#include <peersafe/app/tx/DirectApply.h>
#include <peersafe/app/misc/ContractHelper.h> 
#include <ripple/basics/StringUtilities.h>
#include <ripple/app/ledger/LedgerMaster.h>
#include <peersafe/rpc/TableUtils.h>
#include <ripple/rpc/handlers/Handlers.h>
#include <peersafe/app/sql/TxStore.h>
#include <peersafe/core/Tuning.h>
#include <ripple/json/json_reader.h>
#include <ripple/json/json_writer.h>
#include <ripple/rpc/impl/RPCHelpers.h>
#include <ripple/protocol/Feature.h>
#include <peersafe/protocol/STMap256.h>
#include <eth/vm/VMFace.h>

namespace ripple {
    //just raw function for zxc, all paras should be tranformed in extvmFace modules.

    SleOps::SleOps(ApplyContext& ctx)
        :ctx_(ctx),
		bTransaction_(false)
    {
    }

	SleOps::~SleOps()
	{
		releaseResource();
	}

    SLE::pointer SleOps::getSle(AccountID const & addr) const
    {        
        ApplyView& view = ctx_.view();
        auto const k = keylet::account(addr);
        return view.peek(k);
    }

	void SleOps::incSequence(AccountID const& addr)
	{
		SLE::pointer pSle = getSle(addr);
		if (pSle)
		{
			uint32_t sequence = pSle->getFieldU32(sfSequence);
			{
				pSle->setFieldU32(sfSequence, ++sequence);
				ctx_.view().update(pSle);
			}
		}		
	}

	uint32_t SleOps::getSequence(AccountID const& addr)
	{
		SLE::pointer pSle = getSle(addr);
		if (pSle)
			return pSle->getFieldU32(sfSequence);
		else
			return 0;
	}

	const STTx& SleOps::getTx()
	{
		return ctx_.tx;
	}

	bool SleOps::addressHasCode(AccountID const& addr)
	{
		SLE::pointer pSle = getSle(addr);
		if (pSle && pSle->isFieldPresent(sfContractCode))
			return true;
		return false;
	}

	void SleOps::setCode(AccountID const& addr, eth::bytes&& code)
	{
		SLE::pointer pSle = getSle(addr);
		if(pSle)
			pSle->setFieldVL(sfContractCode,code);
	}

	eth::bytes const& SleOps::code(AccountID const& addr) 	
    {
		if (contractCacheCode_.find(addr) == contractCacheCode_.end())
		{
			SLE::pointer pSle = getSle(addr);
			if (!pSle)
			{				
                static Blob staticBlob;
                return staticBlob;
			}
			contractCacheCode_.emplace(addr, pSle->getFieldVL(sfContractCode));
		}
		return contractCacheCode_[addr];
	}

	uint256 SleOps::codeHash(AccountID const& addr)
	{
        eth::bytes const& code = SleOps::code(addr);
		return sha512Half(makeSlice(code));
	}

	size_t SleOps::codeSize(AccountID const& addr)
    {
        if (ctx_.app.getPreContractFace().isPrecompiledDiy(addr))
        {
            return 1;
		}
		eth::bytes const& code = SleOps::code(addr);
        return code.size();    
	}

    TER
    SleOps::transferBalance(
        AccountID const& _from,
        AccountID const& _to,
        uint256 const& _value)
    {
        if (_value == uint256(0))
            return tesSUCCESS;

        if (auto res = checkAuthority(_from, lsfPaymentAuth, _to);
            res != tesSUCCESS)
            return res;

        int64_t value = fromUint256(_value);
        auto ret = subBalance(_from, value, addressHasCode(_from));
        if (ret == tesSUCCESS)
            addBalance(_to, value);
        return ret;
    }

	TER SleOps::doPayment(AccountID const& _from, AccountID const& _to, uint256 const& _value)
	{
		int64_t value = fromUint256(_value);
		STTx paymentTx(ttPAYMENT,
			[&_from,&_to,&value](auto& obj)
		{
			obj.setAccountID(sfAccount, _from);
			obj.setAccountID(sfDestination, _to);
			obj.setFieldAmount(sfAmount, ZXCAmount(value));
		});
        paymentTx.setParentTxID(ctx_.tx.getTransactionID());
		auto ret = applyDirect(ctx_.app, ctx_.view(), paymentTx, ctx_.app.journal("Executive"));
		return ret;
	}

	// gateWay  currency transfer
	ripple::TER SleOps::doPayment(AccountID const& _from, AccountID const& _to, std::string const& _value, std::string const& _sendMax, std::string const& _sCurrency, AccountID const& _issuer)
	{
		try
		{
            if (addressHasCode(_from))
            {
                // from is contract
                if (_from != ctx_.tx.getAccountID(sfContractAddress))
                {
                    return tefMISMATCH_CONTRACT_ADDRESS;
                }
            }
            else
            {
                // from is user
                if (_from != ctx_.tx.getAccountID(sfAccount))
                {
                    return tefMISMATCH_TRANSACTION_ADDRESS;
                }
            }
			STTx paymentTx(ttPAYMENT,
				[&_from, &_to, &_value, &_sendMax, &_sCurrency, &_issuer](auto& obj)
			{
				obj.setAccountID(sfAccount, _from);
				obj.setAccountID(sfDestination, _to);

				obj.setFieldAmount(sfSendMax, ripple::amountFromString(Issue{ to_currency(_sCurrency),_issuer }, _sendMax));
				obj.setFieldAmount(sfAmount, ripple::amountFromString(Issue{ to_currency(_sCurrency),_issuer }, _value));
			});
			paymentTx.setParentTxID(ctx_.tx.getTransactionID());
			auto ret = applyDirect(ctx_.app, ctx_.view(), paymentTx, ctx_.app.journal("Executive"));
			return ret;
		}
		catch (std::exception const& e)
		{
			auto j = ctx_.app.journal("Executive");
			JLOG(j.fatal()) << e.what();
			return tefEXCEPTION;
		}
	}

	TER SleOps::createContractAccount(AccountID const& _from, AccountID const& _to, uint256 const& _value)
	{
		// Open a ledger for editing.
		auto const k = keylet::account(_to);
		SLE::pointer sleDst = ctx_.view().peek(k);

		if (!sleDst)
		{
			// Create the account.
			sleDst = std::make_shared<SLE>(k);
			sleDst->setAccountID(sfAccount, _to);
            std::uint32_t const seqno{
				ctx_.view().rules().enabled(featureDeletableAccounts) ? ctx_.view().seq(): 1};
			sleDst->setFieldU32(sfSequence, seqno);

			//Add to directory
            auto viewJ = ctx_.app.journal("Executive");
            auto result = dirAdd(
                ctx_.view(),
                keylet::contract_index(),
                k.key,
                false,
                [](std::shared_ptr<SLE> const& sle) {},
                viewJ);
            STMap256& mapExtension = sleDst->peekFieldM256(sfStorageExtension);
            mapExtension[NODE_TYPE_CONTRACTKEY] = uint256(*result);
            if (!result)
                return tecDIR_FULL;
            ctx_.view().insert(sleDst);
		}

		if (_value != uint256(0))
		{
			return transferBalance(_from, _to, _value);
		}

		return tesSUCCESS;
	}

	void SleOps::addBalance(AccountID const& addr, int64_t const& amount)
	{
		SLE::pointer pSle = getSle(addr);
		if (pSle)
		{
			auto balance = pSle->getFieldAmount(sfBalance).zxc().drops();
			int64_t finalBanance = balance + amount;
			if (amount > 0)
			{
				pSle->setFieldAmount(sfBalance, ZXCAmount(finalBanance));
				ctx_.view().update(pSle);
			}			
		}
	}

    TER
    SleOps::subBalance(
        AccountID const& addr,
        int64_t const& amount,
        bool isContract)
    {
        SLE::pointer pSle = getSle(addr);
        if (pSle)
        {
            // This is the total reserve in drops.
            auto const uOwnerCount = pSle->getFieldU32(sfOwnerCount);
            auto const reserve =
                ctx_.view().fees().accountReserve(uOwnerCount, isContract);

            auto balance = pSle->getFieldAmount(sfBalance).zxc().drops();
            int64_t finalBanance = balance - amount;
            // no reserve demand for contract
            if (finalBanance >= reserve.drops() ||
                (pSle->isFieldPresent(sfContractCode) && finalBanance >= 0))
            {
                pSle->setFieldAmount(sfBalance, ZXCAmount(finalBanance));
                ctx_.view().update(pSle);
            }
            else
                return tecUNFUNDED_PAYMENT;
        }
        return tesSUCCESS;
    }

	int64_t SleOps::balance(AccountID const& address)
	{
		SLE::pointer pSle = getSle(address);
		if (pSle)
			return pSle->getFieldAmount(sfBalance).zxc().drops();
		else
			return 0;
	}

	void SleOps::clearStorage(AccountID const& _contract)
	{
		SLE::pointer pSle = getSle(_contract);
		if (pSle)
		{
			pSle->makeFieldAbsent(sfContractCode);
			ctx_.view().update(pSle);
		}			
	}

    void SleOps::PubContractEvents(const AccountID& contractID, uint256 const * aTopic, int iTopicNum, const Blob& byValue)
    {     
        //add log to tx
        Json::Value jsonTopic, jsonLog;
        for (int i = 0; i < iTopicNum; i++)
        {
            jsonTopic.append(to_string(aTopic[i]));            
        }
        jsonLog[jss::contract_topics] = jsonTopic;
        std::string strData(byValue.begin(), byValue.end());
        jsonLog[jss::contract_data] = strHex(strData);
        getTx().addLog(jsonLog);

        jsonLog[jss::account] = to_string(contractID);
        auto j = ctx_.app.journal("Executive");        
        JLOG(j.debug()) << "Contract log or event: " << jsonLog;

        //getTx().
        ctx_.app.getOPs().PubContractEvents(contractID, aTopic, iTopicNum, byValue);
    }

    void SleOps::kill(AccountID addr)
    {
		SLE::pointer pSle = getSle(addr);
		if (pSle)
			ctx_.view().erase(pSle);
    }

	int64_t SleOps::executeSQL(AccountID const& _account, AccountID const& _owner, TableOpType _iType, std::string _sTableName, std::string _sRaw)
	{
		return 0;
	}

	void SleOps::addCommonFields(STObject& obj,AccountID const& _account)
	{
		obj.setAccountID(sfAccount, _account);

		obj.setFieldU32(sfSequence, 0);
		obj.setFieldAmount(sfFee, STAmount());
		obj.setFieldVL(sfSigningPubKey, Slice(nullptr, 0));
	}

	std::pair<bool, STArray> SleOps::genTableFields(const ApplyContext &_ctx, AccountID const& _account, std::string _sTableName, std::string _tableNewName, bool bNewNameInDB)
	{
		STArray tables;
		STObject table(sfTable);
		table.setFieldVL(sfTableName, strCopy(_sTableName));
		if(!_tableNewName.empty())
			table.setFieldVL(sfTableNewName, strCopy(_tableNewName));
		if (bNewNameInDB)
		{
			auto nameInDB = generateNameInDB(_ctx.view().seq(), _account, _sTableName);
			table.setFieldH160(sfNameInDB, nameInDB);
			if (bTransaction_)
				sqlTxsNameInDB_.emplace(_sTableName, nameInDB);
		}
		else
		{
			uint160 nameInDB = (uint160)0;
			if (bTransaction_)
			{
				if(sqlTxsNameInDB_.find(_sTableName) != sqlTxsNameInDB_.end())
					nameInDB = sqlTxsNameInDB_.at(_sTableName);
			}
			//
			if(!nameInDB)
			{
				auto tup = getTableEntry(_ctx.view(), _account, _sTableName);
				auto pEntry = std::get<1>(tup);
				if (!pEntry)
				{
					auto j = _ctx.app.journal("Executive");
					JLOG(j.info())
						<< "SleOps genTableFields getNameInDB failed,account="
						<< to_string(_account)
						<< ",tableName="
						<< _sTableName;
					return std::make_pair(false, tables);
				}
				nameInDB = pEntry->getFieldH160(sfNameInDB);
			}
			//
			table.setFieldH160(sfNameInDB, nameInDB);
		}
		
		tables.push_back(table);
		return std::make_pair(true, tables);

	}

	int64_t SleOps::disposeTableTx(STTx tx, AccountID const& _account, std::string _sTableName, std::string _tableNewName, bool bNewNameInDB)
	{
		AccountID ownerAccountID = _account;
		std::string sAccount = ripple::toBase58(ownerAccountID);
		if (tx.isFieldPresent(sfOwner))
			ownerAccountID = tx.getAccountID(sfOwner);
		std::string sOwner = ripple::toBase58(ownerAccountID);
		auto tables = genTableFields(ctx_, ownerAccountID, _sTableName, _tableNewName, bNewNameInDB);
		if(!tables.first)
			return tefTABLE_NOTEXIST;
		//
		tx.setFieldArray(sfTables, tables.second);
		SleOps::addCommonFields(tx, _account);
		//
		auto j = ctx_.app.journal("Executive");
		JLOG(j.trace()) <<
			"SleOps --- disposeTableTx subTx: " << tx;
		if (bTransaction_) {
			sqlTxsStatements_.push_back(tx);
			return tesSUCCESS;
		}
		auto ret = applyDirect(ctx_.app, ctx_.view(), tx, ctx_.app.journal("SleOps"));
		if (ret != tesSUCCESS)
		{
			auto j = ctx_.app.journal("Executive");
			JLOG(j.warn())
				<< "SleOps disposeTableTx,apply result:"
				<< transToken(ret);
		}
		if (ctx_.view().flags() & tapForConsensus)
		{
			ctx_.tx.addSubTx(tx);
		}
		return TERtoInt(ret);
	}

	//table operation
	int64_t SleOps::createTable(AccountID const& _account, std::string const& _sTableName, std::string const& _raw)
	{
//		const ApplyContext &_ctx = ctx_;
		STTx tx(ttTABLELISTSET,
			[&_raw](auto& obj)
		{

			obj.setFieldU16(sfOpType, T_CREATE);
			obj.setFieldVL(sfRaw, strCopy(_raw));
		});
        tx.setParentTxID(ctx_.tx.getTransactionID());

		return disposeTableTx(tx, _account, _sTableName, "", true);
	}

	int64_t SleOps::dropTable(AccountID const& _account, std::string const& _sTableName)
	{
//		const ApplyContext &_ctx = ctx_;
		STTx tx(ttTABLELISTSET,
			[&_account](auto& obj)
		{
			obj.setFieldU16(sfOpType, T_DROP);
			obj.setAccountID(sfAccount, _account);
		});
        tx.setParentTxID(ctx_.tx.getTransactionID());
		return disposeTableTx(tx, _account, _sTableName);
	}

	int64_t SleOps::renameTable(AccountID const& _account, std::string const& _sTableName, std::string const& _sTableNewName)
	{
//		const ApplyContext &_ctx = ctx_;
		STTx tx(ttTABLELISTSET,
			[&_account](auto& obj)
		{
			SleOps::addCommonFields(obj, _account);
			//
			obj.setFieldU16(sfOpType, T_RENAME);
			obj.setAccountID(sfAccount, _account);
		});
        tx.setParentTxID(ctx_.tx.getTransactionID());


		return disposeTableTx(tx, _account, _sTableName, _sTableNewName);
	}
	
	int64_t SleOps::grantTable(AccountID const& _account, AccountID const& _account2, std::string const& _sTableName, std::string const& _raw)
	{
//		const ApplyContext &_ctx = ctx_;
		STTx tx(ttTABLELISTSET,
			[&_account, &_account2, &_raw](auto& obj)
		{
			SleOps::addCommonFields(obj, _account);
			//
			obj.setFieldU16(sfOpType, T_GRANT);
			obj.setAccountID(sfAccount, _account);
			obj.setAccountID(sfUser, _account2);
            std::string _sRaw;
            if (_raw[0] == '[')
                _sRaw = _raw;     
            else
				_sRaw = "[" + _raw + "]";
			
			obj.setFieldVL(sfRaw, strCopy(_sRaw));
		});
        tx.setParentTxID(ctx_.tx.getTransactionID());

		return disposeTableTx(tx, _account, _sTableName);
	}

	int64_t SleOps::updateFieldsTable(AccountID const& _account, TableOpType& _opType, std::string const& _sTableName, std::string const& _raw)
    {
//		const ApplyContext &_ctx = ctx_;
		STTx tx(ttTABLELISTSET,
			[&_account, &_opType, &_raw](auto& obj)
		{
			SleOps::addCommonFields(obj, _account);
			//
			obj.setFieldU16(sfOpType, _opType);
			obj.setAccountID(sfAccount, _account);
			obj.setFieldVL(sfRaw, strCopy(_raw));
		});
        tx.setParentTxID(ctx_.tx.getTransactionID());
		return disposeTableTx(tx, _account, _sTableName);
    }

	//CRUD operation
    int64_t
    SleOps::insertData(
            AccountID const& _account,
            AccountID const& _owner,
            std::string const& _sTableName,
            std::string const& _raw,
            std::string const& _autoFillField)
	{
//		const ApplyContext &_ctx = ctx_;
		STTx tx(ttSQLSTATEMENT,
			[&_account, &_owner, &_raw,&_autoFillField](auto& obj)
		{
			SleOps::addCommonFields(obj, _account);
			//
			obj.setFieldU16(sfOpType, R_INSERT);
			obj.setAccountID(sfAccount, _account);
			obj.setAccountID(sfOwner, _owner);
			obj.setFieldVL(sfRaw, strCopy(_raw));
            if (_autoFillField != "")
            {
                obj.setFieldVL(sfAutoFillField, strCopy(_autoFillField));      
			}
		});
        tx.setParentTxID(ctx_.tx.getTransactionID());
		//
		return disposeTableTx(tx, _account, _sTableName);
	}

	int64_t SleOps::deleteData(AccountID const& _account, AccountID const& _owner, std::string const& _sTableName, std::string const& _raw)
	{
		const ApplyContext &_ctx = ctx_;
		STTx tx(ttSQLSTATEMENT,
			[&_account, &_owner, &_sTableName, &_raw, &_ctx](auto& obj)
		{
			SleOps::addCommonFields(obj, _account);
			//
			obj.setFieldU16(sfOpType, R_DELETE);
			obj.setAccountID(sfAccount, _account);
			obj.setAccountID(sfOwner, _owner);
            std::string _sRaw;
			if (_raw[0] == '[')
                _sRaw = _raw;     
            else
				_sRaw = "[" + _raw + "]";
			
			obj.setFieldVL(sfRaw, strCopy(_sRaw));
		});
        tx.setParentTxID(ctx_.tx.getTransactionID());
		//
		return disposeTableTx(tx, _account, _sTableName);
	}

	int64_t SleOps::updateData(AccountID const& _account, AccountID const& _owner, std::string const& _sTableName, std::string const& _getRaw, std::string const& _updateRaw)
	{
//		const ApplyContext &_ctx = ctx_;
		STTx tx(ttSQLSTATEMENT,
			[&_account, &_owner, &_getRaw, &_updateRaw](auto& obj)
		{
			SleOps::addCommonFields(obj, _account);
			//
			obj.setFieldU16(sfOpType, R_UPDATE);
			obj.setAccountID(sfAccount, _account);
			obj.setAccountID(sfOwner, _owner);
			std::string _sRaw;
			if (_getRaw.empty())
				_sRaw = "[" + _updateRaw + "]";
			else
				_sRaw = "[" + _updateRaw + "," + _getRaw + "]";
			obj.setFieldVL(sfRaw, strCopy(_sRaw));
		});
        tx.setParentTxID(ctx_.tx.getTransactionID());
		//
		return disposeTableTx(tx, _account, _sTableName);
	}

	int64_t SleOps::updateData(AccountID const& _account, AccountID const& _owner, std::string const& _sTableName, std::string const& _raw)
	{
		const ApplyContext &_ctx = ctx_;
		STTx tx(ttSQLSTATEMENT,
			[&_account, &_owner, &_sTableName, &_raw, &_ctx](auto& obj)
		{
			SleOps::addCommonFields(obj, _account);
			//
			obj.setFieldU16(sfOpType, R_UPDATE);
			obj.setAccountID(sfAccount, _account);
			obj.setAccountID(sfOwner, _owner);
			obj.setFieldVL(sfRaw, strCopy(_raw));
		});
        tx.setParentTxID(ctx_.tx.getTransactionID());
		//
		return disposeTableTx(tx, _account, _sTableName);
	}

	//Select related
	uint256 SleOps::getDataHandle(AccountID const& _account, AccountID const& _owner, std::string const& _sTableName, std::string const& _raw)
	{
		Json::Value jvCommand, tableJson;
		jvCommand[jss::tx_json][jss::Owner] = to_string(_owner);
		jvCommand[jss::tx_json][jss::Account] = to_string(_account);
		if (!_raw.empty())//append select conditions
		{
            jvCommand[jss::tx_json][jss::Raw] = _raw;
        }
        else
        {
            Json::Value _fields(Json::arrayValue);  // select fields
            jvCommand[jss::tx_json][jss::Raw].append(_fields);  // append select fields
        }
		jvCommand[jss::tx_json][jss::OpType] = R_GET;
		tableJson[jss::Table][jss::TableName] = _sTableName;

		auto ledgerSeq = ctx_.app.getLedgerMaster().getValidLedgerIndex();
		auto nameInDB = ctx_.app.getLedgerMaster().getNameInDB(ledgerSeq, _owner, _sTableName);
		if (!nameInDB)
		{
			auto j = ctx_.app.journal("Executive");
			JLOG(j.info())
				<< "SleOps getDataHandle getNameInDB failed,account="
				<< to_string(_owner)
				<< ",tableName="
				<< _sTableName;
		}
		else
		{
			tableJson[jss::Table][jss::NameInDB] = to_string(nameInDB);
		}
		jvCommand[jss::tx_json][jss::Tables].append(tableJson);
		//
		Resource::Charge loadType = -1;
		Resource::Consumer c;
		RPC::JsonContext context{ 
			ctx_.app.journal("RPCHandler"), 
			ctx_.app,loadType,
			ctx_.app.getOPs(), 
			ctx_.app.getLedgerMaster(),
			c, 
			Role::ADMIN,
			{},
			{},
			RPC::APINumberVersionSupported,
			jvCommand
		};

		auto result = ripple::doGetRecord2D(context);
		if (!result.second.empty())
		{
			auto j = ctx_.app.journal("Executive");
			JLOG(j.error())
				<< "SleOps getDataHandle failed, error: "
				<< result.second;
			//
			return uint256(0);
		}
		//
		uint256 handle = ctx_.app.getContractHelper().genRandomUniqueHandle();
		ctx_.app.getContractHelper().addRecord(handle, result.first);
		handleList_.push_back(handle);
		//
		return handle;
	}
	uint256 SleOps::getDataRowCount(uint256 const& _handle)
	{
		auto& vecRes = ctx_.app.getContractHelper().getRecord(_handle);
		return uint256(vecRes.size());
	}

	uint256 SleOps::getDataColumnCount(uint256 const& _handle)
	{
		auto& vecRes = ctx_.app.getContractHelper().getRecord(_handle);
		if (vecRes.size() == 0)
			return uint256(0);
		else
			return uint256(vecRes[0].size());
	}
	std::string	SleOps::getByKey(uint256 const& _handle, size_t row, std::string const& _key)
	{
		auto& vecRes = ctx_.app.getContractHelper().getRecord(_handle);
		if (vecRes.empty() || vecRes.size() <= row)
			return "";
		//
		auto& vecCol = vecRes[row];
		for (Json::Value const& value : vecCol)
		{
			if (value.isMember(_key))
				return value[_key].toStyledString();
		}
		return "";
	}
	std::string	SleOps::getByIndex(uint256 const& _handle, size_t row, size_t column)
	{
		auto& vecRes = ctx_.app.getContractHelper().getRecord(_handle);
		if (vecRes.empty() || vecRes.size() <= row || vecRes[row].size() <= column)
			return "";

		auto& value = vecRes[row][column];
		Json::Value first = *value.begin();
		return first.toStyledString();
	}
	void	SleOps::releaseResource()
	{
		resetTransactionCache();
		for(auto const& handle : handleList_)
			ctx_.app.getContractHelper().releaseHandle(handle);
	}

	//transaction related
	void	SleOps::transactionBegin()
	{
		resetTransactionCache();
		bTransaction_ = true;
	}

	int64_t	SleOps::transactionCommit(AccountID const& _account, bool _bNeedVerify)
	{
		if (!bTransaction_)
		{
			auto j = ctx_.app.journal("Executive");
			JLOG(j.info())
				<< "SleOps transactionCommit failed, because no exist 'transaction Begin'.";
			return false;
		}
		Json::Value vec;
		for (auto tx : sqlTxsStatements_) {
			vec.append(tx.getJson(JsonOptions::none));
		}
		Blob _statements = ripple::strCopy(vec.toStyledString());
		STTx tx(ttSQLTRANSACTION,
			[&_account, &_bNeedVerify, &_statements](auto& obj)
		{
			obj.setFieldVL(sfStatements, _statements);
			obj.setAccountID(sfAccount, _account);
			obj.setFieldU32(sfNeedVerify, _bNeedVerify);
			SleOps::addCommonFields(obj, _account);
		});
        tx.setParentTxID(ctx_.tx.getTransactionID());
		//
		auto ret = applyDirect(ctx_.app, ctx_.view(), tx, ctx_.app.journal("SleOps"));
		if (ret != tesSUCCESS)
		{
			auto j = ctx_.app.journal("Executive");
			JLOG(j.info())
				<< "SleOps transactionCommit,apply result:"
				<< transToken(ret);
			JLOG(j.warn()) << " transactionCommit.ret: " << ret << " " << transToken(ret) << " >>>++++>>>";
		}

		if (ctx_.view().flags() & tapForConsensus)
		{
			ctx_.tx.addSubTx(tx);
		}
		//
		resetTransactionCache();
		return TERtoInt(ret);
	}

	void SleOps::resetTransactionCache()
	{
		bTransaction_ = false;
		if (!sqlTxsNameInDB_.empty())
			sqlTxsNameInDB_.clear();
		if (!sqlTxsStatements_.empty())
			sqlTxsStatements_.clear();
	}

	int64_t SleOps::accountSet(AccountID const& _account, uint32_t nFlag, bool bSet)
	{
		STTx accountSetTx(ttACCOUNT_SET,
			[&nFlag, &bSet](auto& obj)
		{
			if (bSet)   obj.setFieldU32(sfSetFlag,   nFlag);
			else        obj.setFieldU32(sfClearFlag, nFlag);			
		});

		accountSetTx.setParentTxID(ctx_.tx.getTransactionID());
		SleOps::addCommonFields(accountSetTx, _account);
		auto ret = applyDirect(ctx_.app, ctx_.view(), accountSetTx, ctx_.app.journal("SleOps"));
		return TERtoInt(ret);
	}
	
	//   sRate  [1.0 - 2.0]
	int64_t SleOps::setTransferFee(AccountID const& _gateWay,  std::string & _feeRate,  std::string & _minFee,  std::string & _maxFee)
	{
		// convert [1.0,2.0]  -->  [1000000000,2000000000]
		double dRate = atof(_feeRate.c_str());
		if ( dRate !=0 && ( dRate < 1.0 || dRate >2.0) )
			return temBAD_TRANSFER_RATE;

		_feeRate.erase(std::remove(_feeRate.begin(), _feeRate.end(), '.'), _feeRate.end());
		int nLen = 10 - _feeRate.size();
		while (nLen > 0)
		{
			_feeRate.append("0");
			nLen--;
		}

		STTx accountSetTx(ttACCOUNT_SET,
			[&_feeRate,&_minFee, &_maxFee](auto& obj)
		{
			std::uint32_t uRate = atoi(_feeRate.c_str());
			obj.setFieldU32(sfTransferRate, uRate);
			obj.setFieldVL(sfTransferFeeMin, strCopy(_minFee));
			obj.setFieldVL(sfTransferFeeMax, strCopy(_maxFee));
		});


		accountSetTx.setParentTxID(ctx_.tx.getTransactionID());
		SleOps::addCommonFields(accountSetTx, _gateWay);
		auto ret = applyDirect(ctx_.app, ctx_.view(), accountSetTx, ctx_.app.journal("Executive"));
		return TERtoInt(ret);
	}

    int64_t SleOps::trustSet(AccountID const& _account, std::string const& _value, std::string const& _sCurrency, AccountID const& _issuer)
	{
		Currency currency;
		if (!to_currency(currency, _sCurrency))
			return tefINVALID_CURRENY;

		STTx accountSetTx(ttTRUST_SET,
			[&_value, &_issuer,&currency](auto& obj)
		{
			obj.setFieldAmount(sfLimitAmount, ripple::amountFromString(Issue{ currency,_issuer }, _value));
		});

		accountSetTx.setParentTxID(ctx_.tx.getTransactionID());
		SleOps::addCommonFields(accountSetTx, _account);
		auto ret = applyDirect(ctx_.app, ctx_.view(), accountSetTx, ctx_.app.journal("Executive"));
		return TERtoInt(ret);
	}

    int64_t SleOps::trustLimit(AccountID const& _account, AccountID const& _issuer, std::string const& _sCurrency, uint64_t _power)
	{
		Json::Value lines;
		bool  hasLines= getAccountLines(_account,lines);

		if (!hasLines || lines.isNull() || !lines.isArray())
			return -1;

		Json::UInt size = lines.size();
		for (Json::UInt index = 0; index < size; index++) {
			Json::Value& v = lines[index];

			if (v[jss::account] == to_string(_issuer) && v[jss::currency] == _sCurrency) {

				std::string sLimit;
				if (v[jss::limit].isString())
					sLimit = v[jss::limit].asString();

				if (_power == 0) {
					return  atof(sLimit.c_str());
				}
				else {

					STAmount  limitST = ripple::amountFromString(noIssue(), sLimit);
					STAmount  powerST(noIssue(), (uint64_t)std::pow(10, _power));

					limitST = multiply(limitST, powerST,noIssue());
					std::string sLimit = limitST.getText();
					return atof(sLimit.c_str());
				}
			}

		}

		return -1;
	}


	bool SleOps::getAccountLines(AccountID const&  _account, Json::Value& _lines)
	{
		Json::Value jvCommand;
		jvCommand[jss::account] = to_string(_account);

		Resource::Charge loadType = -1;
		Resource::Consumer c;

		RPC::JsonContext context{
			ctx_.app.journal("RPCHandler"),
			ctx_.app,
			loadType,
			ctx_.app.getOPs(),
			ctx_.app.getLedgerMaster(),
			c,
			Role::ADMIN ,
			{},{},
			RPC::APINumberVersionSupported,
			jvCommand,};

		auto result = ripple::doAccountLines(context);

		if (result.isMember(jss::lines)) {

			_lines = result[jss::lines];
			return true;
		}
		else {
			return false;
		}			
	}

    int64_t SleOps::gatewayBalance(AccountID const& _account, AccountID const& _issuer, std::string const& _sCurrency,uint64_t _power)
	{
		Json::Value lines;
		bool  hasLines = getAccountLines(_account, lines);

		if (!hasLines || lines.isNull() || !lines.isArray())
			return -1;

		Json::UInt size = lines.size();
		for (Json::UInt index = 0; index < size; index++) {
			Json::Value& v = lines[index];

			if (v[jss::account] == to_string(_issuer) && v[jss::currency] == _sCurrency) {

				std::string sBalance;
				if(v[jss::balance].isString())
					sBalance = v[jss::balance].asString();

				if (_power == 0) {
					return  atof(sBalance.c_str());
				}
				else {

					STAmount balanceST = ripple::amountFromString(noIssue(), sBalance);
					STAmount  powerST(noIssue(), (uint64_t)std::pow(10, _power));

					balanceST = multiply(balanceST, powerST, noIssue());
					std::string sBalance = balanceST.getText();
					return atof(sBalance.c_str());
				}
			}

		}

		return -1;
	}

    TER
    SleOps::checkAuthority(
        AccountID const account,
        LedgerSpecificFlags flag,
        boost::optional<AccountID> dst)
    {
        auto const sle = ctx_.view().read(keylet::account(account));
        if (!sle)
            return tefINTERNAL;

        if (ctx_.app.config().ADMIN && account == *ctx_.app.config().ADMIN)
            return tesSUCCESS;

		// allow payment with super admin
		if (flag == lsfPaymentAuth && dst && ctx_.app.config().ADMIN &&
            dst == ctx_.app.config().ADMIN)
            return tesSUCCESS;

        if (ctx_.app.config().DEFAULT_AUTHORITY_ENABLED)
        {
            if (!(sle->getFlags() & flag))
                return tecNO_PERMISSION;
        }
        else
        {
            if (sle->getFlags() & flag)
                return tecNO_PERMISSION;
        }

        return tesSUCCESS;
    }

}
