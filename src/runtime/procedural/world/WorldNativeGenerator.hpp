#pragma once
#include "WorldGeneratorDirector.hpp"
namespace procedural::world::rewrite
{
// Shared adapter for native pure/atomic compilers. Aggregate Input structs carry
// geometric domain, settings and context instead of hidden global state.
template <class Input, class Output, class Function>
GenerationTask nativeGenerator(uint64_t domain, const std::string &name, const std::string &input, const std::string &output, GenerationScope scope, Function compile, bool parallelSafe = false)
{
  GenerationKey a{input, domain}, b{output, domain};
  GenerationTask task;
  task.descriptor = {name, domain, 1, scope, {{a, typeid(Input), 1}}, {{b, typeid(Output), 1}}};
  task.parallelSafe = parallelSafe;
  task.generate = [=](const GenerationRequest &r, GenerationSnapshot &delta, std::string *error)
  {
    const auto *brief = r.context.get<Input>(a);
    if (!brief)
      return false;
    Output product;
    if (!compile(*brief, product, error))
      return false;
    delta.emplace(b, GenerationProduct{std::move(product), 1});
    return true;
  };
  return task;
}
} // namespace procedural::world::rewrite
