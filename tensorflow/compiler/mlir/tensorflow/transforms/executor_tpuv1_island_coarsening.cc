/* Copyright 2020 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

// This transformation pass takes TensorFlow executor dialect IslandOps and
// merges the one that contains operation marked to run on TPU.

#include <algorithm>
#include <iterator>
#include <queue>
#include <tuple>

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/None.h"
#include "llvm/ADT/Optional.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/iterator_range.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/Debug.h"
#include "mlir/IR/Attributes.h"  // from @llvm-project
#include "mlir/IR/Block.h"  // from @llvm-project
#include "mlir/IR/Builders.h"  // from @llvm-project
#include "mlir/IR/Location.h"  // from @llvm-project
#include "mlir/IR/Operation.h"  // from @llvm-project
#include "mlir/IR/SymbolTable.h"  // from @llvm-project
#include "mlir/IR/UseDefLists.h"  // from @llvm-project
#include "mlir/IR/Visitors.h"  // from @llvm-project
#include "mlir/Pass/Pass.h"  // from @llvm-project
#include "mlir/Pass/PassRegistry.h"  // from @llvm-project
#include "mlir/Support/LLVM.h"  // from @llvm-project
#include "tensorflow/compiler/mlir/tensorflow/ir/tf_executor.h"
#include "tensorflow/compiler/mlir/tensorflow/ir/tf_ops.h"
#include "tensorflow/compiler/mlir/tensorflow/transforms/passes.h"
#include "tensorflow/compiler/mlir/tensorflow/transforms/passes_detail.h"
#include "tensorflow/compiler/mlir/tensorflow/utils/attribute_utils.h"
#include "tensorflow/core/platform/logging.h"

#define DEBUG_TYPE "tf-executor-tpu-v1-island-coarsening"

namespace mlir {
namespace tf_executor {

namespace {

constexpr llvm::StringRef kTpuStatusAttr = "_tpu_compilation_status";
constexpr llvm::StringRef kNoReplicationCluster = "__no_replication_cluster";

// This pass is a variant of the island coarsening that is limited to
// TPU-annotated operations and intended to preserve backward compatibility with
// TFv1.
struct TpuV1BridgeExecutorIslandCoarsening
    : public TF::TpuV1BridgeExecutorIslandCoarseningPassBase<
          TpuV1BridgeExecutorIslandCoarsening> {
  void runOnOperation() override;
};

// Returns name of TPU cluster, if op belongs to a TPU cluster. Otherwise,
// returns `llvm::None`.
llvm::Optional<llvm::StringRef> GetTpuClusterName(Operation* op) {
  if (auto tpu_status = op->getAttrOfType<StringAttr>(kTpuStatusAttr)) {
    // Borrow cluster name from TPU status (for `TPUCompilationResult` op).
    return tpu_status.getValue();
  }
  auto device_type = op->getAttrOfType<StringAttr>(TF::kCompileDeviceTypeAttr);
  if (!device_type || device_type.getValue() != TF::kTpuDevice) {
    // Op does not belong to a TPU cluster.
    return llvm::None;
  }
  // Op belongs to a TPU cluster.
  if (auto replication_info =
          op->getAttrOfType<StringAttr>(TF::kReplicationInfoAttr)) {
    // Borrow cluster name from replication info.
    return replication_info.getValue();
  }
  // Use special cluster name for non-replicated case.
  return kNoReplicationCluster;
}

bool HasDataDependencyWithUnscheduledOp(
    Operation& op, Block* block, SmallPtrSet<Operation*, 16>& unscheduled_ops) {
  WalkResult ready_to_schedule = op.walk([&](Operation* nested_op) {
    for (Value operand : nested_op->getOperands()) {
      Operation* defining_op = operand.getDefiningOp();
      if (!defining_op) continue;
      Operation* producer_in_block = block->findAncestorOpInBlock(*defining_op);
      if (producer_in_block && producer_in_block != &op &&
          unscheduled_ops.count(producer_in_block)) {
        // Found an operand that isn't scheduled yet, interrupt the walk.
        return WalkResult::interrupt();
      }
    }
    return WalkResult::advance();
  });
  return ready_to_schedule.wasInterrupted();
}

bool HasControlDependencyWithUnscheduledOp(
    Operation& op, Block* block, SmallPtrSet<Operation*, 16>& unscheduled_ops) {
  IslandOp island_op = dyn_cast<IslandOp>(op);
  if (!island_op) {
    return false;
  }
  for (Value input : island_op.controlInputs()) {
    Operation* defining_op = input.getDefiningOp();
    if (!defining_op) continue;
    Operation* producer_in_block = block->findAncestorOpInBlock(*defining_op);
    if (producer_in_block && producer_in_block != &op &&
        unscheduled_ops.count(producer_in_block)) {
      // Found an operand that isn't scheduled yet, return true.
      return true;
    }
  }
  return false;
}

// Sorts the operations in the provided range to enforce dominance.
// This is useful after fusing / reorganizing Operations in a block and later
// needing to readjust the ordering to ensure dominance.
LogicalResult SortTopologically(Block::iterator begin, Block::iterator end) {
  Block* block = begin->getBlock();
  // Either sort from `begin` to end of block or both `begin` and
  // `end` should belong to the same block.
  assert(end == block->end() ||
         end->getBlock() == block && "ops must be in the same block");

  // Track the ops that still need to be scheduled in a set.
  SmallPtrSet<Operation*, 16> unscheduled_ops;
  for (Operation& op : llvm::make_range(begin, end))
    unscheduled_ops.insert(&op);

  Block::iterator last_scheduled_op = begin;
  while (!unscheduled_ops.empty()) {
    bool scheduled_at_least_once = false;
    // Loop over the ops that are not sorted yet, try to find the ones "ready",
    // i.e. the ones for which there aren't any operand produced by an op in the
    // set, and "schedule" it (move it before the last_scheduled_op).
    for (Operation& op : llvm::make_range(last_scheduled_op, end)) {
      if (HasDataDependencyWithUnscheduledOp(op, block, unscheduled_ops) ||
          HasControlDependencyWithUnscheduledOp(op, block, unscheduled_ops)) {
        continue;
      }
      unscheduled_ops.erase(&op);
      if (Block::iterator(op) != last_scheduled_op)
        op.moveBefore(block, last_scheduled_op);
      else
        ++last_scheduled_op;
      scheduled_at_least_once = true;
    }
    if (!scheduled_at_least_once) return failure();
  }
  return success();
}

// Looks for an IslandOp that wraps a single operation tagged with the
// _replication_info attribute, and merges it with all the following operations
// in the block. Sets the `changed` boolean to true if any island is merged.
// Returns a failure if a cycle prevents the merge from happening correctly
// without breaking dominance. The IR is left in invalid state in case of
// failure.
void CollectCandidateIslands(
    llvm::function_ref<bool(llvm::StringRef, Operation*)>
        is_op_calling_func_for_cluster,
    Operation* op, StringRef cluster_name,
    SmallPtrSet<Operation*, 16>& islands_set,
    SmallPtrSet<Operation*, 16>& wrapped_ops) {
  for (Operation& candidate_op : llvm::make_early_inc_range(
           llvm::make_range(op->getIterator(), op->getBlock()->end()))) {
    IslandOp candidate_island = dyn_cast<IslandOp>(candidate_op);
    if (!candidate_island || !candidate_island.WrapsSingleOp()) continue;
    // Check if we have an operation with the expected attribute.
    Operation& candidate_wrapped_op = candidate_island.GetBody().front();

    // The op might be a special TPU input/output op and may have been already
    // added to the list of islands to be merged.
    if (wrapped_ops.contains(&candidate_wrapped_op)) {
      continue;
    }

    llvm::Optional<llvm::StringRef> result =
        GetTpuClusterName(&candidate_wrapped_op);
    llvm::StringRef candidate_cluster_name;
    if (result.hasValue()) {
      candidate_cluster_name = result.getValue();
    } else if (is_op_calling_func_for_cluster(cluster_name,
                                              &candidate_wrapped_op)) {
      candidate_cluster_name = cluster_name;
    }
    if (candidate_cluster_name != cluster_name) continue;

    // Add the current op to the set of ops which are planned to be merged into
    // one cluster.
    islands_set.insert(candidate_island);
    wrapped_ops.insert(&candidate_wrapped_op);
  }
}

IslandOp CreateMergedIsland(IslandOp island, SmallVector<IslandOp, 16>& islands,
                            SmallPtrSet<Operation*, 16>& wrapped_ops) {
  // Compute the result of the merged island, these are the values produced by
  // the islands that are merged if they have a use in an island not merged,
  // i.e. a value that escapes.
  llvm::SmallVector<Type, 4> result_types;
  for (IslandOp new_op : islands) {
    for (Value result : new_op.outputs()) {
      if (llvm::any_of(result.getUsers(), [&](OpOperand user) {
            return !wrapped_ops.count(user.getOwner());
          }))
        result_types.push_back(result.getType());
    }
  }

  IslandOp new_island = OpBuilder(island).create<IslandOp>(
      island.getLoc(), result_types,
      /*control=*/ControlType::get(island.getContext()),
      /*controlInputs=*/island.getOperands());
  new_island.body().push_back(new Block);

  // Move the operations in the new island, gather the results of the new yield.
  Block& island_body = new_island.GetBody();
  SmallVector<Value, 16> yield_operands;
  for (IslandOp island : islands) {
    Operation& wrapped_op = island.GetBody().front();
    wrapped_op.moveBefore(&island_body, island_body.end());

    // For every result of the wrapped_op, it needs to get passed to the yield
    // operation, only if it escapes the island.
    for (auto result : llvm::zip(island.outputs(), wrapped_op.getResults())) {
      if (llvm::any_of(std::get<0>(result).getUsers(), [&](OpOperand user) {
            return !wrapped_ops.count(user.getOwner());
          }))
        yield_operands.push_back(std::get<1>(result));
    }
  }
  OpBuilder::atBlockEnd(&island_body)
      .create<YieldOp>(new_island.getLoc(), yield_operands);

  // remap results of the new islands to the user outside of the island.
  int current_result = 0;
  Value control = new_island.control();
  for (IslandOp island : islands) {
    YieldOp yield_op = island.GetYield();
    for (const auto& idx_result : llvm::enumerate(island.outputs())) {
      Value result = idx_result.value();

      bool has_external_use = false;
      for (OpOperand& use : llvm::make_early_inc_range(result.getUses())) {
        if (wrapped_ops.count(use.getOwner()))
          use.set(yield_op.getOperand(idx_result.index()));
        else
          has_external_use = true;
      }
      if (has_external_use) {
        result.replaceAllUsesWith(new_island.getResult(current_result));
        ++current_result;
      }
    }
    island.control().replaceAllUsesWith(control);
    island.erase();
  }
  return new_island;
}

// Looks for an IslandOp that wraps a single operation tagged with the
// _replication_info attribute, and merges it with all the following operations
// in the block. Sets the `changed` boolean to true if any island is merged.
// Returns a failure if a cycle prevents the merge from happening correctly
// without breaking dominance. The IR is left in invalid state in case of
// failure.
LogicalResult MergeIsland(
    llvm::function_ref<bool(StringRef, Operation*)>
        is_op_calling_func_for_cluster,
    llvm::SmallDenseMap<StringRef, llvm::SmallDenseSet<Operation*>>&
        cluster_to_tpu_ops_map,
    Operation* op, bool* changed) {
  // Find the first island wrapping a single operation with the
  // `_replication_info` attribute, it'll be used as the root of the algorithm
  // to find the other operations that are part of the same cluster.
  IslandOp island = dyn_cast<IslandOp>(*op);
  if (!island || !island.WrapsSingleOp()) return success();
  Operation& wrapped_op = island.GetBody().front();

  llvm::Optional<llvm::StringRef> result = GetTpuClusterName(&wrapped_op);
  if (!result.hasValue()) return success();
  llvm::StringRef cluster_name = result.getValue();

  // We found a _replication_info, let's build an island for the full cluster!
  LLVM_DEBUG(llvm::dbgs() << "Processing candidate island: "
                          << *island.getOperation() << "\n");

  // Collect the islands to merge together in this new cluster starting with the
  // given island.
  SmallVector<IslandOp, 16> islands;
  SmallPtrSet<Operation*, 16> islands_set;
  SmallPtrSet<Operation*, 16> wrapped_ops;

  CollectCandidateIslands(is_op_calling_func_for_cluster, op, cluster_name,
                          islands_set, wrapped_ops);

  if (cluster_to_tpu_ops_map.count(cluster_name)) {
    for (auto tpu_op : cluster_to_tpu_ops_map[cluster_name]) {
      islands_set.insert(tpu_op);
      wrapped_ops.insert(&dyn_cast<IslandOp>(*tpu_op).GetBody().front());
    }
  }

  // Get the sequential order of the candidate islands in the block.
  // Later we can merge the candidate islands in this order.
  // The dominance is guaranteed in this order.
  for (Operation& candidate_op : llvm::make_early_inc_range(
           llvm::make_range(op->getBlock()->begin(), op->getBlock()->end()))) {
    IslandOp candidate_island = dyn_cast<IslandOp>(candidate_op);
    if (!candidate_island || !candidate_island.WrapsSingleOp()) continue;
    if (islands_set.contains(candidate_island)) {
      islands.push_back(candidate_island);
    }
  }

  // If no other island was found to merge with the existing one, just move on.
  if (islands.size() <= 1) return success();

  *changed = true;
  Operation* first_op_after = islands.back()->getNextNode();

  // We create the merged island at the location of the first island that was
  // merged (excluding special TPU input/output ops).
  IslandOp new_island = CreateMergedIsland(island, islands, wrapped_ops);

  // Ensure dominance by sorting the range of islands that were merged.
  return SortTopologically(Block::iterator(new_island.getOperation()),
                           Block::iterator(first_op_after));
}

// Returns all functions that can be reached from TPUPartitionedCall ops.
SmallPtrSet<Operation*, 16> FindTPUPartitionedCallReachableFunctions(
    ModuleOp module) {
  SymbolTableCollection table;
  SymbolUserMap symbol_map(table, module);
  llvm::DenseMap<func::FuncOp, llvm::DenseSet<func::FuncOp>> caller_callee_map;
  // Creates work queue for determining reachability below.
  std::queue<func::FuncOp> function_worklist;

  for (auto func : module.getOps<func::FuncOp>()) {
    for (auto user : symbol_map.getUsers(func)) {
      // Populates work queue with func ops called from TPUPartionedCall.
      if (llvm::isa<TF::TPUPartitionedCallOp>(user)) {
        function_worklist.push(func);
      }
      // Populates caller to called func map.
      if (func::FuncOp caller = user->getParentOfType<func::FuncOp>()) {
        caller_callee_map[caller].insert(func);
      }
    }
  }

  // Determines reached ops starting from TPUPartionedCall ops
  // and iteratively descending through called ops.
  SmallPtrSet<Operation*, 16> reachable_functions;
  while (!function_worklist.empty()) {
    func::FuncOp caller = function_worklist.front();
    function_worklist.pop();
    if (reachable_functions.insert(caller).second) {
      for (auto callee : caller_callee_map[caller]) {
        function_worklist.push(callee);
      }
    }
  }
  return reachable_functions;
}

// Looks at captured operands of `candidate_wrapped_op` to bring special TPU
// ops such as tf.TPUReplicatedInput and tf.TPUPartitionedInput into the
// island as well. These ops are brought in only if they do not already
// have a cluster assigned to them (via `_replication_info` attribute value).
void AddSpecialTpuInputOps(
    IslandOp candidate_island, llvm::StringRef cluster_name,
    llvm::SmallDenseMap<llvm::StringRef, llvm::SmallDenseSet<Operation*>>&
        cluster_to_tpu_op_map,
    SmallPtrSetImpl<Operation*>& visited_wrapped_ops) {
  auto has_all_outputs_consumed_by_cluster = [&](IslandOp island) -> bool {
    for (Value result : island->getResults()) {
      for (auto user : result.getUsers()) {
        if (!visited_wrapped_ops.contains(user)) {
          return false;
        }
      }
    }
    return true;
  };
  Operation* candidate_wrapped_op = &candidate_island.GetBody().front();
  std::queue<Operation*> op_worklist;
  op_worklist.push(candidate_wrapped_op);
  while (!op_worklist.empty()) {
    Operation* current_op = op_worklist.front();
    op_worklist.pop();
    for (Value operand : current_op->getOperands()) {
      IslandOp wrapper = dyn_cast_or_null<IslandOp>(operand.getDefiningOp());
      if (!wrapper || !wrapper.WrapsSingleOp()) continue;
      Operation& wrapped_op = wrapper.GetBody().front();
      if (!isa<TF::TPUReplicatedInputOp, TF::TPUPartitionedInputOp>(wrapped_op))
        continue;
      // Only inputs that do not have a cluster name assigned are considered for
      // special handling. Otherwise, island coarsening logic should be able to
      // handle it.
      if (wrapped_op.hasAttrOfType<StringAttr>(TF::kReplicationInfoAttr)) {
        continue;
      }
      if (!has_all_outputs_consumed_by_cluster(wrapper)) {
        continue;
      }
      if (visited_wrapped_ops.contains(&wrapped_op)) {
        continue;
      }
      op_worklist.push(&wrapped_op);
      cluster_to_tpu_op_map[cluster_name].insert(wrapper);
      visited_wrapped_ops.insert(&wrapped_op);
    }
  }
}

// Looks at the results of `candidate_island` to bring special TPU
// ops such as tf.TPUReplicatedOutput and tf.TPUPartitionedOutput into the
// island as well. These ops are brought in only if they do not already have
// cluster (`_tpu_replicate` attribute) assigned to them.
// TODO(hanxiongwang): Consider de-duping this with the AddSpecialTpuInputOps
// function above.
void AddSpecialTpuOutputOps(
    IslandOp candidate_island, llvm::StringRef cluster_name,
    llvm::SmallDenseMap<llvm::StringRef, llvm::SmallDenseSet<Operation*>>&
        cluster_to_tpu_op_map,
    SmallPtrSetImpl<Operation*>& visited_wrapped_ops) {
  auto only_has_inputs_from_cluster = [&](Operation* op) -> bool {
    for (Value operand : op->getOperands()) {
      IslandOp defining_island =
          llvm::dyn_cast_or_null<IslandOp>(operand.getDefiningOp());
      if (!defining_island) return false;
      Operation& defining_wrapped_op = defining_island.GetBody().front();
      if (!visited_wrapped_ops.contains(&defining_wrapped_op)) return false;
    }
    return true;
  };

  std::queue<IslandOp> island_worklist;
  island_worklist.push(candidate_island);
  while (!island_worklist.empty()) {
    IslandOp current_island = island_worklist.front();
    island_worklist.pop();
    for (Value result : current_island.getResults()) {
      for (OpOperand use : result.getUsers()) {
        Operation* user = use.getOwner();
        IslandOp wrapper = dyn_cast_or_null<IslandOp>(user->getParentOp());
        if (!wrapper || !wrapper.WrapsSingleOp()) continue;
        if (!isa<TF::TPUReplicatedOutputOp, TF::TPUPartitionedOutputOp>(user))
          continue;
        // Only users that do not have a cluster name assigned are considered
        // for special handling. Otherwise, island coarsening logic should be
        // able to handle it.
        if (user->hasAttrOfType<StringAttr>(TF::kReplicationInfoAttr)) continue;
        if (!only_has_inputs_from_cluster(user)) continue;
        if (visited_wrapped_ops.contains(user)) continue;
        visited_wrapped_ops.insert(user);
        cluster_to_tpu_op_map[cluster_name].insert(wrapper);
        island_worklist.push(wrapper);
      }
    }
  }
}

LogicalResult CollectSpecialTpuOps(
    llvm::function_ref<bool(llvm::StringRef, Operation*)>
        is_op_calling_func_for_cluster,
    Operation* op,
    llvm::SmallDenseMap<llvm::StringRef, llvm::SmallDenseSet<Operation*>>&
        cluster_to_tpu_op_map,
    SmallPtrSet<Operation*, 16>& visited_wrapped_ops) {
  IslandOp island = dyn_cast<IslandOp>(*op);
  if (!island || !island.WrapsSingleOp()) return success();
  Operation& wrapped_op = island.GetBody().front();

  if (visited_wrapped_ops.contains(&wrapped_op)) return success();

  llvm::Optional<llvm::StringRef> result = GetTpuClusterName(&wrapped_op);
  if (!result.hasValue()) return success();
  llvm::StringRef cluster_name = result.getValue();

  visited_wrapped_ops.insert(&wrapped_op);

  AddSpecialTpuInputOps(island, cluster_name, cluster_to_tpu_op_map,
                        visited_wrapped_ops);
  AddSpecialTpuOutputOps(island, cluster_name, cluster_to_tpu_op_map,
                         visited_wrapped_ops);

  return success();
}

void TpuV1BridgeExecutorIslandCoarsening::runOnOperation() {
  SymbolTable symbol_table(getOperation());

  // Map tpu cluster names to the functions that contain operations for this
  // cluster.
  DenseMap<StringRef, DenseSet<func::FuncOp>> tpu_funcs;
  for (func::FuncOp func_op : getOperation().getOps<func::FuncOp>()) {
    func_op.walk([&](Operation* op) {
      llvm::Optional<llvm::StringRef> cluster_name_opt = GetTpuClusterName(op);
      if (cluster_name_opt.hasValue()) {
        tpu_funcs[cluster_name_opt.getValue()].insert(func_op);
      }
    });
  }

  // Return true if the operation is containing a reference to a function
  // containing operations for this cluster.
  auto is_op_calling_func_for_cluster = [&](llvm::StringRef cluster,
                                            Operation* op) {
    auto funcs_for_cluster = tpu_funcs.find(cluster);
    assert(funcs_for_cluster != tpu_funcs.end());
    assert(!funcs_for_cluster->second.empty());
    if (funcs_for_cluster->second.size() == 1) return false;
    for (NamedAttribute attr : op->getAttrs()) {
      auto symbol_ref = attr.getValue().dyn_cast<FlatSymbolRefAttr>();
      if (!symbol_ref) continue;
      func::FuncOp callee =
          symbol_table.lookup<func::FuncOp>(symbol_ref.getValue());
      if (!callee) continue;
      if (funcs_for_cluster->second.count(callee)) return true;
    }
    return false;
  };

  // Populates skip set with functions reachable from TPUPartionedCall ops.
  const auto functions_to_skip =
      FindTPUPartitionedCallReachableFunctions(getOperation());
  for (func::FuncOp func_op : getOperation().getOps<func::FuncOp>()) {
    if (functions_to_skip.contains(func_op)) {
      OpBuilder builder(func_op);
      // Mark this function as being skipped in island outlining.
      func_op->setAttr(mlir::TF::kSkipIslandOutlining,
                       builder.getBoolAttr(true));
      continue;
    }

    func_op.walk([&](GraphOp graph) {
      Block& graph_body = graph.GetBody();
      llvm::SmallDenseMap<llvm::StringRef, llvm::SmallDenseSet<Operation*>>
          cluster_to_tpu_ops_map;
      SmallPtrSet<Operation*, 16> visited_ops;
      for (Operation& op : graph_body) {
        if (failed(CollectSpecialTpuOps(is_op_calling_func_for_cluster, &op,
                                        cluster_to_tpu_ops_map, visited_ops))) {
          graph.emitError() << "Collect special Tpu ops failed: "
                            << "Graph contains some unsupported ops\n";
          signalPassFailure();
          return WalkResult::interrupt();
        }
      }
      // Iterate until fixed point on the block, as it may contain multiple
      // clusters.
      bool changed = true;
      while (changed) {
        changed = false;
        for (Operation& op : graph_body) {
          if (failed(MergeIsland(is_op_calling_func_for_cluster,
                                 cluster_to_tpu_ops_map, &op, &changed))) {
            graph.emitError()
                << "Merging island failed: the TPU cluster likely "
                << "contains a cycle with non-TPU operations or has "
                   "unsupported ops\n";
            signalPassFailure();
            return WalkResult::interrupt();
          }
          // If islands were merged, restart scanning the block from the
          // beginning as we lost track of where to continue.
          if (changed) break;
        }
      }
      return WalkResult::advance();
    });
  }
}

}  // namespace

std::unique_ptr<OperationPass<ModuleOp>>
CreateTFExecutorTPUV1IslandCoarseningPass() {
  return std::make_unique<TpuV1BridgeExecutorIslandCoarsening>();
}

}  // namespace tf_executor
}  // namespace mlir
