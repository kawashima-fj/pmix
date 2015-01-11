/*
 * Copyright (c) 2004-2010 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2011 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2006-2013 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2009-2012 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2011      Oak Ridge National Labs.  All rights reserved.
 * Copyright (c) 2013-2014 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015      Artem Y. Polyakov <artpol84@gmail.com>.
 *                         All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <event.h>
extern int errno;
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "pmix_server.h"
#include "src/class/pmix_list.h"
#include "src/util/argv.h"
#include "src/util/pmix_environ.h"
#include "src/util/output.h"
#include "src/usock/usock.h"

#include "test_common.h"
#include "pmix_srv_common.h"

/* setup the PMIx server module */
static int authenticate(char *credential);
static int terminated(const char namespace[], int rank);
static int abort_fn(int status, const char msg[]);
static int fencenb_fn(const pmix_range_t ranges[], size_t nranges,
                      int barrier, int collect_data,
                      pmix_modex_cbfunc_t cbfunc, void *cbdata);
static int store_modex_fn(pmix_scope_t scope, pmix_modex_data_t *data);
static int get_modexnb_fn(const char namespace[], int rank,
                          pmix_modex_cbfunc_t cbfunc, void *cbdata);
static int get_job_info_fn(const char namespace[], int rank,
                           pmix_info_t *info[], size_t *ninfo);
static int publish_fn(pmix_scope_t scope, const pmix_info_t info[], size_t ninfo);
static int lookup_fn(pmix_scope_t scope,
                     pmix_info_t info[], size_t ninfo,
                     char *namespace[]);
static int unpublish_fn(pmix_scope_t scope, char **keys);
static int spawn_fn(const pmix_app_t apps[],
                    size_t napps,
                    pmix_spawn_cbfunc_t cbfunc, void *cbdata);
static int connect_fn(const pmix_range_t ranges[], size_t nranges,
                      pmix_connect_cbfunc_t cbfunc, void *cbdata);
static int disconnect_fn(const pmix_range_t ranges[], size_t nranges,
                         pmix_connect_cbfunc_t cbfunc, void *cbdata);

static pmix_server_module_t mymodule = {
    authenticate,
    terminated,
    abort_fn,
    fencenb_fn,
    store_modex_fn,
    get_modexnb_fn,
    get_job_info_fn,
    publish_fn,
    lookup_fn,
    unpublish_fn,
    spawn_fn,
    connect_fn,
    disconnect_fn
};

int service = true;
pmix_modex_data_t **pmix_db = NULL;
struct event_base *server_base = NULL;
int listen_fd = -1;
int client_fd = -1;
pid_t client_pid = -1;

struct event *cli_ev, *exit_ev;
struct event *listen_ev = NULL;

static int recv_cli_auth(int sd);
static void connection_handler(int incomind_sd, short flags, void* cbdata);
static void message_handler(int sd, short flags, void* cbdata);


int main(int argc, char **argv, char **env)
{
    struct sockaddr_un address;
    int rc;

    fprintf(stderr,"Start PMIx smoke test\n");

    /* initialize the event library */
    if (NULL == (server_base = event_base_new())) {
        printf("Cannot create libevent event\n");
        return 0;
    }

    /* setup the server library */
    if (PMIX_SUCCESS != (rc = PMIx_server_init_light(&mymodule, NULL, "1234"))) {
        fprintf(stderr, "Init failed with error %d\n", rc);
        return rc;
    }
    /* prepare database */
    //prepare_db();
    address = PMIx_get_addr();

    fprintf(stderr,"PMIx srv: Initialization finished\n");

    /* start listening */
    listen_fd = start_listening(&address);
    /* setup to listen via the event lib */
    listen_ev = event_new(server_base, listen_fd,
                          EV_READ|EV_PERSIST, connection_handler, 0);
    event_add(listen_ev, 0);
    fprintf(stderr, "PMIx srv: Server is listening for incoming connections\n");

    /* fork/exec the test */
    if( 0 > (client_pid = run_client(address, server_base, env, &service) ) ){
        fprintf(stderr,"PMIx srv: Cannot spawn the client\n");
        exit(0);
    }

    /* cycle the event library until complete */
    while( service ){
         event_base_loop(server_base, EVLOOP_ONCE);
    }

    fprintf(stderr,"PMIx srv: exit service loop. wait() for the client termination\n");
    wait(NULL);

    /* finalize the server library */
    if (PMIX_SUCCESS != (rc = PMIx_server_finalize_light())) {
        fprintf(stderr, "Finalize failed with error %d\n", rc);
    }

    return rc;
}

/*
 * Handler for accepting connections from the event library
 */
static void connection_handler(int incomind_sd, short flags, void* cbdata)
{
    int rc, sd;

    fprintf(stderr, "PMIx srv: Incoming connection from the client\n");

    if( 0 > (sd = accept(incomind_sd,NULL,0)) ){
        fprintf(stderr, "PMIx srv: %s:%d accept() failed %d:%s",
                __FILE__, __LINE__, errno, strerror(errno));
        exit(0);
    }

    /* receive ack from the server */
    if (PMIX_SUCCESS != (rc = recv_cli_auth(sd))) {
        printf("PMIx srv: Bad authentification!\n");
        exit(0);
    }

    cli_ev = event_new(server_base, sd,
                      EV_READ|EV_PERSIST, message_handler, NULL);
    event_add(cli_ev,NULL);
    pmix_usock_set_nonblocking(sd);
    client_fd = sd;
    printf("PMIx srv: New client connection accepted\n");
}

static int recv_cli_auth(int sd)
{
    pmix_message_t *msg = PMIx_message_new();
    int rc;
    pmix_peer_cred_t cred;

    if( PMIX_SUCCESS != (rc = blocking_recv(sd,PMIx_message_hdr_ptr(msg),
                                            PMIx_message_hdr_size(msg))) ){
        return rc;
    }
    if( PMIX_SUCCESS != (rc = PMIx_message_hdr_fix(msg) ) ){
        return rc;
    }
    if( PMIX_SUCCESS != (rc = blocking_recv(sd,PMIx_message_pay_ptr(msg),
                                            PMIx_message_pay_size(msg) )) ){
        return rc;
    }
    rc = PMIx_server_cred_extract(sd, msg, &cred);
    PMIx_message_free(msg);
    msg = PMIx_server_cred_reply(rc);

    if (PMIX_SUCCESS != (rc = pmix_usock_send_blocking(sd, PMIx_message_hdr_ptr(msg),
                                                       PMIx_message_hdr_size(msg) ))) {
        fprintf(stderr,"PMIx srv [%s:%d]: Cannot send header\n",
                __FILE__, __LINE__);
        return rc;
    }
    if (PMIX_SUCCESS != (rc = pmix_usock_send_blocking(sd, PMIx_message_pay_ptr(msg),
                                                       PMIx_message_pay_size(msg) ))) {
        fprintf(stderr,"PMIx srv [%s:%d]: Cannot send header\n",
                __FILE__, __LINE__);
        return rc;
    }
    PMIx_message_free(msg);
    return rc;
}

static void message_handler(int sd, short flags, void* cbdata)
{
    int rc;
    pmix_message_t *msg = PMIx_message_new();

    rc = blocking_recv(sd, PMIx_message_hdr_ptr(msg), PMIx_message_hdr_size(msg));
    /* Process errors first (if any) */
    if ( PMIX_ERR_UNREACH == rc ) {
        /* Communication error */
        fprintf(stderr,"PMIx srv: Client closed connection\n");
        event_del(cli_ev);
        event_free(cli_ev);
        close(client_fd);
        client_fd = -1;
        return;
    } else if( PMIX_ERR_RESOURCE_BUSY == rc ||
               PMIX_ERR_WOULD_BLOCK == rc) {
        /* exit this event and let the event lib progress */
        return;
    }

    PMIx_message_hdr_fix(msg);

    if( 0 < PMIx_message_pay_size(msg) ){
        rc = blocking_recv(sd, PMIx_message_pay_ptr(msg), PMIx_message_pay_size(msg));
        if (PMIX_SUCCESS == rc) {
            PMIx_server_process_msg(sd, msg);
        } else {
            /* Communication error */
            fprintf(stderr, "PMIx srv: %s:%d Error communicating with the client",
                    __FILE__, __LINE__);
            kill(client_pid, SIGKILL);
            // TODO: make sure to kill the client
            exit(0);
        }
    }
    return;
}

/**** PMIx hooks ****/

static int authenticate(char *credential)
{
    if (0 == strcmp(credential, TEST_CREDENTIAL)) {
        return PMIX_SUCCESS;
    }
    return PMIX_ERROR;
}

static int terminated(const char namespace[], int rank)
{
    /*
    test_complete = true;
    */
    return PMIX_SUCCESS;
}

static int abort_fn(int status, const char msg[])
{
    return PMIX_SUCCESS;
}

static int fencenb_fn(const pmix_range_t ranges[], size_t nranges,
                      int barrier, int collect_data,
                      pmix_modex_cbfunc_t cbfunc, void *cbdata)
{
    if (NULL != cbfunc) {
        cbfunc(PMIX_SUCCESS, NULL, 0, cbdata);
    }
    return PMIX_SUCCESS;
}

static int store_modex_fn(pmix_scope_t scope, pmix_modex_data_t *data)
{
    /*
    pmix_test_data_t *mdx;

    pmix_output(0, "Storing data for %s:%d", data->namespace, data->rank);

    mdx = OBJ_NEW(pmix_test_data_t);
    (void)strncpy(mdx->data.namespace, data->namespace, PMIX_MAX_NSLEN);
    mdx->data.rank = data->rank;
    mdx->data.size = data->size;
    if (0 < mdx->data.size) {
        mdx->data.blob = (uint8_t*)malloc(mdx->data.size);
        memcpy(mdx->data.blob, data->blob, mdx->data.size);
    }
    pmix_list_append(&modex, &mdx->super);
    */
    return PMIX_SUCCESS;
}

static int get_modexnb_fn(const char namespace[], int rank,
                          pmix_modex_cbfunc_t cbfunc, void *cbdata)
{
    /*
    pmix_test_data_t *mdx;
    pmix_modex_data_t *mdxarray;
    size_t size, n;

    pmix_output(0, "Getting data for %s:%d", namespace, rank);

    size = pmix_list_get_size(&modex);
    if (0 < size) {
        mdxarray = (pmix_modex_data_t*)malloc(size * sizeof(pmix_modex_data_t));
        n = 0;
        PMIX_LIST_FOREACH(mdx, &modex, pmix_test_data_t) {
            (void)strncpy(mdxarray[n].namespace, mdx->data.namespace, PMIX_MAX_NSLEN);
            mdxarray[n].rank = mdx->data.rank;
            mdxarray[n].size = mdx->data.size;
            if (0 < mdx->data.size) {
                mdxarray[n].blob = (uint8_t*)malloc(mdx->data.size);
                memcpy(mdxarray[n].blob, mdx->data.blob, mdx->data.size);
            }
            n++;
        }
    } else {
        mdxarray = NULL;
    }
    if (NULL != cbfunc) {
        cbfunc(PMIX_SUCCESS, mdxarray, size, cbdata);
    }
    // free the array
    for (n=0; n < size; n++) {
        if (NULL != mdxarray[n].blob) {
            free(mdxarray[n].blob);
        }
    }
    if (NULL != mdxarray) {
        free(mdxarray);
    }
    */
    return PMIX_SUCCESS;
}

static int get_job_info_fn(const char namespace[], int rank,
                           pmix_info_t *info[], size_t *ninfo)
{
    *info = NULL;
    *ninfo = 0;
    return PMIX_SUCCESS;
}

static int publish_fn(pmix_scope_t scope, const pmix_info_t info[], size_t ninfo)
{
    return PMIX_SUCCESS;
}

static int lookup_fn(pmix_scope_t scope,
                     pmix_info_t info[], size_t ninfo,
                     char *namespace[])
{
    *namespace = NULL;
    return PMIX_SUCCESS;
}

static int unpublish_fn(pmix_scope_t scope, char **keys)
{
    return PMIX_SUCCESS;
}

static int spawn_fn(const pmix_app_t apps[],
                    size_t napps,
                    pmix_spawn_cbfunc_t cbfunc, void *cbdata)
{
   if (NULL != cbfunc) {
        cbfunc(PMIX_SUCCESS, "foobar", cbdata);
    }
    return PMIX_SUCCESS;
}

static int connect_fn(const pmix_range_t ranges[], size_t nranges,
                      pmix_connect_cbfunc_t cbfunc, void *cbdata)
{
    if (NULL != cbfunc) {
        cbfunc(PMIX_SUCCESS, cbdata);
    }
   return PMIX_SUCCESS;
}

static int disconnect_fn(const pmix_range_t ranges[], size_t nranges,
                         pmix_connect_cbfunc_t cbfunc, void *cbdata)
{
    if (NULL != cbfunc) {
        cbfunc(PMIX_SUCCESS, cbdata);
    }
    return PMIX_SUCCESS;
}