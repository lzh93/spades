///////////////////////////////////////////////////////////////////////////////
//                   SOFTWARE COPYRIGHT NOTICE AGREEMENT                     //
//       This software and its documentation are copyright (2011) by the     //
//   Broad Institute.  All rights are reserved.  This software is supplied   //
//   without any warranty or guaranteed support whatsoever. The Broad        //
//   Institute is not responsible for its use, misuse, or functionality.     //
///////////////////////////////////////////////////////////////////////////////

// FixPrecompute.  Precompute some ugly stuff needed by both FixSomeIndels and
// FixAssemblyBaseErrors.

// MakeDepend: library OMP
// MakeDepend: cflags OMP_FLAGS

#include <omp.h>

#include "Basevector.h"
#include "FetchReads.h"
#include "MainTools.h"
#include "ParallelVecUtilities.h"
#include "VecTemplate.h"
#include "math/Functions.h"
#include "paths/FixSomeIndelsUtils.h"
#include "paths/UnipathFixerTools.h"
#include "util/SearchFastb2Core.h"

int main(int argc, char *argv[])
{
     RunTime( );

     BeginCommandArguments;
     CommandArgument_String(PRE);
     CommandArgument_String(DATA);
     CommandArgument_String(RUN);
     CommandArgument_String_OrDefault(SUBDIR, "test");
     CommandArgument_Int(K);
     CommandArgument_UnsignedInt_OrDefault_Doc(NUM_THREADS, 0, 
	   "Number of threads to use (use all available processors if set to 0)");
     CommandArgument_String_OrDefault(HEAD, "extended40.shaved");
     CommandArgument_String_OrDefault(SCAFFOLDS_IN, "linear_scaffolds0.patched");
     CommandArgument_String_OrDefault(UNIPATH_PATCH_DIR, "unipath_patch");
     CommandArgument_String_OrDefault(POST_PATCH_DIR, "post_patch");
     CommandArgument_String_OrDefault(JUMP_READS, "jump_reads_filt_cpd");
     EndCommandArguments;

     // Begin.

     double clock = WallClockTime();

     // Thread control (using OMP in SearchFastb)

     NUM_THREADS = configNumThreads(NUM_THREADS);
     omp_set_num_threads( NUM_THREADS );


     // Define directories, etc.

     String data_dir = PRE + "/" + DATA, run_dir = PRE + "/" + DATA + "/" + RUN;
     String sub_dir = run_dir + "/ASSEMBLIES/" + SUBDIR;
     cout << Date( ) << ": " << run_dir << endl;
     String ch_head 
          = sub_dir + "/" + POST_PATCH_DIR + "/PostPatcher." + SCAFFOLDS_IN + ".";
     Mkpath( sub_dir + "/" + POST_PATCH_DIR );

     // Note some files that are needed.

     String unibases_file = run_dir + "/" + HEAD + ".unibases.k" + ToString(K);
     String TIGS_file = ch_head + "TIGS";
     String tigsa_file = sub_dir + "/" + SCAFFOLDS_IN + ".contigs.fasta";
     String jreads_file = run_dir + "/" + JUMP_READS + ".fastb";
     String scaffolds_tig_file = sub_dir + "/" + SCAFFOLDS_IN + ".contigs.vecfasta";

     // Do stuff.
   
     const size_t n_tigs = MastervecFileObjectCount(scaffolds_tig_file);
     {
          // Compute read lengths.

          vec<uint16_t> jread_len;
          cout << Date( ) << ": computing jump read lengths" << endl;
          String JREADLEN_file = sub_dir + "/" + POST_PATCH_DIR 
               + "/PostPatcher.stable.JREADLEN";
          BaseVecVec jreads(jreads_file);
          jread_len.resize(jreads.size());
          for (size_t i = 0; i < jreads.size(); i++) 
          {    ForceAssertLt(jreads[i].size(), 65536u);
               jread_len[i] = jreads[i].size();    }
          BinaryWrite3(JREADLEN_file, jread_len);

          // Prepare contigs file for SearchFastb2

          cout << Date( ) << ": loading contigs" << endl;
          {    BaseVecVec tigs;
               {    // Convert fasta file into fastb file.
                    FetchReads(tigs, 0, tigsa_file);
                    tigs.WriteAll(TIGS_file);    }    }

          // Map the unibases to the contigs.  There is a potential problem with 
          // the way we're doing this: we require that each unibase maps perfectly.

          cout << Date( ) << ": mapping unibases to contigs" << endl;
          String JRALIGNS_file = ch_head + "JRALIGNS";
          String UALIGNS_file = ch_head + "UALIGNS";
          vec< triple<int64_t,int64_t,int> > UALIGNS;
          { 
	    const int max_placements = 1;
	    SearchFastb2( unibases_file, TIGS_file, K, &UALIGNS, 0, max_placements );
	    BinaryWrite3(UALIGNS_file, UALIGNS);
	  }

          // Load segments generated by UnipathPatcher.

          cout << Date( ) << ": loading segments" << endl;
          vec<segalign> JSEGS;
          String JSEGS_file 
               = run_dir + "/" + UNIPATH_PATCH_DIR + "/UnipathPatcher.JSEGS";
          BinaryRead3(JSEGS_file, JSEGS);

          // Load the unibases.

          BaseVecVec unibases;
          unibases.ReadAll(unibases_file);

          // Index the unibase alignments.

          cout << Date( ) << ": indexing unibase alignments" << endl;
          vec<size_t> U_START;
          {    U_START.resize(unibases.size() + 1);
               size_t n_unibases = unibases.size();
               size_t POS = 0;
               for (size_t u = 0; u <= n_unibases; u++) 
               {    while(POS < UALIGNS.size() && UALIGNS[POS].first < (signed)u) 
                         ++POS;
                    U_START[u] = POS;    }    }
  
          // Map the reads to the contigs.  Note that we assume reads are fw on the
          // unibases.

          cout << Date( ) << ": start process of mapping reads to contigs" << endl;
          vec< triple<int64_t,int,int> > JRALIGNS; // (rid, tig, pos)
          {    cout << Date( ) << ": mapping jump reads" << endl;
               for (size_t i = 0; i < JSEGS.size(); i++) 
               {    const segalign& a = JSEGS[i];
                    int64_t rid = a.rid; 
                    int u = a.u, rpos = a.rpos, upos = a.upos;
                    if (U_START[u] < U_START[u+1]) 
                    {    int tig = UALIGNS[U_START[u]].second;
                         int tpos = UALIGNS[U_START[u]].third;
                         int read_start_on_tig;
                         if (tpos >= 0) read_start_on_tig = tpos + upos - rpos;
                         else 
                         {    read_start_on_tig 
                                   = -tpos-1 + unibases[u].isize() - upos
                                   - jread_len[rid] + rpos;    }
                         read_start_on_tig 
                              = Max(0, read_start_on_tig); // don't like!
                         if (tpos >= 0) JRALIGNS.push(rid, tig, read_start_on_tig);
                         else JRALIGNS.push(rid, tig, -read_start_on_tig-1);   }   }
               ParallelUniqueSort(JRALIGNS);
               BinaryWrite3(JRALIGNS_file, JRALIGNS);    }    }

      // Create position-sorted versions of read alignment files and indices to them.
  
      vec<size_t> JRPS_START;
      {    cout << Date( ) << ": getting JRPS_START" << endl;
           {    vec< triple<int64_t,int,int> > JRALIGNS_PSORT;
                BinaryRead3(ch_head + "JRALIGNS", JRALIGNS_PSORT);
                ParallelSort(JRALIGNS_PSORT, cmp_pos);
                BinaryWrite3(ch_head + "JRALIGNS_PSORT", JRALIGNS_PSORT);
                JRPS_START.resize(n_tigs + 1);
                size_t POS = 0;
                for (size_t u = 0; u <= n_tigs; u++) 
                {    while(POS < JRALIGNS_PSORT.size() 
                         && JRALIGNS_PSORT[POS].second < (int64_t)u) 
                     {    ++POS;    }
                     JRPS_START[u] = POS;    }
                BinaryWrite3(ch_head + "JRPS_START", JRPS_START);    }    }
  
     // Done.

     cout << "time used = " << TimeSince(clock) << endl;    }
