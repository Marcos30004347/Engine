struct Params {
 n: u32,
}

struct Edge {
 vertice_origin: u32,
 vertice_target: u32,
};

const workgroup_size = 256u;

@group(0) @binding(0) var<uniform> params: Params;
@group(0) @binding(1) var<storage, read_write> edges: array<Edge>;
@group(0) @binding(2) var<storage, read_write> edgesResult: array<Edge>;
@group(0) @binding(3) var<storage, read_write> permutation: array<u32>;
@group(0) @binding(4) var<storage, read_write> keys: array<u32>;

@compute @workgroup_size(workgroup_size)
fn apply_permutation_in_edges(@builtin(global_invocation_id) gid: vec3<u32>) {
	if(gid.x < params.n) {
		let fromIndex = permutation[gid.x];
		edgesResult[gid.x] = edges[fromIndex];
	}
}

@compute @workgroup_size(workgroup_size)
fn load_origins(@builtin(global_invocation_id) gid: vec3<u32>) {
	if(gid.x < params.n) {
		keys[gid.x] = edges[gid.x].vertice_origin;
		permutation[gid.x] = gid.x;
	}
}

@compute @workgroup_size(workgroup_size)
fn load_targets(@builtin(global_invocation_id) gid: vec3<u32>) {
	if(gid.x < params.n) {
		permutation[gid.x] = gid.x;
		keys[gid.x] = edges[gid.x].vertice_target;
	}
}

