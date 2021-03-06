// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2016 The Bitcoin Core developers
// Copyright (c) 2017 The UltimateOnlineCash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "pow.h"

#include "arith_uint256.h"
#include "chain.h"
#include "util.h"
#include "primitives/block.h"
#include "uint256.h"

unsigned int GetNextWorkRequired(const CBlockIndex* pindexLast, const CBlockHeader *pblock, const Consensus::Params& params)
{
    return GetNextWorkRequiredwithDigiShield(pindexLast, pblock, params);
}

unsigned int GetNextWorkRequiredwithDigiShield(const CBlockIndex* pindexLast, const CBlockHeader *pblock, const Consensus::Params& params)
{
	// DigiShield Retarget Code, found in DigiByte Source

    unsigned int npowWorkLimit = UintToArith256(params.powLimit).GetCompact();
	int blockstogoback = 0;
    bool fTestNet = gArgs.GetBoolArg("-testnet", false);

	//set default to pre-v2.0 values
	int64_t retargetTimespan = params.nPowTargetTimespan;
	//int64_t retargetSpacing = nTargetSpacing;
	int64_t retargetInterval = params.nMinerConfirmationWindow;

	// Genesis block
	if (pindexLast == NULL)
		return npowWorkLimit;

	// Only change once per interval
	if ((pindexLast->nHeight+1) % retargetInterval != 0)
	{
		if (fTestNet)
		{
			// Special difficulty rule for testnet:
			// If the new block's timestamp is more than 2* 10 minutes
			// then allow mining of a min-difficulty block.
			if (pblock->GetBlockTime() > pindexLast->GetBlockTime() + params.nPowTargetSpacing*2)
				return npowWorkLimit;
			else
			{
				// Return the last non-special-min-difficulty-rules-block
				const CBlockIndex* pindex = pindexLast;
				while (pindex->pprev && pindex->nHeight % retargetInterval != 0 && pindex->nBits == npowWorkLimit)
					pindex = pindex->pprev;
				return pindex->nBits;
			}
		}
		return pindexLast->nBits;
	}

	// DigiByte: This fixes an issue where a 51% attack can change difficulty at will.
	// Go back the full period unless it's the first retarget after genesis. Code courtesy of Art Forz
	blockstogoback = retargetInterval-1;
	if ((pindexLast->nHeight+1) != retargetInterval)
		blockstogoback = retargetInterval;

	// Go back by what we want to be 14 days worth of blocks
	const CBlockIndex* pindexFirst = pindexLast;
	for (int i = 0; pindexFirst && i < blockstogoback; i++)
		pindexFirst = pindexFirst->pprev;
	assert(pindexFirst);

	// Limit adjustment step
	int64_t nActualTimespan = pindexLast->GetBlockTime() - pindexFirst->GetBlockTime();

	// thanks to RealSolid & WDC for this code
	if (nActualTimespan < (retargetTimespan - (retargetTimespan/4)) ) nActualTimespan = (retargetTimespan - (retargetTimespan/4));
	if (nActualTimespan > (retargetTimespan + (retargetTimespan/2)) ) nActualTimespan = (retargetTimespan + (retargetTimespan/2));

	arith_uint256 bnNew;
	arith_uint256 bnBefore;
	bnNew.SetCompact(pindexLast->nBits);
	bnBefore=bnNew;
	bnNew *= nActualTimespan;
	bnNew /= retargetTimespan;

	if (bnNew > UintToArith256(params.powLimit))
		bnNew = UintToArith256(params.powLimit);

	return bnNew.GetCompact();
}

unsigned int CalculateNextWorkRequired(const CBlockIndex* pindexLast, int64_t nFirstBlockTime, const Consensus::Params& params)
{
    if (params.fPowNoRetargeting)
        return pindexLast->nBits;

    // Limit adjustment step
    int64_t nActualTimespan = pindexLast->GetBlockTime() - nFirstBlockTime;
    if (nActualTimespan < params.nPowTargetTimespan/4)
        nActualTimespan = params.nPowTargetTimespan/4;
    if (nActualTimespan > params.nPowTargetTimespan*4)
        nActualTimespan = params.nPowTargetTimespan*4;

    // Retarget
    const arith_uint256 bnPowLimit = UintToArith256(params.powLimit);
    arith_uint256 bnNew;
    bnNew.SetCompact(pindexLast->nBits);
    bnNew *= nActualTimespan;
    bnNew /= params.nPowTargetTimespan;

    if (bnNew > bnPowLimit)
        bnNew = bnPowLimit;

    return bnNew.GetCompact();
}

bool CheckProofOfWork(uint256 hash, unsigned int nBits, const Consensus::Params& params)
{
    bool fNegative;
    bool fOverflow;
    arith_uint256 bnTarget;

    bnTarget.SetCompact(nBits, &fNegative, &fOverflow);

    // Check range
    if (fNegative || bnTarget == 0 || fOverflow || bnTarget > UintToArith256(params.powLimit))
        return false;

    // Check proof of work matches claimed amount
    if (UintToArith256(hash) > bnTarget)
        return false;

    return true;
}

bool CheckProofOfWorkPlusAuxPoW(const CBlockHeader& block, const Consensus::Params& params)
{    
       /* Except for legacy blocks with full version 1, ensure that
       the chain ID is correct.  Legacy blocks are not allowed since
       the merge-mining start, which is checked in AcceptBlockHeader
       where the height is known.  */
    if (!block.IsLegacy() && params.fStrictChainId
        && block.GetChainId() != params.nAuxpowChainId)
        return error("%s : block does not have our chain ID"
                     " (got %d, expected %d, full nVersion %d)",
                     __func__, block.GetChainId(),
                     params.nAuxpowChainId, block.nVersion);

    /* If there is no auxpow, just check the block hash.  */
    if (!block.auxpow)
    {
        if (block.IsAuxpow())
            return error("%s : no auxpow on block with auxpow version",
                         __func__);

        if (!CheckProofOfWork(block.GetPoWHash(), block.nBits, params))
            return error("%s : non-AUX proof of work failed", __func__);

        return true;
    }

    /* We have auxpow.  Check it.  */

    if (!block.IsAuxpow())
        return error("%s : auxpow on block with non-auxpow version", __func__);

    /* Temporary check:  Disallow parent blocks with auxpow version.  This is
       for compatibility with the old client.  */
    /* FIXME: Remove this check with a hardfork later on.  */
    if (block.auxpow->getParentBlock().IsAuxpow())
        return error("%s : auxpow parent block has auxpow version", __func__);

    if (!block.auxpow->check(block.GetHash(), block.GetChainId(), params))
        return error("%s : AUX POW is not valid", __func__);
    if (!CheckProofOfWork(block.auxpow->getParentBlockHash(), block.nBits, params))
        return error("%s : AUX proof of work failed", __func__);

    return true;
}