use anyhow::{Context, Result};
use clap::Parser;
use naga::back::spv;
use naga::front::wgsl;
use naga::valid::{Capabilities, ShaderStages, ValidationFlags, Validator};
// Import the Optimizer trait to make its methods available
use spirv_tools::opt::{create, Optimizer};
use spirv_tools::TargetEnv;
use std::fs;
use std::path::PathBuf;

#[derive(Parser, Debug)]
#[command(version, about)]
struct Args {
    input: PathBuf,

    #[arg(short, long)]
    output: PathBuf,

    #[arg(long)]
    int64: bool,

    #[arg(long)]
    atomic_u64: bool,

    #[arg(long)]
    atomic_u64_min_max: bool,

    #[arg(long)]
    float64: bool,

    #[arg(long)]
    texture_int64_atomic: bool,
}

fn main() -> Result<()> {
    let args = Args::parse();

    let wgsl_src = fs::read_to_string(&args.input)
        .with_context(|| format!("Failed to read input file: {:?}", args.input))?;

    let module = wgsl::parse_str(&wgsl_src).map_err(|e| {
        let err_msg = e.emit_to_string(&wgsl_src);
        eprintln!("{}", err_msg);
        anyhow::anyhow!("WGSL parsing failed")
    })?;

    let mut allowed_caps = Capabilities::empty();
    allowed_caps |= Capabilities::MULTIVIEW
        | Capabilities::SUBGROUP
        | Capabilities::SUBGROUP_BARRIER
        | Capabilities::CLIP_DISTANCE
        | Capabilities::CULL_DISTANCE;

    if args.int64 {
        allowed_caps |= Capabilities::SHADER_INT64;
    }
    if args.atomic_u64 {
        allowed_caps |= Capabilities::SHADER_INT64_ATOMIC_ALL_OPS;
    }
    if args.atomic_u64_min_max {
        allowed_caps |= Capabilities::SHADER_INT64_ATOMIC_MIN_MAX;
    }
    if args.float64 {
        allowed_caps |= Capabilities::FLOAT64;
    }
    if args.texture_int64_atomic {
        allowed_caps |= Capabilities::TEXTURE_INT64_ATOMIC;
    }

    let mut validation_flags = ValidationFlags::all();
    validation_flags.remove(ValidationFlags::CONTROL_FLOW_UNIFORMITY);
    let mut validator = Validator::new(validation_flags, allowed_caps);

    // let mut validator = Validator::new(ValidationFlags::all(), allowed_caps);
    validator.subgroup_stages(ShaderStages::COMPUTE);
    validator.subgroup_operations(naga::valid::SubgroupOperationSet::all());

    let module_info = validator
        .validate(&module)
        .map_err(|e| anyhow::anyhow!("Validation failed: {}", e))?;

    // --- Generate Debug SPIR-V ---
    let spv_options = spv::Options {
        lang_version: (1, 0), // Vulkan SPIR-V baseline
        flags: spv::WriterFlags::DEBUG
    };

    let spv_words_debug = spv::write_vec(&module, &module_info, &spv_options, None)
        .context("Failed to generate SPIR-V")?;

    let mut debug_path = args.output.clone();
    if let Some(filename) = debug_path.file_name() {
        let mut new_name = filename.to_os_string();
        new_name.push(".debug");
        debug_path.set_file_name(new_name);
    }

    fs::write(&debug_path, bytemuck::cast_slice(&spv_words_debug))?;
    println!("Debug SPIR-V: {:?}", debug_path);

    // --- Optimize SPIR-V ---
    let mut optimizer = create(Some(TargetEnv::Vulkan_1_2));

    optimizer.register_performance_passes();

    // Add aggressive optimization passes for better performance
    use spirv_tools::opt::Passes;

    // Loop optimizations - unroll loops and optimize control flow
    optimizer
        .register_pass(Passes::LoopPeeling) // Peel loop iterations for optimization
        .register_pass(Passes::LoopUnswitch) // Move loop-invariant branches outside loops
        .register_pass(Passes::LoopInvariantCodeMotion) // Move loop-invariant code out
        // Control flow optimizations
        .register_pass(Passes::IfConversion) // Convert branches to selects when beneficial
        .register_pass(Passes::Simplification) // Algebraic simplification
        .register_pass(Passes::CombineAccessChains) // Merge access chains
        .register_pass(Passes::ConvertRelaxedToHalf) // Use half precision where possible
        // Additional aggressive optimizations
        .register_pass(Passes::DeadBranchElim) // Remove dead branches
        .register_pass(Passes::MergeReturn) // Merge return blocks
        .register_pass(Passes::InlineExhaustive) // Aggressive inlining
        .register_pass(Passes::LocalAccessChainConvert) // Convert access chains to extracts
        .register_pass(Passes::StrengthReduction) // Replace expensive ops with cheaper ones
        .register_pass(Passes::CodeSinking) // Move code closer to use points
        .register_pass(Passes::AggressiveDCE); // Aggressive dead code elimination

    // Fix: Change closure signature to accept Message by value and use Debug formatting
    let spv_optimized = match optimizer.optimize(
        &spv_words_debug,
        &mut |msg| eprintln!("[optimizer] {:?}", msg),
        None,
    ) {
        Ok(binary) => binary.as_words().to_vec(),
        Err(e) => {
            eprintln!("Optimization failed: {:?}. Using unoptimized.", e);
            spv_words_debug
        }
    };

    fs::write(&args.output, bytemuck::cast_slice(&spv_optimized))?;
    println!("Optimized SPIR-V: {:?}", args.output);

    Ok(())
}
