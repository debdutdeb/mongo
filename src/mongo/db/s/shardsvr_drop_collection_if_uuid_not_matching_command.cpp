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

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/drop_collection.h"
#include "mongo/db/commands.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/logv2/log.h"
#include "mongo/s/request_types/drop_collection_if_uuid_not_matching_gen.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {

// TODO SERVER-74324: deprecate _shardsvrDropCollectionIfUUIDNotMatching after 7.0 is lastLTS.
class ShardsvrDropCollectionIfUUIDNotMatchingCommand final
    : public TypedCommand<ShardsvrDropCollectionIfUUIDNotMatchingCommand> {
public:
    bool skipApiVersionCheck() const override {
        /* Internal command (server to server) */
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return Command::AllowedOnSecondary::kNever;
    }

    std::string help() const override {
        return "Internal command aimed to remove stale entries from the local collection catalog.";
    }

    using Request = ShardsvrDropCollectionIfUUIDNotMatchingRequest;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            uassertStatusOK(ShardingState::get(opCtx)->canAcceptShardedCommands());

            opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

            uassertStatusOK(dropCollectionIfUUIDNotMatching(
                opCtx, ns(), request().getExpectedCollectionUUID()));

            WriteConcernResult ignoreResult;
            auto latestOpTime = repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
            uassertStatusOK(waitForWriteConcern(
                opCtx, latestOpTime, CommandHelpers::kMajorityWriteConcern, &ignoreResult));
        }

    private:
        NamespaceString ns() const override {
            return request().getNamespace();
        }

        bool supportsWriteConcern() const override {
            return false;
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(
                            ResourcePattern::forClusterResource(request().getDbName().tenantId()),
                            ActionType::dropCollection));
        }
    };
} shardSvrDropCollectionIfUUIDNotMatching;

class ShardsvrDropCollectionIfUUIDNotMatchingWithWriteConcernCommand final
    : public TypedCommand<ShardsvrDropCollectionIfUUIDNotMatchingWithWriteConcernCommand> {
public:
    bool skipApiVersionCheck() const override {
        /* Internal command (server to server) */
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return Command::AllowedOnSecondary::kNever;
    }

    std::string help() const override {
        return "Internal command aimed to remove stale entries from the local collection catalog.";
    }

    using Request = ShardsvrDropCollectionIfUUIDNotMatchingWithWriteConcernRequest;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            uassertStatusOK(ShardingState::get(opCtx)->canAcceptShardedCommands());

            CommandHelpers::uassertCommandRunWithMajority(Request::kCommandName,
                                                          opCtx->getWriteConcern());

            opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

            uassertStatusOK(dropCollectionIfUUIDNotMatching(
                opCtx, ns(), request().getExpectedCollectionUUID()));
        }

    private:
        NamespaceString ns() const override {
            return request().getNamespace();
        }

        bool supportsWriteConcern() const override {
            return true;
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(
                            ResourcePattern::forClusterResource(request().getDbName().tenantId()),
                            ActionType::dropCollection));
        }
    };
} shardSvrDropCollectionIfUUIDNotMatchingWithWriteConcern;
}  // namespace
}  // namespace mongo
