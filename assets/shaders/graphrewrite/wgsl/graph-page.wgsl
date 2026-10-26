// Page metadata and the directory search, shared by every graph-rewrite kernel.
//
// This mirrors runtime/PartitionDirectory.cpp. Two details are load-bearing:
//
//   - The search returns a RUN of pages, not one page. Fixed-size chunking of a
//     sorted edge list leaves a high-degree vertex straddling a page boundary,
//     so adjacent pages share that key.
//
//   - The bounds are i32. `high = mid - 1` at mid == 0 underflows a u32 and
//     walks off the front of the array.
//
// Every loop breaks unconditionally when its bounds cross: naga rejects a loop
// that can only leave by returning, with "Function is invalid" and no line.

const not_resident = 4294967295u;
const page_capacity = 256u;

// `target` is a RESERVED KEYWORD in WGSL, so the field cannot carry the name the
// C++ Edge uses. The layout is identical - two u32s - only the spelling differs.
struct Edge {
  origin : u32,
  dest   : u32,
};

struct PageMeta {
  part_id   : u32,   // `partition` is also reserved
  min_key   : u32,
  max_key   : u32,
  count     : u32,
  slot      : u32,
  flags     : u32,
};

// first > last means "no page can hold this key".
struct PageRun {
  first : u32,
  last  : u32,
};

// Bindings 1..3 are shared by every kernel that reads pages. Binding 0 is each
// shader's own params, and binding 4 is whatever that shader needs alongside
// (request bits for matching, tombstones for rewriting), so both stay out of
// here.
//
// page_pool is read_write because the rewrite compacts in place; a shader that
// only reads it simply never writes.
@group(0) @binding(1) var<storage, read>       page_meta  : array<PageMeta>;
@group(0) @binding(2) var<storage, read>       part_first : array<u32>;
@group(0) @binding(3) var<storage, read_write> page_pool  : array<Edge>;

fn run_is_empty(run : PageRun) -> bool {
  return run.last < run.first;
}

fn directory_range(part : u32, key : u32) -> PageRun {
  let first_page = i32(part_first[part]);
  let last_page  = i32(part_first[part + 1u]) - 1;

  var result : PageRun;

  if (last_page < first_page) {
    result.first = 1u;
    result.last  = 0u;
    return result;
  }

  // First page with max_key >= key.
  var lo = last_page + 1;
  var a = first_page;
  var b = last_page;

  loop {
    if (a > b) { break; }
    let m = a + (b - a) / 2;
    if (page_meta[m].max_key >= key) { lo = m; b = m - 1; } else { a = m + 1; }
  }

  // Last page with min_key <= key.
  var hi = first_page - 1;
  a = first_page;
  b = last_page;

  loop {
    if (a > b) { break; }
    let m = a + (b - a) / 2;
    if (page_meta[m].min_key <= key) { hi = m; a = m + 1; } else { b = m - 1; }
  }

  if (lo > hi) {
    result.first = 1u;
    result.last  = 0u;
    return result;
  }

  result.first = u32(lo);
  result.last  = u32(hi);
  return result;
}

fn page_is_resident(page : u32) -> bool {
  return page_meta[page].slot != not_resident;
}

fn page_edge(page : u32, index : u32) -> Edge {
  return page_pool[page_meta[page].slot * page_capacity + index];
}
