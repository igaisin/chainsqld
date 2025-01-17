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

#ifndef RIPPLE_PROTOCOL_STTX_H_INCLUDED
#define RIPPLE_PROTOCOL_STTX_H_INCLUDED

#include <ripple/json/impl/json_assert.h>
#include <ripple/protocol/PublicKey.h>
#include <ripple/protocol/STArray.h>
#include <ripple/protocol/STObject.h>
#include <ripple/protocol/SecretKey.h>
#include <ripple/protocol/TxFormats.h>
#include <boost/container/flat_set.hpp>
#include <boost/logic/tribool.hpp>
#include <functional>

namespace ripple {

enum TxnSql : char {
    txnSqlNew = 'N',
    txnSqlConflict = 'C',
    txnSqlHeld = 'H',
    txnSqlValidated = 'V',
    txnSqlIncluded = 'I',
    txnSqlUnknown = 'U'
};

class STTx final : public STObject, public CountedObject<STTx>
{
public:
    static char const*
    getCountedObjectName()
    {
        return "STTx";
    }

    static std::size_t const minMultiSigners = 1;
    static std::size_t const maxMultiSigners = 8;

public:
    STTx() = delete;
    STTx&
    operator=(STTx const& other) = delete;

    STTx(STTx const& other) = default;

    explicit STTx(
        SerialIter& sit,
        CommonKey::HashType hashType =
            CommonKey::chainHashTypeG) noexcept(false);
    explicit STTx(
        SerialIter&& sit,
        CommonKey::HashType hashType =
            CommonKey::chainHashTypeG) noexcept(false)
        : STTx(sit, hashType)
    {
    }

    explicit STTx(
        STObject&& object,
        CommonKey::HashType hashType = CommonKey::chainHashTypeG);
    explicit STTx(Json::Value& o0bj, AccountID accountID);

    /** Constructs a transaction.

        The returned transaction will have the specified type and
        any fields that the callback function adds to the object
        that's passed in.
    */
    STTx(TxType type, std::function<void(STObject&)> assembler);

private:
    static void
    getOneTx(
        std::vector<STTx>& vec,
        STTx const& tx,
        std::string sTableNameInDB = "");

public:
    STBase*
    copy(std::size_t n, void* buf) const override
    {
        return emplace(n, buf, *this);
    }

    STBase*
    move(std::size_t n, void* buf) override
    {
        return emplace(n, buf, std::move(*this));
    }

    // STObject functions.
    SerializedTypeID
    getSType() const override
    {
        return STI_TRANSACTION;
    }
    std::string
    getFullText() const override;

    // Outer transaction functions / signature functions.
    Blob
    getSignature() const;

    uint256
    getSigningHash() const;

    TxType
    getTxnType() const
    {
        return tx_type_;
    }

    bool
    isChainSqlTableType() const
    {
        return checkChainsqlTableType(tx_type_);
    }

    void
    addSubTx(STTx const& tx) const
    {
        pTxs_->push_back(tx);
    }

    std::vector<STTx> const&
    getSubTxs() const
    {
        return *pTxs_;
    }

    void
    addLog(Json::Value& jsonLog) const
    {
        paJsonLog_->append(jsonLog);
    }

    Json::Value const&
    getLogs() const
    {
        return *paJsonLog_;
    }

    static bool
    checkChainsqlTableType(TxType txType)
    {
        return txType == ttTABLELISTSET || txType == ttSQLSTATEMENT ||
            txType == ttSQLTRANSACTION;
    }

    static bool
    checkChainsqlContractType(TxType txType)
    {
        return txType == ttCONTRACT;
    }

    static std::pair<std::shared_ptr<STTx>, std::string>
    parseSTTx(Json::Value& obj, AccountID accountID);

    static std::vector<STTx>
    getTxs(
        STTx const& tx,
        std::string sTableNameInDB = "",
        std::shared_ptr<STObject const> contractRawMetadata = NULL,
        bool includeAssert = true);

    bool
    isCrossChainUpload() const;

    std::string
    buildRaw(std::string sOperationRule) const;

    Blob
    getSigningPubKey() const
    {
        return getFieldVL(sfSigningPubKey);
    }

    std::uint32_t
    getSequence() const
    {
        return getFieldU32(sfSequence);
    }
    void
    setSequence(std::uint32_t seq)
    {
        return setFieldU32(sfSequence, seq);
    }

    boost::container::flat_set<AccountID>
    getMentionedAccounts() const;

    uint256
    getTransactionID() const
    {
        return tid_;
    }

    Json::Value
    getJson() const;
    Json::Value
    getJson(JsonOptions options) const override;
    Json::Value
    getJson(JsonOptions options, bool binary) const;

    void
    sign(PublicKey const& publicKey, SecretKey const& secretKey);

    /** Check the signature.
        @return `true` if valid signature. If invalid, the error message string.
    */
    enum class RequireFullyCanonicalSig : bool { no, yes };
    std::pair<bool, std::string>
    checkSign(RequireFullyCanonicalSig requireCanonicalSig) const;

    // certificate sign
    std::pair<bool, std::string>
    checkCertificate() const;

    // SQL Functions with metadata.
    static std::string const&
    getMetaSQLInsertReplaceHeader(bool bHasTxResult = true);

    std::string getMetaSQL (
        std::uint32_t inLedger, std::string const& escapedMetaData,
        std::string resultToken = "tesSUCCESS", bool bSaveRaw = false,bool bUseTxResult = false) const;

    std::string
    getMetaSQL(
        Serializer rawTxn,
        std::uint32_t inLedger,
        char status,
        std::string const& escapedMetaData,
        std::string resultToken,
        bool bUseTxResult) const;

    void
    setParentTxID(const uint256& tidParent)
    {
        tidParent_ = tidParent;
    }
    uint256
    getRealTxID() const
    {
        return !tidParent_.isZero() ? tidParent_ : tid_;
    }
    bool
    isSubTransaction() const
    {
        return !tidParent_.isZero();
    }

private:
    std::pair<bool, std::string>
    checkSingleSign(RequireFullyCanonicalSig requireCanonicalSig) const;

    std::pair<bool, std::string>
    checkMultiSign(RequireFullyCanonicalSig requireCanonicalSig) const;

    void
    buildRaw(Json::Value& condition, std::string& rule) const;

    uint256 tidParent_;
    uint256 tid_;
    TxType tx_type_;
    std::shared_ptr<std::vector<STTx>> pTxs_;
    std::shared_ptr<Json::Value> paJsonLog_;
};

bool
passesLocalChecks(STObject const& st, std::string&);

/** Sterilize a transaction.

    The transaction is serialized and then deserialized,
    ensuring that all equivalent transactions are in canonical
    form. This also ensures that program metadata such as
    the transaction's digest, are all computed.
*/
std::shared_ptr<STTx const>
sterilize(STTx const& stx);

/** Check whether a transaction is a pseudo-transaction */
bool
isPseudoTx(STObject const& tx);

}  // namespace ripple

#endif
