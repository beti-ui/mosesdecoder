/*
 * Rule.cpp
 *
 *  Created on: 20 Feb 2014
 *      Author: hieu
 */

#include "Rule.h"
#include "AlignedSentence.h"
#include "ConsistentPhrase.h"

Rule::Rule(const ConsistentPhrase &consistentPhrase, const AlignedSentence &alignedSentence)
:m_consistentPhrase(consistentPhrase)
,m_alignedSentence(alignedSentence)
,m_isValid(true)
,m_canRecurse(true)
{
	CreateSource();
}

Rule::Rule(const Rule &copy, const ConsistentPhrase &cp)
:m_consistentPhrase(copy.m_consistentPhrase)
,m_alignedSentence(copy.m_alignedSentence)
,m_isValid(true)
,m_canRecurse(true)
,m_nonterms(copy.m_nonterms)
{
	m_nonterms.push_back(&cp);
	CreateSource();
}

Rule::~Rule() {
	// TODO Auto-generated destructor stub
}

void Rule::CreateSource()
{
  const ConsistentPhrase *cp = NULL;
  size_t nonTermInd = 0;
  if (nonTermInd < m_nonterms.size()) {
	  cp = m_nonterms[nonTermInd];
  }

  for (int sourcePos = m_consistentPhrase.corners[0];
		  sourcePos <= m_consistentPhrase.corners[1];
		  ++sourcePos) {

	  const RuleSymbol *ruleSymbol;
	  if (cp && cp->corners[0] <= sourcePos && sourcePos <= cp->corners[1]) {
		  // replace words with non-term
		  ruleSymbol = cp;
		  sourcePos = cp->corners[1];
		  if (m_nonterms.size()) {
			  cp = m_nonterms[nonTermInd];
		  }

		  // move to next non-term
		  ++nonTermInd;
		  if (nonTermInd < m_nonterms.size()) {
			  cp = m_nonterms[nonTermInd];
		  }
		  else {
			  cp = NULL;
		  }
	  }
	  else {
		  ruleSymbol = m_alignedSentence.GetPhrase(Moses::Input)[sourcePos];
	  }

	  m_source.push_back(ruleSymbol);
  }
}

int Rule::GetNextSourcePosForNonTerm() const
{
	if (m_nonterms.empty()) {
		// no non-terms so far. Can start next non-term on left corner
		return m_consistentPhrase.corners[0];
	}
	else {
		// next non-term can start just left of previous
		const ConsistentPhrase &cp = *m_nonterms.back();
		int nextPos = cp.corners[1] + 1;
		if (nextPos >= m_alignedSentence.GetPhrase(Moses::Input).size()) {
			return -1;
		}
		else {
			return nextPos;
		}
	}
}

void Rule::Debug(std::ostream &out) const
{
  // source
  for (size_t i =  0; i < m_source.size(); ++i) {
	  const RuleSymbol &symbol = *m_source[i];
	  symbol.Debug(out);
	  out << " ";
  }

  // target
  out << "||| ";
  for (size_t i =  0; i < m_target.size(); ++i) {
	  const RuleSymbol &symbol = *m_target[i];
	  symbol.Debug(out);
	  out << " ";
  }

  // overall range
  out << "||| ";
  m_consistentPhrase.Debug(out);

}
