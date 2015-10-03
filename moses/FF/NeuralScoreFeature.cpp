#include <vector>
#include <string>
#include <sstream>
#include <deque>
#include <list>
#include <algorithm>

#include <boost/foreach.hpp>
#include <boost/interprocess/managed_shared_memory.hpp>
#include <boost/interprocess/containers/string.hpp>
#include <boost/interprocess/containers/vector.hpp>
#include <boost/interprocess/allocators/allocator.hpp>
#include <boost/interprocess/sync/interprocess_mutex.hpp>
#include <boost/interprocess/sync/interprocess_condition.hpp>
#include <boost/interprocess/sync/named_condition.hpp>
#include <boost/interprocess/sync/named_mutex.hpp>
#include <boost/interprocess/sync/scoped_lock.hpp>

#include "moses/ScoreComponentCollection.h"
#include "moses/TargetPhrase.h"
#include "moses/Hypothesis.h"
#include "moses/FF/NeuralScoreFeature.h"
#include "util/string_piece.hh"

namespace Moses
{
 
class NeuralScoreState : public FFState
{  
public:
  NeuralScoreState(void* context, const std::string& lastWord, void* state)
  : m_context(context), m_lastWord(lastWord), m_state(state) {
    m_lastContext.push_back(m_lastWord);
  }

  NeuralScoreState(void* context, const std::vector<std::string>& lastPhrase, void* state)
  : m_context(context), m_lastWord(lastPhrase.back()), m_state(state) {
    for(size_t i = 0; i < lastPhrase.size(); i++)
      m_lastContext.push_back(lastPhrase[i]);
  }
    
  int Compare(const FFState& other) const
  {
    const NeuralScoreState &otherState = static_cast<const NeuralScoreState&>(other);
    if(m_lastContext.size() == otherState.m_lastContext.size() &&
        std::equal(m_lastContext.begin(),
                   m_lastContext.end(),
                   otherState.m_lastContext.begin()))
      return 0;
    return (std::lexicographical_compare(m_lastContext.begin(), m_lastContext.end(),
                   otherState.m_lastContext.begin(),
                   otherState.m_lastContext.end())) ? -1 : +1;
  }

  void LimitLength(size_t length) {
    while(m_lastContext.size() > length)
      m_lastContext.pop_front();
  }
  
  void* GetContext() const {
    return m_context;
  }
  
  void* GetState() const {
    return m_state;
  }
  
  std::string GetLastWord() const {
    return m_lastWord;
  }
  
private:
  void* m_context;
  std::string m_lastWord;
  std::deque<std::string> m_lastContext;
  void* m_state;
};

const FFState* NeuralScoreFeature::EmptyHypothesisState(const InputType &input) const {
  UTIL_THROW_IF2(input.GetType() != SentenceInput,
                 "This feature function requires the Sentence input type");
  
  boost::interprocess::scoped_lock<boost::interprocess::named_mutex> lock(m_mutex);

  const Sentence& sentence = static_cast<const Sentence&>(input);
  std::stringstream ss;
  for(size_t i = 0; i < sentence.GetSize(); i++)
    ss << sentence.GetWord(i).GetString(0) << " ";
  
  void** pyContextVectors = m_segment->construct<void*>("NeuralContextPtr")((void*)0);            
  std::cerr << "Ptr: " << *pyContextVectors << std::endl;
  
  CharAllocator charAlloc(m_segment->get_segment_manager());
  ShmemString* sentenceString = m_segment->construct<ShmemString>("NeuralContextString")(charAlloc);
  std::string sentenceStringTemp = Trim(ss.str());
  sentenceString->insert(sentenceString->end(), sentenceStringTemp.begin(), sentenceStringTemp.end());
  
  m_neural.notify_one();
  m_moses.wait(lock);
  
  m_segment->destroy_ptr(sentenceString);
  
  std::cerr << "Ptr: " << *pyContextVectors << std::endl;
  
  return new NeuralScoreState(*pyContextVectors, "", NULL);
}

NeuralScoreFeature::NeuralScoreFeature(const std::string &line)
  : StatefulFeatureFunction(1, line),
  m_mutex(boost::interprocess::open_or_create,"MyMutex"), m_moses(boost::interprocess::open_or_create, "mosesCondition"),
  m_neural(boost::interprocess::open_or_create,"neuralCondition"),
  m_preCalc(0), /*m_batchSize(1000),*/ m_stateLength(3), m_factor(0)
{
  ReadParameters();
  boost::interprocess::shared_memory_object::remove("NeuralSharedMemory");
  m_segment.reset(new boost::interprocess::managed_shared_memory(boost::interprocess::create_only ,"NeuralSharedMemory", 1024 * 1024 * 1024));
  
  // Do a wait until the other process has loaded?
}

void NeuralScoreFeature::ProcessStack(Collector& collector, size_t index) {
  if(!m_preCalc)
    return;
  
  void* sourceContext = 0;
  std::map<int, const NeuralScoreState*> states;
  
  m_pbl.clear();
  
  size_t covered = 0;
  size_t total = 0;
  
  BOOST_FOREACH(const Hypothesis* h, collector.GetHypotheses()) {
    const Hypothesis& hypothesis = *h;
    
    const FFState* ffstate = hypothesis.GetFFState(index);
    const NeuralScoreState* state
      = static_cast<const NeuralScoreState*>(ffstate);
    
    if(sourceContext == 0) {
      sourceContext = state->GetContext();
      const WordsBitmap hypoBitmap = hypothesis.GetWordsBitmap();
      covered = hypoBitmap.GetNumWordsCovered();
      total = hypoBitmap.GetSize();
    }
   
    size_t hypId = hypothesis.GetId();
    states[hypId] = state;
    
    BOOST_FOREACH(const TranslationOptionList* tol, collector.GetOptions(hypId)) {
      TranslationOptionList::const_iterator iter;
      for (iter = tol->begin() ; iter != tol->end() ; ++iter) {
        const TranslationOption& to = **iter;
        const TargetPhrase& tp = to.GetTargetPhrase();
  
        Prefix prefix;
        for(size_t i = 0; i < tp.GetSize(); ++i) {
          prefix.push_back(to.GetTargetPhrase().GetWord(i).GetString(m_factor).as_string());
          if(m_pbl.size() < prefix.size())
            m_pbl.resize(prefix.size());
            
          m_pbl[prefix.size() - 1][prefix][hypId] = Payload();
        }
      }
    }
  }
  
  std::cerr << "Stack: " << covered << "/" << total << " - ";
  for(size_t l = 0; l < m_pbl.size(); l++) {
    Prefixes& prefixes = m_pbl[l];
  
    const CharAllocator charAlloc(m_segment->get_segment_manager());
    const ShmemStringAllocator stringAlloc(m_segment->get_segment_manager());
    const ShmemFloatAllocator floatAlloc(m_segment->get_segment_manager());
    const ShmemVoidptrAllocator voidptrAlloc(m_segment->get_segment_manager());
    
    ShmemStringVector *allWords = m_segment->construct<ShmemStringVector>("NeuralAllWords")(stringAlloc);
    ShmemStringVector *allLastWords = m_segment->construct<ShmemStringVector>("NeuralAllLastWords")(stringAlloc);
    ShmemVoidptrVector *allStates = m_segment->construct<ShmemVoidptrVector>("NeuralAllStates")(voidptrAlloc);

    ShmemFloatVector *allProbs = m_segment->construct<ShmemFloatVector>("NeuralLogProbs")(floatAlloc);
    ShmemVoidptrVector *allOutStates = m_segment->construct<ShmemVoidptrVector>("NeuralAllOutStates")(voidptrAlloc);
    
    for(Prefixes::iterator it = prefixes.begin(); it != prefixes.end(); it++) {
      const Prefix& prefix = it->first;
      BOOST_FOREACH(SP& hyp, it->second) {
        size_t hypId = hyp.first;
        
        ShmemString tempWord(charAlloc);
        tempWord = prefix[l].c_str();
        allWords->push_back(tempWord);
        void* state;
        if(prefix.size() == 1) {
          state = states[hypId]->GetState();
          ShmemString tempLastWord(charAlloc);
          tempLastWord = states[hypId]->GetLastWord().c_str();
          allLastWords->push_back(tempLastWord);
        }
        else {
          Prefix prevPrefix = prefix;
          prevPrefix.pop_back();
          state = m_pbl[prevPrefix.size() - 1][prevPrefix][hypId].state_;
          ShmemString tempLastWord(charAlloc);
          tempLastWord = prevPrefix.back().c_str();
          allLastWords->push_back(tempLastWord);
        }
        allStates->push_back(state);
      }
    }
    
    std::cerr << (l+1) << ":"
      << m_pbl[l].size() << ":" << allStates->size() << " ";
        
    boost::interprocess::scoped_lock<boost::interprocess::named_mutex> lock(m_mutex);
    m_neural.notify_one();
    m_moses.wait(lock);  
    
    size_t k = 0;
    for(Prefixes::iterator it = prefixes.begin(); it != prefixes.end(); it++) {
      BOOST_FOREACH(SP& hyp, it->second) {
        Payload& payload = hyp.second;
        payload.logProb_ = (*allProbs)[k];
        payload.state_ = (*allOutStates)[k];
        k++;
      }
    }
    
    m_segment->destroy_ptr(allWords);
    m_segment->destroy_ptr(allLastWords);
    m_segment->destroy_ptr(allStates);
    
    m_segment->destroy_ptr(allProbs);
    m_segment->destroy_ptr(allOutStates);
    
  }
  
  std::cerr << "ok" << std::endl;
}

/*
void NeuralScoreFeature::BatchProcess(
    const std::vector<std::string>& nextWords,
    PyObject* pyContextVectors,
    const std::vector<std::string>& lastWords,
    std::vector<PyObject*>& inputStates,
    std::vector<double>& logProbs,
    std::vector<PyObject*>& nextStates) {
  
    size_t items = nextWords.size();
    size_t batches = ceil(items/(float)m_batchSize);
    for(size_t i = 0; i < batches; ++i) {
      size_t thisBatchStart = i * m_batchSize;
      size_t thisBatchEnd = std::min(thisBatchStart + m_batchSize, items);
      
      //if(items > m_batchSize)
      //  std::cerr << "b:" << i << ":" << thisBatchStart << "-" << thisBatchEnd << " ";
      
      std::vector<std::string> nextWordsBatch(nextWords.begin() + thisBatchStart,
                                              nextWords.begin() + thisBatchEnd);
      std::vector<std::string> lastWordsBatch(lastWords.begin() + thisBatchStart,
                                              lastWords.begin() + thisBatchEnd);
      std::vector<PyObject*> inputStatesBatch(inputStates.begin() + thisBatchStart,
                                              inputStates.begin() + thisBatchEnd);
      
      std::vector<double> logProbsBatch;
      std::vector<PyObject*> nextStatesBatch;
      NMT_Wrapper::GetNMT().GetNextLogProbStates(nextWordsBatch,
                                    pyContextVectors,
                                    lastWordsBatch,
                                    inputStatesBatch,
                                    logProbsBatch,
                                    nextStatesBatch);
    
      logProbs.insert(logProbs.end(), logProbsBatch.begin(), logProbsBatch.end());
      nextStates.insert(nextStates.end(), nextStatesBatch.begin(), nextStatesBatch.end());
    }
}
*/

void NeuralScoreFeature::EvaluateInIsolation(const Phrase &source
    , const TargetPhrase &targetPhrase
    , ScoreComponentCollection &scoreBreakdown
    , ScoreComponentCollection &estimatedFutureScore) const
{}

void NeuralScoreFeature::EvaluateWithSourceContext(const InputType &input
    , const InputPath &inputPath
    , const TargetPhrase &targetPhrase
    , const StackVec *stackVec
    , ScoreComponentCollection &scoreBreakdown
    , ScoreComponentCollection *estimatedFutureScore) const
{}

void NeuralScoreFeature::EvaluateTranslationOptionListWithSourceContext(const InputType &input
    , const TranslationOptionList &translationOptionList) const
{}

FFState* NeuralScoreFeature::EvaluateWhenApplied(
  const Hypothesis& cur_hypo,
  const FFState* prev_state,
  ScoreComponentCollection* accumulator) const
{
  NeuralScoreState* prevState = static_cast<NeuralScoreState*>(
                                  const_cast<FFState*>(prev_state));
  
  void* context = prevState->GetContext();
  std::vector<float> newScores(m_numScoreComponents);
  
  const TargetPhrase& tp = cur_hypo.GetCurrTargetPhrase();
  Prefix phrase;
  
  for(size_t i = 0; i < tp.GetSize(); ++i) {
    std::string word = tp.GetWord(i).GetString(m_factor).as_string();
    phrase.push_back(word);
  }
  
  int prevId = cur_hypo.GetPrevHypo()->GetId();
  
  double prob = 0;
  void* state = 0;
  Prefix prefix;
  for(size_t i = 0; i < phrase.size(); i++) {
    prefix.push_back(phrase[i]);
    Payload& payload = const_cast<PrefsByLength&>(m_pbl)[prefix.size() - 1][prefix][prevId];
    state = payload.state_;
    prob += payload.logProb_;
  }
  prevState = new NeuralScoreState(context, phrase, state);
  newScores[0] = prob;
  
  accumulator->PlusEquals(this, newScores);
  
  prevState->LimitLength(m_stateLength);
  
  return prevState;
}

FFState* NeuralScoreFeature::EvaluateWhenApplied(
  const ChartHypothesis& /* cur_hypo */,
  int /* featureID - used to index the state in the previous hypotheses */,
  ScoreComponentCollection* accumulator) const
{
  return new NeuralScoreState(NULL, "", NULL);
}

void NeuralScoreFeature::SetParameter(const std::string& key, const std::string& value)
{
  if (key == "state") {
    m_statePath = value;
  } else if (key == "state-length") {
    m_stateLength = Scan<size_t>(value);
  } else if (key == "precalculate") {
    m_preCalc = Scan<bool>(value);
  //} else if (key == "batch-size") {
  //  m_batchSize = Scan<size_t>(value);
  } else if (key == "model") {
    m_modelPath = value;
  } else if (key == "wrapper-path") {
    m_wrapperPath = value;
  } else if (key == "source-vocab") {
    m_sourceVocabPath = value;
  } else if (key == "target-vocab") {
    m_targetVocabPath = value;
  } else {
    StatefulFeatureFunction::SetParameter(key, value);
  }
}

}

