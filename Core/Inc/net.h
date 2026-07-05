#ifndef NET_H
#define NET_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Thin status layer over the CubeMX-generated LwIP stack (Phase 8).
 *
 * MX_LWIP_Init() (called from StartDefaultTask) already brings up `gnetif`, the
 * "EthLink" PHY-monitor thread, and DHCP. There is nothing to initialise here —
 * these accessors just report link/DHCP status for the Assist tab and gate the
 * assistant client. They read netif fields (not stateful LwIP socket/netconn
 * API), so they are safe to call from the UI task without the tcpip core lock.
 *
 * This header intentionally pulls in no LwIP headers, so UI/C code can depend on
 * network status without inheriting the whole stack's include surface.
 */

/* True once the PHY reports link (cable inserted + auto-negotiation done). */
bool net_link_up(void);

/* True once DHCP has supplied an IPv4 address (implies link up). */
bool net_dhcp_bound(void);

/* Current IPv4 address as a 32-bit value in network byte order (0 if unbound). */
uint32_t net_ip4(void);

/* Write the dotted-decimal IPv4 string ("192.168.1.42") into `out` (`cap` bytes,
   always NUL-terminated). Writes "0.0.0.0" when unbound. */
void net_ip_str(char *out, int cap);

#endif /* NET_H */
