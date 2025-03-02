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

#include <memory>

#include "mongo/db/repl/primary_only_service_test_fixture.h"
#include "mongo/db/s/ddl_lock_manager.h"
#include "mongo/db/s/sharding_ddl_coordinator_service.h"
#include "mongo/executor/network_interface.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/rpc/metadata/egress_metadata_hook_list.h"
#include "mongo/unittest/log_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/fail_point.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {

class ShardingDDLCoordinatorServiceTest : public repl::PrimaryOnlyServiceMongoDTest {
public:
    std::unique_ptr<repl::PrimaryOnlyService> makeService(ServiceContext* serviceContext) override {
        return std::make_unique<ShardingDDLCoordinatorService>(serviceContext);
    }

    void setUp() override {
        PrimaryOnlyServiceMongoDTest::setUp();
        _testExecutor = makeTestExecutor();
    }

    void tearDown() override {
        // Ensure that even on test failures all failpoint state gets reset.
        globalFailPointRegistry().disableAllFailpoints();

        _testExecutor->shutdown();
        _testExecutor->join();
        _testExecutor.reset();

        PrimaryOnlyServiceMongoDTest::tearDown();
    }

    ShardingDDLCoordinatorService* ddlService() {
        return static_cast<ShardingDDLCoordinatorService*>(_service);
    }

    std::shared_ptr<executor::TaskExecutor> makeTestExecutor() {
        ThreadPool::Options threadPoolOptions;
        threadPoolOptions.maxThreads = 1;
        threadPoolOptions.threadNamePrefix = "ShardingDDLCoordinatorServiceTest-";
        threadPoolOptions.poolName = "ShardingDDLCoordinatorServiceTestThreadPool";
        threadPoolOptions.onCreateThread = [](const std::string& threadName) {
            Client::initThread(threadName.c_str());
        };

        auto executor = std::make_shared<executor::ThreadPoolTaskExecutor>(
            std::make_unique<ThreadPool>(threadPoolOptions),
            executor::makeNetworkInterface(
                "ShardingDDLCoordinatorServiceTestNetwork", nullptr, nullptr));
        executor->startup();
        return executor;
    }

    void printState() {
        std::string stateStr;
        switch (ddlService()->_state) {
            case ShardingDDLCoordinatorService::State::kPaused:
                stateStr = "kPaused";
                break;
            case ShardingDDLCoordinatorService::State::kRecovered:
                stateStr = "kRecovered";
                break;
            case ShardingDDLCoordinatorService::State::kRecovering:
                stateStr = "kRecovering";
                break;
            default:
                MONGO_UNREACHABLE;
        }
        LOGV2(7646301, "ShardingDDLCoordinatorService::_state", "state"_attr = stateStr);
    }

    void assertStateIsPaused() {
        ASSERT_EQ(ShardingDDLCoordinatorService::State::kPaused, ddlService()->_state);
    }

    void assertStateIsRecovered() {
        ASSERT_EQ(ShardingDDLCoordinatorService::State::kRecovered, ddlService()->_state);
    }

protected:
    using ScopedBaseDDLLock = DDLLockManager::ScopedBaseDDLLock;

    /**
     * Acquire Database and Collection DDL locks on the given resource.
     */
    std::pair<ScopedBaseDDLLock, ScopedBaseDDLLock> acquireDbAndCollDDLLocks(
        OperationContext* opCtx,
        const NamespaceString& ns,
        StringData reason,
        LockMode mode,
        Milliseconds timeout,
        bool waitForRecovery = true) {
        return std::make_pair(
            ScopedBaseDDLLock{opCtx, ns.dbName(), reason, mode, timeout, waitForRecovery},
            ScopedBaseDDLLock{opCtx, ns, reason, mode, timeout, waitForRecovery});
    }

    /**
     * Acquire Database and Collection DDL locks on the given resource without waiting for recovery
     * state to simulate requests coming from ShardingDDLCoordinators.
     */
    std::pair<ScopedBaseDDLLock, ScopedBaseDDLLock>
    acquireDbAndCollDDLLocksWithoutWaitingForRecovery(OperationContext* opCtx,
                                                      const NamespaceString& ns,
                                                      StringData reason,
                                                      LockMode mode,
                                                      Milliseconds timeout) {
        return acquireDbAndCollDDLLocks(
            opCtx, ns, reason, mode, timeout, false /*waitForRecovery*/);
    }

    std::shared_ptr<executor::TaskExecutor> _testExecutor;
};

TEST_F(ShardingDDLCoordinatorServiceTest, StateTransitions) {
    auto opCtx = makeOperationContext();

    // Reaching a steady state to start the test
    ddlService()->waitForRecoveryCompletion(opCtx.get());
    assertStateIsRecovered();

    // State must be `kPaused` after stepping down
    stepDown();
    assertStateIsPaused();

    // Check state is `kRecovered` once the recovery finishes
    stepUp(opCtx.get());
    ddlService()->waitForRecoveryCompletion(opCtx.get());
    assertStateIsRecovered();
}

TEST_F(ShardingDDLCoordinatorServiceTest,
       DDLLocksCanOnlyBeAcquiredOnceShardingDDLCoordinatorServiceIsRecovered) {
    auto opCtx = makeOperationContext();

    // Reaching a steady state to start the test
    ddlService()->waitForRecoveryCompletion(opCtx.get());

    const std::string reason = "dummyReason";
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.coll");

    // 1- Stepping down
    // Only DDL coordinators can acquire DDL locks after stepping down, otherwise trying to acquire
    // a DDL lock will throw a LockTimeout error
    stepDown();

    ASSERT_THROWS_CODE(
        acquireDbAndCollDDLLocks(opCtx.get(), nss, reason, MODE_X, Milliseconds::zero()),
        DBException,
        ErrorCodes::LockTimeout);

    ASSERT_DOES_NOT_THROW(acquireDbAndCollDDLLocksWithoutWaitingForRecovery(
        opCtx.get(), nss, reason, MODE_X, Milliseconds::zero()));

    // 2- Stepping up and pausing on Recovery state
    // Only DDL coordinators can acquire DDL locks during recovery, otherwise trying to acquire a
    // DDL lock will throw a LockTimeout error
    auto pauseOnRecoveryFailPoint =
        globalFailPointRegistry().find("pauseShardingDDLCoordinatorServiceOnRecovery");
    const auto fpCount = pauseOnRecoveryFailPoint->setMode(FailPoint::alwaysOn);
    stepUp(opCtx.get());
    pauseOnRecoveryFailPoint->waitForTimesEntered(fpCount + 1);

    ASSERT_THROWS_CODE(
        acquireDbAndCollDDLLocks(opCtx.get(), nss, reason, MODE_X, Milliseconds::zero()),
        DBException,
        ErrorCodes::LockTimeout);
    ASSERT_DOES_NOT_THROW(acquireDbAndCollDDLLocksWithoutWaitingForRecovery(
        opCtx.get(), nss, reason, MODE_X, Milliseconds::zero()));

    // 3- Ending Recovery and enter on Recovered state
    // Once ShardingDDLCoordinatorService is recovered, anyone can aquire a DDL lock
    pauseOnRecoveryFailPoint->setMode(FailPoint::off);
    ddlService()->waitForRecoveryCompletion(opCtx.get());

    ASSERT_DOES_NOT_THROW(
        acquireDbAndCollDDLLocks(opCtx.get(), nss, reason, MODE_X, Milliseconds::zero()));
    ASSERT_DOES_NOT_THROW(acquireDbAndCollDDLLocksWithoutWaitingForRecovery(
        opCtx.get(), nss, reason, MODE_X, Milliseconds::zero()));
}

TEST_F(ShardingDDLCoordinatorServiceTest, DDLLockMustBeEventuallyAcquiredAfterAStepUp) {
    auto opCtx = makeOperationContext();

    // Reaching a steady state to start the test
    ddlService()->waitForRecoveryCompletion(opCtx.get());

    const std::string reason = "dummyReason";
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.coll");

    stepDown();

    ASSERT_THROWS_CODE(
        acquireDbAndCollDDLLocks(opCtx.get(), nss, reason, MODE_X, Milliseconds::zero()),
        DBException,
        ErrorCodes::LockTimeout);

    // Start an async task to step up
    auto stepUpFuture = ExecutorFuture<void>(_testExecutor).then([this]() {
        auto pauseOnRecoveryFailPoint =
            globalFailPointRegistry().find("pauseShardingDDLCoordinatorServiceOnRecovery");
        const auto fpCount = pauseOnRecoveryFailPoint->setMode(FailPoint::alwaysOn);


        auto opCtx = makeOperationContext();
        stepUp(opCtx.get());

        // Stay on recovery state for some time to ensure the lock is acquired before transition to
        // recovered state
        sleepFor(Milliseconds(30));
        pauseOnRecoveryFailPoint->waitForTimesEntered(fpCount + 1);
        pauseOnRecoveryFailPoint->setMode(FailPoint::off);
    });

    ASSERT_DOES_NOT_THROW(acquireDbAndCollDDLLocks(opCtx.get(), nss, reason, MODE_X, Seconds(1)));

    // Lock should be acquired after step up conclusion
    ASSERT(stepUpFuture.isReady());
}

}  // namespace mongo
