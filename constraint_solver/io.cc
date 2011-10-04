// Copyright 2010-2011 Google
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

#include <stddef.h>
#include <algorithm>
#include "base/hash.h"
#include <string>
#include <utility>
#include <vector>

#include "base/callback.h"
#include "base/integral_types.h"
#include "base/logging.h"
#include "base/scoped_ptr.h"
#include "base/concise_iterator.h"
#include "base/map-util.h"
#include "base/stl_util.h"
#include "base/hash.h"
#include "constraint_solver/constraint_solver.h"
#include "constraint_solver/constraint_solveri.h"
#include "constraint_solver/model.pb.h"
#include "constraint_solver/search_limit.pb.h"
#include "util/vector_map.h"

namespace operations_research {
namespace {
// ---------- Model Protobuf Writers -----------

// ----- First Pass visitor -----

// This visitor collects all constraints and expressions.  It sorts the
// expressions, such that we can build them in sequence using
// previously created expressions.
class FirstPassVisitor : public ModelVisitor {
 public:
  FirstPassVisitor() {}  // Needed for Visual Studio.
  virtual ~FirstPassVisitor() {}

  // Begin/End visit element.
  virtual void BeginVisitModel(const string& solver_name) {
    // Reset statistics.
    expression_map_.clear();
    delegate_map_.clear();
    expression_list_.clear();
    constraint_list_.clear();
    interval_list_.clear();
  }

  virtual void EndVisitConstraint(const string& type_name,
                                  const Constraint* const constraint) {
    Register(constraint);
  }

  virtual void EndVisitIntegerExpression(const string& type_name,
                                         const IntExpr* const expression) {
    Register(expression);
  }

  virtual void VisitIntegerVariable(const IntVar* const variable,
                                    const IntExpr* const delegate) {
    if (delegate != NULL) {
      delegate->Accept(this);
      delegate_map_[variable] = delegate;
    }
    Register(variable);
  }

  virtual void VisitIntervalVariable(const IntervalVar* const variable,
                                     const string operation,
                                     const IntervalVar* const delegate) {
    if (delegate != NULL) {
      delegate->Accept(this);
    }
    Register(variable);
  }

  virtual void VisitIntervalVariable(const IntervalVar* const variable,
                                     const string operation,
                                     const IntervalVar* const * delegates,
                                     int size) {
    for (int i = 0; i < size; ++i) {
      delegates[i]->Accept(this);
    }
    Register(variable);
  }

  // Visit integer expression argument.
  virtual void VisitIntegerExpressionArgument(
      const string& arg_name,
      const IntExpr* const argument) {
    VisitSubArgument(argument);
  }

  virtual void VisitIntegerVariableArrayArgument(
      const string& arg_name,
      const IntVar* const * arguments,
      int size) {
    for (int i = 0; i < size; ++i) {
      VisitSubArgument(arguments[i]);
    }
  }

  // Visit interval argument.
  virtual void VisitIntervalArgument(const string& arg_name,
                                     const IntervalVar* const argument) {
    VisitSubArgument(argument);
  }

  virtual void VisitIntervalArrayArgument(const string& arg_name,
                                          const IntervalVar* const * arguments,
                                          int size) {
    for (int i = 0; i < size; ++i) {
      VisitSubArgument(arguments[i]);
    }
  }

  // Export
  const hash_map<const IntExpr*, int>& expression_map() const {
    return expression_map_;
  }
  const hash_map<const IntervalVar*, int>& interval_map() const {
    return interval_map_;
  }
  const hash_map<const IntVar*, const IntExpr*>& delegate_map() const {
    return delegate_map_;
  }
  const std::vector<const IntExpr*>& expression_list() const {
    return expression_list_;
  }
  const std::vector<const Constraint*>& constraint_list() const {
    return constraint_list_;
  }
  const std::vector<const IntervalVar*>& interval_list() const {
    return interval_list_;
  }

 private:
  void Register(const IntExpr* const expression) {
    if (!ContainsKey(expression_map_, expression)) {
      const int index = expression_map_.size();
      CHECK_EQ(index, expression_list_.size());
      expression_map_[expression] = index;
      expression_list_.push_back(expression);
    }
  }

  void Register(const Constraint* const constraint) {
    constraint_list_.push_back(constraint);
  }

  void Register(const IntervalVar* const interval) {
    if (!ContainsKey(interval_map_, interval)) {
      const int index = interval_map_.size();
      CHECK_EQ(index, interval_list_.size());
      interval_map_[interval] = index;
      interval_list_.push_back(interval);
    }
  }

  void VisitSubArgument(const IntExpr* const expression) {
    if (!ContainsKey(expression_map_, expression)) {
      expression->Accept(this);
    }
  }

  void VisitSubArgument(const IntervalVar* const interval) {
    if (!ContainsKey(interval_map_, interval)) {
      interval->Accept(this);
    }
  }

  const string filename_;
  hash_map<const IntExpr*, int> expression_map_;
  hash_map<const IntervalVar*, int> interval_map_;
  hash_map<const IntVar*, const IntExpr*> delegate_map_;
  std::vector<const IntExpr*> expression_list_;
  std::vector<const Constraint*> constraint_list_;
  std::vector<const IntervalVar*> interval_list_;
};

// ----- Argument Holder -----

class ArgumentHolder {
 public:
  template <class P> void ExportToProto(VectorMap<string>* const tags,
                                        P* const proto) const {
    for (ConstIter<hash_map<string, int64> > it(integer_argument_);
         !it.at_end();
         ++it) {
      CPArgumentProto* const arg_proto = proto->add_arguments();
      arg_proto->set_argument_index(tags->Add(it->first));
      arg_proto->set_integer_value(it->second);
    }

    for (ConstIter<hash_map<string, std::vector<int64> > > it(
             integer_array_argument_); !it.at_end(); ++it) {
      CPArgumentProto* const arg_proto = proto->add_arguments();
      arg_proto->set_argument_index(tags->Add(it->first));
      for (int i = 0; i < it->second.size(); ++i) {
        arg_proto->add_integer_array(it->second[i]);
      }
    }

    for (ConstIter<hash_map<string, std::pair<int, std::vector<int64> > > > it(
             integer_matrix_argument_); !it.at_end(); ++it) {
      CPArgumentProto* const arg_proto = proto->add_arguments();
      arg_proto->set_argument_index(tags->Add(it->first));
      CPIntegerMatrixProto* const matrix_proto =
          arg_proto->mutable_integer_matrix();
      const int columns = it->second.first;
      CHECK_GT(columns, 0);
      const int rows = it->second.second.size() / columns;
      matrix_proto->set_rows(rows);
      matrix_proto->set_columns(columns);
      for (int i = 0; i < it->second.second.size(); ++i) {
        matrix_proto->add_values(it->second.second[i]);
      }
    }

    for (ConstIter<hash_map<string, int> > it(
             integer_expression_argument_); !it.at_end(); ++it) {
      CPArgumentProto* const arg_proto = proto->add_arguments();
      arg_proto->set_argument_index(tags->Add(it->first));
      arg_proto->set_integer_expression_index(it->second);
    }

    for (ConstIter<hash_map<string, std::vector<int> > > it(
             integer_variable_array_argument_); !it.at_end(); ++it) {
      CPArgumentProto* const arg_proto = proto->add_arguments();
      arg_proto->set_argument_index(tags->Add(it->first));
      for (int i = 0; i < it->second.size(); ++i) {
        arg_proto->add_integer_expression_array(it->second[i]);
      }
    }

    for (ConstIter<hash_map<string, int> > it(interval_argument_);
         !it.at_end();
         ++it) {
      CPArgumentProto* const arg_proto = proto->add_arguments();
      arg_proto->set_argument_index(tags->Add(it->first));
      arg_proto->set_interval_index(it->second);
    }

    for (ConstIter<hash_map<string, std::vector<int> > > it(
             interval_array_argument_); !it.at_end(); ++it) {
      CPArgumentProto* const arg_proto = proto->add_arguments();
      arg_proto->set_argument_index(tags->Add(it->first));
      for (int i = 0; i < it->second.size(); ++i) {
        arg_proto->add_interval_array(it->second[i]);
      }
    }
  }

  const string& type_name() const {
    return type_name_;
  }

  void set_type_name(const string& type_name) {
    type_name_ = type_name;
  }

  void set_integer_argument(const string& arg_name, int64 value) {
    integer_argument_[arg_name] = value;
  }

  void set_integer_array_argument(const string& arg_name,
                                  const int64* const values,
                                  int size) {
    for (int i = 0; i < size; ++i) {
      integer_array_argument_[arg_name].push_back(values[i]);
    }
  }

  void set_integer_matrix_argument(const string& arg_name,
                                   const int64* const * const values,
                                   int rows,
                                   int columns) {
    std::pair<int, std::vector<int64> > matrix = make_pair(columns, std::vector<int64>());
    integer_matrix_argument_[arg_name] = matrix;
    std::vector<int64>* const vals = &integer_matrix_argument_[arg_name].second;
    for (int i = 0; i < rows; ++i) {
      for (int j = 0; j < columns; ++j) {
        vals->push_back(values[i][j]);
      }
    }
  }

  void set_integer_expression_argument(const string& arg_name, int index) {
    integer_expression_argument_[arg_name] = index;
  }

  void set_integer_variable_array_argument(const string& arg_name,
                                           const int* const indices,
                                           int size) {
    for (int i = 0; i < size; ++i) {
      integer_variable_array_argument_[arg_name].push_back(indices[i]);
    }
  }

  void set_interval_argument(const string& arg_name, int index) {
    interval_argument_[arg_name] = index;
  }

  void set_interval_array_argument(const string& arg_name,
                                   const int* const indices,
                                   int size) {
    for (int i = 0; i < size; ++i) {
      interval_array_argument_[arg_name].push_back(indices[i]);
    }
  }

  int64 FindIntegerArgumentWithDefault(const string& arg_name, int64 def) {
    return FindWithDefault(integer_argument_, arg_name, def);
  }

  int64 FindIntegerArgumentOrDie(const string& arg_name) {
    return FindOrDie(integer_argument_, arg_name);
  }

  int64 FindIntegerExpressionArgumentOrDie(const string& arg_name) {
    return FindOrDie(integer_expression_argument_, arg_name);
  }

 private:
  string type_name_;
  hash_map<string, int> integer_expression_argument_;
  hash_map<string, int64> integer_argument_;
  hash_map<string, int> interval_argument_;
  hash_map<string, std::vector<int64> > integer_array_argument_;
  hash_map<string, std::pair<int, std::vector<int64> > > integer_matrix_argument_;
  hash_map<string, std::vector<int> > integer_variable_array_argument_;
  hash_map<string, std::vector<int> > interval_array_argument_;
};

// ----- Second Pass Visitor -----

static const int kModelVersion = 1;

// The second pass visitor will visited sorted expressions, interval
// vars and expressions and export them to a CPModelProto protocol
// buffer.
class SecondPassVisitor : public ModelVisitor {
 public:
  SecondPassVisitor(const FirstPassVisitor& first_pass,
                    CPModelProto* const model_proto)
      : expression_map_(first_pass.expression_map()),
        interval_map_(first_pass.interval_map()),
        delegate_map_(first_pass.delegate_map()),
        expression_list_(first_pass.expression_list()),
        constraint_list_(first_pass.constraint_list()),
        interval_list_(first_pass.interval_list()),
        model_proto_(model_proto) {}

  virtual ~SecondPassVisitor() {}

  virtual void BeginVisitModel(const string& model_name) {
    model_proto_->set_model(model_name);
    model_proto_->set_version(kModelVersion);
    PushArgumentHolder();
    for (ConstIter<std::vector<const IntExpr*> > it(expression_list_);
         !it.at_end();
         ++it) {
      (*it)->Accept(this);
    }

    for (ConstIter<std::vector<const IntervalVar*> > it(interval_list_);
         !it.at_end();
         ++it) {
      (*it)->Accept(this);
    }
  }

  virtual void EndVisitModel(const string& model_name) {
    for (ConstIter<std::vector<ArgumentHolder*> > it(extensions_);
         !it.at_end();
         ++it) {
      WriteModelExtension(*it);
    }
    PopArgumentHolder();
    // Write tags.
    for (int i = 0; i < tags_.size(); ++i) {
      model_proto_->add_tags(tags_.Element(i));
    }
  }

  virtual void BeginVisitConstraint(const string& type_name,
                                    const Constraint* const constraint) {
    PushArgumentHolder();
  }

  virtual void EndVisitConstraint(const string& type_name,
                                  const Constraint* const constraint) {
    // We ignore delegate constraints, they will be regenerated automatically.
    if (constraint->IsDelegate()) {
      return;
    }

    const int index = model_proto_->constraints_size();
    CPConstraintProto* const constraint_proto = model_proto_->add_constraints();
    ExportToProto(constraint, constraint_proto, type_name, index);
    PopArgumentHolder();
  }

  virtual void BeginVisitIntegerExpression(const string& type_name,
                                           const IntExpr* const expression) {
    PushArgumentHolder();
  }

  virtual void EndVisitIntegerExpression(const string& type_name,
                                         const IntExpr* const expression) {
    const int index = model_proto_->expressions_size();
    CPIntegerExpressionProto* const expression_proto =
        model_proto_->add_expressions();
    ExportToProto(expression, expression_proto, type_name, index);
    PopArgumentHolder();
  }

  virtual void BeginVisitExtension(const string& type_name) {
    PushExtension(type_name);
  }

  virtual void EndVisitExtension(const string& type_name) {
    PopAndSaveExtension();
  }

  virtual void VisitIntegerArgument(const string& arg_name, int64 value) {
    top()->set_integer_argument(arg_name, value);
  }

  virtual void VisitIntegerArrayArgument(const string& arg_name,
                                         const int64* const values,
                                         int size) {
    top()->set_integer_array_argument(arg_name, values, size);
  }

  virtual void VisitIntegerMatrixArgument(const string& arg_name,
                                          const int64* const * const values,
                                          int rows,
                                          int columns) {
    top()->set_integer_matrix_argument(arg_name, values, rows, columns);
  }

  virtual void VisitIntegerExpressionArgument(
      const string& arg_name,
      const IntExpr* const argument) {
    top()->set_integer_expression_argument(arg_name,
                                           FindExpressionIndex(argument));
  }

  virtual void VisitIntegerVariableArrayArgument(
      const string& arg_name,
      const IntVar* const * arguments,
      int size) {
    std::vector<int> indices;
    for (int i = 0; i < size; ++i) {
      indices.push_back(FindExpressionIndex(arguments[i]));
    }
    top()->set_integer_variable_array_argument(arg_name,
                                               indices.data(),
                                               indices.size());
  }

  virtual void VisitIntervalArgument(
      const string& arg_name,
      const IntervalVar* argument) {
    top()->set_interval_argument(arg_name, FindIntervalIndex(argument));
  }

  virtual void VisitIntervalArrayArgument(
      const string& arg_name,
      const IntervalVar* const * arguments,
      int size) {
    std::vector<int> indices;
    for (int i = 0; i < size; ++i) {
      indices.push_back(FindIntervalIndex(arguments[i]));
    }
    top()->set_interval_array_argument(arg_name,
                                       indices.data(),
                                       indices.size());
  }

  virtual void VisitIntegerVariable(const IntVar* const variable,
                                    const IntExpr* const delegate) {
    if (delegate != NULL) {
      const int index = model_proto_->expressions_size();
      CPIntegerExpressionProto* const var_proto =
          model_proto_->add_expressions();
      var_proto->set_index(index);
      var_proto->set_type_index(TagIndex(ModelVisitor::kIntegerVariable));
      CPArgumentProto* const sub_proto = var_proto->add_arguments();
      sub_proto->set_argument_index(
          TagIndex(ModelVisitor::kExpressionArgument));
      sub_proto->set_integer_expression_index(FindExpressionIndex(delegate));
    } else {
      const int index = model_proto_->expressions_size();
      CPIntegerExpressionProto* const var_proto =
          model_proto_->add_expressions();
      var_proto->set_index(index);
      var_proto->set_type_index(TagIndex(ModelVisitor::kIntegerVariable));
      if (variable->HasName()) {
        var_proto->set_name(variable->name());
      }
      if (variable->Size() == variable->Max() - variable->Min() + 1) {
        // Contiguous
        CPArgumentProto* const min_proto = var_proto->add_arguments();
        min_proto->set_argument_index(TagIndex(ModelVisitor::kMinArgument));
        min_proto->set_integer_value(variable->Min());
        CPArgumentProto* const max_proto = var_proto->add_arguments();
        max_proto->set_argument_index(TagIndex(ModelVisitor::kMaxArgument));
        max_proto->set_integer_value(variable->Max());
      } else {
        // Non Contiguous
        CPArgumentProto* const values_proto = var_proto->add_arguments();
        values_proto->set_argument_index(
            TagIndex(ModelVisitor::kValuesArgument));
        scoped_ptr<IntVarIterator> it(variable->MakeDomainIterator(false));
        for (it->Init(); it->Ok(); it->Next()) {
          values_proto->add_integer_array(it->Value());
        }
      }
    }
  }

  virtual void VisitIntervalVariable(const IntervalVar* const variable,
                                     const string operation,
                                     const IntervalVar* const delegate) {
    if (delegate != NULL) {
      const int index = model_proto_->intervals_size();
      CPIntervalVariableProto* const var_proto = model_proto_->add_intervals();
      var_proto->set_index(index);
      var_proto->set_type_index(TagIndex(ModelVisitor::kIntervalVariable));
      CPArgumentProto* const sub_proto = var_proto->add_arguments();
      sub_proto->set_argument_index(TagIndex(operation));
      sub_proto->set_interval_index(FindIntervalIndex(delegate));
    } else {
      const int index = model_proto_->intervals_size();
      CPIntervalVariableProto* const var_proto = model_proto_->add_intervals();
      var_proto->set_index(index);
      var_proto->set_type_index(TagIndex(ModelVisitor::kIntervalVariable));
      if (variable->HasName()) {
        var_proto->set_name(variable->name());
      }
      CPArgumentProto* const start_min_proto = var_proto->add_arguments();
      start_min_proto->set_argument_index(
          TagIndex(ModelVisitor::kStartMinArgument));
      start_min_proto->set_integer_value(variable->StartMin());
      CPArgumentProto* const start_max_proto = var_proto->add_arguments();
      start_max_proto->set_argument_index(
          TagIndex(ModelVisitor::kStartMaxArgument));
      start_max_proto->set_integer_value(variable->StartMax());
      CPArgumentProto* const end_min_proto = var_proto->add_arguments();
      end_min_proto->set_argument_index(
          TagIndex(ModelVisitor::kEndMinArgument));
      end_min_proto->set_integer_value(variable->EndMin());
      CPArgumentProto* const end_max_proto = var_proto->add_arguments();
      end_max_proto->set_argument_index(
          TagIndex(ModelVisitor::kEndMaxArgument));
      end_max_proto->set_integer_value(variable->EndMax());
      CPArgumentProto* const duration_min_proto = var_proto->add_arguments();
      duration_min_proto->set_argument_index(
          TagIndex(ModelVisitor::kDurationMinArgument));
      duration_min_proto->set_integer_value(variable->DurationMin());
      CPArgumentProto* const duration_max_proto = var_proto->add_arguments();
      duration_max_proto->set_argument_index(
          TagIndex(ModelVisitor::kDurationMaxArgument));
      duration_max_proto->set_integer_value(variable->DurationMax());
      CPArgumentProto* const optional_proto = var_proto->add_arguments();
      optional_proto->set_argument_index(
          TagIndex(ModelVisitor::kOptionalArgument));
      optional_proto->set_integer_value(!variable->MustBePerformed());
    }
  }

  virtual void VisitIntervalVariable(const IntervalVar* const variable,
                                     const string operation,
                                     const IntervalVar* const * delegates,
                                     int size) {
    CHECK_NOTNULL(delegates);
    CHECK_GT(size, 0);
    const int index = model_proto_->intervals_size();
    CPIntervalVariableProto* const var_proto = model_proto_->add_intervals();
    var_proto->set_index(index);
    var_proto->set_type_index(TagIndex(ModelVisitor::kIntervalVariable));
    CPArgumentProto* const sub_proto = var_proto->add_arguments();
    sub_proto->set_argument_index(TagIndex(operation));
    for (int i = 0; i < size; ++i) {
      sub_proto->add_interval_array(FindIntervalIndex(delegates[i]));
    }
  }

  int TagIndex(const string& tag) {
    return tags_.Add(tag);
  }

 private:
  void WriteModelExtension(ArgumentHolder* const holder) {
    CHECK_NOTNULL(holder);
    if (holder->type_name().compare(kObjectiveExtension) == 0) {
      WriteObjective(holder);
    } else if (holder->type_name().compare(kSearchLimitExtension) == 0) {
      WriteSearchLimit(holder);
    } else if (holder->type_name().compare(kVariableGroupExtension) == 0) {
      WriteVariableGroup(holder);
    } else {
      LOG(INFO) << "Unknown model extension :" << holder->type_name();
    }
  }

  void WriteObjective(ArgumentHolder* const holder) {
    CHECK_NOTNULL(holder);
    const bool maximize = holder->FindIntegerArgumentOrDie(kMaximizeArgument);
    const int64 step = holder->FindIntegerArgumentOrDie(kStepArgument);
    const int objective_index =
        holder->FindIntegerExpressionArgumentOrDie(kExpressionArgument);
    CPObjectiveProto* const objective_proto = model_proto_->mutable_objective();
    objective_proto->set_maximize(maximize);
    objective_proto->set_step(step);
    objective_proto->set_objective_index(objective_index);
  }

  void WriteSearchLimit(ArgumentHolder* const holder) {
    CHECK_NOTNULL(holder);
    SearchLimitProto* const proto = model_proto_->mutable_search_limit();
    proto->set_time(holder->FindIntegerArgumentWithDefault(kTimeLimitArgument,
                                                           kint64max));
    proto->set_branches(holder->FindIntegerArgumentWithDefault(
        kBranchesLimitArgument,
        kint64max));
    proto->set_failures(holder->FindIntegerArgumentWithDefault(
        kFailuresLimitArgument,
        kint64max));
    proto->set_solutions(holder->FindIntegerArgumentWithDefault(
        kSolutionLimitArgument,
        kint64max));
    proto->set_smart_time_check(holder->FindIntegerArgumentWithDefault(
        kSmartTimeCheckArgument,
        false));
    proto->set_cumulative(holder->FindIntegerArgumentWithDefault(
        kCumulativeArgument,
        false));
  }

  void WriteVariableGroup(ArgumentHolder* const holder) {
    CPVariableGroup* const group_proto = model_proto_->add_variable_groups();
    holder->ExportToProto(&tags_, group_proto);
  }

  template <class A, class P> void ExportToProto(const A* const argument,
                                                 P* const proto,
                                                 const string& type_name,
                                                 int index) {
    CHECK_NOTNULL(proto);
    CHECK_NOTNULL(argument);
    proto->set_index(index);
    proto->set_type_index(TagIndex(type_name));
    if (argument->HasName()) {
      proto->set_name(argument->name());
    }
    top()->ExportToProto(&tags_, proto);
    for (ConstIter<std::vector<ArgumentHolder*> > it(extensions_);
         !it.at_end();
         ++it) {
      CPExtensionProto* const extension_proto = proto->add_extensions();
      extension_proto->set_type_index(TagIndex((*it)->type_name()));
      (*it)->ExportToProto(&tags_, extension_proto);
    }
  }

  void PushArgumentHolder() {
    holders_.push_back(new ArgumentHolder);
  }

  void PopArgumentHolder() {
    CHECK(!holders_.empty());
    delete holders_.back();
    holders_.pop_back();
    STLDeleteElements(&extensions_);
    extensions_.clear();
  }

  void PushExtension(const string& type_name) {
    PushArgumentHolder();
    holders_.back()->set_type_name(type_name);
  }

  void PopAndSaveExtension() {
    CHECK(!holders_.empty());
    extensions_.push_back(holders_.back());
    holders_.pop_back();
  }

  ArgumentHolder* top() const {
    CHECK(!holders_.empty());
    return holders_.back();
  }

  int FindExpressionIndex(const IntExpr* const expression) const {
    const int result = FindWithDefault(expression_map_, expression, -1);
    CHECK_NE(-1, result);
    return result;
  }

  int FindIntervalIndex(const IntervalVar* const interval) const {
    const int result = FindWithDefault(interval_map_, interval, -1);
    CHECK_NE(-1, result);
    return result;
  }

  hash_map<const IntExpr*, int> expression_map_;
  hash_map<const IntervalVar*, int> interval_map_;
  hash_map<const IntVar*, const IntExpr*> delegate_map_;
  std::vector<const IntExpr*> expression_list_;
  std::vector<const Constraint*> constraint_list_;
  std::vector<const IntervalVar*> interval_list_;
  CPModelProto* const model_proto_;

  std::vector<ArgumentHolder*> holders_;
  std::vector<ArgumentHolder*> extensions_;
  VectorMap<string> tags_;
};

// ---------- Model Protocol Reader ----------

// ----- Utility Class for Callbacks -----

template <class T> class ArrayWithOffset : public BaseObject {
 public:
  ArrayWithOffset(int64 index_min, int64 index_max)
    : index_min_(index_min),
      index_max_(index_max),
      values_(new T[index_max - index_min + 1]) {
    DCHECK_LE(index_min, index_max);
  }

  virtual ~ArrayWithOffset() {}

  virtual T Evaluate(int64 index) const {
    DCHECK_GE(index, index_min_);
    DCHECK_LE(index, index_max_);
    return values_[index - index_min_];
  }

  void SetValue(int64 index, T value) {
    DCHECK_GE(index, index_min_);
    DCHECK_LE(index, index_max_);
    values_[index - index_min_] = value;
  }

 private:
  const int64 index_min_;
  const int64 index_max_;
  scoped_array<T> values_;
};

template <class T> void MakeCallbackFromProto(
    CPModelBuilder* const builder,
    const CPExtensionProto& proto,
    int tag_index,
    ResultCallback1<T, int64>** callback) {
  DCHECK_EQ(tag_index, proto.type_index());
  Solver* const solver = builder->solver();
  int64 index_min = 0;
  CHECK(builder->ScanArguments(ModelVisitor::kMinArgument, proto, &index_min));
  int64 index_max = 0;
  CHECK(builder->ScanArguments(ModelVisitor::kMaxArgument, proto, &index_max));
  std::vector<int64> values;
  CHECK(builder->ScanArguments(ModelVisitor::kValuesArgument, proto, &values));
  ArrayWithOffset<T>* const array =
      solver->RevAlloc(new ArrayWithOffset<T>(index_min, index_max));
  for (int i = index_min; i <= index_max; ++i) {
    array->SetValue(i, values[i - index_min]);
  }
  *callback = NewPermanentCallback(array, &ArrayWithOffset<T>::Evaluate);
}

#define VERIFY(expr) if (!(expr)) return NULL
#define VERIFY_EQ(e1, e2) if ((e1) != (e2)) return NULL

// ----- kAbs -----

IntExpr* BuildAbs(CPModelBuilder* const builder,
                  const CPIntegerExpressionProto& proto) {
  IntExpr* expr = NULL;
  VERIFY(builder->ScanArguments(ModelVisitor::kExpressionArgument,
                                proto,
                                &expr));
  return builder->solver()->MakeAbs(expr);
}

// ----- kAllDifferent -----

Constraint* BuildAllDifferent(CPModelBuilder* const builder,
                              const CPConstraintProto& proto) {
  std::vector<IntVar*> vars;
  VERIFY(builder->ScanArguments(ModelVisitor::kVarsArgument, proto, &vars));
  int64 range = 0;
  VERIFY(builder->ScanArguments(ModelVisitor::kRangeArgument, proto, &range));
  return builder->solver()->MakeAllDifferent(vars, range);
}

// ----- kAllowedAssignments -----

Constraint* BuildAllowedAssignments(CPModelBuilder* const builder,
                                    const CPConstraintProto& proto) {
  std::vector<IntVar*> vars;
  VERIFY(builder->ScanArguments(ModelVisitor::kVarsArgument, proto, &vars));
  std::vector<std::vector<int64> > tuples;
  VERIFY(builder->ScanArguments(ModelVisitor::kTuplesArgument, proto, &tuples));
  return builder->solver()->MakeAllowedAssignments(vars, tuples);
}

// ----- kBetween -----

Constraint* BuildBetween(CPModelBuilder* const builder,
                         const CPConstraintProto& proto) {
  int64 value_min = 0;
  VERIFY(builder->ScanArguments(ModelVisitor::kMinArgument, proto, &value_min));
  int64 value_max = 0;
  VERIFY(builder->ScanArguments(ModelVisitor::kMaxArgument, proto, &value_max));
  IntExpr* expr = NULL;
  VERIFY(builder->ScanArguments(ModelVisitor::kExpressionArgument,
                                proto,
                                &expr));
  return builder->solver()->MakeBetweenCt(expr->Var(), value_min, value_max);
}

// ----- kConvexPiecewise -----
IntExpr* BuildConvexPiecewise(CPModelBuilder* const builder,
                              const CPIntegerExpressionProto& proto) {
  IntExpr* expr = NULL;
  VERIFY(builder->ScanArguments(ModelVisitor::kExpressionArgument,
                                proto,
                                &expr));
  int64 early_cost = 0;
  VERIFY(builder->ScanArguments(ModelVisitor::kEarlyCostArgument,
                                proto,
                                &early_cost));
  int64 early_date = 0;
  VERIFY(builder->ScanArguments(ModelVisitor::kEarlyDateArgument,
                                proto,
                                &early_date));
  int64 late_cost = 0;
  VERIFY(builder->ScanArguments(ModelVisitor::kLateCostArgument,
                                proto,
                                &late_cost));
  int64 late_date = 0;
  VERIFY(builder->ScanArguments(ModelVisitor::kLateDateArgument,
                                proto,
                                &late_date));
  return builder->solver()->MakeConvexPiecewiseExpr(expr->Var(),
                                                    early_cost,
                                                    early_date,
                                                    late_date,
                                                    late_cost);
}

// ----- kCountEqual -----

Constraint* BuildCountEqual(CPModelBuilder* const builder,
                            const CPConstraintProto& proto) {
  std::vector<IntVar*> vars;
  VERIFY(builder->ScanArguments(ModelVisitor::kVarsArgument, proto, &vars));
  int64 value = 0;
  VERIFY(builder->ScanArguments(ModelVisitor::kValueArgument, proto, &value));
  int64 count = 0;
  if (builder->ScanArguments(ModelVisitor::kCountArgument, proto, &value)) {
    return builder->solver()->MakeCount(vars, value, count);
  } else {
    IntExpr* count_expr = NULL;
    VERIFY(builder->ScanArguments(ModelVisitor::kCountArgument,
                                  proto,
                                  &count_expr));
    return builder->solver()->MakeCount(vars, value, count_expr->Var());
  }
}

// ----- kCumulative -----

Constraint* BuildCumulative(CPModelBuilder* const builder,
                            const CPConstraintProto& proto) {
  std::vector<IntervalVar*> vars;
  VERIFY(builder->ScanArguments(ModelVisitor::kIntervalsArgument,
                                proto,
                                &vars));
  std::vector<int64> demands;
  VERIFY(builder->ScanArguments(ModelVisitor::kDemandsArgument,
                                proto,
                                &demands));
  int64 capacity;
  VERIFY(builder->ScanArguments(ModelVisitor::kCapacityArgument,
                                proto,
                                &capacity));
  string name;
  if (proto.has_name()) {
    name = proto.name();
  }
  return builder->solver()->MakeCumulative(vars, demands, capacity, name);
}

// ----- kDeviation -----

Constraint* BuildDeviation(CPModelBuilder* const builder,
                           const CPConstraintProto& proto) {
  std::vector<IntVar*> vars;
  VERIFY(builder->ScanArguments(ModelVisitor::kVarsArgument, proto, &vars));
  IntExpr* target = NULL;
  VERIFY(builder->ScanArguments(ModelVisitor::kTargetArgument, proto, &target));
  int64 value = 0;
  VERIFY(builder->ScanArguments(ModelVisitor::kValueArgument, proto, &value));
  return builder->solver()->MakeDeviation(vars, target->Var(), value);
}

// ----- kDifference -----

IntExpr* BuildDifference(CPModelBuilder* const builder,
                         const CPIntegerExpressionProto& proto) {
  IntExpr* left = NULL;
  if (builder->ScanArguments(ModelVisitor::kLeftArgument, proto, &left)) {
    IntExpr* right = NULL;
    VERIFY(builder->ScanArguments(ModelVisitor::kRightArgument, proto, &right));
    return builder->solver()->MakeDifference(left, right);
  }
  IntExpr* expr = NULL;
  VERIFY(builder->ScanArguments(
      ModelVisitor::kExpressionArgument, proto, &expr));
  int64 value = 0;
  VERIFY(builder->ScanArguments(ModelVisitor::kValueArgument, proto, &value));
  return builder->solver()->MakeDifference(value, expr);
}

// ----- kDistribute -----

Constraint* BuildDistribute(CPModelBuilder* const builder,
                            const CPConstraintProto& proto) {
  std::vector<IntVar*> vars;
  if (builder->ScanArguments(ModelVisitor::kVarsArgument, proto, &vars)) {
    std::vector<IntVar*> cards;
    if (builder->ScanArguments(ModelVisitor::kCardsArgument, proto, &cards)) {
      std::vector<int64> values;
      if (builder->ScanArguments(ModelVisitor::kValuesArgument,
                                 proto,
                                 &values)) {
        return builder->solver()->MakeDistribute(vars, values, cards);
      } else {
        return builder->solver()->MakeDistribute(vars, cards);
      }
    } else {
      int64 card_min = 0;
      VERIFY(builder->ScanArguments(ModelVisitor::kMinArgument,
                                    proto,
                                    &card_min));
      int64 card_max = 0;
      VERIFY(builder->ScanArguments(ModelVisitor::kMaxArgument,
                                    proto,
                                    &card_max));
      int64 card_size = 0;
      VERIFY(builder->ScanArguments(ModelVisitor::kSizeArgument,
                                    proto,
                                    &card_size));
      return builder->solver()->MakeDistribute(vars,
                                               card_min,
                                               card_max,
                                               card_size);
    }
  } else {
    std::vector<IntVar*> cards;
    VERIFY(builder->ScanArguments(ModelVisitor::kCardsArgument, proto, &cards));
    return builder->solver()->MakeDistribute(vars, cards);
  }
}

// ----- kDivide -----

IntExpr* BuildDivide(CPModelBuilder* const builder,
                     const CPIntegerExpressionProto& proto) {
  IntExpr* expr = NULL;
  VERIFY(builder->ScanArguments(ModelVisitor::kExpressionArgument,
                                proto,
                                &expr));
  int64 value = 0;
  VERIFY(builder->ScanArguments(ModelVisitor::kValueArgument, proto, &value));
  return builder->solver()->MakeDiv(expr, value);
}

// ----- kDurationExpr -----

IntExpr* BuildDurationExpr(CPModelBuilder* const builder,
                           const CPIntegerExpressionProto& proto) {
  IntervalVar* var = NULL;
  VERIFY(builder->ScanArguments(ModelVisitor::kIntervalArgument, proto, &var));
  return var->DurationExpr();
}

// ----- kElement -----

IntExpr* BuildElement(CPModelBuilder* const builder,
                      const CPIntegerExpressionProto& proto) {
  IntExpr* index = NULL;
  VERIFY(builder->ScanArguments(ModelVisitor::kIndexArgument, proto, &index));
  std::vector<int64> values;
  if (proto.extensions_size() > 0) {
    VERIFY_EQ(1, proto.extensions_size());
    Solver::IndexEvaluator1 * callback = NULL;
    const int extension_tag_index =
        builder->TagIndex(ModelVisitor::kInt64ToInt64Extension);
    MakeCallbackFromProto(builder,
                          proto.extensions(0),
                          extension_tag_index,
                          &callback);
    return builder->solver()->MakeElement(callback, index->Var());
  }
  if (builder->ScanArguments(ModelVisitor::kValuesArgument, proto, &values)) {
    return builder->solver()->MakeElement(values, index->Var());
  }
  std::vector<IntVar*> vars;
  if (builder->ScanArguments(ModelVisitor::kVarsArgument, proto, &vars)) {
    return builder->solver()->MakeElement(vars, index->Var());
  }
  return NULL;
}

// ----- kElementEqual -----
// TODO(user): Add API on solver and uncomment this method.
/*
  Constraint* BuildElementEqual(CPModelBuilder* const builder,
  const CPConstraintProto& proto) {
  IntExpr* target = NULL;
  VERIFY(builder->ScanArguments(ModelVisitor::kTargetArgument,
  proto,
  &target));
  std::vector<int64> values;
  if (builder->ScanArguments(ModelVisitor::kValuesArgument,
  proto,
  &values)) {
  IntExpr* index = NULL;
  VERIFY(builder->ScanArguments(ModelVisitor::kIndexArgument,
  proto,
  &index));
  return builder->solver()->MakeElement(values, index->Var());
  }
  std::vector<IntVar*> vars;
  if (builder->ScanArguments(ModelVisitor::kVarsArgument,
  proto,
  &vars)) {
  IntExpr* index = NULL;
  VERIFY(builder->ScanArguments(ModelVisitor::kIndexArgument,
  proto,
  &index));
  return builder->solver()->MakeElement(vars, index->Var());
  }
  return NULL;
  }
*/

// ----- kEndExpr -----

IntExpr* BuildEndExpr(CPModelBuilder* const builder,
                      const CPIntegerExpressionProto& proto) {
  IntervalVar* var = NULL;
  VERIFY(builder->ScanArguments(ModelVisitor::kIntervalArgument, proto, &var));
  return var->EndExpr();
}

// ----- kEquality -----

Constraint* BuildEquality(CPModelBuilder* const builder,
                          const CPConstraintProto& proto) {
  IntExpr* left = NULL;
  if (builder->ScanArguments(ModelVisitor::kLeftArgument, proto, &left)) {
    IntExpr* right = NULL;
    VERIFY(builder->ScanArguments(ModelVisitor::kRightArgument, proto, &right));
    return builder->solver()->MakeEquality(left->Var(), right->Var());
  }
  IntExpr* expr = NULL;
  if (builder->ScanArguments(ModelVisitor::kExpressionArgument, proto, &expr)) {
    int64 value = 0;
    VERIFY(builder->ScanArguments(ModelVisitor::kValueArgument, proto, &value));
    return builder->solver()->MakeEquality(expr->Var(), value);
  }
  return NULL;
}

// ----- kFalseConstraint -----

Constraint* BuildFalseConstraint(CPModelBuilder* const builder,
                                 const CPConstraintProto& proto) {
  return builder->solver()->MakeFalseConstraint();
}

// ----- kGreater -----

Constraint* BuildGreater(CPModelBuilder* const builder,
                         const CPConstraintProto& proto) {
  IntExpr* left = NULL;
  VERIFY(builder->ScanArguments(ModelVisitor::kLeftArgument, proto, &left));
  IntExpr* right = NULL;
  VERIFY(builder->ScanArguments(ModelVisitor::kRightArgument, proto, &right));
  return builder->solver()->MakeGreater(left->Var(), right->Var());
}

// ----- kGreaterOrEqual -----

Constraint* BuildGreaterOrEqual(CPModelBuilder* const builder,
                                const CPConstraintProto& proto) {
  IntExpr* left = NULL;
  if (builder->ScanArguments(ModelVisitor::kLeftArgument, proto, &left)) {
    IntExpr* right = NULL;
    VERIFY(builder->ScanArguments(ModelVisitor::kRightArgument, proto, &right));
    return builder->solver()->MakeGreaterOrEqual(left->Var(), right->Var());
  }
  IntExpr* expr = NULL;
  if (builder->ScanArguments(ModelVisitor::kExpressionArgument, proto, &expr)) {
    int64 value = 0;
    VERIFY(builder->ScanArguments(ModelVisitor::kValueArgument, proto, &value));
    return builder->solver()->MakeGreaterOrEqual(expr->Var(), value);
  }
  return NULL;
}

// ----- kIntegerVariable -----

IntExpr* BuildIntegerVariable(CPModelBuilder* const builder,
                              const CPIntegerExpressionProto& proto) {
  IntExpr* sub_expression = NULL;
  if (builder->ScanArguments(ModelVisitor::kExpressionArgument,
                             proto,
                             &sub_expression)) {
    IntVar* const result = sub_expression->Var();
    if (proto.has_name()) {
      result->set_name(proto.name());
    }
    return result;
  }
  int64 var_min = 0;
  if (builder->ScanArguments(ModelVisitor::kMinArgument, proto, &var_min)) {
    int64 var_max = 0;
    VERIFY(builder->ScanArguments(ModelVisitor::kMaxArgument, proto, &var_max));
    IntVar* const result = builder->solver()->MakeIntVar(var_min, var_max);
    if (proto.has_name()) {
      result->set_name(proto.name());
    }
    return result;
  }
  std::vector<int64> values;
  if (builder->ScanArguments(ModelVisitor::kValuesArgument, proto, &values)) {
    IntVar* const result = builder->solver()->MakeIntVar(values);
    if (proto.has_name()) {
      result->set_name(proto.name());
    }
    return result;
  }
  return NULL;
}

// ----- kIntervalBinaryRelation -----

Constraint* BuildIntervalBinaryRelation(CPModelBuilder* const builder,
                                        const CPConstraintProto& proto) {
  IntervalVar* left = NULL;
  VERIFY(builder->ScanArguments(ModelVisitor::kLeftArgument, proto, &left));
  IntervalVar* right = NULL;
  VERIFY(builder->ScanArguments(ModelVisitor::kRightArgument, proto, &right));
  int64 relation = 0;
  VERIFY(builder->ScanArguments(ModelVisitor::kRelationArgument,
                                proto,
                                &relation));
  Solver::BinaryIntervalRelation rel =
      static_cast<Solver::BinaryIntervalRelation>(relation);
  return builder->solver()->MakeIntervalVarRelation(left, rel, right);
}

// ----- kIntervalDisjunction -----

Constraint* BuildIntervalDisjunction(CPModelBuilder* const builder,
                                     const CPConstraintProto& proto) {
  IntervalVar* left = NULL;
  VERIFY(builder->ScanArguments(ModelVisitor::kLeftArgument, proto, &left));
  IntervalVar* right = NULL;
  VERIFY(builder->ScanArguments(ModelVisitor::kRightArgument, proto, &right));
  IntExpr* target = NULL;
  VERIFY(builder->ScanArguments(ModelVisitor::kTargetArgument, proto, &target));
  return builder->solver()->MakeTemporalDisjunction(left, right, target->Var());
}

// ----- kIntervalUnaryRelation -----

Constraint* BuildIntervalUnaryRelation(CPModelBuilder* const builder,
                                       const CPConstraintProto& proto) {
  IntervalVar* interval = NULL;
  VERIFY(builder->ScanArguments(ModelVisitor::kIntervalArgument,
                                proto,
                                &interval));
  int64 date = 0;
  VERIFY(builder->ScanArguments(ModelVisitor::kValueArgument, proto, &date));
  int64 relation = 0;
  VERIFY(builder->ScanArguments(ModelVisitor::kRelationArgument,
                                proto,
                                &relation));
  Solver::UnaryIntervalRelation rel =
      static_cast<Solver::UnaryIntervalRelation>(relation);
  return builder->solver()->MakeIntervalVarRelation(interval, rel, date);
}

// ----- kIntervalVariable -----

IntervalVar* BuildIntervalVariable(CPModelBuilder* const builder,
                                   const CPIntervalVariableProto& proto) {
  Solver* const solver = builder->solver();
  int64 start_min = 0;
  if (builder->ScanArguments(ModelVisitor::kStartMinArgument,
                             proto,
                             &start_min)) {
    int64 start_max = 0;
    VERIFY(builder->ScanArguments(ModelVisitor::kStartMaxArgument,
                                  proto,
                                  &start_max));
    int64 end_min = 0;
    VERIFY(builder->ScanArguments(ModelVisitor::kEndMinArgument,
                                  proto,
                                  &end_min));
    int64 end_max = 0;
    VERIFY(builder->ScanArguments(ModelVisitor::kEndMaxArgument,
                                  proto,
                                  &end_max));
    int64 duration_min = 0;
    VERIFY(builder->ScanArguments(ModelVisitor::kDurationMinArgument,
                                  proto,
                                  &duration_min));
    int64 duration_max = 0;
    VERIFY(builder->ScanArguments(ModelVisitor::kDurationMaxArgument,
                                  proto,
                                  &duration_max));
    int64 optional = 0;
    VERIFY(builder->ScanArguments(ModelVisitor::kOptionalArgument,
                                  proto,
                                  &optional));
    VERIFY_EQ(duration_max, duration_min);
    VERIFY_EQ(end_max - duration_max, start_max);
    VERIFY_EQ(end_min - duration_min, start_min);
    const string name = proto.name();
    if (start_min == start_max) {
      return solver->MakeFixedInterval(start_min, duration_min, name);
    } else {
      return solver->MakeFixedDurationIntervalVar(start_min,
                                                  start_max,
                                                  duration_min,
                                                  optional,
                                                  name);
    }
  } else {
    VERIFY_EQ(1, proto.arguments_size());
    const CPArgumentProto& sub_proto = proto.arguments(0);
    IntervalVar* const derived =
        builder->IntervalVariable(sub_proto.interval_index());
    const int operation_index = sub_proto.argument_index();
    DCHECK_NE(-1, operation_index);
    if (operation_index == builder->TagIndex(ModelVisitor::kMirrorOperation)) {
      return solver->MakeMirrorInterval(derived);
    } else if (operation_index ==
               builder->TagIndex(ModelVisitor::kRelaxedMaxOperation)) {
      solver->MakeIntervalRelaxedMax(derived);
    } else if (operation_index ==
               builder->TagIndex(ModelVisitor::kRelaxedMinOperation)) {
      solver->MakeIntervalRelaxedMin(derived);
    }
  }
  return NULL;
}

// ----- kIsBetween -----

Constraint* BuildIsBetween(CPModelBuilder* const builder,
                           const CPConstraintProto& proto) {
  int64 value_min = 0;
  VERIFY(builder->ScanArguments(ModelVisitor::kMinArgument, proto, &value_min));
  int64 value_max = 0;
  VERIFY(builder->ScanArguments(ModelVisitor::kMaxArgument, proto, &value_max));
  IntExpr* expr = NULL;
  VERIFY(builder->ScanArguments(
      ModelVisitor::kExpressionArgument, proto, &expr));
  IntExpr* target = NULL;
  VERIFY(builder->ScanArguments(ModelVisitor::kTargetArgument, proto, &target));
  return builder->solver()->MakeIsBetweenCt(expr->Var(),
                                            value_min,
                                            value_max,
                                            target->Var());
}

// ----- kIsDifferent -----

Constraint* BuildIsDifferent(CPModelBuilder* const builder,
                             const CPConstraintProto& proto) {
  int64 value = 0;
  VERIFY(builder->ScanArguments(ModelVisitor::kValueArgument, proto, &value));
  IntExpr* expr = NULL;
  VERIFY(builder->ScanArguments(ModelVisitor::kExpressionArgument,
                                proto,
                                &expr));
  IntExpr* target = NULL;
  VERIFY(builder->ScanArguments(ModelVisitor::kTargetArgument, proto, &target));
  return builder->solver()->MakeIsDifferentCstCt(expr->Var(),
                                                 value,
                                                 target->Var());
}

// ----- kIsEqual -----

Constraint* BuildIsEqual(CPModelBuilder* const builder,
                         const CPConstraintProto& proto) {
  int64 value = 0;
  VERIFY(builder->ScanArguments(ModelVisitor::kValueArgument, proto, &value));
  IntExpr* expr = NULL;
  VERIFY(builder->ScanArguments(
      ModelVisitor::kExpressionArgument, proto, &expr));
  IntExpr* target = NULL;
  VERIFY(builder->ScanArguments(ModelVisitor::kTargetArgument, proto, &target));
  return builder->solver()->MakeIsEqualCstCt(expr->Var(),
                                             value,
                                             target->Var());
}

// ----- kIsGreaterOrEqual -----

Constraint* BuildIsGreaterOrEqual(CPModelBuilder* const builder,
                                  const CPConstraintProto& proto) {
  int64 value = 0;
  VERIFY(builder->ScanArguments(ModelVisitor::kValueArgument, proto, &value));
  IntExpr* expr = NULL;
  VERIFY(builder->ScanArguments(
      ModelVisitor::kExpressionArgument, proto, &expr));
  IntExpr* target = NULL;
  VERIFY(builder->ScanArguments(ModelVisitor::kTargetArgument, proto, &target));
  return builder->solver()->MakeIsGreaterOrEqualCstCt(expr->Var(),
                                                      value,
                                                      target->Var());
}

// ----- kIsLessOrEqual -----

Constraint* BuildIsLessOrEqual(CPModelBuilder* const builder,
                               const CPConstraintProto& proto) {
  int64 value = 0;
  VERIFY(builder->ScanArguments(ModelVisitor::kValueArgument, proto, &value));
  IntExpr* expr = NULL;
  VERIFY(builder->ScanArguments(ModelVisitor::kExpressionArgument,
                                proto,
                                &expr));
  IntExpr* target = NULL;
  VERIFY(builder->ScanArguments(ModelVisitor::kTargetArgument, proto, &target));
  return builder->solver()->MakeIsLessOrEqualCstCt(expr->Var(),
                                                   value,
                                                   target->Var());
}

// ----- kIsMember -----

Constraint* BuildIsMember(CPModelBuilder* const builder,
                          const CPConstraintProto& proto) {
  std::vector<int64> values;
  VERIFY(builder->ScanArguments(ModelVisitor::kValuesArgument, proto, &values));
  IntExpr* expr = NULL;
  VERIFY(builder->ScanArguments(
      ModelVisitor::kExpressionArgument, proto, &expr));
  IntExpr* target = NULL;
  VERIFY(builder->ScanArguments(ModelVisitor::kTargetArgument, proto, &target));
  return builder->solver()->MakeIsMemberCt(expr->Var(), values, target->Var());
}

// ----- kLess -----

Constraint* BuildLess(CPModelBuilder* const builder,
                      const CPConstraintProto& proto) {
  IntExpr* left = NULL;
  VERIFY(builder->ScanArguments(ModelVisitor::kLeftArgument, proto, &left));
  IntExpr* right = NULL;
  VERIFY(builder->ScanArguments(ModelVisitor::kRightArgument, proto, &right));
  return builder->solver()->MakeLess(left->Var(), right->Var());
}

// ----- kLessOrEqual -----

Constraint* BuildLessOrEqual(CPModelBuilder* const builder,
                             const CPConstraintProto& proto) {
  IntExpr* left = NULL;
  if (builder->ScanArguments(ModelVisitor::kLeftArgument, proto, &left)) {
    IntExpr* right = NULL;
    VERIFY(builder->ScanArguments(ModelVisitor::kRightArgument, proto, &right));
    return builder->solver()->MakeLessOrEqual(left->Var(), right->Var());
  }
  IntExpr* expr = NULL;
  if (builder->ScanArguments(ModelVisitor::kExpressionArgument, proto, &expr)) {
    int64 value = 0;
    VERIFY(builder->ScanArguments(ModelVisitor::kValueArgument, proto, &value));
    return builder->solver()->MakeLessOrEqual(expr->Var(), value);
  }
  return NULL;
}

// ----- kMapDomain -----

Constraint* BuildMapDomain(CPModelBuilder* const builder,
                           const CPConstraintProto& proto) {
  std::vector<IntVar*> vars;
  VERIFY(builder->ScanArguments(ModelVisitor::kVarsArgument, proto, &vars));
  IntExpr* target = NULL;
  VERIFY(builder->ScanArguments(ModelVisitor::kTargetArgument, proto, &target));
  return builder->solver()->MakeMapDomain(target->Var(), vars);
}

// ----- kMax -----

IntExpr* BuildMax(CPModelBuilder* const builder,
                  const CPIntegerExpressionProto& proto) {
  IntExpr* left = NULL;
  if (builder->ScanArguments(ModelVisitor::kLeftArgument, proto, &left)) {
    IntExpr* right = NULL;
    VERIFY(builder->ScanArguments(ModelVisitor::kRightArgument, proto, &right));
    return builder->solver()->MakeMax(left, right);
  }
  IntExpr* expr = NULL;
  if (builder->ScanArguments(
          ModelVisitor::kExpressionArgument, proto, &expr)) {
    int64 value = 0;
    VERIFY(builder->ScanArguments(ModelVisitor::kValueArgument, proto, &value));
    return builder->solver()->MakeMax(expr, value);
  }
  std::vector<IntVar*> vars;
  VERIFY(builder->ScanArguments(ModelVisitor::kVarsArgument, proto, &vars));
  return builder->solver()->MakeMax(vars);
}

// ----- kMaxEqual -----

// TODO(user): Add API on solver and uncomment this method.
/*
  Constraint* BuildMaxEqual(CPModelBuilder* const builder,
  const CPConstraintProto& proto) {
  std::vector<IntVar*> vars;
  VERIFY(builder->ScanArguments(ModelVisitor::kVarsArgument,
  proto,
  &vars));
  IntExpr* target = NULL;
  VERIFY(builder->ScanArguments(ModelVisitor::kTargetArgument,
  proto,
  &target));
  return builder->solver()->MakeMaxEqual(vars, target->Var());
  }
*/

// ----- kMember -----

Constraint* BuildMember(CPModelBuilder* const builder,
                        const CPConstraintProto& proto) {
  std::vector<int64> values;
  VERIFY(builder->ScanArguments(ModelVisitor::kValuesArgument, proto, &values));
  IntExpr* expr = NULL;
  VERIFY(builder->ScanArguments(ModelVisitor::kExpressionArgument,
                                proto,
                                &expr));
  return builder->solver()->MakeMemberCt(expr->Var(), values);
}

// ----- kMin -----

IntExpr* BuildMin(CPModelBuilder* const builder,
                  const CPIntegerExpressionProto& proto) {
  IntExpr* left = NULL;
  if (builder->ScanArguments(ModelVisitor::kLeftArgument, proto, &left)) {
    IntExpr* right = NULL;
    VERIFY(builder->ScanArguments(ModelVisitor::kRightArgument, proto, &right));
    return builder->solver()->MakeMin(left, right);
  }
  IntExpr* expr = NULL;
  if (builder->ScanArguments(ModelVisitor::kExpressionArgument, proto, &expr)) {
    int64 value = 0;
    VERIFY(builder->ScanArguments(ModelVisitor::kValueArgument, proto, &value));
    return builder->solver()->MakeMin(expr, value);
  }
  std::vector<IntVar*> vars;
  VERIFY(builder->ScanArguments(ModelVisitor::kVarsArgument, proto, &vars));
  return builder->solver()->MakeMin(vars);
}

// ----- kMinEqual -----

// TODO(user): Add API on solver and implement this method.

// ----- kNoCycle -----

Constraint* BuildNoCycle(CPModelBuilder* const builder,
                         const CPConstraintProto& proto) {
  std::vector<IntVar*> nexts;
  VERIFY(builder->ScanArguments(ModelVisitor::kNextsArgument, proto, &nexts));
  std::vector<IntVar*> active;
  VERIFY(builder->ScanArguments(ModelVisitor::kActiveArgument, proto, &active));
  int64 assume_paths = 0;
  VERIFY(builder->ScanArguments(ModelVisitor::kAssumePathsArgument,
                                proto,
                                &assume_paths));
  ResultCallback1<bool, int64>* sink_handler = NULL;
  if (proto.extensions_size() > 0) {
    VERIFY_EQ(1, proto.extensions_size());
    const int tag_index =
        builder->TagIndex(ModelVisitor::kInt64ToBoolExtension);
    MakeCallbackFromProto(builder,
                          proto.extensions(0),
                          tag_index,
                          &sink_handler);
  }
  return builder->solver()->MakeNoCycle(nexts, active, NULL, assume_paths);
}

// ----- kNonEqual -----

Constraint* BuildNonEqual(CPModelBuilder* const builder,
                          const CPConstraintProto& proto) {
  IntExpr* left = NULL;
  if (builder->ScanArguments(ModelVisitor::kLeftArgument, proto, &left)) {
    IntExpr* right = NULL;
    VERIFY(builder->ScanArguments(ModelVisitor::kRightArgument, proto, &right));
    return builder->solver()->MakeNonEquality(left->Var(), right->Var());
  }
  IntExpr* expr = NULL;
  if (builder->ScanArguments(ModelVisitor::kExpressionArgument, proto, &expr)) {
    int64 value = 0;
    VERIFY(builder->ScanArguments(ModelVisitor::kValueArgument, proto, &value));
    return builder->solver()->MakeNonEquality(expr->Var(), value);
  }
  return NULL;
}

// ----- kOpposite -----

IntExpr* BuildOpposite(CPModelBuilder* const builder,
                       const CPIntegerExpressionProto& proto) {
  IntExpr* expr = NULL;
  VERIFY(builder->ScanArguments(ModelVisitor::kExpressionArgument,
                                proto,
                                &expr));
  return builder->solver()->MakeOpposite(expr);
}

// ----- kPack -----

bool AddUsageLessConstantDimension(Pack* const pack,
                                   CPModelBuilder* const builder,
                                   const CPExtensionProto& proto) {
  std::vector<int64> weights;
  VERIFY(builder->ScanArguments(ModelVisitor::kCoefficientsArgument,
                                proto,
                                &weights));
  std::vector<int64> upper;
  VERIFY(builder->ScanArguments(ModelVisitor::kValuesArgument, proto, &upper));
  pack->AddWeightedSumLessOrEqualConstantDimension(weights, upper);
  return true;
}

bool AddCountAssignedItemsDimension(Pack* const pack,
                                    CPModelBuilder* const builder,
                                    const CPExtensionProto& proto) {
  IntExpr* target = NULL;
  VERIFY(builder->ScanArguments(ModelVisitor::kTargetArgument, proto, &target));
  pack->AddCountAssignedItemsDimension(target->Var());
  return true;
}

bool AddCountUsedBinDimension(Pack* const pack,
                              CPModelBuilder* const builder,
                              const CPExtensionProto& proto) {
  IntExpr* target = NULL;
  VERIFY(builder->ScanArguments(ModelVisitor::kTargetArgument, proto, &target));
  pack->AddCountUsedBinDimension(target->Var());
  return true;
}

bool AddUsageEqualVariableDimension(Pack* const pack,
                                    CPModelBuilder* const builder,
                                    const CPExtensionProto& proto) {
  std::vector<int64> weights;
  VERIFY(builder->ScanArguments(ModelVisitor::kCoefficientsArgument,
                                proto,
                                &weights));
  std::vector<IntVar*> loads;
  VERIFY(builder->ScanArguments(ModelVisitor::kVarsArgument, proto, &loads));
  pack->AddWeightedSumEqualVarDimension(weights, loads);
  return true;
}

bool AddVariableUsageLessConstantDimension(Pack* const pack,
                                           CPModelBuilder* const builder,
                                           const CPExtensionProto& proto) {
  std::vector<int64> uppers;
  VERIFY(builder->ScanArguments(ModelVisitor::kValuesArgument, proto, &uppers));
  std::vector<IntVar*> usages;
  VERIFY(builder->ScanArguments(ModelVisitor::kVarsArgument, proto, &usages));
  pack->AddSumVariableWeightsLessOrEqualConstantDimension(usages, uppers);
  return true;
}

bool AddWeightedSumOfAssignedDimension(Pack* const pack,
                                       CPModelBuilder* const builder,
                                       const CPExtensionProto& proto) {
  std::vector<int64> weights;
  VERIFY(builder->ScanArguments(ModelVisitor::kCoefficientsArgument,
                                proto,
                                &weights));
  IntExpr* target = NULL;
  VERIFY(builder->ScanArguments(ModelVisitor::kTargetArgument, proto, &target));
  pack->AddWeightedSumOfAssignedDimension(weights, target->Var());
  return true;
}

#define IS_TYPE(index, builder, tag)            \
  index == builder->TagIndex(ModelVisitor::tag)

Constraint* BuildPack(CPModelBuilder* const builder,
                      const CPConstraintProto& proto) {
  std::vector<IntVar*> vars;
  VERIFY(builder->ScanArguments(ModelVisitor::kVarsArgument, proto, &vars));
  int64 bins = 0;
  VERIFY(builder->ScanArguments(ModelVisitor::kSizeArgument, proto, &bins));
  Pack* const pack = builder->solver()->MakePack(vars, bins);
  // Add dimensions. They are stored as extensions in the proto.
  for (int i = 0; i < proto.extensions_size(); ++i) {
    const CPExtensionProto& dimension_proto = proto.extensions(i);
    const int type_index = dimension_proto.type_index();
    if (IS_TYPE(type_index, builder, kUsageLessConstantExtension)) {
      VERIFY(AddUsageLessConstantDimension(pack, builder, dimension_proto));
    } else if (IS_TYPE(type_index, builder, kCountAssignedItemsExtension)) {
      VERIFY(AddCountAssignedItemsDimension(pack, builder, dimension_proto));
    } else if (IS_TYPE(type_index, builder, kCountUsedBinsExtension)) {
      VERIFY(AddCountUsedBinDimension(pack, builder, dimension_proto));
    } else if (IS_TYPE(type_index, builder, kUsageEqualVariableExtension)) {
      VERIFY(AddUsageEqualVariableDimension(pack, builder, dimension_proto));
    } else if (IS_TYPE(type_index,
                       builder,
                       kVariableUsageLessConstantExtension)) {
      VERIFY(AddVariableUsageLessConstantDimension(pack,
                                                   builder,
                                                   dimension_proto));
    } else if (IS_TYPE(type_index,
                       builder,
                       kWeightedSumOfAssignedEqualVariableExtension)) {
      VERIFY(AddWeightedSumOfAssignedDimension(pack, builder, dimension_proto));
    } else {
      LOG(ERROR) << "Unrecognized extension " << dimension_proto.DebugString();
      return NULL;
    }
  }
  return pack;
}
#undef IS_TYPE

// ----- kPathCumul -----

Constraint* BuildPathCumul(CPModelBuilder* const builder,
                           const CPConstraintProto& proto) {
  std::vector<IntVar*> nexts;
  VERIFY(builder->ScanArguments(ModelVisitor::kNextsArgument, proto, &nexts));
  std::vector<IntVar*> active;
  VERIFY(builder->ScanArguments(ModelVisitor::kActiveArgument, proto, &active));
  std::vector<IntVar*> cumuls;
  VERIFY(builder->ScanArguments(ModelVisitor::kCumulsArgument, proto, &cumuls));
  std::vector<IntVar*> transits;
  VERIFY(builder->ScanArguments(ModelVisitor::kTransitsArgument,
                                proto,
                                &transits));
  return builder->solver()->MakePathCumul(nexts, active, cumuls, transits);
}

// ----- kPerformedExpr -----

IntExpr* BuildPerformedExpr(CPModelBuilder* const builder,
                            const CPIntegerExpressionProto& proto) {
  IntervalVar* var = NULL;
  VERIFY(builder->ScanArguments(ModelVisitor::kIntervalArgument, proto, &var));
  return var->PerformedExpr();
}

// ----- kProduct -----

IntExpr* BuildProduct(CPModelBuilder* const builder,
                      const CPIntegerExpressionProto& proto) {
  IntExpr* left = NULL;
  if (builder->ScanArguments(ModelVisitor::kLeftArgument, proto, &left)) {
    IntExpr* right = NULL;
    VERIFY(builder->ScanArguments(ModelVisitor::kRightArgument, proto, &right));
    return builder->solver()->MakeProd(left, right);
  }
  IntExpr* expr = NULL;
  VERIFY(builder->ScanArguments(ModelVisitor::kExpressionArgument,
                                proto,
                                &expr));
  int64 value = 0;
  VERIFY(builder->ScanArguments(ModelVisitor::kValueArgument, proto, &value));
  return builder->solver()->MakeProd(expr, value);
}

// ----- kScalProd -----

IntExpr* BuildScalProd(CPModelBuilder* const builder,
                       const CPIntegerExpressionProto& proto) {
  std::vector<IntVar*> vars;
  VERIFY(builder->ScanArguments(ModelVisitor::kVarsArgument, proto, &vars));
  std::vector<int64> values;
  VERIFY(builder->ScanArguments(ModelVisitor::kCoefficientsArgument,
                                proto,
                                &values));
  return builder->solver()->MakeScalProd(vars, values);
}

// ----- kScalProdEqual -----

Constraint* BuildScalProdEqual(CPModelBuilder* const builder,
                               const CPConstraintProto& proto) {
  std::vector<IntVar*> vars;
  VERIFY(builder->ScanArguments(ModelVisitor::kVarsArgument, proto, &vars));
  std::vector<int64> values;
  VERIFY(builder->ScanArguments(ModelVisitor::kCoefficientsArgument,
                                proto,
                                &values));
  int64 value = 0;
  VERIFY(builder->ScanArguments(ModelVisitor::kValueArgument, proto, &value));
  return builder->solver()->MakeScalProdEquality(vars, values, value);
}

// ----- kScalProdGreaterOrEqual -----

Constraint* BuildScalProdGreaterOrEqual(CPModelBuilder* const builder,
                                        const CPConstraintProto& proto) {
  std::vector<IntVar*> vars;
  VERIFY(builder->ScanArguments(ModelVisitor::kVarsArgument, proto, &vars));
  std::vector<int64> values;
  VERIFY(builder->ScanArguments(ModelVisitor::kCoefficientsArgument,
                                proto,
                                &values));
  int64 value = 0;
  VERIFY(builder->ScanArguments(ModelVisitor::kValueArgument, proto, &value));
  return builder->solver()->MakeScalProdGreaterOrEqual(vars, values, value);
}

// ----- kScalProdLessOrEqual -----

Constraint* BuildScalProdLessOrEqual(CPModelBuilder* const builder,
                                     const CPConstraintProto& proto) {
  std::vector<IntVar*> vars;
  VERIFY(builder->ScanArguments(ModelVisitor::kVarsArgument, proto, &vars));
  std::vector<int64> values;
  VERIFY(builder->ScanArguments(ModelVisitor::kCoefficientsArgument,
                                proto,
                                &values));
  int64 value = 0;
  VERIFY(builder->ScanArguments(ModelVisitor::kValueArgument, proto, &value));
  return builder->solver()->MakeScalProdLessOrEqual(vars, values, value);
}

// ----- kSemiContinuous -----

IntExpr* BuildSemiContinuous(CPModelBuilder* const builder,
                             const CPIntegerExpressionProto& proto) {
  IntExpr* expr = NULL;
  VERIFY(builder->ScanArguments(ModelVisitor::kExpressionArgument,
                                proto,
                                &expr));
  int64 fixed_charge = 0;
  VERIFY(builder->ScanArguments(ModelVisitor::kFixedChargeArgument,
                                proto,
                                &fixed_charge));
  int64 step = 0;
  VERIFY(builder->ScanArguments(ModelVisitor::kStepArgument, proto, &step));
  return builder->solver()->MakeSemiContinuousExpr(expr, fixed_charge, step);
}

// ----- kSequence -----

Constraint* BuildSequence(CPModelBuilder* const builder,
                          const CPConstraintProto& proto) {
  std::vector<IntervalVar*> vars;
  VERIFY(builder->ScanArguments(ModelVisitor::kIntervalsArgument,
                                proto,
                                &vars));
  return builder->solver()->MakeSequence(vars, proto.name());
}

// ----- kSquare -----

IntExpr* BuildSquare(CPModelBuilder* const builder,
                     const CPIntegerExpressionProto& proto) {
  IntExpr* expr = NULL;
  VERIFY(builder->ScanArguments(ModelVisitor::kExpressionArgument,
                                proto,
                                &expr));
  return builder->solver()->MakeSquare(expr);
}

// ----- kStartExpr -----

IntExpr* BuildStartExpr(CPModelBuilder* const builder,
                        const CPIntegerExpressionProto& proto) {
  IntervalVar* var = NULL;
  VERIFY(builder->ScanArguments(ModelVisitor::kIntervalArgument, proto, &var));
  return var->StartExpr();
}

// ----- kSum -----

IntExpr* BuildSum(CPModelBuilder* const builder,
                  const CPIntegerExpressionProto& proto) {
  IntExpr* left = NULL;
  if (builder->ScanArguments(ModelVisitor::kLeftArgument, proto, &left)) {
    IntExpr* right = NULL;
    VERIFY(builder->ScanArguments(ModelVisitor::kRightArgument, proto, &right));
    return builder->solver()->MakeSum(left, right);
  }
  IntExpr* expr = NULL;
  if (builder->ScanArguments(
          ModelVisitor::kExpressionArgument, proto, &expr)) {
    int64 value = 0;
    VERIFY(builder->ScanArguments(ModelVisitor::kValueArgument, proto, &value));
    return builder->solver()->MakeSum(expr, value);
  }
  std::vector<IntVar*> vars;
  VERIFY(builder->ScanArguments(ModelVisitor::kVarsArgument, proto, &vars));
  return builder->solver()->MakeSum(vars);
}

// ----- kSumEqual -----

Constraint* BuildSumEqual(CPModelBuilder* const builder,
                          const CPConstraintProto& proto) {
  std::vector<IntVar*> vars;
  VERIFY(builder->ScanArguments(ModelVisitor::kVarsArgument, proto, &vars));
  int64 value = 0;
  if (builder->ScanArguments(ModelVisitor::kValueArgument, proto, &value)) {
    return builder->solver()->MakeSumEquality(vars, value);
  }
  IntExpr* target = NULL;
  VERIFY(builder->ScanArguments(ModelVisitor::kTargetArgument, proto, &target));
  return builder->solver()->MakeSumEquality(vars, target->Var());
}

// ----- kSumGreaterOrEqual -----

Constraint* BuildSumGreaterOrEqual(CPModelBuilder* const builder,
                                   const CPConstraintProto& proto) {
  std::vector<IntVar*> vars;
  VERIFY(builder->ScanArguments(ModelVisitor::kVarsArgument, proto, &vars));
  int64 value = 0;
  VERIFY(builder->ScanArguments(ModelVisitor::kValueArgument, proto, &value));
  return builder->solver()->MakeSumGreaterOrEqual(vars, value);
}

// ----- kSumLessOrEqual -----

Constraint* BuildSumLessOrEqual(CPModelBuilder* const builder,
                                const CPConstraintProto& proto) {
  std::vector<IntVar*> vars;
  VERIFY(builder->ScanArguments(ModelVisitor::kVarsArgument, proto, &vars));
  int64 value = 0;
  VERIFY(builder->ScanArguments(ModelVisitor::kValueArgument, proto, &value));
  return builder->solver()->MakeSumLessOrEqual(vars, value);
}

// ----- kTransition -----

Constraint* BuildTransition(CPModelBuilder* const builder,
                            const CPConstraintProto& proto) {
  std::vector<IntVar*> vars;
  VERIFY(builder->ScanArguments(ModelVisitor::kVarsArgument, proto, &vars));
  std::vector<std::vector<int64> > tuples;
  VERIFY(builder->ScanArguments(ModelVisitor::kTuplesArgument, proto, &tuples));
  int64 initial_state = 0;
  VERIFY(builder->ScanArguments(ModelVisitor::kInitialState,
                                proto,
                                &initial_state));
  std::vector<int64> final_states;
  VERIFY(builder->ScanArguments(ModelVisitor::kFinalStatesArgument,
                                proto,
                                &final_states));



  return builder->solver()->MakeTransitionConstraint(vars,
                                                     tuples,
                                                     initial_state,
                                                     final_states);
}

// ----- kTrueConstraint -----

Constraint* BuildTrueConstraint(CPModelBuilder* const builder,
                                const CPConstraintProto& proto) {
  return builder->solver()->MakeTrueConstraint();
}

#undef VERIFY
#undef VERIFY_EQ
}  // namespace

// ----- CPModelBuilder -----

bool CPModelBuilder::BuildFromProto(const CPIntegerExpressionProto& proto) {
  const int index = proto.index();
  const int tag_index = proto.type_index();
  Solver::IntegerExpressionBuilder* const builder =
      solver_->GetIntegerExpressionBuilder(tags_.Element(tag_index));
  if (!builder) {
    return false;
  }
  IntExpr* const built = builder->Run(this, proto);
  if (!built) {
    return false;
  }
  expressions_.resize(std::max(static_cast<int>(expressions_.size()),
                               index + 1));
  expressions_[index] = built;
  return true;
}

Constraint* CPModelBuilder::BuildFromProto(const CPConstraintProto& proto) {
  const int tag_index = proto.type_index();
  Solver::ConstraintBuilder* const builder =
      solver_->GetConstraintBuilder(tags_.Element(tag_index));
  if (!builder) {
    return NULL;
  }
  Constraint* const built = builder->Run(this, proto);
  return built;
}

bool CPModelBuilder::BuildFromProto(const CPIntervalVariableProto& proto) {
  const int index = proto.index();
  const int tag_index = proto.type_index();
  Solver::IntervalVariableBuilder* const builder =
      solver_->GetIntervalVariableBuilder(tags_.Element(tag_index));
  if (!builder) {
    return NULL;
  }
  IntervalVar* const built = builder->Run(this, proto);
  if (!built) {
    return false;
  }
  intervals_.resize(std::max(static_cast<int>(intervals_.size()), index + 1));
  intervals_[index] = built;
  return true;
}

IntExpr* CPModelBuilder::IntegerExpression(int index) const {
  CHECK_GE(index, 0);
  CHECK_LT(index, expressions_.size());
  CHECK_NOTNULL(expressions_[index]);
  return expressions_[index];
}

IntervalVar* CPModelBuilder::IntervalVariable(int index) const {
  CHECK_GE(index, 0);
  CHECK_LT(index, intervals_.size());
  CHECK_NOTNULL(intervals_[index]);
  return intervals_[index];
}

bool CPModelBuilder::ScanOneArgument(int type_index,
                                     const CPArgumentProto& arg_proto,
                                     int64* to_fill) {
  if (arg_proto.argument_index() == type_index &&
      arg_proto.has_integer_value()) {
    *to_fill = arg_proto.integer_value();
    return true;
  }
  return false;
}

bool CPModelBuilder::ScanOneArgument(int type_index,
                                     const CPArgumentProto& arg_proto,
                                     IntExpr** to_fill) {
  if (arg_proto.argument_index() == type_index &&
      arg_proto.has_integer_expression_index()) {
    const int expression_index = arg_proto.integer_expression_index();
    CHECK_NOTNULL(expressions_[expression_index]);
    *to_fill = expressions_[expression_index];
    return true;
  }
  return false;
}

bool CPModelBuilder::ScanOneArgument(int type_index,
                                     const CPArgumentProto& arg_proto,
                                     std::vector<int64>* to_fill) {
  if (arg_proto.argument_index() == type_index) {
    const int values_size = arg_proto.integer_array_size();
    for (int j = 0; j < values_size; ++j) {
      to_fill->push_back(arg_proto.integer_array(j));
    }
    return true;
  }
  return false;
}

bool CPModelBuilder::ScanOneArgument(int type_index,
                                     const CPArgumentProto& arg_proto,
                                     std::vector<std::vector<int64> >* to_fill) {
  if (arg_proto.argument_index() == type_index &&
      arg_proto.has_integer_matrix()) {
    to_fill->clear();
    const CPIntegerMatrixProto& matrix = arg_proto.integer_matrix();
    const int rows = matrix.rows();
    const int columns = matrix.columns();
    to_fill->resize(rows);
    int counter = 0;
    for (int i = 0; i < rows; ++i) {
      for (int j = 0; j < columns; ++j) {
        const int64 value = matrix.values(counter++);
        (*to_fill)[i].push_back(value);
      }
    }
    CHECK_EQ(matrix.values_size(), counter);
    return true;
  }
  return false;
}

bool CPModelBuilder::ScanOneArgument(int type_index,
                                     const CPArgumentProto& arg_proto,
                                     std::vector<IntVar*>* to_fill) {
  if (arg_proto.argument_index() == type_index) {
    const int vars_size = arg_proto.integer_expression_array_size();
    for (int j = 0; j < vars_size; ++j) {
      const int expression_index = arg_proto.integer_expression_array(j);
      CHECK_NOTNULL(expressions_[expression_index]);
      to_fill->push_back(expressions_[expression_index]->Var());
    }
    return true;
  }
  return false;
}

bool CPModelBuilder::ScanOneArgument(int type_index,
                                     const CPArgumentProto& arg_proto,
                                     IntervalVar** to_fill) {
  if (arg_proto.argument_index() == type_index &&
      arg_proto.has_interval_index()) {
    const int interval_index = arg_proto.interval_index();
    CHECK_NOTNULL(intervals_[interval_index]);
    *to_fill = intervals_[interval_index];
    return true;
  }
  return false;
}

bool CPModelBuilder::ScanOneArgument(int type_index,
                                     const CPArgumentProto& arg_proto,
                                     std::vector<IntervalVar*>* to_fill) {
  if (arg_proto.argument_index() == type_index) {
    const int vars_size = arg_proto.interval_array_size();
    for (int j = 0; j < vars_size; ++j) {
      const int interval_index = arg_proto.interval_array(j);
      CHECK_NOTNULL(intervals_[interval_index]);
      to_fill->push_back(intervals_[interval_index]);
    }
    return true;
  }
  return false;
}

// ----- Solver API -----

void Solver::ExportModel(const std::vector<SearchMonitor*>& monitors,
                         CPModelProto* const model_proto) const {
  CHECK_NOTNULL(model_proto);
  FirstPassVisitor first_pass;
  Accept(&first_pass);
  for (ConstIter<std::vector<SearchMonitor*> > it(monitors); !it.at_end(); ++it) {
    (*it)->Accept(&first_pass);
  }
  SecondPassVisitor second_pass(first_pass, model_proto);
  for (ConstIter<std::vector<SearchMonitor*> > it(monitors); !it.at_end(); ++it) {
    (*it)->Accept(&second_pass);
  }
  Accept(&second_pass);
}

void Solver::ExportModel(CPModelProto* const model_proto) const {
  CHECK_NOTNULL(model_proto);
  FirstPassVisitor first_pass;
  Accept(&first_pass);
  SecondPassVisitor second_pass(first_pass, model_proto);
  Accept(&second_pass);
}

bool Solver::LoadModel(const CPModelProto& model_proto) {
  return LoadModel(model_proto, NULL);
}

bool Solver::LoadModel(const CPModelProto& model_proto,
                       std::vector<SearchMonitor*>* monitors) {
  if (model_proto.version() > kModelVersion) {
    LOG(ERROR) << "Model protocol buffer version is greater than"
               << " the one compiled in the reader ("
               << model_proto.version() << " vs " << kModelVersion << ")";
    return false;
  }
  CPModelBuilder builder(this);
  for (int i = 0; i < model_proto.tags_size(); ++i) {
    builder.AddTag(model_proto.tags(i));
  }
  for (int i = 0; i < model_proto.intervals_size(); ++i) {
    if (!builder.BuildFromProto(model_proto.intervals(i))) {
      LOG(ERROR) << "Interval variable proto "
                 << model_proto.intervals(i).DebugString()
                 << " was not parsed correctly";
      return false;
    }
  }
  for (int i = 0; i < model_proto.expressions_size(); ++i) {
    if (!builder.BuildFromProto(model_proto.expressions(i))) {
      LOG(ERROR) << "Integer expression proto "
                 << model_proto.expressions(i).DebugString()
                 << " was not parsed correctly";
      return false;
    }
  }
  for (int i = 0; i < model_proto.constraints_size(); ++i) {
    Constraint* const constraint =
        builder.BuildFromProto(model_proto.constraints(i));
    if (constraint == NULL) {
      LOG(ERROR) << "Constraint proto "
                 << model_proto.constraints(i).DebugString()
                 << " was not parsed correctly";
      return false;
    }
    AddConstraint(constraint);
  }
  if (monitors != NULL) {
    if (model_proto.has_search_limit()) {
      monitors->push_back(MakeLimit(model_proto.search_limit()));
    }
    if (model_proto.has_objective()) {
      const CPObjectiveProto& objective_proto = model_proto.objective();
      IntVar* const objective_var =
          builder.IntegerExpression(objective_proto.objective_index())->Var();
      const bool maximize = objective_proto.maximize();
      const int64 step = objective_proto.step();
      OptimizeVar* const objective =
          MakeOptimize(maximize, objective_var, step);
      monitors->push_back(objective);
    }
  }
  return true;
}

bool Solver::UpgradeModel(CPModelProto* const proto) {
  if (proto->version() == kModelVersion) {
    LOG(INFO) << "Model already up to date with version " << kModelVersion;
  }
  return true;
}

void Solver::RegisterBuilder(const string& tag,
                             ConstraintBuilder* const builder) {
  InsertOrDie(&constraint_builders_, tag, builder);
}

void Solver::RegisterBuilder(const string& tag,
                             IntegerExpressionBuilder* const builder) {
  InsertOrDie(&expression_builders_, tag, builder);
}

void Solver::RegisterBuilder(const string& tag,
                             IntervalVariableBuilder* const builder) {
  InsertOrDie(&interval_builders_, tag, builder);
}

Solver::ConstraintBuilder*
Solver::GetConstraintBuilder(const string& tag) const {
  return FindPtrOrNull(constraint_builders_, tag);
}

Solver::IntegerExpressionBuilder*
Solver::GetIntegerExpressionBuilder(const string& tag) const {
  return FindPtrOrNull(expression_builders_, tag);
}

Solver::IntervalVariableBuilder*
Solver::GetIntervalVariableBuilder(const string& tag) const {
  IntervalVariableBuilder* const builder =
      FindPtrOrNull(interval_builders_, tag);
  return builder;
}

// ----- Manage builders -----

#define REGISTER(tag, func)                                             \
  RegisterBuilder(ModelVisitor::tag, NewPermanentCallback(&func))

void Solver::InitBuilders() {
  REGISTER(kAbs, BuildAbs);
  REGISTER(kAllDifferent, BuildAllDifferent);
  REGISTER(kAllowedAssignments, BuildAllowedAssignments);
  REGISTER(kBetween, BuildBetween);
  REGISTER(kConvexPiecewise, BuildConvexPiecewise);
  REGISTER(kCountEqual, BuildCountEqual);
  REGISTER(kCumulative, BuildCumulative);
  REGISTER(kDeviation, BuildDeviation);
  REGISTER(kDifference, BuildDifference);
  REGISTER(kDistribute, BuildDistribute);
  REGISTER(kDivide, BuildDivide);
  REGISTER(kDurationExpr, BuildDurationExpr);
  REGISTER(kElement, BuildElement);
  //  REGISTER(kElementEqual, BuildElementEqual);
  REGISTER(kEndExpr, BuildEndExpr);
  REGISTER(kEquality, BuildEquality);
  REGISTER(kFalseConstraint, BuildFalseConstraint);
  REGISTER(kGreater, BuildGreater);
  REGISTER(kGreaterOrEqual, BuildGreaterOrEqual);
  REGISTER(kIntegerVariable, BuildIntegerVariable);
  REGISTER(kIntervalBinaryRelation, BuildIntervalBinaryRelation);
  REGISTER(kIntervalDisjunction, BuildIntervalDisjunction);
  REGISTER(kIntervalUnaryRelation, BuildIntervalUnaryRelation);
  REGISTER(kIntervalVariable, BuildIntervalVariable);
  REGISTER(kIsBetween, BuildIsBetween);
  REGISTER(kIsDifferent, BuildIsDifferent);
  REGISTER(kIsEqual, BuildIsEqual);
  REGISTER(kIsGreaterOrEqual, BuildIsGreaterOrEqual);
  REGISTER(kIsLessOrEqual, BuildIsLessOrEqual);
  REGISTER(kIsMember, BuildIsMember);
  REGISTER(kLess, BuildLess);
  REGISTER(kLessOrEqual, BuildLessOrEqual);
  REGISTER(kMapDomain, BuildMapDomain);
  REGISTER(kMax, BuildMax);
  //  REGISTER(kMaxEqual, BuildMaxEqual);
  REGISTER(kMember, BuildMember);
  REGISTER(kMin, BuildMin);
  //  REGISTER(kMinEqual, BuildMinEqual);
  REGISTER(kNoCycle, BuildNoCycle);
  REGISTER(kNonEqual, BuildNonEqual);
  REGISTER(kOpposite, BuildOpposite);
  REGISTER(kPack, BuildPack);
  REGISTER(kPathCumul, BuildPathCumul);
  REGISTER(kPerformedExpr, BuildPerformedExpr);
  REGISTER(kProduct, BuildProduct);
  REGISTER(kScalProd, BuildScalProd);
  REGISTER(kScalProdEqual, BuildScalProdEqual);
  REGISTER(kScalProdGreaterOrEqual, BuildScalProdGreaterOrEqual);
  REGISTER(kScalProdLessOrEqual, BuildScalProdLessOrEqual);
  REGISTER(kSemiContinuous, BuildSemiContinuous);
  REGISTER(kSequence, BuildSequence);
  REGISTER(kSquare, BuildSquare);
  REGISTER(kStartExpr, BuildStartExpr);
  REGISTER(kSum, BuildSum);
  REGISTER(kSumEqual, BuildSumEqual);
  REGISTER(kSumGreaterOrEqual, BuildSumGreaterOrEqual);
  REGISTER(kSumLessOrEqual, BuildSumLessOrEqual);
  REGISTER(kTransition, BuildTransition);
  REGISTER(kTrueConstraint, BuildTrueConstraint);
}
#undef REGISTER

void Solver::DeleteBuilders() {
  STLDeleteValues(&expression_builders_);
  STLDeleteValues(&constraint_builders_);
  STLDeleteValues(&interval_builders_);
}
}  // namespace operations_research