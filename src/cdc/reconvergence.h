#ifndef OPENCDC_CDC_RECONVERGENCE_H
#define OPENCDC_CDC_RECONVERGENCE_H

#include <vector>

#include "cdc/crossing.h"
#include "clock/domain.h"
#include "ir/graph.h"

namespace opencdc::cdc {

class ReconvergenceAnalyzer {
   public:
    std::vector<Finding> analyze(const ir::Graph& graph,
                                 const std::vector<clock::ClockDomain>& domains,
                                 const std::vector<Finding>& crossings);

    void set_max_hops(int hops) {
        max_hops_ = hops < 1 ? 1 : hops;
    }

   private:
    int max_hops_ = 3;
    std::vector<uint64_t> find_fanout_sources(const ir::Graph& graph,
                                              const std::vector<clock::ClockDomain>& domains) const;

    std::vector<Finding> check_pairs(const ir::Graph& graph,
                                     const std::vector<clock::ClockDomain>& domains,
                                     const std::vector<Finding>& crossings,
                                     uint64_t source_id) const;
};

}  // namespace opencdc::cdc

#endif  // OPENCDC_CDC_RECONVERGENCE_H
