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

#include <ripple/basics/contract.h>
#include <ripple/basics/strHex.h>
#include <ripple/protocol/PublicKey.h>
#include <ripple/protocol/digest.h>
#include <ripple/protocol/impl/secp256k1.h>
#include <boost/multiprecision/cpp_int.hpp>
#include <ed25519-donna/ed25519.h>
#include <peersafe/crypto/ECIES.h>
#include <type_traits>

namespace ripple {

std::ostream&
operator<<(std::ostream& os, PublicKey const& pk)
{
    os << strHex(pk);
    return os;
}

std::ostringstream&
operator<<(std::ostringstream& os, PublicKey const& pk)
{
    os << strHex(pk);
    return os;
}

bool publicKeyComp(PublicKey const& lhs, PublicKey const& rhs)
{
    return lhs < rhs;
}

using uint264 = boost::multiprecision::number<
    boost::multiprecision::cpp_int_backend<
        264, 264, boost::multiprecision::signed_magnitude,
            boost::multiprecision::unchecked, void>>;

template<>
boost::optional<PublicKey>
parseBase58(TokenType type, std::string const& s)
{
    auto const result = decodeBase58Token(s, type);
    auto const pks = makeSlice(result);
    if (!publicKeyType(pks))
        return boost::none;
    if (result.size() != 33 && result.size() != 65)
        return boost::none;
    // if (nullptr == GmEncryptObj::getInstance())
    // {
    //     if (result.size() != 33)
    //         return boost::none;
    // }
    // else
    // {
    //     if (result.size() != 65)
    //         return boost::none;
    // }    
    
    return PublicKey(pks);
}

//------------------------------------------------------------------------------

// Parse a length-prefixed number
//  Format: 0x02 <length-byte> <number>
static boost::optional<Slice>
sigPart(Slice& buf)
{
    if (buf.size() < 3 || buf[0] != 0x02)
        return boost::none;
    auto const len = buf[1];
    buf += 2;
    if (len > buf.size() || len < 1 || len > 33)
        return boost::none;
    // Can't be negative
    if ((buf[0] & 0x80) != 0)
        return boost::none;
    if (buf[0] == 0)
    {
        // Can't be zero
        if (len == 1)
            return boost::none;
        // Can't be padded
        if ((buf[1] & 0x80) == 0)
            return boost::none;
    }
    boost::optional<Slice> number = Slice(buf.data(), len);
    buf += len;
    return number;
}

static std::string
sliceToHex(Slice const& slice)
{
    std::string s;
    if (slice[0] & 0x80)
    {
        s.reserve(2 * (slice.size() + 2));
        s = "0x00";
    }
    else
    {
        s.reserve(2 * (slice.size() + 1));
        s = "0x";
    }
    for (int i = 0; i < slice.size(); ++i)
    {
        s += "0123456789ABCDEF"[((slice[i] & 0xf0) >> 4)];
        s += "0123456789ABCDEF"[((slice[i] & 0x0f) >> 0)];
    }
    return s;
}

/** Determine whether a signature is canonical.
    Canonical signatures are important to protect against signature morphing
    attacks.
    @param vSig the signature data
    @param sigLen the length of the signature
    @param strict_param whether to enforce strictly canonical semantics

    @note For more details please see:
    https://ripple.com/wiki/Transaction_Malleability
    https://bitcointalk.org/index.php?topic=8392.msg127623#msg127623
    https://github.com/sipa/bitcoin/commit/58bc86e37fda1aec270bccb3df6c20fbd2a6591c
*/
boost::optional<ECDSACanonicality>
ecdsaCanonicality(Slice const& sig)
{
    using uint264 =
        boost::multiprecision::number<boost::multiprecision::cpp_int_backend<
            264,
            264,
            boost::multiprecision::signed_magnitude,
            boost::multiprecision::unchecked,
            void>>;

    static uint264 const G(
        "0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEBAAEDCE6AF48A03BBFD25E8CD0364141");

    // The format of a signature should be:
    // <30> <len> [ <02> <lenR> <R> ] [ <02> <lenS> <S> ]
    if ((sig.size() < 8) || (sig.size() > 72))
        return boost::none;
    if ((sig[0] != 0x30) || (sig[1] != (sig.size() - 2)))
        return boost::none;
    Slice p = sig + 2;
    auto r = sigPart(p);
    auto s = sigPart(p);
    if (!r || !s || !p.empty())
        return boost::none;

    uint264 R(sliceToHex(*r));
    if (R >= G)
        return boost::none;

    uint264 S(sliceToHex(*s));
    if (S >= G)
        return boost::none;

    // (R,S) and (R,G-S) are canonical,
    // but is fully canonical when S <= G-S
    auto const Sp = G - S;
    if (S > Sp)
        return ECDSACanonicality::canonical;
    return ECDSACanonicality::fullyCanonical;
}

static bool
ed25519Canonical(Slice const& sig)
{
    if (sig.size() != 64)
        return false;
    // Big-endian Order, the Ed25519 subgroup order
    std::uint8_t const Order[] = {
        0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x14, 0xDE, 0xF9, 0xDE, 0xA2, 0xF7,
        0x9C, 0xD6, 0x58, 0x12, 0x63, 0x1A, 0x5C, 0xF5, 0xD3, 0xED,
    };
    // Take the second half of signature
    // and byte-reverse it to big-endian.
    auto const le = sig.data() + 32;
    std::uint8_t S[32];
    std::reverse_copy(le, le + 32, S);
    // Must be less than Order
    return std::lexicographical_compare(S, S + 32, Order, Order + 32);
}

//------------------------------------------------------------------------------

PublicKey::PublicKey(Slice const& slice)
{
    if (!publicKeyType(slice))
        LogicError("PublicKey::PublicKey invalid type");
    size_ = slice.size();
    std::memcpy(buf_, slice.data(), size_);
}

PublicKey::PublicKey(PublicKey const& other) : size_(other.size_)
{
    if (size_)
        std::memcpy(buf_, other.buf_, size_);
};

PublicKey&
PublicKey::operator=(PublicKey const& other)
{
    size_ = other.size_;
    if (size_)
        std::memcpy(buf_, other.buf_, size_);
    return *this;
}

//------------------------------------------------------------------------------

boost::optional<KeyType>
publicKeyType(Slice const& slice)
{
    if (slice.size() == 33)
    {
        if (slice[0] == 0xED)
            return KeyType::ed25519;

        if (slice[0] == 0x02 || slice[0] == 0x03)
            return KeyType::secp256k1;
    }
    if (slice.size() == 65 && slice[0] == 0x47)
        return KeyType::gmalg;
    return boost::none;
}

bool
verifyDigest(
    PublicKey const& publicKey,
    uint256 const& digest,
    Slice const& sig,
    bool mustBeFullyCanonical)
{
    auto const type = publicKeyType(publicKey.slice());
    if (! type)
        LogicError("verifyDigest: invalid type");

    switch(*type)
    {
    case KeyType::secp256k1:
	{
        auto const canonicality = ecdsaCanonicality(sig);
        if (!canonicality)
            return false;
        if (mustBeFullyCanonical &&
            (*canonicality != ECDSACanonicality::fullyCanonical))
            return false;

        secp256k1_pubkey pubkey_imp;
        if (secp256k1_ec_pubkey_parse(
                secp256k1Context(),
                &pubkey_imp,
                reinterpret_cast<unsigned char const*>(publicKey.data()),
                publicKey.size()) != 1)
            return false;

        secp256k1_ecdsa_signature sig_imp;
        if (secp256k1_ecdsa_signature_parse_der(
                secp256k1Context(),
                &sig_imp,
                reinterpret_cast<unsigned char const*>(sig.data()),
                sig.size()) != 1)
            return false;
        if (*canonicality != ECDSACanonicality::fullyCanonical)
        {
            secp256k1_ecdsa_signature sig_norm;
            if (secp256k1_ecdsa_signature_normalize(
                    secp256k1Context(), &sig_norm, &sig_imp) != 1)
                return false;
            return secp256k1_ecdsa_verify(
                    secp256k1Context(),
                    &sig_norm,
                    reinterpret_cast<unsigned char const*>(digest.data()),
                    &pubkey_imp) == 1;
        }
        return secp256k1_ecdsa_verify(
                secp256k1Context(),
                &sig_imp,
                reinterpret_cast<unsigned char const*>(digest.data()),
                &pubkey_imp) == 1;
	}
	case KeyType::gmalg:
	{
        GmEncrypt* hEObj = GmEncryptObj::getInstance();
		unsigned long rv = 0;

		std::pair<unsigned char*, int> pub4Verify = std::make_pair((unsigned char*)publicKey.data(), publicKey.size());
		rv = hEObj->SM2ECCVerify(pub4Verify, (unsigned char*)digest.data(), digest.bytes, (unsigned char*)sig.data(), sig.size());
		if (rv)
		{
			DebugPrint("ECCVerify Digest error! rv = 0x%04x", rv);
			return false;
		}
		DebugPrint("ECCVerify Digest OK!");
		return true;
	}
    default:
        LogicError("verifyDigest: invalid type");
    }
}

bool
verify(
    PublicKey const& publicKey,
    Slice const& m,
    Slice const& sig,
    bool mustBeFullyCanonical)
{
    if (auto const type = publicKeyType(publicKey))
    {
        if (*type == KeyType::secp256k1)
        {
            return verifyDigest(
                publicKey, sha512Half(m), sig, mustBeFullyCanonical);
        }
        else if (*type == KeyType::ed25519)
        {
            if (!ed25519Canonical(sig))
                return false;

            // We internally prefix Ed25519 keys with a 0xED
            // byte to distinguish them from secp256k1 keys
            // so when verifying the signature, we need to
            // first strip that prefix.
            return ed25519_sign_open(
                       m.data(), m.size(), publicKey.data() + 1, sig.data()) ==
                0;
        }
        else if (*type == KeyType::gmalg)
        {
            unsigned long rv = 0;
            unsigned char hashData[32] = { 0 };
            unsigned long hashDataLen = 32;

            GmEncrypt* hEObj = GmEncryptObj::getInstance();
            std::pair<unsigned char*, int> pub4Verify = std::make_pair((unsigned char*)publicKey.data(), publicKey.size());
            hEObj->SM3HashTotal((unsigned char*)m.data(), m.size(), hashData, &hashDataLen);
            rv = hEObj->SM2ECCVerify(pub4Verify, hashData, hashDataLen, (unsigned char*)sig.data(), sig.size());
            if (rv)
            {
                DebugPrint("ECCVerify error! rv = 0x%04x", rv);
                return false;
            }
            DebugPrint("ECCVerify OK!");
            return true;
        }
    }
    return false;
}

Blob
encrypt(const Blob& passBlob,PublicKey const& publicKey)
{
    auto const type = publicKeyType(publicKey);

    switch (*type)
    {
    case KeyType::gmalg:
    {
        Blob vucCipherText;

        GmEncrypt* hEObj = GmEncryptObj::getInstance();
        std::pair<unsigned char*, int> pub4Encrypt = 
                        std::make_pair((unsigned char*)publicKey.data(), publicKey.size());
        hEObj->SM2ECCEncrypt(pub4Encrypt,
                        (unsigned char*)&passBlob[0], passBlob.size(), vucCipherText);
        return vucCipherText;
    }
    default:
        // Blob publickBlob(publicKey.data(), publicKey.data()+publicKey.size());
        return ripple::asymEncrypt(passBlob, publicKey);
    }
}

bool generateAddrAndPubFile(int pubType, int index, std::string filePath)
{
    if(GmEncryptObj::hEType_ == GmEncryptObj::gmAlgType::soft)
    {
        return true;
    }

	GmEncrypt* hEObj = GmEncryptObj::getInstance();
	std::string fileName = "";
	unsigned char publicKeyTemp[PUBLIC_KEY_EXT_LEN] = { 0 };
	if (hEObj != NULL)
	{
		std::pair<unsigned char*, int> tempPublickey;
		std::string pubKeyStr = "";
		PublicKey* newPubKey = nullptr;
		if (hEObj->syncTableKey == pubType)
		{
			tempPublickey = hEObj->getECCSyncTablePubKey(publicKeyTemp);
			fileName = "/synctablePub.txt";
			newPubKey = new PublicKey(Slice(tempPublickey.first, tempPublickey.second));
			pubKeyStr = toBase58(TokenType::AccountPublic, *newPubKey);
		}
		else if (hEObj->nodeVerifyKey == pubType)
		{
			tempPublickey = hEObj->getECCNodeVerifyPubKey(publicKeyTemp,index);
			fileName = "/nodeverifyPub.txt";
			newPubKey = new PublicKey(Slice(tempPublickey.first, tempPublickey.second));
			pubKeyStr = toBase58(TokenType::NodePublic, *newPubKey);
		}

		std::string addrStr = toBase58(calcAccountID(*newPubKey));
		std::string fileBuffer = pubKeyStr + "\r\n" + addrStr + "\r\n";
		if (filePath.empty())
		{
			filePath = hEObj->GetHomePath();
			filePath += fileName;
		}
		hEObj->FileWrite(filePath.c_str(), "wb+", (const unsigned char*)fileBuffer.c_str(), fileBuffer.size());
		return true;
	}
	return false;
}

NodeID
calcNodeID(PublicKey const& pk)
{
    ripesha_hasher h;
    h(pk.data(), pk.size());
    auto const digest = static_cast<ripesha_hasher::result_type>(h);
    static_assert(NodeID::bytes == sizeof(ripesha_hasher::result_type), "");
    NodeID result;
    std::memcpy(result.data(), digest.data(), digest.size());
    return result;
}

bool SignatureStruct::isValid() const noexcept
{
    static const uint256 s_max =
            from_hex_text<uint256>("0xfffffffffffffffffffffffffffffffebaaedce6af48a03bbfd25e8cd0364141");
    static const uint256 s_zero;

    return (v <= 1 && r > s_zero && s > s_zero && r < s_max && s < s_max);
}

secp256k1_context const* getCtx()
{
    static std::unique_ptr<secp256k1_context, decltype(&secp256k1_context_destroy)> s_ctx{
        secp256k1_context_create(SECP256K1_CONTEXT_SIGN | SECP256K1_CONTEXT_VERIFY),
        &secp256k1_context_destroy
    };
    return s_ctx.get();
}

Blob recover(Signature const& _sig, uint256 const& _message)
{
    int v = _sig[64];
    if (v > 3)
        return {};

    auto* ctx = getCtx();
    secp256k1_ecdsa_recoverable_signature rawSig;
    if (!secp256k1_ecdsa_recoverable_signature_parse_compact(ctx, &rawSig, _sig.data(), v))
        return {};

    secp256k1_pubkey rawPubkey;
    if (!secp256k1_ecdsa_recover(ctx, &rawPubkey, &rawSig, _message.data()))
        return {};

    std::array<byte, 65> serializedPubkey;
    size_t serializedPubkeySize = serializedPubkey.size();
    secp256k1_ec_pubkey_serialize(
            ctx, serializedPubkey.data(), &serializedPubkeySize,
            &rawPubkey, SECP256K1_EC_UNCOMPRESSED
    );
    assert(serializedPubkeySize == serializedPubkey.size());
    assert(serializedPubkey[0] == 0x04);
    
    return Blob(serializedPubkey.data()+1, serializedPubkey.data() + serializedPubkey.size());
}

}  // namespace ripple
