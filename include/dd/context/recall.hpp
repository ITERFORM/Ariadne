#pragma once
// dd/context/recall.hpp — what the maze hands a turn: its knowledge in layers, in full.
//
// Recaller: ONE wide embedding recall per turn (the pod serialises recalls and a wide one costs
// what a narrow one costs: 4.0 s for 6 rows, 4.4 s for 24 rows with whole bodies, measured
// 2026-11-12 through the tunnel), partitioned here by label into what the graph actually holds:
//
//   knowledge   decision: bug: invariant: ops: fact: status: signal: user: feedback: reference:
//               -- what somebody wrote down on purpose; the top rows are rendered WHOLE
//   skills      skill:*  -- procedures the maze holds, their head
//   chains      auto:reasoning:*  -- how a similar problem was thought through before
//   episodes    auto:turn:* session:* and the rest -- what was literally said and done
//   facts       afe* rows the pod extracted, and the facts of the top knowledge rows
//
// then the graph around it in parallel: spreading activation from the best knowledge rows read
// back by id, the extracted facts of the top rows, and -- for a question about earlier -- this
// session's own turns by id. Nothing the window already carries comes back (the ledger knows what
// is on the tape), and the block fits a budget that is the deep leg's own.
//
// The partition is done here and not with the pod's `scope`, because that filter leaks (a
// curated-scoped recall answers with transcripts at rank 1: bug:mazemaker-recall-scope-leaks) and
// because one call is the price of any number of layers.
//
// The skill catalogue is not here: dd/know owns it (and every other decision about when and what
// to look up); this is the pod's deep leg.

#include <atomic>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "dd/context/memory.hpp"

namespace dd {
namespace context {

struct Prefetch {
    std::string block;        // <memory-context>… or ""
    std::string intent, skill;   // skill: set by the router, for the card
    int hits = 0;             // recalled rows rendered, every layer
    int knowledge = 0, skills = 0, chains = 0, episodes = 0;   // ... by layer
    int linked = 0, facts = 0, earlier = 0, dropped = 0;       // dropped: already in the window, or under the floor
    std::vector<long> ids;    // what was injected
    // the same rows for a recall card: 'r' knowledge, 's' skill, 'c' reasoning chain, 'p' episode,
    // 'l' linked, 'f' fact, 'e' earlier in this session; content is a snippet
    std::vector<std::pair<char, Hit>> shown;
    double ms = 0;
};

// which layer a label belongs to
enum class Layer { Knowledge, Skill, Chain, Episode, Fact, Plan };
Layer layer_of(const std::string& label);

class Recaller {
public:
    // limit: rows asked of the pod in the one recall (wide: the layers share it)
    explicit Recaller(std::shared_ptr<Memory> mem, int limit = 24) : mem_(std::move(mem)), limit_(limit) {}
    // in_window: ids already on the tape; earlier: this session's turn ids that are not.
    Prefetch run(const std::string& query, const std::set<long>& in_window, const std::vector<long>& earlier,
                 size_t budget_chars = 10000) const;

    static constexpr double kFloor = 0.25;   // a recall row under this similarity is noise, not memory

private:
    std::shared_ptr<Memory> mem_;
    int limit_;
};

}  // namespace context
}  // namespace dd
