#! /bin/bash
g++  -g -O2 --static  -o pacbiokangas AssembGraph.o  BKSProvider.o  BKSRequester.o  MAConsensus.o  pacbiokanga.o  PacBioUtility.o  PBAssemb.o  PBECContigs.o  PBErrCorrect.o  PBFilter.o  PBSim.o  PBSWService.o  SeqStore.o  SQLiteSummaries.o  SSW.o  SWAlign.o  ../libbiokanga/libbiokanga.a ../libbiokanga/zlib/libz.a ../libBKPLPlot/libBKPLPlot.a -lrt -ldl -lpthread -lgsl -lgslcblas 
