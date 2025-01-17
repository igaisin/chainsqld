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

#ifndef RIPPLE_CORE_CONFIGSECTIONS_H_INCLUDED
#define RIPPLE_CORE_CONFIGSECTIONS_H_INCLUDED

#include <string>

namespace ripple {

// VFALCO DEPRECATED in favor of the BasicConfig interface
struct ConfigSection
{
    explicit ConfigSection() = default;

    static std::string
    nodeDatabase()
    {
        return "node_db";
    }
    static std::string
    shardDatabase()
    {
        return "shard_db";
    }
    static std::string
    importNodeDatabase()
    {
        return "import_db";
    }

    static std::string
    syncTables()
    {
        return "sync_tables";
    }
    static std::string
    autoSync()
    {
        return "auto_sync";
    }
    static std::string
    pressSwitch()
    {
        return "press_switch";
    }
    static std::string
    cryptoAlg()
    {
        return "crypto_alg";
    }
    static std::string
    prometheus()
    {
        return "prometheus";
    }
    static std::string
    remoteSync()
    {
        return "remote_sync";
    }
};

// VFALCO TODO Rename and replace these macros with variables.
#define SECTION_AMENDMENTS "amendments"
#define SECTION_CLUSTER_NODES "cluster_nodes"
#define SECTION_COMPRESSION "compression"
#define SECTION_DEBUG_LOGFILE "debug_logfile"
#define SECTION_ELB_SUPPORT "elb_support"
#define SECTION_FEE_DEFAULT "fee_default"
#define SECTION_FEE_ACCOUNT_RESERVE "fee_account_reserve"
#define SECTION_FEE_OWNER_RESERVE "fee_owner_reserve"
#define SECTION_FETCH_DEPTH "fetch_depth"
#define SECTION_LEDGER_HISTORY "ledger_history"
#define SECTION_INSIGHT "insight"
#define SECTION_IPS "ips"
#define SECTION_IPS_FIXED "ips_fixed"
#define SECTION_AMENDMENT_MAJORITY_TIME "amendment_majority_time"
#define SECTION_NETWORK_QUORUM "network_quorum"
#define SECTION_NODE_SEED "node_seed"
#define SECTION_NODE_SIZE "node_size"
#define SECTION_PATH_SEARCH_OLD "path_search_old"
#define SECTION_PATH_SEARCH "path_search"
#define SECTION_PATH_SEARCH_FAST "path_search_fast"
#define SECTION_PATH_SEARCH_MAX "path_search_max"
#define SECTION_PEER_PRIVATE "peer_private"
#define SECTION_PEERS_MAX "peers_max"
#define SECTION_RELAY_PROPOSALS "relay_proposals"
#define SECTION_RELAY_VALIDATIONS "relay_validations"
#define SECTION_RPC_STARTUP "rpc_startup"
#define SECTION_SIGNING_SUPPORT "signing_support"
#define SECTION_SNTP "sntp_servers"
#define SECTION_SSL_VERIFY "ssl_verify"
#define SECTION_SSL_VERIFY_FILE "ssl_verify_file"
#define SECTION_SSL_VERIFY_DIR "ssl_verify_dir"
#define SECTION_VALIDATORS_FILE "validators_file"
#define SECTION_VALIDATION_SEED "validation_seed"
#define SECTION_VALIDATION_PUBLIC_KEY "validation_public_key"
#define SECTION_WEBSOCKET_PING_FREQ "websocket_ping_frequency"
#define SECTION_VALIDATOR_KEYS "validator_keys"
#define SECTION_GM_SELF_CHECK           "gm_self_check"
#define SECTION_HASH_ALG                "hash_alg"

//#define SECTION_FEE_OFFER               "fee_offer"

#define SECTION_VALIDATOR_KEY_REVOCATION "validator_key_revocation"
#define SECTION_VALIDATOR_LIST_KEYS "validator_list_keys"
#define SECTION_VALIDATOR_LIST_SITES "validator_list_sites"
#define SECTION_VALIDATORS "validators"
#define SECTION_VALIDATOR_TOKEN "validator_token"
#define SECTION_VETO_AMENDMENTS "veto_amendments"
#define SECTION_WORKERS "workers"

#define SECTION_CACERTS_LIST_KEYS       "ca_certs_keys"
#define SECTION_CACERTS_LIST_SITES		"ca_certs_sites"

#define SECTION_CONSENSUS               "consensus"

#define SECTION_USER_X509_ROOT_PATH     "x509_crt_path"

#define SECTION_SCHEMAS					"schemas"
#define SECTION_SCHEMA					"schema"

#define LEDGER_TXS_TABLES               "ledger_tx_tables"

#define SECTION_GOVERNANCE              "governance"

#define SECTION_PEER_X509_ROOT_PATH     "peer_x509_root_path"
#define SECTION_PEER_X509_CRED_PATH     "peer_x509_cred_path"

#define SECTOIN_TRUSTED_CA_LIST         "trusted_ca_list"
#define SECTION_CMD_SSL_CERT            "cmd_ssl_cert"

}  // namespace ripple

#endif
