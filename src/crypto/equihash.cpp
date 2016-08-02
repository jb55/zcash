// Copyright (c) 2016 Jack Grigg
// Copyright (c) 2016 The Zcash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Implementation of the Equihash Proof-of-Work algorithm.
//
// Reference
// =========
// Alex Biryukov and Dmitry Khovratovich
// Equihash: Asymmetric Proof-of-Work Based on the Generalized Birthday Problem
// NDSS ’16, 21-24 February 2016, San Diego, CA, USA
// https://www.internetsociety.org/sites/default/files/blogs-media/equihash-asymmetric-proof-of-work-based-generalized-birthday-problem.pdf

#include "crypto/equihash.h"
#include "util.h"

#include <algorithm>
#include <iostream>
#include <stdexcept>

#include <boost/optional.hpp>

EhSolverCancelledException solver_cancelled;

template<unsigned int N, unsigned int K>
int Equihash<N,K>::InitialiseState(eh_HashState& base_state)
{
    uint32_t le_N = htole32(N);
    uint32_t le_K = htole32(K);
    unsigned char personalization[crypto_generichash_blake2b_PERSONALBYTES] = {};
    memcpy(personalization, "ZcashPoW", 8);
    memcpy(personalization+8,  &le_N, 4);
    memcpy(personalization+12, &le_K, 4);
    return crypto_generichash_blake2b_init_salt_personal(&base_state,
                                                         NULL, 0, // No key.
                                                         N/8,
                                                         NULL,    // No salt.
                                                         personalization);
}

// Big-endian so that lexicographic array comparison is equivalent to integer
// comparison
void EhIndexToArray(const eh_index i, unsigned char* array)
{
    assert(sizeof(eh_index) == 4);
    eh_index bei = htobe32(i);
    memcpy(array, &bei, sizeof(eh_index));
}

// Big-endian so that lexicographic array comparison is equivalent to integer
// comparison
eh_index ArrayToEhIndex(const unsigned char* array)
{
    assert(sizeof(eh_index) == 4);
    eh_index bei;
    memcpy(&bei, array, sizeof(eh_index));
    return be32toh(bei);
}

eh_trunc TruncateIndex(const eh_index i, const unsigned int ilen)
{
    // Truncate to 8 bits
    assert(sizeof(eh_trunc) == 1);
    return (i >> (ilen - 8)) & 0xff;
}

eh_index UntruncateIndex(const eh_trunc t, const eh_index r, const unsigned int ilen)
{
    eh_index i{t};
    return (i << (ilen - 8)) | r;
}

template<size_t WIDTH>
StepRow<WIDTH>::StepRow(unsigned int n, unsigned int k, const eh_HashState& base_state, eh_index i)
{
    unsigned int CollisionBitLength = n/(k+1),
                 CollisionByteLength = (CollisionBitLength+7)/8,
                 ExpandedHashLength=(k+1)*CollisionByteLength;
    eh_HashState state;
    state = base_state;
    unsigned char array[sizeof(eh_index)];
    eh_index lei = htole32(i);
    memcpy(array, &lei, sizeof(eh_index));
    crypto_generichash_blake2b_update(&state, array, sizeof(eh_index));
    crypto_generichash_blake2b_final(&state, hash, ExpandedHashLength);
    for (unsigned int i = 0; i < k+1; i++) {
        hash[i*CollisionByteLength] &= 0xFF >> (8*CollisionByteLength - CollisionBitLength);
    }
}

template<size_t WIDTH> template<size_t W>
StepRow<WIDTH>::StepRow(const StepRow<W>& a)
{
    assert(W <= WIDTH);
    std::copy(a.hash, a.hash+W, hash);
}

template<size_t WIDTH>
FullStepRow<WIDTH>::FullStepRow(unsigned int n, unsigned int k, const eh_HashState& base_state, eh_index i) :
        StepRow<WIDTH> {n, k, base_state, i}
{
    unsigned int CollisionBitLength = n/(k+1),
                 CollisionByteLength = (CollisionBitLength+7)/8,
                 ExpandedHashLength=(k+1)*CollisionByteLength;
    EhIndexToArray(i, hash+ExpandedHashLength);
}

template<size_t WIDTH> template<size_t W>
FullStepRow<WIDTH>::FullStepRow(const FullStepRow<W>& a, const FullStepRow<W>& b, size_t len, size_t lenIndices, int trim) :
        StepRow<WIDTH> {a}
{
    assert(len+lenIndices <= W);
    assert(len-trim+(2*lenIndices) <= WIDTH);
    for (int i = trim; i < len; i++)
        hash[i-trim] = a.hash[i] ^ b.hash[i];
    if (a.IndicesBefore(b, len, lenIndices)) {
        std::copy(a.hash+len, a.hash+len+lenIndices, hash+len-trim);
        std::copy(b.hash+len, b.hash+len+lenIndices, hash+len-trim+lenIndices);
    } else {
        std::copy(b.hash+len, b.hash+len+lenIndices, hash+len-trim);
        std::copy(a.hash+len, a.hash+len+lenIndices, hash+len-trim+lenIndices);
    }
}

template<size_t WIDTH>
FullStepRow<WIDTH>& FullStepRow<WIDTH>::operator=(const FullStepRow<WIDTH>& a)
{
    std::copy(a.hash, a.hash+WIDTH, hash);
    return *this;
}

template<size_t WIDTH>
bool StepRow<WIDTH>::IsZero(size_t len)
{
    // This doesn't need to be constant time.
    for (int i = 0; i < len; i++) {
        if (hash[i] != 0)
            return false;
    }
    return true;
}

template<size_t WIDTH>
std::vector<eh_index> FullStepRow<WIDTH>::GetIndices(size_t len, size_t lenIndices) const
{
    std::vector<eh_index> ret;
    for (int i = 0; i < lenIndices; i += sizeof(eh_index)) {
        ret.push_back(ArrayToEhIndex(hash+len+i));
    }
    return ret;
}

template<size_t WIDTH>
bool HasCollision(StepRow<WIDTH>& a, StepRow<WIDTH>& b, int l)
{
    // This doesn't need to be constant time.
    for (int j = 0; j < l; j++) {
        if (a.hash[j] != b.hash[j])
            return false;
    }
    return true;
}

template<size_t WIDTH>
TruncatedStepRow<WIDTH>::TruncatedStepRow(unsigned int n, unsigned int k, const eh_HashState& base_state, eh_index i, unsigned int ilen) :
        StepRow<WIDTH> {n, k, base_state, i}
{
    unsigned int CollisionBitLength = n/(k+1),
                 CollisionByteLength = (CollisionBitLength+7)/8,
                 ExpandedHashLength=(k+1)*CollisionByteLength;
    hash[ExpandedHashLength] = TruncateIndex(i, ilen);
}

template<size_t WIDTH> template<size_t W>
TruncatedStepRow<WIDTH>::TruncatedStepRow(const TruncatedStepRow<W>& a, const TruncatedStepRow<W>& b, size_t len, size_t lenIndices, int trim) :
        StepRow<WIDTH> {a}
{
    assert(len+lenIndices <= W);
    assert(len-trim+(2*lenIndices) <= WIDTH);
    for (int i = trim; i < len; i++)
        hash[i-trim] = a.hash[i] ^ b.hash[i];
    if (a.IndicesBefore(b, len, lenIndices)) {
        std::copy(a.hash+len, a.hash+len+lenIndices, hash+len-trim);
        std::copy(b.hash+len, b.hash+len+lenIndices, hash+len-trim+lenIndices);
    } else {
        std::copy(b.hash+len, b.hash+len+lenIndices, hash+len-trim);
        std::copy(a.hash+len, a.hash+len+lenIndices, hash+len-trim+lenIndices);
    }
}

template<size_t WIDTH>
TruncatedStepRow<WIDTH>& TruncatedStepRow<WIDTH>::operator=(const TruncatedStepRow<WIDTH>& a)
{
    std::copy(a.hash, a.hash+WIDTH, hash);
    return *this;
}

template<size_t WIDTH>
std::shared_ptr<eh_trunc> TruncatedStepRow<WIDTH>::GetTruncatedIndices(size_t len, size_t lenIndices) const
{
    std::shared_ptr<eh_trunc> p (new eh_trunc[lenIndices]);
    std::copy(hash+len, hash+len+lenIndices, p.get());
    return p;
}

template<unsigned int N, unsigned int K>
std::set<std::vector<eh_index>> Equihash<N,K>::BasicSolve(const eh_HashState& base_state, const std::function<bool(EhSolverCancelCheck)> cancelled)
{
    eh_index init_size { 1 << (CollisionBitLength + 1) };

    // 1) Generate first list
    LogPrint("pow", "N = %d, K = %d\n", N, K);
    LogPrint("pow", "Generating first list\n");
    size_t hashLen = ExpandedHashLength;
    size_t lenIndices = sizeof(eh_index);
    std::vector<FullStepRow<FullWidth>> X;
    X.reserve(init_size);
    for (eh_index i = 0; i < init_size; i++) {
        X.emplace_back(N, K, base_state, i);
        if (cancelled(ListGeneration)) throw solver_cancelled;
    }

    // 3) Repeat step 2 until 2n/(k+1) bits remain
    for (int r = 1; r < K && X.size() > 0; r++) {
        LogPrint("pow", "Round %d:\n", r);
        LogPrint("pow", "- Size %d\n", X.size());
        // 2a) Sort the list
        LogPrint("pow", "- Sorting list\n");
        std::sort(X.begin(), X.end(), CompareSR(CollisionByteLength));
        if (cancelled(ListSorting)) throw solver_cancelled;

        LogPrint("pow", "- Finding collisions\n");
        int i = 0;
        int posFree = 0;
        std::vector<FullStepRow<FullWidth>> Xc;
        while (i < X.size() - 1) {
            // 2b) Find next set of unordered pairs with collisions on the next n/(k+1) bits
            int j = 1;
            while (i+j < X.size() &&
                    HasCollision(X[i], X[i+j], CollisionByteLength)) {
                j++;
            }

            // 2c) Calculate tuples (X_i ^ X_j, (i, j))
            for (int l = 0; l < j - 1; l++) {
                for (int m = l + 1; m < j; m++) {
                    if (DistinctIndices(X[i+l], X[i+m], hashLen, lenIndices)) {
                        Xc.emplace_back(X[i+l], X[i+m], hashLen, lenIndices, CollisionByteLength);
                    }
                }
            }

            // 2d) Store tuples on the table in-place if possible
            while (posFree < i+j && Xc.size() > 0) {
                X[posFree++] = Xc.back();
                Xc.pop_back();
            }

            i += j;
            if (cancelled(ListColliding)) throw solver_cancelled;
        }

        // 2e) Handle edge case where final table entry has no collision
        while (posFree < X.size() && Xc.size() > 0) {
            X[posFree++] = Xc.back();
            Xc.pop_back();
        }

        if (Xc.size() > 0) {
            // 2f) Add overflow to end of table
            X.insert(X.end(), Xc.begin(), Xc.end());
        } else if (posFree < X.size()) {
            // 2g) Remove empty space at the end
            X.erase(X.begin()+posFree, X.end());
            X.shrink_to_fit();
        }

        hashLen -= CollisionByteLength;
        lenIndices *= 2;
        if (cancelled(RoundEnd)) throw solver_cancelled;
    }

    // k+1) Find a collision on last 2n(k+1) bits
    LogPrint("pow", "Final round:\n");
    LogPrint("pow", "- Size %d\n", X.size());
    std::set<std::vector<eh_index>> solns;
    if (X.size() > 1) {
        LogPrint("pow", "- Sorting list\n");
        std::sort(X.begin(), X.end(), CompareSR(hashLen));
        if (cancelled(FinalSorting)) throw solver_cancelled;
        LogPrint("pow", "- Finding collisions\n");
        int i = 0;
        while (i < X.size() - 1) {
            int j = 1;
            while (i+j < X.size() &&
                    HasCollision(X[i], X[i+j], hashLen)) {
                j++;
            }

            for (int l = 0; l < j - 1; l++) {
                for (int m = l + 1; m < j; m++) {
                    FullStepRow<FinalFullWidth> res(X[i+l], X[i+m], hashLen, lenIndices, 0);
                    if (DistinctIndices(X[i+l], X[i+m], hashLen, lenIndices)) {
                        solns.insert(res.GetIndices(hashLen, 2*lenIndices));
                    }
                }
            }

            i += j;
            if (cancelled(FinalColliding)) throw solver_cancelled;
        }
    } else
        LogPrint("pow", "- List is empty\n");

    LogPrint("pow", "- Number of solutions found: %d\n", solns.size());
    return solns;
}

bool IsProbablyDuplicate(std::shared_ptr<eh_trunc> indices, size_t lenIndices)
{
    bool checked_index[lenIndices] = {false};
    for (int z = 0; z < lenIndices; z++) {
        if (!checked_index[z]) {
            for (int y = z+1; y < lenIndices; y++) {
                if (!checked_index[y] && indices.get()[z] == indices.get()[y]) {
                    // Pair found
                    checked_index[y] = true;
                    checked_index[z] = true;
                    break;
                }
            }
        }
    }
    bool is_probably_duplicate = true;
    for (int z = 0; z < lenIndices && is_probably_duplicate; z++) {
        is_probably_duplicate &= checked_index[z];
    }
    return is_probably_duplicate;
}

template<size_t WIDTH>
void CollideBranches(std::vector<FullStepRow<WIDTH>>& X, const size_t hlen, const size_t lenIndices, const unsigned int clen, const unsigned int ilen, const eh_trunc lt, const eh_trunc rt)
{
    int i = 0;
    int posFree = 0;
    std::vector<FullStepRow<WIDTH>> Xc;
    while (i < X.size() - 1) {
        // 2b) Find next set of unordered pairs with collisions on the next n/(k+1) bits
        int j = 1;
        while (i+j < X.size() &&
                HasCollision(X[i], X[i+j], clen)) {
            j++;
        }

        // 2c) Calculate tuples (X_i ^ X_j, (i, j))
        for (int l = 0; l < j - 1; l++) {
            for (int m = l + 1; m < j; m++) {
                if (DistinctIndices(X[i+l], X[i+m], hlen, lenIndices)) {
                    if (IsValidBranch(X[i+l], hlen, ilen, lt) && IsValidBranch(X[i+m], hlen, ilen, rt)) {
                        Xc.emplace_back(X[i+l], X[i+m], hlen, lenIndices, clen);
                    } else if (IsValidBranch(X[i+m], hlen, ilen, lt) && IsValidBranch(X[i+l], hlen, ilen, rt)) {
                        Xc.emplace_back(X[i+m], X[i+l], hlen, lenIndices, clen);
                    }
                }
            }
        }

        // 2d) Store tuples on the table in-place if possible
        while (posFree < i+j && Xc.size() > 0) {
            X[posFree++] = Xc.back();
            Xc.pop_back();
        }

        i += j;
    }

    // 2e) Handle edge case where final table entry has no collision
    while (posFree < X.size() && Xc.size() > 0) {
        X[posFree++] = Xc.back();
        Xc.pop_back();
    }

    if (Xc.size() > 0) {
        // 2f) Add overflow to end of table
        X.insert(X.end(), Xc.begin(), Xc.end());
    } else if (posFree < X.size()) {
        // 2g) Remove empty space at the end
        X.erase(X.begin()+posFree, X.end());
        X.shrink_to_fit();
    }
}

template<unsigned int N, unsigned int K>
std::set<std::vector<eh_index>> Equihash<N,K>::OptimisedSolve(const eh_HashState& base_state, const std::function<bool(EhSolverCancelCheck)> cancelled)
{
    eh_index init_size { 1 << (CollisionBitLength + 1) };
    eh_index recreate_size { UntruncateIndex(1, 0, CollisionBitLength + 1) };

    // First run the algorithm with truncated indices

    eh_index soln_size { 1 << K };
    std::vector<std::shared_ptr<eh_trunc>> partialSolns;
    std::set<std::vector<eh_index>> solns;
    int invalidCount = 0;
    {

        // 1) Generate first list
        LogPrint("pow", "N = %d, K = %d\n", N, K);
        LogPrint("pow", "Generating first list\n");
        size_t hashLen = ExpandedHashLength;
        size_t lenIndices = sizeof(eh_trunc);
        std::vector<TruncatedStepRow<TruncatedWidth>> Xt;
        Xt.reserve(init_size);
        for (eh_index i = 0; i < init_size; i++) {
            Xt.emplace_back(N, K, base_state, i, CollisionBitLength + 1);
            if (cancelled(ListGeneration)) throw solver_cancelled;
        }

        // 3) Repeat step 2 until 2n/(k+1) bits remain
        for (int r = 1; r < K && Xt.size() > 0; r++) {
            LogPrint("pow", "Round %d:\n", r);
            LogPrint("pow", "- Size %d\n", Xt.size());
            // 2a) Sort the list
            LogPrint("pow", "- Sorting list\n");
            std::sort(Xt.begin(), Xt.end(), CompareSR(CollisionByteLength));
            if (cancelled(ListSorting)) throw solver_cancelled;

            LogPrint("pow", "- Finding collisions\n");
            int i = 0;
            int posFree = 0;
            std::vector<TruncatedStepRow<TruncatedWidth>> Xc;
            while (i < Xt.size() - 1) {
                // 2b) Find next set of unordered pairs with collisions on the next n/(k+1) bits
                int j = 1;
                while (i+j < Xt.size() &&
                        HasCollision(Xt[i], Xt[i+j], CollisionByteLength)) {
                    j++;
                }

                // 2c) Calculate tuples (X_i ^ X_j, (i, j))
                bool checking_for_zero = (i == 0 && Xt[0].IsZero(hashLen));
                for (int l = 0; l < j - 1; l++) {
                    for (int m = l + 1; m < j; m++) {
                        // We truncated, so don't check for distinct indices here
                        TruncatedStepRow<TruncatedWidth> Xi {Xt[i+l], Xt[i+m],
                                                             hashLen, lenIndices,
                                                             CollisionByteLength};
                        if (!(Xi.IsZero(hashLen-CollisionByteLength) &&
                              IsProbablyDuplicate(Xi.GetTruncatedIndices(hashLen-CollisionByteLength, 2*lenIndices),
                                                  2*lenIndices))) {
                            Xc.emplace_back(Xi);
                        }
                    }
                }

                // 2d) Store tuples on the table in-place if possible
                while (posFree < i+j && Xc.size() > 0) {
                    Xt[posFree++] = Xc.back();
                    Xc.pop_back();
                }

                i += j;
                if (cancelled(ListColliding)) throw solver_cancelled;
            }

            // 2e) Handle edge case where final table entry has no collision
            while (posFree < Xt.size() && Xc.size() > 0) {
                Xt[posFree++] = Xc.back();
                Xc.pop_back();
            }

            if (Xc.size() > 0) {
                // 2f) Add overflow to end of table
                Xt.insert(Xt.end(), Xc.begin(), Xc.end());
            } else if (posFree < Xt.size()) {
                // 2g) Remove empty space at the end
                Xt.erase(Xt.begin()+posFree, Xt.end());
                Xt.shrink_to_fit();
            }

            hashLen -= CollisionByteLength;
            lenIndices *= 2;
            if (cancelled(RoundEnd)) throw solver_cancelled;
        }

        // k+1) Find a collision on last 2n(k+1) bits
        LogPrint("pow", "Final round:\n");
        LogPrint("pow", "- Size %d\n", Xt.size());
        if (Xt.size() > 1) {
            LogPrint("pow", "- Sorting list\n");
            std::sort(Xt.begin(), Xt.end(), CompareSR(hashLen));
            if (cancelled(FinalSorting)) throw solver_cancelled;
            LogPrint("pow", "- Finding collisions\n");
            int i = 0;
            while (i < Xt.size() - 1) {
                int j = 1;
                while (i+j < Xt.size() &&
                        HasCollision(Xt[i], Xt[i+j], hashLen)) {
                    j++;
                }

                for (int l = 0; l < j - 1; l++) {
                    for (int m = l + 1; m < j; m++) {
                        TruncatedStepRow<FinalTruncatedWidth> res(Xt[i+l], Xt[i+m], hashLen, lenIndices, 0);
                        partialSolns.push_back(res.GetTruncatedIndices(hashLen, 2*lenIndices));
                    }
                }

                i += j;
                if (cancelled(FinalColliding)) throw solver_cancelled;
            }
        } else
            LogPrint("pow", "- List is empty\n");

    } // Ensure Xt goes out of scope and is destroyed

    LogPrint("pow", "Found %d partial solutions\n", partialSolns.size());

    // Now for each solution run the algorithm again to recreate the indices
    LogPrint("pow", "Culling solutions\n");
    for (std::shared_ptr<eh_trunc> partialSoln : partialSolns) {
        size_t hashLen;
        size_t lenIndices;
        std::vector<boost::optional<std::vector<FullStepRow<FinalFullWidth>>>> X;
        X.reserve(K+1);

        // 3) Repeat steps 1 and 2 for each partial index
        for (eh_index i = 0; i < soln_size; i++) {
            // 1) Generate first list of possibilities
            std::vector<FullStepRow<FinalFullWidth>> icv;
            icv.reserve(recreate_size);
            for (eh_index j = 0; j < recreate_size; j++) {
                eh_index newIndex { UntruncateIndex(partialSoln.get()[i], j, CollisionBitLength + 1) };
                icv.emplace_back(N, K, base_state, newIndex);
                if (cancelled(PartialGeneration)) throw solver_cancelled;
            }
            boost::optional<std::vector<FullStepRow<FinalFullWidth>>> ic = icv;

            // 2a) For each pair of lists:
            hashLen = ExpandedHashLength;
            lenIndices = sizeof(eh_index);
            size_t rti = i;
            for (size_t r = 0; r <= K; r++) {
                // 2b) Until we are at the top of a subtree:
                if (r < X.size()) {
                    if (X[r]) {
                        // 2c) Merge the lists
                        ic->reserve(ic->size() + X[r]->size());
                        ic->insert(ic->end(), X[r]->begin(), X[r]->end());
                        std::sort(ic->begin(), ic->end(), CompareSR(hashLen));
                        if (cancelled(PartialSorting)) throw solver_cancelled;
                        size_t lti = rti-(1<<r);
                        CollideBranches(*ic, hashLen, lenIndices,
                                        CollisionByteLength,
                                        CollisionBitLength + 1,
                                        partialSoln.get()[lti], partialSoln.get()[rti]);

                        // 2d) Check if this has become an invalid solution
                        if (ic->size() == 0)
                            goto invalidsolution;

                        X[r] = boost::none;
                        hashLen -= CollisionByteLength;
                        lenIndices *= 2;
                        rti = lti;
                    } else {
                        X[r] = *ic;
                        break;
                    }
                } else {
                    X.push_back(ic);
                    break;
                }
                if (cancelled(PartialSubtreeEnd)) throw solver_cancelled;
            }
            if (cancelled(PartialIndexEnd)) throw solver_cancelled;
        }

        // We are at the top of the tree
        assert(X.size() == K+1);
        for (FullStepRow<FinalFullWidth> row : *X[K]) {
            solns.insert(row.GetIndices(hashLen, lenIndices));
        }
        if (cancelled(PartialEnd)) throw solver_cancelled;
        continue;

invalidsolution:
        invalidCount++;
    }
    LogPrint("pow", "- Number of invalid solutions found: %d\n", invalidCount);
    LogPrint("pow", "- Number of solutions found: %d\n", solns.size());

    return solns;
}

template<unsigned int N, unsigned int K>
bool Equihash<N,K>::IsValidSolution(const eh_HashState& base_state, std::vector<eh_index> soln)
{
    eh_index soln_size { 1u << K };
    if (soln.size() != soln_size) {
        LogPrint("pow", "Invalid solution size: %d\n", soln.size());
        return false;
    }

    std::vector<FullStepRow<FinalFullWidth>> X;
    X.reserve(soln_size);
    for (eh_index i : soln) {
        X.emplace_back(N, K, base_state, i);
    }

    size_t hashLen = ExpandedHashLength;
    size_t lenIndices = sizeof(eh_index);
    while (X.size() > 1) {
        std::vector<FullStepRow<FinalFullWidth>> Xc;
        for (int i = 0; i < X.size(); i += 2) {
            if (!HasCollision(X[i], X[i+1], CollisionByteLength)) {
                LogPrint("pow", "Invalid solution: invalid collision length between StepRows\n");
                LogPrint("pow", "X[i]   = %s\n", X[i].GetHex(hashLen));
                LogPrint("pow", "X[i+1] = %s\n", X[i+1].GetHex(hashLen));
                return false;
            }
            if (X[i+1].IndicesBefore(X[i], hashLen, lenIndices)) {
                return false;
                LogPrint("pow", "Invalid solution: Index tree incorrectly ordered\n");
            }
            if (!DistinctIndices(X[i], X[i+1], hashLen, lenIndices)) {
                LogPrint("pow", "Invalid solution: duplicate indices\n");
                return false;
            }
            Xc.emplace_back(X[i], X[i+1], hashLen, lenIndices, CollisionByteLength);
        }
        X = Xc;
        hashLen -= CollisionByteLength;
        lenIndices *= 2;
    }

    assert(X.size() == 1);
    return X[0].IsZero(hashLen);
}

// Explicit instantiations for Equihash<200,9>
template int Equihash<200,9>::InitialiseState(eh_HashState& base_state);
template std::set<std::vector<eh_index>> Equihash<200,9>::BasicSolve(const eh_HashState& base_state, const std::function<bool(EhSolverCancelCheck)> cancelled);
template std::set<std::vector<eh_index>> Equihash<200,9>::OptimisedSolve(const eh_HashState& base_state, const std::function<bool(EhSolverCancelCheck)> cancelled);
template bool Equihash<200,9>::IsValidSolution(const eh_HashState& base_state, std::vector<eh_index> soln);

// Explicit instantiations for Equihash<216,8>
template int Equihash<216,8>::InitialiseState(eh_HashState& base_state);
template std::set<std::vector<eh_index>> Equihash<216,8>::BasicSolve(const eh_HashState& base_state, const std::function<bool(EhSolverCancelCheck)> cancelled);
template std::set<std::vector<eh_index>> Equihash<216,8>::OptimisedSolve(const eh_HashState& base_state, const std::function<bool(EhSolverCancelCheck)> cancelled);
template bool Equihash<216,8>::IsValidSolution(const eh_HashState& base_state, std::vector<eh_index> soln);

// Explicit instantiations for Equihash<208,12>
template int Equihash<208,12>::InitialiseState(eh_HashState& base_state);
template std::set<std::vector<eh_index>> Equihash<208,12>::BasicSolve(const eh_HashState& base_state, const std::function<bool(EhSolverCancelCheck)> cancelled);
template std::set<std::vector<eh_index>> Equihash<208,12>::OptimisedSolve(const eh_HashState& base_state, const std::function<bool(EhSolverCancelCheck)> cancelled);
template bool Equihash<208,12>::IsValidSolution(const eh_HashState& base_state, std::vector<eh_index> soln);

// Explicit instantiations for Equihash<144,5>
template int Equihash<144,5>::InitialiseState(eh_HashState& base_state);
template std::set<std::vector<eh_index>> Equihash<144,5>::BasicSolve(const eh_HashState& base_state, const std::function<bool(EhSolverCancelCheck)> cancelled);
template std::set<std::vector<eh_index>> Equihash<144,5>::OptimisedSolve(const eh_HashState& base_state, const std::function<bool(EhSolverCancelCheck)> cancelled);
template bool Equihash<144,5>::IsValidSolution(const eh_HashState& base_state, std::vector<eh_index> soln);

// Explicit instantiations for Equihash<96,3>
template int Equihash<96,3>::InitialiseState(eh_HashState& base_state);
template std::set<std::vector<eh_index>> Equihash<96,3>::BasicSolve(const eh_HashState& base_state, const std::function<bool(EhSolverCancelCheck)> cancelled);
template std::set<std::vector<eh_index>> Equihash<96,3>::OptimisedSolve(const eh_HashState& base_state, const std::function<bool(EhSolverCancelCheck)> cancelled);
template bool Equihash<96,3>::IsValidSolution(const eh_HashState& base_state, std::vector<eh_index> soln);

// Explicit instantiations for Equihash<96,5>
template int Equihash<96,5>::InitialiseState(eh_HashState& base_state);
template std::set<std::vector<eh_index>> Equihash<96,5>::BasicSolve(const eh_HashState& base_state, const std::function<bool(EhSolverCancelCheck)> cancelled);
template std::set<std::vector<eh_index>> Equihash<96,5>::OptimisedSolve(const eh_HashState& base_state, const std::function<bool(EhSolverCancelCheck)> cancelled);
template bool Equihash<96,5>::IsValidSolution(const eh_HashState& base_state, std::vector<eh_index> soln);

// Explicit instantiations for Equihash<48,5>
template int Equihash<48,5>::InitialiseState(eh_HashState& base_state);
template std::set<std::vector<eh_index>> Equihash<48,5>::BasicSolve(const eh_HashState& base_state, const std::function<bool(EhSolverCancelCheck)> cancelled);
template std::set<std::vector<eh_index>> Equihash<48,5>::OptimisedSolve(const eh_HashState& base_state, const std::function<bool(EhSolverCancelCheck)> cancelled);
template bool Equihash<48,5>::IsValidSolution(const eh_HashState& base_state, std::vector<eh_index> soln);
