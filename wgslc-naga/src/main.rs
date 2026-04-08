use anyhow::{Context, Result};
use clap::Parser;
use naga::back::hlsl;
use naga::back::spv;
use naga::front::wgsl;
use naga::valid::{Capabilities, ShaderStages, ValidationFlags, Validator};
use naga::AddressSpace;
use std::fs;
use std::path::PathBuf;

#[derive(Parser, Debug)]
#[command(version, about)]
struct Args {
    input: PathBuf,

    #[arg(short, long)]
    output: PathBuf,

    #[arg(long, help = "Compile to HLSL instead of SPIR-V")]
    hlsl: bool,

    #[arg(
        long,
        help = "HLSL shader model version (default: 6_0)",
        default_value = "6_0"
    )]
    hlsl_shader_model: String,

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

fn parse_shader_model(model_str: &str) -> Result<hlsl::ShaderModel> {
    match model_str {
        "5_0" => Ok(hlsl::ShaderModel::V5_0),
        "5_1" => Ok(hlsl::ShaderModel::V5_1),
        "6_0" => Ok(hlsl::ShaderModel::V6_0),
        "6_1" => Ok(hlsl::ShaderModel::V6_1),
        "6_2" => Ok(hlsl::ShaderModel::V6_2),
        "6_3" => Ok(hlsl::ShaderModel::V6_3),
        "6_4" => Ok(hlsl::ShaderModel::V6_4),
        "6_5" => Ok(hlsl::ShaderModel::V6_5),
        "6_6" => Ok(hlsl::ShaderModel::V6_6),
        "6_7" => Ok(hlsl::ShaderModel::V6_7),
        _ => Err(anyhow::anyhow!("Invalid shader model: {}", model_str)),
    }
}

fn dump_bindings(module: &naga::Module) {
    println!("--- Resource Bindings (WGSL → Vulkan) ---");

    for (_, var) in module.global_variables.iter() {
        let Some(binding) = &var.binding else {
            continue;
        };

        let kind = match var.space {
            AddressSpace::Uniform => "uniform-buffer",
            AddressSpace::Storage { .. } => "storage-buffer",
            AddressSpace::Handle => "texture/sampler",
            _ => continue,
        };

        println!(
            "group={} binding={} kind={}",
            binding.group, binding.binding, kind
        );
    }
}

fn generate_hlsl_binding_map(module: &naga::Module) -> hlsl::BindingMap {
    let mut binding_map = hlsl::BindingMap::default();

    for (_, var) in module.global_variables.iter() {
        let Some(binding) = &var.binding else {
            continue;
        };

        let hlsl_binding = match var.space {
            AddressSpace::Uniform => hlsl::BindTarget {
                space: binding.group as u8,
                register: binding.binding,
                binding_array_size: None,
                dynamic_storage_buffer_offsets_index: None,
                restrict_indexing: false,
            },
            AddressSpace::Storage { .. } => hlsl::BindTarget {
                space: binding.group as u8,
                register: binding.binding,
                binding_array_size: None,
                dynamic_storage_buffer_offsets_index: None,
                restrict_indexing: false,
            },
            AddressSpace::Handle => hlsl::BindTarget {
                space: binding.group as u8,
                register: binding.binding,
                binding_array_size: None,
                dynamic_storage_buffer_offsets_index: None,
                restrict_indexing: false,
            },
            _ => continue,
        };

        binding_map.insert(
            naga::ResourceBinding {
                group: binding.group,
                binding: binding.binding,
            },
            hlsl_binding,
        );
    }

    binding_map
}

fn add_hlsl_metadata(hlsl_code: &str, module: &naga::Module, entry_point_name: &str) -> String {
    let entry_point = module
        .entry_points
        .iter()
        .find(|ep| ep.name == entry_point_name)
        .expect("Entry point not found");

    let workgroup_size = entry_point.workgroup_size;

    let mut output = String::new();

    output.push_str("// Generated from WGSL\n");
    output.push_str(&format!("// Entry point: {}\n", entry_point_name));
    output.push_str(&format!(
        "// Workgroup size: [{}, {}, {}]\n",
        workgroup_size[0], workgroup_size[1], workgroup_size[2]
    ));
    output.push_str("//\n");
    output.push_str("// Resource Bindings:\n");

    for (_, var) in module.global_variables.iter() {
        if let Some(binding) = &var.binding {
            let reg_type = match var.space {
                AddressSpace::Uniform => "cbuffer (b register)",
                AddressSpace::Storage { access } => {
                    if access.contains(naga::StorageAccess::STORE) {
                        "RWStructuredBuffer (u register)"
                    } else {
                        "StructuredBuffer (t register)"
                    }
                }
                AddressSpace::Handle => "texture/sampler (t/s register)",
                _ => continue,
            };

            if let Some(name) = &var.name {
                output.push_str(&format!(
                    "//   {} : space{}, binding{} ({})\n",
                    name, binding.group, binding.binding, reg_type
                ));
            }
        }
    }
    output.push_str("//\n\n");

    output.push_str(hlsl_code);

    let entry_pattern = format!("void {}(", entry_point_name);
    if let Some(pos) = output.rfind(&entry_pattern) {
        // Check if [numthreads] is already present
        let prefix = &output[..pos];
        if !prefix.ends_with("]") && !prefix.contains("[numthreads(") {
            let numthreads = format!(
                "[numthreads({}, {}, {})]\n",
                workgroup_size[0], workgroup_size[1], workgroup_size[2]
            );
            output.insert_str(pos, &numthreads);
        }
    }

    output
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
    allowed_caps |= Capabilities::MULTIVIEW
        | Capabilities::SUBGROUP
        | Capabilities::SUBGROUP_BARRIER
        | Capabilities::CLIP_DISTANCE
        | Capabilities::CULL_DISTANCE;

    let mut validation_flags = ValidationFlags::all();
    validation_flags.remove(ValidationFlags::CONTROL_FLOW_UNIFORMITY);
    let mut validator = Validator::new(validation_flags, allowed_caps);

    validator.subgroup_stages(ShaderStages::COMPUTE | ShaderStages::FRAGMENT);
    validator.subgroup_operations(naga::valid::SubgroupOperationSet::all());

    let module_info = validator
        .validate(&module)
        .map_err(|e| anyhow::anyhow!("Validation failed: {}", e))?;

    if args.hlsl {
        let shader_model = parse_shader_model(&args.hlsl_shader_model)?;

        dump_bindings(&module);

        let binding_map = generate_hlsl_binding_map(&module);

        let hlsl_options = hlsl::Options {
            shader_model,
            binding_map,
            fake_missing_bindings: false,
            special_constants_binding: None,
            zero_initialize_workgroup_memory: true,
            ..Default::default()
        };

        for ep in module.entry_points.iter() {
            if ep.stage != naga::ShaderStage::Compute {
                continue;
            }

            println!("Emitting HLSL entry: {}", ep.name);

            let pipeline_options = hlsl::PipelineOptions {
                entry_point: Some((naga::ShaderStage::Compute, ep.name.clone())),
            };

            let mut hlsl_output = String::new();
            let mut writer = hlsl::Writer::new(&mut hlsl_output, &hlsl_options, &pipeline_options);

            writer
                .write(&module, &module_info, None)
                .with_context(|| format!("Failed to generate HLSL for {}", ep.name))?;

            // Add metadata and fix entry point
            let complete_hlsl = add_hlsl_metadata(&hlsl_output, &module, &ep.name);

            let mut out_path = args.output.clone();
            out_path.set_file_name(format!(
                "{}.{}.hlsl",
                out_path
                    .file_stem()
                    .and_then(|s| s.to_str())
                    .unwrap_or("shader"),
                ep.name
            ));

            fs::write(&out_path, complete_hlsl.as_bytes())?;
            println!("HLSL output: {:?}", out_path);
        }
    } else {
        let spv_options = spv::Options {
            lang_version: (1, 5),
            flags: spv::WriterFlags::DEBUG,
            ..Default::default()
        };

        let spv_words = spv::write_vec(&module, &module_info, &spv_options, None)
            .context("Failed to generate SPIR-V")?;

        fs::write(&args.output, bytemuck::cast_slice(&spv_words))?;
        println!("SPIR-V output: {:?}", args.output);
    }

    Ok(())
}
