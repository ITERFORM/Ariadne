// src/know/prompt.cpp — impl of dd/know/prompt.hpp.
#include "dd/know/prompt.hpp"

#include <algorithm>
#include <cctype>

namespace dd {
namespace know {

PromptComposer& PromptComposer::section(const std::string& text) {
    const size_t b = text.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return *this;
    sections_.push_back(text.substr(b, text.find_last_not_of(" \t\r\n") - b + 1));
    return *this;
}

std::string PromptComposer::str() const {
    std::string out;
    for (const auto& s : sections_) out += (out.empty() ? "" : "\n\n") + s;
    return out;
}

std::string PromptComposer::retrieval_contract(const Contract& c) {
    std::string s =
        "## Retrieval\n"
        "The harness looks things up for you and attaches <retrieval> notes: pointers" +
        std::string(c.skills && c.memory ? " to skills and memories" : c.skills ? " to skills" : c.memory ? " to memories" : "") +
        ", never answers. A note after a tool result means the task shifted or a call failed.\n";
    if (c.skills || c.memory) {
        s += "- A pointer that fits the task MUST be opened before you act on it:";
        if (c.skills) s += " read_file(\"skill://name\")";
        if (c.skills && c.memory) s += ",";
        if (c.memory) s += " read_file(\"mem://id\")";
        s += ". A long skill continues with #2, #3.\n";
    }
    s += "- NEVER guess a path, a command or a name. Not sure where something is:";
    s += c.find ? " find(\"what you are looking for\") first, then read what it returns.\n" : " look with list_dir or a search command first, then read.\n";
    s += "- Live state (GPU, RAM, files, ports, the running model): run the command. Memory is history and may be out of date.\n";
    s += std::string("- The same call twice with the same result means stop") + (c.find ? ": find(), then change the approach.\n" : ": change the approach.\n");
    if (c.memory)
        s += "- The harness already writes every finished turn to the maze. Save only distilled facts with mazemaker_remember "
             "(labels decision:, bug:, fact:, invariant:, ops:, status:).\n";
    if (c.memory && c.plan)
        s += "- When your plan reaches all-done the harness writes the closed ledger as status:plan:<session>:<rev>. "
             "That node says what was planned, not what you learned: before the turn ends, write the findings "
             "yourself — one mazemaker_remember per fact (bug:, decision:, invariant:, ops:), with the number, the "
             "path or the command that proves it. A fix nobody can recall is a fix that will be made twice.\n";
    return s;
}

std::string PromptComposer::workflow_contract(const Contract& c) {
    std::string s =
        "## Workflow\n"
        "- A task with more than one step gets a plan first: write it with `todo`, a few items saying what will change, then work it in order.\n"
        "- The ledger rides the tape as numbered revisions: the first of an epoch carries the whole list, later ones only what moved, and a higher revision number wins. After a rebase the list comes back as the anchor of the new epoch.\n"
        "- Facts come from running, not guessing. Read the file, run the command, look at what it actually returned.\n"
        "- A change is not done when it compiles; it is done when the thing that changed was run and its result was looked at.\n"
        "- Fix the cause. Suppressing an error or special-casing an input to make a symptom disappear is not a fix.\n"
        "- Independent steps run together; steps that depend on an earlier result run in order.\n"
        "- Skills are guidelines you read and adapt, not a ritual to follow exactly.\n";
    if (c.learn) s += "- Close a turn by recording what is true now: the decision, the path, the number — not a retelling of the work.\n";
    return s;
}

std::string PromptComposer::catalogue_map(const std::vector<std::string>& lines) {
    std::string body;
    for (const auto& l : lines)
        if (!l.empty()) body += "- " + l + "\n";
    if (body.empty()) return "";
    return "Catalogues you can search:\n" + body;
}

std::string PromptComposer::harness_contract(const Harness& h) {
    // The one section that says what the model is inside. It exists because a model that is told
    // only "here are tools" reads Ariadne as an anonymous chat box: it guesses paths, re-asks what
    // memory already holds, and never reaches for a child. Every sentence names a capability this
    // turn actually has — a child, which cannot delegate, does not get the delegation line.
    std::string s = "## The harness\n";
    s += "You are the model inside Ariadne " + (h.version.empty() ? std::string("(the C++ agent harness)") : h.version) +
         ", a C++ agent harness: this prompt, the tape that carries the conversation, and " +
         (h.tools ? std::to_string(h.tools) + " tools" : std::string("the tools")) + ".\n";
    s += "- Tools: read_file / write_file / edit_file / list_dir on paths, run_terminal for a shell, "
         "job to start and watch a long command";
    if (h.background) s += " (its status/wait/kill live under job)";
    s += ", todo for the plan.\n";
    if (h.memory)
        s += "- Memory: mazemaker is the harness's past. Every finished turn is written to it, recall for a "
             "turn is attached to you automatically, and mazemaker_recall / mazemaker_get / mazemaker_think "
             "ask it directly. When the work ends with a fact worth keeping (a decision, a path, a number), "
             "save it with mazemaker_remember.\n";
    else
        s += "- Memory: no mazemaker server is configured for this home, so nothing is recalled for you and no\n"
             "  turn is written back. Say so rather than guessing at history.\n";
    if (h.skills)
        s += "- Skills: the catalogue map below names what exists; find(\"what you are doing\") ranks skills, "
             "memories and deferred tools, skills_list(\"<group>\") names them, read_file(\"skill://<name>\") opens one.\n";
    if (h.delegate)
        s += "- Delegation: delegate_task with tasks:[{goal,context},…] (up to three) runs children in "
             "PARALLEL, each with its own transcript, the same working directory and the same tools; read "
             "the same files twice rather than delegating a task you could finish in one call. Results "
             "come back in task order; a child cannot delegate further. A batch holds the turn until it "
             "finishes unless you say action:\"start\": that returns a job_id at once, and the children run "
             "on while you do other work — action:\"status\" polls, \"wait\" blocks, \"collect\" takes the "
             "results, \"kill\" stops them. Collect before the turn ends; the results are all that survives it.\n";
    if (h.depth == 0)
        s += "- You are the top-level turn: the operator reads your answer, so make it the finding, not a "
             "log of the steps.\n";
    else
        s += "- You are a delegate child (depth " + std::to_string(h.depth) +
             "): your summary is the only thing the parent sees. Report what you did, what you found, the "
             "files you touched and what failed.\n";
    s += "- The tape compresses the middle of a long conversation and keeps the plan when it rebases: a "
         "fact that must survive belongs in the plan (todo) or in memory, not in a sentence you hope is "
         "still there.\n";
    return s;
}

PromptComposer::Instructions PromptComposer::instructions_mode(const std::string& configured, bool is_maze) {
    std::string c;
    for (char ch : configured) c.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    if (c == "harness") return Instructions::Harness;
    if (c == "verbatim") return Instructions::Verbatim;
    if (c == "off" || c == "false" || c == "none") return Instructions::Off;
    return is_maze ? Instructions::Harness : Instructions::Verbatim;
}

std::string PromptComposer::server_instructions(Instructions mode, const std::string& text) {
    return mode == Instructions::Verbatim ? text : std::string();
}

}  // namespace know
}  // namespace dd
