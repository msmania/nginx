
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#include <ngx_crypt.h>


#define NGX_HTTP_AUTH_BUF_SIZE  2048


typedef struct {
    ngx_http_complex_value_t  *realm;
    ngx_http_complex_value_t   user_file;
    ngx_chain_t cache;
} ngx_http_auth_basic_loc_conf_t;


static ngx_int_t ngx_http_auth_basic_handler(ngx_http_request_t *r);
static ngx_int_t ngx_http_auth_basic_crypt_handler(ngx_http_request_t *r,
    ngx_str_t *passwd, ngx_str_t *realm);
static ngx_int_t ngx_http_auth_basic_set_realm(ngx_http_request_t *r,
    ngx_str_t *realm);
static void *ngx_http_auth_basic_create_loc_conf(ngx_conf_t *cf);
static char *ngx_http_auth_basic_merge_loc_conf(ngx_conf_t *cf,
    void *parent, void *child);
static ngx_int_t ngx_http_auth_basic_init(ngx_conf_t *cf);
static char *ngx_http_auth_basic_user_file(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);


static ngx_command_t  ngx_http_auth_basic_commands[] = {

    { ngx_string("auth_basic"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_HTTP_LMT_CONF
                        |NGX_CONF_TAKE1,
      ngx_http_set_complex_value_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_auth_basic_loc_conf_t, realm),
      NULL },

    { ngx_string("auth_basic_user_file"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_HTTP_LMT_CONF
                        |NGX_CONF_TAKE1,
      ngx_http_auth_basic_user_file,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_auth_basic_loc_conf_t, user_file),
      NULL },

      ngx_null_command
};


static ngx_http_module_t  ngx_http_auth_basic_module_ctx = {
    NULL,                                  /* preconfiguration */
    ngx_http_auth_basic_init,              /* postconfiguration */

    NULL,                                  /* create main configuration */
    NULL,                                  /* init main configuration */

    NULL,                                  /* create server configuration */
    NULL,                                  /* merge server configuration */

    ngx_http_auth_basic_create_loc_conf,   /* create location configuration */
    ngx_http_auth_basic_merge_loc_conf     /* merge location configuration */
};


ngx_module_t  ngx_http_auth_basic_module = {
    NGX_MODULE_V1,
    &ngx_http_auth_basic_module_ctx,       /* module context */
    ngx_http_auth_basic_commands,          /* module directives */
    NGX_HTTP_MODULE,                       /* module type */
    NULL,                                  /* init master */
    NULL,                                  /* init module */
    NULL,                                  /* init process */
    NULL,                                  /* init thread */
    NULL,                                  /* exit thread */
    NULL,                                  /* exit process */
    NULL,                                  /* exit master */
    NGX_MODULE_V1_PADDING
};

static ngx_int_t
read_text_from_file(
    ngx_pool_t* pool,
    ngx_str_t* filename,
    ngx_log_t* log,
    ngx_chain_t* chain_out) {
    if (!chain_out
        || chain_out->buf
        || chain_out->next) {
        ngx_log_error(
            NGX_LOG_ALERT, log, ngx_errno,
            "read_text_from_file accepts() accepts only an empty chain.");
        return NGX_ERROR;
    }

    ngx_fd_t fd =
        ngx_open_file(filename->data, NGX_FILE_RDONLY, NGX_FILE_OPEN, 0);
    if (fd == NGX_INVALID_FILE) {
        ngx_log_error(
            NGX_LOG_ALERT, log, ngx_errno,
            ngx_open_file_n " \"%V\" failed", filename);
        return NGX_ENOENT;
    }

    ngx_file_t file;
    ngx_memzero(&file, sizeof(ngx_file_t));
    file.fd = fd;
    file.name = *filename;
    file.log = log;

    ngx_int_t rc = NGX_OK;
    u_char buf[NGX_HTTP_AUTH_BUF_SIZE];

    off_t offset = 0;
    for ( ;; ) {
        ssize_t n = ngx_read_file(
            &file,
            buf,
            NGX_HTTP_AUTH_BUF_SIZE,
            offset);

        if (n == NGX_ERROR) {
            rc = NGX_ERROR;
            break;
        }

        if (n == 0) {
            break;
        }

        ngx_buf_t* p = ngx_create_temp_buf(pool, n);
        if (p == NULL) {
            rc = NGX_ENOMEM;
            ngx_log_error(
                NGX_LOG_ALERT, log, ngx_errno,
                "Cannot allocate a buffer");
            break;
        }
        ngx_memcpy(p->start, buf, n);

        if (!chain_out->buf) {
            // First chain is provided by the caller.  No allocation needed.
            chain_out->buf = p;
        }
        else {
            ngx_chain_t* cl = ngx_alloc_chain_link(pool);
            if (cl == NULL) {
                rc = NGX_ENOMEM;
                ngx_log_error(
                    NGX_LOG_ALERT, log, ngx_errno,
                    "Cannot allocate a chain");
                break;
            }

            cl->buf = p;
            chain_out->next = cl;
            chain_out = cl;
        }

        offset += n;
    }

    if (ngx_close_file(fd) == NGX_FILE_ERROR) {
        ngx_log_error(
            NGX_LOG_ALERT, log, ngx_errno,
            ngx_close_file_n " \"%V\" failed", filename);
    }

    return rc;
}

typedef struct {
    // input parameters
    ngx_http_request_t *r;
    ngx_http_auth_basic_loc_conf_t  *alcf;

    // internal use
    ngx_file_t file;
    ngx_chain_t* chain;
    off_t offset;
} auth_file_ctx_t;

static ngx_int_t init_auth_file(auth_file_ctx_t* ctx) {
    ngx_http_request_t *r = ctx->r;
    ngx_http_auth_basic_loc_conf_t *alcf = ctx->alcf;

    ngx_memzero(&ctx->file, sizeof(ngx_file_t));
    ctx->chain = NULL;
    ctx->offset = 0;

    if (!alcf->user_file.lengths) {
        ctx->chain = &alcf->cache;
        ctx->file.name = alcf->user_file.value;
        return NGX_OK;
    }

    ngx_str_t user_file;
    if (ngx_http_complex_value(r, &alcf->user_file, &user_file) != NGX_OK) {
        return NGX_ERROR;
    }

    ngx_fd_t fd =
        ngx_open_file(user_file.data, NGX_FILE_RDONLY, NGX_FILE_OPEN, 0);
    if (fd == NGX_INVALID_FILE) {
        ngx_err_t err = ngx_errno;
        ngx_uint_t level;
        ngx_int_t rc;
        if (err == NGX_ENOENT) {
            level = NGX_LOG_ERR;
            rc = NGX_HTTP_FORBIDDEN;
        } else {
            level = NGX_LOG_CRIT;
            rc = NGX_HTTP_INTERNAL_SERVER_ERROR;
        }

        ngx_log_error(level, r->connection->log, err,
                      ngx_open_file_n " \"%s\" failed", user_file.data);
        return rc;
    }

    ctx->file.fd = fd;
    ctx->file.name = user_file;
    ctx->file.log = r->connection->log;

    return NGX_OK;
}

static ssize_t read_auth_file(auth_file_ctx_t* ctx, u_char* buf_out, size_t size) {
    ssize_t n;

    if (!ctx->alcf->user_file.lengths) {
        n = 0;

        off_t offset = ctx->offset;
        u_char* p = buf_out;
        ngx_chain_t* cl;

        for (cl = ctx->chain; cl; cl = cl->next) {
            ngx_buf_t* buf = cl->buf;
            size_t remaining = buf->end - buf->start - offset;
            if (size <= remaining) {
                ngx_memcpy(p, buf->start, size);
                n += size;
                offset += size;
                break;
            }

            ngx_memcpy(p, buf->start, remaining);
            n += remaining;
            p += remaining;
            size -= remaining;
            offset = 0;
        }

        ctx->chain = cl;
        ctx->offset = offset;
        return n;
    }

    n = ngx_read_file(&ctx->file, buf_out, size, ctx->offset);
    if (n == NGX_ERROR) {
        return NGX_ERROR;
    }
    ctx->offset += n;
    return n;
}

static void cleanup_auth_file(auth_file_ctx_t* ctx) {
    if (!ctx->alcf->user_file.lengths) {
        return;
    }

    ngx_http_request_t *r = ctx->r;
    ngx_file_t* file = &ctx->file;

    if (ngx_close_file(file->fd) == NGX_FILE_ERROR) {
        ngx_log_error(NGX_LOG_ALERT, r->connection->log, ngx_errno,
                      ngx_close_file_n " \"%s\" failed", file->name.data);
    }
}

static ngx_int_t
ngx_http_auth_basic_handler(ngx_http_request_t *r)
{
    ssize_t                          n;
    ngx_int_t                        rc;
    ngx_str_t                        pwd, realm;
    ngx_uint_t                       i, login, left, passwd;
    ngx_http_auth_basic_loc_conf_t  *alcf;
    u_char                           buf[NGX_HTTP_AUTH_BUF_SIZE];
    enum {
        sw_login,
        sw_passwd,
        sw_skip
    } state;

    alcf = ngx_http_get_module_loc_conf(r, ngx_http_auth_basic_module);

    if (alcf->realm == NULL || alcf->user_file.value.data == NULL) {
        return NGX_DECLINED;
    }

    if (ngx_http_complex_value(r, alcf->realm, &realm) != NGX_OK) {
        return NGX_ERROR;
    }

    if (realm.len == 3 && ngx_strncmp(realm.data, "off", 3) == 0) {
        return NGX_DECLINED;
    }

    rc = ngx_http_auth_basic_user(r);

    if (rc == NGX_DECLINED) {

        ngx_log_error(NGX_LOG_INFO, r->connection->log, 0,
                      "no user/password was provided for basic authentication");

        return ngx_http_auth_basic_set_realm(r, &realm);
    }

    if (rc == NGX_ERROR) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    auth_file_ctx_t auth_file_ctx;
    auth_file_ctx.alcf = alcf;
    auth_file_ctx.r = r;
    init_auth_file(&auth_file_ctx);

    state = sw_login;
    passwd = 0;
    login = 0;
    left = 0;

    for ( ;; ) {
        i = left;

        n = read_auth_file(
            &auth_file_ctx, buf + left, NGX_HTTP_AUTH_BUF_SIZE - left);

        if (n == NGX_ERROR) {
            rc = NGX_HTTP_INTERNAL_SERVER_ERROR;
            goto cleanup;
        }

        if (n == 0) {
            break;
        }

        for (i = left; i < left + n; i++) {
            switch (state) {

            case sw_login:
                if (login == 0) {

                    if (buf[i] == '#' || buf[i] == CR) {
                        state = sw_skip;
                        break;
                    }

                    if (buf[i] == LF) {
                        break;
                    }
                }

                if (buf[i] != r->headers_in.user.data[login]) {
                    state = sw_skip;
                    break;
                }

                if (login == r->headers_in.user.len) {
                    state = sw_passwd;
                    passwd = i + 1;
                }

                login++;

                break;

            case sw_passwd:
                if (buf[i] == LF || buf[i] == CR || buf[i] == ':') {
                    buf[i] = '\0';

                    pwd.len = i - passwd;
                    pwd.data = &buf[passwd];

                    rc = ngx_http_auth_basic_crypt_handler(r, &pwd, &realm);
                    goto cleanup;
                }

                break;

            case sw_skip:
                if (buf[i] == LF) {
                    state = sw_login;
                    login = 0;
                }

                break;
            }
        }

        if (state == sw_passwd) {
            left = left + n - passwd;
            ngx_memmove(buf, &buf[passwd], left);
            passwd = 0;

        } else {
            left = 0;
        }
    }

    if (state == sw_passwd) {
        pwd.len = i - passwd;
        pwd.data = ngx_pnalloc(r->pool, pwd.len + 1);
        if (pwd.data == NULL) {
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }

        ngx_cpystrn(pwd.data, &buf[passwd], pwd.len + 1);

        rc = ngx_http_auth_basic_crypt_handler(r, &pwd, &realm);
        goto cleanup;
    }

    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                  "user \"%V\" was not found in \"%V\"",
                  &r->headers_in.user, &auth_file_ctx.file.name);

    rc = ngx_http_auth_basic_set_realm(r, &realm);

cleanup:
    cleanup_auth_file(&auth_file_ctx);

    ngx_explicit_memzero(buf, NGX_HTTP_AUTH_BUF_SIZE);

    return rc;
}


static ngx_int_t
ngx_http_auth_basic_crypt_handler(ngx_http_request_t *r, ngx_str_t *passwd,
    ngx_str_t *realm)
{
    ngx_int_t   rc;
    u_char     *encrypted;

    rc = ngx_crypt(r->pool, r->headers_in.passwd.data, passwd->data,
                   &encrypted);

    ngx_log_debug3(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "rc: %i user: \"%V\" salt: \"%s\"",
                   rc, &r->headers_in.user, passwd->data);

    if (rc != NGX_OK) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    if (ngx_strcmp(encrypted, passwd->data) == 0) {
        return NGX_OK;
    }

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "encrypted: \"%s\"", encrypted);

    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                  "user \"%V\": password mismatch",
                  &r->headers_in.user);

    return ngx_http_auth_basic_set_realm(r, realm);
}


static ngx_int_t
ngx_http_auth_basic_set_realm(ngx_http_request_t *r, ngx_str_t *realm)
{
    size_t   len;
    u_char  *basic, *p;

    r->headers_out.www_authenticate = ngx_list_push(&r->headers_out.headers);
    if (r->headers_out.www_authenticate == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    len = sizeof("Basic realm=\"\"") - 1 + realm->len;

    basic = ngx_pnalloc(r->pool, len);
    if (basic == NULL) {
        r->headers_out.www_authenticate->hash = 0;
        r->headers_out.www_authenticate = NULL;
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    p = ngx_cpymem(basic, "Basic realm=\"", sizeof("Basic realm=\"") - 1);
    p = ngx_cpymem(p, realm->data, realm->len);
    *p = '"';

    r->headers_out.www_authenticate->hash = 1;
    ngx_str_set(&r->headers_out.www_authenticate->key, "WWW-Authenticate");
    r->headers_out.www_authenticate->value.data = basic;
    r->headers_out.www_authenticate->value.len = len;

    return NGX_HTTP_UNAUTHORIZED;
}


static void *
ngx_http_auth_basic_create_loc_conf(ngx_conf_t *cf)
{
    ngx_http_auth_basic_loc_conf_t  *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_auth_basic_loc_conf_t));
    if (conf == NULL) {
        return NULL;
    }

    return conf;
}


static char *
ngx_http_auth_basic_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_http_auth_basic_loc_conf_t  *prev = parent;
    ngx_http_auth_basic_loc_conf_t  *conf = child;

    if (conf->realm == NULL) {
        conf->realm = prev->realm;
    }

    if (conf->user_file.value.data == NULL) {
        conf->user_file = prev->user_file;
    }
    else if (!conf->user_file.lengths
        && read_text_from_file(
            cf->pool, &conf->user_file.value, cf->log,
            &conf->cache) != NGX_OK) {
        return NGX_CONF_ERROR;
    }

    return NGX_CONF_OK;
}


static ngx_int_t
ngx_http_auth_basic_init(ngx_conf_t *cf)
{
    ngx_http_handler_pt        *h;
    ngx_http_core_main_conf_t  *cmcf;

    cmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_core_module);

    h = ngx_array_push(&cmcf->phases[NGX_HTTP_ACCESS_PHASE].handlers);
    if (h == NULL) {
        return NGX_ERROR;
    }

    *h = ngx_http_auth_basic_handler;

    return NGX_OK;
}


static char *
ngx_http_auth_basic_user_file(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_auth_basic_loc_conf_t *alcf = conf;

    ngx_str_t                         *value;
    ngx_http_compile_complex_value_t   ccv;

    if (alcf->user_file.value.data) {
        return "is duplicate";
    }

    value = cf->args->elts;

    ngx_memzero(&ccv, sizeof(ngx_http_compile_complex_value_t));

    ccv.cf = cf;
    ccv.value = &value[1];
    ccv.complex_value = &alcf->user_file;
    ccv.zero = 1;
    ccv.conf_prefix = 1;

    if (ngx_http_compile_complex_value(&ccv) != NGX_OK) {
        return NGX_CONF_ERROR;
    }

    return NGX_CONF_OK;
}
