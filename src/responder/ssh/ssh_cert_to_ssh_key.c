/*
   SSSD - certificate handling utils

   Copyright (C) Sumit Bose <sbose@redhat.com> 2018

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "util/util.h"
#include "util/cert.h"
#include "util/crypto/sss_crypto.h"
#include "util/child_common.h"
#include "lib/certmap/sss_certmap.h"

struct cert_to_ssh_key_state {
    struct tevent_context *ev;
    const char *logfile;
    time_t timeout;
    const char **extra_args;
    const char **certs;
    struct ldb_val *keys;
    size_t cert_count;
    size_t iter;
    size_t valid_keys;
};

static errno_t cert_to_ssh_key_step(struct tevent_req *req);
static void cert_to_ssh_key_done(int child_status,
                                 struct tevent_signal *sige,
                                 void *pvt);

struct tevent_req *cert_to_ssh_key_send(TALLOC_CTX *mem_ctx,
                                        struct tevent_context *ev,
                                        const char *logfile, time_t timeout,
                                        const char *ca_db,
                                        struct sss_certmap_ctx *sss_certmap_ctx,
                                        size_t cert_count,
                                        struct ldb_val *bin_certs,
                                        const char *verify_opts)
{
    struct tevent_req *req;
    struct cert_to_ssh_key_state *state;
    size_t arg_c;
    size_t c;
    int ret;

    req = tevent_req_create(mem_ctx, &state, struct cert_to_ssh_key_state);
    if (req == NULL) {
        return NULL;
    }

    if (ca_db == NULL) {
        DEBUG(SSSDBG_CRIT_FAILURE, "Missing CA DB path.\n");
        ret = EINVAL;
        goto done;
    }

    state->ev = ev;
    state->logfile = logfile;
    state->timeout = timeout;

    state->keys = talloc_zero_array(state, struct ldb_val, cert_count);
    if (state->keys == NULL) {
        DEBUG(SSSDBG_OP_FAILURE, "talloc_zero_array failed.\n");
        ret = ENOMEM;
        goto done;
    }
    state->valid_keys = 0;

    state->extra_args = talloc_zero_array(state, const char *, 10);
    if (state->extra_args == NULL) {
        DEBUG(SSSDBG_OP_FAILURE, "talloc_zero_array failed.\n");
        ret = ENOMEM;
        goto done;
    }
    /* extra_args are added in revers order, base64 encoded certificate is
     * added at 0 */
    arg_c = 1;
    state->extra_args[arg_c++] = "--certificate";
    state->extra_args[arg_c++] = ca_db;
    state->extra_args[arg_c++] = "--ca_db";
    if (verify_opts != NULL) {
        state->extra_args[arg_c++] = verify_opts;
        state->extra_args[arg_c++] = "--verify";
    }
    state->extra_args[arg_c++] = "--verification";
    if (timeout > 0) {
        state->extra_args[arg_c++] = talloc_asprintf(state, "%lu", timeout);
        if (state->extra_args[arg_c - 1] == NULL) {
            ret = ENOMEM;
            goto done;
        }
        state->extra_args[arg_c++] = "--timeout";
    }

    state->certs = talloc_zero_array(state, const char *, cert_count);
    if (state->certs == NULL) {
        DEBUG(SSSDBG_OP_FAILURE, "talloc_zero_array failed.\n");
        ret = ENOMEM;
        goto done;
    }

    state->cert_count = 0;
    for (c = 0; c < cert_count; c++) {

        if (sss_certmap_ctx != NULL) {
            ret = sss_certmap_match_cert(sss_certmap_ctx, bin_certs[c].data,
                                         bin_certs[c].length);
            if (ret != 0) {
                DEBUG(SSSDBG_TRACE_ALL, "Certificate does not match matching "
                                        "rules and is ignored.\n");
                continue;
            }
        }
        state->certs[state->cert_count] = sss_base64_encode(state->certs,
                                                            bin_certs[c].data,
                                                            bin_certs[c].length);
        if (state->certs[state->cert_count] == NULL) {
            DEBUG(SSSDBG_OP_FAILURE, "sss_base64_encode failed.\n");
            ret = EINVAL;
            goto done;
        }

        state->cert_count++;
    }

    state->iter = 0;

    ret = cert_to_ssh_key_step(req);

done:
    if (ret != EAGAIN) {
        if (ret == EOK) {
            tevent_req_done(req);
        } else {
            tevent_req_error(req, ret);
        }
        tevent_req_post(req, ev);
    }

    return req;
}

static void p11_child_timeout(struct tevent_context *ev,
                              struct tevent_timer *te,
                              struct timeval tv, void *pvt)
{
    struct tevent_req *req = talloc_get_type(pvt, struct tevent_req);

    DEBUG(SSSDBG_MINOR_FAILURE, "Timeout reached for p11_child.\n");
    tevent_req_error(req, ERR_P11_CHILD_TIMEOUT);
}

static errno_t cert_to_ssh_key_step(struct tevent_req *req)
{
    struct cert_to_ssh_key_state *state = tevent_req_data(req,
                                                  struct cert_to_ssh_key_state);
    int ret;

    if (state->iter >= state->cert_count) {
        return EOK;
    }

    state->extra_args[0] = state->certs[state->iter];

    ret = sss_child_start(state, state->ev, P11_CHILD_PATH,
                          state->extra_args, false, state->logfile,
                          -1, /* ssh cares only about exit code, so no 'io' */
                          cert_to_ssh_key_done, req,
                          (unsigned)(state->timeout), p11_child_timeout, req, true,
                          NULL);
    if (ret != EOK) {
        DEBUG(SSSDBG_OP_FAILURE, "sss_child_start failed [%d]: %s\n",
              ret, sss_strerror(ret));
        return ret;
    }

    return EAGAIN;
}

static void cert_to_ssh_key_done(int child_status,
                                 struct tevent_signal *sige,
                                 void *pvt)
{
    struct tevent_req *req = talloc_get_type(pvt, struct tevent_req);
    struct cert_to_ssh_key_state *state = tevent_req_data(req,
                                                  struct cert_to_ssh_key_state);
    int ret;
    bool valid = false;

    if (WIFEXITED(child_status)) {
        if (WEXITSTATUS(child_status) != 0) {
            DEBUG(SSSDBG_OP_FAILURE,
                  P11_CHILD_PATH " failed with status [%d]\n", child_status);
        } else {
            valid = true;
        }
    }

    if (WIFSIGNALED(child_status)) {
        DEBUG(SSSDBG_OP_FAILURE,
              P11_CHILD_PATH " was terminated by signal [%d]\n",
              WTERMSIG(child_status));
    }

    if (valid) {
        DEBUG(SSSDBG_TRACE_LIBS, "Certificate [%s] is valid.\n",
                                  state->certs[state->iter]);
        ret = get_ssh_key_from_derb64(state->keys,
                                      state->certs[state->iter],
                                      &state->keys[state->iter].data,
                                      &state->keys[state->iter].length);
        if (ret == EOK) {
            state->valid_keys++;
        } else {
            DEBUG(SSSDBG_OP_FAILURE, "get_ssh_key_from_cert failed, "
                                     "skipping certificate [%s].\n",
                                     state->certs[state->iter]);
            state->keys[state->iter].data = NULL;
            state->keys[state->iter].length = 0;
        }
    } else {
        DEBUG(SSSDBG_MINOR_FAILURE, "Certificate [%s] is not valid.\n",
                                    state->certs[state->iter]);
        state->keys[state->iter].data = NULL;
        state->keys[state->iter].length = 0;
    }

    state->iter++;
    ret = cert_to_ssh_key_step(req);
    if (ret != EAGAIN) {
        if (ret == EOK) {
            tevent_req_done(req);
        } else {
            tevent_req_error(req, ret);
        }
    }
}

errno_t cert_to_ssh_key_recv(struct tevent_req *req, TALLOC_CTX *mem_ctx,
                             struct ldb_val **keys, size_t *valid_keys)
{
    struct cert_to_ssh_key_state *state = tevent_req_data(req,
                                                  struct cert_to_ssh_key_state);

    TEVENT_REQ_RETURN_ON_ERROR(req);

    if (keys != NULL) {
        *keys = talloc_steal(mem_ctx, state->keys);
    }

    if (valid_keys != NULL) {
        *valid_keys = state->valid_keys;
    }

    return EOK;
}
