/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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


#include "mongo/platform/basic.h"

#include "mongo/s/sharding_initialization.h"

#include <memory>
#include <string>

#include "mongo/base/status.h"
#include "mongo/db/audit.h"
#include "mongo/db/keys_collection_client_sharded.h"
#include "mongo/db/keys_collection_manager.h"
#include "mongo/db/logical_time_validator.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/time_proof_service.h"
#include "mongo/executor/async_multicaster.h"
#include "mongo/executor/connection_pool.h"
#include "mongo/executor/connection_pool_stats.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/network_interface_thread_pool.h"
#include "mongo/executor/scoped_task_executor.h"
#include "mongo/executor/task_executor.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/metadata/metadata_hook.h"
#include "mongo/s/balancer_configuration.h"
#include "mongo/s/catalog/sharding_catalog_client_impl.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/client/num_hosts_targeted_metrics.h"
#include "mongo/s/client/shard_factory.h"
#include "mongo/s/client/sharding_network_connection_hook.h"
#include "mongo/s/cluster_identity_loader.h"
#include "mongo/s/grid.h"
#include "mongo/s/initialize_tenant_to_shard_cache.h"
#include "mongo/s/mongod_and_mongos_server_parameters_gen.h"
#include "mongo/s/query/cluster_cursor_manager.h"
#include "mongo/s/query_analysis_client.h"
#include "mongo/s/query_analysis_sampler.h"
#include "mongo/s/sharding_task_executor.h"
#include "mongo/s/sharding_task_executor_pool_controller.h"
#include "mongo/s/sharding_task_executor_pool_gen.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/exit.h"
#include "mongo/util/net/socket_utils.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {

using executor::ConnectionPool;
using executor::NetworkInterface;
using executor::NetworkInterfaceThreadPool;
using executor::TaskExecutorPool;
using executor::ThreadPoolTaskExecutor;

static constexpr auto kRetryInterval = Seconds{2};

std::shared_ptr<executor::TaskExecutor> makeShardingFixedTaskExecutor(
    std::unique_ptr<NetworkInterface> net) {
    auto executor =
        std::make_unique<ThreadPoolTaskExecutor>(std::make_unique<ThreadPool>([] {
                                                     ThreadPool::Options opts;
                                                     opts.poolName = "Sharding-Fixed";
                                                     opts.maxThreads =
                                                         ThreadPool::Options::kUnlimited;
                                                     return opts;
                                                 }()),
                                                 std::move(net));

    return std::make_shared<executor::ShardingTaskExecutor>(std::move(executor));
}

std::unique_ptr<TaskExecutorPool> makeShardingTaskExecutorPool(
    std::unique_ptr<NetworkInterface> fixedNet,
    rpc::ShardingEgressMetadataHookBuilder metadataHookBuilder,
    ConnectionPool::Options connPoolOptions,
    boost::optional<size_t> taskExecutorPoolSize) {
    std::vector<std::shared_ptr<executor::TaskExecutor>> executors;

    const auto poolSize = taskExecutorPoolSize.value_or(TaskExecutorPool::getSuggestedPoolSize());

    for (size_t i = 0; i < poolSize; ++i) {
        auto exec = makeShardingTaskExecutor(
            executor::makeNetworkInterface("TaskExecutorPool-" + std::to_string(i),
                                           std::make_unique<ShardingNetworkConnectionHook>(),
                                           metadataHookBuilder(),
                                           connPoolOptions));

        executors.emplace_back(std::move(exec));
    }

    // Add executor used to perform non-performance critical work.
    auto fixedExec = makeShardingFixedTaskExecutor(std::move(fixedNet));

    auto executorPool = std::make_unique<TaskExecutorPool>();
    executorPool->addExecutors(std::move(executors), std::move(fixedExec));
    return executorPool;
}

/**
 * Uses an AsyncMulticaster to ping all of the hosts in order to establish
 * ShardingTaskExecutorPoolMinSize connections. This does not wait
 * for the connections to be established nor does it check how many were established.
 */
void preWarmConnections(OperationContext* opCtx, std::vector<HostAndPort> allHosts) {
    auto const grid = Grid::get(opCtx);
    auto arbi = grid->getExecutorPool()->getArbitraryExecutor();
    auto executor = executor::ScopedTaskExecutor(arbi);
    executor::AsyncMulticaster::Options options;

    auto results =
        executor::AsyncMulticaster(*executor, options)
            .multicast(allHosts,
                       "admin",
                       BSON("ping" << 1),
                       opCtx,
                       Milliseconds(gWarmMinConnectionsInShardingTaskExecutorPoolOnStartupWaitMS));
}

}  // namespace

std::unique_ptr<executor::TaskExecutor> makeShardingTaskExecutor(
    std::unique_ptr<NetworkInterface> net) {
    auto netPtr = net.get();
    auto executor = std::make_unique<ThreadPoolTaskExecutor>(
        std::make_unique<NetworkInterfaceThreadPool>(netPtr), std::move(net));

    return std::make_unique<executor::ShardingTaskExecutor>(std::move(executor));
}

Status initializeGlobalShardingState(
    OperationContext* opCtx,
    std::unique_ptr<CatalogCache> catalogCache,
    std::unique_ptr<ShardRegistry> shardRegistry,
    rpc::ShardingEgressMetadataHookBuilder hookBuilder,
    boost::optional<size_t> taskExecutorPoolSize,
    std::function<std::unique_ptr<KeysCollectionClient>(ShardingCatalogClient*)> initKeysClient) {

    ConnectionPool::Options connPoolOptions;
    std::shared_ptr<ShardRegistry> srsp(std::move(shardRegistry));
    connPoolOptions.controllerFactory = [srwp = std::weak_ptr(srsp)] {
        return std::make_shared<ShardingTaskExecutorPoolController>(srwp);
    };

    auto network = executor::makeNetworkInterface(
        "ShardRegistry", std::make_unique<ShardingNetworkConnectionHook>(), hookBuilder());
    auto networkPtr = network.get();
    auto executorPool = makeShardingTaskExecutorPool(
        std::move(network), hookBuilder, connPoolOptions, taskExecutorPoolSize);
    executorPool->startup();

    auto& numHostsTargetedMetrics = NumHostsTargetedMetrics::get(opCtx);
    numHostsTargetedMetrics.startup();

    const auto service = opCtx->getServiceContext();
    auto const grid = Grid::get(service);

    grid->init(std::make_unique<ShardingCatalogClientImpl>(nullptr /* overrideConfigShard */),
               std::move(catalogCache),
               std::move(srsp),
               std::make_unique<ClusterCursorManager>(service->getPreciseClockSource()),
               std::make_unique<BalancerConfiguration>(),
               std::move(executorPool),
               networkPtr);

    // The shard registry must be started once the grid is initialized
    grid->shardRegistry()->startupPeriodicReloader(opCtx);

    // Start up the cluster time keys manager with a sharded keys client.
    auto keysCollectionClient = initKeysClient(grid->catalogClient());
    auto keyManager =
        std::make_shared<KeysCollectionManager>(KeysCollectionManager::kKeyManagerPurposeString,
                                                std::move(keysCollectionClient),
                                                Seconds(KeysRotationIntervalSec));
    keyManager->startMonitoring(service);

    LogicalTimeValidator::set(service, std::make_unique<LogicalTimeValidator>(keyManager));
    initializeTenantToShardCache(service);

    analyze_shard_key::QueryAnalysisClient::get(opCtx).setTaskExecutor(
        service, Grid::get(service)->getExecutorPool()->getFixedExecutor());
    if (analyze_shard_key::supportsSamplingQueries(service)) {
        analyze_shard_key::QueryAnalysisSampler::get(service).onStartup();
    }

    return Status::OK();
}

void loadCWWCFromConfigServerForReplication(OperationContext* opCtx) {
    if (!serverGlobalParams.clusterRole.exclusivelyHasShardRole()) {
        // Cluster wide read/write concern in a sharded cluster lives on the config server, so a
        // config server node's local cache will be correct and explicitly checking for a default
        // write concern via remote command is unnecessary.
        return;
    }

    repl::ReplicationCoordinator::get(opCtx)->recordIfCWWCIsSetOnConfigServerOnStartup(opCtx);
}

Status loadGlobalSettingsFromConfigServer(OperationContext* opCtx,
                                          ShardingCatalogClient* catalogClient) {
    while (!globalInShutdownDeprecated()) {
        auto stopStatus = opCtx->checkForInterruptNoAssert();
        if (!stopStatus.isOK()) {
            return stopStatus;
        }

        try {
            // TODO SERVER-78051: Re-evaluate use of ClusterIdentityLoader.
            //
            // Skip loading the cluster id on config servers to avoid an issue where a failed
            // initial sync may lead the config server to transiently have a shard identity document
            // but no cluster id, which would trigger infinite retries.
            //
            // To match the shard behavior, the config server should load the cluster id, but
            // currently shards never use the loaded cluster id, so skipping the load is safe. Only
            // the config server uses it when adding a new shard, and each config server node will
            // load this on its first step up to primary.
            if (!serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer)) {
                uassertStatusOK(ClusterIdentityLoader::get(opCtx)->loadClusterId(
                    opCtx, catalogClient, repl::ReadConcernLevel::kMajorityReadConcern));
            }

            // Assert will be raised on failure to talk to config server.
            loadCWWCFromConfigServerForReplication(opCtx);

            return Status::OK();
        } catch (const DBException& ex) {
            Status status = ex.toStatus();
            LOGV2_WARNING(23834,
                          "Error loading global settings from config server. Sleeping for 2 "
                          "seconds and retrying",
                          "error"_attr = status);
            sleepFor(kRetryInterval);
            continue;
        }
    }

    return {ErrorCodes::ShutdownInProgress, "aborted loading global settings from config server"};
}

void preCacheMongosRoutingInfo(OperationContext* opCtx) {
    if (!gLoadRoutingTableOnStartup) {
        return;
    }

    // There's no reason this can't run on a shard or config server, but it currently only runs on a
    // mongos, and we'd need to consider the implications of it running on either kind of mongod.
    tassert(71960,
            "Unexpectedly pre caching mongos routing info on shard or config server node",
            serverGlobalParams.clusterRole.has(ClusterRole::None));

    auto grid = Grid::get(opCtx);
    auto catalogClient = grid->catalogClient();
    auto catalogCache = grid->catalogCache();
    auto allDbs = catalogClient->getAllDBs(opCtx, repl::ReadConcernLevel::kMajorityReadConcern);

    for (auto& db : allDbs) {
        for (auto& coll : catalogClient->getAllShardedCollectionsForDb(
                 opCtx, db.getName(), repl::ReadConcernLevel::kMajorityReadConcern)) {
            auto resp = catalogCache->getShardedCollectionRoutingInfoWithRefresh(opCtx, coll);
            if (!resp.isOK()) {
                LOGV2_WARNING(6203600,
                              "Failed to warmup collection routing information",
                              logAttrs(coll),
                              "error"_attr = redact(resp.getStatus()));
            }
        }
    }
}

Status preWarmConnectionPool(OperationContext* opCtx) {
    if (!gWarmMinConnectionsInShardingTaskExecutorPoolOnStartup) {
        return Status::OK();
    }

    // There's no reason this can't run on a shard or config server, but it currently only runs on a
    // mongos, and we'd need to consider the implications of it running on either kind of mongod.
    tassert(71961,
            "Unexpectedly pre warming connection pool on shard or config server node",
            serverGlobalParams.clusterRole.has(ClusterRole::None));

    std::vector<HostAndPort> allHosts;
    auto const grid = Grid::get(opCtx);
    auto allShardsStatus =
        grid->catalogClient()->getAllShards(opCtx, repl::ReadConcernLevel::kMajorityReadConcern);
    if (!allShardsStatus.isOK()) {
        return allShardsStatus.getStatus();
    }
    auto allShards = allShardsStatus.getValue().value;

    for (auto& shard : allShards) {
        auto connStrStatus = ConnectionString::parse(shard.getHost());
        if (!connStrStatus.isOK()) {
            return connStrStatus.getStatus();
        }
        auto connStr = connStrStatus.getValue();
        for (const auto& hostEntry : connStr.getServers()) {
            allHosts.push_back(hostEntry);
        }
    }
    try {
        opCtx->runWithDeadline(
            opCtx->getServiceContext()->getPreciseClockSource()->now() +
                Milliseconds(gWarmMinConnectionsInShardingTaskExecutorPoolOnStartupWaitMS),
            ErrorCodes::ExceededTimeLimit,
            [&] { preWarmConnections(opCtx, allHosts); });
    } catch (const ExceptionFor<ErrorCodes::ExceededTimeLimit>&) {
        // if we've timed out, eat the exception and continue
    } catch (const DBException& ex) {
        return ex.toStatus();
    }

    return Status::OK();
}

}  // namespace mongo
