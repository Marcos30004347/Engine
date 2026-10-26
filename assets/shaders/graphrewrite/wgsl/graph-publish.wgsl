// Publishing a completed pattern.
//
// When a pattern's last instruction finishes, its table is copied into that
// pattern's own result buffer and a record is written into a small shared status
// buffer the CPU reads back. `generation` exists because the readback is a frame
// or two behind: without it, a status from an earlier step is indistinguishable
// from this one's.

const state_running    = 0u;
const state_complete   = 1u;
const state_overflowed = 2u;

// `enabled` exists because a pass records its command buffer ONCE, at
// registration: the dispatches are fixed, and which of them actually do
// anything is decided per frame by what the CPU writes here.
struct PublishParams {
  pattern_id    : u32,
  rows          : u32,
  cols          : u32,
  generation    : u32,
  result_offset : u32,
  overflowed    : u32,
  enabled       : u32,
  _pad0         : u32,
};

struct PatternStatus {
  state         : u32,
  rows          : u32,
  cols          : u32,
  generation    : u32,
  result_offset : u32,
  _pad0         : u32,
  _pad1         : u32,
  _pad2         : u32,
};

@group(0) @binding(0) var<uniform>             publish_params : PublishParams;
@group(0) @binding(1) var<storage, read>       source_table   : array<u32>;
@group(0) @binding(2) var<storage, read_write> pattern_result : array<u32>;
@group(0) @binding(3) var<storage, read_write> pattern_status : array<PatternStatus>;

@compute @workgroup_size(256)
fn publish(@builtin(global_invocation_id) gid : vec3<u32>) {
  if (publish_params.enabled == 0u) {
    return;
  }

  let total = publish_params.rows * publish_params.cols;

  // The status record is written by exactly one invocation, and written even
  // when the table is empty - a pattern that matched nothing still has to say
  // so, or the CPU cannot tell "no matches" from "not finished".
  if (gid.x == 0u) {
    let id = publish_params.pattern_id;

    pattern_status[id].state = select(state_complete, state_overflowed, publish_params.overflowed != 0u);
    pattern_status[id].rows = publish_params.rows;
    pattern_status[id].cols = publish_params.cols;
    pattern_status[id].generation = publish_params.generation;
    pattern_status[id].result_offset = publish_params.result_offset;
  }

  if (gid.x >= total) {
    return;
  }

  // The source is already packed, so this is a straight copy.
  pattern_result[publish_params.result_offset + gid.x] = source_table[gid.x];
}
