/**
 * @file AIGInliner.cpp
 * Implements class AIGInliner.
 */

#include <climits>

#include "Lib/Environment.hpp"
#include "Lib/Int.hpp"
#include "Lib/MapToLIFO.hpp"
#include "Lib/SharedSet.hpp"
#include "Lib/Stack.hpp"

#include "Kernel/Formula.hpp"
#include "Kernel/FormulaUnit.hpp"
#include "Kernel/Inference.hpp"
#include "Kernel/LiteralComparators.hpp"
#include "Kernel/Matcher.hpp"
#include "Kernel/Problem.hpp"
#include "Kernel/Signature.hpp"
#include "Kernel/SortHelper.hpp"
#include "Kernel/Term.hpp"

#include "Indexing/LiteralSubstitutionTree.hpp"

#include "AIGSubst.hpp"
#include "Flattening.hpp"
#include "Options.hpp"
#include "PDUtils.hpp"
#include "SimplifyFalseTrue.hpp"

#include "AIGInliner.hpp"

namespace Shell
{

AIGInliner::EquivInfo::EquivInfo(Literal* lhs, Formula* rhs, FormulaUnit* unit)
: lhs(lhs), rhs(rhs), unit(unit)
{
  CALL("AIGInliner::EquivInfo::EquivInfo");

  posLhs = Literal::positiveLiteral(lhs);
  active = true;
}

/**
 * Compare literals for the purpose of @c tryGetEquiv()
 */
bool AIGInliner::EquivInfo::litIsLess(Literal* l1, Literal* l2)
{
  CALL("AIGInliner::EquivInfo::litIsLess");
  bool l1Protected = env.signature->getPredicate(l1->functor())->protectedSymbol();
  bool l2Protected = env.signature->getPredicate(l2->functor())->protectedSymbol();
  if(l1Protected!=l2Protected) {
    return l1Protected;
  }
  if(l1->functor()!=l2->functor()) {
    return l1->functor()<l2->functor();
  }
  return LiteralComparators::NormalizedLinearComparatorByWeight<true>().compare(l1, l2)==LESS;
}

/**
 * Attempt to get an equivalence with atom on the lhs from @c fu,
 * if unsuccessful, return 0.
 *
 * If both sides of an equivalence can become lhs, we pick the one with
 * larger predicate number. If equivalence is between atoms of the same
 * predicate, we use some other deterministic ordering to pick.
 */
AIGInliner::EquivInfo* AIGInliner::EquivInfo::tryGetEquiv(FormulaUnit* fu)
{
  CALL("AIGInliner::EquivInfo::tryGetEquiv");

  Formula* f = fu->formula();
  Formula::VarList* qvars = 0;
  if(f->connective()==FORALL) {
    qvars = f->vars();
    f = f->qarg();
  }

  if(f->connective()==LITERAL) {
    Literal* lhs = f->literal();
    if(env.signature->getPredicate(lhs->functor())->protectedSymbol()) {
      return 0;
    }
    return new EquivInfo(lhs, Formula::trueFormula(), fu);
  }
  if(f->connective()!=IFF) {
    return 0;
  }
  Formula* c1 = f->left();
  Formula* c2 = f->right();
  if(c1->connective()!=LITERAL) {
    swap(c1,c2);
  }
  else if(c2->connective()==LITERAL) {
    Literal* l1 = c1->literal();
    Literal* l2 = c2->literal();
    bool l1DH = PDUtils::isDefinitionHead(l1);
    bool l2DH = PDUtils::isDefinitionHead(l2);
    if(l1DH==l2DH) {
      if(l1->functor()==l2->functor()) {
	if(l1==l2) { return 0; }
	if(l1==Literal::complementaryLiteral(l2)) { return 0; }
      }
      bool less = litIsLess(l1, l2);
      if(less) {
	swap(c1, c2);
      }
    }
    else if(l2DH) {
      swap(c1, c2);
    }
  }

  if(c1->connective()!=LITERAL) {
    return 0;
  }
  Literal* lhs = c1->literal();
  if(env.signature->getPredicate(lhs->functor())->protectedSymbol()) {
    return 0;
  }
  Formula* rhs = c2;

  Formula::VarList* lhsVars = c1->freeVariables();

  static Stack<unsigned> qVarStack;
  static Stack<unsigned> lhsVarStack;
  qVarStack.reset();
  qVarStack.loadFromIterator(Formula::VarList::Iterator(qvars));
  std::sort(qVarStack.begin(), qVarStack.end());
  lhsVarStack.reset();
  lhsVarStack.loadFromIterator(Formula::VarList::DestructiveIterator(lhsVars));
  std::sort(lhsVarStack.begin(), lhsVarStack.end());

  if(qVarStack!=lhsVarStack) {
    return 0;
  }

  return new EquivInfo(lhs, rhs, fu);
}

AIGInliner::AIGInliner()
 : _aig(_fsh.aig()), _atr(_aig), _acompr(_aig)
{
  CALL("AIGInliner::AIGInliner");

  _onlySingleAtomPreds = false;

  _lhsIdx = new LiteralSubstitutionTree();
}

AIGInliner::~AIGInliner()
{
  CALL("AIGInliner::~AIGInliner");

  delete _lhsIdx;

  while(_eqInfos.isNonEmpty()) {
    delete _eqInfos.pop();
  }
}

bool AIGInliner::addInfo(EquivInfo* inf)
{
  CALL("AIGInliner::addInfo");

  if(_lhsIdx->getUnificationCount(inf->posLhs, false)!=0) {
    //TODO: one day try to do something smarter
    delete inf;
    return false;
  }

  AIGRef rhsAig = _fsh.apply(inf->rhs).second;
  if(inf->lhs->isNegative()) {
    rhsAig = rhsAig.neg();
  }
  rhsAig = _acompr.compress(rhsAig);
  inf->activeAIGRhs = rhsAig;

  //now we know we have a definition we'll use, so we insert it into structures

  _eqInfos.push(inf);

  Literal* idxLhs = inf->posLhs;
  _lhsIdx->insert(idxLhs, 0);
  _defs.insert(idxLhs, inf);
  _unit2def.insert(inf->unit, inf);

  LOG("pp_aiginl_equiv","equivalence for inlining: "<<(*inf->posLhs)<<" <=> "<<rhsAig);
  return true;
}

void AIGInliner::collectDefinitions(UnitList* units, Stack<AIGRef>& relevantAigs)
{
  CALL("AIGInliner::collectDefinitions");

  UnitList::Iterator uit(units);
  while(uit.hasNext()) {
    Unit* u = uit.next();
    if(u->isClause()) {
      relevantAigs.push(_fsh.getAIG(static_cast<Clause*>(u)));
      continue;
    }
    FormulaUnit* fu = static_cast<FormulaUnit*>(u);
    EquivInfo* inf = EquivInfo::tryGetEquiv(fu);
    Formula* relevantForm;
    if(inf && addInfo(inf)) {
      relevantForm = inf->rhs;
    }
    else {
      relevantForm = fu->formula();
    }
    relevantAigs.push(_fsh.apply(relevantForm).second);
  }
#if VDEBUG
  _relevantAigs.loadFromIterator(Stack<AIGRef>::Iterator(relevantAigs));
#endif
}

void AIGInliner::updateModifiedProblem(Problem& prb)
{
  CALL("AIGInliner::updateModifiedProblem");

  prb.invalidateByRemoval();
}

/**
 * Try expanding atom using definitions
 *
 * @param atom Atom to be expanded. Must be an atom aig with positive polarity.
 */
bool AIGInliner::tryExpandAtom(AIGRef atom, AIGRef& res)
{
  CALL("AIGInliner::tryExpandAtom");
  ASS(atom.isAtom());
  ASS(atom.polarity());

  Literal* lit = atom.getPositiveAtom();
  SLQueryResultIterator defIt = _lhsIdx->getGeneralizations(lit, false, false);

  if(!defIt.hasNext()) {
    return false;
  }

  SLQueryResult idxRes = defIt.next();

  if(defIt.hasNext()) {
    defIt = _lhsIdx->getGeneralizations(lit, false, false);
    LOGV("bug", *lit);
    while(defIt.hasNext()) {
      LOGV("bug", *defIt.next().literal);
    }
  }
  ASS(!defIt.hasNext()); //we made sure there is always only one way to inline

  Literal* defLhs = idxRes.literal;
  AIGRef defRhs = _defs.get(defLhs)->activeAIGRhs;

  if(lit==defLhs) {
    res = defRhs;
    return true;
  }

  typedef DHMap<unsigned,TermList> BindingMap;
  static BindingMap binding;
  binding.reset();
  MatchingUtils::MapRefBinder<BindingMap> binder(binding);

  ALWAYS(MatchingUtils::match(defLhs, lit, false, binder));

  SubstHelper::MapApplicator<BindingMap> applicator(&binding);

  res = AIGSubst(_aig).apply(applicator, defRhs);
  LOG("pp_aiginl_instance","instantiated AIG definition"<<endl<<
      "  src: "<<atom<<endl<<
      "  lhs: "<<(*defLhs)<<endl<<
      "  rhs: "<<defRhs<<endl<<
      "  tgt: "<<res<<endl
      );
  return true;
}

/**
 * Units must not contain predicate eqivalences
 */
void AIGInliner::scan(UnitList* units)
{
  CALL("AIGInliner::scan");

//  scanOccurrences(units);

  FormulaStack lhsForms;
  FormulaStack rhsForms;
  Stack<FormulaUnit*> defUnits;

  DHMap<AIGRef,AIGRef> atomMap;

  Stack<AIGRef> relevantAigs;

  collectDefinitions(units, relevantAigs);

  AIGInsideOutPosIterator ait;
  ait.reset();
  ait.addManyToTraversal(Stack<AIGRef>::Iterator(relevantAigs));

  while(ait.hasNext()) {
    AIGRef a = ait.next();
    if(!a.isAtom()) {
      continue;
    }
    ASS(a.polarity());
    AIGRef tgt;
    if(!tryExpandAtom(a, tgt)) {
      continue;
    }
    ALWAYS(atomMap.insert(a, tgt));
    ait.addToTraversal(tgt);
  }

  _inlMap.loadFromMap(atomMap);

//  TRACE("bug",
//      AIGTransformer::RefMap::Iterator mit(_inlMap);
//      while(mit.hasNext()) {
//	AIGRef src, tgt;
//	mit.next(src, tgt);
//	tout << "-inl---" << endl;
//	tout << "  src: " << src << endl;
//	tout << "  tgt: " << tgt << endl;
//      }
//  );


  _atr.saturateMap(_inlMap);

//  TRACE("bug",
//      AIGTransformer::RefMap::Iterator mit(_inlMap);
//      while(mit.hasNext()) {
//	AIGRef src, tgt;
//	mit.next(src, tgt);
//	tout << "-inl-sat--" << endl;
//	tout << "  src: " << src << endl;
//	tout << "  tgt: " << tgt << endl;
//	tout << "  srcI: " << src.toInternalString() << endl;
//	tout << "  tgtI: " << tgt.toInternalString() << endl;
//      }
//  );



  ait.reset();

  Stack<AIGRef>::Iterator baseAigIt(relevantAigs);
  while(baseAigIt.hasNext()) {
    AIGRef baseAig = baseAigIt.next();
    AIGRef inlAig = AIGTransformer::lev0Deref(baseAig, _inlMap);
//    LOGV("bug",baseAig);
//    LOGV("bug",inlAig);
//    LOGV("bug",inlAig.toInternalString());
    ait.addToTraversal(inlAig);
  }

  _acompr.populateBDDCompressingMap(ait, _simplMap);

//  LOGV("bug", _simplMap.size());
//  TRACE("bug",
//      AIGTransformer::RefMap::Iterator mit(_simplMap);
//      while(mit.hasNext()) {
//	AIGRef src, tgt;
//	mit.next(src, tgt);
//	tout << "----" << endl;
//	tout << "  src: " << src << endl;
//	tout << "  tgt: " << tgt << endl;
//	tout << "  srcI: " << src.toInternalString() << endl;
//	tout << "  tgtI: " << tgt.toInternalString() << endl;
//      }
//  );
}

AIGRef AIGInliner::apply(AIGRef a)
{
  CALL("AIGInliner::apply(AIGRef)");

  AIGRef inl = AIGTransformer::lev0Deref(a, _inlMap);
  AIGRef res = AIGTransformer::lev0Deref(inl, _simplMap);
  COND_LOG("pp_aiginl_aig", a!=res, "inlining aig transformation:"<<endl
      <<"  src: "<<a<<endl
      <<"  inl: "<<inl<<endl
      <<"  tgt: "<<res<<endl
      <<"  tSm: "<<_acompr.compress(res)<<endl
      <<"  srcI: "<<a.toInternalString()<<endl
      <<"  inlI: "<<inl.toInternalString()<<endl
      <<"  tgtI: "<<res.toInternalString()
  );
  COND_LOG("bug", res!=_acompr.compress(res),
      "missed simplification in aig inlining:"<<endl
            <<"  src: "<<a<<endl
            <<"  inl: "<<inl<<endl
            <<"  tgt: "<<res<<endl
            <<"  tSm: "<<_acompr.compress(res)<<endl
            <<"  srcI: "<<a.toInternalString()<<endl
            <<"  inlI: "<<inl.toInternalString()<<endl
            <<"  tgtI: "<<res.toInternalString()
      );

  ASS_REP(_relevantAigs.contains(a), a);

  return res;
}

Formula* AIGInliner::apply(Formula* f)
{
  CALL("AIGInliner::apply(Formula*)");

  AIGRef a = _fsh.apply(f).second;
  AIGRef tgt = apply(a);
  if(tgt==a) {
    return f;
  }
  return _fsh.aigToFormula(tgt);
}

bool AIGInliner::apply(FormulaUnit* unit, Unit*& res)
{
  CALL("AIGInliner::apply(FormulaUnit*,FormulaUnit*&)");
  LOG_UNIT("pp_aiginl_unit_args", unit);

  Formula* f;

  EquivInfo* einf;
  if(_unit2def.find(unit, einf)) {
    Formula* newRhs = apply(einf->rhs);
    if(newRhs==einf->rhs) {
      return false;
    }
    Formula* lhs = new AtomicFormula(einf->lhs);
    Formula::VarList* qvars = lhs->freeVariables();
    if(newRhs->connective()==TRUE) {
      f = lhs;
    }
    else if(newRhs->connective()==FALSE) {
      f = new AtomicFormula(Literal::complementaryLiteral(lhs->literal()));
    }
    else {
      f = new BinaryFormula(IFF, lhs, newRhs);
    }
    if(qvars) {
      f = new QuantifiedFormula(FORALL, qvars, f);
    }
  }
  else {
    f = apply(unit->formula());
    if(f->connective()==TRUE) {
      res = 0;
      return true;
    }
    if(f==unit->formula()) {
      return false;
    }
  }

  //TODO: collect proper inferences
  Inference* inf = new Inference1(Inference::PREDICATE_DEFINITION_UNFOLDING, unit);
  FormulaUnit* res0 = new FormulaUnit(f, inf, unit->inputType());

  res = Flattening::flatten(res0);

  LOG_SIMPL("pp_aiginl_unit", unit, res);

  return true;
}


////////////////////////////
// AIGDefinitionIntroducer
//

AIGDefinitionIntroducer::AIGDefinitionIntroducer()
{
  CALL("AIGDefinitionIntroducer::AIGDefinitionIntroducer");

  _namingRefCntThreshold = 4; //TODO: add option
  _mergeEquivDefs = false; //not implemented yet
}

void AIGDefinitionIntroducer::scanDefinition(FormulaUnit* def)
{
  CALL("AIGDefinitionIntroducer::scanDefinition");

  Literal* lhs;
  Formula* rhs;
  PDUtils::splitDefinition(def, lhs, rhs);

  AIGRef rhsAig = _fsh.apply(rhs).second;
  AIGRef lhsAig = _fsh.apply(lhs);

  if(!rhsAig.polarity()) {
    rhsAig = rhsAig.neg();
    lhsAig = lhsAig.neg();
  }

  if(!_defs.insert(rhsAig, lhsAig)) {
    //rhs is already defined
    AIGRef oldDefTgt;
    ALWAYS(_defs.find(rhsAig, oldDefTgt));
    if(_mergeEquivDefs) {
//      _equivs.insert(lhs, oldDefTgt);
      NOT_IMPLEMENTED;
    }
    return;
  }
  ALWAYS(_defUnits.insert(rhsAig,def));

  _toplevelAIGs.push(make_pair(rhsAig,def));
}

void AIGDefinitionIntroducer::collectTopLevelAIGsAndDefs(UnitList* units)
{
  CALL("AIGDefinitionIntroducer::collectTopLevelAIGsAndDefs");

  UnitList::Iterator uit(units);
  while(uit.hasNext()) {
    Unit* u = uit.next();
    if(u->isClause()) {
      continue;
    }
    FormulaUnit* fu = static_cast<FormulaUnit*>(u);

    if(PDUtils::hasDefinitionShape(fu)) {
      scanDefinition(fu);
      continue;
    }

    Formula* form = fu->formula();
    AIGRef formAig = _fsh.apply(form).second;
    _toplevelAIGs.push(make_pair(formAig,fu));
  }
}

void AIGDefinitionIntroducer::doFirstRefAIGPass()
{
  CALL("AIGDefinitionIntroducer::doFirstRefAIGPass");

  ASS(_refAIGInfos.isEmpty());
  size_t aigCnt = _refAIGs.size();
  for(size_t i=0; i<aigCnt; ++i) {
    AIGRef r = _refAIGs[i];
    LOG("pp_aigdef_used_aigs","used at "<<i<<": "<<r.toInternalString());
    ASS(r.polarity());
    _aigIndexes.insert(r, i);

    _refAIGInfos.push(NodeInfo());
    NodeInfo& ni = _refAIGInfos.top();

    ni._directRefCnt = 0;
    ni._hasName = _defs.find(r, ni._name);

    ni._hasQuant[0] = false;
    ni._hasQuant[1] = r.isQuantifier();

    unsigned parCnt = r.parentCnt();
    for(unsigned pi = 0; pi<parCnt; ++pi) {
      AIGRef par = r.parent(pi);
      bool neg = !par.polarity();
      AIGRef posPar = neg ? par.neg() : par;
      unsigned parStackIdx = _aigIndexes.get(posPar);
      NodeInfo& pni = _refAIGInfos[parStackIdx];

      pni._directRefCnt++;
      ni._hasQuant[0^neg] |= pni._hasQuant[0];
      ni._hasQuant[1^neg] |= pni._hasQuant[1];
    }

    //initialize values for the second pass
    ni._inPol[0] = ni._inPol[1] = false;
    ni._inQuant[0] = ni._inQuant[1] = false;
    ni._formRefCnt = 0;

  }
}

/**
 * Return the aig at given index before names are introduced but after
 * the AIG compression.
 *
 * Can be called after the call to @c doFirstRefAIGPass returns.
 */
AIGRef AIGDefinitionIntroducer::getPreNamingAig(unsigned aigStackIdx)
{
  CALL("AIGDefinitionIntroducer::getPreNamingAig");

  return _refAIGs[aigStackIdx];
}

bool AIGDefinitionIntroducer::shouldIntroduceName(unsigned aigStackIdx)
{
  CALL("AIGDefinitionIntroducer::shouldIntroduceName");

  AIGRef a = getPreNamingAig(aigStackIdx);
  if(a.isPropConst() || a.isAtom()) {
    return false;
  }
  NodeInfo& ni = _refAIGInfos[aigStackIdx];

  if(!_namingRefCntThreshold || ni._formRefCnt<_namingRefCntThreshold) {
    return false;
  }
  if(_defs.find(a)) {
    return false;
  }
  return true;
}

Literal* AIGDefinitionIntroducer::getNameLiteral(unsigned aigStackIdx)
{
  CALL("AIGDefinitionIntroducer::getNameLiteral");

  AIGRef a = getPreNamingAig(aigStackIdx);
  Formula* rhs = _fsh.aigToFormula(a);
  Formula::VarList* freeVars = rhs->freeVariables(); //careful!! this traverses the formula as tree, not as DAG, so may take exponential time!

  static DHMap<unsigned,unsigned> varSorts;
  varSorts.reset();
  SortHelper::collectVariableSorts(rhs, varSorts); //careful!! this traverses the formula as tree, not as DAG, so may take exponential time!

  static TermStack args;
  args.reset();
  static Stack<unsigned> argSorts;
  argSorts.reset();
  while(freeVars) {
    unsigned var = Formula::VarList::pop(freeVars);
    args.push(TermList(var, false));
    argSorts.push(varSorts.get(var));
  }

  unsigned arity = args.size();
  unsigned pred = env.signature->addFreshPredicate(arity, "sP","aig_name");
  env.signature->getPredicate(pred)->setType(PredicateType::makeType(arity, argSorts.begin(), Sorts::SRT_BOOL));

  Literal* res = Literal::create(pred, arity, true, false, args.begin());
  return res;
}

void AIGDefinitionIntroducer::introduceName(unsigned aigStackIdx)
{
  CALL("AIGDefinitionIntroducer::introduceName");

  AIGRef a = getPreNamingAig(aigStackIdx);
  NodeInfo& ni = _refAIGInfos[aigStackIdx];
  ASS(!ni._hasName);
  ASS(!_defs.find(a.getPositive()));

  ni._formRefCnt = 1;
  Literal* nameLit = getNameLiteral(aigStackIdx);
  ni._hasName = true;
  ni._name = _fsh.apply(nameLit);
  if(a.polarity()) {
    ALWAYS(_defs.insert(a, ni._name));
  }
  else {
    ALWAYS(_defs.insert(a.neg(), ni._name.neg()));
  }

  Formula* lhs = new AtomicFormula(nameLit);
  Formula* rhs = _fsh.aigToFormula(a);
  //TODO: weaken definition based on the way subforula occurrs
  Formula* equiv = new BinaryFormula(IFF, lhs, rhs);
  Formula::VarList* vars = equiv->freeVariables(); //careful!! this traverses the formula as tree, not as DAG, so may take exponential time!
  if(vars) {
    equiv = new QuantifiedFormula(FORALL, vars, equiv);
  }
  FormulaUnit* def = new FormulaUnit(equiv, new Inference(Inference::PREDICATE_DEFINITION), Unit::AXIOM);
  ALWAYS(_defUnits.insert(a, def));
  _newDefs.push(def);

  LOG_UNIT("pp_aigdef_intro", def);
}

void AIGDefinitionIntroducer::doSecondRefAIGPass()
{
  CALL("AIGDefinitionIntroducer::doSecondRefAIGPass");

  TopLevelStack::Iterator tlit(_toplevelAIGs);
  while(tlit.hasNext()) {
    AIGRef a = tlit.next().first;
    bool neg = !a.polarity();
    AIGRef aPos = neg ? a.neg() : a;
    unsigned stackIdx = _aigIndexes.get(aPos);
    NodeInfo& ni = _refAIGInfos[stackIdx];
    ni._formRefCnt++;
    ni._inPol[a.polarity()] = true;
  }

  size_t aigCnt = _refAIGs.size();
  for(size_t i=aigCnt; i>0;) {
    i--;

    AIGRef r = _refAIGs[i];
    NodeInfo& ni = _refAIGInfos[i];

    if(ni._hasName) {
      ni._formRefCnt = 1;
    }

    if(shouldIntroduceName(i)) {
      introduceName(i);
    }

    unsigned parCnt = r.parentCnt();
    for(unsigned pi = 0; pi<parCnt; ++pi) {
      AIGRef par = r.parent(pi);
      bool neg = !par.polarity();
      AIGRef posPar = neg ? par.neg() : par;
      unsigned parStackIdx = _aigIndexes.get(posPar);
      NodeInfo& pni = _refAIGInfos[parStackIdx];

      if(r.isQuantifier()) {
	pni._inQuant[!neg] = true;
      }

      pni._inQuant[0^neg] |= ni._inQuant[0];
      pni._inQuant[1^neg] |= ni._inQuant[1];

      pni._inPol[0^neg] |= ni._inPol[0];
      pni._inPol[1^neg] |= ni._inPol[1];

      pni._formRefCnt += ni._formRefCnt;
    }
  }
}

void AIGDefinitionIntroducer::scan(UnitList* units)
{
  CALL("AIGDefinitionIntroducer::scan");

  collectTopLevelAIGsAndDefs(units);

  _refAIGs.loadFromIterator( getMappingIterator(TopLevelStack::Iterator(_toplevelAIGs), GetFirstOfPair<TopLevelPair>()) );
  _fsh.aigTransf().makeOrderedAIGGraphStack(_refAIGs);

  doFirstRefAIGPass();
  doSecondRefAIGPass();
  _fsh.aigTransf().saturateMap(_defs);
}
//
//struct AIGDefinitionIntroducer::DefIntroRewriter : public FormulaTransformer
//{
//  DefIntroRewriter(AIGDefinitionIntroducer& parent) : _parent(parent), _fsh(_parent._fsh) {}
//
//  virtual bool preApply(Formula*& f)
//  {
//    CALL("AIGDefinitionIntroducer::DefIntroRewriter::postApply");
//
//    if(f->connective()==LITERAL) { return true; }
//
//    LOG("pp_aigdef_apply_subform", "checking: "<<(*f));
//    AIGFormulaSharer::ARes ares = _fsh.apply(f);
////    f = ares.first;
//    AIGRef a = ares.second;
//    LOG("pp_aigdef_apply_subform", "  aig: "<<a.toInternalString());
//
//    bool neg = !a.polarity();
//    if(neg) {
//      a = a.neg();
//    }
//    unsigned refStackIdx = _parent._aigIndexes.get(a);
//    NodeInfo& ni = _parent._refAIGInfos[refStackIdx];
//    if(ni._hasName) {
//      //we replace the formula by definition
//      LOG("pp_aigdef_apply_subform", "found name: "<<ni._name);
//      AIGRef nameAig = ni._name;
//      if(neg) {
//	nameAig = nameAig.neg();
//      }
//      f = _fsh.aigToFormula(nameAig);
//      return false;
//    }
//    return true;
//  }
//private:
//  AIGDefinitionIntroducer& _parent;
//  AIGFormulaSharer& _fsh;
//};

bool AIGDefinitionIntroducer::apply(FormulaUnit* unit, Unit*& res)
{
  CALL("AIGDefinitionIntroducer::apply(FormulaUnit*,Unit*&)");

//  DefIntroRewriter rwr(*this);
  Formula* f0 = unit->formula();
//  Formula* f = rwr.transform(f0);

  AIGRef f0Aig = _fsh.apply(f0).second;
  AIGRef resAig;
  bool neg = !f0Aig.polarity();
  if(neg) {
    f0Aig = f0Aig.neg();
  }
  if(!_defs.find(f0Aig, resAig)) {
    return false;
  }
  ASS_NEQ(f0Aig, resAig);
  if(neg) {
    resAig = resAig.neg();
  }
  Formula* f = _fsh.aigToFormula(resAig);

  if(f->connective()==TRUE) {
    res = 0;
    LOG("pp_aigdef_apply","orig: " << (*unit) << endl << "  simplified to tautology");
    return true;
  }
//  if(f==f0) {
//    return false;
//  }
  //TODO add proper inferences
  res = new FormulaUnit(f, new Inference1(Inference::DEFINITION_FOLDING,unit), unit->inputType());
  LOG("pp_aigdef_apply","orig: " << (*unit) << endl << "intr: " << (*res));
  ASS(!res->isClause());
  res = SimplifyFalseTrue().simplify(static_cast<FormulaUnit*>(res));
  res = Flattening::flatten(static_cast<FormulaUnit*>(res));
  return true;
}

UnitList* AIGDefinitionIntroducer::getIntroducedFormulas()
{
  CALL("AIGDefinitionIntroducer::getIntroducedFormulas");
  LOG("pp_aigdef_apply","processing definitions");

  UnitList* res = 0;

  Stack<FormulaUnit*>::Iterator uit(_newDefs);
  while(uit.hasNext()) {
    FormulaUnit* def0 = uit.next();
    Unit* def;
    if(!apply(def0, def)) {
      def = def0;
    }
    UnitList::push(def, res);
  }
  return res;
}

}

