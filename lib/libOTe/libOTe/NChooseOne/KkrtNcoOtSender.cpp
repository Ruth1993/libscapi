#include "KkrtNcoOtSender.h"
#include <cryptoTools/Network/IOService.h>
#include "libOTe/Tools/Tools.h"
#include <cryptoTools/Common/Log.h>
#include "KkrtDefines.h"

namespace osuCrypto
{
    using namespace std;

    void KkrtNcoOtSender::setBaseOts(
        gsl::span<block> baseRecvOts,
        const BitVector & choices)
    {
        if (choices.size() != baseRecvOts.size())
            throw std::runtime_error("size mismatch");

        if (choices.size() % (sizeof(block) * 8) != 0)
            throw std::runtime_error("only multiples of 128 are supported");


        mBaseChoiceBits = choices;
        mGens.resize(choices.size());
        mGensBlkIdx.resize(choices.size(), 0);

        for (u64 i = 0; i < baseRecvOts.size(); i++)
        {
            mGens[i].setKey(baseRecvOts[i]);
        }

        mChoiceBlks.resize(choices.size() / (sizeof(block) * 8));
        for (u64 i = 0; i < mChoiceBlks.size(); ++i)
        {
            mChoiceBlks[i] = toBlock(mBaseChoiceBits.data() + (i * sizeof(block)));
        }
    }

    std::unique_ptr<NcoOtExtSender> KkrtNcoOtSender::split()
    {
        auto* raw = new KkrtNcoOtSender();

        std::vector<block> base(mGens.size());

        // use some of the OT extension PRNG to new base OTs
        for (u64 i = 0; i < base.size(); ++i)
        {
            mGens[i].ecbEncCounterMode(mGensBlkIdx[i]++, 1, &base[i]);
            //base[i] = mGens[i].get<block>();
        }
        raw->setBaseOts(base, mBaseChoiceBits);

        return std::unique_ptr<NcoOtExtSender>(raw);
    }

    void KkrtNcoOtSender::init(
        u64 numOTExt, PRNG& prng, Channel& chl)
    {
        static const u8 superBlkSize(8);

        // round up
        numOTExt = ((numOTExt + 127) / 128) * 128;

        // We need two matrices, one for the senders matrix T^i_{b_i} and 
        // one to hold the the correction values. This is sometimes called 
        // the u = T0 + T1 + C matrix in the papers.
        mT.resize(numOTExt, mGens.size() / 128);
        mCorrectionVals.resize(numOTExt, mGens.size() / 128);

        // The receiver will send us correction values, this is the index of
        // the next one they will send.
        mCorrectionIdx = 0;

        // we are going to process OTs in blocks of 128 * superblkSize messages.
        u64 numSuperBlocks = (numOTExt / 128 + superBlkSize - 1) / superBlkSize;

        // the index of the last OT that we have completed.
        u64 doneIdx = 0;

        // a temp that will be used to transpose the sender's matrix
        std::array<std::array<block, superBlkSize>, 128> t;

        u64 numCols = mGens.size();

        for (u64 superBlkIdx = 0; superBlkIdx < numSuperBlocks; ++superBlkIdx)
        {
            // compute at what row does the user want use to stop.
            // the code will still compute the transpose for these
            // extra rows, but it is thrown away.
            u64 stopIdx
                = doneIdx
                + std::min<u64>(u64(128) * superBlkSize, mT.bounds()[0] - doneIdx);

            // transpose 128 columns at at time. Each column will be 128 * superBlkSize = 1024 bits long.
            for (u64 i = 0; i < numCols / 128; ++i)
            {
                // generate the columns using AES-NI in counter mode.
                for (u64 tIdx = 0, colIdx = i * 128; tIdx < 128; ++tIdx, ++colIdx)
                {
                    mGens[colIdx].ecbEncCounterMode(mGensBlkIdx[colIdx], superBlkSize, ((block*)t.data() + superBlkSize * tIdx));
                    mGensBlkIdx[colIdx] += superBlkSize;
                }

                // transpose our 128 columns of 1024 bits. We will have 1024 rows, 
                // each 128 bits wide.
                sse_transpose128x1024(t);

                // This is the index of where we will store the matrix long term.
                // doneIdx is the starting row. i is the offset into the blocks of 128 bits.
                // __restrict isn't crucial, it just tells the compiler that this pointer
                // is unique and it shouldn't worry about pointer aliasing. 
                block* __restrict mTIter = mT.data() + doneIdx * mT.stride() + i;

                for (u64 rowIdx = doneIdx, j = 0; rowIdx < stopIdx; ++j)
                {
                    // because we transposed 1024 rows, the indexing gets a bit weird. But this
                    // is the location of the next row that we want. Keep in mind that we had long
                    // **contiguous** columns. 
                    block* __restrict tIter = (((block*)t.data()) + j);

                    // do the copy!
                    for (u64 k = 0; rowIdx < stopIdx && k < 128; ++rowIdx, ++k)
                    {
                        *mTIter = *tIter;

                        tIter += superBlkSize;
                        mTIter += mT.stride();
                    }
                }

            }

            doneIdx = stopIdx;
        }
    }


//
//    void KkrtNcoOtSender::encode(
//        u64 otIdx,
//        const gsl::span<block> inputword,
//        u8* dest,
//        u64 destSize)
//    {
//
//#ifndef NDEBUG
//        u64 expectedSize = mGens.size() / (sizeof(block) * 8);
//
//        if (inputword.size() != expectedSize)
//            throw std::invalid_argument("Bad input word" LOCATION);
//#endif // !NDEBUG
//
//        encode(otIdx, inputword.data(), dest, destSize);
//    }
//
    void KkrtNcoOtSender::encode(u64 otIdx, const block * inputword, u8 * dest, u64 destSize)
    {

#ifndef NDEBUG
        if (eq(mCorrectionVals[otIdx][0], ZeroBlock))
            throw std::invalid_argument("appears that we haven't received the receiver's choice yet. " LOCATION);
#endif // !NDEBUG

        std::array<block, 10> codeword;

        auto* corVal = mCorrectionVals.data() + otIdx * mCorrectionVals.stride();
        auto* tVal = mT.data() + otIdx * mT.stride();


        // This is the hashing phase. Here we are using pseudo-random codewords.
        // That means we assume inputword is a hash of some sort.
        for (u64 i = 0; i < mT.stride(); ++i)
        {
            block t0 = corVal[i] ^ inputword[i];
            block t1 = t0 & mChoiceBlks[i];

            codeword[i]
                = tVal[i]
                ^ t1;
        }

#ifdef KKRT_SHA_HASH

        SHA1  sha1;
        u8 hashBuff[SHA1::HashSize];
        // hash it all to get rid of the correlation.
        sha1.Update((u8*)codeword.data(), sizeof(block) * mT.stride());
        sha1.Final(hashBuff);
        memcpy(dest, hashBuff, std::min(destSize, SHA1::HashSize));
        //val = toBlock(hashBuff);
#else
        std::array<block, 10> aesBuff;
        mAesFixedKey.ecbEncBlocks(codeword.data(), mT.stride(), aesBuff.data());

        auto val = ZeroBlock;
        for (u64 i = 0; i < mT.stride(); ++i)
            val = val ^ codeword[i] ^ aesBuff[i];

        memcpy(dest, hashBuff, std::min(destSize, sizeof(block)));
#endif


    }


    void KkrtNcoOtSender::getParams(
        bool maliciousSecure,
        u64 compSecParm,
        u64 statSecParam,
        u64 inputBitCount,
        u64 inputCount,
        u64 & inputBlkSize,
        u64 & baseOtCount)
    {

        //if (maliciousSecure) throw std::runtime_error("");
        baseOtCount = roundUpTo(compSecParm * (maliciousSecure ? 7 : 4), 128);
        inputBlkSize = baseOtCount / 128;
    }

    void KkrtNcoOtSender::recvCorrection(Channel & chl, u64 recvCount)
    {

#ifndef NDEBUG
        if (recvCount > mCorrectionVals.bounds()[0] - mCorrectionIdx)
            throw std::runtime_error("bad receiver, will overwrite the end of our buffer" LOCATION);
#endif // !NDEBUG        

        // receive the next OT correction values. This will be several rows of the form u = T0 + T1 + C(w)
        // there c(w) is a pseudo-random code.
        auto dest = mCorrectionVals.begin() + (mCorrectionIdx * mCorrectionVals.stride());
        chl.recv(&*dest,
            recvCount * sizeof(block) * mCorrectionVals.stride());

        // update the index of there we should store the next set of correction values.
        mCorrectionIdx += recvCount;
    }



}
