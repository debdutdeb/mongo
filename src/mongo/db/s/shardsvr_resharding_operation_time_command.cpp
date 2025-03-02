/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/primary_only_service.h"
#include "mongo/db/s/metrics/sharding_data_transform_metrics.h"
#include "mongo/db/s/resharding/resharding_recipient_service.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/service_context.h"
#include "mongo/s/request_types/resharding_operation_time_gen.h"
#include "mongo/util/duration.h"

namespace mongo {
namespace {

class ShardsvrReshardingOperationTimeCmd final
    : public TypedCommand<ShardsvrReshardingOperationTimeCmd> {
public:
    class OperationTime {
    public:
        explicit OperationTime(boost::optional<Milliseconds> elapsedMillis,
                               boost::optional<Milliseconds> remainingMillis)
            : _elapsedMillis{elapsedMillis}, _remainingMillis{remainingMillis} {}
        void serialize(BSONObjBuilder* bob) const {
            if (_elapsedMillis) {
                bob->append("elapsedMillis", _elapsedMillis->count());
            }
            if (_remainingMillis) {
                bob->append("remainingMillis", _remainingMillis->count());
            }
        }

    private:
        boost::optional<Milliseconds> _elapsedMillis;
        boost::optional<Milliseconds> _remainingMillis;
    };

    using Request = _shardsvrReshardingOperationTime;
    using Response = OperationTime;

    bool skipApiVersionCheck() const override {
        // Internal command (server to server).
        return true;
    }

    std::string help() const override {
        return "Internal command used by the resharding coordinator to query the elapsed and "
               "remaining time for the active resharding operation on participant shards.";
    }

    bool adminOnly() const override {
        return true;
    }

    Command::AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return Command::AllowedOnSecondary::kNever;
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        bool supportsWriteConcern() const override {
            return false;
        }

        NamespaceString ns() const override {
            return request().getCommandParameter();
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(
                            ResourcePattern::forClusterResource(request().getDbName().tenantId()),
                            ActionType::internal));
        }

        Response typedRun(OperationContext* opCtx) {
            uassertStatusOK(ShardingState::get(opCtx)->canAcceptShardedCommands());
            auto instances = resharding::getReshardingStateMachines<
                ReshardingRecipientService,
                ReshardingRecipientService::RecipientStateMachine>(opCtx, ns());
            if (instances.empty()) {
                return Response{boost::none, boost::none};
            }
            invariant(instances.size() == 1);
            const auto& metrics = instances[0]->getMetrics();
            return Response{duration_cast<Milliseconds>(metrics.getOperationRunningTimeSecs()),
                            metrics.getHighEstimateRemainingTimeMillis()};
        }
    };
} _shardsvrReshardingOperationTime;

}  // namespace
}  // namespace mongo
