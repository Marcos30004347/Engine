#pragma once

// What the applier tells an observer about one matched row.
//
// A row used to either fire or not. With schedules there are three outcomes
// worth telling: it fired now; it was put in the queue for later; or its time
// came and the match no longer held, so it was cancelled. The narrator wants
// all three - "they set a date", "they were married", "the wedding never took
// place" are three different sentences.

#include <cstdint>
#include <vector>

namespace graphrewrite::runtime {

enum class RewriteEventKind
{
  Applied,   // the edges changed, now
  Scheduled, // queued for `time`; nothing changed yet
  Released,  // its time came, the match still held, the edges landed
  Cancelled, // its time came and a WHEN edge was gone
};

struct RewriteEvent
{
  uint32_t rule = 0u;
  std::vector<uint32_t> bound; // matched vertices, then the ones THEN created
  uint32_t rowIndex = 0u;
  RewriteEventKind kind = RewriteEventKind::Applied;

  // For Scheduled: when it is due. For Applied and Cancelled: the current time.
  uint32_t time = 0u;
};

} // namespace graphrewrite::runtime
