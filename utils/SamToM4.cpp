/*
 * =====================================================================================
 *
 *       Filename:  SamToM4.cpp
 *
 *    Description:  Convert a sam file to a blasr m4 file.
 *
 *        Version:  1.0
 *        Created:  04/03/2013 01:19:43 PM
 *       Revision:  none
 *       Compiler:  gcc
 *
 *         Author:  Yuan Li (yli), yli@pacificbiosciences.com
 *        Company:  Pacific Biosciences
 *
 * =====================================================================================
 */

#include <iostream>

#include <alignment/algorithms/alignment/AlignmentUtils.hpp>
#include <alignment/algorithms/alignment/DistanceMatrixScoreFunction.hpp>
#include <alignment/datastructures/alignment/AlignmentCandidate.hpp>
#include <alignment/datastructures/alignment/SAMToAlignmentCandidateAdapter.hpp>
#include <alignment/format/IntervalPrinter.hpp>
#include <pbdata/ChangeListID.hpp>
#include <pbdata/CommandLineParser.hpp>
#include <pbdata/FASTAReader.hpp>
#include <pbdata/FASTASequence.hpp>
#include <pbdata/sam/SAMReader.hpp>

char VERSION[] = "v0.1.0";
char PERFORCE_VERSION_STRING[] = "$Change: 126414 $";

int main(int argc, char* argv[])
{
    std::string program = "samtom4";
    std::string versionString = VERSION;
    AppendPerforceChangelist(PERFORCE_VERSION_STRING, versionString);

    std::string samFileName, refFileName, outFileName;
    bool printHeader = false;
    bool parseSmrtTitle = false;
    bool useShortRefName = false;

    CommandLineParser clp;
    clp.SetProgramName(program);
    clp.SetVersion(versionString);
    clp.SetProgramSummary("Converts a SAM file generated by blasr to M4 format.");
    clp.RegisterStringOption("in.sam", &samFileName, "Input SAM file, which is produced by blasr.");
    clp.RegisterStringOption("reference.fasta", &refFileName,
                             "Reference used to generate file.sam.");
    clp.RegisterStringOption("out.m4", &outFileName, "Output in blasr M4 format.");
    clp.RegisterPreviousFlagsAsHidden();
    clp.RegisterFlagOption("header", &printHeader, "Print M4 header.");
    clp.RegisterFlagOption("useShortRefName", &useShortRefName,
                           "Use abbreviated reference names obtained "
                           "from file.sam instead of using full names "
                           "from reference.fasta.");
    //clp.SetExamples(program + " file.sam reference.fasta out.m4");

    clp.ParseCommandLine(argc, argv);

    std::ostream* outFilePtr = &std::cout;
    std::ofstream outFileStrm;
    if (outFileName != "") {
        CrucialOpen(outFileName, outFileStrm, std::ios::out);
        outFilePtr = &outFileStrm;
    }

    SAMReader<SAMFullReferenceSequence, SAMReadGroup, SAMAlignment> samReader;
    FASTAReader fastaReader;

    //
    // Initialize samReader and fastaReader.
    //
    samReader.Initialize(samFileName);
    fastaReader.Initialize(refFileName);

    //
    // Configure the file log.
    //
    std::string command;
    CommandLineParser::CommandLineToString(argc, argv, command);

    //
    // Read necessary input.
    //
    std::vector<FASTASequence> references;
    fastaReader.ReadAllSequences(references);

    AlignmentSet<SAMFullReferenceSequence, SAMReadGroup, SAMAlignment> alignmentSet;
    samReader.ReadHeader(alignmentSet);

    //
    // The order of references in std::vector<FASTASequence> references and
    // AlignmentSet<, , >alignmentSet.references can be different.
    // Rearrange alignmentSet.references such that it is ordered in
    // exactly the same way as std::vector<FASTASequence> references.
    //
    alignmentSet.RearrangeReferences(references);

    //
    // Map short names for references obtained from file.sam to
    // full names obtained from reference.fasta
    //
    std::map<std::string, std::string> shortRefNameToFull;
    std::map<std::string, std::string>::iterator it;
    assert(references.size() == alignmentSet.references.size());
    if (!useShortRefName) {
        for (size_t i = 0; i < references.size(); i++) {
            std::string shortRefName = alignmentSet.references[i].GetSequenceName();
            std::string fullRefName(references[i].title);
            if (shortRefNameToFull.find(shortRefName) != shortRefNameToFull.end()) {
                std::cout << "ERROR, Found more than one reference " << shortRefName
                          << "in sam header" << std::endl;
                std::exit(EXIT_FAILURE);
            }
            shortRefNameToFull[shortRefName] = fullRefName;
            alignmentSet.references[i].sequenceName = fullRefName;
        }
    }

    // Map reference name obtained from SAM file to indices
    std::map<std::string, int> refNameToIndex;
    for (size_t i = 0; i < references.size(); i++) {
        std::string refName = alignmentSet.references[i].GetSequenceName();
        refNameToIndex[refName] = i;
    }

    //
    // Store the alignments.
    //
    SAMAlignment samAlignment;
    size_t alignIndex = 0;

    //
    // For 150K, each chip produces about 300M sequences
    // (not including quality values and etc.).
    // Let's assume that the sam file and reference data can
    // fit in the memory.
    // Need to scale for larger sequal data in the future.
    //
    if (printHeader) IntervalOutput::PrintHeader(*outFilePtr);

    // The socre matrix does not matter because we will use the
    // aligner's score from SAM file anyway.
    DistanceMatrixScoreFunction<DNASequence, DNASequence> distScoreFn;

    while (samReader.GetNextAlignment(samAlignment)) {
        if (samAlignment.rName == "*") {
            continue;
        }

        if (!useShortRefName) {
            //convert shortRefName to fullRefName
            it = shortRefNameToFull.find(samAlignment.rName);
            if (it == shortRefNameToFull.end()) {
                std::cout << "ERROR, Could not find " << samAlignment.rName
                          << " in the reference repository." << std::endl;
                std::exit(EXIT_FAILURE);
            }
            samAlignment.rName = (*it).second;
        }

        // The padding character 'P' is not supported
        if (samAlignment.cigar.find('P') != std::string::npos) {
            std::cout << "WARNING. Could not process sam record with 'P' in its cigar string."
                      << std::endl;
            continue;
        }

        std::vector<AlignmentCandidate<> > convertedAlignments;

        //
        // Keep reference as forward.
        // So if IsReverseComplement(sam.flag)==true, then qStrand is reverse
        // and tStrand is forward.
        //
        bool keepRefAsForward = false;

        SAMAlignmentsToCandidates(samAlignment, references, refNameToIndex, convertedAlignments,
                                  parseSmrtTitle, keepRefAsForward);

        if (convertedAlignments.size() > 1) {
            std::cout << "WARNING. Ignore an alignment which has multiple segments." << std::endl;
            continue;
        }

        //all alignments are unique single-ended alignments.
        for (int i = 0; i < 1; i++) {
            AlignmentCandidate<>& alignment = convertedAlignments[i];

            ComputeAlignmentStats(alignment, alignment.qAlignedSeq.seq, alignment.tAlignedSeq.seq,
                                  distScoreFn);

            // Use aligner's score from SAM file anyway.
            alignment.score = samAlignment.as;
            alignment.mapQV = samAlignment.mapQV;

            // Since SAM only has the aligned sequence, many info of the
            // original query (e.g. the full length) is missing.
            // Overwrite alignment.qLength (which is length of the query
            // in the SAM alignment) with xq (which is the length of the
            // original query sequence saved by blasr) right before printing
            // the output so that one can reconstruct a blasr m4 record from
            // a blasr sam alignment.
            if (samAlignment.xq != 0) alignment.qLength = samAlignment.xq;

            IntervalOutput::PrintFromSAM(alignment, *outFilePtr);

            alignment.FreeSubsequences();
        }
        ++alignIndex;
    }

    if (outFileName != "") {
        outFileStrm.close();
    }
    return 0;
}
