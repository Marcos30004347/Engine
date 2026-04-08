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

/// @brief Reads a single cell from the read-table at the given row and column.
/// @param row The zero-based row index.
/// @param col The zero-based column index.
/// @returns The value stored at the specified table cell.
fn get_table_entry(row: u32, col: u32) -> u32 {
	return read_table[table_header_size + params.current_table_cols * row + col];
}

/// @brief Computes the ceiling of integer division x / y.
/// @param x The dividend.
/// @param y The divisor.
/// @returns The smallest integer greater than or equal to x / y.
fn div_round_up(x: u32, y : u32) -> u32 {
	return (x + y - 1u) / y;
}

/// @brief Compares an edge's origin vertex with a target vertex for binary search ordering.
/// @param e0 The edge to compare.
/// @param vorigin The vertex origin value to compare against.
/// @returns 1 if e0.vertice_origin > vorigin, -1 if less, 0 if equal.
fn cmp_edge_with_origin(e0 : Edge, vorigin:u32) -> i32 {
	if(e0.vertice_origin > vorigin) {
		return 1;
	}
	if(e0.vertice_origin < vorigin) {
		return -1;
	}
	return 0;
}

/// @brief Compares an edge's target vertex with a given value for binary search ordering.
/// @param e0 The edge to compare.
/// @param vtarget The vertex target value to compare against.
/// @returns 1 if e0.vertice_target > vtarget, -1 if less, 0 if equal.
fn cmp_edge_with_target(e0 : Edge, vtarget:u32) -> i32 {
	if(e0.vertice_target > vtarget) {
		return 1;
	}
	if(e0.vertice_target < vtarget) {
		return -1;
	}
	return 0;
}

/// @brief Finds the first index in the data graph where the edge origin equals the given vertex.
/// @param vertice The origin vertex to search for.
/// @returns The index of the first matching forward edge, or not_found if none exists.
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

/// @brief Compares an edge against an (origin, target) pair for binary search ordering.
/// @param e0 The edge to compare.
/// @param vorigin The origin vertex value to compare against.
/// @param vtarget The target vertex value to compare against.
/// @returns 1 if e0 is greater, -1 if less, 0 if equal.
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

/// @brief Finds the index of the edge with the given origin and target vertices using binary search.
/// @param vertice_origin The origin vertex of the edge to find.
/// @param vertice_target The target vertex of the edge to find.
/// @returns The index of the matching edge in the data graph, or not_found if not present.
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

/// @brief Finds the last index in the data graph where the edge origin equals the given vertex.
/// @param vertice The origin vertex to search for.
/// @returns The index of the last matching forward edge, or not_found if none exists.
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

/// @brief Finds the first index in the data graph where the edge target equals the given vertex.
/// @param vertice The target vertex to search for.
/// @returns The index of the first matching backward edge, or not_found if none exists.
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

/// @brief Finds the last index in the data graph where the edge target equals the given vertex.
/// @param vertice The target vertex to search for.
/// @returns The index of the last matching backward edge, or not_found if none exists.
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

/// @brief Rounds val up to the nearest multiple of x.
/// @param val The value to round up.
/// @param x The multiple to align to.
/// @returns The smallest multiple of x that is >= val.
fn ceil_to_next_multiple(val: u32, x: u32) -> u32 {
    if x == 0u {
        return 0u;
    }
		if val % x == 0u {
        return val;
    }
    return val + (x - (val % x));
}

/// @brief Finds the largest row index whose cumulative candidate count is <= val using binary search.
/// @param size The number of rows to search.
/// @param val The target cumulative candidate count.
/// @returns The largest row index r such that row_candidates_tmp[r] <= val.
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

/// @brief Returns the number of forward edges whose origin matches the given vertex.
/// @param vertice The origin vertex to count edges for.
/// @returns The count of forward edges originating from the vertex.
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

/// @brief Returns the number of backward edges whose target matches the given vertex.
/// @param vertice The target vertex to count edges for.
/// @returns The count of backward edges terminating at the vertex.
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

/// @brief Counts the number of candidate match rows for each table row via forward edges.
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

/// @brief Counts the number of candidate match rows for each table row via backward edges.
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

/// @brief Checks whether a forward or backward edge can extend the current row without duplicate vertices.
/// @param d_origin The origin vertex of the candidate edge.
/// @param d_target The target vertex of the candidate edge.
/// @param d_current The vertex from the table column used to look up the edge.
/// @returns 1 if the edge extends the match uniquely, 0 if it would introduce a duplicate.
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

/// @brief Checks whether an edge matches a cyclic pattern by verifying both endpoints exist in the row.
/// @param d_origin The origin vertex of the candidate edge.
/// @param d_target The target vertex of the candidate edge.
/// @param d_current The vertex from the table column used to look up the edge.
/// @returns 1 if both endpoints are found in the relevant columns, 0 otherwise.
fn check_row_cyclic_edge(d_origin: u32, d_target: u32, d_current: u32) -> u32 {		
	let found_target = private_row[pattern_edge.fromColumn] == d_target || private_row[pattern_edge.writeColumn] == d_target;
	let found_origin = private_row[pattern_edge.fromColumn] == d_origin || private_row[pattern_edge.writeColumn] == d_origin;
	return select(0u, 1u, found_target && found_origin);
}

/// @brief Copies a table row into the thread-private row buffer.
/// @param row The zero-based row index to load from the read-table.
fn load_row(row: u32) {
	for(var i = 0u; i < params.current_table_cols; i++) {
		private_row[i] = get_table_entry(row, i);
	}
}

/// @brief Loads up to matches_per_thread backward edges starting at the given offset into the private edge buffer.
/// @param offset Starting index in the data graph array.
/// @param table_vertice The expected target vertex; loading stops if an edge doesn't match.
/// @returns The number of edges actually loaded.
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

/// @brief Loads up to matches_per_thread forward edges starting at the given offset into the private edge buffer.
/// @param offset Starting index in the data graph array.
/// @param table_vertice The expected origin vertex; loading stops if an edge doesn't match.
/// @returns The number of edges actually loaded.
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

/// @brief Counts valid forward-edge matches for each thread's candidate block and accumulates into row_candidates.
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

/// @brief Counts valid cyclic-edge matches for each thread's candidate block and accumulates into row_candidates.
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


/// @brief Counts valid backward-edge matches for each thread's candidate block and accumulates into row_candidates.
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


/// @brief Writes matched forward-edge rows into the write-table, appending the new target vertex column.
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

/// @brief Writes matched cyclic-edge rows into the write-table without adding a new column.
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

/// @brief Writes matched backward-edge rows into the write-table, appending the origin vertex column.
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

/// @brief Hashes a u32 seed to a float in [0, 1) using a multiply-xorshift mix.
/// @param seed The input seed value.
/// @returns A pseudo-random float in [0, 1).
fn hash_to_float(seed: u32) -> f32 {
    var x = seed;
    x ^= x >> 16u;
    x *= 0x85ebca6bu;
    x ^= x >> 13u;
    x *= 0xc2b2ae35u;
    x ^= x >> 16u;
    return f32(x & 0x007FFFFFu) / f32(0x007FFFFFu);
}

/// @brief Looks up the data-graph edge index for each matched row and stores it with probability sampling.
@compute @workgroup_size(workgroup_size)
fn get_matched_edges_index(@builtin(global_invocation_id) gid: vec3<u32>) {
	let row = gid.x;
	
	var vertice_origin = select(pattern_edge.fromColumn, pattern_edge.writeColumn, pattern_edge.direction > backwardEdgeValue);
	var vertice_target = select(pattern_edge.writeColumn, pattern_edge.fromColumn, pattern_edge.direction > backwardEdgeValue);

	let index = get_edge_index(vertice_origin, vertice_target);

	workgroupBarrier();
	
	row_candidates_tmp[row] = select(not_found, index,  hash_to_float(params.seed + gid.x) < params.probability);
}
