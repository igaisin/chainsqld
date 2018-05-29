#ifndef CHAINSQL_APP_MISC_EXTVM_H_INCLUDED
#define CHAINSQL_APP_MISC_EXTVM_H_INCLUDED

#include <peersafe/app/misc/SleOps.h>
#include <ripple/app/tx/impl/ApplyContext.h>
#include <peersafe/vm/ExtVMFace.h>
#include <peersafe/app/misc/TypeTransform.h>

#include <functional>
#include <map>

namespace ripple
{

class ExtVM : public ExtVMFace
{
public:    
    ExtVM(SleOps& _s, EnvInfo const& _envInfo, AccountID const& _myAddress,
        AccountID const& _caller, AccountID const& _origin, uint256 _value, uint256 _gasPrice, bytesConstRef _data,
        bytesConstRef _code, uint256 const& _codeHash, int32_t _depth, bool _isCreate,  bool _staticCall)
      : ExtVMFace(_envInfo, toEvmC(_myAddress), toEvmC(_caller), toEvmC(_origin), toEvmC(_value), toEvmC(_gasPrice), _data, _code.toBytes(), toEvmC(_codeHash), _depth, _isCreate, _staticCall),
        oSle_(_s)
    {
        // Contract: processing account must exist. In case of CALL, the ExtVM
        // is created only if an account has code (so exist). In case of CREATE
        // the account must be created first.
        
        //assert(m_s.addressInUse(_myAddress));
    }

	/// Read storage location.
    virtual evmc_uint256be store(evmc_uint256be const& key) override final;

	/// Write a value in storage.
	virtual void setStore(evmc_uint256be const& key, evmc_uint256be const& value) override final;

    /// Read address's balance.
    virtual evmc_uint256be balance(evmc_address const& addr) override final;

    /// Read address's code.
    virtual bytes const& codeAt(evmc_address const& addr)  override final;

    /// @returns the size of the code in bytes at the given address.
    virtual size_t codeSizeAt(evmc_address const& addr)  override final;

    /// Does the account exist?
    virtual bool exists(evmc_address const& addr) override final;

    /// Suicide the associated contract and give proceeds to the given address.
    virtual void suicide(evmc_address const& addr) override final;

    /// Hash of a block if within the last 256 blocks, or h256() otherwise.
    virtual evmc_uint256be blockHash(int64_t  const& iSeq) override final;

    /// Create a new (contract) account.
    virtual CreateResult create(evmc_uint256be const& endowment, int64_t const& ioGas,
        bytesConstRef const& code, Instruction op, evmc_uint256be const& salt) override final;

    /// Make a new message call.
    virtual CallResult call(CallParameters&) override final;

    /// Revert any changes made (by any of the other calls).
    virtual void log(evmc_uint256be const* /*topics*/, size_t /*numTopics*/, bytesConstRef const& data) override final;
    
    SleOps const& state() const { return oSle_; }

private:
	SleOps&                                                      oSle_;
    beast::Journal                                               journal_;
};

}

#endif