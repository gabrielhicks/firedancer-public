#ifndef HEADER_fd_src_flamenco_stakes_fd_stakes_h
#define HEADER_fd_src_flamenco_stakes_fd_stakes_h

#include "../fd_flamenco_base.h"
#include "../types/fd_types.h"

FD_PROTOTYPES_BEGIN

/* fd_stake_weights_by_node converts Stakes (unordered list of (vote
   acc, active stake) tuples) to an ordered list of (stake, node
   identity) sorted by (stake descending, node identity descending).

   weights points to an array suitable to hold ...

     fd_vote_accounts_pair_t_map_size( accs->vote_accounts_pool,
                                       accs->vote_accounts_root )

   ... items.  On return, weights be an ordered list.

   Returns the number of items in weights (which is <= no of vote accs).
   On failure returns ULONG_MAX.  Reasons for failure include not enough
   bump allocator space available. */
#define STAKE_ACCOUNT_SIZE ( 200 )

struct fd_compute_stake_delegations {
   ulong                           epoch;
   fd_stake_history_t const *      stake_history;
   ulong *                         new_rate_activation_epoch;
   fd_stake_weight_t_mapnode_t *   delegation_pool;
   fd_stake_weight_t_mapnode_t *   delegation_root;
   ulong                           vote_states_pool_sz;
   fd_spad_t * *                   spads;
};
typedef struct fd_compute_stake_delegations fd_compute_stake_delegations_t;

struct fd_accumulate_delegations_task_args {
   fd_exec_slot_ctx_t const *         slot_ctx;
   fd_stake_history_t const *         stake_history;
   ulong *                            new_rate_activation_epoch;
   fd_stake_history_entry_t *         accumulator;
   fd_epoch_info_t *                  temp_info;
   fd_spad_t * *                      spads;
   fd_delegation_pair_t_mapnode_t *   stake_delegations_pool;
   ulong                              epoch;
};
typedef struct fd_accumulate_delegations_task_args fd_accumulate_delegations_task_args_t;

struct fd_delegation_slim {
  ulong       next; /* ULONG_MAX if last */
  fd_pubkey_t voter_pubkey;
  ulong       stake;
  ulong       activation_epoch;
  ulong       deactivation_epoch;
  double      warmup_cooldown_rate;
};
typedef struct fd_delegation_slim fd_delegation_slim_t;
#define FD_DELEGATION_SLIM_ALIGN alignof(fd_delegation_slim_t)

struct fd_vote_epoch_credits_slim {
  ulong epoch;
  ulong credits;
  ulong prev_credits;
};
typedef struct fd_vote_epoch_credits_slim fd_vote_epoch_credits_slim_t;

struct fd_vote_account_slim {
  fd_pubkey_t                  key;
  fd_pubkey_t                  node_pubkey;
  ulong                        stake;
  ulong                        commission;
  ulong                        credits_observed;
  ulong                        epoch_credits_cnt;
  fd_vote_epoch_credits_slim_t epoch_credits[64];
  ulong                        root_slot;
  ulong                        delegations;
};
typedef struct fd_vote_account_slim fd_vote_account_slim_t;

#define FD_VOTE_ACCOUNTS_SLIM_MAX (20000)
#define FD_DELEGATIONS_SLIM_MAX (10000)
struct __attribute__((aligned(64UL))) fd_vote_accounts_slim {
  /* Accounts are sorted by key, so we can use binary search to find an account */
  ulong vote_accounts_cnt;
  ulong delegations_pool_cnt;
  fd_vote_account_slim_t vote_accounts[FD_VOTE_ACCOUNTS_SLIM_MAX];
  fd_delegation_slim_t delegations_pool[FD_DELEGATIONS_SLIM_MAX];
};
typedef struct fd_vote_accounts_slim fd_vote_accounts_slim_t;
#define FD_VOTE_ACCOUNTS_SLIM_ALIGN (64UL)

struct __attribute__((aligned(64UL))) fd_stakes_slim {
  ulong epoch;
  fd_vote_accounts_slim_t vote_accounts;
};
typedef struct fd_stakes_slim fd_stakes_slim_t;
#define FD_STAKES_SLIM_ALIGN (64UL)

ulong
fd_stake_weights_by_node( fd_vote_accounts_slim_t const * accs,
                          fd_stake_weight_t *     weights,
                          fd_spad_t *             runtime_spad );


void
fd_stakes_activate_epoch( fd_exec_slot_ctx_t *  slot_ctx,
                          ulong *               new_rate_activation_epoch,
                          fd_epoch_info_t *     temp_info,
                          fd_spad_t * *         exec_spads,
                          ulong                 exec_spad_cnt,
                          fd_spad_t *           runtime_spad );

fd_stake_history_entry_t
stake_and_activating( fd_delegation_t const * delegation,
                      ulong                   target_epoch,
                      fd_stake_history_t *    stake_history,
                      ulong *                 new_rate_activation_epoch );

fd_stake_history_entry_t
stake_activating_and_deactivating( fd_delegation_t const * delegation,
                                   ulong                   target_epoch,
                                   fd_stake_history_t *    stake_history,
                                   ulong *                 new_rate_activation_epoch );

int
write_stake_state( fd_txn_account_t *    stake_acc_rec,
                   fd_stake_state_v2_t * stake_state );

void
fd_stakes_remove_stake_delegation( fd_exec_slot_ctx_t * slot_ctx, fd_borrowed_account_t * stake_account, ulong * new_rate_activation_epoch );

void
fd_stakes_upsert_stake_delegation( fd_exec_slot_ctx_t * slot_ctx, fd_borrowed_account_t * stake_account, ulong * new_rate_activation_epoch );

void
fd_refresh_vote_accounts( fd_exec_slot_ctx_t *       slot_ctx,
                          fd_stakes_slim_t const *   stakes,
                          ulong *                    new_rate_activation_epoch,
                          fd_epoch_info_t *          temp_info,
                          fd_spad_t * *              exec_spads,
                          fd_spad_t *                runtime_spad );

/* A workaround to mimic Agave function get_epoch_reward_calculate_param_info
   https://github.com/anza-xyz/agave/blob/v2.2.14/runtime/src/bank/partitioned_epoch_rewards/calculation.rs#L299 */
void
fd_populate_vote_accounts( fd_exec_slot_ctx_t *       slot_ctx,
                          fd_stakes_slim_t const *   stakes,
                          ulong *                    new_rate_activation_epoch,
                          fd_epoch_info_t *          temp_info,
                          fd_spad_t * *              exec_spads,
                          fd_spad_t *                runtime_spad );

void
fd_accumulate_stake_infos( fd_exec_slot_ctx_t const * slot_ctx,
                           fd_stakes_slim_t const *   stakes,
                           ulong *                    new_rate_activation_epoch,
                           fd_stake_history_entry_t * accumulator,
                           fd_epoch_info_t *          temp_info,
                           fd_spad_t * *              exec_spads,
                           ulong                      exec_spads_cnt,
                           fd_spad_t *                runtime_spad );

FD_PROTOTYPES_END

#endif /* HEADER_fd_src_flamenco_stakes_fd_stakes_h */
