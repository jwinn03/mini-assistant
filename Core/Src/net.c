#include "net.h"

#include "lwip/netif.h"
#include "lwip/dhcp.h"
#include "lwip/ip4_addr.h"

/* Defined in LWIP/App/lwip.c (CubeMX-generated). lwip.h does not export it, so
   we re-declare the extern here — the same pattern ST's own examples use. */
extern struct netif gnetif;

bool net_link_up(void)
{
    return netif_is_link_up(&gnetif) != 0;
}

bool net_dhcp_bound(void)
{
    /* dhcp_supplied_address() returns true only once the DHCP state machine has
       bound a lease; gate on link too so a yanked cable reads as "not bound". */
    return netif_is_link_up(&gnetif) && (dhcp_supplied_address(&gnetif) != 0);
}

uint32_t net_ip4(void)
{
    return ip4_addr_get_u32(netif_ip4_addr(&gnetif));
}

void net_ip_str(char *out, int cap)
{
    if (out == NULL || cap <= 0) {
        return;
    }
    /* ip4addr_ntoa_r is the reentrant formatter (no shared static buffer), no
       libm — pure integer-to-decimal. Writes "0.0.0.0" for an unbound netif. */
    ip4addr_ntoa_r(netif_ip4_addr(&gnetif), out, cap);
}
