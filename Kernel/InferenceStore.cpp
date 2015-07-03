/**
 * @file InferenceStore.cpp
 * Implements class InferenceStore.
 */

#include "Lib/Allocator.hpp"
#include "Lib/DHSet.hpp"
#include "Lib/Environment.hpp"
#include "Lib/Int.hpp"
#include "Lib/ScopedPtr.hpp"
#include "Lib/SharedSet.hpp"
#include "Lib/Stack.hpp"
#include "Lib/StringUtils.hpp"
#include "Lib/ScopedPtr.hpp"

#include "Shell/LaTeX.hpp"
#include "Shell/Options.hpp"
#include "Shell/Statistics.hpp"

#include "Parse/TPTP.hpp"

#include "Signature.hpp"
#include "Clause.hpp"
#include "Formula.hpp"
#include "FormulaUnit.hpp"
#include "FormulaVarIterator.hpp"
#include "Inference.hpp"
#include "Term.hpp"
#include "TermIterators.hpp"
#include "SortHelper.hpp"

#include "InferenceStore.hpp"

//TODO: when we delete clause, we should also delete all its records from the inference store

namespace Kernel
{

using namespace std;
using namespace Lib;
using namespace Shell;

void InferenceStore::FullInference::increasePremiseRefCounters()
{
  CALL("InferenceStore::FullInference::increasePremiseRefCounters");

  for(unsigned i=0;i<premCnt;i++) {
    if (premises[i].isClause()) {
      premises[i].cl()->incRefCnt();
    }
  }
}



InferenceStore::InferenceStore()
{
}

vstring InferenceStore::getUnitIdStr(UnitSpec cs)
{
  CALL("InferenceStore::getUnitIdStr");

  if (!cs.isClause()) {
    return Int::toString(cs.unit()->number());
  }
  vstring suffix=getClauseIdSuffix(cs);
  if (suffix=="") {
    return Int::toString(cs.cl()->number());
  }
  return Int::toString(cs.cl()->number())+"_"+suffix;
}

vstring InferenceStore::getClauseIdSuffix(UnitSpec cs)
{
  CALL("InferenceStore::getClauseIdSuffix");

  FullInference* finf;
  if (_data.find(cs,finf)) {
    if (!finf->csId) {
      finf->csId=_nextClIds.insert(cs.cl());
    }
    return Int::toString(finf->csId);
  } else {
      return "";
  }
}


/**
 * Increase the reference counter on premise clauses and store @c inf as inference
 * of @c unit.
 */
void InferenceStore::recordInference(UnitSpec unit, FullInference* inf)
{
  CALL("InferenceStore::recordInference");

  inf->increasePremiseRefCounters();
  _data.set(unit, inf);
}


/**
 * Records informations needed for outputting proofs of general splitting
 */
void InferenceStore::recordSplittingNameLiteral(UnitSpec us, Literal* lit)
{
  CALL("InferenceStore::recordSplittingNameLiteral");

  //each clause is result of a splitting only once
  ALWAYS(_splittingNameLiterals.insert(us, lit));
}


/**
 * Record the introduction of a new symbol
 */
void InferenceStore::recordIntroducedSymbol(Unit* u, bool func, unsigned number)
{
  CALL("InferenceStore::recordIntroducedSymbol");

  SymbolStack* pStack;
  _introducedSymbols.getValuePtr(u->number(),pStack);
  pStack->push(SymbolId(func,number));
}


/**
 * Get the parents of unit represented by us and fill in the rule used to generate this unit
 *
 * This will first check if the unit was generated by a special inference that was
 * recorded in the InferenceStore and if not, use the inference stored in the unit itself
 */
UnitSpecIterator InferenceStore::getParents(UnitSpec us, Inference::Rule& rule)
{
  CALL("InferenceStore::getParents/2");
  ASS(!us.isEmpty());

  FullInference* finf;
  //Check if special inference stored in _data
  if (_data.find(us, finf)) {
    rule=finf->rule;
    return pvi( PointerIterator<UnitSpec>(finf->premises, finf->premises+finf->premCnt) );
  }
  //Otherwise the unit itself stores the inference
  Unit* u=us.unit();
  List<UnitSpec>* res=0;
  Inference* inf=u->inference();
  Inference::Iterator iit=inf->iterator();
  while(inf->hasNext(iit)) {
    Unit* premUnit=inf->next(iit);
    List<UnitSpec>::push(UnitSpec(premUnit, true), res);
  }
  rule=inf->rule();
  res=res->reverse(); //we want items in the same order
  return pvi( List<UnitSpec>::DestructiveIterator(res) );
}

/**
 * Get parents where we do not care about the generating rule
 */
UnitSpecIterator InferenceStore::getParents(UnitSpec us)
{
  CALL("InferenceStore::getParents/1");

  Inference::Rule aux;
  return getParents(us, aux);
}

/**
 * Return @b inner quentified over variables in @b vars
 *
 * It is caller's responsibility to ensure that variables in @b vars are unique.
 */
template<typename VarContainer>
vstring getQuantifiedStr(const VarContainer& vars, vstring inner, DHMap<unsigned,unsigned>& t_map, bool innerParentheses=true){
  CALL("getQuantifiedStr(VarContainer, vstring, map)");

  VirtualIterator<unsigned> vit=pvi( getContentIterator(vars) );
  vstring varStr;
  bool first=true;
  while(vit.hasNext()) {
    unsigned var =vit.next();
    if (!first) {
      varStr+=",";
    }
    vstring ty="";
    unsigned t;
    if(t_map.find(var,t)){
      switch(t){
        //create types for variable
        case Sorts::SRT_DEFAULT : ty=""; break; // alternatively could do $i
        case Sorts::SRT_INTEGER : ty=":$int";break;
        case Sorts::SRT_RATIONAL : ty=":$rat";break;
        case Sorts::SRT_REAL    : ty=":$real";break;
        default: ASSERTION_VIOLATION_REP(t); // user-defined sorts require sort definitions
      }
    }
    varStr+=vstring("X")+Int::toString(var)+ty;
    first=false;
  }

  if (first) {
    //we didn't quantify any variable
    return inner;
  }

  if (innerParentheses) {
    return "( ! ["+varStr+"] : ("+inner+") )";
  }
  else {
    return "( ! ["+varStr+"] : "+inner+" )";
  }
}

/**
 * Return @b inner quentified over variables in @b vars
 *
 * It is caller's responsibility to ensure that variables in @b vars are unique.
 */
template<typename VarContainer>
vstring getQuantifiedStr(const VarContainer& vars, vstring inner, bool innerParentheses=true)
{
  CALL("getQuantifiedStr(VarContainer, vstring)");
  static DHMap<unsigned,unsigned> d;
  return getQuantifiedStr(vars,inner,d,innerParentheses);
}

/**
 * Return vstring containing quantified unit @b u.
 */
vstring getQuantifiedStr(Unit* u, List<unsigned>* nonQuantified=0)
{
  CALL("getQuantifiedStr(Unit*...)");

  Set<unsigned> vars;
  vstring res;
  DHMap<unsigned,unsigned> t_map;
  SortHelper::collectVariableSorts(u,t_map);
  if (u->isClause()) {
    Clause* cl=static_cast<Clause*>(u);
    unsigned clen=cl->length();
    for(unsigned i=0;i<clen;i++) {
      TermVarIterator vit( (*cl)[i] );
      while(vit.hasNext()) {
	unsigned var=vit.next();
	if (nonQuantified->member(var)) {
	  continue;
	}
	vars.insert(var);
      }
    }
    res=cl->nonPropToString();
  } else {
    Formula* formula=static_cast<FormulaUnit*>(u)->formula();
    FormulaVarIterator fvit( formula );
    while(fvit.hasNext()) {
      unsigned var=fvit.next();
      if (nonQuantified->member(var)) {
        continue;
      }
      vars.insert(var);
    }
    res=formula->toString();
  }

  return getQuantifiedStr(vars, res, t_map);
}

struct InferenceStore::ProofPrinter
{
  CLASS_NAME(InferenceStore::ProofPrinter);
  USE_ALLOCATOR(InferenceStore::ProofPrinter);
  
  ProofPrinter(ostream& out, InferenceStore* is)
  : _is(is), out(out)
  {
    CALL("InferenceStore::ProofPrinter::ProofPrinter");

    outputAxiomNames=env.options->outputAxiomNames();
  }

  void scheduleForPrinting(UnitSpec us)
  {
    CALL("InferenceStore::ProofPrinter::scheduleForPrinting");

    outKernel.push(us);
    handledKernel.insert(us);
  }


  virtual ~ProofPrinter() {}

  virtual void print()
  {
    CALL("InferenceStore::ProofPrinter::print");

    while(outKernel.isNonEmpty()) {
      UnitSpec cs=outKernel.pop();
      handleStep(cs);
    }
  }

protected:

  virtual bool hideProofStep(Inference::Rule rule)
  {
    return false;
  }

  void requestProofStep(UnitSpec prem)
  {
    if (!handledKernel.contains(prem)) {
      handledKernel.insert(prem);
      outKernel.push(prem);
    }
  }

  virtual void printStep(UnitSpec cs)
  {
    CALL("InferenceStore::ProofPrinter::printStep");
    Inference::Rule rule;
    UnitSpecIterator parents=_is->getParents(cs, rule);

    //cout << rule << " with parents ";
    //while(parents.hasNext()){
    //  cout << _is->getUnitIdStr(parents.next())<< " ";
    //}
    //cout<<"\n";
    //parents = _is->getParents(cs,rule);

    out << _is->getUnitIdStr(cs) << ". ";
    if (cs.isClause()) {
      Clause* cl=cs.cl();
      out << cl->nonPropToString();
      if (cl->splits() && !cl->splits()->isEmpty()) {
        out << " {" << cl->splits()->toString() << "}";
      }
      out << " ("<<cl->age()<<':'<<cl->weight()<<") ";
    }
    else {
      FormulaUnit* fu=static_cast<FormulaUnit*>(cs.unit());
      out << fu->formula()->toString() << ' ';
    }

    out <<"["<<Inference::ruleName(rule);

    if (outputAxiomNames && rule==Inference::INPUT) {
      ASS(!parents.hasNext()); //input clauses don't have parents
      vstring name;
      if (Parse::TPTP::findAxiomName(cs.unit(), name)) {
	out << " " << name;
      }
    }

    bool first=true;
    while(parents.hasNext()) {
      UnitSpec prem=parents.next();
      out << (first ? ' ' : ',');
      out << _is->getUnitIdStr(prem);
      first=false;
    }

    out << "]" << endl;

  }

  void handleStep(UnitSpec cs)
  {
    CALL("InferenceStore::ProofPrinter::handleStep");
    Inference::Rule rule;
    UnitSpecIterator parents=_is->getParents(cs, rule);

    while(parents.hasNext()) {
      UnitSpec prem=parents.next();
      ASS(prem!=cs);
      requestProofStep(prem);
    }

    if (!hideProofStep(rule)) {
      printStep(cs);
    }
  }



  Stack<UnitSpec> outKernel;
  Set<UnitSpec,UnitSpec> handledKernel; // use UnitSpec to provide its own hash and equals

  InferenceStore* _is;
  ostream& out;

  bool outputAxiomNames;
};

struct InferenceStore::TPTPProofPrinter
: public InferenceStore::ProofPrinter
{
  CLASS_NAME(InferenceStore::TPTPProofPrinter);
  USE_ALLOCATOR(InferenceStore::TPTPProofPrinter);
  
  TPTPProofPrinter(ostream& out, InferenceStore* is)
  : ProofPrinter(out, is) {}

protected:

  vstring getRole(Inference::Rule rule, Unit::InputType origin)
  {
    switch(rule) {
    case Inference::INPUT:
      if (origin==Unit::CONJECTURE) {
	return "conjecture";
      }
      else {
	return "axiom";
      }
    case Inference::NEGATED_CONJECTURE:
      return "negated_conjecture";
    default:
      return "plain";
    }
  }

  vstring tptpRuleName(Inference::Rule rule)
  {
    return StringUtils::replaceChar(Inference::ruleName(rule), ' ', '_');
  }

  vstring unitIdToTptp(vstring unitId)
  {
    return "f"+unitId;
  }

  vstring tptpUnitId(UnitSpec us)
  {
    return unitIdToTptp(_is->getUnitIdStr(us));
  }

  vstring tptpDefId(UnitSpec us)
  {
    return unitIdToTptp(Int::toString(us.unit()->number())+"_D");
  }

  vstring splitsToString(SplitSet* splits)
  {
    CALL("InferenceStore::TPTPProofPrinter::splitsToString");
    ASS_G(splits->size(),0);

    if (splits->size()==1) {
      return splitPrefix+Int::toString(splits->sval());
    }
    SplitSet::Iterator sit(*splits);
    vstring res("(");
    while(sit.hasNext()) {
      res+=splitPrefix+Int::toString(sit.next());
      if (sit.hasNext()) {
	res+=" | ";
      }
    }
    res+=")";
    return res;
  }

  vstring quoteAxiomName(vstring n)
  {
    CALL("InferenceStore::TPTPProofPrinter::quoteAxiomName");

    static vstring allowedFirst("0123456789abcdefghijklmnopqrstuvwxyz");
    const char* allowed="_ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789abcdefghijklmnopqrstuvwxyz";

    if (n.size()==0 || allowedFirst.find(n[0])==vstring::npos ||
	n.find_first_not_of(allowed)!=vstring::npos) {
      n='\''+n+'\'';
    }
    return n;
  }

  vstring getFofString(vstring id, vstring formula, vstring inference, Inference::Rule rule, Unit::InputType origin=Unit::AXIOM)
  {
    CALL("InferenceStore::TPTPProofPrinter::getFofString");

    return "fof("+id+","+getRole(rule,origin)+",("+"\n"
	+"  "+formula+"),\n"
	+"  "+inference+").";
  }

  vstring getFormulaString(UnitSpec us)
  {
    CALL("InferenceStore::TPTPProofPrinter::getFormulaString");

    vstring formulaStr;
    if (us.isClause()) {
      Clause* cl=us.cl();
      formulaStr=getQuantifiedStr(cl);
      if (cl->splits() && !cl->splits()->isEmpty()) {
	formulaStr+=" | "+splitsToString(cl->splits());
      }
    }
    else {
      FormulaUnit* fu=static_cast<FormulaUnit*>(us.unit());
      formulaStr=getQuantifiedStr(fu);
    }
    return formulaStr;
  }

  bool hasNewSymbols(Unit* u) {
    CALL("InferenceStore::TPTPProofPrinter::hasNewSymbols");
    bool res = _is->_introducedSymbols.find(u->number());
    ASS(!res || _is->_introducedSymbols.get(u->number()).isNonEmpty());
    return res;
  }
  vstring getNewSymbols(vstring origin, vstring symStr) {
    CALL("InferenceStore::TPTPProofPrinter::getNewSymbols(vstring,vstring)");
    return "new_symbols(" + origin + ",[" +symStr + "])";
  }
  /** It is an iterator over SymbolId */
  template<class It>
  vstring getNewSymbols(vstring origin, It symIt) {
    CALL("InferenceStore::TPTPProofPrinter::getNewSymbols(vstring,It)");

    vostringstream symsStr;
    while(symIt.hasNext()) {
      SymbolId sym = symIt.next();
      if (sym.first) {
	symsStr << env.signature->functionName(sym.second);
      }
      else {
	symsStr << env.signature->predicateName(sym.second);
      }
      if (symIt.hasNext()) {
	symsStr << ',';
      }
    }
    return getNewSymbols(origin, symsStr.str());
  }
  vstring getNewSymbols(vstring origin, Unit* u) {
    CALL("InferenceStore::TPTPProofPrinter::getNewSymbols(vstring,Unit*)");
    ASS(hasNewSymbols(u));

    SymbolStack& syms = _is->_introducedSymbols.get(u->number());
    return getNewSymbols(origin, SymbolStack::ConstIterator(syms));
  }

  void printStep(UnitSpec us)
  {
    CALL("InferenceStore::TPTPProofPrinter::printStep");

    Inference::Rule rule;
    UnitSpecIterator parents=_is->getParents(us, rule);

    switch(rule) {
    case Inference::SAT_SPLITTING_COMPONENT:
    case Inference::GENERAL_SPLITTING_COMPONENT:
      printGeneralSplittingComponent(us);
      return;
    case Inference::GENERAL_SPLITTING:
      printSplitting(us);
      return;
    case Inference::BDDZATION:
      printBddize(us);
      return;
    default: ;
    }


    //get vstring representing the formula

    vstring formulaStr=getFormulaString(us);

    //get inference vstring

    vstring inferenceStr;
    if (rule==Inference::INPUT) {
      vstring fileName;
      if (env.options->inputFile()=="") {
	fileName="unknown";
      }
      else {
	fileName="'"+env.options->inputFile()+"'";
      }
      vstring axiomName;
      if (!outputAxiomNames || !Parse::TPTP::findAxiomName(us.unit(), axiomName)) {
	axiomName="unknown";
      }
      inferenceStr="file("+fileName+","+quoteAxiomName(axiomName)+")";
    }
    else if (!parents.hasNext()) {
      vstring newSymbolInfo;
      if (hasNewSymbols(us.unit())) {
	newSymbolInfo = getNewSymbols("naming",us.unit());
      }
      inferenceStr="introduced("+tptpRuleName(rule)+",["+newSymbolInfo+"])";
    }
    else {
      ASS(parents.hasNext());
      vstring statusStr;
      if (rule==Inference::SKOLEMIZE) {
	statusStr="status(esa),"+getNewSymbols("skolem",us.unit());
      }

      inferenceStr="inference("+tptpRuleName(rule);

      inferenceStr+=",["+statusStr+"],[";
      bool first=true;
      while(parents.hasNext()) {
        UnitSpec prem=parents.next();
        if (!first) {
          inferenceStr+=',';
        }
        inferenceStr+=tptpUnitId(prem);
        first=false;
      }
      inferenceStr+="])";
    }

    out<<getFofString(tptpUnitId(us), formulaStr, inferenceStr, rule, us.unit()->inputType())<<endl;
  }

  void printSplitting(UnitSpec us)
  {
    CALL("InferenceStore::TPTPProofPrinter::printSplitting");
    ASS(us.isClause());

    Inference::Rule rule;
    UnitSpecIterator parents=_is->getParents(us, rule);
    ASS(rule==Inference::GENERAL_SPLITTING);


    vstring inferenceStr="inference("+tptpRuleName(rule)+",[],[";

    //here we rely on the fact that the base premise is always put as the first premise in
    //GeneralSplitting::apply 

    ALWAYS(parents.hasNext());
    UnitSpec base=parents.next();
    inferenceStr+=tptpUnitId(base);

    ASS(parents.hasNext()); //we always split off at least one component
    while(parents.hasNext()) {
      UnitSpec comp=parents.next();
      ASS(_is->_splittingNameLiterals.find(comp));
      inferenceStr+=","+tptpDefId(comp);
    }
    inferenceStr+="])";

    out<<getFofString(tptpUnitId(us), getFormulaString(us), inferenceStr, rule)<<endl;
  }

  void printGeneralSplittingComponent(UnitSpec us)
  {
    CALL("InferenceStore::TPTPProofPrinter::printGeneralSplittingComponent");
    ASS(us.isClause());

    Inference::Rule rule;
    UnitSpecIterator parents=_is->getParents(us, rule);
    ASS(!parents.hasNext());

    Literal* nameLit=_is->_splittingNameLiterals.get(us); //the name literal must always be stored

    vstring defId=tptpDefId(us);

    out<<getFofString(tptpUnitId(us), getFormulaString(us),
	"inference("+tptpRuleName(Inference::CLAUSIFY)+",[],["+defId+"])", Inference::CLAUSIFY)<<endl;


    List<unsigned>* nameVars=0;
    VariableIterator vit(nameLit);
    while(vit.hasNext()) {
      unsigned var=vit.next().var();
      ASS(!nameVars->member(var)); //each variable appears only once in the naming literal
      List<unsigned>::push(var,nameVars);
    }

    vstring compStr;
    List<unsigned>* compOnlyVars=0;
    Clause::Iterator lits(*us.cl());
    bool first=true;
    bool multiple=false;
    while(lits.hasNext()) {
      Literal* lit=lits.next();
      if (lit==nameLit) {
	continue;
      }
      if (first) {
	first=false;
      }
      else {
	multiple=true;
	compStr+=" | ";
      }
      compStr+=lit->toString();

      VariableIterator lvit(lit);
      while(lvit.hasNext()) {
        unsigned var=lvit.next().var();
        if (!nameVars->member(var) && !compOnlyVars->member(var)) {
          List<unsigned>::push(var,compOnlyVars);
        }
      }
    }
    ASS(!first);

    compStr=getQuantifiedStr(compOnlyVars, compStr, multiple);
    compOnlyVars->destroy();

    vstring defStr=compStr+" <=> "+Literal::complementaryLiteral(nameLit)->toString();
    defStr=getQuantifiedStr(nameVars, defStr);
    nameVars->destroy();

    SymbolId nameSymbol = SymbolId(false,nameLit->functor());
    vostringstream originStm;
    originStm << "introduced(" << tptpRuleName(rule)
	      << ",[" << getNewSymbols("naming",getSingletonIterator(nameSymbol))
	      << "])";

    out<<getFofString(defId, defStr, originStm.str(), rule)<<endl;
  }

  void printBddize(UnitSpec us)
  {
    CALL("InferenceStore::TPTPProofPrinter::printBddize");
    ASS(us.isClause());

    Inference::Rule rule;
    UnitSpecIterator parents=_is->getParents(us, rule);
    ASS_EQ(rule, Inference::BDDZATION);

    ALWAYS(parents.hasNext());
    UnitSpec parent=parents.next();
    ASS(!parents.hasNext());

    Clause* cl=us.cl();
    IntList* bddVars=_is->_bddizeVars.get(cl);
    ASS(bddVars);


    vstring premiseIds=tptpUnitId(parent);

    IntList::Iterator vit(bddVars);
    while(vit.hasNext()) {
      int var=vit.next();
      ASS_G(var,0);
      vstring defId="fbd"+Int::toString(var);
      premiseIds+=","+defId;
      if (!printedBddizeDefs.insert(var)) {
	continue;
      }

      vstring predName;
      vstring defStr= predName+" <=> "+bddPrefix+Int::toString(var);

      out<<getFofString(defId, defStr, "introduced("+tptpRuleName(rule)+",[])", rule)<<endl;
    }


    out<<getFofString(tptpUnitId(us), getFormulaString(us),
	"inference("+tptpRuleName(Inference::DEFINITION_FOLDING)+",[],["+premiseIds+"])", Inference::DEFINITION_FOLDING)<<endl;

  }

  void handleSplitting(SplittingRecord* sr)
  {
    CALL("InferenceStore::TPTPProofPrinter::handleSplitting");

    INVALID_OPERATION("The function InferenceStore::TPTPProofPrinter::handleSplitting should not be called");
  }

  DHSet<int> printedBddizeDefs;

  static const char* bddPrefix;
  static const char* splitPrefix;
};

const char* InferenceStore::TPTPProofPrinter::bddPrefix = "$bdd";
const char* InferenceStore::TPTPProofPrinter::splitPrefix = "$spl";

struct InferenceStore::ProofCheckPrinter
: public InferenceStore::ProofPrinter
{
  CLASS_NAME(InferenceStore::ProofCheckPrinter);
  USE_ALLOCATOR(InferenceStore::ProofCheckPrinter);
  
  ProofCheckPrinter(ostream& out, InferenceStore* is)
  : ProofPrinter(out, is) {}

protected:
  void printStep(UnitSpec cs)
  {
    CALL("InferenceStore::ProofCheckPrinter::printStep");
    Inference::Rule rule;
    UnitSpecIterator parents=_is->getParents(cs, rule);
 
    vstring kind = "fof";
    if(env.statistics->hasTypes){ kind="tff"; } 

    out << kind
        << "(r"<<_is->getUnitIdStr(cs)
    	<< ",conjecture, "
    	<< getQuantifiedStr(cs.unit()) 
    	<< " ). %"<<Inference::ruleName(rule)<<"\n";

    while(parents.hasNext()) {
      UnitSpec prem=parents.next();
      out << kind 
        << "(pr"<<_is->getUnitIdStr(prem)
  	<< ",axiom, "
  	<< getQuantifiedStr(prem.unit());
      out << " ).\n";
    }
    out << "%#\n";
  }


  bool hideProofStep(Inference::Rule rule)
  {
    switch(rule) {
    case Inference::INPUT:
    case Inference::CLAUSE_NAMING:
    case Inference::INEQUALITY_SPLITTING_NAME_INTRODUCTION:
    case Inference::INEQUALITY_SPLITTING:
    case Inference::SKOLEMIZE:
    case Inference::EQUALITY_PROXY_REPLACEMENT:
    case Inference::EQUALITY_PROXY_AXIOM1:
    case Inference::EQUALITY_PROXY_AXIOM2:
    case Inference::BDDZATION:
      return true;
    default:
      return false;
    }
  }

  void print()
  {
    ProofPrinter::print();
    out << "%#\n";
  }
};

InferenceStore::ProofPrinter* InferenceStore::createProofPrinter(ostream& out)
{
  CALL("InferenceStore::createProofPrinter");

  switch(env.options->proof()) {
  case Options::Proof::ON:
    return new ProofPrinter(out, this);
  case Options::Proof::PROOFCHECK:
    return new ProofCheckPrinter(out, this);
  case Options::Proof::TPTP:
    return new TPTPProofPrinter(out, this);
  case Options::Proof::OFF:
    return 0;
  }
  ASSERTION_VIOLATION;
  return 0;
}

/**
 * Output a proof of refutation to out
 *
 *
 */
void InferenceStore::outputProof(ostream& out, Unit* refutation)
{
  CALL("InferenceStore::outputProof(ostream&,Unit*)");

  ScopedPtr<ProofPrinter> pp(createProofPrinter(out));
  if (!pp) {
    return;
  }
  pp->scheduleForPrinting(UnitSpec(refutation));
  pp->print();
}

/**
 * Output a proof of units to out
 *
 */
void InferenceStore::outputProof(ostream& out, UnitList* units)
{
  CALL("InferenceStore::outputProof(ostream&,UnitList*)");

  ScopedPtr<ProofPrinter> pp(createProofPrinter(out));
  if (!pp) {
    return;
  }
  UnitList::Iterator uit(units);
  while(uit.hasNext()) {
    Unit* u = uit.next();
    pp->scheduleForPrinting(UnitSpec(u));
  }
  pp->print();
}

InferenceStore* InferenceStore::instance()
{
  static ScopedPtr<InferenceStore> inst(new InferenceStore());
  
  return inst.ptr();
}


}
