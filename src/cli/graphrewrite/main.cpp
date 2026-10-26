// graphrewritec: compile a .graphwrite rule file into a programme blob.
//
// The system this is ported from had a compiler and no driver, so nothing ever
// called it and it rotted into dead code. This exists so the compiler is always
// reachable from a command line.

#include "graphrewrite/compiler/Compiler.hpp"
#include "graphrewrite/compiler/ProgramSerializer.hpp"

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace {

int usage()
{
  std::cerr << "usage: graphrewritec <input.graphwrite> [-o <output.grp>] [--dump] [--max-variants N]\n";

  return 2;
}

} // namespace

int main(int argc, char **argv)
{
  std::string inputPath;
  std::string outputPath;
  bool dump = false;

  graphrewrite::compiler::CompileOptions options;

  for (int i = 1; i < argc; ++i)
  {
    const std::string argument(argv[i]);

    if (argument == "-o" && i + 1 < argc)
    {
      outputPath = argv[++i];
    }
    else if (argument == "--dump")
    {
      dump = true;
    }
    else if (argument == "--max-variants" && i + 1 < argc)
    {
      options.expansion.maxVariantsPerRule = static_cast<uint32_t>(std::stoul(argv[++i]));
    }
    else if (!argument.empty() && argument[0] == '-')
    {
      return usage();
    }
    else
    {
      inputPath = argument;
    }
  }

  if (inputPath.empty())
  {
    return usage();
  }

  std::ifstream input(inputPath);

  if (!input)
  {
    std::cerr << "graphrewritec: cannot open " << inputPath << "\n";

    return 1;
  }

  std::ostringstream buffer;

  buffer << input.rdbuf();

  graphrewrite::DiagnosticSink sink;
  graphrewrite::compiler::CompiledProgram program;
  graphrewrite::compiler::UsageValues usage;

  const bool ok = graphrewrite::compiler::compile(buffer.str(), inputPath, options, sink, program, usage);

  for (const graphrewrite::Diagnostic &diagnostic : sink.all())
  {
    std::cerr << diagnostic.format(inputPath) << "\n";
  }

  if (!ok)
  {
    return 1;
  }

  uint32_t queries = 0u;
  uint32_t shared = 0u;

  for (const graphrewrite::compiler::Instruction &instruction : program.instructions)
  {
    if (instruction.op != graphrewrite::compiler::Op::Select && instruction.op != graphrewrite::compiler::Op::Where)
    {
      continue;
    }

    ++queries;

    if (instruction.rules.size() > 1u)
    {
      ++shared;
    }
  }

  std::cout << inputPath << ": " << program.edgeTypes.size() << " edge types, " << program.ruleNames.size() << " rule variants, "
            << program.threadStarts.size() << " thread(s), " << queries << " queries (" << shared << " shared)\n";

  if (dump)
  {
    std::cout << graphrewrite::compiler::dumpProgram(program);
  }

  if (outputPath.empty())
  {
    return 0;
  }

  std::vector<uint8_t> blob;

  graphrewrite::compiler::writeProgram(program, usage, blob);

  std::ofstream output(outputPath, std::ios::binary);

  if (!output)
  {
    std::cerr << "graphrewritec: cannot write " << outputPath << "\n";

    return 1;
  }

  output.write(reinterpret_cast<const char *>(blob.data()), static_cast<std::streamsize>(blob.size()));

  std::cout << "wrote " << blob.size() << " bytes to " << outputPath << "\n";

  return 0;
}
