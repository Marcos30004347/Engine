#pragma once

#include "virtualgeometry/VirtualGeometryScene.hpp"

#include <array>
#include <cstdint>
#include <queue>
#include <string>
#include <unordered_map>
#include <vector>

namespace virtualgeometry
{

// VirtualGeometryGeneratedPayloadManager is the CPU-side control plane for
// pages whose payload bytes/decoded geometry are produced by GPU passes.
//
// It intentionally does not build geometry.  Its job is small metadata work:
// request dedupe, dependency-aware reservation, exposing write descriptors to a
// producer pass, and publishing/cancelling the final scene install record.
class VirtualGeometryGeneratedPayloadManager
{
public:
  using TicketId = uint64_t;
  static constexpr TicketId INVALID_TICKET_ID = 0u;

  struct Request
  {
    std::string objectName;
    uint32_t localPageIndex = UINT32_MAX;
    uint32_t parentLocalPageIndex = UINT32_MAX;
    uint32_t fallbackLocalPageIndex = UINT32_MAX;
    uint64_t pageGeneration = 0u;
    uint64_t geometryRevision = 0u;
    uint64_t materialRevision = 0u;
    uint32_t unitScaleBits = 0u;
    uint32_t encodedPageBytes = 0u;
    uint32_t clusterCount = 0u;
    uint32_t decodedVertexCount = 0u;
    uint32_t decodedIndexCount = 0u;
    uint32_t decodedBoneWeightCount = 0u;
    bool overrideHierarchyLeafMetadata = false;
    std::array<float, 3u> hierarchyBoundsMin{};
    std::array<float, 3u> hierarchyBoundsMax{};
    float hierarchySelfError = 0.0f;
    float hierarchyParentError = 0.0f;
    bool hierarchyParentSphereValid = false;
    std::array<float, 3u> hierarchyParentBoundsMin{};
    std::array<float, 3u> hierarchyParentBoundsMax{};
    bool decodedClusterFlags = false;
    uint32_t priority = 0u;
    bool rootPage = false;
    bool hasTracePayload = false;
    bool hasMaterialPayload = false;
    bool requireProtectedParentOrFallback = false;
  };

  struct GpuWriteDescriptor
  {
    TicketId ticketId = INVALID_TICKET_ID;
    std::string objectName;
    uint32_t localPageIndex = UINT32_MAX;
    uint32_t globalPageTableIndex = UINT32_MAX;
    uint64_t pageGeneration = 0u;
    uint64_t geometryRevision = 0u;
    uint64_t materialRevision = 0u;
    uint32_t encodedPageByteOffset = UINT32_MAX;
    uint32_t encodedPageByteSize = 0u;
    uint32_t clusterOffset = 0u;
    uint32_t clusterCount = 0u;
    uint32_t decodedVertexOffset = 0u;
    uint32_t decodedVertexCount = 0u;
    uint32_t decodedIndexOffset = 0u;
    uint32_t decodedIndexCount = 0u;
    uint32_t decodedBoneWeightOffset = 0u;
    uint32_t decodedBoneWeightCount = 0u;
    uint32_t prioritySlot = UINT32_MAX;
  };

  struct PublishInfo
  {
    TicketId ticketId = INVALID_TICKET_ID;
    bool gpuWroteEncodedPageData = true;
    bool gpuWroteDecodedGeometry = false;
    bool enqueuePageDecode = true;
    // Optional compact draw contract reported by a GPU producer.  The backing
    // allocation remains the reservation capacity for retirement/fence
    // ownership, while the page table and hierarchy leaf advertise only the
    // actually emitted drawable range.
    uint32_t emittedClusterCount = 0u;
    uint32_t emittedDecodedVertexCount = 0u;
    uint32_t emittedDecodedIndexCount = 0u;
    uint32_t emittedDecodedBoneWeightCount = 0u;
  };

  struct Stats
  {
    uint64_t requestsSubmitted = 0u;
    uint64_t requestsDeduplicated = 0u;
    uint64_t reservationsCreated = 0u;
    uint64_t pagesPublished = 0u;
    uint64_t reservationsCancelled = 0u;
    uint64_t reservationFailures = 0u;
    uint64_t publishFailures = 0u;
    uint32_t pendingRequestCount = 0u;
    uint32_t activeReservationCount = 0u;
  };

  explicit VirtualGeometryGeneratedPayloadManager(VirtualGeometryScene &scene);

  bool enqueueRequest(const Request &request, std::string *error = nullptr);
  uint32_t reserveReady(
      uint32_t maxReservations,
      std::vector<GpuWriteDescriptor> *outDescriptors = nullptr,
      std::vector<std::string> *errors = nullptr);
  bool publishCompleted(const PublishInfo &publish, std::string *error = nullptr);
  bool cancel(TicketId ticketId, std::string *error = nullptr);
  bool hasActiveReservation(TicketId ticketId) const;
  const GpuWriteDescriptor *getDescriptor(TicketId ticketId) const;
  Stats getStats() const;

private:
  struct QueueEntry
  {
    uint32_t priority = 0u;
    uint64_t serial = 0u;

    bool operator<(const QueueEntry &other) const
    {
      if (priority != other.priority)
        return priority < other.priority;
      return serial > other.serial;
    }
  };

  struct Ticket
  {
    TicketId id = INVALID_TICKET_ID;
    Request request{};
    VirtualGeometryScene::RuntimeGeneratedPageReservation reservation{};
    GpuWriteDescriptor descriptor{};
  };

  static std::string makeKey(const std::string &objectName, uint32_t localPageIndex);
  static bool requestSupersedes(const Request &candidate, const Request &existing);
  static VirtualGeometryScene::RuntimeGeneratedPageReserveInfo toReserveInfo(const Request &request);
  static GpuWriteDescriptor makeDescriptor(TicketId ticketId, const Request &request, const VirtualGeometryScene::RuntimeGeneratedPageReservation &reservation);
  Request normalizePageIdentity(Request request) const;

  VirtualGeometryScene &scene_;
  uint64_t nextSerial_ = 1u;
  TicketId nextTicketId_ = 1u;
  Stats stats_{};
  std::priority_queue<QueueEntry> pendingQueue_;
  std::unordered_map<uint64_t, Request> pendingBySerial_;
  std::unordered_map<std::string, uint64_t> pendingSerialByKey_;
  std::unordered_map<TicketId, Ticket> activeTickets_;
  std::unordered_map<std::string, TicketId> activeTicketByKey_;

  struct PageIdentity
  {
    uint64_t pageGeneration = 0u;
    uint64_t geometryRevision = 0u;
    uint64_t materialRevision = 0u;
  };
  std::unordered_map<std::string, PageIdentity> latestIdentityByKey_;
};

} // namespace virtualgeometry
