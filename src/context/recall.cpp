// src/context/recall.cpp — impl of dd/context/recall.hpp.
#include "dd/context/recall.hpp"

#include "dd/context/maze.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <thread>

namespace dd {
namespace context {

namespace {

std::string squash(const std::string& s) {
    std::string o;
    bool sp = false;
    for (char c : s) {
        if (c == ' ' || c == '\n' || c == '\t' || c == '\r') {
            sp = !o.empty();
            continue;
        }
        if (sp) o.push_back(' ');
        sp = false;
        o.push_back(c);
    }
    return o;
}

std::string utf8_cut(const std::string& s, size_t n) {
    if (s.size() <= n) return s;
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;
    return s.substr(0, n) + "…";
}

bool starts(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }

// A whole row keeps its paragraphs (a decision reads as it was written); a snippet is one line.
// "#id": a row that is cut continues with read_file("mem://id") or mazemaker_get(memory_ids=[id]).
std::string line(const Hit& h, size_t max, bool whole) {
    std::string head = "- " + (h.id > 0 ? "#" + std::to_string(h.id) : std::string()) +
                       (h.label.empty() ? std::string() : (h.id > 0 ? " [" : "[") + h.label + "]");
    if (!whole) return head + " " + utf8_cut(squash(h.content), max);
    std::string body = h.content;
    const size_t b = body.find_first_not_of(" \t\r\n");
    body = b == std::string::npos ? std::string() : body.substr(b, body.find_last_not_of(" \t\r\n") - b + 1);
    const bool cut = body.size() > max || (h.chars > 0 && h.chars > h.content.size());
    body = utf8_cut(body, max);
    std::string out;   // continuation lines are indented so the row reads as one item
    for (char c : body) {
        out.push_back(c);
        if (c == '\n') out += "  ";
    }
    if (cut && h.id > 0) out += "\n  (cut: read_file(\"mem://" + std::to_string(h.id) + "\") has the rest)";
    return head + "\n  " + out;
}

// A row that is bookkeeping of the graph, not something to read: a dream's cluster node carries a
// fragment of its own JSON as content ("\", \"similarity\": 0.84 ..."), and a ledger revision is the tape's.
bool bookkeeping(const std::string& label) { return starts(label, "derived:cluster:") || starts(label, "ariadne:plan:"); }

}  // namespace

Layer layer_of(const std::string& label) {
    if (starts(label, "skill:")) return Layer::Skill;
    if (starts(label, "auto:reasoning:")) return Layer::Chain;
    if (starts(label, "afe")) return Layer::Fact;
    if (starts(label, "ariadne:plan:")) return Layer::Plan;
    if (starts(label, "status:plan:")) return Layer::Knowledge;   // a closed ledger is a finding
    for (const char* p : {"decision:", "bug:", "invariant:", "ops:", "fact:", "status:", "signal:", "user:", "feedback:", "reference:", "rule:"})
        if (starts(label, p)) return Layer::Knowledge;
    return Layer::Episode;   // auto:turn:, session:, claude…, and whatever else a writer called it
}

// ---------------- Recaller ----------------

Prefetch Recaller::run(const std::string& query, const std::set<long>& in_window, const std::vector<long>& earlier,
                       size_t budget) const {
    Prefetch out;
    if (!mem_ || !mem_->configured()) return out;
    const auto t0 = std::chrono::steady_clock::now();
    out.intent = mem_->intent(query);

    // one wide recall, cut into layers here: the pod's order inside a layer is its rank
    std::vector<Hit> knowledge, skills, chains, episodes, podfacts;
    std::set<long> seen(in_window);
    for (auto& h : mem_->recall(query, limit_)) {
        if (in_window.count(h.id) || (h.score > 0 && h.score < kFloor)) {
            ++out.dropped;
            continue;
        }
        if (h.content.empty() || bookkeeping(h.label) || !seen.insert(h.id).second) continue;
        switch (layer_of(h.label)) {
            case Layer::Knowledge: knowledge.push_back(std::move(h)); break;
            case Layer::Skill: skills.push_back(std::move(h)); break;
            case Layer::Chain: chains.push_back(std::move(h)); break;
            case Layer::Fact: podfacts.push_back(std::move(h)); break;
            case Layer::Plan: break;   // never reached: bookkeeping() dropped it; the layer exists for layer_of's callers
            case Layer::Episode: episodes.push_back(std::move(h)); break;
        }
    }
    // the graph walks out from what was written on purpose; failing that, from the best row of any kind
    std::vector<long> roots;
    for (const auto& h : knowledge)
        if (roots.size() < 2) roots.push_back(h.id);
    if (roots.empty())
        for (const auto* v : {&skills, &chains, &episodes})
            if (roots.empty() && !v->empty()) roots.push_back(v->front().id);

    // the graph around it, in parallel: each branch writes only its own result
    std::vector<Hit> linked, facts, before;
    std::thread walk([&] {
        std::vector<long> ids;
        for (long root : roots) {
            for (const auto& a : mem_->think(root, 2)) {
                if (seen.count(a.id) || starts(a.label, "skill") || bookkeeping(a.label)) continue;
                if (std::find(ids.begin(), ids.end(), a.id) != ids.end()) continue;
                ids.push_back(a.id);
                if (ids.size() == 4) break;
            }
            if (ids.size() == 4) break;
        }
        if (!ids.empty()) linked = mem_->get(ids);
    });
    std::thread extract([&] {
        for (long root : roots) {
            auto f = mem_->facts(root, 3);
            facts.insert(facts.end(), f.begin(), f.end());
        }
    });
    std::thread history([&] {
        if (out.intent != "temporal" || earlier.empty()) return;
        std::vector<long> ids(earlier.end() - static_cast<long>(std::min<size_t>(3, earlier.size())), earlier.end());
        before = mem_->get(ids);
    });
    walk.join();
    extract.join();
    history.join();
    for (auto& f : podfacts) facts.push_back(std::move(f));   // the pod's own fact rows join the extracted ones

    std::string body;
    // whole: the first `whole` rows of the layer are rendered in full (up to max_whole chars); the rest are snippets
    auto add = [&](char kind, const std::string& head, const std::vector<Hit>& rows, size_t cap, size_t whole, size_t max_whole,
                   size_t max_snip, int& n) {
        bool headed = false;
        size_t k = 0;
        for (const auto& h : rows) {
            if (h.content.empty() || k >= cap) continue;
            const bool full = k < whole;
            const std::string l = line(h, full ? max_whole : max_snip, full) + "\n";
            if (body.size() + l.size() > budget) {
                if (!full) return;   // no room for a snippet: the layer is done
                // no room for a whole row: a snippet of it still says it exists
                const std::string s = line(h, max_snip, false) + "\n";
                if (body.size() + s.size() > budget) return;
                if (!headed) body += head + "\n", headed = true;
                body += s;
            } else {
                if (!headed) body += head + "\n", headed = true;
                body += l;
            }
            ++k;
            out.ids.push_back(h.id);
            Hit s = h;
            s.content = utf8_cut(squash(h.content), 240);
            out.shown.emplace_back(kind, std::move(s));
            ++n;
        }
    };
    add('r', "Knowledge (written down on purpose; read it as the harness's own notes):", knowledge, 5, 2, 2400, 480, out.knowledge);
    add('s', "Skills the maze holds for this (the head; read_file(\"mem://id\") for the rest):", skills, 2, 1, 1400, 400, out.skills);
    add('c', "How this was reasoned through before:", chains, 2, 0, 0, 600, out.chains);
    add('p', "Earlier sessions on this (what was said and done):", episodes, 3, 0, 0, 420, out.episodes);
    add('l', "Connected in the graph:", linked, 4, 0, 0, 360, out.linked);
    add('f', "Extracted facts:", facts, 6, 0, 0, 200, out.facts);
    add('e', "Earlier in this session (out of the window):", before, 3, 0, 0, 420, out.earlier);
    out.hits = out.knowledge + out.skills + out.chains + out.episodes;
    out.block = context_block(body);
    out.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return out;
}

}  // namespace context
}  // namespace dd
