// $Id$

/***********************************************************************
Moses - factored phrase-based language decoder
Copyright (C) 2006 University of Edinburgh

This library is free software; you can redistribute it and/or
modify it under the terms of the GNU Lesser General Public
License as published by the Free Software Foundation; either
version 2.1 of the License, or (at your option) any later version.

This library is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public
License along with this library; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
***********************************************************************/

#include "TrellisPath.h"
#include "TrellisPathCollection.h"
#include "StaticData.h"

using namespace std;

TrellisPath::TrellisPath(const Hypothesis *hypo)
:	m_prevEdgeChanged(NOT_FOUND)
{
	m_scoreBreakdown					= hypo->GetScoreBreakdown();
	m_totalScore = hypo->GetTotalScore();

	// enumerate path using prevHypo
	while (hypo != NULL)
	{
		m_path.push_back(hypo);
		hypo = hypo->GetPrevHypo();
	}
}

TrellisPath::TrellisPath(const TrellisPath &copy, size_t edgeIndex, const Hypothesis *arc)
:m_prevEdgeChanged(edgeIndex)
{
	for (size_t currEdge = 0 ; currEdge < edgeIndex ; currEdge++)
	{ // copy path from parent
		m_path.push_back(copy.m_path[currEdge]);
	}
	
	// 1 deviation
	m_path.push_back(arc);

	// rest of path comes from following best path backwards
	const Hypothesis *prevHypo = arc->GetPrevHypo();
	while (prevHypo != NULL)
	{
		m_path.push_back(prevHypo);
		prevHypo = prevHypo->GetPrevHypo();
	}

	// Calc score
	m_totalScore		= m_path[0]->GetWinningHypo()->GetTotalScore();
	m_scoreBreakdown= m_path[0]->GetWinningHypo()->GetScoreBreakdown();

	size_t sizePath = m_path.size();
	for (size_t pos = 0 ; pos < sizePath ; pos++)
	{
		const Hypothesis *hypo = m_path[pos];
		const Hypothesis *winningHypo = hypo->GetWinningHypo();
		if (hypo != winningHypo)
		{
			m_totalScore = m_totalScore - winningHypo->GetTotalScore() + hypo->GetTotalScore();
			m_scoreBreakdown.MinusEquals(winningHypo->GetScoreBreakdown());
			m_scoreBreakdown.PlusEquals(hypo->GetScoreBreakdown());
		}
	}
}

void TrellisPath::CreateDeviantPaths(TrellisPathCollection &pathColl) const
{
	const size_t sizePath = m_path.size();

	if (m_prevEdgeChanged == NOT_FOUND)
	{ // initial enumration from a pure hypo
		for (size_t currEdge = 0 ; currEdge < sizePath ; currEdge++)
		{
			const Hypothesis	*hypo		= static_cast<const Hypothesis*>(m_path[currEdge]);
			const ArcList *pAL = hypo->GetArcList();
      if (!pAL) continue;
			const ArcList &arcList = *pAL;

			// every possible Arc to replace this edge
			ArcList::const_iterator iterArc;
			for (iterArc = arcList.begin() ; iterArc != arcList.end() ; ++iterArc)
			{
				const Hypothesis *arc = *iterArc;
				TrellisPath *deviantPath = new TrellisPath(*this, currEdge, arc);
				pathColl.Add(deviantPath);
			}
		}
	}
	else
	{	// wiggle 1 of the edges only
		for (size_t currEdge = m_prevEdgeChanged + 1 ; currEdge < sizePath ; currEdge++)
		{
			const ArcList *pAL = m_path[currEdge]->GetArcList();
    	if (!pAL) continue;
			const ArcList &arcList = *pAL;
			ArcList::const_iterator iterArc;

			for (iterArc = arcList.begin() ; iterArc != arcList.end() ; ++iterArc)
			{	// copy this Path & change 1 edge
				const Hypothesis *arcReplace = *iterArc;

				TrellisPath *deviantPath = new TrellisPath(*this, currEdge, arcReplace);
				pathColl.Add(deviantPath);						
			} // for (iterArc...
		} // for (currEdge = 0 ...
	}
}

Phrase TrellisPath::GetTargetPhrase() const
{
	Phrase targetPhrase(Output);

	int numHypo = (int) m_path.size();
	for (int node = numHypo - 2 ; node >= 0 ; --node)
	{ // don't do the empty hypo - waste of time and decode step id is invalid
		const Hypothesis &hypo = *m_path[node];
		const Phrase &currTargetPhrase = hypo.GetCurrTargetPhrase();

		targetPhrase.Append(currTargetPhrase);
	}

	return targetPhrase;
}

Phrase TrellisPath::GetSurfacePhrase() const
{
	const std::vector<FactorType> &outputFactor = StaticData::Instance().GetOutputFactorOrder();
	Phrase targetPhrase = GetTargetPhrase()
				,ret(Output);

	for (size_t pos = 0 ; pos < targetPhrase.GetSize() ; ++pos)
	{
		Word &newWord = ret.AddWord();
		for (size_t i = 0 ; i < outputFactor.size() ; i++)
		{
			FactorType factorType = outputFactor[i];
			const Factor *factor = ret.GetFactor(pos, factorType);
			assert(factor);
			newWord[factorType] = factor;
		}
	}

	return ret;
}

TO_STRING_BODY(TrellisPath);

