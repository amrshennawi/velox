/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <folly/json.h>

#include <utility>

#include "velox/core/PlanNode.h"
#include "velox/exec/TaskTraceReader.h"
#include "velox/exec/TraceUtil.h"
#include "velox/exec/tests/utils/AssertQueryBuilder.h"
#include "velox/exec/tests/utils/PlanBuilder.h"
#include "velox/tool/trace/OperatorReplayerBase.h"

using namespace facebook::velox;

namespace facebook::velox::tool::trace {
OperatorReplayerBase::OperatorReplayerBase(
    std::string traceDir,
    std::string queryId,
    std::string taskId,
    std::string nodeId,
    std::string operatorType)
    : queryId_(std::string(std::move(queryId))),
      taskId_(std::move(taskId)),
      nodeId_(std::move(nodeId)),
      operatorType_(std::move(operatorType)),
      taskTraceDir_(
          exec::trace::getTaskTraceDirectory(traceDir, queryId_, taskId_)),
      nodeTraceDir_(exec::trace::getNodeTraceDirectory(taskTraceDir_, nodeId_)),
      fs_(filesystems::getFileSystem(taskTraceDir_, nullptr)),
      pipelineIds_(exec::trace::listPipelineIds(nodeTraceDir_, fs_)),
      maxDrivers_(exec::trace::getNumDrivers(
          nodeTraceDir_,
          pipelineIds_.front(),
          fs_)) {
  VELOX_USER_CHECK(!taskTraceDir_.empty());
  VELOX_USER_CHECK(!taskId_.empty());
  VELOX_USER_CHECK(!nodeId_.empty());
  VELOX_USER_CHECK(!operatorType_.empty());
  if (operatorType_ == "HashJoin") {
    VELOX_USER_CHECK_EQ(pipelineIds_.size(), 2);
  } else {
    VELOX_USER_CHECK_EQ(pipelineIds_.size(), 1);
  }

  const auto taskMetaReader = exec::trace::TaskTraceMetadataReader(
      taskTraceDir_, memory::MemoryManager::getInstance()->tracePool());
  taskMetaReader.read(queryConfigs_, connectorConfigs_, planFragment_);
  queryConfigs_[core::QueryConfig::kQueryTraceEnabled] = "false";
}

RowVectorPtr OperatorReplayerBase::run() {
  const auto restoredPlanNode = createPlan();
  return exec::test::AssertQueryBuilder(restoredPlanNode)
      .maxDrivers(maxDrivers_)
      .configs(queryConfigs_)
      .connectorSessionProperties(connectorConfigs_)
      .copyResults(memory::MemoryManager::getInstance()->tracePool());
}

core::PlanNodePtr OperatorReplayerBase::createPlan() const {
  const auto* replayNode = core::PlanNode::findFirstNode(
      planFragment_.get(),
      [this](const core::PlanNode* node) { return node->id() == nodeId_; });

  if (replayNode->name() == "TableScan") {
    return exec::test::PlanBuilder()
        .addNode(replayNodeFactory(replayNode))
        .planNode();
  }

  return exec::test::PlanBuilder(planNodeIdGenerator_)
      .traceScan(
          nodeTraceDir_,
          pipelineIds_.front(),
          exec::trace::getDataType(planFragment_, nodeId_))
      .addNode(replayNodeFactory(replayNode))
      .planNode();
}

std::function<core::PlanNodePtr(std::string, core::PlanNodePtr)>
OperatorReplayerBase::replayNodeFactory(const core::PlanNode* node) const {
  return [=](const core::PlanNodeId& nodeId,
             const core::PlanNodePtr& source) -> core::PlanNodePtr {
    return createPlanNode(node, nodeId, source);
  };
}
} // namespace facebook::velox::tool::trace