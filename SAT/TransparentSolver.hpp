/**
 * @file TransparentSolver.hpp
 * Defines class TransparentSolver.
 */

#ifndef __TransparentSolver__
#define __TransparentSolver__

#include "Forwards.hpp"

#include "Lib/DArray.hpp"
#include "Lib/ScopedPtr.hpp"
#include "Lib/Stack.hpp"

#include "SATSolver.hpp"


namespace SAT {

class TransparentSolver : public SATSolver
{
public:
  TransparentSolver(SATSolver* inner);

  virtual Status getStatus() { return _inner->getStatus(); }
  virtual SATClause* getRefutation() { return _inner->getRefutation(); }

  virtual void ensureVarCnt(unsigned newVarCnt);
  virtual void addClauses(SATClauseIterator cit, bool onlyPropagate);
  virtual MaybeBool getAssignment(unsigned var);

  virtual void addAssumption(SATLiteral lit, bool onlyPropagate);
  virtual void retractAllAssumptions();
  virtual bool hasAssumptions() const { return _assumptions.isNonEmpty(); }
private:

  void processUnprocessed();

  void processUnit(SATClause* cl);

  void makeVarNonPure(unsigned var);

  bool tryWatchOrSubsume(SATClause* cl, unsigned forbiddenVar=0);

  bool tryToSweepPure(unsigned var, bool eager);

  void flushClausesToInner(bool onlyPropagate);

  void addInnerAssumption(SATLiteral lit, bool onlyPropagate);

  struct VarInfo
  {
    VarInfo() : _unseen(true), _isRewritten(false), _unit(0), _hasAssumption(false) {}

    /** If true, _isPure needs yet to be initilized */
    bool _unseen;
    bool _isPure;
    bool _isRewritten;
    /** If variable has an unit clause, it contains it, otherwise zero */
    SATClause* _unit;
    /** Relevant if _isPure. True, if there are only positive occurences
     * of the variable. */
    bool _isPurePositive;
    /** If !_isPure, must be empty */
    SATClauseStack _watched;
    /** If _isRewritter, contains literal to which the variable translates */
    SATLiteral _root;

    bool _hasAssumption;
    bool _assumedPolarity;
  };

  SATSolverSCP _inner;

  //local variables (are invalid when the execution leaves the object)
  SATClauseStack _unprocessed;
  SATClauseStack _toBeAdded;

  DArray<VarInfo> _vars;

  SATLiteralStack _assumptions;
};

}

#endif // __TransparentSolver__