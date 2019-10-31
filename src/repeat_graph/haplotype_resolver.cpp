#include "haplotype_resolver.h"
#include "graph_processing.h"


//This function collapses simple bubbles caused by
//alternative haplotypes / strains. They are defined as follows:
//1. Structure: 1 input, 2 branches, 1 output: -<>-
//2. Size of each branch is shorter than MAX_BUBBLE_LEN below
//3. Each branch is shorter than both entrace and exits. We need this to
//   distinguish from the case of two repeats of multiplicity 2
//Note that we are not using any global coverage assumptions here.
int HaplotypeResolver::findHeterozygousBulges(bool removeAlternatives)
{
	//const float MAX_COV_VAR = 1.5;
	const int MAX_BUBBLE_LEN = Config::get("max_bubble_length");

	GraphProcessor proc(_graph, _asmSeqs);
	auto unbranchingPaths = proc.getUnbranchingPaths();

	std::unordered_set<FastaRecord::Id> toSeparate;
	int numMasked = 0;
	for (auto& path : unbranchingPaths)
	{
		if (path.isLooped()) continue;

		std::vector<UnbranchingPath*> twoPaths;
		for (auto& candEdge : unbranchingPaths)
		{
			if (candEdge.nodeLeft() == path.nodeLeft() &&
				candEdge.nodeRight() == path.nodeRight()) 
			{
				twoPaths.push_back(&candEdge);
			}
		}

		//making sure the structure is ok
		if (twoPaths.size() != 2) continue;
		if (twoPaths[0]->id == twoPaths[1]->id.rc()) continue;
		if (toSeparate.count(twoPaths[0]->id) || 
			toSeparate.count(twoPaths[1]->id)) continue;
		if (twoPaths[0]->nodeLeft()->inEdges.size() != 1 ||
			twoPaths[0]->nodeLeft()->outEdges.size() != 2 ||
			twoPaths[0]->nodeRight()->outEdges.size() != 1 ||
			twoPaths[0]->nodeRight()->inEdges.size() != 2) continue;

		UnbranchingPath* entrancePath = nullptr;
		UnbranchingPath* exitPath = nullptr;
		for (auto& cand : unbranchingPaths)
		{
			if (cand.nodeRight() == 
				twoPaths[0]->nodeLeft()) entrancePath = &cand;
			if (cand.nodeLeft() == twoPaths[0]->nodeRight()) exitPath = &cand;
		}

		//sanity check for maximum bubble size
		if (std::max(twoPaths[0]->length, twoPaths[1]->length) > 
			MAX_BUBBLE_LEN) continue;

		//coverage requirement: sum over two branches roughly equals to
		//exit and entrance coverage or less
		//float covSum = twoPaths[0]->meanCoverage + twoPaths[1]->meanCoverage;
		//if (covSum > std::min(entrancePath->meanCoverage * MAX_COV_VAR,
		//					  exitPath->meanCoverage * MAX_COV_VAR)) continue;

		//require bubble branches to be shorter than entrance or exit,
		//to distinguish from the case of two consecutive repeats
		//of multiplicity 2
		if (std::max(twoPaths[0]->length, twoPaths[1]->length) >
			std::max(entrancePath->length, exitPath->length)) continue;

		if (twoPaths[0]->meanCoverage > twoPaths[1]->meanCoverage)
		{
			std::swap(twoPaths[0], twoPaths[1]);
		}

		if (!twoPaths[0]->path.front()->altHaplotype ||
			!twoPaths[1]->path.front()->altHaplotype) ++numMasked;

		for (size_t i = 0; i < 2; ++i)
		{
			for (auto& edge : twoPaths[i]->path)
			{
				edge->altHaplotype = true;
				_graph.complementEdge(edge)->altHaplotype = true;
			}
		}

		//link edges
		GraphEdge* inEdge = entrancePath->path.back();
		GraphEdge* outEdge = exitPath->path.front();
		_graph.linkEdges(inEdge, outEdge);
		_graph.linkEdges(_graph.complementEdge(outEdge),
						 _graph.complementEdge(inEdge));

		//bridging sequence
		DnaSequence pathSeq = this->pathSequence(twoPaths[0]->path);
		_bridgingSeqs[std::make_pair(inEdge, outEdge)] = pathSeq;
		_bridgingSeqs[std::make_pair(_graph.complementEdge(outEdge), 
									 _graph.complementEdge(inEdge))] = 
												pathSeq.complement();

	}

	Logger::get().debug() << "[SIMPL] Masked " << numMasked
		<< " heterozygous bulges";
	return numMasked;
}

//This function collapses simple loops:
//1. One loop edge with one entrance and one exit
//2. Loop length is shorter than lengths of entrance/exit
//3. Loop coverage is roughly equal or less than coverage of entrance/exit
int HaplotypeResolver::findHeterozygousLoops(bool removeAlternatives)
{
	const float COV_MULT = 1.5;

	GraphProcessor proc(_graph, _asmSeqs);
	auto unbranchingPaths = proc.getUnbranchingPaths();

	std::unordered_set<FastaRecord::Id> toUnroll;
	std::unordered_set<FastaRecord::Id> toRemove;
	int numMasked = 0;
	for (auto& loop : unbranchingPaths)
	{
		if (!loop.id.strand()) continue;
		if (!loop.isLooped()) continue;
		if (loop.path.front()->selfComplement) continue;

		GraphNode* node = loop.nodeLeft();
		if (node->inEdges.size() != 2 ||
			node->outEdges.size() != 2) continue;

		UnbranchingPath* entrancePath = nullptr;
		UnbranchingPath* exitPath = nullptr;
		for (auto& cand : unbranchingPaths)
		{
			if (cand.nodeRight() == node &&
				loop.id != cand.id) entrancePath = &cand;
			if (cand.nodeLeft() == node &&
				loop.id != cand.id) exitPath = &cand;
		}

		if (entrancePath->isLooped()) continue;
		if (entrancePath->id == exitPath->id.rc()) continue;

		//loop coverage should be roughly equal or less
		if (loop.meanCoverage > 
				COV_MULT * std::min(entrancePath->meanCoverage, 
									entrancePath->meanCoverage)) continue;

		//loop should not be longer than other branches
		if (loop.length > std::max(entrancePath->length, 
								   exitPath->length)) continue;

		if (!loop.path.front()->altHaplotype) ++numMasked;
		for (auto& edge : loop.path)
		{
			edge->altHaplotype = true;
			_graph.complementEdge(edge)->altHaplotype = true;
		}

		//links
		GraphEdge* inEdge = entrancePath->path.back();
		GraphEdge* outEdge = exitPath->path.front();
		_graph.linkEdges(inEdge, outEdge);
		_graph.linkEdges(_graph.complementEdge(outEdge),
						 _graph.complementEdge(inEdge));

		//bridging sequence.
		//either remove or unroll loop, depending on the coverage
		if (loop.meanCoverage < 
			(entrancePath->meanCoverage + exitPath->meanCoverage) / 4)
		{
			_bridgingSeqs[std::make_pair(inEdge, outEdge)] = DnaSequence();
			_bridgingSeqs[std::make_pair(_graph.complementEdge(outEdge), 
							  			  _graph.complementEdge(inEdge))] =
															DnaSequence();

		}
		else
		{
			DnaSequence seq = this->pathSequence(loop.path);
			_bridgingSeqs[std::make_pair(inEdge, outEdge)] = seq;
			_bridgingSeqs[std::make_pair(_graph.complementEdge(outEdge), 
							  			  _graph.complementEdge(inEdge))] =
														seq.complement();
		}
	}

	Logger::get().debug() << "[SIMPL] Masked " << numMasked << " heterozygous loops"; return numMasked;
}

HaplotypeResolver::VariantPaths 
	HaplotypeResolver::findVariantSegment(GraphEdge* startEdge,
										  const std::vector<GraphAlignment>& alnignments,
										  const std::unordered_set<GraphEdge*>& loopedEdges)
{
	//first, extract alnignment paths starting from
	//the current edge and sort them from longest to shortest
	std::vector<GraphAlignment> outPaths;
	for (auto& aln : alnignments)
	{
		for (size_t i = 0; i < aln.size(); ++i)
		{
			if (aln[i].edge == startEdge)
			{
				outPaths.emplace_back(GraphAlignment(aln.begin() + i, 
													 aln.end()));
				break;
			}
		}
	}
	if (outPaths.empty()) return VariantPaths();

	std::sort(outPaths.begin(), outPaths.end(),
			  [](GraphAlignment& a1, GraphAlignment& a2)
			  {return a1.back().overlap.curEnd - a1.front().overlap.curEnd >
					  a2.back().overlap.curEnd - a2.front().overlap.curEnd;});

	Logger::get().debug() << "Haplo paths " 
		<< startEdge->edgeId.signedId() << " " << outPaths.size();
	/*for (auto& aln : outPaths)
	{
		std::string pathStr;
		for (size_t i = 0; i < aln.size(); ++i)
		{
			pathStr += std::to_string(aln[i].edge->edgeId.signedId()) + " -> ";
		}
		Logger::get().debug() << "\tPath: " << pathStr;
	}*/

	//now group the path by containmnent. For each group we'll have 
	//a longest "reference" path.

	//const int MIN_SCORE = std::max(2UL, outPaths.size() / 10);
	const int MIN_SCORE = 2;
	std::vector<PathWithScore> pathGroups;
	for (auto& trgPath: outPaths)
	{
		bool newPath = true;
		for (auto& referencePath : pathGroups)
		{
			bool contained = true;
			for (size_t i = 0; i < std::min(trgPath.size(), 
											referencePath.path.size()); ++i)
			{
				if (trgPath[i].edge != referencePath.path[i].edge)
				{
					contained = false;
					break;
				}
			}
			if (contained)
			{
				newPath = false;
				++referencePath.score;
				break;
			}
		}
		if (newPath)
		{
			pathGroups.push_back({trgPath, 1});
		}
	}

	pathGroups.erase(std::remove_if(pathGroups.begin(), pathGroups.end(),
					 [MIN_SCORE](PathWithScore& p)
					 {return p.score < MIN_SCORE;}), pathGroups.end());

	for (auto& aln : pathGroups)
	{
		std::string pathStr;
		for (size_t i = 0; i < aln.path.size(); ++i)
		{
			pathStr += std::to_string(aln.path[i].edge->edgeId.signedId()) + " -> ";
		}
		Logger::get().debug() << "\tGroup: " << pathStr << aln.score;
	}

	if (pathGroups.size() < 2) return VariantPaths();

	//mark edges that appear more than once as repeats
	std::unordered_set<GraphEdge*> repeats;
	for (size_t groupId = 0; groupId < pathGroups.size(); ++groupId)
	{
		std::unordered_set<GraphEdge*> seen;
		for (size_t i = 0; i < pathGroups[groupId].path.size(); ++i)
		{
			if (seen.count(pathGroups[groupId].path[i].edge))
			{
				repeats.insert(pathGroups[groupId].path[i].edge);
			}
			seen.insert(pathGroups[groupId].path[i].edge);
		}
	}

	//now, set the longest path as reference, and find
	//edges where other groups coverge with the reference
	PathWithScore& refPath = pathGroups.front();
	std::unordered_set<GraphEdge*> convergenceEdges;
	for (size_t i = 0; i < refPath.path.size(); ++i)
	{
		if (!loopedEdges.count(refPath.path[i].edge) &&
			!repeats.count(refPath.path[i].edge))
		{
			convergenceEdges.insert(refPath.path[i].edge);
		}
	}
	for (size_t groupId = 1; groupId < pathGroups.size(); ++groupId)
	{
		std::unordered_set<GraphEdge*> newSet;
		for (size_t i = 0; i < pathGroups[groupId].path.size(); ++i)
		{
			if (convergenceEdges.count(pathGroups[groupId].path[i].edge))
			{
				newSet.insert(pathGroups[groupId].path[i].edge);
			}
		}
		convergenceEdges = newSet;
	}

	//get the bubble start (paths might be convergent for a bit)
	size_t bubbleStartId = 0;
	for (;;)
	{
		bool agreement = true;
		for (size_t groupId = 1; groupId < pathGroups.size(); ++groupId)
		{
			if (bubbleStartId + 1 >= pathGroups[groupId].path.size() ||
				!convergenceEdges.count(pathGroups[0].path[bubbleStartId + 1].edge) ||
					(pathGroups[groupId].path[bubbleStartId + 1].edge !=
					 pathGroups[0].path[bubbleStartId + 1].edge))
			{
				agreement = false;
				break;
			}
		}
		if (!agreement) break;
		++bubbleStartId;
	}
	if (!convergenceEdges.count(refPath.path[bubbleStartId].edge)) return VariantPaths();

	//get the bubble end
	bool foundEnd = false;
	size_t bubbleEndId = bubbleStartId + 1;
	for (; bubbleEndId < refPath.path.size(); ++bubbleEndId)
	{
		if (convergenceEdges.count(refPath.path[bubbleEndId].edge))
		{
			foundEnd = true;
			break;
		}
	}
	if (!foundEnd) return VariantPaths();

	//shorten all branches accordingly
	std::vector<PathWithScore> bubbleBranches;
	for (size_t groupId = 0; groupId < pathGroups.size(); ++groupId)
	{
		size_t groupStart = 0;
		size_t groupEnd = 0;
		for (size_t i = 0; i < pathGroups[groupId].path.size(); ++i)
		{
			if (pathGroups[groupId].path[i].edge == 
				refPath.path[bubbleStartId].edge) groupStart = i;

			if (pathGroups[groupId].path[i].edge == 
				refPath.path[bubbleEndId].edge) groupEnd = i;
		}
		GraphAlignment newPath(pathGroups[groupId].path.begin() + groupStart,
							   pathGroups[groupId].path.begin() + groupEnd + 1);
		PathWithScore newBranch = {newPath, pathGroups[groupId].score};

		bool duplicate = false;
		for (size_t branchId = 0; branchId < bubbleBranches.size(); ++branchId)
		{
			if (newBranch.path.size() != 
				bubbleBranches[branchId].path.size()) continue;
			if (std::equal(newBranch.path.begin(), newBranch.path.end(),
						   bubbleBranches[branchId].path.begin(),
						   [](EdgeAlignment& a1, EdgeAlignment& a2)
						   {return a1.edge == a2.edge;}))
			{
				duplicate = true;
				bubbleBranches[branchId].score += newBranch.score;
			}
		}
		if (!duplicate) bubbleBranches.push_back(newBranch);
	}
	if (bubbleBranches.size() < 2) return VariantPaths();

	for (auto& aln : bubbleBranches)
	{
		std::string pathStr;
		for (size_t i = 0; i < aln.path.size(); ++i)
		{
			pathStr += std::to_string(aln.path[i].edge->edgeId.signedId()) + " -> ";
		}
		Logger::get().debug() << "\tBranch: " << pathStr << aln.score;
	}

	VariantPaths vp;
	vp.startEdge = refPath.path[bubbleStartId].edge;
	vp.endEdge = refPath.path[bubbleEndId].edge;
	vp.altPaths = bubbleBranches;
	return vp;
}

//this function reveals complex heterogenities on the graph
//(more than just two alternative branches) using read-paths
int HaplotypeResolver::findComplexHaplotypes()
{
	auto alnIndex = _aligner.makeAlignmentIndex();

	GraphProcessor proc(_graph, _asmSeqs);
	auto unbranchingPaths = proc.getUnbranchingPaths();
	std::unordered_set<GraphEdge*> loopedEdges;
	for (auto& path : unbranchingPaths)
	{
		if (path.isLooped())
		{
			loopedEdges.insert(path.path.begin(), path.path.end());
		}
	}

	std::unordered_set<GraphEdge*> usedEdges;
	std::vector<VariantPaths> foundVariants;
	for (auto& startPath: unbranchingPaths)
	{
		GraphEdge* startEdge = startPath.path.back();
		//if (startEdge->nodeRight->outEdges.size() < 2) continue;
		if (loopedEdges.count(startEdge)) continue;
		if (usedEdges.count(startEdge)) continue;
		
		auto varSeg = this->findVariantSegment(startEdge, alnIndex[startEdge], 
											   loopedEdges);
		if (varSeg.startEdge && varSeg.endEdge &&
			varSeg.startEdge != _graph.complementEdge(varSeg.endEdge))
		{
			auto revSeg = 
				this->findVariantSegment(_graph.complementEdge(varSeg.endEdge), 
										 alnIndex[_graph.complementEdge(varSeg.endEdge)], 
										 loopedEdges);
			if (revSeg.endEdge == _graph.complementEdge(varSeg.startEdge))
			{
				foundVariants.push_back(varSeg);
				usedEdges.insert(revSeg.startEdge);
				Logger::get().debug() << "Complex bulge: " << varSeg.startEdge->edgeId.signedId()
					<< " : " << varSeg.endEdge->edgeId.signedId();
			}
		}
	}

	for (auto& varSegment : foundVariants)
	{
		for (auto& branch : varSegment.altPaths)
		{
			for (size_t i = 1; i < branch.path.size() - 1; ++i)
			{
				branch.path[i].edge->altHaplotype = true;
				_graph.complementEdge(branch.path[i].edge)->altHaplotype = true;
			}
		}

		//add links
		_graph.linkEdges(varSegment.startEdge, varSegment.endEdge);
		_graph.linkEdges(_graph.complementEdge(varSegment.endEdge), 
						 _graph.complementEdge(varSegment.startEdge));

		//add bridging sequences
		auto readId = varSegment.altPaths[0].path[0].overlap.curId;
		int32_t readStart = varSegment.altPaths[0].path[0].overlap.curEnd;
		int32_t readEnd = varSegment.altPaths[0].path.back().overlap.curBegin;

		const int MAGIC_100 = 100;
		readEnd = std::max(readStart + MAGIC_100 - 1, readEnd);	
		DnaSequence seq = _readSeqs.getSeq(readId).substr(readStart, 
														  readEnd - readStart);
		
		auto fwdPair = std::make_pair(varSegment.startEdge, varSegment.endEdge);
		auto revPair = std::make_pair(_graph.complementEdge(varSegment.endEdge), 
									  _graph.complementEdge(varSegment.startEdge));
		_bridgingSeqs[fwdPair] = seq;
		_bridgingSeqs[revPair] = seq.complement();
	}

	Logger::get().debug() << "[SIMPL] Masked " << foundVariants.size()
		<< " complex haplotypes";
	return foundVariants.size();
}

void HaplotypeResolver::collapseHaplotypes()
{
	int numBridged = 0;
	std::unordered_set<GraphEdge*> separatedEdges;
	for (auto& inEdge : _graph.iterEdges())
	{
		if (!inEdge->rightLink) continue;
		if (separatedEdges.count(inEdge)) continue;

		GraphEdge* outEdge = inEdge->rightLink;
		if (!_graph.hasEdge(outEdge))
		{
			Logger::get().warning() << "Missing linked edge";
			continue;
		}
		if (outEdge->leftLink != inEdge)
		{
			Logger::get().warning() << "Broken link";
			continue; 
		}

		if (!_bridgingSeqs.count(std::make_pair(inEdge, outEdge)))
		{
			Logger::get().warning() << "No bridging path!";
			continue;
		}

		++numBridged;
		separatedEdges.insert(_graph.complementEdge(outEdge));

		if (inEdge->nodeRight == outEdge->nodeLeft)
		{
			this->separeteAdjacentEdges(inEdge, outEdge);
			this->separeteAdjacentEdges(_graph.complementEdge(outEdge),
										_graph.complementEdge(inEdge));
		}
		else
		{
			DnaSequence& insertSeq = _bridgingSeqs[std::make_pair(inEdge, outEdge)];

			FastaRecord::Id edgeId = _graph.newEdgeId();
			std::stringstream ss;
				ss << "edge_" << edgeId.signedId() << "_haplotype";
			EdgeSequence edgeSeq = 
				_graph.addEdgeSequence(insertSeq, 0, insertSeq.length(), ss.str());

			this->separateDistantEdges(inEdge, outEdge, edgeSeq, edgeId);
			this->separateDistantEdges(_graph.complementEdge(outEdge),
									   _graph.complementEdge(inEdge),
									   edgeSeq.complement(), edgeId.rc());
		}
	}

	_aligner.updateAlignments();
	Logger::get().debug() << "[SIMPL] Collapsed " << numBridged << " haplotypes";
}

DnaSequence HaplotypeResolver::pathSequence(GraphPath& path)
{
	std::string strSeq;
	for (size_t i = 0; i < path.size(); ++i)
	{
		strSeq += _graph.edgeSequences()
			.getSeq(path[i]->seqSegments.front().edgeSeqId).str();
	}
	if (strSeq.empty()) strSeq = "A";
	return DnaSequence(strSeq);
}

void HaplotypeResolver::separeteAdjacentEdges(GraphEdge* inEdge, GraphEdge* outEdge)
{
	GraphNode* newNode = _graph.addNode();

	vecRemove(inEdge->nodeRight->inEdges, inEdge);
	inEdge->nodeRight = newNode;
	newNode->inEdges.push_back(inEdge);

	vecRemove(outEdge->nodeLeft->outEdges, outEdge);
	outEdge->nodeLeft = newNode;
	newNode->outEdges.push_back(outEdge);
}

void HaplotypeResolver::separateDistantEdges(GraphEdge* inEdge, GraphEdge* outEdge,
						  					 EdgeSequence insertSeq, FastaRecord::Id newId)
{
	GraphNode* leftNode = _graph.addNode();
	vecRemove(inEdge->nodeRight->inEdges, inEdge);
	inEdge->nodeRight = leftNode;
	leftNode->inEdges.push_back(inEdge);

	GraphNode* rightNode = _graph.addNode();
	GraphEdge* newEdge = _graph.addEdge(GraphEdge(leftNode, rightNode,
												  newId));
	newEdge->seqSegments.push_back(insertSeq);
	int32_t pathCoverage = (inEdge->meanCoverage +
							outEdge->meanCoverage) / 2;
	newEdge->meanCoverage = pathCoverage;

	vecRemove(outEdge->nodeLeft->outEdges, outEdge);
	outEdge->nodeLeft = rightNode;
	rightNode->outEdges.push_back(outEdge);
}
