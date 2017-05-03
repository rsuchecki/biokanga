// genDESeq.cpp : Defines the entry point for the console application.
// Purpose -
// Loads the output generated by maploci2features processing with '-O<featuremapfile>' and combines these into a tab delimited form ready for processing in 'R' by DESeq
// Optionally the user can request an additional column be generated containing the feature length
// 1.1.8  Changed pre-increment to post-increment in for() processing
// 1.1.9  Increased max samplesets to be no more than 100 (was previously 50)
//        Allow for no experimental counts - control only counts reported        
// 1.1.11 Bugfix, was allowing transcripts with no counts in any sample to pass filtering when filtering was on MinExonIntronCnts 

#include "stdafx.h"

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#if _WIN32
#include "../libbiokanga/commhdrs.h"
#else
#include "../libbiokanga/commhdrs.h"
#endif

const char *cpszProgVer = "1.1.11";			// increment with each release

const double cNormCntsScale = 1.0;			// normalise ExonCnts or RPKMs to counts by scaling with this multiplier

const unsigned int cMaxInFileSpecs = 20;	// allow user to specify up to this many input file specs


const int cMaxSampleSets = 100;				// can handle at most feature counts for this many samplesets
const int cMaxSamplesetNameLen = 40;		// sampleset names can be at most this long including terminating '\0'

const int cWrtBuffSize = 0x0fffff;			// use a 1 MB write buffer size
const int cAllocFeatRPKMs = 200000;			// allocate for feature RPKMs in this many instances

// processing modes
typedef enum TAG_ePMode {		
	ePMExonCnts,				// default processing is to use total counts over isoform transcript exons
	ePMRPKM,					// processing is for RPKM
	ePMplaceholder				// used to set the enumeration range
	} etPMode;

#pragma pack(1)

typedef struct TAG_sFeatRPKM {
	UINT32 SampleSetID;				// feature is in this sampleset
	UINT32 FeatID;						// uniquely identifies this feature in sampleset
	UINT32 FeatLen;						// feature length
	UINT32 ExonIntronCnts;				// total number of exon + intron counts ascribed to this feature
	char szFeatName[cMaxFeatNameLen];	// '\0' terminated feature name
	double RPKM;						// RPKM for this feature
} tsFeatRPKM;

#pragma pack()

teBSFrsltCodes Process(etPMode PMode,		// processing mode
	        bool bFeatLen,					// if true then also output the feature length
			int MinFeatLen,					// feature has to be at least this length
			int MaxFeatLen,					// but no longer than this length
			int MinCtrlCnts,				// sum of control counts for a feature must be at least this
			int MinExprCnts,				// sum of experiment counts for a feature must be at least this
			int MinCtrlExprCnts,			// sum of control and experiment counts for a feature must be at least this

   		double NormCntsScale,				// counts normalisation scale factor
		char *pszControlTitle,				// control title
		char *pszExprTitle,					// experiment title
		int	NumInputControlFiles,			// number of input control file specs
		char *pszInControlFileSpecs[],		// names of control input files (wildcards allowed)
		int	NumInputExprFiles,				// number of input experiment file specs
		char *pszInExprFileSpecs[],			// names of experiment input files (wildcards allowed)
		char *pszOutfile);					// output into this file

teBSFrsltCodes LoadFile(int SampleSetID,	// uniquely identifies sampleset
			 int MinFeatLen,				// feature has to be at least this length
			int MaxFeatLen,					// but no longer than this length
			int MinExonIntronCnts,			// must be at least this many counts in exons/introns
			char *pszFile);					// process from this file

int
AddFeatureRPKM(int SampleSetID,			// processed from this sampleset
				UINT32 FeatID,				// unique feature identifier within this sampleset
				UINT32 FeatLen,				// feature length
			   char *pszFeatName,			// feature name
			   UINT32 ExonIntronCnts,		// total number of exon + intron counts ascribed to this feature
			   double RPKM);				// associated RPKM

int WriteoutDESeq(bool bFeatLen,		// output feature length as an additional column
				int MinExonIntronCnts);	// only write out those features having sum of exon/intron counts at least MinExonIntronCnts

static int SortFeatRPKMs(const void *arg1, const void *arg2);

CStopWatch gStopWatch;
CDiagnostics gDiagnostics;				// for writing diagnostics messages to log file
char gszProcName[_MAX_FNAME];			// process name
bool gbActivity;						// used to determine if activity messages vi printf's can be used - output activity if eDLInfo or less

#ifdef _WIN32
// required by str library
#if !defined(__AFX_H__)  ||  defined(STR_NO_WINSTUFF)
HANDLE STR_get_stringres()
{
	return NULL;	//Works for EXEs; in a DLL, return the instance handle
}
#endif

const STRCHAR* STR_get_debugname()
{
	return _T("genDESeq");
}
// end of str library required code
#endif


#ifdef _WIN32
int _tmain(int argc, char* argv[])
{
// determine my process name
_splitpath(argv[0],NULL,NULL,gszProcName,NULL);
#else
int 
main(int argc, const char** argv)
{
// determine my process name
CUtility::splitpath((char *)argv[0],NULL,gszProcName);
#endif
int iScreenLogLevel;		// level of file diagnostics
int iFileLogLevel;			// level of file diagnostics
char szLogFile[_MAX_PATH];	// write diagnostics to this file
int Rslt = 0;   			// function result code >= 0 represents success, < 0 on failure
int Idx;					// general iteration indexer

etPMode PMode;				// processing mode
double NormCntsScale;		// counts normalisation scale factor

bool bFeatLen = false;			// if true then also output the feature length
int MinFeatLen=0;				// feature has to be at least this length
int MaxFeatLen=0;				// but no longer than this length
int MinCtrlCnts=0;				// sum of control counts for a feature must be at least this
int MinExprCnts=0;				// sum of experiment counts for a feature must be at least this
int MinCtrlExprCnts=0;			// sum of control and experiment counts for a feature must be at least this

char szTitleControl[cMaxDatasetSpeciesChrom];	// to hold control title
int NumInputControlFiles;	// number of control input files
char *pszInControlFileSpecs[cMaxInFileSpecs];  // input (wildcards allowed) control files

char szTitleExperiment[cMaxDatasetSpeciesChrom];	// to hold experiment title
int NumInputExprFiles;			// number of experiment input files
char *pszInExprFileSpecs[cMaxInFileSpecs];  // input (wildcards allowed) experiment files

char szOutfile[_MAX_PATH];  // output rds file or csv if user requested stats

// command line args
struct arg_lit  *help    = arg_lit0("hH","help",                "print this help and exit");
struct arg_lit  *version = arg_lit0("v","version,ver",			"print version information and exit");
struct arg_int *FileLogLevel=arg_int0("f", "FileLogLevel",		"<int>","Level of diagnostics written to screen and logfile 0=fatal,1=errors,2=info,3=diagnostics,4=debug");
struct arg_file *LogFile = arg_file0("F","log","<file>",		"diagnostics log file");
struct arg_int *pmode = arg_int0("m","mode","<int>",		    "processing mode: 0 - ExonCnts, 1 RPKM (default is ExonCnts)");
struct arg_lit  *bfeatlen    = arg_lit0("E","featlen",          "output additional column (non-DEseq conformant) containing the feature length");

struct arg_dbl *normcntsscale = arg_dbl0("s","mode","<dbl>",	"counts normalisation scale factor (default is 1.0)");

struct arg_int *minfeatlen = arg_int0("l","minfeatlen","<int>",		    "minimum feature length (default is 0 for no min length filtering)");
struct arg_int *maxfeatlen = arg_int0("L","maxfeatlen","<int>",		    "maximum feature length  (default is 0 for no max length filtering)");
struct arg_int *minctrlcnts = arg_int0("c","minctrlcnts","<int>",		"minimum sum of feature control counts in exon/introns or RPKM (default is 0)");
struct arg_int *minexprcnts = arg_int0("e","minexprcnts","<int>",		"minimum sum of feature experiment counts in exon/introns or RPKM (default is 0)");
struct arg_int *minctrlexprcnts = arg_int0("C","minctrlexprcnts","<int>","minimum sum of feature control and experiment counts in exon/introns or RPKM (default is 0)");

struct arg_file *incontrolfiles = arg_filen("i","control","<file>",0,cMaxInFileSpecs,"input control from these maploci2features generated files, wildcards allowed");
struct arg_file *inexperfiles = arg_filen("I","experiment","<file>",0,cMaxInFileSpecs,"input experiment from these maploci2features generated files, wildcards allowed (optional)");
struct arg_str  *titlecontrol = arg_str1("t","titlecontrol","<string>", "use this control title");
struct arg_str  *titleexperiment = arg_str1("T","titleexperiment","<string>", "use this experiment title");
struct arg_file *outfile = arg_file1("o","out","<file>",		"output accepted processed reads to this file as DESeq tab delimited");

struct arg_end *end = arg_end(20);

void *argtable[] = {help,version,FileLogLevel,LogFile,
					pmode,bfeatlen,minfeatlen,maxfeatlen,minctrlcnts,minexprcnts,minctrlexprcnts,normcntsscale,titlecontrol,titleexperiment,incontrolfiles,inexperfiles,outfile,
					end};

char **pAllArgs;
int argerrors;
argerrors = CUtility::arg_parsefromfile(argc,(char **)argv,&pAllArgs);
if(argerrors >= 0)
	argerrors = arg_parse(argerrors,pAllArgs,argtable);

/* special case: '--help' takes precedence over error reporting */
if (help->count > 0)
        {
		printf("\n%s process for DESeq, Version %s\nOptions ---\n", gszProcName,cpszProgVer);
        arg_print_syntax(stdout,argtable,"\n");
        arg_print_glossary(stdout,argtable,"  %-25s %s\n");
		printf("\nNote: Parameters can be entered into a parameter file, one parameter per line.");
		printf("\n      To invoke this parameter file then precede its name with '@'");
		printf("\n      e.g. %s @myparams.txt\n",gszProcName);
		printf("\nPlease report any issues regarding usage of %s at https://github.com/csiro-crop-informatics/biokanga/issues\n\n",gszProcName);
		exit(1);
        }

    /* special case: '--version' takes precedence error reporting */
if (version->count > 0)
        {
		printf("\n%s Version %s\n",gszProcName,cpszProgVer);
		exit(1);
        }

if (!argerrors)
	{
	gbActivity = true;
	if(FileLogLevel->count && !LogFile->count)
		{
		printf("\nError: FileLogLevel '-f%d' specified but no logfile '-F<logfile>'",FileLogLevel->ival[0]);
		exit(1);
		}

	iScreenLogLevel = iFileLogLevel = FileLogLevel->count ? FileLogLevel->ival[0] : eDLInfo;
	if(iFileLogLevel < eDLNone || iFileLogLevel > eDLDebug)
		{
		printf("\nError: FileLogLevel '-l%d' specified outside of range %d..%d",iFileLogLevel,eDLNone,eDLDebug);
		exit(1);
		}
	if(LogFile->count)
		{
		strncpy(szLogFile,LogFile->filename[0],_MAX_PATH);
		szLogFile[_MAX_PATH-1] = '\0';
		}
	else
		{
		iFileLogLevel = eDLNone;
		szLogFile[0] = '\0';
		}

	PMode = (etPMode)(pmode->count ? pmode->ival[0] : ePMExonCnts);
	if(PMode < ePMExonCnts || PMode >= ePMplaceholder)
		{
		printf("\nError: Processing mode '-m%d' specified outside of range %d..%d",PMode,0,(int)ePMplaceholder-1);
		exit(1);
		}

	bFeatLen = bfeatlen->count ? true : false;

	MinFeatLen = minfeatlen->count ? minfeatlen->ival[0] : 0;
	if(MinFeatLen < 0 || MinFeatLen >= 1000000)
		{
		printf("\nError: Minimum feature length '-l%d' specified outside of range 0..1000000",MinFeatLen);
		exit(1);
		}

	MaxFeatLen = maxfeatlen->count ? maxfeatlen->ival[0] : 0;
	if(MaxFeatLen < 0 || MaxFeatLen >= 1000000)
		{
		printf("\nError: Maximum feature length '-L%d' specified outside of range 0..1000000",MaxFeatLen);
		exit(1);
		}

	MinCtrlCnts = minctrlcnts->count ? minctrlcnts->ival[0] : 0;
	if(MinCtrlCnts < 0 || MinCtrlCnts >= 1000000)
		{
		printf("\nError: Minimum control counts '-c%d' specified outside of range 0..1000000",MinCtrlCnts);
		exit(1);
		}

	MinExprCnts = minexprcnts->count ? minexprcnts->ival[0] : 0;
	if(MinExprCnts < 0 || MinExprCnts >= 1000000)
		{
		printf("\nError: Minimum experiment counts '-e%d' specified outside of range 0..1000000",MinExprCnts);
		exit(1);
		}

	MinCtrlExprCnts = minctrlexprcnts->count ? minctrlexprcnts->ival[0] : 0;
	if(MinCtrlExprCnts < 0 || MinCtrlExprCnts >= 1000000)
		{
		printf("\nError: Minimum control and experiment counts '-C%d' specified outside of range 0..1000000",MinCtrlExprCnts);
		exit(1);
		}


	NormCntsScale = (double)(normcntsscale->count ? normcntsscale->dval[0] : cNormCntsScale);
	if(NormCntsScale < 0.0001 || NormCntsScale >= 1000.0)
		{
		printf("\nError: Counts normalisation scale factor '-m%f' specified outside of range %f..%f",NormCntsScale,0.001,1000.0);
		exit(1);
		}

	strncpy(szTitleControl,titlecontrol->sval[0],sizeof(szTitleControl));
	szTitleControl[sizeof(szTitleControl)-1] = '\0';
	CUtility::TrimQuotedWhitespcExtd(szTitleControl);
	if(!strlen(szTitleControl))
		{
		printf("\nError: After removal of whitespace, no control title specified with '-t<title>' option)");
		exit(1);
		}

	strncpy(szTitleExperiment,titleexperiment->sval[0],sizeof(szTitleExperiment));
	szTitleExperiment[sizeof(szTitleExperiment)-1] = '\0';
	CUtility::TrimQuotedWhitespcExtd(szTitleExperiment);
	if(!strlen(szTitleExperiment))
		{
		printf("\nError: After removal of whitespace, no experiment title specified with '-T<title>' option)");
		exit(1);
		}

	if(!incontrolfiles->count)
		{
		printf("\nError: No input control file(s) specified with with '-i<filespec>' option)");
		exit(1);
		}

	for(NumInputControlFiles=Idx=0;NumInputControlFiles < cMaxInFileSpecs && Idx < incontrolfiles->count; Idx++)
		{
		pszInControlFileSpecs[Idx] = NULL;
		if(pszInControlFileSpecs[NumInputControlFiles] == NULL)
			pszInControlFileSpecs[NumInputControlFiles] = new char [_MAX_PATH];
		strncpy(pszInControlFileSpecs[NumInputControlFiles],incontrolfiles->filename[Idx],_MAX_PATH);
		pszInControlFileSpecs[NumInputControlFiles][_MAX_PATH-1] = '\0';
		CUtility::TrimQuotedWhitespcExtd(pszInControlFileSpecs[NumInputControlFiles]);
		if(pszInControlFileSpecs[NumInputControlFiles][0] != '\0')
			NumInputControlFiles++;
		}

	if(!NumInputControlFiles)
		{
		printf("\nError: After removal of whitespace, no input file(s) specified with '-i<filespec>' option)");
		exit(1);
		}

	if(inexperfiles->count)
		{
		for(NumInputExprFiles=Idx=0;NumInputExprFiles < cMaxInFileSpecs && Idx < inexperfiles->count; Idx++)
			{
			pszInExprFileSpecs[Idx] = NULL;
			if(pszInExprFileSpecs[NumInputExprFiles] == NULL)
				pszInExprFileSpecs[NumInputExprFiles] = new char [_MAX_PATH];
			strncpy(pszInExprFileSpecs[NumInputExprFiles],inexperfiles->filename[Idx],_MAX_PATH);
			pszInExprFileSpecs[NumInputExprFiles][_MAX_PATH-1] = '\0';
			CUtility::TrimQuotedWhitespcExtd(pszInExprFileSpecs[NumInputExprFiles]);
			if(pszInExprFileSpecs[NumInputExprFiles][0] != '\0')
				NumInputExprFiles++;
			}

		if(!NumInputExprFiles)
			{
			printf("\nError: After removal of whitespace, no input file(s) specified with '-I<filespec>' option)");
			exit(1);
			}
		}
	else
		{
		pszInExprFileSpecs[0] = NULL;
		NumInputExprFiles = 0;
		}
	strncpy(szOutfile,outfile->filename[0],_MAX_PATH);
	szOutfile[_MAX_PATH-1] = '\0';
	CUtility::TrimQuotedWhitespcExtd(szOutfile);
	if(szOutfile[0] == '\0')
		{
		printf("\nError: No output file ('-o<file> option') specified after removal of leading/trailing quotes and whitespace");
		exit(1);
		}

			// now that command parameters have been parsed then initialise diagnostics log system
	if(!gDiagnostics.Open(szLogFile,(etDiagLevel)iScreenLogLevel,(etDiagLevel)iFileLogLevel,true))
		{
		printf("\nError: Unable to start diagnostics subsystem.");
		if(szLogFile[0] != '\0')
			printf(" Most likely cause is that logfile '%s' can't be opened/created",szLogFile);
		exit(1);
		}

	gDiagnostics.DiagOut(eDLInfo,gszProcName,"Version: %s Processing parameters:",cpszProgVer);

	const char *pszProcMode;
	switch(PMode) {
		case ePMRPKM:
			pszProcMode = "Processing maploci2feature generated csv input files for RPKMs";
			break;
		case ePMExonCnts:
			pszProcMode = "Processing maploci2feature generated csv input files total exon counts";
			break;
		default:
			pszProcMode = "Unknown mode, defaulting to processing maploci2feature generated csv input files into DESeq tab delimited file";
			PMode = ePMExonCnts;
			break;
		};
	gDiagnostics.DiagOutMsgOnly(eDLInfo,"Processing mode: '%s'",pszProcMode);
	gDiagnostics.DiagOutMsgOnly(eDLInfo,"Report feature length: '%s'",bFeatLen ? "Yes" : "No");
	gDiagnostics.DiagOutMsgOnly(eDLInfo,"Minimum feature length: %d",MinFeatLen);
	gDiagnostics.DiagOutMsgOnly(eDLInfo,"Maximum feature length: %d",MaxFeatLen);

	gDiagnostics.DiagOutMsgOnly(eDLInfo,"Minimum sum of feature control counts or RPKM: %d",MinCtrlCnts);
	gDiagnostics.DiagOutMsgOnly(eDLInfo,"Minimum sum of feature experimental counts or RPKM: %d",MinExprCnts);
	gDiagnostics.DiagOutMsgOnly(eDLInfo,"Minimum sum of feature control and experiment counts or RPKM: %d",MinCtrlExprCnts);


	gDiagnostics.DiagOutMsgOnly(eDLInfo,"counts normalisation scale factor: %0.3f",NormCntsScale);

	gDiagnostics.DiagOutMsgOnly(eDLInfo,"Control title: '%s'",szTitleControl);
	gDiagnostics.DiagOutMsgOnly(eDLInfo,"Experiment title: '%s'",szTitleExperiment);
	
	for(Idx=0; Idx < NumInputControlFiles; Idx++)
		gDiagnostics.DiagOutMsgOnly(eDLInfo,"input maploci2feature control file (%d): '%s'",Idx+1,pszInControlFileSpecs[Idx]);

	if(NumInputExprFiles)
		{
		for(Idx=0; Idx < NumInputExprFiles; Idx++)
			gDiagnostics.DiagOutMsgOnly(eDLInfo,"input maploci2feature experiment file (%d): '%s'",Idx+1,pszInExprFileSpecs[Idx]);
		}
	else
		gDiagnostics.DiagOutMsgOnly(eDLInfo,"no input maploci2feature experiment files specified, control only counts processing");

	gDiagnostics.DiagOutMsgOnly(eDLInfo,"output file to create: '%s'", szOutfile);
#ifdef _WIN32
	SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
#endif
	gStopWatch.Start();
	Rslt = Process(PMode,						// processing mode
					bFeatLen,					// true if feature length also to be reported
				    MinFeatLen,					// feature has to be at least this length
					MaxFeatLen,					// but no longer than this length
					MinCtrlCnts,				// sum of control counts for a feature must be at least this
					MinExprCnts,				// sum of experiment counts for a feature must be at least this
					MinCtrlExprCnts,			// sum of control and experiment counts for a feature must be at least this
					NormCntsScale,				// counts normalisation scale factor
					szTitleControl,				// control title
					szTitleExperiment,			// experiment title
					NumInputControlFiles,		// number of input control file specs
					pszInControlFileSpecs,		// names of control input files (wildcards allowed)
					NumInputExprFiles,			// number of input experiment file specs
					pszInExprFileSpecs,			// names of experiment input files (wildcards allowed)
					szOutfile);					// output into this file
	gStopWatch.Stop();
	Rslt = Rslt >=0 ? 0 : 1;
	gDiagnostics.DiagOut(eDLInfo,gszProcName,"Exit code: %d Total processing time: %s",Rslt,gStopWatch.Read());
	exit(Rslt);
	}
else
	{
	printf("\n%s process for DESeq, Version %s\n", gszProcName,cpszProgVer);
	arg_print_errors(stdout,end,gszProcName);
	arg_print_syntax(stdout,argtable,"\nUse '-h' to view option and parameter usage\n");
	exit(1);
	}
return 0;
}



const int cDataBuffAlloc = 0x0fffffff;		// allocation size to hold features

etPMode m_PMode;							// processing mode
double m_NormCntsScale;						// normalise the counts by scaling with this factor
UINT32 m_NumSampleSets;						// total number of sample datasets (1 per input file)
UINT32 m_NumExperimentSets;					// number of experiment datasets (1 per input file)
UINT32 m_NumControlSets;					// number of control datasets (1 per input file)
char *m_pSampleSetNames;					// array of sampleset names

tsFeatRPKM *m_pFeatRPKMs = NULL;
int m_AllocdFeatRPMs = 0;
int m_NumFeatRPMs = 0;

int m_NumUnderLength;						// number of features under length
int m_NumOverLength;						// number of features over length
int m_NumUnderCnts;							// number of features with too few counts

size_t m_DataBuffLen = 0;					// total memory allocated 
size_t m_DataBuffOfs = 0;					// offset at which to next write

UINT8 *m_pWrtBuff;							// used to buffer output writes to file

int m_hOutFile;								// output results file


void
Reset(void)
{
if(m_hOutFile != -1)
	{
	close(m_hOutFile);
	m_hOutFile = -1;
	}
if(m_pFeatRPKMs != NULL)
	{
	free(m_pFeatRPKMs);
	m_pFeatRPKMs = NULL;
	}
if(m_pWrtBuff != NULL)
	{
	delete m_pWrtBuff;
	m_pWrtBuff = NULL;
	}
if(m_pSampleSetNames != NULL)
	{
	delete m_pSampleSetNames;
	m_pSampleSetNames = NULL;
	}
m_AllocdFeatRPMs = 0;
m_NumFeatRPMs = 0;
m_NumSampleSets = 0;
m_NumExperimentSets = 0;
m_NumControlSets = 0;
m_NormCntsScale = cNormCntsScale;
}

void
Init(void)
{
m_pFeatRPKMs = NULL;
m_pWrtBuff = NULL;
m_pSampleSetNames = NULL;
m_hOutFile = -1;
Reset();
}

teBSFrsltCodes
Process(etPMode PMode,						// processing mode
        bool bFeatLen,					// if true then also output the feature length
		int MinFeatLen,					// feature has to be at least this length
		int MaxFeatLen,					// but no longer than this length
		int MinCtrlCnts,				// sum of control counts for a feature must be at least this
		int MinExprCnts,				// sum of experiment counts for a feature must be at least this
		int MinCtrlExprCnts,			// sum of control and experiment counts for a feature must be at least this
		double NormCntsScale,				// counts normalisation scale factor
		char *pszControlTitle,				// control title
		char *pszExprTitle,					// experiment title
		int	NumInputControlFiles,			// number of input control file specs
		char *pszInControlFileSpecs[],		// names of control input files (wildcards allowed)
		int	NumInputExprFiles,				// number of input experiment file specs
		char *pszInExprFileSpecs[],			// names of experiment input files (wildcards allowed)
		char *pszOutfile)					// output into this file

{
teBSFrsltCodes Rslt;
char *pszInfile;
int Idx;			// general processing iteration index
int NumControls;

Init();
m_PMode = PMode;
m_NormCntsScale = NormCntsScale;

m_NumSampleSets = 0;
m_NumExperimentSets = 0;
m_NumControlSets = 0;


if((m_pSampleSetNames = new char [cMaxSampleSets * cMaxSamplesetNameLen]) == NULL) // array of sampleset names
	{
	gDiagnostics.DiagOut(eDLFatal,gszProcName,"Unable to allocate memory for samplenames");
	Reset();
	return(eBSFerrMem);
	}

#ifdef _WIN32
if((m_hOutFile = open(pszOutfile, _O_RDWR | _O_BINARY | _O_SEQUENTIAL | _O_CREAT | _O_TRUNC, _S_IREAD | _S_IWRITE ))==-1)
#else
if((m_hOutFile = open(pszOutfile,O_RDWR | O_CREAT |O_TRUNC, S_IREAD | S_IWRITE))==-1)
#endif
	{
	gDiagnostics.DiagOut(eDLFatal,gszProcName,"Unable to create or truncate %s - %s",pszOutfile,strerror(errno));
	Reset();
	return(eBSFerrCreateFile);
	}

gDiagnostics.DiagOut(eDLInfo,gszProcName,"Output results file created/truncated: '%s'",pszOutfile);

// process the control files
CSimpleGlob glob(SG_GLOB_FULLSORT);
for(Idx = 0; Idx < NumInputControlFiles; Idx++)
	{
	glob.Init();
	if(glob.Add(pszInControlFileSpecs[Idx]) < SG_SUCCESS)
		{
		gDiagnostics.DiagOut(eDLFatal,gszProcName,"Unable to glob '%s",pszInControlFileSpecs[Idx]);
		Reset();
		return(eBSFerrOpnFile);	// treat as though unable to open file
		}

	if(glob.FileCount() <= 0)
		{
		gDiagnostics.DiagOut(eDLFatal,gszProcName,"Unable to locate any source file matching '%s",pszInControlFileSpecs[Idx]);
		continue;
		}

	Rslt = eBSFSuccess;
	for (int FileID = 0; Rslt >= eBSFSuccess &&  FileID < glob.FileCount(); FileID+=1 )
		{
		pszInfile = glob.File(FileID);
		m_NumUnderLength = 0;
		m_NumOverLength = 0;
		m_NumUnderCnts = 0;
		m_NumSampleSets += 1;
		sprintf(&m_pSampleSetNames[(m_NumSampleSets-1) * cMaxSamplesetNameLen],"%s:%d",pszControlTitle,m_NumSampleSets);
		gDiagnostics.DiagOut(eDLInfo,gszProcName,"Processing input file '%s' as '%s'\n",pszInfile,&m_pSampleSetNames[(m_NumSampleSets-1) * cMaxSamplesetNameLen]);
		
		Rslt = LoadFile(m_NumSampleSets,MinFeatLen,MaxFeatLen,MinCtrlCnts,pszInfile);
		if(Rslt != eBSFSuccess)
			{
			gDiagnostics.DiagOut(eDLFatal,gszProcName,"Process: Load failed for input file '%s'\n",pszInfile);
			Reset();
			return(Rslt);
			}
		if(m_NumUnderLength)
			gDiagnostics.DiagOut(eDLInfo,gszProcName,"Process: %d features filtered out because under length",m_NumUnderLength);
		if(m_NumOverLength)
			gDiagnostics.DiagOut(eDLInfo,gszProcName,"Process: %d features filtered out because over length",m_NumOverLength);
		if(m_NumUnderCnts)
			gDiagnostics.DiagOut(eDLInfo,gszProcName,"Process: %d features filtered out because insufficient exon/intron counts or RPKM",m_NumUnderCnts);
		}
	}
if(!m_NumSampleSets)
	{
	gDiagnostics.DiagOut(eDLFatal,gszProcName,"Process: Unable to load any control input files");
	Reset();
	return(eBSFerrOpnFile);
	}

m_NumControlSets = m_NumSampleSets;

// process the experiment files (if any)
NumControls = m_NumSampleSets;
if(NumInputExprFiles)
	{
	for(Idx = 0; Idx < NumInputExprFiles; Idx++)
		{
		glob.Init();
		if(glob.Add(pszInExprFileSpecs[Idx]) < SG_SUCCESS)
			{
			gDiagnostics.DiagOut(eDLFatal,gszProcName,"Unable to glob '%s",pszInExprFileSpecs[Idx]);
			Reset();
			return(eBSFerrOpnFile);	// treat as though unable to open file
			}

		if(glob.FileCount() <= 0)
			{
			gDiagnostics.DiagOut(eDLFatal,gszProcName,"Unable to locate any source file matching '%s",pszInExprFileSpecs[Idx]);
			continue;
			}

		Rslt = eBSFSuccess;
		for (int FileID = 0; Rslt >= eBSFSuccess &&  FileID < glob.FileCount(); FileID += 1)
			{
			pszInfile = glob.File(FileID);
			m_NumUnderLength = 0;
			m_NumOverLength = 0;
			m_NumUnderCnts = 0;
			m_NumSampleSets += 1;
			sprintf(&m_pSampleSetNames[(m_NumSampleSets-1) * cMaxSamplesetNameLen],"%s:%d",pszExprTitle,m_NumSampleSets);
			gDiagnostics.DiagOut(eDLInfo,gszProcName,"Processing input file '%s' as '%s'\n",pszInfile,&m_pSampleSetNames[(m_NumSampleSets-1) * cMaxSamplesetNameLen]);
		
			Rslt = LoadFile(m_NumSampleSets,MinFeatLen,MaxFeatLen,MinExprCnts,pszInfile);
			if(Rslt != eBSFSuccess)
				{
				gDiagnostics.DiagOut(eDLFatal,gszProcName,"Process: Load failed for input file '%s'\n",pszInfile);
				Reset();
				return(Rslt);
				}
			if(m_NumUnderLength)
				gDiagnostics.DiagOut(eDLInfo,gszProcName,"Process: %d features filtered out because under length",m_NumUnderLength);
			if(m_NumOverLength)
				gDiagnostics.DiagOut(eDLInfo,gszProcName,"Process: %d features filtered out because over length",m_NumOverLength);
			if(m_NumUnderCnts)
				gDiagnostics.DiagOut(eDLInfo,gszProcName,"Process: %d features filtered out because insufficient exon/intron counts or RPKM",m_NumUnderCnts);
			}
		}
	if(NumControls == m_NumSampleSets)
		{
		gDiagnostics.DiagOut(eDLFatal,gszProcName,"Process: Unable to load any experiment input files");
		Reset();
		return(eBSFerrOpnFile);
		}

	m_NumExperimentSets = m_NumSampleSets - m_NumControlSets;
	}
else
	m_NumExperimentSets = 0;

#ifdef _DEBUG
#ifdef _WIN32
_ASSERTE( _CrtCheckMemory());
#endif
#endif
gDiagnostics.DiagOut(eDLInfo,gszProcName,"%d input files were accepted for processing (%d control, %d experiment)", m_NumSampleSets,m_NumControlSets,m_NumExperimentSets);
if(m_NumSampleSets == 0)
	{
	gDiagnostics.DiagOut(eDLInfo,gszProcName,"Nothing to do, no files to be processed");
	return(eBSFSuccess);
	}



gDiagnostics.DiagOut(eDLInfo,gszProcName,"Processing elements...\n");
Rslt = (teBSFrsltCodes)WriteoutDESeq(bFeatLen,MinCtrlExprCnts);
gDiagnostics.DiagOut(eDLInfo,gszProcName,"Processing completed\n");
Reset();	// will close output file

return(eBSFSuccess);
}

teBSFrsltCodes
LoadFile(int SampleSetID,					// uniquely identifies experiment
		 int MinFeatLen,					// feature has to be at least this length
		 int MaxFeatLen,					// but no longer than this length
		 int MinExonIntronCnts,				// must be at least this many counts in exons/introns
		 char *pszInFile)					// process from this file
{
int Rslt;
int CSVLineNum;
int LenCSVline;
char szCSVline[128];
int NumFields;
int NumLines;
int FeatID;
char *pszFeatName;
int FeatLen;		// feature length
int ExonIntronCnts; // sum of all exon and intron cnts
int TmpCnt;

int NumUnderLength;
int NumOverLength;
int NumUnderCnts;

double RPKM;
CCSVFile *m_pCSVFile;					// used for processing input CSV file
if((m_pCSVFile = new CCSVFile)==NULL)
	{
	gDiagnostics.DiagOut(eDLFatal,gszProcName,"Unable to instantiate CCSVFile");
	Reset();
	return(eBSFerrObj);
	}

if((Rslt=m_pCSVFile->Open(pszInFile)) !=eBSFSuccess)
	{
	while(m_pCSVFile->NumErrMsgs())
		gDiagnostics.DiagOut(eDLFatal,gszProcName,m_pCSVFile->GetErrMsg());
	gDiagnostics.DiagOut(eDLFatal,gszProcName,"Unable to open '%s' for processing",pszInFile);
	delete m_pCSVFile;
	Reset();
	return(eBSFerrOpnFile);
	}
NumLines = 0;
NumUnderLength = 0;
NumOverLength = 0;
NumUnderCnts = 0;
while((Rslt=m_pCSVFile->NextLine()) > 0)	// onto next line containing fields
	{
	NumLines += 1;
	NumFields = m_pCSVFile->GetCurFields();
	if(m_PMode == ePMRPKM && NumFields < 13)
		{
		CSVLineNum = m_pCSVFile->GetLineNumber();
		gDiagnostics.DiagOut(eDLFatal,gszProcName,"Expected at least 13 fields at line %d in '%s', GetCurFields() returned '%d'",CSVLineNum,pszInFile,NumFields);
		LenCSVline = m_pCSVFile->GetLine(sizeof(szCSVline)-1,szCSVline);
		szCSVline[LenCSVline] = '\0';
		gDiagnostics.DiagOut(eDLFatal,gszProcName,"Line %d contents: '%s'",CSVLineNum,szCSVline);
		delete m_pCSVFile;
		return(eBSFerrFieldCnt);
		}
	else
		if(m_PMode == ePMExonCnts && NumFields < 14)
			{
			CSVLineNum = m_pCSVFile->GetLineNumber();
			gDiagnostics.DiagOut(eDLFatal,gszProcName,"Expected at least 14 fields at line %d in '%s', GetCurFields() returned '%d'",CSVLineNum,pszInFile,NumFields);
			LenCSVline = m_pCSVFile->GetLine(sizeof(szCSVline)-1,szCSVline);
			szCSVline[LenCSVline] = '\0';
			gDiagnostics.DiagOut(eDLFatal,gszProcName,"Line %d contents: '%s'",CSVLineNum,szCSVline);
			delete m_pCSVFile;
			return(eBSFerrFieldCnt);
			}

	if(NumLines == 1)		// 1st line should be a title line 
		{
		m_pCSVFile->GetText(1,&pszFeatName);
		if(!stricmp(pszFeatName,"FeatID"))
			continue;
		gDiagnostics.DiagOut(eDLFatal,gszProcName,"Expected title line as the 1st line in %s",pszInFile);
		delete m_pCSVFile;
		return(eBSFerrFieldCnt);
		}

	m_pCSVFile->GetInt(1,&FeatID);
	m_pCSVFile->GetText(2,&pszFeatName);
	m_pCSVFile->GetInt(3,&FeatLen);

	if(MinFeatLen > 0 && FeatLen < MinFeatLen)
		{
		NumUnderLength += 1;
		continue;
		}

	if(MaxFeatLen > 0 && FeatLen > MaxFeatLen)
		{
		NumOverLength += 1;
		continue;
		}

	switch(m_PMode) {
		case ePMRPKM:
			m_pCSVFile->GetDouble(13,&RPKM);
			break;
		case ePMExonCnts:
			m_pCSVFile->GetDouble(14,&RPKM);
			break;
		}

	ExonIntronCnts = 0;
	m_pCSVFile->GetInt(5,&TmpCnt);	// CDS
	ExonIntronCnts += TmpCnt;
	m_pCSVFile->GetInt(6,&TmpCnt);	// 5'UTR
	ExonIntronCnts += TmpCnt;
	m_pCSVFile->GetInt(7,&TmpCnt);  // 3'UTR
	ExonIntronCnts += TmpCnt;
	m_pCSVFile->GetInt(8,&TmpCnt);	// Intron
	ExonIntronCnts += TmpCnt;

	if(MinExonIntronCnts > 0 && (ExonIntronCnts < MinExonIntronCnts || RPKM  < (double)MinExonIntronCnts))
		{
		NumUnderCnts += 1;
		continue;
		}

	if((Rslt=AddFeatureRPKM(SampleSetID,FeatID,FeatLen,pszFeatName,ExonIntronCnts,RPKM))!=eBSFSuccess)
		{
		delete m_pCSVFile;
		if(m_pFeatRPKMs == NULL)
			{
			free(m_pFeatRPKMs);
			m_pFeatRPKMs = NULL;
			}
		return((teBSFrsltCodes)Rslt);
		}
	}

delete m_pCSVFile;
m_NumUnderLength += NumUnderLength;
m_NumOverLength += NumOverLength;
m_NumUnderCnts += NumUnderCnts;
return((teBSFrsltCodes)Rslt);
}



int
AddFeatureRPKM(int SampleSetID,			// processed from this experiment
				UINT32 FeatID,				// unique feature identifier within this experiment
				UINT32 FeatLen,				// feature length
			   char *pszFeatName,			// feature name
			   UINT32 ExonIntronCnts,		// total number of exon + intron counts ascribed to this feature
			   double RPKM)					// associated RPKM
{
size_t memreq;

// just keep adding to array of features (assume 64bytes/feature) so for 20 experiments times say 100K features this requires a couple of hundred MB - easy!
// when all features added then can do a sort on feature name plus experiment, then iterate and output the DESeq file
tsFeatRPKM *pFeatRPKM;

if(m_pFeatRPKMs == NULL)
	{
	memreq = sizeof(tsFeatRPKM) * cAllocFeatRPKMs;
	if((m_pFeatRPKMs = (tsFeatRPKM *)malloc(memreq))==NULL)
		{
		gDiagnostics.DiagOut(eDLFatal,gszProcName,"AddFeatureRPKM: Memory allocation of %lld bytes - %s",(INT64)memreq,strerror(errno));
		Reset();
		return(eBSFerrMem);
		}
	m_AllocdFeatRPMs = cAllocFeatRPKMs;
	m_NumFeatRPMs = 0;
	}
else
	{
	if(m_NumFeatRPMs == m_AllocdFeatRPMs)
		{
		memreq = sizeof(tsFeatRPKM) * (m_AllocdFeatRPMs + cAllocFeatRPKMs);
		if((pFeatRPKM = (tsFeatRPKM *)realloc(m_pFeatRPKMs,memreq))==NULL)
			{
			gDiagnostics.DiagOut(eDLFatal,gszProcName,"AddFeatureRPKM: Memory allocation of %lld bytes - %s",(INT64)memreq,strerror(errno));
			Reset();
			return(eBSFerrMem);
			}
		m_pFeatRPKMs = pFeatRPKM;
		m_AllocdFeatRPMs += cAllocFeatRPKMs;
		}
	}
pFeatRPKM = &m_pFeatRPKMs[m_NumFeatRPMs++];
pFeatRPKM->SampleSetID = SampleSetID;
pFeatRPKM->ExonIntronCnts = ExonIntronCnts;
pFeatRPKM->FeatID = FeatID;
pFeatRPKM->FeatLen = FeatLen;
pFeatRPKM->RPKM = RPKM;
strncpy(pFeatRPKM->szFeatName,pszFeatName,sizeof(pFeatRPKM->szFeatName)); 
pFeatRPKM->szFeatName[cMaxFeatNameLen-1] = '\0';
return(eBSFSuccess);
}

int
WriteoutDESeq(bool bFeatLens,			// if true then write out the feature length
				int MinExonIntronCnts)	// only write out those features having sum of exon/intron counts at least MinExonIntronCnts	
{
UINT32 ExpIdx;
UINT32 CurExperimentID;
UINT32 Cnts[cMaxSampleSets];
tsFeatRPKM *pFeatRPKM;
tsFeatRPKM *pMrkFeatRPKM;
char *pszCurFeature;
char szBuff[16000];
int BuffIdx;
UINT32 NumSamples;
UINT32 TotExonIntronCnts;
UINT32 NumMinExonIntronCnts;
double TotRPKM;
UINT32 NumFeats;
UINT32 NumFeaturesDEd;
UINT32 FeatLen;
int Idx;

if(m_pFeatRPKMs == NULL)
	return(-1);
if(m_NumFeatRPMs > 1)
	qsort(m_pFeatRPKMs,m_NumFeatRPMs,sizeof(tsFeatRPKM),SortFeatRPKMs);


pFeatRPKM = m_pFeatRPKMs;
pMrkFeatRPKM = pFeatRPKM;
pszCurFeature = NULL;
TotExonIntronCnts = 0;
TotRPKM = 0;
NumMinExonIntronCnts = 0;
NumSamples = 0;
NumFeats = 0;
for(Idx = 0; Idx < m_NumFeatRPMs; Idx++,pFeatRPKM++)
	{
	if(Idx == 0)
		{
		pMrkFeatRPKM = pFeatRPKM;
		pszCurFeature = pFeatRPKM->szFeatName;
		TotExonIntronCnts = pFeatRPKM->ExonIntronCnts;
		TotRPKM = pFeatRPKM->RPKM;
		NumSamples = 1;
		continue;
		}
	// ensure all control+experiments are present
	if(!stricmp(pszCurFeature,pFeatRPKM->szFeatName))	// same feature?
		{
		NumSamples += 1;
		TotExonIntronCnts += pFeatRPKM->ExonIntronCnts;
		TotRPKM += pFeatRPKM->RPKM;
		}
	else
		{
		// should have been exactly m_NumSampleSets samples; also check that the min exon/intron cnts or RPKM is above the min
		if(NumSamples < m_NumSampleSets || (MinExonIntronCnts > 0 && (TotExonIntronCnts < (UINT32)MinExonIntronCnts || TotRPKM < (double)MinExonIntronCnts)))
			{
			if(NumSamples >= m_NumSampleSets)
				NumMinExonIntronCnts += 1;
			while(pMrkFeatRPKM != pFeatRPKM)
				{
				pMrkFeatRPKM->FeatID = 0;
				pMrkFeatRPKM += 1;
				}
			}
		else
			NumFeats += 1;
		pMrkFeatRPKM = pFeatRPKM;
		pszCurFeature = pFeatRPKM->szFeatName;
		TotExonIntronCnts = pFeatRPKM->ExonIntronCnts;
		TotRPKM = pFeatRPKM->RPKM;
		NumSamples = 1;
		}
	}

if(NumSamples < m_NumSampleSets || (MinExonIntronCnts > 0 && (TotExonIntronCnts < (UINT32)MinExonIntronCnts || TotRPKM < (double)MinExonIntronCnts)))
	{
	if(NumSamples >= m_NumSampleSets)
		NumMinExonIntronCnts += 1;

	while(pMrkFeatRPKM != pFeatRPKM)
		{
		pMrkFeatRPKM->FeatID = 0;
		pMrkFeatRPKM += 1;
		}
	}
else
	NumFeats += 1;

gDiagnostics.DiagOut(eDLInfo,gszProcName,"Removed %d features because total exon/intron counts, or RPKM less than %d",NumMinExonIntronCnts,MinExonIntronCnts);

if(!NumFeats)
	{
	gDiagnostics.DiagOut(eDLInfo,gszProcName,"Nothing to do as no features remain!");
	close(m_hOutFile);
	m_hOutFile = -1;
	return(0);
	}

BuffIdx = 0;
if(bFeatLens)
	BuffIdx += sprintf(&szBuff[BuffIdx],"Feature\tLength");
for(ExpIdx = 0; ExpIdx < m_NumSampleSets; ExpIdx++)
	BuffIdx += sprintf(&szBuff[BuffIdx],"\t%s",&m_pSampleSetNames[ExpIdx * cMaxSamplesetNameLen]);
pFeatRPKM = m_pFeatRPKMs;
pszCurFeature = NULL;
NumFeaturesDEd = 0;
for(int Idx = 0; Idx < m_NumFeatRPMs; Idx++,pFeatRPKM++)
	{
	if(pFeatRPKM->FeatID == 0)			// 0 if feature has been filtered out because of insufficient counts
		continue;

	if(pszCurFeature == NULL || stricmp(pszCurFeature,pFeatRPKM->szFeatName))
		{
		if(pszCurFeature != NULL)
			{
			BuffIdx += sprintf(&szBuff[BuffIdx],"\n%s",pszCurFeature);
			if(bFeatLens)
				BuffIdx += sprintf(&szBuff[BuffIdx],"\t%d",FeatLen);
			for(ExpIdx = 0; ExpIdx < m_NumSampleSets; ExpIdx++)
				BuffIdx += sprintf(&szBuff[BuffIdx],"\t%d",Cnts[ExpIdx]);
			NumFeaturesDEd += 1;
			if(BuffIdx > (sizeof(szBuff)-500))
				{
				CUtility::SafeWrite(m_hOutFile,szBuff,BuffIdx);
				BuffIdx = 0;
				}
			}
		pszCurFeature = pFeatRPKM->szFeatName;
		memset(Cnts,0,sizeof(Cnts));
		CurExperimentID = pFeatRPKM->SampleSetID;
		FeatLen = pFeatRPKM->FeatLen;
		}

	if(pFeatRPKM->RPKM < 0.0 || pFeatRPKM->RPKM > 250000000.0)
		gDiagnostics.DiagOut(eDLFatal,gszProcName,"WriteoutDESeq: Check - %s - as counts are rather abnormal at %f",pFeatRPKM->szFeatName,pFeatRPKM->RPKM);
	Cnts[pFeatRPKM->SampleSetID-1] = (UINT32)(0.5 + (pFeatRPKM->RPKM * m_NormCntsScale));
	if(((int)Cnts[pFeatRPKM->SampleSetID-1]) < 0 || Cnts[pFeatRPKM->SampleSetID-1] > 250000000)
		gDiagnostics.DiagOut(eDLFatal,gszProcName,"WriteoutDESeq: Check - %s - as scaled counts are rather abnormal at %d",pFeatRPKM->szFeatName,pFeatRPKM->RPKM);
	}

if(pszCurFeature != NULL && pszCurFeature[0] != '\0')
	{	
	BuffIdx += sprintf(&szBuff[BuffIdx],"\n%s",pszCurFeature);
	if(bFeatLens)
		BuffIdx += sprintf(&szBuff[BuffIdx],"\t%d",FeatLen);
	for(ExpIdx = 0; ExpIdx < m_NumSampleSets; ExpIdx++)
		BuffIdx += sprintf(&szBuff[BuffIdx],"\t%d",Cnts[ExpIdx]);
	NumFeaturesDEd += 1;
	}
if(BuffIdx)
	CUtility::SafeWrite(m_hOutFile,szBuff,BuffIdx);
close(m_hOutFile);
m_hOutFile = -1;
gDiagnostics.DiagOut(eDLInfo,gszProcName,"Generated DE counts for %d features",NumFeaturesDEd);
return(0);
}


// SortFeatRPKMs
// Sort by ascending tsFeatRPKM.szFeatName then tsFeatRPKM.SampleSetID 
static int
SortFeatRPKMs(const void *arg1, const void *arg2)
{
int Cmp;
tsFeatRPKM *pEl1 = (tsFeatRPKM *)arg1;
tsFeatRPKM *pEl2 = (tsFeatRPKM *)arg2;

if((Cmp = stricmp(pEl1->szFeatName,pEl2->szFeatName))!=0)
		return(Cmp);

if(pEl1->SampleSetID > pEl2->SampleSetID )
	return(1);
if(pEl1->SampleSetID < pEl2->SampleSetID )
		return(-1);

if(pEl1->FeatID > pEl2->FeatID )
	return(1);
if(pEl1->FeatID < pEl2->FeatID )
		return(-1);
return(0);
}
