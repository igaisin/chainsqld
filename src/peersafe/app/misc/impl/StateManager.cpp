
#include <peersafe/app/misc/StateManager.h>
#include <ripple/app/ledger/OpenLedger.h>
#include <peersafe/schema/Schema.h>

namespace ripple {

uint32_t
StateManager::getAccountSeq(AccountID const& id, ReadView const& view)
{
	std::lock_guard lock(mutex_);
	if (accountState_.find(id) != accountState_.end())
	{
		return accountState_[id].sequence;
	}
	
	auto sle = view.read(keylet::account(id));
	if (sle)
	{
		accountState_[id].sequence = sle->getFieldU32(sfSequence);
		return sle->getFieldU32(sfSequence);
	}
	else
	{
		return 0;
	}
}

uint32_t
StateManager::getAccountSeq(AccountID const& id,std::shared_ptr<const SLE> const sle)
{
    std::lock_guard lock(mutex_);
    if (accountState_.find(id) != accountState_.end() && 
		accountState_[id].sequence >= sle->getFieldU32(sfSequence))
    {
		return accountState_[id].sequence;
    }
    else
    {
        accountState_[id].sequence = sle->getFieldU32(sfSequence);
        return sle->getFieldU32(sfSequence);
    }
}

void StateManager::resetAccountSeq(AccountID const& id)
{
	std::lock_guard lock(mutex_);
	if (accountState_.find(id) != accountState_.end())
	{
		accountState_.erase(id);
	}	
}

void StateManager::incrementSeq(AccountID const& id)
{
	std::lock_guard lock(mutex_);
	if (accountState_.find(id) != accountState_.end())
	{
		++accountState_[id].sequence;
		return;
	}
	auto sle = app_.openLedger().current()->read(keylet::account(id));
	if (sle)
	{
		accountState_[id].sequence = sle->getFieldU32(sfSequence) + 1;
	}
}

void StateManager::clear()
{
	std::lock_guard lock(mutex_);
	if (accountState_.size() > 0)
	{
		accountState_.clear();
	}
}

}