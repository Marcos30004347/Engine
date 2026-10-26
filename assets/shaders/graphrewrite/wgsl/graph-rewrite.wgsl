#include "graph-page.wgsl"

// Applying a pattern's rewrite on the GPU.
//
//   filter_rows        dice and staleness, one thread per row (or, in phase 2,
//                      the CPU's verdict per row)
//   gather_candidates  phase 1 only: copy every live row's vertices out for the
//                      CPU to judge the rule's clauses on
//   claim_removals     every live row claims every edge it removes
//   resolve_claims     a row that did not win every claim dies; survivors are
//                      numbered, and that ordinal names their fresh vertices
//   mark_removals      tombstone every edge the rule consumes
//   emit_insertions    append every edge it produces to a delta buffer
//   emit_fired         one record per row that fired: rule, row, every vertex
//   fill_sort_args     dispatch arguments for the pages that changed
//   compact_pages      one workgroup per DIRTY page: drop tombstones, re-sort
//
// Removal is a tombstone bit set with atomicOr, which makes it idempotent: two
// rules matching the same edge both mark it and it is removed once. Within ONE
// rule, though, two rows removing the same edge must not both fire - the CPU
// applier claims, and a row that cannot claim everything it removes does not
// fire at all. The claim here is a hashed table: every live row atomicMax'es
// (generation << 24 | 0xFFFFFF - row) at the hash of each edge it removes, so
// the lowest row wins and a stale generation never does; a hash collision
// between two different edges is a spurious rejection, rare and deterministic.
//
// Phases. A rule without clauses runs in one frame (phase 0). A rule WITH
// clauses - a guard, a bind, a schedule - runs twice: phase 1 filters and
// gathers its candidate rows for the CPU, which evaluates the clauses and
// writes one control word per row (fire or not) plus the ids of any vertex a
// bind clause named; phase 2 reads that verdict instead of rolling again.
//
// Insertions go to a DELTA rather than into the sorted runs. Splitting a page
// means allocating an id and inserting into the directory, which is pointer
// surgery a kernel should not be doing; the CPU merges the delta into the runs
// afterwards. That is the LSM shape the storage design already assumes.


// One edge of a rule's THEN/removal set. The columns are TABLE columns; a
// vertex the THEN creates has none and is named by `fresh` instead - bits 0-15
// for the origin, 16-31 for the target, 0 meaning "read the column" and k+1
// meaning "this row's k-th new vertex". See RuleEdgeDesc in GraphRewritePass.hpp.
struct RuleEdgeDesc {
  origin_column : u32,
  target_column : u32,
  part_id       : u32,
  fresh         : u32,
};

struct DeltaEdge {
  part_id : u32,
  origin  : u32,
  dest    : u32,
  _pad    : u32,
};

struct RewriteParams {
  row_count        : u32,
  table_word_base  : u32,
  table_stride     : u32,
  removal_count    : u32,
  insertion_count  : u32,
  removal_base     : u32,   // index into rule_edges
  insertion_base   : u32,
  matched_vertices : u32,
  total_vertices   : u32,
  delta_capacity   : u32,
  dirty_capacity   : u32,
  vertex_base      : u32,   // first id handed to a new vertex

  // Word offsets of the tombstone buffer's three regions. These used to be
  // implied - edge bits at 0, dirty flags at `dirty_capacity` - which silently
  // overlapped whenever the pool held more edge bits than dirty_capacity words
  // (845 slots needs 6760 words against a 4096 capacity). The overlap tombstoned
  // live edges in whichever slots the dirty words collided with, and only showed
  // up when one of those slots also happened to be dirty.
  dirty_base       : u32,
  row_base         : u32,
  seed             : u32,   // per-run dice seed, rule already folded in
  chance           : u32,   // thousandths, 0..1000; 1000 means ALWAYS
  phase            : u32,   // 0 apply; 1 candidates only; 2 apply the CPU's verdict
  claim_gen        : u32,   // generation folded into every claim
  rule_id          : u32,   // written into fired records
  ctrl_stride      : u32,   // words per row in row_ctrl: 1 verdict + binds
  claim_mask       : u32,   // claim table size - 1 (a power of two)
  candidate_cap    : u32,   // records
  fired_cap        : u32,   // records
  pad0             : u32,
};

// The candidate and fired records are fixed-size: a count word at [0], then
// `record_words` u32 per record.
const record_words = 16u;
const record_vertices = 12u;

struct Counters {
  delta_count  : atomic<u32>,
  dirty_count  : atomic<u32>,
  vertex_count : atomic<u32>,
  removed      : atomic<u32>,
  // A BITFIELD, not a flag: 1 dirty pages, 2 delta, 4 candidates, 8 fired.
  // One flag meant a run that overflowed could only be debugged by guessing
  // which buffer to enlarge, and guessing wrong three times.
  overflow     : atomic<u32>,
  declined     : atomic<u32>,  // rows whose dice said no
  stale        : atomic<u32>,  // rows whose edges a previous rewrite removed
  rejected     : atomic<u32>,  // rows that lost a claim
  fired        : atomic<u32>,  // rows that fired (= records written, if they fit)
  candidates   : atomic<u32>,  // records gathered in phase 1
};

@group(0) @binding(0) var<uniform>             params     : RewriteParams;
@group(0) @binding(4) var<storage, read_write> tombstone  : array<atomic<u32>>;

// Survivors of every compacted page, gathered by DIRTY INDEX rather than by
// slot. The pool is sized for the working set - a million slots at the default
// budget - so copying it back to decide a handful of rewrites is the one cost
// that grows with memory instead of with work. Indexed this way the readback is
// dirty_count pages long, and the CPU never touches a slot it did not change.
//
// It lives in group 0 because group 1 already holds six storage buffers, and a
// seventh in one group is where this RHI silently drops bindings.
@group(0) @binding(5) var<storage, read_write> dirty_edges : array<Edge>;

@group(1) @binding(0) var<storage, read>       rule_edges   : array<RuleEdgeDesc>;
@group(1) @binding(1) var<storage, read>       table        : array<u32>;
@group(1) @binding(2) var<storage, read_write> dirty_pages  : array<u32>;
@group(1) @binding(3) var<storage, read_write> delta_edges  : array<DeltaEdge>;
@group(1) @binding(4) var<storage, read_write> counters     : Counters;
@group(1) @binding(5) var<storage, read_write> sort_args    : array<u32>;

// Phase traffic with the CPU, and the claim table.
@group(2) @binding(0) var<storage, read_write> candidates : array<u32>;
@group(2) @binding(1) var<storage, read>       row_ctrl   : array<u32>;
@group(2) @binding(2) var<storage, read_write> fired      : array<u32>;
@group(2) @binding(3) var<storage, read_write> claims     : array<atomic<u32>>;
@group(2) @binding(4) var<storage, read_write> row_ordinal : array<u32>;

// The rule's vertex v sits in table column vertex_columns[v]: the table is
// the merged graph's, shared by every rule of the thread, and a rule's own
// numbering starts wherever it was merged in. Records are written in RULE
// order, which is the order the CPU's clauses and the narrator expect.
@group(2) @binding(5) var<storage, read>       vertex_columns : array<u32>;

fn table_at(column : u32, row : u32) -> u32 {
  return table[params.table_word_base + column * params.table_stride + row];
}

// The vertex an edge endpoint names for `row`: a table read, or one of the
// vertices this row created. New vertices are numbered per ROW so every row of
// the match gets its own and every insertion in that row sees the same ones.
fn endpoint(column : u32, code : u32, row : u32) -> u32 {
  if (code == 0u) {
    return table_at(column, row);
  }

  // A bind clause gave this vertex its identity on the CPU (phase 2).
  if (params.phase == 2u) {
    let bound = row_ctrl[row * params.ctrl_stride + code];

    if (bound != 0u) {
      return bound;
    }
  }

  // Otherwise it is numbered by the row's ORDINAL among the rows that fired,
  // not by the row: a table of a million rows of which ten fire must not
  // burn ten million ids.
  let new_per_row = params.total_vertices - params.matched_vertices;

  return params.vertex_base + row_ordinal[row] * new_per_row + (code - 1u);
}

fn origin_of(desc : RuleEdgeDesc, row : u32) -> u32 {
  return endpoint(desc.origin_column, desc.fresh & 0xffffu, row);
}

fn target_of(desc : RuleEdgeDesc, row : u32) -> u32 {
  return endpoint(desc.target_column, desc.fresh >> 16u, row);
}

fn mark_dirty(page : u32) {
  // One entry per page, however many of its edges were tombstoned: the flag bit
  // is what makes the append idempotent.
  let word = page >> 5u;
  let bit  = 1u << (page & 31u);

  let previous = atomicOr(&tombstone[params.dirty_base + word], bit);

  if ((previous & bit) != 0u) {
    return;
  }

  let at = atomicAdd(&counters.dirty_count, 1u);

  if (at < params.dirty_capacity) {
    dirty_pages[at] = page;
  } else {
    atomicOr(&counters.overflow, 1u);
  }
}


// --- the dice ---------------------------------------------------------------
//
// Stateless: the roll for a row is a hash of (seed, row), not a draw from a
// sequence. That is what lets one thread per row roll independently, and it is
// also what keeps the CPU and GPU runners in agreement - a sequential RNG would
// have to be walked in the same order on both, which the GPU cannot promise.
//
// `seed` already has the rule folded into it by the host, so two rules matching
// the same row do not roll the same number.
fn dice_roll(row : u32) -> u32 {
  var h = params.seed ^ (row * 0x9e3779b9u);

  h = h ^ (h >> 16u);
  h = h * 0x85ebca6bu;
  h = h ^ (h >> 13u);
  h = h * 0xc2b2ae35u;
  h = h ^ (h >> 16u);

  return h;
}

fn row_fires(row : u32) -> bool {
  // chance is in thousandths, 0..1000; 1000 (or an ALWAYS rule, which the host
  // encodes as 1000) skips the roll entirely so an always-rule costs nothing.
  // Mirrors dice::fires in Dice.hpp exactly.
  if (params.chance >= 1000u) {
    return true;
  }

  if (params.chance == 0u) {
    return false;
  }

  return (dice_roll(row) % 1000u) < params.chance;
}

// The pool index of this edge inside one page, or not_resident.
//
// A page is sorted by KEY (the origin, for an origin-ordered partition), so
// the first index whose origin reaches the one wanted is a binary search, and
// only the span of equal origins after it has to be read. Edges of one origin
// are not ordered by dest - insert keeps only the key order - so that span is
// scanned.
fn find_in_page(page : u32, origin : u32, dest : u32) -> u32 {
  let slot = page_meta[page].slot;

  if (slot == not_resident) {
    return not_resident;
  }

  let base  = slot * page_capacity;
  let limit = page_meta[page].count;

  if (limit == 0u) {
    return not_resident;
  }

  // First index with origin >= the one wanted. Bounds are i32: `hi = mid - 1`
  // at mid == 0 underflows a u32 and walks off the front.
  var lo = i32(limit);
  var a  = 0;
  var b  = i32(limit) - 1;

  loop {
    if (a > b) { break; }
    let m = a + (b - a) / 2;
    if (page_pool[base + u32(m)].origin >= origin) { lo = m; b = m - 1; } else { a = m + 1; }
  }

  var i = u32(max(lo, 0));

  loop {
    if (i >= limit) { break; }

    let edge = page_pool[base + i];

    if (edge.origin != origin) { break; }
    if (edge.dest == dest) { return base + i; }

    i = i + 1u;
  }

  return not_resident;
}

// The pool index of the first live copy of an edge, or not_resident.
//
// The directory is binary-searched for the RUN of pages whose key range covers
// the origin, the way the match kernel does it. Scanning every page of the
// partition instead - which is what this did - is thousands of iterations per
// row per edge once a partition holds thousands of pages, and it is paid by
// filter_rows, claim_removals, resolve_claims and mark_removals alike.
fn find_edge(part_id : u32, origin : u32, dest : u32) -> u32 {
  let run = directory_range(part_id, origin);

  if (run_is_empty(run)) {
    return not_resident;
  }

  for (var page = run.first; page <= run.last; page = page + 1u) {
    let at = find_in_page(page, origin, dest);

    if (at != not_resident) {
      return at;
    }
  }

  return not_resident;
}

// Does a live copy of this edge still exist? A previous rule's rewrite may have
// removed it and compacted it out of its page, which leaves this row's match
// referring to a graph that no longer holds.
fn edge_exists(part_id : u32, origin : u32, dest : u32) -> bool {
  return find_edge(part_id, origin, dest) != not_resident;
}

fn set_row_live(row : u32, live : bool) {
  var value = 0u;

  if (live) {
    value = 1u;
  }

  atomicStore(&tombstone[params.row_base + row], value);
}

fn row_is_live(row : u32) -> bool {
  return atomicLoad(&tombstone[params.row_base + row]) != 0u;
}

// One thread per matched row, run BEFORE either apply kernel. It decides two
// separate things and writes one answer:
//
//   1. did this row's dice come up, and
//   2. does the graph still contain every edge this row is about to remove.
//
// (2) is why this is a pass of its own rather than a test inside the apply
// kernels: once mark_removals starts tombstoning, a later row can no longer tell
// "this edge was already gone" from "another row in this same batch just took
// it". Deciding it up front makes every row see the same graph.
@compute @workgroup_size(64)
fn filter_rows(@builtin(global_invocation_id) gid : vec3<u32>) {
  let row = gid.x;

  if (row >= params.row_count) {
    return;
  }

  if (params.phase == 2u) {
    // The CPU already saw the dice and the staleness in phase 1 and has judged
    // the clauses; its word is final.
    let verdict = row_ctrl[row * params.ctrl_stride];

    set_row_live(row, verdict != 0u);

    if (verdict == 0u) {
      atomicAdd(&counters.declined, 1u);
    }

    return;
  }

  if (!row_fires(row)) {
    set_row_live(row, false);
    atomicAdd(&counters.declined, 1u);

    return;
  }

  for (var slot = 0u; slot < params.removal_count; slot = slot + 1u) {
    let desc = rule_edges[params.removal_base + slot];

    if (!edge_exists(desc.part_id, origin_of(desc, row), target_of(desc, row))) {
      set_row_live(row, false);
      atomicAdd(&counters.stale, 1u);

      return;
    }
  }

  set_row_live(row, true);
}

// Phase 1: every live row's matched vertices, compactly, for the CPU.
@compute @workgroup_size(64)
fn gather_candidates(@builtin(global_invocation_id) gid : vec3<u32>) {
  let row = gid.x;

  if (params.phase != 1u || row >= params.row_count || !row_is_live(row)) {
    return;
  }

  let at = atomicAdd(&counters.candidates, 1u);

  if (at >= params.candidate_cap) {
    atomicOr(&counters.overflow, 4u);
    return;
  }

  let base = 1u + at * record_words;

  candidates[base] = row;
  candidates[base + 1u] = params.matched_vertices;

  for (var v = 0u; v < params.matched_vertices && v < record_vertices; v = v + 1u) {
    candidates[base + 2u + v] = table_at(vertex_columns[v], row);
  }
}

fn claim_hash(index : u32) -> u32 {
  var h = index * 0x9e3779b1u;
  h = h ^ (h >> 15u);
  h = h * 0x85ebca6bu;
  h = h ^ (h >> 13u);
  return h & params.claim_mask;
}

fn claim_key(row : u32) -> u32 {
  return (params.claim_gen << 24u) | (0xFFFFFFu - row);
}

fn edge_index(part_id : u32, origin : u32, dest : u32) -> u32 {
  return find_edge(part_id, origin, dest);
}

@compute @workgroup_size(64)
fn claim_removals(@builtin(global_invocation_id) gid : vec3<u32>) {
  if (params.phase == 1u || params.removal_count == 0u) {
    return;
  }

  let total = params.row_count * params.removal_count;

  if (gid.x >= total) {
    return;
  }

  let row  = gid.x / params.removal_count;
  let slot = gid.x % params.removal_count;

  if (!row_is_live(row)) {
    return;
  }

  let desc = rule_edges[params.removal_base + slot];
  let index = edge_index(desc.part_id & 0x7FFFFFFFu, origin_of(desc, row), target_of(desc, row));

  if (index == not_resident) {
    return;
  }

  atomicMax(&claims[claim_hash(index)], claim_key(row));
}

// One thread per row, after every claim is in: a row that lost any claim
// dies; the rest are numbered in order of arrival, and that ordinal is what
// names their fresh vertices and places their fired record.
@compute @workgroup_size(64)
fn resolve_claims(@builtin(global_invocation_id) gid : vec3<u32>) {
  let row = gid.x;

  if (params.phase == 1u || row >= params.row_count || !row_is_live(row)) {
    return;
  }

  for (var slot = 0u; slot < params.removal_count; slot = slot + 1u) {
    let desc = rule_edges[params.removal_base + slot];
    let index = edge_index(desc.part_id & 0x7FFFFFFFu, origin_of(desc, row), target_of(desc, row));

    if (index == not_resident) {
      continue;
    }

    if (atomicLoad(&claims[claim_hash(index)]) != claim_key(row)) {
      set_row_live(row, false);
      atomicAdd(&counters.rejected, 1u);

      return;
    }
  }

  row_ordinal[row] = atomicAdd(&counters.fired, 1u);
}

// One record per row that fired: rule, row, counts, then every vertex -
// matched first, then the ones this row created. This is what the CPU reads
// to narrate; it never needs the table itself.
@compute @workgroup_size(64)
fn emit_fired(@builtin(global_invocation_id) gid : vec3<u32>) {
  let row = gid.x;

  if (params.phase == 1u || row >= params.row_count || !row_is_live(row)) {
    return;
  }

  let at = row_ordinal[row];

  if (at >= params.fired_cap) {
    atomicOr(&counters.overflow, 8u);
    return;
  }

  let base = 1u + at * record_words;

  fired[base] = params.rule_id;
  fired[base + 1u] = row;
  fired[base + 2u] = params.matched_vertices;
  fired[base + 3u] = params.total_vertices;

  for (var v = 0u; v < params.total_vertices && v < record_vertices; v = v + 1u) {
    if (v < params.matched_vertices) {
      fired[base + 4u + v] = table_at(vertex_columns[v], row);
    } else {
      fired[base + 4u + v] = endpoint(0u, v - params.matched_vertices + 1u, row);
    }
  }
}

@compute @workgroup_size(64)
fn mark_removals(@builtin(global_invocation_id) gid : vec3<u32>) {
  if (params.phase == 1u || params.removal_count == 0u) {
    return;
  }

  let total = params.row_count * params.removal_count;

  if (gid.x >= total) {
    return;
  }

  let row  = gid.x / params.removal_count;
  let slot = gid.x % params.removal_count;

  if (!row_is_live(row)) {
    return;
  }

  let desc = rule_edges[params.removal_base + slot];

  let origin = origin_of(desc, row);
  let dest   = target_of(desc, row);

  // The directory narrows this to the run of pages whose key range covers the
  // origin; a page that is not resident cannot hold an edge this match found,
  // because the match read it from a resident one.
  let run = directory_range(desc.part_id & 0x7FFFFFFFu, origin);

  if (run_is_empty(run)) {
    return;
  }

  for (var page = run.first; page <= run.last; page = page + 1u) {
    let index = find_in_page(page, origin, dest);

    if (index == not_resident) {
      continue;
    }

    let previous = atomicOr(&tombstone[index >> 5u], 1u << (index & 31u));

    if ((previous & (1u << (index & 31u))) == 0u) {
      atomicAdd(&counters.removed, 1u);
      mark_dirty(page);
    }

    return; // one copy is enough
  }
}

@compute @workgroup_size(64)
fn emit_insertions(@builtin(global_invocation_id) gid : vec3<u32>) {
  if (params.phase == 1u || params.insertion_count == 0u) {
    return;
  }

  let total = params.row_count * params.insertion_count;

  if (gid.x >= total) {
    return;
  }

  let row  = gid.x / params.insertion_count;
  let slot = gid.x % params.insertion_count;

  if (!row_is_live(row)) {
    return;
  }

  let desc = rule_edges[params.insertion_base + slot];

  // Bit 31 of part_id marks an insertion the CPU scheduled for later: the
  // queue lands it on its day, not this kernel.
  if ((desc.part_id & 0x80000000u) != 0u) {
    return;
  }

  let new_per_row = params.total_vertices - params.matched_vertices;

  let origin = origin_of(desc, row);
  let dest   = target_of(desc, row);

  let at = atomicAdd(&counters.delta_count, 1u);

  if (at >= params.delta_capacity) {
    atomicOr(&counters.overflow, 2u);
    return;
  }

  delta_edges[at].part_id = desc.part_id;
  delta_edges[at].origin  = origin;
  delta_edges[at].dest    = dest;

  if (new_per_row != 0u) {
    atomicMax(&counters.vertex_count, params.vertex_base + (row_ordinal[row] + 1u) * new_per_row);
  }
}

fn div_ceil(value : u32, divisor : u32) -> u32 {
  return (value + divisor - 1u) / divisor;
}

@compute @workgroup_size(1)
fn fill_sort_args(@builtin(global_invocation_id) gid : vec3<u32>) {
  // One workgroup per dirty page. This is the indirect dispatch the whole point
  // of the dirty list: pages nothing touched are never visited.
  let dirty = min(atomicLoad(&counters.dirty_count), params.dirty_capacity);

  // The record counts travel with the records, so one copy brings both.
  fired[0] = min(atomicLoad(&counters.fired), params.fired_cap);
  candidates[0] = min(atomicLoad(&counters.candidates), params.candidate_cap);

  sort_args[0] = dirty;
  sort_args[1] = 1u;
  sort_args[2] = 1u;
}

// Two scalar arrays rather than one array<Edge, N>. A workgroup array of a
// STRUCT type makes SPIRV-Cross emit spvArrayCopyFromConstantToThreadGroup
// against an spvUnsafeArray, which does not compile under MoltenVK:
//   "no matching function for call to 'spvArrayCopyFromConstantToThreadGroup'"
// Scalars avoid the generated copy entirely.
var<workgroup> shared_origin : array<u32, page_capacity>;
var<workgroup> shared_dest   : array<u32, page_capacity>;
var<workgroup> shared_count  : atomic<u32>;

// One workgroup per dirty page: drop the tombstoned edges and re-sort what is
// left. A page is 256 edges, so the sort fits entirely in workgroup memory.
@compute @workgroup_size(page_capacity)
fn compact_pages(@builtin(workgroup_id) group : vec3<u32>, @builtin(local_invocation_id) local : vec3<u32>) {
  let dirty = min(atomicLoad(&counters.dirty_count), params.dirty_capacity);

  if (group.x >= dirty) {
    return;
  }

  let page = dirty_pages[group.x];
  let slot = page_meta[page].slot;

  if (slot == not_resident) {
    return;
  }

  if (local.x == 0u) {
    atomicStore(&shared_count, 0u);
  }

  workgroupBarrier();

  let limit = page_meta[page].count;
  let base  = slot * page_capacity;

  // Survivors keep their relative order, which is already sorted, so compaction
  // alone restores the invariant - no comparison sort is needed.
  if (local.x < limit) {
    let index = base + local.x;
    let alive = (atomicLoad(&tombstone[index >> 5u]) & (1u << (index & 31u))) == 0u;

    if (alive) {
      var rank = 0u;

      for (var i = 0u; i < local.x; i = i + 1u) {
        let other = base + i;
        if ((atomicLoad(&tombstone[other >> 5u]) & (1u << (other & 31u))) == 0u) {
          rank = rank + 1u;
        }
      }

      let survivor = page_pool[index];

      shared_origin[rank] = survivor.origin;
      shared_dest[rank]   = survivor.dest;

      atomicAdd(&shared_count, 1u);
    }
  }

  workgroupBarrier();

  let survivors = atomicLoad(&shared_count);

  if (local.x < survivors) {
    page_pool[base + local.x].origin = shared_origin[local.x];
    page_pool[base + local.x].dest   = shared_dest[local.x];

    let gathered = group.x * page_capacity + local.x;

    dirty_edges[gathered].origin = shared_origin[local.x];
    dirty_edges[gathered].dest   = shared_dest[local.x];
  }

  workgroupBarrier();

  if (local.x == 0u) {
    // The new count goes into the dirty list beside the page id, so the CPU can
    // read both back in one go.
    dirty_pages[params.dirty_capacity + group.x] = survivors;
  }
}

@compute @workgroup_size(1)
fn reset_counters(@builtin(global_invocation_id) gid : vec3<u32>) {
  // Dispatch arguments for every per-row and per-edge kernel below, from the
  // row count: a frame that applies no rule (every match frame) launches
  // nothing, instead of the row CAPACITY's worth of empty workgroups.
  let row_groups = div_ceil(params.row_count, 64u);
  let per_row = max(params.removal_count, params.insertion_count);
  let edge_groups = div_ceil(params.row_count * max(per_row, 1u), 64u);

  sort_args[3] = row_groups;
  sort_args[4] = 1u;
  sort_args[5] = 1u;
  sort_args[6] = edge_groups;
  sort_args[7] = 1u;
  sort_args[8] = 1u;

  atomicStore(&counters.delta_count, 0u);
  atomicStore(&counters.dirty_count, 0u);
  atomicStore(&counters.vertex_count, params.vertex_base);
  atomicStore(&counters.removed, 0u);
  atomicStore(&counters.overflow, 0u);
  atomicStore(&counters.declined, 0u);
  atomicStore(&counters.stale, 0u);
  atomicStore(&counters.rejected, 0u);
  atomicStore(&counters.fired, 0u);
  atomicStore(&counters.candidates, 0u);
}

@compute @workgroup_size(256)
fn clear_claims(@builtin(global_invocation_id) gid : vec3<u32>) {
  if (gid.x <= params.claim_mask) {
    atomicStore(&claims[gid.x], 0u);
  }
}
