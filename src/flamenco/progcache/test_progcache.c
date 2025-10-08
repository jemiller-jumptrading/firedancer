#include "fd_progcache_admin.h"
#include "fd_progcache_user.h"
#include "../runtime/fd_bank.h"

/* Load in programdata for tests */
FD_IMPORT_BINARY( valid_program_data,        "src/ballet/sbpf/fixtures/hello_solana_program.so" );
FD_IMPORT_BINARY( bigger_valid_program_data, "src/ballet/sbpf/fixtures/clock_sysvar_program.so" );
FD_IMPORT_BINARY( invalid_program_data,      "src/ballet/sbpf/fixtures/malformed_bytecode.so"   );

struct test_env {
  fd_wksp_t *          wksp;

  fd_progcache_admin_t progcache_admin[1];
  fd_progcache_t       progcache[1];
  fd_funk_t            accdb[1];
  fd_features_t        features[1];
};

typedef struct test_env test_env_t;

static test_env_t *
test_env_create( fd_wksp_t * wksp ) {
  ulong txn_max           = 16UL;
  ulong accdb_rec_max     = 32UL;
  ulong progcache_rec_max = 32UL;
  ulong wksp_tag          =  1UL;

  void * accdb_mem = fd_wksp_alloc_laddr( wksp, fd_funk_align(), fd_funk_footprint( txn_max, accdb_rec_max ), wksp_tag );
  FD_TEST( fd_funk_new( accdb_mem, wksp_tag, 1UL, txn_max, accdb_rec_max ) );

  void * progcache_mem = fd_wksp_alloc_laddr( wksp, fd_funk_align(), fd_funk_footprint( txn_max, progcache_rec_max ), wksp_tag );
  FD_TEST( fd_funk_new( progcache_mem, wksp_tag, 1UL, txn_max, progcache_rec_max ) );

  test_env_t * env = fd_wksp_alloc_laddr( wksp, alignof(test_env_t), sizeof(test_env_t), wksp_tag );
  FD_TEST( env );
  memset( env, 0, sizeof(test_env_t) );

  env->wksp = wksp;
  FD_TEST( fd_progcache_admin_join( env->progcache_admin, progcache_mem ) );
  FD_TEST( fd_progcache_join( env->progcache, progcache_mem ) );
  FD_TEST( fd_funk_join( env->accdb, accdb_mem ) );

  return env;
}

static void
test_env_destroy( test_env_t * env ) {
  void * accdb_mem = NULL;
  FD_TEST( fd_progcache_admin_leave( env->progcache_admin, &accdb_mem ) );
  FD_TEST( fd_progcache_leave      ( env->progcache,       &accdb_mem ) );
  fd_wksp_free_laddr( fd_funk_delete( accdb_mem ) );

  void * progcache_mem = NULL;
  FD_TEST( fd_funk_leave( env->accdb, &progcache_mem ) );
  fd_wksp_free_laddr( fd_funk_delete( progcache_mem ) );

  fd_wksp_free_laddr( env );
}

static void
test_env_txn_prepare( test_env_t *              env,
                      fd_funk_txn_xid_t const * parent,
                      fd_funk_txn_xid_t const * xid ) {
  fd_funk_txn_xid_t root[1];
  if( !parent ) {
    fd_funk_txn_xid_set_root( root );
    parent = root;
  }
  fd_funk_txn_prepare( env->accdb, parent, xid );
  fd_progcache_txn_prepare( env->progcache_admin, parent, xid );
}

static void
test_env_txn_cancel( test_env_t *              env,
                     fd_funk_txn_xid_t const * xid ) {
  fd_funk_txn_cancel( env->accdb, xid );
  fd_progcache_txn_cancel( env->progcache_admin, xid );
}

static void
test_env_txn_publish( test_env_t *              env,
                      fd_funk_txn_xid_t const * xid ) {
  fd_funk_txn_publish( env->accdb, xid );
  fd_progcache_txn_publish( env->progcache_admin, xid );
  (void)test_env_txn_prepare; (void)test_env_txn_cancel;
}

static fd_funk_rec_key_t
test_key( ulong x ) {
  fd_funk_rec_key_t key = {0};
  key.ul[0] = x;
  return key;
}

static void
create_test_account( test_env_t * env,
                     fd_funk_txn_xid_t const * xid,
                     void const * pubkey_,
                     void const * owner_,
                     void const * data,
                     ulong        data_len,
                     uchar        executable ) {
  fd_pubkey_t pubkey = FD_LOAD( fd_pubkey_t, pubkey_ );
  fd_pubkey_t owner  = FD_LOAD( fd_pubkey_t, owner_ ) ;

  FD_TXN_ACCOUNT_DECL( acc );
  fd_funk_rec_prepare_t prepare = {0};
  int err = fd_txn_account_init_from_funk_mutable( /* acc         */ acc,
                                                   /* pubkey      */ &pubkey,
                                                   /* funk        */ env->accdb,
                                                   /* xid         */ xid,
                                                   /* do_create   */ 1,
                                                   /* min_data_sz */ data_len,
                                                   /* prepare     */ &prepare );
  FD_TEST( !err );

  if( data ) {
    fd_txn_account_set_data( acc, data, data_len );
  }

  acc->starting_lamports = 1UL;
  acc->starting_dlen     = data_len;
  fd_txn_account_set_lamports( acc, 1UL );
  fd_txn_account_set_executable( acc, executable );
  fd_txn_account_set_owner( acc, &owner );

  fd_txn_account_mutable_fini( acc, env->accdb, &prepare );
}

/* test_empty: Account database and progcache completely empty.
   Query at root should fail. */

static void
test_empty( fd_wksp_t * wksp ) {
  test_env_t * env = test_env_create( wksp );

  fd_funk_txn_xid_t xid[1]; fd_funk_txn_xid_set_root( xid );
  fd_funk_rec_key_t key = test_key( 1UL );
  fd_prog_load_env_t load_env = {
    .features    = env->features,
    .slot        = 1UL,
    .epoch       = 0UL,
    .epoch_slot0 = 0UL
  };
  fd_progcache_rec_t const * rec = fd_progcache_pull( env->progcache, env->accdb, xid, &key, &load_env );
  FD_TEST( !rec );

  test_env_destroy( env );
}

/* test_account_does_not_exist: Program account missing, but querying at
   a fork. */

static void
test_account_does_not_exist( fd_wksp_t * wksp ) {
  test_env_t * env = test_env_create( wksp );
  fd_funk_txn_xid_t fork_a = { .ul = { 1UL, 1UL } };
  test_env_txn_prepare( env, NULL, &fork_a );

  (void)test_env_txn_publish;

  test_env_txn_cancel( env, &fork_a );
  test_env_destroy( env );
}

/* test_invalid_owner: Account exists but is not owned by BPF loader */

static void
test_invalid_owner( fd_wksp_t * wksp ) {
  test_env_t * env = test_env_create( wksp );
  fd_funk_txn_xid_t fork_a = { .ul = { 1UL, 1UL } };
  test_env_txn_prepare( env, NULL, &fork_a );

  fd_funk_rec_key_t key = test_key( 1UL );
  create_test_account( env, &fork_a, &key,
                       &fd_solana_system_program_id, /* not a BPF laoder */
                       invalid_program_data,
                       invalid_program_data_sz,
                       1 );

  fd_prog_load_env_t load_env = {
    .features    = env->features,
    .slot        = 1UL,
    .epoch       = 0UL,
    .epoch_slot0 = 0UL
  };
  FD_TEST( !fd_progcache_pull( env->progcache, env->accdb, &fork_a, &key, &load_env ) );

  test_env_txn_cancel( env, &fork_a );
  test_env_destroy( env );
}

static void
test_invalid_program( fd_wksp_t * wksp ) {
  test_env_t * env = test_env_create( wksp );
  fd_funk_txn_xid_t fork_a = { .ul = { 1UL, 1UL } };
  test_env_txn_prepare( env, NULL, &fork_a );

  fd_funk_rec_key_t key = test_key( 1UL );
  create_test_account( env, &fork_a, &key,
                       &fd_solana_bpf_loader_program_id,
                       invalid_program_data,
                       invalid_program_data_sz,
                       1 );

  FD_TEST( !fd_progcache_peek( env->progcache, &fork_a, &key, 0UL ) );
  FD_TEST( env->progcache->fork_depth==2UL );
  FD_TEST( fd_funk_txn_xid_eq( &env->progcache->fork[ 0 ], &fork_a ) );
  FD_TEST( fd_funk_txn_xid_eq( &env->progcache->fork[ 1 ], fd_funk_root( env->progcache->funk ) ) );

  fd_prog_load_env_t load_env = {
    .features    = env->features,
    .slot        = 1UL,
    .epoch       = 0UL,
    .epoch_slot0 = 0UL
  };
  fd_progcache_rec_t const * rec = fd_progcache_pull( env->progcache, env->accdb, &fork_a, &key, &load_env );
  FD_TEST( rec );
  FD_TEST( !rec->executable );

  test_env_txn_cancel( env, &fork_a );
  test_env_destroy( env );
}

static void
test_valid_program( fd_wksp_t * wksp ) {
  test_env_t * env = test_env_create( wksp );
  fd_funk_txn_xid_t fork_a = { .ul = { 1UL, 1UL } };
  test_env_txn_prepare( env, NULL, &fork_a );

  fd_funk_rec_key_t key = test_key( 1UL );
  create_test_account( env, &fork_a, &key,
                       &fd_solana_bpf_loader_program_id,
                       valid_program_data,
                       valid_program_data_sz,
                       1 );

  FD_TEST( !fd_progcache_peek( env->progcache, &fork_a, &key, 0UL ) );
  FD_TEST( env->progcache->fork_depth==2UL );
  FD_TEST( fd_funk_txn_xid_eq( &env->progcache->fork[ 0 ], &fork_a ) );
  FD_TEST( fd_funk_txn_xid_eq( &env->progcache->fork[ 1 ], fd_funk_root( env->progcache->funk ) ) );

  fd_prog_load_env_t load_env = {
    .features    = env->features,
    .slot        = 1UL,
    .epoch       = 0UL,
    .epoch_slot0 = 0UL
  };
  fd_progcache_rec_t const * rec = fd_progcache_pull( env->progcache, env->accdb, &fork_a, &key, &load_env );
  FD_TEST( rec );
  FD_TEST( rec->executable );
  FD_TEST( fd_progcache_peek( env->progcache, &fork_a, &key, 0UL )==rec );
  FD_TEST( env->progcache->fork_depth==2UL );

  fd_funk_txn_xid_t fork_b = { .ul = { 64UL, 2UL } };
  test_env_txn_prepare( env, &fork_a, &fork_b );
  FD_TEST( fd_progcache_peek( env->progcache, &fork_b, &key, 0UL )==rec );
  FD_TEST( env->progcache->fork_depth==3UL );

  load_env.slot        = 64UL;
  load_env.epoch       =  0UL;
  load_env.epoch_slot0 =  0UL;
  fd_progcache_rec_t const * rec2 = fd_progcache_pull( env->progcache, env->accdb, &fork_b, &key, &load_env );
  FD_TEST( rec==rec2 );
  FD_TEST( fd_progcache_peek( env->progcache, &fork_b, &key, 0UL )==rec );

  test_env_txn_cancel( env, &fork_a ); /* should also cancel fork_b */
  test_env_destroy( env );
}

static void
test_epoch_boundary( fd_wksp_t * wksp ) {
  test_env_t * env = test_env_create( wksp );
  fd_funk_txn_xid_t fork_a = { .ul = { 1UL, 1UL } };
  test_env_txn_prepare( env, NULL, &fork_a );

  fd_funk_rec_key_t key = test_key( 1UL );
  create_test_account( env, &fork_a, &key,
                       &fd_solana_bpf_loader_program_id,
                       valid_program_data,
                       valid_program_data_sz,
                       1 );

  FD_TEST( !fd_progcache_peek( env->progcache, &fork_a, &key, 0UL ) );
  FD_TEST( env->progcache->fork_depth==2UL );
  FD_TEST( fd_funk_txn_xid_eq( &env->progcache->fork[ 0 ], &fork_a ) );
  FD_TEST( fd_funk_txn_xid_eq( &env->progcache->fork[ 1 ], fd_funk_root( env->progcache->funk ) ) );

  fd_prog_load_env_t load_env = {
    .features    = env->features,
    .slot        = 1UL,
    .epoch       = 0UL,
    .epoch_slot0 = 0UL
  };
  fd_progcache_rec_t const * rec = fd_progcache_pull( env->progcache, env->accdb, &fork_a, &key, &load_env );
  FD_TEST( rec );
  FD_TEST( rec->executable );
  FD_TEST( fd_progcache_peek( env->progcache, &fork_a, &key, 0UL )==rec );

  fd_funk_txn_xid_t fork_b = { .ul = { 64UL, 2UL } };
  test_env_txn_prepare( env, &fork_a, &fork_b );
  load_env.slot        = 64UL;
  load_env.epoch       =  1UL;
  load_env.epoch_slot0 = 64UL;
  fd_progcache_rec_t const * rec2 = fd_progcache_pull( env->progcache, env->accdb, &fork_b, &key, &load_env );
  FD_TEST( rec2 );
  FD_TEST( rec!=rec2 );
  FD_TEST( rec2->executable );
  FD_TEST( fd_progcache_peek( env->progcache, &fork_b, &key, 1UL )==rec2 );

  test_env_txn_cancel( env, &fork_b );
  test_env_txn_cancel( env, &fork_a );
  test_env_destroy( env );
}

static void
test_program_in_cache_queued_for_reverification( fd_wksp_t * wksp ) {
  (void)wksp;
}

static void
test_program_queued_for_reverification_account_does_not_exist( fd_wksp_t * wksp ) {
  (void)wksp;
}

static void
test_program_in_cache_queued_for_reverification_and_processed( fd_wksp_t * wksp ) {
  (void)wksp;
}

static void
test_invalid_genesis_program_reverified_after_genesis( fd_wksp_t * wksp ) {
  (void)wksp;
}

static void
test_valid_genesis_program_reverified_after_genesis( fd_wksp_t * wksp ) {
  (void)wksp;
}

static void
test_program_upgraded_with_larger_programdata( fd_wksp_t * wksp ) {
  (void)wksp;
}

static void
test_program_rooted( fd_wksp_t * wksp ) {
  (void)wksp;
}


struct test_case {
  char const * name;
  void      (* fn)( fd_wksp_t * wksp );
};

static int
match_test_name( char const * test_name,
                 int          argc,
                 char **      argv ) {
  if( argc<=1 ) return 1;
  for( int i=1; i<argc; i++ ) {
    if( strstr( test_name, argv[ i ] ) ) return 1;
  }
  return 0;
}

int
main( int     argc,
      char ** argv ) {
  fd_boot( &argc, &argv );

  ulong cpu_idx = fd_tile_cpu_id( fd_tile_idx() );
  if( cpu_idx>fd_shmem_cpu_cnt() ) cpu_idx = 0UL;

  char const * _page_sz  = fd_env_strip_cmdline_cstr ( &argc, &argv, "--page-sz",   NULL, "gigantic"                   );
  ulong        page_cnt  = fd_env_strip_cmdline_ulong( &argc, &argv, "--page-cnt",  NULL, 2UL                          );
  ulong        numa_idx  = fd_env_strip_cmdline_ulong( &argc, &argv, "--numa-idx",  NULL, fd_shmem_numa_idx( cpu_idx ) );

  ulong page_sz = fd_cstr_to_shmem_page_sz( _page_sz );
  if( FD_UNLIKELY( !page_sz ) ) FD_LOG_ERR(( "unsupported --page-sz" ));

  FD_LOG_NOTICE(( "Creating workspace (--page-cnt %lu, --page-sz %s, --numa-idx %lu)", page_cnt, _page_sz, numa_idx ));
  fd_wksp_t * wksp = fd_wksp_new_anonymous( page_sz, page_cnt, fd_shmem_cpu_idx( numa_idx ), "wksp", 0UL );
  FD_TEST( wksp );

# define TEST( name ) { #name, name }
  struct test_case cases[] = {
    TEST( test_empty ),
    TEST( test_account_does_not_exist ),
    TEST( test_invalid_owner ),
    TEST( test_invalid_program ),
    TEST( test_valid_program ),
    TEST( test_epoch_boundary ),
    TEST( test_program_in_cache_queued_for_reverification ),
    TEST( test_program_queued_for_reverification_account_does_not_exist ),
    TEST( test_program_in_cache_queued_for_reverification_and_processed ),
    TEST( test_invalid_genesis_program_reverified_after_genesis ),
    TEST( test_valid_genesis_program_reverified_after_genesis ),
    TEST( test_program_upgraded_with_larger_programdata ),
    TEST( test_program_rooted ),
    {0}
  };
# undef TEST
  for( struct test_case * tc = cases; tc->name; tc++ ) {
    if( match_test_name( tc->name, argc, argv ) ) {
      FD_LOG_NOTICE(( "Running %s", tc->name ));
      tc->fn( wksp );
    }
  }

  fd_wksp_delete_anonymous( wksp );

  FD_LOG_NOTICE(( "pass" ));
  fd_halt();
  return 0;
}
