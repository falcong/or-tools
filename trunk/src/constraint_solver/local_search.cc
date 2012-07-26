// Copyright 2010-2012 Google
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

#include <string.h>
#include <algorithm>
#include "base/hash.h"
#include "base/hash.h"
#include <iterator>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "base/callback.h"
#include "base/commandlineflags.h"
#include "base/integral_types.h"
#include "base/logging.h"
#include "base/macros.h"
#include "base/scoped_ptr.h"
#include "base/bitmap.h"
#include "base/concise_iterator.h"
#include "base/map-util.h"
#include "base/hash.h"
#include "constraint_solver/constraint_solver.h"
#include "constraint_solver/constraint_solveri.h"
#include "graph/hamiltonian_path.h"
#include "base/random.h"

DEFINE_int32(cp_local_search_sync_frequency, 16,
             "Frequency of checks for better solutions in the solution pool.");

DEFINE_int32(cp_local_search_tsp_opt_size, 13,
             "Size of TSPs solved in the TSPOpt operator.");

DEFINE_int32(cp_local_search_tsp_lns_size, 10,
             "Size of TSPs solved in the TSPLns operator.");

namespace operations_research {

// Utility methods to ensure the communication between local search and the
// search.

// Returns true if a local optimum has been reached and cannot be improved.
bool LocalOptimumReached(Search* const search);

// Returns true if the search accepts the delta (actually checking this by
// calling AcceptDelta on the monitors of the search).
bool AcceptDelta(Search* const search,
                 Assignment* delta,
                 Assignment* deltadelta);

// Notifies the search that a neighbor has been accepted by local search.
void AcceptNeighbor(Search* const search);

// ----- Base operator class for operators manipulating IntVars -----

IntVarLocalSearchOperator::IntVarLocalSearchOperator()
: vars_(NULL),
  size_(0),
  values_(NULL),
  old_values_(NULL),
  activated_(0, false),
  was_activated_(0, false),
  has_changed_(0, false),
  has_delta_changed_(0, false),
  cleared_(true) {}

IntVarLocalSearchOperator::IntVarLocalSearchOperator(const IntVar* const* vars,
                                                     int size)
    : vars_(NULL),
      size_(0),
      values_(NULL),
      old_values_(NULL),
      activated_(size, false),
      was_activated_(size, false),
      has_changed_(size, false),
      has_delta_changed_(size, false),
      cleared_(true) {
  CHECK_GE(size_, 0);
  AddVars(vars, size);
}

IntVarLocalSearchOperator::~IntVarLocalSearchOperator() {}

void IntVarLocalSearchOperator::AddVars(const IntVar* const* vars, int size) {
  if (size > 0) {
    const int new_size = size_ + size;
    IntVar** new_vars = new IntVar*[new_size];
    if (size_ > 0) {
      memcpy(new_vars, vars_.get(), size_ * sizeof(*new_vars));
    }
    memcpy(new_vars + size_, vars, size * sizeof(*vars));
    vars_.reset(new_vars);
    values_.reset(new int64[new_size]);
    old_values_.reset(new int64[new_size]);
    activated_.Resize(new_size, false);
    was_activated_.Resize(new_size, false);
    has_changed_.Resize(new_size, false);
    has_delta_changed_.Resize(new_size, false);
    size_ = new_size;
  }
}

void IntVarLocalSearchOperator::Start(const Assignment* assignment) {
  const Assignment::IntContainer& container = assignment->IntVarContainer();
  const int size = Size();
  CHECK_LE(size, container.Size())
      << "Assignment contains fewer variables than operator";
  for (int i = 0; i < size; ++i) {
    const IntVarElement* element = &(container.Element(i));
    IntVar* const var = vars_[i];
    if (element->Var() != var) {
      CHECK(container.Contains(var))
          << "Assignment does not contain operator variable " << var;
      element = &(container.Element(vars_[i]));
    }
    const int64 value = element->Value();
    values_[i] = value;
    old_values_[i] = value;
    const bool activated = element->Activated();
    activated_.Set(i, activated);
    was_activated_.Set(i, activated);
  }
  OnStart();
}

void IntVarLocalSearchOperator::SetValue(int64 index, int64 value) {
  values_[index] = value;
  MarkChange(index);
}

bool IntVarLocalSearchOperator::Activated(int64 index) const {
  return activated_.Get(index);
}

void IntVarLocalSearchOperator::Activate(int64 index) {
  activated_.Set(index, true);
  MarkChange(index);
}

void IntVarLocalSearchOperator::Deactivate(int64 index) {
  activated_.Set(index, false);
  MarkChange(index);
}

bool IntVarLocalSearchOperator::ApplyChanges(Assignment* delta,
                                             Assignment* deltadelta) const {
  for (ConstIter<std::vector<int64> > it(changes_); !it.at_end(); ++it) {
    const int64 index = *it;
    IntVar* var = Var(index);
    const int64 value = Value(index);
    const bool activated = activated_.Get(index);
    if (!activated) {
      if (!cleared_ && has_delta_changed_.Get(index) && IsIncremental()) {
        deltadelta->FastAdd(var).Deactivate();
      }
      delta->FastAdd(var).Deactivate();
    } else if (value != OldValue(index) || !SkipUnchanged(index)) {
      if (!cleared_ && has_delta_changed_.Get(index) && IsIncremental()) {
        deltadelta->FastAdd(var).SetValue(value);
      }
      delta->FastAdd(var).SetValue(value);
    }
  }
  return true;
}

void IntVarLocalSearchOperator::RevertChanges(bool incremental) {
  cleared_ = false;
  has_delta_changed_.SetAll(false);
  if (incremental && IsIncremental()) return;
  cleared_ = true;
  for (ConstIter<std::vector<int64> > it(changes_); !it.at_end(); ++it) {
    const int index = *it;
    values_[index] =  old_values_[index];
    activated_.Set(index, was_activated_.Get(index));
    has_changed_.Set(index, false);
  }
  changes_.clear();
}

void IntVarLocalSearchOperator::MarkChange(int64 index) {
  if (!has_delta_changed_.Get(index)) {
    has_delta_changed_.Set(index, true);
  }
  if (!has_changed_.Get(index)) {
    changes_.push_back(index);
    has_changed_.Set(index, true);
  }
}

bool IntVarLocalSearchOperator::MakeNextNeighbor(Assignment* delta,
                                                 Assignment* deltadelta) {
  CHECK_NOTNULL(delta);
  while (true) {
    RevertChanges(true);

    if (!MakeOneNeighbor()) {
      return false;
    }

    if (ApplyChanges(delta, deltadelta)) {
      VLOG(2) << "Delta = " << delta->DebugString();
      return true;
    }
  }
  return false;
}
// TODO(user): Make this a pure virtual.
bool IntVarLocalSearchOperator::MakeOneNeighbor() {
  return true;
}

// ----- Sequence Var Local Search Operator -----

SequenceVarLocalSearchOperator::SequenceVarLocalSearchOperator()
  : vars_(NULL),
    size_(0),
    values_(NULL),
    backward_values_(NULL),
    old_values_(NULL),
    activated_(0, false),
    was_activated_(0, false),
    has_changed_(0, false),
    has_delta_changed_(0, false),
    cleared_(true) {}

SequenceVarLocalSearchOperator::SequenceVarLocalSearchOperator(
    const SequenceVar* const* vars, int size)
    : vars_(NULL),
      size_(0),
      values_(NULL),
      backward_values_(NULL),
      old_values_(NULL),
      activated_(size, false),
      was_activated_(size, false),
      has_changed_(size, false),
      has_delta_changed_(size, false),
      cleared_(true) {
  CHECK_GE(size_, 0);
  AddVars(vars, size);
}

SequenceVarLocalSearchOperator::~SequenceVarLocalSearchOperator() {}

void SequenceVarLocalSearchOperator::AddVars(const SequenceVar* const* vars,
                                             int size) {
  if (size > 0) {
    const int new_size = size_ + size;
    SequenceVar** const new_vars = new SequenceVar*[new_size];
    if (size_ > 0) {
      memcpy(new_vars, vars_.get(), size_ * sizeof(*new_vars));
    }
    memcpy(new_vars + size_, vars, size * sizeof(*vars));
    vars_.reset(new_vars);
    values_.reset(new std::vector<int>[new_size]);
    backward_values_.reset(new std::vector<int>[new_size]);
    old_values_.reset(new std::vector<int>[new_size]);
    activated_.Resize(new_size, false);
    was_activated_.Resize(new_size, false);
    has_changed_.Resize(new_size, false);
    has_delta_changed_.Resize(new_size, false);
    size_ = new_size;
  }
}

void SequenceVarLocalSearchOperator::Start(const Assignment* assignment) {
  const Assignment::SequenceContainer& container =
      assignment->SequenceVarContainer();
  const int size = Size();
  CHECK_LE(size, container.Size())
      << "Assignment contains fewer variables than operator";
  for (int i = 0; i < size; ++i) {
    const SequenceVarElement* element = &(container.Element(i));
    SequenceVar* const var = vars_[i];
    if (element->Var() != var) {
      CHECK(container.Contains(var))
          << "Assignment does not contain operator variable " << var;
      element = &(container.Element(vars_[i]));
    }
    const std::vector<int>& value = element->ForwardSequence();
    CHECK_EQ(vars_[i]->size(), value.size());
    values_[i] = value;
    backward_values_[i].clear();
    old_values_[i] = value;
    const bool activated = element->Activated();
    activated_.Set(i, activated);
    was_activated_.Set(i, activated);
  }
  OnStart();
}

void SequenceVarLocalSearchOperator::SetForwardSequence(
    int64 index, const std::vector<int>& value) {
  values_[index] = value;
  MarkChange(index);
}

void SequenceVarLocalSearchOperator::SetBackwardSequence(
    int64 index, const std::vector<int>& value) {
  backward_values_[index] = value;
  MarkChange(index);
}

bool SequenceVarLocalSearchOperator::Activated(int64 index) const {
  return activated_.Get(index);
}

void SequenceVarLocalSearchOperator::Activate(int64 index) {
  activated_.Set(index, true);
  MarkChange(index);
}

void SequenceVarLocalSearchOperator::Deactivate(int64 index) {
  activated_.Set(index, false);
  MarkChange(index);
}

bool SequenceVarLocalSearchOperator::ApplyChanges(
    Assignment* delta,
    Assignment* deltadelta) const {
  for (ConstIter<std::vector<int64> > it(changes_); !it.at_end(); ++it) {
    const int64 index = *it;
    SequenceVar* const var = Var(index);
    const std::vector<int>& value = Sequence(index);
    const bool activated = activated_.Get(index);
    if (!activated) {
      if (!cleared_ && has_delta_changed_.Get(index) && IsIncremental()) {
        deltadelta->FastAdd(var).Deactivate();
      }
      delta->FastAdd(var).Deactivate();
    } else if (value != OldSequence(index) || !SkipUnchanged(index)) {
      if (!cleared_ && has_delta_changed_.Get(index) && IsIncremental()) {
        SequenceVarElement* const fast_element = &deltadelta->FastAdd(var);
        fast_element->SetForwardSequence(value);
        fast_element->SetBackwardSequence(backward_values_[index]);
      }
      SequenceVarElement* const element = &delta->FastAdd(var);
      element->SetForwardSequence(value);
      element->SetBackwardSequence(backward_values_[index]);
    }
  }
  return true;
}

void SequenceVarLocalSearchOperator::RevertChanges(bool incremental) {
  cleared_ = false;
  has_delta_changed_.SetAll(false);
  if (incremental && IsIncremental()) return;
  cleared_ = true;
  for (ConstIter<std::vector<int64> > it(changes_); !it.at_end(); ++it) {
    const int index = *it;
    values_[index] =  old_values_[index];
    backward_values_[index].clear();
    activated_.Set(index, was_activated_.Get(index));
    has_changed_.Set(index, false);
  }
  changes_.clear();
}

void SequenceVarLocalSearchOperator::MarkChange(int64 index) {
  if (!has_delta_changed_.Get(index)) {
    has_delta_changed_.Set(index, true);
  }
  if (!has_changed_.Get(index)) {
    changes_.push_back(index);
    has_changed_.Set(index, true);
  }
}

// ----- Base Large Neighborhood Search operator -----

BaseLNS::BaseLNS(const IntVar* const* vars, int size)
    : IntVarLocalSearchOperator(vars, size) {}

BaseLNS::BaseLNS(const std::vector<IntVar*>& vars)
    : IntVarLocalSearchOperator(vars.data(), vars.size()) {}

BaseLNS::~BaseLNS() {}

bool BaseLNS::MakeOneNeighbor() {
  std::vector<int> fragment;
  if (NextFragment(&fragment)) {
    for (int i = 0; i < fragment.size(); ++i) {
      DCHECK_LT(fragment[i], Size());
      Deactivate(fragment[i]);
    }
    return true;
  } else {
    return false;
  }
}

void BaseLNS::OnStart() {
  InitFragments();
}

void BaseLNS::InitFragments() {}

// ----- Simple Large Neighborhood Search operator -----

// Frees number_of_variables (contiguous in vars) variables.

namespace {
class SimpleLNS : public BaseLNS {
 public:
  SimpleLNS(const IntVar* const* vars, int size, int number_of_variables)
      : BaseLNS(vars, size),
        index_(0),
        number_of_variables_(number_of_variables) {
    CHECK_GT(number_of_variables_, 0);
  }
  ~SimpleLNS() {}
  virtual void InitFragments() { index_ = 0; }
  virtual bool NextFragment(std::vector<int>* fragment);
 private:
  int index_;
  const int number_of_variables_;
};

bool SimpleLNS::NextFragment(std::vector<int>* fragment) {
  const int size = Size();
  if (index_ < size) {
    for (int i = index_; i < index_ + number_of_variables_; ++i) {
      fragment->push_back(i % size);
    }
    ++index_;
    return true;
  } else {
    return false;
  }
}

// ----- Random Large Neighborhood Search operator -----

// Frees up to number_of_variables random variables.

class RandomLNS : public BaseLNS {
 public:
  RandomLNS(const IntVar* const* vars,
            int size,
            int number_of_variables,
            int32 seed)
      : BaseLNS(vars, size),
        rand_(seed),
        number_of_variables_(number_of_variables) {
    CHECK_GT(number_of_variables_, 0);
    CHECK_LE(number_of_variables_, Size());
  }
  ~RandomLNS() {}
  virtual bool NextFragment(std::vector<int>* fragment);
 private:
  ACMRandom rand_;
  const int number_of_variables_;
};

bool RandomLNS::NextFragment(std::vector<int>* fragment) {
  for (int i = 0; i < number_of_variables_; ++i) {
    fragment->push_back(rand_.Uniform(Size()));
  }
  return true;
}
}  // namespace

LocalSearchOperator* Solver::MakeRandomLNSOperator(const std::vector<IntVar*>& vars,
                                                   int number_of_variables) {
  return MakeRandomLNSOperator(vars,
                               number_of_variables,
                               ACMRandom::HostnamePidTimeSeed());
}

LocalSearchOperator* Solver::MakeRandomLNSOperator(const std::vector<IntVar*>& vars,
                                                   int number_of_variables,
                                                   int32 seed) {
  return RevAlloc(new RandomLNS(vars.data(),
                                vars.size(),
                                number_of_variables,
                                seed));
}

// ----- Move Toward Target Local Search operator -----

// A local search operator that compares the current assignment with a target
// one, and that generates neighbors corresponding to a single variable being
// changed from its current value to its target value.
namespace {
class MoveTowardTargetLS: public IntVarLocalSearchOperator {
 public:
  MoveTowardTargetLS(const std::vector<IntVar*>& variables,
                     const std::vector<int64>& target_values)
      : IntVarLocalSearchOperator(variables.data(), variables.size()),
        target_(target_values),
        // Initialize variable_index_ at the number of the of variables minus
        // one, so that the first to be tried (after one increment) is the one
        // of index 0.
        variable_index_(Size() - 1) {
    CHECK_EQ(target_values.size(), variables.size()) << "Illegal arguments.";
  }

  virtual ~MoveTowardTargetLS() {}

 protected:
  // Make a neighbor assigning one variable to its target value.
  virtual bool MakeOneNeighbor() {
    while (num_var_since_last_start_ < Size()) {
      ++num_var_since_last_start_;
      variable_index_ = (variable_index_ + 1) % Size();
      const int64 target_value = target_.at(variable_index_);
      const int64 current_value = OldValue(variable_index_);
      if (current_value != target_value) {
        SetValue(variable_index_, target_value);
        return true;
      }
    }
    return false;
  }

 private:
  virtual void OnStart() {
    // Do not change the value of variable_index_: this way, we keep going from
    // where we last modified something. This is because we expect that most
    // often, the variables we have just checked are less likely to be able
    // to be changed to their target values than the ones we have not yet
    // checked.
    //
    // Consider the case where oddly indexed variables can be assigned to their
    // target values (no matter in what order they are considered), while even
    // indexed ones cannot. Restarting at index 0 each time an odd-indexed
    // variable is modified will cause a total of Theta(n^2) neighbors to be
    // generated, while not restarting will produce only Theta(n) neighbors.
    CHECK_GE(variable_index_, 0);
    CHECK_LT(variable_index_, Size());
    num_var_since_last_start_ = 0;
  }

  // Target values
  const std::vector<int64> target_;

  // Index of the next variable to try to restore
  int64 variable_index_;

  // Number of variables checked since the last call to OnStart().
  int64 num_var_since_last_start_;
};
}  // namespace

LocalSearchOperator* Solver::MakeMoveTowardTargetOperator(
    const Assignment& target) {
  typedef std::vector<IntVarElement> Elements;
  const Elements& elements = target.IntVarContainer().elements();
  // Copy target values and construct the vector of variables
  std::vector<IntVar*> vars;
  std::vector<int64> values;
  vars.reserve(target.NumIntVars());
  values.reserve(target.NumIntVars());
  for (ConstIter<Elements> it(elements); !it.at_end(); ++it) {
    vars.push_back(it->Var());
    values.push_back(it->Value());
  }
  return MakeMoveTowardTargetOperator(vars, values);
}

LocalSearchOperator* Solver::MakeMoveTowardTargetOperator(
      const std::vector<IntVar*>& variables,
      const std::vector<int64>& target_values) {
  return RevAlloc(new MoveTowardTargetLS(variables, target_values));
}

// ----- ChangeValue Operators -----

ChangeValue::ChangeValue(const IntVar* const* vars, int size)
    : IntVarLocalSearchOperator(vars, size), index_(0) {}

ChangeValue::~ChangeValue() {}

bool ChangeValue::MakeOneNeighbor() {
  const int size = Size();
  while (index_ < size) {
    const int64 value = ModifyValue(index_, Value(index_));
    SetValue(index_, value);
    ++index_;
    return true;
  }
  return false;
}

void ChangeValue::OnStart() {
  index_ = 0;
}

// Increments the current value of variables.

namespace {
class IncrementValue : public ChangeValue {
 public:
  IncrementValue(const IntVar* const* vars, int size)
      : ChangeValue(vars, size) {}
  virtual ~IncrementValue() {}
  virtual int64 ModifyValue(int64 index, int64 value) { return value + 1; }
};

// Decrements the current value of variables.

class DecrementValue : public ChangeValue {
 public:
  DecrementValue(const IntVar* const* vars, int size)
      : ChangeValue(vars, size) {}
  virtual ~DecrementValue() {}
  virtual int64 ModifyValue(int64 index, int64 value) { return value - 1; }
};
}  // namespace

// ----- Path-based Operators -----

PathOperator::PathOperator(const IntVar* const* next_vars,
                           const IntVar* const* path_vars,
                           int size,
                           int number_of_base_nodes)
    : IntVarLocalSearchOperator(next_vars, size),
      number_of_nexts_(size),
      ignore_path_vars_(path_vars == NULL),
      base_nodes_(number_of_base_nodes),
      end_nodes_(number_of_base_nodes),
      base_paths_(number_of_base_nodes),
      just_started_(false),
      first_start_(true) {
  if (!ignore_path_vars_) {
    AddVars(path_vars, size);
  }
}

void PathOperator::OnStart() {
  InitializeBaseNodes();
  OnNodeInitialization();
}

bool PathOperator::MakeOneNeighbor() {
  while (IncrementPosition()) {
    // Need to revert changes here since MakeNeighbor might have returned false
    // and have done changes in the previous iteration.
    RevertChanges(true);
    if (MakeNeighbor()) {
      return true;
    }
  }
  return false;
}

bool PathOperator::SkipUnchanged(int index) const {
  if (ignore_path_vars_) {
    return true;
  }
  if (index < number_of_nexts_) {
    int path_index = index + number_of_nexts_;
    return Value(path_index) == OldValue(path_index);
  } else {
    int next_index = index - number_of_nexts_;
    return Value(next_index) == OldValue(next_index);
  }
}

bool PathOperator::MoveChain(int64 before_chain,
                             int64 chain_end,
                             int64 destination) {
  if (CheckChainValidity(before_chain, chain_end, destination)
      && !IsPathEnd(chain_end)
      && !IsPathEnd(destination)) {
    const int64 destination_path = Path(destination);
    const int64 after_chain = Next(chain_end);
    SetNext(chain_end, Next(destination), destination_path);
    if (!ignore_path_vars_) {
      int current = destination;
      int next = Next(before_chain);
      while (current != chain_end) {
        SetNext(current, next, destination_path);
        current = next;
        next = Next(next);
      }
    } else {
      SetNext(destination, Next(before_chain), destination_path);
    }
    SetNext(before_chain, after_chain, Path(before_chain));
    return true;
  }
  return false;
}

bool PathOperator::ReverseChain(int64 before_chain,
                                int64 after_chain,
                                int64* chain_last) {
  if (CheckChainValidity(before_chain, after_chain, -1)) {
    int64 path = Path(before_chain);
    int64 current = Next(before_chain);
    if (current == after_chain) {
      return false;
    }
    int64 current_next = Next(current);
    SetNext(current, after_chain, path);
    while (current_next != after_chain) {
      const int64 next = Next(current_next);
      SetNext(current_next, current, path);
      current = current_next;
      current_next = next;
    }
    SetNext(before_chain, current, path);
    *chain_last = current;
    return true;
  }
  return false;
}

bool PathOperator::MakeActive(int64 node, int64 destination) {
  if (!IsPathEnd(destination)) {
    int64 destination_path = Path(destination);
    SetNext(node, Next(destination), destination_path);
    SetNext(destination, node, destination_path);
    return true;
  } else {
    return false;
  }
}

bool PathOperator::MakeChainInactive(int64 before_chain, int64 chain_end) {
  const int64 kNoPath = -1;
  if (CheckChainValidity(before_chain, chain_end, -1)
      && !IsPathEnd(chain_end)) {
    const int64 after_chain = Next(chain_end);
    int64 current = Next(before_chain);
    while (current != after_chain) {
      const int64 next = Next(current);
      SetNext(current, current, kNoPath);
      current = next;
    }
    SetNext(before_chain, after_chain, Path(before_chain));
    return true;
  }
  return false;
}

bool PathOperator::CheckEnds() const {
  const int base_node_size = base_nodes_.size();
  for (int i = 0; i < base_node_size; ++i) {
    if (base_nodes_[i] != end_nodes_[i]) {
      return true;
    }
  }
  return false;
}

bool PathOperator::IncrementPosition() {
  const int base_node_size = base_nodes_.size();
  if (!just_started_) {
    const int number_of_paths = path_starts_.size();
    // Finding next base node positions.
    // Increment the position of inner base nodes first (higher index nodes);
    // if a base node is at the end of a path, reposition it at the start
    // of the path and increment the position of the preceding base node (this
    // action is called a restart).
    int last_restarted = base_node_size;
    for (int i = base_node_size - 1; i >= 0; --i) {
      if (base_nodes_[i] < number_of_nexts_) {
        base_nodes_[i] = OldNext(base_nodes_[i]);
        break;
      }
      base_nodes_[i] = StartNode(i);
      last_restarted = i;
    }
    // At the end of the loop, base nodes with indexes in
    // [last_restarted, base_node_size[ have been restarted.
    // Restarted base nodes are then repositioned by the virtual
    // GetBaseNodeRestartPosition to reflect position constraints between
    // base nodes (by default GetBaseNodeRestartPosition leaves the nodes
    // at the start of the path).
    // Base nodes are repositioned in ascending order to ensure that all
    // base nodes "below" the node being repositioned have their final
    // position.
    for (int i = last_restarted; i < base_node_size; ++i) {
      base_nodes_[i] = GetBaseNodeRestartPosition(i);
    }
    if (last_restarted > 0) {
      return CheckEnds();
    }
    // If all base nodes have been restarted, base nodes are moved to new paths.
    for (int i = base_node_size - 1; i >= 0; --i) {
      const int next_path_index = base_paths_[i] + 1;
      if (next_path_index < number_of_paths) {
        base_paths_[i] = next_path_index;
        base_nodes_[i] = path_starts_[next_path_index];
        if (i == 0 || !OnSamePathAsPreviousBase(i)) {
          return CheckEnds();
        }
      } else {
        base_paths_[i] = 0;
        base_nodes_[i] = path_starts_[0];
      }
    }
  } else {
    just_started_ = false;
    return true;
  }
  return CheckEnds();
}

void PathOperator::InitializePathStarts() {
  path_starts_.clear();
  Bitmap has_prevs(number_of_nexts_, false);
  for (int i = 0; i < number_of_nexts_; ++i) {
    const int next = OldNext(i);
    if (next < number_of_nexts_) {
      has_prevs.Set(next, true);
    }
  }
  for (int i = 0; i < number_of_nexts_; ++i) {
    if (!has_prevs.Get(i)) {
      path_starts_.push_back(i);
    }
  }
}

void PathOperator::InitializeInactives() {
  inactives_.clear();
  for (int i = 0; i < number_of_nexts_; ++i) {
    inactives_.push_back(OldNext(i) == i);
  }
}

void PathOperator::InitializeBaseNodes() {
  InitializePathStarts();
  InitializeInactives();
  if (first_start_ || InitPosition()) {
    // Only do this once since the following starts will continue from the
    // preceding position
    for (int i = 0; i < base_nodes_.size(); ++i) {
      base_paths_[i] = 0;
      base_nodes_[i] = path_starts_[0];
    }
    first_start_ = false;
  }
  for (int i = 0; i < base_nodes_.size(); ++i) {
    // If base node has been made inactive, restart from path start.
    int64 base_node = base_nodes_[i];
    if (RestartAtPathStartOnSynchronize() || IsInactive(base_node)) {
      base_node = path_starts_[base_paths_[i]];
      base_nodes_[i] = base_node;
    }
    end_nodes_[i] = base_node;
  }
  // Repair end_nodes_ in case some must be on the same path and are not anymore
  // (due to other operators moving these nodes).
  for (int i = 1; i < base_nodes_.size(); ++i) {
    if (OnSamePathAsPreviousBase(i)
        && !OnSamePath(base_nodes_[i - 1], base_nodes_[i])) {
      const int64 base_node = base_nodes_[i -1];
      base_nodes_[i] = base_node;
      end_nodes_[i] = base_node;
    }
  }
  just_started_ = true;
}

bool PathOperator::OnSamePath(int64 node1, int64 node2) const {
  if (IsInactive(node1) != IsInactive(node2)) {
    return false;
  }
  for (int node = node1; !IsPathEnd(node); node = OldNext(node)) {
    if (node == node2) {
      return true;
    }
  }
  for (int node = node2; !IsPathEnd(node); node = OldNext(node)) {
    if (node == node1) {
      return true;
    }
  }
  return false;
}

// Rejects chain if chain_end is not after before_chain on the path or if
// the chain contains exclude. Given before_chain is the node before the
// chain, if before_chain and chain_end are the same the chain is rejected too.
// Also rejects cycles (cycle detection is detected through chain length
//  overflow).
bool PathOperator::CheckChainValidity(int64 before_chain,
                                      int64 chain_end,
                                      int64 exclude) const {
  if (before_chain == chain_end || before_chain == exclude) return false;
  int64 current = before_chain;
  int chain_size = 0;
  while (current != chain_end) {
    if (chain_size > number_of_nexts_) {
      return false;
    }
    if (IsPathEnd(current)) {
      return false;
    }
    current = Next(current);
    ++chain_size;
    if (current == exclude) {
      return false;
    }
  }
  return true;
}

namespace {
// ----- 2Opt -----

// Reverves a sub-chain of a path. It is called 2Opt because it breaks
// 2 arcs on the path; resulting paths are called 2-optimal.
// Possible neighbors for the path 1 -> 2 -> 3 -> 4 -> 5
// (where (1, 5) are first and last nodes of the path and can therefore not be
// moved):
// 1 -> 3 -> 2 -> 4 -> 5
// 1 -> 4 -> 3 -> 2 -> 5
// 1 -> 2 -> 4 -> 3 -> 5
class TwoOpt : public PathOperator {
 public:
  TwoOpt(const IntVar* const* vars,
         const IntVar* const* secondary_vars,
         int size)
      : PathOperator(vars, secondary_vars, size, 2), last_base_(-1), last_(-1) {
  }
  virtual ~TwoOpt() {}
  virtual bool MakeNeighbor();
  virtual bool IsIncremental() const { return true; }

 protected:
  virtual bool OnSamePathAsPreviousBase(int64 base_index) {
    // Both base nodes have to be on the same path.
    return true;
  }

 private:
  virtual void OnNodeInitialization() { last_ = -1; }

  int64 last_base_;
  int64 last_;
};

bool TwoOpt::MakeNeighbor() {
  DCHECK_EQ(StartNode(0), StartNode(1));
  if (last_base_ != BaseNode(0) || last_ == -1) {
    RevertChanges(false);
    if (IsPathEnd(BaseNode(0))) {
      last_ = -1;
      return false;
    }
    last_base_ = BaseNode(0);
    last_ = Next(BaseNode(0));
    int64 chain_last;
    if (ReverseChain(BaseNode(0), BaseNode(1), &chain_last)) {
      return true;
    } else {
      last_ = -1;
      return false;
    }
  } else {
    const int64 to_move = Next(last_);
    DCHECK_EQ(Next(to_move), BaseNode(1));
    return MoveChain(last_, to_move, BaseNode(0));
  }
}

// ----- Relocate -----

// Moves a sub-chain of a path to another position; the specified chain length
// is the fixed length of the chains being moved. When this length is 1 the
// operator simply moves a node to another position.
// Possible neighbors for the path 1 -> 2 -> 3 -> 4 -> 5, for a chain length
// of 2 (where (1, 5) are first and last nodes of the path and can
// therefore not be moved):
// 1 -> 4 -> 2 -> 3 -> 5
// 1 -> 3 -> 4 -> 2 -> 5
//
// Using Relocate with chain lengths of 1, 2 and 3 together is equivalent to
// the OrOpt operator on a path. The OrOpt operator is a limited version of
// 3Opt (breaks 3 arcs on a path).

class Relocate : public PathOperator {
 public:
  Relocate(const IntVar* const* vars,
           const IntVar* const* secondary_vars,
           int size,
           int64 chain_length = 1LL,
           bool single_path = false)
      : PathOperator(vars, secondary_vars, size, 2),
        chain_length_(chain_length),
        single_path_(single_path) {
    CHECK_GT(chain_length_, 0);
  }
  virtual ~Relocate() {}
  virtual bool MakeNeighbor();

 protected:
  virtual bool OnSamePathAsPreviousBase(int64 base_index) {
    // Both base nodes have to be on the same path when it's the single path
    // version.
    return single_path_;
  }

 private:
  const int64 chain_length_;
  const bool single_path_;
};

bool Relocate::MakeNeighbor() {
  DCHECK(!single_path_ || StartNode(0) == StartNode(1));
  const int64 before_chain = BaseNode(0);
  int64 chain_end = before_chain;
  for (int i = 0; i < chain_length_; ++i) {
    if (IsPathEnd(chain_end)) {
      return false;
    }
    chain_end = Next(chain_end);
  }
  const int64 destination = BaseNode(1);
  return MoveChain(before_chain, chain_end, destination);
}

// ----- Exchange -----

// Exchanges the positions of two nodes.
// Possible neighbors for the path 1 -> 2 -> 3 -> 4 -> 5
// (where (1, 5) are first and last nodes of the path and can therefore not
// be moved):
// 1 -> 3 -> 2 -> 4 -> 5
// 1 -> 4 -> 3 -> 2 -> 5
// 1 -> 2 -> 4 -> 3 -> 5

class Exchange : public PathOperator {
 public:
  Exchange(const IntVar* const* vars,
           const IntVar* const* secondary_vars,
           int size)
      : PathOperator(vars, secondary_vars, size, 2) {}
  virtual ~Exchange() {}
  virtual bool MakeNeighbor();
};

bool Exchange::MakeNeighbor() {
  const int64 prev_node0 = BaseNode(0);
  if (IsPathEnd(prev_node0)) return false;
  const int64 node0 = Next(prev_node0);
  const int64 prev_node1 = BaseNode(1);
  if (IsPathEnd(prev_node1)) return false;
  const int64 node1 = Next(prev_node1);
  if (node0 == prev_node1) {
    return MoveChain(prev_node1, node1, prev_node0);
  } else if (node1 == prev_node0) {
    return MoveChain(prev_node0, node0, prev_node1);
  } else {
    return MoveChain(prev_node0, node0, prev_node1)
        && MoveChain(node0, Next(node0), prev_node0);
  }
  return false;
}

// ----- Cross -----

// Cross echanges the starting chains of 2 paths, including exchanging the
// whole paths.
// First and last nodes are not moved.
// Possible neighbors for the paths 1 -> 2 -> 3 -> 4 -> 5 and 6 -> 7 -> 8
// (where (1, 5) and (6, 8) are first and last nodes of the paths and can
// therefore not be moved):
// 1 -> 7 -> 3 -> 4 -> 5  6 -> 2 -> 8
// 1 -> 7 -> 4 -> 5       6 -> 2 -> 3 -> 8
// 1 -> 7 -> 5            6 -> 2 -> 3 -> 4 -> 8

class Cross : public PathOperator {
 public:
  Cross(const IntVar* const* vars,
        const IntVar* const* secondary_vars,
        int size)
      : PathOperator(vars, secondary_vars, size, 2) {}
  virtual ~Cross() {}
  virtual bool MakeNeighbor();
};

bool Cross::MakeNeighbor() {
  const int64 node0 = BaseNode(0);
  const int64 start0 = StartNode(0);
  const int64 node1 = BaseNode(1);
  const int64 start1 = StartNode(1);
  if (start1 == start0) {
    return false;
  }
  if (!IsPathEnd(node0) && !IsPathEnd(node1)) {
    return MoveChain(start0, node0, start1)
        && MoveChain(node0, node1, start0);
  } else if (!IsPathEnd(node0)) {
    return MoveChain(start0, node0, start1);
  } else if (!IsPathEnd(node1)) {
    return MoveChain(start1, node1, start0);
  }
  return false;
}

// ----- BaseInactiveNodeToPathOperator -----
// Base class of path operators which make inactive nodes active.

class BaseInactiveNodeToPathOperator : public PathOperator {
 public:
  BaseInactiveNodeToPathOperator(const IntVar* const* vars,
                             const IntVar* const* secondary_vars,
                             int size,
                             int number_of_base_nodes)
      : PathOperator(vars, secondary_vars, size, number_of_base_nodes),
        inactive_node_(0) {}
  virtual ~BaseInactiveNodeToPathOperator() {}

 protected:
  virtual bool MakeOneNeighbor();
  int64 GetInactiveNode() const { return inactive_node_; }

 private:
  virtual void OnNodeInitialization();

  int inactive_node_;
};

void BaseInactiveNodeToPathOperator::OnNodeInitialization() {
  for (int i = 0; i < Size(); ++i) {
    if (IsInactive(i)) {
      inactive_node_ = i;
      return;
    }
  }
  inactive_node_ = Size();
}

bool BaseInactiveNodeToPathOperator::MakeOneNeighbor() {
  while (inactive_node_ < Size()) {
    if (!IsInactive(inactive_node_) || !PathOperator::MakeOneNeighbor()) {
      ResetPosition();
      ++inactive_node_;
    } else {
      return true;
    }
  }
  return false;
}

// ----- MakeActiveOperator -----

// MakeActiveOperator inserts an inactive node into a path.
// Possible neighbors for the path 1 -> 2 -> 3 -> 4 with 5 inactive (where 1 and
// 4 are first and last nodes of the path) are:
// 1 -> 5 -> 2 -> 3 -> 4
// 1 -> 2 -> 5 -> 3 -> 4
// 1 -> 2 -> 3 -> 5 -> 4

class MakeActiveOperator : public BaseInactiveNodeToPathOperator {
 public:
  MakeActiveOperator(const IntVar* const* vars,
                     const IntVar* const* secondary_vars,
                     int size)
      : BaseInactiveNodeToPathOperator(vars, secondary_vars, size, 1) {}
  virtual ~MakeActiveOperator() {}
  virtual bool MakeNeighbor();
};

bool MakeActiveOperator::MakeNeighbor() {
  return MakeActive(GetInactiveNode(), BaseNode(0));
}

// ----- MakeInactiveOperator -----

// MakeInactiveOperator makes path nodes inactive.
// Possible neighbors for the path 1 -> 2 -> 3 -> 4 (where 1 and 4 are first
// and last nodes of the path) are:
// 1 -> 3 -> 4 & 2 inactive
// 1 -> 2 -> 4 & 3 inactive

class MakeInactiveOperator : public PathOperator {
 public:
  MakeInactiveOperator(const IntVar* const* vars,
                       const IntVar* const* secondary_vars,
                       int size)
      : PathOperator(vars, secondary_vars, size, 1) {}
  virtual ~MakeInactiveOperator() {}
  virtual bool MakeNeighbor() {
    const int64 base = BaseNode(0);
    if (IsPathEnd(base)) {
      return false;
    }
    return MakeChainInactive(base,  Next(base));
  }
};

// ----- SwapActiveOperator -----

// SwapActiveOperator replaces an active node by an inactive one.
// Possible neighbors for the path 1 -> 2 -> 3 -> 4 with 5 inactive (where 1 and
// 4 are first and last nodes of the path) are:
// 1 -> 5 -> 3 -> 4 & 2 inactive
// 1 -> 2 -> 5 -> 4 & 3 inactive

class SwapActiveOperator : public BaseInactiveNodeToPathOperator {
 public:
  SwapActiveOperator(const IntVar* const* vars,
                     const IntVar* const* secondary_vars,
                     int size)
      : BaseInactiveNodeToPathOperator(vars, secondary_vars, size, 1) {}
  virtual ~SwapActiveOperator() {}
  virtual bool MakeNeighbor();
};

bool SwapActiveOperator::MakeNeighbor() {
  const int64 base = BaseNode(0);
  if (IsPathEnd(base)) {
    return false;
  }
  return MakeChainInactive(base, Next(base))
      && MakeActive(GetInactiveNode(), base);
}

// ----- ExtendedSwapActiveOperator -----

// ExtendedSwapActiveOperator makes an inactive node active and an active one
// inactive. It is similar to SwapActiveOperator excepts that it tries to
// insert the inactive node in all possible positions instead of just the
// position of the node made inactive.
// Possible neighbors for the path 1 -> 2 -> 3 -> 4 with 5 inactive (where 1 and
// 4 are first and last nodes of the path) are:
// 1 -> 5 -> 3 -> 4 & 2 inactive
// 1 -> 3 -> 5 -> 4 & 2 inactive
// 1 -> 5 -> 2 -> 4 & 3 inactive
// 1 -> 2 -> 5 -> 4 & 3 inactive

class ExtendedSwapActiveOperator : public BaseInactiveNodeToPathOperator {
 public:
  ExtendedSwapActiveOperator(const IntVar* const* vars,
                             const IntVar* const* secondary_vars,
                             int size)
      : BaseInactiveNodeToPathOperator(vars, secondary_vars, size, 2) {}
  virtual ~ExtendedSwapActiveOperator() {}
  virtual bool MakeNeighbor();
};

bool ExtendedSwapActiveOperator::MakeNeighbor() {
  const int64 base0 = BaseNode(0);
  if (IsPathEnd(base0)) {
    return false;
  }
  const int64 base1 = BaseNode(1);
  if (IsPathEnd(base1)) {
    return false;
  }
  if (Next(base0) == base1) {
    return false;
  }
  return MakeChainInactive(base0, Next(base0))
      && MakeActive(GetInactiveNode(), base1);
}

// ----- TSP-based operators -----

// Sliding TSP operator
// Uses an exact dynamic programming algorithm to solve the TSP corresponding
// to path sub-chains.
// For a subchain 1 -> 2 -> 3 -> 4 -> 5 -> 6, solves the TSP on nodes A, 2, 3,
// 4, 5, where A is a merger of nodes 1 and 6 such that cost(A,i) = cost(1,i)
// and cost(i,A) = cost(i,6).

class TSPOpt : public PathOperator {
 public:
  TSPOpt(const IntVar* const* vars,
         const IntVar* const* secondary_vars,
         int size,
         Solver::IndexEvaluator3* evaluator,
         int chain_length);
  virtual ~TSPOpt() {}
  virtual bool MakeNeighbor();
 private:
  std::vector<std::vector<int64> > cost_;
  HamiltonianPathSolver<int64> hamiltonian_path_solver_;
  scoped_ptr<Solver::IndexEvaluator3> evaluator_;
  const int chain_length_;
};

TSPOpt::TSPOpt(const IntVar* const* vars,
               const IntVar* const* secondary_vars,
               int size,
               Solver::IndexEvaluator3* evaluator,
               int chain_length)
    : PathOperator(vars, secondary_vars, size, 1),
      hamiltonian_path_solver_(cost_),
      evaluator_(evaluator),
      chain_length_(chain_length) {}

bool TSPOpt::MakeNeighbor() {
  std::vector<int64> nodes;
  int64 chain_end = BaseNode(0);
  for (int i = 0; i < chain_length_ + 1; ++i) {
    nodes.push_back(chain_end);
    if (IsPathEnd(chain_end)) {
      break;
    }
    chain_end = Next(chain_end);
  }
  if (nodes.size() <= 3) {
    return false;
  }
  int64 chain_path = Path(BaseNode(0));
  const int size = nodes.size() - 1;
  cost_.resize(size);
  for (int i = 0; i < size; ++i) {
    cost_[i].resize(size);
    cost_[i][0] = evaluator_->Run(nodes[i], nodes[size], chain_path);
    for (int j = 1; j < size; ++j) {
      cost_[i][j] = evaluator_->Run(nodes[i], nodes[j], chain_path);
    }
  }
  hamiltonian_path_solver_.ChangeCostMatrix(cost_);
  std::vector<PathNodeIndex> path;
  hamiltonian_path_solver_.TravelingSalesmanPath(&path);
  CHECK_EQ(size + 1, path.size());
  for (int i = 0; i < size - 1; ++i) {
    SetNext(nodes[path[i]], nodes[path[i + 1]], chain_path);
  }
  SetNext(nodes[path[size - 1]], nodes[size], chain_path);
  return true;
}

// TSP-base lns
// Randomly merge consecutive nodes until n "meta"-nodes remain and solve the
// corresponding TSP. This can be seen as a large neighborhood search operator
// although decisions are taken with the operator.
// This is an "unlimited" neighborhood which must be stopped by search limits.
// To force diversification, the operator iteratively forces each node to serve
// as base of a meta-node.

class TSPLns : public PathOperator {
 public:
  TSPLns(const IntVar* const* vars,
         const IntVar* const* secondary_vars,
         int size,
         Solver::IndexEvaluator3* evaluator,
         int tsp_size);
  virtual ~TSPLns() {}
  virtual bool MakeNeighbor();

 protected:
  virtual bool MakeOneNeighbor();

 private:
  std::vector<std::vector<int64> > cost_;
  HamiltonianPathSolver<int64> hamiltonian_path_solver_;
  scoped_ptr<Solver::IndexEvaluator3> evaluator_;
  const int tsp_size_;
  ACMRandom rand_;
};

TSPLns::TSPLns(const IntVar* const* vars,
               const IntVar* const* secondary_vars,
               int size,
               Solver::IndexEvaluator3* evaluator,
               int tsp_size)
    : PathOperator(vars, secondary_vars, size, 1),
      hamiltonian_path_solver_(cost_),
      evaluator_(evaluator),
      tsp_size_(tsp_size),
      rand_(ACMRandom::HostnamePidTimeSeed()) {
  cost_.resize(tsp_size_);
  for (int i = 0; i < tsp_size_; ++i) {
    cost_[i].resize(tsp_size_);
  }
}

bool TSPLns::MakeOneNeighbor() {
  while (true) {
    if (PathOperator::MakeOneNeighbor()) {
      return true;
    }
  }
  return false;
}

bool TSPLns::MakeNeighbor() {
  const int64 base_node = BaseNode(0);
  if (IsPathEnd(base_node)) {
    return false;
  }
  std::vector<int64> nodes;
  for (int64 node = StartNode(0); !IsPathEnd(node); node = Next(node)) {
    nodes.push_back(node);
  }
  if (nodes.size() <= tsp_size_) {
    return false;
  }
  // Randomly select break nodes (final nodes of a meta-node, after which
  // an arc is relaxed.
  hash_set<int64> breaks_set;
  // Always add base node to break nodes (diversification)
  breaks_set.insert(base_node);
  while (breaks_set.size() < tsp_size_) {
    const int64 one_break = nodes[rand_.Uniform(nodes.size())];
    if (!ContainsKey(breaks_set, one_break)) {
      breaks_set.insert(one_break);
    }
  }
  CHECK_EQ(breaks_set.size(), tsp_size_);
  // Setup break node indexing and internal meta-node cost (cost of partial
  // route starting at first node of the meta-node and ending at its last node);
  // this cost has to be added to the TSP matrix cost in order to respect the
  // triangle inequality.
  std::vector<int> breaks;
  std::vector<int64> meta_node_costs;
  int64 cost = 0;
  int64 node = StartNode(0);
  int64 node_path = Path(node);
  while (!IsPathEnd(node)) {
    int64 next = Next(node);
    if (ContainsKey(breaks_set, node)) {
      breaks.push_back(node);
      meta_node_costs.push_back(cost);
      cost = 0;
    } else {
      cost += evaluator_->Run(node, next, node_path);
    }
    node = next;
  }
  meta_node_costs[0] += cost;
  CHECK_EQ(breaks.size(), tsp_size_);
  // Setup TSP cost matrix
  CHECK_EQ(meta_node_costs.size(), tsp_size_);
  for (int i = 0; i < tsp_size_; ++i) {
    cost_[i][0] = meta_node_costs[i]
        + evaluator_->Run(breaks[i], Next(breaks[tsp_size_ - 1]), node_path);
    for (int j = 1; j < tsp_size_; ++j) {
      cost_[i][j] = meta_node_costs[i]
          + evaluator_->Run(breaks[i], Next(breaks[j - 1]), node_path);
    }
    cost_[i][i] = 0;
  }
  // Solve TSP and inject solution in delta (only if it leads to a new solution)
  hamiltonian_path_solver_.ChangeCostMatrix(cost_);
  std::vector<PathNodeIndex> path;
  hamiltonian_path_solver_.TravelingSalesmanPath(&path);
  bool nochange = true;
  for (int i = 0; i < path.size() - 1; ++i) {
    if (path[i] != i) {
      nochange = false;
      break;
    }
  }
  if (nochange) {
    return false;
  }
  CHECK_EQ(0, path[path.size() - 1]);
  for (int i = 0; i < tsp_size_ - 1; ++i) {
    SetNext(breaks[path[i]], OldNext(breaks[path[i + 1] - 1]), node_path);
  }
  SetNext(breaks[path[tsp_size_ - 1]],
          OldNext(breaks[tsp_size_ - 1]),
          node_path);
  return true;
}

// ----- Lin Kernighan -----

// For each variable in vars, stores the 'size' pairs(i,j) with the smallest
// value according to evaluator, where i is the index of the variable in vars
// and j is in the domain of the variable.
// Note that the resulting pairs are sorted.
// Works in O(size) per variable on average (same approach as qsort)

class NearestNeighbors {
 public:
  NearestNeighbors(Solver::IndexEvaluator3* evaluator,
                   const PathOperator& path_operator,
                   int size);
  void Initialize();
  const std::vector<int>& Neighbors(int index) const;
 private:
  void ComputeNearest(int row);
  static void Pivot(int start,
                    int end,
                    int* neighbors,
                    int64* row,
                    int* index);
  static void Swap(int i, int j, int* neighbors, int64* row);

  std::vector<std::vector<int> > neighbors_;
  Solver::IndexEvaluator3* evaluator_;
  const PathOperator& path_operator_;
  const int size_;
  bool initialized_;

  DISALLOW_COPY_AND_ASSIGN(NearestNeighbors);
};

NearestNeighbors::NearestNeighbors(Solver::IndexEvaluator3* evaluator,
                                   const PathOperator& path_operator,
                                   int size)
    : evaluator_(evaluator),
      path_operator_(path_operator),
      size_(size),
      initialized_(false) {}

void NearestNeighbors::Initialize() {
  // TODO(user): recompute if node changes path ?
  if (!initialized_) {
    initialized_ = true;
    for (int i = 0; i < path_operator_.number_of_nexts(); ++i) {
      neighbors_.push_back(std::vector<int>());
      ComputeNearest(i);
    }
  }
}

const std::vector<int>& NearestNeighbors::Neighbors(int index) const {
  return neighbors_[index];
}

void NearestNeighbors::ComputeNearest(int row) {
  // Find size_ nearest neighbors for row of index 'row'.
  const int path = path_operator_.Path(row);
  const IntVar* var = path_operator_.Var(row);
  const int64 var_min = var->Min();
  const int var_size = var->Max() - var_min + 1;
  scoped_array<int> neighbors(new int[var_size]);
  scoped_array<int64> row_data(new int64[var_size]);
  for (int i = 0; i < var_size; ++i) {
    const int index = i + var_min;
    neighbors[i] = index;
    row_data[i] = evaluator_->Run(row, index, path);
  }

  if (var_size > size_) {
    int start = 0;
    int end = var_size;
    int size = size_;
    while (size > 0) {
      int index = (end - start) / 2;
      Pivot(start, end, neighbors.get(), row_data.get(), &index);
      if (index - start >= size) {
        end = index;
      } else {
        start = index + 1;
        size -= start;
      }
    }
  }

  // Setup global neighbor matrix for row row_index
  for (int i = 0; i < std::min(size_, var_size); ++i) {
    neighbors_[row].push_back(neighbors[i]);
    std::sort(neighbors_[row].begin(), neighbors_[row].end());
  }
}

void NearestNeighbors::Pivot(int start,
                             int end,
                             int* neighbors,
                             int64* row,
                             int* index) {
  Swap(start, *index, neighbors, row);
  int j = start;
  for (int i = start + 1; i < end; ++i) {
    if (row[i] < row[j]) {
      Swap(j, i, neighbors, row);
      ++j;
      Swap(i, j, neighbors, row);
    }
  }
  *index = j;
}

void NearestNeighbors::Swap(int i, int j, int* neighbors, int64* row) {
  const int64 row_i = row[i];
  const int neighbor_i = neighbors[i];
  row[i] = row[j];
  neighbors[i] = neighbors[j];
  row[j] = row_i;
  neighbors[j] = neighbor_i;
}

class LinKernighan : public PathOperator {
 public:
  LinKernighan(const IntVar* const* vars,
               const IntVar* const* secondary_vars,
               int size,
               Solver::IndexEvaluator3* evaluator,
               bool owner,  // Owner of callback
               bool topt);
  virtual ~LinKernighan();
  virtual bool MakeNeighbor();
 private:
  virtual void OnNodeInitialization();

  static const int kNeighbors;

  bool InFromOut(int64 in_i, int64 in_j, int64* out, int64* gain);

  Solver::IndexEvaluator3* const evaluator_;
  bool owner_;
  NearestNeighbors neighbors_;
  hash_set<int64> marked_;
  const bool topt_;
};

// While the accumulated local gain is positive, perform a 2opt or a 3opt move
// followed by a series of 2opt moves. Return a neighbor for which the global
// gain is positive.

LinKernighan::LinKernighan(const IntVar* const* vars,
                           const IntVar* const* secondary_vars,
                           int size,
                           Solver::IndexEvaluator3* evaluator,
                           bool owner,
                           bool topt)
    : PathOperator(vars, secondary_vars, size, 1),
      evaluator_(evaluator),
      owner_(owner),
      neighbors_(evaluator, *this, kNeighbors),
      topt_(topt) {}

LinKernighan::~LinKernighan() {
  if (owner_) {
    delete evaluator_;
  }
}

void LinKernighan::OnNodeInitialization() {
  neighbors_.Initialize();
}

bool LinKernighan::MakeNeighbor() {
  marked_.clear();
  int64 node = BaseNode(0);
  if (IsPathEnd(node)) return false;
  int64 path = Path(node);
  int64 base = node;
  int64 next = Next(node);
  if (IsPathEnd(next)) return false;
  int64 out = -1;
  int64 gain = 0;
  marked_.insert(node);
  if (topt_) {  // Try a 3opt first
    if (InFromOut(node, next, &out, &gain)) {
      marked_.insert(next);
      marked_.insert(out);
      const int64 node1 = out;
      if (IsPathEnd(node1)) return false;
      const int64 next1 = Next(node1);
      if (IsPathEnd(next1)) return false;
      if (InFromOut(node1, next1, &out, &gain)) {
        marked_.insert(next1);
        marked_.insert(out);
        if (MoveChain(out, node1, node)) {
          const int64 next_out = Next(out);
          int64 in_cost = evaluator_->Run(node, next_out, path);
          int64 out_cost = evaluator_->Run(out, next_out, path);
          if (gain - in_cost + out_cost > 0)
            return true;
          node = out;
          if (IsPathEnd(node)) {
            return false;
          }
          next = next_out;
          if (IsPathEnd(next)) {
            return false;
          }
        } else {
          return false;
        }
      } else {
        return false;
      }
    } else {
      return false;
    }
  }
  // Try 2opts
  while (InFromOut(node, next, &out, &gain)) {
    marked_.insert(next);
    marked_.insert(out);
    int64 chain_last;
    if (!ReverseChain(node, out, &chain_last)) {
      return false;
    }
    int64 in_cost = evaluator_->Run(base, chain_last, path);
    int64 out_cost = evaluator_->Run(chain_last, out, path);
    if (gain - in_cost + out_cost > 0) {
      return true;
    }
    node = chain_last;
    if (IsPathEnd(node)) {
      return false;
    }
    next = out;
    if (IsPathEnd(next)) {
      return false;
    }
  }
  return false;
}

const int LinKernighan::kNeighbors = 5 + 1;

bool LinKernighan::InFromOut(int64 in_i, int64 in_j, int64* out, int64* gain) {
  const std::vector<int>& nexts = neighbors_.Neighbors(in_j);
  int64 best_gain = kint64min;
  int64 path = Path(in_i);
  int64 out_cost = evaluator_->Run(in_i, in_j, path);
  const int64 current_gain = *gain + out_cost;
  for (int k = 0; k < nexts.size(); ++k) {
    const int64 next = nexts[k];
    if (next != in_j) {
      int64 in_cost = evaluator_->Run(in_j, next, path);
      int64 new_gain = current_gain - in_cost;
      if (new_gain > 0
          && next != Next(in_j)
          && marked_.count(in_j) == 0
          && marked_.count(next) == 0) {
        if (best_gain < new_gain) {
          *out = next;
          best_gain = new_gain;
        }
      }
    }
  }
  *gain = best_gain;
  return (best_gain > kint64min);
}

// ----- Path-based Large Neighborhood Search -----

// Breaks number_of_chunks chains of chunk_size arcs.

class PathLNS : public PathOperator {
 public:
  PathLNS(const IntVar* const* vars,
          const IntVar* const* secondary_vars,
          int size,
          int number_of_chunks,
          int chunk_size,
          bool unactive_fragments)
      : PathOperator(vars, secondary_vars, size, number_of_chunks),
        number_of_chunks_(number_of_chunks),
        chunk_size_(chunk_size),
        unactive_fragments_(unactive_fragments) {
    CHECK_GT(chunk_size_, 0);
  }
  virtual ~PathLNS() {}
  virtual bool MakeNeighbor();
 private:
  void DeactivateChain(int64 node0);
  void DeactivateUnactives();

  const int number_of_chunks_;
  const int chunk_size_;
  const bool unactive_fragments_;
};

bool PathLNS::MakeNeighbor() {
  for (int i = 0; i < number_of_chunks_; ++i) {
    DeactivateChain(BaseNode(i));
  }
  DeactivateUnactives();
  return true;
}

void PathLNS::DeactivateChain(int64 node) {
  for (int i = 0, current = node;
       i < chunk_size_ && !IsPathEnd(current);
       ++i, current = Next(current)) {
    Deactivate(current);
    if (!ignore_path_vars_) {
      Deactivate(number_of_nexts_ + current);
    }
  }
}

void PathLNS::DeactivateUnactives() {
  if (unactive_fragments_) {
    for (int i = 0; i < Size(); ++i) {
      if (IsInactive(i)) {
        Deactivate(i);
        if (!ignore_path_vars_) {
          Deactivate(number_of_nexts_ + i);
        }
      }
    }
  }
}

// ----- Limit the number of neighborhoods explored -----

class NeighborhoodLimit : public LocalSearchOperator {
 public:
  NeighborhoodLimit(LocalSearchOperator* const op, int64 limit)
      : operator_(op), limit_(limit), next_neighborhood_calls_(0) {
    CHECK_NOTNULL(op);
    CHECK_GT(limit, 0);
  }

  virtual void Start(const Assignment* assignment) {
    next_neighborhood_calls_ = 0;
    operator_->Start(assignment);
  }

  virtual bool MakeNextNeighbor(Assignment* delta, Assignment* deltadelta) {
    if (next_neighborhood_calls_ >= limit_) {
      return false;
    }
    ++next_neighborhood_calls_;
    return operator_->MakeNextNeighbor(delta, deltadelta);
  }

 private:
  LocalSearchOperator* const operator_;
  const int64 limit_;
  int64 next_neighborhood_calls_;
};
}  // namespace

LocalSearchOperator* Solver::MakeNeighborhoodLimit(
    LocalSearchOperator* const op, int64 limit) {
  return RevAlloc(new NeighborhoodLimit(op, limit));
}

// ----- Concatenation of operators -----

namespace {
class CompoundOperator : public LocalSearchOperator {
 public:
  CompoundOperator(
      const std::vector<LocalSearchOperator*>& operators,
      ResultCallback2<int64, int, int>* const evaluator);
  virtual ~CompoundOperator() {}
  virtual void Start(const Assignment* assignment);
  virtual bool MakeNextNeighbor(Assignment* delta, Assignment* deltadelta);

 private:
  class OperatorComparator {
   public:
    OperatorComparator(ResultCallback2<int64, int, int>* const evaluator,
                       int active_operator)
        : evaluator_(evaluator), active_operator_(active_operator) {
      evaluator_->CheckIsRepeatable();
    }
    bool operator() (int lhs, int rhs) const {
      const int64 lhs_value = Evaluate(lhs);
      const int64 rhs_value = Evaluate(rhs);
      return lhs_value < rhs_value || (lhs_value == rhs_value && lhs < rhs);
    }

   private:
    int64 Evaluate(int operator_index) const {
      return evaluator_->Run(active_operator_, operator_index);
    }

    ResultCallback2<int64, int, int>* const evaluator_;
    const int active_operator_;
  };

  int64 index_;
  int64 size_;
  scoped_array<LocalSearchOperator*> operators_;
  scoped_array<int> operator_indices_;
  scoped_ptr<ResultCallback2<int64, int, int> > evaluator_;
};

CompoundOperator::CompoundOperator(
    const std::vector<LocalSearchOperator*>& operators,
    ResultCallback2<int64, int, int>* const evaluator)
      : index_(0),
        size_(0),
        operators_(NULL),
        operator_indices_(NULL),
        evaluator_(evaluator) {
  for (int i = 0; i < operators.size(); ++i) {
    if (operators[i] != NULL) {
      ++size_;
    }
  }
  operators_.reset(new LocalSearchOperator*[size_]);
  operator_indices_.reset(new int[size_]);
  int index = 0;
  for (int i = 0; i < operators.size(); ++i) {
    if (operators[i] != NULL) {
      operators_[index] = operators[i];
      operator_indices_[index] = index;
      ++index;
    }
  }
}

void CompoundOperator::Start(const Assignment* assignment) {
  if (size_ > 0) {
    for (int i = 0; i < size_; ++i) {
      operators_[i]->Start(assignment);
    }
    OperatorComparator comparator(evaluator_.get(), operator_indices_[index_]);
    std::sort(operator_indices_.get(),
              operator_indices_.get() + size_,
              comparator);
    index_ = 0;
  }
}

bool CompoundOperator::MakeNextNeighbor(Assignment* delta,
                                        Assignment* deltadelta) {
  if (size_ > 0) {
    do {
      // TODO(user): keep copy of delta in case MakeNextNeighbor
      // pollutes delta on a fail.
      if (operators_[operator_indices_[index_]]->MakeNextNeighbor(delta,
                                                                  deltadelta)) {
        return true;
      }
      ++index_;
      if (index_ == size_) {
        index_ = 0;
      }
    } while (index_ != 0);
  }
  return false;
}

int64 CompoundOperatorNoRestart(int size,
                                int active_index,
                                int operator_index) {
  if (operator_index < active_index) {
    return size + operator_index - active_index;
  } else {
    return operator_index - active_index;
  }
}

int64 CompoundOperatorRestart(int active_index, int operator_index) {
  return 0;
}
}  // namespace

LocalSearchOperator* Solver::ConcatenateOperators(
    const std::vector<LocalSearchOperator*>& ops) {
  return ConcatenateOperators(ops, false);
}

LocalSearchOperator* Solver::ConcatenateOperators(
    const std::vector<LocalSearchOperator*>& ops,
    bool restart) {
  if (restart) {
    return ConcatenateOperators(
        ops,
        NewPermanentCallback(&CompoundOperatorRestart));
  } else {
    return ConcatenateOperators(
        ops,
        NewPermanentCallback(&CompoundOperatorNoRestart,
                             static_cast<int>(ops.size())));
  }
}

LocalSearchOperator* Solver::ConcatenateOperators(
    const std::vector<LocalSearchOperator*>& ops,
    ResultCallback2<int64, int, int>* const evaluator) {
  return RevAlloc(new CompoundOperator(ops, evaluator));
}

namespace {
class RandomCompoundOperator : public LocalSearchOperator {
 public:
  explicit RandomCompoundOperator(
      const std::vector<LocalSearchOperator*>& operators);
  RandomCompoundOperator(
      const std::vector<LocalSearchOperator*>& operators, int32 seed);
  virtual ~RandomCompoundOperator() {}
  virtual void Start(const Assignment* assignment);
  virtual bool MakeNextNeighbor(Assignment* delta, Assignment* deltadelta);
 private:
  const int size_;
  ACMRandom rand_;
  scoped_array<LocalSearchOperator*> operators_;
};

void RandomCompoundOperator::Start(const Assignment* assignment) {
  for (int i = 0; i < size_; ++i) {
    operators_[i]->Start(assignment);
  }
}

RandomCompoundOperator::RandomCompoundOperator(
    const std::vector<LocalSearchOperator*>& operators)
    : size_(operators.size()),
      rand_(ACMRandom::HostnamePidTimeSeed()),
      operators_(new LocalSearchOperator*[size_]) {
  for (int i = 0; i < size_; ++i) {
    operators_[i] = operators[i];
  }
}

RandomCompoundOperator::RandomCompoundOperator(
    const std::vector<LocalSearchOperator*>& operators, int32 seed)
    : size_(operators.size()),
      rand_(seed),
      operators_(new LocalSearchOperator*[size_]) {
  for (int i = 0; i < size_; ++i) {
    operators_[i] = operators[i];
  }
}


bool RandomCompoundOperator::MakeNextNeighbor(Assignment* delta,
                                              Assignment* deltadelta) {
  std::vector<int> indices(size_);
  for (int i = 0; i < size_; ++i) {
    indices[i] = i;
  }
  random_shuffle(indices.begin(), indices.end(), rand_);
  for (int i = 0; i < size_; ++i) {
    if (operators_[indices[i]]->MakeNextNeighbor(delta, deltadelta)) {
      return true;
    }
  }
  return false;
}
}  // namespace

LocalSearchOperator* Solver::RandomConcatenateOperators(
    const std::vector<LocalSearchOperator*>& ops) {
  return RevAlloc(new RandomCompoundOperator(ops));
}

LocalSearchOperator* Solver::RandomConcatenateOperators(
    const std::vector<LocalSearchOperator*>& ops, int32 seed) {
  return RevAlloc(new RandomCompoundOperator(ops, seed));
}

// ----- Operator factory -----

LocalSearchOperator* Solver::MakeOperator(const std::vector<IntVar*>& vars,
                                          Solver::LocalSearchOperators op) {
  return MakeOperator(vars, std::vector<IntVar*>(), op);
}

LocalSearchOperator* Solver::MakeOperator(const std::vector<IntVar*>& vars,
                                          const std::vector<IntVar*>& secondary_vars,
                                          Solver::LocalSearchOperators op) {
  const int size = vars.size();
  LocalSearchOperator* result = NULL;
  switch (op) {
    case Solver::TWOOPT: {
      result = RevAlloc(new TwoOpt(vars.data(), secondary_vars.data(), size));
      break;
    }
    case Solver::OROPT: {
      std::vector<LocalSearchOperator*> operators;
      for (int i = 1; i < 4; ++i)
        operators.push_back(RevAlloc(new Relocate(vars.data(),
                                                  secondary_vars.data(),
                                                  size,
                                                  i,
                                                  true)));
      result = ConcatenateOperators(operators);
      break;
    }
    case Solver::RELOCATE: {
      result = RevAlloc(new Relocate(vars.data(), secondary_vars.data(), size));
      break;
    }
    case Solver::EXCHANGE: {
      result = RevAlloc(new Exchange(vars.data(), secondary_vars.data(), size));
      break;
    }
    case Solver::CROSS: {
      result = RevAlloc(new Cross(vars.data(), secondary_vars.data(), size));
      break;
    }
    case Solver::MAKEACTIVE: {
      result = RevAlloc(
          new MakeActiveOperator(vars.data(), secondary_vars.data(), size));
      break;
    }
    case Solver::MAKEINACTIVE: {
      result = RevAlloc(
          new MakeInactiveOperator(vars.data(), secondary_vars.data(), size));
      break;
    }
    case Solver::SWAPACTIVE: {
      result = RevAlloc(
          new SwapActiveOperator(vars.data(), secondary_vars.data(), size));
      break;
    }
    case Solver::EXTENDEDSWAPACTIVE: {
      result = RevAlloc(
          new ExtendedSwapActiveOperator(
              vars.data(),
              secondary_vars.data(),
              size));
      break;
    }
    case Solver::PATHLNS: {
      result = RevAlloc(
          new PathLNS(vars.data(), secondary_vars.data(), size, 2, 3, false));
      break;
    }
    case Solver::UNACTIVELNS: {
      result = RevAlloc(
          new PathLNS(vars.data(), secondary_vars.data(), size, 1, 6, true));
      break;
    }
    case Solver::INCREMENT: {
      if (secondary_vars.size() == 0) {
        result = RevAlloc(new IncrementValue(vars.data(), size));
      } else {
        LOG(FATAL) << "Operator " << op
                   << " does not support secondary variables";
      }
      break;
    }
    case Solver::DECREMENT: {
      if (secondary_vars.size() == 0) {
        result = RevAlloc(new DecrementValue(vars.data(), size));
      } else {
        LOG(FATAL) << "Operator " << op
                   << " does not support secondary variables";
      }
      break;
    }
    case Solver::SIMPLELNS: {
      if (secondary_vars.size() == 0) {
        result = RevAlloc(new SimpleLNS(vars.data(), size, 1));
      } else {
        LOG(FATAL) << "Operator " << op
                   << " does not support secondary variables";
      }
      break;
    }
    default:
      LOG(FATAL) << "Unknown operator " << op;
  }
  return result;
}

LocalSearchOperator* Solver::MakeOperator(
    const std::vector<IntVar*>& vars,
    Solver::IndexEvaluator3* const evaluator,
    Solver::EvaluatorLocalSearchOperators op) {
  return MakeOperator(vars, std::vector<IntVar*>(), evaluator, op);
}

LocalSearchOperator* Solver::MakeOperator(
    const std::vector<IntVar*>& vars,
    const std::vector<IntVar*>& secondary_vars,
    Solver::IndexEvaluator3* const evaluator,
    Solver::EvaluatorLocalSearchOperators op) {
  const int size = vars.size();
  LocalSearchOperator* result = NULL;
  switch (op) {
    case Solver::LK: {
      std::vector<LocalSearchOperator*> operators;
      operators.push_back(
          RevAlloc(new LinKernighan(vars.data(),
                                    secondary_vars.data(),
                                    size,
                                    evaluator,
                                    true, false)));
      operators.push_back(
          RevAlloc(new LinKernighan(vars.data(),
                                    secondary_vars.data(),
                                    size,
                                    evaluator,
                                    false, true)));
      result = ConcatenateOperators(operators);
      break;
    }
    case Solver::TSPOPT: {
      result = RevAlloc(new TSPOpt(vars.data(),
                                   secondary_vars.data(),
                                   size,
                                   evaluator,
                                   FLAGS_cp_local_search_tsp_opt_size));
      break;
    }
    case Solver::TSPLNS: {
      result = RevAlloc(new TSPLns(vars.data(),
                                   secondary_vars.data(),
                                   size,
                                   evaluator,
                                   FLAGS_cp_local_search_tsp_lns_size));
      break;
    }
    default:
      LOG(FATAL) << "Unknown operator " << op;
  }
  return result;
}

namespace {
// Abstract class for Local Search Operation. It is passed to Local Search
// Filter.

class LSOperation {
 public:
  LSOperation() {}
  virtual ~LSOperation() {}
  virtual void Init() = 0;
  virtual void Update(int64 update) = 0;
  virtual void Remove(int64 remove) = 0;
  virtual int64 value() = 0;
  virtual void set_value(int64 new_value) = 0;
};

class SumOperation : public LSOperation {
 public:
  virtual void Init() { value_ = 0; }
  virtual void Update(int64 update) {
    value_ += update;
  }
  virtual void Remove(int64 remove) {
    value_ -= remove;
  }
  virtual int64 value() { return value_; }
  virtual void set_value(int64 new_value) { value_ = new_value; }

 private:
  int64 value_;
};

class ProductOperation : public LSOperation {
 public:
  virtual void Init() { value_ = 1; }
  virtual void Update(int64 update) {
    value_ *= update;
  }
  virtual void Remove(int64 remove) {
    if (remove != 0) {
      value_ /= remove;
    }
  }
  virtual int64 value() { return value_; }
  virtual void set_value(int64 new_value) { value_ = new_value; }

 private:
  int64 value_;
};

class MaxMinOperation : public LSOperation {
 public:
  explicit MaxMinOperation(bool max) : max_(max) {}
  virtual void Init() {
    values_set_.clear();
  }
  virtual void Update(int64 update) {
    values_set_.insert(update);
  }
  virtual void Remove(int64 remove) {
    values_set_.erase(remove);
  }
  virtual int64 value() {
    if (!values_set_.empty()) {
      if (max_) {
        return *values_set_.rbegin();
      } else {
        return *values_set_.begin();
      }
    } else {
      return 0;
    }
  }
  virtual void set_value(int64 new_value) {}

 private:
  std::set<int64> values_set_;
  bool max_;
};

// ----- Variable domain filter -----
// Rejects assignments to values outside the domain of variables

class VariableDomainFilter : public LocalSearchFilter {
 public:
  VariableDomainFilter() {}
  virtual ~VariableDomainFilter() {}
  virtual bool Accept(const Assignment* delta, const Assignment* deltadelta);
  virtual void Synchronize(const Assignment* assignment) {}
};

bool VariableDomainFilter::Accept(const Assignment* delta,
                                  const Assignment* deltadelta) {
  const Assignment::IntContainer& container = delta->IntVarContainer();
  const int size = container.Size();
  for (int i = 0; i < size; ++i) {
    const IntVarElement& element = container.Element(i);
    if (element.Activated() && !element.Var()->Contains(element.Value())) {
      return false;
    }
  }
  return true;
}
}  // namespace

LocalSearchFilter* Solver::MakeVariableDomainFilter() {
  return RevAlloc(new VariableDomainFilter());
}

// ----- IntVarLocalSearchFilter -----

IntVarLocalSearchFilter::IntVarLocalSearchFilter(const IntVar* const* vars,
                                                 int size)
    : vars_(NULL), values_(NULL), size_(0) {
  AddVars(vars, size);
  CHECK_GE(size_, 0);
}

void IntVarLocalSearchFilter::AddVars(const IntVar* const* vars, int size) {
  if (size > 0) {
    for (int i = 0; i < size; ++i) {
      var_to_index_[vars[i]] = i + size_;
    }
    const int new_size = size_ + size;
    IntVar** new_vars = new IntVar*[new_size];
    if (size_ > 0) {
      memcpy(new_vars, vars_.get(), size_ * sizeof(*new_vars));
    }
    memcpy(new_vars + size_, vars, size * sizeof(*vars));
    vars_.reset(new_vars);
    values_.reset(new int64[new_size]);
    memset(values_.get(), 0, sizeof(int64) * new_size);  // NOLINT
    size_ = new_size;
  }
}

IntVarLocalSearchFilter::~IntVarLocalSearchFilter() {}

void IntVarLocalSearchFilter::Synchronize(const Assignment* assignment) {
  const Assignment::IntContainer& container = assignment->IntVarContainer();
  const int size = container.Size();
  for (int i = 0; i < size; ++i) {
    const IntVarElement& element = container.Element(i);
    const IntVar* var = element.Var();
    if (i < size_ && vars_[i] == var) {
        values_[i] = element.Value();
    } else {
      const int64 kUnallocated = -1;
      int64 index = kUnallocated;
      if (FindIndex(var, &index)) {
        values_[index] = element.Value();
      }
    }
  }
  OnSynchronize();
}

// ----- Objective filter ------
// Assignment is accepted if it improves the best objective value found so far.
// 'Values' callback takes an index of a variable and its value and returns the
// contribution into the objective value. The type of objective function
// is determined by LocalSearchOperation enum. Conditions on neighbor
// acceptance are presented in LocalSearchFilterBound enum. Objective function
// can be represented by any variable.

namespace {
class ObjectiveFilter : public IntVarLocalSearchFilter {
 public:
  ObjectiveFilter(const IntVar* const* vars,
                  int size,
                  const IntVar* const objective,
                  Solver::LocalSearchFilterBound filter_enum,
                  LSOperation* op);
  virtual ~ObjectiveFilter();
  virtual bool Accept(const Assignment* delta, const Assignment* deltadelta);
  virtual int64 SynchronizedElementValue(int64 index) = 0;
  virtual bool EvaluateElementValue(const Assignment::IntContainer& container,
                                    int index,
                                    int* container_index,
                                    int64* obj_value) = 0;
  virtual bool IsIncremental() const { return true; }

 protected:
  const int primary_vars_size_;
  int64* const cache_;
  int64* const delta_cache_;
  const IntVar* const objective_;
  Solver::LocalSearchFilterBound filter_enum_;
  scoped_ptr<LSOperation> op_;
  int64 old_value_;
  int64 old_delta_value_;
  bool incremental_;

 private:
  virtual void OnSynchronize();
  int64 Evaluate(const Assignment* delta,
                 int64 current_value,
                 const int64* const out_values,
                 bool cache_delta_values);
};

ObjectiveFilter::ObjectiveFilter(const IntVar* const* vars,
                                 int var_size,
                                 const IntVar* const objective,
                                 Solver::LocalSearchFilterBound filter_enum,
                                 LSOperation* op)
    : IntVarLocalSearchFilter(vars, var_size),
      primary_vars_size_(var_size),
      cache_(new int64[var_size]),
      delta_cache_(new int64[var_size]),
      objective_(objective),
      filter_enum_(filter_enum),
      op_(op),
      old_value_(0),
      old_delta_value_(0),
      incremental_(false) {
  CHECK(op_ != NULL);
  for (int i = 0; i < Size(); ++i) {
    cache_[i] = 0;
    delta_cache_[i] = 0;
  }
  op_->Init();
  old_value_ = op_->value();
}

ObjectiveFilter::~ObjectiveFilter() {
  delete [] cache_;
  delete [] delta_cache_;
}

bool ObjectiveFilter::Accept(const Assignment* delta,
                             const Assignment* deltadelta) {
  if (delta == NULL) {
    return false;
  }
  int64 value = 0;
  if (!deltadelta->Empty()) {
    if (!incremental_) {
      value = Evaluate(delta, old_value_, cache_, true);
    } else {
      value = Evaluate(deltadelta, old_delta_value_, delta_cache_, true);
    }
    incremental_ = true;
  } else {
    if (incremental_) {
      for (int i = 0; i < primary_vars_size_; ++i) {
        delta_cache_[i] = cache_[i];
      }
      old_delta_value_ = old_value_;
    }
    incremental_ = false;
    value = Evaluate(delta, old_value_, cache_, false);
  }
  old_delta_value_ = value;
  int64 var_min = objective_->Min();
  int64 var_max = objective_->Max();
  if (delta->Objective() == objective_) {
    var_min = std::max(var_min, delta->ObjectiveMin());
    var_max = std::min(var_max, delta->ObjectiveMax());
  }
  switch (filter_enum_) {
    case Solver::LE: {
      return value <= var_max;
    }
    case Solver::GE: {
      return value >= var_min;
    }
    case Solver::EQ: {
      return value <= var_max && value >= var_min;
    }
    default: {
      LOG(ERROR) << "Unknown local search filter enum value";
      return false;
    }
  }
}

void ObjectiveFilter::OnSynchronize() {
  op_->Init();
  for (int i = 0; i < primary_vars_size_; ++i) {
    const int64 obj_value = SynchronizedElementValue(i);
    cache_[i] = obj_value;
    delta_cache_[i] = obj_value;
    op_->Update(obj_value);
  }
  old_value_ = op_->value();
  old_delta_value_ = old_value_;
  incremental_ = false;
}

int64 ObjectiveFilter::Evaluate(const Assignment* delta,
                                int64 current_value,
                                const int64* const out_values,
                                bool cache_delta_values) {
  if (current_value == kint64max) return current_value;
  op_->set_value(current_value);
  const Assignment::IntContainer& container = delta->IntVarContainer();
  const int size = container.Size();
  for (int i = 0; i < size; ++i) {
    const IntVarElement& new_element = container.Element(i);
    const IntVar* var = new_element.Var();
    int64 index = -1;
    if (FindIndex(var, &index) && index < primary_vars_size_) {
      op_->Remove(out_values[index]);
      int64 obj_value = 0LL;
      if (EvaluateElementValue(container, index, &i, &obj_value)) {
        op_->Update(obj_value);
        if (cache_delta_values) {
          delta_cache_[index] = obj_value;
        }
      }
    }
  }
  return op_->value();
}

class BinaryObjectiveFilter : public ObjectiveFilter {
 public:
  BinaryObjectiveFilter(const IntVar* const* vars,
                        int size,
                        Solver::IndexEvaluator2* values,
                        const IntVar* const objective,
                        Solver::LocalSearchFilterBound filter_enum,
                        LSOperation* op);
  virtual ~BinaryObjectiveFilter() {}
  virtual int64 SynchronizedElementValue(int64 index);
  virtual bool EvaluateElementValue(const Assignment::IntContainer& container,
                                    int index,
                                    int* container_index,
                                    int64* obj_value);
 private:
  scoped_ptr<Solver::IndexEvaluator2> value_evaluator_;
};

BinaryObjectiveFilter::BinaryObjectiveFilter(
    const IntVar* const* vars,
    int size,
    Solver::IndexEvaluator2* value_evaluator,
    const IntVar* const objective,
    Solver::LocalSearchFilterBound filter_enum,
    LSOperation* op)
    : ObjectiveFilter(vars, size, objective, filter_enum, op),
      value_evaluator_(value_evaluator) {
  value_evaluator_->CheckIsRepeatable();
}

int64 BinaryObjectiveFilter::SynchronizedElementValue(int64 index) {
  return value_evaluator_->Run(index, Value(index));
}

bool BinaryObjectiveFilter::EvaluateElementValue(
    const Assignment::IntContainer& container,
    int index,
    int* container_index,
    int64* obj_value) {
  const IntVarElement& element = container.Element(*container_index);
  if (element.Activated()) {
    *obj_value = value_evaluator_->Run(index, element.Value());
    return true;
  } else {
    const IntVar* var = element.Var();
    if (var->Bound()) {
      *obj_value = value_evaluator_->Run(index, var->Min());
      return true;
    }
  }
  return false;
}

class TernaryObjectiveFilter : public ObjectiveFilter {
 public:
  TernaryObjectiveFilter(const IntVar* const* vars,
                         const IntVar* const* secondary_vars,
                         int size,
                         Solver::IndexEvaluator3* value_evaluator,
                         const IntVar* const objective,
                         Solver::LocalSearchFilterBound filter_enum,
                         LSOperation* op);
  virtual ~TernaryObjectiveFilter() {}
  virtual int64 SynchronizedElementValue(int64 index);
  bool EvaluateElementValue(const Assignment::IntContainer& container,
                            int index,
                            int* container_index,
                            int64* obj_value);
 private:
  int secondary_vars_offset_;
  scoped_ptr<Solver::IndexEvaluator3> value_evaluator_;
};

TernaryObjectiveFilter::TernaryObjectiveFilter(
    const IntVar* const* vars,
    const IntVar* const* secondary_vars,
    int var_size,
    Solver::IndexEvaluator3* value_evaluator,
    const IntVar* const objective,
    Solver::LocalSearchFilterBound filter_enum,
    LSOperation* op)
    : ObjectiveFilter(vars, var_size, objective, filter_enum, op),
      secondary_vars_offset_(var_size),
      value_evaluator_(value_evaluator) {
  value_evaluator_->CheckIsRepeatable();
  AddVars(secondary_vars, var_size);
  CHECK_GE(Size(), 0);
}

int64 TernaryObjectiveFilter::SynchronizedElementValue(int64 index) {
  DCHECK_LT(index, secondary_vars_offset_);
  return value_evaluator_->Run(index,
                               Value(index),
                               Value(index + secondary_vars_offset_));
}

bool TernaryObjectiveFilter::EvaluateElementValue(
    const Assignment::IntContainer& container,
    int index,
    int* container_index,
    int64* obj_value) {
  DCHECK_LT(index, secondary_vars_offset_);
  *obj_value = 0LL;
  const IntVarElement& element = container.Element(*container_index);
  const IntVar* secondary_var = Var(index + secondary_vars_offset_);
  if (element.Activated()) {
    const int64 value = element.Value();
    int hint_index = *container_index + 1;
    if (hint_index < container.Size()
        && secondary_var == container.Element(hint_index).Var()) {
      *obj_value =
          value_evaluator_->Run(index,
                                value,
                                container.Element(hint_index).Value());
      *container_index = hint_index;
    } else {
      *obj_value =
          value_evaluator_->Run(index,
                                value,
                                container.Element(secondary_var).Value());
    }
    return true;
  } else {
    const IntVar* var = element.Var();
    if (var->Bound() && secondary_var->Bound()) {
      *obj_value = value_evaluator_->Run(index,
                                         var->Min(),
                                         secondary_var->Min());
      return true;
    }
  }
  return false;
}

// ---- Local search filter factory ----

LSOperation* OperationFromEnum(Solver::LocalSearchOperation op_enum) {
  LSOperation* operation = NULL;
  switch (op_enum) {
    case Solver::SUM: {
      operation = new SumOperation();
      break;
    }
    case Solver::PROD: {
      operation = new ProductOperation();
      break;
    }
    case Solver::MAX: {
      operation = new MaxMinOperation(true);
      break;
    }
    case Solver::MIN: {
      operation = new MaxMinOperation(false);
      break;
    }
    default:
      LOG(FATAL) << "Unknown operator " << op_enum;
  }
  return operation;
}
}  // namespace


LocalSearchFilter* Solver::MakeLocalSearchObjectiveFilter(
    const std::vector<IntVar*>& vars,
    Solver::IndexEvaluator2* const values,
    const IntVar* const objective,
    Solver::LocalSearchFilterBound filter_enum,
    Solver::LocalSearchOperation op_enum) {
  return RevAlloc(new BinaryObjectiveFilter(vars.data(),
                                            vars.size(),
                                            values,
                                            objective,
                                            filter_enum,
                                            OperationFromEnum(op_enum)));
}

LocalSearchFilter* Solver::MakeLocalSearchObjectiveFilter(
    const std::vector<IntVar*>& vars,
    const std::vector<IntVar*>& secondary_vars,
    Solver::IndexEvaluator3* const values,
    const IntVar* const objective,
    Solver::LocalSearchFilterBound filter_enum,
    Solver::LocalSearchOperation op_enum) {
  return  RevAlloc(new TernaryObjectiveFilter(vars.data(),
                                              secondary_vars.data(),
                                              vars.size(),
                                              values,
                                              objective,
                                              filter_enum,
                                              OperationFromEnum(op_enum)));
                   }


// ----- Finds a neighbor of the assignment passed -----

class FindOneNeighbor : public DecisionBuilder {
 public:
  FindOneNeighbor(Assignment* const assignment,
                  SolutionPool* const pool,
                  LocalSearchOperator* const ls_operator,
                  DecisionBuilder* const sub_decision_builder,
                  const SearchLimit* const limit,
                  const std::vector<LocalSearchFilter*>& filters);
  virtual ~FindOneNeighbor() {}
  virtual Decision* Next(Solver* const solver);
  virtual string DebugString() const {
    return "FindOneNeighbor";
  }

 private:
  bool FilterAccept(const Assignment* delta, const Assignment* deltadelta);
  void SynchronizeAll();
  void SynchronizeFilters(const Assignment* assignment);

  Assignment* const assignment_;
  scoped_ptr<Assignment> reference_assignment_;
  SolutionPool* const pool_;
  LocalSearchOperator* const ls_operator_;
  DecisionBuilder* const sub_decision_builder_;
  SearchLimit* limit_;
  const SearchLimit* const original_limit_;
  bool neighbor_found_;
  std::vector<LocalSearchFilter*> filters_;
};

// reference_assignment_ is used to keep track of the last assignment on which
// operators were started, assignment_ corresponding to the last successful
// neighbor.
FindOneNeighbor::FindOneNeighbor(Assignment* const assignment,
                                 SolutionPool* const pool,
                                 LocalSearchOperator* const ls_operator,
                                 DecisionBuilder* const sub_decision_builder,
                                 const SearchLimit* const limit,
                                 const std::vector<LocalSearchFilter*>& filters)
    : assignment_(assignment),
      reference_assignment_(new Assignment(assignment_)),
      pool_(pool),
      ls_operator_(ls_operator),
      sub_decision_builder_(sub_decision_builder),
      limit_(NULL),
      original_limit_(limit),
      neighbor_found_(false),
      filters_(filters) {
  CHECK(NULL != assignment);
  CHECK(NULL != ls_operator);

  // If limit is NULL, default limit is 1 solution
  if (NULL == limit) {
    Solver* const solver = assignment_->solver();
    limit_ = solver->MakeLimit(kint64max, kint64max, kint64max, 1);
  } else {
    limit_ = limit->MakeClone();
  }
}

Decision* FindOneNeighbor::Next(Solver* const solver) {
  CHECK(NULL != solver);

  if (original_limit_ != NULL) {
    limit_->Copy(original_limit_);
  }

  if (!neighbor_found_) {
    // Only called on the first call to Next(), reference_assignment_ has not
    // been synced with assignment_ yet

    // Keeping the code in case a performance problem forces us to
    // use the old code with a zero test on pool_.
    // reference_assignment_->Copy(assignment_);
    pool_->Initialize(assignment_);
    SynchronizeAll();
  }

  {
    // Another assignment is needed to apply the delta
    Assignment* assignment_copy =
        solver->MakeAssignment(reference_assignment_.get());
    int counter = 0;

    DecisionBuilder* restore =
        solver->MakeRestoreAssignment(assignment_copy);
    if (sub_decision_builder_) {
      restore = solver->Compose(restore, sub_decision_builder_);
    }
    Assignment* delta = solver->MakeAssignment();
    Assignment* deltadelta = solver->MakeAssignment();
    while (true) {
      delta->Clear();
      deltadelta->Clear();
      solver->TopPeriodicCheck();
      if (++counter >= FLAGS_cp_local_search_sync_frequency &&
          pool_->SyncNeeded(reference_assignment_.get())) {
        // TODO(user) : SyncNeed(assignment_) ?
        counter = 0;
        SynchronizeAll();
      }

      if (!limit_->Check()
          && ls_operator_->MakeNextNeighbor(delta, deltadelta)) {
        solver->neighbors_ += 1;
        // All filters must be called for incrementality reasons.
        // Empty deltas must also be sent to incremental filters; can be needed
        // to resync filters on non-incremental (empty) moves.
        // TODO(user): Don't call both if no filter is incremental and one
        // of them returned false.
        const bool mh_filter =
            AcceptDelta(solver->ParentSearch(), delta, deltadelta);
        const bool move_filter = FilterAccept(delta, deltadelta);
        if (mh_filter && move_filter) {
          solver->filtered_neighbors_ += 1;
          assignment_copy->Copy(reference_assignment_.get());
          assignment_copy->Copy(delta);
          if (solver->SolveAndCommit(restore)) {
            solver->accepted_neighbors_ += 1;
            assignment_->Store();
            neighbor_found_ = true;
            return NULL;
          }
        }
      } else {
        if (neighbor_found_) {
          AcceptNeighbor(solver->ParentSearch());
          // Keeping the code in case a performance problem forces us to
          // use the old code with a zero test on pool_.
          //          reference_assignment_->Copy(assignment_);
          pool_->RegisterNewSolution(assignment_);
          SynchronizeAll();
        } else {
          break;
        }
      }
    }
  }
  solver->Fail();
  return NULL;
}

bool FindOneNeighbor::FilterAccept(const Assignment* delta,
                                   const Assignment* deltadelta) {
  bool ok = true;
  for (int i = 0; i < filters_.size(); ++i) {
    if (filters_[i]->IsIncremental()) {
      ok = filters_[i]->Accept(delta, deltadelta) && ok;
    } else {
      ok = ok && filters_[i]->Accept(delta, deltadelta);
    }
  }
  return ok;
}

void FindOneNeighbor::SynchronizeAll() {
  pool_->GetNextSolution(reference_assignment_.get());
  neighbor_found_ = false;
  limit_->Init();
  ls_operator_->Start(reference_assignment_.get());
  SynchronizeFilters(reference_assignment_.get());
}

void FindOneNeighbor::SynchronizeFilters(const Assignment* assignment) {
  for (int i = 0; i < filters_.size(); ++i) {
    filters_[i]->Synchronize(assignment);
  }
}

// ---------- Local Search Phase Parameters ----------

class LocalSearchPhaseParameters : public BaseObject {
 public:
  LocalSearchPhaseParameters(SolutionPool* const pool,
                             LocalSearchOperator* ls_operator,
                             DecisionBuilder* sub_decision_builder,
                             SearchLimit* const limit,
                             const std::vector<LocalSearchFilter*>& filters)
      : solution_pool_(pool),
        ls_operator_(ls_operator),
        sub_decision_builder_(sub_decision_builder),
        limit_(limit),
        filters_(filters) {}
  ~LocalSearchPhaseParameters() {}
  virtual string DebugString() const { return "LocalSearchPhaseParameters"; }

  SolutionPool* solution_pool() const { return solution_pool_; }
  LocalSearchOperator* ls_operator() const { return ls_operator_; }
  DecisionBuilder* sub_decision_builder() const {
    return sub_decision_builder_;
  }
  SearchLimit* limit() const { return limit_; }
  const std::vector<LocalSearchFilter*>& filters() const { return filters_; }

 private:
  SolutionPool* const solution_pool_;
  LocalSearchOperator* const ls_operator_;
  DecisionBuilder* const sub_decision_builder_;
  SearchLimit* const limit_;
  std::vector<LocalSearchFilter*> filters_;
};

LocalSearchPhaseParameters* Solver::MakeLocalSearchPhaseParameters(
    LocalSearchOperator* const ls_operator,
    DecisionBuilder* const sub_decision_builder) {
  return MakeLocalSearchPhaseParameters(MakeDefaultSolutionPool(),
                                        ls_operator,
                                        sub_decision_builder,
                                        NULL,
                                        std::vector<LocalSearchFilter*>());
}

LocalSearchPhaseParameters* Solver::MakeLocalSearchPhaseParameters(
    LocalSearchOperator* const ls_operator,
    DecisionBuilder* const sub_decision_builder,
    SearchLimit* const limit) {
  return MakeLocalSearchPhaseParameters(MakeDefaultSolutionPool(),
                                        ls_operator,
                                        sub_decision_builder,
                                        limit,
                                        std::vector<LocalSearchFilter*>());
}

LocalSearchPhaseParameters* Solver::MakeLocalSearchPhaseParameters(
    LocalSearchOperator* const ls_operator,
    DecisionBuilder* const sub_decision_builder,
    SearchLimit* const limit,
    const std::vector<LocalSearchFilter*>& filters) {
  return MakeLocalSearchPhaseParameters(MakeDefaultSolutionPool(),
                                        ls_operator,
                                        sub_decision_builder,
                                        limit,
                                        filters);
}

LocalSearchPhaseParameters* Solver::MakeLocalSearchPhaseParameters(
    SolutionPool* const pool,
    LocalSearchOperator* const ls_operator,
    DecisionBuilder* const sub_decision_builder) {
  return MakeLocalSearchPhaseParameters(pool,
                                        ls_operator,
                                        sub_decision_builder,
                                        NULL,
                                        std::vector<LocalSearchFilter*>());
}

LocalSearchPhaseParameters* Solver::MakeLocalSearchPhaseParameters(
    SolutionPool* const pool,
    LocalSearchOperator* const ls_operator,
    DecisionBuilder* const sub_decision_builder,
    SearchLimit* const limit) {
  return MakeLocalSearchPhaseParameters(pool,
                                        ls_operator,
                                        sub_decision_builder,
                                        limit,
                                        std::vector<LocalSearchFilter*>());
}

LocalSearchPhaseParameters* Solver::MakeLocalSearchPhaseParameters(
    SolutionPool* const pool,
    LocalSearchOperator* const ls_operator,
    DecisionBuilder* const sub_decision_builder,
    SearchLimit* const limit,
    const std::vector<LocalSearchFilter*>& filters) {
  return RevAlloc(new LocalSearchPhaseParameters(pool,
                                                 ls_operator,
                                                 sub_decision_builder,
                                                 limit,
                                                 filters));
}

namespace {
// ----- NestedSolve decision wrapper -----

// This decision calls a nested Solve on the given DecisionBuilder in its
// left branch; does nothing in the left branch.
// The state of the decision corresponds to the result of the nested Solve:
// DECISION_PENDING - Nested Solve not called yet
// DECISION_FAILED  - Nested Solve failed
// DECISION_FOUND   - Nested Solve succeeded

class NestedSolveDecision : public Decision {
 public:
  // This enum is used internally to tag states in the local search tree.
  enum StateType {
    DECISION_PENDING,
    DECISION_FAILED,
    DECISION_FOUND
  };

  NestedSolveDecision(DecisionBuilder* const db,
                      bool restore,
                      const std::vector<SearchMonitor*>& monitors);
  NestedSolveDecision(DecisionBuilder* const db,
                      bool restore);
  virtual ~NestedSolveDecision() {}
  virtual void Apply(Solver* const solver);
  virtual void Refute(Solver* const solver);
  virtual string DebugString() const {
    return "NestedSolveDecision";
  }
  int state() const { return state_; }

 private:
  DecisionBuilder* const db_;
  bool restore_;
  std::vector<SearchMonitor*> monitors_;
  int state_;
};

NestedSolveDecision::NestedSolveDecision(DecisionBuilder* const db,
                                         bool restore,
                                         const std::vector<SearchMonitor*>& monitors)
    : db_(db),
      restore_(restore),
      monitors_(monitors),
      state_(DECISION_PENDING) {
  CHECK(NULL != db);
}

NestedSolveDecision::NestedSolveDecision(DecisionBuilder* const db,
                                         bool restore)
    : db_(db), restore_(restore), state_(DECISION_PENDING) {
  CHECK(NULL != db);
}

void NestedSolveDecision::Apply(Solver* const solver) {
  CHECK(NULL != solver);
  if (restore_) {
    if (solver->Solve(db_, monitors_)) {
      solver->SaveAndSetValue(&state_, static_cast<int>(DECISION_FOUND));
    } else {
      solver->SaveAndSetValue(&state_, static_cast<int>(DECISION_FAILED));
    }
  } else {
    if (solver->SolveAndCommit(db_, monitors_)) {
      solver->SaveAndSetValue(&state_, static_cast<int>(DECISION_FOUND));
    } else {
      solver->SaveAndSetValue(&state_, static_cast<int>(DECISION_FAILED));
    }
  }
}

void NestedSolveDecision::Refute(Solver* const solver) {}

// ----- Local search decision builder -----

// Given a first solution (resulting from either an initial assignment or the
// result of a decision builder), it searches for neighbors using a local
// search operator. The first solution corresponds to the first leaf of the
// search.
// The local search applies to the variables contained either in the assignment
// or the vector of variables passed.

class LocalSearch : public DecisionBuilder {
 public:
  LocalSearch(Assignment* const assignment,
              SolutionPool* const pool,
              LocalSearchOperator* const ls_operator,
              DecisionBuilder* const sub_decision_builder,
              SearchLimit* const limit,
              const std::vector<LocalSearchFilter*>& filters);
  // TODO(user): find a way to not have to pass vars here: redundant with
  // variables in operators
  LocalSearch(IntVar* const* vars,
              int size,
              SolutionPool* const pool,
              DecisionBuilder* const first_solution,
              LocalSearchOperator* const ls_operator,
              DecisionBuilder* const sub_decision_builder,
              SearchLimit* const limit,
              const std::vector<LocalSearchFilter*>& filters);
  virtual ~LocalSearch();
  virtual Decision* Next(Solver* const solver);
  virtual string DebugString() const {
    return "LocalSearch";
  }
  virtual void Accept(ModelVisitor* const visitor) const;

 protected:
  void PushFirstSolutionDecision(DecisionBuilder* first_solution);
  void PushLocalSearchDecision();

 private:
  Assignment* assignment_;
  SolutionPool* const pool_;
  LocalSearchOperator* const ls_operator_;
  DecisionBuilder* const sub_decision_builder_;
  std::vector<NestedSolveDecision*> nested_decisions_;
  int nested_decision_index_;
  SearchLimit* const limit_;
  const std::vector<LocalSearchFilter*> filters_;
  bool has_started_;
};

LocalSearch::LocalSearch(Assignment* const assignment,
                         SolutionPool* const pool,
                         LocalSearchOperator* const ls_operator,
                         DecisionBuilder* const sub_decision_builder,
                         SearchLimit* const limit,
                         const std::vector<LocalSearchFilter*>& filters)
    : assignment_(assignment),
      pool_(pool),
      ls_operator_(ls_operator),
      sub_decision_builder_(sub_decision_builder),
      nested_decision_index_(0),
      limit_(limit),
      filters_(filters),
      has_started_(false) {
  CHECK(NULL != assignment);
  CHECK(NULL != ls_operator);
  Solver* const solver = assignment_->solver();
  DecisionBuilder* restore = solver->MakeRestoreAssignment(assignment_);
  PushFirstSolutionDecision(restore);
  PushLocalSearchDecision();
}

LocalSearch::LocalSearch(IntVar* const* vars,
                         int size,
                         SolutionPool* const pool,
                         DecisionBuilder* const first_solution,
                         LocalSearchOperator* const ls_operator,
                         DecisionBuilder* const sub_decision_builder,
                         SearchLimit* const limit,
                         const std::vector<LocalSearchFilter*>& filters)
    : assignment_(NULL),
      pool_(pool),
      ls_operator_(ls_operator),
      sub_decision_builder_(sub_decision_builder),
      nested_decision_index_(0),
      limit_(limit),
      filters_(filters),
      has_started_(false) {
  CHECK(NULL != first_solution);
  CHECK(NULL != ls_operator);
  CHECK_GE(size, 1);
  Solver* const solver = vars[0]->solver();
  assignment_ = solver->MakeAssignment();
  assignment_->Add(vars, size);
  PushFirstSolutionDecision(first_solution);
  PushLocalSearchDecision();
}

LocalSearch::~LocalSearch() {}

// Model Visitor support.
void LocalSearch::Accept(ModelVisitor* const visitor) const {
  DCHECK(assignment_ != NULL);
  visitor->BeginVisitExtension(ModelVisitor::kVariableGroupExtension);
  // We collect decision variables from the assignment.
  const std::vector<IntVarElement>& elements =
      assignment_->IntVarContainer().elements();
  if (!elements.empty()) {
    std::vector<IntVar*> vars;
    for (ConstIter<std::vector<IntVarElement> > it(elements); !it.at_end(); ++it) {
      vars.push_back((*it).Var());
    }
    visitor->VisitIntegerVariableArrayArgument(ModelVisitor::kVarsArgument,
                                               vars.data(),
                                               vars.size());
  }
  const std::vector<IntervalVarElement>& interval_elements =
      assignment_->IntervalVarContainer().elements();
  if (!interval_elements.empty()) {
    std::vector<IntervalVar*> interval_vars;
    for (ConstIter<std::vector<IntervalVarElement> > it(interval_elements);
         !it.at_end();
         ++it) {
      interval_vars.push_back((*it).Var());
    }
    visitor->VisitIntervalArrayArgument(ModelVisitor::kIntervalsArgument,
                                        interval_vars.data(),
                                        interval_vars.size());
  }
  visitor->EndVisitExtension(ModelVisitor::kVariableGroupExtension);
}


// This is equivalent to a multi-restart decision builder
// TODO(user): abstract this from the local search part
// TODO(user): handle the case where the tree depth is not enough to hold
//                all solutions.

Decision* LocalSearch::Next(Solver* const solver) {
  CHECK(NULL != solver);
  CHECK_LT(0, nested_decisions_.size());
  if (!has_started_) {
    nested_decision_index_ = 0;
    solver->SaveAndSetValue(&has_started_, true);
  } else if (nested_decision_index_ < 0) {
    solver->Fail();
  }
  NestedSolveDecision* decision = nested_decisions_[nested_decision_index_];
  const int state = decision->state();
  switch (state) {
    case NestedSolveDecision::DECISION_FAILED: {
      if (!LocalOptimumReached(solver->ActiveSearch())) {
        nested_decision_index_ = -1;  // Stop the search
      }
      solver->Fail();
      return NULL;
    }
    case NestedSolveDecision::DECISION_PENDING: {
      // TODO(user): Find a way to make this balancing invisible to the
      // user (no increase in branch or fail counts for instance).
      const int32 kLocalSearchBalancedTreeDepth = 32;
      const int depth = solver->SearchDepth();
      if (depth < kLocalSearchBalancedTreeDepth) {
        return solver->balancing_decision();
      } else if (depth > kLocalSearchBalancedTreeDepth) {
        solver->Fail();
      }
      return decision;
    }
    case NestedSolveDecision::DECISION_FOUND: {
      // Next time go to next decision
      if (nested_decision_index_ + 1 < nested_decisions_.size()) {
        ++nested_decision_index_;
      }
      return NULL;
    }
    default: {
      LOG(ERROR) << "Unknown local search state";
      return NULL;
    }
  }
  return NULL;
}

void LocalSearch::PushFirstSolutionDecision(
    DecisionBuilder* first_solution) {
  CHECK(first_solution);
  Solver* const solver = assignment_->solver();
  DecisionBuilder* store = solver->MakeStoreAssignment(assignment_);
  DecisionBuilder* first_solution_and_store =
      solver->Compose(first_solution, sub_decision_builder_, store);
  std::vector<SearchMonitor*> monitor;
  monitor.push_back(limit_);
  nested_decisions_.push_back(
      solver->RevAlloc(new NestedSolveDecision(first_solution_and_store,
                                               false,
                                               monitor)));
}

void LocalSearch::PushLocalSearchDecision() {
  Solver* const solver = assignment_->solver();
  DecisionBuilder* find_neighbors =
      solver->RevAlloc(new FindOneNeighbor(assignment_,
                                           pool_,
                                           ls_operator_,
                                           sub_decision_builder_,
                                           limit_,
                                           filters_));
  nested_decisions_.push_back(
      solver->RevAlloc(new NestedSolveDecision(find_neighbors, false)));
}

class DefaultSolutionPool : public SolutionPool {
 public:
  DefaultSolutionPool() : reference_assignment_(NULL) {}

  virtual ~DefaultSolutionPool() {}

  virtual void Initialize(Assignment* const assignment) {
    reference_assignment_.reset(new Assignment(assignment));
  }

  virtual void RegisterNewSolution(Assignment* const assignment) {
    reference_assignment_->Copy(assignment);
  }

  virtual void GetNextSolution(Assignment* const assignment) {
    assignment->Copy(reference_assignment_.get());
  }

  virtual bool SyncNeeded(Assignment* const local_assignment) {
    return false;
  }
 private:
  scoped_ptr<Assignment> reference_assignment_;
};
}  // namespace

SolutionPool* Solver::MakeDefaultSolutionPool() {
  return RevAlloc(new DefaultSolutionPool());
}

DecisionBuilder* Solver::MakeLocalSearchPhase(
    Assignment* assignment,
    LocalSearchPhaseParameters* parameters) {
  return RevAlloc(new LocalSearch(assignment,
                                  parameters->solution_pool(),
                                  parameters->ls_operator(),
                                  parameters->sub_decision_builder(),
                                  parameters->limit(),
                                  parameters->filters()));
}

DecisionBuilder* Solver::MakeLocalSearchPhase(
    const std::vector<IntVar*>& vars,
    DecisionBuilder* first_solution,
    LocalSearchPhaseParameters* parameters) {
  return RevAlloc(new LocalSearch(vars.data(), vars.size(),
                                  parameters->solution_pool(),
                                  first_solution,
                                  parameters->ls_operator(),
                                  parameters->sub_decision_builder(),
                                  parameters->limit(),
                                  parameters->filters()));
}
}  // namespace operations_research