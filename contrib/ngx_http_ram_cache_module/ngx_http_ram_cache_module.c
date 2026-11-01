/* Shared-memory response cache for small, complete public GET responses. */
#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#include <ngx_http_cache.h>

#define RAM_HEADER(h, s) \
    ((h).key.len == sizeof(s) - 1 \
     && ngx_strncasecmp((h).key.data, (u_char *) s, sizeof(s) - 1) == 0)

#define RAM_BLOCK_SIZE (64 * 1024)
#define RAM_MAX_OBJECT (8 * 1024 * 1024)

typedef enum {
    RAM_ENTRY_BUILDING = 0,
    RAM_ENTRY_READY,
    RAM_ENTRY_EVICTING,
    RAM_ENTRY_EXPIRED
} ngx_http_ram_cache_state_e;

extern ngx_module_t ngx_http_slice_filter_module;

static ngx_str_t ram_cache_slice_range_name = ngx_string("slice_range");

typedef struct ngx_http_ram_cache_entry_s ngx_http_ram_cache_entry_t;
typedef struct ngx_http_ram_cache_capture_s ngx_http_ram_cache_capture_t;

struct ngx_http_ram_cache_capture_s {
    ngx_queue_t   queue;
    ngx_pid_t     pid;
    size_t        bytes;
};

struct ngx_http_ram_cache_entry_s {
    ngx_queue_t       queue;
    ngx_uint_t        refs;
    ngx_uint_t        state;
    ngx_uint_t        protected;
    ngx_uint_t        block_count;
    ngx_uint_t        accounted;
    ngx_uint_t        status;
    ngx_atomic_t      generation;
    time_t            expires;
    time_t            created;
    size_t            key_len;
    size_t            type_len;
    size_t            etag_len;
    size_t            modified_len;
    size_t            ranges_len;
    size_t            content_range_len;
    size_t            body_len;
    size_t            alloc_bytes;
    u_char            data[1];
};

typedef struct {
    ngx_queue_t       probation;
    ngx_queue_t       protected;
    ngx_queue_t       captures;
    ngx_atomic_t      building;
    ngx_atomic_t      building_bytes;
    ngx_atomic_t      generation;
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
    size_t            used_bytes;
    size_t            protected_bytes;
} ngx_http_ram_cache_sh_t;

typedef struct {
    ngx_shm_zone_t              *zone;
    ngx_slab_pool_t             *shpool;
    ngx_http_ram_cache_sh_t     *sh;
    ngx_str_t                    purge_token;
    size_t                       max_object;
    ngx_uint_t                   ttl;
} ngx_http_ram_cache_main_conf_t;

typedef struct {
    ngx_flag_t enabled;
} ngx_http_ram_cache_loc_conf_t;

typedef struct {
    ngx_http_request_t *r;
    ngx_str_t    key;
    ngx_str_t    content_type;
    ngx_str_t    etag;
    ngx_str_t    last_modified;
    ngx_str_t    accept_ranges;
    ngx_str_t    content_range;
    u_char      *body;
    size_t       len;
    size_t       reserved_bytes;
    size_t       expected_len;
    ngx_uint_t   response_status;
    time_t       expires;
    ngx_atomic_t generation;
    ngx_uint_t   hit:1;
    ngx_uint_t   admit:1;
    ngx_uint_t   reserved:1;
    ngx_uint_t   complete:1;
    ngx_http_ram_cache_capture_t *capture;
} ngx_http_ram_cache_ctx_t;

static ngx_http_ram_cache_main_conf_t *ram_cache_conf;
static ngx_http_output_header_filter_pt ram_cache_next_header_filter;
static ngx_http_output_body_filter_pt ram_cache_next_body_filter;

static void *ram_cache_create_main_conf(ngx_conf_t *cf);
static char *ram_cache_zone(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
static ngx_int_t ram_cache_zone_init(ngx_shm_zone_t *zone, void *data);
static ngx_int_t ram_cache_init(ngx_conf_t *cf);
static ngx_int_t ram_cache_add_variables(ngx_conf_t *cf);
static ngx_int_t ram_cache_stat_variable(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t data);
static ngx_int_t ram_cache_add_status(ngx_http_request_t *r,
    const char *value, size_t len);
static ngx_int_t ram_cache_purge_request(ngx_http_request_t *r,
    ngx_str_t *token);
static ngx_int_t ram_cache_handler(ngx_http_request_t *r);
static ngx_int_t ram_cache_header_filter(ngx_http_request_t *r);
static ngx_int_t ram_cache_body_filter(ngx_http_request_t *r, ngx_chain_t *in);
static ngx_int_t ram_cache_make_key(ngx_http_request_t *r, ngx_str_t *key);
static ngx_int_t ram_cache_is_slice_subrequest(ngx_http_request_t *r);
static ngx_http_variable_value_t *ram_cache_slice_range(ngx_http_request_t *r);
static void ram_cache_unpin(void *data);
static void ram_cache_release_capture(void *data);
static void ram_cache_finalize_capture(void *data);
static void ram_cache_store_capture(ngx_http_ram_cache_ctx_t *ctx);
static void ram_cache_reap_captures_locked(void);
static ngx_int_t ram_cache_cacheable(ngx_http_request_t *r);
static ngx_int_t ram_cache_request_allowed(ngx_http_request_t *r,
    ngx_flag_t slice_request);
static void *ram_cache_create_loc_conf(ngx_conf_t *cf);
static char *ram_cache_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child);

static ngx_command_t ram_cache_commands[] = {
    { ngx_string("ram_cache_zone"), NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE2,
      ram_cache_zone, NGX_HTTP_MAIN_CONF_OFFSET, 0, NULL },
    { ngx_string("ram_cache_max_object"), NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_size_slot, NGX_HTTP_MAIN_CONF_OFFSET,
      offsetof(ngx_http_ram_cache_main_conf_t, max_object), NULL },
    { ngx_string("ram_cache_ttl"), NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_num_slot, NGX_HTTP_MAIN_CONF_OFFSET,
      offsetof(ngx_http_ram_cache_main_conf_t, ttl), NULL },
    { ngx_string("ram_cache"), NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_flag_slot, NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_ram_cache_loc_conf_t, enabled), NULL },
    { ngx_string("ram_cache_purge_token"), NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_str_slot, NGX_HTTP_MAIN_CONF_OFFSET,
      offsetof(ngx_http_ram_cache_main_conf_t, purge_token), NULL },
    ngx_null_command
};

static ngx_http_module_t ram_cache_module_ctx = {
    ram_cache_add_variables, ram_cache_init,
    ram_cache_create_main_conf, NULL, NULL, NULL,
    ram_cache_create_loc_conf, ram_cache_merge_loc_conf
};

ngx_module_t ngx_http_ram_cache_module = {
    NGX_MODULE_V1, &ram_cache_module_ctx, ram_cache_commands, NGX_HTTP_MODULE,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NGX_MODULE_V1_PADDING
};

static ngx_int_t
ram_cache_add_variables(ngx_conf_t *cf)
{
    static ngx_str_t names[] = {
        ngx_string("ram_cache_requests"),
        ngx_string("ram_cache_hits"),
        ngx_string("ram_cache_misses"),
        ngx_string("ram_cache_bypasses"),
        ngx_string("ram_cache_hit_bytes"),
        ngx_string("ram_cache_evictions"),
        ngx_string("ram_cache_expirations"),
        ngx_string("ram_cache_admission_accepted"),
        ngx_string("ram_cache_admission_rejected"),
        ngx_string("ram_cache_allocation_failures"),
        ngx_string("ram_cache_used_bytes"),
        ngx_string("ram_cache_building_bytes"),
        ngx_string("ram_cache_object_count"),
        ngx_string("ram_cache_building_count"),
        ngx_string("ram_cache_l2_hits"),
        ngx_string("ram_cache_l2_misses")
    };
    ngx_http_variable_t *var;
    ngx_uint_t i;

    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        var = ngx_http_add_variable(cf, &names[i], NGX_HTTP_VAR_NOCACHEABLE);
        if (var == NULL) return NGX_ERROR;
        var->get_handler = ram_cache_stat_variable;
        var->data = i;
    }
    return NGX_OK;
}


static ngx_int_t
ram_cache_stat_variable(ngx_http_request_t *r, ngx_http_variable_value_t *v,
    uintptr_t data)
{
    ngx_atomic_t value;
    u_char *p;

    if (ram_cache_conf == NULL || ram_cache_conf->sh == NULL) {
        v->not_found = 1;
        return NGX_OK;
    }

    switch (data) {
    case 0: value = ram_cache_conf->sh->requests; break;
    case 1: value = ram_cache_conf->sh->hits; break;
    case 2: value = ram_cache_conf->sh->misses; break;
    case 3: value = ram_cache_conf->sh->bypasses; break;
    case 4: value = ram_cache_conf->sh->hit_bytes; break;
    case 5: value = ram_cache_conf->sh->evictions; break;
    case 6: value = ram_cache_conf->sh->expirations; break;
    case 7: value = ram_cache_conf->sh->accepted; break;
    case 8: value = ram_cache_conf->sh->rejected; break;
    case 9: value = ram_cache_conf->sh->allocation_failures; break;
    case 10:
        ngx_shmtx_lock(&ram_cache_conf->shpool->mutex);
        value = ram_cache_conf->sh->used_bytes;
        ngx_shmtx_unlock(&ram_cache_conf->shpool->mutex);
        break;
    case 11: value = ram_cache_conf->sh->building_bytes; break;
    case 12: value = ram_cache_conf->sh->objects; break;
    case 13: value = ram_cache_conf->sh->building; break;
    case 14: value = ram_cache_conf->sh->l2_hits; break;
    case 15: value = ram_cache_conf->sh->l2_misses; break;
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


static ngx_int_t
ram_cache_add_status(ngx_http_request_t *r, const char *value, size_t len)
{
    ngx_table_elt_t *h;

    h = ngx_list_push(&r->headers_out.headers);
    if (h == NULL) return NGX_ERROR;
    ngx_str_set(&h->key, "X-RAM-Cache-Status");
    h->value.data = (u_char *) value;
    h->value.len = len;
    h->hash = 1;
    return NGX_OK;
}

static void *
ram_cache_create_main_conf(ngx_conf_t *cf)
{
    ngx_http_ram_cache_main_conf_t *conf;
    conf = ngx_pcalloc(cf->pool, sizeof(*conf));
    if (conf == NULL) return NULL;
    conf->max_object = NGX_CONF_UNSET_SIZE;
    conf->ttl = NGX_CONF_UNSET_UINT;
    return conf;
}

static void *
ram_cache_create_loc_conf(ngx_conf_t *cf)
{
    ngx_http_ram_cache_loc_conf_t *conf;
    conf = ngx_pcalloc(cf->pool, sizeof(*conf));
    if (conf == NULL) return NULL;
    conf->enabled = NGX_CONF_UNSET;
    return conf;
}

static char *
ram_cache_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_http_ram_cache_loc_conf_t *prev = parent;
    ngx_http_ram_cache_loc_conf_t *conf = child;
    ngx_conf_merge_value(conf->enabled, prev->enabled, 0);
    return NGX_CONF_OK;
}

static char *
ram_cache_zone(ngx_conf_t *cf, ngx_command_t *cmd, void *data)
{
    ngx_http_ram_cache_main_conf_t *conf = data;
    ngx_str_t *value = cf->args->elts;
    ssize_t size;
    if (conf->zone != NULL) return "is duplicate";
    size = ngx_parse_size(&value[2]);
    if (size == NGX_ERROR || size < (ssize_t) (8 * ngx_pagesize))
        return "invalid shared memory zone size";
    conf->zone = ngx_shared_memory_add(cf, &value[1], size,
                                       &ngx_http_ram_cache_module);
    if (conf->zone == NULL) return NGX_CONF_ERROR;
    conf->zone->init = ram_cache_zone_init;
    conf->zone->data = conf;
    return NGX_CONF_OK;
}

static ngx_int_t
ram_cache_zone_init(ngx_shm_zone_t *zone, void *old_data)
{
    ngx_http_ram_cache_main_conf_t *conf = zone->data;
    ngx_slab_pool_t *shpool = (ngx_slab_pool_t *) zone->shm.addr;
    ngx_http_ram_cache_main_conf_t *old_conf = old_data;
    ngx_http_ram_cache_sh_t *sh;
    if (old_conf != NULL) {
        conf->sh = old_conf->sh;
        conf->shpool = shpool;
        return NGX_OK;
    }
    sh = ngx_slab_alloc(shpool, sizeof(*sh));
    if (sh == NULL) return NGX_ERROR;
    ngx_queue_init(&sh->probation);
    ngx_queue_init(&sh->protected);
    ngx_queue_init(&sh->captures);
    sh->building = 0;
    sh->building_bytes = 0;
    sh->generation = 1;
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
    sh->used_bytes = 0;
    sh->protected_bytes = 0;
    shpool->data = sh;
    conf->sh = sh;
    conf->shpool = shpool;
    return NGX_OK;
}

static ngx_int_t
ram_cache_init(ngx_conf_t *cf)
{
    ngx_http_core_main_conf_t *cmcf;
    ngx_http_handler_pt *handler;
    ngx_http_ram_cache_main_conf_t *conf;
    conf = ngx_http_conf_get_module_main_conf(cf, ngx_http_ram_cache_module);
    ngx_conf_init_size_value(conf->max_object, RAM_MAX_OBJECT);
    ngx_conf_init_uint_value(conf->ttl, 300);
    if (conf->zone == NULL) return NGX_OK;
    if (conf->max_object > RAM_MAX_OBJECT) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "ram_cache_max_object cannot exceed 8m");
        return NGX_ERROR;
    }
    ram_cache_conf = conf;
    cmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_core_module);
    handler = ngx_array_push(&cmcf->phases[NGX_HTTP_PRECONTENT_PHASE].handlers);
    if (handler == NULL) return NGX_ERROR;
    *handler = ram_cache_handler;
    ram_cache_next_header_filter = ngx_http_top_header_filter;
    ngx_http_top_header_filter = ram_cache_header_filter;
    ram_cache_next_body_filter = ngx_http_top_body_filter;
    ngx_http_top_body_filter = ram_cache_body_filter;
    return NGX_OK;
}

static ngx_int_t
ram_cache_make_key(ngx_http_request_t *r, ngx_str_t *key)
{
    size_t host_len = r->headers_in.host ? r->headers_in.host->value.len : 0;
    size_t args_len = r->args.len;
    size_t scheme_len, slice_len, len, i;
    ngx_list_part_t *part;
    ngx_table_elt_t *h;
    ngx_http_variable_value_t *slice_value;
    u_char *p, *scheme;

    slice_len = 0;
    slice_value = ram_cache_slice_range(r);
    if (slice_value != NULL && !slice_value->not_found) {
        slice_len = slice_value->len;
    }

    scheme = (u_char *) "http";
    scheme_len = sizeof("http") - 1;
#if (NGX_SSL)
    if (r->connection->ssl != NULL) {
        scheme = (u_char *) "https";
        scheme_len = sizeof("https") - 1;
    }
#endif

    len = scheme_len + host_len + r->uri.len + args_len + slice_len + 4;
    part = &r->headers_in.headers.part;
    h = part->elts;
    for (;;) {
        for (i = 0; i < part->nelts; i++) {
            if (h[i].hash
                && !(slice_len && RAM_HEADER(h[i], "Range"))
                && !RAM_HEADER(h[i], "X-RAM-Cache-Purge"))
                len += h[i].key.len + h[i].value.len + 2;
        }
        if (part->next == NULL) break;
        part = part->next; h = part->elts;
    }
    key->len = len;
    key->data = ngx_pnalloc(r->pool, key->len);
    if (key->data == NULL) return NGX_ERROR;
    p = key->data;
    p = ngx_cpymem(p, scheme, scheme_len);
    *p++ = '|';
    if (host_len) p = ngx_cpymem(p, r->headers_in.host->value.data, host_len);
    *p++ = '|';
    p = ngx_cpymem(p, r->uri.data, r->uri.len);
    *p++ = '?';
    if (args_len) p = ngx_cpymem(p, r->args.data, args_len);
    *p++ = '|';
    if (slice_len) p = ngx_cpymem(p, slice_value->data, slice_len);
    part = &r->headers_in.headers.part;
    h = part->elts;
    for (;;) {
        for (i = 0; i < part->nelts; i++) {
            if (h[i].hash
                && !(slice_len && RAM_HEADER(h[i], "Range"))
                && !RAM_HEADER(h[i], "X-RAM-Cache-Purge")) {
                *p++ = '\n'; p = ngx_cpymem(p, h[i].key.data, h[i].key.len);
                *p++ = ':'; p = ngx_cpymem(p, h[i].value.data, h[i].value.len);
            }
        }
        if (part->next == NULL) break;
        part = part->next; h = part->elts;
    }
    return NGX_OK;
}


static ngx_int_t
ram_cache_is_slice_subrequest(ngx_http_request_t *r)
{
    return r != r->main
           && ngx_http_get_module_ctx(r, ngx_http_slice_filter_module) != NULL;
}


static ngx_http_variable_value_t *
ram_cache_slice_range(ngx_http_request_t *r)
{
    return ngx_http_get_variable(
        r, &ram_cache_slice_range_name,
        ngx_hash_key_lc(ram_cache_slice_range_name.data,
                        ram_cache_slice_range_name.len));
}

static ngx_http_ram_cache_entry_t *
ram_cache_find(ngx_str_t *key)
{
    ngx_queue_t *q;
    ngx_http_ram_cache_entry_t *entry;
    ngx_queue_t *queues[2];
    ngx_uint_t i;

    queues[0] = &ram_cache_conf->sh->probation;
    queues[1] = &ram_cache_conf->sh->protected;
    for (i = 0; i < 2; i++) {
        for (q = ngx_queue_head(queues[i]);
             q != ngx_queue_sentinel(queues[i]); q = ngx_queue_next(q))
        {
            entry = ngx_queue_data(q, ngx_http_ram_cache_entry_t, queue);
            if (entry->state == RAM_ENTRY_READY
                && entry->key_len == key->len
                && ngx_memcmp(entry->data, key->data, key->len) == 0)
                return entry;
        }
    }
    return NULL;
}


static size_t
ram_cache_blocks_offset(ngx_http_ram_cache_entry_t *entry)
{
    size_t n = entry->key_len + entry->type_len + entry->etag_len
               + entry->modified_len + entry->ranges_len
               + entry->content_range_len;
    return (n + sizeof(void *) - 1) & ~(sizeof(void *) - 1);
}


static u_char **
ram_cache_blocks(ngx_http_ram_cache_entry_t *entry)
{
    return (u_char **) (entry->data + ram_cache_blocks_offset(entry));
}


static void
ram_cache_free_entry_locked(ngx_http_ram_cache_entry_t *entry)
{
    ngx_uint_t i;
    u_char **blocks;

    blocks = ram_cache_blocks(entry);
    for (i = 0; i < entry->block_count; i++) {
        if (blocks[i] != NULL) {
            ngx_slab_free_locked(ram_cache_conf->shpool, blocks[i]);
        }
    }

    if (entry->accounted) {
        entry->accounted = 0;
        if (entry->protected) {
            ram_cache_conf->sh->protected_bytes -= entry->alloc_bytes;
        }
        ram_cache_conf->sh->used_bytes -= entry->alloc_bytes;
        (void) ngx_atomic_fetch_add(&ram_cache_conf->sh->objects, -1);
    }

    ngx_slab_free_locked(ram_cache_conf->shpool, entry);
}


static ngx_int_t
ram_cache_evict_one_locked(void)
{
    ngx_queue_t *queues[2], *q, *sentinel;
    ngx_http_ram_cache_entry_t *entry;
    ngx_uint_t i;

    queues[0] = &ram_cache_conf->sh->probation;
    queues[1] = &ram_cache_conf->sh->protected;
    for (i = 0; i < 2; i++) {
        sentinel = ngx_queue_sentinel(queues[i]);
        for (q = ngx_queue_last(queues[i]); q != sentinel;
             q = ngx_queue_prev(q))
        {
            entry = ngx_queue_data(q, ngx_http_ram_cache_entry_t, queue);
            if (entry->refs != 0) {
                continue;
            }
            ngx_queue_remove(q);
            entry->state = RAM_ENTRY_EVICTING;
            ram_cache_free_entry_locked(entry);
            (void) ngx_atomic_fetch_add(&ram_cache_conf->sh->evictions, 1);
            return NGX_OK;
        }
    }
    return NGX_DECLINED;
}


static void
ram_cache_protect_locked(ngx_http_ram_cache_entry_t *entry)
{
    ngx_queue_t *q;
    ngx_http_ram_cache_entry_t *oldest;
    size_t cap = ram_cache_conf->zone->shm.size * 80 / 100;

    if (entry->protected) {
        ngx_queue_remove(&entry->queue);
        ngx_queue_insert_head(&ram_cache_conf->sh->protected, &entry->queue);
        return;
    }

    ngx_queue_remove(&entry->queue);
    entry->protected = 1;
    ram_cache_conf->sh->protected_bytes += entry->alloc_bytes;
    ngx_queue_insert_head(&ram_cache_conf->sh->protected, &entry->queue);

    while (ram_cache_conf->sh->protected_bytes > cap
           && !ngx_queue_empty(&ram_cache_conf->sh->protected))
    {
        q = ngx_queue_last(&ram_cache_conf->sh->protected);
        oldest = ngx_queue_data(q, ngx_http_ram_cache_entry_t, queue);
        ngx_queue_remove(q);
        oldest->protected = 0;
        ram_cache_conf->sh->protected_bytes -= oldest->alloc_bytes;
        ngx_queue_insert_head(&ram_cache_conf->sh->probation, &oldest->queue);
    }
}

static ngx_int_t
ram_cache_handler(ngx_http_request_t *r)
{
    static u_char purge_method[] = "PURGE";
    ngx_str_t key;
    ngx_http_ram_cache_entry_t *entry;
    ngx_pool_cleanup_t *cleanup;
    ngx_table_elt_t *status;
    ngx_buf_t *b;
    ngx_chain_t *out, *cl, **last;
    u_char *type, *etag, *modified, *ranges, *content_range, **blocks;
    size_t offset, n;
    time_t now;
    ngx_uint_t i;
    ngx_http_ram_cache_loc_conf_t *lcf;
    ngx_http_ram_cache_ctx_t *ctx;
    ngx_int_t slice;
    ngx_http_variable_value_t *slice_value;
    lcf = ngx_http_get_module_loc_conf(r, ngx_http_ram_cache_module);
    if (r->method_name.len == sizeof(purge_method) - 1
        && ngx_strncasecmp(r->method_name.data, purge_method,
                           sizeof(purge_method) - 1) == 0)
        return ram_cache_purge_request(r, &ram_cache_conf->purge_token);
    slice_value = ram_cache_slice_range(r);
    slice = slice_value != NULL && !slice_value->not_found;
    if (!lcf->enabled || ram_cache_conf == NULL || ram_cache_conf->sh == NULL)
        return NGX_DECLINED;
    (void) ngx_atomic_fetch_add(&ram_cache_conf->sh->requests, 1);
    if ((r != r->main && !slice)
        || !ram_cache_request_allowed(r->main, slice) || r->header_only)
    {
        (void) ngx_atomic_fetch_add(&ram_cache_conf->sh->bypasses, 1);
        return NGX_DECLINED;
    }
    if (ram_cache_make_key(r, &key) != NGX_OK) return NGX_DECLINED;
    ngx_shmtx_lock(&ram_cache_conf->shpool->mutex);
    entry = ram_cache_find(&key);
    if (entry == NULL) {
        ngx_shmtx_unlock(&ram_cache_conf->shpool->mutex);
        (void) ngx_atomic_fetch_add(&ram_cache_conf->sh->misses, 1);
        return NGX_DECLINED;
    }
    if (entry->body_len > ram_cache_conf->max_object) {
        ngx_queue_remove(&entry->queue);
        entry->state = RAM_ENTRY_EXPIRED;
        (void) ngx_atomic_fetch_add(&ram_cache_conf->sh->rejected, 1);
        if (entry->refs == 0) {
            ram_cache_free_entry_locked(entry);
        }
        ngx_shmtx_unlock(&ram_cache_conf->shpool->mutex);
        (void) ngx_atomic_fetch_add(&ram_cache_conf->sh->misses, 1);
        return NGX_DECLINED;
    }
    now = ngx_time();
    if (entry->expires <= now || now < entry->created
        || (ngx_uint_t) (now - entry->created) >= ram_cache_conf->ttl)
    {
        ngx_queue_remove(&entry->queue);
        entry->state = RAM_ENTRY_EXPIRED;
        (void) ngx_atomic_fetch_add(&ram_cache_conf->sh->expirations, 1);
        if (entry->refs == 0) {
            ram_cache_free_entry_locked(entry);
        }
        ngx_shmtx_unlock(&ram_cache_conf->shpool->mutex);
        (void) ngx_atomic_fetch_add(&ram_cache_conf->sh->misses, 1);
        return NGX_DECLINED;
    }
    entry->refs++;
    ram_cache_protect_locked(entry);
    (void) ngx_atomic_fetch_add(&ram_cache_conf->sh->hits, 1);
    (void) ngx_atomic_fetch_add(&ram_cache_conf->sh->hit_bytes,
                                entry->body_len);
    type = entry->data + entry->key_len;
    etag = type + entry->type_len;
    modified = etag + entry->etag_len;
    ranges = modified + entry->modified_len;
    content_range = ranges + entry->ranges_len;
    blocks = ram_cache_blocks(entry);
    ngx_shmtx_unlock(&ram_cache_conf->shpool->mutex);
    cleanup = ngx_pool_cleanup_add(r->pool, sizeof(entry));
    if (cleanup == NULL) { ram_cache_unpin(&entry); return NGX_HTTP_INTERNAL_SERVER_ERROR; }
    *(ngx_http_ram_cache_entry_t **) cleanup->data = entry;
    cleanup->handler = ram_cache_unpin;
    ctx = ngx_pcalloc(r->pool, sizeof(*ctx));
    if (ctx == NULL) return NGX_HTTP_INTERNAL_SERVER_ERROR;
    ctx->hit = 1;
    ngx_http_set_ctx(r, ctx, ngx_http_ram_cache_module);
    r->headers_out.status = entry->status;
    r->headers_out.content_length_n = entry->body_len;
    r->headers_out.content_type.data = type;
    r->headers_out.content_type.len = entry->type_len;
    r->headers_out.content_type_len = entry->type_len;
    status = ngx_list_push(&r->headers_out.headers);
    if (status == NULL) return NGX_HTTP_INTERNAL_SERVER_ERROR;
    ngx_str_set(&status->key, "X-RAM-Cache");
    ngx_str_set(&status->value, "HIT");
    status->hash = 1;
    if (ram_cache_add_status(r, "L1_HIT", sizeof("L1_HIT") - 1) != NGX_OK)
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    if (entry->etag_len) {
        status = ngx_list_push(&r->headers_out.headers);
        if (status == NULL) return NGX_HTTP_INTERNAL_SERVER_ERROR;
        ngx_str_set(&status->key, "ETag");
        status->value.data = etag; status->value.len = entry->etag_len;
        status->hash = 1;
    }
    if (entry->modified_len) {
        status = ngx_list_push(&r->headers_out.headers);
        if (status == NULL) return NGX_HTTP_INTERNAL_SERVER_ERROR;
        ngx_str_set(&status->key, "Last-Modified");
        status->value.data = modified; status->value.len = entry->modified_len;
        status->hash = 1;
    }
    if (entry->ranges_len) {
        status = ngx_list_push(&r->headers_out.headers);
        if (status == NULL) return NGX_HTTP_INTERNAL_SERVER_ERROR;
        ngx_str_set(&status->key, "Accept-Ranges");
        status->value.data = ranges; status->value.len = entry->ranges_len;
        status->hash = 1;
    }
    if (entry->content_range_len) {
        status = ngx_list_push(&r->headers_out.headers);
        if (status == NULL) return NGX_HTTP_INTERNAL_SERVER_ERROR;
        ngx_str_set(&status->key, "Content-Range");
        status->value.data = content_range;
        status->value.len = entry->content_range_len;
        status->hash = 1;
        r->headers_out.content_range = status;
    }
    if (ngx_http_send_header(r) == NGX_ERROR) return NGX_ERROR;
    out = NULL;
    last = &out;
    offset = 0;
    for (i = 0; i < entry->block_count; i++) {
        n = ngx_min((size_t) RAM_BLOCK_SIZE, entry->body_len - offset);
        cl = ngx_alloc_chain_link(r->pool);
        b = ngx_calloc_buf(r->pool);
        if (cl == NULL || b == NULL) return NGX_HTTP_INTERNAL_SERVER_ERROR;
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
    ngx_int_t rc = ngx_http_output_filter(r, out);
    ngx_http_finalize_request(r, rc);
    return NGX_DONE;
}

static ngx_int_t
ram_cache_request_allowed(ngx_http_request_t *r, ngx_flag_t slice_request)
{
    ngx_list_part_t *part = &r->headers_in.headers.part;
    ngx_table_elt_t *h = part->elts;
    ngx_uint_t i;
    if (r->method != NGX_HTTP_GET || r->headers_in.host == NULL
        || r->headers_in.content_length_n > 0 || r->headers_in.chunked
        || r->headers_in.authorization
        || r->headers_in.cookie
        || (r->headers_in.range && !slice_request) || r->headers_in.if_range
        || r->headers_in.if_none_match || r->headers_in.if_modified_since
        || r->headers_in.if_match || r->headers_in.if_unmodified_since)
        return 0;
    for (;;) {
        for (i = 0; i < part->nelts; i++) {
            if (!h[i].hash) continue;
            if (RAM_HEADER(h[i], "Cache-Control") || RAM_HEADER(h[i], "Pragma")
                || RAM_HEADER(h[i], "Proxy-Authorization")
                || RAM_HEADER(h[i], "Cookie2")
                || RAM_HEADER(h[i], "X-RAM-Cache-Purge"))
                return 0;
        }
        if (part->next == NULL) break;
        part = part->next; h = part->elts;
    }
    return 1;
}

static ngx_int_t
ram_cache_purge_request(ngx_http_request_t *r,
    ngx_str_t *token)
{
    static u_char purge_header[] = "X-RAM-Cache-Purge";
    ngx_list_part_t *part;
    ngx_table_elt_t *h;
    ngx_uint_t i, j, authorized, diff;
    ngx_queue_t *queues[2], *q, *next;
    ngx_http_ram_cache_entry_t *entry;
    ngx_str_t key;
    ngx_int_t rc;

    if (ram_cache_conf == NULL || ram_cache_conf->sh == NULL)
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

    ngx_shmtx_lock(&ram_cache_conf->shpool->mutex);
    (void) ngx_atomic_fetch_add(&ram_cache_conf->sh->generation, 1);
    if (r->uri.len == 1 && r->uri.data[0] == '/' && r->args.len == 0) {
        queues[0] = &ram_cache_conf->sh->probation;
        queues[1] = &ram_cache_conf->sh->protected;
        for (i = 0; i < 2; i++) {
            for (q = ngx_queue_head(queues[i]);
                 q != ngx_queue_sentinel(queues[i]); q = next)
            {
                next = ngx_queue_next(q);
                entry = ngx_queue_data(q, ngx_http_ram_cache_entry_t, queue);
                ngx_queue_remove(q);
                entry->state = RAM_ENTRY_EXPIRED;
                if (entry->refs == 0) ram_cache_free_entry_locked(entry);
            }
        }
    } else if (ram_cache_make_key(r, &key) == NGX_OK) {
        entry = ram_cache_find(&key);
        if (entry != NULL) {
            ngx_queue_remove(&entry->queue);
            entry->state = RAM_ENTRY_EXPIRED;
            if (entry->refs == 0) ram_cache_free_entry_locked(entry);
        }
    }
    ngx_shmtx_unlock(&ram_cache_conf->shpool->mutex);

    r->headers_out.status = NGX_HTTP_NO_CONTENT;
    r->headers_out.content_length_n = 0;
    r->header_only = 1;
    rc = ngx_http_send_header(r);
    ngx_http_finalize_request(r, rc);
    return NGX_DONE;
}


static ngx_int_t
ram_cache_parse_slice_range(ngx_http_request_t *r, off_t *start,
    off_t *requested_end)
{
    ngx_http_variable_value_t *v;
    u_char *dash, *slash;

    v = ram_cache_slice_range(r);
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
ram_cache_slice_segment(ngx_http_request_t *r, off_t *start, off_t *end,
    off_t *complete)
{
    off_t requested_start, requested_end;

    if (ram_cache_parse_slice_range(r, &requested_start, &requested_end)
        != NGX_OK
        || r->headers_out.content_length_n <= 0
        || r->headers_out.content_offset != requested_start
        || requested_start >= r->headers_out.content_length_n)
        return NGX_ERROR;

    *start = requested_start;
    *complete = r->headers_out.content_length_n;
    *end = ngx_min(requested_end, *complete - 1);
    if (*end < *start) return NGX_ERROR;
    return NGX_OK;
}


static ngx_int_t
ram_cache_cacheable(ngx_http_request_t *r)
{
    ngx_list_part_t *part;
    ngx_table_elt_t *h;
    ngx_uint_t i;
    ngx_int_t slice;
    off_t start, end, complete;
    ngx_table_elt_t *etag;
    ngx_http_variable_value_t *slice_value;

    slice_value = ram_cache_slice_range(r);
    slice = slice_value != NULL && !slice_value->not_found;
    if (r->cache == NULL
        || (!slice && r->headers_out.status != NGX_HTTP_OK)
        || (slice && r->headers_out.status != NGX_HTTP_OK)
        || r->headers_out.content_length_n < 0
        || r->headers_out.content_type.len == 0
        || r->headers_out.content_type.len > 128
        || r->headers_in.authorization || r->headers_in.cookie
        || (r->headers_in.range && !slice) || r->headers_in.if_range
        || r->headers_in.if_none_match || r->headers_in.if_modified_since
        || r->headers_in.if_match || r->headers_in.if_unmodified_since)
        return 0;

    if (slice) {
        etag = r->headers_out.etag;
        if (ram_cache_slice_segment(r, &start, &end, &complete) != NGX_OK
            || (size_t) (end - start + 1) > ram_cache_conf->max_object
            || etag == NULL || etag->value.len == 0
            || (etag->value.len >= 2
                && ngx_strncmp(etag->value.data, "W/", 2) == 0))
            return 0;
    } else if ((size_t) r->headers_out.content_length_n
               > ram_cache_conf->max_object)
    {
        return 0;
    }
    part = &r->headers_out.headers.part;
    h = part->elts;
    for ( ;; ) {
        for (i = 0; i < part->nelts; i++) {
            if (h[i].hash == 0) continue;
            if (RAM_HEADER(h[i], "Set-Cookie") || RAM_HEADER(h[i], "Vary")
                || RAM_HEADER(h[i], "Content-Encoding")
                || (RAM_HEADER(h[i], "Content-Range") && !slice)
                || RAM_HEADER(h[i], "Cache-Control") || RAM_HEADER(h[i], "Expires"))
                return 0;
            if (!RAM_HEADER(h[i], "Date") && !RAM_HEADER(h[i], "Server")
                && !RAM_HEADER(h[i], "Accept-Ranges")
                && !RAM_HEADER(h[i], "ETag")
                && !RAM_HEADER(h[i], "Last-Modified"))
                return 0;
        }
        if (part->next == NULL) break;
        part = part->next;
        h = part->elts;
    }
    return 1;
}

static ngx_int_t
ram_cache_header_filter(ngx_http_request_t *r)
{
    ngx_http_ram_cache_ctx_t *ctx;
    ngx_http_ram_cache_loc_conf_t *lcf;
    ngx_int_t slice;
    ngx_int_t rc;
    ngx_http_variable_value_t *slice_value;
    const char *cache_status;
    size_t cache_status_len;

    lcf = ngx_http_get_module_loc_conf(r, ngx_http_ram_cache_module);
    slice_value = ram_cache_slice_range(r);
    slice = slice_value != NULL && !slice_value->not_found;
    ctx = ngx_http_get_module_ctx(r, ngx_http_ram_cache_module);
    if (ctx != NULL && ctx->hit) return ram_cache_next_header_filter(r);

    if (slice) {
        r->filter_need_in_memory = 1;
        r->main_filter_need_in_memory = 1;
    }
    if (lcf->enabled && (r == r->main || ram_cache_is_slice_subrequest(r))
        && ram_cache_request_allowed(r->main, slice)
        && ram_cache_cacheable(r))
    {
        size_t reserve;
        ngx_atomic_int_t reserved;
        off_t start, end, complete;
        u_char *content_range;

        ctx = ngx_pcalloc(r->pool, sizeof(*ctx));
        if (ctx != NULL) {
            reserve = (size_t) r->headers_out.content_length_n;
            if (slice) {
                if (ram_cache_slice_segment(r, &start, &end, &complete)
                    != NGX_OK)
                    return ram_cache_next_header_filter(r);
                reserve = (size_t) (end - start + 1);
                content_range = ngx_pnalloc(r->pool,
                    sizeof("bytes ") - 1 + 3 * NGX_OFF_T_LEN + 2);
                if (content_range == NULL) return ram_cache_next_header_filter(r);
                ctx->content_range.data = content_range;
                ctx->content_range.len = ngx_sprintf(content_range,
                    "bytes %O-%O/%O", start, end, complete) - content_range;
            }
            if (reserve == 0) {
                (void) ngx_atomic_fetch_add(&ram_cache_conf->sh->rejected, 1);
                return ram_cache_next_header_filter(r);
            }

            ngx_shmtx_lock(&ram_cache_conf->shpool->mutex);
            ram_cache_reap_captures_locked();
            reserved = ram_cache_conf->sh->building_bytes;
            if ((size_t) reserved + reserve
                > ram_cache_conf->zone->shm.size / 10)
            {
                ngx_shmtx_unlock(&ram_cache_conf->shpool->mutex);
                (void) ngx_atomic_fetch_add(&ram_cache_conf->sh->rejected, 1);
                return ram_cache_next_header_filter(r);
            }
            ctx->capture = ngx_slab_alloc_locked(ram_cache_conf->shpool,
                                                  sizeof(*ctx->capture));
            if (ctx->capture == NULL) {
                ngx_shmtx_unlock(&ram_cache_conf->shpool->mutex);
                (void) ngx_atomic_fetch_add(&ram_cache_conf->sh->rejected, 1);
                (void) ngx_atomic_fetch_add(
                    &ram_cache_conf->sh->allocation_failures, 1);
                return ram_cache_next_header_filter(r);
            }
            ctx->capture->pid = ngx_pid;
            ctx->capture->bytes = reserve;
            ngx_queue_insert_head(&ram_cache_conf->sh->captures,
                                  &ctx->capture->queue);
            (void) ngx_atomic_fetch_add(&ram_cache_conf->sh->building_bytes,
                                       reserve);
            (void) ngx_atomic_fetch_add(&ram_cache_conf->sh->building, 1);
            ngx_shmtx_unlock(&ram_cache_conf->shpool->mutex);
            ctx->reserved = 1;
            ctx->reserved_bytes = reserve;
            ngx_pool_cleanup_t *cleanup = ngx_pool_cleanup_add(r->pool, 0);
            if (cleanup != NULL) {
                cleanup->handler = ram_cache_finalize_capture;
                cleanup->data = ctx;
            }
            ctx->body = ngx_pnalloc(r->pool, reserve);
            if (cleanup != NULL && ctx->body != NULL
                && ram_cache_make_key(r, &ctx->key) == NGX_OK) {
                ctx->r = r;
                ctx->admit = 1;
                ctx->expected_len = reserve;
                ctx->generation = ram_cache_conf->sh->generation;
                ctx->response_status = slice ? NGX_HTTP_PARTIAL_CONTENT
                                              : r->headers_out.status;
                ctx->expires = ngx_time()
                               + (time_t) ram_cache_conf->ttl;
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
                ngx_http_set_ctx(r, ctx, ngx_http_ram_cache_module);
            } else {
                ram_cache_release_capture(ctx);
            }
        }
    }

    if (lcf->enabled) {
        cache_status = "BYPASS";
        cache_status_len = sizeof("BYPASS") - 1;
        if (r->upstream != NULL
            && r->upstream->cache_status == NGX_HTTP_CACHE_HIT)
        {
            (void) ngx_atomic_fetch_add(&ram_cache_conf->sh->l2_hits, 1);
        } else if (r->upstream != NULL
                   && r->upstream->cache_status == NGX_HTTP_CACHE_MISS)
        {
            (void) ngx_atomic_fetch_add(&ram_cache_conf->sh->l2_misses, 1);
        }
        if (ctx != NULL && ctx->admit) {
            if (r->upstream != NULL
                && r->upstream->cache_status == NGX_HTTP_CACHE_HIT)
            {
                cache_status = "L1_MISS_L2_HIT";
                cache_status_len = sizeof("L1_MISS_L2_HIT") - 1;
            } else if (r->upstream != NULL
                       && r->upstream->cache_status == NGX_HTTP_CACHE_MISS)
            {
                cache_status = "L1_MISS_L2_MISS";
                cache_status_len = sizeof("L1_MISS_L2_MISS") - 1;
            } else {
                cache_status = "L1_MISS";
                cache_status_len = sizeof("L1_MISS") - 1;
            }
        }
        if (ram_cache_add_status(r, cache_status, cache_status_len) != NGX_OK)
            return NGX_ERROR;
    }
    rc = ram_cache_next_header_filter(r);
    if (rc == NGX_ERROR && ctx != NULL && ctx->admit) {
        ctx->admit = 0;
        ram_cache_release_capture(ctx);
    }
    return rc;
}

static ngx_int_t
ram_cache_body_filter(ngx_http_request_t *r, ngx_chain_t *in)
{
    ngx_http_ram_cache_ctx_t *ctx;
    ngx_chain_t *cl;
    size_t n;
    ngx_uint_t complete;

    if (r != r->main && !ram_cache_is_slice_subrequest(r))
        return ram_cache_next_body_filter(r, in);
    ctx = ngx_http_get_module_ctx(r, ngx_http_ram_cache_module);
    if (ctx == NULL || !ctx->admit || ctx->complete)
        return ram_cache_next_body_filter(r, in);

    complete = 0;
    for (cl = in; cl != NULL; cl = cl->next) {
        if (cl->buf->in_file || !ngx_buf_in_memory(cl->buf)) {
            ctx->admit = 0;
            ram_cache_release_capture(ctx);
            break;
        }
        n = cl->buf->last - cl->buf->pos;
        if (ctx->len > ctx->reserved_bytes
            || n > ctx->reserved_bytes - ctx->len)
        {
            ctx->admit = 0;
            ram_cache_release_capture(ctx);
            break;
        }
        if (n) ngx_memcpy(ctx->body + ctx->len, cl->buf->pos, n);
        ctx->len += n;
        if (cl->buf->last_buf || cl->buf->last_in_chain
            || ctx->len == ctx->expected_len)
            complete = 1;
    }

    if (ctx->admit && complete) {
        if (ctx->len != ctx->expected_len || ctx->len == 0) {
            ctx->admit = 0;
            ram_cache_release_capture(ctx);
        } else {
            ctx->complete = 1;
        }
    }
    return ram_cache_next_body_filter(r, in);
}


static void
ram_cache_finalize_capture(void *data)
{
    ngx_http_ram_cache_ctx_t *ctx = data;

    if (ctx->admit && ctx->complete) {
        ram_cache_store_capture(ctx);
    }
    ram_cache_release_capture(ctx);
}


static void
ram_cache_store_capture(ngx_http_ram_cache_ctx_t *ctx)
{
    ngx_http_request_t *r = ctx->r;
    ngx_http_ram_cache_entry_t *entry;
    size_t entry_alloc, metadata_len, copy_offset, block_len;
    size_t required, usable;
    ngx_uint_t i, block_count, allocation_failed;
    u_char *p, **blocks;

    if (r->cache == NULL || r->cache->node == NULL
        || !r->cache->node->exists || r->cache->valid_sec <= ngx_time())
    {
        ctx->admit = 0;
        (void) ngx_atomic_fetch_add(&ram_cache_conf->sh->rejected, 1);
        return;
    }
    ctx->expires = ngx_min(ctx->expires, r->cache->valid_sec);

    block_count = (ctx->len + RAM_BLOCK_SIZE - 1) / RAM_BLOCK_SIZE;
    metadata_len = ctx->key.len + ctx->content_type.len + ctx->etag.len
                   + ctx->last_modified.len + ctx->accept_ranges.len
                   + ctx->content_range.len;
    entry_alloc = offsetof(ngx_http_ram_cache_entry_t, data)
                  + ((metadata_len + sizeof(void *) - 1)
                     & ~(sizeof(void *) - 1))
                  + block_count * sizeof(u_char *);

    ngx_shmtx_lock(&ram_cache_conf->shpool->mutex);
    if (ram_cache_find(&ctx->key) != NULL) {
        ngx_shmtx_unlock(&ram_cache_conf->shpool->mutex);
        ctx->admit = 0;
        return;
    }

    entry = NULL;
    allocation_failed = 0;
    required = entry_alloc + ctx->len;
    usable = ram_cache_conf->zone->shm.size * 80 / 100;
    while (required <= usable
           && ram_cache_conf->sh->used_bytes > usable - required)
    {
        if (ram_cache_evict_one_locked() != NGX_OK) break;
    }

    if (required <= usable
        && ram_cache_conf->sh->used_bytes <= usable - required)
    {
        for ( ;; ) {
            entry = ngx_slab_alloc_locked(ram_cache_conf->shpool, entry_alloc);
            if (entry == NULL) {
                allocation_failed = 1;
                if (ram_cache_evict_one_locked() == NGX_OK) continue;
                break;
            }

            ngx_memzero(entry, entry_alloc);
            entry->state = RAM_ENTRY_BUILDING;
            entry->key_len = ctx->key.len;
            entry->type_len = ctx->content_type.len;
            entry->etag_len = ctx->etag.len;
            entry->modified_len = ctx->last_modified.len;
            entry->ranges_len = ctx->accept_ranges.len;
            entry->content_range_len = ctx->content_range.len;
            entry->body_len = ctx->len;
            entry->block_count = block_count;
            entry->alloc_bytes = required;
            entry->status = ctx->response_status;
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
            blocks = ram_cache_blocks(entry);

            copy_offset = 0;
            for (i = 0; i < block_count; i++) {
                block_len = ngx_min((size_t) RAM_BLOCK_SIZE,
                                    ctx->len - copy_offset);
                blocks[i] = ngx_slab_alloc_locked(ram_cache_conf->shpool,
                                                   block_len);
                if (blocks[i] == NULL) {
                    allocation_failed = 1;
                    break;
                }
                copy_offset += block_len;
            }
            if (i == block_count) break;

            ram_cache_free_entry_locked(entry);
            entry = NULL;
            if (ram_cache_evict_one_locked() != NGX_OK) break;
        }
    }

    if (entry == NULL) {
        ngx_shmtx_unlock(&ram_cache_conf->shpool->mutex);
        (void) ngx_atomic_fetch_add(&ram_cache_conf->sh->rejected, 1);
        if (allocation_failed) {
            (void) ngx_atomic_fetch_add(
                &ram_cache_conf->sh->allocation_failures, 1);
        }
        ctx->admit = 0;
        return;
    }
    blocks = ram_cache_blocks(entry);
    ngx_shmtx_unlock(&ram_cache_conf->shpool->mutex);

    copy_offset = 0;
    for (i = 0; i < block_count; i++) {
        block_len = ngx_min((size_t) RAM_BLOCK_SIZE,
                            ctx->len - copy_offset);
        ngx_memcpy(blocks[i], ctx->body + copy_offset, block_len);
        copy_offset += block_len;
    }

    ngx_shmtx_lock(&ram_cache_conf->shpool->mutex);
    if (ram_cache_find(&ctx->key) == NULL
        && ctx->generation == ram_cache_conf->sh->generation
        && r->cache->node->exists && r->cache->valid_sec > ngx_time())
    {
        entry->state = RAM_ENTRY_READY;
        entry->expires = ctx->expires;
        entry->created = ngx_time();
        entry->accounted = 1;
        ram_cache_conf->sh->used_bytes += entry->alloc_bytes;
        (void) ngx_atomic_fetch_add(&ram_cache_conf->sh->objects, 1);
        ngx_queue_insert_head(&ram_cache_conf->sh->probation, &entry->queue);
        (void) ngx_atomic_fetch_add(&ram_cache_conf->sh->accepted, 1);
    } else {
        entry->state = RAM_ENTRY_BUILDING;
        ram_cache_free_entry_locked(entry);
    }
    ngx_shmtx_unlock(&ram_cache_conf->shpool->mutex);
}


static void
ram_cache_unpin(void *data)
{
    ngx_http_ram_cache_entry_t *entry = *(ngx_http_ram_cache_entry_t **) data;
    ngx_shmtx_lock(&ram_cache_conf->shpool->mutex);
    if (entry->refs != 0) {
        entry->refs--;
    }
    if (entry->refs == 0 && entry->state == RAM_ENTRY_EXPIRED) {
        ram_cache_free_entry_locked(entry);
    }
    ngx_shmtx_unlock(&ram_cache_conf->shpool->mutex);
}

static void
ram_cache_release_capture(void *data)
{
    ngx_http_ram_cache_ctx_t *ctx = data;
    if (ctx->reserved) {
        ctx->reserved = 0;
        ngx_shmtx_lock(&ram_cache_conf->shpool->mutex);
        ngx_queue_remove(&ctx->capture->queue);
        (void) ngx_atomic_fetch_add(&ram_cache_conf->sh->building, -1);
        (void) ngx_atomic_fetch_add(&ram_cache_conf->sh->building_bytes,
                                   -((ngx_atomic_int_t) ctx->reserved_bytes));
        ngx_slab_free_locked(ram_cache_conf->shpool, ctx->capture);
        ctx->capture = NULL;
        ngx_shmtx_unlock(&ram_cache_conf->shpool->mutex);
    }
}


static void
ram_cache_reap_captures_locked(void)
{
    ngx_queue_t *q, *next;
    ngx_http_ram_cache_capture_t *capture;

    for (q = ngx_queue_head(&ram_cache_conf->sh->captures);
         q != ngx_queue_sentinel(&ram_cache_conf->sh->captures); q = next)
    {
        next = ngx_queue_next(q);
        capture = ngx_queue_data(q, ngx_http_ram_cache_capture_t, queue);
        if (kill(capture->pid, 0) != -1 || ngx_errno != NGX_ESRCH) continue;
        ngx_queue_remove(q);
        (void) ngx_atomic_fetch_add(&ram_cache_conf->sh->building, -1);
        (void) ngx_atomic_fetch_add(&ram_cache_conf->sh->building_bytes,
                                   -((ngx_atomic_int_t) capture->bytes));
        ngx_slab_free_locked(ram_cache_conf->shpool, capture);
    }
}
