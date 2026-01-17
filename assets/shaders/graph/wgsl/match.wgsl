const workgroup_size = 256u;
const not_found = 4294967295u;
const matches_per_thread = 8u;
const max_table_cols = 32u;

const forwardEdgeValue = 1u;
const backwardEdgeValue = 0u;
const cyclicEdgeValue = 2u;

struct Edge {
 vertice_origin: u32,
 vertice_target: u32,
};

struct PatternEdge {
 vertice_origin: u32,
 edge_label: u32,
 vertice_target: u32,
 direction: u32,
 fromColumn: u32,
 writeColumn: u32,
 columnsCount: u32,
 dataEdgesCount: u32,
};

struct Params {
 current_table_cols: u32,
 current_table_rows: u32,
 current_table_capacity: u32,
 seed: u32,
 probability: f32,
};

// TODO: should be uniforms
@group(0) @binding(0) var<uniform> params: Params;

@group(0) @binding(1) var<storage, read_write> read_table: array<u32>;
@group(0) @binding(2) var<storage, read_write> write_table: array<u32>;

// NOTE: can be just one binding but doubling the array size
@group(0) @binding(3) var<storage, read> data_graph: array<Edge>;

@group(0) @binding(4) var<uniform> pattern_edge: PatternEdge;
@group(0) @binding(5) var<storage, read_write> row_candidates : array<atomic<u32>>;
@group(0) @binding(6) var<storage, read_write> row_candidates_tmp : array<u32>;


var<private> private_row : array<u32, max_table_cols>;
var<private> private_edges : array<Edge, matches_per_thread>;

const table_header_size = 3u;

fn get_table_entry(row: u32, col: u32) -> u32 {
	return read_table[table_header_size + params.current_table_cols * row + col];
}

fn div_round_up(x: u32, y : u32) -> u32 {
	return (x + y - 1u) / y;
}


fn cmp_edge_with_origin(e0 : Edge, vorigin:u32) -> i32 {
	if(e0.vertice_origin > vorigin) {
		return 1;
	}
	if(e0.vertice_origin < vorigin) {
		return -1;
	}
	return 0;
}

fn cmp_edge_with_target(e0 : Edge, vtarget:u32) -> i32 {
	if(e0.vertice_target > vtarget) {
		return 1;
	}
	if(e0.vertice_target < vtarget) {
		return -1;
	}
	return 0;
}

fn get_fwd_edges_first_offset(vertice: u32) -> u32 {
	let s = i32(pattern_edge.dataEdgesCount);// i32(params.graph_edges_count);

	var l = 0;
	var r = s - 1;
	var i = -1;
	
	while(l <= r) {
		let m = l + (r - l) / 2;
		let c = cmp_edge_with_origin(data_graph[m], vertice);
		if c == 0 {
			i = m;
			r = m - 1;
		}
		else if(c < 0) {
			l = m + 1;
		}
		else {
			r = m - 1;
		}
	}
	
	return u32(i);
}

fn cmp_edge_with_origin_target(e0: Edge, vorigin: u32, vtarget: u32) -> i32 {
	if e0.vertice_origin > vorigin {
		return 1;
  } else if e0.vertice_origin < vorigin {
		return -1;
   } else {
		if e0.vertice_target > vtarget {
			return 1;
    } else if e0.vertice_target < vtarget {
			return -1;
    }
		return 0;
	}
}

fn get_edge_index(vertice_origin: u32, vertice_target: u32) -> u32 {
	var l = 0;
	var r = i32(pattern_edge.dataEdgesCount) - 1;

  while l <= r {
		let m = l + (r - l) / 2;
		let c = cmp_edge_with_origin_target(data_graph[m], vertice_origin, vertice_target);

		if c == 0 {
			return u32(m);
		} else if c < 0 {
			l = m + 1;
		} else {
			r = m - 1;
		}
  }

  return not_found;
}

fn get_fwd_edges_last_offset(vertice: u32) -> u32 {
	let s = i32(pattern_edge.dataEdgesCount);// i32(params.graph_edges_count);

	var l = 0;
	var r = s - 1;
	var i = -1;
	
	while(l <= r) {
		let m = l + (r - l) / 2;
		let c = cmp_edge_with_origin(data_graph[m], vertice);
		if c == 0 {
				i = m;
				l = m + 1;
		}
		else if(c < 0) {
			l = m + 1;
		}
		else {
			r = m - 1;
		}
	}
	
	return u32(i);
}

fn get_bwd_edges_first_offset(vertice: u32) -> u32 {
	let s = i32(pattern_edge.dataEdgesCount);// i32(params.graph_edges_count);

	var l = 0;
	var r = s - 1;

	var i = -1;
	
	while(l <= r) {
		let m = l + (r - l) / 2;
		let c = cmp_edge_with_target(data_graph[m], vertice);
		if c == 0 {
				i = m;
				r = m - 1;
		}
		else if(c < 0) {
			l = m + 1;
		}
		else {
			r = m - 1;
		}
	}
	
	return u32(i);
}

fn get_bwd_edges_last_offset(vertice: u32) -> u32 {
	let s = i32(pattern_edge.dataEdgesCount);// i32(params.graph_edges_count);

	var l = 0;
	var r = s - 1;

	var i = -1;
	
	while(l <= r) {
		let m = l + (r - l) / 2;
		let c = cmp_edge_with_target(data_graph[m], vertice);
		if c == 0 {
				i = m;
				l = m + 1;
		}
		else if(c < 0) {
			l = m + 1;
		}
		else {
			r = m - 1;
		}
	}
	
	return u32(i);
}

fn ceil_to_next_multiple(val: u32, x: u32) -> u32 {
    if x == 0u {
        return 0u;
    }
		if val % x == 0u {
        return val;
    }
    return val + (x - (val % x));
}

fn find_row(size: u32, val: u32) -> u32 {
	var l = 0;
	var r = i32(size) - 1;
	var i = -1;//not_found;

	while(l <= r) {
		let m = l + (r - l) / 2;
		// we use new table to store the result of the prefix sum 
		if row_candidates_tmp[m] <= val {
			i = m;
			l = m + 1;
		} else {
			r = m - 1;
		}
	}

	return u32(i);
}

fn get_fwd_edges_count(vertice: u32) -> u32 {
	 let start = get_fwd_edges_first_offset(vertice);

	 if start == not_found {
		 return 0u;
	 }

	 let end = get_fwd_edges_last_offset(vertice);

	 if end == not_found {
		 return 0u;
	 }
 
	 return end - start + 1u;
}

fn get_bwd_edges_count(vertice: u32) -> u32 {
	 let start = get_bwd_edges_first_offset(vertice);
	 
	 if(start == not_found) {
		 return 0u;
	 }
	 
	 let end = get_bwd_edges_last_offset(vertice);

	 if(end == not_found) {
		 return 0u;
	 }
	 
	 return end - start + 1u;
}

@compute @workgroup_size(workgroup_size)
fn count_candidates_forward_edge(@builtin(global_invocation_id) gid: vec3<u32>) {
	// TODO: Move to utils and call a another dispatch
	if gid.x >= params.current_table_rows {
		return;
	}
	
	let row_index = gid.x;
	let p_col = pattern_edge.fromColumn;
	let d_vertice = get_table_entry(row_index, p_col);
	let count = get_fwd_edges_count(d_vertice);

	atomicStore(&row_candidates[row_index], ceil_to_next_multiple(count, matches_per_thread));
}

@compute @workgroup_size(workgroup_size)
fn count_candidates_backward_edge(@builtin(global_invocation_id) gid: vec3<u32>) {
	// TODO: Move to utils and call a another dispatch
	if gid.x >= params.current_table_rows {
		return;
	}
	
	let row_index = gid.x;
	let p_col = pattern_edge.fromColumn;
	let d_vertice = get_table_entry(row_index, p_col);
	let count = get_bwd_edges_count(d_vertice);
	atomicStore(&row_candidates[row_index], ceil_to_next_multiple(count, matches_per_thread));
}

fn check_row_forward_and_backward_edge(d_origin: u32, d_target: u32, d_current: u32) -> u32 {		
	let d_other = select(d_origin, d_target, d_current == d_origin);
	var result = 1u;
	
	for(var col = 0u; col < params.current_table_cols; col++) {
		if(d_other == private_row[col]) {
			result = 0u;
		}
	}
	
	return result;
}

fn check_row_cyclic_edge(d_origin: u32, d_target: u32, d_current: u32) -> u32 {		
	let found_target = private_row[pattern_edge.fromColumn] == d_target || private_row[pattern_edge.writeColumn] == d_target;
	let found_origin = private_row[pattern_edge.fromColumn] == d_origin || private_row[pattern_edge.writeColumn] == d_origin;
	return select(0u, 1u, found_target && found_origin);
}

fn load_row(row: u32) {
	for(var i = 0u; i < params.current_table_cols; i++) {
		private_row[i] = get_table_entry(row, i);
	}
}

fn load_edges_backward(offset: u32, table_vertice: u32) -> u32 {
	var count = 0u;

	for(var i = 0u; i < matches_per_thread; i++) {
		if offset + i >= pattern_edge.dataEdgesCount {
			break;
		}

		var edge = data_graph[offset + i];

		let invalid_vertices = edge.vertice_target != table_vertice;

		if invalid_vertices {
			break;
		}

		count += 1u;

		private_edges[i] = edge;
	}
	
	return count;
}

fn load_edges_forward(offset: u32, table_vertice: u32) -> u32 {
	var count = 0u;
	
	for(var i = 0u; i < matches_per_thread; i++) {
		if offset + i >= pattern_edge.dataEdgesCount {
			break;
		}
		
		var edge = data_graph[offset + i];

		let invalid_vertices = edge.vertice_origin != table_vertice;

		if invalid_vertices {
			break;
		}

		count += 1u;

		private_edges[i] = edge;
	}
	
	return count;
}

@compute @workgroup_size(workgroup_size)
fn table_count_matches_forward_edge(@builtin(global_invocation_id) gid: vec3<u32>) {
	let p_col = pattern_edge.fromColumn;
	let skip_candidates = matches_per_thread * gid.x;
	
	var row = find_row(params.current_table_rows, skip_candidates);

	workgroupBarrier();

	load_row(row);

	let p_origin = pattern_edge.vertice_origin;
	let p_target = pattern_edge.vertice_target;

	let d_vertice = get_table_entry(row, p_col);

	let offset = get_fwd_edges_first_offset(d_vertice) + skip_candidates - row_candidates_tmp[row];

	workgroupBarrier();

	let count = load_edges_forward(offset, d_vertice);
	
	workgroupBarrier();
	
	var matches = 0u;	

	for(var i = 0u; i < count; i++) {
		let d_origin = private_edges[i].vertice_origin;
		let d_target = private_edges[i].vertice_target;
		
		matches += check_row_forward_and_backward_edge(d_origin, d_target, d_vertice);
	}
	

	atomicAdd(&row_candidates[row], matches);
}

@compute @workgroup_size(workgroup_size)
fn table_count_matches_cyclic_edge(@builtin(global_invocation_id) gid: vec3<u32>) {
	let p_col = pattern_edge.fromColumn;
	let skip_candidates = matches_per_thread * gid.x;
	
	var row = find_row(params.current_table_rows, skip_candidates);

	workgroupBarrier();

	load_row(row);

	let p_origin = pattern_edge.vertice_origin;
	let p_target = pattern_edge.vertice_target;

	let d_vertice = get_table_entry(row, p_col);

	let offset = get_fwd_edges_first_offset(d_vertice) + skip_candidates - row_candidates_tmp[row];

	workgroupBarrier();

	let count = load_edges_forward(offset, d_vertice);

	workgroupBarrier();

	var matches = 0u;	

	for(var i = 0u; i < count; i++) {
		let d_origin = private_edges[i].vertice_origin;
		let d_target = private_edges[i].vertice_target;
		
		matches += check_row_cyclic_edge(d_origin, d_target, d_vertice);
	}
	
	atomicAdd(&row_candidates[row], matches);
}


@compute @workgroup_size(workgroup_size)
fn table_count_matches_backward_edge(@builtin(global_invocation_id) gid: vec3<u32>) {
	let p_col = pattern_edge.fromColumn;
	let skip_candidates = matches_per_thread * gid.x;
	
	var row = find_row(params.current_table_rows, skip_candidates);

	workgroupBarrier();
	
	load_row(row);

	let p_origin = pattern_edge.vertice_origin;
	let p_target = pattern_edge.vertice_target;

	let d_vertice = get_table_entry(row, p_col);

	let offset = get_bwd_edges_first_offset(d_vertice) + skip_candidates - row_candidates_tmp[row];

	workgroupBarrier();

	let count = load_edges_backward(offset, d_vertice);

	workgroupBarrier();

	var matches = 0u;	

	for(var i = 0u; i < count; i++) {
		if offset + i >= pattern_edge.dataEdgesCount {
			break;
		}
		
		let d_origin = private_edges[i].vertice_origin;
		let d_target = private_edges[i].vertice_target;
		
		matches += check_row_forward_and_backward_edge(d_origin, d_target, d_vertice);
	}

	atomicAdd(&row_candidates[row], matches);
}


@compute @workgroup_size(workgroup_size)
fn table_write_matches_forward_edge(@builtin(global_invocation_id) gid: vec3<u32>) {
	let skip_candidates = matches_per_thread * gid.x;
	let p_col = pattern_edge.fromColumn;

	var row = find_row(params.current_table_rows, skip_candidates);

	workgroupBarrier();

	load_row(row);
	
	let p_origin = pattern_edge.vertice_origin;
	let p_target = pattern_edge.vertice_target;
	let write_col = pattern_edge.writeColumn;
	
	let d_vertice = get_table_entry(row, p_col);

	let offset = get_fwd_edges_first_offset(d_vertice) + skip_candidates - row_candidates_tmp[row];
	let max_rows = (params.current_table_capacity - table_header_size) / pattern_edge.columnsCount;

	workgroupBarrier();

	let count = load_edges_forward(offset, d_vertice);

	workgroupBarrier();

	for(var i = 0u; i < count; i++) {
		let d_origin = private_edges[i].vertice_origin;
		let d_target = private_edges[i].vertice_target;
		
		let is_match = check_row_forward_and_backward_edge(d_origin, d_target, d_vertice);

		if is_match == 1u {
			let row_index = atomicAdd(&row_candidates[row], 1u);
			let row_addr  = row_index * pattern_edge.columnsCount;

			if row_addr >= max_rows {
				return;
			}

			for(var i = 0u; i < params.current_table_cols; i++) {
				write_table[table_header_size + row_addr + i] = private_row[i];
			}
			
			write_table[table_header_size + row_addr + write_col] = d_target;
		}
	}
}

@compute @workgroup_size(workgroup_size)
fn table_write_matches_cyclic_edge(@builtin(global_invocation_id) gid: vec3<u32>) {
	let skip_candidates = matches_per_thread * gid.x;
	let p_col = pattern_edge.fromColumn;

	var row = find_row(params.current_table_rows, skip_candidates);

	workgroupBarrier();

	load_row(row);
	
	let p_origin = pattern_edge.vertice_origin;
	let p_target = pattern_edge.vertice_target;
	let write_col = pattern_edge.writeColumn;
	
	let d_vertice = get_table_entry(row, p_col);

	let offset = get_fwd_edges_first_offset(d_vertice) + skip_candidates - row_candidates_tmp[row];

	workgroupBarrier();

	let count = load_edges_forward(offset, d_vertice);
	
	workgroupBarrier();

	for(var i = 0u; i < count; i++) {
		let d_origin = private_edges[i].vertice_origin;
		let d_target = private_edges[i].vertice_target;
		
		let is_match = check_row_cyclic_edge(d_origin, d_target, d_vertice);

		if is_match == 1u {
			let row_index = atomicAdd(&row_candidates[row], 1u);
			let row_addr  = row_index * pattern_edge.columnsCount;
			

			for(var i = 0u; i < params.current_table_cols; i++) {
				write_table[table_header_size + row_addr + i] = private_row[i];
			}	
		}
	}
}

@compute @workgroup_size(workgroup_size)
fn table_write_matches_backward_edge(@builtin(global_invocation_id) gid: vec3<u32>) {
	let skip_candidates = matches_per_thread * gid.x;
	let p_col = pattern_edge.fromColumn;

	var row = find_row(params.current_table_rows, skip_candidates);

	workgroupBarrier();

	load_row(row);
	
	let p_origin = pattern_edge.vertice_origin;
	let p_target = pattern_edge.vertice_target;
	let write_col = pattern_edge.writeColumn;
	let d_vertice = get_table_entry(row, p_col);

	let	offset = get_bwd_edges_first_offset(d_vertice) + skip_candidates - row_candidates_tmp[row];
	let max_rows = (params.current_table_capacity - table_header_size) / pattern_edge.columnsCount;
	
	workgroupBarrier();

	let count = load_edges_backward(offset, d_vertice);
	
	workgroupBarrier();

	for(var i = 0u; i < count; i++) {
		let d_origin = private_edges[i].vertice_origin;
		let d_target = private_edges[i].vertice_target;
		
		let is_match = check_row_forward_and_backward_edge(d_origin, d_target, d_vertice);

		if is_match == 1u {
			let row_index = atomicAdd(&row_candidates[row], 1u);
			let row_addr  = row_index * pattern_edge.columnsCount;

			if row_addr >= max_rows {
				return;
			}
			
			for(var i = 0u; i < params.current_table_cols; i++) {
				write_table[table_header_size + row_addr + i] = private_row[i];
			}
			
			write_table[table_header_size + row_addr + write_col] = d_origin;
		}
	}
}

fn hash_to_float(seed: u32) -> f32 {
    var x = seed;
    x ^= x >> 16u;
    x *= 0x85ebca6bu;
    x ^= x >> 13u;
    x *= 0xc2b2ae35u;
    x ^= x >> 16u;
    return f32(x & 0x007FFFFFu) / f32(0x007FFFFFu);
}

@compute @workgroup_size(workgroup_size)
fn get_matched_edges_index(@builtin(global_invocation_id) gid: vec3<u32>) {
	let row = gid.x;
	
	var vertice_origin = select(pattern_edge.fromColumn, pattern_edge.writeColumn, pattern_edge.direction > backwardEdgeValue);
	var vertice_target = select(pattern_edge.writeColumn, pattern_edge.fromColumn, pattern_edge.direction > backwardEdgeValue);

	let index = get_edge_index(vertice_origin, vertice_target);

	workgroupBarrier();
	
	row_candidates_tmp[row] = select(not_found, index,  hash_to_float(params.seed + gid.x) < params.probability);
}
