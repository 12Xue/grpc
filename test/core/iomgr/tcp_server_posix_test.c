/*
 *
 * Copyright 2015, Google Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include "src/core/lib/iomgr/port.h"

// This test won't work except with posix sockets enabled
#ifdef GRPC_POSIX_SOCKET

#include "src/core/lib/iomgr/tcp_server.h"

#include <errno.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <grpc/grpc.h>
#include <grpc/support/alloc.h>
#include <grpc/support/log.h>
#include <grpc/support/sync.h>
#include <grpc/support/time.h>

#include "src/core/lib/iomgr/error.h"
#include "src/core/lib/iomgr/iomgr.h"
#include "src/core/lib/iomgr/resolve_address.h"
#include "src/core/lib/iomgr/sockaddr_utils.h"
#include "test/core/util/port.h"
#include "test/core/util/test_config.h"

#define LOG_TEST(x) gpr_log(GPR_INFO, "%s", #x)

static gpr_mu *g_mu;
static grpc_pollset *g_pollset;
static int g_nconnects = 0;

typedef struct {
  /* Owns a ref to server. */
  grpc_tcp_server *server;
  unsigned port_index;
  unsigned fd_index;
  int server_fd;
} on_connect_result;

typedef struct {
  grpc_tcp_server *server;

  /* arg is this server_weak_ref. */
  grpc_closure server_shutdown;
} server_weak_ref;

#define MAX_URI 1024
typedef struct {
  grpc_resolved_address addr;
  char str[MAX_URI];
} test_addr;

#define MAX_ADDRS 100
typedef struct {
  size_t naddrs;
  test_addr addrs[MAX_ADDRS];
} test_addrs;

static on_connect_result g_result = {NULL, 0, 0, -1};

static char family_name_buf[1024];
static const char *sock_family_name(int family) {
  if (family == AF_INET) {
    return "AF_INET";
  } else if (family == AF_INET6) {
    return "AF_INET6";
  } else if (family == AF_UNSPEC) {
    return "AF_UNSPEC";
  } else {
    sprintf(family_name_buf, "%d", family);
    return family_name_buf;
  }
}

static void on_connect_result_init(on_connect_result *result) {
  result->server = NULL;
  result->port_index = 0;
  result->fd_index = 0;
  result->server_fd = -1;
}

static void on_connect_result_set(on_connect_result *result,
                                  const grpc_tcp_server_acceptor *acceptor) {
  result->server = grpc_tcp_server_ref(acceptor->from_server);
  result->port_index = acceptor->port_index;
  result->fd_index = acceptor->fd_index;
  result->server_fd = grpc_tcp_server_port_fd(
      result->server, acceptor->port_index, acceptor->fd_index);
}

static void server_weak_ref_shutdown(grpc_exec_ctx *exec_ctx, void *arg,
                                     grpc_error *error) {
  server_weak_ref *weak_ref = arg;
  weak_ref->server = NULL;
}

static void server_weak_ref_init(server_weak_ref *weak_ref) {
  weak_ref->server = NULL;
  grpc_closure_init(&weak_ref->server_shutdown, server_weak_ref_shutdown,
                    weak_ref, grpc_schedule_on_exec_ctx);
}

/* Make weak_ref->server_shutdown a shutdown_starting cb on server.
   grpc_tcp_server promises that the server object will live until
   weak_ref->server_shutdown has returned. A strong ref on grpc_tcp_server
   should be held until server_weak_ref_set() returns to avoid a race where the
   server is deleted before the shutdown_starting cb is added. */
static void server_weak_ref_set(server_weak_ref *weak_ref,
                                grpc_tcp_server *server) {
  grpc_tcp_server_shutdown_starting_add(server, &weak_ref->server_shutdown);
  weak_ref->server = server;
}

static void test_addr_init_str(test_addr *addr) {
  char *str = NULL;
  if (grpc_sockaddr_to_string(&str, &addr->addr, 0) != -1) {
    size_t str_len;
    memcpy(addr->str, str, (str_len = strnlen(str, sizeof(addr->str) - 1)));
    addr->str[str_len] = '\0';
    gpr_free(str);
  } else {
    addr->str[0] = '\0';
  }
}

static void on_connect(grpc_exec_ctx *exec_ctx, void *arg, grpc_endpoint *tcp,
                       grpc_pollset *pollset,
                       grpc_tcp_server_acceptor *acceptor) {
  grpc_endpoint_shutdown(exec_ctx, tcp, GRPC_ERROR_CREATE("Connected"));
  grpc_endpoint_destroy(exec_ctx, tcp);

  on_connect_result temp_result;
  on_connect_result_set(&temp_result, acceptor);
  gpr_free(acceptor);

  gpr_mu_lock(g_mu);
  g_result = temp_result;
  g_nconnects++;
  GPR_ASSERT(
      GRPC_LOG_IF_ERROR("pollset_kick", grpc_pollset_kick(g_pollset, NULL)));
  gpr_mu_unlock(g_mu);
}

static void test_no_op(void) {
  grpc_exec_ctx exec_ctx = GRPC_EXEC_CTX_INIT;
  grpc_tcp_server *s;
  GPR_ASSERT(GRPC_ERROR_NONE ==
             grpc_tcp_server_create(&exec_ctx, NULL, NULL, &s));
  grpc_tcp_server_unref(&exec_ctx, s);
  grpc_exec_ctx_finish(&exec_ctx);
}

static void test_no_op_with_start(void) {
  grpc_exec_ctx exec_ctx = GRPC_EXEC_CTX_INIT;
  grpc_tcp_server *s;
  GPR_ASSERT(GRPC_ERROR_NONE ==
             grpc_tcp_server_create(&exec_ctx, NULL, NULL, &s));
  LOG_TEST("test_no_op_with_start");
  grpc_tcp_server_start(&exec_ctx, s, NULL, 0, on_connect, NULL);
  grpc_tcp_server_unref(&exec_ctx, s);
  grpc_exec_ctx_finish(&exec_ctx);
}

static void test_no_op_with_port(void) {
  grpc_exec_ctx exec_ctx = GRPC_EXEC_CTX_INIT;
  grpc_resolved_address resolved_addr;
  struct sockaddr_in *addr = (struct sockaddr_in *)resolved_addr.addr;
  grpc_tcp_server *s;
  GPR_ASSERT(GRPC_ERROR_NONE ==
             grpc_tcp_server_create(&exec_ctx, NULL, NULL, &s));
  LOG_TEST("test_no_op_with_port");

  memset(&resolved_addr, 0, sizeof(resolved_addr));
  resolved_addr.len = sizeof(struct sockaddr_in);
  addr->sin_family = AF_INET;
  int port = -1;
  GPR_ASSERT(grpc_tcp_server_add_port(s, &resolved_addr, &port) ==
                 GRPC_ERROR_NONE &&
             port > 0);

  grpc_tcp_server_unref(&exec_ctx, s);
  grpc_exec_ctx_finish(&exec_ctx);
}

static void test_no_op_with_port_and_start(void) {
  grpc_exec_ctx exec_ctx = GRPC_EXEC_CTX_INIT;
  grpc_resolved_address resolved_addr;
  struct sockaddr_in *addr = (struct sockaddr_in *)resolved_addr.addr;
  grpc_tcp_server *s;
  GPR_ASSERT(GRPC_ERROR_NONE ==
             grpc_tcp_server_create(&exec_ctx, NULL, NULL, &s));
  LOG_TEST("test_no_op_with_port_and_start");
  int port = -1;

  memset(&resolved_addr, 0, sizeof(resolved_addr));
  resolved_addr.len = sizeof(struct sockaddr_in);
  addr->sin_family = AF_INET;
  GPR_ASSERT(grpc_tcp_server_add_port(s, &resolved_addr, &port) ==
                 GRPC_ERROR_NONE &&
             port > 0);

  grpc_tcp_server_start(&exec_ctx, s, NULL, 0, on_connect, NULL);

  grpc_tcp_server_unref(&exec_ctx, s);
  grpc_exec_ctx_finish(&exec_ctx);
}

static grpc_error *tcp_connect(grpc_exec_ctx *exec_ctx, const test_addr *remote,
                               on_connect_result *result) {
  gpr_timespec deadline = grpc_timeout_seconds_to_deadline(10);
  int clifd;
  int nconnects_before;
  const struct sockaddr *remote_addr =
      (const struct sockaddr *)remote->addr.addr;

  gpr_log(GPR_INFO, "Connecting to %s", remote->str);
  gpr_mu_lock(g_mu);
  nconnects_before = g_nconnects;
  on_connect_result_init(&g_result);
  clifd = socket(remote_addr->sa_family, SOCK_STREAM, 0);
  if (clifd < 0) {
    gpr_mu_unlock(g_mu);
    return GRPC_OS_ERROR(errno, "Failed to create socket");
  }
  gpr_log(GPR_DEBUG, "start connect to %s", remote->str);
  if (connect(clifd, remote_addr, (socklen_t)remote->addr.len) != 0) {
    gpr_mu_unlock(g_mu);
    close(clifd);
    return GRPC_OS_ERROR(errno, "connect");
  }
  gpr_log(GPR_DEBUG, "wait");
  while (g_nconnects == nconnects_before &&
         gpr_time_cmp(deadline, gpr_now(deadline.clock_type)) > 0) {
    grpc_pollset_worker *worker = NULL;
    grpc_error *err;
    if ((err = grpc_pollset_work(exec_ctx, g_pollset, &worker,
                                 gpr_now(GPR_CLOCK_MONOTONIC), deadline)) !=
        GRPC_ERROR_NONE) {
      gpr_mu_unlock(g_mu);
      close(clifd);
      return err;
    }
    gpr_mu_unlock(g_mu);
    grpc_exec_ctx_finish(exec_ctx);
    gpr_mu_lock(g_mu);
  }
  gpr_log(GPR_DEBUG, "wait done");
  if (g_nconnects != nconnects_before + 1) {
    gpr_mu_unlock(g_mu);
    close(clifd);
    return GRPC_ERROR_CREATE("Didn't connect");
  }
  close(clifd);
  *result = g_result;

  gpr_mu_unlock(g_mu);
  gpr_log(GPR_INFO, "Result (%d, %d) fd %d", result->port_index,
          result->fd_index, result->server_fd);
  grpc_tcp_server_unref(exec_ctx, result->server);
  return GRPC_ERROR_NONE;
}

/* Tests a tcp server on "::" listeners with multiple ports. If channel_args is
   non-NULL, pass them to the server. If dst_addrs is non-NULL, use valid addrs
   as destination addrs (port is not set). If dst_addrs is NULL, use listener
   addrs as destination addrs. If test_dst_addrs is true, test connectivity with
   each destination address, set grpc_resolved_address::len=0 for failures, but
   don't fail the overall unitest. */
static void test_connect(size_t num_connects,
                         const grpc_channel_args *channel_args,
                         test_addrs *dst_addrs, bool test_dst_addrs) {
  grpc_exec_ctx exec_ctx = GRPC_EXEC_CTX_INIT;
  grpc_resolved_address resolved_addr;
  grpc_resolved_address resolved_addr1;
  struct sockaddr_storage *const addr =
      (struct sockaddr_storage *)resolved_addr.addr;
  struct sockaddr_storage *const addr1 =
      (struct sockaddr_storage *)resolved_addr1.addr;
  unsigned svr_fd_count;
  int port;
  int svr_port;
  unsigned svr1_fd_count;
  int svr1_port;
  grpc_tcp_server *s;
  const unsigned num_ports = 2;
  GPR_ASSERT(GRPC_ERROR_NONE ==
             grpc_tcp_server_create(&exec_ctx, NULL, channel_args, &s));
  unsigned port_num;
  server_weak_ref weak_ref;
  server_weak_ref_init(&weak_ref);
  server_weak_ref_set(&weak_ref, s);
  LOG_TEST("test_connect");
  gpr_log(GPR_INFO,
          "clients=%lu, num chan args=%lu, remote IP=%s, test_dst_addrs=%d",
          (unsigned long)num_connects,
          (unsigned long)(channel_args != NULL ? channel_args->num_args : 0),
          dst_addrs != NULL ? "<specific>" : "::", test_dst_addrs);
  memset(&resolved_addr, 0, sizeof(resolved_addr));
  memset(&resolved_addr1, 0, sizeof(resolved_addr1));
  resolved_addr.len = sizeof(struct sockaddr_storage);
  resolved_addr1.len = sizeof(struct sockaddr_storage);
  addr->ss_family = addr1->ss_family = AF_INET;
  GPR_ASSERT(GRPC_LOG_IF_ERROR(
      "grpc_tcp_server_add_port",
      grpc_tcp_server_add_port(s, &resolved_addr, &svr_port)));
  gpr_log(GPR_INFO, "Allocated port %d", svr_port);
  GPR_ASSERT(svr_port > 0);
  /* Cannot use wildcard (port==0), because add_port() will try to reuse the
     same port as a previous add_port(). */
  svr1_port = grpc_pick_unused_port_or_die();
  GPR_ASSERT(svr1_port > 0);
  gpr_log(GPR_INFO, "Picked unused port %d", svr1_port);
  grpc_sockaddr_set_port(&resolved_addr1, svr1_port);
  GPR_ASSERT(grpc_tcp_server_add_port(s, &resolved_addr1, &port) ==
                 GRPC_ERROR_NONE &&
             port == svr1_port);

  /* Bad port_index. */
  GPR_ASSERT(grpc_tcp_server_port_fd_count(s, 2) == 0);
  GPR_ASSERT(grpc_tcp_server_port_fd(s, 2, 0) < 0);

  /* Bad fd_index. */
  GPR_ASSERT(grpc_tcp_server_port_fd(s, 0, 100) < 0);
  GPR_ASSERT(grpc_tcp_server_port_fd(s, 1, 100) < 0);

  /* Got at least one fd per port. */
  svr_fd_count = grpc_tcp_server_port_fd_count(s, 0);
  GPR_ASSERT(svr_fd_count >= 1);
  svr1_fd_count = grpc_tcp_server_port_fd_count(s, 1);
  GPR_ASSERT(svr1_fd_count >= 1);

  grpc_tcp_server_start(&exec_ctx, s, &g_pollset, 1, on_connect, NULL);

  if (dst_addrs != NULL) {
    int ports[] = {svr_port, svr1_port};
    for (port_num = 0; port_num < num_ports; ++port_num) {
      size_t dst_idx;
      size_t num_tested = 0;
      for (dst_idx = 0; dst_idx < dst_addrs->naddrs; ++dst_idx) {
        test_addr dst = dst_addrs->addrs[dst_idx];
        on_connect_result result;
        grpc_error *err;
        if (dst.addr.len == 0) {
          gpr_log(GPR_DEBUG, "Skipping test of non-functional local IP %s",
                  dst.str);
          continue;
        }
        GPR_ASSERT(grpc_sockaddr_set_port(&dst.addr, ports[port_num]));
        test_addr_init_str(&dst);
        ++num_tested;
        on_connect_result_init(&result);
        if ((err = tcp_connect(&exec_ctx, &dst, &result)) == GRPC_ERROR_NONE &&
            result.server_fd >= 0 && result.server == s) {
          continue;
        }
        gpr_log(GPR_ERROR, "Failed to connect to %s: %s", dst.str,
                grpc_error_string(err));
        GPR_ASSERT(test_dst_addrs);
        dst_addrs->addrs[dst_idx].addr.len = 0;
        GRPC_ERROR_UNREF(err);
      }
      GPR_ASSERT(num_tested > 0);
    }
  } else {
    for (port_num = 0; port_num < num_ports; ++port_num) {
      const unsigned num_fds = grpc_tcp_server_port_fd_count(s, port_num);
      unsigned fd_num;
      for (fd_num = 0; fd_num < num_fds; ++fd_num) {
        int fd = grpc_tcp_server_port_fd(s, port_num, fd_num);
        size_t connect_num;
        test_addr dst;
        GPR_ASSERT(fd >= 0);
        dst.addr.len = sizeof(dst.addr.addr);
        GPR_ASSERT(getsockname(fd, (struct sockaddr *)dst.addr.addr,
                               (socklen_t *)&dst.addr.len) == 0);
        GPR_ASSERT(dst.addr.len <= sizeof(dst.addr.addr));
        test_addr_init_str(&dst);
        gpr_log(GPR_INFO, "(%d, %d) fd %d family %s listening on %s", port_num,
                fd_num, fd, sock_family_name(addr->ss_family), dst.str);
        for (connect_num = 0; connect_num < num_connects; ++connect_num) {
          on_connect_result result;
          on_connect_result_init(&result);
          GPR_ASSERT(GRPC_LOG_IF_ERROR("tcp_connect",
                                       tcp_connect(&exec_ctx, &dst, &result)));
          GPR_ASSERT(result.server_fd == fd);
          GPR_ASSERT(result.port_index == port_num);
          GPR_ASSERT(result.fd_index == fd_num);
          GPR_ASSERT(result.server == s);
          GPR_ASSERT(
              grpc_tcp_server_port_fd(s, result.port_index, result.fd_index) ==
              result.server_fd);
        }
      }
    }
  }
  /* Weak ref to server valid until final unref. */
  GPR_ASSERT(weak_ref.server != NULL);
  GPR_ASSERT(grpc_tcp_server_port_fd(s, 0, 0) >= 0);

  grpc_tcp_server_unref(&exec_ctx, s);
  grpc_exec_ctx_finish(&exec_ctx);

  /* Weak ref lost. */
  GPR_ASSERT(weak_ref.server == NULL);
}

static void destroy_pollset(grpc_exec_ctx *exec_ctx, void *p,
                            grpc_error *error) {
  grpc_pollset_destroy(p);
}

int main(int argc, char **argv) {
  grpc_closure destroyed;
  grpc_exec_ctx exec_ctx = GRPC_EXEC_CTX_INIT;
  grpc_arg chan_args[] = {
      {GRPC_ARG_INTEGER, GRPC_ARG_EXPAND_WILDCARD_ADDRS, {.integer = 1}}};
  const grpc_channel_args channel_args = {1, chan_args};
  struct ifaddrs *ifa = NULL;
  struct ifaddrs *ifa_it;
  // Zalloc dst_addrs to avoid oversized frames.
  test_addrs *dst_addrs = gpr_zalloc(sizeof(*dst_addrs));
  grpc_test_init(argc, argv);
  grpc_init();
  g_pollset = gpr_zalloc(grpc_pollset_size());
  grpc_pollset_init(g_pollset, &g_mu);

  test_no_op();
  test_no_op_with_start();
  test_no_op_with_port();
  test_no_op_with_port_and_start();

  if (getifaddrs(&ifa) != 0 || ifa == NULL) {
    gpr_log(GPR_ERROR, "getifaddrs: %s", strerror(errno));
    return EXIT_FAILURE;
  }
  dst_addrs->naddrs = 0;
  for (ifa_it = ifa; ifa_it != NULL && dst_addrs->naddrs < MAX_ADDRS;
       ifa_it = ifa_it->ifa_next) {
    if (ifa_it->ifa_addr == NULL) {
      continue;
    } else if (ifa_it->ifa_addr->sa_family == AF_INET) {
      dst_addrs->addrs[dst_addrs->naddrs].addr.len = sizeof(struct sockaddr_in);
    } else if (ifa_it->ifa_addr->sa_family == AF_INET6) {
      dst_addrs->addrs[dst_addrs->naddrs].addr.len =
          sizeof(struct sockaddr_in6);
    } else {
      continue;
    }
    memcpy(dst_addrs->addrs[dst_addrs->naddrs].addr.addr, ifa_it->ifa_addr,
           dst_addrs->addrs[dst_addrs->naddrs].addr.len);
    GPR_ASSERT(
        grpc_sockaddr_set_port(&dst_addrs->addrs[dst_addrs->naddrs].addr, 0));
    test_addr_init_str(&dst_addrs->addrs[dst_addrs->naddrs]);
    ++dst_addrs->naddrs;
  }
  freeifaddrs(ifa);
  ifa = NULL;

  /* Connect to same addresses as listeners. */
  test_connect(1, NULL, NULL, false);
  test_connect(10, NULL, NULL, false);

  /* Set dst_addrs->addrs[i].len=0 for dst_addrs that are unreachable with a
     "::" listener. */
  test_connect(1, NULL, dst_addrs, true);

  /* Test connect(2) with dst_addrs. */
  test_connect(1, &channel_args, dst_addrs, false);
  /* Test connect(2) with dst_addrs. */
  test_connect(10, &channel_args, dst_addrs, false);

  grpc_closure_init(&destroyed, destroy_pollset, g_pollset,
                    grpc_schedule_on_exec_ctx);
  grpc_pollset_shutdown(&exec_ctx, g_pollset, &destroyed);
  grpc_exec_ctx_finish(&exec_ctx);
  grpc_shutdown();
  gpr_free(dst_addrs);
  gpr_free(g_pollset);
  return EXIT_SUCCESS;
}

#else /* GRPC_POSIX_SOCKET */

int main(int argc, char **argv) { return 1; }

#endif /* GRPC_POSIX_SOCKET */
