#include "graphrewrite/runtime/StoryClauses.hpp"

#include <vector>

namespace graphrewrite::runtime {

void registerStoryClauses(ExpressionHost &host, const uint32_t dayBase, std::set<std::pair<uint32_t, uint32_t>> &booked,
                          std::mt19937 &rolls)
{
  // One tick is one WEEK, and the clauses count in weeks.
  host.setUnit("Weeks", 1u);
  host.setUnit("Week", 1u);
  host.setUnit("Months", 4u);
  host.setUnit("Month", 4u);
  host.setUnit("Years", 52u);
  host.setUnit("Year", 52u);

  host.registerHandler("nowInDays", [](const std::vector<uint32_t> &, const std::vector<std::string> &, const CallContext &context,
                                       void *, uint32_t &out) {
    out = context.now;

    return true;
  });

  // availableAt(place, week): true, and the week is booked, if nobody has it.
  // The booking is the guard's side effect, which is why the applier only asks
  // for rows that are otherwise certain to fire.
  host.registerHandler(
      "availableAt",
      [](const std::vector<uint32_t> &args, const std::vector<std::string> &, const CallContext &, void *user, uint32_t &out) {
        if (args.size() < 2u)
        {
          return false;
        }

        auto &table = *static_cast<std::set<std::pair<uint32_t, uint32_t>> *>(user);

        if (!table.emplace(args[0], args[1]).second)
        {
          return false;
        }

        out = 1u;

        return true;
      },
      &booked);

  // Dates as nodes. week(x) -> node; weekOf(D) -> x; weekBefore(D) -> x - 1,
  // refusing if that is not still ahead; inTime(D) / passed(D) are the guards
  // a plot uses to know whether the wedding can still be stopped. The `day*`
  // spellings are the same handlers under their old names.
  const ExpressionHost::Handler weekNode = [dayBase](const std::vector<uint32_t> &args, const std::vector<std::string> &, const CallContext &, void *,
                                     uint32_t &out) {
    if (args.empty())
    {
      return false;
    }

    out = dayBase + args[0];

    return true;
  };

  const ExpressionHost::Handler weekOf = [dayBase](const std::vector<uint32_t> &args, const std::vector<std::string> &, const CallContext &, void *,
                                   uint32_t &out) {
    if (args.empty() || args[0] < dayBase)
    {
      return false;
    }

    out = args[0] - dayBase;

    return true;
  };

  const ExpressionHost::Handler weekBefore = [dayBase](const std::vector<uint32_t> &args, const std::vector<std::string> &,
                                       const CallContext &context, void *, uint32_t &out) {
    if (args.empty() || args[0] < dayBase + 1u || args[0] - dayBase - 1u <= context.now)
    {
      return false;
    }

    out = args[0] - dayBase - 1u;

    return true;
  };

  host.registerHandler("week", weekNode);
  host.registerHandler("day", weekNode);
  host.registerHandler("weekOf", weekOf);
  host.registerHandler("dayOf", weekOf);
  host.registerHandler("weekBefore", weekBefore);
  host.registerHandler("dayBefore", weekBefore);

  host.registerHandler("inTime", [dayBase](const std::vector<uint32_t> &args, const std::vector<std::string> &,
                                           const CallContext &context, void *, uint32_t &out) {
    // There is still a week before the date on which to act.
    out = (!args.empty() && args[0] >= dayBase + 1u && args[0] - dayBase - 1u > context.now) ? 1u : 0u;

    return out != 0u;
  });

  // weeksLeft(D, n): at least n whole weeks remain before D. A slow poison
  // needs two; a knife needs one.
  host.registerHandler("weeksLeft", [dayBase](const std::vector<uint32_t> &args, const std::vector<std::string> &,
                                              const CallContext &context, void *, uint32_t &out) {
    out = (args.size() >= 2u && args[0] >= dayBase && static_cast<uint64_t>(args[0] - dayBase) > static_cast<uint64_t>(context.now) + args[1])
              ? 1u
              : 0u;

    return out != 0u;
  });

  host.registerHandler("passed", [dayBase](const std::vector<uint32_t> &args, const std::vector<std::string> &,
                                           const CallContext &context, void *, uint32_t &out) {
    out = (!args.empty() && args[0] >= dayBase && args[0] - dayBase <= context.now) ? 1u : 0u;

    return out != 0u;
  });

  // olderThan(D, years): at least that many years have passed since week D.
  // weeksSince(D, n) is the same in weeks. Both are what an age is: a birth
  // week node and a guard, so the facts can say `born in 965` and the rules
  // never carry a number that has to be kept in step with the clock.
  host.registerHandler("olderThan", [dayBase](const std::vector<uint32_t> &args, const std::vector<std::string> &,
                                              const CallContext &context, void *, uint32_t &out) {
    out = (args.size() >= 2u && args[0] >= dayBase &&
           static_cast<uint64_t>(args[0] - dayBase) + static_cast<uint64_t>(args[1]) * 52u <= context.now)
              ? 1u
              : 0u;

    return out != 0u;
  });

  host.registerHandler("weeksSince", [dayBase](const std::vector<uint32_t> &args, const std::vector<std::string> &,
                                               const CallContext &context, void *, uint32_t &out) {
    out = (args.size() >= 2u && args[0] >= dayBase && static_cast<uint64_t>(args[0] - dayBase) + args[1] <= context.now) ? 1u : 0u;

    return out != 0u;
  });

  // years(n) / months(n): spans, for `now() + years(2)`.
  host.registerHandler("years", [](const std::vector<uint32_t> &args, const std::vector<std::string> &, const CallContext &, void *,
                                   uint32_t &out) {
    out = args.empty() ? 0u : args[0] * 52u;

    return !args.empty();
  });

  host.registerHandler("months", [](const std::vector<uint32_t> &args, const std::vector<std::string> &, const CallContext &, void *,
                                    uint32_t &out) {
    out = args.empty() ? 0u : args[0] * 4u;

    return !args.empty();
  });

  // roll(lo, hi): a span somewhere in [lo, hi], from the story's own seeded
  // generator - the same seed rolls the same spans in the same order, so a
  // wedding "some months out" is still reproducible.
  host.registerHandler(
      "roll",
      [](const std::vector<uint32_t> &args, const std::vector<std::string> &, const CallContext &, void *user, uint32_t &out) {
        if (args.size() < 2u || args[1] < args[0])
        {
          return false;
        }

        auto &generator = *static_cast<std::mt19937 *>(user);

        out = args[0] + static_cast<uint32_t>(generator() % (args[1] - args[0] + 1u));

        return true;
      },
      &rolls);

}

} // namespace graphrewrite::runtime
