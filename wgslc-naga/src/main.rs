use anyhow::{Context, Result};
use clap::Parser;
use naga::back::spv;
use naga::front::wgsl;
use naga::valid::{Capabilities, ValidationFlags, Validator};
use std::fs;
use std::path::PathBuf;

#[derive(Parser, Debug)]
#[command(version, about, long_about = None)]
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
    // Validate
    let mut validator = Validator::new(ValidationFlags::all(), allowed_caps);
    let module_info = validator
        .validate(&module)
        .map_err(|e| anyhow::anyhow!("Validation failed: {}", e))?;

    // Generate SPIR-V
    let options = spv::Options {
        lang_version: (1, 3),           // Target Vulkan 1.1+ (1.0 is (1,0))
        flags: spv::WriterFlags::DEBUG, // Include debug names
        ..Default::default()
    };

    let spv_data = spv::write_vec(&module, &module_info, &options, None)
        .context("Failed to generate SPIR-V")?;

    let bytes = bytemuck::cast_slice(&spv_data);
    fs::write(&args.output, bytes)
        .with_context(|| format!("Failed to write output file: {:?}", args.output))?;

    Ok(())
}
