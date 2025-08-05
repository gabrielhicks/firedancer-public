#include "fd_ssarchive.h"

#if !FD_HAS_HOSTED
#error "This target requires FD_HAS_HOSTED"
#endif

#include <stdlib.h>
#include <assert.h>

int
LLVMFuzzerInitialize( int  *   argc,
                      char *** argv ) {
  /* Set up shell without signal handlers */
  putenv( "FD_LOG_BACKTRACE=0" );
  fd_boot( argc, argv );
  atexit( fd_halt );
  fd_log_level_core_set   ( 4 );
  fd_log_level_logfile_set( 4 );

  return 0;
}

int
LLVMFuzzerTestOneInput( uchar const * data,
                        ulong         size ) {
  (void)size;
  ulong full_slot;
  ulong incremental_slot;
  uchar decoded_hash[ FD_HASH_FOOTPRINT ];
  fd_ssarchive_parse_filename( (char *)data, &full_slot, &incremental_slot, decoded_hash );
  return 0;
}
