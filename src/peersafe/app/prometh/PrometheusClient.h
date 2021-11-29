#ifndef RIPPLE_RPC_PROMETHEUS_CLIENT_UTIL_H_INCLUDED
#define RIPPLE_RPC_PROMETHEUS_CLIENT_UTIL_H_INCLUDED

#include <prometheus/counter.h>
#include <prometheus/exposer.h>
#include <prometheus/registry.h>
#include <ripple/core/Config.h>
#include <ripple/beast/utility/PropertyStream.h>
#include <ripple/protocol/Protocol.h>
#include <ripple/basics/Log.h>
#include <ripple/basics/TaggedCache.h>
#include <peersafe/schema/Schema.h>
#include <ripple/app/ledger/OpenLedger.h>
#include <ripple/ledger/ApplyView.h>
#include <array>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>

namespace ripple {
	//prometheus sync tool class
	
	class PrometheusClient {

	public:
		PrometheusClient(Schema& app, Config& cfg,std::string const& pubKey, beast::Journal journal);
		
		virtual ~PrometheusClient();
		void timerEntry(NetClock::time_point const& now);
        int getSchemaCount(Schema& app);
       
	private:
		Schema&										app_;
		beast::Journal                              journal_;
		Config&                                     cfg_;
		
        std::string pubkey_node_;
		ripple::Section prometh;
		NetClock::time_point mPromethTime;
		std::shared_ptr<prometheus::Exposer>  exposer;
        std::shared_ptr<prometheus::Registry> registry;
		prometheus::Gauge& schema_gauge;
		prometheus::Gauge& peer_gauge;
		prometheus::Gauge& txSucessCount_gauge;
		prometheus::Gauge& txFailCount_gauge;
		prometheus::Gauge& contractCreateCount_gauge;
		prometheus::Gauge& contractCallCount_gauge;
		prometheus::Gauge& accountCount_gauge;
		prometheus::Gauge& blockHeight_gauge;
        
		
	};
	 static std::chrono::seconds const promethDataCollectionInterval(5);
}

#endif
