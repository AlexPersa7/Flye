//(c) 2016 by Authors
//This file is a part of ABruijn program.
//Released under the BSD license (see LICENSE file)

#pragma once

#include "repeat_graph.h"
#include "read_aligner.h"
#include "multiplicity_inferer.h"

class RepeatResolver
{
public:
	RepeatResolver(RepeatGraph& graph, const SequenceContainer& asmSeqs,
				   const SequenceContainer& readSeqs, 
				   ReadAligner& aligner,
				   const MultiplicityInferer& multInf): 
		_graph(graph), _asmSeqs(asmSeqs), _readSeqs(readSeqs), 
		_aligner(aligner), _multInf(multInf) {}

	void findRepeats();
	void resolveRepeats();


private:
	struct Connection
	{
		GraphPath path;
		SequenceSegment readSequence;
	};

	void clearResolvedRepeats();
	void removeUnsupportedEdges();
	std::vector<Connection> getConnections();
	int  resolveConnections(const std::vector<Connection>& conns);
	void separatePath(const GraphPath& path, SequenceSegment segment,
					  FastaRecord::Id startId);

	RepeatGraph& _graph;
	const SequenceContainer&   _asmSeqs;
	const SequenceContainer&   _readSeqs;
	ReadAligner& _aligner;
	const MultiplicityInferer& _multInf;
};
