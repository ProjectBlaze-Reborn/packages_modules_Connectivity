/*
 * Copyright 2018 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * main.c - main function
 */

#include <errno.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/capability.h>
#include <unistd.h>

#include "resolv_netid.h"

#include "clatd.h"
#include "common.h"
#include "config.h"
#include "logging.h"
#include "setif.h"
#include "tun.h"

#define DEVICEPREFIX "v4-"

/* function: print_help
 * in case the user is running this on the command line
 */
void print_help() {
  printf("android-clat arguments:\n");
  printf("-i [uplink interface]\n");
  printf("-p [plat prefix]\n");
  printf("-n [NetId]\n");
  printf("-m [socket mark]\n");
}

/* function: main
 * allocate and setup the tun device, then run the event loop
 */
int main(int argc, char **argv) {
  struct tun_data tunnel;
  int opt;
  char *uplink_interface = NULL, *plat_prefix = NULL, *net_id_str = NULL, *mark_str = NULL;
  unsigned net_id = NETID_UNSET;
  uint32_t mark   = MARK_UNSET;
  unsigned len;

  while ((opt = getopt(argc, argv, "i:p:n:m:h")) != -1) {
    switch (opt) {
      case 'i':
        uplink_interface = optarg;
        break;
      case 'p':
        plat_prefix = optarg;
        break;
      case 'n':
        net_id_str = optarg;
        break;
      case 'm':
        mark_str = optarg;
        break;
      case 'h':
        print_help();
        exit(0);
      default:
        logmsg(ANDROID_LOG_FATAL, "Unknown option -%c. Exiting.", (char)optopt);
        exit(1);
    }
  }

  if (uplink_interface == NULL) {
    logmsg(ANDROID_LOG_FATAL, "clatd called without an interface");
    exit(1);
  }

  if (net_id_str != NULL && !parse_unsigned(net_id_str, &net_id)) {
    logmsg(ANDROID_LOG_FATAL, "invalid NetID %s", net_id_str);
    exit(1);
  }

  if (mark_str != NULL && !parse_unsigned(mark_str, &mark)) {
    logmsg(ANDROID_LOG_FATAL, "invalid mark %s", mark_str);
    exit(1);
  }

  len = snprintf(tunnel.device4, sizeof(tunnel.device4), "%s%s", DEVICEPREFIX, uplink_interface);
  if (len >= sizeof(tunnel.device4)) {
    logmsg(ANDROID_LOG_FATAL, "interface name too long '%s'", tunnel.device4);
    exit(1);
  }

  logmsg(ANDROID_LOG_INFO, "Starting clat version %s on %s netid=%s mark=%s", CLATD_VERSION,
         uplink_interface, net_id_str ? net_id_str : "(none)", mark_str ? mark_str : "(none)");

  // run under a regular user but keep needed capabilities
  drop_root_but_keep_caps();

  // open our raw sockets before dropping privs
  open_sockets(&tunnel, mark);

  // keeps only admin capability
  set_capability(1 << CAP_NET_ADMIN);

  // we can create tun devices as non-root because we're in the VPN group.
  tunnel.fd4 = tun_open();
  if (tunnel.fd4 < 0) {
    logmsg(ANDROID_LOG_FATAL, "tun_open4 failed: %s", strerror(errno));
    exit(1);
  }

  // When run from netd, the environment variable ANDROID_DNS_MODE is set to
  // "local", but that only works for the netd process itself. Removing the
  // following line causes XLAT failure in permissive mode.
  unsetenv("ANDROID_DNS_MODE");

  configure_interface(uplink_interface, plat_prefix, &tunnel, net_id);

  update_clat_ipv6_address(&tunnel, uplink_interface);

  // Loop until someone sends us a signal or brings down the tun interface.
  if (signal(SIGTERM, stop_loop) == SIG_ERR) {
    logmsg(ANDROID_LOG_FATAL, "sigterm handler failed: %s", strerror(errno));
    exit(1);
  }

  event_loop(&tunnel);

  logmsg(ANDROID_LOG_INFO, "Shutting down clat on %s", uplink_interface);
  del_anycast_address(tunnel.write_fd6, &Global_Clatd_Config.ipv6_local_subnet);

  return 0;
}
