/*
 * Copyright (C) 2012 Tsukasa Hamano <hamano@osstech.co.jp>
 * Copyright (C) 2012 Open Source Solution Technology Corporation
 */

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

#include "am_web.h"

typedef struct{
    ngx_str_t boot_file;
    ngx_str_t conf_file;
}ngx_http_am_main_conf_t;

static ngx_int_t ngx_http_am_init(ngx_conf_t *cf);
static void *ngx_http_am_create_main_conf(ngx_conf_t *cf);
static char *ngx_http_am_init_main_conf(ngx_conf_t *cf, void *conf);
static ngx_int_t ngx_http_am_init_process(ngx_cycle_t *cycle);
static void ngx_http_am_exit_process(ngx_cycle_t *cycle);
static ngx_int_t ngx_http_am_handler(ngx_http_request_t *r);

static am_status_t
ngx_http_am_func_set_user(void **args, const char *user)
{
    am_status_t st = AM_SUCCESS;
    ngx_http_request_t *r = args[0];
    ngx_log_error(NGX_LOG_DEBUG, r->connection->log, 0,
                  "user=%s", user);
    return st;
}

static am_status_t
ngx_http_am_func_render_result(void **args, am_web_result_t result, char *data)
{
    am_status_t st = AM_SUCCESS;
    ngx_http_request_t *r = args[0];
    int *ret = args[1];
    ngx_table_elt_t *header;

    ngx_log_error(NGX_LOG_DEBUG, r->connection->log, 0,
                  "RESULT=%s(%d)",
                  am_web_result_num_to_str(result), result);

    switch(result){
    case AM_WEB_RESULT_OK:
        *ret = NGX_DECLINED;
        break;
    case AM_WEB_RESULT_OK_DONE:
        ngx_log_error(NGX_LOG_WARN, r->connection->log, 0,
                      "openam responsed AM_WEB_RESULT_OK_DONE. "
                      "I don't know this case, please tell me how to reproduce"
            );
        *ret = NGX_DECLINED;
        break;
    case AM_WEB_RESULT_REDIRECT:
        header = ngx_list_push(&r->headers_out.headers);
        if(!header){
            ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                          "insufficient memory");
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }
        if(!data){
            ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                          "redirect data is null.");
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }
        header->hash = 1;
        ngx_str_set(&header->key, "Location");
        header->value.len = strlen(data);
        header->value.data = ngx_palloc(r->pool, header->value.len);
        ngx_memcpy(header->value.data, data, header->value.len);
        *ret = NGX_HTTP_MOVED_TEMPORARILY;
        break;
    case AM_WEB_RESULT_FORBIDDEN:
        *ret = NGX_HTTP_FORBIDDEN;
        break;
    case AM_WEB_RESULT_ERROR:
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "AM_WEB_RESULT_ERROR");
        *ret = NGX_HTTP_INTERNAL_SERVER_ERROR;
        break;
    default:
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "Unknown Error result=%s(%d)",
                      am_web_result_num_to_str(result), result);
        *ret = NGX_HTTP_INTERNAL_SERVER_ERROR;
        break;
    }
    return st;
}

/*
 * duplicate null-terminated string from pascal string.
 */
static char *ngx_pstrdup_nul(ngx_pool_t *pool, ngx_str_t *src)
{
    char *dst;
    dst = ngx_pnalloc(pool, src->len + 1);
    if(!dst){
        return NULL;
    }
    ngx_memcpy(dst, src->data, src->len);
    dst[src->len] = '\0';
    return dst;
}

/*
 * get cookie
 * TODO: use ngx_http_parse_multi_header_lines()
 */
static char* ngx_http_am_get_cookie(ngx_http_request_t *r){
    ngx_array_t *cookies = &r->headers_in.cookies;
    ngx_table_elt_t  **elts = cookies->elts;
    char *cookie = NULL;

    if(cookies->nelts == 1){
        cookie = ngx_pstrdup_nul(r->pool, &elts[0]->value);
    }else if(cookies->nelts > 1){
        unsigned int i;
        for(i = 0; i < cookies->nelts; i++){
            cookie = ngx_pstrdup_nul(r->pool, &elts[i]->value);
            // FIXME: merge multiple cookie
        }
    }
    return cookie;
}

/*
 * construct full url
 * NOTE: Shoud we use am_web_get_all_request_urls()
 * sjsws agent is using it but apache agent is not using
 */
static char* ngx_http_am_get_url(ngx_http_request_t *r){
    char *proto = NULL;
    char *host = NULL;
    char *path = NULL;
    char *url = NULL;
    ngx_table_elt_t  *elts;
    size_t len = 4; // "://" + '\0'
    int is_ssl = 0;

#if (NGX_HTTP_SSL)
    /* detect SSL connection */
    if(r->connection->ssl){
        is_ssl = 1;
    }
#endif
    if(is_ssl){
        proto = "https";
        len += 5;
    }else{
        proto = "http";
        len += 4;
    }

    elts = r->headers_in.host;
    if(elts){
        if(!(host = ngx_pstrdup_nul(r->pool, &elts->value))){
            return NULL;
        }
        len += elts->value.len;
    }else{
        // FIXME: no Host header case
        host = "none";
        len += 4;
    }

    // Should we chop query parameter from the uri?
    // see http://java.net/jira/browse/OPENSSO-5552
    len += r->unparsed_uri.len;
    if(!(path = ngx_pstrdup_nul(r->pool, &r->unparsed_uri))){
        return NULL;
    }
    if(!(url = ngx_pnalloc(r->pool, len))){
        return NULL;
    }
    // construct url PROTO://HOST:PORT/PATH
    // No need to append default port(80 or 443), may be...
    snprintf(url, len, "%s://%s%s", proto, host, path);
    return url;
}

static ngx_int_t
ngx_http_am_setup_request_parms(ngx_http_request_t *r,
                                am_web_request_params_t *parms){
    memset(parms, 0, sizeof(am_web_request_params_t));
    char *url = ngx_http_am_get_url(r);
    if(!url){
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "insufficient memory");
        return NGX_ERROR;
    }

    char *query = ngx_pstrdup_nul(r->pool, &r->args);
    if(!query){
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "insufficient memory");
        return NGX_ERROR;
    }

    char *method = ngx_pstrdup_nul(r->pool, &r->method_name);
    if(!method){
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "insufficient memory");
        return NGX_ERROR;
    }
    char *addr = ngx_pstrdup_nul(r->pool, &r->connection->addr_text);
    if(!addr){
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "insufficient memory");
        return NGX_ERROR;
    }
    char *cookie = ngx_http_am_get_cookie(r);

    parms->url = url;
    parms->query = query;
    parms->method = am_web_method_str_to_num(method);
    parms->path_info = NULL; // TODO: What is using the parameter for?
    parms->client_ip = addr;
    parms->cookie_header_val = cookie;

    ngx_log_error(NGX_LOG_DEBUG, r->connection->log, 0,
                  "Request Params: url=%s, query=%s, method=%s, "
                  "path_info=%s, client_ip=%s, cookie=%s",
                  parms->url, parms->query, method,
                  parms->path_info?parms->path_info:"(null)",
                  parms->client_ip,
                  parms->cookie_header_val?parms->cookie_header_val:"(null)");
    return NGX_OK;
}

static ngx_int_t
ngx_http_am_setup_request_func(ngx_http_request_t *r,
                               am_web_request_func_t *func,
                               void **args
    ){
    memset((void *)func, 0, sizeof(am_web_request_func_t));
    func->set_user.func = ngx_http_am_func_set_user;
    func->set_user.args = args;
    func->render_result.func = ngx_http_am_func_render_result;
    func->render_result.args = args;
    return NGX_OK;
}

static ngx_command_t ngx_http_am_commands[] = {
    { ngx_string("am_boot_file"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_str_slot,
      NGX_HTTP_MAIN_CONF_OFFSET,
      offsetof(ngx_http_am_main_conf_t, boot_file),
      NULL },
    { ngx_string("am_conf_file"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_str_slot,
      NGX_HTTP_MAIN_CONF_OFFSET,
      offsetof(ngx_http_am_main_conf_t, conf_file),
      NULL },
    ngx_null_command
};

static ngx_http_module_t ngx_http_am_module_ctx = {
    NULL,                          /* preconfiguration */
    ngx_http_am_init,              /* postconfiguration */

    ngx_http_am_create_main_conf,  /* create main configuration */
    ngx_http_am_init_main_conf,    /* init main configuration */

    NULL,                          /* create server configuration */
    NULL,                          /* merge server configuration */

    NULL,                          /* create location configuration */
    NULL,                          /* merge location configuration */
};

ngx_module_t ngx_http_am_module = {
    NGX_MODULE_V1,
    &ngx_http_am_module_ctx,       /* module context */
    ngx_http_am_commands,          /* module directives */
    NGX_HTTP_MODULE,               /* module type */
    NULL,                          /* init master */
    NULL,                          /* init module */
    ngx_http_am_init_process,      /* init process */
    NULL,                          /* init thread */
    NULL,                          /* exit thread */
    ngx_http_am_exit_process,      /* exit process */
    NULL,                          /* exit master */
    NGX_MODULE_V1_PADDING
};

static ngx_int_t
ngx_http_am_init(ngx_conf_t *cf)
{
    ngx_log_error(NGX_LOG_DEBUG, cf->log, 0, "ngx_http_am_init()");
    ngx_http_handler_pt        *h;
    ngx_http_core_main_conf_t  *cmcf;

    cmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_core_module);
    h = ngx_array_push(&cmcf->phases[NGX_HTTP_ACCESS_PHASE].handlers);
    if (h == NULL) {
        return NGX_ERROR;
    }

    *h = ngx_http_am_handler;
    return NGX_OK;
}

static void *
ngx_http_am_create_main_conf(ngx_conf_t *cf)
{
    ngx_http_am_main_conf_t *conf;
    conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_am_main_conf_t));
    if (conf == NULL) {
        return NGX_CONF_ERROR;
    }
    return conf;
}

static char *
ngx_http_am_init_main_conf(ngx_conf_t *cf, void *conf)
{
    ngx_http_am_main_conf_t *amcf = conf;

    if(amcf->boot_file.len == 0){
        ngx_log_error(NGX_LOG_ERR, cf->log, 0,
                      "insufficiency configration. "
                      "please set am_boot_file.");
        return NGX_CONF_ERROR;
    }

    if(amcf->conf_file.len == 0){
        ngx_log_error(NGX_LOG_ERR, cf->log, 0,
                      "insufficiency configration. "
                      "please set am_conf_file.");
        return NGX_CONF_ERROR;
    }

    return NGX_CONF_OK;
}

static ngx_int_t ngx_http_am_init_process(ngx_cycle_t *cycle)
{
    ngx_http_am_main_conf_t *conf;
    am_status_t status;
    boolean_t initialized = B_FALSE;

    ngx_log_error(NGX_LOG_DEBUG, cycle->log, 0,
                  "ngx_http_am_init_process()");

    conf = ngx_http_cycle_get_module_main_conf(cycle, ngx_http_am_module);

    // TODO: is this safe?(null-terminated?)
    status = am_web_init((char *)conf->boot_file.data,
                         (char *)conf->conf_file.data);
    if(status != AM_SUCCESS){
        ngx_log_error(NGX_LOG_ERR, cycle->log, 0,
                      "am_web_init error status=%s(%d)",
                      am_status_to_name(status), status);
        return NGX_ERROR;
    }

    // No need to synchronize due to nginx is single thread model.
    status = am_agent_init(&initialized);
    if(status != AM_SUCCESS){
        ngx_log_error(NGX_LOG_ERR, cycle->log, 0,
                      "am_agent_init error status=%s(%d)",
                      am_status_to_name(status), status);
        return NGX_ERROR;
    }

    return NGX_OK;
}

static void
ngx_http_am_exit_process(ngx_cycle_t *cycle)
{
    am_cleanup();
}

static ngx_int_t
ngx_http_am_notification_handler(ngx_http_request_t *r)
{
    static ngx_str_t type = ngx_string("text/plain");
    static ngx_str_t value = ngx_string("OK\n");
    ngx_http_complex_value_t cv;
    ngx_log_error(NGX_LOG_DEBUG, r->connection->log, 0,
                  "notification request.");
    ngx_memzero(&cv, sizeof(ngx_http_complex_value_t));
    cv.value = value;
    return ngx_http_send_response(r, NGX_HTTP_OK, &type, &cv);
}

static ngx_int_t
ngx_http_am_handler(ngx_http_request_t *r)
{
    am_status_t status;
    int ret = NGX_HTTP_INTERNAL_SERVER_ERROR;
    am_web_result_t result;
    am_web_request_params_t req_params;
    am_web_request_func_t req_func;
    void *args[2] = {r, &ret};
    void *agent_config = NULL;

    ngx_log_error(NGX_LOG_DEBUG, r->connection->log, 0,
                  "ngx_http_am_handler()");

    /* we response to 'GET' and 'HEAD' and 'POST' requests only */
    if (!(r->method & (NGX_HTTP_GET|NGX_HTTP_HEAD|NGX_HTTP_POST))) {
        return NGX_HTTP_NOT_ALLOWED;
    }

    if(ngx_http_am_setup_request_parms(r, &req_params) != NGX_OK){
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "error at ngx_http_am_setup_request_parms()");
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    agent_config = am_web_get_agent_configuration();
    if(!agent_config){
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "error at am_web_get_agent_configuration()");
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    if(am_web_is_notification(req_params.url, agent_config) == B_TRUE){
        // Hmmm, How I notify to all process when working multiprocess mode.
        ngx_http_am_notification_handler(r);
        am_web_delete_agent_configuration(agent_config);
        return NGX_HTTP_OK;
    }

    if(ngx_http_am_setup_request_func(r, &req_func, args) != NGX_OK){
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "error at ngx_http_am_setup_request_func()");
        am_web_delete_agent_configuration(agent_config);
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    result = am_web_process_request(
        &req_params, &req_func, &status, agent_config);
    if(status != AM_SUCCESS){
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "am_web_process_request error. "
                      "status=%s(%d)", am_status_to_name(status), status);
        am_web_delete_agent_configuration(agent_config);
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    am_web_delete_agent_configuration(agent_config);
    ngx_log_error(NGX_LOG_DEBUG, r->connection->log, 0,
                  "return code=%d", ret);
    return ret;
}

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */