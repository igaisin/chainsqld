#include <peersafe/app/consensus/ViewChange.h>

namespace ripple {

	uint256
		viewChangeUniqueId(
			std::uint32_t const prevSeq,
			uint256 const& prevHash,
			PublicKey const nodePublic,
			std::uint64_t const& toView
		)
	{
		Serializer s(512);
		s.add32(prevSeq);
		s.add256(prevHash);
		s.addVL(nodePublic);
		s.add64(toView);

		return s.getSHA512Half();
	}

    // -----------------------------------------------------------------------------------
    // class CommitteeViewChange

    CommitteeViewChange::CommitteeViewChange(protocol::TMCommitteeViewChange const& m)
    {
        mView = m.toview();
        mPreSeq = m.previousledgerseq();
        memcpy(mPreHash.begin(), m.previousledgerhash().data(), 32);

        for (auto it : m.signatures())
        {
            PublicKey const publicKey(makeSlice(it.publickey()));
            Slice signature(it.signature().data(), it.signature().size());

            mSignatures.emplace(publicKey, signature);
        }
    }

    bool CommitteeViewChange::checkValidity(std::unique_ptr<ValidatorList> const& validators)
    {
        int count = 0;

        for (auto it : mSignatures)
        {
            if (!validators->trusted(it.first))
            {
                return false;
            }
            ViewChange viewChange{ mPreSeq, mPreHash, it.first, mView, it.second };
            if (!viewChange.checkSign())
            {
                return false;
            }

            count++;
        }

        if (count < validators->quorum())
        {
            return false;
        }

        return true;
    }
}