/*
 * CSIRO Open Source Software License Agreement (variation of the BSD / MIT License)
 * Copyright (c) 2017, Commonwealth Scientific and Industrial Research Organisation (CSIRO) ABN 41 687 119 230.
 * See LICENCE for the complete licence information (https://github.com/csiro-crop-informatics/biokanga/LICENSE)
 * Contact: Alex Whan <alex.whan@csiro.au>
 */
#include "stdafx.h"

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#if _WIN32
#include <process.h>
#include "../libbiokanga/commhdrs.h"
#else
#include <sys/mman.h>
#include <pthread.h>
#include "../libbiokanga/commhdrs.h"
#endif

#include "pacbiokanga.h"
#include "../libbiokanga/bgzf.h"
#include "PacBioUtility.h"
#include "MAFKMerDist.h"

// currently the minimum sequence length and consensus scores are not actually processed
#undef SQLENCONSENSUSIMP

int
		GenKmerDistFromMAF(int MinSeqLen,		// sequences must be of at least this minimum length
			 int MinConcScore,					// K-mer distributions from subsequences with consensus score to be at least this threshold
			char *pszKmerDistFile,				// name of file into which write error corrected sequences
			char *pszMultiAlignFile);			// name of file containing multiple alignments to process


#ifdef _WIN32
int ProcKMerDist(int argc, char* argv[])
{
// determine my process name
_splitpath(argv[0],NULL,NULL,gszProcName,NULL);
#else
int
ProcKMerDist(int argc, char** argv)
{
// determine my process name
CUtility::splitpath((char *)argv[0],NULL,gszProcName);
#endif

int iFileLogLevel;			// level of file diagnostics
int iScreenLogLevel;		// level of file diagnostics
char szLogFile[_MAX_PATH];	// write diagnostics to this file
int Rslt = 0;   			// function result code >= 0 represents success, < 0 on failure

int PMode;					// processing mode
int MinSeqLen;				// sequences must be of at least this minimum length
int MinConcScore;					// K-mer distributions from subsequences with consensus score to be at least this threshold
char szKmerDistFile[_MAX_PATH];				// name of file into which write error corrected sequences
char szMultiAlignFile[_MAX_PATH];			// name of file containing multiple alignments to process
char szSQLiteDatabase[_MAX_PATH];	// results summaries to this SQLite file
char szExperimentName[cMaxDatasetSpeciesChrom+1];			// experiment name
char szExperimentDescr[1000];		// describes experiment

struct arg_lit  *help    = arg_lit0("h","help",                 "print this help and exit");
struct arg_lit  *version = arg_lit0("v","version,ver",			"print version information and exit");
struct arg_int *FileLogLevel=arg_int0("f", "FileLogLevel",		"<int>","Level of diagnostics written to screen and logfile 0=fatal,1=errors,2=info,3=diagnostics,4=debug");
struct arg_file *LogFile = arg_file0("F","log","<file>",		"diagnostics log file");

struct arg_int *pmode = arg_int0("m","pmode","<int>",			"processing mode - 0 default");
#ifdef SQLENCONSENSUSIMP
struct arg_int *minseqlen = arg_int0("l","minseqlen","<int>",	"sequences must be of at least this length (default is 1000, range 100..100000)");
struct arg_int *minconsscore = arg_int0("s","minconsscore","<int>",	"minimum consensus score (default 5, range 3..9)");
#endif
struct arg_file *maffile = arg_file1("i","maffile","<file>",		"file containing multialignments generated by 'pacbiokanga ecreads'");
struct arg_file *outfile = arg_file1("o","out","<file>",			"output K-mer distributions to this file");

struct arg_file *summrslts = arg_file0("q","sumrslts","<file>",				"Output results summary to this SQLite3 database file");
struct arg_str *experimentname = arg_str0("w","experimentname","<str>",		"experiment name SQLite3 database file");
struct arg_str *experimentdescr = arg_str0("W","experimentdescr","<str>",	"experiment description SQLite3 database file");

struct arg_end *end = arg_end(200);

void *argtable[] = {help,version,FileLogLevel,LogFile,
					pmode,
#ifdef SQLENCONSENSUSIMP
					minseqlen,minconsscore,
#endif
					summrslts,experimentname,experimentdescr,
					maffile,outfile,
					end};

char **pAllArgs;
int argerrors;
argerrors = CUtility::arg_parsefromfile(argc,(char **)argv,&pAllArgs);
if(argerrors >= 0)
	argerrors = arg_parse(argerrors,pAllArgs,argtable);

/* special case: '--help' takes precedence over error reporting */
if (help->count > 0)
        {
		printf("\n%s %s %s, Version %s\nOptions ---\n", gszProcName,gpszSubProcess->pszName,gpszSubProcess->pszFullDescr,cpszProgVer);
        arg_print_syntax(stdout,argtable,"\n");
        arg_print_glossary(stdout,argtable,"  %-25s %s\n");
		printf("\nNote: Parameters can be entered into a parameter file, one parameter per line.");
		printf("\n      To invoke this parameter file then precede it's name with '@'");
		printf("\n      e.g. %s %s @myparams.txt\n",gszProcName,gpszSubProcess->pszName);
		printf("\nPlease report any issues regarding usage of %s to stuart.stephen@csiro.au\n\n",gszProcName);
		return(1);
        }

    /* special case: '--version' takes precedence error reporting */
if (version->count > 0)
        {
		printf("\n%s %s Version %s\n",gszProcName,gpszSubProcess->pszName,cpszProgVer);
		return(1);
        }

if (!argerrors)
	{
	if(FileLogLevel->count && !LogFile->count)
		{
		printf("\nError: FileLogLevel '-f%d' specified but no logfile '-F<logfile>\n'",FileLogLevel->ival[0]);
		exit(1);
		}

	iScreenLogLevel = iFileLogLevel = FileLogLevel->count ? FileLogLevel->ival[0] : eDLInfo;
	if(iFileLogLevel < eDLNone || iFileLogLevel > eDLDebug)
		{
		printf("\nError: FileLogLevel '-l%d' specified outside of range %d..%d\n",iFileLogLevel,eDLNone,eDLDebug);
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

	// now that log parameters have been parsed then initialise diagnostics log system
	if(!gDiagnostics.Open(szLogFile,(etDiagLevel)iScreenLogLevel,(etDiagLevel)iFileLogLevel,true))
		{
		printf("\nError: Unable to start diagnostics subsystem\n");
		if(szLogFile[0] != '\0')
			printf(" Most likely cause is that logfile '%s' can't be opened/created\n",szLogFile);
		exit(1);
		}

	gDiagnostics.DiagOut(eDLInfo,gszProcName,"Subprocess %s Version %s starting",gpszSubProcess->pszName,cpszProgVer);
	gExperimentID = 0;
	gProcessID = 0;
	gProcessingID = 0;
	szSQLiteDatabase[0] = '\0';
	szExperimentName[0] = '\0';
	szExperimentDescr[0] = '\0';
	szKmerDistFile[0] = '\0';
	szMultiAlignFile[0] = '\0';
	MinSeqLen = 1000;
	MinConcScore = 5;

	if(experimentname->count)
		{
		strncpy(szExperimentName,experimentname->sval[0],sizeof(szExperimentName));
		szExperimentName[sizeof(szExperimentName)-1] = '\0';
		CUtility::TrimQuotedWhitespcExtd(szExperimentName);
		CUtility::ReduceWhitespace(szExperimentName);
		}
	else
		szExperimentName[0] = '\0';

	gExperimentID = 0;
	gProcessID = 0;
	gProcessingID = 0;
	szSQLiteDatabase[0] = '\0';
	szExperimentDescr[0] = '\0';

	if(summrslts->count)
		{
		strncpy(szSQLiteDatabase,summrslts->filename[0],sizeof(szSQLiteDatabase)-1);
		szSQLiteDatabase[sizeof(szSQLiteDatabase)-1] = '\0';
		CUtility::TrimQuotedWhitespcExtd(szSQLiteDatabase);
		if(strlen(szSQLiteDatabase) < 1)
			{
			gDiagnostics.DiagOut(eDLFatal,gszProcName,"Error: After removal of whitespace, no SQLite database specified with '-q<filespec>' option");
			return(1);
			}

		if(strlen(szExperimentName) < 1)
			{
			gDiagnostics.DiagOut(eDLFatal,gszProcName,"Error: After removal of whitespace, no SQLite experiment name specified with '-w<str>' option");
			return(1);
			}
		if(experimentdescr->count)
			{
			strncpy(szExperimentDescr,experimentdescr->sval[0],sizeof(szExperimentDescr)-1);
			szExperimentDescr[sizeof(szExperimentDescr)-1] = '\0';
			CUtility::TrimQuotedWhitespcExtd(szExperimentDescr);
			}
		if(strlen(szExperimentDescr) < 1)
			{
			gDiagnostics.DiagOut(eDLFatal,gszProcName,"Error: After removal of whitespace, no SQLite experiment description specified with '-W<str>' option");
			return(1);
			}

		gExperimentID = gSQLiteSummaries.StartExperiment(szSQLiteDatabase,false,true,szExperimentName,szExperimentName,szExperimentDescr);
		if(gExperimentID < 1)
			return(1);
		gProcessID = gSQLiteSummaries.AddProcess((char *)gpszSubProcess->pszName,(char *)gpszSubProcess->pszName,(char *)gpszSubProcess->pszFullDescr);
		if(gProcessID < 1)
			return(1);
		gProcessingID = gSQLiteSummaries.StartProcessing(gExperimentID,gProcessID,(char *)cpszProgVer);
		if(gProcessingID < 1)
			return(1);
		gDiagnostics.DiagOut(eDLInfo,gszProcName,"Initialised SQLite database '%s' for results summary collection",szSQLiteDatabase);
		gDiagnostics.DiagOut(eDLInfo,gszProcName,"SQLite database experiment identifier for '%s' is %d",szExperimentName,gExperimentID);
		gDiagnostics.DiagOut(eDLInfo,gszProcName,"SQLite database process identifier for '%s' is %d",(char *)gpszSubProcess->pszName,gProcessID);
		gDiagnostics.DiagOut(eDLInfo,gszProcName,"SQLite database processing instance identifier is %d",gProcessingID);
		}
	else
		{
		szSQLiteDatabase[0] = '\0';
		szExperimentDescr[0] = '\0';
		}

	PMode = pmode->count ? pmode->ival[0] : (int)0;
	if(PMode < 0 || PMode > 0)
		{
		gDiagnostics.DiagOut(eDLFatal,gszProcName,"Error: Processing mode '-m%d' must be in range 0..%d",PMode,0);
		return(1);
		}


#ifdef SQLENCONSENSUSIMP
	MinSeqLen = minseqlen->count ? minseqlen->ival[0] : 1000;
	if(MinSeqLen < 100 || MinSeqLen > 100000)
		{
		gDiagnostics.DiagOut(eDLFatal,gszProcName,"Error: Minimum sequence length '-l%d' must be in range %d..%d",MinSeqLen,100,100000);
		return(1);
		}

	MinConcScore = minconsscore->count ? minconsscore->ival[0] : 5;
	if(MinConcScore < 3 || MinConcScore > 9)
		{
		gDiagnostics.DiagOut(eDLFatal,gszProcName,"Error: Minimum consensus score '-s%d' must be in range 3..9",MinConcScore);
		return(1);
		}
#endif


	strcpy(szMultiAlignFile,maffile->filename[0]);
	szMultiAlignFile[_MAX_PATH-1] = '\0';
	CUtility::TrimQuotedWhitespcExtd(szMultiAlignFile);
	if(strlen(szMultiAlignFile) == 0)
		{
		gDiagnostics.DiagOut(eDLFatal,gszProcName,"Error: After removal of whitespace no input MAF file specified with '-i<maffile>'");
		return(1);
		}

	strcpy(szKmerDistFile,outfile->filename[0]);
	szKmerDistFile[_MAX_PATH-1] = '\0';
	CUtility::TrimQuotedWhitespcExtd(szKmerDistFile);
	if(strlen(szKmerDistFile) == 0)
		{
		gDiagnostics.DiagOut(eDLFatal,gszProcName,"Error: After removal of whitespace no output file specified with '-o<outfile>'");
		return(1);
		}

// show user current resource limits
#ifndef _WIN32
	gDiagnostics.DiagOut(eDLInfo, gszProcName, "Resources: %s",CUtility::ReportResourceLimits());
#endif

	gDiagnostics.DiagOut(eDLInfo,gszProcName,"Processing parameters:");

	char *pszMode;
	switch(PMode) {
		case 0:									// currently only processing mode
			pszMode = (char *)"K-mer distribution generation";
			break;
		}

	gDiagnostics.DiagOutMsgOnly(eDLInfo,"processing mode: '%s'",pszMode);
#ifdef SQLENCONSENSUSIMP
	gDiagnostics.DiagOutMsgOnly(eDLInfo,"sequences must be of at least this length: %dbp",MinSeqLen);
	gDiagnostics.DiagOutMsgOnly(eDLInfo,"minimum consensus score: %dbp",MinConcScore);
#endif
	gDiagnostics.DiagOutMsgOnly(eDLInfo,"processing K-mers in MAF file: '%s'",szMultiAlignFile);
	gDiagnostics.DiagOutMsgOnly(eDLInfo,"write K-mer distributions to file: '%s'",szKmerDistFile);

	if(szExperimentName[0] != '\0')
		gDiagnostics.DiagOutMsgOnly(eDLInfo,"This processing reference: %s",szExperimentName);

	if(gExperimentID > 0)
		{
		int ParamID;
		ParamID = gSQLiteSummaries.AddParameter(gProcessingID,ePTText,(int)strlen(szLogFile),"log",szLogFile);

		ParamID = gSQLiteSummaries.AddParameter(gProcessingID,ePTInt32,(int)sizeof(PMode),"pmode",&PMode);
#ifdef SQLENCONSENSUSIMP
		ParamID = gSQLiteSummaries.AddParameter(gProcessingID,ePTInt32,(int)sizeof(MinSeqLen),"minseqlen",&MinSeqLen);
		ParamID = gSQLiteSummaries.AddParameter(gProcessingID,ePTInt32,(int)sizeof(MinConcScore),"minconsscore",&MinConcScore);
#endif
		ParamID = gSQLiteSummaries.AddParameter(gProcessingID,ePTText,(int)strlen(szKmerDistFile),"out",szKmerDistFile);
		ParamID = gSQLiteSummaries.AddParameter(gProcessingID,ePTText,(int)strlen(szMultiAlignFile),"maffile",szMultiAlignFile);
		ParamID = gSQLiteSummaries.AddParameter(gProcessingID,ePTText,(int)strlen(szSQLiteDatabase),"sumrslts",szSQLiteDatabase);
		ParamID = gSQLiteSummaries.AddParameter(gProcessingID,ePTText,(int)strlen(szExperimentName),"experimentname",szExperimentName);
		ParamID = gSQLiteSummaries.AddParameter(gProcessingID,ePTText,(int)strlen(szExperimentDescr),"experimentdescr",szExperimentDescr);
		}


#ifdef _WIN32
	SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
#endif
	gStopWatch.Start();
	Rslt = GenKmerDistFromMAF( MinSeqLen, MinConcScore, szKmerDistFile, szMultiAlignFile);
	Rslt = Rslt >=0 ? 0 : 1;
	if(gExperimentID > 0)
		{
		if(gProcessingID)
			gSQLiteSummaries.EndProcessing(gProcessingID,Rslt);
		gSQLiteSummaries.EndExperiment(gExperimentID);
		}
	gStopWatch.Stop();

	gDiagnostics.DiagOut(eDLInfo,gszProcName,"Exit code: %d Total processing time: %s",Rslt,gStopWatch.Read());
	exit(Rslt);
	}
else
	{
    printf("\n%s %s %s, Version %s\n", gszProcName,gpszSubProcess->pszName,gpszSubProcess->pszFullDescr,cpszProgVer);
	arg_print_errors(stdout,end,gszProcName);
	arg_print_syntax(stdout,argtable,"\nUse '-h' to view option and parameter usage\n");
	exit(1);
	}
return 0;
}

int
GenKmerDistFromMAF(int MinSeqLen,		// sequences must be of at least this minimum length
			 int MinConcScore,					// K-mer distributions from subsequences with consensus score to be at least this threshold
			char *pszKmerDistFile,				// name of file into which write error corrected sequences
			char *pszMultiAlignFile)			// name of file containing multiple alignments to process
{
int Rslt;
CMAFKMerDist *pMAFKmerDist;
pMAFKmerDist = new CMAFKMerDist;
Rslt = pMAFKmerDist->GenKmerDistFromMAF(MinSeqLen,MinConcScore,pszKmerDistFile,pszMultiAlignFile);

delete pMAFKmerDist;
return(Rslt);
}

CMAFKMerDist::CMAFKMerDist()
{
m_pszMAFAlignBuff = NULL;		
m_pConsConfSeq = NULL;		
m_pszRptBuff = NULL;
m_hConsSeqFile = -1;
m_hMAFFile = -1;	
m_gzFile = NULL;
Reset();
}


CMAFKMerDist::~CMAFKMerDist()
{
Reset();
}

void
CMAFKMerDist::Reset(void)
{
if(m_hConsSeqFile != -1)
	{
	close(m_hConsSeqFile);
	m_hConsSeqFile = -1;
	}

if(m_hMAFFile >= 0)
	{
	close(m_hMAFFile);
	m_hMAFFile = -1;
	}
if(m_gzFile != NULL)
	{
	gzclose(m_gzFile);
	m_gzFile = NULL;
	}

if(m_pszMAFAlignBuff != NULL)
	{
	delete m_pszMAFAlignBuff;
	m_pszMAFAlignBuff = NULL;
	}
if(m_pConsConfSeq != NULL)
	{
	delete m_pConsConfSeq;
	m_pConsConfSeq = NULL;
	}
if(m_pszRptBuff != NULL)
	{
	delete m_pszRptBuff;
	m_pszRptBuff = NULL;
	}

m_NumParsedBlocks = 0;
m_OfsRptBuff = 0;
m_AllocRptBuffSize = 0;
m_bIsGZ = false;					
m_hMAFFile = -1;					
m_hConsSeqFile = -1;				
m_MAFFileOfs = 0;					
m_MAFAlignBuffIdx = 0;			
m_MAFAlignBuffered = 0;			
m_MAFFileLineNum = 0;			
m_AllocMAFAlignBuffSize = 0;	
}

int
CMAFKMerDist::GenKmerDistFromMAF(int MinSeqLen,		// sequences must be of at least this minimum length
			 int MinConcScore,							// K-mer distributions from sequences with consensus score to be at least this threshold
			char *pszKmerDistFile,						// name of file into which write error corrected sequences
			char *pszMultiAlignFile)					// name of file containing multiple alignments to process
{
int Rslt;
int Idx;
int BuffCnt;
int BuffTopUp;
bool bCpltdReadMAF;
Reset();

m_AllocMAFAlignBuffSize = (cMaxMAFKmerDistBlockLen * 4);
if(m_pszMAFAlignBuff == NULL)
	{
	if((m_pszMAFAlignBuff = new char [m_AllocMAFAlignBuffSize + 10])==NULL)
		{
		gDiagnostics.DiagOut(eDLFatal,gszProcName,"Unable to allocate memory for MAF block buffering");
		Reset();
		return(eBSFerrMem);
		}
	}
m_MAFAlignBuffIdx = 0;
m_MAFFileOfs = 0;
bCpltdReadMAF = false;

m_AllocRptBuffSize = cAllocRptBuffSize;
if(m_pszRptBuff == NULL)
	{
	if((m_pszRptBuff = new char [cAllocRptBuffSize])==NULL)
		{
		gDiagnostics.DiagOut(eDLFatal,gszProcName,"Unable to allocate memory for consensus sequence buffering");
		Reset();
		return(eBSFerrMem);
		}
	}

if(m_pConsConfSeq == NULL)
	{
	if((m_pConsConfSeq = new tsKmerConfBase [cMaxMAFBlockRowLen + 10])==NULL)
		{
		gDiagnostics.DiagOut(eDLFatal,gszProcName,"Unable to allocate memory for consensus confidence + bases buffering");
		Reset();
		return(eBSFerrMem);
		}
	}

m_MAFAlignBuffIdx = 0;
m_MAFFileOfs = 0;
m_MAFAlignBuffered = 0;
m_NumParsedBlocks = 0;
#ifdef _WIN32
m_hConsSeqFile = open(pszKmerDistFile,( O_WRONLY | _O_BINARY | _O_SEQUENTIAL | _O_CREAT | _O_TRUNC),(_S_IREAD | _S_IWRITE));
#else
if((m_hConsSeqFile = open(pszKmerDistFile,O_WRONLY | O_CREAT,S_IREAD | S_IWRITE))!=-1)
	if(ftruncate(m_hConsSeqFile,0)!=0)
		{
		gDiagnostics.DiagOut(eDLFatal,gszProcName,"Unable to truncate %s - %s",pszKmerDistFile,strerror(errno));
		Reset();
		return(eBSFerrCreateFile);
		}
#endif
if(m_hConsSeqFile < 0)
	{
	gDiagnostics.DiagOut(eDLFatal,gszProcName,"Process: unable to create/truncate output file '%s'",pszKmerDistFile);
	Reset();
	return(eBSFerrCreateFile);
	}

m_OfsRptBuff = sprintf(m_pszRptBuff,"\"ProbeID\",\"ProbeAlignLen\",\"ProbeOvlpLen\",\"TargOvlpLen\",\"OvlpNormFact5Kbp\"");
for(Idx = 1; Idx <= cMaxKMerLenCnts; Idx++)
	m_OfsRptBuff+=sprintf(&m_pszRptBuff[m_OfsRptBuff],",\"%d-mer Exact\"",Idx);
for(Idx = 1; Idx <= cMaxKMerLenCnts; Idx++)
	m_OfsRptBuff+=sprintf(&m_pszRptBuff[m_OfsRptBuff],",\"%d-mer RT Norm 5Kbp\"",Idx);
for(Idx = 1; Idx <= cMaxKMerLenCnts; Idx++)
	m_OfsRptBuff+=sprintf(&m_pszRptBuff[m_OfsRptBuff],",\"%d-mer InDel\"",Idx);
for(Idx = 1; Idx <= cMaxKMerLenCnts; Idx++)
	m_OfsRptBuff+=sprintf(&m_pszRptBuff[m_OfsRptBuff],",\"%d-mer InDel RT Norm 5Kbp\"",Idx);

CUtility::SafeWrite(m_hConsSeqFile, m_pszRptBuff,m_OfsRptBuff);
m_OfsRptBuff = 0;

// now open the pszMultiAlignFile
// if file has extension of ".gz' then assume that this file has been compressed and needs processing with gzopen/gzread/gzclose
int NameLen = (int)strlen(pszMultiAlignFile);
if(NameLen >= 4 && !stricmp(".gz",&pszMultiAlignFile[NameLen-3]))
	{
	if((m_gzFile = gzopen(pszMultiAlignFile,"r"))==NULL)
		{
		gDiagnostics.DiagOut(eDLFatal,gszProcName,"Unable to open %s as a gzip'd file - %s",pszMultiAlignFile,strerror(errno));
		Rslt = eBSFerrOpnFile;
		Reset();
		return(Rslt);
		}

	if(gzbuffer(m_gzFile,cgzAllocInBuffer)!=0)
		{
		gDiagnostics.DiagOut(eDLFatal,gszProcName,"Unable to set gzbuffer size to %d",cgzAllocInBuffer);
		Rslt = eBSFerrMem;
		Reset();
		return(Rslt);
		}

	if(!gzdirect(m_gzFile))			// false if file has been compressed
		m_bIsGZ = true;
	else
		m_bIsGZ = false;
	}
else
	{
	m_bIsGZ = false;
#ifdef _WIN32
	m_hMAFFile = open(pszMultiAlignFile, O_READSEQ );		// file access is normally sequential..
#else
	m_hMAFFile = open64(pszMultiAlignFile, O_READSEQ );		// file access is normally sequential..
#endif
	if(m_hMAFFile == -1)							// check if file open succeeded
		{
		gDiagnostics.DiagOut(eDLFatal,gszProcName,"Unable to open %s - %s",pszMultiAlignFile,strerror(errno));
		Rslt = eBSFerrOpnFile;
		Reset();
		return(eBSFerrOpnFile);
		}
	}

if(m_gzFile != NULL)
	{
	m_MAFFileOfs = gztell(m_gzFile);
	m_MAFAlignBuffered = gzread(m_gzFile, m_pszMAFAlignBuff, m_AllocMAFAlignBuffSize);
	}
else
	{
	m_MAFFileOfs = _lseeki64(m_hMAFFile,0,SEEK_CUR);
	m_MAFAlignBuffered = read(m_hMAFFile, m_pszMAFAlignBuff, m_AllocMAFAlignBuffSize);
	}

if(m_MAFAlignBuffered < 100)
	{
	gDiagnostics.DiagOut(eDLFatal,gszProcName,"Unable to open %s - %s",pszMultiAlignFile,strerror(errno));
	Rslt = eBSFerrOpnFile;
	Reset();
	return(eBSFerrOpnFile);
	}

do {
	if(!bCpltdReadMAF)
		{
		BuffTopUp = m_MAFAlignBuffered - m_MAFAlignBuffIdx;
		if(BuffTopUp < (int)(cMaxMAFKmerDistBlockLen * 2))
			{
			if(m_MAFAlignBuffIdx > 0 && BuffTopUp > 0)
				memmove(m_pszMAFAlignBuff,&m_pszMAFAlignBuff[m_MAFAlignBuffIdx],BuffTopUp);
			m_MAFAlignBuffered = BuffTopUp;
			m_MAFAlignBuffIdx = 0;
			if(m_gzFile != NULL)
				{
				m_MAFFileOfs = gztell(m_gzFile);
				BuffCnt = gzread(m_gzFile, &m_pszMAFAlignBuff[m_MAFAlignBuffered], m_AllocMAFAlignBuffSize - m_MAFAlignBuffered);
				}
			else
				{
				m_MAFFileOfs = _lseeki64(m_hMAFFile,0,SEEK_CUR);
				BuffCnt = read(m_hMAFFile, &m_pszMAFAlignBuff[m_MAFAlignBuffered], m_AllocMAFAlignBuffSize - m_MAFAlignBuffered);
				}
			if(BuffCnt <= 0)
				bCpltdReadMAF = true;
			else
				m_MAFAlignBuffered += BuffCnt;
			}
		}

	Rslt = ParseConsConfSeq(bCpltdReadMAF,MinSeqLen);		// parses complete multialignment blocks
	if(Rslt < 0 || (Rslt == eBSFSuccess && bCpltdReadMAF))
		break;
	}
while(Rslt >= 0);

if(m_OfsRptBuff)
	{
	CUtility::SafeWrite(m_hConsSeqFile, m_pszRptBuff,m_OfsRptBuff);
	m_OfsRptBuff = 0;
	}

if(m_hConsSeqFile != -1)
	{
#ifdef _WIN32
	_commit(m_hConsSeqFile);
#else
	fsync(m_hConsSeqFile);
#endif
	close(m_hConsSeqFile);
	m_hConsSeqFile = -1;
	}
if(m_hMAFFile >= 0)
	{
	close(m_hMAFFile);
	m_hMAFFile = -1;
	}
if(m_gzFile != NULL)
	{
	gzclose(m_gzFile);
	m_gzFile = NULL;
	}

if(m_NumParsedBlocks == 0)
	{
	gDiagnostics.DiagOut(eDLFatal,gszProcName,"Unable to parse any multiple alignment blocks from file '%s', is this a multiple alignment file?",pszMultiAlignFile);
	Rslt = eBSFerrAlignBlk;
	}

Reset();
return(Rslt);
}


// States
// 0 parsing starting from m_pszMAFAlignBuff[m_MAFAlignBuffIdx] until line starting with 'CB_<nnnnnn>' where <nnnnnn> is the probe sequence identifier
// 1 parsing remainder of 'CB_<nnnnnn>' line for consensus bases
// 2 parsing for line starting with 'CC_nnnnnn' where <nnnnnn> is the probe sequence identifier
// 3 parsing remainder of 'CC_nnnnnn' line for consensus confidence scores
// 4 parsing for line starting with 'CS_nnn' sequence starting from CS_001 as the probe sequence, subsequent sequences are target sequences
// 5 parsing remainder of sequence
// 6 iterating states 4 through 6 until all sequences parsed

// MAF assembly block processing is terminated on either errors or on the next occurrence of  'CB_nnnnnn'
//  
int    // parse out the next multialignment block consensus bases and consensus confidence scores into m_pConsConfSeq
CMAFKMerDist::ParseConsConfSeq(bool bCpltdReadMAF,// true if m_pConsConfSeq contains all remaining multialignment blocks loaded from file 
						 int MinErrCorrectLen)		// error corrected sequences must be at least this minimum length
{
int State;
int Cnt;
int Ofs;
int NumConBases;
int NumConConfs;
int ExpNumConConfs;
int SeqLen;
bool bSloughLine;
UINT32 ProbeID;
char Chr;
char *pBuff;
etSeqBase Base;
tsKmerConfBase *pConsConfSeq;

UINT32 SeqId;
UINT32 ExpSeqID;
INT32 NumProbeSeqBases;
INT32 NumTargSeqBases;

m_MAFFileLineNum = 1;
State = 0;
NumConBases = 0;
NumConConfs = 0;
SeqLen = 0;
SeqId = 0;
ExpSeqID = 0;
NumProbeSeqBases = 0;
NumTargSeqBases = 0;
bSloughLine = false;
pBuff = &m_pszMAFAlignBuff[m_MAFAlignBuffIdx];
pConsConfSeq = m_pConsConfSeq;
pConsConfSeq->ConsBase = eBaseEOS;
pConsConfSeq->ProbeBase = eBaseEOS;
pConsConfSeq->TargBase = eBaseEOS;
pConsConfSeq->Conf = 0;
time_t Now;
time_t Then;

Then = time(NULL);

for(; m_MAFAlignBuffIdx < m_MAFAlignBuffered; m_MAFAlignBuffIdx += 1,pBuff+=1)
	{
	if((m_MAFAlignBuffIdx % 100000) == 0)
		{
		Now = time(NULL);
		if(Now - Then >= 60)
			{
			gDiagnostics.DiagOut(eDLInfo,gszProcName,"Processed %d alignment blocks",m_NumParsedBlocks);
			Then += 60;
			if(m_OfsRptBuff > 0)	// once every 60 secs ensuring user can observe output file is growing!
				{
				CUtility::SafeWrite(m_hConsSeqFile, m_pszRptBuff,m_OfsRptBuff);
				m_OfsRptBuff = 0;
				}
			}
		}
	if(!bCpltdReadMAF && State == 0 && (m_MAFAlignBuffered - m_MAFAlignBuffIdx) < (cMaxMAFKmerDistBlockLen + 100))
		{
		m_pConsConfSeq->ConsBase = eBaseEOS;
		m_pConsConfSeq->ProbeBase = eBaseEOS;
		m_pConsConfSeq->TargBase = eBaseEOS;
		m_pConsConfSeq->Conf = 0;
		return(0);					// force load of more multialignments
		}

	Chr = toupper(*pBuff);
	if(Chr == ' ' || Chr == '\t') // slough any space or tabs
		continue;
	if(bSloughLine && Chr != '\n')	    // sloughing until start of next line?
		continue;
	if((State == 0 || State == 4) && Chr == '\n')
		{
		m_MAFFileLineNum += 1;
		bSloughLine = false;
		continue;
		}
	if(State == 0 && !(Chr == 'C' && pBuff[1] == 'B'))		// when expecting consensus lines to start then slough all lines not starting with 'CB', treating those sloughed as comment lines
		{
		bSloughLine = true;
		continue;
		}
	bSloughLine = false;

	switch(State) {
		case 0:						// expecting consensus line to start
			NumConBases = 0;
			NumConConfs = 0;
			SeqId = 0;
			ExpSeqID = 0;
			NumProbeSeqBases = 0;
			NumTargSeqBases = 0;	
			SeqLen = 0;
			pConsConfSeq = m_pConsConfSeq;
			pConsConfSeq->ConsBase = eBaseEOS;
			pConsConfSeq->ProbeBase = eBaseEOS;
			pConsConfSeq->TargBase = eBaseEOS;
			pConsConfSeq->Conf = 0;
			if((Cnt=sscanf(pBuff,"CB_%u%n",&ProbeID,&Ofs))==1)
				{
				if(ProbeID == 0)
					{
					gDiagnostics.DiagOut(eDLFatal,gszProcName,"LoadMAFBlock: Probe identifier is 0 at line %lld",m_MAFFileLineNum);
					return(eBSFerrAlignBlk);
					}
				State = 1;			// next expecting to parse out the consensus bases
				m_MAFAlignBuffIdx += Ofs;
				pBuff += Ofs;
				continue;
				}
			bSloughLine = true;		// not a consensus sequence line so treating as a comment line or perhaps sequence lines from the previous multialignment block
			continue;

		case 1:						// parsing out consensus bases
			switch(Chr) {
				case '\r':			// treating as whitespace
					continue;
				case '\n':			// expecting consensus confidence scores line as next line
					m_MAFFileLineNum += 1;
					State = 2;
					continue;
				case 'A': 
					Base = eBaseA;
					break;
				case 'C': 
					Base = eBaseC;
					break;
				case 'G': 
					Base = eBaseG;
					break;
				case 'T': case 'U':
					Base = eBaseT;
					break;
				case 'N': 
					Base = eBaseN;
					break;
				case '.':
					Base = eBaseUndef;
					break;
				case '-':
					Base = eBaseInDel;
					break;
				default:
					m_pConsConfSeq->ConsBase = eBaseEOS;
					m_pConsConfSeq->ProbeBase = eBaseEOS;
					m_pConsConfSeq->TargBase = eBaseEOS;
					m_pConsConfSeq->Conf = 0;
					gDiagnostics.DiagOut(eDLFatal,gszProcName,"LoadMAFBlock: Unexpected chars in consensus sequence at line %lld",m_MAFFileLineNum);
					return(eBSFerrAlignBlk);
				}
			if(NumConBases + 10 >= cMaxMAFBlockRowLen)
				{
				m_pConsConfSeq->ConsBase = eBaseEOS;
				m_pConsConfSeq->ProbeBase = eBaseEOS;
				m_pConsConfSeq->TargBase = eBaseEOS;
				m_pConsConfSeq->Conf = 0;
				gDiagnostics.DiagOut(eDLFatal,gszProcName,"LoadMAFBlock: Excessively long consensus sequence at line %lld",m_MAFFileLineNum);
				return(eBSFerrAlignBlk);
				}
			NumConBases += 1;
			if(Base != eBaseUndef)
				SeqLen += 1;
			pConsConfSeq->ConsBase = Base;
			pConsConfSeq->Conf = 0;
			pConsConfSeq += 1;
			pConsConfSeq->ConsBase = eBaseEOS;
			pConsConfSeq->ProbeBase = eBaseEOS;
			pConsConfSeq->TargBase = eBaseEOS;
			pConsConfSeq->Conf = 0;
			continue;

		case 2:						// expecting consensus confidence scores line to start
			if(NumConBases < MinErrCorrectLen)  // if previous consensus sequence less than the minimum required then no point in continuing with the current multialignment
				{
				bSloughLine = true;
				State = 0;
				continue;
				}
			// earlier releases did not include the number of confidence scores
			if(Chr == 'C' && pBuff[1] == 'C' && pBuff[2] == ' ')
				{
				ExpNumConConfs = NumConBases;
				Ofs = 2;
				}
			else
				{
				if((Cnt=sscanf(pBuff,"CC_%u%n",&ExpNumConConfs,&Ofs))==1)
					{
					if(ExpNumConConfs != NumConBases)
						{
						m_pConsConfSeq->ConsBase = eBaseEOS;
						m_pConsConfSeq->ProbeBase = eBaseEOS;
						m_pConsConfSeq->TargBase = eBaseEOS;
						m_pConsConfSeq->Conf = 0;
						gDiagnostics.DiagOut(eDLFatal,gszProcName,"LoadMAFBlock: Inconsistency in number of consensus confidence scores at line %lld",m_MAFFileLineNum);
						return(eBSFerrAlignBlk);
						}
					}
				else
					{
					m_pConsConfSeq->ConsBase = eBaseEOS;
					m_pConsConfSeq->ProbeBase = eBaseEOS;
					m_pConsConfSeq->TargBase = eBaseEOS;
					m_pConsConfSeq->Conf = 0;
					gDiagnostics.DiagOut(eDLFatal,gszProcName,"LoadMAFBlock: Expected consensus confidence scores at line %lld",m_MAFFileLineNum);
					return(eBSFerrAlignBlk);
					}
				}

			NumConConfs = 0;
			State = 3;
			m_MAFAlignBuffIdx += Ofs;
			pBuff += Ofs;
			pConsConfSeq = m_pConsConfSeq;
			continue;

		case 3:					// parsing out consensus confidence scores
			if(Chr == '\r')		// treating as whitespace
				continue;
			if(Chr >= '0' && Chr <='9')
				{
				if(NumConConfs >= ExpNumConConfs)
					{
					m_pConsConfSeq->ConsBase = eBaseEOS;
					m_pConsConfSeq->ProbeBase = eBaseEOS;
					m_pConsConfSeq->TargBase = eBaseEOS;
					m_pConsConfSeq->Conf = 0;
					gDiagnostics.DiagOut(eDLFatal,gszProcName,"LoadMAFBlock: Inconsistency in numbers of consensus confidence scores and consensus bases at line %lld",m_MAFFileLineNum);
					return(eBSFerrAlignBlk);
					}
				NumConConfs += 1;
				pConsConfSeq->Conf = (UINT8)(Chr - '0');	
				pConsConfSeq += 1;	
				continue;
				}
			if(Chr == '\n')
				{
				if(NumConConfs == ExpNumConConfs)	// must be same
					{
					m_pConsConfSeq[NumConConfs].ConsBase = eBaseEOS;
					m_pConsConfSeq[NumConConfs].ProbeBase = eBaseEOS;
					m_pConsConfSeq[NumConConfs].TargBase = eBaseEOS;
					m_pConsConfSeq[NumConConfs].Conf = 0;
					m_MAFFileLineNum += 1;
				    SeqId = 0;
					ExpSeqID = 1;
					NumProbeSeqBases = 0;
					NumTargSeqBases = 0;
					State = 4;
					continue;
					}
				m_pConsConfSeq->ConsBase = eBaseEOS;
				m_pConsConfSeq->ProbeBase = eBaseEOS;
				m_pConsConfSeq->TargBase = eBaseEOS;
				m_pConsConfSeq->Conf = 0;
				gDiagnostics.DiagOut(eDLFatal,gszProcName,"LoadMAFBlock: Inconsistency in numbers of consensus confidence scores and expected scores at line %lld",m_MAFFileLineNum);
				return(eBSFerrAlignBlk);
				}
			m_pConsConfSeq->ConsBase = eBaseEOS;
			m_pConsConfSeq->ProbeBase = eBaseEOS;
			m_pConsConfSeq->TargBase = eBaseEOS;
			m_pConsConfSeq->Conf = 0;
			gDiagnostics.DiagOut(eDLFatal,gszProcName,"LoadMAFBlock: Unexpected char '%c' in line %lld",Chr,m_MAFFileLineNum);
			return(eBSFerrAlignBlk);

		case 4:					// expecting start of sequence or possibly a new alignment block 'CB_nnnnnn" starting 
			char CChr;
			if((Cnt=sscanf(pBuff,"C%c_%u%n",&CChr,&SeqId,&Ofs))!=2)
				{
				gDiagnostics.DiagOut(eDLFatal,gszProcName,"LoadMAFBlock: Expected CS_nnn sequence at line %lld",m_MAFFileLineNum);
				return(eBSFerrAlignBlk);
				}
			if(CChr == 'B')		// it is a new alignment block starting
				{
				m_MAFAlignBuffIdx -= 1;
				pBuff-=1;
				State = 0;
				m_NumParsedBlocks += 1;
				continue;
				}
			
			if(ExpSeqID != SeqId || CChr != 'S')
				{
				gDiagnostics.DiagOut(eDLFatal,gszProcName,"LoadMAFBlock: Expected CS_%3d sequence at line %lld",ExpSeqID,m_MAFFileLineNum);
				return(eBSFerrAlignBlk);
				}	
			State = 5;
			m_MAFAlignBuffIdx += Ofs;
			pBuff += Ofs;
			if(SeqId == 1)
				NumProbeSeqBases = 0;
			NumTargSeqBases = 0;
			NumConConfs = 0;
			pConsConfSeq = m_pConsConfSeq;
			continue;

		case 5:						// parsing remainder of sequence
			switch(Chr) {
				case '\r':			// treating as whitespace
					continue;
				case '\n':			// could be start of next sequence line
					if(NumConConfs == ExpNumConConfs)
						{
						if(ExpSeqID >= 2)
							GenKMerDist(ProbeID,NumConConfs);
						ExpSeqID += 1;
						m_MAFFileLineNum += 1;
						State = 4;
						continue;
						}
					m_pConsConfSeq->ConsBase = eBaseEOS;
					m_pConsConfSeq->ProbeBase = eBaseEOS;
					m_pConsConfSeq->TargBase = eBaseEOS;
					m_pConsConfSeq->Conf = 0;
					gDiagnostics.DiagOut(eDLFatal,gszProcName,"LoadMAFBlock: Truncated line %lld",m_MAFFileLineNum);
					return(eBSFerrAlignBlk);

				case 'A': 
					Base = eBaseA;
					break;
				case 'C': 
					Base = eBaseC;
					break;
				case 'G': 
					Base = eBaseG;
					break;
				case 'T': case 'U':
					Base = eBaseT;
					break;
				case 'N': 
					Base = eBaseN;
					break;
				case '.':
					Base = eBaseUndef;
					break;
				case '-':
					Base = eBaseInDel;
					break;
				default:
					m_pConsConfSeq->ConsBase = eBaseEOS;
					m_pConsConfSeq->ProbeBase = eBaseEOS;
					m_pConsConfSeq->TargBase = eBaseEOS;
					m_pConsConfSeq->Conf = 0;
					gDiagnostics.DiagOut(eDLFatal,gszProcName,"LoadMAFBlock: Unexpected chars in sequence at line %lld",m_MAFFileLineNum);
					return(eBSFerrAlignBlk);
				}

			if(NumConConfs >= ExpNumConConfs)
				{
				m_pConsConfSeq->ConsBase = eBaseEOS;
				m_pConsConfSeq->ProbeBase = eBaseEOS;
				m_pConsConfSeq->TargBase = eBaseEOS;
				m_pConsConfSeq->Conf = 0;
				gDiagnostics.DiagOut(eDLFatal,gszProcName,"LoadMAFBlock: Excessively long sequence at line %lld",m_MAFFileLineNum);
				return(eBSFerrAlignBlk);
				}

			if(ExpSeqID == 1)
				{
				pConsConfSeq->ProbeBase = Base;
				pConsConfSeq->TargBase = eBaseEOS;
   				pConsConfSeq += 1;
				pConsConfSeq->ProbeBase = eBaseEOS;
				NumProbeSeqBases += 1;
				}
			else
				{
				pConsConfSeq->TargBase = Base;
				pConsConfSeq += 1;
				NumTargSeqBases += 1;
				}		
			pConsConfSeq->TargBase = eBaseEOS;
		    NumConConfs += 1;
			continue;
		}
	}

if(State == 0)
	{
	gDiagnostics.DiagOut(eDLInfo,gszProcName,"Completed processing of %d alignment blocks",m_NumParsedBlocks);
	return(eBSFSuccess);
	}

if(State == 4 && ExpSeqID >= 2)
	{
	m_NumParsedBlocks += 1;
	gDiagnostics.DiagOut(eDLInfo,gszProcName,"Completed processing of %d alignment blocks",m_NumParsedBlocks);
	return(eBSFSuccess);
	}

if(State == 5 && ExpSeqID >= 1 && NumConConfs == ExpNumConConfs)
	{
	if(ExpSeqID >= 2)
		GenKMerDist(ProbeID,NumConConfs);

	m_NumParsedBlocks += 1;
	gDiagnostics.DiagOut(eDLInfo,gszProcName,"Completed processing of %d alignment blocks",m_NumParsedBlocks);
	return(eBSFSuccess);
	}

gDiagnostics.DiagOut(eDLInfo,gszProcName,"Completed processing of %d alignment blocks",m_NumParsedBlocks);
gDiagnostics.DiagOut(eDLFatal,gszProcName,"LoadMAFBlock: File truncated at line %lld",m_MAFFileLineNum);
return(eBSFerrAlignBlk);
}

int
CMAFKMerDist::GenKMerDist(int ProbeID,			// alignment was using this probe identifier
						  int NumAlignCols)		// number of alignment columns
{
int ColIdx;
int ProbeOvlpLen;
int TargOvlpLen;
tsKmerConfBase *pConsConfSeq;
int KMerLen;
int KMerInDelLen;

int KMerLenCnts[cMaxKMerLenCnts];

int KMerInDelCnts[cMaxKMerLenCnts];

memset(KMerLenCnts,0,sizeof(KMerLenCnts));
memset(KMerInDelCnts,0,sizeof(KMerInDelCnts));

KMerLen = 0;
KMerInDelLen = 0;
ProbeOvlpLen = 0;
TargOvlpLen = 0;
pConsConfSeq = m_pConsConfSeq;
for(ColIdx = 0; ColIdx < NumAlignCols; ColIdx++,pConsConfSeq++)
	{
	if(pConsConfSeq->TargBase == eBaseUndef)
		{
		if(TargOvlpLen > 0)
			break;
		continue;
		}
	if(pConsConfSeq->ProbeBase != eBaseInDel)
		ProbeOvlpLen += 1;
	if(pConsConfSeq->TargBase != eBaseInDel)
		TargOvlpLen += 1;
	
	if(pConsConfSeq->ProbeBase != eBaseN && pConsConfSeq->ProbeBase == pConsConfSeq->TargBase)
		{
		if(KMerInDelLen != 0)
			{
			if(KMerInDelLen > cMaxKMerLenCnts)
				KMerInDelLen = cMaxKMerLenCnts;
			KMerInDelCnts[KMerInDelLen-1] += 1;
			KMerInDelLen = 0;
			}
		if(pConsConfSeq->ProbeBase == eBaseInDel)
			continue;
		KMerLen += 1;
		continue;
		}

	if(pConsConfSeq->ProbeBase == eBaseInDel || pConsConfSeq->TargBase == eBaseInDel)
		KMerInDelLen += 1;
	else
		if(KMerInDelLen != 0)
			{
			if(KMerInDelLen > cMaxKMerLenCnts)
				KMerInDelLen = cMaxKMerLenCnts;
			KMerInDelCnts[KMerInDelLen-1] += 1;
			KMerInDelLen = 0;
			}

	if(KMerLen != 0)
		{
		if(KMerLen > cMaxKMerLenCnts)
			KMerLen = cMaxKMerLenCnts;
		KMerLenCnts[KMerLen-1] += 1;
		KMerLen = 0;
		}
	}
if(KMerLen != 0)
	{
	if(KMerLen > cMaxKMerLenCnts)
		KMerLen = cMaxKMerLenCnts;
	KMerLenCnts[KMerLen-1] += 1;
	}

if(KMerInDelLen != 0)
	{
	if(KMerInDelLen > cMaxKMerLenCnts)
		KMerInDelLen = cMaxKMerLenCnts;
	KMerInDelCnts[KMerInDelLen-1] += 1;
	KMerInDelLen = 0;
	}
 
int Idx;
int SumCnts;
int SumInDels;
double NormFact5Kbp;
NormFact5Kbp = 5000.0 / (double)((1 + ProbeOvlpLen + TargOvlpLen)/2);
SumCnts = 0;
SumInDels = 0;
m_OfsRptBuff += sprintf(&m_pszRptBuff[m_OfsRptBuff],"\n%d,%d,%d,%d,%1.4f",ProbeID,NumAlignCols,ProbeOvlpLen,TargOvlpLen,NormFact5Kbp);
for(Idx = 0; Idx < cMaxKMerLenCnts; Idx++)
	{
	SumCnts += KMerLenCnts[Idx];
	m_OfsRptBuff+=sprintf(&m_pszRptBuff[m_OfsRptBuff],",%d",KMerLenCnts[Idx]);
	}

for(Idx = 0; Idx < cMaxKMerLenCnts; Idx++)
	{
	m_OfsRptBuff+=sprintf(&m_pszRptBuff[m_OfsRptBuff],",%1.3f",(double)SumCnts * NormFact5Kbp);
	SumCnts -= KMerLenCnts[Idx];
	}


for(Idx = 0; Idx < cMaxKMerLenCnts; Idx++)
	{
	SumInDels += KMerInDelCnts[Idx];
	m_OfsRptBuff+=sprintf(&m_pszRptBuff[m_OfsRptBuff],",%d",KMerInDelCnts[Idx]);
	}

for(Idx = 0; Idx < cMaxKMerLenCnts; Idx++)
	{
	m_OfsRptBuff+=sprintf(&m_pszRptBuff[m_OfsRptBuff],",%1.3f",(double)SumInDels * NormFact5Kbp);
	SumInDels -= KMerInDelCnts[Idx];
	}


if(m_OfsRptBuff + 5000 >= m_AllocRptBuffSize)
	{
	CUtility::SafeWrite(m_hConsSeqFile, m_pszRptBuff,m_OfsRptBuff);
	m_OfsRptBuff = 0;
	}

return(0);
}

