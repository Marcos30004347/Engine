const workgroup_size = 256u;
const matches_per_thread = 8u;

struct Params {
 current_table_cols: u32,
 current_table_rows: u32,
 current_table_capacity: u32,
 seed:u32,
 probability: f32,
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


const table_header_size = 3u;

@group(0) @binding(0) var<storage, read_write> params: Params;
@group(0) @binding(1) var<storage, read_write> row_candidates : array<u32>;
@group(0) @binding(2) var<uniform> pattern_edge: PatternEdge;
@group(0) @binding(3) var<storage, read_write> dispatch_buffer: array<u32>;
@group(0) @binding(4) var<storage, read_write> read_table: array<u32>;
@group(0) @binding(4) var<storage, read_write> write_table: array<u32>;

/// @brief Initializes the write-table header with the column count, row count, and overflow flag.
@compute @workgroup_size(1)
fn initiate_table_rows_and_cols(@builtin(global_invocation_id) gid: vec3<u32>) {
	params.current_table_rows = min(pattern_edge.dataEdgesCount,  (params.current_table_capacity - table_header_size) / pattern_edge.columnsCount);
	params.current_table_cols = pattern_edge.columnsCount;

	write_table[0] = params.current_table_cols;
	write_table[1] = params.current_table_rows;
	write_table[2] = select(0u, 1u, pattern_edge.dataEdgesCount * pattern_edge.columnsCount >= params.current_table_capacity);
}

/// @brief Updates the params and write-table header with the new row/column counts after a match pass.
@compute @workgroup_size(1)
fn update_table_rows_and_cols(@builtin(global_invocation_id) gid: vec3<u32>) {
	params.current_table_rows = min(row_candidates[params.current_table_rows], (params.current_table_capacity - table_header_size) / pattern_edge.columnsCount);
	params.current_table_cols = pattern_edge.columnsCount;

	write_table[0] = params.current_table_cols;
	write_table[1] = params.current_table_rows;
	write_table[2] = select(0u, 1u, pattern_edge.dataEdgesCount * pattern_edge.columnsCount >= params.current_table_capacity);
}

/// @brief Computes the ceiling of integer division x / y.
/// @param x The dividend.
/// @param y The divisor.
/// @returns The smallest integer greater than or equal to x / y.
fn div_round_up(x: u32, y : u32) -> u32 {
	return (x + y - 1u) / y;
}

/// @brief Fills the indirect dispatch buffer with the workgroup count needed to process all table rows.
@compute @workgroup_size(1)
fn fill_count_candidates_dispatch_buffer(@builtin(global_invocation_id) gid: vec3<u32>) {
		dispatch_buffer[0] = div_round_up(params.current_table_rows, workgroup_size);
		dispatch_buffer[1] = 1u;
		dispatch_buffer[2] = 1u;
}

/// @brief Fills the indirect dispatch buffer with the workgroup count needed to process all match candidates.
@compute @workgroup_size(1)
fn fill_count_matches_dispatch_buffer(@builtin(global_invocation_id) gid: vec3<u32>) {
	//if(gid.x == 0u) {
		let totalCandidates = row_candidates[params.current_table_rows];
		let threadsCount = div_round_up(totalCandidates, matches_per_thread);
		let workgroupsCount = div_round_up(threadsCount, workgroup_size);
		
		dispatch_buffer[0] = workgroupsCount;
		dispatch_buffer[1] = 1u;
		dispatch_buffer[2] = 1u;
		//}
}

/// @brief Loads the current table column and row counts from the read-table header into params.
@compute @workgroup_size(1)
fn load_params_from_table(@builtin(global_invocation_id) gid: vec3<u32>) {
	params.current_table_cols = read_table[0];
	params.current_table_rows = read_table[1];
}

/// @brief Fills the indirect dispatch buffer with the workgroup count needed for the edge-index lookup pass.
@compute @workgroup_size(1)
fn fill_get_edges_index_dispatch_buffer(@builtin(global_invocation_id) gid: vec3<u32>) {
		dispatch_buffer[0] = div_round_up(params.current_table_rows, workgroup_size);
		dispatch_buffer[1] = 1u;
		dispatch_buffer[2] = 1u;
}


/// @brief Clears the row_candidates array to zero for each thread's element.
@compute @workgroup_size(workgroup_size)
fn zero_row_candidates(@builtin(global_invocation_id) gid: vec3<u32>) {
	row_candidates[gid.x] = 0u;
}

