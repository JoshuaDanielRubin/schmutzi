/*
 * mitonchondrialDeam
 * Date: Mar-26-2014 
 * Author : Gabriel Renaud gabriel.reno [at sign here ] gmail.com
 *
 */




//TODO
//- print to log/genomes if booleans set
//documentation

// #define DEBUG1
// #define DEBUG2
// #define DEBUG3


// #include <boost/math/special_functions/beta.hpp>

// CODE ORGANIZATION
//
// main()
//   call initScores() to initialize probability scores
//   read arguments
//   read error and deamnination profiles
//   read fasta reference
//   call iterateOverReads() to populate infoPPos using reads from the BAM file
//   call printLogAndGenome() to print the log and genome using infoPPos
//   if a length or deamination prior is used:
//      call computePriorOnReads() to compute the prior on being endogenous or contaminant for each read using deamination/length using the endogenous base in iterateOverReads
//      re-call iterateOverReads() to incorporate the prior on being endogenous
//      re-call printLogAndGenome() to print the final genome.
//   
// computePriorOnReads(): Used to compute probability that a given read is endogenous
//   if deamination is used 
//      compute likelihood for illumina error
//      compute likelihood for deamination
//   if length is used
//      compute pdf for endogenous distribution
//      compute pdf for contaminant distribution
//   compute prob(endogenous) using the two tests + contamination prior and store read2endoProb 
//
// iterateOverReads(): Use to call MyPileupVisitor::visit() and populated infoPPos
//   clear and initialize infoPPos
//   open BAM file and iterate over reads using an MyPileupVisitor object. The Visit() subroutine will be called for each position
//   
// MyPileupVisitor::Visit() : called for a single position in the sorted BAM file
//   compute average mapping quality
//   Compute likelihood of variants:
//
//     For insertion in the reads/deletion in the reference:
//       Iterate over each read to get all possible inserts
//       Initialize log likelihood for all inserts (including the no insert empty string) to zero
//       if we assume a single contaminant
//         store likelihood for all pairs (endo,cont) of inserts in insertion2loglikeEndoCont
//       if we do not assume a single contaminant
//         store likelihood in insertion2loglike
//
//     For deletion in the sample (or insertion in the reference)
//       Iterate over each read
//       if we assume a single contaminant
//          Compute llikDeletionBoth : likelihood that both endo. and cont. have an deletion
//          Compute llikDeletionCont : likelihood that only the cont. (not endo.) has an deletion
//          Compute llikDeletionEndo : likelihood that only the endo. (not cont.) has an deletion
//          Compute llikDeletionNone : likelihood that neither the endo. nor the cont. have an deletion
//       if we do not assume a single contaminant
//          Compute llikDeletion for the likelihood of having a deletion
//          Compute llikNoDeletion for the likelihood of not having a deletion
//
//     For single nucleotides
//       Iterate over each read
//       Compute the probability of deamination for that given base given the distance to the 5p and 3p end
//       if we assume a single contaminant
//          Compute the likelihood for every pair of nucleotide (endo.,cont.) and store it in likeBaseNoindelCont[endo. nuc][cont. nuc]
//       if we do not assume a single contaminant
//          Compute the likelihood for every endogenous nucleotide and store it likeBaseNoindel[endo nuc]
//
// printLogAndGenome(): Subroutine that uses infoPPos to generate the genome and the log file
//   For each position on the mitonchondria
//     Call deletionInSample() to detect deletions in the endogenous/contaminant/insertions in the reference
//     Call noCoverage to skip positions with no coverage
//     Call callSingleNucleotide to call the endogenous (or contaminant)
//     Call insertionInSample to call insertions after the base
//
//
   
#include <api/BamConstants.h>
#include <api/BamMultiReader.h>
#include <utils/bamtools_fasta.h>
#include <utils/bamtools_options.h>
#include <utils/bamtools_pileup_engine.h>
#include <utils/bamtools_utilities.h>
#include <gzstream.h>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <limits>
#include <math.h>

#include "utils.h"
#include "miscfunc.h"
#include "ReconsReferenceBAM.h"

using namespace BamTools;
using namespace std;

#define MAXCOV    5000
#define INDELERRORPROB 1.0e-5 // http://genomebiology.com/2011/12/11/R112
#define LOGRATIOFORINDEL 50  //beyond that difference in log, a indel will be called
#define MIN(a,b) (((a)<(b))?(a):(b))

const long double PI  = atanl(1.0L)*4;   


//! Computes log of probability density function
/*!
  Computes the log base 10 of pdf(x)
  
  \param mu The mean (location)
  \param sigma The variance (scale)
  \param x The value for which we want the pdf
  \return The values of log base 10 of (pdf(x))
*/
long double logcomppdf(long double mu,long double sigma,long double x){
    if(x==0){
	x=1.0;
    }

    long double two = 2.0;   
    long double exponent = log(x) - mu;
    exponent *= -exponent;
    exponent /= two * sigma * sigma;
    
    long double result = exponent/logl(10);
    result -= logl(sigma * sqrtl(two * PI) * x)/logl(10);

    return result;
}




// typedef struct { 
//     double likeBaseNoindel[4];
//     int  covPerBase[4];
//     double mapqAvg;
    
//     int numDel;
//     vector<string> insertionRight;
//     int cov;
// } positionInformation;


char offsetQual=33;
double likeMatch[64];
double likeMismatch[64];
double likeMatchMQ[64][64];
double likeMismatchMQ[64][64];

double likeMatchProb[64];
double likeMismatchProb[64];
double likeMatchProbMQ[64][64];
double likeMismatchProbMQ[64][64];

double probMapping[64];
double probMismapping[64];


double probLengthEndo[1000];

probSubstition illuminaErrorsProb;
vector<probSubstition> sub5p;
vector<probSubstition> sub3p;
probSubstition defaultSubMatch;

string dnaAlphabet="ACGT";
map<int, PHREDgeno> pos2phredgeno;

map<string, long double > read2endoProb; //map seq id to probability that the read is endogenous using a deamination model
long double read2endoProbInit=false;



template <typename T>
inline string arrayToStringInt(const T toPrint[] ,const int size,const string separator=","){
    if(size == 0){
    	return "";
    }
    string toReturn="";
    for(int i=0;i<(size-1);i++){
    	toReturn+=(stringify(int(toPrint[i]))+separator);
    }
    toReturn+=(stringify(int(toPrint[ size -1 ])));
    return toReturn;
}


inline void transformRef(char * refeBase,char * readBase){
    if( (*refeBase) == 'M'){
	(*refeBase)=(*readBase);
    }
    
}


inline bool hasIinfirstOrLastTwoBases(const string & reconstructedReference){
    if(reconstructedReference.length() <= 4){
	cerr<<"ERROR read has length less than 4 bp"<<endl;
	exit(1);
    }

    for(unsigned int j=0;j<2;j++){
	if(reconstructedReference[j] == 'I')
	    return true;
    }


    for(unsigned int j=(reconstructedReference.length()-2);
	j<(reconstructedReference.length());
	j++){
	if(reconstructedReference[j] == 'I')
	    return true;
    }

    return false;
}

inline bool deletionsNextToTwo(const BamAlignment  * al){
    vector<int> lengthOfNonDels;
    vector<CigarOp> cigarData=al->CigarData;
    bool foundDel=false;
    for(unsigned int i=0;i<cigarData.size();i++){
        if(cigarData[i].Type == 'D'){
	    foundDel=true;
	}else{
	    lengthOfNonDels.push_back(cigarData[i].Length);
	}
    }

    if(foundDel){
	if(lengthOfNonDels[0]<=2)
	    return true;
	if(lengthOfNonDels[ lengthOfNonDels.size() -1 ]<=2)
	    return true;
    }
    
    return false;
}

 

//checks for an 'R' or 'S' for soft clip
inline bool hasBadCharacter(const string & reconstructedReference){

    for(unsigned int j=0;j<(reconstructedReference.length());j++){
	if(reconstructedReference[j] == 'R'  || 
	   reconstructedReference[j] == 'S' ){
	    return true;
	}
    }
    return false;
}

inline bool skipAlign(const string & reconstructedReference,const BamAlignment  * al,unsigned int * skipped){
    if(hasBadCharacter(reconstructedReference)){
	(*skipped)++;
	return true;
    }
	

    if(hasIinfirstOrLastTwoBases(reconstructedReference)){
	(*skipped)++;
	return true;
    }

    if(deletionsNextToTwo(al)){
	(*skipped)++;
	return true;
    }

    return false;
}


//This method calls the best nucleotide and 
//
inline void callBestNucleotideGivenLikelihood( int         & bestNuc,
					       long double & sumLogLikeAll,        // sum of all the logs
					       long double & sumLogLikeOnlyBest,   // sum of the logs for the best
					       long double & sumLogLikeAllButBest, // sum of the logs for all but the best
					       long double sumLogForNucs[],
					       const long double likeBaseNoindel [],
					       const vector<singlePosInfo> & infoPPos,
					       const int i
					       ){

    //init
    for(int nuc=0;nuc<4;nuc++){	    
	sumLogForNucs[nuc]   = 0.0;
    }

    sumLogLikeAll          = 0.0; // sum of all the logs
    sumLogLikeOnlyBest     = 0.0; // sum of the logs for the best
    sumLogLikeAllButBest   = 0.0; // sum of the logs for all but the best
    // bool sumLogLikeAllB           = true;
    // bool sumLogLikeOnlyBestB      = true;
    // bool sumLogLikeAllButBestB    = true;
    // bool  sumLogForNucsB[4]; //sumLogForNucs has to be initialized
    // for(int nuc=0;nuc<4;nuc++){
    // 	sumLogForNucsB[nuc]=true;
    // }
    
    long double bestLike=-INFINITY;
    //int    bestNuc=-1;

    //Determining most likely nucleotide
    for(unsigned int nuc=0;nuc<4;nuc++){
	// if(i==146){
	//     cout<<nuc<<"\t"<<likeBaseNoindel[nuc]<<endl;
	// }
	if(likeBaseNoindel[nuc] > bestLike){
	    bestLike=likeBaseNoindel[nuc];
	    bestNuc=nuc;
	}
    }

    //If there are more than one with equal likelihood (this is highly unlikely)
    //take the one with the greatest coverage 
    vector<int> bestNucs;

    for(unsigned int nuc=0;nuc<4;nuc++){
	if(likeBaseNoindel[nuc] == bestLike){
	    bestNucs.push_back(nuc);
	}
    }
	 
    if(bestNucs.size() > 1){ // multiple equally likely nuc, use coverage to call best one
	// cerr<<"size "<<bestNucs.size()<<endl;
	// return 1;
	int bestCov=-1;
	int bestCovN=bestNucs[0];
	     
	for(unsigned int bc=0;bc<bestNucs.size();bc++){
	    if(infoPPos[i].covPerBase[ bestNucs[bc] ] > bestCov){
		bestCov  =infoPPos[i].covPerBase[ bestNucs[bc] ];
		bestCovN =                        bestNucs[bc];		     
	    }
	}
	     
	bestNuc = bestCovN;
    }
    //end
	 
    
    

    //computing the probability of error
    for(int nuc=0;nuc<4;nuc++){
	// cout<<(i+1)<<"\tnuc\t"<<dnaAlphabet[nuc]<<"\t"<<dnaAlphabet[bestNuc]<<"\t"<<infoPPos[i].likeBaseNoindel[nuc]<<"\t"<<likeBaseNoindel[nuc]<<"\t"<<pow(10.0,infoPPos[i].likeBaseNoindel[nuc])<<endl;
	     
	for(int nuc2=0;nuc2<4;nuc2++){
	    if(nuc!=nuc2){
		
		//sumLogForNucs[nuc]         += pow(10.0,likeBaseNoindel[nuc2]);
		sumLogForNucs[nuc]  = oplusInit(sumLogForNucs[nuc] , likeBaseNoindel[nuc2]);

	    }
	}


	//sumLogLikeAll              +=  pow(10.0,likeBaseNoindel[nuc]);
	sumLogLikeAll =  oplusInit(sumLogLikeAll,likeBaseNoindel[nuc]);
	
	if(nuc==bestNuc){	    
	    //sumLogLikeOnlyBest    +=  pow(10.0,likeBaseNoindel[nuc]);
	    sumLogLikeOnlyBest   = oplusInit(sumLogLikeOnlyBest,likeBaseNoindel[nuc]);
	}else{

	    //sumLogLikeAllButBest  +=  pow(10.0,likeBaseNoindel[nuc]);
	    sumLogLikeAllButBest  = oplusInit( sumLogLikeAllButBest,likeBaseNoindel[nuc]);

	}		 	    
    }//end for nuc

    // for(int nuc=0;nuc<4;nuc++){	    
    // 	sumLogForNucs[nuc]   = log(sumLogForNucs[nuc])/log(10);

    // 	// if(i==146){
    // 	//     cout<<nuc<<"\t"<<sumLogForNucs[nuc]<<endl;
    // 	// }
    // }

    // sumLogLikeAll        = log(sumLogLikeAll)       /log(10);
    // sumLogLikeOnlyBest   = log(sumLogLikeOnlyBest)  /log(10);
    // sumLogLikeAllButBest = log(sumLogLikeAllButBest)/log(10);
    
}

// typedef struct { 
//     char ref;
//     char alt;
//      unsigned int pos;
//     double contFreq;
//     unsigned char cov;
//     unsigned char refCov;
//     unsigned char altCov;

//     //    unsigned int ;

//     bool      refOrAlt[MAXCOV]; //ref = false, alt=true
//     unsigned char mapq[MAXCOV];
//     unsigned char quals[MAXCOV];;
//     unsigned char dist5p[MAXCOV];;
//     unsigned char dist3p[MAXCOV];;   
//  } positionInfo;


void insertionInSample(const int i,
		       const string & genomeRef,
		       const vector<singlePosInfo> & infoPPos, 
		       const bool singleCont,
		       string & genomeToPrint,
		       string & genomeToPrintC,
		       stringstream * logToPrint,
		       stringstream * logToPrintC){


    if(infoPPos[i].allInserts.size() != 0){  // there are potential insertions


	if(singleCont){
	    //calling the endogenous
	    string       bestInsertEndo       ="";
	    long double  bestInsertLogLikeEndo;
	    bool         initializeBEndo      =false;
	    long double  sumLogLikeEndo       =0.0;

	    //iterate for each endogenous insert
	    for(set<string>::const_iterator it1 = infoPPos[i].allInserts.begin(); 
 		it1 != infoPPos[i].allInserts.end(); 
 		++it1) {

		 long double sumLogLikeForThisIns=0.0;

		 //marginalize over each possible contaminant
		 for(set<string>::const_iterator it2 = infoPPos[i].allInserts.begin(); 
		     it2 != infoPPos[i].allInserts.end(); 
		     ++it2) {
		     pair<string,string> keytouse (*it1,*it2);
		     // infoPPos[i].insertion2loglikeEndoCont[ keytouse ] = 0.0;
		     sumLogLikeForThisIns =  oplusInit(sumLogLikeForThisIns,
						       infoPPos[i].insertion2loglikeEndoCont.at( keytouse ));
		 }
		 
		 if(!initializeBEndo){
		     bestInsertEndo            = *it1;
		     bestInsertLogLikeEndo     = sumLogLikeForThisIns;
		     initializeBEndo           = true;
		 }else{
		     if(bestInsertLogLikeEndo < sumLogLikeForThisIns){
			 bestInsertEndo        = *it1;
			 bestInsertLogLikeEndo = sumLogLikeForThisIns;
		     }
		 }
		 sumLogLikeEndo = oplusInit(sumLogLikeEndo,sumLogLikeForThisIns);
	     }//end for each endogenous insert
	     
	     string       bestInsertCont       ="";
	     long double  bestInsertLogLikeCont;
	     bool         initializeBCont      =false;
	     long double  sumLogLikeCont       =0.0;

	     //iterate for each contaminant insert
	     for(set<string>::const_iterator it2 = infoPPos[i].allInserts.begin(); 
		 it2 != infoPPos[i].allInserts.end(); 
		 ++it2) {

		 long double sumLogLikeForThisIns=0.0;

		 //marginalize over each possible endegenous		
		 for(set<string>::const_iterator it1 = infoPPos[i].allInserts.begin(); 
		     it1 != infoPPos[i].allInserts.end(); 
		     ++it1) {
		     pair<string,string> keytouse (*it1,*it2);
		     // infoPPos[i].insertion2loglikeEndoCont[ keytouse ] = 0.0;
		     sumLogLikeForThisIns =  oplusInit(sumLogLikeForThisIns,
						       infoPPos[i].insertion2loglikeEndoCont.at( keytouse ));
		 }
		 
		 if(!initializeBCont){
		     bestInsertCont            = *it2;
		     bestInsertLogLikeCont     = sumLogLikeForThisIns;
		     initializeBCont           = true;
		 }else{
		     if(bestInsertLogLikeCont < sumLogLikeForThisIns){
			 bestInsertCont        = *it2;
			 bestInsertLogLikeCont = sumLogLikeForThisIns;
		     }
		 }
		 sumLogLikeCont = oplusInit(sumLogLikeCont,sumLogLikeForThisIns);
	     }//end for each endogenous insert
	     

	     if(bestInsertEndo != ""){ //more likely there is an insert in the endogenous
		long double  sumLogLikeButTheBestEndo=log( pow(10.0,sumLogLikeEndo) - pow(10.0,bestInsertLogLikeEndo) )/log(10);
		
		for(unsigned int k=0;k<(bestInsertEndo.size());k++){
		    (*logToPrint)<<(i+1)<<"i\t"<<"-"<<"\t"<<bestInsertEndo[k]<<"\t"<<-10.0*( sumLogLikeButTheBestEndo - sumLogLikeEndo)<<"\t"<<infoPPos[i].mapqAvg<<"\t"<<infoPPos[i].cov<<"\t"<<infoPPos[i].insertion2count.at(bestInsertEndo)<<"\t0.0\t0.0\t0.0\t0.0"<<endl;
		}

		genomeToPrint+=bestInsertEndo;
	    }


	     if(bestInsertCont != ""){ //more likely there is an insert in the endogenous
		long double  sumLogLikeButTheBestCont=log( pow(10.0,sumLogLikeCont) - pow(10.0,bestInsertLogLikeCont) )/log(10);

		for(unsigned int k=0;k<(bestInsertCont.size());k++){
		    (*logToPrintC)<<(i+1)<<"i\t"<<"-"<<"\t"<<bestInsertCont[k]<<"\t"<<-10.0*( sumLogLikeButTheBestCont - sumLogLikeCont)<<"\t"<<infoPPos[i].mapqAvg<<"\t"<<infoPPos[i].cov<<"\t"<<infoPPos[i].insertion2count.at(bestInsertCont)<<"\t0.0\t0.0\t0.0\t0.0"<<endl;
		}

		genomeToPrintC+=bestInsertCont;
	    }





	    
	}else{ //cannot assume we have a single contaminant
	    //for each potential insert
	    string       bestInsert       ="";
	    long double  bestInsertLogLike;
	    bool         initializeB      =false;
	    long double  sumLogLike       =0.0;

	    for(set<string>::const_iterator it1 = infoPPos[i].allInserts.begin(); 
		it1 != infoPPos[i].allInserts.end(); 
		++it1) {
		if(!initializeB){
		    bestInsertLogLike     = infoPPos[i].insertion2loglike.at(*it1);
		    bestInsert            = *it1;		    
		    initializeB = true;
		}else{
		    if( bestInsertLogLike < infoPPos[i].insertion2loglike.at(*it1) ){
			bestInsertLogLike = infoPPos[i].insertion2loglike.at(*it1);
			bestInsert        = *it1;		    
		    }
		}
		sumLogLike = oplusInit(sumLogLike,infoPPos[i].insertion2loglike.at(*it1));
	    }
	    

	    
	    if(bestInsert != ""){ //more likely there is an insert
		long double  sumLogLikeButTheBest=log( pow(10.0,sumLogLike) - pow(10.0,bestInsertLogLike) )/log(10);

		for(unsigned int k=0;k<(bestInsert.size());k++){
		    (*logToPrint)<<(i+1)<<"i\t"<<"-"<<"\t"<<bestInsert[k]<<"\t"<<-10.0*( sumLogLikeButTheBest - sumLogLike)<<"\t"<<infoPPos[i].mapqAvg<<"\t"<<infoPPos[i].cov<<"\t"<<infoPPos[i].insertion2count.at(bestInsert)<<"\t0.0\t0.0\t0.0\t0.0"<<endl;

		}
	    }
	    
	}

	
	

	
    }



    //old way
    // if(infoPPos[i].insertionRight.size() != 0){

    // 	map<string,int> insert2count;
    // 	for(unsigned int k=0;k<infoPPos[i].insertionRight.size();k++){		 
    // 	    insert2count[ infoPPos[i].insertionRight[k] ]++;
    // 	}
	     
	     
    // 	int     mostCommonInsCount=-1;
    // 	string  mostCommonIns="";


    // 	for (map<string,int>::iterator itdel=insert2count.begin(); 
    // 	     itdel!=insert2count.end(); ++itdel){
    // 	    //cout<<itdel->first<<"\t"<<itdel->second<<endl;
    // 	    if( itdel->second > mostCommonInsCount){
    // 		mostCommonInsCount = itdel->second;
    // 		mostCommonIns      = itdel->first;
    // 	    }		     
    // 	}


    // 	//if half of the reads support an insertions in reads (deletions in reference) 
    // 	//TODO add contamination and endogenous split
    // 	//add probability of correctness "Evaluation of genomic high-throughput sequencing data generated on Illumina HiSeq and Genome Analyzer systems" Minoche
    // 	if( ((long double)(mostCommonInsCount)/(long double)(infoPPos[i].cov)) >= 0.5){
    // 	    //outSeqFP<<mostCommonIns;
    // 	    //return 1;
    // 	    //if( (-10.0*(log(1.0-double(mostCommonInsCount)/double(infoPPos[i].cov))/log(10.0)))>=minQual){
    // 	    //min quality ? based on what ?
    // 	    genomeToPrint+=mostCommonIns;
    // 	    //}else{
    // 	    //do not add insert
    // 	    //}

    // 	    //logToPrint<<string('!',mostCommonIns.size());
    // 	    for(unsigned int k=0;k<(mostCommonIns.size());k++){
    // 		(*logToPrint)<<(i+1)<<"i\t"<<"-"<<"\t"<<mostCommonIns[k]<<"\t"<<-10.0*(log(1.0-(long double)(mostCommonInsCount)/(long double)(infoPPos[i].cov))/log(10.0))<<"\t"<<infoPPos[i].mapqAvg<<"\t"<<infoPPos[i].cov<<"\t"<<mostCommonInsCount<<"\t0.0\t0.0\t0.0\t0.0"<<endl;

    // 	    }
		 
    // 	}

    // }//end insertions in the sample

    

}


void deletionInSample(const int i,
		      const string & genomeRef,
		      const vector<singlePosInfo> & infoPPos,
		      const bool singleCont,
		      stringstream * logToPrint,
		      stringstream * logToPrintC,
		      bool & skipEndo,
		      bool & skipCont){
    
    if(  infoPPos[i].numDel >= 0){

	// cout<<(i+1)<<"\t"<<genomeRef[i]<<"\t"<<"D"<<"\t"<<(long double)(infoPPos[i].numDel)/(long double)(infoPPos[i].cov)<<"\t"<<infoPPos[i].mapqAvg<<"\t"<<infoPPos[i].cov<<"\t"<<infoPPos[i].numDel<<"\t0.0\t0.0\t0.0\t0.0"<<endl;
	// cout<<infoPPos[i].llikDeletion<<endl;
	// cout<<infoPPos[i].llikNoDeletion<<endl;

	// // cout<<infoPPos[i].llikDeletionCont<<endl;
	// // cout<<infoPPos[i].llikNoDeletionCont<<endl;
	// cout<<"llik\t"<<infoPPos[i].llikDeletionBoth<<"\t"<<infoPPos[i].llikDeletionEndo<<"\t"<<infoPPos[i].llikDeletionCont<<"\t"<<infoPPos[i].llikDeletionNone<<endl;
	    

	if(singleCont){
	    long double logLikeDel[] = { infoPPos[i].llikDeletionBoth,infoPPos[i].llikDeletionEndo,infoPPos[i].llikDeletionCont,infoPPos[i].llikDeletionNone };
	    
	    pair<long double,long double> maxAndSecondMax = firstAndSecondHighestArray( logLikeDel,4 );

	    long double sumLogLikeAll=0.0;
	    for(int n=0;n<4;n++){
		sumLogLikeAll =  oplusInit(sumLogLikeAll,logLikeDel[n]);
	    }

	    // cout<<maxAndSecondMax.first<<"\t"<<maxAndSecondMax.second<<endl;

	    // if(i==513){
	    // 	cout<<"POSllik\t"<<infoPPos[i].llikDeletionBoth<<"\t"<<infoPPos[i].llikDeletionEndo<<"\t"<<infoPPos[i].llikDeletionCont<<"\t"<<infoPPos[i].llikDeletionNone<<endl;
	    // }

	    //both the contaminant and endegenous have a deletion wrt the reference, no need to call any base
	    if( (infoPPos[i].llikDeletionBoth == maxAndSecondMax.first) &&
		((infoPPos[i].llikDeletionBoth - maxAndSecondMax.second) > LOGRATIOFORINDEL)){
	
		// cout<<"both"<<endl;
		long double sumLogLikeCase=0.0;
		for(int n=0;n<4;n++){
		    if(n!=0)
			sumLogLikeCase =  oplusInit(sumLogLikeCase,logLikeDel[n]);
		}

		(*logToPrint)<<(i+1)<<"\t"<<genomeRef[i]<<"\t"<<"D"<<"\t"<<(-10.0*(sumLogLikeCase-sumLogLikeAll  )/log(10.0))<<"\t"<<infoPPos[i].mapqAvg<<"\t"<<infoPPos[i].cov<<"\t"<<infoPPos[i].numDel<<"\t0.0\t0.0\t0.0\t0.0"<<endl;
		(*logToPrintC)<<(i+1)<<"\t"<<genomeRef[i]<<"\t"<<"D"<<"\t"<<(-10.0*(sumLogLikeCase-sumLogLikeAll )/log(10.0))<<"\t"<<infoPPos[i].mapqAvg<<"\t"<<infoPPos[i].cov<<"\t"<<infoPPos[i].numDel<<"\t0.0\t0.0\t0.0\t0.0"<<endl;
		
		PHREDgeno toadd;
		
		toadd.ref       = genomeRef[i];
		toadd.consensus = 'D';
		
		pos2phredgeno[   (i+1)   ] = toadd;
		
		//continue;
		skipEndo=true;
		skipCont=true;
	    }
	    

	    //deletion only in the endogenous, need to call the base for the contaminant
	    if( (infoPPos[i].llikDeletionEndo == maxAndSecondMax.first) &&
		((infoPPos[i].llikDeletionEndo - maxAndSecondMax.second) > LOGRATIOFORINDEL)){

		// cout<<"endo"<<endl;

		long double sumLogLikeCase=0.0;
		for(int n=0;n<4;n++){
		    if(n!=1)
			sumLogLikeCase =  oplusInit(sumLogLikeCase,logLikeDel[n]);
		}

		(*logToPrint)<<(i+1)<<"\t"<<genomeRef[i]<<"\t"<<"D"<<"\t"<<(-10.0*(sumLogLikeCase-sumLogLikeAll  )/log(10.0))<<"\t"<<infoPPos[i].mapqAvg<<"\t"<<infoPPos[i].cov<<"\t"<<infoPPos[i].numDel<<"\t0.0\t0.0\t0.0\t0.0"<<endl;
		//(*logToPrintD)<<(i+1)<<"\t"<<genomeRef[i]<<"\t"<<"D"<<"\t"<<(-10.0*(sumLogLikeCase-sumLogLikeAll )/log(10.0))<<"\t"<<infoPPos[i].mapqAvg<<"\t"<<infoPPos[i].cov<<"\t"<<infoPPos[i].numDel<<"\t0.0\t0.0\t0.0\t0.0"<<endl;
		
		PHREDgeno toadd;
		
		toadd.ref       = genomeRef[i];
		toadd.consensus = 'D';
		
		pos2phredgeno[   (i+1)   ] = toadd;
		
		//continue;
		skipEndo=true;
		skipCont=false;
	    }



	    //deletion only in the contaminant, need to call the base for the endogenous
	    if( (infoPPos[i].llikDeletionCont == maxAndSecondMax.first) &&
		((infoPPos[i].llikDeletionCont - maxAndSecondMax.second) > LOGRATIOFORINDEL)){
		// cout<<"cont"<<endl;

		long double sumLogLikeCase=0.0;
		for(int n=0;n<4;n++){
		    if(n!=2)
			sumLogLikeCase =  oplusInit(sumLogLikeCase,logLikeDel[n]);
		}

		//(*logToPrint)<<(i+1)<<"\t"<<genomeRef[i]<<"\t"<<"D"<<"\t"<<(-10.0*(sumLogLikeCase-sumLogLikeAll  )/log(10.0))<<"\t"<<infoPPos[i].mapqAvg<<"\t"<<infoPPos[i].cov<<"\t"<<infoPPos[i].numDel<<"\t0.0\t0.0\t0.0\t0.0"<<endl;
		(*logToPrintC)<<(i+1)<<"\t"<<genomeRef[i]<<"\t"<<"D"<<"\t"<<(-10.0*(sumLogLikeCase-sumLogLikeAll )/log(10.0))<<"\t"<<infoPPos[i].mapqAvg<<"\t"<<infoPPos[i].cov<<"\t"<<infoPPos[i].numDel<<"\t0.0\t0.0\t0.0\t0.0"<<endl;
		
		// PHREDgeno toadd;
		
		// toadd.ref       = genomeRef[i];
		// toadd.consensus = 'D';
		
		// pos2phredgeno[   (i+1)   ] = toadd;

		//continue;
		skipEndo=false;
		skipCont=true;
	    }





	}else{
	
	    // double llikDel  =0;
	    // double llikNoDel=0;
		
	    if( infoPPos[i].llikDeletion - infoPPos[i].llikNoDeletion > LOGRATIOFORINDEL){
		(*logToPrint)<<(i+1)<<"\t"<<genomeRef[i]<<"\t"<<"D"<<"\t"<<-10.0*( (infoPPos[i].llikNoDeletion - oplus(infoPPos[i].llikDeletion,infoPPos[i].llikNoDeletion))/log(10.0))<<"\t"<<infoPPos[i].mapqAvg<<"\t"<<infoPPos[i].cov<<"\t"<<infoPPos[i].numDel<<"\t0.0\t0.0\t0.0\t0.0"<<endl;

		PHREDgeno toadd;
		
		toadd.ref       = genomeRef[i];
		toadd.consensus = 'D';
		
		pos2phredgeno[   (i+1)   ] = toadd;
		
		//continue;
		skipEndo=true;
		skipCont=true;

	    }
	}
	//TODO add contamination detection
	     
    }

}


void noCoverage(const int i,
		const string & genomeRef,
		const vector<singlePosInfo> & infoPPos,
		const bool outLogCflag,
		string & genomeToPrint,
		string & genomeToPrintC,
		stringstream * logToPrint,
		stringstream * logToPrintC,
		bool & skipEndo,
		bool & skipCont){


    if(infoPPos[i].cov == 0){
	genomeToPrint+="N";
	(*logToPrint)<<(i+1)<<"\t"<<genomeRef[i]<<"\tN\t0\t0\t0\t0\t0.0\t0.0\t0.0\t0.0"<<endl;
	if(outLogCflag){
	    genomeToPrintC+="N";
	    (*logToPrintC)<<(i+1)<<"\t"<<genomeRef[i]<<"\tN\t0\t0\t0\t0\t0.0\t0.0\t0.0\t0.0"<<endl;	    
	}
	
	skipEndo=true;
	skipCont=true;	
    }

}


void callSingleNucleotide(const int i,
			  const string & genomeRef,
			  const vector<singlePosInfo> & infoPPos,
			  const bool singleCont,
			  const int minQual,			  
			  string & genomeToPrint,
			  string & genomeToPrintC,
			  stringstream * logToPrint,
			  stringstream * logToPrintC,
			  bool & skipEndo,
			  bool & skipCont){
    
    int    bestNuc=-1;
    int    bestNucC=-1;

    long double sumLogLikeAll           = 0.0; // sum of all the logs
    long double sumLogLikeOnlyBest      = 0.0; // sum of the logs for the best
    long double sumLogLikeAllButBest    = 0.0; // sum of the logs for all but the best
    long double sumLogLikeAllC          = 0.0; // sum of all the logs
    long double sumLogLikeOnlyBestC     = 0.0; // sum of the logs for the best
    long double sumLogLikeAllButBestC   = 0.0; // sum of the logs for all but the best

    // bool sumLogLikeAllB           = true;
    // bool sumLogLikeOnlyBestB      = true;
    // bool sumLogLikeAllButBestB    = true;
	 
    // int nuc=0;
    // sumLogLikeAll              =  infoPPos[i].likeBaseNoindel[nuc];  //oplus= log10( pow(10,x)+pow(10,y) )
    // if(nuc==bestNuc)
    //     sumLogLikeOnlyBest     =  infoPPos[i].likeBaseNoindel[nuc];
    // else
    //     sumLogLikeAllButBest   =  infoPPos[i].likeBaseNoindel[nuc];

    long double sumLogForNucs[4];  //sum of the logs for all but the nucleotide itself
    long double sumLogForNucsC[4]; //sum of the logs for all but the nucleotide itself

    long double likeBaseNoindel[4];  //log likelihood for each possible endogenous base
    long double likeBaseNoindelC[4]; //log likelihood for each possible endogenous base


    for(unsigned int nuc=0;nuc<4;nuc++){ 
	sumLogForNucs[nuc]     = 0.0;
	sumLogForNucsC[nuc]  = 0.0;
		  
	likeBaseNoindel[nuc]  = 0.0;
	likeBaseNoindelC[nuc] = 0.0;
    }



    if(singleCont){//if single contaminant need to marginalized over each contaminant
	     
	//Calling the endogenous base
	for(unsigned int nuce=0;nuce<4;nuce++){ //iterate over each possible endogenous base

		 
	    likeBaseNoindel[nuce]      = 0.0;

		 
	    for(unsigned int  nucc=0;nucc<4;nucc++){ //marginalize over each possible contaminant base A,C,G,T
		     		     
		//likeBaseNoindel[nuce]  +=  pow(10.0,infoPPos[i].likeBaseNoindelCont[nuce][nucc])*0.25;
		likeBaseNoindel[nuce]  =  oplusInit(likeBaseNoindel[nuce],
						    infoPPos[i].likeBaseNoindelCont[nuce][nucc] + log(0.25)/log(10) );
		// if(i==146)
		// 	 cout<<"E"<<(i+1)<<"\tllik\t"<<dnaAlphabet[nuce]<<"\t"<<dnaAlphabet[nucc]<<"\t"<<(infoPPos[i].likeBaseNoindelCont[nuce][nucc])<<"\t"<<likeBaseNoindel[nuce]<<endl;
	    }
	    // if(i==146)
	    //     cout<<"E"<<(i+1)<<"\tllik\t"<<dnaAlphabet[nuce]<<"\t"<<likeBaseNoindel[nuce]<<endl;

	    //likeBaseNoindel[nuce] = log(likeBaseNoindel[nuce])/log(10);
	}

	//Calling the contaminant base
	for(unsigned int nucc=0;nucc<4;nucc++){ //iterate over each possible contaminant base
		 
	    likeBaseNoindelC[nucc]      = 0.0;

	    for(unsigned int nuce=0;nuce<4;nuce++){ //iterate over each possible endogenous base C,G,T
		     
		//likeBaseNoindelC[nucc]  += pow(10.0,infoPPos[i].likeBaseNoindelCont[nuce][nucc])*0.25;
		likeBaseNoindelC[nucc]  = oplusInit(likeBaseNoindelC[nucc], infoPPos[i].likeBaseNoindelCont[nuce][nucc] + log(0.25)/log(10) );
		     
		// if(i==146)
		// 	 cout<<"C"<<(i+1)<<"\tllik\t"<<dnaAlphabet[nuce]<<"\t"<<dnaAlphabet[nucc]<<"\t"<<(infoPPos[i].likeBaseNoindelCont[nuce][nucc])<<"\t"<<likeBaseNoindelC[nucc]<<endl;
	    }
	    // if(i==146)
	    //     cout<<"C"<<(i+1)<<"\tllik\t"<<dnaAlphabet[nucc]<<"\t"<<likeBaseNoindelC[nucc]<<endl;
	    //likeBaseNoindelC[nucc] = log ( likeBaseNoindelC[nucc] )/log(10);
	}	     


	     
	     

    }else{

	for(unsigned int nuc=0;nuc<4;nuc++){ //iterate over each possible endogenous base
	    likeBaseNoindel[nuc] = infoPPos[i].likeBaseNoindel[nuc];
	}

    }


    //Begin determining best endogenous nucleotide

    callBestNucleotideGivenLikelihood( bestNuc,
				       sumLogLikeAll,          // sum of all the logs
				       sumLogLikeOnlyBest,     // sum of the logs for the best
				       sumLogLikeAllButBest,   // sum of the logs for all but the best
				       sumLogForNucs,
				       likeBaseNoindel,
				       infoPPos,
				       i );

    // if(i==146){
    //     for(unsigned int nuc=0;nuc<4;nuc++){ //iterate over each possible endogenous base
    // 	 cout<<"E\t"<<dnaAlphabet[nuc]<<"\tbest="<<dnaAlphabet[bestNuc]<<"\t"<<likeBaseNoindel[nuc]<<"\t"<<likeBaseNoindelC[nuc]<<endl;
    // 	 //likeBaseNoindel[nuc] = infoPPos[i].likeBaseNoindel[nuc];
    //     }		
    // }

    if(singleCont){//if single contaminant, call the contaminant

	callBestNucleotideGivenLikelihood( bestNucC,
					   sumLogLikeAllC,        // sum of all the logs
					   sumLogLikeOnlyBestC,   // sum of the logs for the best
					   sumLogLikeAllButBestC, // sum of the logs for all but the best
					   sumLogForNucsC,
					   likeBaseNoindelC,
					   infoPPos,
					   i );

	// if(i==146){
	// 	 for(unsigned int nuc=0;nuc<4;nuc++){ //iterate over each possible endogenous base
	// 	     cout<<"C\t"<<dnaAlphabet[nuc]<<"\tbest="<<dnaAlphabet[bestNucC]<<"\t"<<likeBaseNoindel[nuc]<<"\t"<<likeBaseNoindelC[nuc]<<endl;
	// 	     //likeBaseNoindel[nuc] = infoPPos[i].likeBaseNoindel[PHREDgeno];
	// 	 }		
	// }

    }

    // if(i==513){
    // 	cout<<"REF1 "<<pos2phredgeno[   (i+1)   ].ref<<"\t"<<pos2phredgeno[   (i+1)   ].consensus<<endl;
    // }



    
    if(!skipEndo  && !skipCont){  //need to define both

	PHREDgeno toadd;

	if( (-10.0*(sumLogLikeAllButBest-sumLogLikeAll)) >= minQual){
	    genomeToPrint+=dnaAlphabet[bestNuc];
	}else{
	    genomeToPrint+="N";
	}

	(*logToPrint)<<(i+1)<<"\t"<<genomeRef[i]<<"\t"<<dnaAlphabet[bestNuc]<<"\t"<<(-10*(sumLogLikeAllButBest-sumLogLikeAll)+0.0)<<"\t"<<infoPPos[i].mapqAvg<<"\t"<<infoPPos[i].cov<<"\t"<<infoPPos[i].covPerBase[bestNuc];//<<endl;


	for(int nuc=0;nuc<4;nuc++){
	    (*logToPrint)<<"\t"<<      (-10*(sumLogForNucs[nuc]-sumLogLikeAll));
	    toadd.phred[nuc]  =     (-10*(sumLogForNucs[nuc]-sumLogLikeAll));
	    toadd.perror[nuc] = pow(10.0,(sumLogForNucs[nuc]-sumLogLikeAll));
	}
	(*logToPrint)<<endl;
	toadd.ref       = genomeRef[i];
	toadd.consensus = dnaAlphabet[bestNuc];



	if(singleCont){//if single contaminant, call the contaminant
		
	    if( (-10.0*(sumLogLikeAllButBestC-sumLogLikeAllC)) >= minQual){
		genomeToPrintC+=dnaAlphabet[bestNucC];
	    }else{
		genomeToPrintC+="N";
	    }

	    (*logToPrintC)<<(i+1)<<"\t"<<genomeRef[i]<<"\t"<<dnaAlphabet[bestNucC]<<"\t"<<(-10*(sumLogLikeAllButBestC-sumLogLikeAllC)+0.0)<<"\t"<<infoPPos[i].mapqAvg<<"\t"<<infoPPos[i].cov<<"\t"<<infoPPos[i].covPerBase[bestNucC];//<<endl;
	 
	    for(int nuc=0;nuc<4;nuc++){
		(*logToPrintC)<<"\t"<<      (-10*(sumLogForNucsC[nuc]-sumLogLikeAllC));
		toadd.phredC[nuc]  =     (-10*(sumLogForNucsC[nuc]-sumLogLikeAllC));
		toadd.perrorC[nuc] = pow(10.0,(sumLogForNucsC[nuc]-sumLogLikeAllC));
	    }
	    (*logToPrintC)<<endl;
	}
	
	pos2phredgeno[   (i+1)   ] = toadd;

    }

    
    if(skipEndo  && !skipCont){  //need not to define contamination

	if(singleCont){//if single contaminant, call the contaminant
		
	    if( (-10.0*(sumLogLikeAllButBestC-sumLogLikeAllC)) >= minQual){
		genomeToPrintC+=dnaAlphabet[bestNucC];
	    }else{
		genomeToPrintC+="N";
	    }

	    (*logToPrintC)<<(i+1)<<"\t"<<genomeRef[i]<<"\t"<<dnaAlphabet[bestNucC]<<"\t"<<(-10*(sumLogLikeAllButBestC-sumLogLikeAllC)+0.0)<<"\t"<<infoPPos[i].mapqAvg<<"\t"<<infoPPos[i].cov<<"\t"<<infoPPos[i].covPerBase[bestNucC];//<<endl;
	 
	    for(int nuc=0;nuc<4;nuc++){
		(*logToPrintC)<<"\t"<<      (-10*(sumLogForNucsC[nuc]-sumLogLikeAllC));
		pos2phredgeno[   (i+1)   ].phredC[nuc]  =     (-10*(sumLogForNucsC[nuc]-sumLogLikeAllC));
		pos2phredgeno[   (i+1)   ].perrorC[nuc] = pow(10.0,(sumLogForNucsC[nuc]-sumLogLikeAllC));
	    }
	    (*logToPrintC)<<endl;
	}

    }

    
    if(!skipEndo  && skipCont){  //need not to define endogenous
	
	PHREDgeno toadd;

	if( (-10.0*(sumLogLikeAllButBest-sumLogLikeAll)) >= minQual){
	    genomeToPrint+=dnaAlphabet[bestNuc];
	}else{
	    genomeToPrint+="N";
	}

	(*logToPrint)<<(i+1)<<"\t"<<genomeRef[i]<<"\t"<<dnaAlphabet[bestNuc]<<"\t"<<(-10*(sumLogLikeAllButBest-sumLogLikeAll)+0.0)<<"\t"<<infoPPos[i].mapqAvg<<"\t"<<infoPPos[i].cov<<"\t"<<infoPPos[i].covPerBase[bestNuc];//<<endl;


	for(int nuc=0;nuc<4;nuc++){
	    (*logToPrint)<<"\t"<<      (-10*(sumLogForNucs[nuc]-sumLogLikeAll));
	    toadd.phred[nuc]  =     (-10*(sumLogForNucs[nuc]-sumLogLikeAll));
	    toadd.perror[nuc] = pow(10.0,(sumLogForNucs[nuc]-sumLogLikeAll));
	}
	(*logToPrint)<<endl;
	toadd.ref       = genomeRef[i];
	toadd.consensus = dnaAlphabet[bestNuc];

	pos2phredgeno[   (i+1)   ] = toadd;

    }



    // if(i==513){
    // 	cout<<"REF2 "<<pos2phredgeno[   (i+1)   ].ref<<"\t"<<pos2phredgeno[   (i+1)   ].consensus<<endl;
    // }



    // cout<<(i+1)<<"\t"<<toadd.ref<<"\t"<<toadd.consensus<<endl;

}
				
void  printLogAndGenome(const int sizeGenome,
			const vector<singlePosInfo> & infoPPos,
			const string outSeq,
			const string outLog, 
			const string genomeRef,
			const int minQual,
			const string nameMT,
			const bool singleCont,
			const string outSeqC,
			const string outLogC,
			const bool   outSeqCflag,
			const bool   outLogCflag,
			const string nameMTC){
    // cerr<<outSeq<<"\t"<<outLog<<"\t#"<<outSeqC<<"#\t"<<outLogC<<"#\t"<<outSeqCflag<<"#\t"<<outLogCflag<<"#\t"<<endl;
    // exit(1);
    ofstream outSeqFP ;
    ofstream outLogFP;

    ofstream outSeqFPC;
    ofstream outLogFPC;


    outSeqFP.open(outSeq.c_str());

    if (!outSeqFP.is_open()){
	cerr << "Unable to write to seq file "<<outSeq<<endl;
	exit(1);
    }

    outLogFP.open(outLog.c_str());

    if (!outLogFP.is_open()){
	cerr << "Unable to write to qual file "<<outLog<<endl;
	exit(1);
    }


    if(outSeqCflag){
	outSeqFPC.open(outSeqC.c_str());
	
	if (!outSeqFPC.is_open()){
	    cerr << "Unable to write to seq file "<<outSeqC<<endl;
	    exit(1);
	}
    }

    if(outLogCflag){
	outLogFPC.open(outLogC.c_str());

	if (!outLogFPC.is_open()){
	    cerr << "Unable to write to qual file "<<outLogC<<endl;
	    exit(1);
	}

    }

    string genomeToPrint="";
    string genomeToPrintC="";

    stringstream logToPrint;
    stringstream logToPrintC;


    logToPrint<<"pos\trefBase\tbase\tqual\tavgmapq\tcov\tsupp\tpa\tpc\tpg\tpt\n";
    if(outLogCflag){
	logToPrintC<<"pos\trefBase\tbase\tqual\tavgmapq\tcov\tsupp\tpa\tpc\tpg\tpt\n";
    }

    //genomeRef
    for(int i=0;i<sizeGenome;i++){
	bool skipEndo=false;
	bool skipCont=false;

	//There are 4 possibilities:
	//1) There is a deletion in the sample (or insertion in the reference)
	//2) There is no coverage
	//3) There are no bases
	//4) There is a insertion in the sample (or deletion in the reference)

	


	 /////////////////////////////////////////
	 //                                     //
	 //        Deletion in the sample       //
	 //                                     //
	 /////////////////////////////////////////
	deletionInSample(i,
			 genomeRef,
			 infoPPos,
			 singleCont,
			 &logToPrint,
			 &logToPrintC,
			 skipEndo,
			 skipCont);
	    
	if(skipCont && skipEndo)
	    continue;

	/////////////////////////////////////////
	//                                     //
	//            No coverage              //
	//                                     //
	/////////////////////////////////////////

	noCoverage(i,
		   genomeRef,
		   infoPPos,
		   outLogCflag,	
		   genomeToPrint,
		   genomeToPrintC,
		   &logToPrint,
		   &logToPrintC,
		   skipEndo,
		   skipCont);
	if(skipCont && skipEndo)
	    continue;
	     

	 /////////////////////////////////////////
	 //                                     //
	 //      Calling single nucleotides     //
	 //                                     //
	 /////////////////////////////////////////

	callSingleNucleotide(i,
			     genomeRef,
			     infoPPos,
			     singleCont,
			     minQual,
			     genomeToPrint,
			     genomeToPrintC,
			     &logToPrint,
			     &logToPrintC,
			     skipEndo,
			     skipCont);





	 /////////////////////////////////////////
	 //                                     //
	 //      Insertions in the sample       //
	 //                                     //
	 /////////////////////////////////////////

	insertionInSample(i,
			  genomeRef,
			  infoPPos,
			  singleCont,
			  genomeToPrint,
			  genomeToPrintC,
			  &logToPrint,
			  &logToPrintC);


    }//end for each position in the genome


     string genomeToPrintCopy="";
     for(unsigned int i=1;i<(genomeToPrint.size()+1);i++){
	 genomeToPrintCopy+=genomeToPrint[i-1];
	 if(i!=0 && (i%80 == 0))
	     genomeToPrintCopy+="\n";
     }

     string genomeToPrintCopyC="";
     for(unsigned int i=1;i<(genomeToPrintC.size()+1);i++){
	 genomeToPrintCopyC+=genomeToPrintC[i-1];
	 if(i!=0 && (i%80 == 0))
	     genomeToPrintCopyC+="\n";
     }



     outSeqFP<<nameMT+"\n"+genomeToPrintCopy+"\n";
     outSeqFP.close() ;

     if(outSeqCflag)
     if(singleCont){//if single contaminant need to marginalized over each contaminant
	 outSeqFPC<<nameMTC+"\n"+genomeToPrintCopyC+"\n";
	 outSeqFPC.close() ;
     }

     outLogFP<<logToPrint.str()<<endl;
     outLogFP.close();

     if(outLogCflag)
     if(singleCont){//if single contaminant need to marginalized over each contaminant
	 outLogFPC<<logToPrintC.str()<<endl;
	 outLogFPC.close();
     }
}








//unsigned int posFound;
//vector<positionInfo> * positionsInfoFound;
// map<string, map<unsigned int,contaminationInfo> > contFreq;

class MyPileupVisitor : public PileupVisitor {
  
    public:
    MyPileupVisitor(const RefVector& references,
		    Fasta * fastaReference,
		    vector<singlePosInfo> * infoPPos,
		    const int sizeGenome,
		    const bool ignoreMQ,
		    const long double contaminationPrior,
		    const bool singleCont)
            : PileupVisitor()
	    , m_references(references)
	    , m_fastaReference(fastaReference)
	    , m_infoPPos(infoPPos)
	    , sizeGenome(sizeGenome)
	    , ignoreMQ(ignoreMQ)
	    , contaminationPrior(contaminationPrior)
	    , singleCont(singleCont)
        { 

	}
        ~MyPileupVisitor(void) { }
  
    // PileupVisitor interface implementation
    public:
	// prints coverage results ( tab-delimited )
        void Visit(const PileupPosition& pileupData) {
	    //cout<<"visit"<<endl;
	    char referenceBase = 'N';
	    
	    unsigned int posAlign = pileupData.Position+1;
	    int posVector=int(pileupData.Position)%sizeGenome;
	    

	    // if( (posAlign%100) == 0){
	    // 	cerr<<"pos  = "<<posAlign<<endl;
	    // }
	    //cout<<endl<<"pos = "<<posAlign<<"\t"<<posVector;
	    

	    //for some reason, we have to do -1 on the .Position
	    if ( !m_fastaReference->GetBase(pileupData.RefId, posAlign-1, referenceBase ) ) {
		cerr << "bamtools convert ERROR: pileup conversion - could not read reference base from FASTA file at chr "<<pileupData.RefId<<" position "<<(posAlign-1) << endl;
		exit(1);
	    }


	    //cout<<"\t"<<referenceBase<<"\t"<<endl;


	    for(unsigned int i=0;i<pileupData.PileupAlignments.size();i++){
		m_infoPPos->at(posVector).cov++;

		//Add mapq
		m_infoPPos->at(posVector).mapqAvg += pow(10.0, (long double)(pileupData.PileupAlignments[i].Alignment.MapQuality)/-10.0);

		// cout<<pileupData.PileupAlignments[i].Alignment.Name<<"\t"<<
		//     pileupData.PileupAlignments[i].IsCurrentDeletion<<"\t"<<
		//     pileupData.PileupAlignments[i].IsNextDeletion<<"\t"<<
		//     pileupData.PileupAlignments[i].IsNextInsertion<<"\t"<<
		//     pileupData.PileupAlignments[i].DeletionLength<<"\t"<<
		//     pileupData.PileupAlignments[i].InsertionLength<<"\t"<<
		//     pileupData.PileupAlignments[i].IsSegmentBegin<<"\t"<<
		//     pileupData.PileupAlignments[i].IsSegmentEnd<<"\t"<<
		//     endl;
	    }

	    m_infoPPos->at(posVector).mapqAvg = m_infoPPos->at(posVector).mapqAvg/(long double)(m_infoPPos->at(posVector).cov);
	    m_infoPPos->at(posVector).mapqAvg = -10.0*( log( m_infoPPos->at(posVector).mapqAvg )/log(10.0) );




	    //There are 3 possibilities:
	    //1) There is a insertion in the sample (or deletion in the reference)
	    //2) There is a deletion in the sample (or insertion in the reference)
	    //3) There is potentially a variation of a single nucleotide
	    	    


	    /////////////////////////////////////////////////////
	    //insertion in the reads/deletion in the reference
	    ///////////////////////////////////////////////////
	    //detecting to find all possible insertions
	    // set<string> allInsert;
	    for(unsigned int i=0;i<pileupData.PileupAlignments.size();i++){				

		if( !pileupData.PileupAlignments[i].IsCurrentDeletion &&
		    pileupData.PileupAlignments[i].IsNextInsertion &&
		    (pileupData.PileupAlignments[i].InsertionLength>0)){ //has insertion
		    string insert="";
		    for(int del=1;del<=pileupData.PileupAlignments[i].InsertionLength;del++){
			insert+=  pileupData.PileupAlignments[i].Alignment.QueryBases[ pileupData.PileupAlignments[i].PositionInAlignment+del ];
		    }
		    m_infoPPos->at(posVector).allInserts.insert(insert);
		    
		    m_infoPPos->at(posVector).insertionRight.push_back(insert);
		    m_infoPPos->at(posVector).insertion2count[insert]++;
		} //else{
		// string insert="";
		// m_infoPPos->at(posVector).allInserts.insert(insert);
		    //}
	    }
	    //entering an "null" model of not having an insertion
	    m_infoPPos->at(posVector).allInserts.insert("");
	    
	    //init likelihood for all possible pairs of inserts
	    for(set<string>::const_iterator it1 = m_infoPPos->at(posVector).allInserts.begin(); 
		it1 != m_infoPPos->at(posVector).allInserts.end(); 
		++it1) {
		for(set<string>::const_iterator it2 = m_infoPPos->at(posVector).allInserts.begin(); 
		    it2 != m_infoPPos->at(posVector).allInserts.end(); 
		    ++it2) {
		    pair<string,string> keytouse (*it1,*it2);
		    m_infoPPos->at(posVector).insertion2loglikeEndoCont[ keytouse ] = 0.0;
		}
	    }



	    //re-iterate over each read
	    for(unsigned int i=0;i<pileupData.PileupAlignments.size();i++){				
		int  m   = int(pileupData.PileupAlignments[i].Alignment.MapQuality);
		long double probEndogenous=1.0-contaminationPrior;
		if(read2endoProbInit){ //include probability of endogenous		    
		    map<string,long double>::iterator itRead2endoProb = read2endoProb.find( pileupData.PileupAlignments[i].Alignment.Name+
											    "#"+ 
											    stringify(pileupData.PileupAlignments[i].Alignment.AlignmentFlag) );
		    
		    
		    if( itRead2endoProb == read2endoProb.end() ){     // skipped due to deletions near the end or something			    
			probEndogenous = 1.0-contaminationPrior;      // assume that a read is equally to belong to either the endo or the contaminant
		    }else{
			probEndogenous = itRead2endoProb->second;
		    }
		}


		
		if( !pileupData.PileupAlignments[i].IsCurrentDeletion &&
		    pileupData.PileupAlignments[i].IsNextInsertion &&
		    (pileupData.PileupAlignments[i].InsertionLength>0)){ //has insertion
		    
		    string insert="";
		    for(int del=1;del<=pileupData.PileupAlignments[i].InsertionLength;del++){
			insert+=  pileupData.PileupAlignments[i].Alignment.QueryBases[ pileupData.PileupAlignments[i].PositionInAlignment+del ];
		    }
		    //cout<<"ins "<<insert<<endl;
		    
		    
		    if(singleCont){
			// m_infoPPos->at(posVector).insertion2loglike[insert]     += log( (    probEndogenous)*probMapping[m]*(    INDELERRORPROB))/log(10);
			// m_infoPPos->at(posVector).insertion2loglikeCont[insert] += log( (1.0-probEndogenous)*probMapping[m]*(    INDELERRORPROB))/log(10);
			//both endo and cont have the insert, both right
			if(1){
			    pair<string,string> keytouse (insert,insert);
			    m_infoPPos->at(posVector).insertion2loglikeEndoCont [ keytouse ] += 
				log( (           probEndogenous)*probMapping[m]*(1.0-INDELERRORPROB) + (1.0-probEndogenous)*probMapping[m]*(1.0-INDELERRORPROB)  )/log(10);
			}
			//only endo has the insert, endo has it right, cont has is wrong
			for(set<string>::const_iterator it = m_infoPPos->at(posVector).allInserts.begin(); 
			    it != m_infoPPos->at(posVector).allInserts.end(); 
			    ++it) {
			    if( *it != insert){
				pair<string,string> keytouse (insert,*it);
				m_infoPPos->at(posVector).insertion2loglikeEndoCont [ keytouse ] += 
				    log( (   probEndogenous)*probMapping[m]*(1.0-INDELERRORPROB) + (1.0-probEndogenous)*probMapping[m]*(    INDELERRORPROB)  )/log(10);
			    }
			    
			}
			
			//only cont has the insert, endo has it wrong, cont has is cont
			for(set<string>::const_iterator it = m_infoPPos->at(posVector).allInserts.begin(); 
			    it != m_infoPPos->at(posVector).allInserts.end(); 
			    ++it) {
			    if( *it != insert){
				pair<string,string> keytouse (*it   ,insert);
				m_infoPPos->at(posVector).insertion2loglikeEndoCont [ keytouse ] += 
				    log( (   probEndogenous)*probMapping[m]*(    INDELERRORPROB) + (1.0-probEndogenous)*probMapping[m]*(1.0-INDELERRORPROB)  )/log(10);
			    }
			    
			}
			
			//none have the insert, both have it wrong
			for(set<string>::const_iterator it1 = m_infoPPos->at(posVector).allInserts.begin(); 
			    it1 != m_infoPPos->at(posVector).allInserts.end(); 
			    ++it1) {
			    if( *it1 != insert){
				for(set<string>::const_iterator it2 = m_infoPPos->at(posVector).allInserts.begin(); 
				    it2 != m_infoPPos->at(posVector).allInserts.end(); 
				    ++it2) {
				    if( *it2 != insert){
					pair<string,string> keytouse (*it1  ,*it2);
					m_infoPPos->at(posVector).insertion2loglikeEndoCont [ keytouse ] += 
					    log((probEndogenous)*probMapping[m]*(INDELERRORPROB) + (1.0-probEndogenous)*probMapping[m]*(    INDELERRORPROB)  )/log(10);
				    }
				}
			    }
			}
			
			
		    }else{ //not single cont
			//got it right for the insert
		      m_infoPPos->at(posVector).insertion2loglike[insert]         += log( (    probEndogenous)*probMapping[m]*(1.0-INDELERRORPROB))/log(10);
		      //The remaining insertions have it wrong
		      for(set<string>::const_iterator it = m_infoPPos->at(posVector).allInserts.begin(); 
			  it != m_infoPPos->at(posVector).allInserts.end(); 
			  ++it) {
			if( *it != insert)
			  m_infoPPos->at(posVector).insertion2loglike[*it]    += log( (    probEndogenous)*probMapping[m]*(    INDELERRORPROB))/log(10);
		      }
		    }
		    
		}else{ //does not have insert but bases
		    string insert="";
		    //push none
		    if(singleCont){
			// m_infoPPos->at(posVector).insertion2loglike[""]         += log( (    probEndogenous)*probMapping[m]*(    INDELERRORPROB))/log(10);
			// m_infoPPos->at(posVector).insertion2loglikeCont[""]     += log( (1.0-probEndogenous)*probMapping[m]*(    INDELERRORPROB))/log(10);
			//both endo and cont have the insert, both right
			if(1){
			    pair<string,string> keytouse (insert,insert);
			    m_infoPPos->at(posVector).insertion2loglikeEndoCont [ keytouse ] += 
				log( (           probEndogenous)*probMapping[m]*(1.0-INDELERRORPROB) + (1.0-probEndogenous)*probMapping[m]*(1.0-INDELERRORPROB)  )/log(10);
			}
			//only endo has the insert, endo has it right, cont has is wrong
			for(set<string>::const_iterator it = m_infoPPos->at(posVector).allInserts.begin(); 
			    it != m_infoPPos->at(posVector).allInserts.end(); 
			    ++it) {
			    if( *it != insert){
				pair<string,string> keytouse (insert,*it);
				m_infoPPos->at(posVector).insertion2loglikeEndoCont [ keytouse ] += 
				    log( (   probEndogenous)*probMapping[m]*(1.0-INDELERRORPROB) + (1.0-probEndogenous)*probMapping[m]*(    INDELERRORPROB)  )/log(10);
			    }

			}
			
			//only cont has the insert, endo has it wrong, cont has is cont
			for(set<string>::const_iterator it = m_infoPPos->at(posVector).allInserts.begin(); 
			    it != m_infoPPos->at(posVector).allInserts.end(); 
			    ++it) {
			    if( *it != insert){
				pair<string,string> keytouse (*it   ,insert);
				m_infoPPos->at(posVector).insertion2loglikeEndoCont [ keytouse ] += 
				    log( (   probEndogenous)*probMapping[m]*(    INDELERRORPROB) + (1.0-probEndogenous)*probMapping[m]*(1.0-INDELERRORPROB)  )/log(10);
			    }
			    
			}
			
			//none have the insert, both have it wrong
			for(set<string>::const_iterator it1 = m_infoPPos->at(posVector).allInserts.begin(); 
			    it1 != m_infoPPos->at(posVector).allInserts.end(); 
			    ++it1) {
			    if( *it1 != insert){
				for(set<string>::const_iterator it2 = m_infoPPos->at(posVector).allInserts.begin(); 
				    it2 != m_infoPPos->at(posVector).allInserts.end(); 
				    ++it2) {
				    if( *it2 != insert){
					pair<string,string> keytouse (*it1  ,*it2);
					m_infoPPos->at(posVector).insertion2loglikeEndoCont [ keytouse ] += 
					    log((probEndogenous)*probMapping[m]*(INDELERRORPROB) + (1.0-probEndogenous)*probMapping[m]*(    INDELERRORPROB)  )/log(10);
				    }
				}
			    }
			}
			
			
		    }else{ //not single cont
			//got it right for the insert
			m_infoPPos->at(posVector).insertion2loglike[insert]         += log( (    probEndogenous)*probMapping[m]*(1.0-INDELERRORPROB))/log(10);
			//The remaining insertions have it wrong
			for(set<string>::const_iterator it = m_infoPPos->at(posVector).allInserts.begin(); 
			    it != m_infoPPos->at(posVector).allInserts.end(); 
			    ++it) {
			    if( *it != insert)
				m_infoPPos->at(posVector).insertion2loglike[*it]    += log( (    probEndogenous)*probMapping[m]*(    INDELERRORPROB))/log(10);
			}
		    }
		    
		}//end, no insert
	    }//end for each read
	





	    // for(map<int,int>::iterator it = test2test.begin(); 
	    // 	it != test2test.end(); 
	    // 	++it) {

	    // m_infoPPos->at(posVector).llikNoDeletion       += log( (    probEndogenous)*probMapping[m]*(1.0-INDELERRORPROB))/log(10); //got it right

	    /////////////////////////////////////////////////////
	    //deletion in the reads/insertion in the reference
	    /////////////////////////////////////////////////////
	    for(unsigned int i=0;i<pileupData.PileupAlignments.size();i++){		
		int  m   = int(pileupData.PileupAlignments[i].Alignment.MapQuality);
		long double probEndogenous=1.0-contaminationPrior;
		string rg;
		pileupData.PileupAlignments[i].Alignment.GetTag("RG",rg);//REMOVE ME
		if(read2endoProbInit){ //include probability of endogenous
		    
		    map<string,long double>::iterator itRead2endoProb = read2endoProb.find( pileupData.PileupAlignments[i].Alignment.Name+
											    "#"+ 
											    stringify(pileupData.PileupAlignments[i].Alignment.AlignmentFlag) );
		    
		    
		    if( itRead2endoProb == read2endoProb.end() ){     // skipped due to deletions near the end or something			    
			probEndogenous = 1.0-contaminationPrior;      // assume that a read is equally to belong to either the endo or the contaminant
		    }else{
			probEndogenous = itRead2endoProb->second;
		    }
		}

		// if(posVector>=513 && posVector<=516){
		//     cout<<"pos ="<<posVector<<"\t"<<rg<<endl;
		// }
		
		if( pileupData.PileupAlignments[i].IsCurrentDeletion &&
		    pileupData.PileupAlignments[i].IsNextInsertion &&
		    (pileupData.PileupAlignments[i].InsertionLength == 0)){ //has deletion
		    //continue;
		    //cout<<"del"<<endl;
		    
		    // if(posVector>=513 && posVector<=516){
		    // 	cout<<"pos ="<<posVector<<"\tdel\t"<<rg<<endl;
		    // }

		    m_infoPPos->at(posVector).numDel++;

		    if(singleCont){
			// m_infoPPos->at(posVector).llikDeletion         += log( (    probEndogenous)*probMapping[m]*(1.0-INDELERRORPROB))/log(10); //got it right
			// m_infoPPos->at(posVector).llikNoDeletion       += log( (    probEndogenous)*probMapping[m]*(    INDELERRORPROB))/log(10); //got it wrong
			// m_infoPPos->at(posVector).llikDeletionCont     += log( (1.0-probEndogenous)*probMapping[m]*(1.0-INDELERRORPROB))/log(10); //got it right	
			// m_infoPPos->at(posVector).llikNoDeletionCont   += log( (1.0-probEndogenous)*probMapping[m]*(    INDELERRORPROB))/log(10); //got it wrong	

			//there is a deletion in both, got it right 2x
			m_infoPPos->at(posVector).llikDeletionBoth += log( probEndogenous*probMapping[m]*(1.0-INDELERRORPROB) +  (1.0-probEndogenous)*probMapping[m]*(1.0-INDELERRORPROB)  )/log(10); 
			//only in endo, right if endo, wrong if cont
			m_infoPPos->at(posVector).llikDeletionEndo += log( probEndogenous*probMapping[m]*(1.0-INDELERRORPROB) +  (1.0-probEndogenous)*probMapping[m]*(    INDELERRORPROB)  )/log(10); 
			//only in cont, wrong if endo, right if cont
			m_infoPPos->at(posVector).llikDeletionCont += log( probEndogenous*probMapping[m]*(    INDELERRORPROB) +  (1.0-probEndogenous)*probMapping[m]*(1.0-INDELERRORPROB)  )/log(10); 
			//none have it, wrong in both
			m_infoPPos->at(posVector).llikDeletionNone += log( probEndogenous*probMapping[m]*(    INDELERRORPROB) +  (1.0-probEndogenous)*probMapping[m]*(    INDELERRORPROB)  )/log(10); 

		    }else{
			m_infoPPos->at(posVector).llikDeletion         += log( (    probEndogenous)*probMapping[m]*(1.0-INDELERRORPROB))/log(10); //got it right
			m_infoPPos->at(posVector).llikNoDeletion       += log( (    probEndogenous)*probMapping[m]*(    INDELERRORPROB))/log(10); //got it wrong
		    }
		}else{ //does not have deletion

		    // if(posVector>=513 && posVector<=516){
		    // 	cout<<"pos ="<<posVector<<"\tnodel\t"<<rg<<endl;
		    // }

		    if(singleCont){
			//there is a deletion in both, got it wrong 2x
			m_infoPPos->at(posVector).llikDeletionBoth += log( probEndogenous*probMapping[m]*(    INDELERRORPROB) +  (1.0-probEndogenous)*probMapping[m]*(    INDELERRORPROB)  )/log(10); 
			//only in endo, wrong in endo, right in cont
			m_infoPPos->at(posVector).llikDeletionEndo += log( probEndogenous*probMapping[m]*(    INDELERRORPROB) +  (1.0-probEndogenous)*probMapping[m]*(1.0-INDELERRORPROB)  )/log(10); 
			//only in cont, right in endo, wrong in cont
			m_infoPPos->at(posVector).llikDeletionCont += log( probEndogenous*probMapping[m]*(1.0-INDELERRORPROB) +  (1.0-probEndogenous)*probMapping[m]*(    INDELERRORPROB)  )/log(10); 
			//none have it, right in both
			m_infoPPos->at(posVector).llikDeletionNone += log( probEndogenous*probMapping[m]*(1.0-INDELERRORPROB) +  (1.0-probEndogenous)*probMapping[m]*(1.0-INDELERRORPROB)  )/log(10); 

			// m_infoPPos->at(posVector).llikDeletion         += log( (    probEndogenous)*probMapping[m]*(    INDELERRORPROB))/log(10);   //got it wrong
			// m_infoPPos->at(posVector).llikNoDeletion       += log( (    probEndogenous)*probMapping[m]*(1.0-INDELERRORPROB))/log(10);   //got it right
			// m_infoPPos->at(posVector).llikDeletionCont     += log( (1.0-probEndogenous)*probMapping[m]*(    INDELERRORPROB))/log(10);   //got it wrong
			// m_infoPPos->at(posVector).llikNoDeletionCont   += log( (1.0-probEndogenous)*probMapping[m]*(1.0-INDELERRORPROB))/log(10);   //got it right

		    }else{
			m_infoPPos->at(posVector).llikDeletion         += log( (    probEndogenous)*probMapping[m]*(    INDELERRORPROB))/log(10); //got it wrong
			m_infoPPos->at(posVector).llikNoDeletion       += log( (    probEndogenous)*probMapping[m]*(1.0-INDELERRORPROB))/log(10); //got it right
		    }
		}
	    }
	    





	    /////////////////////////////////////////////////////
	    //       Variations of a single nucleotide         //
	    /////////////////////////////////////////////////////
	    for(unsigned int i=0;i<pileupData.PileupAlignments.size();i++){		 //for each alignment
		unsigned int numReads=0;
		    
		numReads++;
		if(numReads >= MAXCOV){
		    break;
		}
		
		//skip deletion in the reads/insertion in the reference
		if( pileupData.PileupAlignments[i].IsCurrentDeletion &&
		    pileupData.PileupAlignments[i].IsNextInsertion ){
		    continue;
		}
		
		//base that was read
		char b   = pileupData.PileupAlignments[i].Alignment.QueryBases[pileupData.PileupAlignments[i].PositionInAlignment];
		//quality score
		char q   = pileupData.PileupAlignments[i].Alignment.Qualities[pileupData.PileupAlignments[i].PositionInAlignment]-offsetQual;
		int  m   = int(pileupData.PileupAlignments[i].Alignment.MapQuality);
		
		//skip unresolved
		if(b == 'N')
		    continue;
		// if(posVector== 145){
		//     cout<<b<<endl;
		// }

		// BEGIN DEAMINATION COMPUTATION
		//zero base distance to the 5p/3p end
		int dist5p=-1;
		int dist3p=-1;

		if( pileupData.PileupAlignments[i].Alignment.IsReverseStrand() ){
		    dist5p = pileupData.PileupAlignments[i].Alignment.QueryBases.size() - pileupData.PileupAlignments[i].PositionInAlignment-1;
		    dist3p = pileupData.PileupAlignments[i].PositionInAlignment;
		}else{
		    dist5p = pileupData.PileupAlignments[i].PositionInAlignment;
		    dist3p = pileupData.PileupAlignments[i].Alignment.QueryBases.size() - pileupData.PileupAlignments[i].PositionInAlignment-1;
		}
		    		    
		probSubstition * probSubMatchToUseEndo = &defaultSubMatch ;
		probSubstition * probSubMatchToUseCont = &defaultSubMatch ;

		// for(int jas=0;jas<16;jas++){
		// 	cout<<jas<<"\t"<<probSubMatchToUse->s[jas]<<endl;
		// }
		// cout<<"sub\t"<<dist5p<<"\t"<<dist3p<<"\t"<<int(sub5p.size())<< "\t"<<int(sub3p.size()) <<endl;

		if(dist5p <= (int(sub5p.size()) -1)){
		    probSubMatchToUseEndo = &sub5p[ dist5p ];			
		    // cout<<"5p sub"<<endl;
		}
		    
		if(dist3p <= (int(sub3p.size()) -1)){
		    probSubMatchToUseEndo = &sub3p[ dist3p ];
		    // cout<<"3p sub"<<endl;
		}
		    
		//we have substitution probabilities for both... take the closest
		if(dist5p <= (sub5p.size() -1) &&
		   dist3p <= (sub3p.size() -1) ){
		    
		    if(dist5p < dist3p){
			probSubMatchToUseEndo = &sub5p[ dist5p ];
			// cout<<"5p sub"<<endl;
		    }else{
			probSubMatchToUseEndo = &sub3p[ dist3p ];
			// cout<<"3p sub"<<endl;
		    }
		    
		}
		// END DEAMINATION COMPUTATION

		long double probEndogenous=1.0-contaminationPrior;
		if(read2endoProbInit){ //include probability of endogenous

		    map<string,long double>::iterator itRead2endoProb = read2endoProb.find( pileupData.PileupAlignments[i].Alignment.Name+
											    "#"+ 
											    stringify(pileupData.PileupAlignments[i].Alignment.AlignmentFlag) );
		    

		    if( itRead2endoProb == read2endoProb.end() ){     // skipped due to deletions near the end or something			    
			probEndogenous = 1.0-contaminationPrior;      // assume that a read is equally to belong to either the endo or the contaminant
		    }else{
			probEndogenous = itRead2endoProb->second;
		    }
		}






		if(singleCont){ // we consider a single contaminant

		    for(unsigned int nuce=0;nuce<4;nuce++){ //iterate over each possible endogenous base

			char currentNuc=dnaAlphabet[nuce];
			

			if(currentNuc == b){//match
			    m_infoPPos->at(posVector).covPerBase[nuce]++;			
			}else{//mismatch
			}
			
			// b   is the observed
			// nuce is the model for the endogenous base
			int dinucIndexe;//The index depends on the strand
			if( pileupData.PileupAlignments[i].Alignment.IsReverseStrand() ){
			    dinucIndexe= (3-nuce)*4+baseResolved2int(complement(b));
			}else{
			    dinucIndexe=    nuce *4+baseResolved2int(b);
			}

		        // probability of generating the base for the endogenous
			//                          (1-e)          *  p(sub|1-e)                               + (         e                ) *  p(sub|1-e)
			long double probBasee =  likeMatchProb[int(q)]  * (probSubMatchToUseEndo->s[dinucIndexe] )  + (1.0 - likeMatchProb[int(q)])*(illuminaErrorsProb.s[dinucIndexe]);

			for(unsigned int nucc=0;nucc<4;nucc++){ //iterate over each possible contaminant base
			    // b   is the observed
			    // nucc is the model for the endogenous base
			    int dinucIndexc;//The index depends on the strand
			    if( pileupData.PileupAlignments[i].Alignment.IsReverseStrand() ){
				dinucIndexc= (3-nucc)*4+baseResolved2int(complement(b));
			    }else{
				dinucIndexc=    nucc *4+baseResolved2int(b);
			    }

			    // probability of generating the base for the contaminant
			    // The substitution probability is probSubMatchToUseCont 
			    // which is simply the Illumina sub probability
			    //                          (1-e)          *  p(sub|1-e)                               + (         e                ) *  p(sub|1-e)
			    long double probBasec =  likeMatchProb[int(q)]  * (probSubMatchToUseCont->s[dinucIndexc] )  + (1.0 - likeMatchProb[int(q)])*(illuminaErrorsProb.s[dinucIndexc]);

			    
			    long double probBase = ( probEndogenous )*(probBasee)  + ( 1.0-probEndogenous )*(probBasec) ;
			    long double probFinal;


			    if(ignoreMQ){ //ignore MQ
				probFinal = (               probBase                          );
			    }else{
				probFinal = (probMapping[m]*probBase + probMismapping[m]*0.25);
			    }
			

			    m_infoPPos->at(posVector).likeBaseNoindelCont[nuce][nucc] += log(probFinal)/log(10);

#ifdef DEBUG3		    
			    if(posVector== 146){
				cout<<posAlign<<"\t"<<"b_obs="<<b<<" e_b="<< dnaAlphabet[nuce]<<" c_b="<< dnaAlphabet[nucc]<<" q="<<int(q)<<" m="<<m<<" p(endo) "<<probEndogenous<<" p(cont) "<<( 1.0-probEndogenous ) 
				    <<"\t"<<"final="<<(probFinal)<<"\t"<<log(probFinal)/log(10)<<"\t"
				    <<"\tllik\t"<<(m_infoPPos->at(posVector).likeBaseNoindelCont[nuce][nucc])<<"\t"
				    <<"p(match)= "<<likeMatchProb[int(q)]  <<"\t"
				    <<"p(sub|matche)= "<< (probSubMatchToUseEndo->s[dinucIndexe] ) <<"\t"
				    <<"p(sub|matchc)= "<< (probSubMatchToUseCont->s[dinucIndexc] ) <<"\t"
				    <<"p(match)*p(sub|matche) "<< (likeMatchProb[int(q)]  * (probSubMatchToUseEndo->s[dinucIndexe] )) <<"\t"
				    <<"p(match)*p(sub|matchc) "<< (likeMatchProb[int(q)]  * (probSubMatchToUseCont->s[dinucIndexc] )) <<"\t"
				    <<"p(mismatch)= "<<(1.0 - likeMatchProb[int(q)]) <<"\t"
				    <<"p(sub|mismatche)= "<<(illuminaErrorsProb.s[dinucIndexe])<<"\t"
				    <<"p(sub|mismatchc)= "<<(illuminaErrorsProb.s[dinucIndexc])<<"\t"
				    <<"p(mismatch)*p(sub|mismatche) "<<( (1.0 - likeMatchProb[int(q)]) *(illuminaErrorsProb.s[dinucIndexe]) )<<"\t"
				    <<"p(mismatch)*p(sub|mismatchc) "<<( (1.0 - likeMatchProb[int(q)]) *(illuminaErrorsProb.s[dinucIndexc]) )<<endl;
				//cout<<"-----------------"<<endl;
			    }
#endif


			}//end for each contaminant

		    }//end for each endo

#ifdef DEBUG3
	if(posVector== 146){
	    cout<<"-----------------"<<endl;
	}	    
#endif

		}else{ //if we cannot assume that there is a single contaminant


		    for(unsigned int nuc=0;nuc<4;nuc++){ //iterate over each possible endogenous base
			char currentNuc=dnaAlphabet[nuc];


			if(currentNuc == b){//match
			    m_infoPPos->at(posVector).covPerBase[nuc]++;			
			}else{//mismatch
			}


			// b   is the observed
			// nuc is the model
			int dinucIndex;//The index depends on the strand
			if( pileupData.PileupAlignments[i].Alignment.IsReverseStrand() ){
			    dinucIndex= (3-nuc)*4+baseResolved2int(complement(b));
			}else{
			    dinucIndex=    nuc *4+baseResolved2int(b);
			}
			// = nuc*4+baseResolved2int(b);
		    
			//                        (1-e)           *  p(sub|1-e)                          + (         e                 ) *  p(sub|1-e)
			long double probBase =  likeMatchProb[int(q)]  * (probSubMatchToUseEndo->s[dinucIndex] )  + (1.0 - likeMatchProb[int(q)])*(illuminaErrorsProb.s[dinucIndex]);
			//m_infoPPos->at(posVector).likeBaseNoindel[nuc] += 
			long double probFinal;
		    

			if(read2endoProbInit){ //include probability of endogenous

			    probBase = ( probEndogenous )*(probBase)  + ( 1.0-probEndogenous )*(0.25) ;
			}



			if(ignoreMQ){ //ignore MQ
			    probFinal = (               probBase                          );
			}else{
			    probFinal = (probMapping[m]*probBase + probMismapping[m]*0.25);
			}
			
			
			m_infoPPos->at(posVector).likeBaseNoindel[nuc] += log(probFinal)/log(10);

#ifdef DEBUG2		    
			cout<<"b_obs="<<b<<" n_model="<<currentNuc<<"\tindex="<<dinucIndex<<" q="<<int(q)<<" m="<<m<<endl;
			cout<<"p(match)="<<likeMatchProb[int(q)]  <<"\t"
			    <<"p(sub|match)="<< (probSubMatchToUse->s[dinucIndex] ) <<"\t"
			    <<"p(match)*p(sub|match)"<< (likeMatchProb[int(q)]  * (probSubMatchToUse->s[dinucIndex] )) <<"\t"
			    <<"p(mismatch)="<<(1.0 - likeMatchProb[int(q)]) <<"\t"
			    <<"p(sub|mismatch)="<<(illuminaErrorsProb.s[dinucIndex])<<"\t"
			    <<"p(mismatch)*p(sub|mismatch)"<<( (1.0 - likeMatchProb[int(q)]) *(illuminaErrorsProb.s[dinucIndex]) )
			    <<"\t"<<"final="<<(probFinal)<<"\t"<<log(probFinal)/log(10)<<endl;
#endif


		    

#ifdef DEBUG2	
			cout<<posAlign<<"\tllik\t"<<(m_infoPPos->at(posVector).likeBaseNoindel[nuc])<<endl;
			cout<<"-----------------"<<endl;
#endif

		    }//end for each nuc

		}//end for !singleCont





	    }//end each read
	    
#ifdef DEBUG3

	    if(singleCont){ // we consider a single contaminant

		cout<<endl;
		for(unsigned int nuce=0;nuce<4;nuce++){ //iterate over each possible endogenous base
		    for(unsigned int nucc=0;nucc<4;nucc++){ //iterate over each possible contaminant base			    
			cout<<posVector<<"\te="<<dnaAlphabet[nuce]<<"\tc="<<dnaAlphabet[nucc]<<"\t"<<m_infoPPos->at(posVector).likeBaseNoindelCont[nuce][nucc]<<endl;
		    }
		}
	    }
#endif

	    //exit(1);
	    //store
	    // toStore.cov = numReads;

	    // toStore.alt = alt;
	    //  toStore.pos = posAlign;

	    // positionsInfoFound->push_back( toStore );

	    // if(positionsInfoFound->size()%10000 == 0){
	    // 	cerr<<"Found  "<<thousandSeparator(positionsInfoFound->size())<<" positions "<<endl;
	    // }
	    
	    
	    // skiptonextpos:
	    //     return;

	    // cout <<m_references[pileupData.RefId].RefName << "\t" 
	    // 	 <<referenceBase<<"\t"
	    // 	 << pileupData.Position << "\t" 
	    // 	 	 << pileupData.PileupAlignments.size() << endl;
        }//end visit
        
    private:
    RefVector m_references;
    Fasta * m_fastaReference;
    vector<singlePosInfo> * m_infoPPos;
    int sizeGenome;
    bool ignoreMQ;
    long double contaminationPrior;
    bool singleCont;

    //        ostream*  m_out;
};

























void iterateOverReads(const string fastaFile,
		      const string bamfiletopen,
		      vector<singlePosInfo>  * infoPPos,
		      const int sizeGenome,
		      const bool ignoreMQ ,
		      const long double contaminationPrior,
		      const bool singleCont){

  infoPPos->clear(); //clear previous data

  cerr<<"Reading genome file ..."<<endl;
  for(int i=0;i<sizeGenome;i++){
    singlePosInfo toadd;
    toadd.numDel              = 0;
    
    toadd.cov                 = 0;
    toadd.mapqAvg             = 0.0;

    toadd.llikDeletion        = 0.0;
    toadd.llikNoDeletion      = 0.0;
    // toadd.llikDeletionCont    = 0.0;
    // toadd.llikNoDeletionCont  = 0.0;

    toadd.llikDeletionBoth=0;
    toadd.llikDeletionCont=0;
    toadd.llikDeletionEndo=0;
    toadd.llikDeletionNone=0;
    
    for(unsigned int nuc=0;nuc<4;nuc++){
      toadd.likeBaseNoindel[nuc] = 0;
      toadd.covPerBase[nuc]      = 0;
      for(unsigned int nuc2=0;nuc2<4;nuc2++){
	toadd.likeBaseNoindelCont[nuc][nuc2] = 0;
      }
    }
    
    infoPPos->push_back(toadd);
  }
  cerr<<"... done"<<endl;

    // cout<<fastaFile<<"\t"<<bamfiletopen<<"\t"<<infoPPos->size()<<"\t"<<sizeGenome<<"\t"<<ignoreMQ<<endl;


    Fasta fastaReference;
    if ( !fastaReference.Open(fastaFile , fastaFile+".fai") ){ 
	cerr << "ERROR: failed to open fasta file " <<fastaFile<<" and index " << fastaFile<<".fai"<<endl;
	exit(1);
    }
	
	
    BamReader reader;

    if ( !reader.Open(bamfiletopen) ) {
	cerr << "Could not open input BAM files " <<bamfiletopen<< endl;
	exit(1);
    }

    if ( !reader.OpenIndex(bamfiletopen+".bai") ) {
	cerr << "Could not open input index for BAM files" <<bamfiletopen+".bai"<< endl;
	exit(1);
    }
	
    //  // retrieve reference data
    const RefVector  references = reader.GetReferenceData();



    MyPileupVisitor* cv = new MyPileupVisitor(references,&fastaReference,infoPPos,sizeGenome,ignoreMQ,contaminationPrior,singleCont);
    PileupEngine pileup;
    pileup.AddVisitor(cv);


    BamAlignment al;
    unsigned int numReads=0;
    cerr<<"Reading BAM file ..."<<endl;

    while ( reader.GetNextAlignment(al) ) {
	//cerr<<al.Name<<endl;
	numReads++;
	if(numReads !=0 && (numReads%100000)==0){
	    cerr<<"Read "<<thousandSeparator(numReads)<<" reads"<<endl;
	}

	if(al.IsMapped() && 
	   !al.IsFailedQC()){
	    // cerr<<al.Name<<endl;
	    pileup.AddAlignment(al);
	}
	    
    }
    cerr<<"...  done"<<endl;
    
    

    

    //clean up
    pileup.Flush();

    reader.Close();
    fastaReference.Close();
    delete cv;



}


void computePriorOnReads(const string bamfiletopen,
			 const bool deamread,
			 const bool useLengthPrior,
			 const long double contaminationPrior
			 ){

    //iterate over the reads once again and compute prob of deam
	
    cerr<<"Reading BAM to set priors for each read ..."<<endl;
    BamReader reader;
    BamAlignment al;
    if ( !reader.Open(bamfiletopen) ) {
	cerr << "Could not open input BAM files " <<bamfiletopen<< endl;
	exit(1);
    }
    unsigned int skipped      =0;

    while ( reader.GetNextAlignment(al) ) { //for each read
	//cout<<al.Name<<endl;
	//those reads are ignored later anyway..
	if(!al.IsMapped()){   continue; }
	if( al.IsFailedQC()){ continue; }

	pair< string,vector<int> > reconstructedReference = reconstructRefWithPos(&al);
	    
	if( skipAlign(reconstructedReference.first,&al,&skipped) ){ 
	    // cout<<al.QueryBases<<endl<<reconstructedReference.first<<endl<<endl;
	    continue; 
	}

	if(al.QueryBases.size() != reconstructedReference.first.size()){
	    cerr<<"Query bases line is not the same size as the reconstructed reference for read "<<al.Name<<endl;
	    // return 1;
	    exit(1);
	}

	if(al.QueryBases.size() != reconstructedReference.second.size()){
	    cerr<<"Query bases line is not the same size as the reconstructed positions on the reference for read "<<al.Name<<endl;
	    exit(1);
	    // return 1;
	}

	// for(unsigned int i=0;i<reconstructedReference.first.size();i++){
	// 	cout<<reconstructedReference.first[i]<<"\t"<<reconstructedReference.second[i]<<endl;
	// }


	long double deamLogLike=0.0;
	long double nullLogLike=0.0;
	if(deamread){
	  for(unsigned int i=0;i<al.QueryBases.size();i++){//for each base
		
	    char refeBase =toupper(reconstructedReference.first[i]);
	    char readBase =toupper(         al.QueryBases[i]);
	    char q        = al.Qualities[i]-offsetQual;


	    int pos       = reconstructedReference.second[i]+1;
	    // cout<<i<<"\t"<<reconstructedReference.second[i]<<endl;
	    transformRef(&refeBase,&readBase);

	    if(refeBase == 'I'   ){
	      continue;
	    }

	    if(refeBase == 'N' ){
	      continue;
	    }

	    if(readBase == 'N' ){
	      continue;
	    }

	    if(pos2phredgeno[ pos ].consensus == 'D'){//skip deletions
	      continue;
	    }


	   	    
	    if(pos2phredgeno[ pos ].ref != refeBase){
	      cerr<<"Query reference base is not the same for read "<<al.Name<<" pos "<<pos<<endl;

	      cout<<pos<<"\t"<<al.QueryBases[i]<<"\t"<<reconstructedReference.first[i]<<"\t"<<refeBase<<"\t"<<readBase<<"\tR="<<pos2phredgeno[ pos ].ref<<"\tC="<<pos2phredgeno[ pos ].consensus<<"\t"<<al.Name<<endl;
	      for(unsigned int j=0;j<al.QueryBases.size();j++){
		cout<<j<<"\t"<<reconstructedReference.first[j]<<"\t"<<reconstructedReference.second[j]<<endl;
	      }

	      //return 1;
	      exit(1);
	    }
	    
	    //deam model
		

		
	    int dist5p=-1;
	    int dist3p=-1;

	    if( al.IsReverseStrand() ){
	      dist5p = int(al.QueryBases.size()) - int(i)-1;
	      dist3p = int(i);
	    }else{
	      dist5p = int(i);
	      dist3p = int(al.QueryBases.size()) - int(i)-1;
	    }
		    		    
	    probSubstition * probSubMatchDeam = &defaultSubMatch ;
	    probSubstition * probSubMatchNull = &defaultSubMatch ;

	    if(dist5p <= (int(sub5p.size()) -1)){
	      probSubMatchDeam = &sub5p[ dist5p ];			
	    }
		    
	    if(dist3p <= (int(sub3p.size()) -1)){
	      probSubMatchDeam = &sub3p[ dist3p ];
	    }
		    
	    //we have substitution probabilities for both... take the closest
	    if(dist5p <= (sub5p.size() -1) &&
	       dist3p <= (sub3p.size() -1) ){
	      
	      if(dist5p < dist3p){
		probSubMatchDeam = &sub5p[ dist5p ];			
	      }else{
		probSubMatchDeam = &sub3p[ dist3p ];
	      }
			
	    }


	    // cout<<pos<<"\t"<<al.QueryBases[i]<<"\t"<<reconstructedReference.first[i]<<"\t"<<refeBase<<"\t"<<readBase<<"\tR="<<pos2phredgeno[ pos ].ref<<"\tC="<<pos2phredgeno[ pos ].consensus<<"\t"<<al.Name<<"\t"<<dist5p<<"\t"<<dist3p<<endl;
		
	    // b   is the observed
	    // nuc is the model
		
	    int obsReadInt;
	    if( al.IsReverseStrand() ){
	      obsReadInt = baseResolved2int(complement(readBase));
	    }else{
	      obsReadInt = baseResolved2int(readBase);
	    }

	    long double probBaseDeam = 0.0;
	    long double probBaseNull = 0.0;

	    for(unsigned int nuc=0;nuc<4;nuc++){


	      int dinucIndex;
	      if( al.IsReverseStrand() ){
		dinucIndex= (3-nuc)*4+obsReadInt;
	      }else{
		dinucIndex=     nuc*4+obsReadInt;
	      }
	      probBaseDeam +=
		(1-pos2phredgeno[ pos ].perror[nuc])
		*
			//      (1-e)           *  p(sub|1-e)                         + (e)                          *  p(sub|1-e)
		(likeMatchProb[int(q)]  * (probSubMatchDeam->s[dinucIndex] )  + (1.0 - likeMatchProb[int(q)])*(illuminaErrorsProb.s[dinucIndex]));


	      probBaseNull +=
		(1-pos2phredgeno[ pos ].perror[nuc])
		*
		//      (1-e)           *  p(sub|1-e)                         + (e)                          *  p(sub|1-e)
		(likeMatchProb[int(q)]  * (probSubMatchNull->s[dinucIndex] )  + (1.0 - likeMatchProb[int(q)])*(illuminaErrorsProb.s[dinucIndex])); 

			


	    }//end for each possible base

	    deamLogLike+=log(probBaseDeam);
	    nullLogLike+=log(probBaseNull);

	
	    //m_infoPPos->at(posVector).likeBaseNoindel[nuc] += 
	    // double probFinal;
	    
	    // if(ignoreMQ){ //ignore MQ
	    //     probFinal = (               probBase                          );
	    // }else{
	    //     probFinal = (probMapping[m]*probBase + probMismapping[m]*0.25);
	    // }
		    

	    //null model
	    
	  }//end for all bases
	}else{//if we use deamination priors
	  //this means the read is equally likely to be deaminated or not, we will revert to our prior
	  deamLogLike=0.5;
	  nullLogLike=0.5;
	}
	    



	long double probDeamUnscaled = 
	    exp(deamLogLike)
	    /
	    (    exp(deamLogLike) + exp(nullLogLike) );
	    
	//probLengthEndo[max(al.QueryBases.size(),1000)];
	long double probEndoProd;
	long double probContProd;
	long double probLengthEndoForRead;
	    
	if(useLengthPrior){
	  probLengthEndoForRead = probLengthEndo[min(int(al.QueryBases.size()),999)];
	}else{
	  //this means the read is equally likely to be generated by either 
	  //distribution, we will revert to our prior
	  probLengthEndoForRead = 0.5;
	}

	//if(useLengthPrior){
	probEndoProd   =      probDeamUnscaled  *      probLengthEndoForRead;
	probContProd   = (1.0-probDeamUnscaled) * (1.0-probLengthEndoForRead);
	// }else{
	// 	probEndoProd   = 0.5;
	// 	probContProd   = 0.5;

	// }
	long double probEndo         = 
	    (1-contaminationPrior)*probEndoProd
	    /
	    (  ((1-contaminationPrior)*probEndoProd) + contaminationPrior*probContProd );

	//long double probEndo         = boost::math::ibeta(contaminationPrior,1.0-contaminationPrior,probDeamUnscaled);
	//cout<<al.Name<<"\t"<<int(al.QueryBases.size())<<"\t"<<probEndo<<"\t"<<probDeamUnscaled<<"\t"<<probLengthEndoForRead<<"\t"<<probEndoProd<<"\t"<<probContProd<<endl;


	
	read2endoProb[ al.Name+"#"+ stringify(al.AlignmentFlag) ] = probEndo;

    } //for each read
}


void initScores(){

    for(int i=0;i<2;i++){
        likeMatch[i]        = log1p(    -pow(10.0,2.0/-10.0) )    /log(10);         
        likeMismatch[i]     = log  (     pow(10.0,2.0/-10.0)/3.0 )/log(10);

	likeMatchProb[i]           = 1.0-pow(10.0,2.0/-10.0) ;
        likeMismatchProb[i]        =     pow(10.0,2.0/-10.0)/3.0 ;
    }


    //Computing for quality scores 2 and up
    for(int i=2;i<64;i++){
        likeMatch[i]        = log1p(    -pow(10.0,i/-10.0) )     /log(10);          
        likeMismatch[i]     = log  (     pow(10.0,i/-10.0)/3.0  )/log(10);

        likeMatchProb[i]           = 1.0-pow(10.0,i/-10.0);
        likeMismatchProb[i]        =     pow(10.0,i/-10.0)/3.0;
    }


    //Adding mismapping probability
    for(int m=0;m<64;m++){

	double incorrectMappingProb   =     pow(10.0,m/-10.0); //m
	double correctMappingProb     = 1.0-pow(10.0,m/-10.0); //1-m
	
	probMapping[m]    = correctMappingProb;    //1-m
	probMismapping[m] = incorrectMappingProb;  //m

#ifdef DEBUG1
	cerr<<"m\t"<<m<<"\t"<<incorrectMappingProb<<"\t"<<correctMappingProb<<endl;
#endif
	
    	for(int i=0;i<2;i++){
    	    likeMatchMQ[m][i]           = log(  correctMappingProb*(1.0-pow(10.0,2.0/-10.0)    ) + incorrectMappingProb/4.0   )/log(10);         
    	    likeMismatchMQ[m][i]        = log(  correctMappingProb*(    pow(10.0,2.0/-10.0)/3.0) + incorrectMappingProb/4.0   )/log(10);
    	    likeMatchProbMQ[m][i]       = correctMappingProb*(1.0-pow(10.0,2.0/-10.0)    ) + incorrectMappingProb/4.0;
    	    likeMismatchProbMQ[m][i]    = correctMappingProb*(    pow(10.0,2.0/-10.0)/3.0) + incorrectMappingProb/4.0;
    	}


    	//Computing for quality scores 2 and up
    	for(int i=2;i<64;i++){
	    //  (1-m)(1-e) + m/4  = 1-m-e+me +m/4  = 1+3m/4-e+me
    	    likeMatchMQ[m][i]         = log(  correctMappingProb*(1.0-pow(10.0,i/-10.0)    ) + incorrectMappingProb/4.0    )/log(10);    
	    //  (1-m)(e/3) + m/4  = e/3 -me/3 + m/4
    	    likeMismatchMQ[m][i]      = log(  correctMappingProb*(    pow(10.0,i/-10.0)/3.0) + incorrectMappingProb/4.0    )/log(10);    
	    
    	    likeMatchProbMQ[m][i]           = correctMappingProb*(1.0-pow(10.0,i/-10.0)    ) + incorrectMappingProb/4.0;
    	    likeMismatchProbMQ[m][i]        = correctMappingProb*(    pow(10.0,i/-10.0)/3.0) + incorrectMappingProb/4.0;
    	}


#ifdef DEBUG1
    	for(int i=0;i<64;i++){
	    cerr<<"m\t"<<m<<"\t"<<i<<"\t"<<likeMatchMQ[m][i]<<"\t"<<likeMismatchMQ[m][i]<<"\t"<<likeMatchProbMQ[m][i]<<"\t"<<likeMismatchProbMQ[m][i]<<endl;
	}
#endif

    }

}

int main (int argc, char *argv[]) {

    int sizeGenome=0;
    string outSeq  = "/dev/stdout";
    string outLog  = "/dev/stderr";
    string nameMT  = "MT";

    string outSeqC  = "";
    string outLogC  = "";
    bool   outSeqCflag  = false;
    bool   outLogCflag  = false;


    string nameMTC  = "MTc";

  
    int minQual=0;
    bool ignoreMQ=false;


    ////////////////////////////////////
    // BEGIN Initializing scores      //
    ////////////////////////////////////
    initScores();
    ////////////////////////////////////
    //    END Initializing scores     //
    ////////////////////////////////////


    long double locatione=0.0;
    long double locationc=0.0;
    long double scalee   =0.0;
    long double scalec   =0.0;



    ////////////////////////////////////
    // BEGIN Parsing arguments        //
    ////////////////////////////////////

    //    return 1;
    string errFile    = getCWD(argv[0])+"illuminaProf/error.prof";
    string deam5pfreq = getCWD(argv[0])+"deaminationProfile/none.prof";
    string deam3pfreq = getCWD(argv[0])+"deaminationProfile/none.prof";
    // substitutionRates freqIlluminaError;
    vector<substitutionRates>    deam5Psub;
    vector<substitutionRates>    deam3Psub;
    bool deamread=false;
    bool useLengthPrior  = false;
    long double contaminationPrior=0.5;
    bool singleCont=false;
    bool specifiedContPrior=false;
    bool specifiedLoce   = false;
    bool specifiedLocc   = false;
    bool specifiedScalee = false;
    bool specifiedScalec = false;


    const string usage=string("\nThis program takes an aligned BAM file for a mitonchondria and calls a\nconsensus for the endogenous material\n\n\t"+
			      string(argv[0])+			      
			      " [options]  [reference fasta] [bam file] "+"\n\n"+

			      "\n\tOutput options:\n"+	
			      "\t\t"+"-seq  [fasta file]" +"\t\t"+"Output fasta file (default: stdout)"+"\n"+
			      "\t\t"+"-log  [log file]" +"\t\t"+"Output log  (default: stderr)"+"\n"+
			      "\t\t"+"-name [name]" +"\t\t\t"  +"Name  (default "+nameMT+") "+"\n"+
			      "\t\t"+"-qual [minimum quality]" +"\t\t"  +"Filter bases with quality less than this  (default "+stringify(minQual)+") "+"\n"+
			      // "\t\t"+"-cont" +"\t\t"+"Contamination allele frequency"+"\n"+

			      "\n\tContamination options:\n"+				      
			      "\t\t"+"-deam5p [.prof file]" +"\t\t"+"5p deamination frequency (default: "+deam5pfreq+")"+"\n"+
			      "\t\t"+"-deam3p [.prof file]" +"\t\t"+"3p deamination frequency (default: "+deam3pfreq+")"+"\n"+
			      "\t\t"+"-deamread" +"\t\t\t"+"Set a prior on reads according to their deamination pattern (default: "+ booleanAsString(deamread) +")"+"\n"+
			      "\t\t"+"-cont [cont prior]"+"\t\t"+"If the -deamread option is specified, this is the contamination prior (default: "+ stringify(contaminationPrior) +")"+"\n"+

			      "\t\t"+"-single"+"\t\t\t\t"+"Try to determine the contaminant under the assumption that there is a single\n\t\t\t\t\t\tone  (default: "+ booleanAsString(singleCont) +")"+"\n"+

			      "\t\tIf the -single option is used, the following are available:\n"+
			      "\t\t\t"+"-seqc  [fasta file]" +"\t\t"+"Output contaminant as fasta file (default: none)"+"\n"+
			      "\t\t\t"+"-logc  [log file]" +"\t\t"+"Output contaminant as log  (default: none)"+"\n"+
			      "\t\t\t"+"-namec [name]" +"\t\t\t"  +"Name of contaminant sequence (default "+nameMTC+") "+"\n"+




			      "\n\tLength options:\n"+				      
			      "\t\t"+"--loce"+  "\t\t\t\t"+"Location for lognormal dist for the endogenous sequences (default none)"+"\n"+
			      "\t\t"+"--scalee"+"\t\t\t"+"Scale for lognormal dist for the endogenous sequences (default none)"+"\n"+
			      "\t\t"+"--locc"+  "\t\t\t\t"+"Location for lognormal dist for the contaminant sequences (default none)"+"\n"+
			      "\t\t"+"--scalec"+"\t\t\t"+"Scale for lognormal dist for the contaminant sequences (default none)"+"\n"+

			      "\n\tComputation options:\n"+	
			      "\t\t"+"-nomq" +"\t\t\t\t"+"Ignore mapping quality (default: "+booleanAsString(ignoreMQ)+")"+"\n"+
			      "\t\t"+"-err" +"\t\t\t\t"+"Illumina error profile (default: "+errFile+")"+"\n"+
			      
			      "\n\tReference options:\n"+	
			      "\t\t"+"-l [length]" +"\t\t\t"+"Actual length of the genome used for"+"\n"+
			      "\t\t"+"  " +"\t\t\t\t"+"the reference as been wrapped around"+"\n"+
			      "\t\t"+"  " +"\t\t\t\t"+"by default, the length of the genome will be used "+"\n"+
			      
			      "");
			      
    if( (argc== 1) ||
	(argc== 2 && string(argv[1]) == "-h") ||
	(argc== 2 && string(argv[1]) == "-help") ||
	(argc== 2 && string(argv[1]) == "--help") ){
	cout<<usage<<endl;
	return 1;
    }

    string bamfiletopen = string(argv[argc-1]);//bam file
    string fastaFile    = string(argv[argc-2]);//fasta file

    for(int i=1;i<(argc-2);i++){ //all but the last 3 args

	
	if(strcmp(argv[i],"--loce") == 0 ){
	    locatione =destringify<long double>(argv[i+1]);
	    i++;
	    specifiedLoce=true;
	    continue;
	}

	if(strcmp(argv[i],"--scalee") == 0 ){
	    scalee =destringify<long double>(argv[i+1]);
	    i++;
	    specifiedScalee=true;
	    continue;
	}

	if(strcmp(argv[i],"--locc") == 0 ){
	    locationc =destringify<long double>(argv[i+1]);
	    i++;
	    specifiedLocc=true;
	    continue;
	}

	if(strcmp(argv[i],"--scalec") == 0 ){
	    scalec =destringify<long double>(argv[i+1]);
	    i++;
	    specifiedScalec=true;
	    continue;
	}

	if(string(argv[i]) == "-err"  ){
	    errFile=string(argv[i+1]);
	    i++;
	    continue;
	}

	if(string(argv[i]) == "-deam5p"  ){
	    deam5pfreq=string(argv[i+1]);
	    i++;
	    continue;
	}

	if(string(argv[i]) == "-deam3p"  ){
	    deam3pfreq=string(argv[i+1]);
	    i++;
	    continue;
	}

	if(string(argv[i]) == "-deamread"  ){
	    deamread=true;
	    continue;
	}


	if(string(argv[i]) == "-single"  ){
	    singleCont=true;
	    continue;
	}


	if(strcmp(argv[i],"-cont") == 0 ){
	    contaminationPrior=destringify<long double>(argv[i+1]);
	    specifiedContPrior=true;
	    i++;
	    continue;
	}

	// if(strcmp(argv[i],"-deam5") == 0 ){
	//     deam5File=string(argv[i+1]);
	//     i++;
	//     continue;
	// }

	if(strcmp(argv[i],"-nomq") == 0 ){
	    ignoreMQ=true;
	    continue;
	}

	if(strcmp(argv[i],"-seq") == 0 ){
	    outSeq=string(argv[i+1]);
	    i++;
	    continue;
	}

	if(strcmp(argv[i],"-log") == 0 ){
	    outLog=string(argv[i+1]);
	    i++;
	    continue;
	}

	if(strcmp(argv[i],"-name") == 0 ){
	    nameMT=string(argv[i+1]);
	    i++;
	    continue;
	}


	if(strcmp(argv[i],"-seqc") == 0 ){
	    outSeqC=string(argv[i+1]);
	    outSeqCflag = true;
	    i++;
	    continue;
	}

	if(strcmp(argv[i],"-logc") == 0 ){
	    outLogC=string(argv[i+1]);
	    outLogCflag = true;
	    i++;
	    continue;
	}

	if(strcmp(argv[i],"-namec") == 0 ){
	    nameMTC=string(argv[i+1]);
	    i++;
	    continue;
	}

	if(strcmp(argv[i],"-qual") == 0 ){
	    minQual=destringify<int>(argv[i+1]);
	    i++;
	    continue;
	}



	if(strcmp(argv[i],"-l") == 0 ){
	    sizeGenome=atoi(argv[i+1]);
	    i++;
	    continue;
	}


	cerr<<"Error: unknown option "<<string(argv[i])<<endl;
	return 1;
    }


    if(specifiedLoce     ||
       specifiedLocc     || 
       specifiedScalee   ||
       specifiedScalec ){

	if( !(specifiedLoce     &&
	      specifiedLocc     && 
	      specifiedScalee   &&
	      specifiedScalec ) ){
	    cerr<<"Error: need to specify the location and scale for both the endogenous and contaminant"<<endl;
	    return 1;
	}

	for(int lengthMolecule=0;lengthMolecule<1000;lengthMolecule++){
	    long double pdfEndo = pow(10.0,(long double)(logcomppdf(locatione,scalee ,(long double)(lengthMolecule))));
	    long double pdfCont = pow(10.0,(long double)(logcomppdf(locationc,scalec ,(long double)(lengthMolecule))));

	    //double pdfCont = logcomppdf  
	    //cout<<lengthMolecule<<"\t"<<pdfEndo<<"\t"<<pdfCont<<"\t"<<(pdfEndo/(pdfEndo+pdfCont))<<endl;
	    //dobuel 
	    probLengthEndo[lengthMolecule] = (pdfEndo/(pdfEndo+pdfCont));
	    
	}
	useLengthPrior=true;

    }

    if(specifiedContPrior ){
	if( !(deamread || useLengthPrior) ){
	    cerr<<"Error: cannot specify -cont if you do not specify -deamread or use the length distribution priors, exiting"<<endl;
	    return 1;	
	}
    }


    ////////////////////////////////////
    // END Parsing arguments        //
    ////////////////////////////////////











    ////////////////////////////////////////////////////////////////////////
    //
    // BEGIN READING ERROR PROFILE
    //
    ////////////////////////////////////////////////////////////////////////

    readIlluminaError(errFile,illuminaErrorsProb);

    // for(int nuc1=0;nuc1<4;nuc1++){
    // 	for(int nuc2=0;nuc2<4;nuc2++){
	    
    // 	    cout<<illuminaErrorsProb.s[nuc2+nuc1*4]<<"\t";
    // 	}
    // 	cout<<endl;
    // }
    // return 1;
    
    ////////////////////////////////////////////////////////////////////////
    //
    // END  READING ERROR PROFILE
    //
    ////////////////////////////////////////////////////////////////////////











    ////////////////////////////////////////////////////////////////////////
    //
    // BEGIN DEAMINATION PROFILE
    //
    ////////////////////////////////////////////////////////////////////////
    readNucSubstitionFreq(deam5pfreq,sub5p);
    readNucSubstitionFreq(deam3pfreq,sub3p);

    // cout<<sub5p.size()<<endl;
    // cout<<sub3p.size()<<endl;

    // return 1;
    

    int defaultSubMatchIndex=0;

    for(int nuc1=0;nuc1<4;nuc1++){
    	for(int nuc2=0;nuc2<4;nuc2++){	    
    	    if(nuc1==nuc2)
		defaultSubMatch.s[ defaultSubMatchIndex++ ] = 1.0;
	    else
		defaultSubMatch.s[ defaultSubMatchIndex++ ] = 0.0;
    	}
    	
    }


    // for(int nuc1=0;nuc1<4;nuc1++){
    // 	for(int nuc2=0;nuc2<4;nuc2++){	    
    // 	    cout<<sub5p[0].s[nuc2+nuc1*4]<<"\t";
    // 	}
    // 	cout<<endl;
    // }

    // for(int nuc1=0;nuc1<4;nuc1++){
    // 	for(int nuc2=0;nuc2<4;nuc2++){	    
    // 	    cout<<sub3p[0].s[nuc2+nuc1*4]<<"\t";
    // 	}
    // 	cout<<endl;
    // }

    // return 1;
    
    ////////////////////////////////////////////////////////////////////////
    //
    // END  DEAMINATION PROFILE
    //
    ////////////////////////////////////////////////////////////////////////










    nameMT = ">"+nameMT;














     
    vector<singlePosInfo> infoPPos;

    if(isDos(fastaFile) || isMac(fastaFile) ){
	cerr << "File  "<<fastaFile<<" must be unix formatted, exiting"<<endl;
	return 1;
    }


    string line;

    bool firstFastaLine=true;
    string genomeRef="";

    igzstream fastaRefFile;
    fastaRefFile.open(fastaFile.c_str(),ios::in);
    if (fastaRefFile.good()){

	while ( getline (fastaRefFile,line)){
	
	    if(firstFastaLine){
		if(line[0]!='>'){
		    cerr << "File  "<<fastaFile<<" does not appear to be in fasta format"<<endl;
		    return 1;
		}
		firstFastaLine=false;
		continue;		
	    }

	    genomeRef+=line;
	}
	fastaRefFile.close();

    }else{
	cerr << "Cannot open fasta file  "<<fastaFile<<""<<endl;
	return 1;
    }

    // cerr<<genomeRef<<endl;

    // return 1;
    if(sizeGenome == 0){
	sizeGenome=genomeRef.size();//use the one from the fasta
    }

    // double ** likelihoodPBasePPos = new double * [genome.size()];
    // for(unsigned int i = 0; i< genome.size(); i++) {
    // 	likelihoodPBasePPos[i] = new double  [4];
    // }
    // //return 1;


    // void iterateOverReads(const string fastaFile,
    // 		      const string bamfiletopen,
    // 		      vector<singlePosInfo>  infoPPos,
    // 		      const int sizeGenome,
    // 		      const bool ignoreMQ ){

    iterateOverReads(fastaFile,
		     bamfiletopen,
		     &infoPPos,
		     sizeGenome,
		     ignoreMQ,
		     contaminationPrior,
		     singleCont);



    //printLogAndGenome(sizeGenome, infoPPos,outSeq,outLog);
    printLogAndGenome(sizeGenome, 
		      infoPPos,
		      outSeq,
		      outLog, 
		      genomeRef,
		      minQual,
		      nameMT,
		      singleCont,
		      outSeqC,
		      outLogC,
		      outSeqCflag,
		      outLogCflag,`
		      nameMTC);

    // delete coverageCounter;














    if(deamread || useLengthPrior){
	

      computePriorOnReads(bamfiletopen,
			  deamread,
			  useLengthPrior,
			  contaminationPrior);
      
      cerr<<"...  done"<<endl;
      read2endoProbInit=true;
	








	//iterate over the reads once again
	
	//iterateOverReads();
	// cerr<<"Reading BAM to find to compute probability of deamination ..."<<endl;

	iterateOverReads(fastaFile,
			 bamfiletopen,
			 &infoPPos,
			 sizeGenome,
			 ignoreMQ,
			 contaminationPrior,
			 singleCont);
	

	


	//printLogAndGenome(sizeGenome, infoPPos,outSeq,outLog);
	printLogAndGenome(sizeGenome, infoPPos,outSeq,outLog, genomeRef,minQual,nameMT,singleCont,		      outSeqC,
		      outLogC,
		      outSeqCflag,
		      outLogCflag,
 
		      nameMTC);

    }


    return 0;
}

