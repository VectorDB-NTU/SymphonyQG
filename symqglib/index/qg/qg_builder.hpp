#pragma once

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <mutex>
#include <unordered_set>
#include <vector>

#include "../../common.hpp"
#include "../../space/space.hpp"
#include "../../third/ngt/hashset.hpp"
#include "../../utils/tools.hpp"
#include "./qg.hpp"

namespace symqg {
constexpr size_t kMaxBsIter = 5;
using CandidateList = std::vector<Candidate<float>>;

class QGBuilder {
   private:
    QuantizedGraph& qg_;
    size_t EF_;
    size_t num_threads_;
    size_t num_nodes_;
    size_t dim_;
    size_t max_degree_;
    size_t max_candidate_pool_size_ = 750;
    size_t max_pruned_size_ = 300;
    DistFunc<float> dist_func_;
    std::vector<CandidateList> new_neighbors_;
    std::vector<CandidateList> pruned_neighbors_;
    void random_init();
    void update_neighbors(bool);
    void heuristic_prune(PID, CandidateList&, CandidateList&, bool);
    void add_reverse_edges(PID, std::vector<std::mutex>&, bool);
    void add_pruned_edges(
        const CandidateList&, const CandidateList&, CandidateList&, float
    );
    void graph_refine();

   public:
    explicit QGBuilder(
        QuantizedGraph& index, uint32_t ef_build, const float* data, size_t num_threads
    )
        : qg_{index}
        , EF_{ef_build}
        , num_threads_{std::min(num_threads, total_threads())}
        , num_nodes_{qg_.num_vertices()}
        , dim_{qg_.dimension()}
        , max_degree_(qg_.degree_bound())
        , dist_func_{space::l2_sqr}
        , new_neighbors_(qg_.num_vertices())
        , pruned_neighbors_(qg_.num_vertices()) {
        omp_set_num_threads(static_cast<int>(num_threads_));
        std::vector<float> medoid =
            space::compute_medioid(data, num_nodes_, dim_, num_threads_);
        PID entry_point = space::compute_entrypoint(
            data, medoid.data(), num_nodes_, dim_, num_threads_, dist_func_
        );
        std::cout << "Setting entry_point to " << entry_point << '\n' << std::flush;
        qg_.set_ep(entry_point);
        qg_.copy_vectors(data);
        random_init();
    }

    void build(bool refine) {
        if (refine) {
            for (size_t i = 0; i < num_nodes_; ++i) {
                pruned_neighbors_[i].clear();
                pruned_neighbors_[i].reserve(max_pruned_size_);
            }
        }

        update_neighbors(refine);

        std::vector<std::mutex> locks(num_nodes_);
#pragma omp parallel for schedule(dynamic)
        for (size_t i = 0; i < num_nodes_; ++i) {
            add_reverse_edges(i, locks, refine);
        }

        // Use pruned edges to refine graph
        if (refine) {
            graph_refine();
        }

        // update qg
#pragma omp parallel for schedule(dynamic)
        for (size_t i = 0; i < num_nodes_; ++i) {
            qg_.update_qg(i, new_neighbors_[i]);
        }
    }

    void check_dup() const {
#pragma omp parallel for
        for (size_t i = 0; i < num_nodes_; ++i) {
            std::unordered_set<PID> edges;
            for (auto nei : new_neighbors_[i]) {
                if (edges.find(nei.id) != edges.end()) {
                    std::cout << "dup edges\n";
                }
                edges.emplace(nei.id);
            }
        }
    }

    [[nodiscard]] auto avg_degree() const -> float {
        size_t degrees = 0;
        for (size_t i = 0; i < num_nodes_; ++i) {
            degrees += qg_.get_degree(i);
        }
        float res = static_cast<float>(degrees) / static_cast<float>(num_nodes_);
        return res;
    }
};

inline void QGBuilder::add_pruned_edges(
    const CandidateList& result,
    const CandidateList& pruned_list,
    CandidateList& new_result,
    float threshold
) {
    size_t start = 0;
    new_result.clear();
    new_result = result;

    while (new_result.size() < max_degree_ && start < pruned_list.size()) {
        const auto& cur = pruned_list[start];
        bool occlude = false;
        const float* cur_data = qg_.get_vector(cur.id);
        float dik_sqr = cur.distance;
        for (auto& nei : new_result) {
            if (cur.id == nei.id) {
                occlude = true;
                break;
            }
            float dij_sqr = nei.distance;
            if (dij_sqr > dik_sqr) {
                break;
            }
            float djk_sqr = dist_func_(qg_.get_vector(nei.id), cur_data, dim_);
            float cosine =
                (dik_sqr + dij_sqr - djk_sqr) / (2 * std::sqrt(dij_sqr * dik_sqr));
            if (cosine > threshold) {
                occlude = true;
                break;
            }
        }

        if (!occlude) {
            new_result.emplace_back(cur);
            std::sort(new_result.begin(), new_result.end());
        }

        ++start;
    }
}

inline void QGBuilder::heuristic_prune(
    PID cur_id, CandidateList& pool, CandidateList& pruned_results, bool refine
) {
    if (pool.empty()) {
        return;
    }
    pruned_results.clear();
    size_t poolsize = pool.size();

    if (poolsize <= max_degree_) {
        for (auto&& nei : pool) {
            pruned_results.emplace_back(nei);
        }
        return;
    }

    std::vector<bool> pruned(poolsize, false);
    size_t start = 0;

    while (pruned_results.size() < max_degree_ && start < poolsize) {
        auto candidate_id = pool[start].id;
        if (pruned[start] || candidate_id == cur_id) {
            ++start;
            continue;
        }

        pruned_results.emplace_back(pool[start]);
        const float* data_j = qg_.get_vector(candidate_id);

        for (size_t i = start + 1; i < poolsize; ++i) {
            if (pruned[i]) {
                continue;
            }
            float dik = pool[i].distance;
            auto djk = dist_func_(data_j, qg_.get_vector(pool[i].id), dim_);

            if (djk < dik) {
                if (refine && pruned_neighbors_[cur_id].size() < max_pruned_size_) {
                    pruned_neighbors_[cur_id].emplace_back(pool[i]);
                }
                pruned[i] = true;
            }
        }

        ++start;
    }
}

inline void QGBuilder::update_neighbors(bool refine) {
    std::vector<HashBasedBooleanSet> visited_list(
        num_threads_, HashBasedBooleanSet(std::max(EF_ * EF_, num_nodes_ / 10))
    );
#pragma omp parallel for schedule(dynamic)
    for (size_t i = 0; i < num_nodes_; ++i) {
        PID cur_id = i;
        auto tid = omp_get_thread_num();
        CandidateList candidates;
        HashBasedBooleanSet& vis = visited_list[tid];
        candidates.reserve(2 * max_candidate_pool_size_);
        vis.clear();
        const float* query = qg_.get_vector(cur_id);
        qg_.find_candidates(query, EF_, candidates, vis);

        // add current neighbors
        for (auto& nei : new_neighbors_[cur_id]) {
            auto neighbor_id = nei.id;
            if (neighbor_id != cur_id && !vis.get(neighbor_id)) {
                candidates.emplace_back(nei);
            }
        }

        size_t min_size = std::min(candidates.size(), max_candidate_pool_size_);
        std::partial_sort(
            candidates.begin(),
            candidates.begin() + static_cast<long>(min_size),
            candidates.end()
        );
        candidates.resize(min_size);

        heuristic_prune(cur_id, candidates, new_neighbors_[cur_id], refine);
    }
}

inline void QGBuilder::add_reverse_edges(
    PID data_id, std::vector<std::mutex>& locks, bool refine
) {
    for (auto&& nei : new_neighbors_[data_id]) {
        PID dst = nei.id;
        bool dup = false;
        CandidateList& dst_neighbors = new_neighbors_[dst];
        std::lock_guard lock(locks[dst]);
        for (auto& nei : dst_neighbors) {
            if (nei.id == data_id) {
                dup = true;
                break;
            }
        }
        if (dup) {
            continue;
        }

        if (dst_neighbors.size() < max_degree_) {
            dst_neighbors.emplace_back(data_id, nei.distance);
        } else {
            CandidateList tmp_pool = dst_neighbors;
            tmp_pool.reserve(max_degree_ + 1);
            tmp_pool.emplace_back(data_id, nei.distance);
            std::sort(tmp_pool.begin(), tmp_pool.end());
            heuristic_prune(dst, tmp_pool, dst_neighbors, refine);
        }
    }
}

inline void QGBuilder::random_init() {
    const PID min_id = 0;
    const PID max_id = num_nodes_ - 1;
#pragma omp parallel for
    for (size_t i = 0; i < num_nodes_; ++i) {
        std::unordered_set<PID> neighbor_set;
        neighbor_set.reserve(max_degree_);
        while (neighbor_set.size() < max_degree_) {
            PID rand_id = rand_integer<PID>(min_id, max_id);
            if (rand_id != i) {
                neighbor_set.emplace(rand_id);
            }
        }

        const float* cur_data = qg_.get_vector(i);
        new_neighbors_[i].reserve(max_degree_);
        for (PID cur_neigh : neighbor_set) {
            new_neighbors_[i].emplace_back(
                cur_neigh, dist_func_(cur_data, qg_.get_vector(cur_neigh), dim_)
            );
        }
    }

#pragma omp parallel for schedule(dynamic)
    for (size_t i = 0; i < num_nodes_; ++i) {
        qg_.update_qg(i, new_neighbors_[i]);
    }
}

inline void QGBuilder::graph_refine() {
    std::cout << "Supplementing edges...\n";

#pragma omp parallel for schedule(dynamic)
    for (size_t i = 0; i < num_nodes_; ++i) {
        CandidateList& cur_neighbors = new_neighbors_[i];
        size_t cur_degree = cur_neighbors.size();
        if (cur_degree >= max_degree_) {
            continue;
        }

        CandidateList& pruned_list = pruned_neighbors_[i];
        CandidateList new_result;
        new_result.reserve(max_degree_);

        float left = 0.5;  // bound of cosine
        float right = 1.0;
        size_t iter = 0;

        std::sort(pruned_list.begin(), pruned_list.end());

        while (iter++ < kMaxBsIter) {
            float mid = (left + right) / 2;
            add_pruned_edges(cur_neighbors, pruned_list, new_result, mid);
            if (new_result.size() < max_degree_) {
                left = mid;
            } else {
                right = mid;
            }
        }

        if (new_result.size() < max_degree_) {
            add_pruned_edges(cur_neighbors, pruned_list, new_result, right);
            if (new_result.size() < max_degree_) {
                std::unordered_set<PID> ids;
                ids.reserve(max_degree_);
                for (auto& neighbor : new_result) {
                    ids.emplace(neighbor.id);
                }
                while (new_result.size() < max_degree_) {
                    PID rand_id = rand_integer<PID>(0, static_cast<PID>(num_nodes_) - 1);
                    if (rand_id != static_cast<PID>(i) && ids.find(rand_id) == ids.end()) {
                        new_result.emplace_back(
                            rand_id,
                            dist_func_(qg_.get_vector(rand_id), qg_.get_vector(i), dim_)
                        );
                        ids.emplace(rand_id);
                    }
                }
            }
        }

        cur_neighbors = new_result;
    }
    std::cout << "Supplementing finished...\n";
}
}  // namespace symqg