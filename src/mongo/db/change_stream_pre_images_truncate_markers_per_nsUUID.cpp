/**
 *    Copyright (C) 2023-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/change_stream_pre_images_truncate_markers_per_nsUUID.h"

#include "mongo/db/change_stream_pre_image_util.h"
#include "mongo/db/change_stream_serverless_helpers.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo {
namespace {
// Returns true if the pre-image with highestRecordId and highestWallTime is expired.
bool isExpired(OperationContext* opCtx,
               const boost::optional<TenantId>& tenantId,
               const RecordId& highestRecordId,
               Date_t highestWallTime) {
    auto currentTimeForTimeBasedExpiration =
        change_stream_pre_image_util::getCurrentTimeForPreImageRemoval(opCtx);

    if (tenantId) {
        // In a serverless environment, the 'expireAfterSeconds' is set per tenant and is the only
        // criteria considered when determining whether a marker is expired.
        //
        // The oldest marker is expired if:
        //   'wallTime' of the oldest marker <= current node time - 'expireAfterSeconds'
        auto expireAfterSeconds =
            Seconds{change_stream_serverless_helpers::getExpireAfterSeconds(tenantId.get())};
        auto preImageExpirationTime = currentTimeForTimeBasedExpiration - expireAfterSeconds;
        return highestWallTime <= preImageExpirationTime;
    }

    // In a non-serverless environment, a marker is expired if either:
    //     (1) 'highestWallTime' of the (partial) marker <= current node time -
    //     'expireAfterSeconds' OR
    //     (2) Timestamp of the 'highestRecordId' in the oldest marker <
    //     Timestamp of earliest oplog entry

    // The 'expireAfterSeconds' may or may not be set in a non-serverless environment.
    const auto preImageExpirationTime = change_stream_pre_image_util::getPreImageExpirationTime(
        opCtx, currentTimeForTimeBasedExpiration);
    bool expiredByTimeBasedExpiration =
        preImageExpirationTime ? highestWallTime <= preImageExpirationTime : false;

    const auto currentEarliestOplogEntryTs =
        repl::StorageInterface::get(opCtx->getServiceContext())->getEarliestOplogTimestamp(opCtx);
    auto highestRecordTimestamp =
        change_stream_pre_image_util::getPreImageTimestamp(highestRecordId);
    return expiredByTimeBasedExpiration || highestRecordTimestamp < currentEarliestOplogEntryTs;
}

}  // namespace

PreImagesTruncateMarkersPerNsUUID::PreImagesTruncateMarkersPerNsUUID(
    boost::optional<TenantId> tenantId,
    std::deque<Marker> markers,
    int64_t leftoverRecordsCount,
    int64_t leftoverRecordsBytes,
    int64_t minBytesPerMarker,
    CollectionTruncateMarkers::MarkersCreationMethod creationMethod)
    : CollectionTruncateMarkersWithPartialExpiration(
          std::move(markers), leftoverRecordsCount, leftoverRecordsBytes, minBytesPerMarker),
      _tenantId(std::move(tenantId)),
      _creationMethod(creationMethod) {}

CollectionTruncateMarkers::RecordIdAndWallTime
PreImagesTruncateMarkersPerNsUUID::getRecordIdAndWallTime(const Record& record) {
    BSONObj preImageObj = record.data.toBson();
    return CollectionTruncateMarkers::RecordIdAndWallTime(
        record.id, preImageObj[ChangeStreamPreImage::kOperationTimeFieldName].date());
}

CollectionTruncateMarkers::InitialSetOfMarkers
PreImagesTruncateMarkersPerNsUUID::createInitialMarkersFromSamples(
    OperationContext* opCtx,
    const UUID& nsUUID,
    const std::vector<CollectionTruncateMarkers::RecordIdAndWallTime>& samples,
    int64_t estimatedRecordsPerMarker,
    int64_t estimatedBytesPerMarker) {
    std::deque<CollectionTruncateMarkers::Marker> markers;
    auto numSamples = samples.size();
    invariant(numSamples > 0);
    for (size_t i = CollectionTruncateMarkers::kRandomSamplesPerMarker - 1; i < numSamples;
         i = i + CollectionTruncateMarkers::kRandomSamplesPerMarker) {
        const auto& [id, wallTime] = samples[i];
        LOGV2_DEBUG(
            7658602,
            0,
            "Marking entry as a potential future truncation point for pre-images collection",
            "wall"_attr = wallTime,
            "ts"_attr = id);
        markers.emplace_back(estimatedRecordsPerMarker, estimatedBytesPerMarker, id, wallTime);
    }

    // Sampling is best effort estimations and at this step, only account for the whole markers
    // generated and leave the 'currentRecords' and 'currentBytes' to be filled in at a later time.
    // Additionally, the time taken is relatively arbitrary as the expensive part of the operation
    // was retrieving the samples.
    return CollectionTruncateMarkers::InitialSetOfMarkers{
        std::move(markers),
        0 /** currentRecords **/,
        0 /** currentBytes **/,
        Microseconds{0} /** timeTaken **/,
        CollectionTruncateMarkers::MarkersCreationMethod::Sampling};
}

CollectionTruncateMarkers::InitialSetOfMarkers
PreImagesTruncateMarkersPerNsUUID::createInitialMarkersScanning(OperationContext* opCtx,
                                                                RecordStore* rs,
                                                                const UUID& nsUUID,
                                                                int64_t minBytesPerMarker) {
    Timer scanningTimer;

    RecordIdBound minRecordIdBound =
        change_stream_pre_image_util::getAbsoluteMinPreImageRecordIdBoundForNs(nsUUID);
    RecordId minRecordId = minRecordIdBound.recordId();

    RecordIdBound maxRecordIdBound =
        change_stream_pre_image_util::getAbsoluteMaxPreImageRecordIdBoundForNs(nsUUID);
    RecordId maxRecordId = maxRecordIdBound.recordId();

    auto cursor = rs->getCursor(opCtx, true);
    auto record = cursor->seekNear(minRecordId);

    // A forward seekNear will return the previous entry if one does not match exactly. In most
    // cases, we will need to call next() to get our correct UUID.
    while (record && record->id < minRecordId) {
        record = cursor->next();
    }

    if (!record || (record && record->id > maxRecordId)) {
        return CollectionTruncateMarkers::InitialSetOfMarkers{
            {}, 0, 0, Microseconds{0}, MarkersCreationMethod::EmptyCollection};
    }

    int64_t currentRecords = 0;
    int64_t currentBytes = 0;
    std::deque<CollectionTruncateMarkers::Marker> markers;
    while (record && record->id < maxRecordId) {
        currentRecords++;
        currentBytes += record->data.size();

        auto [rId, wallTime] = getRecordIdAndWallTime(*record);
        if (currentBytes >= minBytesPerMarker) {
            LOGV2_DEBUG(7500500,
                        1,
                        "Marking entry as a potential future truncation point for collection with "
                        "pre-images enabled",
                        "wallTime"_attr = wallTime,
                        "nsUuid"_attr = nsUUID);

            markers.emplace_back(
                std::exchange(currentRecords, 0), std::exchange(currentBytes, 0), rId, wallTime);
        }
        record = cursor->next();
    }

    return CollectionTruncateMarkers::InitialSetOfMarkers{
        std::move(markers),
        currentRecords,
        currentBytes,
        scanningTimer.elapsed(),
        CollectionTruncateMarkers::MarkersCreationMethod::Scanning};
}

void PreImagesTruncateMarkersPerNsUUID::updatePartialMarkerForInitialisation(
    OperationContext* opCtx,
    int64_t numBytes,
    RecordId recordId,
    Date_t wallTime,
    int64_t numRecords) {
    updateCurrentMarker(opCtx, numBytes, recordId, wallTime, numRecords);
}

bool PreImagesTruncateMarkersPerNsUUID::_hasExcessMarkers(OperationContext* opCtx) const {
    const auto& markers = getMarkers();
    if (markers.empty()) {
        // If there's nothing in the markers queue then we don't have excess markers by definition.
        return false;
    }

    const Marker& oldestMarker = markers.front();
    return isExpired(opCtx, _tenantId, oldestMarker.lastRecord, oldestMarker.wallTime);
}

bool PreImagesTruncateMarkersPerNsUUID::_hasPartialMarkerExpired(OperationContext* opCtx) const {
    const auto& [highestSeenRecordId, highestSeenWallTime] = getPartialMarker();
    return isExpired(opCtx, _tenantId, highestSeenRecordId, highestSeenWallTime);
}
}  // namespace mongo
