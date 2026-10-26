#include "virtualgeometry/VirtualGeometryFile.hpp"

#include "lz4.h"

#include <climits>
#include <cstring>
#include <iostream>
#include <vector>

#define USE_MINIZ
#ifdef USE_MINIZ
#include "miniz/miniz.h"
#endif

namespace virtualgeometry
{

namespace
{

uint32_t f32_to_u32(float v)
{
  uint32_t u;
  std::memcpy(&u, &v, 4);
  return u;
}

void writeStringRaw(FILE *file, const std::string &value)
{
  const uint32_t length = static_cast<uint32_t>(value.size());
  std::fwrite(&length, sizeof(length), 1u, file);
  if (!value.empty())
    std::fwrite(value.data(), 1u, value.size(), file);
}

} // namespace

bool VirtualGeometryFile::write(const VirtualGeometryEncodedData &data, const std::vector<VirtualGeometryBuildPage> &build_pages, MeshletCompression compression)
{
  if (!file_ || !write_mode_)
    return false;

  std::cout << "Encoding pages...\n";
  std::vector<PageBuffer> page_buffers;
  page_buffers.reserve(data.pages.size());
  for (const auto &p : data.pages)
    page_buffers.push_back(PageBuffer::encode(p));

  uint32_t max_page_size = 0;
  for (const auto &b : page_buffers)
  {
    const uint32_t size_bytes = static_cast<uint32_t>(b.data.size() * sizeof(uint32_t));
    if (size_bytes > max_page_size)
      max_page_size = size_bytes;
  }
  std::cout << "Maximum page size: " << max_page_size << " bytes\n";

  if (data.buildSettings.padPagesToMaxSize)
  {
    for (auto &b : page_buffers)
    {
      const uint32_t current_size = static_cast<uint32_t>(b.data.size() * sizeof(uint32_t));
      if (current_size < max_page_size)
        b.data.resize(b.data.size() + (max_page_size - current_size) / sizeof(uint32_t), 0u);
    }
  }

  VirtualGeometryMetadata hdr{};
  hdr.magic = VMESH_MAGIC;
  hdr.version = VMESH_VERSION;
  hdr.endian_tag = VMESH_ENDIAN_LITTLE;
  hdr.hierarchy_node_count = static_cast<uint32_t>(data.hierarchy.size());
  hdr.page_count = static_cast<uint32_t>(data.pages.size());
  hdr.root_page_index = data.rootPageIndex;
  hdr.shape_count = static_cast<uint32_t>(data.shapes.size());
  hdr.material_count = static_cast<uint32_t>(data.materialFiles.size());
  hdr.skeleton_count = (data.skeleton.empty() && data.meshParts.empty()) ? 0u : 1u;
  hdr.max_page_size = max_page_size;
  hdr.flags = 0u;
  std::memcpy(&hdr.unit_scale_bits, &data.quantizationConfig.unit_scale, sizeof(uint32_t));
  hdr.quantization_factor = data.quantizationConfig.quantization_factor;
  for (const auto &p : data.pages)
    hdr.total_meshlet_count += static_cast<uint32_t>(p.meshlets.size());

  const long header_pos = ftell(file_);
  if (!writeMetadata(hdr))
    return false;

  hdr.hierarchy_offset = ftell(file_);
  for (const auto &n : data.hierarchy)
  {
    uint32_t words[HIERARCHY_WORDS];
    words[0] = f32_to_u32(n.max_x);
    words[1] = f32_to_u32(n.max_y);
    words[2] = f32_to_u32(n.max_z);
    words[3] = f32_to_u32(n.min_x);
    words[4] = f32_to_u32(n.min_y);
    words[5] = f32_to_u32(n.min_z);
    words[6] = f32_to_u32(n.max_center_x);
    words[7] = f32_to_u32(n.max_center_y);
    words[8] = f32_to_u32(n.max_center_z);
    words[9] = f32_to_u32(n.max_radius);
    words[10] = f32_to_u32(n.min_lod_error);
    words[11] = f32_to_u32(n.max_parent_lod_error);
    words[12] = n.child_start;
    words[13] = n.child_count;
    words[14] = n.pageIndex;
    words[15] = n.meshPartIndex;
    words[16] = n.flags;
    fwrite(words, sizeof(uint32_t), HIERARCHY_WORDS, file_);
  }
  hdr.hierarchy_size = data.hierarchy.size() * HIERARCHY_WORDS * sizeof(uint32_t);

  hdr.shape_table_offset = ftell(file_);
  if (!writeShapes(data.shapes))
    return false;
  hdr.shape_table_size = static_cast<uint64_t>(ftell(file_)) - hdr.shape_table_offset;

  hdr.material_table_offset = ftell(file_);
  if (!writeMaterialTable(data.materialFiles))
    return false;
  hdr.material_table_size = static_cast<uint64_t>(ftell(file_)) - hdr.material_table_offset;

  hdr.skeleton_table_offset = ftell(file_);
  if (!writeSkeletonTable(data.skeleton, data.meshParts))
    return false;
  hdr.skeleton_table_size = static_cast<uint64_t>(ftell(file_)) - hdr.skeleton_table_offset;

  hdr.page_table_offset = ftell(file_);
  hdr.page_table_size = data.pages.size() * 7u * sizeof(uint32_t);
  std::vector<VirtualGeometryPageDescriptor> page_table(data.pages.size());
  for (size_t i = 0; i < page_table.size(); ++i)
  {
    VirtualGeometryPageDescriptor dummy{};
    writeU64(file_, dummy.file_offset);
    writeU32(file_, dummy.compressed_size);
    writeU32(file_, dummy.uncompressed_size);
    writeU32(file_, dummy.hierarchy_offset);
    writeU32(file_, dummy.hierarchy_count);
    writeU32(file_, dummy.meshlet_count);
    writeU32(file_, dummy.max_hierarchy_depth);
  }

  std::vector<std::vector<uint32_t>> dependencies;
  dependencies.reserve(build_pages.size());
  for (const auto &page : build_pages)
    dependencies.push_back(page.dependencies);
  hdr.page_dependency_offset = ftell(file_);
  if (!writePageDependencies(dependencies))
    return false;
  hdr.page_dependency_size = static_cast<uint64_t>(ftell(file_)) - hdr.page_dependency_offset;

  hdr.page_install_update_offset = ftell(file_);
  {
    std::vector<PageUpdateList> all_updates;
    all_updates.reserve(build_pages.size());
    for (const auto &page : build_pages)
      all_updates.push_back(page.installUpdates);
    if (!writePageUpdateLists(all_updates))
      return false;
  }
  hdr.page_install_update_size = static_cast<uint64_t>(ftell(file_)) - hdr.page_install_update_offset;

  hdr.page_uninstall_update_offset = ftell(file_);
  {
    std::vector<PageUpdateList> all_updates;
    all_updates.reserve(build_pages.size());
    for (const auto &page : build_pages)
      all_updates.push_back(page.uninstallUpdates);
    if (!writePageUpdateLists(all_updates))
      return false;
  }
  hdr.page_uninstall_update_size = static_cast<uint64_t>(ftell(file_)) - hdr.page_uninstall_update_offset;

  hdr.page_data_offset = ftell(file_);
  std::cout << "Writing " << page_buffers.size() << " pages...\n";
  for (size_t i = 0; i < page_buffers.size(); ++i)
  {
    auto &desc = page_table[i];
    const uint32_t uncompressed_size = static_cast<uint32_t>(page_buffers[i].data.size() * sizeof(uint32_t));

    std::vector<uint8_t> compressed_data;
#ifdef USE_MINIZ
    if (compression == MESHLET_MINIZ)
    {
      mz_ulong max_size = mz_compressBound(uncompressed_size);
      compressed_data.resize(max_size);
      mz_ulong out_size = max_size;
      if (mz_compress(compressed_data.data(), &out_size, reinterpret_cast<const unsigned char *>(page_buffers[i].data.data()), uncompressed_size) != MZ_OK)
      {
        std::cerr << "ERROR: Failed to compress page " << i << "\n";
        return false;
      }
      compressed_data.resize(out_size);
    }
    else
#endif
        if (compression == MESHLET_LZ4)
    {
      if (uncompressed_size > static_cast<uint32_t>(INT_MAX))
      {
        std::cerr << "ERROR: Page " << i << " is too large for LZ4 block compression\n";
        return false;
      }

      const int max_compressed_size = LZ4_compressBound(static_cast<int>(uncompressed_size));
      compressed_data.resize(static_cast<size_t>(max_compressed_size));
      const int compressed_size = LZ4_compress_default(
          reinterpret_cast<const char *>(page_buffers[i].data.data()),
          reinterpret_cast<char *>(compressed_data.data()),
          static_cast<int>(uncompressed_size),
          max_compressed_size);
      if (compressed_size <= 0)
      {
        std::cerr << "ERROR: Failed to LZ4-compress page " << i << "\n";
        return false;
      }
      compressed_data.resize(static_cast<size_t>(compressed_size));
    }
    else
    {
      compressed_data.resize(uncompressed_size);
      std::memcpy(compressed_data.data(), page_buffers[i].data.data(), uncompressed_size);
    }

    desc.file_offset = ftell(file_);
    desc.compressed_size = static_cast<uint32_t>(compressed_data.size());
    desc.uncompressed_size = uncompressed_size;
    desc.meshlet_count = static_cast<uint32_t>(data.pages[i].meshlets.size());
    if (i < build_pages.size())
    {
      desc.hierarchy_offset = build_pages[i].hierarchyOffset;
      desc.hierarchy_count = build_pages[i].hierarchyCount;
      desc.max_hierarchy_depth = build_pages[i].maxHierarchyDepth;
    }

    writeU32(file_, compression);
    writeU32(file_, uncompressed_size);
    fwrite(compressed_data.data(), 1, compressed_data.size(), file_);
    std::cout << "  Page " << i << ": " << uncompressed_size << " bytes (padded), compressed to " << compressed_data.size() << " bytes\n";
  }

  fseek(file_, header_pos, SEEK_SET);
  if (!writeMetadata(hdr))
    return false;

  fseek(file_, hdr.page_table_offset, SEEK_SET);
  for (const auto &desc : page_table)
  {
    writeU64(file_, desc.file_offset);
    writeU32(file_, desc.compressed_size);
    writeU32(file_, desc.uncompressed_size);
    writeU32(file_, desc.hierarchy_offset);
    writeU32(file_, desc.hierarchy_count);
    writeU32(file_, desc.meshlet_count);
    writeU32(file_, desc.max_hierarchy_depth);
  }

  std::cout << "✓ Write complete\n";
  return true;
}

bool VirtualGeometryFile::writeMetadata(const VirtualGeometryMetadata &h)
{
  writeU32(file_, h.magic);
  writeU32(file_, h.version);
  writeU32(file_, h.endian_tag);
  writeU32(file_, h.total_meshlet_count);
  writeU32(file_, h.hierarchy_node_count);
  writeU32(file_, h.page_count);
  writeU32(file_, h.root_page_index);
  writeU32(file_, h.shape_count);
  writeU32(file_, h.material_count);
  writeU32(file_, h.skeleton_count);
  writeU32(file_, h.max_page_size);
  writeU64(file_, h.hierarchy_offset);
  writeU64(file_, h.hierarchy_size);
  writeU64(file_, h.shape_table_offset);
  writeU64(file_, h.shape_table_size);
  writeU64(file_, h.material_table_offset);
  writeU64(file_, h.material_table_size);
  writeU64(file_, h.skeleton_table_offset);
  writeU64(file_, h.skeleton_table_size);
  writeU64(file_, h.page_table_offset);
  writeU64(file_, h.page_table_size);
  writeU64(file_, h.page_dependency_offset);
  writeU64(file_, h.page_dependency_size);
  writeU64(file_, h.page_install_update_offset);
  writeU64(file_, h.page_install_update_size);
  writeU64(file_, h.page_uninstall_update_offset);
  writeU64(file_, h.page_uninstall_update_size);
  writeU64(file_, h.page_data_offset);
  writeU32(file_, h.quantization_factor);
  writeU32(file_, h.unit_scale_bits);
  writeU32(file_, h.flags);
  return true;
}

bool VirtualGeometryFile::writeShapes(const std::vector<VirtualGeometryShapeInfo> &shapes)
{
  for (const auto &shape : shapes)
  {
    writeU32(file_, shape.root_node_index);
    writeU32(file_, shape.root_page_index);
    writeU32(file_, shape.hierarchy_node_count);
    writeU32(file_, shape.materialIndex);
  }
  return true;
}

bool VirtualGeometryFile::writeMaterialTable(const std::vector<std::string> &materialFiles)
{
  for (const std::string &material_file : materialFiles)
    writeStringRaw(file_, material_file);
  return true;
}

bool VirtualGeometryFile::writeSkeletonTable(const rendering::animation::Skeleton &skeleton, const std::vector<MeshPartInfo> &meshParts)
{
  writeU32(file_, static_cast<uint32_t>(meshParts.size()));
  writeU32(file_, skeleton.getBoneCount());

  for (const MeshPartInfo &mesh_part : meshParts)
    writeU32(file_, mesh_part.dominantBoneIndex);

  const math::Mat4f &default_transform = skeleton.getDefaultTransform();
  for (float value : default_transform.data)
    writeF32(file_, value);

  for (const auto &bone : skeleton.getBones())
  {
    writeU32(file_, bone.parentIndex < 0 ? UINT32_MAX : static_cast<uint32_t>(bone.parentIndex));
    writeStringRaw(file_, bone.name);
    for (float value : bone.defaultLocalTransform.data)
      writeF32(file_, value);
    for (float value : bone.inverseBindMatrix.data)
      writeF32(file_, value);
  }

  return true;
}

bool VirtualGeometryFile::writePageDependencies(const std::vector<std::vector<uint32_t>> &dependencies)
{
  for (const auto &dependency_list : dependencies)
  {
    writeU32(file_, static_cast<uint32_t>(dependency_list.size()));
    for (uint32_t dependency : dependency_list)
      writeU32(file_, dependency);
  }
  return true;
}

void VirtualGeometryFile::serializePageUpdateList(FILE *f, const PageUpdateList &list)
{
  writeU32(f, static_cast<uint32_t>(list.hierarchyUpdates.size()));
  for (const auto &update : list.hierarchyUpdates)
  {
    writeU32(f, update.hierarchyNodeIndex);
    const uint32_t masks_packed =
        static_cast<uint32_t>(update.streamingLeafsBitset) |
        (static_cast<uint32_t>(update.enabledClustersBitset) << 8u);
    writeU32(f, masks_packed);
  }
}

bool VirtualGeometryFile::writePageUpdateLists(const std::vector<PageUpdateList> &lists)
{
  for (const auto &list : lists)
    serializePageUpdateList(file_, list);
  return true;
}

} // namespace virtualgeometry
