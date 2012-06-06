/* -*- mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*- */
/*
 *  Main authors:
 *     Guido Tack <tack@gecode.org>
 *
 *  Copyright:
 *     Guido Tack, 2007
 *
 *  Last modified:
 *     $Date: 2010-07-02 19:18:43 +1000 (Fri, 02 Jul 2010) $ by $Author: tack $
 *     $Revision: 11149 $
 *
 *  This file is part of Gecode, the generic constraint
 *  development environment:
 *     http://www.gecode.org
 *
 *  Permission is hereby granted, free of charge, to any person obtaining
 *  a copy of this software and associated documentation files (the
 *  "Software"), to deal in the Software without restriction, including
 *  without limitation the rights to use, copy, modify, merge, publish,
 *  distribute, sublicense, and/or sell copies of the Software, and to
 *  permit persons to whom the Software is furnished to do so, subject to
 *  the following conditions:
 *
 *  The above copyright notice and this permission notice shall be
 *  included in all copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 *  EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 *  MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 *  NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
 *  LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 *  OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 *  WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#include "base/stringprintf.h"
#include "flatzinc/flatzinc.h"

#include <vector>
#include <string>
using namespace std;

namespace operations_research {

FlatZincModel::FlatZincModel(void)
    : int_var_count(-1),
      bool_var_count(-1),
      set_var_count(-1),
      objective_variable_(-1),
      solve_annotations_(NULL),
      solver_("FlatZincSolver"),
      objective_(NULL),
      output_(NULL) {}

void FlatZincModel::Init(int intVars, int boolVars, int setVars) {
  int_var_count = 0;
  integer_variables_.resize(intVars);
  bool_var_count = 0;
  boolean_variables_.resize(boolVars);
  set_var_count = 0;
}

void FlatZincModel::NewIntVar(const std::string& name, IntVarSpec* const vs) {
  if (vs->alias) {
    integer_variables_[int_var_count++] = integer_variables_[vs->i];
  } else if (vs->assigned) {
    integer_variables_[int_var_count++] = solver_.MakeIntConst(vs->i, name);
  } else {
    if (!vs->HasDomain()) {
        integer_variables_[int_var_count++] =
            solver_.MakeIntVar(kint32min, kint32max, name);
    } else {
      AST::SetLit* const domain = vs->Domain();
      if (domain->interval) {
        integer_variables_[int_var_count++] =
            solver_.MakeIntVar(domain->min, domain->max, name);
      } else {
        integer_variables_[int_var_count++] =
            solver_.MakeIntVar(domain->s, name);
      }
    }
    VLOG(1) << "Create IntVar: "
            << integer_variables_[int_var_count - 1]->DebugString();
    if (!vs->introduced) {
      active_variables_.push_back(integer_variables_[int_var_count - 1]);
    }
  }
}

void FlatZincModel::SkipIntVar() {
  integer_variables_[int_var_count++] = NULL;
}

void FlatZincModel::NewBoolVar(const std::string& name, BoolVarSpec* const vs) {
  if (vs->alias) {
    boolean_variables_[bool_var_count++] = boolean_variables_[vs->i];
  } else if (vs->assigned) {
    boolean_variables_[bool_var_count++] = solver_.MakeIntConst(vs->i, name);
  } else {
    boolean_variables_[bool_var_count++] = solver_.MakeBoolVar(name);
    VLOG(1) << "Create BoolVar: "
            << boolean_variables_[bool_var_count - 1]->DebugString();
    if (!vs->introduced) {
      active_variables_.push_back(boolean_variables_[bool_var_count - 1]);
    }
  }
}

void FlatZincModel::SkipBoolVar() {
  boolean_variables_[bool_var_count++] = NULL;
}

// void FlatZincModel::newSetVar(SetVarSpec* vs) {
//   if (vs->alias) {
//     sv[set_var_count++] = sv[vs->i];
//   } else {
//     LOG(FATAL) << "SetVar not supported";
//     sv[set_var_count++] = SetVar();
//   }
//   sv_introduced[set_var_count-1] = vs->introduced;
// }

void FlattenAnnotations(AST::Array* const annotations,
                        std::vector<AST::Node*>& out) {
  for (unsigned int i=0; i < annotations->a.size(); i++) {
    if (annotations->a[i]->isCall("seq_search")) {
      AST::Call* c = annotations->a[i]->getCall();
      if (c->args->isArray()) {
        FlattenAnnotations(c->args->getArray(), out);
      } else {
        out.push_back(c->args);
      }
    } else {
      out.push_back(annotations->a[i]);
    }
  }
}

void FlatZincModel::CreateDecisionBuilders(bool ignore_unknown,
                                           bool ignore_annotations) {
  AST::Node* annotations = solve_annotations_;
  if (annotations && !ignore_annotations) {
    std::vector<AST::Node*> flat_annotations;
    if (annotations->isArray()) {
      FlattenAnnotations(annotations->getArray(), flat_annotations);
    } else {
      flat_annotations.push_back(annotations);
    }

    if (method_ != SAT && flat_annotations.size() == 1) {
      builders_.push_back(solver_.MakePhase(active_variables_,
                                            Solver::CHOOSE_FIRST_UNBOUND,
                                            Solver::ASSIGN_MIN_VALUE));
      VLOG(1) << "Decision builder = " << builders_.back()->DebugString();
    }

    for (unsigned int i=0; i < flat_annotations.size(); i++) {
      try {
        AST::Call *call = flat_annotations[i]->getCall("int_search");
        LOG(INFO) << call->DebugString();
        AST::Array *args = call->getArgs(4);
        LOG(INFO) << "args = " << args->DebugString();
        AST::Array *vars = args->a[0]->getArray();
        Solver::IntVarStrategy str = Solver::CHOOSE_FIRST_UNBOUND;
        if (args->hasAtom("first_fail")) {
          str = Solver::CHOOSE_MIN_SIZE;
        }
        if (args->hasAtom("anti_first_fail")) {
          str = Solver::CHOOSE_MAX_SIZE;
        }
        if (args->hasAtom("smallest")) {
          str = Solver::CHOOSE_LOWEST_MIN;
        }
        if (args->hasAtom("largest")) {
          str = Solver::CHOOSE_HIGHEST_MAX;
        }
        Solver::IntValueStrategy vstr = Solver::ASSIGN_MIN_VALUE;
        if (args->hasAtom("indomain_max")) {
          vstr = Solver::ASSIGN_MAX_VALUE;
        }
        if (args->hasAtom("indomain_median")) {
          vstr = Solver::ASSIGN_CENTER_VALUE;
        }
        std::vector<IntVar*> int_vars;
        for (int i = 0; i < vars->a.size(); ++i) {
          int_vars.push_back(integer_variables_[vars->a[i]->getIntVar()]);
        }
        builders_.push_back(solver_.MakePhase(int_vars, str, vstr));
      } catch (AST::TypeError& e) {
        (void) e;
        try {
          AST::Call *call = flat_annotations[i]->getCall("bool_search");
          AST::Array *args = call->getArgs(4);
          AST::Array *vars = args->a[0]->getArray();
          std::vector<IntVar*> int_vars;
          for (int i = 0; i < vars->a.size(); ++i) {
            int_vars.push_back(boolean_variables_[vars->a[i]->getBoolVar()]);
          }
          builders_.push_back(solver_.MakePhase(int_vars,
                                                Solver::CHOOSE_FIRST_UNBOUND,
                                                Solver::ASSIGN_MAX_VALUE));
        } catch (AST::TypeError& e) {
          (void) e;
          try {
            AST::Call *call = flat_annotations[i]->getCall("set_search");
            AST::Array *args = call->getArgs(4);
            AST::Array *vars = args->a[0]->getArray();
            LOG(FATAL) << "Search on set variables not supported";
          } catch (AST::TypeError& e) {
            (void) e;
            if (!ignore_unknown) {
              LOG(WARNING) << "Warning, ignored search annotation: "
                           << flat_annotations[i]->DebugString();
            }
          }
        }
      }
      VLOG(1) << "Adding decision builder = "
              << builders_.back()->DebugString();
    }
  } else {
    builders_.push_back(solver_.MakePhase(active_variables_,
                                          Solver::CHOOSE_FIRST_UNBOUND,
                                          Solver::ASSIGN_MIN_VALUE));
    VLOG(1) << "Decision builder = " << builders_.back()->DebugString();
  }
}

void FlatZincModel::Satisfy(AST::Array* const annotations) {
  method_ = SAT;
  solve_annotations_ = annotations;
}

void FlatZincModel::Minimize(int var, AST::Array* const annotations) {
  method_ = MIN;
  objective_variable_ = var;
  solve_annotations_ = annotations;
  // Branch on optimization variable to ensure that it is given a value.
  AST::Array* args = new AST::Array(4);
  args->a[0] = new AST::Array(new AST::IntVar(objective_variable_));
  args->a[1] = new AST::Atom("input_order");
  args->a[2] = new AST::Atom("indomain_min");
  args->a[3] = new AST::Atom("complete");
  AST::Call* c = new AST::Call("int_search", args);
  if (!solve_annotations_) {
    solve_annotations_ = new AST::Array(c);
  } else {
    solve_annotations_->a.push_back(c);
  }
  objective_ = solver_.MakeMinimize(integer_variables_[objective_variable_], 1);
}

void FlatZincModel::Maximize(int var, AST::Array* const annotations) {
  method_ = MAX;
  objective_variable_ = var;
  solve_annotations_ = annotations;
  // Branch on optimization variable to ensure that it is given a value.
  AST::Array* args = new AST::Array(4);
  args->a[0] = new AST::Array(new AST::IntVar(objective_variable_));
  args->a[1] = new AST::Atom("input_order");
  args->a[2] = new AST::Atom("indomain_min");
  args->a[3] = new AST::Atom("complete");
  AST::Call* c = new AST::Call("int_search", args);
  if (!solve_annotations_) {
    solve_annotations_ = new AST::Array(c);
  } else {
    solve_annotations_->a.push_back(c);
  }
  objective_ = solver_.MakeMaximize(integer_variables_[objective_variable_], 1);
}

FlatZincModel::~FlatZincModel(void) {
  delete solve_annotations_;
  delete output_;
}

void FlatZincModel::Solve(int solve_frequency,
                          bool use_log,
                          bool all_solutions,
                          bool ignore_annotations,
                          int num_solutions,
                          int time_limit_in_ms) {
  CreateDecisionBuilders(false, ignore_annotations);
  if (all_solutions && num_solutions == 0) {
    num_solutions = kint32max;
  } else if (objective_ == NULL) {
    num_solutions = 1;
  }
  std::vector<SearchMonitor*> monitors;
  switch (method_) {
    case MIN:
    case MAX: {
      SearchMonitor* const log = use_log ?
          solver_.MakeSearchLog(solve_frequency, objective_) :
          NULL;
      monitors.push_back(log);
      monitors.push_back(objective_);
      break;
    }
    case SAT: {
      SearchMonitor* const log = use_log ?
          solver_.MakeSearchLog(solve_frequency) :
          NULL;
      monitors.push_back(log);
      break;
    }
  }

  if (time_limit_in_ms > 0) {
    monitors.push_back(solver_.MakeLimit(time_limit_in_ms,
                                         kint64max,
                                         kint64max,
                                         kint64max));
  }

  int count = 0;
  solver_.NewSearch(solver_.Compose(builders_), monitors);
  while (solver_.NextSolution()) {
    if (output_ != NULL) {
      for (unsigned int i = 0; i < output_->a.size(); i++) {
        std::cout << DebugString(output_->a[i]);
      }
      std::cout << "----------" << std::endl;
    }
    count++;
    if (num_solutions > 0 && count >= num_solutions) {
      break;
    }
  }
  solver_.EndSearch();
}

void FlatZincModel::InitOutput(AST::Array* const output) {
  output_ = output;
}

string FlatZincModel::DebugString(AST::Node* const ai) const {
  string output;
  int k;
  if (ai->isArray()) {
    AST::Array* aia = ai->getArray();
    int size = aia->a.size();
    output += "[";
    for (int j = 0; j < size; j++) {
      output += DebugString(aia->a[j]);
      if (j < size - 1) {
        output += ", ";
      }
    }
    output += "]";
  } else if (ai->isInt(k)) {
    output += StringPrintf("%d", k);
  } else if (ai->isIntVar()) {
    IntVar* const var = integer_variables_[ai->getIntVar()];
    output += StringPrintf("%d", var->Value());
  } else if (ai->isBoolVar()) {
    IntVar* const var = boolean_variables_[ai->getBoolVar()];
    output += var->Value() ? "true" : "false";
  } else if (ai->isSetVar()) {
    LOG(FATAL) << "Set variables not implemented";
  } else if (ai->isBool()) {
    output += (ai->getBool() ? "true" : "false");
  } else if (ai->isSet()) {
    AST::SetLit* s = ai->getSet();
    if (s->interval) {
      output += StringPrintf("%d..%d", s->min, s->max);
    } else {
      output += "{";
      for (unsigned int i=0; i<s->s.size(); i++) {
        output += StringPrintf("%d%s",
                               s->s[i],
                               (i < s->s.size()-1 ? ", " : "}"));
      }
    }
  } else if (ai->isString()) {
    string s = ai->getString();
    for (unsigned int i = 0; i < s.size(); i++) {
      if (s[i] == '\\' && i < s.size() - 1) {
        switch (s[i+1]) {
          case 'n': output += "\n"; break;
          case '\\': output += "\\"; break;
          case 't': output += "\t"; break;
          default: {
            output += "\\";
            output += s[i+1];
          }
        }
        i++;
      } else {
        output += s[i];
      }
    }
  }
  return output;
}

IntVar* FlatZincModel::GetIntVar(AST::Node* const node) {
  if (node->isIntVar()) {
    return integer_variables_[node->getIntVar()];
  } else if (node->isBoolVar()) {
    return boolean_variables_[node->getBoolVar()];
  } else if (node->isInt()) {
    return solver_.MakeIntConst(node->getInt());
  } else if (node->isBool()) {
    return solver_.MakeIntConst(node->getBool());
  } else {
    LOG(FATAL) << "Cannot build an IntVar from " << node->DebugString();
    return NULL;
  }
}

}  // namespace operations_research
