// Copyright (c) 2024 PaddlePaddle Authors. All Rights Reserved.
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

#include "paddle/cinn/hlir/dialect/operator/transforms/group_merge/substitute_dim_expr_based_on_constraints_pass.h"

#include "paddle/cinn/common/dim_expr_util.h"
#include "paddle/cinn/common/union_find.h"

namespace cinn {
namespace dialect {
namespace ir {

namespace {

template <typename DoEachT>
void VisitEachOp(pir::ModuleOp module_op, const DoEachT& DoEach) {
  for (uint32_t i = 0; i < module_op->num_regions(); i++) {
    for (pir::Block& block : module_op->region(i)) {
      for (pir::Operation& op : block) {
        DoEach(op);
      }
    }
  }
}

template <typename DoEachT>
void VisitEachValue(const pir::Operation& op, const DoEachT& DoEach) {
  for (std::size_t i = 0; i < op.num_operands(); ++i) {
    DoEach(op.operand_source(i));
  }
  for (std::size_t i = 0; i < op.num_results(); ++i) {
    DoEach(op.result(i));
  }
}

symbol::TensorShapeOrDataDimExprs SubstituteTensorShapeOrData(
    const symbol::TensorShapeOrDataDimExprs& shape_or_data,
    const std::unordered_map<symbol::DimExpr, symbol::DimExpr>&
        substitution_pattern) {
  auto SubstituteOneDimExpr =
      [](const std::vector<symbol::DimExpr>& original_dim_expr,
         const std::unordered_map<symbol::DimExpr, symbol::DimExpr>&
             substitution_pattern) -> std::vector<symbol::DimExpr> {
    std::vector<symbol::DimExpr> substituted_dim_expr{};
    for (const symbol::DimExpr& dim_expr : original_dim_expr) {
      substituted_dim_expr.push_back(
          cinn::common::SubstituteDimExpr(dim_expr, substitution_pattern));
    }
    return substituted_dim_expr;
  };

  std::vector<symbol::DimExpr> substituted_shape =
      SubstituteOneDimExpr(shape_or_data.shape(), substitution_pattern);
  if (!shape_or_data.data().has_value()) {
    return symbol::ShapeOrData<symbol::DimExpr>(substituted_shape);
  } else {
    std::vector<symbol::DimExpr> substituted_data = SubstituteOneDimExpr(
        shape_or_data.data().value(), substitution_pattern);
    return symbol::ShapeOrData<symbol::DimExpr>(substituted_shape,
                                                substituted_data);
  }
}

symbol::ShapeOrDataDimExprs SubstituteShapeOrData(
    const symbol::ShapeOrDataDimExprs& shape_or_data,
    const std::unordered_map<symbol::DimExpr, symbol::DimExpr>&
        substitution_pattern) {
  auto lambdas = symbol::Overloaded{
      [&](const symbol::TensorShapeOrDataDimExprs& tensor_shape_or_data) {
        return symbol::ShapeOrDataDimExprs(SubstituteTensorShapeOrData(
            tensor_shape_or_data, substitution_pattern));
      },
      [&](const symbol::TensorListShapeOrDataDimExprs& tensor_list) {
        symbol::TensorListShapeOrDataDimExprs substituted_tensor_list;
        for (symbol::TensorShapeOrDataDimExprs tensor_shape_or_data :
             tensor_list) {
          substituted_tensor_list.push_back(SubstituteTensorShapeOrData(
              tensor_shape_or_data, substitution_pattern));
        }
        return symbol::ShapeOrDataDimExprs(substituted_tensor_list);
      }};
  return std::visit(lambdas, shape_or_data.variant());
}

std::unordered_map<symbol::DimExpr, symbol::DimExpr> GetDimExprSubstitution(
    pir::ShapeConstraintIRAnalysis* shape_analysis) {
  const std::vector<symbol::DimExprConstraint>& dim_expr_constraints =
      shape_analysis->CreateDimExprBuilder().constraints();
  const cinn::common::UnionFindSet<symbol::DimExpr>& union_find_set = [&]() {
    cinn::common::UnionFindSet<symbol::DimExpr> union_find_set;
    for (const auto& constraint : dim_expr_constraints) {
      CHECK(std::holds_alternative<symbol::Equal<symbol::DimExpr>>(constraint))
          << "The DimExprConstraint type is no Equal<DimExpr>, this part is to "
             "be completed.";
      const auto& data =
          std::get<symbol::Equal<symbol::DimExpr>>(constraint).data;
      union_find_set.Union(data->lhs, data->rhs);
    }
    return union_find_set;
  }();

  const std::vector<std::vector<symbol::DimExpr>>& dim_expr_clusters =
      union_find_set.Clusters();
  std::unordered_map<symbol::DimExpr, symbol::DimExpr> substitution_pattern;
  for (const auto& dim_expr_cluster : dim_expr_clusters) {
    CHECK(!dim_expr_cluster.empty());
    auto dim_expr_root = dim_expr_cluster[0];
    for (const auto& dim_expr : dim_expr_cluster) {
      if (std::holds_alternative<std::int64_t>(dim_expr)) {
        dim_expr_root = dim_expr;
        break;
      }
    }
    for (const auto& dim_expr : dim_expr_cluster) {
      if (dim_expr != dim_expr_root) {
        substitution_pattern[dim_expr] = dim_expr_root;
      }
    }
  }
  return substitution_pattern;
}

void SubstituteDimExprBasedOnConstraints(pir::ModuleOp module_op) {
  VLOG(4) << "SubstituteDimExprBasedOnConstraints start";
  pir::ShapeConstraintIRAnalysis shape_analysis =
      pir::ShapeAnalysisManager::Instance().Get(module_op.program());
  const std::unordered_map<symbol::DimExpr, symbol::DimExpr>&
      substitution_pattern = GetDimExprSubstitution(&shape_analysis);
  VisitEachOp(module_op, [&](pir::Operation& op) {
    VisitEachValue(op, [&](pir::Value value) {
      if (!shape_analysis.HasShapeOrDataForValue(value)) {
        VLOG(4) << "Can not find ShapeOrData for value of op(" << op.name()
                << ") in shape_analysis";
      } else {
        const symbol::ShapeOrDataDimExprs& origin_shape_or_data =
            shape_analysis.GetShapeOrDataForValue(value);
        const symbol::ShapeOrDataDimExprs& substituted_shape_or_data =
            SubstituteShapeOrData(origin_shape_or_data, substitution_pattern);
        shape_analysis.SetShapeOrDataForValue(value, substituted_shape_or_data);
      }
    });
    // TODO(JiaWenxuan): substitute the attribute "sym_shape_str" of the op
  });
  VLOG(4) << "SubstituteDimExprBasedOnConstraints end";
}

class SubstituteDimExprBasedOnConstraintsPass : public pir::Pass {
 public:
  SubstituteDimExprBasedOnConstraintsPass()
      : pir::Pass("substitute_dim_expr_based_on_constraints_pass", 1) {}

  void Run(pir::Operation* op) override {
    pir::ModuleOp module_op = op->dyn_cast<pir::ModuleOp>();
    SubstituteDimExprBasedOnConstraints(module_op);
  }

  bool CanApplyOn(pir::Operation* op) const override {
    return op->isa<pir::ModuleOp>() && op->num_regions() > 0;
  }
};

}  // namespace

std::unique_ptr<::pir::Pass> CreateSubstituteDimExprBasedOnConstraintsPass() {
  return std::make_unique<SubstituteDimExprBasedOnConstraintsPass>();
}

}  // namespace ir
}  // namespace dialect
}  // namespace cinn
