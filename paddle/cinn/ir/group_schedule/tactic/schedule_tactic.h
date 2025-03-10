// Copyright (c) 2023 CINN Authors. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include <string>
#include "paddle/cinn/common/integer_set.h"
#include "paddle/cinn/ir/ir.h"
#include "paddle/cinn/ir/schedule/ir_schedule.h"
#include "paddle/cinn/ir/schedule_block_graph.h"

namespace cinn {
namespace ir {

struct IterativeSpaceInfo {
  enum class AxisType : int {
    kSerial = 0,
    kCudaThreadX = 1,
    kCudaThreadY = 2,
    kCudaThreadZ = 3,
    kCudaBlockX = 4,
    kCudaBlockY = 5,
    kCudaBlockZ = 6,
  };
  // pure spatial iterative space
  std::vector<std::tuple<ir::Expr, AxisType>> sp_space;
  // reduce or broadcast iterative space
  std::vector<std::tuple<ir::Expr, AxisType>> rb_space;
  // total sp extent
  ir::Expr total_sp_extent;
  // total rb extent
  ir::Expr total_rb_extent;
  // original loop order with same iteration order as the memory order
  std::vector<ir::Expr> memory_consistent_order_space;
  // index that transform from memory consistent order to rb last order
  // for example:
  // the memory consistent order axis is [A, B, C], and the B axis is reduce，
  // the rb last order axis is [A, C, B], and rb_last_order is [0, 2, 1].
  std::vector<int> rb_last_order;

  std::string PrintIterSpace() const {
    std::stringstream ss;
    ss << "[sp space]: ";
    for (const auto& axis : sp_space) {
      ss << "<" << std::get<0>(axis) << ", AxisType = ["
         << static_cast<int>(std::get<1>(axis)) << "]>  ";
    }
    ss << "\n[rb space]: ";
    for (const auto& axis : rb_space) {
      ss << "<" << std::get<0>(axis) << ", AxisType = ["
         << static_cast<int>(std::get<1>(axis)) << "]>  ";
    }
    return ss.str();
  }
};

struct BucketInfo {
  int sp_lower_bound = 0;
  int sp_upper_bound = UINT_MAX;
  int rb_lower_bound = 0;
  int rb_upper_bound = UINT_MAX;
};

struct GroupTileInfo {
  GroupTileInfo() {}

  std::vector<int64_t> reduce_axis_;
  int64_t data_rank;

  int64_t block_num{-1};
  int64_t warp_num;
  int64_t spatial_inner_num;
  int64_t reduce_numel;
  int64_t reduce_inner_num;
  int64_t reduce_block;

  std::set<std::string> reduce_tensor_names;
  std::set<std::string> temp_var_names;

  std::set<std::string> shared_var_names;
  std::set<std::string> direct_output_var_names;
  std::vector<std::string> thread_sync_before_names;

  ReduceMethod reduce_method{NoneReduceMethod()};

  std::unordered_map<std::string, BroadcastInfo> broadcast_info;
  std::unordered_map<std::string, BroadcastInfo> broadcast_to_elementwise;
};

struct ScheduleContext {
  // TODO(BiynXu): Unify fields with similar meanings
  std::unordered_set<std::string> output_names;
  Target target;
  IterativeSpaceInfo iter_space_info;
  BucketInfo bucket_info;
  // Will tile information be modified during the schedule process?
  // If so, it is necessary to store a separate copy for each context
  std::shared_ptr<GroupTileInfo> group_tile_info;
};

class ScheduleTactic {
 public:
  virtual void Init(ScheduleContext* context) = 0;

  virtual void Apply(ir::IRSchedule* sch, const std::string& block_id) = 0;

  virtual std::string TacticName() const = 0;
};

}  // namespace ir
}  // namespace cinn
