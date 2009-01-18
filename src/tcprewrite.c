/* $Id$ */

/*
 * Copyright (c) 2004-2007 Aaron Turner.
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
 
/*
 * Purpose: Modify packets in a pcap file based on rules provided by the
 * user to offload work from tcpreplay and provide a easier means of 
 * reproducing traffic for testing purposes.
 */


#include "config.h"
#include "defines.h"
#include "common.h"

#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>

#include "tcprewrite.h"
#include "tcprewrite_opts.h"
#include "tcpedit/tcpedit.h"

#ifdef DEBUG
int debug;
#endif

#ifdef ENABLE_VERBOSE
/* tcpdump handle */
tcpdump_t tcpdump;
#endif

tcprewrite_opt_t options;
tcpedit_t *tcpedit;

/* local functions */
void tcprewrite_init(void);
void post_args(int argc, char *argv[]);
void verify_input_pcap(pcap_t *pcap);
int rewrite_packets(tcpedit_t *tcpedit, pcap_t *pin, pcap_dumper_t *pout);

int 
main(int argc, char *argv[])
{
    int optct, rcode;
    pcap_t *dlt_pcap;
#ifdef ENABLE_FRAGROUTE
    char ebuf[FRAGROUTE_ERRBUF_LEN];
#endif
    tcprewrite_init();

    /* call autoopts to process arguments */
    optct = optionProcess(&tcprewriteOptions, argc, argv);
    argc -= optct;
    argv += optct;

    /* parse the tcprewrite args */
    post_args(argc, argv);
    
    /* init tcpedit context */
    if (tcpedit_init(&tcpedit, pcap_datalink(options.pin)) < 0) {
        errx(-1, "Error initializing tcpedit: %s", tcpedit_geterr(tcpedit));
    }
    
    /* parse the tcpedit args */
    rcode = tcpedit_post_args(&tcpedit);
    if (rcode < 0) {
        errx(-1, "Unable to parse args: %s", tcpedit_geterr(tcpedit));
    } else if (rcode == 1) {
        warnx("%s", tcpedit_geterr(tcpedit));
    }


    if (tcpedit_validate(tcpedit) < 0) {
        errx(-1, "Unable to edit packets given options:\n%s",
                tcpedit_geterr(tcpedit));
    }
    
   /* open up the output file */
    options.outfile = safe_strdup(OPT_ARG(OUTFILE));
    dbgx(1, "Rewriting DLT to %s",
            pcap_datalink_val_to_name(tcpedit_get_output_dlt(tcpedit)));
    if ((dlt_pcap = pcap_open_dead(tcpedit_get_output_dlt(tcpedit), 65535)) == NULL)
        err(-1, "Unable to open dead pcap handle.");

    dbgx(1, "DLT of dlt_pcap is %s",
        pcap_datalink_val_to_name(pcap_datalink(dlt_pcap)));

#ifdef ENABLE_FRAGROUTE
    if (options.fragroute_args) {
        if ((options.frag_ctx = fragroute_init(65535, pcap_datalink(dlt_pcap), options.fragroute_args, ebuf)) == NULL)
            errx(-1, "%s", ebuf);
    }
#endif

#ifdef ENABLE_VERBOSE
    if (options.verbose) {
        tcpdump_open(&tcpdump, dlt_pcap);
    }
#endif

    if ((options.pout = pcap_dump_open(dlt_pcap, options.outfile)) == NULL)
        errx(-1, "Unable to open output pcap file: %s", pcap_geterr(dlt_pcap));
    pcap_close(dlt_pcap);

    /* rewrite packets */
    if (rewrite_packets(tcpedit, options.pin, options.pout) != 0)
        errx(-1, "Error rewriting packets: %s", tcpedit_geterr(tcpedit));


    /* clean up after ourselves */
    pcap_dump_close(options.pout);
    pcap_close(options.pin);

#ifdef ENABLE_VERBOSE
    tcpdump_close(&tcpdump);
#endif

#ifdef ENABLE_DMALLOC
    dmalloc_shutdown();
#endif
    return 0;
}

void 
tcprewrite_init(void)
{

    memset(&options, 0, sizeof(options));

#ifdef ENABLE_VERBOSE
    /* clear out tcpdump struct */
    memset(&tcpdump, '\0', sizeof(tcpdump_t));
#endif
    
    if (fcntl(STDERR_FILENO, F_SETFL, O_NONBLOCK) < 0)
        warnx("Unable to set STDERR to non-blocking: %s", strerror(errno));
}

/**
 * post AutoGen argument processing
 */
void 
post_args(_U_ int argc, _U_ char *argv[])
{
    char ebuf[PCAP_ERRBUF_SIZE];
     
#ifdef DEBUG
    if (HAVE_OPT(DBUG))
        debug = OPT_VALUE_DBUG;
#else
    if (HAVE_OPT(DBUG))
        warn("not configured with --enable-debug.  Debugging disabled.");
#endif
    

#ifdef ENABLE_VERBOSE
    if (HAVE_OPT(VERBOSE))
        options.verbose = 1;
    
    if (HAVE_OPT(DECODE))
        tcpdump.args = safe_strdup(OPT_ARG(DECODE));    
#endif


#ifdef ENABLE_FRAGROUTE
    if (HAVE_OPT(FRAGROUTE))
        options.fragroute_args = safe_strdup(OPT_ARG(FRAGROUTE));
    
    options.fragroute_dir = FRAGROUTE_DIR_BOTH;
    if (HAVE_OPT(FRAGDIR)) {
        if (strcmp(OPT_ARG(FRAGDIR), "c2s") == 0) {
            options.fragroute_dir = FRAGROUTE_DIR_C2S;
        } else if (strcmp(OPT_ARG(FRAGDIR), "s2c") == 0) {
            options.fragroute_dir = FRAGROUTE_DIR_S2C;
        } else if (strcmp(OPT_ARG(FRAGDIR), "both") == 0) {
            options.fragroute_dir = FRAGROUTE_DIR_BOTH;
        } else {
            errx(-1, "Unknown --fragdir value: %s", OPT_ARG(FRAGDIR));
        }
    }
#endif

    /* open up the input file */
    options.infile = safe_strdup(OPT_ARG(INFILE));
    if ((options.pin = pcap_open_offline(options.infile, ebuf)) == NULL)
        errx(-1, "Unable to open input pcap file: %s", ebuf);
}

/** 
 * Main loop to rewrite packets
 */
int
rewrite_packets(tcpedit_t *tcpedit, pcap_t *pin, pcap_dumper_t *pout)
{
    tcpr_dir_t cache_result = TCPR_DIR_C2S;     /* default to primary */
    struct pcap_pkthdr pkthdr, *pkthdr_ptr;     /* packet header */
    const u_char *pktconst = NULL;              /* packet from libpcap */
    u_char **pktdata = NULL;
    static u_char *pktdata_buff;
    static char *frag = NULL;
    COUNTER packetnum = 0;
    int rcode, frag_len, i;
    
    pkthdr_ptr = &pkthdr;

    if (pktdata_buff == NULL)
        pktdata_buff = (u_char *)safe_malloc(MAXPACKET);
        
    pktdata = &pktdata_buff;
    
    if (frag == NULL)
        frag = (char *)safe_malloc(MAXPACKET);

    /* MAIN LOOP 
     * Keep sending while we have packets or until
     * we've sent enough packets
     */
    while ((pktconst = pcap_next(pin, pkthdr_ptr)) != NULL) {
        packetnum++;
        dbgx(2, "packet " COUNTER_SPEC " caplen %d", packetnum, pkthdr.caplen);

        /* 
         * copy over the packet so we can pad it out if necessary and
         * because pcap_next() returns a const ptr
         */
        memcpy(*pktdata, pktconst, pkthdr.caplen);
        
#ifdef ENABLE_VERBOSE
        if (options.verbose)
            tcpdump_print(&tcpdump, pkthdr_ptr, *pktdata);
#endif

        /* Dual nic processing? */
        if (options.cachedata != NULL) {
            cache_result = check_cache(options.cachedata, packetnum);
        }

        /* sometimes we should not send the packet, in such cases
         * no point in editing this packet at all, just write it to the
         * output file (note, we can't just remove it, or the tcpprep cache
         * file will loose it's indexing
         */

        if (cache_result == TCPR_DIR_NOSEND)
            goto WRITE_PACKET; /* still need to write it so cache stays in sync */

        if ((rcode = tcpedit_packet(tcpedit, &pkthdr_ptr, pktdata, cache_result)) == TCPEDIT_ERROR) {
            return -1;
        } else if ((rcode == TCPEDIT_SOFT_ERROR) && HAVE_OPT(SKIP_SOFT_ERRORS)) {
            /* don't write packet */
            dbgx(1, "Packet " COUNTER_SPEC " is suppressed from being written due to soft errors", packetnum);
            continue;
        }


WRITE_PACKET:
#ifdef ENABLE_FRAGROUTE
        if (options.frag_ctx == NULL) {
            /* write the packet when there's no fragrouting to be done */
            pcap_dump((u_char *)pout, pkthdr_ptr, *pktdata);
        } else {
            /* packet needs to be fragmented */
            if ((options.fragroute_dir == FRAGROUTE_DIR_BOTH) ||
                    (cache_result == TCPR_DIR_C2S && options.fragroute_dir == FRAGROUTE_DIR_C2S) ||
                    (cache_result == TCPR_DIR_S2C && options.fragroute_dir == FRAGROUTE_DIR_S2C)) {

                if (fragroute_process(options.frag_ctx, *pktdata, pkthdr_ptr->caplen) < 0)
                    errx(-1, "Error processing packet via fragroute: %s", options.frag_ctx->errbuf);

                i = 0;
                while ((frag_len = fragroute_getfragment(options.frag_ctx, &frag)) > 0) {
                    /* frags get the same timestamp as the original packet */
                    dbgx(1, "processing packet " COUNTER_SPEC " frag: %u (%d)", packetnum, i++, frag_len);
                    pkthdr_ptr->caplen = frag_len;
                    pkthdr_ptr->len = frag_len;
                    pcap_dump((u_char *)pout, pkthdr_ptr, (u_char *)frag);
                }
            } else {
                /* write the packet without fragroute */
                pcap_dump((u_char *)pout, pkthdr_ptr, *pktdata);
            }
        }
#else
    /* write the packet when there's no fragrouting to be done */
    pcap_dump((u_char *)pout, pkthdr_ptr, *pktdata);

#endif
    } /* while() */
    return 0;
}   


/*
 Local Variables:
 mode:c
 indent-tabs-mode:nil
 c-basic-offset:4
 End:
*/
