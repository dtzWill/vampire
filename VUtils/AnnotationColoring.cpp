/**
 * @file AnnotationColoring.cpp
 * Implements class AnnotationColoring.
 */

#include "Lib/Environment.hpp"

#include "Kernel/Unit.hpp"
#include "Kernel/Signature.hpp"

#include "Shell/CommandLine.hpp"
#include "Shell/TPTP.hpp"
#include "Shell/UIHelper.hpp"


#include "AnnotationColoring.hpp"

namespace VUtils
{

/**
 * Output color info on symbol @c sym, unless it is an equality. In that
 * case do nothing.
 */
void AnnotationColoring::outputColorInfo(ostream& out, SymId sym, string color)
{
  CALL("AnnotationColoring::outputColorInfo");

  bool pred;
  unsigned functor;
  symEx.decodeSymId(sym, pred, functor);
  if(pred) {
    if(functor==0) {
      return;  //we do not color equality
    }
    out<<"vampire(symbol,predicate,"
	<<env.signature->predicateName(functor)<<","
	<<env.signature->predicateArity(functor)<<","<<color<<")."<<endl;
  }
  else {
    out<<"vampire(symbol,function,"
	<<env.signature->functionName(functor)<<","
	<<env.signature->functionArity(functor)<<","<<color<<")."<<endl;
  }
}


/**
 * Try to assign colors to symbols of a problem according to
 * their apearance in axioms and conjecture.
 * Return 0 if both left and right color could be assigned to
 * at least one symbol; otherwise return 1.
 */
int AnnotationColoring::perform(int argc, char** argv)
{
  CALL("AnnotationColoring::perform");
  ASS_GE(argc,2);

  bool conjectureColoring = string(argv[1])=="conjecture_coloring";
  ASS_REP(conjectureColoring || string(argv[1])=="axiom_coloring", argv[1]);

  //remove the first argument
  argc--; argv++;

  Shell::CommandLine cl(argc,argv);
  cl.interpret(*env.options);

  UnitList* units=UIHelper::getInputUnits();

  UnitList::Iterator uit(units);
  while(uit.hasNext()) {
    Unit* u=uit.next();

    bool isAxiom;
    if(conjectureColoring) {
      isAxiom = u->inputType()!=Unit::CONJECTURE;
    }
    else {
      isAxiom = u->inputType()==Unit::AXIOM;
    }

    SymIdSet& localSet = isAxiom ? axiomSymbols : conjectureSymbols;
    SymIdIterator sit = symEx.extractSymIds(u);
    while(sit.hasNext()) {
      SymId s=sit.next();

      symbols.insert(s);
      localSet.insert(s);
    }

    if(isAxiom) {
      axiomStack.push(u);
    }
    else {
      conjectureStack.push(u);
    }
  }
  bool assignedToSome[2]={false, false};

  env.beginOutput();

  VirtualIterator<SymId> leftIt = axiomSymbols.iterator();
  while(leftIt.hasNext()) {
    SymId sym = leftIt.next();

    if(conjectureSymbols.find(sym)) {
      continue; //shared symbol
    }
    outputColorInfo(env.out(), sym, "left");
    assignedToSome[0] = true;
  }

  VirtualIterator<SymId> rightIt = conjectureSymbols.iterator();
  while(rightIt.hasNext()) {
    SymId sym = rightIt.next();

    if(axiomSymbols.find(sym)) {
      continue; //shared symbol
    }
    outputColorInfo(env.out(), sym, "right");
    assignedToSome[1] = true;
  }

  env.out()<<endl;

  env.out()<<"vampire(left_formula)."<<endl;
  Stack<Unit*>::BottomFirstIterator leftUnitIt(axiomStack);
  while(leftUnitIt.hasNext()) {
    Unit* u = leftUnitIt.next();
    env.out()<<TPTP::toString(u)<<endl;
  }
  env.out()<<"vampire(end_formula)."<<endl<<endl<<endl;

  env.out()<<"vampire(right_formula)."<<endl;
  Stack<Unit*>::BottomFirstIterator rightUnitIt(conjectureStack);
  while(rightUnitIt.hasNext()) {
    Unit* u = rightUnitIt.next();
    env.out()<<TPTP::toString(u)<<endl;
  }
  env.out()<<"vampire(end_formula)."<<endl<<endl<<endl;

  env.endOutput();

  return (assignedToSome[0] && assignedToSome[1]) ? 0 : 1;
}

}