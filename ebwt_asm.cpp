#include <iostream>
#include <fstream>
#include <string>
#include <cassert>
#include <getopt.h>
#include "assert_helpers.h"
#include "sequence_io.h"
#include "tokenize.h"
#include "timer.h"
#include "ref_read.h"
#include "hit.h"
#include "rot_buf.h"

/**
 * \file Driver for the bowtie-asm assembly tool.
 */

static bool verbose     = false; // be talkative (default)
static int sanityCheck  = 0;     // do slow sanity checks
static bool showVersion = false; // show version info and exit
static bool sorted      = false; // alignments are pre-sorted?

/**
 * Print a detailed usage message to the provided output stream.
 */
static void printUsage(ostream& out) {
	out << "Usage: bowtie-asm [options]* <alignments_in> [<reference_in>]" << endl
	    << "    alignments_in        output from bowtie" << endl
	    << "    reference_in         write Ebwt data to files with this dir/basename" << endl
	    << "Options:" << endl
	    << "    -v/--verbose         verbose output (for debugging)" << endl
	    << "    -s/--sorted          treat input alignments as already sorted" << endl
	    //<< "    -s/--sanity          enable sanity checks (much slower/increased memory usage)" << endl
	    << "    -h/--help            print detailed description of tool and its options" << endl
	    << "    --version            print version information and quit" << endl
	    ;
}

static const char *short_options = "hvs?";

enum {
	ARG_VERSION = 256,
	ARG_SANITY
};

static struct option long_options[] = {
	{"verbose", no_argument, 0, 'v'},
	{"sorted",  no_argument, 0, 's'},
	{"sanity",  no_argument, 0, ARG_SANITY},
	{"help",    no_argument, 0, 'h'},
	{"version", no_argument, 0, ARG_VERSION},
	{0, 0, 0, 0} // terminator
};

/**
 * Parse an int out of optarg and enforce that it be at least 'lower';
 * if it is less than 'lower', then output the given error message and
 * exit with an error and a usage message.
 */
template<typename T>
static int parseNumber(T lower, const char *errmsg) {
	char *endPtr= NULL;
	T t = (T)strtoll(optarg, &endPtr, 10);
	if (endPtr != NULL) {
		if (t < lower) {
			cerr << errmsg << endl;
			printUsage(cerr);
			exit(1);
		}
		return t;
	}
	cerr << errmsg << endl;
	printUsage(cerr);
	exit(1);
	return -1;
}

/**
 * Read command-line arguments
 */
static void parseOptions(int argc, char **argv) {
    int option_index = 0;
	int next_option;
	do {
		next_option = getopt_long(argc, argv, short_options, long_options, &option_index);
		switch (next_option) {
	   		case 'h':
	   		case '?':
				//printLongUsage(cout);
				printUsage(cout);
				exit(0);
				break;
	   		case 'v': verbose = false; break;
	   		case 's': sorted = true; break;
	   		case ARG_SANITY: sanityCheck = true; break;
	   		case ARG_VERSION: showVersion = true; break;
			case -1: /* Done with options. */
				break;
			case 0:
				if (long_options[option_index].flag != 0)
					break;
			default:
				cerr << "Unknown option: " << (char)next_option << endl;
				printUsage(cerr);
				exit(1);
		}
	} while(next_option != -1);
}

static char *argv0 = NULL;

/**
 * Sort alignments that are partitioned, where partitions are sorted
 * but alignments within partitions are not.
 */
template<typename T>
static void processAlignments(
		const string& filename,
		istream& alfile,
        AlignmentSink<T, 1024>& asink,
        ColumnAnalyzer<T>& analyzer,
        bool sorted)
{
	char buf[4096];     // buffer for current alignment
	char partbuf[4096]; // buffer for partition name in previous alignment
	vector<Hit> bucket; // bucket for buffering alignments in a particular partition
	size_t als = 0;
	if(verbose) {
		cout << "Processing ";
		if(sorted) cout << "sorted ";
		cout << "alignment file " << filename<< endl;
	}
	while(true) {
		alfile.getline(buf, 4096);
		size_t len = alfile.gcount();
		if(alfile.eof()) break;
		if(alfile.bad()) {
			cerr << "Alignment file set \"bad\" bit" << endl;
			exit(1);
		}
		if(alfile.fail()) {
			cerr << "A line from the alignment file was longer than 4K" << endl;
			exit(1);
		}
		if(len == 0) {
			cerr << "A line from the alignment file was empty" << endl;
			exit(1);
		}

		// Determine whether this alignment has the same partition
		// name as the last one; either way, make sure that 'partbuf'
		// contains this alignment's partition name when we exit the
		// loop.
		bool samePart = true; // assume hit has same partition as last hit
		if(!sorted) {
			bool sawSpace = false;
			size_t pos = 0;
			while(buf[pos] != '\t') {
				if(buf[pos] == ' ') sawSpace = true;
				if(samePart && partbuf[pos] != buf[pos]) {
					// The partition name is different in character at
					// 'pos' so mark them as different
					samePart = false;
				}
				if(!samePart) {
					// We've already established that this partition name
					// is different; copy it into 'partbuf' in preparation
					// for the next alignment
					partbuf[pos] = buf[pos];
				}
				pos++; // next character
				assert_lt(pos, len);
			}
			// Final case to check: buf is a prefix of partbuf
			if(partbuf[pos] != '\t') {
				samePart = false;
			}
			partbuf[pos] = '\t';
			// This override ensure that if the input is not partitioned
			// (i.e. if it's simply an unsorted list of unpartitioned
			// alignments), we'll treat the whole thing as belonging in a
			// single bucket.
			if(!sawSpace) {
				samePart = true;
			}
		}

		// We're moving into a new partition, so sort the elements
		// belonging to the last partition and push them into the
		// alignment sink
		if(!samePart && !bucket.empty()) {
			assert(!sorted);
			size_t bs = bucket.size();
			if(bs > 1) {
				sort(bucket.begin(), bucket.end());
			}
			for(size_t i = 0; i < bs; i++) {
				asink.addAlignment(bucket[i], &analyzer);
			}
			als += bs;
			bucket.clear();
		}

		// A true alignment line will have a tab
		bool hasTab = false;
		for(size_t i = 0; i < len; i++) {
			if(buf[i] == '\t') {
				hasTab = true;
				break;
			}
		}
		if(!hasTab) {
			// Not an alignment; could be the summary line
			continue;
		}

		// Create an istream from 'buf'
		std::istringstream ss;
		ss.rdbuf()->pubsetbuf(buf, len);
		ss.clear();

		if(!sorted) {
			// Add next alignment to back of current bucket
			bucket.resize(bucket.size() + 1);
			VerboseHitSink::readHit(bucket.back(), ss, NULL, verbose);
			assert_gt(bucket.back().length(), 0);
		} else {
			// Add next alignment directly to the alignment sink
			Hit h;
			// Parse the alignment from the istream
			VerboseHitSink::readHit(h, ss, NULL, verbose);
			assert_gt(h.length(), 0);
			// Send the alignment to the sink
			asink.addAlignment(h, &analyzer);
			als++;
		}
	}

	// Sort and flush the bucket if necessary
	if(!bucket.empty()) {
		assert(!sorted);
		size_t bs = bucket.size();
		if(bs > 1) {
			sort(bucket.begin(), bucket.end());
		}
		for(size_t i = 0; i < bs; i++) {
			asink.addAlignment(bucket[i], &analyzer);
		}
		als += bs;
		bucket.clear();
	}

	// Output summary
	if(verbose) {
		cout << "  Processed " << als << " alignments" << endl;
	}
}

/**
 * main function.  Parses command-line arguments.
 */
int main(int argc, char **argv) {

	string infile;
	vector<string> infiles;
	parseOptions(argc, argv);
	argv0 = argv[0];
	if(showVersion) {
		cout << argv0 << " version " << BOWTIE_VERSION << endl;
		cout << "Built on " << BUILD_HOST << endl;
		cout << BUILD_TIME << endl;
		cout << "Compiler: " << COMPILER_VERSION << endl;
		cout << "Options: " << COMPILER_OPTIONS << endl;
		cout << "Sizeof {int, long, long long, void*, size_t}: {" << sizeof(int)
		     << ", " << sizeof(long) << ", " << sizeof(long long)
		     << ", " << sizeof(void *)
		     << ", " << sizeof(size_t) << "}" << endl;
		cout << "Source hash: " << EBWT_ASM_HASH << endl;
		return 0;
	}

	// Get input filename
	if(optind >= argc) {
		cerr << "No alignments file specified!" << endl;
		printUsage(cerr);
		return 1;
	}
	infile = argv[optind++];
	tokenize(infile, ",", infiles);

	// For all input files
	SNPColumnCharPairAnalyzer ca(std::cout);
	RotatingCharPairAlignmentBuf<1024> cpBuf(verbose); // hoist this?
	for(size_t i = 0; i < infiles.size(); i++) {
		cpBuf.reset(0, &ca);
		// Input is partitioned, so call the set of functions that
		// know how to deal with partitioned input
		if(infiles[i] == "-") {
			processAlignments(infiles[i], cin, cpBuf, ca, sorted);
		} else {
			ifstream inin(infiles[i].c_str());
			//if(!inin.good()) {
			//	cerr << "Warning: could not open alignment file \""
			//		 << infiles[i] << "\" for reading" << endl;
			//	continue;
			//}
			processAlignments(infiles[i], inin, cpBuf, ca, sorted);
		}
		cpBuf.finalize(&ca);
		if(verbose) {
			cout << "Finished processing alignment file " << infiles[i] << endl;
		}
	}

	return 0;
}