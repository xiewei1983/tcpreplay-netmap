/* $Id$ */

/*
 * Copyright (c) 2001-2007 Aaron Turner.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the names of the copyright owners nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
 * GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER
 * IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "defines.h"
#include "common.h"

#include "tcpedit-int.h"
#include "edit_packet.h"
#include "checksum.h"
#include "lib/sll.h"
#include "dlt.h"

#include <string.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static u_int32_t randomize_ipv4_addr(tcpedit_t *tcpedit, u_int32_t ip);
static u_int32_t remap_ipv4(tcpedit_t *tcpedit, tcpr_cidr_t *cidr, const u_int32_t original);
static int is_unicast_ipv4(tcpedit_t *tcpedit, u_int32_t ip);

/**
 * this code re-calcs the IP and Layer 4 checksums
 * the IMPORTANT THING is that the Layer 4 header 
 * is contiguious in memory after *ip_hdr we're actually
 * writing to the layer 4 header via the ip_hdr ptr.
 * (Yes, this sucks, but that's the way libnet works, and
 * I was too lazy to re-invent the wheel.
 * Returns 0 on sucess, -1 on error
 */
int
fix_checksums(tcpedit_t *tcpedit, struct pcap_pkthdr *pkthdr, ipv4_hdr_t *ip_hdr)
{
    int ret1 = 0, ret2 = 0;
    assert(tcpedit);
    assert(pkthdr);
    assert(ip_hdr);
    

    /* calc the L4 checksum if we have the whole packet && not a frag or first frag */
    if (pkthdr->caplen == pkthdr->len && (htons(ip_hdr->ip_off) & IP_OFFMASK) == 0) {
        ret1 = do_checksum(tcpedit, (u_char *) ip_hdr, 
                ip_hdr->ip_p, ntohs(ip_hdr->ip_len) - (ip_hdr->ip_hl << 2));
        if (ret1 < 0)
            return TCPEDIT_ERROR;
    }
    
    /* calc IP checksum */
    ret2 = do_checksum(tcpedit, (u_char *) ip_hdr, IPPROTO_IP, ntohs(ip_hdr->ip_len));
    if (ret2 < 0)
        return TCPEDIT_ERROR;

    /* what do we return? */
    if (ret1 == TCPEDIT_WARN || ret2 == TCPEDIT_WARN)
        return TCPEDIT_WARN;
    
    return TCPEDIT_OK;
}

/**
 * returns a new 32bit integer which is the randomized IP 
 * based upon the user specified seed
 */
static u_int32_t
randomize_ipv4_addr(tcpedit_t *tcpedit, u_int32_t ip)
{
    assert(tcpedit);
    
    /* don't rewrite broadcast addresses */
    if (tcpedit->skip_broadcast && !is_unicast_ipv4(tcpedit, ip))
        return ip;
        
    return ((ip ^ htonl(tcpedit->seed)) - (ip & htonl(tcpedit->seed)));
}


/**
 * randomizes the source and destination IP addresses based on a 
 * pseudo-random number which is generated via the seed.
 * return 1 since we changed one or more IP addresses
 */
int
randomize_ipv4(tcpedit_t *tcpedit, struct pcap_pkthdr *pkthdr, 
        u_char *pktdata, ipv4_hdr_t *ip_hdr)
{
#ifdef DEBUG
    char srcip[16], dstip[16];
#endif
    assert(tcpedit);
    assert(pkthdr);
    assert(pktdata);
    assert(ip_hdr);

#ifdef DEBUG
    strlcpy(srcip, get_addr2name4(ip_hdr->ip_src.s_addr, 
                RESOLVE), 16);
    strlcpy(dstip, get_addr2name4(ip_hdr->ip_dst.s_addr, 
                RESOLVE), 16);
#endif

    /* randomize IP addresses based on the value of random */
    dbgx(1, "Old Src IP: %s\tOld Dst IP: %s", srcip, dstip);

    /* don't rewrite broadcast addresses */
    if ((tcpedit->skip_broadcast && is_unicast_ipv4(tcpedit, (u_int32_t)ip_hdr->ip_dst.s_addr)) 
        || !tcpedit->skip_broadcast) {
        ip_hdr->ip_dst.s_addr = randomize_ipv4_addr(tcpedit, ip_hdr->ip_dst.s_addr);
    }
    
    if ((tcpedit->skip_broadcast && is_unicast_ipv4(tcpedit, (u_int32_t)ip_hdr->ip_src.s_addr))
        || !tcpedit->skip_broadcast) {
        ip_hdr->ip_src.s_addr = randomize_ipv4_addr(tcpedit, ip_hdr->ip_src.s_addr);
    }

#ifdef DEBUG    
    strlcpy(srcip, get_addr2name4(ip_hdr->ip_src.s_addr, 
                RESOLVE), 16);
    strlcpy(dstip, get_addr2name4(ip_hdr->ip_dst.s_addr, 
                RESOLVE), 16);
#endif

    dbgx(1, "New Src IP: %s\tNew Dst IP: %s\n", srcip, dstip);

    return(1);
}


/**
 * this code will untruncate a packet via padding it with null
 * or resetting the actual IPv4 packet len to the snaplen - L2 header.  
 * return 0 if no change, 1 if change, -1 on error.
 */

int
untrunc_packet(tcpedit_t *tcpedit, struct pcap_pkthdr *pkthdr, 
        u_char *pktdata, ipv4_hdr_t *ip_hdr)
{
    int l2len;
    assert(tcpedit);
    assert(pkthdr);
    assert(pktdata);
    assert(ip_hdr);

    /* if actual len == cap len or there's no IP header, don't do anything */
    if ((pkthdr->caplen == pkthdr->len) || (ip_hdr == NULL)) {
        return(0);
    }
    
    if ((l2len = layer2len(tcpedit)) < 0) {
        tcpedit_seterr(tcpedit, "Non-sensical layer 2 length: %d", l2len);
        return -1;
    }

    /* Pad packet or truncate it */
    if (tcpedit->fixlen == TCPEDIT_FIXLEN_PAD) {
        /*
         * this should be an unnecessary check
  	     * but I've gotten a report that sometimes the caplen > len
  	     * which seems like a corrupted pcap
  	     */
        if (pkthdr->len > pkthdr->caplen) {
            memset(pktdata + pkthdr->caplen, '\0', pkthdr->len - pkthdr->caplen);
            pkthdr->caplen = pkthdr->len;
        } else if (pkthdr->len < pkthdr->caplen) {
            /* i guess this is necessary if we've got a bogus pcap */
            //ip_hdr->ip_len = htons(pkthdr->caplen - l2len);
            tcpedit_seterr(tcpedit, "%s", "WTF?  Why is your packet larger then the capture len?");
            return -1;
        }
    }
    else if (tcpedit->fixlen == TCPEDIT_FIXLEN_TRUNC) {
        if (pkthdr->len != pkthdr->caplen)
            ip_hdr->ip_len = htons(pkthdr->caplen - l2len);
        pkthdr->len = pkthdr->caplen;
    }
    else {
        tcpedit_seterr(tcpedit, "Invalid fixlen value: 0x%x", tcpedit->fixlen);
        return -1;
    }

    return(1);
}

/**
 * Extracts the layer 7 data from the packet for TCP, UDP, ICMP
 * returns the number of bytes and a pointer to the layer 7 data. 
 * Returns 0 for no data
 */
int
extract_data(tcpedit_t *tcpedit, const u_char *pktdata, int caplen, 
        char *l7data[])
{
    int datalen = 0;
    ipv4_hdr_t *ip_hdr = NULL;
    tcp_hdr_t *tcp_hdr = NULL;
    udp_hdr_t *udp_hdr = NULL;
    u_char ipbuff[MAXPACKET];
    u_char *dataptr = NULL;
    
    assert(tcpedit);
    assert(pktdata);
    assert(l7data);

    /* grab our IPv4 header */
    dataptr = ipbuff;
    if ((ip_hdr = (ipv4_hdr_t*)get_ipv4(pktdata, caplen, 
                    tcpedit->runtime.dlt1, &dataptr)) == NULL)
        return 0;

    /* figure out the actual datalen which might be < the caplen
     * due to ethernet padding 
     */
    if (caplen > ntohs(ip_hdr->ip_len)) {
        datalen = ntohs(ip_hdr->ip_len);
    } else {
        datalen = caplen - tcpedit->dlt_ctx->l2len;
    }

    /* update the datlen to not include the IP header len */
    datalen -= ip_hdr->ip_hl << 2;
    dataptr += ip_hdr->ip_hl << 2;
    if (datalen <= 0)
        goto nodata;

    /* TCP ? */
    if (ip_hdr->ip_p == IPPROTO_TCP) {
        tcp_hdr = (tcp_hdr_t *) get_layer4(ip_hdr);
        datalen -= tcp_hdr->th_off << 2;
        if (datalen <= 0)
            goto nodata;

        dataptr += tcp_hdr->th_off << 2;
    }

    /* UDP ? */
    else if (ip_hdr->ip_p == IPPROTO_UDP) {
        udp_hdr = (udp_hdr_t *) get_layer4(ip_hdr);
        datalen -= TCPR_UDP_H;
        if (datalen <= 0)
            goto nodata;

        dataptr += TCPR_UDP_H;
    }

    /* ICMP ? just ignore it for now */
    else if (ip_hdr->ip_p == IPPROTO_ICMP) {
        dbg(2, "Ignoring any possible data in ICMP packet");
        goto nodata;
    }

    /* unknown proto, just dump everything past the IP header */
    else {
        dbg(2, "Unknown protocol, dumping everything past the IP header");
        dataptr = (u_char *)ip_hdr;
    }

    dbgx(2, "packet had %d bytes of layer 7 data", datalen);
    memcpy(l7data, dataptr, datalen);
    return datalen;

  nodata:
    dbg(2, "packet has no data, skipping...");
    return 0;
}

/**
 * rewrites an IPv4 packet's TTL based on the rules
 * return 0 if no change, 1 if changed 
 */
int
rewrite_ipv4_ttl(tcpedit_t *tcpedit, ipv4_hdr_t *ip_hdr)
{
    assert(tcpedit);

    /* make sure there's something to edit */
    if (ip_hdr == NULL || tcpedit->ttl_mode == TCPEDIT_TTL_OFF)
        return(0);
        
    switch(tcpedit->ttl_mode) {
    case TCPEDIT_TTL_SET:
        if (ip_hdr->ip_ttl == tcpedit->ttl_value)
            return(0);           /* no change required */
        ip_hdr->ip_ttl = tcpedit->ttl_value;
        break;
    case TCPEDIT_TTL_ADD:
        if (((int)ip_hdr->ip_ttl + tcpedit->ttl_value) > 255) {
            ip_hdr->ip_ttl = 255;
        } else {
            ip_hdr->ip_ttl += tcpedit->ttl_value;
        }
        break;
    case TCPEDIT_TTL_SUB:
        if (ip_hdr->ip_ttl <= tcpedit->ttl_value) {
            ip_hdr->ip_ttl = 1;
        } else {
            ip_hdr->ip_ttl -= tcpedit->ttl_value;
        }
        break;
    default:
        errx(1, "invalid ttl_mode: %d", tcpedit->ttl_mode);
    }
    return(1);
}

/**
 * takes a CIDR notation netblock and uses that to "remap" given IP
 * onto that netblock.  ie: 10.0.0.0/8 and 192.168.55.123 -> 10.168.55.123
 * while 10.150.9.0/24 and 192.168.55.123 -> 10.150.9.123
 */
static u_int32_t
remap_ipv4(tcpedit_t *tcpedit, tcpr_cidr_t *cidr, const u_int32_t original)
{
    u_int32_t ipaddr = 0, network = 0, mask = 0, result = 0;

    assert(tcpedit);
    assert(cidr);
    
    /* don't rewrite broadcast addresses */
    if (tcpedit->skip_broadcast && !is_unicast_ipv4(tcpedit, original))
        return original;

    mask = 0xffffffff; /* turn on all the bits */

    /* shift over by correct # of bits */
    mask = mask << (32 - cidr->masklen);

    /* apply the mask to the network */
    network = htonl(cidr->network) & mask;

    /* apply the reverse of the mask to the IP */
    mask = mask ^ 0xffffffff;
    ipaddr = ntohl(original) & mask;

    /* merge the network portion and ip portions */
    result = network ^ ipaddr;
    
    /* return the result in network byte order */
    return(htonl(result));
}

/**
 * rewrite IP address (layer3)
 * uses -N to rewrite (map) one subnet onto another subnet
 * also support --srcipmap and --dstipmap
 * return 0 if no change, 1 or 2 if changed
 */
int
rewrite_ipv4l3(tcpedit_t *tcpedit, ipv4_hdr_t *ip_hdr, tcpr_dir_t direction)
{
    tcpr_cidrmap_t *cidrmap1 = NULL, *cidrmap2 = NULL;
    int didsrc = 0, diddst = 0, loop = 1;

    assert(tcpedit);
    assert(ip_hdr);

    /* first check the src/dst IP maps */
    if (tcpedit->srcipmap != NULL) {
        if (ip_in_cidr(tcpedit->srcipmap->from, ip_hdr->ip_src.s_addr)) {
            ip_hdr->ip_src.s_addr = remap_ipv4(tcpedit, tcpedit->srcipmap->to, ip_hdr->ip_src.s_addr);
            dbgx(2, "Remapped src addr to: %s", get_addr2name4(ip_hdr->ip_src.s_addr, RESOLVE));
        }
    }

    if (tcpedit->dstipmap != NULL) {
        if (ip_in_cidr(tcpedit->dstipmap->from, ip_hdr->ip_dst.s_addr)) {
            ip_hdr->ip_dst.s_addr = remap_ipv4(tcpedit, tcpedit->dstipmap->to, ip_hdr->ip_dst.s_addr);
            dbgx(2, "Remapped src addr to: %s", get_addr2name4(ip_hdr->ip_dst.s_addr, RESOLVE));
        }
    }

    /* anything else to rewrite? */
    if (tcpedit->cidrmap1 == NULL)
        return(0);

    /* don't play with the main pointers */
    if (direction == TCPR_DIR_C2S) {
        cidrmap1 = tcpedit->cidrmap1;
        cidrmap2 = tcpedit->cidrmap2;
    } else {
        cidrmap1 = tcpedit->cidrmap2;
        cidrmap2 = tcpedit->cidrmap1;
    }
    

    /* loop through the cidrmap to rewrite */
    do {
        if ((! diddst) && ip_in_cidr(cidrmap2->from, ip_hdr->ip_dst.s_addr)) {
            ip_hdr->ip_dst.s_addr = remap_ipv4(tcpedit, cidrmap2->to, ip_hdr->ip_dst.s_addr);
            dbgx(2, "Remapped dst addr to: %s", get_addr2name4(ip_hdr->ip_dst.s_addr, RESOLVE));
            diddst = 1;
        }
        if ((! didsrc) && ip_in_cidr(cidrmap1->from, ip_hdr->ip_src.s_addr)) {
            ip_hdr->ip_src.s_addr = remap_ipv4(tcpedit, cidrmap1->to, ip_hdr->ip_src.s_addr);
            dbgx(2, "Remapped src addr to: %s", get_addr2name4(ip_hdr->ip_src.s_addr, RESOLVE));
            didsrc = 1;
        }

        /*
         * loop while we haven't modified both src/dst AND
         * at least one of the cidr maps have a next pointer
         */
        if ((! (diddst && didsrc)) &&
            (! ((cidrmap1->next == NULL) && (cidrmap2->next == NULL)))) {

            /* increment our ptr's if possible */
            if (cidrmap1->next != NULL)
                cidrmap1 = cidrmap1->next;

            if (cidrmap2->next != NULL)
                cidrmap2 = cidrmap2->next;

        } else {
            loop = 0;
        }

        /* Later on we should support various IP protocols which embed
         * the IP address in the application layer.  Things like
         * DNS and FTP.
         */

    } while (loop);

    /* return how many changes we made */
    return (diddst + didsrc);
}

/**
 * Randomize the IP addresses in an ARP packet based on the user seed
 * return 0 if no change, or 1 for a change
 */
int 
randomize_iparp(tcpedit_t *tcpedit, struct pcap_pkthdr *pkthdr, 
        u_char *pktdata, int datalink)
{
    arp_hdr_t *arp_hdr = NULL;
    int l2len = 0;
    u_int32_t *ip, tempip;
    u_char *add_hdr;

    assert(tcpedit);
    assert(pkthdr);
    assert(pktdata);

    l2len = get_l2len(pktdata, pkthdr->caplen, datalink);
    arp_hdr = (arp_hdr_t *)(pktdata + l2len);

    /*
     * only rewrite IP addresses from REPLY/REQUEST's
     */
    if ((ntohs(arp_hdr->ar_pro) == ETHERTYPE_IP) &&
        ((ntohs(arp_hdr->ar_op) == ARPOP_REQUEST) ||
         (ntohs(arp_hdr->ar_op) == ARPOP_REPLY))) {

        /* jump to the addresses */
        add_hdr = (u_char *)arp_hdr;
        add_hdr += sizeof(arp_hdr_t) + arp_hdr->ar_hln;
        ip = (u_int32_t *)add_hdr;
        tempip = randomize_ipv4_addr(tcpedit, *ip);
        memcpy(ip, &tempip, sizeof(u_int32_t));

        add_hdr += arp_hdr->ar_pln + arp_hdr->ar_hln;
        ip = (u_int32_t *)add_hdr;
        tempip = randomize_ipv4_addr(tcpedit, *ip);
        memcpy(ip, &tempip, sizeof(u_int32_t));
    }

    return 1; /* yes we changed the packet */
}

/**
 * rewrite IP address (arp)
 * uses -a to rewrite (map) one subnet onto another subnet
 * pointer must point to the WHOLE and CONTIGOUS memory buffer
 * because the arp_hdr_t doesn't have the space for the IP/MAC
 * addresses
 * return 0 if no change, 1 or 2 if changed
 */
int
rewrite_iparp(tcpedit_t *tcpedit, arp_hdr_t *arp_hdr, int cache_mode)
{
    u_char *add_hdr = NULL;
    u_int32_t *ip1 = NULL, *ip2 = NULL;
    u_int32_t newip = 0;
    tcpr_cidrmap_t *cidrmap1 = NULL, *cidrmap2 = NULL;
    int didsrc = 0, diddst = 0, loop = 1;
#ifdef FORCE_ALIGN
    u_int32_t iptemp;
#endif

    assert(tcpedit);
    assert(arp_hdr);

   /* figure out what mapping to use */
    if (cache_mode == TCPR_DIR_C2S) {
        cidrmap1 = tcpedit->cidrmap1;
        cidrmap2 = tcpedit->cidrmap2;
    } else if (cache_mode == TCPR_DIR_S2C) {
        cidrmap1 = tcpedit->cidrmap2;
        cidrmap2 = tcpedit->cidrmap1;
    }

    /* anything to rewrite? */
    if (cidrmap1 == NULL || cidrmap2 == NULL)
        return(0);

    /*
     * must be IPv4 and request or reply 
     * Do other op codes use the same subheader stub?
     * If so we won't need to check the op code.
     */
    if ((ntohs(arp_hdr->ar_pro) == ETHERTYPE_IP) &&
        ((ntohs(arp_hdr->ar_op) == ARPOP_REQUEST) ||
         (ntohs(arp_hdr->ar_op) == ARPOP_REPLY)))
        {
        /* jump to the addresses */
        add_hdr = (u_char *)arp_hdr;
        add_hdr += sizeof(arp_hdr_t) + arp_hdr->ar_hln;
        ip1 = (u_int32_t *)add_hdr;
        add_hdr += arp_hdr->ar_pln + arp_hdr->ar_hln;
#ifdef FORCE_ALIGN
        /* copy IP2 to a temporary buffer for processing */
        memcpy(&iptemp, add_hdr, sizeof(u_int32_t));
        ip2 = &iptemp;
#else
        ip2 = (u_int32_t *)add_hdr;
#endif
        

        /* loop through the cidrmap to rewrite */
        do {
            /* arp request ? */
            if (ntohs(arp_hdr->ar_op) == ARPOP_REQUEST) {
                if ((!diddst) && ip_in_cidr(cidrmap2->from, *ip1)) {
                    newip = remap_ipv4(tcpedit, cidrmap2->to, *ip1);
                    memcpy(ip1, &newip, 4);
                    diddst = 1;
                }
                if ((!didsrc) && ip_in_cidr(cidrmap1->from, *ip2)) {
                    newip = remap_ipv4(tcpedit, cidrmap1->to, *ip2);
                    memcpy(ip2, &newip, 4);
                    didsrc = 1;
                }
            } 
            /* else it's an arp reply */
            else {
                if ((!diddst) && ip_in_cidr(cidrmap2->from, *ip2)) {
                    newip = remap_ipv4(tcpedit, cidrmap2->to, *ip2);
                    memcpy(ip2, &newip, 4);
                    diddst = 1;
                }
                if ((!didsrc) && ip_in_cidr(cidrmap1->from, *ip1)) {
                    newip = remap_ipv4(tcpedit, cidrmap1->to, *ip1);
                    memcpy(ip1, &newip, 4);
                    didsrc = 1;
                }
            }

#ifdef FORCE_ALIGN
            /* copy temporary IP to IP2 location in buffer */
            memcpy(add_hdr, &iptemp, sizeof(u_int32_t));
#endif

            /*
             * loop while we haven't modified both src/dst AND
             * at least one of the cidr maps have a next pointer
             */
            if ((! (diddst && didsrc)) &&
                (! ((cidrmap1->next == NULL) && (cidrmap2->next == NULL)))) {
                
                /* increment our ptr's if possible */
                if (cidrmap1->next != NULL)
                    cidrmap1 = cidrmap1->next;
                
                if (cidrmap2->next != NULL)
                    cidrmap2 = cidrmap2->next;
                
            } else {
                loop = 0;
            }

        } while (loop);
        
    } else {
        warn("ARP packet isn't for IPv4!  Can't rewrite IP's");
    }

    return(didsrc + diddst);
}

/**
 * returns 1 if the IP address is a unicast address, otherwise, returns 0
 * for broadcast/multicast addresses.  Returns -1 on error
 */
static int
is_unicast_ipv4(tcpedit_t *tcpedit, u_int32_t ip)
{
    assert(tcpedit);
   
    /* multicast/broadcast is 224.0.0.0 or greater */
    if (ip > 3758096384)
        return 0;
        
    return 1;
}
/*
 Local Variables:
 mode:c
 indent-tabs-mode:nil
 c-basic-offset:4
 End:
*/
