#include "utils/fd_ssping.h"
#include "utils/fd_sshttp.h"
#include "utils/fd_ssctrl.h"
#include "utils/fd_ssarchive.h"

#include "../../disco/topo/fd_topo.h"
#include "../../disco/metrics/fd_metrics.h"
#include "../../disco/plugin/fd_plugin.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define NAME "snaprd"

/* The snaprd tile at a high level is a state machine that downloads
   snapshots from the network or reads snapshots from disk and produces
   a byte stream that is parsed by downstream snapshot consumer tiles.
   The snaprd tile gathers the latest SnapshotHashes information from
   gossip to decide whether to download snapshots or read local
   snapshots from disk.  If the snaprd tile needs to download a snapshot,
   it goes through the process of discovering and selecting elegible
   peers from gossip to download from. */

#define FD_SNAPRD_STATE_WAITING_FOR_PEERS            ( 0) /* Waiting for first peer to arrive from gossip to download from */
#define FD_SNAPRD_STATE_COLLECTING_PEERS             ( 1) /* First peer arrived, wait a little longer to see if a better one arrives */
#define FD_SNAPRD_STATE_READING_FULL_FILE            ( 2) /* Full file looks better than peer, reading it from disk */
#define FD_SNAPRD_STATE_FLUSHING_FULL_FILE           ( 3) /* Full file was read ok, confirm it decompressed and inserted ok */
#define FD_SNAPRD_STATE_FLUSHING_FULL_FILE_RESET     ( 4) /* Resetting to load full snapshot from file again, confirm decompress and inserter are reset too */
#define FD_SNAPRD_STATE_READING_INCREMENTAL_FILE     ( 5) /* Incremental file looks better than peer, reading it from disk */
#define FD_SNAPRD_STATE_FLUSHING_INCREMENTAL_FILE    ( 6) /* Incremental file was read ok, confirm it decompressed and inserted ok */
#define FD_SNAPRD_STATE_READING_FULL_URL_HTTP        ( 7) /* Peer was selected, getting full snapshot URL from HTTP redirect headers */
#define FD_SNAPRD_STATE_READING_FULL_HTTP            ( 8) /* Reading full snapshot from HTTP */
#define FD_SNAPRD_STATE_FLUSHING_FULL_HTTP           ( 9) /* Full snapshot was downloaded ok, confirm it decompressed and inserted ok */
#define FD_SNAPRD_STATE_FLUSHING_FULL_HTTP_RESET     (10) /* Resetting to load full snapshot from HTTP again, confirm decompress and inserter are reset too */
#define FD_SNAPRD_STATE_READING_INCREMENTAL_URL_HTTP (11) /* Peer was selected, getting incremental snapshot URL from HTTP redirect headers */
#define FD_SNAPRD_STATE_READING_INCREMENTAL_HTTP     (12) /* Reading incremental snapshot from HTTP */
#define FD_SNAPRD_STATE_FLUSHING_INCREMENTAL_HTTP    (13) /* Incremental snapshot was downloaded ok, confirm it decompressed and inserted ok */
#define FD_SNAPRD_STATE_SHUTDOWN                     (14) /* The tile is done, and has likely already exited */

#define FD_SNAPRD_PLUGIN_OUT_IDX (1)
struct fd_snaprd_tile {
  fd_ssping_t * ssping;
  fd_sshttp_t * sshttp;

  int   state;
  int   malformed;
  long  deadline_nanos;
  ulong ack_cnt;
  int   peer_selection;

  /* ipvr address of the peer we are connected to */
  fd_ip4_port_t addr;

  struct {
    ulong full_snapshot_slot;
    int   full_snapshot_fd;
    char  full_snapshot_path[ PATH_MAX ];
    ulong incremental_snapshot_slot;
    int   incremental_snapshot_fd;
    char  incremental_snapshot_path[ PATH_MAX ];
  } local;

  struct {
    ulong full_snapshot_slot;
    char  full_snapshot_path[ PATH_MAX ];
    ulong incremental_snapshot_slot;
    char  incremental_snapshot_path[ PATH_MAX ];
  } remote;

  struct {
    int  do_download;
    int  incremental_snapshot_fetch;
    uint maximum_local_snapshot_age;
    uint minimum_download_speed_mib;
    uint maximum_download_retry_abort;
  } config;

  struct {
    struct {
      ulong bytes_read;
      ulong bytes_total;
      uint  num_retries;
    } full;

    struct {
      ulong bytes_read;
      ulong bytes_total;
      uint  num_retries;
    } incremental;
  } metrics;

  struct {
    fd_wksp_t * wksp;
    ulong       chunk0;
    ulong       wmark;
    ulong       chunk;
    ulong       mtu;
  } out;

  struct {
    fd_wksp_t * wksp;
    ulong       chunk0;
    ulong       wmark;
    ulong       chunk;
    ulong       mtu;
  } plugin_out;
};

typedef struct fd_snaprd_tile fd_snaprd_tile_t;

static ulong
scratch_align( void ) {
  return alignof(fd_snaprd_tile_t);
}

static ulong
scratch_footprint( fd_topo_tile_t const * tile ) {
  (void)tile;
  ulong l = FD_LAYOUT_INIT;
  l = FD_LAYOUT_APPEND( l, alignof(fd_snaprd_tile_t), sizeof(fd_snaprd_tile_t)       );
  l = FD_LAYOUT_APPEND( l, fd_sshttp_align(),         fd_sshttp_footprint()          );
  l = FD_LAYOUT_APPEND( l, fd_ssping_align(),         fd_ssping_footprint( 65536UL ) );
  return FD_LAYOUT_FINI( l, alignof(fd_snaprd_tile_t) );
}

static inline int
should_shutdown( fd_snaprd_tile_t * ctx ) {
  return ctx->state==FD_SNAPRD_STATE_SHUTDOWN;
}

static void
metrics_write( fd_snaprd_tile_t * ctx ) {
  FD_MGAUGE_SET( SNAPRD, FULL_BYTES_READ,               ctx->metrics.full.bytes_read );
  FD_MGAUGE_SET( SNAPRD, FULL_BYTES_TOTAL,              ctx->metrics.full.bytes_total );
  FD_MGAUGE_SET( SNAPRD, INCREMENTAL_BYTES_READ,        ctx->metrics.incremental.bytes_read );
  FD_MGAUGE_SET( SNAPRD, INCREMENTAL_BYTES_TOTAL,       ctx->metrics.incremental.bytes_total );
  FD_MGAUGE_SET( SNAPRD, FULL_DOWNLOAD_RETRIES,         ctx->metrics.full.num_retries );
  FD_MGAUGE_SET( SNAPRD, INCREMENTAL_DOWNLOAD_RETRIES,  ctx->metrics.incremental.num_retries );

  FD_MGAUGE_SET( SNAPRD, STATE, (ulong)ctx->state );
}

static void
fd_snaprd_publish_snapshot_update( fd_snaprd_tile_t *  ctx,
                                        fd_stem_context_t * stem,
                                        int                 type,
                                        int                 remote,
                                        ulong               slot,
                                        const char *        path ) {
  fd_restore_snapshot_update_t * snapshot_update = fd_chunk_to_laddr( ctx->plugin_out.wksp , ctx->plugin_out.chunk );
  snapshot_update->type = type;
  snapshot_update->slot = slot;
  snprintf( snapshot_update->download_path, PATH_MAX, "%s", path );
  snapshot_update->peer_ip_addr = fd_uint_if( remote, ctx->addr.addr, 0U );
  snapshot_update->peer_port = fd_ushort_if( remote, ctx->addr.port, 0U );
  
  ulong tspub  = (ulong)fd_frag_meta_ts_comp( fd_tickcount() );
  fd_stem_publish( stem, FD_SNAPRD_PLUGIN_OUT_IDX, FD_PLUGIN_MSG_SNAPSHOT_UPDATE, ctx->plugin_out.chunk, sizeof(fd_restore_snapshot_update_t), 0UL, tspub, tspub );
  ctx->plugin_out.chunk = fd_dcache_compact_next( ctx->plugin_out.chunk, sizeof(fd_restore_snapshot_update_t), ctx->plugin_out.chunk0, ctx->plugin_out.wmark );
}

static void
read_file_data( fd_snaprd_tile_t *  ctx,
                fd_stem_context_t * stem ) {
  uchar * out = fd_chunk_to_laddr( ctx->out.wksp, ctx->out.chunk );

  FD_TEST( ctx->state==FD_SNAPRD_STATE_READING_INCREMENTAL_FILE || ctx->state==FD_SNAPRD_STATE_READING_FULL_FILE );


  switch( ctx->state ) {
    case FD_SNAPRD_STATE_READING_INCREMENTAL_FILE:
      fd_snaprd_publish_snapshot_update( ctx, stem, FD_PLUGIN_MSG_SNAPSHOT_TYPE_INCREMENTAL, 0, ctx->local.incremental_snapshot_slot, ctx->local.incremental_snapshot_path );
      break;
    case FD_SNAPRD_STATE_READING_FULL_FILE:
      fd_snaprd_publish_snapshot_update( ctx, stem, FD_PLUGIN_MSG_SNAPSHOT_TYPE_FULL, 0, ctx->local.full_snapshot_slot, ctx->local.full_snapshot_path );
      break;
    default:
      break;
  }

  int full = ctx->state==FD_SNAPRD_STATE_READING_FULL_FILE;
  long result = read( full ? ctx->local.full_snapshot_fd : ctx->local.incremental_snapshot_fd , out, ctx->out.mtu );
  if( FD_UNLIKELY( -1==result && errno==EAGAIN ) ) return;
  else if( FD_UNLIKELY( -1==result ) ) FD_LOG_ERR(( "read() failed (%i-%s)", errno, fd_io_strerror( errno ) ));

  switch( ctx->state ) {
    case FD_SNAPRD_STATE_READING_INCREMENTAL_FILE:
      ctx->metrics.incremental.bytes_read += (ulong)result;
      break;
    case FD_SNAPRD_STATE_READING_FULL_FILE:
      ctx->metrics.full.bytes_read += (ulong)result;
      break;
    default:
      break;
  }

  if( FD_UNLIKELY( !result ) ) {
    switch( ctx->state ) {
      case FD_SNAPRD_STATE_READING_INCREMENTAL_FILE:
        fd_stem_publish( stem, 0UL, FD_SNAPSHOT_MSG_CTRL_DONE, 0UL, 0UL, 0UL, 0UL, 0UL );
        ctx->state = FD_SNAPRD_STATE_FLUSHING_INCREMENTAL_FILE;
        fd_snaprd_publish_snapshot_update( ctx, stem, FD_PLUGIN_MSG_SNAPSHOT_TYPE_INCREMENTAL, 0, ctx->local.incremental_snapshot_slot, ctx->local.incremental_snapshot_path );
        break;
      case FD_SNAPRD_STATE_READING_FULL_FILE:
        if( FD_LIKELY( ctx->config.incremental_snapshot_fetch ) ) {
          fd_stem_publish( stem, 0UL, FD_SNAPSHOT_MSG_CTRL_EOF_FULL, 0UL, 0UL, 0UL, 0UL, 0UL );
        } else {
          fd_stem_publish( stem, 0UL, FD_SNAPSHOT_MSG_CTRL_DONE, 0UL, 0UL, 0UL, 0UL, 0UL );
        }
        ctx->state = FD_SNAPRD_STATE_FLUSHING_FULL_FILE;
        fd_snaprd_publish_snapshot_update( ctx, stem, FD_PLUGIN_MSG_SNAPSHOT_TYPE_FULL, 0, ctx->local.full_snapshot_slot, ctx->local.full_snapshot_path );
        break;
      default:
        break;
    }
    return;
  }

  fd_stem_publish( stem, 0UL, FD_SNAPSHOT_MSG_DATA, ctx->out.chunk, (ulong)result, 0UL, 0UL, 0UL );
  ctx->out.chunk = fd_dcache_compact_next( ctx->out.chunk, (ulong)result, ctx->out.chunk0, ctx->out.wmark );
}

static ulong
fd_snaprd_parse_snapshot_slot( const char *str, ulong sz, ulong nth_occurrence ) {
  ulong number = 0UL;
  ulong parse_count = 0UL;
  char const * base = str;

  for (; (ulong)(str - base) < sz && *str!='\0'; str++) {
    if (*str >= '0' && *str <= '9') {
      number = number * 10UL + (ulong)(*str - '0');
    } else if (number > 0) {
      parse_count++;
      if( FD_LIKELY( parse_count==nth_occurrence ) ) return number;
      number = 0UL;
    }
  }

  FD_LOG_ERR(( "unexpected snapshot path: %s", base ));
}

static void
read_http_data( fd_snaprd_tile_t *  ctx,
                fd_stem_context_t * stem,
                long                now ) {
  uchar * out = fd_chunk_to_laddr( ctx->out.wksp, ctx->out.chunk );

  ulong data_len = ctx->out.mtu;
  int result = fd_sshttp_advance( ctx->sshttp, &data_len, out, now );

  switch( result ) {
    case FD_SSHTTP_ADVANCE_AGAIN: {
      char * path = fd_sshttp_opt_redirect_path( ctx->sshttp );
      if( FD_LIKELY( path!=NULL ) ) {
        switch( ctx->state ) {
          case FD_SNAPRD_STATE_READING_INCREMENTAL_URL_HTTP:
            fd_cstr_printf( ctx->remote.incremental_snapshot_path, sizeof(ctx->remote.incremental_snapshot_path), NULL, "%s", path );
            ctx->remote.incremental_snapshot_slot = fd_snaprd_parse_snapshot_slot( ctx->remote.incremental_snapshot_path, sizeof(ctx->remote.incremental_snapshot_path), 2UL );
            fd_snaprd_publish_snapshot_update( ctx, stem, FD_PLUGIN_MSG_SNAPSHOT_TYPE_INCREMENTAL, 1, ctx->remote.incremental_snapshot_slot, ctx->remote.incremental_snapshot_path );
            ctx->state = FD_SNAPRD_STATE_READING_INCREMENTAL_HTTP;
            break;
          case FD_SNAPRD_STATE_READING_FULL_URL_HTTP:
            fd_cstr_printf( ctx->remote.full_snapshot_path, sizeof(ctx->remote.full_snapshot_path), NULL, "%s", path );
            ctx->remote.full_snapshot_slot = fd_snaprd_parse_snapshot_slot( ctx->remote.full_snapshot_path, sizeof(ctx->remote.full_snapshot_path), 1UL );
            fd_snaprd_publish_snapshot_update( ctx, stem, FD_PLUGIN_MSG_SNAPSHOT_TYPE_FULL, 1, ctx->remote.full_snapshot_slot, ctx->remote.full_snapshot_path );
            ctx->state = FD_SNAPRD_STATE_READING_FULL_HTTP;
            break;
          default:
            break;
        }
      }
    break;
    }
    case FD_SSHTTP_ADVANCE_ERROR: {
      FD_LOG_NOTICE(( "error downloading snapshot from http://" FD_IP4_ADDR_FMT ":%hu/snapshot.tar.bz2",
                      FD_IP4_ADDR_FMT_ARGS( ctx->addr.addr ), ctx->addr.port ));
      fd_ssping_invalidate( ctx->ssping, ctx->addr, now );
      fd_stem_publish( stem, 0UL, FD_SNAPSHOT_MSG_CTRL_RESET_FULL, 0UL, 0UL, 0UL, 0UL, 0UL );
      ctx->state = FD_SNAPRD_STATE_FLUSHING_FULL_HTTP_RESET;
      ctx->deadline_nanos = now;
      break;
    }
    case FD_SSHTTP_ADVANCE_DONE: {
      switch( ctx->state ) {
        case FD_SNAPRD_STATE_READING_INCREMENTAL_HTTP:
          fd_stem_publish( stem, 0UL, FD_SNAPSHOT_MSG_CTRL_DONE, 0UL, 0UL, 0UL, 0UL, 0UL );
          ctx->state = FD_SNAPRD_STATE_FLUSHING_INCREMENTAL_HTTP;
          break;
        case FD_SNAPRD_STATE_READING_FULL_HTTP:
          if( FD_LIKELY( ctx->config.incremental_snapshot_fetch ) ) {
            fd_stem_publish( stem, 0UL, FD_SNAPSHOT_MSG_CTRL_EOF_FULL, 0UL, 0UL, 0UL, 0UL, 0UL );
          } else {
            fd_stem_publish( stem, 0UL, FD_SNAPSHOT_MSG_CTRL_DONE, 0UL, 0UL, 0UL, 0UL, 0UL );
          }
          ctx->state = FD_SNAPRD_STATE_FLUSHING_FULL_HTTP;
          break;
        default:
          break;
      }
      break;
    }
    case FD_SSHTTP_ADVANCE_DATA: {
      switch( ctx->state ) {
        case FD_SNAPRD_STATE_READING_INCREMENTAL_HTTP:
          ctx->metrics.incremental.bytes_read += data_len;
          break;
        case FD_SNAPRD_STATE_READING_FULL_HTTP:
          ctx->metrics.full.bytes_read += data_len;
          break;
        default:
          FD_LOG_ERR(( "unexpected state %d", ctx->state ));
          break;
      }

      fd_stem_publish( stem, 0UL, FD_SNAPSHOT_MSG_DATA, ctx->out.chunk, data_len, 0UL, 0UL, 0UL );
      ctx->out.chunk = fd_dcache_compact_next( ctx->out.chunk, data_len, ctx->out.chunk0, ctx->out.wmark );
      break;
    }
    default:
      FD_LOG_ERR(( "unexpected fd_sshttp_advance result %d", result ));
      break;
  }
}

static void
after_credit( fd_snaprd_tile_t *  ctx,
              fd_stem_context_t * stem,
              int *               opt_poll_in,
              int *               charge_busy ) {
  (void)stem;
  (void)opt_poll_in;
  (void)charge_busy;

  long now = fd_log_wallclock();
  if( FD_LIKELY( ctx->peer_selection ) ) {
    fd_ssping_advance( ctx->ssping, now );
  }

  /* All control fragments sent by the snaprd tile must be fully
     acknowledged by all downstream consumers before processing can
     proceed, to prevent tile state machines from getting out of sync
     (see fd_ssctrl.h for more details).  Currently there are two
     downstream consumers, snapdc and snapin. */
#define NUM_SNAP_CONSUMERS (2UL)

  switch ( ctx->state ) {
    case FD_SNAPRD_STATE_WAITING_FOR_PEERS: {
      fd_ip4_port_t best = fd_ssping_best( ctx->ssping );
      if( FD_LIKELY( best.l ) ) {
        ctx->state = FD_SNAPRD_STATE_COLLECTING_PEERS;
        ctx->deadline_nanos = now + 500L*1000L*1000L;
      }
      break;
    }
    case FD_SNAPRD_STATE_COLLECTING_PEERS: {
      if( FD_UNLIKELY( now<ctx->deadline_nanos ) ) break;

      fd_ip4_port_t best = fd_ssping_best( ctx->ssping );
      if( FD_UNLIKELY( !best.l ) ) {
        ctx->state = FD_SNAPRD_STATE_WAITING_FOR_PEERS;
        break;
      }

      ulong highest_cluster_slot = 0UL; /* TODO: Implement, using incremental snapshot slot for age */
  
      if( FD_LIKELY( ctx->local.full_snapshot_slot!=ULONG_MAX && ctx->local.full_snapshot_slot>=fd_ulong_sat_sub( highest_cluster_slot, ctx->config.maximum_local_snapshot_age ) ) ) {
        FD_LOG_NOTICE(( "loading full snapshot from local file `%s`", ctx->local.full_snapshot_path ));
        ctx->state = FD_SNAPRD_STATE_READING_FULL_FILE;
      } else {
        FD_LOG_NOTICE(( "downloading full snapshot from http://" FD_IP4_ADDR_FMT ":%hu/snapshot.tar.bz2", FD_IP4_ADDR_FMT_ARGS( best.addr ), best.port ));
        ctx->addr  = best;
        ctx->state = FD_SNAPRD_STATE_READING_FULL_URL_HTTP;
        fd_sshttp_init( ctx->sshttp, best, "/snapshot.tar.bz2", 17UL, now );
      }
      break;
    }
    case FD_SNAPRD_STATE_READING_FULL_FILE:
    case FD_SNAPRD_STATE_READING_INCREMENTAL_FILE:
      read_file_data( ctx, stem );
      break;
    case FD_SNAPRD_STATE_READING_FULL_URL_HTTP:
    case FD_SNAPRD_STATE_READING_INCREMENTAL_URL_HTTP:
      read_http_data( ctx, stem, now );
      break;
    case FD_SNAPRD_STATE_READING_FULL_HTTP:
    case FD_SNAPRD_STATE_READING_INCREMENTAL_HTTP: {
      read_http_data( ctx, stem, now );
      break;
    }
    case FD_SNAPRD_STATE_FLUSHING_INCREMENTAL_FILE:
    case FD_SNAPRD_STATE_FLUSHING_INCREMENTAL_HTTP:
      if( FD_UNLIKELY( ctx->ack_cnt<NUM_SNAP_CONSUMERS ) ) break;
      ctx->ack_cnt = 0UL;

      if( FD_UNLIKELY( ctx->malformed ) ) {
        fd_stem_publish( stem, 0UL, FD_SNAPSHOT_MSG_CTRL_RESET_FULL, 0UL, 0UL, 0UL, 0UL, 0UL );
        ctx->state = FD_SNAPRD_STATE_FLUSHING_FULL_HTTP_RESET;
        ctx->malformed = 0;
        break;
      }

      ctx->state = FD_SNAPRD_STATE_SHUTDOWN;
      fd_stem_publish( stem, 0UL, FD_SNAPSHOT_MSG_CTRL_SHUTDOWN, 0UL, 0UL, 0UL, 0UL, 0UL );
      break;
    case FD_SNAPRD_STATE_FLUSHING_FULL_FILE:
      if( FD_UNLIKELY( ctx->ack_cnt<NUM_SNAP_CONSUMERS ) ) break;
      ctx->ack_cnt = 0UL;

      if( FD_LIKELY( !ctx->config.incremental_snapshot_fetch ) ) {
        ctx->state = FD_SNAPRD_STATE_SHUTDOWN;
        fd_stem_publish( stem, 0UL, FD_SNAPSHOT_MSG_CTRL_SHUTDOWN, 0UL, 0UL, 0UL, 0UL, 0UL );
        break;
      }

      FD_LOG_NOTICE(( "reading incremental snapshot from local file `%s`", ctx->local.incremental_snapshot_path ));
      ctx->state = FD_SNAPRD_STATE_READING_INCREMENTAL_FILE;
      break;
    case FD_SNAPRD_STATE_FLUSHING_FULL_HTTP:
      if( FD_UNLIKELY( ctx->ack_cnt<NUM_SNAP_CONSUMERS ) ) break;
      ctx->ack_cnt = 0UL;

      if( FD_UNLIKELY( ctx->malformed ) ) {
        fd_stem_publish( stem, 0UL, FD_SNAPSHOT_MSG_CTRL_RESET_FULL, 0UL, 0UL, 0UL, 0UL, 0UL );
        ctx->state = FD_SNAPRD_STATE_FLUSHING_FULL_HTTP_RESET;
        ctx->malformed = 0;
        break;
      }

      if( FD_LIKELY( !ctx->config.incremental_snapshot_fetch ) ) {
        ctx->state = FD_SNAPRD_STATE_SHUTDOWN;
        fd_stem_publish( stem, 0UL, FD_SNAPSHOT_MSG_CTRL_SHUTDOWN, 0UL, 0UL, 0UL, 0UL, 0UL );
        break;
      }

      FD_LOG_NOTICE(( "downloading incremental snapshot from http://" FD_IP4_ADDR_FMT ":%hu/incremental-snapshot.tar.bz2", FD_IP4_ADDR_FMT_ARGS( ctx->addr.addr ), ctx->addr.port ));
      fd_sshttp_init( ctx->sshttp, ctx->addr, "/incremental-snapshot.tar.bz2", 29UL, fd_log_wallclock() );
      ctx->state = FD_SNAPRD_STATE_READING_INCREMENTAL_URL_HTTP;
      break;
    case FD_SNAPRD_STATE_FLUSHING_FULL_HTTP_RESET:
    case FD_SNAPRD_STATE_FLUSHING_FULL_FILE_RESET:
      if( FD_UNLIKELY( ctx->ack_cnt<NUM_SNAP_CONSUMERS ) ) break;
      ctx->ack_cnt = 0UL;

      ctx->state = FD_SNAPRD_STATE_COLLECTING_PEERS;
      ctx->deadline_nanos = 0L;
      break;
    case FD_SNAPRD_STATE_SHUTDOWN:
      break;
    default: {
      FD_LOG_ERR(( "unexpected state %d", ctx->state ));
      break;
    }
  }
}

static void
after_frag( fd_snaprd_tile_t *  ctx,
            ulong               in_idx,
            ulong               seq,
            ulong               sig,
            ulong               sz,
            ulong               tsorig,
            ulong               tspub,
            fd_stem_context_t * stem ) {
  (void)in_idx;
  (void)seq;
  (void)tsorig;
  (void)tspub;
  (void)sz;

  FD_TEST( sig==FD_SNAPSHOT_MSG_CTRL_ACK || sig==FD_SNAPSHOT_MSG_CTRL_MALFORMED );

  if( FD_LIKELY( sig==FD_SNAPSHOT_MSG_CTRL_ACK ) ) ctx->ack_cnt++;
  else {
    FD_TEST( ctx->state!=FD_SNAPRD_STATE_SHUTDOWN &&
             ctx->state!=FD_SNAPRD_STATE_COLLECTING_PEERS &&
             ctx->state!=FD_SNAPRD_STATE_WAITING_FOR_PEERS );

    switch( ctx->state) {
      case FD_SNAPRD_STATE_READING_FULL_FILE:
      case FD_SNAPRD_STATE_FLUSHING_FULL_FILE:
      case FD_SNAPRD_STATE_FLUSHING_FULL_FILE_RESET:
        FD_LOG_ERR(( "Error reading snapshot from local file `%s`", ctx->local.full_snapshot_path ));
      case FD_SNAPRD_STATE_READING_INCREMENTAL_FILE:
      case FD_SNAPRD_STATE_FLUSHING_INCREMENTAL_FILE:
        FD_LOG_ERR(( "Error reading snapshot from local file `%s`", ctx->local.incremental_snapshot_path ));
      case FD_SNAPRD_STATE_READING_FULL_HTTP:
      case FD_SNAPRD_STATE_READING_FULL_URL_HTTP:
      case FD_SNAPRD_STATE_READING_INCREMENTAL_HTTP:
      case FD_SNAPRD_STATE_READING_INCREMENTAL_URL_HTTP:
        FD_LOG_NOTICE(( "error downloading snapshot from http://" FD_IP4_ADDR_FMT ":%hu/snapshot.tar.bz2",
                        FD_IP4_ADDR_FMT_ARGS( ctx->addr.addr ), ctx->addr.port ));
        fd_sshttp_cancel( ctx->sshttp );
        fd_ssping_invalidate( ctx->ssping, ctx->addr, fd_log_wallclock() );
        fd_stem_publish( stem, 0UL, FD_SNAPSHOT_MSG_CTRL_RESET_FULL, 0UL, 0UL, 0UL, 0UL, 0UL );
        ctx->state = FD_SNAPRD_STATE_FLUSHING_FULL_HTTP_RESET;
        break;
      case FD_SNAPRD_STATE_FLUSHING_FULL_HTTP:
      case FD_SNAPRD_STATE_FLUSHING_INCREMENTAL_HTTP:
        FD_LOG_NOTICE(( "error downloading snapshot from http://" FD_IP4_ADDR_FMT ":%hu/snapshot.tar.bz2",
                        FD_IP4_ADDR_FMT_ARGS( ctx->addr.addr ), ctx->addr.port ));
        fd_sshttp_cancel( ctx->sshttp );
        fd_ssping_invalidate( ctx->ssping, ctx->addr, fd_log_wallclock() );
        /* We would like to transition to FULL_HTTP_RESET, but we can't
           do it just yet, because we have already sent a DONE control
           fragment, and need to wait for acknowledges to come back
           first, to ensure there's only one control message outstanding
           at a time. */
        ctx->malformed = 1;
        break;
      case FD_SNAPRD_STATE_FLUSHING_FULL_HTTP_RESET:
        break;
      default:
        FD_LOG_ERR(( "unexpected state %d", ctx->state ));
        break;
    }
  }
}

static void
privileged_init( fd_topo_t *      topo,
                 fd_topo_tile_t * tile ) {
  void * scratch = fd_topo_obj_laddr( topo, tile->tile_obj_id );

  FD_SCRATCH_ALLOC_INIT( l, scratch );
  fd_snaprd_tile_t * ctx = FD_SCRATCH_ALLOC_APPEND( l, alignof(fd_snaprd_tile_t),  sizeof(fd_snaprd_tile_t) );

  /* By default, the snaprd tile selects peers and its initial state is
     WAITING_FOR_PEERS. */
  ctx->peer_selection = 1;
  ctx->state          = FD_SNAPRD_STATE_WAITING_FOR_PEERS;

  ulong full_slot = ULONG_MAX;
  ulong incremental_slot = ULONG_MAX;
  char full_path[ PATH_MAX ] = {0};
  char incremental_path[ PATH_MAX ] = {0};
  if( FD_UNLIKELY( -1==fd_ssarchive_latest_pair( tile->snaprd.snapshots_path,
                                                 tile->snaprd.incremental_snapshot_fetch,
                                                 &full_slot,
                                                 &incremental_slot,
                                                 full_path,
                                                 incremental_path ) ) ) {
    ctx->local.full_snapshot_slot = ULONG_MAX;
    ctx->local.incremental_snapshot_slot = ULONG_MAX;
  } else {
    FD_TEST( full_slot!=ULONG_MAX );
    ctx->local.full_snapshot_slot = full_slot;
    ctx->local.incremental_snapshot_slot = incremental_slot;

    strncpy( ctx->local.full_snapshot_path, full_path, PATH_MAX );
    ctx->local.full_snapshot_fd = open( ctx->local.full_snapshot_path, O_RDONLY|O_CLOEXEC|O_NONBLOCK );
    if( FD_UNLIKELY( -1==ctx->local.full_snapshot_fd ) ) FD_LOG_ERR(( "open() failed `%s` (%i-%s)", ctx->local.full_snapshot_path, errno, fd_io_strerror( errno ) ));

    if( tile->snaprd.incremental_snapshot_fetch ) {
      FD_TEST( incremental_slot!=ULONG_MAX );
    }

    if( FD_LIKELY( incremental_slot!=ULONG_MAX ) ) {
      strncpy( ctx->local.incremental_snapshot_path, incremental_path, PATH_MAX );
      ctx->local.incremental_snapshot_fd = open( ctx->local.incremental_snapshot_path, O_RDONLY|O_CLOEXEC|O_NONBLOCK );
      if( FD_UNLIKELY( -1==ctx->local.incremental_snapshot_fd ) ) FD_LOG_ERR(( "open() failed `%s` (%i-%s)", ctx->local.incremental_snapshot_path, errno, fd_io_strerror( errno ) ));
    }

    if( FD_UNLIKELY( tile->snaprd.maximum_local_snapshot_age==0 ) ) {
      /* Disable peer selection if we are reading snapshots from disk
         and there is no maximum local snapshot age set.
         Set the initial state to READING_FULL_FILE to avoid peer
         selection logic. */
      ctx->peer_selection = 0;
      ctx->state          = FD_SNAPRD_STATE_READING_FULL_FILE;
    }
  }
}

static void
unprivileged_init( fd_topo_t *      topo,
                   fd_topo_tile_t * tile ) {
  void * scratch = fd_topo_obj_laddr( topo, tile->tile_obj_id );

  FD_SCRATCH_ALLOC_INIT( l, scratch );
  fd_snaprd_tile_t * ctx  = FD_SCRATCH_ALLOC_APPEND( l, alignof(fd_snaprd_tile_t),  sizeof(fd_snaprd_tile_t)       );
  void * _sshttp          = FD_SCRATCH_ALLOC_APPEND( l, fd_sshttp_align(),          fd_sshttp_footprint()          );
  void * _ssping          = FD_SCRATCH_ALLOC_APPEND( l, fd_ssping_align(),          fd_ssping_footprint( 65536UL ) );

  ctx->ack_cnt = 0UL;
  ctx->malformed = 0;

  fd_memset( &ctx->metrics, 0, sizeof(ctx->metrics) );

  ctx->config.incremental_snapshot_fetch = tile->snaprd.incremental_snapshot_fetch;
  ctx->config.do_download                = tile->snaprd.do_download;
  ctx->config.maximum_local_snapshot_age = tile->snaprd.maximum_local_snapshot_age;
  ctx->config.minimum_download_speed_mib = tile->snaprd.minimum_download_speed_mib;

  if( FD_UNLIKELY( !tile->snaprd.maximum_download_retry_abort ) ) ctx->config.maximum_download_retry_abort = UINT_MAX;
  else                                                            ctx->config.maximum_download_retry_abort = tile->snaprd.maximum_download_retry_abort;

  ctx->ssping = fd_ssping_join( fd_ssping_new( _ssping, 65536UL, 1UL ) );
  FD_TEST( ctx->ssping );

  ctx->sshttp = fd_sshttp_join( fd_sshttp_new( _sshttp ) );
  FD_TEST( ctx->sshttp );

  if( FD_LIKELY( !strcmp( tile->snaprd.cluster, "testnet" ) ) ) {
    fd_ip4_port_t initial_peers[ 2UL ] = {
      { .addr = FD_IP4_ADDR( 145, 40, 95, 69 ), .port = 8899 }, /* Solana testnet peer */
      { .addr = FD_IP4_ADDR( 177, 54, 155, 187 ), .port = 8899 } /* A fast testnet peer from snapshot-finder script */
    };

    for( ulong i=0UL; i<2UL; i++ ) fd_ssping_add( ctx->ssping, initial_peers[ i ] );
  } else if( FD_LIKELY( !strcmp( tile->snaprd.cluster, "private" ) ) ) {
    fd_ip4_port_t initial_peers[ 1UL ] = {
      { .addr = FD_IP4_ADDR( 147, 28, 185, 47 ), .port = 8899 } /* A private cluster peer */
    };

    for( ulong i=0UL; i<1UL; i++ ) fd_ssping_add( ctx->ssping, initial_peers[ i ] );
  } else {
    FD_LOG_ERR(( "unexpected cluster %s", tile->snaprd.cluster ));
  }

  if( FD_UNLIKELY( tile->out_cnt!=2UL ) ) FD_LOG_ERR(( "tile `" NAME "` has %lu outs, expected 2", tile->out_cnt ));

  ctx->out.wksp   = topo->workspaces[ topo->objs[ topo->links[ tile->out_link_id[ 0 ] ].dcache_obj_id ].wksp_id ].wksp;
  ctx->out.chunk0 = fd_dcache_compact_chunk0( ctx->out.wksp, topo->links[ tile->out_link_id[ 0 ] ].dcache );
  ctx->out.wmark  = fd_dcache_compact_wmark ( ctx->out.wksp, topo->links[ tile->out_link_id[ 0 ] ].dcache, topo->links[ tile->out_link_id[ 0 ] ].mtu );
  ctx->out.chunk  = ctx->out.chunk0;
  ctx->out.mtu    = topo->links[ tile->out_link_id[ 0 ] ].mtu;

  ctx->plugin_out.wksp   = topo->workspaces[ topo->objs[ topo->links[ tile->out_link_id[ FD_SNAPRD_PLUGIN_OUT_IDX ] ].dcache_obj_id ].wksp_id ].wksp;
  ctx->plugin_out.chunk0 = fd_dcache_compact_chunk0( ctx->plugin_out.wksp, topo->links[ tile->out_link_id[ FD_SNAPRD_PLUGIN_OUT_IDX ] ].dcache );
  ctx->plugin_out.wmark  = fd_dcache_compact_wmark ( ctx->plugin_out.wksp, topo->links[ tile->out_link_id[ FD_SNAPRD_PLUGIN_OUT_IDX ] ].dcache, topo->links[ tile->out_link_id[ 1 ] ].mtu );
  ctx->plugin_out.chunk  = ctx->plugin_out.chunk0;
  ctx->plugin_out.mtu    = topo->links[ tile->out_link_id[ FD_SNAPRD_PLUGIN_OUT_IDX ] ].mtu;
}

#define STEM_BURST 2UL /* One control message, and one data message */
#define STEM_LAZY  1000L

#define STEM_CALLBACK_CONTEXT_TYPE  fd_snaprd_tile_t
#define STEM_CALLBACK_CONTEXT_ALIGN alignof(fd_snaprd_tile_t)

#define STEM_CALLBACK_SHOULD_SHUTDOWN should_shutdown
#define STEM_CALLBACK_METRICS_WRITE   metrics_write
#define STEM_CALLBACK_AFTER_CREDIT    after_credit
#define STEM_CALLBACK_AFTER_FRAG      after_frag

#include "../../disco/stem/fd_stem.c"

fd_topo_run_tile_t fd_tile_snaprd = {
  .name                 = NAME,
  .scratch_align        = scratch_align,
  .scratch_footprint    = scratch_footprint,
  .privileged_init      = privileged_init,
  .unprivileged_init    = unprivileged_init,
  .run                  = stem_run,
  .keep_host_networking = 1,
  .allow_connect        = 1
};

#undef NAME
