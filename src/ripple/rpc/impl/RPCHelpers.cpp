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

#include <ripple/app/ledger/LedgerMaster.h>
#include <ripple/app/ledger/OpenLedger.h>
#include <ripple/app/misc/Transaction.h>
#include <ripple/ledger/View.h>
#include <ripple/net/RPCErr.h>
#include <ripple/protocol/AccountID.h>
#include <ripple/protocol/Feature.h>
#include <ripple/rpc/Context.h>
#include <ripple/rpc/DeliveredAmount.h>
#include <ripple/rpc/impl/RPCHelpers.h>
#include <boost/algorithm/string/case_conv.hpp>

#include <ripple/rpc/impl/GRPCHelpers.h>

namespace ripple {
namespace RPC {

boost::optional<AccountID>
accountFromStringStrict(std::string const& account)
{
    boost::optional<AccountID> result;

    auto const publicKey =
        parseBase58<PublicKey>(TokenType::AccountPublic, account);

    if (publicKey)
        result = calcAccountID(*publicKey);
    else
        result = parseBase58<AccountID>(account);

    return result;
}

error_code_i
accountFromStringWithCode(
    AccountID& result,
    std::string const& strIdent,
    bool bStrict)
{
    if (auto accountID = accountFromStringStrict(strIdent))
    {
        result = *accountID;
        return rpcSUCCESS;
    }

    if (bStrict)
    {
        auto id = deprecatedParseBitcoinAccountID(strIdent);
        return id ? rpcACT_BITCOIN : rpcACT_MALFORMED;
    }

    // We allow the use of the seeds which is poor practice
    // and merely for debugging convenience.
    auto const seed = parseGenericSeed(strIdent);

    if (!seed)
        return rpcBAD_SEED;

    std::pair<PublicKey, SecretKey> keypair;
    // if (nullptr == GmEncryptObj::getInstance())
    if(true)//can not judge the crypto alg from seed, so default use secp256k1
    {
        keypair = generateKeyPair(
            KeyType::secp256k1,
            *seed);
    }
    else
    {
        keypair = generateKeyPair(KeyType::gmalg,*seed);
    }

    result = calcAccountID(keypair.first);
    return rpcSUCCESS;
}

Json::Value
accountFromString(AccountID& result, std::string const& strIdent, bool bStrict)
{
    error_code_i code = accountFromStringWithCode(result, strIdent, bStrict);
    if (code != rpcSUCCESS)
        return rpcError(code);
    else
        return Json::objectValue;
}

bool
getAccountObjects(
    ReadView const& ledger,
    AccountID const& account,
    boost::optional<std::vector<LedgerEntryType>> const& typeFilter,
    uint256 dirIndex,
    uint256 const& entryIndex,
    std::uint32_t const limit,
    Json::Value& jvResult)
{
    auto const root = keylet::ownerDir(account);
    auto found = false;

    if (dirIndex.isZero())
    {
        dirIndex = root.key;
        found = true;
    }

    auto dir = ledger.read({ltDIR_NODE, dirIndex});
    if (!dir)
        return false;

    std::uint32_t i = 0;
    auto& jvObjects = (jvResult[jss::account_objects] = Json::arrayValue);
    for (;;)
    {
        auto const& entries = dir->getFieldV256(sfIndexes);
        auto iter = entries.begin();

        if (!found)
        {
            iter = std::find(iter, entries.end(), entryIndex);
            if (iter == entries.end())
                return false;

            found = true;
        }

        for (; iter != entries.end(); ++iter)
        {
            auto const sleNode = ledger.read(keylet::child(*iter));

            auto typeMatchesFilter =
                [](std::vector<LedgerEntryType> const& typeFilter,
                   LedgerEntryType ledgerType) {
                    auto it = std::find(
                        typeFilter.begin(), typeFilter.end(), ledgerType);
                    return it != typeFilter.end();
                };

            if (!typeFilter.has_value() ||
                typeMatchesFilter(typeFilter.value(), sleNode->getType()))
            {
                jvObjects.append(sleNode->getJson(JsonOptions::none));

                if (++i == limit)
                {
                    if (++iter != entries.end())
                    {
                        jvResult[jss::limit] = limit;
                        jvResult[jss::marker] =
                            to_string(dirIndex) + ',' + to_string(*iter);
                        return true;
                    }

                    break;
                }
            }
        }

        auto const nodeIndex = dir->getFieldU64(sfIndexNext);
        if (nodeIndex == 0)
            return true;

        dirIndex = keylet::page(root, nodeIndex).key;
        dir = ledger.read({ltDIR_NODE, dirIndex});
        if (!dir)
            return true;

        if (i == limit)
        {
            auto const& e = dir->getFieldV256(sfIndexes);
            if (!e.empty())
            {
                jvResult[jss::limit] = limit;
                jvResult[jss::marker] =
                    to_string(dirIndex) + ',' + to_string(*e.begin());
            }

            return true;
        }
    }
}

namespace {

bool
isValidatedOld(LedgerMaster& ledgerMaster, bool standalone)
{
    if (standalone)
        return false;

    return ledgerMaster.getValidatedLedgerAge() > Tuning::maxValidatedLedgerAge;
}

}  // namespace

template <class T>
Status
ledgerFromRequest(T& ledger, JsonContext& context)
{
    ledger.reset();

    auto& params = context.params;

    auto indexValue = params[jss::ledger_index];
    auto hashValue = params[jss::ledger_hash];

    // We need to support the legacy "ledger" field.
    auto& legacyLedger = params[jss::ledger];
    if (legacyLedger)
    {
        if (legacyLedger.asString().size() > 12)
            hashValue = legacyLedger;
        else
            indexValue = legacyLedger;
    }

    if (hashValue)
    {
        if (!hashValue.isString())
            return {rpcINVALID_PARAMS, "ledgerHashNotString"};

        uint256 ledgerHash;
        if (!ledgerHash.SetHex(hashValue.asString()))
            return {rpcINVALID_PARAMS, "ledgerHashMalformed"};
        return getLedger(ledger, ledgerHash, context);
    }
    else if (indexValue.isNumeric())
    {
        return getLedger(ledger, indexValue.asInt(), context);
    }
    else
    {
        auto const index = indexValue.asString();
        if (index == "validated")
        {
            return getLedger(ledger, LedgerShortcut::VALIDATED, context);
        }
        else
        {
            if (index.empty() || index == "current")
                return getLedger(ledger, LedgerShortcut::CURRENT, context);
            else if (index == "closed")
                return getLedger(ledger, LedgerShortcut::CLOSED, context);
            else
            {
                return {rpcINVALID_PARAMS, "ledgerIndexMalformed"};
            }
        }
    }

    return Status::OK;
}

template <class T>
Status
ledgerFromRequest(
    T& ledger,
    GRPCContext<org::zxcl::rpc::v1::GetAccountInfoRequest>& context)
{
    ledger.reset();

    org::zxcl::rpc::v1::GetAccountInfoRequest& request = context.params;

    using LedgerCase = org::zxcl::rpc::v1::LedgerSpecifier::LedgerCase;
    LedgerCase ledgerCase = request.ledger().ledger_case();
    switch (ledgerCase)
    {
        case LedgerCase::kHash: {
            uint256 ledgerHash =
                uint256::fromVoid(request.ledger().hash().data());
            return getLedger(ledger, ledgerHash, context);
        }
        case LedgerCase::kSequence:
            return getLedger(ledger, request.ledger().sequence(), context);
        case LedgerCase::kShortcut:
            [[fallthrough]];
        case LedgerCase::LEDGER_NOT_SET: {
            auto const shortcut = request.ledger().shortcut();
            if (shortcut ==
                org::zxcl::rpc::v1::LedgerSpecifier::SHORTCUT_VALIDATED)
                return getLedger(ledger, LedgerShortcut::VALIDATED, context);
            else
            {
                // note, if unspecified, defaults to current ledger
                if (shortcut ==
                        org::zxcl::rpc::v1::LedgerSpecifier::
                            SHORTCUT_UNSPECIFIED ||
                    shortcut ==
                        org::zxcl::rpc::v1::LedgerSpecifier::SHORTCUT_CURRENT)
                {
                    return getLedger(ledger, LedgerShortcut::CURRENT, context);
                }
                else if (
                    shortcut ==
                    org::zxcl::rpc::v1::LedgerSpecifier::SHORTCUT_CLOSED)
                {
                    return getLedger(ledger, LedgerShortcut::CLOSED, context);
                }
            }
        }
    }

    return Status::OK;
}

// explicit instantiation of above function
template Status
ledgerFromRequest<>(
    std::shared_ptr<ReadView const>&,
    GRPCContext<org::zxcl::rpc::v1::GetAccountInfoRequest>&);

Status
getLedger(
    std::shared_ptr<ReadView const>& ledger,
    uint256 const& ledgerHash,
    Context& context)
{
    ledger = context.ledgerMaster.getLedgerByHash(ledgerHash);
    if (ledger == nullptr)
        return {rpcLGR_NOT_FOUND, "ledgerNotFound"};
    return Status::OK;
}

template <class T>
Status
getLedger(T& ledger, uint32_t ledgerIndex, Context& context)
{
    ledger = context.ledgerMaster.getLedgerBySeq(ledgerIndex);
    if (ledger == nullptr)
    {
        auto cur = context.ledgerMaster.getCurrentLedger();
        if (cur->info().seq == ledgerIndex)
        {
            ledger = cur;
        }
    }

    if (ledger == nullptr)
        return {rpcLGR_NOT_FOUND, "ledgerNotFound"};

    if (ledger->info().seq > context.ledgerMaster.getValidLedgerIndex() &&
        isValidatedOld(context.ledgerMaster, context.app.config().standalone()))
    {
        ledger.reset();
        if (context.apiVersion == 1)
            return {rpcNO_NETWORK, "InsufficientNetworkMode"};
        return {rpcNOT_SYNCED, "notSynced"};
    }

    return Status::OK;
}

template <class T>
Status
getLedger(T& ledger, LedgerShortcut shortcut, Context& context)
{
    if (isValidatedOld(context.ledgerMaster, context.app.config().standalone()))
    {
        if (context.apiVersion == 1)
            return {rpcNO_NETWORK, "InsufficientNetworkMode"};
        return {rpcNOT_SYNCED, "notSynced"};
    }

    if (shortcut == LedgerShortcut::VALIDATED)
    {
        ledger = context.ledgerMaster.getValidatedLedger();
        if (ledger == nullptr)
        {
            if (context.apiVersion == 1)
                return {rpcNO_NETWORK, "InsufficientNetworkMode"};
            return {rpcNOT_SYNCED, "notSynced"};
        }

        assert(!ledger->open());
    }
    else
    {
        if (shortcut == LedgerShortcut::CURRENT)
        {
            ledger = context.ledgerMaster.getCurrentLedger();
            assert(ledger->open());
        }
        else if (shortcut == LedgerShortcut::CLOSED)
        {
            ledger = context.ledgerMaster.getClosedLedger();
            assert(!ledger->open());
        }
        else
        {
            return {rpcINVALID_PARAMS, "ledgerIndexMalformed"};
        }

        if (ledger == nullptr)
        {
            if (context.apiVersion == 1)
                return {rpcNO_NETWORK, "InsufficientNetworkMode"};
            return {rpcNOT_SYNCED, "notSynced"};
        }

        static auto const minSequenceGap = 10;

        if (ledger->info().seq + minSequenceGap <
            context.ledgerMaster.getValidLedgerIndex())
        {
            ledger.reset();
            if (context.apiVersion == 1)
                return {rpcNO_NETWORK, "InsufficientNetworkMode"};
            return {rpcNOT_SYNCED, "notSynced"};
        }
    }
    return Status::OK;
}

// Explicit instantiaion of above three functions
template Status
getLedger<>(std::shared_ptr<ReadView const>&, uint32_t, Context&);

template Status
getLedger<>(
    std::shared_ptr<ReadView const>&,
    LedgerShortcut shortcut,
    Context&);

bool
isValidated(
    LedgerMaster& ledgerMaster,
    ReadView const& ledger,
    Schema& app)
{
    if (ledger.open())
        return false;

    if (ledger.info().validated)
        return true;

    auto seq = ledger.info().seq;
    try
    {
        // Use the skip list in the last validated ledger to see if ledger
        // comes before the last validated ledger (and thus has been
        // validated).
        auto hash =
            ledgerMaster.walkHashBySeq(seq, InboundLedger::Reason::GENERIC);

        if (!hash || ledger.info().hash != *hash)
        {
            // This ledger's hash is not the hash of the validated ledger
            if (hash)
            {
                assert(hash->isNonZero());
                uint256 valHash = getHashByIndex(seq, app);
                if (valHash == ledger.info().hash)
                {
                    // SQL database doesn't match ledger chain
                    ledgerMaster.clearLedger(seq);
                }
            }
            return false;
        }
    }
    catch (SHAMapMissingNode const& mn)
    {
        auto stream = app.journal("RPCHandler").warn();
        JLOG(stream) << "Ledger #" << seq << ": " << mn.what();
        return false;
    }

    // Mark ledger as validated to save time if we see it again.
    ledger.info().validated = true;
    return true;
}

// The previous version of the lookupLedger command would accept the
// "ledger_index" argument as a string and silently treat it as a request to
// return the current ledger which, while not strictly wrong, could cause a lot
// of confusion.
//
// The code now robustly validates the input and ensures that the only possible
// values for the "ledger_index" parameter are the index of a ledger passed as
// an integer or one of the strings "current", "closed" or "validated".
// Additionally, the code ensures that the value passed in "ledger_hash" is a
// string and a valid hash. Invalid values will return an appropriate error
// code.
//
// In the absence of the "ledger_hash" or "ledger_index" parameters, the code
// assumes that "ledger_index" has the value "current".
//
// Returns a Json::objectValue.  If there was an error, it will be in that
// return value.  Otherwise, the object contains the field "validated" and
// optionally the fields "ledger_hash", "ledger_index" and
// "ledger_current_index", if they are defined.
Status
lookupLedger(
    std::shared_ptr<ReadView const>& ledger,
    JsonContext& context,
    Json::Value& result)
{
    if (auto status = ledgerFromRequest(ledger, context))
        return status;

    auto& info = ledger->info();

    if (!ledger->open())
    {
        result[jss::ledger_hash] = to_string(info.hash);
        result[jss::ledger_index] = info.seq;
    }
    else
    {
        result[jss::ledger_current_index] = info.seq;
    }

    result[jss::validated] =
        isValidated(context.ledgerMaster, *ledger, context.app);
    return Status::OK;
}

Json::Value
lookupLedger(std::shared_ptr<ReadView const>& ledger, JsonContext& context)
{
    Json::Value result;
    if (auto status = lookupLedger(ledger, context, result))
        status.inject(result);

    return result;
}

hash_set<AccountID>
parseAccountIds(Json::Value const& jvArray)
{
    hash_set<AccountID> result;
    for (auto const& jv : jvArray)
    {
        if (!jv.isString())
            return hash_set<AccountID>();
        auto const id = parseBase58<AccountID>(jv.asString());
        if (!id)
            return hash_set<AccountID>();
        result.insert(*id);
    }
    return result;
}

void
injectSLE(Json::Value& jv, SLE const& sle)
{
    jv = sle.getJson(JsonOptions::none);
    if (sle.getType() == ltACCOUNT_ROOT)
    {
        if (sle.isFieldPresent(sfEmailHash))
        {
            auto const& hash = sle.getFieldH128(sfEmailHash);
            Blob const b(hash.begin(), hash.end());
            std::string md5 = strHex(makeSlice(b));
            boost::to_lower(md5);
            // VFALCO TODO Give a name and move this constant
            //             to a more visible location. Also
            //             shouldn't this be https?
            jv[jss::urlgravatar] =
                str(boost::format("http://www.gravatar.com/avatar/%s") % md5);
        }
    }
    else
    {
        jv[jss::Invalid] = true;
    }
}

boost::optional<Json::Value>
readLimitField(
    unsigned int& limit,
    Tuning::LimitRange const& range,
    JsonContext const& context)
{
    limit = range.rdefault;
    if (auto const& jvLimit = context.params[jss::limit])
    {
        if (!(jvLimit.isUInt() || (jvLimit.isInt() && jvLimit.asInt() >= 0)))
            return RPC::expected_field_error(jss::limit, "unsigned integer");

        limit = jvLimit.asUInt();
        if (!isUnlimited(context.role))
            limit = std::max(range.rmin, std::min(range.rmax, limit));
    }
    return boost::none;
}

boost::optional<Seed>
parseRippleLibSeed(Json::Value const& value)
{
    // ripple-lib encodes seed used to generate an Ed25519 wallet in a
    // non-standard way. While rippled never encode seeds that way, we
    // try to detect such keys to avoid user confusion.
    if (!value.isString())
        return boost::none;

    auto const result = decodeBase58Token(value.asString(), TokenType::None);

    if (result.size() == 18 &&
        static_cast<std::uint8_t>(result[0]) == std::uint8_t(0xE1) &&
        static_cast<std::uint8_t>(result[1]) == std::uint8_t(0x4B))
        return Seed(makeSlice(result.substr(2)));

    return boost::none;
}

boost::optional<Seed>
getSeedFromRPC(Json::Value const& params, Json::Value& error)
{
    // The array should be constexpr, but that makes Visual Studio unhappy.
    static char const* const seedTypes[]{
        jss::passphrase.c_str(), jss::seed.c_str(), jss::seed_hex.c_str()};

    // Identify which seed type is in use.
    char const* seedType = nullptr;
    int count = 0;
    for (auto t : seedTypes)
    {
        if (params.isMember(t))
        {
            ++count;
            seedType = t;
        }
    }

    if (count != 1)
    {
        error = RPC::make_param_error(
            "Exactly one of the following must be specified: " +
            std::string(jss::passphrase) + ", " + std::string(jss::seed) +
            " or " + std::string(jss::seed_hex));
        return boost::none;
    }

    // Make sure a string is present
    if (!params[seedType].isString())
    {
        error = RPC::expected_field_error(seedType, "string");
        return boost::none;
    }

    auto const fieldContents = params[seedType].asString();

    // Convert string to seed.
    boost::optional<Seed> seed;

    if (seedType == jss::seed.c_str())
        seed = parseBase58<Seed>(fieldContents);
    else if (seedType == jss::passphrase.c_str())
        seed = parseGenericSeed(fieldContents);
    else if (seedType == jss::seed_hex.c_str())
    {
        uint128 s;

        if (s.SetHexExact(fieldContents))
            seed.emplace(Slice(s.data(), s.size()));
    }

    if (!seed)
        error = rpcError(rpcBAD_SEED);

    return seed;
}

std::pair<PublicKey, SecretKey>
keypairForSignature(Json::Value const& params, Json::Value& error)
{
    bool const has_key_type = params.isMember(jss::key_type);

    // All of the secret types we allow, but only one at a time.
    // The array should be constexpr, but that makes Visual Studio unhappy.
    static char const* const secretTypes[]{
        jss::passphrase.c_str(),
        jss::secret.c_str(),
        jss::seed.c_str(),
        jss::seed_hex.c_str()};

    // Identify which secret type is in use.
    char const* secretType = nullptr;
    int count = 0;
    for (auto t : secretTypes)
    {
        if (params.isMember(t))
        {
            ++count;
            secretType = t;
        }
    }

    if (count == 0 || secretType == nullptr)
    {
        error = RPC::missing_field_error(jss::secret);
        return {};
    }

    if (count > 1)
    {
        error = RPC::make_param_error(
            "Exactly one of the following must be specified: " +
            std::string(jss::passphrase) + ", " + std::string(jss::secret) +
            ", " + std::string(jss::seed) + " or " +
            std::string(jss::seed_hex));
        return {};
    }

    boost::optional<KeyType> keyType = KeyType::secp256k1;
    boost::optional<Seed> seed;

    if (has_key_type)
    {
        if (!params[jss::key_type].isString())
        {
            error = RPC::expected_field_error(jss::key_type, "string");
            return {};
        }

        keyType = *(keyTypeFromString(params[jss::key_type].asString()));

        if (!keyType)
        {
            error = RPC::invalid_field_error(jss::key_type);
            return {};
        }

        if (secretType == jss::secret.c_str())
        {
            error = RPC::make_param_error(
                "The secret field is not allowed if " +
                std::string(jss::key_type) + " is used.");
            return {};
        }
    }

	// ripple-lib encodes seed used to generate an Ed25519 wallet in a
	// non-standard way. While we never encode seeds that way, we try
	// to detect such keys to avoid user confusion.
	if (secretType != jss::seed_hex.c_str())
	{
	    seed = RPC::parseRippleLibSeed(params[secretType]);

	    if (seed)
	    {
	        // If the user passed in an Ed25519 seed but *explicitly*
	        // requested another key type, return an error.
	        if (keyType.value_or(KeyType::ed25519) != KeyType::ed25519)
	        {
	            error = RPC::make_error (rpcBAD_SEED,
	                "Specified seed is for an Ed25519 wallet.");
	            return { };
	        }

	        keyType = KeyType::ed25519;
	    }
	}
	// if (!keyType)
	// 	keyType = KeyType::secp256k1;

    std::string privateKeyStr;
    bool isExitSecret = params.isMember(jss::secret);
    if(isExitSecret)
        privateKeyStr = params[jss::secret].asString();
    
    if ((isExitSecret && !privateKeyStr.empty() && ('p' == privateKeyStr[0])))
    {
        //GmEncrypt* hEObj = GmEncryptObj::getInstance();
        std::string privateKeyStr = params[jss::secret].asString();
        TokenType tokenType = TokenType::AccountSecret;
        if (params.isMember(jss::for_node) && params[jss::for_node].asBool())
        {
            tokenType = TokenType::NodePrivate;
        }
        
        std::string privateKeyStrDe58 = decodeBase58Token(privateKeyStr, tokenType);
        if (privateKeyStrDe58.empty())
        {
            error = RPC::make_error(
                rpcBAD_SEED, "Specified secret derive key-pair failed.");
            return {};
        }
		SecretKey secretkeyTemp(Slice(privateKeyStrDe58.c_str(), privateKeyStrDe58.size()), KeyType::gmalg);
        auto publicKey = derivePublicKey(KeyType::gmalg, secretkeyTemp);

        return std::make_pair(publicKey, secretkeyTemp);
    }
    else
    {
		if (!seed)
		{
			if (has_key_type)
				seed = getSeedFromRPC(params, error);
			else
			{
				if (!params[jss::secret].isString())
				{
					error = RPC::expected_field_error(jss::secret, "string");
					return {};
				}

				seed = parseGenericSeed(params[jss::secret].asString());
			}
		}

        if (!seed)
        {
            if (!contains_error(error))
            {
                error = RPC::make_error(rpcBAD_SEED,
                    RPC::invalid_field_message(secretType));
            }
            return{};
        }
        if (keyType != KeyType::secp256k1 && keyType != KeyType::ed25519)
            LogicError("keypairForSignature: invalid key type");

		if (params.isMember(jss::for_node) && params[jss::for_node].asBool())
		{
			auto const private_key = generateSecretKey(*keyType, *seed);
			auto const public_key = derivePublicKey(*keyType, private_key);
			return { public_key , private_key };
		}
		else
		{
			return generateKeyPair(*keyType, *seed);
		}
    }
}

std::pair<RPC::Status, LedgerEntryType>
chooseLedgerEntryType(Json::Value const& params)
{
    std::pair<RPC::Status, LedgerEntryType> result{RPC::Status::OK, ltINVALID};
    if (params.isMember(jss::type))
    {
        static std::array<std::pair<char const*, LedgerEntryType>, 16> const
            types{
                {{jss::account, ltACCOUNT_ROOT},
                 {jss::amendments, ltAMENDMENTS},
                 {jss::check, ltCHECK},
                 {jss::deposit_preauth, ltDEPOSIT_PREAUTH},
                 {jss::directory, ltDIR_NODE},
                 {jss::escrow, ltESCROW},
                 {jss::fee, ltFEE_SETTINGS},
                 {jss::hashes, ltLEDGER_HASHES},
                 {jss::offer, ltOFFER},
                 {jss::payment_channel, ltPAYCHAN},
                 {jss::signer_list, ltSIGNER_LIST},
                 {jss::state, ltRIPPLE_STATE},
                 {jss::ticket, ltTICKET},
                 {jss::tablelist, ltTABLELIST },
                 {jss::table, ltTABLE },
                 {jss::schema, ltSCHEMA }}
                 };

        auto const& p = params[jss::type];
        if (!p.isString())
        {
            result.first = RPC::Status{
                rpcINVALID_PARAMS, "Invalid field 'type', not string."};
            assert(result.first.type() == RPC::Status::Type::error_code_i);
            return result;
        }

        auto const filter = p.asString();
        auto iter = std::find_if(
            types.begin(), types.end(), [&filter](decltype(types.front())& t) {
                return t.first == filter;
            });
        if (iter == types.end())
        {
            result.first =
                RPC::Status{rpcINVALID_PARAMS, "Invalid field 'type'."};
            assert(result.first.type() == RPC::Status::Type::error_code_i);
            return result;
        }
        result.second = iter->second;
    }
    return result;
}

beast::SemanticVersion const firstVersion("1.0.0");
beast::SemanticVersion const goodVersion("1.0.0");
beast::SemanticVersion const lastVersion("1.0.0");

unsigned int
getAPIVersionNumber(Json::Value const& jv)
{
    static Json::Value const minVersion(RPC::ApiMinimumSupportedVersion);
    static Json::Value const maxVersion(RPC::ApiMaximumSupportedVersion);
    static Json::Value const invalidVersion(RPC::APIInvalidVersion);

    Json::Value requestedVersion(RPC::APIVersionIfUnspecified);
    if (jv.isObject())
    {
        requestedVersion = jv.get(jss::api_version, requestedVersion);
    }
    if (!(requestedVersion.isInt() || requestedVersion.isUInt()) ||
        requestedVersion < minVersion || requestedVersion > maxVersion)
    {
        requestedVersion = invalidVersion;
    }
    return requestedVersion.asUInt();
}

}  // namespace RPC
}  // namespace ripple
