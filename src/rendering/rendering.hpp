#pragma once

#include "rhi/RHI.hpp"
#include <unordered_map>

using namespace rhi;

namespace rendering {

enum ResourceType {
    ResourceTypeTexture,
    ResourceTypeDepthTexture,
    ResourceTypeStorageBuffer,
    ResourceTypeVertexBuffer,
    ResourceTypeIndirectBuffer,
    ResourceTypeVertexShader,
    ResourceTypeComputeShader,
    ResourceTypeFragmentShader,
};

enum ResourceUse {
    ResourceUseUniform,
    ResourceUseRead,
    ResourceUseWrite,
    ResourceUseReadWrite,
};

enum class ResourceHash : std::uint32_t {};

class Resource {
    public:
    const ResourceHash hash;
    const ResourceType type;
    
    bool operator==(const Resource& other) const noexcept {
        return hash == other.hash && type == other.type && version == other.version;
    }
};

using ResourceContext = std::unordered_map<ResourceHash, const Resource>;

class Pass {
    public:
    const char* passName = "BasePass";

    virtual ResourceContext getOuputResources(Device* device, const ResourceContext& context) = 0;
    virtual ResourceUse getResourceUse(const ResourceHash resource) = 0;
};

class RenderGraph {
    public:
    std::vector<Pass> passes;
    ResourceContext resources;

    void addPass(Device* device, Pass& pass);
};

// Examples
class VirtualGeometryCullPass : Pass {
    public:
    const char* passName = "VirtualGeometryCullPass";

    using VirtualGeometryResourceId = ResourceId;

    // Resources
    constexpr VirtualGeometryResourceId ResourceIdMeshMetadatasBuffer = static_cast<VirtualGeometryResourceId>(0);
    constexpr VirtualGeometryResourceId ResourceIdMeshPagesBuffer = static_cast<VirtualGeometryResourceId>(1);
    constexpr VirtualGeometryResourceId ResourceIdCullShader = static_cast<VirtualGeometryResourceId>(2);

    void initializePass(Device* device) override;
    void executePass(Device* device, Resource* resources) override;

    private:
    void dispatchCullInstancesCommand(ComputePipelineHandle, Resource* resources); 
    void dispatchCullClustersCommand(ComputePipelineHandle, Resource* resources); 
    void dispatchDrawClustersCommand(ComputePipelineHandle, Resource* resources); 
};

};

namespace std {
    template<typename E>
    constexpr auto to_underlying(E e) noexcept {
        return static_cast<underlying_type_t<E>>(e);
    }

    template <>
    struct hash<rendering::Resource> {
        size_t operator()(const rendering::Resource& r) const noexcept {
            size_t h1 = std::hash<uint32_t>{}(to_underlying(r.hash));
            size_t h2 = std::hash<uint32_t>{}(static_cast<uint32_t>(r.type));
            return h1 ^ (h2 << 1);
        }
    };
}