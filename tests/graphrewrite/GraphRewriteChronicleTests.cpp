// The chronicle layer: substitutions, the event record, dialogue and the
// month-by-month composer.
//
// The questions worth asking here are the ones that are easy to get wrong and
// invisible in the prose: that a grammar choice is reproducible, that a
// condition looking "back" cannot see the event it is attached to, that a
// month's budget loses no events (they become counts), and that threading
// puts sentences about one set of people in one paragraph.

#include "TestHarness.hpp"
#include "graphrewrite/compiler/Compiler.hpp"
#include "graphrewrite/runtime/Chronicle.hpp"
#include "graphrewrite/runtime/Chronicler.hpp"
#include "graphrewrite/runtime/ConsequenceGraph.hpp"
#include "graphrewrite/runtime/DialogueBook.hpp"
#include "graphrewrite/runtime/FactsFile.hpp"
#include "graphrewrite/runtime/Grammar.hpp"
#include "graphrewrite/runtime/Narrator.hpp"
#include "graphrewrite/runtime/StoryRunner.hpp"

#include <fstream>
#include <set>
#include <sstream>

using namespace graphrewrite;
using namespace graphrewrite::runtime;
using namespace graphrewrite::testing;

namespace {

std::string readAsset(const char *name)
{
  std::ifstream file(std::string(GRAPHREWRITE_ASSET_DIR) + "/" + name);
  std::ostringstream buffer;

  buffer << file.rdbuf();

  return buffer.str();
}

// --- the substitution grammar -------------------------------------------------

void grammarBasics()
{
  DiagnosticSink sink;
  Grammar grammar;

  check(Grammar::isSubLine("sub anger: fury"), "a sub line is recognised");
  check(!Grammar::isSubLine("subtle: no"), "a word starting with 'sub' is not a sub line");

  check(grammar.addLine("sub anger: fury | a cold rage | ice", "t", 1u, sink), "a sub parses");
  checkEqual(grammar.alternativesOf("anger").size(), size_t(3), "three alternatives");

  check(grammar.addLine("sub mood: <anger>", "t", 2u, sink), "a nesting sub parses");

  // Same key, same words: a story is reproducible.
  const std::string once = grammar.expand("he felt <anger>", 7u);
  const std::string again = grammar.expand("he felt <anger>", 7u);

  checkEqual(once, again, "the same key gives the same expansion");
  check(once != "he felt <anger>", "the placeholder was replaced");

  // Nesting resolves through to a leaf.
  const std::string nested = grammar.expand("<mood>", 7u);

  check(nested.find('<') == std::string::npos, "a nested sub resolves to a leaf");

  // Different keys reach different alternatives across a spread of rolls.
  std::set<std::string> seen;

  for (uint64_t key = 0u; key < 60u; ++key)
  {
    seen.insert(grammar.expand("<anger>", key));
  }

  checkEqual(seen.size(), size_t(3), "every alternative is reachable");

  // An unknown name is left exactly as written rather than silently emptied.
  checkEqual(grammar.expand("a <nosuchsub> here", 1u), std::string("a <nosuchsub> here"), "an unknown sub survives untouched");
  checkEqual(grammar.expand("2 < 3 and 4 > 1", 1u), std::string("2 < 3 and 4 > 1"), "a bare angle bracket is not a placeholder");

  // Two placeholders in one text roll separately.
  Grammar coin;

  coin.add("coin", {"a", "b"});

  bool differed = false;

  for (uint64_t key = 0u; key < 40u && !differed; ++key)
  {
    const std::string pair = coin.expand("<coin><coin>", key);

    differed = pair == "ab" || pair == "ba";
  }

  check(differed, "two placeholders in one text can differ");
}

void grammarWeights()
{
  DiagnosticSink sink;
  Grammar grammar;

  check(grammar.addLine("sub pick: heavy *4 | light", "t", 1u, sink), "a weighted sub parses");
  checkEqual(grammar.alternativesOf("pick").size(), size_t(5), "a weight of 4 is four copies");

  uint32_t heavy = 0u;

  for (uint64_t key = 0u; key < 200u; ++key)
  {
    heavy += grammar.expand("<pick>", key) == "heavy" ? 1u : 0u;
  }

  check(heavy > 120u && heavy < 190u, "the weighted alternative dominates without taking everything");
}

void grammarErrors()
{
  DiagnosticSink sink;
  Grammar grammar;

  check(!grammar.addLine("sub broken", "t", 1u, sink), "a sub with no colon is an error");
  check(!grammar.addLine("sub empty:", "t", 2u, sink), "a sub with no alternatives is an error");
  checkEqual(grammar.size(), size_t(0), "neither was stored");

  // A self-referencing sub terminates rather than hanging.
  Grammar loop;

  loop.add("loop", {"<loop>"});

  const std::string out = loop.expand("<loop>", 1u);

  check(!out.empty(), "a self-referencing sub terminates");
}

// --- the chronicle -------------------------------------------------------------

// Two rules, A/B and A/C, enough to ask every question the conditions ask.
const char *const rules = R"(
let Person knows Person.
let Person met Person.
let Person hates Person.

rule they-meet:
when A knows B
then A knows B, A met B.

rule they-fall-out:
when A knows C
then A knows C, A hates C.
)";

struct Fixture
{
  compiler::CompiledProgram program;
  compiler::UsageValues usage;
  DiagnosticSink sink;
  bool ok = false;

  uint32_t ruleOf(const std::string &title) const
  {
    for (uint32_t i = 0u; i < program.ruleNames.size(); ++i)
    {
      if (ruleTitleOf(program.ruleNames[i]) == title)
      {
        return i;
      }
    }

    return ~uint32_t(0);
  }
};

void compileFixture(Fixture &out)
{
  out.ok = compiler::compile(rules, "chronicle.graphwrite", compiler::CompileOptions(), out.sink, out.program, out.usage);

  if (!out.ok)
  {
    for (const Diagnostic &diagnostic : out.sink.all())
    {
      std::cerr << "    " << diagnostic.format("chronicle") << "\n";
    }
  }
}

FiredEvent fired(const uint32_t tick, const uint32_t rule, const std::vector<uint32_t> &bound)
{
  FiredEvent event;

  event.tick = tick;
  event.rule = rule;
  event.bound = bound;

  return event;
}

void chronicleQueries()
{
  Fixture fixture;

  compileFixture(fixture);
  check(fixture.ok, "the chronicle fixture compiles");

  if (!fixture.ok)
  {
    return;
  }

  const uint32_t meet = fixture.ruleOf("they-meet");
  const uint32_t fallOut = fixture.ruleOf("they-fall-out");

  check(meet != ~uint32_t(0) && fallOut != ~uint32_t(0), "both rules are in the programme");

  Narrator narrator;
  DiagnosticSink sink;

  narrator.load("weight 5 they-meet\ncategory meeting: they-meet\nsummary meeting: a meeting / {n} meetings\n", "n", sink);

  // 1 and 2 meet at week 10; 1 and 3 fall out at week 20; 1 and 2 meet again
  // at week 40. Node 1000 is above the ceiling: a date, not a person.
  std::vector<FiredEvent> log{
      fired(10u, meet, {1u, 2u, 1000u}),
      fired(20u, fallOut, {1u, 3u}),
      fired(40u, meet, {1u, 2u}),
  };

  Chronicle chronicle;

  chronicle.setNodeCeiling(1000u);
  chronicle.build(log, fixture.program, narrator);

  checkEqual(chronicle.size(), size_t(3), "every firing is kept");
  checkEqual(chronicle.cast().size(), size_t(3), "the date is not in the cast");
  checkEqual(chronicle.at(0u).weight, 5u, "a weight line reaches the chronicle");
  checkEqual(chronicle.at(0u).category, std::string("meeting"), "a category line reaches the chronicle");
  checkEqual(chronicle.at(1u).weight, 1u, "an unweighted rule is worth one");

  check(chronicle.took(1u, "they-meet", 50u), "the participant index answers");
  check(!chronicle.took(3u, "they-meet", 50u), "someone who was not there did not take part");

  // The window is STRICTLY before, which is what stops an event being its own
  // evidence when a dialogue line asks at its own tick.
  check(!chronicle.took(1u, "they-meet", 10u), "an event at the moment asked about does not count");
  check(chronicle.took(1u, "they-meet", 11u), "an event one week earlier does");

  check(chronicle.took(1u, "they-meet", 50u, Window{20u}), "a bounded window reaches back far enough");
  check(!chronicle.took(1u, "they-meet", 100u, Window{20u}), "and not further than it says");

  check(chronicle.tookTogether(1u, 2u, "they-meet", 50u), "a pair in the same event");
  check(!chronicle.tookTogether(2u, 3u, "they-meet", 50u), "two people who were never in one together");
  check(!chronicle.tookTogether(1u, 3u, "they-meet", 50u), "the right pair, the wrong rule");

  checkEqual(chronicle.count(1u, "they-meet", 50u), 2u, "counting repeats");
  checkEqual(chronicle.count(1u, "they-meet", 30u), 1u, "counting respects the moment");

  checkEqual(narrator.summaryPhrase("meeting", 1u), std::string("a meeting"), "the singular summary");
  checkEqual(narrator.summaryPhrase("meeting", 3u), std::string("three meetings"), "the plural summary spells the count");
  checkEqual(narrator.summaryPhrase("nosuch", 3u), std::string(), "a category with no summary line says nothing");
}

// --- dialogue -------------------------------------------------------------------

void dialogueSelection()
{
  Fixture fixture;

  compileFixture(fixture);

  if (!fixture.ok)
  {
    return;
  }

  const uint32_t meet = fixture.ruleOf("they-meet");
  const uint32_t fallOut = fixture.ruleOf("they-fall-out");

  Narrator narrator;
  DiagnosticSink narrationSink;

  narrator.load("", "n", narrationSink);

  const char *const book = R"(
sub said: said

dialogue they-fall-out:
  when A met C in they-meet:
    A to C: "We were friends once, {C}."
  when A in they-meet:
    A: "I have known better company."
  unless A in they-meet:
    A: "I do not know you and I do not want to."
)";

  DialogueBook dialogue;
  DiagnosticSink sink;

  dialogue.setGrammar(&narrator.grammar());

  check(dialogue.load(book, "t.dialogue", sink), "the dialogue file parses");
  checkEqual(dialogue.blockCount(), size_t(1), "one block");
  check(dialogue.hasLinesFor("they-fall-out"), "the block is found by rule title");
  check(!dialogue.hasLinesFor("they-meet"), "a rule with no block has none");

  NameTable names(1u);

  names.setName(1u, "Aldric");
  names.setName(2u, "Bertwald");
  names.setName(3u, "Cenric");

  const auto genderOf = [](uint32_t) { return Gender::Masculine; };

  // Nothing has happened: only the `unless` clause holds.
  {
    Chronicle empty;

    const std::vector<std::string> lines =
        dialogue.speak(fired(20u, fallOut, {1u, 3u}), empty, fixture.program, names, genderOf);

    checkEqual(lines.size(), size_t(1), "a stranger says one line");
    check(lines[0].find("I do not know you") != std::string::npos, "the unless clause is the one that held");
  }

  // 1 met 3 before: the first clause holds and outranks the others.
  {
    std::vector<FiredEvent> log{fired(10u, meet, {1u, 3u})};

    Chronicle chronicle;

    chronicle.build(log, fixture.program, narrator);

    const std::vector<std::string> lines =
        dialogue.speak(fired(20u, fallOut, {1u, 3u}), chronicle, fixture.program, names, genderOf);

    checkEqual(lines.size(), size_t(1), "one line");
    checkEqual(lines[0], std::string("Aldric said to Cenric, \"We were friends once, Cenric.\""), "the pair clause, rendered");
  }

  // 1 met someone, but not 3: the first clause fails, the second holds.
  {
    std::vector<FiredEvent> log{fired(10u, meet, {1u, 2u})};

    Chronicle chronicle;

    chronicle.build(log, fixture.program, narrator);

    const std::vector<std::string> lines =
        dialogue.speak(fired(20u, fallOut, {1u, 3u}), chronicle, fixture.program, names, genderOf);

    checkEqual(lines.size(), size_t(1), "one line");
    check(lines[0].find("better company") != std::string::npos, "the second clause held");
    check(lines[0].rfind("Aldric said,", 0u) == 0, "a line with no listener has no 'to'");
  }

  // A rule with no block says nothing at all.
  {
    Chronicle empty;

    check(dialogue.speak(fired(20u, meet, {1u, 2u}), empty, fixture.program, names, genderOf).empty(), "no block, no line");
  }
}

void dialogueCountsAndChance()
{
  Fixture fixture;

  compileFixture(fixture);

  if (!fixture.ok)
  {
    return;
  }

  const uint32_t meet = fixture.ruleOf("they-meet");
  const uint32_t fallOut = fixture.ruleOf("they-fall-out");

  Narrator narrator;
  DiagnosticSink narrationSink;

  narrator.load("", "n", narrationSink);

  DialogueBook dialogue;
  DiagnosticSink sink;

  check(dialogue.load(R"(
dialogue they-fall-out:
  when A in they-meet twice:
    A: "Twice now."
  else:
    A: "Once is once."
)",
                      "t.dialogue", sink),
        "a `twice` condition parses");

  NameTable names(1u);

  names.setName(1u, "Aldric");

  const auto genderOf = [](uint32_t) { return Gender::Masculine; };

  std::vector<FiredEvent> log{fired(10u, meet, {1u, 2u})};

  Chronicle once;

  once.build(log, fixture.program, narrator);

  check(dialogue.speak(fired(30u, fallOut, {1u, 3u}), once, fixture.program, names, genderOf)[0].find("Once is once") !=
            std::string::npos,
        "one prior meeting does not satisfy `twice`");

  log.push_back(fired(20u, meet, {1u, 2u}));

  Chronicle twice;

  twice.build(log, fixture.program, narrator);

  check(dialogue.speak(fired(30u, fallOut, {1u, 3u}), twice, fixture.program, names, genderOf)[0].find("Twice now") != std::string::npos,
        "two do");

  // `chance` silences some firings and not others, deterministically.
  DialogueBook rare;
  DiagnosticSink rareSink;

  rare.load("dialogue they-fall-out chance 50:\n  A: \"Something.\"\n", "t.dialogue", rareSink);

  uint32_t spoke = 0u;

  for (uint32_t tick = 0u; tick < 100u; ++tick)
  {
    spoke += rare.speak(fired(tick, fallOut, {1u, 3u}), twice, fixture.program, names, genderOf).empty() ? 0u : 1u;
  }

  check(spoke > 25u && spoke < 75u, "a chance of 50 silences roughly half");

  const bool first = rare.speak(fired(7u, fallOut, {1u, 3u}), twice, fixture.program, names, genderOf).empty();
  const bool second = rare.speak(fired(7u, fallOut, {1u, 3u}), twice, fixture.program, names, genderOf).empty();

  checkEqual(first, second, "the chance roll is reproducible for one firing");
}

void dialogueErrors()
{
  DialogueBook dialogue;
  DiagnosticSink sink;

  check(!dialogue.load("  A: \"orphan\"\n", "t", sink), "a line outside a block is an error");
  check(!dialogue.load("dialogue a-rule:\n  when A of b-rule:\n    A: \"x\"\n", "t", sink), "a condition missing 'in' is an error");
  check(!dialogue.load("dialogue a-rule:\n  A said nothing\n", "t", sink), "a speaker line with no colon is an error");
  check(!dialogue.load("dialogue:\n", "t", sink), "a block with no rule title is an error");
}

// --- the composer ----------------------------------------------------------------

void composerBudgetAndThreads()
{
  Fixture fixture;

  compileFixture(fixture);

  if (!fixture.ok)
  {
    return;
  }

  const uint32_t meet = fixture.ruleOf("they-meet");
  const uint32_t fallOut = fixture.ruleOf("they-fall-out");

  Narrator narrator;
  DiagnosticSink sink;

  narrator.load("they-meet: {A} met {B}.\n"
                "they-fall-out: {A} fell out with {C}.\n"
                "weight 20 they-meet\n"
                "category meeting: they-meet\n"
                "category quarrel: they-fall-out\n"
                "summary meeting: a meeting / {n} meetings\n"
                "summary quarrel: a quarrel / {n} quarrels\n",
                "n", sink);

  check(sink.errorCount() == 0u, "the composer's narration parses");

  ChronicleSettings settings;

  settings.startWeek = 0u;
  settings.eventsPerMonth = 3u;
  settings.dialoguePerMonth = 0u;
  settings.nodeCeiling = 1000u;

  // One month (weeks 1-4), six events: two threads of people, 1/2 and 3/4,
  // plus two extra quarrels that will not fit the budget.
  std::vector<FiredEvent> log{
      fired(1u, meet, {1u, 2u}), fired(2u, fallOut, {1u, 5u}),  fired(2u, meet, {3u, 4u}),
      fired(3u, fallOut, {3u, 6u}), fired(3u, fallOut, {7u, 8u}), fired(4u, fallOut, {9u, 10u}),
  };

  NameTable names(1u);

  for (uint32_t id = 1u; id <= 10u; ++id)
  {
    names.setName(id, "P" + std::to_string(id));
  }

  Chronicler chronicler(narrator, fixture.program, settings);

  const std::vector<ChronicledMonth> months = chronicler.compose(log, names, [](uint32_t) { return Gender::Masculine; });

  checkEqual(months.size(), size_t(1), "four weeks are one month");

  if (months.empty())
  {
    return;
  }

  const ChronicledMonth &month = months[0];

  checkEqual(month.told, 3u, "the budget is held");
  checkEqual(month.told + month.summarised, 6u, "no event is lost: what is not told is counted");

  // The two meetings weigh 20, so they are kept along with one quarrel, and
  // the quarrel kept is the earliest of them.
  const std::string all = Chronicler::joinParagraph(month.paragraphs);

  check(all.find("P1 met P2") != std::string::npos, "the first meeting is told");
  check(all.find("P3 met P4") != std::string::npos, "the second meeting is told");
  check(all.find("Elsewhere that month") != std::string::npos, "the dropped events are summarised");
  check(all.find("three quarrels") != std::string::npos, "and counted by category");

  // Sharing ONE person is not enough to be one narrative: P1's meeting and
  // P1's quarrel are two things that happen to involve him, and run together
  // they read as a ramble. They go in separate blocks, and P3's meeting -
  // sharing nobody - in another again.
  for (const std::string &paragraph : month.paragraphs)
  {
    check(!(paragraph.find("P1 met P2") != std::string::npos && paragraph.find("fell out with P5") != std::string::npos),
          "one person in common and no cause is not one block");

    check(!(paragraph.find("P1 met P2") != std::string::npos && paragraph.find("P3 met P4") != std::string::npos),
          "unrelated people are not in one block");
  }

  check(month.blocks.size() >= 3u, "three unrelated things are three blocks");

  // Blocks that share a person are kept ADJACENT even though they are not
  // merged, so a man's several strands sit together.
  size_t first = month.blocks.size();
  size_t second = month.blocks.size();

  for (size_t i = 0u; i < month.blocks.size(); ++i)
  {
    const std::string text = Chronicler::joinParagraph(month.blocks[i].paragraphs);

    if (text.find("P1 met P2") != std::string::npos)
    {
      first = i;
    }

    if (text.find("fell out with P5") != std::string::npos)
    {
      second = i;
    }
  }

  check(first < month.blocks.size() && second < month.blocks.size(), "both of P1's blocks are there");

  if (first < month.blocks.size() && second < month.blocks.size())
  {
    const size_t gap = first < second ? second - first : first - second;

    checkEqual(gap, size_t(1), "two blocks about the same man are adjacent");
  }

  // Two people in common IS one narrative: the same pair, twice.
  {
    std::vector<FiredEvent> pair{fired(1u, meet, {1u, 2u}), fired(2u, fallOut, {1u, 2u})};

    ChronicleSettings paired = settings;

    paired.eventsPerMonth = 0u;

    const std::vector<ChronicledMonth> together =
        Chronicler(narrator, fixture.program, paired).compose(pair, names, [](uint32_t) { return Gender::Masculine; });

    checkEqual(together[0].blocks.size(), size_t(1), "two people in common is one block");
  }

  // A budget of zero tells everything and summarises nothing.
  ChronicleSettings all_in = settings;

  all_in.eventsPerMonth = 0u;

  const std::vector<ChronicledMonth> full =
      Chronicler(narrator, fixture.program, all_in).compose(log, names, [](uint32_t) { return Gender::Masculine; });

  checkEqual(full[0].told, 6u, "a budget of zero tells everything");
  checkEqual(full[0].summarised, 0u, "and summarises nothing");
  check(Chronicler::joinParagraph(full[0].paragraphs).find("Elsewhere") == std::string::npos, "with no closing count");
}

void composerMonthsAndDeterminism()
{
  Fixture fixture;

  compileFixture(fixture);

  if (!fixture.ok)
  {
    return;
  }

  const uint32_t meet = fixture.ruleOf("they-meet");

  Narrator narrator;
  DiagnosticSink sink;

  narrator.load("they-meet: {A} met {B}.\nsub then: Then | Later,\n", "n", sink);

  ChronicleSettings settings;

  settings.startWeek = 0u;
  settings.nodeCeiling = 1000u;
  settings.eventsPerMonth = 0u;
  settings.dialoguePerMonth = 0u;

  // Weeks 1 and 2 are January; week 30 is well into the year.
  std::vector<FiredEvent> log{fired(1u, meet, {1u, 2u}), fired(2u, meet, {1u, 2u}), fired(30u, meet, {1u, 2u})};

  NameTable names(1u);

  names.setName(1u, "Aldric");
  names.setName(2u, "Bertwald");

  Chronicler chronicler(narrator, fixture.program, settings);

  const auto genderOf = [](uint32_t) { return Gender::Masculine; };

  const std::vector<ChronicledMonth> months = chronicler.compose(log, names, genderOf);

  checkEqual(months.size(), size_t(2), "two months are two entries");
  check(months[0].key < months[1].key, "months come out in order");
  check(!months[0].heading.empty() && months[0].heading != months[1].heading, "each month has its own heading");

  // A second sentence in one thread takes a connective.
  check(months[0].paragraphs[0].find("Then") != std::string::npos || months[0].paragraphs[0].find("Later,") != std::string::npos,
        "the second sentence of a paragraph takes a connective");

  // The whole composition is a pure function of its inputs.
  NameTable again(1u);

  again.setName(1u, "Aldric");
  again.setName(2u, "Bertwald");

  checkEqual(chronicler.text(log, again, genderOf), chronicler.text(log, names, genderOf), "composition is reproducible");
}

// --- cause ------------------------------------------------------------------------
//
// The causal relation is read off the programme, never declared, so what is
// worth testing is that it says yes only where an edge actually carries the
// cause across.

const char *const causalRules = R"(
let Person knows Person.
let Person met Person.
let Person hates Person.
let Person is alive.

rule they-meet:
when A knows B
then A knows B, A met B.

rule they-fall-out:
when A met B
then A met B, A hates B.

rule unrelated:
when A is alive
then A is alive.
)";

void consequenceGraph()
{
  DiagnosticSink sink;
  compiler::CompiledProgram program;
  compiler::UsageValues usage;

  const bool ok = compiler::compile(causalRules, "cause.graphwrite", compiler::CompileOptions(), sink, program, usage);

  check(ok, "the cause fixture compiles");

  if (!ok)
  {
    for (const Diagnostic &diagnostic : sink.all())
    {
      std::cerr << "    " << diagnostic.format("cause") << "\n";
    }

    return;
  }

  const auto ruleOf = [&program](const std::string &title) {
    for (uint32_t i = 0u; i < program.ruleNames.size(); ++i)
    {
      if (ruleTitleOf(program.ruleNames[i]) == title)
      {
        return i;
      }
    }

    return ~uint32_t(0);
  };

  const uint32_t meet = ruleOf("they-meet");
  const uint32_t fallOut = ruleOf("they-fall-out");
  const uint32_t idle = ruleOf("unrelated");

  ConsequenceGraph graph;

  graph.build(program);

  // they-meet inserts `? met ?`, which they-fall-out matches.
  check(graph.leadsTo(meet, fallOut), "a rule leads to one that matches what it inserts");
  check(!graph.leadsTo(fallOut, meet), "and not the other way round");
  check(!graph.leadsTo(meet, idle), "a rule that shares no produced edge leads nowhere");

  const std::vector<std::string> types = graph.linkTypes(meet, fallOut, program);

  checkEqual(types.size(), size_t(1), "one edge type carries it");

  if (!types.empty())
  {
    checkEqual(types[0], std::string("? met ?"), "and it is the one that was inserted");
  }

  // `? knows ?` is restated by they-meet, not produced by it, so it is not a
  // reason to say one rule brought another about. This is the whole of what
  // keeps the relation from linking everything to everything.
  check(!graph.leadsTo(meet, meet), "a rule does not cause itself through an edge it merely restates");

  // An `incidental` type is dropped from the links entirely.
  compiler::EdgeTypeId met = compiler::invalidEdgeType;

  program.edgeTypes.find("? met ?", met);

  ConsequenceGraph ignoring;

  ignoring.build(program, {met});

  check(!ignoring.leadsTo(meet, fallOut), "an incidental edge type carries no cause");

  // At the level of FIRINGS the cause must come first and share a person.
  ChronicleEvent cause;
  ChronicleEvent effect;

  cause.tick = 10u;
  cause.rule = meet;
  cause.bound = {1u, 2u};

  effect.tick = 20u;
  effect.rule = fallOut;
  effect.bound = {1u, 3u};

  check(graph.linked(cause, effect, 1000u), "two firings with someone in common are linked");

  effect.bound = {4u, 5u};
  check(!graph.linked(cause, effect, 1000u), "two firings with nobody in common are not");

  effect.bound = {1u, 3u};
  effect.tick = 5u;
  check(!graph.linked(cause, effect, 1000u), "and an effect cannot come before its cause");

  // A node at or above the ceiling is a date, not a person in common.
  cause.bound = {9000u};
  effect.bound = {9000u};
  effect.tick = 20u;
  check(!graph.linked(cause, effect, 1000u), "a shared week node is not someone in common");
}

// --- dialogue for a whole arc -------------------------------------------------------

void chainDialogue()
{
  Fixture fixture;

  compileFixture(fixture);

  if (!fixture.ok)
  {
    return;
  }

  const uint32_t meet = fixture.ruleOf("they-meet");
  const uint32_t fallOut = fixture.ruleOf("they-fall-out");

  Narrator narrator;
  DiagnosticSink narrationSink;

  narrator.load("", "n", narrationSink);

  DialogueBook dialogue;
  DiagnosticSink sink;

  dialogue.setGrammar(&narrator.grammar());

  const char *const book = R"(
dialogue they-meet:
  A: "On its own."

dialogue chain they-meet + they-fall-out:
  when 2.C in they-meet:
    1.A to 1.B: "We were introduced, once."
    2.A to 2.C: "And you I have had enough of."
  else:
    1.A to 1.B: "A pleasure."
    2.A to 2.C: "You, less so."
)";

  check(dialogue.load(book, "t.dialogue", sink), "a chain block parses");
  checkEqual(dialogue.blockCount(), size_t(2), "two blocks");

  NameTable names(1u);

  for (uint32_t id = 1u; id <= 6u; ++id)
  {
    names.setName(id, "P" + std::to_string(id));
  }

  const auto genderOf = [](uint32_t) { return Gender::Masculine; };

  const FiredEvent first = fired(10u, meet, {1u, 2u});
  const FiredEvent second = fired(11u, fallOut, {1u, 3u});

  Chronicle empty;

  // Both parts offered together: the chain block wins over the single-rule
  // one, and a `2.C` role resolves against the SECOND firing.
  {
    const std::vector<const FiredEvent *> parts{&first, &second};
    const std::vector<std::string> lines = dialogue.speak(parts, empty, fixture.program, names, genderOf);

    checkEqual(lines.size(), size_t(2), "a chain block speaks both its lines");

    if (lines.size() == 2u)
    {
      check(lines[0].find("A pleasure") != std::string::npos, "the else clause held");
      check(lines[0].rfind("P1 ", 0u) == 0, "the first line is the first part's A");
      check(lines[1].find("to P3") != std::string::npos, "and 2.C is the second part's C");
    }
  }

  // The first part alone falls back to its own rule's block.
  {
    const std::vector<std::string> lines = dialogue.speak(first, empty, fixture.program, names, genderOf);

    checkEqual(lines.size(), size_t(1), "a lone firing uses the single-rule block");
    check(lines[0].find("On its own") != std::string::npos, "which is the right one");
  }

  // A part-addressed CONDITION reads the part it names.
  {
    std::vector<FiredEvent> log{fired(1u, meet, {3u, 4u})};

    Chronicle chronicle;

    chronicle.build(log, fixture.program, narrator);

    const std::vector<const FiredEvent *> parts{&first, &second};
    const std::vector<std::string> lines = dialogue.speak(parts, chronicle, fixture.program, names, genderOf);

    checkEqual(lines.size(), size_t(2), "still two lines");

    if (!lines.empty())
    {
      check(lines[0].find("introduced") != std::string::npos, "the 2.C condition held on the second part's C");
    }
  }

  // A block whose rules do not match the parts in order is not used.
  {
    const std::vector<const FiredEvent *> reversed{&second, &first};
    const std::vector<std::string> lines = dialogue.speak(reversed, empty, fixture.program, names, genderOf);

    for (const std::string &line : lines)
    {
      check(line.find("A pleasure") == std::string::npos, "a chain block does not match its rules out of order");
    }
  }

  // A second block for the same rule is dead text and is reported.
  DialogueBook shadowed;
  DiagnosticSink shadowSink;

  check(!shadowed.load("dialogue they-meet:\n  A: \"one\"\ndialogue they-meet:\n  A: \"two\"\n", "t", shadowSink),
        "a second block for one rule is an error");
}

void causalCondition()
{
  DiagnosticSink sink;
  compiler::CompiledProgram program;
  compiler::UsageValues usage;

  if (!compiler::compile(causalRules, "cause.graphwrite", compiler::CompileOptions(), sink, program, usage))
  {
    return;
  }

  const auto ruleOf = [&program](const std::string &title) {
    for (uint32_t i = 0u; i < program.ruleNames.size(); ++i)
    {
      if (ruleTitleOf(program.ruleNames[i]) == title)
      {
        return i;
      }
    }

    return ~uint32_t(0);
  };

  const uint32_t meet = ruleOf("they-meet");
  const uint32_t fallOut = ruleOf("they-fall-out");

  ConsequenceGraph graph;

  graph.build(program);

  Narrator narrator;
  DiagnosticSink narrationSink;

  narrator.load("", "n", narrationSink);

  DialogueBook dialogue;
  DiagnosticSink bookSink;

  check(dialogue.load("dialogue they-fall-out:\n"
                      "  when because they-meet:\n"
                      "    A: \"We had met.\"\n"
                      "  else:\n"
                      "    A: \"A stranger.\"\n",
                      "t", bookSink),
        "a `because` condition parses");

  NameTable names(1u);

  names.setName(1u, "Aldric");

  const auto genderOf = [](uint32_t) { return Gender::Masculine; };

  // A prior they-meet involving the same man: they-meet can bring
  // they-fall-out about, so the causal clause holds.
  {
    std::vector<FiredEvent> log{fired(5u, meet, {1u, 2u})};

    Chronicle chronicle;

    chronicle.build(log, program, narrator);

    const std::vector<std::string> lines =
        dialogue.speak(fired(10u, fallOut, {1u, 3u}), chronicle, program, names, genderOf, &graph);

    check(!lines.empty() && lines[0].find("We had met") != std::string::npos, "`because` holds on a real causal link");
  }

  // The same prior event, but involving nobody in this firing.
  {
    std::vector<FiredEvent> log{fired(5u, meet, {7u, 8u})};

    Chronicle chronicle;

    chronicle.build(log, program, narrator);

    const std::vector<std::string> lines =
        dialogue.speak(fired(10u, fallOut, {1u, 3u}), chronicle, program, names, genderOf, &graph);

    check(!lines.empty() && lines[0].find("A stranger") != std::string::npos, "and not on one involving other people");
  }

  // Nothing prior at all.
  {
    Chronicle empty;

    const std::vector<std::string> lines = dialogue.speak(fired(10u, fallOut, {1u, 3u}), empty, program, names, genderOf, &graph);

    check(!lines.empty() && lines[0].find("A stranger") != std::string::npos, "nor with no history at all");
  }

  // Without a graph the causal clause simply never holds, rather than
  // asserting or holding vacuously.
  {
    std::vector<FiredEvent> log{fired(5u, meet, {1u, 2u})};

    Chronicle chronicle;

    chronicle.build(log, program, narrator);

    const std::vector<std::string> lines = dialogue.speak(fired(10u, fallOut, {1u, 3u}), chronicle, program, names, genderOf);

    check(!lines.empty() && lines[0].find("A stranger") != std::string::npos, "and never without a cause graph");
  }
}

// --- the streaming driver --------------------------------------------------------
//
// The streaming driver is what the GPU path uses: it never sees the whole
// log, so its month must come out the same as the batch driver's or a large
// world tells a different story from a small one for no reason.

void streamMatchesBatch()
{
  Fixture fixture;

  compileFixture(fixture);

  if (!fixture.ok)
  {
    return;
  }

  const uint32_t meet = fixture.ruleOf("they-meet");
  const uint32_t fallOut = fixture.ruleOf("they-fall-out");

  Narrator narrator;
  DiagnosticSink sink;

  narrator.load("they-meet: {A} met {B}.\n"
                "they-fall-out: {A} fell out with {C}.\n"
                "weight 20 they-meet\n"
                "category meeting: they-meet\n"
                "category quarrel: they-fall-out\n"
                "summary meeting: a meeting / {n} meetings\n"
                "summary quarrel: a quarrel / {n} quarrels\n",
                "n", sink);

  ChronicleSettings settings;

  settings.startWeek = 0u;
  settings.eventsPerMonth = 3u;
  settings.dialoguePerMonth = 0u;
  settings.nodeCeiling = 1000u;

  // Two months, mixed weights, more than the budget in each.
  std::vector<FiredEvent> log{
      fired(1u, fallOut, {1u, 5u}),  fired(1u, meet, {1u, 2u}),   fired(2u, fallOut, {3u, 6u}),
      fired(2u, meet, {3u, 4u}),     fired(3u, fallOut, {7u, 8u}), fired(4u, fallOut, {9u, 10u}),
      fired(9u, meet, {1u, 2u}),     fired(9u, fallOut, {1u, 5u}), fired(10u, fallOut, {2u, 6u}),
      fired(11u, meet, {11u, 12u}),  fired(12u, fallOut, {13u, 14u}),
  };

  NameTable names(1u);
  NameTable streamed(1u);

  for (uint32_t id = 1u; id <= 14u; ++id)
  {
    names.setName(id, "P" + std::to_string(id));
    streamed.setName(id, "P" + std::to_string(id));
  }

  const auto genderOf = [](uint32_t) { return Gender::Masculine; };

  const std::vector<ChronicledMonth> batch =
      Chronicler(narrator, fixture.program, settings).compose(log, names, genderOf);

  // The same log, a week at a time, as the narration worker would feed it.
  MonthStream stream(narrator, fixture.program, settings);

  stream.chronicle().setNodeCeiling(settings.nodeCeiling);

  std::vector<ChronicledMonth> streamedMonths;

  for (uint32_t week = 1u; week <= 13u; ++week)
  {
    std::vector<FiredEvent> ofWeek;

    for (const FiredEvent &event : log)
    {
      if (event.tick == week)
      {
        ofWeek.push_back(event);
      }
    }

    stream.remember(ofWeek);

    for (const NarratedTick &tick : narrator.narrate(ofWeek, fixture.program, streamed, genderOf))
    {
      stream.offer(tick, ofWeek);
    }

    for (ChronicledMonth &month : stream.takeCompleted(week, streamed, genderOf))
    {
      streamedMonths.push_back(std::move(month));
    }
  }

  for (ChronicledMonth &month : stream.takeAll(streamed, genderOf))
  {
    streamedMonths.push_back(std::move(month));
  }

  checkEqual(streamedMonths.size(), batch.size(), "the two drivers produce the same number of months");

  for (size_t i = 0u; i < streamedMonths.size() && i < batch.size(); ++i)
  {
    checkEqual(streamedMonths[i].heading, batch[i].heading, "month " + std::to_string(i) + " heading");
    checkEqual(streamedMonths[i].told, batch[i].told, "month " + std::to_string(i) + " told");
    checkEqual(streamedMonths[i].summarised, batch[i].summarised, "month " + std::to_string(i) + " summarised");
    checkEqual(Chronicler::joinParagraph(streamedMonths[i].paragraphs), Chronicler::joinParagraph(batch[i].paragraphs),
               "month " + std::to_string(i) + " prose");
  }

  // A safe tick before the story's first week has no complete month in it -
  // the worker asks exactly that on the opening weeks of a run.
  MonthStream early(narrator, fixture.program, settings);

  check(early.takeCompleted(0u, streamed, genderOf).empty(), "nothing is complete before the story starts");
}

// At scale the heaviest rule fires thousands of times a month, and a month
// ranked on weight alone was twenty identical sentences. The cap is what
// buys variety; the summary still accounts for every one it turns away.
void perRuleCap()
{
  Fixture fixture;

  compileFixture(fixture);

  if (!fixture.ok)
  {
    return;
  }

  const uint32_t meet = fixture.ruleOf("they-meet");
  const uint32_t fallOut = fixture.ruleOf("they-fall-out");

  Narrator narrator;
  DiagnosticSink sink;

  narrator.load("they-meet: {A} met {B}.\n"
                "they-fall-out: {A} fell out with {C}.\n"
                "weight 20 they-meet\n"
                "category meeting: they-meet\n"
                "category quarrel: they-fall-out\n"
                "summary meeting: a meeting / {n} meetings\n"
                "summary quarrel: a quarrel / {n} quarrels\n",
                "n", sink);

  ChronicleSettings settings;

  settings.startWeek = 0u;
  settings.eventsPerMonth = 6u;
  settings.dialoguePerMonth = 0u;
  settings.nodeCeiling = 1000u;
  settings.perRuleCap = 2u;

  // Six heavy meetings and two light quarrels, all in one month. Without a
  // cap the budget is six meetings and the quarrels are never heard of.
  std::vector<FiredEvent> log;

  for (uint32_t i = 0u; i < 6u; ++i)
  {
    log.push_back(fired(1u + i / 3u, meet, {100u + i * 2u, 101u + i * 2u}));
  }

  log.push_back(fired(3u, fallOut, {1u, 2u}));
  log.push_back(fired(3u, fallOut, {3u, 4u}));

  NameTable names(1u);

  for (uint32_t id = 1u; id <= 120u; ++id)
  {
    names.setName(id, "P" + std::to_string(id));
  }

  const auto genderOf = [](uint32_t) { return Gender::Masculine; };

  const std::vector<ChronicledMonth> months = Chronicler(narrator, fixture.program, settings).compose(log, names, genderOf);

  checkEqual(months.size(), size_t(1), "one month");

  if (months.empty())
  {
    return;
  }

  const std::string all = Chronicler::joinParagraph(months[0].paragraphs);

  size_t meetings = 0u;
  size_t at = 0u;

  while ((at = all.find(" met ", at)) != std::string::npos)
  {
    ++meetings;
    ++at;
  }

  checkEqual(meetings, size_t(2), "the cap holds: two of the heavy rule, not six");
  check(all.find("fell out with") != std::string::npos, "and the lighter rule is heard at all");
  checkEqual(months[0].told + months[0].summarised, 8u, "every event is still accounted for");
  check(all.find("four meetings") != std::string::npos, "the ones turned away are counted");

  // Lifting the cap puts the month back to weight alone.
  ChronicleSettings uncapped = settings;

  uncapped.perRuleCap = 0u;

  const std::vector<ChronicledMonth> flat = Chronicler(narrator, fixture.program, uncapped).compose(log, names, genderOf);

  size_t unbounded = 0u;

  at = 0u;

  const std::string plain = Chronicler::joinParagraph(flat[0].paragraphs);

  while ((at = plain.find(" met ", at)) != std::string::npos)
  {
    ++unbounded;
    ++at;
  }

  checkEqual(unbounded, size_t(6), "without the cap the heavy rule takes the whole budget");
}

void chronicleBounding()
{
  Fixture fixture;

  compileFixture(fixture);

  if (!fixture.ok)
  {
    return;
  }

  const uint32_t meet = fixture.ruleOf("they-meet");
  const uint32_t fallOut = fixture.ruleOf("they-fall-out");

  Narrator narrator;
  DiagnosticSink sink;

  narrator.load("", "n", sink);

  // Remembering only what can be asked about is what makes a large world
  // affordable; it must not change an answer about a remembered rule.
  Chronicle bounded;

  bounded.setRemembered({"they-meet"});

  std::vector<FiredEvent> log{fired(10u, meet, {1u, 2u}), fired(11u, fallOut, {1u, 3u}), fired(12u, meet, {1u, 4u})};

  bounded.build(log, fixture.program, narrator);

  checkEqual(bounded.size(), size_t(2), "only the remembered rule is kept");
  checkEqual(bounded.seen(), uint64_t(3), "but the chronicle counts what it was offered");
  check(bounded.took(1u, "they-meet", 20u), "a remembered rule still answers");
  check(!bounded.took(1u, "they-fall-out", 20u), "an unremembered one never did happen, as far as it knows");

  // Forgetting drops a prefix and keeps every later answer intact.
  Chronicle aging;

  aging.build(log, fixture.program, narrator);
  aging.forgetBefore(12u);

  checkEqual(aging.size(), size_t(1), "the old events are gone");
  check(!aging.took(1u, "they-meet", 11u), "and cannot be asked about");
  check(aging.took(1u, "they-meet", 20u), "while what remains still answers");
  check(aging.tookTogether(1u, 4u, "they-meet", 20u), "including as a pair");
  checkEqual(aging.cast().size(), size_t(2), "a person with nothing left leaves the cast");

  // Forgetting everything, and forgetting nothing, are both well behaved.
  aging.forgetBefore(1u);
  checkEqual(aging.size(), size_t(1), "forgetting before everything drops nothing");
  aging.forgetBefore(100u);
  checkEqual(aging.size(), size_t(0), "forgetting past the end empties it");
  check(!aging.took(1u, "they-meet", 200u), "an empty chronicle answers no");
}

void dialogueDeclaresItsNeeds()
{
  DialogueBook bounded;
  DiagnosticSink sink;

  bounded.load("dialogue a:\n  when A in the-vows-are-said within 40:\n    A: \"x\"\n"
               "dialogue b:\n  when B met C in the-heads-make-a-match within 12:\n    B: \"y\"\n",
               "t", sink);

  const std::set<std::string> titles = bounded.referencedTitles();

  checkEqual(titles.size(), size_t(2), "both titles are reported");
  check(titles.count("the-vows-are-said") != 0u, "by name");
  checkEqual(bounded.longestWindow(), 40u, "the longest window is the horizon");

  DialogueBook forever;
  DiagnosticSink foreverSink;

  forever.load("dialogue a:\n  when A in the-vows-are-said:\n    A: \"x\"\n", "t", foreverSink);

  checkEqual(forever.longestWindow(), 0u, "one unbounded condition means nothing may be forgotten");
}

// --- the month heading -------------------------------------------------------------

void monthPhraseBeforeTheStart()
{
  uint32_t key = 0u;

  // The narration worker runs its safe tick behind the clock, so it asks
  // about weeks before the story's first one. That used to index off the end
  // of the month table.
  const std::string before = StoryRunner::monthPhrase(10u, 52u, key);

  check(!before.empty(), "a week before the start has a phrase");
  check(before.find("before") != std::string::npos, "and says so");

  uint32_t first = 0u;

  StoryRunner::monthPhrase(53u, 52u, first);

  checkEqual(first, 0u, "the first week of the story is month zero");
}

// --- the real assets ---------------------------------------------------------------

void realAssetsCompose()
{
  DiagnosticSink sink;
  compiler::CompiledProgram program;
  compiler::UsageValues usage;

  bool ok = compiler::compile(readAsset("story.graphwrite"), "story.graphwrite", compiler::CompileOptions(), sink, program, usage);

  const uint32_t dayBase = StorySettings().dayBase;

  LoadedFacts world;

  world.valueResolver = [dayBase](const uint32_t year, uint32_t &id) {
    id = dayBase + year * 52u;

    return true;
  };

  ok = ok && loadFactsText(readAsset("story.graphfacts"), program.edgeTypes, "story.graphfacts", sink, world);

  Narrator narrator;

  ok = ok && narrator.load(readAsset("story.narration"), "story.narration", sink);

  DialogueBook dialogue;

  dialogue.setGrammar(&narrator.grammar());

  ok = ok && dialogue.load(readAsset("story.dialogue"), "story.dialogue", sink);

  if (!ok)
  {
    for (const Diagnostic &diagnostic : sink.all())
    {
      std::cerr << "    " << diagnostic.format("story") << "\n";
    }
  }

  check(ok, "the shipped narration and dialogue files load clean");

  if (!ok)
  {
    return;
  }

  check(narrator.grammar().size() >= 3u, "the narration file declares subs");
  check(dialogue.blockCount() >= 10u, "the dialogue file declares blocks");

  // Every rule name in the narration and dialogue files must be a rule that
  // exists. A typo in one of these is invisible: the line simply never
  // fires, the event is told by the fallback or not at all, and nothing
  // reports it. With a hundred and ninety rules across three files that is
  // the single most likely mistake in the whole set.
  std::set<std::string> titles;

  for (const std::string &name : program.ruleNames)
  {
    titles.insert(ruleTitleOf(name));
  }

  std::vector<std::string> unknown;

  for (const std::string &named : dialogue.blockTitles())
  {
    if (titles.count(named) == 0u)
    {
      unknown.push_back("dialogue " + named);
    }
  }

  for (const std::string &named : dialogue.referencedTitles())
  {
    if (titles.count(named) == 0u)
    {
      unknown.push_back("dialogue condition " + named);
    }
  }

  for (const std::string &named : narrator.namedRules())
  {
    // `scheduled:`, `released:` and `cancel:` prefix a rule title.
    std::string bare = named;

    const size_t colon = bare.find(':');

    if (colon != std::string::npos)
    {
      bare = bare.substr(colon + 1u);
    }

    if (titles.count(bare) == 0u)
    {
      unknown.push_back("narration " + named);
    }
  }

  for (const std::string &name : unknown)
  {
    std::cerr << "    names no such rule: " << name << "\n";
  }

  checkEqual(unknown.size(), size_t(0), "every rule the narration and dialogue files name exists");

  StorySettings settings;

  settings.seed = 3u;
  settings.ticks = 52u * 2u;
  settings.populationCap = 48u;

  StoryRunner story(program, usage, world, narrator, settings);

  story.run(52u);

  check(!story.events().empty(), "the run produced events");

  ChronicleSettings composed;

  composed.seed = settings.seed;
  composed.startWeek = settings.startWeek;
  composed.nodeCeiling = settings.dayBase;
  composed.eventsPerMonth = 6u;
  composed.dialoguePerMonth = 2u;

  Chronicler chronicler(narrator, program, composed);

  chronicler.setDialogue(&dialogue);

  const std::vector<ChronicledMonth> months =
      chronicler.compose(story.events(), story.names(), [&story](const uint32_t id) { return story.genderOf(id); });

  check(months.size() >= 12u, "two years compose into months");

  uint32_t spoken = 0u;
  uint32_t overBudget = 0u;
  uint32_t leftovers = 0u;

  for (const ChronicledMonth &month : months)
  {
    check(!month.heading.empty(), "every month has a heading");

    overBudget += month.told > composed.eventsPerMonth ? 1u : 0u;

    for (const std::string &paragraph : month.paragraphs)
    {
      spoken += paragraph.find('"') != std::string::npos ? 1u : 0u;
      leftovers += paragraph.find("{") != std::string::npos || paragraph.find('<') != std::string::npos ? 1u : 0u;
    }
  }

  checkEqual(overBudget, 0u, "no month tells more than its budget");
  checkEqual(leftovers, 0u, "no unresolved placeholder or sub reaches the prose");
  check(spoken > 0u, "somebody speaks over two years");
}

} // namespace

int main()
{
  grammarBasics();
  grammarWeights();
  grammarErrors();
  chronicleQueries();
  dialogueSelection();
  dialogueCountsAndChance();
  dialogueErrors();
  composerBudgetAndThreads();
  composerMonthsAndDeterminism();
  streamMatchesBatch();
  perRuleCap();
  consequenceGraph();
  chainDialogue();
  causalCondition();
  chronicleBounding();
  dialogueDeclaresItsNeeds();
  monthPhraseBeforeTheStart();
  realAssetsCompose();

  return report("GraphRewriteChronicleTests");
}
