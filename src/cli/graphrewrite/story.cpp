// graphrewrite-story: run a rule programme over a world and write the story.
//
//   graphrewrite-story story.graphwrite story.graphfacts --seeds 3 --ticks 80
//
// One text file per seed. The same seed always gives the same story.

#include "company/Company.hpp"
#include "graphrewrite/compiler/Compiler.hpp"
#include "graphrewrite/runtime/Chronicler.hpp"
#include "graphrewrite/runtime/DialogueBook.hpp"
#include "graphrewrite/runtime/FactsFile.hpp"
#include "graphrewrite/runtime/RuleRows.hpp"
#include "graphrewrite/runtime/GpuStoryRunner.hpp"
#include "graphrewrite/runtime/StoryRunner.hpp"
#include "rendering/gpu/backend/vulkan/VulkanRHI.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

namespace {

int usage()
{
  std::cerr << "company game: graphrewrite-story --company [--seed N] [--path video] [--help]\n";
  std::cerr << "usage: graphrewrite-story <rules.graphwrite> <world.graphfacts> [--narration <file>] [--seed N] [--seeds K]\n"
               "                          [--weeks N | --years N | --ticks N] [--cap N] [--rows N] [--out <dir>] [--events N]\n"
               "                          [--no-text] [--tick-times <csv>] [--gpu] [--gpu-timers] [--table-words N]\n"
               "                          [--important] [--by-month]\n"
               "                          [--chronicle] [--dialogue <file>] [--per-month N] [--dialogues N]\n"
               "--important tells only the rules the narration file lists as important; --by-month groups the text by month\n"
               "--chronicle composes the story month by month in paragraphs, ranked by weight, with a closing count of\n"
               "  what was too small to tell; works on the CPU and the GPU path alike;\n"
               "  --dialogue loads spoken lines (default <rules>.dialogue if it exists)\n"
               "--per-month is how many events a month tells at length (0: all); --dialogues how many may be spoken;\n"
               "  --per-rule caps how many of ONE rule a month may tell (0: no cap), which is what stops a large\n"
               "  world spending a whole month on the heaviest rule firing a thousand times\n"
               "one tick is one week; the default run is one year (52 weeks)\n"
               "--no-text writes only the footer (no narration, no event log); --tick-times writes one row per tick\n"
               "--gpu simulates on the GPU (the CPU evaluates clauses and narrates on a second thread)\n";

  return 2;
}

bool readAll(const std::string &path, std::string &out)
{
  std::ifstream file(path);

  if (!file)
  {
    return false;
  }

  std::ostringstream buffer;

  buffer << file.rdbuf();
  out = buffer.str();

  return true;
}

} // namespace

int main(int argc, char **argv)
{
  if (argc > 1 && std::string(argv[1]) == "--company")
    return company::runCli(argc - 1, argv + 1);
  std::string rulesPath;
  std::string worldPath;
  std::string narrationPath;
  std::string dialoguePath;
  bool chronicle = false;
  bool dialogueGiven = false;
  graphrewrite::runtime::ChronicleSettings chronicleSettings;
  std::string outDir = ".";

  graphrewrite::runtime::StorySettings settings;

  // Above this many living people the death rule bites; below it, natural
  // death is by age alone. Forty lets a village of twenty-odd grow for a
  // generation before the cap does anything.
  settings.populationCap = 40u;

  bool capGiven = false;
  bool noText = false;
  bool useGpu = false;
  bool gpuTimers = false;
  bool onlyImportant = false;
  uint32_t tableWords = 0u; // 0: sized from the programme and --rows
  std::string tickTimesPath;
  uint32_t seeds = 1u;
  uint32_t dumpEvents = 0u;

  for (int i = 1; i < argc; ++i)
  {
    const std::string argument(argv[i]);
    const bool hasValue = i + 1 < argc;

    if (argument == "--narration" && hasValue)
    {
      narrationPath = argv[++i];
    }
    else if (argument == "--seed" && hasValue)
    {
      settings.seed = static_cast<uint32_t>(std::stoul(argv[++i]));
    }
    else if (argument == "--seeds" && hasValue)
    {
      seeds = static_cast<uint32_t>(std::stoul(argv[++i]));
    }
    else if ((argument == "--ticks" || argument == "--weeks") && hasValue)
    {
      settings.ticks = static_cast<uint32_t>(std::stoul(argv[++i]));
    }
    else if (argument == "--no-text")
    {
      settings.recordEvents = false;
      noText = true;
    }
    else if (argument == "--tick-times" && hasValue)
    {
      tickTimesPath = argv[++i];
    }
    else if (argument == "--gpu")
    {
      useGpu = true;
    }
    else if (argument == "--important")
    {
      onlyImportant = true;
    }
    else if (argument == "--chronicle")
    {
      chronicle = true;
    }
    else if (argument == "--dialogue" && hasValue)
    {
      dialoguePath = argv[++i];
      dialogueGiven = true;
    }
    else if (argument == "--per-month" && hasValue)
    {
      chronicleSettings.eventsPerMonth = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
    }
    else if (argument == "--per-rule" && hasValue)
    {
      chronicleSettings.perRuleCap = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
    }
    else if (argument == "--dialogues" && hasValue)
    {
      chronicleSettings.dialoguePerMonth = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
    }
    else if (argument == "--by-month")
    {
      settings.byMonth = true;
    }
    else if (argument == "--gpu-timers")
    {
      gpuTimers = true;
    }
    else if (argument == "--table-words" && hasValue)
    {
      tableWords = static_cast<uint32_t>(std::stoul(argv[++i]));
    }
    else if (argument == "--rows" && hasValue)
    {
      settings.rowCapacityPerTable = static_cast<uint32_t>(std::stoul(argv[++i]));
    }
    else if (argument == "--years" && hasValue)
    {
      settings.ticks = 52u * static_cast<uint32_t>(std::stoul(argv[++i]));
    }
    else if (argument == "--cap" && hasValue)
    {
      settings.populationCap = static_cast<uint32_t>(std::stoul(argv[++i]));
      capGiven = true;
    }
    else if (argument == "--out" && hasValue)
    {
      outDir = argv[++i];
    }
    else if (argument == "--events" && hasValue)
    {
      dumpEvents = static_cast<uint32_t>(std::stoul(argv[++i]));
    }
    else if (!argument.empty() && argument[0] == '-')
    {
      return usage();
    }
    else if (rulesPath.empty())
    {
      rulesPath = argument;
    }
    else if (worldPath.empty())
    {
      worldPath = argument;
    }
    else
    {
      return usage();
    }
  }

  if (rulesPath.empty() || worldPath.empty())
  {
    return usage();
  }

  if (narrationPath.empty())
  {
    // story.graphwrite -> story.narration
    const size_t dot = rulesPath.rfind('.');

    narrationPath = (dot == std::string::npos ? rulesPath : rulesPath.substr(0u, dot)) + ".narration";
  }

  if (dialoguePath.empty())
  {
    // story.graphwrite -> story.dialogue, used if it is there. Not having one
    // is not an error; it means nobody speaks.
    const size_t dot = rulesPath.rfind('.');

    dialoguePath = (dot == std::string::npos ? rulesPath : rulesPath.substr(0u, dot)) + ".dialogue";
  }

  std::string source;

  if (!readAll(rulesPath, source))
  {
    std::cerr << "graphrewrite-story: cannot open " << rulesPath << "\n";

    return 1;
  }

  graphrewrite::DiagnosticSink sink;
  graphrewrite::compiler::CompiledProgram program;
  graphrewrite::compiler::UsageValues usageValues;

  const bool compiled = graphrewrite::compiler::compile(source, rulesPath, graphrewrite::compiler::CompileOptions(), sink, program,
                                                        usageValues);

  graphrewrite::runtime::LoadedFacts world;

  // A bare number in the facts is a YEAR: `Aldric born in 965` is an edge to
  // the week node of that year's first week, which the age guards read.
  world.valueResolver = [&settings](const uint32_t year, uint32_t &id) {
    id = settings.dayBase + year * 52u;

    return true;
  };

  const bool loaded = compiled && graphrewrite::runtime::loadFactsFile(worldPath, program.edgeTypes, sink, world);

  graphrewrite::runtime::Narrator narrator;

  const bool narrated = loaded && narrator.loadFile(narrationPath, sink);

  narrator.setOnlyImportant(onlyImportant);

  // The dialogue book shares the narrator's grammar, so a `sub` written in
  // either file is one vocabulary for the whole story.
  graphrewrite::runtime::DialogueBook dialogue;

  dialogue.setGrammar(&narrator.grammar());

  bool spoke = false;

  if (std::filesystem::exists(dialoguePath))
  {
    spoke = dialogue.loadFile(dialoguePath, sink);
  }
  else if (dialogueGiven)
  {
    std::cerr << "graphrewrite-story: cannot open " << dialoguePath << "\n";

    return 1;
  }

  for (const graphrewrite::Diagnostic &diagnostic : sink.all())
  {
    std::cerr << diagnostic.format(rulesPath) << "\n";
  }

  if (!compiled || !loaded || !narrated)
  {
    return 1;
  }

  std::cout << rulesPath << ": " << program.ruleNames.size() << " rule variants; " << worldPath << ": " << world.names.size()
            << " people, " << world.facts.size() << " edges; " << narrationPath << ": " << narrator.templateCount() << " templates, "
            << narrator.chainCount() << " chains, " << narrator.grammar().size() << " subs\n";

  if (spoke)
  {
    std::cout << dialoguePath << ": " << dialogue.blockCount() << " dialogue blocks\n";
  }

  if (!graphrewrite::runtime::ruleColumnsAreIdentity(program))
  {
    std::cout << "note: rule columns are not identity in this programme; rows are gathered through the column mapping\n";
  }

  // Without --cap, the cap is twice the starting population (at least forty):
  // a cast of two hundred against a cap of forty is dead in a week.
  if (!capGiven)
  {
    graphrewrite::compiler::EdgeTypeId alive = graphrewrite::compiler::invalidEdgeType;

    program.edgeTypes.find(settings.aliveType, alive);

    uint32_t living = 0u;

    for (const graphrewrite::runtime::Fact &fact : world.facts)
    {
      living += std::get<1>(fact) == alive ? 1u : 0u;
    }

    settings.populationCap = std::max(40u, 2u * living);
  }

  const uint32_t firstSeed = settings.seed;

  ::rendering::backend::vulkan::VulkanRHI *rhi = nullptr;

  for (uint32_t n = 0u; n < seeds; ++n)
  {
    settings.seed = firstSeed + n;

    if (useGpu)
    {
      if (rhi == nullptr)
      {
        ::rendering::DeviceRequiredLimits limits = {
            .minimumMemory = 0,
            .minimumComputeSharedMemory = 0,
            .minimumComputeWorkGroupInvocations = 0,
        };

        rhi = new ::rendering::backend::vulkan::VulkanRHI(::rendering::backend::vulkan::Vulkan_1_2, limits,
                                                          ::rendering::DeviceFeatures::DeviceFeatures_Compute, {});

        std::vector<VkSurfaceKHR> surfaces;

        rhi->init(surfaces);
      }

      graphrewrite::runtime::GpuRunnerSettings gpu;

      gpu.rowCapacityPerTable = settings.rowCapacityPerTable;
      gpu.maxRows = settings.rowCapacityPerTable;
      gpu.tableWords = tableWords;
      // Per RULE application; every one is copied whole on the frame that
      // produces it, so these are sized for a rule, not a week - but a rule
      // that fires on a quarter of a million rows in one week is ordinary at
      // this scale, and the overflow it caused reported itself as a table
      // overflow with a row count that was already large enough.
      gpu.deltaCapacity = 1u << 21u;
      // The dirty list is one entry per PAGE a rule touched, so it has to be
      // larger than the whole store's page count: an always-rule that removes
      // an edge from every corner of the graph dirties every page, and at
      // 200k that is far past sixteen thousand.
      gpu.dirtyCapacity = 1u << 16u;
      gpu.candidateCapacity = 1u << 21u;
      gpu.firedCapacity = 1u << 21u;
      gpu.maxPagesPerPartition = 1u << 16u;
      gpu.diceSeed = settings.seed;
      gpu.enableTimers = gpuTimers;
      gpu.recordFrames = true;

      graphrewrite::runtime::GpuStoryRunner story(rhi, program, usageValues, world, narrator, settings, gpu);

      if (chronicle)
      {
        // The GPU runner keeps no log - it narrates as the weeks go by - so
        // the composer runs inside its narration thread, a month at a time.
        graphrewrite::runtime::ChronicleSettings composed = chronicleSettings;

        composed.seed = settings.seed;
        composed.startWeek = settings.startWeek;

        narrator.setGrammarSeed(settings.seed);
        dialogue.setSeed(settings.seed);

        story.setChronicle(composed, spoke ? &dialogue : nullptr);
      }

      story.run(52u);

      std::error_code directoryError;

      std::filesystem::create_directories(outDir, directoryError);

      const std::string path = outDir + "/story-seed-" + std::to_string(settings.seed) + "-gpu.txt";
      std::ofstream out(path);

      if (!out)
      {
        std::cerr << "graphrewrite-story: cannot write " << path << "\n";

        return 1;
      }

      out << (noText ? "Seed " + std::to_string(settings.seed) + ". Narration off (--no-text).\n\n" : story.text() + "\n")
          << story.footer();

      if (!tickTimesPath.empty())
      {
        const std::string csvPath = seeds == 1u ? tickTimesPath : tickTimesPath + "." + std::to_string(settings.seed);
        std::ofstream csv(csvPath);

        // One row per GPU FRAME - a tick of the programme runner: a match
        // instruction, or one phase of one rule's rewrite.
        csv << "frame,week,kind,instruction,rule,rows,wall_ms,gpu_ms,match_gpu_ms,rewrite_gpu_ms\n";

        for (const graphrewrite::runtime::GpuStoryRunner::WeekFrame &entry : story.frames())
        {
          const graphrewrite::runtime::FrameTiming &f = entry.frame;

          csv << f.frame << ',' << entry.week << ',' << (f.kind == 0u ? "match" : f.kind == 1u ? "candidates" : "apply") << ','
              << f.instruction << ',' << (f.rule == ~0u ? std::string("-") : std::to_string(f.rule)) << ',' << f.rows << ','
              << std::fixed << std::setprecision(3) << f.wallMs << ',' << f.gpuMs << ',' << f.matchGpuMs << ',' << f.rewriteGpuMs
              << "\n";
        }

        std::ofstream weeks(csvPath + ".weeks.csv");

        weeks << "week,ms,match_ms,rewrite_ms,clause_ms,submit_wait_ms,program_ticks,submits,fired,scheduled,candidates,"
                 "claims_rejected,population,overflowed\n";

        for (const graphrewrite::runtime::GpuTickSummary &t : story.summaries())
        {
          weeks << (t.tick - settings.startWeek) << ',' << std::fixed << std::setprecision(3) << t.seconds * 1000.0 << ','
                << t.matchSeconds * 1000.0 << ',' << t.rewriteSeconds * 1000.0 << ',' << t.clauseSeconds * 1000.0 << ','
                << t.submitWaitMs << ',' << t.programTicks << ',' << t.submits << ',' << t.fired << ',' << t.scheduled << ','
                << t.candidates << ',' << t.claimsRejected << ',' << t.population << ',' << (t.overflowed ? 1 : 0) << "\n";
        }
      }

      std::cout << "seed " << settings.seed << " (GPU): " << story.summaries().size() << " ticks, " << story.eventCount()
                << " events, population " << (story.summaries().empty() ? 0u : story.summaries().front().population) << " -> "
                << story.population() << ", " << std::fixed << std::setprecision(2) << story.elapsedSeconds() << " s -> " << path
                << "\n";

      if (story.overflowedWeeks() != 0u)
      {
        std::cerr << "graphrewrite-story: seed " << settings.seed << ": overflow in " << story.overflowedWeeks()
                  << " weeks (kinds " << story.overflowKinds() << "); largest table needed " << story.peakRowsNeeded()
                  << " rows - see the footer\n";
      }

      continue;
    }

    graphrewrite::runtime::StoryRunner story(program, usageValues, world, narrator, settings);

    story.run(52u);

    std::error_code directoryError;

    std::filesystem::create_directories(outDir, directoryError);

    const std::string path = outDir + "/story-seed-" + std::to_string(settings.seed) + ".txt";

    std::ofstream out(path);

    if (!out)
    {
      std::cerr << "graphrewrite-story: cannot write " << path << "\n";

      return 1;
    }

    if (noText)
    {
      out << "Seed " << settings.seed << ". Narration off (--no-text).\n\n" << story.footer();
    }
    else if (chronicle)
    {
      graphrewrite::runtime::ChronicleSettings composed = chronicleSettings;

      composed.seed = settings.seed;
      composed.startWeek = settings.startWeek;
      composed.nodeCeiling = settings.dayBase;

      narrator.setGrammarSeed(settings.seed);
      dialogue.setSeed(settings.seed);

      graphrewrite::runtime::Chronicler chronicler(narrator, program, composed);

      if (spoke)
      {
        chronicler.setDialogue(&dialogue);
      }

      out << "Seed " << settings.seed << ". " << story.events().size() << " events.\n\n"
          << chronicler.text(story.events(), story.names(), [&story](const uint32_t id) { return story.genderOf(id); }) << story.footer();
    }
    else
    {
      out << story.text() << "\n" << story.footer();
    }

    if (!tickTimesPath.empty())
    {
      const std::string csvPath = seeds == 1u ? tickTimesPath : tickTimesPath + "." + std::to_string(settings.seed);
      std::ofstream csv(csvPath);

      csv << "tick,ms,match_ms,gather_ms,rewrite_ms,program_ticks,rows,fired,population,overflowed\n";

      for (const graphrewrite::runtime::TickSummary &t : story.summaries())
      {
        csv << (t.tick - settings.startWeek) << ',' << std::fixed << std::setprecision(3) << t.seconds * 1000.0 << ','
            << t.matchSeconds * 1000.0 << ',' << t.gatherSeconds * 1000.0 << ',' << t.applySeconds * 1000.0 << ','
            << t.programTicks << ',' << t.matchedRows << ',' << t.fired << ',' << t.population << ',' << (t.overflowed ? 1 : 0)
            << "\n";
      }
    }

    // Raw events, before narration: which rule, on which node ids, which tick.
    for (uint32_t e = 0u; e < dumpEvents && e < story.events().size(); ++e)
    {
      const graphrewrite::runtime::FiredEvent &event = story.events()[e];

      std::cout << "  t" << event.tick << " " << program.ruleNames[event.rule] << " [";

      for (const uint32_t id : event.bound)
      {
        std::cout << ' ' << id;
      }

      std::cout << " ]\n";
    }

    const auto &ticks = story.summaries();

    std::cout << "seed " << settings.seed << ": " << ticks.size() << " ticks, " << story.events().size() << " events, population "
              << (ticks.empty() ? 0u : ticks.front().population) << " -> " << story.population() << ", " << std::fixed
              << std::setprecision(2) << story.elapsedSeconds() << " s -> " << path << "\n";

    if (story.overflowedWeeks() != 0u)
    {
      std::cerr << "graphrewrite-story: seed " << settings.seed << ": the matcher overflowed a table in " << story.overflowedWeeks()
                << " weeks and lost matches; rerun with a larger --rows (now " << settings.rowCapacityPerTable << ")\n";
    }
  }

  delete rhi;

  return 0;
}
