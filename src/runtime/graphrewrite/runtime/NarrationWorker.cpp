#include "graphrewrite/runtime/NarrationWorker.hpp"

#include "graphrewrite/runtime/StoryRunner.hpp"

#include <chrono>
#include <sstream>

namespace graphrewrite::runtime {

NarrationWorker::NarrationWorker(const Narrator &teller, const compiler::CompiledProgram &compiled, const uint32_t nameSeed,
                                 const std::vector<std::string> &worldNames, const uint32_t dayBase, const uint32_t start,
                                 const bool groupByMonth)
    : narrator(teller), program(compiled), names(nameSeed), startWeek(start), window(teller.longestChainWindow()), byMonth(groupByMonth)
{
  for (uint32_t id = 0u; id < static_cast<uint32_t>(worldNames.size()); ++id)
  {
    names.setName(id, worldNames[id]);
  }

  names.setGenderLookup([this](const uint32_t id) { return genderLookup(id); });
  names.setFamilyLookup([this](const uint32_t id) { return familyLookup(id); });

  names.setRangeNamer(dayBase, [dayBase, start](const uint32_t id) { return StoryRunner::weekPhrase(id - dayBase, start); });

  thread = std::thread([this] { loop(); });
}

NarrationWorker::~NarrationWorker()
{
  finish();
}

void NarrationWorker::setChronicle(ChronicleSettings settings, const DialogueBook *book)
{
  settings.startWeek = startWeek;

  stream = std::make_unique<MonthStream>(narrator, program, settings);

  if (book != nullptr)
  {
    stream->setDialogue(book);
  }
}

void NarrationWorker::push(const uint32_t tick, std::vector<FiredEvent> events, std::vector<NodeFacts> facts)
{
  {
    std::lock_guard<std::mutex> lock(mutex);

    inbox.push_back(Batch{tick, std::move(events), std::move(facts)});
  }

  wake.notify_one();
}

void NarrationWorker::finish()
{
  {
    std::lock_guard<std::mutex> lock(mutex);

    if (done)
    {
      return;
    }

    done = true;
  }

  wake.notify_one();

  if (thread.joinable())
  {
    thread.join();
  }
}

std::string NarrationWorker::text()
{
  std::lock_guard<std::mutex> lock(mutex);

  return out;
}

void NarrationWorker::narrateUpTo(const uint32_t uptoTick)
{
  if (held.empty())
  {
    return;
  }

  const auto started = std::chrono::steady_clock::now();

  std::vector<bool> consumed;

  const auto genderOf = [this](const uint32_t id) { return genderLookup(id); };

  const std::vector<NarratedTick> told = narrator.narrateUpTo(held, uptoTick, program, names, genderOf, consumed);

  std::ostringstream text;

  for (const NarratedTick &tick : told)
  {
    if (stream == nullptr)
    {
      StoryRunner::appendNarratedTick(text, tick, startWeek, byMonth, &lastMonth);
    }
    else
    {
      stream->offer(tick, held);
    }

    ++narratedWeeks;
  }

  if (stream != nullptr)
  {
    // A month is composed once the narrator has moved past it, which the
    // hold-back already guarantees for everything before `uptoTick`.
    const std::vector<ChronicledMonth> months =
        uptoTick == ~0u ? stream->takeAll(names, genderOf) : stream->takeCompleted(uptoTick, names, genderOf);

    for (const ChronicledMonth &month : months)
    {
      Chronicler::appendMonth(text, month);
    }
  }

  std::vector<FiredEvent> rest;

  for (size_t i = 0u; i < held.size(); ++i)
  {
    if (i >= consumed.size() || !consumed[i])
    {
      rest.push_back(std::move(held[i]));
    }
  }

  held.swap(rest);

  seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();

  std::lock_guard<std::mutex> lock(mutex);

  out += text.str();
}

void NarrationWorker::loop()
{
  uint32_t latest = 0u;

  for (;;)
  {
    std::vector<Batch> batches;
    bool finishing = false;

    {
      std::unique_lock<std::mutex> lock(mutex);

      wake.wait(lock, [this] { return done || !inbox.empty(); });

      batches.swap(inbox);
      finishing = done;
    }

    for (Batch &batch : batches)
    {
      for (const NodeFacts &entry : batch.facts)
      {
        genders[entry.id] = entry.gender;

        if (entry.family != ~uint32_t(0))
        {
          families[entry.id] = entry.family;
        }
      }

      // Into the chronicle before anything of that week is composed: a
      // condition asks strictly before its own tick, so a week cannot be
      // evidence for itself either way.
      if (stream != nullptr)
      {
        stream->remember(batch.events);
      }

      held.insert(held.end(), std::make_move_iterator(batch.events.begin()), std::make_move_iterator(batch.events.end()));
      latest = std::max(latest, batch.tick);
    }

    // Everything older than the longest chain window is final.
    if (!batches.empty() && latest > window + 1u)
    {
      narrateUpTo(latest - window - 1u);
    }

    if (finishing)
    {
      narrateUpTo(~0u);

      return;
    }
  }
}

} // namespace graphrewrite::runtime
