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

#include <peersafe/protocol/Contract.h>
#include <eth/vm/Common.h>
#include <ripple/protocol/digest.h>

namespace ripple {

    AccountID Contract::calcNewAddress(AccountID sender, int nonce)
    {
        eth::bytes data(sender.begin(), sender.end());
        data.push_back((eth::byte)((nonce >> 24) & 0xff));
        data.push_back((eth::byte)((nonce >> 16) & 0xff));
        data.push_back((eth::byte)((nonce >> 8) & 0xff));
        data.push_back((eth::byte)(nonce & 0xff));

        ripesha_hasher rsh;
        rsh(data.data(), data.size());
        auto const d = static_cast<
            ripesha_hasher::result_type>(rsh);
        AccountID id;
        static_assert(sizeof(d) == id.size(), "");
        std::memcpy(id.data(), d.data(), d.size());
        return id;
    }

} // ripple
