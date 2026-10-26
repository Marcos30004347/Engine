#include "virtualgeometry/VirtualGeometryGeneratedPayloadManager.hpp"

#include <algorithm>
#include <limits>

namespace virtualgeometry
{

namespace
{

bool setManagerError(std::string *error, const std::string &message)
{
  if (error != nullptr)
    *error = message;
  return false;
}

} // namespace

VirtualGeometryGeneratedPayloadManager::VirtualGeometryGeneratedPayloadManager(
    VirtualGeometryScene &scene)
    : scene_(scene)
{
}

std::string VirtualGeometryGeneratedPayloadManager::makeKey(
    const std::string &objectName,
    const uint32_t localPageIndex)
{
  return objectName + "#" + std::to_string(localPageIndex);
}

bool VirtualGeometryGeneratedPayloadManager::requestSupersedes(
    const Request &candidate,
    const Request &existing)
{
  if (candidate.pageGeneration != existing.pageGeneration)
    return candidate.pageGeneration > existing.pageGeneration;
  if (candidate.geometryRevision != existing.geometryRevision)
    return candidate.geometryRevision > existing.geometryRevision;
  if (candidate.materialRevision != existing.materialRevision)
    return candidate.materialRevision > existing.materialRevision;
  return candidate.priority > existing.priority;
}

VirtualGeometryScene::RuntimeGeneratedPageReserveInfo
VirtualGeometryGeneratedPayloadManager::toReserveInfo(const Request &request)
{
  VirtualGeometryScene::RuntimeGeneratedPageReserveInfo reserve{};
  reserve.objectName = request.objectName;
  reserve.localPageIndex = request.localPageIndex;
  reserve.parentLocalPageIndex = request.parentLocalPageIndex;
  reserve.fallbackLocalPageIndex = request.fallbackLocalPageIndex;
  reserve.pageGeneration = request.pageGeneration;
  reserve.geometryRevision = request.geometryRevision;
  reserve.materialRevision = request.materialRevision;
  reserve.unitScaleBits = request.unitScaleBits;
  reserve.encodedPageBytes = request.encodedPageBytes;
  reserve.clusterCount = request.clusterCount;
  reserve.decodedVertexCount = request.decodedVertexCount;
  reserve.decodedIndexCount = request.decodedIndexCount;
  reserve.decodedBoneWeightCount = request.decodedBoneWeightCount;
  reserve.overrideHierarchyLeafMetadata =
      request.overrideHierarchyLeafMetadata;
  reserve.hierarchyBoundsMin = request.hierarchyBoundsMin;
  reserve.hierarchyBoundsMax = request.hierarchyBoundsMax;
  reserve.hierarchySelfError = request.hierarchySelfError;
  reserve.hierarchyParentError = request.hierarchyParentError;
  reserve.hierarchyParentSphereValid = request.hierarchyParentSphereValid;
  reserve.hierarchyParentBoundsMin = request.hierarchyParentBoundsMin;
  reserve.hierarchyParentBoundsMax = request.hierarchyParentBoundsMax;
  reserve.decodedClusterFlags = request.decodedClusterFlags;
  reserve.rootPage = request.rootPage;
  reserve.hasTracePayload = request.hasTracePayload;
  reserve.hasMaterialPayload = request.hasMaterialPayload;
  reserve.requireProtectedParentOrFallback =
      request.requireProtectedParentOrFallback;
  return reserve;
}

VirtualGeometryGeneratedPayloadManager::GpuWriteDescriptor
VirtualGeometryGeneratedPayloadManager::makeDescriptor(
    const TicketId ticketId,
    const Request &request,
    const VirtualGeometryScene::RuntimeGeneratedPageReservation &reservation)
{
  GpuWriteDescriptor descriptor{};
  descriptor.ticketId = ticketId;
  descriptor.objectName = request.objectName;
  descriptor.localPageIndex = request.localPageIndex;
  descriptor.globalPageTableIndex = reservation.globalPageTableIndex;
  descriptor.pageGeneration = request.pageGeneration;
  descriptor.geometryRevision = request.geometryRevision;
  descriptor.materialRevision = request.materialRevision;
  descriptor.encodedPageByteOffset = reservation.pageAllocation.bufferOffset;
  descriptor.encodedPageByteSize = reservation.pageAllocation.size;
  descriptor.clusterOffset = reservation.pageAllocation.clusterOffset;
  descriptor.clusterCount = reservation.pageAllocation.clusterCount;
  descriptor.decodedVertexOffset = reservation.pageAllocation.decodedVertexOffset;
  descriptor.decodedVertexCount = reservation.pageAllocation.decodedVertexCount;
  descriptor.decodedIndexOffset = reservation.pageAllocation.decodedIndexOffset;
  descriptor.decodedIndexCount = reservation.pageAllocation.decodedIndexCount;
  descriptor.decodedBoneWeightOffset =
      reservation.pageAllocation.decodedBoneWeightOffset;
  descriptor.decodedBoneWeightCount =
      reservation.pageAllocation.decodedBoneWeightCount;
  descriptor.prioritySlot = reservation.pageAllocation.prioritySlot;
  return descriptor;
}

VirtualGeometryGeneratedPayloadManager::Request
VirtualGeometryGeneratedPayloadManager::normalizePageIdentity(
    Request request) const
{
  const std::string key = makeKey(request.objectName, request.localPageIndex);
  uint64_t minimumGeneration = request.pageGeneration;
  const auto accountIdentity =
      [&](const uint64_t pageGeneration,
          const uint64_t geometryRevision,
          const uint64_t materialRevision)
  {
    if (pageGeneration == 0u)
      return;
    if (geometryRevision != request.geometryRevision ||
        materialRevision != request.materialRevision)
    {
      if (pageGeneration == std::numeric_limits<uint64_t>::max())
        minimumGeneration = std::numeric_limits<uint64_t>::max();
      else
        minimumGeneration = std::max(minimumGeneration, pageGeneration + 1u);
    }
    else
    {
      minimumGeneration = std::max(minimumGeneration, pageGeneration);
    }
  };

  if (const auto *status = scene_.getRuntimeObjectPageStatus(
          request.objectName, request.localPageIndex);
      status != nullptr && status->installed)
  {
    accountIdentity(
        status->pageGeneration,
        status->geometryRevision,
        status->materialRevision);
  }

  const auto knownIt = latestIdentityByKey_.find(key);
  if (knownIt != latestIdentityByKey_.end())
  {
    accountIdentity(
        knownIt->second.pageGeneration,
        knownIt->second.geometryRevision,
        knownIt->second.materialRevision);
  }

  request.pageGeneration = std::max<uint64_t>(1u, minimumGeneration);
  return request;
}

bool VirtualGeometryGeneratedPayloadManager::enqueueRequest(
    const Request &incomingRequest,
    std::string *error)
{
  const Request request = normalizePageIdentity(incomingRequest);
  if (error != nullptr)
    error->clear();
  if (request.objectName.empty())
    return setManagerError(error, "generated VG payload request needs an object name");
  if (request.localPageIndex == UINT32_MAX)
    return setManagerError(error, "generated VG payload request needs a local page index");
  if (request.pageGeneration == 0u ||
      request.geometryRevision == 0u ||
      request.materialRevision == 0u)
  {
    return setManagerError(error, "generated VG payload request revisions must be non-zero");
  }
  if (request.clusterCount == 0u ||
      request.clusterCount > VirtualGeometryScene::MAX_CLUSTERS_PER_PAGE)
  {
    return setManagerError(error, "generated VG payload request cluster count is invalid");
  }
  if (request.decodedVertexCount == 0u || request.decodedIndexCount == 0u)
    return setManagerError(error, "generated VG payload request decoded geometry budget is invalid");

  const std::string key = makeKey(request.objectName, request.localPageIndex);
  const auto activeIt = activeTicketByKey_.find(key);
  if (activeIt != activeTicketByKey_.end())
  {
    const auto ticketIt = activeTickets_.find(activeIt->second);
    if (ticketIt != activeTickets_.end() &&
        !requestSupersedes(request, ticketIt->second.request))
    {
      ++stats_.requestsDeduplicated;
      return true;
    }
  }

  const auto pendingIt = pendingSerialByKey_.find(key);
  if (pendingIt != pendingSerialByKey_.end())
  {
    const auto existingIt = pendingBySerial_.find(pendingIt->second);
    if (existingIt != pendingBySerial_.end())
    {
      if (!requestSupersedes(request, existingIt->second))
      {
        ++stats_.requestsDeduplicated;
        return true;
      }
      pendingBySerial_.erase(existingIt);
    }
  }

  const uint64_t serial = nextSerial_++;
  pendingSerialByKey_[key] = serial;
  pendingBySerial_[serial] = request;
  QueueEntry entry{};
  entry.priority = request.priority;
  entry.serial = serial;
  pendingQueue_.push(entry);
  ++stats_.requestsSubmitted;
  return true;
}

uint32_t VirtualGeometryGeneratedPayloadManager::reserveReady(
    const uint32_t maxReservations,
    std::vector<GpuWriteDescriptor> *outDescriptors,
    std::vector<std::string> *errors)
{
  if (outDescriptors != nullptr)
    outDescriptors->clear();
  if (errors != nullptr)
    errors->clear();
  if (maxReservations == 0u)
    return 0u;

  uint32_t reservedCount = 0u;
  std::vector<QueueEntry> blockedByActiveReservation;
  while (reservedCount < maxReservations && !pendingQueue_.empty())
  {
    const QueueEntry entry = pendingQueue_.top();
    pendingQueue_.pop();

    auto pendingIt = pendingBySerial_.find(entry.serial);
    if (pendingIt == pendingBySerial_.end())
      continue;

    Request request = normalizePageIdentity(pendingIt->second);
    pendingIt->second = request;
    const std::string key = makeKey(request.objectName, request.localPageIndex);
    const auto serialIt = pendingSerialByKey_.find(key);
    if (serialIt == pendingSerialByKey_.end() ||
        serialIt->second != entry.serial)
    {
      pendingBySerial_.erase(pendingIt);
      continue;
    }

    if (activeTicketByKey_.find(key) != activeTicketByKey_.end())
    {
      blockedByActiveReservation.push_back(entry);
      continue;
    }

    VirtualGeometryScene::RuntimeGeneratedPageReservation reservation{};
    std::string reserveError;
    if (!scene_.reserveRuntimeGeneratedPage(
            toReserveInfo(request),
            reservation,
            &reserveError))
    {
      ++stats_.reservationFailures;
      if (errors != nullptr)
        errors->push_back(reserveError);
      pendingBySerial_.erase(pendingIt);
      pendingSerialByKey_.erase(key);
      continue;
    }

    const TicketId ticketId = nextTicketId_++;
    Ticket ticket{};
    ticket.id = ticketId;
    ticket.request = request;
    ticket.reservation = reservation;
    ticket.descriptor = makeDescriptor(ticketId, request, reservation);

    activeTicketByKey_[key] = ticketId;
    activeTickets_[ticketId] = ticket;
    latestIdentityByKey_[key] = PageIdentity{
      request.pageGeneration,
      request.geometryRevision,
      request.materialRevision};
    pendingBySerial_.erase(pendingIt);
    pendingSerialByKey_.erase(key);
    if (outDescriptors != nullptr)
      outDescriptors->push_back(ticket.descriptor);
    ++stats_.reservationsCreated;
    ++reservedCount;
  }

  for (const QueueEntry &entry : blockedByActiveReservation)
    pendingQueue_.push(entry);
  return reservedCount;
}

bool VirtualGeometryGeneratedPayloadManager::publishCompleted(
    const PublishInfo &publish,
    std::string *error)
{
  if (error != nullptr)
    error->clear();
  if (publish.ticketId == INVALID_TICKET_ID)
    return setManagerError(error, "generated VG payload publish needs a valid ticket");

  auto ticketIt = activeTickets_.find(publish.ticketId);
  if (ticketIt == activeTickets_.end())
    return setManagerError(error, "generated VG payload ticket is not active");

  const auto &reservation = ticketIt->second.reservation;
  const auto validateEmittedCount =
      [&](const uint32_t emitted,
          const uint32_t capacity,
          const char *label) -> bool
  {
    if (emitted == 0u)
      return true;
    if (emitted > capacity)
    {
      return setManagerError(
          error,
          std::string("generated VG payload emitted ") + label +
              " count exceeds reservation capacity");
    }
    return true;
  };
  if (!validateEmittedCount(
          publish.emittedClusterCount,
          reservation.pageAllocation.clusterCount,
          "cluster") ||
      !validateEmittedCount(
          publish.emittedDecodedVertexCount,
          reservation.pageAllocation.decodedVertexCount,
          "decoded vertex") ||
      !validateEmittedCount(
          publish.emittedDecodedIndexCount,
          reservation.pageAllocation.decodedIndexCount,
          "decoded index") ||
      !validateEmittedCount(
          publish.emittedDecodedBoneWeightCount,
          reservation.pageAllocation.decodedBoneWeightCount,
          "decoded bone weight"))
  {
    ++stats_.publishFailures;
    return false;
  }

  VirtualGeometryScene::RuntimeGeneratedPageInstallInfo install{};
  install.reservation = reservation;
  if (publish.emittedClusterCount != 0u)
    install.reservation.pageTableEntry.clusterCount =
        publish.emittedClusterCount;
  install.gpuWroteEncodedPageData = publish.gpuWroteEncodedPageData;
  install.gpuWroteDecodedGeometry = publish.gpuWroteDecodedGeometry;
  install.enqueuePageDecode = publish.enqueuePageDecode;

  if (!scene_.installRuntimeGeneratedPage(install, error))
  {
    ++stats_.publishFailures;
    return false;
  }

  const std::string key =
      makeKey(ticketIt->second.request.objectName,
              ticketIt->second.request.localPageIndex);
  latestIdentityByKey_[key] = PageIdentity{
    ticketIt->second.request.pageGeneration,
    ticketIt->second.request.geometryRevision,
    ticketIt->second.request.materialRevision};
  activeTicketByKey_.erase(key);
  activeTickets_.erase(ticketIt);
  ++stats_.pagesPublished;
  return true;
}

bool VirtualGeometryGeneratedPayloadManager::cancel(
    const TicketId ticketId,
    std::string *error)
{
  if (error != nullptr)
    error->clear();
  if (ticketId == INVALID_TICKET_ID)
    return setManagerError(error, "generated VG payload cancel needs a valid ticket");
  auto ticketIt = activeTickets_.find(ticketId);
  if (ticketIt == activeTickets_.end())
    return setManagerError(error, "generated VG payload ticket is not active");

  if (!scene_.cancelRuntimeGeneratedPageReservation(ticketIt->second.reservation, error))
    return false;

  const std::string key =
      makeKey(ticketIt->second.request.objectName,
              ticketIt->second.request.localPageIndex);
  activeTicketByKey_.erase(key);
  activeTickets_.erase(ticketIt);
  ++stats_.reservationsCancelled;
  return true;
}

bool VirtualGeometryGeneratedPayloadManager::hasActiveReservation(
    const TicketId ticketId) const
{
  return activeTickets_.find(ticketId) != activeTickets_.end();
}

const VirtualGeometryGeneratedPayloadManager::GpuWriteDescriptor *
VirtualGeometryGeneratedPayloadManager::getDescriptor(const TicketId ticketId) const
{
  const auto ticketIt = activeTickets_.find(ticketId);
  if (ticketIt == activeTickets_.end())
    return nullptr;
  return &ticketIt->second.descriptor;
}

VirtualGeometryGeneratedPayloadManager::Stats
VirtualGeometryGeneratedPayloadManager::getStats() const
{
  Stats stats = stats_;
  stats.pendingRequestCount = static_cast<uint32_t>(pendingBySerial_.size());
  stats.activeReservationCount = static_cast<uint32_t>(activeTickets_.size());
  return stats;
}

} // namespace virtualgeometry
