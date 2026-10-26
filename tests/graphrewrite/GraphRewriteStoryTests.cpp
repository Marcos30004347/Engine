// The story pipeline: a world file, names, narration, and a full run.

#include "TestHarness.hpp"
#include "graphrewrite/compiler/Compiler.hpp"
#include "graphrewrite/runtime/CpuProgramRunner.hpp"
#include "graphrewrite/runtime/FactsFile.hpp"
#include "graphrewrite/runtime/NameGenerator.hpp"
#include "graphrewrite/runtime/Narrator.hpp"
#include "graphrewrite/runtime/RuleRows.hpp"
#include "graphrewrite/runtime/StoryRunner.hpp"

#include <fstream>
#include <map>
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

struct Story
{
  compiler::CompiledProgram program;
  compiler::UsageValues usage;
  LoadedFacts world;
  Narrator narrator;
  DiagnosticSink sink;
  bool ok = false;
};

void loadStory(Story &out)
{
  out.ok = compiler::compile(readAsset("story.graphwrite"), "story.graphwrite", compiler::CompileOptions(), out.sink, out.program,
                             out.usage);
  // A bare number in the facts is a birth YEAR, resolved to that year's week
  // node the way graphrewrite-story does it.
  const uint32_t dayBase = StorySettings().dayBase;

  out.world.valueResolver = [dayBase](const uint32_t year, uint32_t &id) {
    id = dayBase + year * 52u;

    return true;
  };

  out.ok = out.ok && loadFactsText(readAsset("story.graphfacts"), out.program.edgeTypes, "story.graphfacts", out.sink, out.world);
  out.ok = out.ok && out.narrator.load(readAsset("story.narration"), "story.narration", out.sink);

  if (!out.ok)
  {
    for (const Diagnostic &diagnostic : out.sink.all())
    {
      std::cerr << "    " << diagnostic.format("story") << "\n";
    }
  }
}

void factsFile()
{
  Story story;

  loadStory(story);

  check(story.ok, "the story assets compile and load");
  check(story.world.names.size() >= 12u, "the world names its cast (" + std::to_string(story.world.names.size()) + ")");
  // A generated or family-named cast writes one TOKEN per person, given name
  // and family joined by an underscore; the narrator shows it with a space.
  checkEqual(story.world.ids.count("Aldric_Aldring"), size_t(1), "Aldric of the Aldrings is a node");
  checkEqual(story.world.ids.count("Wealdholt"), size_t(1), "and so is the village");

  // One name, one node, however many lines mention it.
  std::set<std::string> distinct(story.world.names.begin(), story.world.names.end());

  checkEqual(distinct.size(), story.world.names.size(), "every name is interned once");

  // A line that matches nothing is an error, not a silently dropped edge.
  DiagnosticSink sink;
  LoadedFacts bad;

  const bool loaded = loadFactsText("Aldric is alive\nAldric flies to Stanmere\n", story.program.edgeTypes, "bad", sink, bad);

  check(!loaded, "an unmatched line fails the load");
  checkEqual(sink.errorCount(), 1u, "with one error");
  checkEqual(bad.facts.size(), size_t(1), "and the good line still landed");
}

void names()
{
  const std::string a = generateName(7u, 100u, Gender::Masculine);
  const std::string b = generateName(7u, 100u, Gender::Masculine);
  const std::string c = generateName(8u, 100u, Gender::Masculine);

  checkEqual(a, b, "the same seed and id give the same name");
  check(a != c, "a different seed gives a different name");
  check(!a.empty() && a[0] >= 'A' && a[0] <= 'Z', "names are capitalised");

  NameTable table(3u);

  table.setName(0u, "Aldric");
  table.setGenderLookup([](const uint32_t id) { return id % 2u == 0u ? Gender::Masculine : Gender::Feminine; });

  checkEqual(table.nameOf(0u), std::string("Aldric"), "a given name is kept");

  const std::string born = table.nameOf(41u);

  check(!born.empty(), "an unnamed node gets a name on demand");
  checkEqual(table.nameOf(41u), born, "and keeps it");

  std::set<std::string> seen;

  for (uint32_t id = 1u; id < 60u; ++id)
  {
    seen.insert(table.nameOf(id));
  }

  checkEqual(seen.size(), size_t(59), "sixty people, sixty names");
}

void narration()
{
  Story story;

  loadStory(story);

  check(story.narrator.templateCount() > 10u, "the narration file has templates");
  check(story.narrator.chainCount() >= 3u, "and chains");

  NameTable names(1u);

  names.setName(0u, "Cenric");
  names.setName(1u, "Maerwyn");
  names.setName(2u, "Aldric");

  const auto gender = [](const uint32_t id) { return id == 1u ? Gender::Feminine : Gender::Masculine; };

  const auto ruleNamed = [&story](const std::string &wanted) {
    for (uint32_t rule = 0u; rule < static_cast<uint32_t>(story.program.ruleNames.size()); ++rule)
    {
      if (ruleTitleOf(story.program.ruleNames[rule]) == wanted)
      {
        return rule;
      }
    }

    return ~0u;
  };

  // Bind by variable NAME, in the rule's own vertex order, so the test does
  // not depend on the order the variables happen to appear in the rule text.
  const auto bind = [&story](const uint32_t rule, const std::map<std::string, uint32_t> &by) {
    std::vector<uint32_t> bound;

    for (const std::string &variable : story.program.ruleVertexNames[rule])
    {
      const auto found = by.find(variable);

      bound.push_back(found == by.end() ? 0u : found->second);
    }

    return bound;
  };

  const uint32_t learned = ruleNamed("the-widow-learns-who-killed-him");
  const uint32_t swore = ruleNamed("the-widow-swears-vengeance");
  const uint32_t death = ruleNamed("death-comes");

  check(learned != ~0u && swore != ~0u && death != ~0u, "the rules the test narrates exist");

  // A is the killer (Cenric), B the widow (Maerwyn), C the dead husband (Aldric).
  FiredEvent learnt{3u, learned, bind(learned, {{"A", 0u}, {"B", 1u}, {"C", 2u}})};
  FiredEvent sworn{5u, swore, bind(swore, {{"A", 0u}, {"B", 1u}, {"C", 2u}})};
  FiredEvent died{9u, death, {2u}};

  const std::string one = story.narrator.tell(learnt, story.program, names, gender);

  check(one.find("Maerwyn learned it was Cenric") != std::string::npos, "a single firing renders its template: " + one);
  check(one.find("hate him") != std::string::npos, "with the pronoun picked by gender");

  const std::vector<NarratedTick> told = story.narrator.narrate({learnt, sworn, died}, story.program, names, gender);

  checkEqual(told.size(), size_t(2), "the learning and the oath collapse into one chained event");
  check(told[0].tick == 5u && told[0].sentences.size() == 1u, "told at the tick the chain resolves");
  check(told[0].sentences[0].find("would pay for it") != std::string::npos, "with the chain's sentence: " + told[0].sentences[0]);
  check(told[1].sentences[0] == "Aldric died.", "and the death is told on its own: " + told[1].sentences[0]);

  // A chain needs the SAME person in the SAME role: an oath by a different
  // widow against the same killer is not this widow's story.
  FiredEvent otherSworn{5u, swore, bind(swore, {{"A", 0u}, {"B", 3u}, {"C", 2u}})};

  names.setName(3u, "Sunniva");

  const std::vector<NarratedTick> apart = story.narrator.narrate({learnt, otherSworn}, story.program, names, gender);

  checkEqual(apart.size(), size_t(2), "two widows' firings do not chain");

  // Silent edges never leak through the fallback, and an empty template says
  // nothing at all.
  uint32_t mourns = ~0u;

  for (uint32_t rule = 0u; rule < static_cast<uint32_t>(story.program.ruleNames.size()); ++rule)
  {
    if (ruleTitleOf(story.program.ruleNames[rule]) == "the-widow-mourns")
    {
      mourns = rule;
    }
  }

  checkEqual(story.narrator.tell(FiredEvent{1u, mourns, {2u, 1u}}, story.program, names, gender), std::string(),
             "a bookkeeping rule says nothing");
}

void ruleRowsUseTheColumnMapping()
{
  Story story;

  loadStory(story);

  // The mapping is what makes shared tables safe. It need not be identity, but
  // every matched vertex of every rule must map somewhere.
  for (uint32_t rule = 0u; rule < static_cast<uint32_t>(story.program.ruleNames.size()); ++rule)
  {
    const std::vector<uint32_t> columns = ruleVertexColumns(story.program, rule);
    const uint32_t matched = story.program.ruleRewrites[rule].matchedVertexCount;

    bool complete = columns.size() >= matched;

    for (uint32_t vertex = 0u; vertex < matched && complete; ++vertex)
    {
      complete = columns[vertex] != compiler::noRuleColumn;
    }

    check(complete, "every matched vertex of '" + story.program.ruleNames[rule] + "' has a table column");
  }

  std::cout << "    rule columns are " << (ruleColumnsAreIdentity(story.program) ? "identity" : "NOT identity")
            << " for the story programme\n";

  // Every register table must be wide enough for the merged columns its own
  // instruction writes. Sized by one rule's column count instead, the CPU
  // matcher wrote past the end of its cells - heap corruption that surfaced
  // hundreds of ticks later, in the narrator.
  EdgePageStore store;

  store.configure(story.program.edgeTypes);

  CpuProgramRunner runner(story.program, story.usage, store, CpuProgramRunner::Settings());

  runner.seed(story.world.facts);
  runner.runToCompletion();

  bool wide = true;

  for (const compiler::Instruction &instruction : story.program.instructions)
  {
    if (instruction.op != compiler::Op::Select && instruction.op != compiler::Op::Where)
    {
      continue;
    }

    const MatchTable *table = runner.tableForRegister(instruction.reg);

    if (table != nullptr && table->columns() <= std::max(instruction.fromColumn, instruction.writeColumn))
    {
      wide = false;
    }
  }

  check(wide, "every register table is at least as wide as the columns its instruction touches");
}

void aStoryRuns()
{
  Story story;

  loadStory(story);

  StorySettings settings;

  settings.seed = 11u;
  settings.ticks = 104u;

  StoryRunner runner(story.program, story.usage, story.world, story.narrator, settings);

  const uint32_t before = runner.population();

  runner.run();

  check(!runner.events().empty(), "something happened (" + std::to_string(runner.events().size()) + " events)");
  check(runner.summaries().size() >= 2u, "over several ticks (" + std::to_string(runner.summaries().size()) + ")");

  // Leofric and Cuthswith start out mutually smitten; the engagement rule is
  // the only thing that consumes a mutual liking, so given two years at
  // maybe 4 it has fired unless the dice were extraordinary. The wedding is
  // then dated weeks out and said on its week by a guarded rule.
  bool engaged = false;
  bool wed = false;
  bool scheduledDeath = false;

  for (const FiredEvent &event : runner.events())
  {
    const std::string title = ruleTitleOf(story.program.ruleNames[event.rule]);

    engaged = engaged || title == "they-get-engaged";
    wed = wed || title == "the-vows-are-said";
    scheduledDeath = scheduledDeath || (event.kind == RewriteEventKind::Scheduled &&
                                        (title == "poison" || title == "a-knife-in-the-dark" || title == "the-river"));
  }

  check(engaged, "the couple who liked each other got engaged");
  check(wed, "and at least one wedding was said on its week");

  const std::string text = runner.text();

  check(text.find("-- Week ") != std::string::npos, "the text has week headers");
  check(text.find("-- Week 1, early January --") != std::string::npos, "and the first is week one, early January");
  check(text.find("were married at") != std::string::npos, "and tells of a wedding at a church");
  check(text.find("-- Tick ") == std::string::npos, "and never counts ticks");

  std::cout << "    a death was scheduled by a plot: " << (scheduledDeath ? "yes" : "no") << "\n";

  // Week nodes live above dayBase; people never do. An edge to a week node
  // used to push the allocator past it, and every child born after was named
  // like a week - and the facts now seed birth weeks, so the ceiling has to be
  // in place before the seed.
  check(runner.graph().vertexCount() < settings.dayBase, "no person was allocated in the week-node range");

  bool personNamedWeek = false;

  for (const auto &entry : runner.names().all())
  {
    const bool calendar = entry.second.rfind("early ", 0u) == 0u || entry.second.rfind("mid-", 0u) == 0u ||
                          entry.second.rfind("late ", 0u) == 0u;

    personNamedWeek = personNamedWeek || (entry.first < settings.dayBase && calendar);
  }

  check(!personNamedWeek, "and no person is named like a week");
  check(text.find("is alive") == std::string::npos, "and never mentions bookkeeping");
  check(text.find('{') == std::string::npos, "and leaves no placeholder unresolved");

  // The same seed tells the same story; a different seed tells a different one.
  StoryRunner again(story.program, story.usage, story.world, story.narrator, settings);

  again.run();

  checkEqual(again.text(), text, "a seed is reproducible");

  settings.seed = 12u;

  StoryRunner other(story.program, story.usage, story.world, story.narrator, settings);

  other.run();

  check(other.text() != text, "and another seed differs");

  std::cout << "    seed 11: population " << before << " -> " << runner.population() << ", " << runner.events().size() << " events, "
            << runner.summaries().size() << " ticks\n";
}

void populationPressure()
{
  Story story;

  loadStory(story);

  // With the cap at one, everyone alive is excess, so the death rule is driven
  // to certainty: the village should be dead within a handful of ticks.
  StorySettings settings;

  settings.seed = 5u;
  settings.ticks = 30u;
  settings.populationCap = 1u;

  StoryRunner runner(story.program, story.usage, story.world, story.narrator, settings);

  runner.run();

  check(runner.population() <= 1u, "a cap of one empties the village (" + std::to_string(runner.population()) + " left)");
}

} // namespace

int main()
{
  factsFile();
  names();
  narration();
  ruleRowsUseTheColumnMapping();
  aStoryRuns();
  populationPressure();

  return report("GraphRewriteStoryTests");
}
