//===--- ConstraintGraph.cpp - Constraint Graph ---------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file implements the \c ConstraintGraph class, which describes the
// relationships among the type variables within a constraint system.
//
//===----------------------------------------------------------------------===//

#include "ConstraintGraph.h"
#include "ConstraintGraphScope.h"
#include "ConstraintSystem.h"
#include "swift/Basic/Statistic.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/SaveAndRestore.h"
#include <algorithm>
#include <memory>
#include <numeric>

using namespace swift;
using namespace constraints;

#define DEBUG_TYPE "ConstraintGraph"

#pragma mark Graph construction/destruction

ConstraintGraph::ConstraintGraph(ConstraintSystem &cs) : CS(cs) { }

ConstraintGraph::~ConstraintGraph() {
  assert(Changes.empty() && "Scope stack corrupted");
  for (unsigned i = 0, n = TypeVariables.size(); i != n; ++i) {
    auto &impl = TypeVariables[i]->getImpl();
    delete impl.getGraphNode();
    impl.setGraphNode(nullptr);
  }
}

#pragma mark Graph accessors

std::pair<ConstraintGraphNode &, unsigned>
ConstraintGraph::lookupNode(TypeVariableType *typeVar) {
  // Check whether we've already created a node for this type variable.
  auto &impl = typeVar->getImpl();
  if (auto nodePtr = impl.getGraphNode()) {
    assert(impl.getGraphIndex() < TypeVariables.size() && "Out-of-bounds index");
    assert(TypeVariables[impl.getGraphIndex()] == typeVar && 
           "Type variable mismatch");
    return { *nodePtr, impl.getGraphIndex() };
  }

  // Allocate the new node.
  auto nodePtr = new ConstraintGraphNode(typeVar);
  unsigned index = TypeVariables.size();
  impl.setGraphNode(nodePtr);
  impl.setGraphIndex(index);

  // Record this type variable.
  TypeVariables.push_back(typeVar);

  // Record the change, if there are active scopes.
  if (ActiveScope)
    Changes.push_back(Change::addedTypeVariable(typeVar));

  // If this type variable is not the representative of its equivalence class,
  // add it to its representative's set of equivalences.
  auto typeVarRep = CS.getRepresentative(typeVar);
  if (typeVar != typeVarRep)
    mergeNodes(typeVar, typeVarRep);
  else if (auto fixed = CS.getFixedType(typeVarRep)) {
    // Bind the type variable.
    bindTypeVariable(typeVar, fixed);
  }

  return { *nodePtr, index };
}

ArrayRef<TypeVariableType *> ConstraintGraphNode::getEquivalenceClass() const{
  assert(TypeVar == TypeVar->getImpl().getRepresentative(nullptr) &&
         "Can't request equivalence class from non-representative type var");
  return getEquivalenceClassUnsafe();
}

ArrayRef<TypeVariableType *>
ConstraintGraphNode::getEquivalenceClassUnsafe() const{
  if (EquivalenceClass.empty())
    EquivalenceClass.push_back(TypeVar);
  return EquivalenceClass;
}

#pragma mark Node mutation
void ConstraintGraphNode::addConstraint(Constraint *constraint) {
  assert(ConstraintIndex.count(constraint) == 0 && "Constraint re-insertion");
  ConstraintIndex[constraint] = Constraints.size();
  Constraints.push_back(constraint);
}

void ConstraintGraphNode::removeConstraint(Constraint *constraint) {
  auto pos = ConstraintIndex.find(constraint);
  assert(pos != ConstraintIndex.end());

  // Remove this constraint from the constraint mapping.
  auto index = pos->second;
  ConstraintIndex.erase(pos);
  assert(Constraints[index] == constraint && "Mismatched constraint");

  // If this is the last constraint, just pop it off the list and we're done.
  unsigned lastIndex = Constraints.size()-1;
  if (index == lastIndex) {
    Constraints.pop_back();
    return;
  }

  // This constraint is somewhere in the middle; swap it with the last
  // constraint, so we can remove the constraint from the vector in O(1)
  // time rather than O(n) time.
  auto lastConstraint = Constraints[lastIndex];
  Constraints[index] = lastConstraint;
  ConstraintIndex[lastConstraint] = index;
  Constraints.pop_back();
}

void ConstraintGraphNode::addToEquivalenceClass(
       ArrayRef<TypeVariableType *> typeVars) {
  assert(TypeVar == TypeVar->getImpl().getRepresentative(nullptr) &&
         "Can't extend equivalence class of non-representative type var");
  if (EquivalenceClass.empty())
    EquivalenceClass.push_back(TypeVar);
  EquivalenceClass.append(typeVars.begin(), typeVars.end());
}

void ConstraintGraphNode::addFixedBinding(TypeVariableType *typeVar) {
  FixedBindings.push_back(typeVar);
}

void ConstraintGraphNode::removeFixedBinding(TypeVariableType *typeVar) {
  FixedBindings.pop_back();
}

#pragma mark Graph scope management
ConstraintGraphScope::ConstraintGraphScope(ConstraintGraph &CG)
  : CG(CG), ParentScope(CG.ActiveScope), NumChanges(CG.Changes.size())
{
  CG.ActiveScope = this;
}

ConstraintGraphScope::~ConstraintGraphScope() {
  // Pop changes off the stack until we hit the change could we had prior to
  // introducing this scope.
  assert(CG.Changes.size() >= NumChanges && "Scope stack corrupted");
  while (CG.Changes.size() > NumChanges) {
    CG.Changes.back().undo(CG);
    CG.Changes.pop_back();
  }

  // The active scope is now the parent scope.
  CG.ActiveScope = ParentScope;
}

ConstraintGraph::Change
ConstraintGraph::Change::addedTypeVariable(TypeVariableType *typeVar) {
  Change result;
  result.Kind = ChangeKind::AddedTypeVariable;
  result.TypeVar = typeVar;
  return result;
}

ConstraintGraph::Change
ConstraintGraph::Change::addedConstraint(Constraint *constraint) {
  Change result;
  result.Kind = ChangeKind::AddedConstraint;
  result.TheConstraint = constraint;
  return result;
}

ConstraintGraph::Change
ConstraintGraph::Change::removedConstraint(Constraint *constraint) {
  Change result;
  result.Kind = ChangeKind::RemovedConstraint;
  result.TheConstraint = constraint;
  return result;
}

ConstraintGraph::Change
ConstraintGraph::Change::extendedEquivalenceClass(TypeVariableType *typeVar,
                                                  unsigned prevSize) {
  Change result;
  result.Kind = ChangeKind::ExtendedEquivalenceClass;
  result.EquivClass.TypeVar = typeVar;
  result.EquivClass.PrevSize = prevSize;
  return result;
}

ConstraintGraph::Change
ConstraintGraph::Change::boundTypeVariable(TypeVariableType *typeVar,
                                           Type fixed) {
  Change result;
  result.Kind = ChangeKind::BoundTypeVariable;
  result.Binding.TypeVar = typeVar;
  result.Binding.FixedType = fixed.getPointer();
  return result;
}

void ConstraintGraph::Change::undo(ConstraintGraph &cg) {
  /// Temporarily change the active scope to null, so we don't record
  /// any changes made while performing the undo operation.
  llvm::SaveAndRestore<ConstraintGraphScope *> prevActiveScope(cg.ActiveScope,
                                                               nullptr);

  switch (Kind) {
  case ChangeKind::AddedTypeVariable:
    cg.removeNode(TypeVar);
    break;

  case ChangeKind::AddedConstraint:
    cg.removeConstraint(TheConstraint);
    break;

  case ChangeKind::RemovedConstraint:
    cg.addConstraint(TheConstraint);
    break;

  case ChangeKind::ExtendedEquivalenceClass: {
    auto &node = cg[EquivClass.TypeVar];
    node.EquivalenceClass.erase(
      node.EquivalenceClass.begin() + EquivClass.PrevSize,
      node.EquivalenceClass.end());
    break;
   }

  case ChangeKind::BoundTypeVariable:
    cg.unbindTypeVariable(Binding.TypeVar, Binding.FixedType);
    break;
  }
}

#pragma mark Graph mutation

void ConstraintGraph::removeNode(TypeVariableType *typeVar) {
  // Remove this node.
  auto &impl = typeVar->getImpl();
  unsigned index = impl.getGraphIndex();
  delete impl.getGraphNode();
  impl.setGraphNode(nullptr);

  // Remove this type variable from the list.
  unsigned lastIndex = TypeVariables.size()-1;
  if (index < lastIndex)
    TypeVariables[index] = TypeVariables[lastIndex];
  TypeVariables.pop_back();
}

void ConstraintGraph::addConstraint(Constraint *constraint) {
  // For the nodes corresponding to each type variable...
  auto referencedTypeVars = constraint->getTypeVariables();
  for (auto typeVar : referencedTypeVars) {
    // Find the node for this type variable.
    auto &node = (*this)[typeVar];

    // Note the constraint within the node for that type variable.
    node.addConstraint(constraint);
  }

  // If the constraint doesn't reference any type variables, it's orphaned;
  // track it as such.
  if (referencedTypeVars.empty()) {
    OrphanedConstraints.push_back(constraint);
  }

  // Record the change, if there are active scopes.
  if (ActiveScope)
    Changes.push_back(Change::addedConstraint(constraint));
}

void ConstraintGraph::removeConstraint(Constraint *constraint) {
  // For the nodes corresponding to each type variable...
  auto referencedTypeVars = constraint->getTypeVariables();
  for (auto typeVar : referencedTypeVars) {
    // Find the node for this type variable.
    auto &node = (*this)[typeVar];

    // Remove the constraint.
    node.removeConstraint(constraint);
  }

  // If this is an orphaned constraint, remove it from the list.
  if (referencedTypeVars.empty()) {
    auto known = std::find(OrphanedConstraints.begin(),
                           OrphanedConstraints.end(),
                           constraint);
    assert(known != OrphanedConstraints.end() && "missing orphaned constraint");
    *known = OrphanedConstraints.back();
    OrphanedConstraints.pop_back();
  }

  // Record the change, if there are active scopes.
  if (ActiveScope)
    Changes.push_back(Change::removedConstraint(constraint));
}

void ConstraintGraph::mergeNodes(TypeVariableType *typeVar1, 
                                 TypeVariableType *typeVar2) {
  assert(CS.getRepresentative(typeVar1) == CS.getRepresentative(typeVar2) &&
         "type representatives don't match");
  
  // Retrieve the node for the representative that we're merging into.
  auto typeVarRep = CS.getRepresentative(typeVar1);
  auto &repNode = (*this)[typeVarRep];

  // Retrieve the node for the non-representative.
  assert((typeVar1 == typeVarRep || typeVar2 == typeVarRep) &&
         "neither type variable is the new representative?");
  auto typeVarNonRep = typeVar1 == typeVarRep? typeVar2 : typeVar1;

  // Record the change, if there are active scopes.
  if (ActiveScope)
    Changes.push_back(Change::extendedEquivalenceClass(
                        typeVarRep,
                        repNode.getEquivalenceClass().size()));

  // Merge equivalence class from the non-representative type variable.
  auto &nonRepNode = (*this)[typeVarNonRep];
  repNode.addToEquivalenceClass(nonRepNode.getEquivalenceClassUnsafe());
}

void ConstraintGraph::bindTypeVariable(TypeVariableType *typeVar, Type fixed) {
  // If there are no type variables in the fixed type, there's nothing to do.
  if (!fixed->hasTypeVariable())
    return;

  SmallVector<TypeVariableType *, 4> typeVars;
  llvm::SmallPtrSet<TypeVariableType *, 4> knownTypeVars;
  fixed->getTypeVariables(typeVars);
  auto &node = (*this)[typeVar];
  for (auto otherTypeVar : typeVars) {
    if (knownTypeVars.insert(otherTypeVar).second) {
      if (typeVar == otherTypeVar) continue;

      (*this)[otherTypeVar].addFixedBinding(typeVar);
      node.addFixedBinding(otherTypeVar);
    }
  }

  // Record the change, if there are active scopes.
  // Note: If we ever use this to undo the actual variable binding,
  // we'll need to store the change along the early-exit path as well.
  if (ActiveScope)
    Changes.push_back(Change::boundTypeVariable(typeVar, fixed));
}

void ConstraintGraph::unbindTypeVariable(TypeVariableType *typeVar, Type fixed){
  // If there are no type variables in the fixed type, there's nothing to do.
  if (!fixed->hasTypeVariable())
    return;

  SmallVector<TypeVariableType *, 4> typeVars;
  llvm::SmallPtrSet<TypeVariableType *, 4> knownTypeVars;
  fixed->getTypeVariables(typeVars);
  auto &node = (*this)[typeVar];
  for (auto otherTypeVar : typeVars) {
    if (knownTypeVars.insert(otherTypeVar).second) {
      (*this)[otherTypeVar].removeFixedBinding(typeVar);
      node.removeFixedBinding(otherTypeVar);
    }
  }
}

llvm::TinyPtrVector<Constraint *> ConstraintGraph::gatherConstraints(
    TypeVariableType *typeVar, GatheringKind kind,
    llvm::function_ref<bool(Constraint *)> acceptConstraint) {
  llvm::TinyPtrVector<Constraint *> constraints;

  /// Add constraints for the given adjacent type variable.
  llvm::SmallPtrSet<TypeVariableType *, 4> typeVars;
  llvm::SmallPtrSet<Constraint *, 4> visitedConstraints;
  auto addAdjacentConstraints = [&](TypeVariableType *adjTypeVar) {
    auto adjTypeVarsToVisit =
        (*this)[CS.getRepresentative(adjTypeVar)].getEquivalenceClass();
    for (auto adjTypeVarEquiv : adjTypeVarsToVisit) {
      if (!typeVars.insert(adjTypeVarEquiv).second)
        continue;

      for (auto constraint : (*this)[adjTypeVarEquiv].getConstraints()) {
        if (!visitedConstraints.insert(constraint).second)
          continue;

        if (acceptConstraint(constraint))
          constraints.push_back(constraint);
      }
    }
  };

  auto &reprNode = (*this)[CS.getRepresentative(typeVar)];
  auto equivClass = reprNode.getEquivalenceClass();
  for (auto typeVar : equivClass) {
    auto &node = (*this)[typeVar];
    for (auto constraint : node.getConstraints()) {
      if (visitedConstraints.insert(constraint).second &&
          acceptConstraint(constraint))
        constraints.push_back(constraint);

      // If we want all mentions, visit type variables within each of our
      // constraints.
      if (kind == GatheringKind::AllMentions) {
        for (auto adjTypeVar : constraint->getTypeVariables()) {
          addAdjacentConstraints(adjTypeVar);
        }
      }
    }

    // For any type variable mentioned in a fixed binding, add adjacent
    // constraints.
    for (auto adjTypeVar : node.getFixedBindings()) {
      addAdjacentConstraints(adjTypeVar);
    }
  }

  return constraints;
}

#pragma mark Algorithms

/// Perform a depth-first search.
///
/// \param cg The constraint graph.
/// \param typeVar The type variable we're searching from.
/// \param preVisitNode Called before traversing a node. Must return \c
/// false when the node has already been visited.
/// \param visitConstraint Called before considering a constraint. If it
/// returns \c false, that constraint will be skipped.
/// \param visitedConstraints Set of already-visited constraints, used
/// internally to avoid duplicated work.
static void depthFirstSearch(
    ConstraintGraph &cg,
    TypeVariableType *typeVar,
    llvm::function_ref<bool(TypeVariableType *)> preVisitNode,
    llvm::function_ref<bool(Constraint *)> visitConstraint,
    llvm::DenseSet<Constraint *> &visitedConstraints) {
  // Visit this node. If we've already seen it, bail out.
  if (!preVisitNode(typeVar))
    return;

  // Local function to visit adjacent type variables.
  auto visitAdjacencies = [&](ArrayRef<TypeVariableType *> adjTypeVars) {
    for (auto adj : adjTypeVars) {
      if (adj == typeVar)
        continue;

      // Recurse into this node.
      depthFirstSearch(cg, adj, preVisitNode, visitConstraint,
                       visitedConstraints);
    }
  };

  auto &node = cg[typeVar];

  // Walk all of the constraints associated with this node to find related
  // nodes.
  for (auto constraint : node.getConstraints()) {
    // If we've already seen this constraint, skip it.
    if (!visitedConstraints.insert(constraint).second)
      continue;

    if (visitConstraint(constraint))
      visitAdjacencies(constraint->getTypeVariables());
  }

  // Visit all of the other nodes in the equivalence class.
  auto repTypeVar = cg.getConstraintSystem().getRepresentative(typeVar);
  if (typeVar == repTypeVar) {
    // We are the representative, so visit all of the other type variables
    // in this equivalence class.
    visitAdjacencies(node.getEquivalenceClass());
  } else {
    // We are not the representative; visit the representative.
    visitAdjacencies(repTypeVar);
  }

  // Walk any type variables related via fixed bindings.
  visitAdjacencies(node.getFixedBindings());
}

/// Perform a depth-first search.
///
/// \param cg The constraint graph.
/// \param typeVar The type variable we're searching from.
/// \param preVisitNode Called before traversing a node. Must return \c
/// false when the node has already been visited.
/// \param visitConstraint Called before considering a constraint. If it
/// returns \c false, that constraint will be skipped.
static void depthFirstSearch(
    ConstraintGraph &cg,
    TypeVariableType *typeVar,
    llvm::function_ref<bool(TypeVariableType *)> preVisitNode,
    llvm::function_ref<bool(Constraint *)> visitConstraint) {
  llvm::DenseSet<Constraint *> visitedConstraints;
  depthFirstSearch(cg, typeVar, preVisitNode, visitConstraint,
                   visitedConstraints);
}

unsigned ConstraintGraph::computeConnectedComponents(
           std::vector<TypeVariableType *> &typeVars,
           std::vector<unsigned> &components) {
  llvm::SmallDenseMap<TypeVariableType *, unsigned> componentsMap;

  // Perform a depth-first search from each type variable to identify
  // what component it is in.
  unsigned numComponents = 0;
  for (auto typeVar : typeVars) {
    // If we've already assigned a component to this type variable, we're done.
    if (componentsMap.count(typeVar) > 0)
      continue;

    // Record this component.
    unsigned component = numComponents++;

    // Perform a depth-first search to mark those type variables that are
    // in the same component as this type variable.
    depthFirstSearch(
        *this, typeVar,
        [&](TypeVariableType *typeVar) {
          // If we have already seen this node, we're done.
          if (componentsMap.count(typeVar) > 0) {
            assert(componentsMap[typeVar] == component && "Wrong component?");
            return false;
          }

          componentsMap[typeVar] = component;
          return true;
        },
        [&](Constraint *constraint) {
          return true;
        });
  }

  // Figure out which components have unbound type variables; these
  // are the only components and type variables we want to report.
  SmallVector<bool, 4> componentHasUnboundTypeVar(numComponents, false);
  for (auto typeVar : typeVars) {
    // If this type variable has a fixed type, skip it.
    if (CS.getFixedType(typeVar))
      continue;

    assert(componentsMap.count(typeVar) > 0);
    componentHasUnboundTypeVar[componentsMap[typeVar]] = true;
  }

  // Renumber the old components to the new components.
  SmallVector<unsigned, 4> componentRenumbering(numComponents, 0);
  numComponents = 0;
  for (unsigned i : indices(componentRenumbering)) {
    // Skip components that have no unbound type variables.
    if (!componentHasUnboundTypeVar[i])
      continue;

    componentRenumbering[i] = numComponents++;
  }

  // Remove type variables in dead components and provide component
  // numbers for those that remain.
  typeVars.erase(
      std::remove_if(
        typeVars.begin(), typeVars.end(),
        [&](TypeVariableType *typeVar) {
          assert(componentsMap.count(typeVar) > 0);
          unsigned component = componentsMap[typeVar];
          // Remove type variables in dead components.
          if (!componentHasUnboundTypeVar[component])
            return true;

          // Record the (renumbered) component.
          components.push_back(componentRenumbering[component]);
          return false;
        }),
      typeVars.end());

  return numComponents + getOrphanedConstraints().size();
}


/// For a given constraint kind, decide if we should attempt to eliminate its
/// edge in the graph.
static bool shouldContractEdge(ConstraintKind kind) {
  switch (kind) {
  case ConstraintKind::Bind:
  case ConstraintKind::BindParam:
  case ConstraintKind::BindToPointerType:
  case ConstraintKind::Equal:
    return true;

  default:
    return false;
  }
}

bool ConstraintGraph::contractEdges() {
  SmallVector<Constraint *, 16> constraints;
  CS.findConstraints(constraints, [&](const Constraint &constraint) {
    // Track how many constraints did contraction algorithm iterated over.
    incrementConstraintsPerContractionCounter();
    return shouldContractEdge(constraint.getKind());
  });

  bool didContractEdges = false;
  for (auto *constraint : constraints) {
    auto kind = constraint->getKind();

    // Contract binding edges between type variables.
    assert(shouldContractEdge(kind));

    auto t1 = constraint->getFirstType()->getDesugaredType();
    auto t2 = constraint->getSecondType()->getDesugaredType();

    auto tyvar1 = t1->getAs<TypeVariableType>();
    auto tyvar2 = t2->getAs<TypeVariableType>();

    if (!(tyvar1 && tyvar2))
      continue;

    auto isParamBindingConstraint = kind == ConstraintKind::BindParam;

    // If the argument is allowed to bind to `inout`, in general,
    // it's invalid to contract the edge between argument and parameter,
    // but if we can prove that there are no possible bindings
    // which result in attempt to bind `inout` type to argument
    // type variable, we should go ahead and allow (temporary)
    // contraction, because that greatly helps with performance.
    // Such action is valid because argument type variable can
    // only get its bindings from related overload, which gives
    // us enough information to decided on l-valueness.
    if (isParamBindingConstraint && tyvar1->getImpl().canBindToInOut()) {
      bool isNotContractable = true;
      if (auto bindings = CS.getPotentialBindings(tyvar1)) {
        for (auto &binding : bindings.Bindings) {
          auto type = binding.BindingType;
          isNotContractable = type.findIf([&](Type nestedType) -> bool {
            if (auto tv = nestedType->getAs<TypeVariableType>()) {
              if (tv->getImpl().canBindToInOut())
                return true;
            }

            return nestedType->is<InOutType>();
          });

          // If there is at least one non-contractable binding, let's
          // not risk contracting this edge.
          if (isNotContractable)
            break;
        }
      }

      if (isNotContractable)
        continue;
    }

    auto rep1 = CS.getRepresentative(tyvar1);
    auto rep2 = CS.getRepresentative(tyvar2);

    if (((rep1->getImpl().canBindToLValue() ==
          rep2->getImpl().canBindToLValue()) ||
         // Allow l-value contractions when binding parameter types.
         isParamBindingConstraint)) {
      if (CS.TC.getLangOpts().DebugConstraintSolver) {
        auto &log = CS.getASTContext().TypeCheckerDebug->getStream();
        if (CS.solverState)
          log.indent(CS.solverState->depth * 2);

        log << "Contracting constraint ";
        constraint->print(log, &CS.getASTContext().SourceMgr);
        log << "\n";
      }

      // Merge the edges and remove the constraint.
      removeEdge(constraint);
      if (rep1 != rep2)
        CS.mergeEquivalenceClasses(rep1, rep2, /*updateWorkList*/ false);
      didContractEdges = true;
    }
  }
  return didContractEdges;
}

void ConstraintGraph::removeEdge(Constraint *constraint) {
  bool isExistingConstraint = false;

  for (auto &active : CS.ActiveConstraints) {
    if (&active == constraint) {
      CS.ActiveConstraints.erase(constraint);
      isExistingConstraint = true;
      break;
    }
  }

  for (auto &inactive : CS.InactiveConstraints) {
    if (&inactive == constraint) {
      CS.InactiveConstraints.erase(constraint);
      isExistingConstraint = true;
      break;
    }
  }

  if (CS.solverState) {
    if (isExistingConstraint)
      CS.solverState->retireConstraint(constraint);
    else
      CS.solverState->removeGeneratedConstraint(constraint);
  }

  removeConstraint(constraint);
}

void ConstraintGraph::optimize() {
  // Merge equivalence classes until a fixed point is reached.
  while (contractEdges()) {}
}

void ConstraintGraph::incrementConstraintsPerContractionCounter() {
  SWIFT_FUNC_STAT;
  auto &context = CS.getASTContext();
  if (context.Stats)
    context.Stats->getFrontendCounters()
        .NumConstraintsConsideredForEdgeContraction++;
}

#pragma mark Debugging output

void ConstraintGraphNode::print(llvm::raw_ostream &out, unsigned indent) {
  out.indent(indent);
  TypeVar->print(out);
  out << ":\n";

  // Print constraints.
  if (!Constraints.empty()) {
    out.indent(indent + 2);
    out << "Constraints:\n";
    SmallVector<Constraint *, 4> sortedConstraints(Constraints.begin(),
                                                   Constraints.end());
    std::sort(sortedConstraints.begin(), sortedConstraints.end());
    for (auto constraint : sortedConstraints) {
      out.indent(indent + 4);
      constraint->print(out, &TypeVar->getASTContext().SourceMgr);
      out << "\n";
    }
  }

  // Print fixed bindings.
  if (!FixedBindings.empty()) {
    out.indent(indent + 2);
    out << "Fixed bindings: ";
    SmallVector<TypeVariableType *, 4> sortedFixedBindings(
        FixedBindings.begin(), FixedBindings.end());
    std::sort(sortedFixedBindings.begin(), sortedFixedBindings.end(),
              [&](TypeVariableType *typeVar1, TypeVariableType *typeVar2) {
                return typeVar1->getID() < typeVar2->getID();
              });

    interleave(sortedFixedBindings,
               [&](TypeVariableType *typeVar) {
                 out << "$T" << typeVar->getID();
               },
               [&]() {
                 out << ", ";
               });
    out << "\n";
  }

  // Print equivalence class.
  if (TypeVar->getImpl().getRepresentative(nullptr) == TypeVar &&
      EquivalenceClass.size() > 1) {
    out.indent(indent + 2);
    out << "Equivalence class:";
    for (unsigned i = 1, n = EquivalenceClass.size(); i != n; ++i) {
      out << ' ';
      EquivalenceClass[i]->print(out);
    }
    out << "\n";
  }
}

void ConstraintGraphNode::dump() {
  llvm::SaveAndRestore<bool>
    debug(TypeVar->getASTContext().LangOpts.DebugConstraintSolver, true);
  print(llvm::dbgs(), 0);
}

void ConstraintGraph::print(llvm::raw_ostream &out) {
  for (auto typeVar : TypeVariables) {
    (*this)[typeVar].print(out, 2);
    out << "\n";
  }
}

void ConstraintGraph::dump() {
  llvm::SaveAndRestore<bool>
    debug(CS.getASTContext().LangOpts.DebugConstraintSolver, true);
  print(llvm::dbgs());
}

void ConstraintGraph::printConnectedComponents(llvm::raw_ostream &out) {
  std::vector<TypeVariableType *> typeVars;
  typeVars.insert(typeVars.end(), TypeVariables.begin(), TypeVariables.end());
  std::vector<unsigned> components;
  unsigned numComponents = computeConnectedComponents(typeVars, components);
  for (unsigned component = 0; component != numComponents; ++component) {
    out.indent(2);
    out << component << ":";
    for (unsigned i = 0, n = typeVars.size(); i != n; ++i) {
      if (components[i] == component) {
        out << ' ';
        typeVars[i]->print(out);
      }
    }
    out << '\n';
  }
}

void ConstraintGraph::dumpConnectedComponents() {
  printConnectedComponents(llvm::dbgs());
}

#pragma mark Verification of graph invariants

/// Require that the given condition evaluate true.
///
/// If the condition is not true, complain about the problem and abort.
///
/// \param condition The actual Boolean condition.
///
/// \param complaint A string that describes the problem.
///
/// \param cg The constraint graph that failed verification.
///
/// \param node If non-null, the graph node that failed verification.
///
/// \param extraContext If provided, a function that will be called to
/// provide extra, contextual information about the failure.
static void _require(bool condition, const Twine &complaint,
                     ConstraintGraph &cg,
                     ConstraintGraphNode *node,
                     const std::function<void()> &extraContext = nullptr) {
  if (condition)
    return;

  // Complain
  llvm::dbgs() << "Constraint graph verification failed: " << complaint << '\n';
  if (extraContext)
    extraContext();

  // Print the graph.
  // FIXME: Highlight the offending node/constraint/etc.
  cg.print(llvm::dbgs());

  abort();
}

/// Print a type variable value.
static void printValue(llvm::raw_ostream &os, TypeVariableType *typeVar) {
  typeVar->print(os);
}

/// Print a constraint value.
static void printValue(llvm::raw_ostream &os, Constraint *constraint) {
  constraint->print(os, nullptr);
}

/// Print an unsigned value.
static void printValue(llvm::raw_ostream &os, unsigned value) {
  os << value;
}

void ConstraintGraphNode::verify(ConstraintGraph &cg) {
#define require(condition, complaint) _require(condition, complaint, cg, this)
#define requireWithContext(condition, complaint, context) \
  _require(condition, complaint, cg, this, context)
#define requireSameValue(value1, value2, complaint)             \
  _require(value1 == value2, complaint, cg, this, [&] {         \
    llvm::dbgs() << "  ";                                       \
    printValue(llvm::dbgs(), value1);                           \
    llvm::dbgs() << " != ";                                     \
    printValue(llvm::dbgs(), value2);                           \
    llvm::dbgs() << '\n';                                       \
  })

  // Verify that the constraint map/vector haven't gotten out of sync.
  requireSameValue(Constraints.size(), ConstraintIndex.size(),
                   "constraint vector and map have different sizes");
  for (auto info : ConstraintIndex) {
    require(info.second < Constraints.size(), "constraint index out-of-range");
    requireSameValue(info.first, Constraints[info.second],
                     "constraint map provides wrong index into vector");
  }
#undef requireSameValue
#undef requireWithContext
#undef require
}

void ConstraintGraph::verify() {
#define require(condition, complaint) \
  _require(condition, complaint, *this, nullptr)
#define requireWithContext(condition, complaint, context) \
  _require(condition, complaint, *this, nullptr, context)
#define requireSameValue(value1, value2, complaint)             \
  _require(value1 == value2, complaint, *this, nullptr, [&] {   \
    llvm::dbgs() << "  ";                                       \
    printValue(llvm::dbgs(), value1);                           \
    llvm::dbgs() << " != ";                                     \
    printValue(llvm::dbgs(), value2);                           \
    llvm::dbgs() << '\n';                                       \
  })

  // Verify that the type variables are either representatives or represented
  // within their representative's equivalence class.
  // FIXME: Also check to make sure the equivalence classes aren't too large?
  for (auto typeVar : TypeVariables) {
    auto typeVarRep = CS.getRepresentative(typeVar);
    auto &repNode = (*this)[typeVarRep];
    if (typeVar != typeVarRep) {
      // This type variable should be in the equivalence class of its
      // representative.
      require(std::find(repNode.getEquivalenceClass().begin(),
                        repNode.getEquivalenceClass().end(),
                        typeVar) != repNode.getEquivalenceClass().end(),
              "type variable not present in its representative's equiv class");
    } else {
      // Each of the type variables in the same equivalence class as this type
      // should have this type variable as their representative.
      for (auto equiv : repNode.getEquivalenceClass()) {
        requireSameValue(
          typeVar, equiv->getImpl().getRepresentative(nullptr),
          "representative and an equivalent type variable's representative");
      }
    }
  }

  // Verify that our type variable map/vector are in sync.
  for (unsigned i = 0, n = TypeVariables.size(); i != n; ++i) {
    auto typeVar = TypeVariables[i];
    auto &impl = typeVar->getImpl();
    requireSameValue(impl.getGraphIndex(), i, "wrong graph node index");
    require(impl.getGraphNode(), "null graph node");
  }

  // Verify consistency of all of the nodes in the graph.
  for (unsigned i = 0, n = TypeVariables.size(); i != n; ++i) {
    auto typeVar = TypeVariables[i];
    auto &impl = typeVar->getImpl();
    impl.getGraphNode()->verify(*this);
  }

  // Collect all of the constraints known to the constraint graph.
  llvm::SmallPtrSet<Constraint *, 4> knownConstraints;
  for (auto typeVar : getTypeVariables()) {
    for (auto constraint : (*this)[typeVar].getConstraints())
      knownConstraints.insert(constraint);
  }

  // Verify that all of the constraints in the constraint system
  // are accounted for.
  for (auto &constraint : CS.getConstraints()) {
    // Check whether the constraint graph knows about this constraint.
    auto referencedTypeVars = constraint.getTypeVariables();
    requireWithContext((knownConstraints.count(&constraint) ||
                        referencedTypeVars.empty()),
                       "constraint graph doesn't know about constraint",
                       [&] {
                         llvm::dbgs() << "constraint = ";
                         printValue(llvm::dbgs(), &constraint);
                         llvm::dbgs() << "\n";
                       });

    // Make sure each of the type variables referenced knows about this
    // constraint.
    for (auto typeVar : referencedTypeVars) {
      auto nodePtr = typeVar->getImpl().getGraphNode();
      requireWithContext(nodePtr,
                         "type variable in constraint not known",
                         [&] {
                           llvm::dbgs() << "type variable = ";
                           printValue(llvm::dbgs(), typeVar);
                           llvm::dbgs() << ", constraint = ";
                           printValue(llvm::dbgs(), &constraint);
                           llvm::dbgs() << "\n";
                         });

      auto &node = *nodePtr;
      auto constraintPos = node.ConstraintIndex.find(&constraint);
      requireWithContext(constraintPos != node.ConstraintIndex.end(),
                         "type variable doesn't know about constraint",
                         [&] {
                           llvm::dbgs() << "type variable = ";
                           printValue(llvm::dbgs(), typeVar);
                           llvm::dbgs() << ", constraint = ";
                           printValue(llvm::dbgs(), &constraint);
                           llvm::dbgs() << "\n";
                         });
    }
  }

#undef requireSameValue
#undef requireWithContext
#undef require
}


