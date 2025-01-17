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

#include <ripple/app/ledger/LedgerMaster.h>
#include <ripple/app/ledger/TransactionMaster.h>
#include <ripple/core/JobQueue.h>
#include <boost/optional.hpp>
#include <ripple/overlay/Peer.h>
#include <peersafe/schema/PeerManager.h>
#include <peersafe/schema/Schema.h>
#include <ripple/json/json_reader.h>
#include <ripple/app/misc/NetworkOPs.h>
#include <peersafe/crypto/AES.h>
#include <peersafe/app/table/TableSyncItem.h>
#include <peersafe/app/sql/TxStore.h>
#include <peersafe/protocol/STEntry.h>
#include <peersafe/protocol/TableDefines.h>
#include <peersafe/crypto/ECIES.h>
#include <peersafe/app/storage/TableStorage.h>
#include <peersafe/app/table/TableStatusDBMySQL.h>
#include <peersafe/app/table/TableStatusDBSQLite.h>
#include <peersafe/app/table/TableStatusDB.h>
#include <peersafe/app/table/TableStatusDB.h>
#include <peersafe/app/table/TableSync.h>
#include <peersafe/app/table/TableTxAccumulator.h>
#include <ripple/app/misc/Transaction.h>
#include <peersafe/app/util/TableSyncUtil.h>
#include <peersafe/rpc/TableUtils.h>

using namespace std::chrono;
auto constexpr TABLE_DATA_OVERTM = 30s;
auto constexpr LEDGER_DATA_OVERTM = 30s;
auto const TXID_LENGTH = 64;

#define OPTYPELEN 6
namespace ripple {

TableSyncItem::~TableSyncItem()
{
    StopSync(true);
}

TableSyncItem::TableSyncItem(Schema& app, beast::Journal journal, Config& cfg, SyncTargetType eTargetType)
    : app_(app)
    ,journal_(journal)
    ,cfg_(cfg)
	,eSyncTargetType_(eTargetType)
{   
    eState_               = SYNC_INIT;
    bOperateSQL_          = false;
    bIsChange_            = true;
    bGetLocalData_        = false;
    conn_                 = NULL;
    sTableNameInDB_       = "";
    sNickName_            = "";
    uCreateLedgerSequence_ = 0;
	deleted_			  = false;
}

TableSyncItem::cond const & TableSyncItem::GetCondition()
{
    return sCond_;
}

time_t TableSyncItem::StringToDatetime(const char *str)
{
    tm tm_;
    int year, month, day, hour, minute, second;
    sscanf(str, "%d-%d-%d %d:%d:%d", &year, &month, &day, &hour, &minute, &second);
    tm_.tm_year = year - 1900;
    tm_.tm_mon = month - 1;
    tm_.tm_mday = day;
    tm_.tm_hour = hour;
    tm_.tm_min = minute;
    tm_.tm_sec = second;
    tm_.tm_isdst = 0;

    time_t t_ = mktime(&tm_) - std::chrono::seconds(days(10957)).count();
    
    return t_;
}


void TableSyncItem::Init(const AccountID& id, const std::string& sName, bool isAutoSync)
{
    accountID_ = id;
    sTableName_ = sName;
    bIsAutoSync_ = isAutoSync;
}

void TableSyncItem::InitCondition(const std::string& cond)
{
	if (cond.size() > 0)
	{
        if (cond[0] == '~')  //SYNC_JUMP
        {
            if (cond.size() - 1 == TXID_LENGTH)
                sCond_.stxid = cond.substr(1);
            else
                sCond_.uledgerIndex = std::stoi(cond.substr(1));
            sCond_.eSyncType = SYNC_JUMP;
        }
        else
        {
            int pos1 = cond.find('-');
            int pos2 = cond.find(':');

            if (pos1 > 0 || pos2 > 0)  //time
            {
                sCond_.utime = StringToDatetime(cond.c_str());
            }
            else  //uledgerIndex
            {
                sCond_.uledgerIndex = std::stoi(cond);
            }
            sCond_.eSyncType = SYNC_PRIOR;
        }
	}
}

void TableSyncItem::Init(const AccountID& id, const std::string& sName, const std::string& cond, bool isAutoSync)
{
	Init(id, sName, isAutoSync);
	InitCondition(cond);
}

void TableSyncItem::Init(const AccountID& id, const std::string& sName, const AccountID& userId, const SecretKey& user_secret, const std::string& condition, bool isAutoSync)
{
	Init(id, sName, isAutoSync);
	user_accountID_ = userId;
	user_secret_ = user_secret;

	InitCondition(condition);
}

void TableSyncItem::ReInit()
{    
    ReSetContex();
    {
        std::lock_guard lock(mutexInfo_);
        eState_ = SYNC_REINIT;
    }
}

void TableSyncItem::SetPara(std::string sNameInDB,LedgerIndex iSeq, uint256 hash, LedgerIndex TxnSeq, uint256 Txnhash, uint256 TxnUpdateHash)
{    
    sTableNameInDB_ = sNameInDB;
    u32SeqLedger_ = iSeq;
    uHash_ = hash;
    uTxSeq_ = TxnSeq; 

    uTxHash_ = Txnhash;
    uTxDBUpdateHash_ = TxnUpdateHash;    
}

ConnectionUnit&
TableSyncItem::getConnectionUnit()
{
    if (pConnectionUnit_ == NULL)
    {
        pConnectionUnit_ = app_.getConnectionPool().getAvailable();
    }
    return *pConnectionUnit_;
}

TxStoreDBConn& TableSyncItem::getTxStoreDBConn()
{    
    return *getConnectionUnit().conn_;
}

TxStore&
TableSyncItem::getTxStore()
{
    return *getConnectionUnit().store_;
}

TableStatusDB& TableSyncItem::getTableStatusDB()
{
    DatabaseCon* dbConn = getTxStoreDBConn().GetDBConn();
    if (pObjTableStatusDB_ == NULL ||
        pObjTableStatusDB_->GetDatabaseConn() != dbConn)
    {
        DatabaseCon::Setup setup = ripple::setup_SyncDatabaseCon(cfg_);
        std::pair<std::string, bool> result = setup.sync_db.find("type");
        if (result.first.compare("sqlite") == 0)
            pObjTableStatusDB_ = std::make_unique<TableStatusDBSQLite>(dbConn, &app_, journal_);
        else
            pObjTableStatusDB_ = std::make_unique<TableStatusDBMySQL>(dbConn, &app_, journal_);
    }

    return *pObjTableStatusDB_;
}

bool TableSyncItem::getAutoSync()
{
    return bIsAutoSync_;
}

void TableSyncItem::ReSetContex()
{
    {
        std::lock_guard lock(mutexInfo_);
        u32SeqLedger_ = uTxSeq_ = 0;
        uHash_.zero();
        uTxHash_.zero();
        uTxDBUpdateHash_.zero();
    }

    {
        std::lock_guard lock(mutexBlockData_);
        aBlockData_.clear();
    }
    {
        std::lock_guard lock(mutexWholeData_);
        aWholeData_.clear();
    }
    {
        std::lock_guard lock(mutexWaitCheckQueue_);
        aWaitCheckData_.clear();
    }
}

void TableSyncItem::ReSetContexAfterDrop()
{
	{
		std::lock_guard lock(mutexInfo_);
		sTableNameInDB_.clear();
		eState_ = SYNC_DELETING;
	}

	ReSetContex();
}

void TableSyncItem::SetIsDataFromLocal(bool bLocal)
{
    bGetLocalData_ = bLocal;
    if(!bLocal)
    {
        cvReadData_.notify_all();
    }
}
void TableSyncItem::PushDataByOrder(std::list <sqldata_type> &aData, sqldata_type &sqlData)
{
    if (aData.size() == 0)
    {
        aData.push_back(sqlData);
        return;
    }
    else if (sqlData.first > aData.rbegin()->first)
    {
        aData.push_back(sqlData);
        return;
    }
    else
    {
        if (aData.size() == 1)
        {
            aData.push_front(sqlData);
            return;
        }
        else
        {
            for (auto it = aData.begin(); it != aData.end(); it++)
            {
                if (sqlData.first == it->first)  return;
                else if (sqlData.first > it->first)
                {
                    aData.insert(it++, sqlData);
                    return;
                }
            }
        }
    }
}

void TableSyncItem::DealWithWaitCheckQueue(std::function<bool (sqldata_type const&)> f)
{
    std::lock_guard lock(mutexWaitCheckQueue_);
    for (auto it = aWaitCheckData_.begin(); it != aWaitCheckData_.end(); it++)
    {
        f(*it);
    }
    aWaitCheckData_.clear();
}

void TableSyncItem::PushDataToWaitCheckQueue(sqldata_type &sqlData)
{
    std::lock_guard lock(mutexWaitCheckQueue_);
    PushDataByOrder(aWaitCheckData_, sqlData);
}

bool TableSyncItem::GetRightRequestRange(TableSyncItem::BaseInfo &stRange)
{
    std::lock_guard lock(mutexBlockData_);

    if (aBlockData_.size() > 0)
    {
        LedgerIndex iBegin = u32SeqLedger_; 
        LedgerIndex iCheckSeq = uTxSeq_;
        uint256 uHash = uHash_;
        uint256 uCheckHash = uTxHash_;

        for (auto it = aBlockData_.begin(); it != aBlockData_.end(); it++)
        {
            if (it->second.seekstop())
            {
                if (iBegin == it->second.lastledgerseq())
                {
                    stRange.u32SeqLedger = 0;
                    stRange.uHash = uint256();
                    stRange.uStopSeq = 0;
                    stRange.uTxSeq = 0;
                    stRange.uTxHash = uint256();;
                    return true;
                }
                else
                {
                    stRange.u32SeqLedger = iBegin;
                    stRange.uHash = uHash;
                    stRange.uStopSeq = it->second.ledgerseq() - 1;
                    stRange.uTxSeq = iCheckSeq;
                    stRange.uTxHash = uCheckHash;
                    return true;
                }                
            }
            
            if (iBegin == it->second.lastledgerseq())
            {
                iBegin = it->second.ledgerseq();
                iCheckSeq = it->second.ledgerseq();
                uHash = uint256(it->second.ledgerhash());
                uCheckHash = uint256(it->second.ledgercheckhash());
            }
            else
            {
                stRange.u32SeqLedger = iBegin;
                stRange.uHash = uHash;
                stRange.uStopSeq = it->second.ledgerseq() - 1;
                stRange.uTxSeq = iCheckSeq;
                stRange.uTxHash = uCheckHash;
                return true;
            }
        }

        stRange.u32SeqLedger = iBegin;
        stRange.uHash = uHash;
        stRange.uStopSeq = (u32SeqLedger_ + 255) & (~255);
        stRange.uTxSeq = iCheckSeq;
        stRange.uTxHash = uCheckHash;
        return true;
    }

    stRange.u32SeqLedger = u32SeqLedger_;
    stRange.uHash = uHash_;
    stRange.uStopSeq = (u32SeqLedger_ + 1 + 255) & (~255);
    stRange.uTxSeq = uTxSeq_;
    stRange.uTxHash = uTxHash_;

    return true;
}

bool TableSyncItem::IsGetLedgerExpire()
{
    if (duration_cast<seconds>(steady_clock::now() - clock_ledger_) > LEDGER_DATA_OVERTM)
    {
        auto iter = std::find_if(lfailList_.begin(), lfailList_.end(),
            [this](beast::IP::Endpoint &item) {
            return item == uPeerAddr_;
        });
        if (iter == lfailList_.end())
        {
            lfailList_.push_back(uPeerAddr_);
        }

        bIsChange_ = true;
        return true;
    }
    return false;
}

bool TableSyncItem::IsGetDataExpire()
{    
    if (duration_cast<seconds>(steady_clock::now() - clock_data_) > TABLE_DATA_OVERTM)
    {
        auto iter = std::find_if(lfailList_.begin(), lfailList_.end(),
            [this](beast::IP::Endpoint &item) {
            return item == uPeerAddr_;
        });
        if (iter == lfailList_.end())
        {
            lfailList_.push_back(uPeerAddr_);
        }

        bIsChange_ = true;
        return true;
    }
    return false;
}

void TableSyncItem::UpdateLedgerTm()
{
    clock_ledger_ = steady_clock::now();
}
void TableSyncItem::UpdateDataTm()
{
    clock_data_ = steady_clock::now();
}

AccountID TableSyncItem::GetAccount()
{
    return accountID_;
}

std::string TableSyncItem::GetTableName()
{
    return sTableName_;
}

std::string TableSyncItem::GetNickName()
{
    return sNickName_;
}

std::mutex &TableSyncItem::WriteDataMutex()
{
    return this->mutexWriteData_;
}

void TableSyncItem::GetSyncLedger(LedgerIndex &iSeq, uint256 &uHash)
{
    std::lock_guard lock(mutexInfo_);
    iSeq = u32SeqLedger_;
    uHash = uHash_;
}

void TableSyncItem::GetSyncTxLedger(LedgerIndex &iSeq, uint256 &uHash)
{
    std::lock_guard lock(mutexInfo_);
    iSeq = uTxSeq_;
    uHash = uTxHash_;
}

TableSyncItem::TableSyncState TableSyncItem::GetSyncState()
{
    std::lock_guard lock(mutexInfo_);
    return eState_;
}

void TableSyncItem::GetBaseInfo(BaseInfo &stInfo)
{
    std::lock_guard lock(mutexInfo_);
    stInfo.accountID                = accountID_;

    stInfo.sTableNameInDB           = sTableNameInDB_;
    stInfo.sTableName               = sTableName_;
    stInfo.sNickName                = sNickName_;
    stInfo.u32SeqLedger             = u32SeqLedger_;
    stInfo.uHash                    = uHash_;
    stInfo.uTxSeq                   = uTxSeq_;
    stInfo.uTxHash                  = uTxHash_;
    stInfo.eState                   = eState_;
    stInfo.lState                   = lState_;
    stInfo.isAutoSync               = bIsAutoSync_;
	stInfo.eTargetType              = eSyncTargetType_;
	stInfo.uTxUpdateHash            = uTxDBUpdateHash_;
	stInfo.isDeleted				= deleted_;
	
}

void TableSyncItem::SetSyncLedger(LedgerIndex iSeq, uint256 uHash)
{
    std::lock_guard lock(mutexInfo_);
    u32SeqLedger_ = iSeq;
    uHash_ = uHash;
}

void TableSyncItem::SetSyncTxLedger(LedgerIndex iSeq, uint256 uHash)
{
    std::lock_guard lock(mutexInfo_);
      
    uTxSeq_ = iSeq;
    uTxHash_ = uHash;
}

void TableSyncItem::SetSyncState(TableSyncState eState)
{
    std::lock_guard lock(mutexInfo_);
	if (eState_ == SYNC_DELETING) {
		if ((eState == SYNC_INIT || eState == SYNC_REMOVE)) {
			eState_ = eState;
		}
	}
    else if (!(eState_ == SYNC_STOP || eState_ == SYNC_REMOVE))
    {
        eState_ = eState;
    }
}

void TableSyncItem::SetDeleted(bool deleted)
{
	std::lock_guard lock(mutexInfo_);
	deleted_ = deleted;
}

void TableSyncItem::SetLedgerState(LedgerSyncState lState)
{
    std::lock_guard lock(mutexInfo_);
    lState_ = lState;
}

bool TableSyncItem::IsInFailList(beast::IP::Endpoint& peerAddr)
{
    bool ret = false;

    auto iter = std::find_if(lfailList_.begin(), lfailList_.end(),
        [peerAddr](beast::IP::Endpoint &item) {
        return item == peerAddr;
    });

    if (iter != lfailList_.end())
        ret = true;
    return ret;
}


std::shared_ptr<Peer> TableSyncItem::GetRightPeerTarget(LedgerIndex iSeq)
{  
    auto peerList = app_.peerManager().getActivePeers();

    bool isChange = GetIsChange();
    if (!isChange)
    {
        for (auto const& peer : peerList)
        {
            if (uPeerAddr_ == peer->getRemoteAddress())
                return peer;
        }
    }  

    std::shared_ptr<Peer> target = NULL;

    if (peerList.size() > 0)
    {
        int iRandom = rand() % (peerList.size());

        for (int i = 0; i < peerList.size(); i++)
        {
            int iRelIndex = (iRandom + i) % peerList.size();
            auto peer = peerList.at(iRelIndex);
            if (peer == NULL) continue;

            auto addrTmp = peer->getRemoteAddress();
            if (IsInFailList(addrTmp))
                continue;
            target = peer;
            SetPeer(peer);
            break;
        }
    }

    if (!target)
    {
        lfailList_.clear();
        if (peerList.size() > 0)
        { 
            target = peerList.at(0);
            SetPeer(target);
        }            
    }

    return target;
}


void TableSyncItem::ClearFailList()
{
    lfailList_.clear();
}
void TableSyncItem::SendTableMessage(Message::pointer const& m)
{
    auto peer = GetRightPeerTarget(u32SeqLedger_);
    if(peer != NULL)
        peer->send(m);
}

TableSyncItem::LedgerSyncState TableSyncItem::GetCheckLedgerState()
{
    std::lock_guard lock(mutexInfo_);
    return lState_;
}
void TableSyncItem::SetTableName(std::string sName)
{
    std::lock_guard lock(mutexInfo_);
    sTableName_ = sName;
}
std::string TableSyncItem::TableNameInDB()
{
    return sTableNameInDB_;
}

TableSyncItem::SyncTargetType TableSyncItem::TargetType()
{
	return eSyncTargetType_;
}

void  TableSyncItem::SetTableNameInDB(uint160 NameInDB)
{
    sTableNameInDB_ = to_string(NameInDB);
}

void  TableSyncItem::SetTableNameInDB(std::string sNameInDB)
{
    sTableNameInDB_ = sNameInDB;
}

void TableSyncItem::TryOperateSQL()
{
    if (bOperateSQL_)    return;

    bOperateSQL_ = true;

    app_.getJobQueue().addJob(jtOPERATESQL, "operateSQL", [this](Job&) { OperateSQLThread(); },app_.doJobCounter());
}

bool TableSyncItem::IsExist(AccountID accountID,  std::string TableNameInDB)
{    
    return app_.getTableStatusDB().IsExist(accountID, TableNameInDB);
}

bool TableSyncItem::IsNameInDBExist(std::string TableName, std::string Owner, bool delCheck, std::string &TableNameInDB)
{
    return app_.getTableStatusDB().isNameInDBExist(TableName, Owner, delCheck, TableNameInDB);
}

bool TableSyncItem::DeleteRecord(AccountID accountID, std::string TableName)
{
    return app_.getTableStatusDB().DeleteRecord(accountID, TableName);
}

bool TableSyncItem::GetMaxTxnInfo(std::string TableName, std::string Owner, LedgerIndex &TxnLedgerSeq, uint256 &TxnLedgerHash)
{
    return app_.getTableStatusDB().GetMaxTxnInfo(TableName, Owner, TxnLedgerSeq, TxnLedgerHash);
}

bool TableSyncItem::DeleteTable(std::string nameInDB)
{
    auto ret = app_.getTxStore().DropTable(nameInDB);
    return ret.first;
}

bool TableSyncItem::RenameRecord(AccountID accountID, std::string TableNameInDB, std::string TableName)
{
    return app_.getTableStatusDB().RenameRecord(accountID, TableNameInDB, TableName);
}

bool TableSyncItem::UpdateSyncDB(AccountID accountID, std::string TableName, std::string TableNameInDB)
{    
    auto ret = app_.getTableStatusDB().UpdateSyncDB(accountID, TableName, TableNameInDB);
	return ret == soci_success;
}

bool TableSyncItem::UpdateStateDB(const std::string & owner, const std::string & tablename, const bool &isAutoSync)
{
    return app_.getTableStatusDB().UpdateStateDB(owner, tablename, isAutoSync);
}

bool TableSyncItem::DoUpdateSyncDB(const std::string &Owner, const std::string &TableNameInDB, bool bDel,
    const std::string &PreviousCommit)
{
    auto ret = app_.getTableStatusDB().UpdateSyncDB(Owner, TableNameInDB, bDel, PreviousCommit);
	if (ret == soci_exception) {
		SetSyncState(SYNC_STOP);
	}
	return ret == soci_success;
}

std::pair<bool, std::string> TableSyncItem::InitPassphrase()
{
	auto ledger = app_.getLedgerMaster().getValidatedLedger();
	if (ledger == NULL)
	{
		return std::make_pair(false, "ledger error");
	}
    auto tup = getTableEntry(*ledger, accountID_, sTableName_);
    auto pEntry = std::get<1>(tup);
    if (pEntry == nullptr)
        return std::make_pair(false, "Can't find table sle.");

    auto& table = *pEntry;
    bool bConfidential = isConfidential(*ledger,accountID_,sTableName_);
    if (bConfidential)
    {
        confidential_ = true;
        std::shared_ptr<STTx const> pTx = nullptr;
        auto pTransaction = app_.getMasterTransaction().fetch(
            table.getFieldH256(sfCreatedTxnHash));

        if (pTransaction)
        {
            pTx = pTransaction->getSTransaction();
        }

        if (!user_accountID_ || user_accountID_->isZero())
        {
            if (pTx)
            {
                app_.getOPs().pubTableTxs(
                    accountID_,
                    sTableName_,
                    *pTx,
                    std::make_tuple(std::string(jss::db_noSyncConfig), "", ""),
                    false);
            }
            
            return std::make_pair(false, "user account is null.");
        }
        auto tup = getUserAuthAndToken(
            *ledger, accountID_, sTableName_, *user_accountID_);

        if (std::get<0>(tup))
        {
            auto selectFlags = getFlagFromOptype(R_GET);
            auto userFlags = std::get<1>(tup);
            if ((userFlags & selectFlags) == 0)
            {
                if (pTx)
                {
                    app_.getOPs().pubTableTxs(
                        accountID_,
                        sTableName_,
                        *pTx,
                        std::make_tuple(std::string(jss::db_noSyncConfig), "", ""),
                        false);
                }
                
                return std::make_pair(false, "no authority.");
            }
            else
            {
                auto token = std::get<2>(tup);
                if (token.size() > 0)
                {
                    bool result = tokenProcObj_.setSymmertryKey( 
                        token, *user_secret_);
                    if (result)
                        return std::make_pair(true, "");
                    else
                    {
                        if (pTx)
                        {
                            app_.getOPs().pubTableTxs(
                                accountID_,
                                sTableName_,
                                *pTx,
                                std::make_tuple(std::string(jss::db_noSyncConfig), "", ""),
                                false);
                        }
                        
                        return std::make_pair(
                            false,
                            "Cann't get password for this table.");
                    }
                }
                else
                {
                    return std::make_pair(false, "table error");
                }
            }

        }
        else
        {
            if (pTx)
            {
                app_.getOPs().pubTableTxs(
                    accountID_,
                    sTableName_,
                    *pTx,
                    std::make_tuple(std::string(jss::db_acctSecretError), "", ""),
                    false);
            }
            
            return std::make_pair(false, "user account secret is incorrect ");
        }
    }
    else
    {
        return std::make_pair(true, "");
    }

    return std::make_pair(false, "error");
}

void TableSyncItem::TryDecryptRaw(std::vector<STTx>& vecTxs)
{
	for (auto& tx : vecTxs)
	{
		TryDecryptRaw(tx);
	}
}

void TableSyncItem::TryDecryptRaw(STTx& tx)
{
	if (!user_accountID_ || !tokenProcObj_.isValidate)
		return;

	if (T_GRANT == tx.getFieldU16(sfOpType))
		return;

	Blob raw;
	if (tx.isFieldPresent(sfRaw))  //check sfTables
	{
		raw = tx.getFieldVL(sfRaw);
		if (raw.size() == 0)
		{
			return;
		}
	}
	else
	{
		return;
	}

    //JLOG(journal_.error()) << "on TableSyncItem, plain password Hex: " << strHex(passBlob_) << std::endl;
    //JLOG(journal_.error()) << "on TableSyncItem, encrypted raw len: " << raw.size();
    //JLOG(journal_.error()) << "on TableSyncItem, encrypted raw HEX: " << strHex(raw) << std::endl;
	
	if (user_accountID_ && user_secret_)
	{
        if (tx.isFieldPresent(sfSigningPubKey))
        {
            auto const pk = tx.getFieldVL (sfSigningPubKey);
            PublicKey publicKey(makeSlice(pk));
		    //decrypt passphrase
            Blob rawDecrypted = tokenProcObj_.symmertryDecrypt(raw, publicKey);
            if (rawDecrypted.size() > 0)
		    {
			    tx.setFieldVL(sfRaw, rawDecrypted);
		    }
        }
	}
}

bool TableSyncItem::DoUpdateSyncDB(const std::string &Owner, const std::string &TableNameInDB, const std::string &TxnLedgerHash,
    const std::string &TxnLedgerSeq, const std::string &LedgerHash, const std::string &LedgerSeq, const std::string &TxHash, 
    const std::string &cond, const std::string &PreviousCommit)
{
    auto ret =  app_.getTableStatusDB().UpdateSyncDB(Owner, TableNameInDB, TxnLedgerHash, TxnLedgerSeq, LedgerHash, 
        LedgerSeq, TxHash, cond, PreviousCommit);
	if (ret == soci_exception) {
		SetSyncState(SYNC_STOP);
	}
	return ret == soci_success;
}

std::string TableSyncItem::getOperationRule(const STTx& tx)
{
	std::string rule;

	auto opType = tx.getFieldU16(sfOpType);
	if (!isSqlStatementOpType((TableOpType)opType))
		return rule;

	auto ledger = app_.getLedgerMaster().getValidatedLedger();
	if (ledger == NULL)
		return rule;

    auto tup = getTableEntry(*ledger, accountID_, sTableName_);
    auto pEntry = std::get<1>(tup);
	if (pEntry != NULL)
        rule = STEntry::getOperationRule(*pEntry,(TableOpType) opType);
	return rule;
}

std::pair<bool, std::string>
TableSyncItem::DealWithTx(
    const std::vector<STTx>& vecTxs,
    std::uint32_t seq,
    std::uint32_t closeTime)
{
	auto ret = std::make_pair(true, std::string(""));
	for (auto& tx : vecTxs)
	{
		auto retTmp = DealTranCommonTx(tx,seq,closeTime);
		if (!retTmp.first && ret.first)
		{
			ret = retTmp;
			break;
		}
	}

	return ret;
}

std::pair<bool, std::string>
TableSyncItem::DealTranCommonTx(
    const STTx& tx,
    std::uint32_t seq,
    std::uint32_t closeTime)
{
	auto ret = std::make_pair(true, std::string(""));
	auto op_type = tx.getFieldU16(sfOpType);
	if (!isNotNeedDisposeType((TableOpType)op_type))
	{
		auto sOperationRule = getOperationRule(tx);
        auto param = SyncParam{seq, sOperationRule, closeTime};
        ret = this->getTxStore().Dispose(tx, param);

		if (ret.first)
		{
			JLOG(journal_.trace()) << "Dispose success";
		}
        else
        {
            JLOG(journal_.trace()) << "Dispose error";
        }
	}
   
	if (ret.first)
	{
		if (T_DROP == op_type)
		{
			this->ReSetContexAfterDrop();
		}
		else if (T_RENAME == op_type)
		{
			auto tables = tx.getFieldArray(sfTables);
			if (tables.size() > 0)
			{
				auto newTableName = strCopy(tables[0].getFieldVL(sfTableNewName));
				sTableName_ = newTableName;
				getTableStatusDB().RenameRecord(accountID_, sTableNameInDB_, newTableName);
			}
		}
	}
	else
	{
		if ((TableOpType)op_type == T_CREATE)
		{
			auto tables = tx.getFieldArray(sfTables);
			if (tables.size() > 0)
			{
				JLOG(journal_.warn()) << "Deleting item where tableName = " << sTableName_ << " because of creating real table failure.";
				getTableStatusDB().DeleteRecord(accountID_, sTableName_);
				this->ReSetContexAfterDrop();
			}
		}
	}

	return ret;
}

bool TableSyncItem::DealWithEveryLedgerData(const std::vector<protocol::TMTableData>& aData)
{
    for (std::vector<protocol::TMTableData>::const_iterator iter = aData.begin(); iter != aData.end(); ++iter)
    {
        std::string LedgerHash = to_string(uint256(iter->ledgerhash()));
        std::string LedgerCheckHash = to_string(uint256(iter->ledgercheckhash()));
        std::string PreviousCommit;
        std::uint32_t closeTime = iter->closetime();
        std::uint32_t seq = iter->ledgerseq();
        std::string LedgerSeq = std::to_string(seq);
        //check for jump one seq, check for deadline time and deadline seq
        CheckConditionState  checkRet = CondFilter(closeTime, seq, uint256(0));
        if (checkRet == CHECK_JUMP)     continue;
        else if (checkRet == CHECK_REJECT)
        {
            SetSyncState(SYNC_STOP);
            break;
        }

        if (iter->txnodes().size() <= 0)
        {
            soci_ret ret = getTableStatusDB().UpdateSyncDB(to_string(accountID_), sTableNameInDB_, LedgerHash, LedgerSeq, PreviousCommit);
            if (ret == soci_exception) {
                SetSyncState(SYNC_STOP);
                break;
            }
            //JLOG(journal_.info()) <<
            //    "find no tx and DoUpdateSyncDB LedgerSeq:" << LedgerSeq;

            continue;
        }

        if (!getTxStoreDBConn().GetDBConn())
        {
            JLOG(journal_.error()) << "Get db connection failed, maybe max-connections too small";

            SetSyncState(SYNC_STOP);
            break;
        }

        try
        {
            // start transaction
            std::shared_ptr<TxStoreTransaction> stTran = nullptr;
            bool earlyCommitTxs = false;

            int count = 0;
            std::vector<std::tuple<STTx, int, std::pair<bool, std::string>>> tmpPubVec;

            for (int i = 0; i < iter->txnodes().size(); i++)
            {
                const protocol::TMLedgerNode& node = iter->txnodes().Get(i);

                auto str = node.nodedata();
                Blob blob;
                blob.assign(str.begin(), str.end());
                STTx tx = std::move((STTx)(SerialIter{ blob.data(), blob.size() }));
                bool isSQLTransaction = tx.getTxnType() == ttSQLTRANSACTION;

                try
                {
                    //check for jump one tx.
                    if (isJumpThisTx(tx.getTransactionID()))
                    {
                        count++;
                        continue;
                    }

                    if (stTran == nullptr)
                    {
                        stTran = std::make_shared<TxStoreTransaction>(&getTxStoreDBConn());
                    }

                    // if the current tx is a transaction,
                    // all previous txs are committed firstly
                    if (isSQLTransaction)
                    {
                        if (earlyCommitTxs)
                        {
                            stTran->commit();
                            // Re-create transaction object for SQLTransaction Tx
                            stTran.reset(new TxStoreTransaction(&getTxStoreDBConn()));
                        }
                        earlyCommitTxs = false;
                    }
                    else
                    {
                        earlyCommitTxs = true;
                    }

                    if (stTran == nullptr)
                    {
                        JLOG(journal_.error()) << "Out of memory while dealing with ledger data.";
                        return false;
                    }

                    std::vector<STTx> vecTxs = app_.getMasterTransaction().getTxs(tx, sTableNameInDB_, nullptr, iter->ledgerseq());

                    if (vecTxs.size() > 0)
                    {
                        TryDecryptRaw(vecTxs);
                        for (auto& tx : vecTxs)
                        {
                            if (tx.isFieldPresent(sfOpType) && T_CREATE == tx.getFieldU16(sfOpType))
                            {
                                DeleteTable(sTableNameInDB_);
                            }
                        }
                    }
                    JLOG(journal_.debug()) << "got sync tx" << tx.getFullText();

                    auto ret = DealWithTx(vecTxs, seq, closeTime);

                    if (isSQLTransaction && ret.first == false) {
                        stTran->rollback();
                        stTran.reset();
                    }

                    if (app_.getOPs().hasChainSQLTxListener())
                        tmpPubVec.emplace_back(tx, vecTxs.size(), ret);

                    count++;
                }
                catch (std::exception const& e)
                {
                    JLOG(journal_.error()) << "Dispose exception: " << e.what();

                    std::tuple<std::string, std::string, std::string> result = 
                        std::make_tuple(std::string(jss::db_error), "", e.what());
                    app_.getOPs().pubTableTxs(accountID_, sTableName_, tx, result, false);

                    if (isSQLTransaction) {
                        stTran->rollback();
                        stTran.reset();
                        continue;
                    }
                }

                if (isSQLTransaction) {
                    stTran->commit();
                    stTran.reset();
                }
            }

            if (stTran)
                stTran->commit();

            //deal with subscribe
            if (app_.getOPs().hasChainSQLTxListener())
            {
                for (auto iter : tmpPubVec)
                {
                    app_.getTableTxAccumulator().onSubtxResponse(std::get<0>(iter), accountID_, sTableName_, std::get<1>(iter), std::get<2>(iter));
                }
            }

            JLOG(journal_.info()) <<
                "find tx and UpdateSyncDB LedgerSeq: " << LedgerSeq << " count: " << count;
            uTxDBUpdateHash_.zero();
            // write tx time into db,so next start chainsqld can get
            // last txntime and compare it to time condition
            auto ret = getTableStatusDB().UpdateSyncDB(
                to_string(accountID_),
                sTableNameInDB_,
                LedgerCheckHash,
                LedgerSeq,
                LedgerHash,
                LedgerSeq,
                to_string(uTxDBUpdateHash_),
                std::to_string(iter->closetime()),
                PreviousCommit);
            if (ret == soci_exception)
            {
                SetSyncState(SYNC_STOP);
                break;
            }
        }
        catch (soci::soci_error& e) {

            JLOG(journal_.error()) << "soci::soci_error : " << std::string(e.what());
            SetSyncState(SYNC_STOP);
            break;
        }
    }

    //release connection lock
    ReleaseConnectionUnit();

    return true;
}

int TableSyncItem::GetWholeDataSize()
{
    std::lock_guard lock(mutexWholeData_);
    return aWholeData_.size();
}

void TableSyncItem::OperateSQLThread()
{
    //check the connection is ok
    if(eSyncTargetType_ != SyncTarget_dump)       getTxStoreDBConn();

	if (GetSyncState() == SYNC_STOP)
	{
		bOperateSQL_ = false;
        return;
    }

    while (GetWholeDataSize() > 0 && GetSyncState() != SYNC_STOP)
    {
        std::vector<protocol::TMTableData> vec_tmdata;
        {
            std::lock_guard lock(mutexWholeData_);
            for (std::list<sqldata_type>::iterator iter = aWholeData_.begin();
                 iter != aWholeData_.end();
                 ++iter)
            {
                vec_tmdata.push_back((*iter).second);
            }
            aWholeData_.clear();
        }
        DealWithEveryLedgerData(vec_tmdata);
    }

    bOperateSQL_ = false;
    cvOperateSql_.notify_all();
}
bool TableSyncItem::isJumpThisTx(uint256 txid)
{
    if (sCond_.eSyncType == SYNC_JUMP && sCond_.stxid.size() > 0 && to_string(txid) == sCond_.stxid)
        return true;
    return false;
}

TableSyncItem::CheckConditionState TableSyncItem::CondFilter(uint32_t time, uint32_t ledgerIndex, uint256 txid)
{
	if (sCond_.eSyncType == SYNC_PRIOR)
	{
		if (sCond_.uledgerIndex > 0) // check ledger index prior condition
		{
			if (sCond_.uledgerIndex < ledgerIndex)
			{
				JLOG(journal_.warn()) << "prior cond_LedgerIndex violate";				
				return CHECK_REJECT;
			}
		}
		if (time > 0 && sCond_.utime > 0) //check time prior condition
		{   
			if (sCond_.utime < time)
			{
				JLOG(journal_.warn()) << "prior cond_time violate";
				
				return CHECK_REJECT;
			}
		}
	}
	else if (sCond_.eSyncType == SYNC_JUMP)
	{
		if (sCond_.stxid.size() > 0)
		{
			if (to_string(txid) == sCond_.stxid)  //check txid jump condition
			{
				JLOG(journal_.error()) <<
					"tx meet jump condition-txid,should be jump";
				return CHECK_JUMP;
			}
		}
		if (sCond_.uledgerIndex > 0)
		{
			if (sCond_.uledgerIndex == ledgerIndex)  //check ledgerIndex jump condition
			{
				JLOG(journal_.warn()) <<
					"tx meet jump condition-ledgerIndex,should be jump";
				return CHECK_JUMP;
			}
		}
	}
	return CHECK_ADVANCED;
}

void
TableSyncItem::PushDataToWholeDataQueue(sqldata_type& sqlData)
{
    std::lock_guard lock(mutexWholeData_);

    aWholeData_.push_back(sqlData);

    SetSyncLedger(sqlData.first, uint256(sqlData.second.ledgerhash()));
    if (sqlData.second.txnodes().size() > 0)
    {
        SetSyncTxLedger(
            sqlData.first, uint256(sqlData.second.ledgercheckhash()));
    }

    if (sqlData.second.seekstop() && !bGetLocalData_)
    {
        SetSyncState(TableSyncItem::SYNC_BLOCK_STOP);
    }
}

void
TableSyncItem::PushDataToBlockDataQueue(sqldata_type& sqlData)
{
    std::lock_guard lock(mutexBlockData_);

    PushDataByOrder(aBlockData_, sqlData);
}

bool
TableSyncItem::TransBlock2Whole(LedgerIndex iSeq)
{
    std::lock_guard lock1(mutexBlockData_);
    std::lock_guard lock2(mutexWholeData_);
    auto iBegin = iSeq;

    bool bHasStop = false;
    while (aBlockData_.size() > 0)
    {
        auto it = aBlockData_.begin();
        if (iBegin == it->second.lastledgerseq())
        {
            uint256 uCurhash = uint256(it->second.ledgerhash());
            uint256 uCheckhash = uint256(it->second.ledgercheckhash());
            iBegin = it->second.ledgerseq();
            bHasStop = it->second.seekstop();

            SetSyncLedger(iBegin, uCurhash);
            if (it->second.txnodes().size() > 0)
            {
                SetSyncTxLedger(iBegin, uCheckhash);
            }
            aWholeData_.push_back(std::move(*it));
            aBlockData_.erase(it);
        }
        else
        {
            break;
        }
    }

    bool bStop = aBlockData_.size() == 0 && bHasStop;
    if (bStop && !bGetLocalData_)
    {
        SetSyncState(SYNC_BLOCK_STOP);
    }
    return bStop;
}

bool TableSyncItem::GetIsChange()
{
    return  bIsChange_;
}

void TableSyncItem::SetPeer(std::shared_ptr<Peer> peer)
{ 
    std::lock_guard lock(mutexInfo_);
    bIsChange_ = false;
    uPeerAddr_ = peer->getRemoteAddress();
}

bool TableSyncItem::WaitChildThread(std::condition_variable &cv, bool &bCheck, bool bForce)
{
    bool bRet = false;
    std::unique_lock<std::mutex> lock(mutexWaitStop_);
    if (bForce)
    {
        bRet = cv.wait_for(lock, 2000ms, [&bCheck](){return !bCheck; });
    }
    else
    {
        cv.wait(lock, [bCheck](){return !bCheck; });
    }
    return true;
}

bool TableSyncItem::StopSync(bool bForce)
{
    SetSyncState(SYNC_STOP);

    if(WaitChildThread(cvReadData_, bGetLocalData_, bForce) && WaitChildThread(cvOperateSql_, bOperateSQL_, bForce)) 
    {
        return true;
    }
    else
    {
        SetSyncState(SYNC_BLOCK_STOP);
        return false;
    }

    return true;
}

std::string TableSyncItem::GetPosInfo(LedgerIndex iTxLedger, std::string sTxLedgerHash, LedgerIndex iCurLedger, std::string sCurLedgerHash, bool bStop, std::string sMsg)
{
    Json::Value jsPos;
    jsPos["Account"] = to_string(accountID_);
    jsPos["TableName"] = sTableName_;
    jsPos["TxnCreateSeq"] = uCreateLedgerSequence_;
    jsPos["TxnHash"] = sTxLedgerHash;
    jsPos["TxnLedgerSeq"] = iTxLedger;
    jsPos["LedgerHash"] = sCurLedgerHash;
    jsPos["LedgerSeq"] = iCurLedger;
    jsPos["State"] = bStop ? "stopped" : "processing";
    jsPos["Message"] = sMsg;
    auto sPos = jsPos.toStyledString();

    return sPos;
}

void TableSyncItem::ReleaseConnectionUnit()
{
    if (pConnectionUnit_) {
        app_.getConnectionPool().releaseConnection(pConnectionUnit_);
        pConnectionUnit_.reset();
    }
}

}
