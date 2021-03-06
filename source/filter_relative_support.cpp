#include <cmath>
#include <iostream>
#include <fstream>
#include <sstream>
#include <tuple>
#include <string>
#include <set>
#include <unordered_map>
#include <vector>
#include "sam.h"
#include "common.hpp"
#include "annotation.hpp"
#include "filter_relative_support.hpp"

using namespace std;

void estimate_expected_fusions(fusions_t& fusions, const unsigned long int mapped_reads) {

	// find all fusion partners for each gene
	unordered_map< gene_t,gene_set_t > fusion_partners;
	unordered_map< tuple<gene_t,position_t,position_t>,char > overlap_duplicates;
	for (fusions_t::iterator fusion = fusions.begin(); fusion != fusions.end(); ++fusion) {
		if (fusion->second.filter == NULL && fusion->second.gene1 != fusion->second.gene2) {
			if (!overlap_duplicates[make_tuple(fusion->second.gene2, fusion->second.breakpoint1, fusion->second.breakpoint2)]++)
				fusion_partners[fusion->second.gene2].insert(fusion->second.gene1);
			if (!overlap_duplicates[make_tuple(fusion->second.gene1, fusion->second.breakpoint1, fusion->second.breakpoint2)]++)
				fusion_partners[fusion->second.gene1].insert(fusion->second.gene2);
		}
	}
	overlap_duplicates.clear();

	// count the number of fusion partners for each gene
	// fusions with genes that have more fusion partners are ignored
	unordered_map<gene_t,int> fusion_partner_count;
	for (auto fusion_partner1 = fusion_partners.begin(); fusion_partner1 != fusion_partners.end(); ++fusion_partner1) {
		for (auto fusion_partner2 = fusion_partner1->second.begin(); fusion_partner2 != fusion_partner1->second.end(); ++fusion_partner2) {
			if (fusion_partner1->second.size() >= fusion_partners[*fusion_partner2].size()) {
				fusion_partner_count[fusion_partner1->first]++;
			}
		}
	}

	// estimate the fraction of spliced breakpoints
	// non-spliced breakpoints get a penalty score based on how much more frequent they are than spliced breakpoints
	vector<unsigned int/*supporting reads*/> spliced_breakpoints(2);
	vector<unsigned int/*supporting reads*/> non_spliced_breakpoints(spliced_breakpoints.size());
	for (fusions_t::iterator i = fusions.begin(); i != fusions.end(); ++i) {
		unsigned int supporting_reads = i->second.supporting_reads();
		if (i->second.filter == NULL) {
			if (i->second.split_reads1 + i->second.split_reads2 > 0 && supporting_reads <= spliced_breakpoints.size()) {
				if (i->second.spliced1 || i->second.spliced2)
					spliced_breakpoints[supporting_reads-1]++;
				else
					non_spliced_breakpoints[supporting_reads-1]++;
			}
		}
	}
	vector<float> spliced_breakpoint_bonus(spliced_breakpoints.size());
	vector<float> non_spliced_breakpoint_bonus(non_spliced_breakpoints.size());
	for (unsigned int j = 0; j < spliced_breakpoints.size(); ++j) {
		spliced_breakpoint_bonus[j] = (float) spliced_breakpoints[j] / (spliced_breakpoints[j] + non_spliced_breakpoints[j]);
		non_spliced_breakpoint_bonus[j] = (float) non_spliced_breakpoints[j] / (spliced_breakpoints[j] + non_spliced_breakpoints[j]);
		// if there are not enough data points to estimate the bonuses, they might be 0
		// in this case we use some reasonable default values
		if (spliced_breakpoint_bonus[j] == 0 || non_spliced_breakpoint_bonus[j] == 0 || spliced_breakpoints[j] + non_spliced_breakpoints[j] == 0) {
			spliced_breakpoint_bonus[j] = 0.1;
			non_spliced_breakpoint_bonus[j] = 0.9;
		}
	}

	// for each fusion, check if the observed number of supporting reads cannot be explained by random chance,
	// i.e. if the number is higher than expected given the number of fusion partners in both genes
	for (fusions_t::iterator i = fusions.begin(); i != fusions.end(); ++i) {

		// pick the gene with the most fusion partners
		float max_fusion_partners = max(
			10000.0 / i->second.gene1->exonic_length * max(fusion_partner_count[i->second.gene1]-1, 0),
			10000.0 / i->second.gene2->exonic_length * max(fusion_partner_count[i->second.gene2]-1, 0)
		);

		// calculate expected number of fusions (e-value)

		// the more reads there are in the rna.bam file, the more likely we find fusions supported by just a few reads (2-4)
		// the likelihood increases linearly, therefore we scale up the e-value proportionately to the number of mapped reads
		// for every 20 million reads, the scaling factor increases by 1 (this is an empirically determined value)
		int supporting_reads;
		if (!i->second.breakpoint_overlaps_both_genes()) // for intergenic fusions we take the sum of all supporting reads (split reads + discordant mates)
			supporting_reads = i->second.supporting_reads();
		else // for intragenic fusions we ignore discordant mates, because they are very abundant
			supporting_reads = i->second.split_reads1 + i->second.split_reads2;

		i->second.evalue = max_fusion_partners * max(1.0, mapped_reads / 20000000.0 * pow(0.02, supporting_reads-2));

		// intergenic and intragenic fusions are scored differently, because they have different frequencies
		if (!(i->second.breakpoint1 >= i->second.gene2->start - 10000 && i->second.breakpoint1 <= i->second.gene2->end + 10000 &&
		      i->second.breakpoint2 >= i->second.gene1->start - 10000 && i->second.breakpoint2 <= i->second.gene1->end + 10000)) {

			// the more fusion partners a gene has, the less likely a fusion is true (hence we multiply the e-value by max_fusion_partners)
			// but the likehood of a false positive decreases near-exponentially with the number of supporting reads (hence we multiply by x^(supporting_reads-2) )
			if (supporting_reads > 1) {
				i->second.evalue *= 0.035;
				if (supporting_reads > 2) {
					i->second.evalue *= 0.2;
					if (supporting_reads > 3)
						i->second.evalue *= pow(0.4, supporting_reads-3);
				}
			}

		} else {

			if (supporting_reads > 1) {
				i->second.evalue *= 0.125;
				if (supporting_reads > 2) {
					i->second.evalue *= 0.3;
					if (supporting_reads > 3)
						i->second.evalue *= pow(0.4, supporting_reads-3);
				}

				// having discordant mates in addition to split reads only gives a small bonus
				if (i->second.discordant_mates > 0)
					i->second.evalue *= 0.9;

			} else if (i->second.discordant_mates > 0) { // event is only supported by discordant mates
				i->second.evalue *= 0.1;
				if (i->second.discordant_mates > 1)
					i->second.evalue *= 0.9;
			}


		}

		// breakpoints at splice-sites get a bonus
		if (supporting_reads > 0) {
			if (i->second.spliced1 || i->second.spliced2) {
				if (supporting_reads <= spliced_breakpoint_bonus.size())
					i->second.evalue *= spliced_breakpoint_bonus[supporting_reads-1];
				else
					i->second.evalue *= spliced_breakpoint_bonus[spliced_breakpoints.size()-1];
			} else {
				if (supporting_reads <= non_spliced_breakpoint_bonus.size())
					i->second.evalue *= non_spliced_breakpoint_bonus[supporting_reads-1];
				else
					i->second.evalue *= non_spliced_breakpoint_bonus[spliced_breakpoints.size()-1];
			}
		}
	}
}

unsigned int filter_relative_support(fusions_t& fusions, const float evalue_cutoff) {
	unsigned int remaining = 0;
	for (fusions_t::iterator i = fusions.begin(); i != fusions.end(); ++i) {
		if (i->second.filter != NULL)
			continue; // fusion has already been filtered

		// throw away fusions which are expected to occur by random chance
		if (i->second.evalue < evalue_cutoff && // only keep fusions with good e-value
		    !(i->second.gene1 == i->second.gene2 && i->second.split_reads1 + i->second.split_reads2 == 0)) { // but ignore intragenic fusions only supported by discordant mates
			remaining++;
		} else {
			i->second.filter = FILTERS.at("relative_support");
		}
	}
	return remaining;
}
