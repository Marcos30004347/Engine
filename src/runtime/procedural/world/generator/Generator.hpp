#pragma once

#include "Ledger.hpp"
#include "PatchRegion.hpp"
#include "procedural/world/WorldGeneratorDirector.hpp"

#include <memory>
#include <string>

namespace procedural::world::generator {

using rewrite::GenerationDescriptor;
using rewrite::GenerationKey;
using rewrite::GenerationProduct;
using rewrite::GenerationRequest;
using rewrite::GenerationScope;
using rewrite::GenerationSnapshot;
using rewrite::GenerationTask;

// Common generator interface. A generator declares its typed ports through
// the descriptor, reads its inputs from the request context, and writes its
// products into the delta. The typed director schedules it like any task.
class GeneratorBase
{
public:
  virtual ~GeneratorBase() = default;

  virtual GenerationDescriptor descriptor() const = 0;

  virtual bool parallelSafe() const
  {
    return false;
  }

  virtual bool generate(const GenerationRequest &request, GenerationSnapshot &delta, Ledger &ledger, std::string *error) = 0;
};

// One typed input product, one typed output product. Parents produce a
// tiling of PatchRegion; children receive their region through the input.
template <class Input, class Output> class Generator : public GeneratorBase
{
public:
  Generator(std::string name, uint64_t domain, std::string input, std::string output, GenerationScope scope)
      : name_(std::move(name)), domain_(domain), input_{std::move(input), domain}, output_{std::move(output), domain}, scope_(scope)
  {
  }

  GenerationDescriptor descriptor() const final
  {
    return {name_, domain_, 1, scope_, {{input_, typeid(Input), 1}}, {{output_, typeid(Output), 1}}};
  }

  bool generate(const GenerationRequest &request, GenerationSnapshot &delta, Ledger &ledger, std::string *error) final
  {
    const Input *input = request.context.template get<Input>(input_);

    if (input == nullptr)
    {
      if (error != nullptr)
      {
        *error = name_ + ": missing input " + input_.product;
      }

      return false;
    }

    Output product;

    if (!run(*input, product, ledger, error))
    {
      return false;
    }

    delta.emplace(output_, GenerationProduct{std::move(product), 1});

    return true;
  }

  const std::string &name() const
  {
    return name_;
  }

  uint64_t domain() const
  {
    return domain_;
  }

protected:
  virtual bool run(const Input &input, Output &output, Ledger &ledger, std::string *error) = 0;

private:
  std::string name_;
  uint64_t domain_;
  GenerationKey input_, output_;
  GenerationScope scope_;
};

// Adapts a generator object to the director's task record. The generator
// must outlive the task; ledger entries are appended to the shared ledger.
inline GenerationTask toTask(std::shared_ptr<GeneratorBase> generator, std::shared_ptr<Ledger> ledger)
{
  GenerationTask task;

  task.descriptor = generator->descriptor();
  task.parallelSafe = generator->parallelSafe();

  task.generate = [generator, ledger](const GenerationRequest &request, GenerationSnapshot &delta, std::string *error) {
    Ledger local;

    const bool ok = generator->generate(request, delta, local, error);

    if (ledger)
    {
      for (const auto &entry : local.entries())
      {
        ledger->add(entry.kind, entry.region, entry.message);
      }
    }

    return ok;
  };

  return task;
}

} // namespace procedural::world::generator
