/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "spdk/stdinc.h"

#include "spdk/queue.h"
#include "spdk/rpc.h"
#include "spdk/env.h"
#include "spdk/log.h"
#include "spdk/string.h"

#define RPC_DEFAULT_PORT	"5260"

static struct sockaddr_un g_rpc_listen_addr_unix = {};

static struct spdk_jsonrpc_server *g_jsonrpc_server = NULL;

struct spdk_rpc_method {
	const char *name;
	spdk_rpc_method_handler func;
	SLIST_ENTRY(spdk_rpc_method) slist;
};

static SLIST_HEAD(, spdk_rpc_method) g_rpc_methods = SLIST_HEAD_INITIALIZER(g_rpc_methods);

static void
spdk_jsonrpc_handler(
	struct spdk_jsonrpc_server_conn *conn,
	const struct spdk_json_val *method,
	const struct spdk_json_val *params,
	const struct spdk_json_val *id)
{
	struct spdk_rpc_method *m;

	assert(method != NULL);

	SLIST_FOREACH(m, &g_rpc_methods, slist) {
		if (spdk_json_strequal(method, m->name)) {
			m->func(conn, params, id);
			return;
		}
	}

	spdk_jsonrpc_send_error_response(conn, id, SPDK_JSONRPC_ERROR_METHOD_NOT_FOUND, "Method not found");
}

int
spdk_rpc_listen(const char *listen_addr)
{
	struct addrinfo		hints;
	struct addrinfo		*res;

	memset(&g_rpc_listen_addr_unix, 0, sizeof(g_rpc_listen_addr_unix));

	if (listen_addr[0] == '/') {
		int rc;

		g_rpc_listen_addr_unix.sun_family = AF_UNIX;
		rc = snprintf(g_rpc_listen_addr_unix.sun_path,
			      sizeof(g_rpc_listen_addr_unix.sun_path),
			      "%s", listen_addr);
		if (rc < 0 || (size_t)rc >= sizeof(g_rpc_listen_addr_unix.sun_path)) {
			SPDK_ERRLOG("RPC Listen address Unix socket path too long\n");
			g_rpc_listen_addr_unix.sun_path[0] = '\0';
			return -1;
		}

		unlink(g_rpc_listen_addr_unix.sun_path);

		g_jsonrpc_server = spdk_jsonrpc_server_listen(AF_UNIX, 0,
				   (struct sockaddr *)&g_rpc_listen_addr_unix,
				   sizeof(g_rpc_listen_addr_unix),
				   spdk_jsonrpc_handler);
	} else {
		char *tmp;
		char *host, *port;

		tmp = strdup(listen_addr);
		if (!tmp) {
			SPDK_ERRLOG("Out of memory\n");
			return -1;
		}

		if (spdk_parse_ip_addr(tmp, &host, &port) < 0) {
			free(tmp);
			SPDK_ERRLOG("Invalid listen address '%s'\n", listen_addr);
			return -1;
		}

		if (port == NULL) {
			port = RPC_DEFAULT_PORT;
		}

		memset(&hints, 0, sizeof(hints));
		hints.ai_family = AF_UNSPEC;
		hints.ai_socktype = SOCK_STREAM;
		hints.ai_protocol = IPPROTO_TCP;

		if (getaddrinfo(host, port, &hints, &res) != 0) {
			free(tmp);
			SPDK_ERRLOG("Unable to look up RPC listen address '%s'\n", listen_addr);
			return -1;
		}

		g_jsonrpc_server = spdk_jsonrpc_server_listen(res->ai_family, res->ai_protocol,
				   res->ai_addr, res->ai_addrlen,
				   spdk_jsonrpc_handler);

		freeaddrinfo(res);
		free(tmp);
	}

	if (g_jsonrpc_server == NULL) {
		SPDK_ERRLOG("spdk_jsonrpc_server_listen() failed\n");
		return -1;
	}

	return 0;
}

void
spdk_rpc_accept(void)
{
	spdk_jsonrpc_server_poll(g_jsonrpc_server);
}

void
spdk_rpc_register_method(const char *method, spdk_rpc_method_handler func)
{
	struct spdk_rpc_method *m;

	m = calloc(1, sizeof(struct spdk_rpc_method));
	assert(m != NULL);

	m->name = strdup(method);
	assert(m->name != NULL);

	m->func = func;

	/* TODO: use a hash table or sorted list */
	SLIST_INSERT_HEAD(&g_rpc_methods, m, slist);
}

void
spdk_rpc_close(void)
{
	if (g_rpc_listen_addr_unix.sun_path[0]) {
		/* Delete the Unix socket file */
		unlink(g_rpc_listen_addr_unix.sun_path);
	}

	if (g_jsonrpc_server) {
		spdk_jsonrpc_server_shutdown(g_jsonrpc_server);
	}
}
