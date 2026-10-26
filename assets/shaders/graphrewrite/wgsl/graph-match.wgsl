#ifndef EDGES_PER_THREAD
#define EDGES_PER_THREAD 1
#endif

#include "graph-page.wgsl"

// One instruction of a match, in five stages.
//
//   plan    per row, advance over the RESIDENT prefix of its candidate pages and
//           request the first missing one
//   count   per row, count matches in that resident batch
//   scan    exclusive prefix sum over the row counts
//   write   expand each row into the write table
//   advance move the cursors, and report whether any row is still incomplete
//
// The per-row cursors are what make this resumable: a row stops at the first
// page it cannot see, and picks up there on a later frame once the streaming
// manager has admitted it. Because the read table is immutable for the life of
// the instruction, each row's cursor only moves forward over disjoint pages, and
// each batch appends at a base that advances by exactly that batch's total, a
// (row, page) pair is processed exactly once however the frames divide.


const direction_backward = 0u;
const direction_forward  = 1u;
const direction_cyclic   = 2u;

// row_state is one buffer rather than five, because a binding group here is
// already close to the point where this RHI silently drops bindings.
// Per-row state. Seven fields rather than five because a row now also records
// WHERE its candidate edges start, so the edge-parallel stage can jump straight
// to them instead of re-searching.
const state_stride     = 8u;
const scan_block_size  = 256u;
const state_cursor    = 0u;
const state_last      = 1u;
const state_batch_end = 2u;
const state_count      = 3u;   // candidate edges for this row in this batch
const state_base       = 4u;   // exclusive scan of state_count
const state_first_page = 5u;   // page its candidates start in
const state_first_slot = 6u;   // index within that page
const state_true_count = 7u;   // candidates before padding

// Candidate edges each thread handles. Every row's candidate count is rounded UP
// to a multiple of this, so a thread's block of candidates always lies inside a
// single row - which is what lets one find_row and one page walk serve all of
// them.
const edges_per_thread : u32 = EDGES_PER_THREAD;

struct MatchParams {
  part_id        : u32,   // `partition` is a reserved keyword in WGSL
  direction      : u32,
  from_column    : u32,
  write_column   : u32,
  read_register  : u32,   // whose row count bounds this instruction
  write_register : u32,   // whose row count it appends to
  read_stride    : u32,   // the stride the input table was WRITTEN at
  read_cols      : u32,
  write_cols     : u32,
  write_capacity : u32,
  first_frame    : u32,
  page_edges     : u32,
  bound_mask     : u32, // one bit per column that already holds a vertex

  // Every table lives in ONE buffer, addressed by a word offset. Bindings are
  // fixed when a pass records, so this is what lets a single recorded pass serve
  // every instruction and every register of a programme.
  // 0 = Select (seed from a partition), 1 = Where (extend the read table).
  // A pass records once, so both paths are always dispatched and this is what
  // decides which of them does anything.
  mode           : u32,
  block_sums_base: u32,   // word offset of the scan's block sums in row_state
  read_base      : u32,
  write_word_base: u32,   // word offset of this table in the shared buffer
  _pad0          : u32,
};

struct MatchStatus {
  incomplete_rows : atomic<u32>,
  stalled_rows    : atomic<u32>,
  written_rows    : atomic<u32>,
  overflowed      : atomic<u32>,
  candidate_total : atomic<u32>,   // candidate edges this batch, = the dispatch size
};

// Split across two groups on purpose. Eight storage bindings in one group has
// silently dropped two of them (and a write) in this engine before, so the page
// tier and the table tier get a group each - four storage bindings apiece.
@group(0) @binding(0) var<uniform>             params       : MatchParams;
@group(0) @binding(4) var<storage, read_write> page_request : array<atomic<u32>>;

@group(1) @binding(0) var<storage, read>       read_table   : array<u32>;
@group(1) @binding(1) var<storage, read_write> write_table  : array<u32>;
@group(1) @binding(2) var<storage, read_write> row_state    : array<u32>;
@group(1) @binding(3) var<storage, read_write> status       : MatchStatus;

// Row counts live on the GPU, one per register. That is what lets the dispatch
// sizes be computed here and read by cmdDispatchIndirect, instead of the CPU
// reading a count back and deciding how many groups to launch.
@group(1) @binding(4) var<storage, read_write> table_rows   : array<atomic<u32>>;
@group(1) @binding(5) var<storage, read_write> dispatch_args: array<u32>;

// The stride a table is read at must be the stride it was WRITTEN at. Tables are
// written unpacked - at their row CAPACITY - because an instruction that resumes
// across frames does not know its final row count until it finishes. Reading one
// back at its row count instead silently shears every column after the first.
fn read_rows() -> u32 {
  return atomicLoad(&table_rows[params.read_register]);
}

fn write_rows() -> u32 {
  return atomicLoad(&table_rows[params.write_register]);
}

fn read_at(column : u32, row : u32) -> u32 {
  return read_table[params.read_base + column * params.read_stride + row];
}

fn write_at(column : u32, row : u32, value : u32) {
  write_table[params.write_word_base + column * params.write_capacity + row] = value;
}

const args_seed  = 0u;   // dispatch_args[0..2]
const args_rows  = 3u;   // dispatch_args[3..5]
const args_edges = 6u;   // dispatch_args[6..8]

fn div_ceil(value : u32, divisor : u32) -> u32 {
  return (value + divisor - 1u) / divisor;
}

fn round_up(value : u32, multiple : u32) -> u32 {
  return div_ceil(value, multiple) * multiple;
}

// Computes the dispatch sizes for this frame from state that lives on the GPU.
// Everything downstream is launched with cmdDispatchIndirect off this buffer, so
// a table with three rows launches one workgroup rather than the worst case.
fn fill_dispatch_args_body() {
  var seed_groups = 0u;
  var row_groups  = 0u;

  if (params.mode == 0u) {
    let first = part_first[params.part_id];
    seed_groups = div_ceil(part_first[params.part_id + 1u] - first, 64u);
  } else {
    row_groups = div_ceil(read_rows(), 256u);
  }

  dispatch_args[args_seed + 0u] = seed_groups;
  dispatch_args[args_seed + 1u] = 1u;
  dispatch_args[args_seed + 2u] = 1u;

  dispatch_args[args_rows + 0u] = row_groups;
  dispatch_args[args_rows + 1u] = 1u;
  dispatch_args[args_rows + 2u] = 1u;
}

// Run after the scan, once the candidate total is known: this is what sizes the
// edge-parallel stage.
@compute @workgroup_size(1)
fn fill_edge_args(@builtin(global_invocation_id) gid : vec3<u32>) {
  var groups = 0u;

  if (params.mode == 1u) {
    groups = div_ceil(div_ceil(atomicLoad(&status.candidate_total), edges_per_thread), 256u);
  }

  dispatch_args[args_edges + 0u] = groups;
  dispatch_args[args_edges + 1u] = 1u;
  dispatch_args[args_edges + 2u] = 1u;
}

// Folds this frame's output into the register's row count. Doing it here rather
// than on the CPU is what keeps the next instruction's dispatch size on the GPU.
@compute @workgroup_size(1)
fn finalize(@builtin(global_invocation_id) gid : vec3<u32>) {
  let produced = atomicLoad(&status.written_rows);

  atomicAdd(&table_rows[params.write_register], produced);
}

// Block sums are appended after the per-row state in the same buffer.
fn block_sum_at(block : u32) -> u32 {
  return row_state[params.block_sums_base + block];
}

fn set_block_sum(block : u32, value : u32) {
  row_state[params.block_sums_base + block] = value;
}

fn state_at(row : u32, field : u32) -> u32 {
  return row_state[row * state_stride + field];
}

fn set_state(row : u32, field : u32, value : u32) {
  row_state[row * state_stride + field] = value;
}

fn key_of(edge : Edge) -> u32 {
  if (params.direction == direction_backward) {
    return edge.dest;
  }
  return edge.origin;
}

fn other_of(edge : Edge) -> u32 {
  if (params.direction == direction_backward) {
    return edge.origin;
  }
  return edge.dest;
}

// Does this candidate extend the row? Cyclic filters against a column that is
// already bound; otherwise the new vertex must not repeat a bound one.
fn accepts(row : u32, bound_vertex : u32, other : u32) -> bool {
  if (params.direction == direction_cyclic) {
    return read_at(params.write_column, row) == other;
  }

  // Only columns that actually hold a vertex take part. A table is allocated at
  // full width up front, so unbound columns are still zero, and letting them
  // vote rejects every match whose new vertex happens to be vertex 0.
  for (var column = 0u; column < params.read_cols; column = column + 1u) {
    if ((params.bound_mask & (1u << column)) == 0u) {
      continue;
    }
    if (read_at(column, row) == other) {
      return false;
    }
  }

  return true;
}

// First index in a page whose key is >= `key`. A page holds up to 256 edges in
// key order, so walking all of them to find the handful that match is 256
// iterations where 8 will do - and that inner loop runs once per row per page,
// twice over (count, then write), which is where the time actually goes.
fn page_lower_bound(page : u32, key : u32) -> u32 {
  let base = page_meta[page].slot * page_capacity;

  var lo = 0;
  var hi = i32(page_meta[page].count) - 1;
  var found = i32(page_meta[page].count);

  loop {
    if (lo > hi) { break; }

    let mid = lo + (hi - lo) / 2;
    let edge = page_pool[base + u32(mid)];
    var mid_key = edge.origin;

    if (params.direction == direction_backward) {
      mid_key = edge.dest;
    }

    if (mid_key >= key) {
      found = mid;
      hi = mid - 1;
    } else {
      lo = mid + 1;
    }
  }

  return u32(found);
}

// How many edges in this page carry `key`, and where they start. A page is
// key-ordered, so both ends are a binary search.
fn page_slice(page : u32, key : u32, out_first : ptr<function, u32>) -> u32 {
  let first = page_lower_bound(page, key);

  *out_first = first;

  let limit = page_meta[page].count;
  let base = page_meta[page].slot * page_capacity;

  var count = 0u;
  var i = first;

  loop {
    if (i >= limit) { break; }

    let edge = page_pool[base + i];
    var edge_key = edge.origin;

    if (params.direction == direction_backward) {
      edge_key = edge.dest;
    }

    if (edge_key != key) { break; }

    count = count + 1u;
    i = i + 1u;
  }

  return count;
}

fn count_row(row : u32) {
  let bound_vertex = read_at(params.from_column, row);

  var candidates = 0u;
  var first_page = state_at(row, state_cursor);
  var first_slot = 0u;
  var found_start = false;

  for (var page = state_at(row, state_cursor); page < state_at(row, state_batch_end); page = page + 1u) {
    if (page_meta[page].slot == not_resident) {
      continue;
    }

    var slot = 0u;
    let count = page_slice(page, bound_vertex, &slot);

    if (count != 0u && !found_start) {
      first_page = page;
      first_slot = slot;
      found_start = true;
    }

    candidates = candidates + count;
  }

  // The scan runs over the PADDED counts, so every row's base is a multiple of
  // edges_per_thread and no thread's block straddles two rows.
  set_state(row, state_count, round_up(candidates, edges_per_thread));
  set_state(row, state_true_count, candidates);
  set_state(row, state_first_page, first_page);
  set_state(row, state_first_slot, first_slot);
}


@compute @workgroup_size(256)
fn plan(@builtin(global_invocation_id) gid : vec3<u32>) {
  if (params.mode != 1u) { return; }

  let row = gid.x;

  if (row >= read_rows()) {
    return;
  }

  if (params.first_frame == 1u) {
    let key = read_at(params.from_column, row);

    let run = directory_range(params.part_id, key);

    if (!run_is_empty(run)) {
      set_state(row, state_cursor, run.first);
      set_state(row, state_last, run.last);
    } else {
      // last < cursor marks the row already complete.
      set_state(row, state_cursor, 1u);
      set_state(row, state_last, 0u);
    }
  }

  var page = state_at(row, state_cursor);
  let last = state_at(row, state_last);

  loop {
    if (page > last) { break; }
    if (page_meta[page].slot == not_resident) { break; }
    page = page + 1u;
  }

  set_state(row, state_batch_end, page);

  if (page <= last) {
    // The GPU emits its own streaming decision: mark the page it is blocked on.
    atomicOr(&page_request[page >> 5u], 1u << (page & 31u));
    atomicAdd(&status.stalled_rows, 1u);
  }

  // Counting is fused in rather than dispatched separately. It reads only
  // state_cursor and state_batch_end FOR THIS ROW, which the lines above just
  // wrote from this same thread - so the barrier that used to sit between them
  // was ordering a thread against itself.
  //
  // That barrier is not free: it is a graph node, which on this backend ends the
  // compute encoder. At a hundred thousand edges a tick is round-trip bound, not
  // compute bound, so a dispatch removed is worth more than any amount of work
  // saved inside one.
  count_row(row);
}

// Counts CANDIDATE edges for each row - the edges whose key matches - without
// testing any of them. The test happens once per edge in match_edges, on a
// thread of its own.

// Exclusive prefix sum over the per-row match counts, in three stages.
//
// A single-invocation serial scan is correct and was what this started as, but
// at benchmark scale it IS the runtime: half a million rows walked by one thread
// took ten seconds where the whole rest of the match took milliseconds. The
// serial part is now over BLOCKS rather than rows, which for a million rows is
// four thousand iterations instead of a million.

var<workgroup> scan_shared : array<u32, scan_block_size>;

// Stage 1: each workgroup scans its own 256 rows and publishes its total.
@compute @workgroup_size(scan_block_size)
fn scan_blocks(@builtin(global_invocation_id) gid : vec3<u32>,
               @builtin(local_invocation_id) local : vec3<u32>,
               @builtin(workgroup_id) group : vec3<u32>) {
  if (params.mode != 1u) { return; }

  let row = gid.x;
  let rows = read_rows();

  scan_shared[local.x] = select(0u, state_at(row, state_count), row < rows);

  workgroupBarrier();

  // Hillis-Steele inclusive scan.
  for (var offset = 1u; offset < scan_block_size; offset = offset << 1u) {
    var addend = 0u;

    if (local.x >= offset) {
      addend = scan_shared[local.x - offset];
    }

    workgroupBarrier();

    if (local.x >= offset) {
      scan_shared[local.x] = scan_shared[local.x] + addend;
    }

    workgroupBarrier();
  }

  // Exclusive = inclusive minus own value.
  if (row < rows) {
    set_state(row, state_base, scan_shared[local.x] - state_at(row, state_count));
  }

  if (local.x == scan_block_size - 1u) {
    set_block_sum(group.x, scan_shared[local.x]);
  }
}

// Stage 2: one invocation over the BLOCK totals. Serial, but over blocks.
@compute @workgroup_size(1)
fn scan_block_sums(@builtin(global_invocation_id) gid : vec3<u32>) {
  if (params.mode != 1u) { return; }

  let blocks = div_ceil(read_rows(), scan_block_size);

  var running = 0u;

  for (var block = 0u; block < blocks; block = block + 1u) {
    let total = block_sum_at(block);

    set_block_sum(block, running);

    running = running + total;
  }

  // The candidate total IS the dispatch size for the edge-parallel stage, which
  // is the whole reason that stage can be launched indirectly.
  atomicStore(&status.candidate_total, running);
}

// Stage 3: each row adds its block's offset.
@compute @workgroup_size(scan_block_size)
fn scan_add_offsets(@builtin(global_invocation_id) gid : vec3<u32>,
                    @builtin(workgroup_id) group : vec3<u32>) {
  if (params.mode != 1u) { return; }

  let row = gid.x;

  if (row >= read_rows()) {
    return;
  }

  set_state(row, state_base, state_at(row, state_base) + block_sum_at(group.x));
}

// Binary search over the candidate bases: which row does this thread's edge
// belong to?
fn find_row(candidate : u32) -> u32 {
  var lo = 0;
  var hi = i32(read_rows()) - 1;
  var found = 0;

  loop {
    if (lo > hi) { break; }

    let mid = lo + (hi - lo) / 2;

    if (state_at(u32(mid), state_base) <= candidate) {
      found = mid;
      lo = mid + 1;
    } else {
      hi = mid - 1;
    }
  }

  return u32(found);
}

// Each thread takes a block of `edges_per_thread` candidate edges.
//
// Testing an edge is comparing two node ids, so at one edge per thread the
// per-thread setup - a binary search for the row, then a walk to the right page -
// costs more than the test it enables. Amortising that over a block of edges is
// what the padding above exists for: the block is guaranteed to sit inside one
// row, so the setup happens once and then it is a straight walk.
//
// Output rows are reserved with one atomic. Row order is not meaningful - see
// the resume argument - so there is nothing to preserve by grouping them.
@compute @workgroup_size(256)
fn match_edges(@builtin(global_invocation_id) gid : vec3<u32>) {
  if (params.mode != 1u) { return; }

  let first_candidate = gid.x * edges_per_thread;

  if (first_candidate >= atomicLoad(&status.candidate_total)) {
    return;
  }

  let row = find_row(first_candidate);
  let row_base = state_at(row, state_base);
  let true_count = state_at(row, state_true_count);

  var offset = first_candidate - row_base;

  if (offset >= true_count) {
    return; // entirely in this row's padding
  }

  let bound_vertex = read_at(params.from_column, row);
  let batch_end = state_at(row, state_batch_end);

  // Walk to the page holding the block's first candidate. A key's edges are
  // contiguous, so this crosses a boundary at most once or twice.
  var page = state_at(row, state_first_page);
  var slot = state_at(row, state_first_slot);
  var remaining_here = 0u;

  loop {
    if (page >= batch_end) { return; }

    if (page_meta[page].slot == not_resident) {
      page = page + 1u;
      continue;
    }

    var start = 0u;
    let count = page_slice(page, bound_vertex, &start);

    if (offset < count) {
      slot = start + offset;
      remaining_here = count - offset;
      break;
    }

    offset = offset - count;
    page = page + 1u;
  }

  var taken = 0u;

  loop {
    if (taken >= edges_per_thread) { break; }
    if (first_candidate - row_base + taken >= true_count) { break; }

    if (remaining_here == 0u) {
      // Move to the next page that carries this key.
      page = page + 1u;

      loop {
        if (page >= batch_end) { return; }
        if (page_meta[page].slot != not_resident) { break; }
        page = page + 1u;
      }

      var start = 0u;
      let count = page_slice(page, bound_vertex, &start);

      if (count == 0u) {
        continue;
      }

      slot = start;
      remaining_here = count;
    }

    let edge = page_pool[page_meta[page].slot * page_capacity + slot];
    let other = other_of(edge);

    if (accepts(row, bound_vertex, other)) {
      let at = write_rows() + atomicAdd(&status.written_rows, 1u);

      if (at >= params.write_capacity) {
        atomicStore(&status.overflowed, 1u);
        return;
      }

      for (var column = 0u; column < params.read_cols; column = column + 1u) {
        write_at(column, at, read_at(column, row));
      }

      if (params.direction != direction_cyclic) {
        write_at(params.write_column, at, other);
      }
    }

    slot = slot + 1u;
    remaining_here = remaining_here - 1u;
    taken = taken + 1u;
  }
}

@compute @workgroup_size(256)
fn advance(@builtin(global_invocation_id) gid : vec3<u32>) {
  if (params.mode != 1u) { return; }

  let row = gid.x;

  if (row >= read_rows()) {
    return;
  }

  set_state(row, state_cursor, state_at(row, state_batch_end));

  if (state_at(row, state_cursor) <= state_at(row, state_last)) {
    atomicAdd(&status.incomplete_rows, 1u);
  }
}

// Seeds a table from every edge of one partition.
//
// The resume unit here is a PAGE rather than a row: row_state[page] is a done
// flag, so a page that was missing this frame is simply picked up on a later
// one, exactly as a stalled row is. The atomic reservation is what lets pages be
// consumed in any order without the result depending on that order.
@compute @workgroup_size(64)
fn seed(@builtin(global_invocation_id) gid : vec3<u32>) {
  if (params.mode != 0u) { return; }

  let first = part_first[params.part_id];
  let page_count = part_first[params.part_id + 1u] - first;

  if (gid.x >= page_count) {
    return;
  }

  let local = gid.x;
  let page  = first + local;

  if (state_at(local, state_cursor) == 1u) {
    return; // already consumed on an earlier frame
  }

  let slot = page_meta[page].slot;

  if (slot == not_resident) {
    atomicOr(&page_request[page >> 5u], 1u << (page & 31u));
    atomicAdd(&status.stalled_rows, 1u);
    atomicAdd(&status.incomplete_rows, 1u);
    return;
  }

  let self_loop = params.direction == direction_cyclic;
  let limit = page_meta[page].count;

  var emit = 0u;

  for (var i = 0u; i < limit; i = i + 1u) {
    let edge = page_pool[slot * page_capacity + i];
    let loops = edge.origin == edge.dest;
    if (loops == self_loop) {
      emit = emit + 1u;
    }
  }

  set_state(local, state_cursor, 1u);

  if (emit == 0u) {
    return;
  }

  var at = write_rows() + atomicAdd(&status.written_rows, emit);

  for (var i = 0u; i < limit; i = i + 1u) {
    let edge = page_pool[slot * page_capacity + i];
    let loops = edge.origin == edge.dest;

    if (loops != self_loop) {
      continue;
    }

    if (at >= params.write_capacity) {
      atomicStore(&status.overflowed, 1u);
      return;
    }

    write_at(params.from_column, at, edge.origin);

    if (!self_loop) {
      write_at(params.write_column, at, edge.dest);
    }

    at = at + 1u;
  }
}

@compute @workgroup_size(1)
fn reset_status(@builtin(global_invocation_id) gid : vec3<u32>) {
  atomicStore(&status.incomplete_rows, 0u);
  atomicStore(&status.stalled_rows, 0u);
  atomicStore(&status.written_rows, 0u);
  atomicStore(&status.candidate_total, 0u);

  // Starting an instruction clears the register it is about to fill.
  if (params.first_frame == 1u) {
    atomicStore(&table_rows[params.write_register], 0u);
  }

  // Sizing the seed/row dispatches reads part_first and table_rows, neither of
  // which the reset above touches except for write_register - and this is a
  // single invocation, so program order already orders it against itself. One
  // kernel, one encoder, instead of two.
  fill_dispatch_args_body();
}
