/*
 * ngx_http_fuck_pcl2_module.c
 *
 * nginx module: drop HTTP requests whose "User-Agent" header field contains
 * the PCL client signature and answer them with "444", i.e. close the
 * connection without sending a single byte of response.
 *
 * Matched substrings (ASCII case-insensitive):
 *
 *     "PCL"      every request that announces a PCL token
 *     "PCL/2"    implied by the first one, kept because it was asked for
 *
 * Only the User-Agent header field is inspected.  The decision is taken in
 * the NGX_HTTP_POST_READ_PHASE, right after the request line and the header
 * block have been read -- before any request body is read, before the access
 * phase, before the request is routed anywhere, and before access_log runs.
 * A dropped request therefore never reaches the log files.
 *
 * Directive:
 *
 *     ngx_fuck_pcl2 on;
 *
 * Allowed in http{} and server{}.  It is stored in a server-level
 * configuration because the check runs before nginx has chosen a location;
 * leaving it out, or "off", disables the module for that server.
 *
 * Copyright (c) 2026.  Distributed under the same license as nginx.
 */

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>


#define NGX_HTTP_FUCK_PCL2_UA_LOG_LEN  256


typedef struct {
    ngx_flag_t    enable;        /* 1 = on, 0 = off, UNSET = inherit */
    ngx_array_t  *needles;       /* ngx_str_t *, folded, built once */
} ngx_http_fuck_pcl2_srv_conf_t;


static ngx_str_t  ngx_http_fuck_pcl2_needles_src[] = {
    ngx_string("PCL"),
    ngx_string("PCL/2")
};


static char *ngx_http_fuck_pcl2(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
static void *ngx_http_fuck_pcl2_create_srv_conf(ngx_conf_t *cf);
static char *ngx_http_fuck_pcl2_merge_srv_conf(ngx_conf_t *cf, void *parent,
    void *child);

static ngx_int_t ngx_http_fuck_pcl2_init(ngx_conf_t *cf);

static u_char ngx_http_fuck_pcl2_fold(u_char c);
static ngx_int_t ngx_http_fuck_pcl2_search(u_char *haystack, size_t len,
    u_char *needle, size_t nlen);
static ngx_int_t ngx_http_fuck_pcl2_match(ngx_connection_t *c,
    ngx_str_t *value, ngx_http_fuck_pcl2_srv_conf_t *plcf);
static ngx_int_t ngx_http_fuck_pcl2_handler(ngx_http_request_t *r);


static ngx_command_t  ngx_http_fuck_pcl2_commands[] = {

    { ngx_string("ngx_fuck_pcl2"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_CONF_FLAG,
      ngx_http_fuck_pcl2,
      NGX_HTTP_SRV_CONF_OFFSET,
      0,
      NULL },

      ngx_null_command
};


static ngx_http_module_t  ngx_http_fuck_pcl2_module_ctx = {
    NULL,                                   /* preconfiguration */
    ngx_http_fuck_pcl2_init,                /* postconfiguration */

    NULL,                                   /* create main configuration */
    NULL,                                   /* init main configuration */

    ngx_http_fuck_pcl2_create_srv_conf,     /* create server configuration */
    ngx_http_fuck_pcl2_merge_srv_conf,      /* merge server configuration */

    NULL,                                   /* create location configuration */
    NULL                                    /* merge location configuration */
};


ngx_module_t  ngx_http_fuck_pcl2_module = {
    NGX_MODULE_V1,
    &ngx_http_fuck_pcl2_module_ctx,         /* module context */
    ngx_http_fuck_pcl2_commands,            /* module directives */
    NGX_HTTP_MODULE,                        /* module type */
    NULL,                                   /* init master */
    NULL,                                   /* init module */
    NULL,                                   /* init process */
    NULL,                                   /* init thread */
    NULL,                                   /* exit thread */
    NULL,                                   /* exit process */
    NULL,                                   /* exit master */
    NGX_MODULE_V1_PADDING
};


/* -------------------------------------------------------------------------
 * Case folding.
 *
 * The needles are ASCII, so folding a byte with XOR 0x20 is exact for them at
 * zero cost.  Bytes >= 0x80 go through a fixed Latin-1 rule instead of the C
 * locale, so the result never depends on the process locale and the module
 * builds cleanly with -Werror.  Both the needles and the incoming header are
 * folded with this one function, so the comparison can never be asymmetric.
 * ------------------------------------------------------------------------- */

static u_char
ngx_http_fuck_pcl2_fold(u_char c)
{
    if (c < 0x80) {
        return (u_char) (c | 0x20);
    }

    /* Latin-1: fold the uppercase accented letters, leave the rest alone. */
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7) {
        return (u_char) (c | 0x20);
    }

    return c;
}


/* -------------------------------------------------------------------------
 * Substring search over already folded bytes.  The needles are 3 and 5 bytes
 * long, so the plain scan beats anything cleverer.
 * ------------------------------------------------------------------------- */

static ngx_int_t
ngx_http_fuck_pcl2_search(u_char *haystack, size_t len, u_char *needle,
    size_t nlen)
{
    u_char    *p, *last;
    ngx_int_t  found = 0;

    if (nlen == 0 || len < nlen) {
        return 0;
    }

    p = haystack;
    last = p + len - nlen + 1;

    while (p < last) {
        if (ngx_strncmp(p, needle, nlen) == 0) {
            found = 1;
            break;
        }
        p++;
    }

    return found;
}


/*
 * ngx_http_alloc_large_header_buffer() may point the User-Agent value straight
 * into the raw client buffer, so the value is never modified in place: it is
 * folded into a scratch allocation from the connection pool and the cursor of
 * the client buffer stays where it is.
 */
static ngx_int_t
ngx_http_fuck_pcl2_match(ngx_connection_t *c, ngx_str_t *value,
    ngx_http_fuck_pcl2_srv_conf_t *plcf)
{
    u_char     *folded;
    ngx_str_t  *needle;
    ngx_uint_t  i;

    if (value->len == 0 || plcf->needles == NULL) {
        return 0;
    }

    folded = ngx_pnalloc(c->pool, value->len);
    if (folded == NULL) {
        /* Cannot inspect the header: let the request through rather than
         * guessing.  The connection pool is reused by the next request. */
        return 0;
    }

    for (i = 0; i < value->len; i++) {
        folded[i] = ngx_http_fuck_pcl2_fold(value->data[i]);
    }

    for (i = 0; i < plcf->needles->nelts; i++) {
        needle = &((ngx_str_t *) plcf->needles->elts)[i];

        if (ngx_http_fuck_pcl2_search(folded, value->len,
                                      needle->data, needle->len))
        {
            return 1;
        }
    }

    return 0;
}


static ngx_int_t
ngx_http_fuck_pcl2_handler(ngx_http_request_t *r)
{
    ngx_table_elt_t                *ua;
    ngx_str_t                       head;
    ngx_http_fuck_pcl2_srv_conf_t  *plcf;

    plcf = ngx_http_get_module_srv_conf(r, ngx_http_fuck_pcl2_module);

    if (plcf == NULL || !plcf->enable) {
        return NGX_DECLINED;
    }

    ua = r->headers_in.user_agent;

    if (ua == NULL || ua->value.len == 0) {
        return NGX_DECLINED;
    }

    if (!ngx_http_fuck_pcl2_match(r->connection, &ua->value, plcf)) {
        return NGX_DECLINED;
    }

    head.data = ua->value.data;
    head.len = ua->value.len < NGX_HTTP_FUCK_PCL2_UA_LOG_LEN ?
                   ua->value.len : NGX_HTTP_FUCK_PCL2_UA_LOG_LEN;

    /* The header value is client controlled: log a bounded prefix of it and
     * leave it outside quotes, since ngx_log escapes long printable runs. */
    ngx_log_error(NGX_LOG_INFO, r->connection->log, 0,
                  "ngx_fuck_pcl2: dropping client %V requesting %V "
                  "(user-agent starts: %V)",
                  &r->connection->addr_text, &r->unparsed_uri, &head);

    /*
     * 444 is the special status that makes nginx close the connection without
     * sending a response at all.  NGX_HTTP_CLOSE is that same code and, unlike
     * a plain 444, skips the request-body draining that would only delay
     * killing the connection.
     */
    return NGX_HTTP_CLOSE;
}


/*
 * ngx_conf_set_flag_slot() validates on/off and stores 1/0 into the first
 * ngx_flag_t of the server configuration; a repeated directive in the same
 * block is reported as a duplicate by nginx itself.
 */
static char *
ngx_http_fuck_pcl2(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    return ngx_conf_set_flag_slot(cf, cmd, conf);
}


static void *
ngx_http_fuck_pcl2_create_srv_conf(ngx_conf_t *cf)
{
    ngx_http_fuck_pcl2_srv_conf_t  *plcf;

    plcf = ngx_pcalloc(cf->pool, sizeof(ngx_http_fuck_pcl2_srv_conf_t));
    if (plcf == NULL) {
        return NULL;
    }

    /* ngx_conf_set_flag_slot() needs a defined value to detect duplicates */
    plcf->enable = NGX_CONF_UNSET;
    plcf->needles = NULL;

    return plcf;
}


static char *
ngx_http_fuck_pcl2_merge_srv_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_http_fuck_pcl2_srv_conf_t  *prev = parent;
    ngx_http_fuck_pcl2_srv_conf_t  *conf = child;

    ngx_array_t  *a;
    ngx_str_t    *src, *dst;
    ngx_uint_t    i, j;

    ngx_conf_merge_value(conf->enable, prev->enable, 0);

    /* One folded needle list is shared by the whole configuration. */
    if (conf->needles == NULL) {

        if (prev->needles != NULL) {
            conf->needles = prev->needles;

        } else {
            a = ngx_array_create(cf->pool,
                                 sizeof(ngx_http_fuck_pcl2_needles_src)
                                 / sizeof(ngx_str_t),
                                 sizeof(ngx_str_t));
            if (a == NULL) {
                return NGX_CONF_ERROR;
            }

            for (i = 0;
                 i < sizeof(ngx_http_fuck_pcl2_needles_src) / sizeof(ngx_str_t);
                 i++)
            {
                src = &ngx_http_fuck_pcl2_needles_src[i];

                dst = ngx_array_push(a);
                if (dst == NULL) {
                    return NGX_CONF_ERROR;
                }

                dst->len = src->len;
                dst->data = ngx_pnalloc(cf->pool, src->len);
                if (dst->data == NULL) {
                    return NGX_CONF_ERROR;
                }

                for (j = 0; j < src->len; j++) {
                    dst->data[j] = ngx_http_fuck_pcl2_fold(src->data[j]);
                }
            }

            conf->needles = a;
        }
    }

    return NGX_CONF_OK;
}


static ngx_int_t
ngx_http_fuck_pcl2_init(ngx_conf_t *cf)
{
    ngx_http_handler_pt        *h;
    ngx_http_core_main_conf_t  *cmcf;

    cmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_core_module);

    h = ngx_array_push(&cmcf->phases[NGX_HTTP_POST_READ_PHASE].handlers);
    if (h == NULL) {
        return NGX_ERROR;
    }

    *h = ngx_http_fuck_pcl2_handler;

    return NGX_OK;
}
