#include <iostream>
#include <fstream>
#include <sstream>
#include <tuple>
#include <string>
#include <set>
#include <unordered_map>
#include "common.hpp"
#include "annotation.hpp"
#include "read_compressed_file.hpp"
#include "recover_known_fusions.hpp"

using namespace std;

unsigned int recover_known_fusions(fusions_t& fusions, const string& known_fusions_file_path, const unordered_map<string,gene_t>& genes) {

	// load known fusions from file
	stringstream known_fusions_file;
	autodecompress_file(known_fusions_file_path, known_fusions_file);
	set< tuple<gene_t,gene_t> > known_fusions;
	string line;
	while (getline(known_fusions_file, line)) {
		if (!line.empty() && line[0] != '#') {
			istringstream iss(line);
			string gene1, gene2;
			iss >> gene1 >> gene2;
			if (genes.find(gene1) != genes.end()) {
				if (genes.find(gene2) != genes.end()) {
					known_fusions.insert(make_tuple(genes.at(gene1), genes.at(gene2)));
					known_fusions.insert(make_tuple(genes.at(gene2), genes.at(gene1)));
				} else {
					cerr << "WARNING: unknown gene in known fusions list: " << gene2 << endl;
				}
			} else {
				cerr << "WARNING: unknown gene in known fusions list: " << gene1 << endl;
			}
		}
	}

	// look for known fusions with low support which were filtered
	for (fusions_t::iterator i = fusions.begin(); i != fusions.end(); ++i) {

		if (i->second.filter == NULL || // fusion has not been filtered, no need to recover
		    i->second.gene1 == i->second.gene2) // don't recover intragenic events
			continue;

		if (i->second.filter != NULL && // fusion has been filtered
		    i->second.filter != FILTERS.at("relative_support") && i->second.filter != FILTERS.at("min_support")) // reason is not low support
			continue; // we won't recover fusions which were not discarded due to low support

		if (i->second.supporting_reads() >= 2 && // we still require at least two reads, otherwise there will be too many false positives
		    known_fusions.find(make_tuple(i->second.gene1, i->second.gene2)) != known_fusions.end()) // fusion is known
			i->second.filter = NULL;
	}

	// count remaining fusions
	unsigned int remaining = 0;
	for (fusions_t::iterator i = fusions.begin(); i != fusions.end(); ++i)
		if (i->second.filter == NULL)
			++remaining;
	return remaining;
}
