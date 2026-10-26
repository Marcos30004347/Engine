#include "graphrewrite/compiler/Ir.hpp"

#include <map>

namespace graphrewrite::compiler {

bool buildEdgeTypeTable(const AnalysedProgram &analysed, EdgeTypeTable &out, DiagnosticSink &sink)
{
  const uint32_t errorsBefore = sink.errorCount();

  for (const LetDecl *declaration : analysed.declarations)
  {
    const Sentence &sentence = declaration->sentence();
    const uint32_t arity = static_cast<uint32_t>(sentence.variables().size());

    if (arity != 1u && arity != 2u)
    {
      sink.error(declaration->span, "pattern '" + sentence.pattern() + "' has arity " + std::to_string(arity) + ", expected 1 or 2");

      continue;
    }

    out.intern(sentence.pattern(), arity);
  }

  return sink.errorCount() == errorsBefore;
}

std::string dumpEdgeTypeTable(const EdgeTypeTable &table)
{
  std::string out;

  for (EdgeTypeId id = 0u; id < table.size(); ++id)
  {
    const EdgeType &type = table[id];

    out += std::to_string(id);
    out += ": \"";
    out += type.pattern;
    out += "\" arity=";
    out += std::to_string(type.arity);
    out += '\n';
  }

  return out;
}

} // namespace graphrewrite::compiler
