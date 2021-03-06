/**
 * Copyright (c) 2015-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD+Patents license found in the
 * LICENSE file in the root directory of this source tree.
 */

// Copyright 2004-present Facebook. All Rights Reserved.
// -*- c++ -*-

#ifndef FAISS_INDEX_IVF_H
#define FAISS_INDEX_IVF_H


#include <vector>


#include "Index.h"
#include "Clustering.h"
#include "Heap.h"


namespace faiss {


/** Encapsulates a quantizer object for the IndexIVF
 *
 * The class isolates the fields that are independent of the storage
 * of the lists (especially training)
 */
struct Level1Quantizer : public Index {
    Index * quantizer;        ///< quantizer that maps vectors to inverted lists
    size_t nlist;             ///< number of possible key values

    /**
     * = 0: use the quantizer as index in a kmeans training
     * = 1: just pass on the training set to the train() of the quantizer
     * = 2: kmeans training on a flat index + add the centroids to the quantizer
     */
    char quantizer_trains_alone;
    bool own_fields;          ///< whether object owns the quantizer

    ClusteringParameters cp; ///< to override default clustering params
    Index *clustering_index; ///< to override index used during clustering

    /// Trains the quantizer and calls train_residual to train sub-quantizers
    void train_q1 (size_t n, const float *x, bool verbose,
                   MetricType metric_type);

    Level1Quantizer (Index * quantizer, size_t nlist);

    Level1Quantizer ();

    ~Level1Quantizer ();

    virtual void add (idx_t n, const float *x) {
        return;
    }
    virtual void search (idx_t n, const float *x, idx_t k,
                         float *distances, idx_t *labels) const  {
        return;
    }
    virtual void reset() {
        return;
    }

};


/** Index based on a inverted file (IVF)
 *
 * In the inverted file, the quantizer (an Index instance) provides a
 * quantization index for each vector to be added. The quantization
 * index maps to a list (aka inverted list or posting list), where the
 * id of the vector is then stored.
 *
 * At search time, the vector to be searched is also quantized, and
 * only the list corresponding to the quantization index is
 * searched. This speeds up the search by making it
 * non-exhaustive. This can be relaxed using multi-probe search: a few
 * (nprobe) quantization indices are selected and several inverted
 * lists are visited.
 *
 * Sub-classes implement a post-filtering of the index that refines
 * the distance estimation from the query to databse vectors.
 */
struct IndexIVF : Level1Quantizer {
    size_t nprobe;            ///< number of probes at query time
    size_t max_codes;         ///< max nb of codes to visit to do a query

    std::vector < std::vector<long> > ids;  ///< Inverted lists for indexes

    size_t code_size;              ///< code size per vector in bytes
    std::vector < std::vector<uint8_t> > codes; // binary codes, size nlist

    /// map for direct access to the elements. Enables reconstruct().
    bool maintain_direct_map;
    std::vector <long> direct_map;

    /** The Inverted file takes a quantizer (an Index) on input,
     * which implements the function mapping a vector to a list
     * identifier. The pointer is borrowed: the quantizer should not
     * be deleted while the IndexIVF is in use.
     */
    IndexIVF (Index * quantizer, size_t d, size_t nlist,
              MetricType metric = METRIC_L2);

    void reset() override;

    /// Trains the quantizer and calls train_residual to train sub-quantizers
    void train(idx_t n, const float* x) override;

    /// Quantizes x and calls add_with_key
    void add(idx_t n, const float* x) override;

    /// Sub-classes that encode the residuals can train their encoders here
    /// does nothing by default
    virtual void train_residual (idx_t n, const float *x);


    /** search a set of vectors, that are pre-quantized by the IVF
     *  quantizer. Fill in the corresponding heaps with the query
     *  results. search() calls this.
     *
     * @param n      nb of vectors to query
     * @param x      query vectors, size nx * d
     * @param assign coarse quantization indices, size nx * nprobe
     * @param centroid_dis
     *               distances to coarse centroids, size nx * nprobe
     * @param distance
     *               output distances, size n * k
     * @param labels output labels, size n * k
     * @param store_pairs store inv list index + inv list offset
     *                     instead in upper/lower 32 bit of result,
     *                     instead of ids (used for reranking).
     */
    virtual void search_preassigned (idx_t n, const float *x, idx_t k,
                                     const idx_t *assign,
                                     const float *centroid_dis,
                                     float *distances, idx_t *labels,
                                     bool store_pairs) const = 0;

    /** assign the vectors, then call search_preassign */
    virtual void search (idx_t n, const float *x, idx_t k,
                         float *distances, idx_t *labels) const override;

    void reconstruct (idx_t key, float* recons) const override;

    /** Reconstruct a subset of the indexed vectors.
     *
     * Overrides default implementation to bypass reconstruct() which requires
     * direct_map to be maintained.
     *
     * @param i0     first vector to reconstruct
     * @param ni     nb of vectors to reconstruct
     * @param recons output array of reconstructed vectors, size ni * d
     */
    void reconstruct_n(idx_t i0, idx_t ni, float* recons) const override;

    /** Similar to search, but also reconstructs the stored vectors (or an
     * approximation in the case of lossy coding) for the search results.
     *
     * Overrides default implementation to avoid having to maintain direct_map
     * and instead fetch the code offsets through the `store_pairs` flag in
     * search_preassigned().
     *
     * @param recons      reconstructed vectors size (n, k, d)
     */
    void search_and_reconstruct (idx_t n, const float *x, idx_t k,
                                 float *distances, idx_t *labels,
                                 float *recons) const override;

    /** Reconstruct a vector given the location in terms of (inv list index +
     * inv list offset) instead of the id.
     *
     * Useful for reconstructing when the direct_map is not maintained and
     * the inv list offset is computed by search_preassigned() with
     * `store_pairs` set.
     */
    virtual void reconstruct_from_offset (long list_no, long offset,
                                          float* recons) const;


    /// Dataset manipulation functions

    long remove_ids(const IDSelector& sel) override;

    /** moves the entries from another dataset to self. On output,
     * other is empty. add_id is added to all moved ids (for
     * sequential ids, this would be this->ntotal */
    virtual void merge_from (IndexIVF &other, idx_t add_id);

    /** copy a subset of the entries index to the other index
     *
     * if subset_type == 0: copies ids in [a1, a2)
     * if subset_type == 1: copies ids if id % a1 == a2
     * if subset_type == 2: copies inverted lists such that a1
     *                      elements are left before and a2 elements are after
     */
    virtual void copy_subset_to (IndexIVF & other, int subset_type,
                                 long a1, long a2) const;

    ~IndexIVF() override;

    size_t get_list_size (size_t list_no) const
    { return ids[list_no].size(); }

    /** intialize a direct map
     *
     * @param new_maintain_direct_map    if true, create a direct map,
     *                                   else clear it
     */
    void make_direct_map (bool new_maintain_direct_map=true);

    /// 1= perfectly balanced, >1: imbalanced
    double imbalance_factor () const;

    /// display some stats about the inverted lists
    void print_stats () const;

    IndexIVF ();
};


struct IndexIVFStats {
    size_t nq;       // nb of queries run
    size_t nlist;    // nb of inverted lists scanned
    size_t ndis;     // nb of distancs computed

    IndexIVFStats () {reset (); }
    void reset ();
};

// global var that collects them all
extern IndexIVFStats indexIVF_stats;





/** Inverted file with stored vectors. Here the inverted file
 * pre-selects the vectors to be searched, but they are not otherwise
 * encoded, the code array just contains the raw float entries.
 */
struct IndexIVFFlat: IndexIVF {

    IndexIVFFlat (
            Index * quantizer, size_t d, size_t nlist_,
            MetricType = METRIC_L2);

    /// same as add_with_ids, with precomputed coarse quantizer
    virtual void add_core (idx_t n, const float * x, const long *xids,
                   const long *precomputed_idx);

    /// implemented for all IndexIVF* classes
    void add_with_ids(idx_t n, const float* x, const long* xids) override;

    void search_preassigned (idx_t n, const float *x, idx_t k,
                             const idx_t *assign,
                             const float *centroid_dis,
                             float *distances, idx_t *labels,
                             bool store_pairs) const override;

    void range_search(
        idx_t n,
        const float* x,
        float radius,
        RangeSearchResult* result) const override;

    /** Update a subset of vectors.
     *
     * The index must have a direct_map
     *
     * @param nv     nb of vectors to update
     * @param idx    vector indices to update, size nv
     * @param v      vectors of new values, size nv*d
     */
    void update_vectors (int nv, idx_t *idx, const float *v);

    void reconstruct_from_offset (long list_no, long offset,
                                  float* recons) const override;

    IndexIVFFlat () {}
};



} // namespace faiss





#endif
