#include "../../shared/commands/run/run.h"
#include "../../shared/commands/configure/configure.h"
#include "../../../disco/topo/fd_topob.h"
#include <unistd.h>

extern fd_topo_obj_callbacks_t * CALLBACKS[];
extern fd_topo_run_tile_t fdctl_tile_run( fd_topo_tile_t const * tile );

static void
bench_solcap_cmd_args( int *    pargc,
                       char *** pargv,
                       args_t * args ) {
  (void)pargc; (void)pargv; (void)args;
}

static void
bench_solcap_topo( config_t * config,
                   args_t *   args ) {
  (void)args;
  fd_topo_t * topo = &config->topo;
  fd_topob_new( &config->topo, config->name );
  topo->max_page_size = fd_cstr_to_shmem_page_sz( config->hugetlbfs.max_page_size );

  fd_topob_wksp( topo, "solcap" );
  fd_topo_tile_t * tile_solcap = fd_topob_tile( topo, "solcap", "solcap", "solcap", 0UL, 0, 0 );
  tile_solcap->solcap.block_cnt =   256UL;
  tile_solcap->solcap.block_sz  = 65536UL;

  fd_topob_finish( topo, CALLBACKS );
  fd_topo_print_log( /* stdout */ 1, topo );
}

static void
bench_solcap_cmd_fn( args_t *   args,
                     config_t * config ) {
  bench_solcap_topo( config, args );

  initialize_workspaces( config );
  initialize_stacks( config );
  fd_topo_t * topo = &config->topo;
  fd_topo_join_workspaces( topo, FD_SHMEM_JOIN_MODE_READ_WRITE );

  fd_topo_run_single_process( topo, 2, config->uid, config->gid, fdctl_tile_run );
  for(;;) pause();
}

static void
bench_solcap_cmd_perm( args_t *         args,
                       fd_cap_chk_t *   chk    FD_PARAM_UNUSED,
                       config_t const * config FD_PARAM_UNUSED ) {
  args->configure.command = CONFIGURE_CMD_INIT;
  ulong stage_idx = 0UL;
  args->configure.stages[ stage_idx++ ] = &fd_cfg_stage_hugetlbfs;
  args->configure.stages[ stage_idx++ ] = NULL;
  configure_cmd_perm( args, chk, config );
  run_cmd_perm( NULL, chk, config );
}

action_t fd_action_bench_solcap = {
  .name        = "bench-solcap",
  .args        = bench_solcap_cmd_args,
  .fn          = bench_solcap_cmd_fn,
  .perm        = bench_solcap_cmd_perm,
  .description = "Benchmark solcap logger"
};
