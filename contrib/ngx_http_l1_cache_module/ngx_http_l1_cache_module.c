/* What: Stores validated HTTP response bodies.
 * Why: Workers share one RAM object cache.
 */
#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#include <ngx_http_cache.h>
#include <ngx_http_proxy_module.h>
#include <ngx_http_upstream.h>

#define L1_HEADER(h, s) \
    ((h).key.len == sizeof(s) - 1 \
     && ngx_strncasecmp((h).key.data, (u_char *) s, sizeof(s) - 1) == 0)

#define L1_BLOCK_SIZE (64 * 1024)
#define L1_MAX_OBJECT (8 * 1024 * 1024)
#define L1_MAX_PROTECTED_PERCENT 80
#define L1_MAX_BUILDING_PERCENT 10

typedef enum {
    L1_ENTRY_BUILDING = 0,
    L1_ENTRY_READY,
    L1_ENTRY_EVICTING,
    L1_ENTRY_EXPIRED
} ngx_http_l1_cache_state_e;

extern ngx_module_t ngx_http_slice_filter_module;
extern ngx_int_t ngx_http_cache_purge_access_handler(ngx_http_request_t *r);
extern ngx_int_t ngx_http_proxy_cache_purge_handler(ngx_http_request_t *r);

static ngx_str_t l1_cache_slice_range_name = ngx_string("slice_range");
static ngx_str_t l1_cache_key_name = ngx_string("l1_cache_key");

typedef struct ngx_http_l1_cache_entry_s ngx_http_l1_cache_entry_t;
typedef struct ngx_http_l1_cache_capture_s ngx_http_l1_cache_capture_t;
typedef struct ngx_http_l1_cache_pin_s ngx_http_l1_cache_pin_t;

typedef struct {
    ngx_http_upstream_srv_conf_t *upstream;
    ngx_http_upstream_init_peer_pt init;
} ngx_http_l1_cache_peer_hook_t;

struct ngx_http_l1_cache_capture_s {
    ngx_queue_t   queue;
    ngx_pid_t     pid;
    ngx_atomic_t  generation;
    /* What: Stores capture bytes or a shared entry pointer.
     * Why: Both capture forms use one shared layout.
     */
    size_t        bytes_or_entry;
    size_t        key_len;
    ngx_uint_t    flags;
    u_char        key[1];
};

struct ngx_http_l1_cache_pin_s {
    ngx_queue_t                queue;
    ngx_pid_t                  pid;
    ngx_http_l1_cache_entry_t *entry;
};

/* What: Requires pointer-sized capture metadata.
 * Why: Shared metadata stores pointer-sized values.
 */
typedef char ngx_http_l1_cache_capture_pointer_fits[
    sizeof(size_t) >= sizeof(uintptr_t) ? 1 : -1];

#define L1_CAPTURE_CANCELLED  0x01
#define L1_CAPTURE_HAS_ENTRY  0x02
#define L1_CAPTURE_PURGE_PENDING  0x04
#define L1_CAPTURE_TRACK_ONLY 0x08
#define L1_CAPTURE_POST_COMMIT 0x10
#define L1_CAPTURE_CHECK_FAILED 0x20
#define L1_CAPTURE_PURGE_RETRYING 0x40
#define L1_CAPTURE_FILL_DONE 0x80

struct ngx_http_l1_cache_entry_s {
    ngx_rbtree_node_t node;
    ngx_queue_t       queue;
    ngx_uint_t        refs;
    ngx_uint_t        state;
    ngx_uint_t        protected;
    ngx_uint_t        block_count;
    ngx_uint_t        accounted;
    ngx_uint_t        indexed;
    ngx_uint_t        force;
    ngx_uint_t        status;
    ngx_atomic_t      generation;
    time_t            expires;
    time_t            created;
    ngx_msec_t        received_msec;
    size_t            key_len;
    size_t            type_len;
    size_t            etag_len;
    size_t            modified_len;
    size_t            ranges_len;
    size_t            content_range_len;
    size_t            date_len;
    size_t            server_len;
    size_t            cache_control_len;
    size_t            expires_header_len;
    size_t            encoding_len;
    size_t            extra_headers_len;
    size_t            body_len;
    size_t            block_size;
    size_t            alloc_bytes;
    time_t            initial_age;
    u_char            data[1];
};

typedef struct {
    uint32_t          version;
    size_t            config_max_object;
    size_t            config_block_size;
    size_t            config_zone_size;
    size_t            config_key_id_len;
    u_char            config_key_id[128];
    ngx_uint_t        config_ttl;
    ngx_uint_t        config_protected_percent;
    ngx_uint_t        config_building_percent;
    ngx_rbtree_t      index;
    ngx_rbtree_node_t sentinel;
    ngx_queue_t       probation;
    ngx_queue_t       protected;
    ngx_queue_t       captures;
    ngx_queue_t       pins;
    ngx_atomic_t      building;
    ngx_atomic_t      building_bytes;
    ngx_atomic_t      tracked_requests;
    ngx_atomic_t      generation;
    ngx_atomic_t      purge_generation;
    ngx_atomic_t      requests;
    ngx_atomic_t      hits;
    ngx_atomic_t      misses;
    ngx_atomic_t      bypasses;
    ngx_atomic_t      hit_bytes;
    ngx_atomic_t      evictions;
    ngx_atomic_t      expirations;
    ngx_atomic_t      accepted;
    ngx_atomic_t      rejected;
    ngx_atomic_t      allocation_failures;
    ngx_atomic_t      l2_hits;
    ngx_atomic_t      l2_misses;
    ngx_atomic_t      objects;
    ngx_atomic_t      pinned;
    ngx_atomic_t      lock_contention;
    ngx_atomic_t      origin_requests;
    size_t            used_bytes;
    size_t            protected_bytes;
    ngx_msec_t        reap_msec;
} ngx_http_l1_cache_sh_t;

typedef struct {
    ngx_shm_zone_t              *zone;
    ngx_slab_pool_t             *shpool;
    ngx_http_l1_cache_sh_t     *sh;
    ngx_str_t                    purge_token;
    size_t                       max_object;
    size_t                       block_size;
    ngx_uint_t                   ttl;
    ngx_uint_t                   protected_percent;
    ngx_uint_t                   building_percent;
    ngx_uint_t                   max_tracked_requests;
    ngx_str_t                    key_id;
    ngx_array_t                  l1_locations;
    ngx_array_t                  peer_hooks;
} ngx_http_l1_cache_main_conf_t;

typedef struct {
    ngx_flag_t enabled;
    ngx_flag_t force;
    ngx_flag_t purge_l2;
    ngx_str_t l2_zone;
    ngx_http_complex_value_t *key;
    ngx_http_handler_pt proxy_content_handler;
} ngx_http_l1_cache_loc_conf_t;

typedef struct {
    ngx_http_core_loc_conf_t *clcf;
    ngx_http_l1_cache_loc_conf_t *lcf;
} ngx_http_l1_cache_location_t;

typedef struct {
    ngx_http_request_t *r;
    ngx_str_t    key;
    ngx_str_t    proxy_key;
    ngx_str_t    content_type;
    ngx_str_t    etag;
    ngx_str_t    last_modified;
    ngx_str_t    accept_ranges;
    ngx_str_t    content_range;
    ngx_str_t    date;
    ngx_str_t    server;
    ngx_str_t    cache_control;
    ngx_str_t    expires_header;
    ngx_str_t    encoding;
    ngx_str_t    extra_headers;
    ngx_chain_t *only_if_cached_chain;
    u_char      *only_if_cached_body;
    size_t       only_if_cached_body_len;
    u_char      *body;
    size_t       len;
    size_t       reserved_bytes;
    size_t       expected_len;
    ngx_uint_t   response_status;
    time_t       expires;
    time_t       valid_sec;
    time_t       initial_age;
    time_t       response_time;
    ngx_msec_t   response_msec;
    ngx_atomic_t generation;
    ngx_atomic_t purge_generation;
    ngx_uint_t   hit:1;
    ngx_uint_t   admit:1;
    ngx_uint_t   reserved:1;
    ngx_uint_t   complete:1;
    ngx_uint_t   lookup:1;
    ngx_uint_t   l2_persisted:1;
    ngx_uint_t   l2_hit:1;
    ngx_uint_t   l2_miss:1;
    ngx_uint_t   error_fallback:1;
    ngx_uint_t   force:1;
    ngx_uint_t   slice_headers_checked:1;
    ngx_uint_t   age_update_failed:1;
    ngx_uint_t   l2_revalidated:1;
    ngx_uint_t   request_bypass:1;
    ngx_uint_t   request_no_store:1;
    ngx_uint_t   request_only_if_cached:1;
    ngx_uint_t   request_revalidate:1;
    ngx_uint_t   l2_status_seen:1;
    ngx_uint_t   invalidate_after_response:1;
    ngx_uint_t   purge_l2_after_response:1;
    ngx_uint_t   internal_l2_purge:1;
    ngx_uint_t   proxy_key_ready:1;
    ngx_uint_t   only_if_cached_peer_guard:1;
    ngx_uint_t   only_if_cached_peer_blocked:1;
    ngx_uint_t   only_if_cached_body_replaced:1;
    ngx_uint_t   capture_finalizer_deferred:1;
    ngx_uint_t   finalized_by_content_handler:1;
    ngx_uint_t   l2_purge_scheduled:1;
    ngx_uint_t   internal_l2_purge_check:1;
    ngx_uint_t   purge_check_performed:1;
    ngx_uint_t   tracked:1;
    ngx_uint_t   tracking_failed:1;
    ngx_pool_cleanup_t *capture_cleanup;
    ngx_http_l1_cache_capture_t *capture;
    ngx_http_event_handler_pt slice_resume_handler;
} ngx_http_l1_cache_ctx_t;

static ngx_http_l1_cache_main_conf_t *l1_cache_conf;
static ngx_http_output_header_filter_pt l1_cache_next_header_filter;
static ngx_http_output_body_filter_pt l1_cache_next_body_filter;
static void l1_cache_lock(ngx_shmtx_t *mtx);
static void l1_cache_invalidate_all_locked(void);

static void *l1_cache_create_main_conf(ngx_conf_t *cf);
static char *l1_cache_zone(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
static ngx_int_t l1_cache_zone_init(ngx_shm_zone_t *zone, void *data);
static ngx_int_t l1_cache_init(ngx_conf_t *cf);
static ngx_int_t l1_cache_body_init(ngx_conf_t *cf);
static ngx_int_t l1_cache_upstream_peer_init(ngx_http_request_t *r,
    ngx_http_upstream_srv_conf_t *uscf);
static ngx_flag_t l1_cache_only_if_cached_delegate(ngx_http_request_t *r,
    ngx_http_l1_cache_ctx_t *ctx);
static ngx_int_t l1_cache_add_variables(ngx_conf_t *cf);
static void l1_cache_mark_error_fallback(ngx_http_l1_cache_ctx_t *ctx);
static ngx_int_t l1_cache_stat_variable(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t data);
static ngx_int_t l1_cache_key_variable(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t data);
static ngx_flag_t l1_cache_method_unsafe(ngx_http_request_t *r);
static ngx_flag_t l1_cache_proxy_identity_matches(ngx_http_request_t *r,
    ngx_http_l1_cache_loc_conf_t *lcf);
static ngx_flag_t l1_cache_proxy_config_matches(ngx_http_request_t *r,
    ngx_http_l1_cache_loc_conf_t *lcf, ngx_flag_t *cache_bypass);
static ngx_int_t l1_cache_zone_variable(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t data);
static ngx_int_t l1_cache_request_policy_variable(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t data);
static ngx_int_t l1_cache_compose_key(ngx_http_request_t *r,
    ngx_str_t *cache_key, ngx_str_t *l1_key);
static ngx_int_t l1_cache_request_key(ngx_http_request_t *r,
    ngx_str_t *key);
static char *l1_cache_set_key(ngx_conf_t *cf, ngx_command_t *cmd, void *data);
static ngx_int_t l1_cache_add_status(ngx_http_request_t *r,
    const char *value, size_t len);
static ngx_table_elt_t *l1_cache_add_header(ngx_http_request_t *r,
    const char *name, size_t name_len, u_char *value, size_t value_len);
static ngx_int_t l1_cache_set_age_header(ngx_http_request_t *r, time_t age);
static ngx_int_t l1_cache_parse_age(ngx_str_t *value, uint64_t *age);
static ngx_int_t l1_cache_persist_initial_age(ngx_http_request_t *r,
    ngx_http_l1_cache_ctx_t *ctx);
static ngx_int_t l1_cache_update_l2_age(ngx_http_request_t *r,
    ngx_http_l1_cache_ctx_t *ctx);
static void l1_cache_update_l2_state(ngx_http_l1_cache_ctx_t *ctx);
static ngx_int_t l1_cache_restore_headers(ngx_http_request_t *r,
    u_char *data, size_t len);
static ngx_int_t l1_cache_purge_request(ngx_http_request_t *r,
    ngx_str_t *key, ngx_str_t *token);
static ngx_int_t l1_cache_purge_accepted_handler(ngx_http_request_t *r);
static ngx_int_t l1_cache_start_l2_purge(ngx_http_request_t *r,
    ngx_str_t *proxy_key, ngx_http_l1_cache_capture_t *capture);
static ngx_int_t l1_cache_start_l2_purge_check(ngx_http_request_t *r,
    ngx_str_t *proxy_key, ngx_http_l1_cache_capture_t *capture);
static ngx_int_t l1_cache_l2_purge_check_content_handler(
    ngx_http_request_t *r);
static ngx_int_t l1_cache_l2_purge_check_post_subrequest(
    ngx_http_request_t *r, void *data, ngx_int_t rc);
static ngx_int_t l1_cache_l2_purge_post_subrequest(ngx_http_request_t *r,
    void *data, ngx_int_t rc);
static void l1_cache_invalidate(ngx_str_t *key,
    ngx_http_l1_cache_capture_t *preserve, ngx_flag_t purge_l2);
static ngx_int_t l1_cache_handler(ngx_http_request_t *r);
static ngx_int_t l1_cache_handle_request(ngx_http_request_t *r,
    ngx_flag_t finalized_by_content_handler);
static ngx_int_t l1_cache_proxy_content_handler(ngx_http_request_t *r);
static ngx_int_t l1_cache_lookup(ngx_http_request_t *r, ngx_str_t *key);
static void l1_cache_slice_write_handler(ngx_http_request_t *r);
static void l1_cache_install_slice_resume(ngx_http_request_t *r);
static ngx_int_t l1_cache_header_filter(ngx_http_request_t *r);
static ngx_int_t l1_cache_body_filter(ngx_http_request_t *r, ngx_chain_t *in);
static ngx_int_t l1_cache_is_slice_subrequest(ngx_http_request_t *r);
static ngx_http_variable_value_t *l1_cache_slice_range(ngx_http_request_t *r);
static ngx_int_t l1_cache_parse_slice_range(ngx_http_request_t *r,
    off_t *start, off_t *requested_end);
static void l1_cache_rbtree_insert(ngx_rbtree_node_t *temp,
    ngx_rbtree_node_t *node, ngx_rbtree_node_t *sentinel);
static ngx_int_t l1_cache_key_compare(const u_char *a, size_t alen,
    const u_char *b, size_t blen);
static ngx_int_t l1_cache_size_add(size_t *total, size_t value);
static void l1_cache_unindex_locked(ngx_http_l1_cache_entry_t *entry);
static void l1_cache_unpin(void *data);
static void l1_cache_release_pin(ngx_http_l1_cache_pin_t *pin);
static void l1_cache_reap_pins_locked(ngx_flag_t force);
static void l1_cache_release_capture(void *data);
static void l1_cache_finalize_capture(void *data);
static ngx_int_t l1_cache_defer_capture_finalize(ngx_http_request_t *r,
    ngx_http_l1_cache_ctx_t *ctx);
static void l1_cache_store_capture(ngx_http_l1_cache_ctx_t *ctx);
static void l1_cache_copy_capture_body(ngx_http_l1_cache_entry_t *entry,
    u_char *body) __attribute__((noinline));
static void l1_cache_reap_captures_locked(void);
static u_char *l1_cache_capture_key(ngx_http_l1_cache_capture_t *capture,
    size_t *len);
static ngx_flag_t l1_cache_has_pending_l2_purge(ngx_str_t *key,
    ngx_http_l1_cache_capture_t **retry);
static void l1_cache_release_purge_retry(
    ngx_http_l1_cache_capture_t *capture);
static ngx_int_t l1_cache_track_request(ngx_http_request_t *r,
    ngx_http_l1_cache_ctx_t *ctx);
static ngx_int_t l1_cache_schedule_post_commit_check(ngx_http_request_t *r,
    ngx_http_l1_cache_ctx_t *ctx);
static void l1_cache_post_commit_check_failed(ngx_http_l1_cache_ctx_t *ctx);
static size_t l1_cache_capture_reserved_bytes(
    ngx_http_l1_cache_capture_t *capture);
static ngx_int_t l1_cache_cacheable(ngx_http_request_t *r,
    ngx_flag_t force);
static ngx_int_t l1_cache_force_header_safe(ngx_table_elt_t *h);
static ngx_int_t l1_cache_cache_control_allowed(ngx_str_t *value);
static ngx_int_t l1_cache_capture_metadata(ngx_http_request_t *r,
    ngx_http_l1_cache_ctx_t *ctx);
static ngx_int_t l1_cache_admit_response(ngx_http_request_t *r,
    ngx_http_l1_cache_ctx_t *ctx, ngx_flag_t slice);
static ngx_int_t l1_cache_request_allowed(ngx_http_request_t *r,
    ngx_flag_t slice_request, ngx_flag_t force,
    ngx_http_l1_cache_ctx_t *ctx);
static void l1_cache_request_cache_policy(ngx_http_request_t *r,
    ngx_http_l1_cache_ctx_t *ctx);
static void *l1_cache_create_loc_conf(ngx_conf_t *cf);
static char *l1_cache_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child);

static ngx_command_t l1_cache_commands[] = {
    { ngx_string("l1_cache_zone"), NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE2,
      l1_cache_zone, NGX_HTTP_MAIN_CONF_OFFSET, 0, NULL },
    { ngx_string("l1_cache_key_id"), NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_str_slot, NGX_HTTP_MAIN_CONF_OFFSET,
      offsetof(ngx_http_l1_cache_main_conf_t, key_id), NULL },
    { ngx_string("l1_cache_max_object"), NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_size_slot, NGX_HTTP_MAIN_CONF_OFFSET,
      offsetof(ngx_http_l1_cache_main_conf_t, max_object), NULL },
    { ngx_string("l1_cache_block_size"), NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_size_slot, NGX_HTTP_MAIN_CONF_OFFSET,
      offsetof(ngx_http_l1_cache_main_conf_t, block_size), NULL },
    { ngx_string("l1_cache_ttl"), NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_num_slot, NGX_HTTP_MAIN_CONF_OFFSET,
      offsetof(ngx_http_l1_cache_main_conf_t, ttl), NULL },
    { ngx_string("l1_cache_protected_percent"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE1, ngx_conf_set_num_slot,
      NGX_HTTP_MAIN_CONF_OFFSET,
      offsetof(ngx_http_l1_cache_main_conf_t, protected_percent), NULL },
    { ngx_string("l1_cache_building_percent"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE1, ngx_conf_set_num_slot,
      NGX_HTTP_MAIN_CONF_OFFSET,
      offsetof(ngx_http_l1_cache_main_conf_t, building_percent), NULL },
    { ngx_string("l1_cache_max_tracked_requests"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE1, ngx_conf_set_num_slot,
      NGX_HTTP_MAIN_CONF_OFFSET,
      offsetof(ngx_http_l1_cache_main_conf_t, max_tracked_requests), NULL },
    { ngx_string("l1_cache"), NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_flag_slot, NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_l1_cache_loc_conf_t, enabled), NULL },
    { ngx_string("l1_cache_force"), NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_flag_slot, NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_l1_cache_loc_conf_t, force), NULL },
    { ngx_string("l1_cache_l2_purge"), NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_flag_slot, NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_l1_cache_loc_conf_t, purge_l2), NULL },
    { ngx_string("l1_cache_l2_zone"), NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF
                                      |NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_str_slot, NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_l1_cache_loc_conf_t, l2_zone), NULL },
    { ngx_string("l1_cache_key"), NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF
                                   |NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      l1_cache_set_key, NGX_HTTP_LOC_CONF_OFFSET, 0, NULL },
    { ngx_string("l1_cache_purge_token"), NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_str_slot, NGX_HTTP_MAIN_CONF_OFFSET,
      offsetof(ngx_http_l1_cache_main_conf_t, purge_token), NULL },
    ngx_null_command
};

static ngx_http_module_t l1_cache_module_ctx = {
    l1_cache_add_variables, l1_cache_init,
    l1_cache_create_main_conf, NULL, NULL, NULL,
    l1_cache_create_loc_conf, l1_cache_merge_loc_conf
};

ngx_module_t ngx_http_l1_cache_module = {
    NGX_MODULE_V1, &l1_cache_module_ctx, l1_cache_commands, NGX_HTTP_MODULE,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NGX_MODULE_V1_PADDING
};

static ngx_http_module_t l1_cache_body_module_ctx = {
    NULL, l1_cache_body_init,
    NULL, NULL, NULL, NULL, NULL, NULL
};

ngx_module_t ngx_http_l1_cache_body_module = {
    NGX_MODULE_V1, &l1_cache_body_module_ctx, NULL, NGX_HTTP_MODULE,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NGX_MODULE_V1_PADDING
};

static ngx_int_t
l1_cache_add_variables(ngx_conf_t *cf)
{
    static ngx_str_t names[] = {
        ngx_string("l1_cache_requests"),
        ngx_string("l1_cache_hits"),
        ngx_string("l1_cache_misses"),
        ngx_string("l1_cache_bypasses"),
        ngx_string("l1_cache_hit_bytes"),
        ngx_string("l1_cache_evictions"),
        ngx_string("l1_cache_expirations"),
        ngx_string("l1_cache_admission_accepted"),
        ngx_string("l1_cache_admission_rejected"),
        ngx_string("l1_cache_allocation_failures"),
        ngx_string("l1_cache_used_bytes"),
        ngx_string("l1_cache_building_bytes"),
        ngx_string("l1_cache_object_count"),
        ngx_string("l1_cache_building_count"),
        ngx_string("l1_cache_l2_hits"),
        ngx_string("l1_cache_l2_misses"),
        ngx_string("l1_cache_free_bytes"),
        ngx_string("l1_cache_pinned_count"),
        ngx_string("l1_cache_lock_contention"),
        ngx_string("l1_cache_origin_requests")
    };
    static ngx_str_t key_name = ngx_string("l1_cache_key");
    static ngx_str_t zone_name = ngx_string("l1_cache_l2_zone");
    static ngx_str_t request_bypass_name =
        ngx_string("l1_cache_request_bypass");
    static ngx_str_t request_no_store_name =
        ngx_string("l1_cache_request_no_store");
    static ngx_str_t request_only_if_cached_name =
        ngx_string("l1_cache_request_only_if_cached");
    ngx_http_variable_t *var;
    ngx_uint_t i;

    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        var = ngx_http_add_variable(cf, &names[i], NGX_HTTP_VAR_NOCACHEABLE);
        if (var == NULL) return NGX_ERROR;
        var->get_handler = l1_cache_stat_variable;
        var->data = i;
    }
    var = ngx_http_add_variable(cf, &key_name, NGX_HTTP_VAR_NOCACHEABLE);
    if (var == NULL) return NGX_ERROR;
    var->get_handler = l1_cache_key_variable;
    var->data = 0;

    var = ngx_http_add_variable(cf, &zone_name, NGX_HTTP_VAR_NOCACHEABLE);
    if (var == NULL) return NGX_ERROR;
    var->get_handler = l1_cache_zone_variable;
    var->data = 0;
    var = ngx_http_add_variable(cf, &request_bypass_name,
                                NGX_HTTP_VAR_NOCACHEABLE);
    if (var == NULL) return NGX_ERROR;
    var->get_handler = l1_cache_request_policy_variable;
    var->data = 0;
    var = ngx_http_add_variable(cf, &request_no_store_name,
                                NGX_HTTP_VAR_NOCACHEABLE);
    if (var == NULL) return NGX_ERROR;
    var->get_handler = l1_cache_request_policy_variable;
    var->data = 1;
    var = ngx_http_add_variable(cf, &request_only_if_cached_name,
                                NGX_HTTP_VAR_NOCACHEABLE);
    if (var == NULL) return NGX_ERROR;
    var->get_handler = l1_cache_request_policy_variable;
    var->data = 2;
    return NGX_OK;
}


static ngx_int_t
l1_cache_request_policy_variable(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t data)
{
    static u_char yes[] = "1";
    static u_char no[] = "0";
    ngx_http_l1_cache_ctx_t *ctx;
    ngx_uint_t value;

    ctx = ngx_http_get_module_ctx(r, ngx_http_l1_cache_module);
    value = 0;
    if (ctx != NULL) {
        if (data == 0) value = ctx->request_bypass;
        if (data == 1) value = ctx->request_no_store;
        if (data == 2) value = ctx->request_only_if_cached;
    }
    v->data = value ? yes : no;
    v->len = 1;
    v->valid = 1;
    v->no_cacheable = 1;
    v->not_found = 0;
    v->escape = 0;
    return NGX_OK;
}


static ngx_int_t
l1_cache_key_source_safe(ngx_str_t *source)
{
    static u_char proxy_host[] = "proxy_host";
    static u_char proxy_port[] = "proxy_port";
    static u_char upstream[] = "upstream_";
    u_char *p, *last, *start;
    size_t len;

    p = source->data;
    last = p + source->len;

    while (p < last) {
        if (*p++ != '$') {
            continue;
        }

        if (p < last && *p == '$') {
            p++;
            continue;
        }

        if (p < last && *p == '{') {
            p++;
        }

        start = p;
        while (p < last
               && ((*p >= 'a' && *p <= 'z')
                   || (*p >= 'A' && *p <= 'Z')
                   || (*p >= '0' && *p <= '9') || *p == '_'))
        {
            p++;
        }

        len = p - start;
        if ((len == sizeof(proxy_host) - 1
             && ngx_strncasecmp(start, proxy_host, len) == 0)
            || (len == sizeof(proxy_port) - 1
                && ngx_strncasecmp(start, proxy_port, len) == 0)
            || (len >= sizeof(upstream) - 1
                && ngx_strncasecmp(start, upstream,
                                   sizeof(upstream) - 1) == 0))
        {
            return NGX_DECLINED;
        }
    }

    return NGX_OK;
}


static char *
l1_cache_set_key(ngx_conf_t *cf, ngx_command_t *cmd, void *data)
{
    ngx_http_l1_cache_loc_conf_t *lcf = data;
    ngx_http_compile_complex_value_t ccv;
    ngx_str_t *value = cf->args->elts;

    if (lcf->key != NULL) return "is duplicate";
    if (l1_cache_key_source_safe(&value[1]) != NGX_OK) {
        return "l1_cache_key cannot use proxy_host, proxy_port, or upstream variables; use request-available origin identity variables";
    }
    lcf->key = ngx_pcalloc(cf->pool, sizeof(*lcf->key));
    if (lcf->key == NULL) return NGX_CONF_ERROR;
    ngx_memzero(&ccv, sizeof(ccv));
    ccv.cf = cf;
    ccv.value = &value[1];
    ccv.complex_value = lcf->key;
    if (ngx_http_compile_complex_value(&ccv) != NGX_OK) {
        return NGX_CONF_ERROR;
    }
    return NGX_CONF_OK;
}


static ngx_int_t
l1_cache_zone_variable(ngx_http_request_t *r, ngx_http_variable_value_t *v,
    uintptr_t data)
{
    ngx_http_l1_cache_loc_conf_t *lcf;

    lcf = ngx_http_get_module_loc_conf(r, ngx_http_l1_cache_module);
    if (lcf->l2_zone.len == 0) {
        v->not_found = 1;
        return NGX_OK;
    }
    v->data = lcf->l2_zone.data;
    v->len = lcf->l2_zone.len;
    v->valid = 1;
    v->no_cacheable = 0;
    v->not_found = 0;
    v->escape = 0;
    return NGX_OK;
}


static ngx_int_t
l1_cache_key_variable(ngx_http_request_t *r, ngx_http_variable_value_t *v,
    uintptr_t data)
{
    static u_char cache_key_version[] = "\0L1-NATIVE-AGE-V2\0";
    ngx_http_l1_cache_loc_conf_t *lcf;
    ngx_http_l1_cache_ctx_t *ctx;
    ngx_list_part_t *part;
    ngx_table_elt_t *h;
    ngx_str_t expanded;
    ngx_uint_t i;
    size_t len, n;
    u_char *p;

    ctx = ngx_http_get_module_ctx(r, ngx_http_l1_cache_module);
    if (ctx != NULL && ctx->internal_l2_purge) {
        if (!ctx->proxy_key_ready) {
            v->not_found = 1;
            return NGX_OK;
        }
        v->data = ctx->proxy_key.data;
        v->len = ctx->proxy_key.len;
        v->valid = 1;
        v->no_cacheable = 1;
        v->not_found = 0;
        v->escape = 0;
        return NGX_OK;
    }

    if (ctx != NULL && ctx->proxy_key_ready) {
        v->data = ctx->proxy_key.data;
        v->len = ctx->proxy_key.len;
        v->valid = 1;
        v->no_cacheable = 1;
        v->not_found = 0;
        v->escape = 0;
        return NGX_OK;
    }

    lcf = ngx_http_get_module_loc_conf(r, ngx_http_l1_cache_module);
    if (lcf->key == NULL || lcf->l2_zone.len == 0) {
        v->not_found = 1;
        return NGX_OK;
    }
    if (ngx_http_complex_value(r, lcf->key, &expanded) != NGX_OK) {
        return NGX_ERROR;
    }

    len = expanded.len;
    if (l1_cache_size_add(&len, sizeof(cache_key_version) - 1) != NGX_OK) {
        return NGX_ERROR;
    }
    if (lcf->force) {
        static u_char marker[] = "\0L1-FORCE-KEY-V1\0";

        if (l1_cache_size_add(&len, sizeof(marker) - 1) != NGX_OK) {
            return NGX_ERROR;
        }
        part = &r->headers_in.headers.part;
        h = part->elts;
        for ( ;; ) {
            for (i = 0; i < part->nelts; i++) {
                if (h[i].hash == 0
                    || L1_HEADER(h[i], "X-L1-Cache-Purge"))
                {
                    continue;
                }
                if (l1_cache_size_add(&len, sizeof(size_t) * 2) != NGX_OK
                    || l1_cache_size_add(&len, h[i].key.len) != NGX_OK
                    || l1_cache_size_add(&len, h[i].value.len) != NGX_OK)
                {
                    return NGX_ERROR;
                }
            }
            if (part->next == NULL) break;
            part = part->next;
            h = part->elts;
        }
    }

    p = ngx_pnalloc(r->pool, len);
    if (p == NULL) return NGX_ERROR;
    if (expanded.len != 0) p = ngx_cpymem(p, expanded.data, expanded.len);
    p = ngx_cpymem(p, cache_key_version, sizeof(cache_key_version) - 1);
    if (lcf->force) {
        static u_char marker[] = "\0L1-FORCE-KEY-V1\0";

        p = ngx_cpymem(p, marker, sizeof(marker) - 1);
        part = &r->headers_in.headers.part;
        h = part->elts;
        for ( ;; ) {
            for (i = 0; i < part->nelts; i++) {
                if (h[i].hash == 0
                    || L1_HEADER(h[i], "X-L1-Cache-Purge"))
                {
                    continue;
                }
                n = h[i].key.len;
                ngx_memcpy(p, &n, sizeof(n)); p += sizeof(n);
                if (n != 0) p = ngx_cpymem(p, h[i].key.data, n);
                n = h[i].value.len;
                ngx_memcpy(p, &n, sizeof(n)); p += sizeof(n);
                if (n != 0) p = ngx_cpymem(p, h[i].value.data, n);
            }
            if (part->next == NULL) break;
            part = part->next;
            h = part->elts;
        }
    }

    v->data = p - len;
    v->len = len;
    v->valid = 1;
    v->no_cacheable = 1;
    v->not_found = 0;
    v->escape = 0;
    if (ctx != NULL && !ctx->internal_l2_purge) {
        ctx->proxy_key.data = v->data;
        ctx->proxy_key.len = v->len;
        ctx->proxy_key_ready = 1;
    }
    return NGX_OK;
}


static ngx_flag_t
l1_cache_method_unsafe(ngx_http_request_t *r)
{
    return r->method != NGX_HTTP_GET
           && r->method != NGX_HTTP_HEAD
           && r->method != NGX_HTTP_OPTIONS
           && r->method != NGX_HTTP_TRACE;
}


static ngx_flag_t
l1_cache_proxy_identity_matches(ngx_http_request_t *r,
    ngx_http_l1_cache_loc_conf_t *lcf)
{
    static u_char key_variable[] = "$l1_cache_key";
    ngx_http_proxy_loc_conf_t *plcf;
    ngx_str_t zone;

    plcf = ngx_http_get_module_loc_conf(r, ngx_http_proxy_module);
    if (plcf == NULL || plcf->upstream.cache == 0
        || plcf->cache_key.value.len != sizeof(key_variable) - 1
        || ngx_memcmp(plcf->cache_key.value.data, key_variable,
                      sizeof(key_variable) - 1) != 0)
    {
        return 0;
    }

    if (plcf->upstream.cache_value != NULL) {
        if (ngx_http_complex_value(r, plcf->upstream.cache_value, &zone)
            != NGX_OK)
        {
            return 0;
        }
    } else if (plcf->upstream.cache_zone != NULL) {
        zone = plcf->upstream.cache_zone->shm.name;
    } else {
        return 0;
    }

    if (zone.len != lcf->l2_zone.len
        || ngx_memcmp(zone.data, lcf->l2_zone.data, zone.len) != 0)
    {
        return 0;
    }

    return 1;
}


static ngx_flag_t
l1_cache_proxy_config_matches(ngx_http_request_t *r,
    ngx_http_l1_cache_loc_conf_t *lcf, ngx_flag_t *cache_bypass)
{
    ngx_http_proxy_loc_conf_t *plcf;

    *cache_bypass = 0;
    if (!l1_cache_proxy_identity_matches(r, lcf)) {
        return 0;
    }

    plcf = ngx_http_get_module_loc_conf(r, ngx_http_proxy_module);
    if (plcf->upstream.cache_bypass != NULL
        && ngx_http_test_predicates(r, plcf->upstream.cache_bypass) != NGX_OK)
    {
        *cache_bypass = 1;
        return 0;
    }

    return 1;
}


static ngx_int_t
l1_cache_compose_key(ngx_http_request_t *r, ngx_str_t *cache_key,
    ngx_str_t *l1_key)
{
    ngx_http_l1_cache_main_conf_t *mcf;
    ngx_http_l1_cache_loc_conf_t *lcf;
    size_t len, n;
    u_char *p;

    mcf = ngx_http_get_module_main_conf(r, ngx_http_l1_cache_module);
    lcf = ngx_http_get_module_loc_conf(r, ngx_http_l1_cache_module);
    len = 0;
    if (mcf == NULL || l1_cache_size_add(&len, sizeof(size_t) * 3) != NGX_OK
        || l1_cache_size_add(&len, lcf->l2_zone.len) != NGX_OK
        || l1_cache_size_add(&len, mcf->key_id.len) != NGX_OK
        || l1_cache_size_add(&len, cache_key->len) != NGX_OK)
    {
        return NGX_ERROR;
    }
    l1_key->data = ngx_pnalloc(r->pool, len);
    if (l1_key->data == NULL) return NGX_ERROR;
    p = l1_key->data;
    n = lcf->l2_zone.len;
    ngx_memcpy(p, &n, sizeof(n)); p += sizeof(n);
    if (n != 0) p = ngx_cpymem(p, lcf->l2_zone.data, n);
    n = mcf->key_id.len;
    ngx_memcpy(p, &n, sizeof(n)); p += sizeof(n);
    if (n != 0) p = ngx_cpymem(p, mcf->key_id.data, n);
    n = cache_key->len;
    ngx_memcpy(p, &n, sizeof(n)); p += sizeof(n);
    if (n != 0) p = ngx_cpymem(p, cache_key->data, n);
    l1_key->len = len;
    return NGX_OK;
}


static ngx_int_t
l1_cache_stat_variable(ngx_http_request_t *r, ngx_http_variable_value_t *v,
    uintptr_t data)
{
    ngx_atomic_t value;
    u_char *p;
    size_t budget, used, reserved;

    if (l1_cache_conf == NULL || l1_cache_conf->sh == NULL) {
        v->not_found = 1;
        return NGX_OK;
    }

    switch (data) {
    case 0: value = l1_cache_conf->sh->requests; break;
    case 1: value = l1_cache_conf->sh->hits; break;
    case 2: value = l1_cache_conf->sh->misses; break;
    case 3: value = l1_cache_conf->sh->bypasses; break;
    case 4: value = l1_cache_conf->sh->hit_bytes; break;
    case 5: value = l1_cache_conf->sh->evictions; break;
    case 6: value = l1_cache_conf->sh->expirations; break;
    case 7: value = l1_cache_conf->sh->accepted; break;
    case 8: value = l1_cache_conf->sh->rejected; break;
    case 9: value = l1_cache_conf->sh->allocation_failures; break;
    case 10:
        l1_cache_lock(&l1_cache_conf->shpool->mutex);
        value = l1_cache_conf->sh->used_bytes;
        ngx_shmtx_unlock(&l1_cache_conf->shpool->mutex);
        break;
    case 11: value = l1_cache_conf->sh->building_bytes; break;
    case 12: value = l1_cache_conf->sh->objects; break;
    case 13: value = l1_cache_conf->sh->building; break;
    case 14: value = l1_cache_conf->sh->l2_hits; break;
    case 15: value = l1_cache_conf->sh->l2_misses; break;
    case 16:
        l1_cache_lock(&l1_cache_conf->shpool->mutex);
        budget = l1_cache_conf->zone->shm.size * 80 / 100;
        used = l1_cache_conf->sh->used_bytes;
        reserved = l1_cache_conf->sh->building_bytes;
        value = used + reserved < budget ? budget - used - reserved : 0;
        ngx_shmtx_unlock(&l1_cache_conf->shpool->mutex);
        break;
    case 17: value = l1_cache_conf->sh->pinned; break;
    case 18: value = l1_cache_conf->sh->lock_contention; break;
    case 19: value = l1_cache_conf->sh->origin_requests; break;
    default: value = 0; break;
    }

    p = ngx_pnalloc(r->pool, NGX_INT64_LEN);
    if (p == NULL) return NGX_ERROR;
    v->data = p;
    v->len = ngx_sprintf(p, "%uA", (uint64_t) value) - p;
    v->valid = 1;
    v->no_cacheable = 1;
    v->not_found = 0;
    v->escape = 0;
    return NGX_OK;
}


static void
l1_cache_lock(ngx_shmtx_t *mtx)
{
    if (ngx_shmtx_trylock(mtx)) {
        return;
    }

    (void) ngx_atomic_fetch_add(&l1_cache_conf->sh->lock_contention, 1);
    ngx_shmtx_lock(mtx);
}


static ngx_int_t
l1_cache_add_status(ngx_http_request_t *r, const char *value, size_t len)
{
    ngx_table_elt_t *h;

    h = ngx_list_push(&r->headers_out.headers);
    if (h == NULL) return NGX_DECLINED;
    ngx_str_set(&h->key, "X-L1-Cache-Status");
    h->value.data = (u_char *) value;
    h->value.len = len;
    h->hash = 1;
    return NGX_OK;
}


/* What: Counts one error fallback per request.
 * Why: Fallback responses are not served from L1.
 */
static void
l1_cache_mark_error_fallback(ngx_http_l1_cache_ctx_t *ctx)
{
    if (ctx == NULL || ctx->error_fallback) {
        return;
    }

    ctx->error_fallback = 1;
    if (l1_cache_conf != NULL && l1_cache_conf->sh != NULL) {
        (void) ngx_atomic_fetch_add(&l1_cache_conf->sh->bypasses, 1);
    }
}


static ngx_table_elt_t *
l1_cache_add_header(ngx_http_request_t *r, const char *name,
    size_t name_len, u_char *value, size_t value_len)
{
    ngx_table_elt_t *h;
    u_char *copy;

    copy = ngx_pnalloc(r->pool, value_len + 1);
    if (copy == NULL) {
        return NULL;
    }
    if (value_len != 0) {
        ngx_memcpy(copy, value, value_len);
    }
    copy[value_len] = '\0';

    h = ngx_list_push(&r->headers_out.headers);
    if (h == NULL) {
        return NULL;
    }

    h->hash = 1;
    h->key.data = (u_char *) name;
    h->key.len = name_len;
    h->value.data = copy;
    h->value.len = value_len;
    return h;
}


static ngx_int_t
l1_cache_parse_age(ngx_str_t *value, uint64_t *age)
{
    u_char   *p, *last;
    uint64_t  parsed, max_age;
    ngx_uint_t digits;

    p = value->data;
    last = p + value->len;
    max_age = (uint64_t) NGX_MAX_INT_T_VALUE;
    parsed = 0;
    digits = 0;

    while (p < last && (*p == ' ' || *p == '\t')) {
        p++;
    }

    while (p < last && *p >= '0' && *p <= '9') {
        digits++;
        if (parsed > (max_age - (*p - '0')) / 10) {
            parsed = max_age;
        } else {
            parsed = parsed * 10 + (*p - '0');
        }
        p++;
    }

    while (p < last && (*p == ' ' || *p == '\t')) {
        p++;
    }

    if (digits == 0 || p != last) {
        return NGX_DECLINED;
    }

    if (age != NULL) {
        *age = parsed;
    }

    return NGX_OK;
}


static ngx_int_t
l1_cache_set_age_header(ngx_http_request_t *r, time_t age)
{
    ngx_list_part_t  *part;
    ngx_table_elt_t  *h, *age_header;
    ngx_uint_t        i, count;
    u_char           *data, *p;
    size_t            value_len;

    data = ngx_pnalloc(r->pool, NGX_TIME_T_LEN + 1);
    if (data == NULL) {
        return NGX_ERROR;
    }

    p = ngx_sprintf(data, "%T", age);
    value_len = p - data;
    age_header = NULL;
    count = 0;
    part = &r->headers_out.headers.part;
    h = part->elts;

    for ( ;; ) {
        for (i = 0; i < part->nelts; i++) {
            if (h[i].hash != 0 && L1_HEADER(h[i], "Age")) {
                age_header = &h[i];
                count++;
            }
        }

        if (part->next == NULL) {
            break;
        }

        part = part->next;
        h = part->elts;
    }

    if (count > 1) {
        return NGX_DECLINED;
    }

    if (age_header != NULL) {
        age_header->hash = 0;
    }

    if (l1_cache_add_header(r, "Age", sizeof("Age") - 1, data, value_len)
        == NULL)
    {
        return NGX_ERROR;
    }

    return NGX_OK;
}


static ngx_int_t
l1_cache_persist_initial_age(ngx_http_request_t *r,
    ngx_http_l1_cache_ctx_t *ctx)
{
    ngx_http_upstream_t  *u;
    ngx_list_part_t      *part;
    ngx_table_elt_t      *h, *age_header;
    ngx_uint_t            i, count;
    u_char               *data, *p, *raw, *raw_end;
    u_char               *line_end, *line_last;
    u_char               *colon, *raw_value, *raw_value_end, *raw_age;
    size_t                value_len, raw_age_count;

    if (ctx == NULL || !ctx->admit || ctx->l2_hit || r->upstream == NULL
        || r->cache == NULL)
    {
        return NGX_DECLINED;
    }

    u = r->upstream;
    age_header = NULL;
    count = 0;
    part = &u->headers_in.headers.part;
    h = part->elts;

    for ( ;; ) {
        for (i = 0; i < part->nelts; i++) {
            if (h[i].hash != 0 && L1_HEADER(h[i], "Age")) {
                age_header = &h[i];
                count++;
            }
        }
        if (part->next == NULL) {
            break;
        }
        part = part->next;
        h = part->elts;
    }

    if (count == 0) {
        return NGX_OK;
    }
    if (count != 1) {
        return NGX_DECLINED;
    }

    if (l1_cache_parse_age(&age_header->value, NULL) != NGX_OK) {
        return NGX_DECLINED;
    }

    data = ngx_pnalloc(r->pool, NGX_TIME_T_LEN + 1);
    if (data == NULL) {
        return NGX_ERROR;
    }
    p = ngx_sprintf(data, "%T", ctx->initial_age);
    value_len = p - data;
    if (value_len > age_header->value.len) {
        return NGX_DECLINED;
    }

    if (u->buffer.start == NULL || u->buffer.pos == NULL
        || u->buffer.pos < u->buffer.start
        || r->cache->header_start
           > (size_t) (u->buffer.pos - u->buffer.start))
    {
        return NGX_DECLINED;
    }

    /* What: Locates the original Age response-header bytes.
     * Why: The native L2 writer persists the raw response buffer.
     */
    raw = u->buffer.start + r->cache->header_start;
    raw_end = u->buffer.pos;
    line_end = ngx_strlchr(raw, raw_end, '\n');
    if (line_end == NULL) {
        return NGX_DECLINED;
    }
    raw = line_end + 1;
    raw_age = NULL;
    raw_age_count = 0;

    while (raw < raw_end) {
        line_end = ngx_strlchr(raw, raw_end, '\n');
        if (line_end == NULL) {
            return NGX_DECLINED;
        }

        line_last = line_end;
        if (line_last > raw && line_last[-1] == '\r') {
            line_last--;
        }

        if (line_last == raw) {
            break;
        }

        colon = ngx_strlchr(raw, line_last, ':');
        if (colon != NULL && colon - raw == 3
            && ngx_strncasecmp(raw, (u_char *) "age", 3) == 0)
        {
            raw_value = colon + 1;
            while (raw_value < line_last
                   && (*raw_value == ' ' || *raw_value == '\t'))
            {
                raw_value++;
            }

            raw_value_end = line_last;
            while (raw_value_end > raw_value
                   && (raw_value_end[-1] == ' '
                       || raw_value_end[-1] == '\t'))
            {
                raw_value_end--;
            }

            if ((size_t) (raw_value_end - raw_value)
                != age_header->value.len
                || ngx_memcmp(raw_value, age_header->value.data,
                              age_header->value.len) != 0)
            {
                return NGX_DECLINED;
            }

            raw_age = raw_value;
            raw_age_count++;
        }

        raw = line_end + 1;
    }

    if (raw_age_count != 1) {
        return NGX_DECLINED;
    }

    ngx_memset(raw_age, '0', age_header->value.len);
    ngx_memcpy(raw_age + age_header->value.len - value_len, data, value_len);

    return NGX_OK;
}


static ngx_int_t
l1_cache_update_l2_age(ngx_http_request_t *r,
    ngx_http_l1_cache_ctx_t *ctx)
{
    ngx_list_part_t  *part;
    ngx_table_elt_t  *h;
    ngx_uint_t        i, age_count;
    time_t            now, resident, apparent_age, age, date;
    uint64_t          age_value, max_age;

    if (ctx == NULL || r->cache == NULL || !r->cached
        || (!ctx->l2_hit
            && (r->upstream == NULL
                || r->upstream->cache_status != NGX_HTTP_CACHE_REVALIDATED)))
    {
        return NGX_OK;
    }

    now = ngx_time();
    resident = now > r->cache->date ? now - r->cache->date : 0;
    age_value = 0;
    max_age = (uint64_t) NGX_MAX_INT_T_VALUE;
    age_count = 0;
    part = &r->headers_out.headers.part;
    h = part->elts;
    for ( ;; ) {
        for (i = 0; i < part->nelts; i++) {
            if (h[i].hash != 0 && L1_HEADER(h[i], "Age")) {
                if (age_count++ != 0
                    || l1_cache_parse_age(&h[i].value, &age_value) != NGX_OK)
                {
                    ctx->age_update_failed = 1;
                    return NGX_OK;
                }
            }
        }
        if (part->next == NULL) break;
        part = part->next;
        h = part->elts;
    }

    date = r->headers_out.date != NULL
           ? ngx_parse_http_time(r->headers_out.date->value.data,
                                 r->headers_out.date->value.len)
           : NGX_ERROR;
    apparent_age = date != NGX_ERROR && date < now ? now - date : 0;
    if ((uint64_t) resident > max_age - age_value) {
        age_value = max_age;
    } else {
        age_value += (uint64_t) resident;
    }
    age = ngx_max(apparent_age, (time_t) age_value);

    if (l1_cache_set_age_header(r, age) != NGX_OK) {
        ctx->age_update_failed = 1;
        return NGX_OK;
    }

    if (ctx != NULL) {
        ctx->initial_age = age;
    }

    return NGX_OK;
}


/* What: Replays stored response headers for Force entries.
 * Why: Vary and Set-Cookie remain visible on L1 responses.
 */
static ngx_int_t
l1_cache_restore_headers(ngx_http_request_t *r, u_char *data, size_t len)
{
    uint32_t name_len, value_len;
    size_t remaining;

    remaining = len;
    while (remaining != 0) {
        if (remaining < 8) {
            return NGX_ERROR;
        }

        name_len = ((uint32_t) data[0] << 24)
                   | ((uint32_t) data[1] << 16)
                   | ((uint32_t) data[2] << 8)
                   | (uint32_t) data[3];
        value_len = ((uint32_t) data[4] << 24)
                    | ((uint32_t) data[5] << 16)
                    | ((uint32_t) data[6] << 8)
                    | (uint32_t) data[7];
        data += 8;
        remaining -= 8;

        if (name_len == 0 || name_len > remaining
            || value_len > remaining - name_len)
        {
            return NGX_ERROR;
        }

        if (l1_cache_add_header(r, (const char *) data, name_len,
                                data + name_len, value_len) == NULL)
        {
            return NGX_ERROR;
        }

        data += name_len + value_len;
        remaining -= name_len + value_len;
    }

    return NGX_OK;
}

static void *
l1_cache_create_main_conf(ngx_conf_t *cf)
{
    ngx_http_l1_cache_main_conf_t *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(*conf));
    if (conf == NULL) return NULL;
    conf->max_object = NGX_CONF_UNSET_SIZE;
    conf->block_size = NGX_CONF_UNSET_SIZE;
    conf->ttl = NGX_CONF_UNSET_UINT;
    conf->protected_percent = NGX_CONF_UNSET_UINT;
    conf->building_percent = NGX_CONF_UNSET_UINT;
    conf->max_tracked_requests = NGX_CONF_UNSET_UINT;
    if (ngx_array_init(&conf->l1_locations, cf->pool, 8,
                       sizeof(ngx_http_l1_cache_location_t)) != NGX_OK)
    {
        return NULL;
    }
    return conf;
}

static void *
l1_cache_create_loc_conf(ngx_conf_t *cf)
{
    ngx_http_l1_cache_loc_conf_t *conf;
    conf = ngx_pcalloc(cf->pool, sizeof(*conf));
    if (conf == NULL) return NULL;
    conf->enabled = NGX_CONF_UNSET;
    conf->force = NGX_CONF_UNSET;
    conf->purge_l2 = NGX_CONF_UNSET;
    return conf;
}

static char *
l1_cache_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_http_l1_cache_loc_conf_t *prev = parent;
    ngx_http_l1_cache_loc_conf_t *conf = child;
    ngx_http_l1_cache_main_conf_t *mcf;
    ngx_http_proxy_loc_conf_t *plcf;
    ngx_http_core_loc_conf_t *clcf;
    ngx_http_l1_cache_location_t *location;
    ngx_http_complex_value_t *predicate;
    ngx_uint_t i;

    mcf = ngx_http_conf_get_module_main_conf(cf,
                                             ngx_http_l1_cache_module);
    ngx_conf_merge_value(conf->enabled, prev->enabled,
                         mcf != NULL && mcf->zone != NULL);
    ngx_conf_merge_value(conf->force, prev->force, 0);
    ngx_conf_merge_value(conf->purge_l2, prev->purge_l2,
                         mcf != NULL && mcf->zone != NULL);
    if (conf->l2_zone.len == 0) conf->l2_zone = prev->l2_zone;
    if (conf->key == NULL) conf->key = prev->key;
    if (conf->proxy_content_handler == NULL) {
        conf->proxy_content_handler = prev->proxy_content_handler;
    }

    plcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_proxy_module);
    if (mcf != NULL && mcf->zone != NULL
        && conf->l2_zone.len != 0 && conf->key != NULL
        && plcf != NULL && plcf->upstream.cache
        && plcf->cache_key.value.len == sizeof("$l1_cache_key") - 1
        && ngx_memcmp(plcf->cache_key.value.data, "$l1_cache_key",
                      sizeof("$l1_cache_key") - 1) == 0)
    {
        if (!conf->purge_l2) {
            return "l1_cache_l2_purge cannot be disabled for a shared cache key";
        }
        clcf = ngx_http_conf_get_module_loc_conf(cf,
                                                  ngx_http_core_module);
        location = ngx_array_push(&mcf->l1_locations);
        if (location == NULL) {
            return NGX_CONF_ERROR;
        }
        location->clcf = clcf;
        location->lcf = conf;
    }

    if (conf->enabled) {
        if (mcf == NULL || mcf->zone == NULL) {
            return "l1_cache_zone must be configured when l1_cache is on";
        }
        if (mcf->key_id.len == 0) {
            return "l1_cache_key_id must be configured when l1_cache is on";
        }
        if (conf->l2_zone.len == 0 || conf->key == NULL) {
            return "l1_cache_l2_zone and l1_cache_key must be configured";
        }
        if (!conf->purge_l2) {
            return "l1_cache_l2_purge cannot be disabled for an L1 cache";
        }
        if (plcf != NULL && plcf->upstream.cache_bypass != NULL) {
            predicate = plcf->upstream.cache_bypass->elts;
            for (i = 0; i < plcf->upstream.cache_bypass->nelts; i++) {
                if (l1_cache_key_source_safe(&predicate[i].value) != NGX_OK) {
                    return "l1_cache requires proxy_cache_bypass variables to be available in PRECONTENT";
                }
            }
        }
        if (!conf->force) {
            if (plcf != NULL
                && (plcf->upstream.ignore_headers
                    & (NGX_HTTP_UPSTREAM_IGN_CACHE_CONTROL
                       | NGX_HTTP_UPSTREAM_IGN_EXPIRES
                       | NGX_HTTP_UPSTREAM_IGN_SET_COOKIE
                       | NGX_HTTP_UPSTREAM_IGN_VARY)))
            {
                return "normal l1_cache cannot be used with proxy_ignore_headers Cache-Control, Expires, Set-Cookie, or Vary";
            }
        }
#if !(NGX_OPENSSL)
        if (conf->force) {
            return "l1_cache_force requires NGINX built with OpenSSL";
        }
#endif
#if (NGX_THREADS)
        clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);
        if (clcf->aio != NGX_HTTP_AIO_THREADS) {
            return "l1_cache requires aio threads in the same location";
        }
#else
        return "l1_cache requires NGINX built with --with-threads";
#endif

        /* What: Wraps the proxy handler for Slice subrequests.
         * Why: Cloned requests skip PRECONTENT and need the L1 lookup.
         * From: PR #1
         */
        plcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_proxy_module);
        if (plcf != NULL && plcf->upstream.cache
            && plcf->cache_key.value.len == sizeof("$l1_cache_key") - 1
            && ngx_memcmp(plcf->cache_key.value.data, "$l1_cache_key",
                          sizeof("$l1_cache_key") - 1) == 0)
        {
            clcf = ngx_http_conf_get_module_loc_conf(cf,
                                                      ngx_http_core_module);
            if (clcf->handler == l1_cache_proxy_content_handler) {
                if (conf->proxy_content_handler == NULL) {
                    return "l1_cache cannot recover the wrapped proxy content handler";
                }
            } else if (clcf->handler != NULL) {
                conf->proxy_content_handler = clcf->handler;
                clcf->handler = l1_cache_proxy_content_handler;
            }
        }
    }

    return NGX_CONF_OK;
}

static char *
l1_cache_zone(ngx_conf_t *cf, ngx_command_t *cmd, void *data)
{
    ngx_http_l1_cache_main_conf_t *conf = data;
    ngx_str_t *value = cf->args->elts;
    ssize_t size;
    if (conf->zone != NULL) return "is duplicate";
    size = ngx_parse_size(&value[2]);
    if (size == NGX_ERROR || size < (ssize_t) (8 * ngx_pagesize))
        return "invalid shared memory zone size";
    conf->zone = ngx_shared_memory_add(cf, &value[1], size,
                                       &ngx_http_l1_cache_module);
    if (conf->zone == NULL) return NGX_CONF_ERROR;
    conf->zone->init = l1_cache_zone_init;
    conf->zone->data = conf;
    return NGX_CONF_OK;
}

static ngx_int_t
l1_cache_zone_init(ngx_shm_zone_t *zone, void *old_data)
{
    ngx_http_l1_cache_main_conf_t *conf = zone->data;
    ngx_slab_pool_t *shpool = (ngx_slab_pool_t *) zone->shm.addr;
    ngx_http_l1_cache_main_conf_t *old_conf = old_data;
    ngx_http_l1_cache_sh_t *sh;

    if (conf->key_id.len > 128) {
        ngx_log_error(NGX_LOG_EMERG, zone->shm.log, 0,
                      "l1_cache_key_id must not exceed 128 bytes");
        return NGX_ERROR;
    }

    if (old_conf != NULL) {
        if (old_conf->sh == NULL
            || old_conf->sh->version != 9)
        {
            ngx_log_error(NGX_LOG_EMERG, zone->shm.log, 0,
                          "incompatible L1 cache shared memory layout");
            return NGX_ERROR;
        }
        conf->sh = old_conf->sh;
        conf->shpool = shpool;
    } else {
        sh = ngx_slab_alloc(shpool, sizeof(*sh));
        if (sh == NULL) return NGX_ERROR;
        ngx_memzero(sh, sizeof(*sh));
        sh->version = 9;
        ngx_rbtree_init(&sh->index, &sh->sentinel, l1_cache_rbtree_insert);
        ngx_queue_init(&sh->probation);
        ngx_queue_init(&sh->protected);
        ngx_queue_init(&sh->captures);
        ngx_queue_init(&sh->pins);
        sh->building = 0;
        sh->building_bytes = 0;
        sh->tracked_requests = 0;
        sh->generation = 1;
        sh->purge_generation = 1;
        sh->requests = 0;
        sh->hits = 0;
        sh->misses = 0;
        sh->bypasses = 0;
        sh->hit_bytes = 0;
        sh->evictions = 0;
        sh->expirations = 0;
        sh->accepted = 0;
        sh->rejected = 0;
        sh->allocation_failures = 0;
        sh->l2_hits = 0;
        sh->l2_misses = 0;
        sh->objects = 0;
        sh->pinned = 0;
        sh->lock_contention = 0;
        sh->origin_requests = 0;
        sh->used_bytes = 0;
        sh->protected_bytes = 0;
        sh->reap_msec = 0;
        sh->config_max_object = conf->max_object;
        sh->config_block_size = conf->block_size;
        sh->config_zone_size = zone->shm.size;
        sh->config_key_id_len = conf->key_id.len;
        if (conf->key_id.len != 0) {
            ngx_memcpy(sh->config_key_id, conf->key_id.data,
                       conf->key_id.len);
        }
        sh->config_ttl = conf->ttl;
        sh->config_protected_percent = conf->protected_percent;
        sh->config_building_percent = conf->building_percent;
        shpool->data = sh;
        conf->sh = sh;
        conf->shpool = shpool;
    }

    /* What: Compares L1 settings after zone attachment.
     * Why: Exact settings prevent incompatible zone reuse.
     */
    l1_cache_conf = conf;
    if (conf->sh->config_max_object != conf->max_object
        || conf->sh->config_block_size != conf->block_size
        || conf->sh->config_zone_size != zone->shm.size
        || conf->sh->config_key_id_len != conf->key_id.len
        || (conf->key_id.len != 0
            && ngx_memcmp(conf->sh->config_key_id, conf->key_id.data,
                          conf->key_id.len) != 0)
        || conf->sh->config_ttl != conf->ttl
        || conf->sh->config_protected_percent != conf->protected_percent
        || conf->sh->config_building_percent != conf->building_percent)
    {
        l1_cache_lock(&conf->shpool->mutex);
        if (conf->sh->config_max_object != conf->max_object
            || conf->sh->config_block_size != conf->block_size
            || conf->sh->config_zone_size != zone->shm.size
            || conf->sh->config_key_id_len != conf->key_id.len
            || (conf->key_id.len != 0
                && ngx_memcmp(conf->sh->config_key_id, conf->key_id.data,
                              conf->key_id.len) != 0)
            || conf->sh->config_ttl != conf->ttl
            || conf->sh->config_protected_percent
               != conf->protected_percent
            || conf->sh->config_building_percent != conf->building_percent)
        {
            (void) ngx_atomic_fetch_add(&conf->sh->generation, 1);
            l1_cache_invalidate_all_locked();
            conf->sh->config_max_object = conf->max_object;
            conf->sh->config_block_size = conf->block_size;
            conf->sh->config_zone_size = zone->shm.size;
            conf->sh->config_key_id_len = conf->key_id.len;
            if (conf->key_id.len != 0) {
                ngx_memcpy(conf->sh->config_key_id, conf->key_id.data,
                           conf->key_id.len);
            }
            conf->sh->config_ttl = conf->ttl;
            conf->sh->config_protected_percent = conf->protected_percent;
            conf->sh->config_building_percent = conf->building_percent;
        }
        ngx_shmtx_unlock(&conf->shpool->mutex);
    }
    return NGX_OK;
}

static ngx_int_t
l1_cache_init(ngx_conf_t *cf)
{
    ngx_http_core_main_conf_t *cmcf;
    ngx_http_upstream_main_conf_t *umcf;
    ngx_http_upstream_srv_conf_t **uscfp;
    ngx_http_l1_cache_peer_hook_t *hook;
    ngx_http_handler_pt *handler;
    ngx_http_l1_cache_main_conf_t *conf;
    ngx_http_l1_cache_location_t *location;
    ngx_uint_t i;
    conf = ngx_http_conf_get_module_main_conf(cf, ngx_http_l1_cache_module);
    ngx_conf_init_size_value(conf->max_object, L1_MAX_OBJECT);
    ngx_conf_init_size_value(conf->block_size, L1_BLOCK_SIZE);
    ngx_conf_init_uint_value(conf->ttl, 300);
    if (conf->key_id.data == NULL) {
        ngx_str_set(&conf->key_id, "");
    }
    ngx_conf_init_uint_value(conf->protected_percent, 80);
    ngx_conf_init_uint_value(conf->building_percent, 10);
    ngx_conf_init_uint_value(conf->max_tracked_requests, 4096);
    if (conf->zone == NULL) return NGX_OK;
    location = conf->l1_locations.elts;
    for (i = 0; i < conf->l1_locations.nelts; i++) {
        if (location[i].clcf->handler != ngx_http_cache_purge_access_handler
            && !(location[i].clcf->handler == l1_cache_proxy_content_handler
                 && location[i].lcf->proxy_content_handler
                    == ngx_http_cache_purge_access_handler))
        {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "l1_cache requires same-location proxy_cache_purge");
            return NGX_ERROR;
        }
    }
    if (conf->max_object == 0 || conf->ttl == 0
        || conf->ttl > NGX_MAX_INT_T_VALUE
        || conf->block_size < ngx_pagesize
        || conf->protected_percent > L1_MAX_PROTECTED_PERCENT
        || conf->building_percent > L1_MAX_BUILDING_PERCENT
        || conf->max_tracked_requests == 0
        || conf->max_tracked_requests > NGX_MAX_INT_T_VALUE)
    {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "invalid L1 cache memory or expiry setting");
        return NGX_ERROR;
    }
    l1_cache_conf = conf;

    umcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_upstream_module);
    if (umcf != NULL && umcf->upstreams.nelts != 0) {
        if (ngx_array_init(&conf->peer_hooks, cf->pool,
                           umcf->upstreams.nelts,
                           sizeof(ngx_http_l1_cache_peer_hook_t)) != NGX_OK)
        {
            return NGX_ERROR;
        }

        uscfp = umcf->upstreams.elts;
        for (i = 0; i < umcf->upstreams.nelts; i++) {
            if (uscfp[i]->peer.init == NULL) {
                continue;
            }

            hook = ngx_array_push(&conf->peer_hooks);
            if (hook == NULL) {
                return NGX_ERROR;
            }

            hook->upstream = uscfp[i];
            hook->init = uscfp[i]->peer.init;
            uscfp[i]->peer.init = l1_cache_upstream_peer_init;
        }
    }

    cmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_core_module);
    handler = ngx_array_push(&cmcf->phases[NGX_HTTP_PRECONTENT_PHASE].handlers);
    if (handler == NULL) return NGX_ERROR;
    *handler = l1_cache_handler;
    l1_cache_next_header_filter = ngx_http_top_header_filter;
    ngx_http_top_header_filter = l1_cache_header_filter;
    return NGX_OK;
}

static ngx_int_t
l1_cache_body_init(ngx_conf_t *cf)
{
    l1_cache_next_body_filter = ngx_http_top_body_filter;
    ngx_http_top_body_filter = l1_cache_body_filter;
    return NGX_OK;
}


static ngx_int_t
l1_cache_upstream_peer_init(ngx_http_request_t *r,
    ngx_http_upstream_srv_conf_t *uscf)
{
    ngx_http_l1_cache_ctx_t *ctx;
    ngx_http_l1_cache_main_conf_t *mcf;
    ngx_http_l1_cache_peer_hook_t *hooks;
    ngx_uint_t i;

    ctx = ngx_http_get_module_ctx(r, ngx_http_l1_cache_module);
    if (ctx != NULL && ctx->only_if_cached_peer_guard) {
        /* What: Stops peer selection after an L2 miss.
         * Why: only-if-cached forbids an origin request.
         */
        ctx->only_if_cached_peer_blocked = 1;
        if (r->upstream != NULL) {
            r->upstream->cacheable = 0;
        }
        return NGX_ERROR;
    }

    mcf = ngx_http_get_module_main_conf(r, ngx_http_l1_cache_module);
    if (mcf == NULL || mcf->peer_hooks.elts == NULL) {
        return NGX_ERROR;
    }

    hooks = mcf->peer_hooks.elts;
    for (i = 0; i < mcf->peer_hooks.nelts; i++) {
        if (hooks[i].upstream == uscf) {
            return hooks[i].init(r, uscf);
        }
    }

    return NGX_ERROR;
}


static ngx_flag_t
l1_cache_only_if_cached_delegate(ngx_http_request_t *r,
    ngx_http_l1_cache_ctx_t *ctx)
{
    ngx_http_l1_cache_main_conf_t *mcf;
    ngx_http_l1_cache_peer_hook_t *hooks;
    ngx_http_proxy_loc_conf_t *plcf;
    ngx_uint_t i;

    if (ctx == NULL || !ctx->request_only_if_cached) {
        return 0;
    }

    plcf = ngx_http_get_module_loc_conf(r, ngx_http_proxy_module);
    if (plcf == NULL || !plcf->upstream.cache
        || plcf->proxy_lengths != NULL || plcf->upstream.upstream == NULL)
    {
        return 0;
    }

    if (plcf->upstream.cache_bypass != NULL
        && ngx_http_test_predicates(r, plcf->upstream.cache_bypass) != NGX_OK)
    {
        return 0;
    }

    mcf = ngx_http_get_module_main_conf(r, ngx_http_l1_cache_module);
    if (mcf == NULL || mcf->peer_hooks.elts == NULL) {
        return 0;
    }

    hooks = mcf->peer_hooks.elts;
    for (i = 0; i < mcf->peer_hooks.nelts; i++) {
        if (hooks[i].upstream == plcf->upstream.upstream) {
            return 1;
        }
    }

    return 0;
}


static ngx_int_t
l1_cache_is_slice_subrequest(ngx_http_request_t *r)
{
    return r != r->main
           && ngx_http_get_module_ctx(r, ngx_http_slice_filter_module) != NULL;
}


static ngx_http_variable_value_t *
l1_cache_slice_range(ngx_http_request_t *r)
{
    return ngx_http_get_variable(
        r, &l1_cache_slice_range_name,
        ngx_hash_key_lc(l1_cache_slice_range_name.data,
                        l1_cache_slice_range_name.len));
}

static ngx_http_l1_cache_entry_t *
l1_cache_find(ngx_str_t *key, ngx_flag_t force)
{
    ngx_int_t                 rc;
    ngx_rbtree_node_t        *node, *sentinel;
    ngx_http_l1_cache_entry_t *entry;
    ngx_rbtree_key_t          hash;

    hash = ngx_crc32_short(key->data, key->len);
    if (force) {
        hash ^= (ngx_rbtree_key_t) 0x80000000;
    }
    node = l1_cache_conf->sh->index.root;
    sentinel = l1_cache_conf->sh->index.sentinel;

    while (node != sentinel) {
        if (hash < node->key) {
            node = node->left;
            continue;
        }

        if (hash > node->key) {
            node = node->right;
            continue;
        }

        entry = ngx_rbtree_data(node, ngx_http_l1_cache_entry_t, node);
        rc = l1_cache_key_compare(key->data, key->len,
                                  entry->data, entry->key_len);
        if (rc == 0 && entry->force != (ngx_uint_t) force) {
            rc = force ? 1 : -1;
        }
        if (rc == 0) {
            return entry->state == L1_ENTRY_READY ? entry : NULL;
        }

        node = rc < 0 ? node->left : node->right;
    }

    return NULL;
}


static void
l1_cache_rbtree_insert(ngx_rbtree_node_t *temp, ngx_rbtree_node_t *node,
    ngx_rbtree_node_t *sentinel)
{
    ngx_rbtree_node_t         **p;
    ngx_http_l1_cache_entry_t  *entry, *parent;
    ngx_int_t                   rc;

    entry = ngx_rbtree_data(node, ngx_http_l1_cache_entry_t, node);

    for ( ;; ) {
        if (node->key < temp->key) {
            p = &temp->left;

        } else if (node->key > temp->key) {
            p = &temp->right;

        } else {
            parent = ngx_rbtree_data(temp, ngx_http_l1_cache_entry_t, node);
            rc = l1_cache_key_compare(entry->data, entry->key_len,
                                      parent->data, parent->key_len);
            if (rc == 0 && entry->force != parent->force) {
                rc = entry->force ? 1 : -1;
            }
            p = rc < 0
                ? &temp->left : &temp->right;
        }

        if (*p == sentinel) {
            break;
        }

        temp = *p;
    }

    *p = node;
    node->parent = temp;
    node->left = sentinel;
    node->right = sentinel;
    ngx_rbt_red(node);
}


static ngx_int_t
l1_cache_key_compare(const u_char *a, size_t alen, const u_char *b,
    size_t blen)
{
    size_t   len;
    ngx_int_t rc;

    len = ngx_min(alen, blen);
    rc = ngx_memcmp(a, b, len);
    if (rc != 0) {
        return rc;
    }

    if (alen == blen) {
        return 0;
    }

    return alen < blen ? -1 : 1;
}


static ngx_int_t
l1_cache_size_add(size_t *total, size_t value)
{
    if (value > NGX_MAX_SIZE_T_VALUE - *total) {
        return NGX_ERROR;
    }

    *total += value;
    return NGX_OK;
}


static void
l1_cache_unindex_locked(ngx_http_l1_cache_entry_t *entry)
{
    if (!entry->indexed) {
        return;
    }

    ngx_rbtree_delete(&l1_cache_conf->sh->index, &entry->node);
    entry->indexed = 0;
}


static size_t
l1_cache_blocks_offset(ngx_http_l1_cache_entry_t *entry)
{
    size_t n = entry->key_len + entry->type_len + entry->etag_len
               + entry->modified_len + entry->ranges_len
               + entry->content_range_len + entry->date_len
               + entry->server_len + entry->cache_control_len
               + entry->expires_header_len + entry->encoding_len
               + entry->extra_headers_len;
    return (n + sizeof(void *) - 1) & ~(sizeof(void *) - 1);
}


static u_char **
l1_cache_blocks(ngx_http_l1_cache_entry_t *entry)
{
    return (u_char **) (entry->data + l1_cache_blocks_offset(entry));
}


static void
l1_cache_free_entry_locked(ngx_http_l1_cache_entry_t *entry)
{
    ngx_uint_t i;
    u_char **blocks;

    l1_cache_unindex_locked(entry);
    blocks = l1_cache_blocks(entry);
    for (i = 0; i < entry->block_count; i++) {
        if (blocks[i] != NULL) {
            ngx_slab_free_locked(l1_cache_conf->shpool, blocks[i]);
        }
    }

    if (entry->accounted) {
        entry->accounted = 0;
        if (entry->protected) {
            l1_cache_conf->sh->protected_bytes -= entry->alloc_bytes;
        }
        l1_cache_conf->sh->used_bytes -= entry->alloc_bytes;
        (void) ngx_atomic_fetch_add(&l1_cache_conf->sh->objects, -1);
    }

    ngx_slab_free_locked(l1_cache_conf->shpool, entry);
}


static void
l1_cache_invalidate_all_locked(void)
{
    ngx_queue_t *queues[2], *q, *next;
    ngx_http_l1_cache_entry_t *entry;
    ngx_http_l1_cache_capture_t *capture;
    ngx_uint_t i;

    for (q = ngx_queue_head(&l1_cache_conf->sh->captures);
         q != ngx_queue_sentinel(&l1_cache_conf->sh->captures);
         q = ngx_queue_next(q))
    {
        capture = ngx_queue_data(q, ngx_http_l1_cache_capture_t, queue);
        capture->flags |= L1_CAPTURE_CANCELLED;
    }

    queues[0] = &l1_cache_conf->sh->probation;
    queues[1] = &l1_cache_conf->sh->protected;

    for (i = 0; i < 2; i++) {
        for (q = ngx_queue_head(queues[i]);
             q != ngx_queue_sentinel(queues[i]); q = next)
        {
            next = ngx_queue_next(q);
            entry = ngx_queue_data(q, ngx_http_l1_cache_entry_t, queue);
            ngx_queue_remove(q);
            l1_cache_unindex_locked(entry);
            entry->state = L1_ENTRY_EXPIRED;
            if (entry->refs == 0) {
                l1_cache_free_entry_locked(entry);
            }
        }
    }
}


static ngx_int_t
l1_cache_evict_one_locked(void)
{
    ngx_queue_t *queues[2], *q, *sentinel;
    ngx_http_l1_cache_entry_t *entry;
    ngx_uint_t i;

    queues[0] = &l1_cache_conf->sh->probation;
    queues[1] = &l1_cache_conf->sh->protected;
    for (i = 0; i < 2; i++) {
        sentinel = ngx_queue_sentinel(queues[i]);
        for (q = ngx_queue_last(queues[i]); q != sentinel;
             q = ngx_queue_prev(q))
        {
            entry = ngx_queue_data(q, ngx_http_l1_cache_entry_t, queue);
            if (entry->refs != 0) {
                continue;
            }
            ngx_queue_remove(q);
            entry->state = L1_ENTRY_EVICTING;
            l1_cache_free_entry_locked(entry);
            (void) ngx_atomic_fetch_add(&l1_cache_conf->sh->evictions, 1);
            return NGX_OK;
        }
    }
    return NGX_DECLINED;
}


static void
l1_cache_protect_locked(ngx_http_l1_cache_entry_t *entry)
{
    ngx_queue_t *q;
    ngx_http_l1_cache_entry_t *oldest;
    size_t usable, cap;

    usable = l1_cache_conf->zone->shm.size * 80 / 100;
    cap = usable * l1_cache_conf->protected_percent / 100;

    if (entry->protected) {
        ngx_queue_remove(&entry->queue);
        ngx_queue_insert_head(&l1_cache_conf->sh->protected, &entry->queue);
        return;
    }

    ngx_queue_remove(&entry->queue);
    entry->protected = 1;
    l1_cache_conf->sh->protected_bytes += entry->alloc_bytes;
    ngx_queue_insert_head(&l1_cache_conf->sh->protected, &entry->queue);

    while (l1_cache_conf->sh->protected_bytes > cap
           && !ngx_queue_empty(&l1_cache_conf->sh->protected))
    {
        q = ngx_queue_last(&l1_cache_conf->sh->protected);
        oldest = ngx_queue_data(q, ngx_http_l1_cache_entry_t, queue);
        ngx_queue_remove(q);
        oldest->protected = 0;
        l1_cache_conf->sh->protected_bytes -= oldest->alloc_bytes;
        ngx_queue_insert_head(&l1_cache_conf->sh->probation, &oldest->queue);
    }
}

static ngx_int_t
l1_cache_handler(ngx_http_request_t *r)
{
    return l1_cache_handle_request(r, 0);
}


static ngx_int_t
l1_cache_handle_request(ngx_http_request_t *r,
    ngx_flag_t finalized_by_content_handler)
{
    static u_char purge_method[] = "PURGE";
    ngx_http_l1_cache_loc_conf_t *lcf;
    ngx_http_l1_cache_ctx_t *ctx;
    ngx_http_core_loc_conf_t *clcf;
    ngx_http_variable_value_t *slice_value;
    ngx_int_t slice, rc;
    ngx_str_t key;
    ngx_flag_t cache_bypass;
    ngx_http_l1_cache_capture_t *retry_capture;

    ctx = ngx_http_get_module_ctx(r, ngx_http_l1_cache_module);
    if (ctx != NULL && ctx->internal_l2_purge_check) {
        return NGX_DECLINED;
    }
    if (ctx != NULL && ctx->internal_l2_purge) {
        clcf = ngx_http_get_module_loc_conf(r, ngx_http_core_module);
        lcf = ngx_http_get_module_loc_conf(r, ngx_http_l1_cache_module);
        if (clcf->handler != ngx_http_cache_purge_access_handler
            && !(clcf->handler == l1_cache_proxy_content_handler
                 && lcf->proxy_content_handler
                    == ngx_http_cache_purge_access_handler))
        {
            ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                          "l1 cache internal L2 purge handler is missing");
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }
        return ngx_http_proxy_cache_purge_handler(r);
    }

    lcf = ngx_http_get_module_loc_conf(r, ngx_http_l1_cache_module);

    if (l1_cache_conf == NULL || l1_cache_conf->sh == NULL) {
        return NGX_DECLINED;
    }

    ctx = ngx_pcalloc(r->pool, sizeof(*ctx));
    if (ctx == NULL) {
        return NGX_DECLINED;
    }

    ctx->r = r;
    ctx->finalized_by_content_handler = finalized_by_content_handler;
    ctx->force = lcf->enabled && lcf->force;
    /* What: Snapshots the current shared configuration generation.
     * Why: The slab mutex protects concurrent reload updates.
     */
    l1_cache_lock(&l1_cache_conf->shpool->mutex);
    ctx->generation = l1_cache_conf->sh->generation;
    ctx->purge_generation = l1_cache_conf->sh->purge_generation;
    ngx_shmtx_unlock(&l1_cache_conf->shpool->mutex);
    l1_cache_request_cache_policy(r, ctx);
    if (ctx->force) {
        /* What: Preserves only-if-cached routing in Force mode.
         * Why: Force changes admission, not request routing.
         */
        ctx->request_bypass = 0;
        ctx->request_no_store = 0;
        ctx->request_revalidate = 0;
    }
    ngx_http_set_ctx(r, ctx, ngx_http_l1_cache_module);

    /* What: Tracks unsafe requests while L1 is bypassed.
     * Why: Successful responses must invalidate the shared cache key.
     * From: PR #1
     */
    if (l1_cache_method_unsafe(r)
        && l1_cache_proxy_identity_matches(r, lcf)
        && l1_cache_request_key(r, &ctx->key) == NGX_OK)
    {
        ctx->invalidate_after_response = 1;
        ctx->purge_l2_after_response = lcf->purge_l2;
    }

    cache_bypass = 0;
    if (lcf->enabled && !l1_cache_proxy_config_matches(
                            r, lcf, &cache_bypass))
    {
        if (ctx->request_only_if_cached) {
            if ((r->method != NGX_HTTP_GET && r->method != NGX_HTTP_HEAD)
                || cache_bypass
                || !l1_cache_only_if_cached_delegate(r, ctx))
            {
                return NGX_HTTP_GATEWAY_TIME_OUT;
            }
            ctx->only_if_cached_peer_guard = 1;
        }

        if (ctx->request_revalidate
            || (cache_bypass && !ctx->request_bypass))
        {
            if (l1_cache_request_key(r, &ctx->key) == NGX_OK) {
                ctx->invalidate_after_response = 1;
            }
        }
        (void) ngx_atomic_fetch_add(&l1_cache_conf->sh->bypasses, 1);
        return NGX_DECLINED;
    }

    if (r->method_name.len == sizeof(purge_method) - 1
        && ngx_strncasecmp(r->method_name.data, purge_method,
                           sizeof(purge_method) - 1) == 0)
    {
        if (l1_cache_request_key(r, &key) != NGX_OK) {
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }
        return l1_cache_purge_request(r, &key,
                                      &l1_cache_conf->purge_token);
    }

    if (ctx->request_only_if_cached) {
        if (r->method != NGX_HTTP_GET && r->method != NGX_HTTP_HEAD) {
            return NGX_HTTP_GATEWAY_TIME_OUT;
        }
        /* What: Guards native misses only for a known static upstream.
         * Why: Dynamic targets may still have a valid L1 object.
         */
        ctx->only_if_cached_peer_guard =
            l1_cache_only_if_cached_delegate(r, ctx);
    }

    if (!lcf->enabled) {
        return ctx->request_only_if_cached
                   && !ctx->only_if_cached_peer_guard
               ? NGX_HTTP_GATEWAY_TIME_OUT : NGX_DECLINED;
    }

    (void) ngx_atomic_fetch_add(&l1_cache_conf->sh->requests, 1);

    if (r->method != NGX_HTTP_GET && r->method != NGX_HTTP_HEAD) {
        return NGX_DECLINED;
    }

    slice_value = l1_cache_slice_range(r);
    slice = slice_value != NULL && !slice_value->not_found;

    if ((r != r->main && !slice)
        || !l1_cache_request_allowed(r->main, slice, lcf->force, ctx))
    {
        (void) ngx_atomic_fetch_add(&l1_cache_conf->sh->bypasses, 1);
        if (ctx->request_only_if_cached) {
            return ctx->only_if_cached_peer_guard ? NGX_DECLINED
                                                   : NGX_HTTP_GATEWAY_TIME_OUT;
        }
        return NGX_DECLINED;
    }

    if (l1_cache_request_key(r, &ctx->key) != NGX_OK) {
        (void) ngx_atomic_fetch_add(&l1_cache_conf->sh->bypasses, 1);
        if (ctx->request_only_if_cached) {
            return ctx->only_if_cached_peer_guard ? NGX_DECLINED
                                                   : NGX_HTTP_GATEWAY_TIME_OUT;
        }
        return NGX_DECLINED;
    }
    ctx->lookup = 1;

    /* What: Avoids stale L1/L2 reuse while a purged fill is still publishing.
     * Why: The native PURGE can race an unfinished cache-file write.
     */
    retry_capture = NULL;
    if (l1_cache_has_pending_l2_purge(&ctx->key, &retry_capture)) {
        if (retry_capture != NULL) {
            if (!ctx->proxy_key_ready
                || l1_cache_start_l2_purge(r, &ctx->proxy_key,
                                           retry_capture) != NGX_OK)
            {
                l1_cache_release_purge_retry(retry_capture);
            }
        }
        ctx->request_bypass = 1;
        ctx->request_no_store = 1;
        (void) ngx_atomic_fetch_add(&l1_cache_conf->sh->bypasses, 1);
        return ctx->request_only_if_cached ? NGX_HTTP_GATEWAY_TIME_OUT
                                           : NGX_DECLINED;
    }

    rc = l1_cache_lookup(r, &ctx->key);
    if (rc == NGX_DECLINED && ctx->request_only_if_cached
        && !ctx->only_if_cached_peer_guard)
    {
        return NGX_HTTP_GATEWAY_TIME_OUT;
    }
    if (rc == NGX_DECLINED && l1_cache_track_request(r, ctx) != NGX_OK)
    {
        /* What: Bypasses both cache tiers when invalidation tracking fails.
         * Why: An untracked fill could republish an object after PURGE.
         */
        ctx->tracking_failed = 1;
        ctx->request_bypass = 1;
        ctx->request_no_store = 1;
        (void) ngx_atomic_fetch_add(&l1_cache_conf->sh->bypasses, 1);
    }
    return rc;
}


static ngx_int_t
l1_cache_proxy_content_handler(ngx_http_request_t *r)
{
    ngx_int_t rc;
    ngx_http_l1_cache_loc_conf_t *lcf;
    ngx_http_l1_cache_ctx_t *ctx;

    ctx = ngx_http_get_module_ctx(r, ngx_http_l1_cache_module);
    if (ctx != NULL && ctx->internal_l2_purge_check) {
        return l1_cache_l2_purge_check_content_handler(r);
    }
    if (ctx != NULL && ctx->internal_l2_purge) {
        return ngx_http_proxy_cache_purge_handler(r);
    }

    lcf = ngx_http_get_module_loc_conf(r, ngx_http_l1_cache_module);

    if (lcf->enabled && r != r->main && l1_cache_is_slice_subrequest(r)) {
        rc = l1_cache_handle_request(r, 1);
        if (rc != NGX_DECLINED) {
            return rc;
        }
    }

    if (lcf->proxy_content_handler == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    return lcf->proxy_content_handler(r);
}


/* What: Uses the configured native handler to purge L2.
 * Why: NGINX exposes no public file-cache purge API.
 */
static ngx_int_t
l1_cache_start_l2_purge(ngx_http_request_t *r, ngx_str_t *proxy_key,
    ngx_http_l1_cache_capture_t *capture)
{
    static ngx_str_t method = ngx_string("PURGE");
    ngx_http_request_t *sr;
    ngx_http_l1_cache_ctx_t *ctx;
    ngx_http_post_subrequest_t *ps;
    ngx_str_t uri, args;

    if (proxy_key == NULL || proxy_key->len == 0) {
        return NGX_ERROR;
    }

    ctx = ngx_pcalloc(r->pool, sizeof(*ctx));
    if (ctx == NULL) {
        return NGX_ERROR;
    }

    uri = r->uri;
    args = r->args;
    ps = NULL;
    if (capture != NULL) {
        ps = ngx_palloc(r->pool, sizeof(*ps));
        if (ps == NULL) {
            return NGX_ERROR;
        }
        ps->handler = l1_cache_l2_purge_post_subrequest;
        ps->data = capture;
    }

    if (ngx_http_subrequest(r, &uri, &args, &sr, ps,
                            NGX_HTTP_SUBREQUEST_BACKGROUND) != NGX_OK)
    {
        return NGX_ERROR;
    }

    sr->method = NGX_HTTP_UNKNOWN;
    sr->method_name = method;
    ctx->r = sr;
    ctx->proxy_key = *proxy_key;
    ctx->proxy_key_ready = 1;
    ctx->internal_l2_purge = 1;
    ngx_http_set_ctx(sr, ctx, ngx_http_l1_cache_module);

    return NGX_OK;
}


/* What: Checks for a racing PURGE after native cache finalization.
 * Why: The purge may arrive just after the output filter's first check.
 */
static ngx_int_t
l1_cache_start_l2_purge_check(ngx_http_request_t *r, ngx_str_t *proxy_key,
    ngx_http_l1_cache_capture_t *capture)
{
    static ngx_str_t method = ngx_string("PURGE");
    ngx_http_request_t *sr;
    ngx_http_l1_cache_ctx_t *ctx;
    ngx_http_post_subrequest_t *ps;
    ngx_str_t uri, args;

    if (proxy_key == NULL || proxy_key->len == 0 || capture == NULL) {
        return NGX_ERROR;
    }

    ctx = ngx_pcalloc(r->pool, sizeof(*ctx));
    ps = ngx_palloc(r->pool, sizeof(*ps));
    if (ctx == NULL || ps == NULL) {
        return NGX_ERROR;
    }

    uri = r->uri;
    args = r->args;
    ps->handler = l1_cache_l2_purge_check_post_subrequest;
    ps->data = ctx;
    if (ngx_http_subrequest(r, &uri, &args, &sr, ps,
                            NGX_HTTP_SUBREQUEST_BACKGROUND) != NGX_OK)
    {
        return NGX_ERROR;
    }

    sr->method = NGX_HTTP_UNKNOWN;
    sr->method_name = method;
    ctx->r = sr;
    ctx->proxy_key = *proxy_key;
    ctx->proxy_key_ready = 1;
    ctx->capture = capture;
    ctx->internal_l2_purge_check = 1;
    ngx_http_set_ctx(sr, ctx, ngx_http_l1_cache_module);
    return NGX_OK;
}


static ngx_int_t
l1_cache_l2_purge_check_content_handler(ngx_http_request_t *r)
{
    ngx_http_l1_cache_ctx_t *ctx;
    ngx_flag_t pending;

    ctx = ngx_http_get_module_ctx(r, ngx_http_l1_cache_module);
    if (ctx == NULL || ctx->capture == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    l1_cache_lock(&l1_cache_conf->shpool->mutex);
    ctx->capture->flags |= L1_CAPTURE_POST_COMMIT;
    pending = (ctx->capture->flags & L1_CAPTURE_PURGE_PENDING) != 0;
    ngx_shmtx_unlock(&l1_cache_conf->shpool->mutex);

    if (pending) {
        ctx->purge_check_performed = 1;
        ctx->internal_l2_purge = 1;
        return ngx_http_proxy_cache_purge_handler(r);
    }

    return ngx_http_send_response(r, NGX_HTTP_NO_CONTENT, NULL, NULL);
}


static ngx_int_t
l1_cache_l2_purge_check_post_subrequest(ngx_http_request_t *r, void *data,
    ngx_int_t rc)
{
    ngx_http_l1_cache_ctx_t *ctx = data;

    if (ctx->purge_check_performed) {
        return l1_cache_l2_purge_post_subrequest(r, ctx->capture, rc);
    }

    return NGX_OK;
}


/* What: Releases the per-key bypass after native L2 purge completion.
 * Why: Requests must not read the in-flight representation before purging.
 */
static ngx_int_t
l1_cache_l2_purge_post_subrequest(ngx_http_request_t *r, void *data,
    ngx_int_t rc)
{
    ngx_http_l1_cache_capture_t *capture = data;
    ngx_uint_t status;
    ngx_flag_t check_failed;

    status = r->headers_out.status;
    if ((status < NGX_HTTP_OK || status >= NGX_HTTP_BAD_REQUEST)
        && status != NGX_HTTP_NOT_FOUND
        && status != NGX_HTTP_PRECONDITION_FAILED)
    {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "l1 cache deferred native L2 purge failed: %i (%ui)",
                      rc, status);
        l1_cache_lock(&l1_cache_conf->shpool->mutex);
        if (capture->flags & L1_CAPTURE_PURGE_PENDING) {
            capture->flags |= L1_CAPTURE_CHECK_FAILED;
        }
        ngx_shmtx_unlock(&l1_cache_conf->shpool->mutex);
        l1_cache_release_purge_retry(capture);
        return NGX_OK;
    }

    l1_cache_lock(&l1_cache_conf->shpool->mutex);
    check_failed = (capture->flags & L1_CAPTURE_CHECK_FAILED) != 0;
    if (check_failed && (capture->flags & L1_CAPTURE_FILL_DONE)) {
        ngx_queue_remove(&capture->queue);
        (void) ngx_atomic_fetch_add(
            &l1_cache_conf->sh->tracked_requests, -1);
        ngx_slab_free_locked(l1_cache_conf->shpool, capture);
    } else if (check_failed) {
        capture->flags &= ~L1_CAPTURE_PURGE_RETRYING;
    } else {
        capture->flags &= ~L1_CAPTURE_PURGE_PENDING;
    }
    ngx_shmtx_unlock(&l1_cache_conf->shpool->mutex);

    return NGX_OK;
}


static ngx_int_t
l1_cache_request_key(ngx_http_request_t *r, ngx_str_t *key)
{
    ngx_http_variable_value_t *value;
    ngx_str_t cache_key;

    value = ngx_http_get_variable(
        r, &l1_cache_key_name,
        ngx_hash_key_lc(l1_cache_key_name.data, l1_cache_key_name.len));
    if (value == NULL || value->not_found || value->len == 0) {
        return NGX_DECLINED;
    }
    cache_key.data = value->data;
    cache_key.len = value->len;
    return l1_cache_compose_key(r, &cache_key, key);
}


static void
l1_cache_invalidate(ngx_str_t *key,
    ngx_http_l1_cache_capture_t *preserve, ngx_flag_t purge_l2)
{
    ngx_queue_t *q;
    ngx_http_l1_cache_entry_t *entry;
    ngx_http_l1_cache_capture_t *capture;
    ngx_uint_t key_force;
    size_t capture_key_len;
    u_char *capture_key;

    if (l1_cache_conf == NULL || l1_cache_conf->sh == NULL) {
        return;
    }

    l1_cache_lock(&l1_cache_conf->shpool->mutex);
    if (purge_l2) {
        (void) ngx_atomic_fetch_add(
            &l1_cache_conf->sh->purge_generation, 1);
    }

    if (key == NULL) {
        (void) ngx_atomic_fetch_add(&l1_cache_conf->sh->generation, 1);
        l1_cache_invalidate_all_locked();
        ngx_shmtx_unlock(&l1_cache_conf->shpool->mutex);
        return;
    }

    for (q = ngx_queue_head(&l1_cache_conf->sh->captures);
         q != ngx_queue_sentinel(&l1_cache_conf->sh->captures);
         q = ngx_queue_next(q))
    {
        capture = ngx_queue_data(q, ngx_http_l1_cache_capture_t, queue);
        if (capture == preserve) {
            continue;
        }
        capture_key = l1_cache_capture_key(capture, &capture_key_len);
        if (capture_key_len == key->len
            && ngx_memcmp(capture_key, key->data, key->len) == 0)
        {
            capture->flags |= L1_CAPTURE_CANCELLED;
            if (purge_l2
                && !(capture->flags & L1_CAPTURE_POST_COMMIT))
            {
                capture->flags |= L1_CAPTURE_PURGE_PENDING;
            }
        }
    }

    for (key_force = 0; key_force < 2; key_force++) {
        entry = l1_cache_find(key, key_force);
        if (entry != NULL) {
            ngx_queue_remove(&entry->queue);
            l1_cache_unindex_locked(entry);
            entry->state = L1_ENTRY_EXPIRED;
            if (entry->refs == 0) {
                l1_cache_free_entry_locked(entry);
            }
        }
    }

    ngx_shmtx_unlock(&l1_cache_conf->shpool->mutex);
}


static ngx_int_t
l1_cache_lookup(ngx_http_request_t *r, ngx_str_t *key)
{
    ngx_http_l1_cache_entry_t *entry;
    ngx_http_l1_cache_pin_t *pin;
    ngx_pool_cleanup_t *cleanup;
    ngx_http_l1_cache_ctx_t *ctx;
    ngx_table_elt_t *status;
    ngx_table_elt_t *etag_header, *modified_header, *ranges_header;
    ngx_table_elt_t *content_range_header, *date_header, *server_header;
    ngx_table_elt_t *control_header, *expires_field, *encoding_header;
    ngx_buf_t *b;
    ngx_chain_t *out, *cl, **last;
    ngx_list_part_t *saved_last;
    u_char *type, *etag, *modified, *ranges, *content_range;
    u_char *date, *server, *cache_control, *expires_data, *encoding;
    u_char *extra_headers;
    u_char **blocks, *age_data, *p;
    size_t offset, n;
    ngx_uint_t saved_nelts;
    size_t age_len;
    uint64_t age, resident;
    ngx_msec_t elapsed;
    time_t now;
    ngx_uint_t i;
    ngx_int_t slice, rc;
    off_t slice_start = 0, slice_end = 0;
    ngx_http_variable_value_t *slice_value;

    ctx = ngx_http_get_module_ctx(r, ngx_http_l1_cache_module);
    if (ctx == NULL || !ctx->lookup) {
        return NGX_DECLINED;
    }

    slice_value = l1_cache_slice_range(r);
    slice = slice_value != NULL && !slice_value->not_found;

    if ((r != r->main && !l1_cache_is_slice_subrequest(r))
        || !l1_cache_request_allowed(r, slice, ctx->force, ctx))
    {
        (void) ngx_atomic_fetch_add(&l1_cache_conf->sh->bypasses, 1);
        return NGX_DECLINED;
    }

    l1_cache_lock(&l1_cache_conf->shpool->mutex);
    l1_cache_reap_pins_locked(0);
    entry = l1_cache_find(key, ctx->force);
    if (entry == NULL) {
        ngx_shmtx_unlock(&l1_cache_conf->shpool->mutex);
        (void) ngx_atomic_fetch_add(&l1_cache_conf->sh->misses, 1);
        return NGX_DECLINED;
    }
    if (entry->generation != ctx->generation) {
        /* What: Rejects entries from another generation.
         * Why: Old workers may serve requests during reload.
         */
        if (entry->generation != l1_cache_conf->sh->generation) {
            ngx_queue_remove(&entry->queue);
            l1_cache_unindex_locked(entry);
            entry->state = L1_ENTRY_EXPIRED;
            if (entry->refs == 0) {
                l1_cache_free_entry_locked(entry);
            }
        }
        ngx_shmtx_unlock(&l1_cache_conf->shpool->mutex);
        (void) ngx_atomic_fetch_add(&l1_cache_conf->sh->misses, 1);
        return NGX_DECLINED;
    }
    if (entry->body_len > l1_cache_conf->max_object) {
        ngx_queue_remove(&entry->queue);
        l1_cache_unindex_locked(entry);
        entry->state = L1_ENTRY_EXPIRED;
        (void) ngx_atomic_fetch_add(&l1_cache_conf->sh->rejected, 1);
        if (entry->refs == 0) {
            l1_cache_free_entry_locked(entry);
        }
        ngx_shmtx_unlock(&l1_cache_conf->shpool->mutex);
        (void) ngx_atomic_fetch_add(&l1_cache_conf->sh->misses, 1);
        return NGX_DECLINED;
    }
    now = ngx_time();
    if (entry->expires <= now || now < entry->created
        || (ngx_uint_t) (now - entry->created) >= l1_cache_conf->ttl)
    {
        ngx_queue_remove(&entry->queue);
        l1_cache_unindex_locked(entry);
        entry->state = L1_ENTRY_EXPIRED;
        (void) ngx_atomic_fetch_add(&l1_cache_conf->sh->expirations, 1);
        if (entry->refs == 0) {
            l1_cache_free_entry_locked(entry);
        }
        ngx_shmtx_unlock(&l1_cache_conf->shpool->mutex);
        (void) ngx_atomic_fetch_add(&l1_cache_conf->sh->misses, 1);
        return NGX_DECLINED;
    }
    pin = ngx_slab_alloc_locked(l1_cache_conf->shpool, sizeof(*pin));
    if (pin == NULL) {
        ngx_shmtx_unlock(&l1_cache_conf->shpool->mutex);
        (void) ngx_atomic_fetch_add(
            &l1_cache_conf->sh->allocation_failures, 1);
        (void) ngx_atomic_fetch_add(&l1_cache_conf->sh->misses, 1);
        return NGX_DECLINED;
    }

    pin->pid = ngx_pid;
    pin->entry = entry;
    ngx_queue_insert_head(&l1_cache_conf->sh->pins, &pin->queue);

    entry->refs++;
    if (entry->refs == 1) {
        (void) ngx_atomic_fetch_add(&l1_cache_conf->sh->pinned, 1);
    }
    type = entry->data + entry->key_len;
    etag = type + entry->type_len;
    modified = etag + entry->etag_len;
    ranges = modified + entry->modified_len;
    content_range = ranges + entry->ranges_len;
    date = content_range + entry->content_range_len;
    server = date + entry->date_len;
    cache_control = server + entry->server_len;
    expires_data = cache_control + entry->cache_control_len;
    encoding = expires_data + entry->expires_header_len;
    extra_headers = encoding + entry->encoding_len;
    blocks = l1_cache_blocks(entry);
    ngx_shmtx_unlock(&l1_cache_conf->shpool->mutex);

    age = entry->initial_age;
    elapsed = ngx_current_msec - entry->received_msec;
    resident = elapsed / 1000;
    if (elapsed % 1000 != 0) {
        resident++;
    }
    if (resident <= UINT64_MAX - age) {
        age += resident;
    } else {
        age = UINT64_MAX;
    }

    if (slice && l1_cache_parse_slice_range(r, &slice_start, &slice_end)
                 != NGX_OK)
    {
        goto fallback;
    }

    age_data = ngx_pnalloc(r->pool, NGX_TIME_T_LEN + 1);
    if (age_data == NULL) {
        goto fallback;
    }
    p = ngx_sprintf(age_data, "%uL", age);
    age_len = p - age_data;

    out = NULL;
    last = &out;

    if (r->method != NGX_HTTP_HEAD) {
        offset = 0;
        for (i = 0; i < entry->block_count; i++) {
            n = ngx_min(entry->block_size, entry->body_len - offset);
            cl = ngx_alloc_chain_link(r->pool);
            b = ngx_calloc_buf(r->pool);
            if (cl == NULL || b == NULL) {
                goto fallback;
            }
            b->pos = blocks[i];
            b->last = blocks[i] + n;
            b->start = b->pos;
            b->end = b->last;
            b->memory = 1;
            b->last_in_chain = (i + 1 == entry->block_count);
            b->last_buf = (i + 1 == entry->block_count && r == r->main);
            cl->buf = b;
            cl->next = NULL;
            *last = cl;
            last = &cl->next;
            offset += n;
        }
    }

    cleanup = ngx_pool_cleanup_add(r->pool, sizeof(pin));
    if (cleanup == NULL) {
        goto fallback;
    }
    *(ngx_http_l1_cache_pin_t **) cleanup->data = pin;
    cleanup->handler = l1_cache_unpin;

    saved_last = r->headers_out.headers.last;
    saved_nelts = saved_last->nelts;

    status = l1_cache_add_header(r, "X-L1-Cache",
                                 sizeof("X-L1-Cache") - 1,
                                 (u_char *) "HIT", sizeof("HIT") - 1);
    if (status == NULL
        || l1_cache_add_status(r, "L1_HIT", sizeof("L1_HIT") - 1) != NGX_OK)
    {
        goto rollback;
    }

    etag_header = NULL;
    modified_header = NULL;
    ranges_header = NULL;
    content_range_header = NULL;
    date_header = NULL;
    server_header = NULL;
    control_header = NULL;
    expires_field = NULL;
    encoding_header = NULL;

    if (entry->etag_len) {
        etag_header = l1_cache_add_header(r, "ETag", sizeof("ETag") - 1,
                                          etag, entry->etag_len);
        if (etag_header == NULL) goto rollback;
    }
    if (entry->modified_len) {
        modified_header = l1_cache_add_header(r, "Last-Modified",
            sizeof("Last-Modified") - 1, modified, entry->modified_len);
        if (modified_header == NULL) goto rollback;
    }
    if (entry->ranges_len) {
        ranges_header = l1_cache_add_header(r, "Accept-Ranges",
            sizeof("Accept-Ranges") - 1, ranges, entry->ranges_len);
        if (ranges_header == NULL) goto rollback;
    }
    if (entry->content_range_len) {
        content_range_header = l1_cache_add_header(r, "Content-Range",
            sizeof("Content-Range") - 1, content_range,
            entry->content_range_len);
        if (content_range_header == NULL) goto rollback;
    }
    if (entry->date_len) {
        date_header = l1_cache_add_header(r, "Date", sizeof("Date") - 1,
                                          date, entry->date_len);
        if (date_header == NULL) goto rollback;
    }
    if (entry->server_len) {
        server_header = l1_cache_add_header(r, "Server",
            sizeof("Server") - 1, server, entry->server_len);
        if (server_header == NULL) goto rollback;
    }
    if (entry->cache_control_len) {
        control_header = l1_cache_add_header(r, "Cache-Control",
            sizeof("Cache-Control") - 1, cache_control,
            entry->cache_control_len);
        if (control_header == NULL) goto rollback;
    }
    if (entry->expires_header_len) {
        expires_field = l1_cache_add_header(r, "Expires",
            sizeof("Expires") - 1, expires_data,
            entry->expires_header_len);
        if (expires_field == NULL) goto rollback;
    }
    if (entry->encoding_len) {
        encoding_header = l1_cache_add_header(r, "Content-Encoding",
            sizeof("Content-Encoding") - 1, encoding, entry->encoding_len);
        if (encoding_header == NULL) goto rollback;
    }
    if (entry->extra_headers_len
        && l1_cache_restore_headers(r, extra_headers,
                                    entry->extra_headers_len) != NGX_OK)
    {
        goto rollback;
    }
    status = l1_cache_add_header(r, "Age", sizeof("Age") - 1,
                                 age_data, age_len);
    if (status == NULL) goto rollback;

    type = ngx_pnalloc(r->pool, entry->type_len + 1);
    if (type == NULL) {
        goto rollback;
    }
    ngx_memcpy(type, entry->data + entry->key_len, entry->type_len);
    type[entry->type_len] = '\0';

    /* What: Promotes after L1 response setup succeeds.
     * Why: Failed setup must not protect an entry.
     */
    l1_cache_lock(&l1_cache_conf->shpool->mutex);
    if (entry->state == L1_ENTRY_READY && entry->indexed) {
        l1_cache_protect_locked(entry);
    }
    ngx_shmtx_unlock(&l1_cache_conf->shpool->mutex);

    /* What: Counts a hit after L1 response setup succeeds.
     * Why: Failed setup falls back to native NGINX.
     */
    (void) ngx_atomic_fetch_add(&l1_cache_conf->sh->hits, 1);
    if (r->method != NGX_HTTP_HEAD) {
        (void) ngx_atomic_fetch_add(&l1_cache_conf->sh->hit_bytes,
                                    entry->body_len);
    }

    ctx->hit = 1;
    r->headers_out.status = entry->status;
    r->headers_out.content_length_n = entry->body_len;
    r->headers_out.content_type.data = type;
    r->headers_out.content_type.len = entry->type_len;
    r->headers_out.content_type_len = entry->type_len;
    r->headers_out.etag = etag_header;
    r->headers_out.last_modified = modified_header;
    r->headers_out.accept_ranges = ranges_header;
    r->headers_out.content_range = content_range_header;
    r->headers_out.date = date_header;
    r->headers_out.server = server_header;
    r->headers_out.cache_control = control_header;
    r->headers_out.expires = expires_field;
    r->headers_out.content_encoding = encoding_header;
    if (slice) {
        r->headers_out.content_offset = slice_start;
    }

    if (r->method == NGX_HTTP_HEAD) {
        rc = ngx_http_send_header(r);
        if (ctx->finalized_by_content_handler) {
            return rc;
        }
        ngx_http_finalize_request(r, rc);
        return NGX_DONE;
    }
    rc = ngx_http_send_header(r);
    if (rc == NGX_ERROR) {
        if (ctx->finalized_by_content_handler) {
            return rc;
        }
        ngx_http_finalize_request(r, rc);
        return NGX_DONE;
    }
    rc = ngx_http_output_filter(r, out);
    if (slice && rc != NGX_ERROR) {
        /* What: Marks an L1 slice child as waited before finalization.
         * Why: Slice checks sr->done before scheduling its next child.
         * From: PR #1
         */
        r->waited = 1;
    }
    if (ctx->finalized_by_content_handler) {
        if (slice && rc != NGX_ERROR) {
            l1_cache_install_slice_resume(r->parent);
        }
        return rc;
    }
    ngx_http_finalize_request(r, rc);

    if (slice && rc != NGX_ERROR) {
        l1_cache_install_slice_resume(r == r->main ? r : r->parent);
    }

    return NGX_DONE;

rollback:
    saved_last->next = NULL;
    saved_last->nelts = saved_nelts;
    r->headers_out.headers.last = saved_last;
    cleanup->handler = NULL;

fallback:
    l1_cache_release_pin(pin);
    l1_cache_mark_error_fallback(ctx);
    return NGX_DECLINED;
}


static void
l1_cache_install_slice_resume(ngx_http_request_t *r)
{
    ngx_http_l1_cache_ctx_t *ctx;

    if (r == NULL || r->done
        || ngx_http_get_module_ctx(r, ngx_http_slice_filter_module) == NULL)
    {
        return;
    }

    ctx = ngx_http_get_module_ctx(r, ngx_http_l1_cache_module);
    if (ctx == NULL || r->write_event_handler == NULL
        || r->write_event_handler == l1_cache_slice_write_handler)
    {
        return;
    }

    /* What: Resumes the parent after its L1 slice child completes.
     * Why: Buffered output may leave the parent without another wakeup.
     * From: PR #1
     */
    ctx->slice_resume_handler = r->write_event_handler;
    r->write_event_handler = l1_cache_slice_write_handler;
}


static void
l1_cache_slice_write_handler(ngx_http_request_t *r)
{
    ngx_int_t rc;
    ngx_http_l1_cache_ctx_t *ctx;
    ngx_http_event_handler_pt writer;

    ctx = ngx_http_get_module_ctx(r, ngx_http_l1_cache_module);
    if (ctx == NULL || ctx->slice_resume_handler == NULL)
    {
        ngx_http_finalize_request(r, NGX_ERROR);
        return;
    }

    writer = ctx->slice_resume_handler;
    ctx->slice_resume_handler = NULL;
    r->write_event_handler = writer;

    rc = ngx_http_output_filter(r, NULL);
    if (rc == NGX_ERROR) {
        ngx_http_finalize_request(r, rc);
        return;
    }

    /* What: Skips the native writer when Slice posted a child.
     * Why: A second pass can dequeue it before its response completes.
     * From: PR #1
     */
    if (!r->done && r->postponed == NULL
        && r->write_event_handler == writer)
    {
        /* What: Preserves the callback across native writer execution.
         * Why: The writer may finalize and free this request.
         * From: PR #1
         */
        ctx->slice_resume_handler = writer;
        r->write_event_handler = l1_cache_slice_write_handler;
        writer(r);
        return;
    }

    if (!r->done && r->write_event_handler == writer) {
        ctx->slice_resume_handler = writer;
        r->write_event_handler = l1_cache_slice_write_handler;
    }
}

static ngx_int_t
l1_cache_request_allowed(ngx_http_request_t *r, ngx_flag_t slice_request,
    ngx_flag_t force, ngx_http_l1_cache_ctx_t *ctx)
{
    ngx_list_part_t *part = &r->headers_in.headers.part;
    ngx_table_elt_t *h = part->elts;
    ngx_uint_t i;
    if ((r->method != NGX_HTTP_GET && r->method != NGX_HTTP_HEAD)
        || r->headers_in.host == NULL
        || r->headers_in.content_length_n > 0 || r->headers_in.chunked
        || (!force && (r->headers_in.authorization
                       || r->headers_in.cookie
                       || r->headers_in.proxy_authorization))
        || (r->headers_in.range && !slice_request) || r->headers_in.if_range
        || r->headers_in.if_none_match || r->headers_in.if_modified_since
        || r->headers_in.if_match || r->headers_in.if_unmodified_since)
        return 0;
    for (;;) {
        for (i = 0; i < part->nelts; i++) {
            if (!h[i].hash) continue;
            if (((!force && (L1_HEADER(h[i], "Cache-Control")
                             || L1_HEADER(h[i], "Pragma")))
                 && !(ctx != NULL && ctx->request_only_if_cached
                      && !ctx->request_bypass))
                || (!force && (L1_HEADER(h[i], "Proxy-Authorization")
                               || L1_HEADER(h[i], "Cookie2")))
                || L1_HEADER(h[i], "X-L1-Cache-Purge"))
                return 0;
        }
        if (part->next == NULL) break;
        part = part->next; h = part->elts;
    }
    return 1;
}


/* What: Passes request policy to native L2.
 * Why: Native proxy cache ignores request directives.
 */
static void
l1_cache_request_cache_policy(ngx_http_request_t *r,
    ngx_http_l1_cache_ctx_t *ctx)
{
    ngx_list_part_t  *part;
    ngx_table_elt_t  *h;
    ngx_uint_t        i;
    u_char           *p, *last, *start, *end, *name_end;
    ngx_uint_t        quoted, escaped, only_if_cached, only_if_cached_only;
    ngx_uint_t        other_bypass;
    size_t            len;

    part = &r->headers_in.headers.part;
    h = part->elts;
    only_if_cached = 0;
    only_if_cached_only = 1;
    other_bypass = 0;

    if (r->headers_in.authorization != NULL
        || r->headers_in.proxy_authorization != NULL
        || r->headers_in.cookie != NULL)
    {
        ctx->request_bypass = 1;
        ctx->request_no_store = 1;
        other_bypass = 1;
    }

    for ( ;; ) {
        for (i = 0; i < part->nelts; i++) {
            if (!h[i].hash) {
                continue;
            }

            if (L1_HEADER(h[i], "Pragma")) {
                ctx->request_bypass = 1;
                ctx->request_revalidate = 1;
                other_bypass = 1;
                continue;
            }

            if (L1_HEADER(h[i], "Cookie2")) {
                ctx->request_bypass = 1;
                ctx->request_no_store = 1;
                other_bypass = 1;
                continue;
            }

            if (!L1_HEADER(h[i], "Cache-Control")) {
                continue;
            }

            ctx->request_bypass = 1;
            ctx->request_revalidate = 1;
            p = h[i].value.data;
            last = p + h[i].value.len;
            start = p;
            quoted = 0;
            escaped = 0;

            for ( ;; ) {
                if (p == last || (!quoted && *p == ',')) {
                    end = p;
                    while (start < end && (*start == ' ' || *start == '\t')) {
                        start++;
                    }
                    while (end > start
                           && (end[-1] == ' ' || end[-1] == '\t'))
                    {
                        end--;
                    }

                    name_end = start;
                    while (name_end < end && *name_end != '=') {
                        name_end++;
                    }
                    while (name_end > start
                           && (name_end[-1] == ' ' || name_end[-1] == '\t'))
                    {
                        name_end--;
                    }
                    len = name_end - start;
                    if (len == sizeof("only-if-cached") - 1
                        && ngx_strncasecmp(start,
                                           (u_char *) "only-if-cached", len)
                           == 0)
                    {
                        only_if_cached = 1;
                        if (ngx_strlchr(start, end, '=') != NULL) {
                            only_if_cached_only = 0;
                        }
                    } else {
                        only_if_cached_only = 0;
                    }

                    if (len == sizeof("no-store") - 1
                        && ngx_strncasecmp(start, (u_char *) "no-store", len)
                           == 0)
                    {
                        ctx->request_no_store = 1;
                    }

                    if (p == last) {
                        break;
                    }
                    p++;
                    start = p;
                    continue;
                }

                if (escaped) {
                    escaped = 0;
                } else if (quoted && *p == '\\') {
                    escaped = 1;
                } else if (*p == '"') {
                    quoted = !quoted;
                }
                p++;
            }
        }

        if (part->next == NULL) {
            break;
        }
        part = part->next;
        h = part->elts;
    }

    if (only_if_cached) {
        ctx->request_only_if_cached = 1;

        if (only_if_cached_only && !other_bypass
            && !ctx->request_no_store)
        {
            ctx->request_bypass = 0;
        }
    }

}

static ngx_int_t
l1_cache_purge_request(ngx_http_request_t *r, ngx_str_t *key,
    ngx_str_t *token)
{
    static u_char purge_header[] = "X-L1-Cache-Purge";
    ngx_list_part_t *part;
    ngx_table_elt_t *h;
    ngx_uint_t i, j, authorized, diff, key_force;
    ngx_queue_t *queues[2], *q, *next;
    ngx_http_l1_cache_entry_t *entry;
    ngx_http_l1_cache_capture_t *capture;
    size_t capture_key_len;
    u_char *capture_key;
    ngx_flag_t l2_pending;
    ngx_flag_t full_zone;

    if (l1_cache_conf == NULL || l1_cache_conf->sh == NULL)
        return NGX_HTTP_SERVICE_UNAVAILABLE;
    if (token->len < 32)
        return NGX_HTTP_NOT_ALLOWED;

    authorized = 0;
    part = &r->headers_in.headers.part;
    h = part->elts;
    for (;;) {
        for (i = 0; i < part->nelts; i++) {
            if (h[i].key.len != sizeof(purge_header) - 1
                || ngx_strncasecmp(h[i].key.data, purge_header,
                                   sizeof(purge_header) - 1) != 0)
                continue;
            if (h[i].value.len != token->len) continue;
            diff = 0;
            for (j = 0; j < h[i].value.len; j++) {
                diff |= h[i].value.data[j] ^ token->data[j];
            }
            if (diff == 0) authorized = 1;
        }
        if (part->next == NULL) break;
        part = part->next;
        h = part->elts;
    }
    if (!authorized) return NGX_HTTP_FORBIDDEN;

    l2_pending = 0;
    full_zone = r->uri.len == 1 && r->uri.data[0] == '/' && r->args.len == 0;
    l1_cache_lock(&l1_cache_conf->shpool->mutex);
    if (full_zone) {
        (void) ngx_atomic_fetch_add(&l1_cache_conf->sh->generation, 1);
        (void) ngx_atomic_fetch_add(
            &l1_cache_conf->sh->purge_generation, 1);
        for (q = ngx_queue_head(&l1_cache_conf->sh->captures);
             q != ngx_queue_sentinel(&l1_cache_conf->sh->captures);
             q = ngx_queue_next(q))
        {
            capture = ngx_queue_data(q, ngx_http_l1_cache_capture_t, queue);
            capture->flags |= L1_CAPTURE_CANCELLED;
            if (!(capture->flags & L1_CAPTURE_POST_COMMIT)) {
                capture->flags |= L1_CAPTURE_PURGE_PENDING;
                l2_pending = 1;
            }
        }
        queues[0] = &l1_cache_conf->sh->probation;
        queues[1] = &l1_cache_conf->sh->protected;
        for (i = 0; i < 2; i++) {
            for (q = ngx_queue_head(queues[i]);
                 q != ngx_queue_sentinel(queues[i]); q = next)
            {
                next = ngx_queue_next(q);
                entry = ngx_queue_data(q, ngx_http_l1_cache_entry_t, queue);
                ngx_queue_remove(q);
                l1_cache_unindex_locked(entry);
                entry->state = L1_ENTRY_EXPIRED;
                if (entry->refs == 0) l1_cache_free_entry_locked(entry);
            }
        }
    } else if (key != NULL) {
        (void) ngx_atomic_fetch_add(
            &l1_cache_conf->sh->purge_generation, 1);
        for (q = ngx_queue_head(&l1_cache_conf->sh->captures);
             q != ngx_queue_sentinel(&l1_cache_conf->sh->captures);
             q = ngx_queue_next(q))
        {
            capture = ngx_queue_data(q, ngx_http_l1_cache_capture_t, queue);
            capture_key = l1_cache_capture_key(capture, &capture_key_len);
            if (capture_key_len == key->len
                && ngx_memcmp(capture_key, key->data, key->len) == 0)
            {
                capture->flags |= L1_CAPTURE_CANCELLED;
                if (!(capture->flags & L1_CAPTURE_POST_COMMIT)) {
                    capture->flags |= L1_CAPTURE_PURGE_PENDING;
                    l2_pending = 1;
                }
            }
        }

        for (key_force = 0; key_force < 2; key_force++) {
            entry = l1_cache_find(key, key_force);
            if (entry != NULL) {
                ngx_queue_remove(&entry->queue);
                l1_cache_unindex_locked(entry);
                entry->state = L1_ENTRY_EXPIRED;
                if (entry->refs == 0) l1_cache_free_entry_locked(entry);
            }
        }
    }
    ngx_shmtx_unlock(&l1_cache_conf->shpool->mutex);

    if (l2_pending && !full_zone) {
        /* What: Responds from the content phase with HTTP 202.
         * Why: Phase handlers cannot finalize a 2xx status as a response.
         */
        r->content_handler = l1_cache_purge_accepted_handler;
        return NGX_DECLINED;
    }

    /* What: Continues into the configured native L2 purge handler.
     * Why: Proxy cache exposes no public module purge API.
     */
    return NGX_DECLINED;
}


/* What: Reports that a fill-racing purge has been accepted for completion.
 * Why: The active fill's post-commit native purge runs asynchronously.
 */
static ngx_int_t
l1_cache_purge_accepted_handler(ngx_http_request_t *r)
{
    static u_char body[] = "Purge accepted; active fill will be purged.\n";
    ngx_str_t content_type;
    ngx_http_complex_value_t response;

    ngx_str_set(&content_type, "text/plain");
    ngx_memzero(&response, sizeof(response));
    response.value.data = body;
    response.value.len = sizeof(body) - 1;

    return ngx_http_send_response(r, NGX_HTTP_ACCEPTED, &content_type,
                                  &response);
}


static ngx_int_t
l1_cache_parse_slice_range(ngx_http_request_t *r, off_t *start,
    off_t *requested_end)
{
    ngx_http_variable_value_t *v;
    u_char *dash, *slash;

    v = l1_cache_slice_range(r);
    if (v == NULL || v->not_found || v->len < sizeof("bytes=0-0") - 1
        || ngx_strncmp(v->data, "bytes=", 6) != 0)
        return NGX_ERROR;

    dash = ngx_strlchr(v->data + 6, v->data + v->len, '-');
    if (dash == NULL || dash == v->data + 6
        || dash + 1 == v->data + v->len)
        return NGX_ERROR;
    slash = ngx_strlchr(dash + 1, v->data + v->len, '/');
    if (slash != NULL) return NGX_ERROR;

    *start = ngx_atoof(v->data + 6, dash - (v->data + 6));
    *requested_end = ngx_atoof(dash + 1, v->data + v->len - (dash + 1));
    if (*start < 0 || *requested_end < *start) return NGX_ERROR;
    return NGX_OK;
}


static ngx_int_t
l1_cache_slice_segment(ngx_http_request_t *r, off_t *start, off_t *end,
    off_t *complete)
{
    off_t requested_start, requested_end;
    ngx_table_elt_t *range;
    u_char *p, *last, *dash, *slash;

    if (l1_cache_parse_slice_range(r, &requested_start, &requested_end)
        != NGX_OK || r->headers_out.status != NGX_HTTP_PARTIAL_CONTENT
        || r->headers_out.content_length_n <= 0
        || (range = r->headers_out.content_range) == NULL
        || range->value.len < sizeof("bytes 0-0/1") - 1
        || ngx_strncmp(range->value.data, "bytes ", 6) != 0)
    {
        return NGX_ERROR;
    }

    p = range->value.data + 6;
    last = range->value.data + range->value.len;
    dash = ngx_strlchr(p, last, '-');
    if (dash == NULL) return NGX_ERROR;
    slash = ngx_strlchr(dash + 1, last, '/');
    if (slash == NULL || ngx_strlchr(slash + 1, last, '/') != NULL
        || ngx_strlchr(p, dash, '*') != NULL
        || ngx_strlchr(dash + 1, slash, '*') != NULL
        || ngx_strlchr(slash + 1, last, '*') != NULL)
    {
        return NGX_ERROR;
    }

    *start = ngx_atoof(p, dash - p);
    *end = ngx_atoof(dash + 1, slash - dash - 1);
    *complete = ngx_atoof(slash + 1, last - slash - 1);
    if (*start < 0 || *end < *start || *complete <= *end
        || *start != requested_start || *end > requested_end
        || r->headers_out.content_length_n != *end - *start + 1)
    {
        return NGX_ERROR;
    }
    return NGX_OK;
}


/* What: Parses Cache-Control tokens and quoted values.
 * Why: Avoids substring matches and rejects malformed policy.
 */
static ngx_int_t
l1_cache_cache_control_allowed(ngx_str_t *value)
{
    u_char   *p, *last, *start;
    size_t    len;
    ngx_uint_t quoted, escaped;

    p = value->data;
    last = p + value->len;

    for ( ;; ) {
        while (p < last && (*p == ' ' || *p == '\t')) p++;
        start = p;

        while (p < last
               && ((*p >= 'a' && *p <= 'z')
                   || (*p >= 'A' && *p <= 'Z')
                   || (*p >= '0' && *p <= '9')
                   || *p == '!' || *p == '#' || *p == '$' || *p == '%'
                   || *p == '&' || *p == '\'' || *p == '*' || *p == '+'
                   || *p == '-' || *p == '.' || *p == '^' || *p == '_'
                   || *p == '`' || *p == '|' || *p == '~'))
        {
            p++;
        }

        len = p - start;
        if (len == 0) return NGX_DECLINED;

        if ((len == sizeof("no-store") - 1
             && ngx_strncasecmp(start, (u_char *) "no-store", len) == 0)
            || (len == sizeof("no-cache") - 1
                && ngx_strncasecmp(start, (u_char *) "no-cache", len) == 0)
            || (len == sizeof("private") - 1
                && ngx_strncasecmp(start, (u_char *) "private", len) == 0))
        {
            return NGX_DECLINED;
        }

        while (p < last && (*p == ' ' || *p == '\t')) p++;
        if (p < last && *p == '=') {
            p++;
            while (p < last && (*p == ' ' || *p == '\t')) p++;

            if (p < last && *p == '"') {
                p++;
                quoted = 1;
                escaped = 0;

                while (p < last) {
                    if (escaped) {
                        if (*p != '\t' && (*p < 0x20 || *p == 0x7f)) {
                            return NGX_DECLINED;
                        }
                        escaped = 0;
                        p++;
                        continue;
                    }

                    if (*p == '\\') {
                        escaped = 1;
                        p++;
                        continue;
                    }

                    if (*p == '"') {
                        quoted = 0;
                        p++;
                        break;
                    }

                    if (*p != '\t' && (*p < 0x20 || *p == 0x7f)) {
                        return NGX_DECLINED;
                    }
                    p++;
                }

                if (quoted || escaped) return NGX_DECLINED;

            } else {
                start = p;
                while (p < last
                       && ((*p >= 'a' && *p <= 'z')
                           || (*p >= 'A' && *p <= 'Z')
                           || (*p >= '0' && *p <= '9')
                           || *p == '!' || *p == '#' || *p == '$'
                           || *p == '%' || *p == '&' || *p == '\''
                           || *p == '*' || *p == '+' || *p == '-'
                           || *p == '.' || *p == '^' || *p == '_'
                           || *p == '`' || *p == '|' || *p == '~'))
                {
                    p++;
                }
                if (p == start) return NGX_DECLINED;
            }
        }

        while (p < last && (*p == ' ' || *p == '\t')) p++;
        if (p == last) return NGX_OK;
        if (*p++ != ',') return NGX_DECLINED;
    }
}


static ngx_int_t
l1_cache_cacheable(ngx_http_request_t *r, ngx_flag_t force)
{
    ngx_list_part_t *part;
    ngx_table_elt_t *h;
    ngx_uint_t i;
    ngx_int_t slice;
    off_t start, end, complete;
    ngx_table_elt_t *etag;
    ngx_http_variable_value_t *slice_value;

    slice_value = l1_cache_slice_range(r);
    slice = slice_value != NULL && !slice_value->not_found;
    /* What: Rejects unvalidated full responses from Slice.
     * Why: A Slice key may identify only one segment.
     */
    if ((!slice && r->headers_out.content_range != NULL)
        || (!slice && r->headers_out.status != NGX_HTTP_OK)
        || (slice && r->headers_out.status != NGX_HTTP_PARTIAL_CONTENT)
        || r->headers_out.content_length_n < 0
        || r->headers_out.content_type.len == 0
        || r->headers_out.content_type.len > 128
        || (!force && (r->headers_in.authorization
                       || r->headers_in.cookie
                       || r->headers_in.proxy_authorization))
        || (r->headers_in.range && !slice) || r->headers_in.if_range
        || r->headers_in.if_none_match || r->headers_in.if_modified_since
        || r->headers_in.if_match || r->headers_in.if_unmodified_since)
        return 0;

    if (slice) {
        etag = r->headers_out.etag;
        if (l1_cache_slice_segment(r, &start, &end, &complete) != NGX_OK
            || (size_t) (end - start + 1) > l1_cache_conf->max_object
            || etag == NULL || etag->value.len == 0
            || (etag->value.len >= 2
                && ngx_strncmp(etag->value.data, "W/", 2) == 0))
            return 0;
    } else if ((size_t) r->headers_out.content_length_n
               > l1_cache_conf->max_object)
    {
        return 0;
    }
    part = &r->headers_out.headers.part;
    h = part->elts;
    for ( ;; ) {
        for (i = 0; i < part->nelts; i++) {
            if (h[i].hash == 0) continue;
            if (force && !l1_cache_force_header_safe(&h[i])) return 0;
            if ((!force && (L1_HEADER(h[i], "Set-Cookie")
                            || L1_HEADER(h[i], "Vary")
                            || L1_HEADER(h[i], "Content-Encoding")))
                || (L1_HEADER(h[i], "Content-Range") && !slice)
                || (!force && L1_HEADER(h[i], "Cache-Control")
                    && l1_cache_cache_control_allowed(&h[i].value) != NGX_OK))
                return 0;
            if (!L1_HEADER(h[i], "Date") && !L1_HEADER(h[i], "Server")
                && !L1_HEADER(h[i], "Accept-Ranges")
                && !L1_HEADER(h[i], "ETag")
                && !L1_HEADER(h[i], "Last-Modified")
                && !L1_HEADER(h[i], "Cache-Control")
                && !L1_HEADER(h[i], "Expires")
                && !L1_HEADER(h[i], "Age")
                && !L1_HEADER(h[i], "Content-Encoding")
                && !L1_HEADER(h[i], "Content-Range")
                && !L1_HEADER(h[i], "Content-Type")
                && !L1_HEADER(h[i], "Content-Length")
                && !force)
                return 0;
        }
        if (part->next == NULL) break;
        part = part->next;
        h = part->elts;
    }
    return 1;
}


/* What: Filters response fields before Force replay.
 * Why: Hop-by-hop and NGINX control fields are unsafe.
 */
static ngx_int_t
l1_cache_force_header_safe(ngx_table_elt_t *h)
{
    return !(L1_HEADER((*h), "Connection")
             || L1_HEADER((*h), "Keep-Alive")
             || L1_HEADER((*h), "Proxy-Authenticate")
             || L1_HEADER((*h), "Proxy-Authentication-Info")
             || L1_HEADER((*h), "Proxy-Authorization")
             || L1_HEADER((*h), "TE")
             || L1_HEADER((*h), "Trailer")
             || L1_HEADER((*h), "Transfer-Encoding")
             || L1_HEADER((*h), "Upgrade")
             || L1_HEADER((*h), "Proxy-Connection")
             || L1_HEADER((*h), "X-L1-Cache")
             || L1_HEADER((*h), "X-L1-Cache-Status")
             || (h->key.len >= sizeof("X-Accel-") - 1
                 && ngx_strncasecmp(h->key.data, (u_char *) "X-Accel-",
                                    sizeof("X-Accel-") - 1) == 0));
}


static ngx_int_t
l1_cache_capture_metadata(ngx_http_request_t *r,
    ngx_http_l1_cache_ctx_t *ctx)
{
    ngx_list_part_t *part;
    ngx_table_elt_t *h;
    ngx_table_elt_t **extra, **saved;
    ngx_array_t extra_headers;
    ngx_uint_t i, date_seen, server_seen, control_seen, expires_seen;
    ngx_uint_t encoding_seen, age_seen, j;
    uint64_t age_value, corrected_age_value, max_age;
    time_t date, now, apparent_age;
    uint64_t response_delay;
    uint64_t delay_seconds;
    size_t metadata_len, extra_len;
    u_char *p;

    date_seen = 0;
    server_seen = 0;
    control_seen = 0;
    expires_seen = 0;
    encoding_seen = 0;
    age_seen = 0;
    age_value = 0;
    date = (time_t) NGX_ERROR;
    max_age = (uint64_t) NGX_MAX_INT_T_VALUE;
    metadata_len = 0;
    extra_len = 0;
    now = ngx_time();

    if (ctx->force
        && ngx_array_init(&extra_headers, r->pool, 4,
                          sizeof(ngx_table_elt_t *)) != NGX_OK)
    {
        return NGX_DECLINED;
    }

    part = &r->headers_out.headers.part;
    h = part->elts;

    for ( ;; ) {
        for (i = 0; i < part->nelts; i++) {
            if (h[i].hash == 0
                || L1_HEADER(h[i], "ETag")
                || L1_HEADER(h[i], "Last-Modified")
                || L1_HEADER(h[i], "Accept-Ranges")
                || L1_HEADER(h[i], "Content-Range")
                || L1_HEADER(h[i], "Content-Type")
                || L1_HEADER(h[i], "Content-Length"))
            {
                continue;
            }

            if (ctx->force
                && (L1_HEADER(h[i], "Set-Cookie")
                    || L1_HEADER(h[i], "Vary")))
            {
                if (h[i].key.len > UINT32_MAX
                    || h[i].value.len > UINT32_MAX
                    || l1_cache_size_add(&extra_len, 8) != NGX_OK
                    || l1_cache_size_add(&extra_len, h[i].key.len) != NGX_OK
                    || l1_cache_size_add(&extra_len, h[i].value.len) != NGX_OK
                    || extra_len > 4096)
                {
                    return NGX_DECLINED;
                }

                extra = ngx_array_push(&extra_headers);
                if (extra == NULL) {
                    return NGX_DECLINED;
                }
                *extra = &h[i];
                continue;
            }

            if (L1_HEADER(h[i], "Date")) {
                if (date_seen++ || h[i].value.len > 128) {
                    return NGX_DECLINED;
                }
                date = ngx_parse_http_time(h[i].value.data, h[i].value.len);
                if (date == (time_t) NGX_ERROR) {
                    return NGX_DECLINED;
                }
                ctx->date = h[i].value;
            } else if (L1_HEADER(h[i], "Age")) {
                if (age_seen++) {
                    return NGX_DECLINED;
                }

                if (l1_cache_parse_age(&h[i].value, &age_value) != NGX_OK) {
                    return NGX_DECLINED;
                }

            } else if (L1_HEADER(h[i], "Server")) {
                if (server_seen++ || h[i].value.len > 1024) {
                    return NGX_DECLINED;
                }
                ctx->server = h[i].value;

            } else if (L1_HEADER(h[i], "Cache-Control")) {
                if (control_seen++ || h[i].value.len > 2048) {
                    return NGX_DECLINED;
                }
                ctx->cache_control = h[i].value;

            } else if (L1_HEADER(h[i], "Expires")) {
                if (expires_seen++ || h[i].value.len > 128) {
                    return NGX_DECLINED;
                }
                ctx->expires_header = h[i].value;

            } else if (L1_HEADER(h[i], "Content-Encoding")) {
                if (encoding_seen++ || h[i].value.len > 256) {
                    return NGX_DECLINED;
                }
                ctx->encoding = h[i].value;

            } else if (ctx->force) {
                if (h[i].key.len > UINT32_MAX
                    || h[i].value.len > UINT32_MAX
                    || l1_cache_size_add(&extra_len, 8) != NGX_OK
                    || l1_cache_size_add(&extra_len, h[i].key.len) != NGX_OK
                    || l1_cache_size_add(&extra_len, h[i].value.len) != NGX_OK
                    || extra_len > 4096)
                {
                    return NGX_DECLINED;
                }

                extra = ngx_array_push(&extra_headers);
                if (extra == NULL) {
                    return NGX_DECLINED;
                }
                *extra = &h[i];
                continue;

            } else {
                return NGX_DECLINED;
            }

            metadata_len += h[i].key.len + h[i].value.len;
            if (metadata_len > 4096) {
                return NGX_DECLINED;
            }
        }

        if (part->next == NULL) {
            break;
        }

        part = part->next;
        h = part->elts;
    }

    if (ctx->force && extra_len != 0) {
        ctx->extra_headers.data = ngx_pnalloc(r->pool, extra_len);
        if (ctx->extra_headers.data == NULL) {
            return NGX_DECLINED;
        }

        ctx->extra_headers.len = extra_len;
        p = ctx->extra_headers.data;
        saved = extra_headers.elts;

        for (j = 0; j < extra_headers.nelts; j++) {
            h = saved[j];
            *p++ = (u_char) (h->key.len >> 24);
            *p++ = (u_char) (h->key.len >> 16);
            *p++ = (u_char) (h->key.len >> 8);
            *p++ = (u_char) h->key.len;
            *p++ = (u_char) (h->value.len >> 24);
            *p++ = (u_char) (h->value.len >> 16);
            *p++ = (u_char) (h->value.len >> 8);
            *p++ = (u_char) h->value.len;
            p = ngx_copy(p, h->key.data, h->key.len);
            p = ngx_copy(p, h->value.data, h->value.len);
        }

        metadata_len += extra_len;
        if (metadata_len > 4096) {
            return NGX_DECLINED;
        }
    }

    ctx->response_time = ngx_time();
    ctx->response_msec = ngx_current_msec;

    /* What: Calculates RFC 9111 corrected initial age.
     * Why: Delay affects corrected age, not apparent age.
     */

    response_delay = 0;
    if (!ctx->l2_hit) {
        if (r->upstream != NULL && r->upstream->state != NULL
            && r->upstream->state->header_time != (ngx_msec_t) -1)
        {
            response_delay = r->upstream->state->header_time;
        }
    }

    delay_seconds = response_delay / 1000;
    if (response_delay % 1000 != 0) {
        delay_seconds++;
    }

    corrected_age_value = delay_seconds > max_age - age_value
                          ? max_age : age_value + delay_seconds;

    apparent_age = date != (time_t) NGX_ERROR && date < now
                   ? now - date : 0;

    if (apparent_age < 0) {
        apparent_age = 0;
    }

    ctx->initial_age = ngx_max(apparent_age, (time_t) corrected_age_value);

    return NGX_OK;
}

static ngx_int_t
l1_cache_admit_response(ngx_http_request_t *r,
    ngx_http_l1_cache_ctx_t *ctx, ngx_flag_t slice)
{
    size_t reserve, capture_alloc;
    ngx_atomic_int_t reserved;
    ngx_pool_cleanup_t *cleanup;
    ngx_queue_t *q;
    ngx_http_l1_cache_capture_t *active;
    off_t start, end, complete;
    time_t now;
    u_char *content_range;
    size_t active_key_len;
    u_char *active_key;
    ngx_http_proxy_loc_conf_t *plcf;

    if (ctx == NULL || ctx->admit || ctx->hit || !ctx->lookup
        || ctx->tracking_failed
        || r->main->method != NGX_HTTP_GET
        || (r != r->main && !slice)
        || !l1_cache_request_allowed(r->main, slice, ctx->force, ctx)
        || !l1_cache_cacheable(r, ctx->force))
    {
        return NGX_DECLINED;
    }

    plcf = ngx_http_get_module_loc_conf(r, ngx_http_proxy_module);
    if (plcf != NULL && plcf->upstream.no_cache != NULL
        && ngx_http_test_predicates(r, plcf->upstream.no_cache) != NGX_OK)
    {
        return NGX_DECLINED;
    }

    if (l1_cache_capture_metadata(r, ctx) != NGX_OK) {
        return NGX_DECLINED;
    }

    reserve = (size_t) r->headers_out.content_length_n;
    if (slice) {
        if (l1_cache_slice_segment(r, &start, &end, &complete) != NGX_OK) {
            return NGX_DECLINED;
        }

        reserve = (size_t) (end - start + 1);
        content_range = ngx_pnalloc(r->pool,
            sizeof("bytes ") - 1 + 3 * NGX_OFF_T_LEN + 2);
        if (content_range == NULL) {
            return NGX_DECLINED;
        }

        ctx->content_range.data = content_range;
        ctx->content_range.len = ngx_sprintf(content_range,
            "bytes %O-%O/%O", start, end, complete) - content_range;
    }

    if (reserve == 0 || ctx->key.len == 0 || ctx->key.len > 8192) {
        return NGX_DECLINED;
    }

    capture_alloc = offsetof(ngx_http_l1_cache_capture_t, key)
                    + ctx->key.len;

    l1_cache_lock(&l1_cache_conf->shpool->mutex);
    l1_cache_reap_captures_locked();
    /* What: Rejects capture if L1 filled after lookup.
     * Why: Avoids duplicate capture after a concurrent fill.
     */
    if (l1_cache_find(&ctx->key, ctx->force) != NULL) {
        ngx_shmtx_unlock(&l1_cache_conf->shpool->mutex);
        (void) ngx_atomic_fetch_add(&l1_cache_conf->sh->rejected, 1);
        return NGX_DECLINED;
    }

    if (ctx->capture != NULL
        && (ctx->capture->flags & L1_CAPTURE_TRACK_ONLY)
        && (ctx->capture->flags
            & (L1_CAPTURE_CANCELLED | L1_CAPTURE_PURGE_PENDING)))
    {
        ngx_shmtx_unlock(&l1_cache_conf->shpool->mutex);
        (void) ngx_atomic_fetch_add(&l1_cache_conf->sh->rejected, 1);
        return NGX_DECLINED;
    }

    /* What: Allows one active capture per key.
     * Why: Duplicate fills consume bounded memory.
     */
    for (q = ngx_queue_head(&l1_cache_conf->sh->captures);
         q != ngx_queue_sentinel(&l1_cache_conf->sh->captures);
         q = ngx_queue_next(q))
    {
        active = ngx_queue_data(q, ngx_http_l1_cache_capture_t, queue);
        if (active == ctx->capture) {
            continue;
        }
        if (active->flags & L1_CAPTURE_TRACK_ONLY) {
            continue;
        }
        active_key = l1_cache_capture_key(active, &active_key_len);
        if (!(active->flags & L1_CAPTURE_CANCELLED)
            && active->generation == ctx->generation
            && active_key_len == ctx->key.len
            && ngx_memcmp(active_key, ctx->key.data, ctx->key.len) == 0)
        {
            ngx_shmtx_unlock(&l1_cache_conf->shpool->mutex);
            (void) ngx_atomic_fetch_add(&l1_cache_conf->sh->rejected, 1);
            return NGX_DECLINED;
        }
    }

    reserved = l1_cache_conf->sh->building_bytes;
    if ((size_t) reserved + reserve
        > l1_cache_conf->zone->shm.size
          * l1_cache_conf->building_percent / 100)
    {
        ngx_shmtx_unlock(&l1_cache_conf->shpool->mutex);
        (void) ngx_atomic_fetch_add(&l1_cache_conf->sh->rejected, 1);
        return NGX_DECLINED;
    }

    if (ctx->capture == NULL) {
        ctx->capture = ngx_slab_alloc_locked(l1_cache_conf->shpool,
                                              capture_alloc);
        if (ctx->capture == NULL) {
            ngx_shmtx_unlock(&l1_cache_conf->shpool->mutex);
            (void) ngx_atomic_fetch_add(
                &l1_cache_conf->sh->allocation_failures, 1);
            l1_cache_mark_error_fallback(ctx);
            (void) ngx_atomic_fetch_add(&l1_cache_conf->sh->rejected, 1);
            return NGX_DECLINED;
        }
        ctx->capture->pid = ngx_pid;
        ctx->capture->generation = ctx->generation;
        ctx->capture->key_len = ctx->key.len;
        ngx_memcpy(ctx->capture->key, ctx->key.data, ctx->key.len);
        ngx_queue_insert_head(&l1_cache_conf->sh->captures,
                              &ctx->capture->queue);
    } else {
        ctx->capture->flags &= ~L1_CAPTURE_TRACK_ONLY;
        (void) ngx_atomic_fetch_add(
            &l1_cache_conf->sh->tracked_requests, -1);
        ctx->tracked = 0;
    }

    ctx->capture->bytes_or_entry = reserve;
    ctx->capture->flags = 0;
    (void) ngx_atomic_fetch_add(&l1_cache_conf->sh->building_bytes, reserve);
    (void) ngx_atomic_fetch_add(&l1_cache_conf->sh->building, 1);
    ngx_shmtx_unlock(&l1_cache_conf->shpool->mutex);

    ctx->reserved = 1;
    ctx->reserved_bytes = reserve;
    cleanup = ctx->capture_cleanup;
    if (cleanup == NULL) {
        cleanup = ngx_pool_cleanup_add(r->pool, 0);
        if (cleanup != NULL) {
            ctx->capture_cleanup = cleanup;
        }
    }
    if (cleanup != NULL) {
        cleanup->handler = l1_cache_finalize_capture;
        cleanup->data = ctx;
    }

    ctx->body = ngx_pnalloc(r->pool, reserve);
    if (cleanup == NULL || ctx->body == NULL) {
        l1_cache_mark_error_fallback(ctx);
        l1_cache_release_capture(ctx);
        (void) ngx_atomic_fetch_add(&l1_cache_conf->sh->rejected, 1);
        return NGX_DECLINED;
    }

    ctx->admit = 1;
    ctx->expected_len = reserve;
    ctx->response_status = slice ? NGX_HTTP_PARTIAL_CONTENT
                                  : r->headers_out.status;
    now = ngx_time();
    if (l1_cache_conf->ttl
        > (ngx_uint_t) (NGX_MAX_INT_T_VALUE - now))
    {
        ctx->expires = NGX_MAX_INT_T_VALUE;
    } else {
        ctx->expires = now + (time_t) l1_cache_conf->ttl;
    }
    if (ctx->valid_sec != 0) {
        ctx->expires = ngx_min(ctx->expires, ctx->valid_sec);
    }
    ctx->content_type = r->headers_out.content_type;
    if (r->headers_out.etag != NULL)
        ctx->etag = r->headers_out.etag->value;
    if (r->headers_out.last_modified != NULL)
        ctx->last_modified = r->headers_out.last_modified->value;
    if (r->headers_out.accept_ranges != NULL)
        ctx->accept_ranges = r->headers_out.accept_ranges->value;
    if (!slice && r->headers_out.content_range != NULL)
        ctx->content_range = r->headers_out.content_range->value;

    r->filter_need_in_memory = 1;
    r->main_filter_need_in_memory = 1;
    return NGX_OK;
}


static ngx_int_t
l1_cache_header_filter(ngx_http_request_t *r)
{
    static u_char only_if_cached_body[] =
        "<html>\r\n<head><title>504 Gateway Time-out</title></head>\r\n"
        "<body>\r\n<center><h1>504 Gateway Time-out</h1></center>\r\n"
        "<hr><center>nginx</center>\r\n</body>\r\n</html>\r\n";
    ngx_http_l1_cache_ctx_t *ctx;
    ngx_http_l1_cache_loc_conf_t *lcf;
    ngx_int_t rc;
    ngx_http_variable_value_t *slice_value;
    ngx_int_t slice;
    const char *cache_status;
    size_t cache_status_len;
    ngx_uint_t l2_status;
    ngx_buf_t *b;
    ngx_chain_t *cl;
    size_t body_len;
    u_char *body;

    lcf = ngx_http_get_module_loc_conf(r, ngx_http_l1_cache_module);
    ctx = ngx_http_get_module_ctx(r, ngx_http_l1_cache_module);
    if (ctx != NULL && ctx->hit) {
        return l1_cache_next_header_filter(r);
    }

    if (ctx != NULL && ctx->only_if_cached_peer_blocked
        && r->headers_out.status == NGX_HTTP_INTERNAL_SERVER_ERROR)
    {
        r->headers_out.status = NGX_HTTP_GATEWAY_TIME_OUT;
        r->headers_out.status_line.len = 0;

        body_len = sizeof(only_if_cached_body) - 1;
        body = ngx_pnalloc(r->pool, body_len);
        b = ngx_calloc_buf(r->pool);
        cl = ngx_alloc_chain_link(r->pool);

        if (body != NULL && b != NULL && cl != NULL) {
            ngx_memcpy(body, only_if_cached_body, body_len);
            b->pos = body;
            b->last = body + body_len;
            b->memory = 1;
            b->last_buf = (r == r->main);
            b->last_in_chain = 1;
            cl->buf = b;
            cl->next = NULL;
            ctx->only_if_cached_body = body;
            ctx->only_if_cached_body_len = body_len;
            ctx->only_if_cached_chain = cl;
            r->headers_out.content_length = NULL;
            r->headers_out.content_length_n = body_len;
        } else {
            /* What: Returns a bodyless 504 on allocation failure.
             * Why: Avoids pairing 504 status with the upstream 500 body.
             */
            r->headers_out.content_length = NULL;
            r->headers_out.content_length_n = 0;
        }
    }

    slice_value = l1_cache_slice_range(r);
    slice = slice_value != NULL && !slice_value->not_found;

    if (ctx != NULL && !ctx->l2_status_seen && r->upstream != NULL) {
        ctx->l2_status_seen = 1;
        l2_status = r->upstream->cache_status;
        if (l2_status == NGX_HTTP_CACHE_HIT) {
            ctx->l2_hit = 1;
            (void) ngx_atomic_fetch_add(&l1_cache_conf->sh->l2_hits, 1);
        } else if (l2_status == NGX_HTTP_CACHE_MISS
                   || l2_status == NGX_HTTP_CACHE_EXPIRED
                   || l2_status == NGX_HTTP_CACHE_REVALIDATED)
        {
            ctx->l2_miss = 1;
            ctx->l2_revalidated =
                l2_status == NGX_HTTP_CACHE_REVALIDATED;
            (void) ngx_atomic_fetch_add(&l1_cache_conf->sh->l2_misses, 1);
            if (!ctx->only_if_cached_peer_blocked) {
                (void) ngx_atomic_fetch_add(
                    &l1_cache_conf->sh->origin_requests, 1);
            }
        }
    }

    if (ctx != NULL && ctx->invalidate_after_response
        && r->headers_out.status >= NGX_HTTP_OK
        && r->headers_out.status < NGX_HTTP_BAD_REQUEST)
    {
        l1_cache_invalidate(&ctx->key, NULL,
                            ctx->purge_l2_after_response);
        if (ctx->purge_l2_after_response && r == r->main
            && l1_cache_start_l2_purge(r, &ctx->proxy_key, NULL) != NGX_OK)
        {
            ngx_log_error(NGX_LOG_WARN, r->connection->log, 0,
                          "l1 cache could not schedule native L2 purge");
        }
        ctx->invalidate_after_response = 0;
        ctx->purge_l2_after_response = 0;
    }

    if (l1_cache_update_l2_age(r, ctx) != NGX_OK) {
        return NGX_ERROR;
    }

    if (lcf->enabled && ctx != NULL && ctx->lookup
        && !ctx->age_update_failed
        && !ctx->slice_headers_checked)
    {
        (void) l1_cache_admit_response(r, ctx, slice);
    }

    if (ctx != NULL && ctx->admit && !ctx->l2_hit && r->cached == 0) {
        rc = l1_cache_persist_initial_age(r, ctx);
        if (rc != NGX_OK) {
            /* What: Rejects an Age value that cannot be updated safely.
             * Why: Neither cache can retain a stale Age representation.
             */
            ctx->age_update_failed = 1;
            ctx->admit = 0;
            if (r->upstream != NULL) {
                r->upstream->cacheable = 0;
            }
            l1_cache_release_capture(ctx);
            (void) ngx_atomic_fetch_add(&l1_cache_conf->sh->rejected, 1);
        }

        if (l1_cache_set_age_header(r, ctx->initial_age) != NGX_OK) {
            ctx->age_update_failed = 1;
        }
    }

    if (lcf->enabled && ctx != NULL
        && (ctx->lookup || ctx->error_fallback || r->upstream != NULL))
    {
        cache_status = "BYPASS";
        cache_status_len = sizeof("BYPASS") - 1;

        if (ctx != NULL && ctx->error_fallback) {
            cache_status = "ERROR_FALLBACK";
            cache_status_len = sizeof("ERROR_FALLBACK") - 1;

        } else if (ctx != NULL && ctx->age_update_failed) {
            cache_status = "BYPASS";
            cache_status_len = sizeof("BYPASS") - 1;

        } else if (ctx != NULL && ctx->lookup && ctx->l2_hit) {
            cache_status = "L1_MISS_L2_HIT";
            cache_status_len = sizeof("L1_MISS_L2_HIT") - 1;

        } else if (ctx != NULL && ctx->lookup && ctx->l2_miss) {
            cache_status = "L1_MISS_L2_MISS";
            cache_status_len = sizeof("L1_MISS_L2_MISS") - 1;
        }

        (void) l1_cache_add_status(r, cache_status, cache_status_len);
    }
    rc = l1_cache_next_header_filter(r);
    if (rc == NGX_ERROR && ctx != NULL && ctx->admit) {
        ctx->admit = 0;
        l1_cache_mark_error_fallback(ctx);
        l1_cache_release_capture(ctx);
    }
    return rc;
}

static ngx_int_t
l1_cache_body_filter(ngx_http_request_t *r, ngx_chain_t *in)
{
    ngx_http_l1_cache_ctx_t *ctx;
    ngx_chain_t *cl;
    size_t n;
    ngx_uint_t complete;

    ctx = ngx_http_get_module_ctx(r, ngx_http_l1_cache_module);
    if (ctx != NULL && ctx->only_if_cached_peer_blocked) {
        if (ctx->only_if_cached_chain != NULL
            && !ctx->only_if_cached_body_replaced && !r->header_only)
        {
            ctx->only_if_cached_body_replaced = 1;
            return l1_cache_next_body_filter(r, ctx->only_if_cached_chain);
        }

        return NGX_OK;
    }

    if (r != r->main && !l1_cache_is_slice_subrequest(r)) {
        return l1_cache_next_body_filter(r, in);
    }

    if (ctx == NULL || ctx->complete) {
        return l1_cache_next_body_filter(r, in);
    }

    if (!ctx->admit) {
        if (!ctx->tracked) {
            return l1_cache_next_body_filter(r, in);
        }

        complete = 0;
        for (cl = in; cl != NULL; cl = cl->next) {
            if (cl->buf->last_buf || cl->buf->last_in_chain) {
                complete = 1;
                break;
            }
        }
        if (complete) {
            ctx->complete = 1;
            if (l1_cache_schedule_post_commit_check(r, ctx) != NGX_OK) {
                l1_cache_post_commit_check_failed(ctx);
            }
        }
        return l1_cache_next_body_filter(r, in);
    }

    complete = 0;
    for (cl = in; cl != NULL; cl = cl->next) {
        if (cl->buf->last_buf || cl->buf->last_in_chain) {
            complete = 1;
        }
        if (ngx_buf_size(cl->buf) == 0) {
            continue;
        }
        if (cl->buf->in_file || !ngx_buf_in_memory(cl->buf)) {
            ctx->admit = 0;
            l1_cache_release_capture(ctx);
            break;
        }
        n = cl->buf->last - cl->buf->pos;
        if (ctx->len > ctx->reserved_bytes
            || n > ctx->reserved_bytes - ctx->len)
        {
            ctx->admit = 0;
            l1_cache_release_capture(ctx);
            break;
        }
        if (n) ngx_memcpy(ctx->body + ctx->len, cl->buf->pos, n);
        ctx->len += n;
        if (ctx->len == ctx->expected_len)
            complete = 1;
    }

    if (ctx->admit && complete) {
        if (ctx->len != ctx->expected_len || ctx->len == 0) {
            ctx->admit = 0;
            l1_cache_release_capture(ctx);
        } else {
            ctx->complete = 1;
            if (l1_cache_schedule_post_commit_check(r, ctx) != NGX_OK) {
                l1_cache_post_commit_check_failed(ctx);
            }

            if (ctx->admit && ctx->l2_miss && !ctx->l2_revalidated
                && l1_cache_defer_capture_finalize(r, ctx) != NGX_OK)
            {
                ctx->admit = 0;
                l1_cache_release_capture(ctx);
                (void) ngx_atomic_fetch_add(
                    &l1_cache_conf->sh->rejected, 1);
            }
        }
    }
    return l1_cache_next_body_filter(r, in);
}


/* What: Verifies new fills after native cache publication.
 * Why: Proxy temp-file cleanup is registered after header filters.
 * From: PR #1
 */
static ngx_int_t
l1_cache_defer_capture_finalize(ngx_http_request_t *r,
    ngx_http_l1_cache_ctx_t *ctx)
{
    ngx_pool_cleanup_t *cleanup;

    if (ctx->capture_finalizer_deferred) {
        return NGX_OK;
    }

    if (ctx->capture_cleanup == NULL) {
        return NGX_ERROR;
    }

    cleanup = ngx_pool_cleanup_add(r->pool, 0);
    if (cleanup == NULL) {
        return NGX_ERROR;
    }

    cleanup->handler = l1_cache_finalize_capture;
    cleanup->data = ctx;
    ctx->capture_cleanup->handler = NULL;
    ctx->capture_finalizer_deferred = 1;

    return NGX_OK;
}


static void
l1_cache_finalize_capture(void *data)
{
    ngx_http_l1_cache_ctx_t *ctx = data;

    l1_cache_update_l2_state(ctx);
    if (ctx->age_update_failed) {
        ctx->l2_persisted = 0;
    }
    if (ctx->admit && ctx->complete) {
        if (ctx->l2_persisted && ctx->l2_miss) {
            /* What: Preserves this fill during key invalidation.
             * Why: The active response may publish the replacement object.
             */
            l1_cache_invalidate(&ctx->key, ctx->capture, 0);
        }
        l1_cache_store_capture(ctx);
    }
    l1_cache_release_capture(ctx);
}


/* What: Checks native L2 publication after completion.
 * Why: A valid node may predate a failed replacement.
 */
static void
l1_cache_update_l2_state(ngx_http_l1_cache_ctx_t *ctx)
{
    ngx_http_cache_t *c;
    ngx_http_file_cache_t *cache;
    ngx_http_upstream_t *u;
    ngx_temp_file_t *tf;
    ngx_file_info_t fi;
    ngx_file_uniq_t write_uniq;
    time_t now;
    ngx_uint_t identity_matches;
    ngx_uint_t request_file_matches;

    if (ctx == NULL || ctx->r == NULL || (c = ctx->r->cache) == NULL
        || c->node == NULL || (cache = c->file_cache) == NULL)
    {
        return;
    }

    now = ngx_time();
    request_file_matches = 0;
    write_uniq = 0;
    if (ctx->l2_miss && !ctx->l2_revalidated
        && (u = ctx->r->upstream) != NULL && u->pipe != NULL
        && (tf = u->pipe->temp_file) != NULL
        && tf->file.fd != NGX_INVALID_FILE
        && ngx_fd_info(tf->file.fd, &fi) != NGX_FILE_ERROR)
    {
        write_uniq = ngx_file_uniq(&fi);
    }

    ngx_shmtx_lock(&cache->shpool->mutex);
    request_file_matches = write_uniq != 0
                           && c->node->uniq == write_uniq;
    /* What: Matches a committed write to its new node generation.
     * Why: Stock NGINX leaves request uniq at the old value.
     */
    /* What: Binds publication checking to this request's L2 file.
     * Why: PURGE can reuse the node before the response completes.
     */
    identity_matches =
        (ctx->l2_hit && c->uniq != 0 && c->node->uniq == c->uniq)
        || (ctx->l2_miss && ctx->l2_revalidated && c->uniq != 0
            && c->node->uniq == c->uniq)
        || (ctx->l2_miss && !ctx->l2_revalidated && c->updated
            && request_file_matches);
    ctx->l2_persisted = c->node->exists && c->node->uniq != 0
                        && identity_matches
                        && c->valid_sec > now && !c->purged
                        && (ctx->l2_hit || ctx->l2_revalidated || c->updated);
    if (ctx->l2_persisted) {
        ctx->valid_sec = c->valid_sec;
        if (ctx->expires != 0) {
            ctx->expires = ngx_min(ctx->expires, c->valid_sec);
        }
    }
    ngx_shmtx_unlock(&cache->shpool->mutex);
}


static void
l1_cache_store_capture(ngx_http_l1_cache_ctx_t *ctx)
{
    ngx_http_l1_cache_entry_t *entry;
    size_t entry_alloc, metadata_len, aligned_metadata, copy_offset, block_len;
    size_t required, usable;
    time_t now;
    ngx_uint_t i, block_count, allocation_failed;
    u_char *p, **blocks;

    now = ngx_time();
    if (!ctx->l2_persisted || ctx->valid_sec <= now || ctx->expires <= now) {
        ctx->admit = 0;
        (void) ngx_atomic_fetch_add(&l1_cache_conf->sh->rejected, 1);
        return;
    }
    ctx->expires = ngx_min(ctx->expires, ctx->valid_sec);

    block_count = ctx->len / l1_cache_conf->block_size;
    if (ctx->len % l1_cache_conf->block_size != 0) {
        block_count++;
    }

    metadata_len = 0;
    if (l1_cache_size_add(&metadata_len, ctx->key.len) != NGX_OK
        || l1_cache_size_add(&metadata_len, ctx->content_type.len) != NGX_OK
        || l1_cache_size_add(&metadata_len, ctx->etag.len) != NGX_OK
        || l1_cache_size_add(&metadata_len, ctx->last_modified.len) != NGX_OK
        || l1_cache_size_add(&metadata_len, ctx->accept_ranges.len) != NGX_OK
        || l1_cache_size_add(&metadata_len, ctx->content_range.len) != NGX_OK
        || l1_cache_size_add(&metadata_len, ctx->date.len) != NGX_OK
        || l1_cache_size_add(&metadata_len, ctx->server.len) != NGX_OK
        || l1_cache_size_add(&metadata_len, ctx->cache_control.len) != NGX_OK
        || l1_cache_size_add(&metadata_len, ctx->expires_header.len) != NGX_OK
        || l1_cache_size_add(&metadata_len, ctx->encoding.len) != NGX_OK
        || l1_cache_size_add(&metadata_len, ctx->extra_headers.len) != NGX_OK
        || metadata_len > NGX_MAX_SIZE_T_VALUE - (sizeof(void *) - 1))
    {
        goto size_overflow;
    }

    aligned_metadata = (metadata_len + sizeof(void *) - 1)
                       & ~(sizeof(void *) - 1);
    entry_alloc = offsetof(ngx_http_l1_cache_entry_t, data);
    if (aligned_metadata > NGX_MAX_SIZE_T_VALUE - entry_alloc) {
        goto size_overflow;
    }
    entry_alloc += aligned_metadata;
    if (block_count
        > (NGX_MAX_SIZE_T_VALUE - entry_alloc) / sizeof(u_char *))
    {
        goto size_overflow;
    }
    entry_alloc += block_count * sizeof(u_char *);
    if (ctx->len > NGX_MAX_SIZE_T_VALUE - entry_alloc) {
        goto size_overflow;
    }

    l1_cache_lock(&l1_cache_conf->shpool->mutex);
    if (ctx->capture->flags & L1_CAPTURE_CANCELLED) {
        ngx_shmtx_unlock(&l1_cache_conf->shpool->mutex);
        ctx->admit = 0;
        (void) ngx_atomic_fetch_add(&l1_cache_conf->sh->rejected, 1);
        return;
    }

    if (l1_cache_find(&ctx->key, ctx->force) != NULL) {
        ngx_shmtx_unlock(&l1_cache_conf->shpool->mutex);
        ctx->admit = 0;
        (void) ngx_atomic_fetch_add(&l1_cache_conf->sh->rejected, 1);
        return;
    }

    entry = NULL;
    allocation_failed = 0;
    required = entry_alloc + ctx->len;
    usable = l1_cache_conf->zone->shm.size * 80 / 100;
    while (required <= usable
           && l1_cache_conf->sh->used_bytes > usable - required)
    {
        if (l1_cache_evict_one_locked() != NGX_OK) break;
    }

    if (required <= usable
        && l1_cache_conf->sh->used_bytes <= usable - required)
    {
        for ( ;; ) {
            entry = ngx_slab_alloc_locked(l1_cache_conf->shpool, entry_alloc);
            if (entry == NULL) {
                allocation_failed = 1;
                if (l1_cache_evict_one_locked() == NGX_OK) continue;
                break;
            }

            ngx_memzero(entry, entry_alloc);
            entry->state = L1_ENTRY_BUILDING;
            entry->key_len = ctx->key.len;
            entry->type_len = ctx->content_type.len;
            entry->etag_len = ctx->etag.len;
            entry->modified_len = ctx->last_modified.len;
            entry->ranges_len = ctx->accept_ranges.len;
            entry->content_range_len = ctx->content_range.len;
            entry->date_len = ctx->date.len;
            entry->server_len = ctx->server.len;
            entry->cache_control_len = ctx->cache_control.len;
            entry->expires_header_len = ctx->expires_header.len;
            entry->encoding_len = ctx->encoding.len;
            entry->extra_headers_len = ctx->extra_headers.len;
            entry->body_len = ctx->len;
            entry->block_size = l1_cache_conf->block_size;
            entry->block_count = block_count;
            entry->alloc_bytes = required;
            entry->status = ctx->response_status;
            entry->force = ctx->force;
            entry->generation = ctx->generation;
            entry->initial_age = ctx->initial_age;
            p = ngx_cpymem(entry->data, ctx->key.data, ctx->key.len);
            p = ngx_cpymem(p, ctx->content_type.data, entry->type_len);
            if (entry->etag_len)
                p = ngx_cpymem(p, ctx->etag.data, entry->etag_len);
            if (entry->modified_len)
                p = ngx_cpymem(p, ctx->last_modified.data,
                               entry->modified_len);
            if (entry->ranges_len)
                p = ngx_cpymem(p, ctx->accept_ranges.data,
                               entry->ranges_len);
            if (entry->content_range_len)
                p = ngx_cpymem(p, ctx->content_range.data,
                               entry->content_range_len);
            if (entry->date_len)
                p = ngx_cpymem(p, ctx->date.data, entry->date_len);
            if (entry->server_len)
                p = ngx_cpymem(p, ctx->server.data, entry->server_len);
            if (entry->cache_control_len)
                p = ngx_cpymem(p, ctx->cache_control.data,
                               entry->cache_control_len);
            if (entry->expires_header_len)
                p = ngx_cpymem(p, ctx->expires_header.data,
                               entry->expires_header_len);
            if (entry->encoding_len)
                p = ngx_cpymem(p, ctx->encoding.data, entry->encoding_len);
            if (entry->extra_headers_len)
                p = ngx_cpymem(p, ctx->extra_headers.data,
                               entry->extra_headers_len);
            blocks = l1_cache_blocks(entry);

            copy_offset = 0;
            for (i = 0; i < block_count; i++) {
                block_len = ngx_min(entry->block_size,
                                    ctx->len - copy_offset);
                blocks[i] = ngx_slab_alloc_locked(l1_cache_conf->shpool,
                                                   block_len);
                if (blocks[i] == NULL) {
                    allocation_failed = 1;
                    break;
                }
                copy_offset += block_len;
            }
            if (i == block_count) break;

            l1_cache_free_entry_locked(entry);
            entry = NULL;
            if (l1_cache_evict_one_locked() != NGX_OK) break;
        }
    }

    if (entry == NULL) {
        ngx_shmtx_unlock(&l1_cache_conf->shpool->mutex);
        (void) ngx_atomic_fetch_add(&l1_cache_conf->sh->rejected, 1);
        if (allocation_failed) {
            (void) ngx_atomic_fetch_add(
                &l1_cache_conf->sh->allocation_failures, 1);
        }
        ctx->admit = 0;
        return;
    }
    ctx->capture->bytes_or_entry = (size_t) (uintptr_t) entry;
    ctx->capture->flags |= L1_CAPTURE_HAS_ENTRY;
    blocks = l1_cache_blocks(entry);
    ngx_shmtx_unlock(&l1_cache_conf->shpool->mutex);

    l1_cache_copy_capture_body(entry, ctx->body);

    l1_cache_lock(&l1_cache_conf->shpool->mutex);
    if (l1_cache_find(&ctx->key, ctx->force) == NULL
        && ctx->generation == l1_cache_conf->sh->generation
        && !(ctx->capture->flags & L1_CAPTURE_CANCELLED)
        && ctx->l2_persisted && ctx->valid_sec > ngx_time()
        && ctx->expires > ngx_time())
    {
        entry->state = L1_ENTRY_READY;
        entry->expires = ctx->expires;
        entry->created = ctx->response_time;
        entry->received_msec = ctx->response_msec;
        entry->node.key = ngx_crc32_short(ctx->key.data, ctx->key.len);
        if (entry->force) {
            entry->node.key ^= (ngx_rbtree_key_t) 0x80000000;
        }
        ngx_rbtree_insert(&l1_cache_conf->sh->index, &entry->node);
        entry->indexed = 1;
        entry->accounted = 1;
        l1_cache_conf->sh->used_bytes += entry->alloc_bytes;
        (void) ngx_atomic_fetch_add(&l1_cache_conf->sh->objects, 1);
        ngx_queue_insert_head(&l1_cache_conf->sh->probation, &entry->queue);
        (void) ngx_atomic_fetch_add(&l1_cache_conf->sh->accepted, 1);
    } else {
        entry->state = L1_ENTRY_BUILDING;
        l1_cache_free_entry_locked(entry);
    }
    ctx->capture->bytes_or_entry = ctx->reserved_bytes;
    ctx->capture->flags &= ~L1_CAPTURE_HAS_ENTRY;
    ngx_shmtx_unlock(&l1_cache_conf->shpool->mutex);
    return;

size_overflow:
    ctx->admit = 0;
    (void) ngx_atomic_fetch_add(&l1_cache_conf->sh->rejected, 1);
}


/* What: Copies completed bodies into shared-memory blocks.
 * Why: The shared lock must not cover the body copy.
 */
static void
l1_cache_copy_capture_body(ngx_http_l1_cache_entry_t *entry, u_char *body)
{
    ngx_uint_t i;
    size_t offset, block_len;
    u_char **blocks;

    blocks = l1_cache_blocks(entry);
    offset = 0;
    for (i = 0; i < entry->block_count; i++) {
        block_len = ngx_min(entry->block_size, entry->body_len - offset);
        ngx_memcpy(blocks[i], body + offset, block_len);
        offset += block_len;
    }
}


static void
l1_cache_unpin(void *data)
{
    ngx_http_l1_cache_pin_t *pin = *(ngx_http_l1_cache_pin_t **) data;

    l1_cache_release_pin(pin);
}


static void
l1_cache_release_pin(ngx_http_l1_cache_pin_t *pin)
{
    ngx_http_l1_cache_entry_t *entry;

    l1_cache_lock(&l1_cache_conf->shpool->mutex);
    entry = pin->entry;
    ngx_queue_remove(&pin->queue);
    ngx_slab_free_locked(l1_cache_conf->shpool, pin);
    if (entry->refs != 0) {
        entry->refs--;
        if (entry->refs == 0) {
            (void) ngx_atomic_fetch_add(&l1_cache_conf->sh->pinned, -1);
        }
    }
    if (entry->refs == 0 && entry->state == L1_ENTRY_EXPIRED) {
        l1_cache_free_entry_locked(entry);
    }
    ngx_shmtx_unlock(&l1_cache_conf->shpool->mutex);
}


static void
l1_cache_reap_pins_locked(ngx_flag_t force)
{
    ngx_queue_t *q, *next;
    ngx_http_l1_cache_pin_t *pin;
    ngx_http_l1_cache_entry_t *entry;
    ngx_msec_t now;

    now = ngx_current_msec;
    if (!force && l1_cache_conf->sh->reap_msec != 0
        && (ngx_msec_t) (now - l1_cache_conf->sh->reap_msec) < 1000)
    {
        return;
    }
    l1_cache_conf->sh->reap_msec = now;

    for (q = ngx_queue_head(&l1_cache_conf->sh->pins);
         q != ngx_queue_sentinel(&l1_cache_conf->sh->pins); q = next)
    {
        next = ngx_queue_next(q);
        pin = ngx_queue_data(q, ngx_http_l1_cache_pin_t, queue);
        if (kill(pin->pid, 0) != -1 || ngx_errno != NGX_ESRCH) continue;

        entry = pin->entry;
        ngx_queue_remove(q);
        ngx_slab_free_locked(l1_cache_conf->shpool, pin);
        if (entry->refs == 0) continue;

        entry->refs--;
        if (entry->refs == 0) {
            (void) ngx_atomic_fetch_add(&l1_cache_conf->sh->pinned, -1);
            if (entry->state == L1_ENTRY_EXPIRED) {
                l1_cache_free_entry_locked(entry);
            }
        }
    }
}

static void
l1_cache_release_capture(void *data)
{
    ngx_http_l1_cache_ctx_t *ctx = data;
    ngx_http_l1_cache_entry_t *entry;
    ngx_flag_t retain_for_purge;

    if (ctx->reserved || ctx->tracked) {
        ngx_flag_t reserved;

        reserved = ctx->reserved;
        ctx->reserved = 0;
        ctx->tracked = 0;
        l1_cache_lock(&l1_cache_conf->shpool->mutex);
        retain_for_purge =
            (ctx->capture->flags & (L1_CAPTURE_CHECK_FAILED
                                    | L1_CAPTURE_PURGE_PENDING))
            == (L1_CAPTURE_CHECK_FAILED | L1_CAPTURE_PURGE_PENDING);
        if (retain_for_purge) {
            if (ctx->capture->flags & L1_CAPTURE_HAS_ENTRY) {
                entry = (ngx_http_l1_cache_entry_t *)
                        (uintptr_t) ctx->capture->bytes_or_entry;
                if (entry->state == L1_ENTRY_BUILDING) {
                    l1_cache_free_entry_locked(entry);
                }
            }
            if (reserved) {
                (void) ngx_atomic_fetch_add(
                    &l1_cache_conf->sh->building, -1);
                (void) ngx_atomic_fetch_add(
                    &l1_cache_conf->sh->building_bytes,
                    -((ngx_atomic_int_t) ctx->reserved_bytes));
                (void) ngx_atomic_fetch_add(
                    &l1_cache_conf->sh->tracked_requests, 1);
            }
            ctx->capture->pid = 0;
            ctx->capture->bytes_or_entry = 0;
            ctx->capture->flags = L1_CAPTURE_TRACK_ONLY
                                  | L1_CAPTURE_CHECK_FAILED
                                  | L1_CAPTURE_PURGE_PENDING
                                  | L1_CAPTURE_FILL_DONE
                                  | (ctx->capture->flags
                                     & L1_CAPTURE_PURGE_RETRYING);
            ctx->capture = NULL;
            ngx_shmtx_unlock(&l1_cache_conf->shpool->mutex);
            return;
        }
        ngx_queue_remove(&ctx->capture->queue);
        if (ctx->capture->flags & L1_CAPTURE_HAS_ENTRY) {
            entry = (ngx_http_l1_cache_entry_t *)
                    (uintptr_t) ctx->capture->bytes_or_entry;
            if (entry->state == L1_ENTRY_BUILDING) {
                l1_cache_free_entry_locked(entry);
            }
        }
        if (reserved) {
            (void) ngx_atomic_fetch_add(&l1_cache_conf->sh->building, -1);
            (void) ngx_atomic_fetch_add(&l1_cache_conf->sh->building_bytes,
                                       -((ngx_atomic_int_t)
                                         ctx->reserved_bytes));
        } else {
            (void) ngx_atomic_fetch_add(
                &l1_cache_conf->sh->tracked_requests, -1);
        }
        ngx_slab_free_locked(l1_cache_conf->shpool, ctx->capture);
        ctx->capture = NULL;
        ngx_shmtx_unlock(&l1_cache_conf->shpool->mutex);
    }
}


static u_char *
l1_cache_capture_key(ngx_http_l1_cache_capture_t *capture, size_t *len)
{
    ngx_http_l1_cache_entry_t *entry;

    if (capture->flags & L1_CAPTURE_HAS_ENTRY) {
        entry = (ngx_http_l1_cache_entry_t *)
                (uintptr_t) capture->bytes_or_entry;
        *len = entry->key_len;
        return entry->data;
    }

    *len = capture->key_len;
    return capture->key;
}


/* What: Tracks an eligible L2 request before its response headers arrive.
 * Why: PURGE must see fills even when L1 later rejects their response body.
 */
static ngx_int_t
l1_cache_track_request(ngx_http_request_t *r, ngx_http_l1_cache_ctx_t *ctx)
{
    ngx_pool_cleanup_t *cleanup;
    ngx_queue_t *q;
    ngx_http_l1_cache_capture_t *capture, *active;
    size_t alloc, active_key_len;
    u_char *active_key;

    if (ctx->capture != NULL || ctx->key.len == 0 || ctx->key.len > 8192) {
        return NGX_ERROR;
    }

    alloc = offsetof(ngx_http_l1_cache_capture_t, key) + ctx->key.len;
    l1_cache_lock(&l1_cache_conf->shpool->mutex);
    l1_cache_reap_captures_locked();
    if (ctx->purge_generation != l1_cache_conf->sh->purge_generation
        || l1_cache_conf->sh->tracked_requests
           >= l1_cache_conf->max_tracked_requests)
    {
        ngx_shmtx_unlock(&l1_cache_conf->shpool->mutex);
        return NGX_ERROR;
    }

    for (q = ngx_queue_head(&l1_cache_conf->sh->captures);
         q != ngx_queue_sentinel(&l1_cache_conf->sh->captures);
         q = ngx_queue_next(q))
    {
        active = ngx_queue_data(q, ngx_http_l1_cache_capture_t, queue);
        if (!(active->flags & L1_CAPTURE_PURGE_PENDING)) {
            continue;
        }
        active_key = l1_cache_capture_key(active, &active_key_len);
        if (active_key_len == ctx->key.len
            && ngx_memcmp(active_key, ctx->key.data, ctx->key.len) == 0)
        {
            ngx_shmtx_unlock(&l1_cache_conf->shpool->mutex);
            return NGX_ERROR;
        }
    }

    capture = ngx_slab_alloc_locked(l1_cache_conf->shpool, alloc);
    if (capture == NULL) {
        ngx_shmtx_unlock(&l1_cache_conf->shpool->mutex);
        (void) ngx_atomic_fetch_add(
            &l1_cache_conf->sh->allocation_failures, 1);
        return NGX_ERROR;
    }

    capture->pid = ngx_pid;
    capture->generation = ctx->generation;
    capture->bytes_or_entry = 0;
    capture->key_len = ctx->key.len;
    capture->flags = L1_CAPTURE_TRACK_ONLY;
    ngx_memcpy(capture->key, ctx->key.data, ctx->key.len);
    ngx_queue_insert_head(&l1_cache_conf->sh->captures, &capture->queue);
    (void) ngx_atomic_fetch_add(&l1_cache_conf->sh->tracked_requests, 1);
    ngx_shmtx_unlock(&l1_cache_conf->shpool->mutex);

    ctx->capture = capture;
    ctx->tracked = 1;
    cleanup = ngx_pool_cleanup_add(r->pool, 0);
    if (cleanup == NULL) {
        l1_cache_release_capture(ctx);
        return NGX_ERROR;
    }
    cleanup->handler = l1_cache_release_capture;
    cleanup->data = ctx;
    ctx->capture_cleanup = cleanup;

    return NGX_OK;
}


/* What: Schedules native PURGE after the tracked response finishes.
 * Why: NGINX may publish its cache file after output filters complete.
 */
static ngx_int_t
l1_cache_schedule_post_commit_check(ngx_http_request_t *r,
    ngx_http_l1_cache_ctx_t *ctx)
{
    if (ctx == NULL || ctx->capture == NULL || ctx->l2_purge_scheduled) {
        return NGX_OK;
    }

    if (!ctx->proxy_key_ready
        || l1_cache_start_l2_purge_check(r, &ctx->proxy_key,
                                         ctx->capture) != NGX_OK)
    {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "l1 cache could not schedule post-commit L2 check");
        return NGX_ERROR;
    }

    ctx->l2_purge_scheduled = 1;
    return NGX_OK;
}


/* What: Retains a failed post-commit check for safe PURGE retry.
 * Why: NGINX may commit L2 before the final body-filter callback.
 */
static void
l1_cache_post_commit_check_failed(ngx_http_l1_cache_ctx_t *ctx)
{
    ctx->admit = 0;
    if (ctx->capture != NULL) {
        l1_cache_lock(&l1_cache_conf->shpool->mutex);
        ctx->capture->flags |= L1_CAPTURE_CHECK_FAILED;
        ngx_shmtx_unlock(&l1_cache_conf->shpool->mutex);
    }
    (void) ngx_atomic_fetch_add(&l1_cache_conf->sh->rejected, 1);
}


/* What: Blocks reuse while a matching fill awaits its deferred L2 purge.
 * Why: Otherwise another request could observe the just-committed stale file.
 */
static ngx_flag_t
l1_cache_has_pending_l2_purge(ngx_str_t *key,
    ngx_http_l1_cache_capture_t **retry)
{
    ngx_queue_t *q;
    ngx_http_l1_cache_capture_t *capture;
    size_t capture_key_len;
    u_char *capture_key;

    *retry = NULL;
    l1_cache_lock(&l1_cache_conf->shpool->mutex);

    for (q = ngx_queue_head(&l1_cache_conf->sh->captures);
         q != ngx_queue_sentinel(&l1_cache_conf->sh->captures);
         q = ngx_queue_next(q))
    {
        capture = ngx_queue_data(q, ngx_http_l1_cache_capture_t, queue);
        if (!(capture->flags & L1_CAPTURE_PURGE_PENDING)) {
            continue;
        }

        capture_key = l1_cache_capture_key(capture, &capture_key_len);
        if (capture_key_len == key->len
            && ngx_memcmp(capture_key, key->data, key->len) == 0)
        {
            if ((capture->flags & (L1_CAPTURE_CHECK_FAILED
                                   | L1_CAPTURE_FILL_DONE
                                   | L1_CAPTURE_PURGE_RETRYING))
                == (L1_CAPTURE_CHECK_FAILED | L1_CAPTURE_FILL_DONE))
            {
                capture->flags |= L1_CAPTURE_PURGE_RETRYING;
                *retry = capture;
            }
            ngx_shmtx_unlock(&l1_cache_conf->shpool->mutex);
            return 1;
        }
    }

    ngx_shmtx_unlock(&l1_cache_conf->shpool->mutex);
    return 0;
}


static void
l1_cache_release_purge_retry(ngx_http_l1_cache_capture_t *capture)
{
    l1_cache_lock(&l1_cache_conf->shpool->mutex);
    capture->flags &= ~L1_CAPTURE_PURGE_RETRYING;
    ngx_shmtx_unlock(&l1_cache_conf->shpool->mutex);
}


static size_t
l1_cache_capture_reserved_bytes(ngx_http_l1_cache_capture_t *capture)
{
    if (capture->flags & L1_CAPTURE_HAS_ENTRY) {
        return ((ngx_http_l1_cache_entry_t *)
                (uintptr_t) capture->bytes_or_entry)->body_len;
    }

    return capture->bytes_or_entry;
}


static void
l1_cache_reap_captures_locked(void)
{
    ngx_queue_t *q, *next;
    ngx_http_l1_cache_capture_t *capture;
    size_t bytes;

    l1_cache_reap_pins_locked(1);

    for (q = ngx_queue_head(&l1_cache_conf->sh->captures);
         q != ngx_queue_sentinel(&l1_cache_conf->sh->captures); q = next)
    {
        next = ngx_queue_next(q);
        capture = ngx_queue_data(q, ngx_http_l1_cache_capture_t, queue);
        if (capture->flags & L1_CAPTURE_FILL_DONE) {
            continue;
        }
        if (kill(capture->pid, 0) != -1 || ngx_errno != NGX_ESRCH) continue;
        if ((capture->flags & (L1_CAPTURE_CHECK_FAILED
                               | L1_CAPTURE_PURGE_PENDING))
            == (L1_CAPTURE_CHECK_FAILED | L1_CAPTURE_PURGE_PENDING))
        {
            bytes = l1_cache_capture_reserved_bytes(capture);
            if (capture->flags & L1_CAPTURE_HAS_ENTRY) {
                l1_cache_free_entry_locked((ngx_http_l1_cache_entry_t *)
                                           (uintptr_t)
                                           capture->bytes_or_entry);
            }
            if (!(capture->flags & L1_CAPTURE_TRACK_ONLY)) {
                (void) ngx_atomic_fetch_add(
                    &l1_cache_conf->sh->building, -1);
                (void) ngx_atomic_fetch_add(
                    &l1_cache_conf->sh->building_bytes,
                    -((ngx_atomic_int_t) bytes));
                (void) ngx_atomic_fetch_add(
                    &l1_cache_conf->sh->tracked_requests, 1);
            }
            capture->pid = 0;
            capture->bytes_or_entry = 0;
            capture->flags = L1_CAPTURE_TRACK_ONLY
                             | L1_CAPTURE_CHECK_FAILED
                             | L1_CAPTURE_PURGE_PENDING
                             | L1_CAPTURE_FILL_DONE
                             | (capture->flags
                                & L1_CAPTURE_PURGE_RETRYING);
            continue;
        }
        ngx_queue_remove(q);
        bytes = l1_cache_capture_reserved_bytes(capture);
        /* What: Frees unpublished blocks after worker exit.
         * Why: Body copying runs outside the shared mutex.
         */
        if (capture->flags & L1_CAPTURE_HAS_ENTRY) {
            l1_cache_free_entry_locked((ngx_http_l1_cache_entry_t *)
                                       (uintptr_t) capture->bytes_or_entry);
        }
        if (capture->flags & L1_CAPTURE_TRACK_ONLY) {
            (void) ngx_atomic_fetch_add(
                &l1_cache_conf->sh->tracked_requests, -1);
        } else {
            (void) ngx_atomic_fetch_add(&l1_cache_conf->sh->building, -1);
            (void) ngx_atomic_fetch_add(&l1_cache_conf->sh->building_bytes,
                                       -((ngx_atomic_int_t) bytes));
        }
        ngx_slab_free_locked(l1_cache_conf->shpool, capture);
    }
}
