/*
 * pppoe, a PPP-over-Ethernet redirector
 * Copyright (C) 1999 Luke Stras <stras@ecf.toronto.edu>
 *
 *    This program is free software; you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation; either version 2 of the License, or
 *    (at your option) any later version.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with this program; if not, write to the Free Software
 *    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 * Revision History
 * 1999/09/22 stras Initial version
 * 1999/09/24 stras Changed header files for greater portability
 * 1999/10/02 stras Added more logging, bug fixes
 * 1999/10/02 mr    Port to bpf/OpenBSD; starvation fixed; efficiency fixes
 * 1999/10/18 stras added BUGGY_AC code, partial forwarding
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <net/if.h>
#ifdef __linux__
#include <net/if_arp.h>
#endif /* __linux__ */
#if defined(__GNU_LIBRARY__) && __GNU_LIBRARY__ < 6
#include <linux/if_ether>
#else
#include <netinet/if_ether.h>
#endif

#include <assert.h> 
#ifdef __linux__
#include <getopt.h>
#endif
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/time.h>
#include <sys/ioctl.h>
#include <sys/wait.h>

#ifdef USE_BPF
#ifndef ETH_ALEN
#define ETH_ALEN 6
#endif /* ETH_ALEN */
/* set this to desired size - you may not get this */
int bpf_buf_size = 65536; 
#include <net/bpf.h>
#include <fcntl.h>
#include <nlist.h>
#include <kvm.h>
unsigned char local_ether[ETH_ALEN]; /* need to this filter packets */
#endif /* USE_BPF */

#include <errno.h>
#ifdef __linux__
extern int errno;
#endif

/* used as the size for a packet buffer */
/* should be > 2 * size of max packet size */
#define PACKETBUF 4096

#define VERSION_MAJOR 0
#define VERSION_MINOR 3

/* references: RFC 2516 */
/* ETHER_TYPE fields for PPPoE */

#define ETH_P_PPPOE_DISC 0x8863 /* discovery stage */
#define ETH_P_PPPOE_SESS 0x8864 /* session stage */

/* ethernet broadcast address */
#define MAC_BCAST_ADDR "\xff\xff\xff\xff\xff\xff"

/* PPPoE packet; includes Ethernet headers and such */
struct pppoe_packet {
#ifdef __linux__
    struct ethhdr ethhdr; /* ethernet header */
#else 
    struct ether_header ethhdr; /* ethernet header */
#endif
    unsigned int ver:4; /* pppoe version */
    unsigned int type:4; /* pppoe type */
    unsigned int code:8; /* pppoe code CODE_* */
    unsigned int session:16; /* session id */
    unsigned short length; /* payload length */
    /* payload follows */
};

/* maximum payload length */
#define MAX_PAYLOAD (1484 - sizeof(struct pppoe_packet))

/* PPPoE codes */
#define CODE_SESS 0x00 /* PPPoE session */
#define CODE_PADI 0x09 /* PPPoE Active Discovery Initiation */
#define CODE_PADO 0x07 /* PPPoE Active Discovery Offer */
#define CODE_PADR 0x19 /* PPPoE Active Discovery Request */
#define CODE_PADS 0x65 /* PPPoE Active Discovery Session-confirmation */
#define CODE_PADT 0xa7 /* PPPoE Active Discovery Terminate */

/* also need */
#define STATE_RUN (-1)

/* PPPoE tag; the payload is a sequence of these */
struct pppoe_tag {
    unsigned short type; /* tag type TAG_* */
    unsigned short length; /* tag length */
    /* payload follows */
};

/* PPPoE tag types */
#define TAG_END_OF_LIST        0x0000
#define TAG_SERVICE_NAME       0x0101
#define TAG_AC_NAME            0x0102
#define TAG_HOST_UNIQ          0x0103
#define TAG_AC_COOKIE          0x0104
#define TAG_VENDOR_SPECIFIC    0x0105
#define TAG_RELAY_SESSION_ID   0x0110
#define TAG_SERVICE_NAME_ERROR 0x0201
#define TAG_AC_SYSTEM_ERROR    0x0202
#define TAG_GENERIC_ERROR      0x0203

/* globals */
int opt_verbose = 0;   /* logging */
int opt_fwd = 0;       /* forward invalid packets */
int opt_fwd_search = 0; /* search for next packet when forwarding */
FILE *log_file = NULL;
FILE *error_file = NULL;

pid_t sess_listen = 0, pppd_listen = 0; /* child processes */
int disc_sock = 0, sess_sock = 0; /* PPPoE sockets */
char src_addr[ETH_ALEN]; /* source hardware address */
char dst_addr[ETH_ALEN]; /* destination hardware address */
char *if_name = NULL; /* interface to use */
int session = 0; /* identifier for our session */
int clean_child = 0; /* flag set when SIGCHLD received */

void
print_hex(unsigned char *buf, int len)
{
    int i;
    
    if (opt_verbose == 0)
      return;

    for (i = 0; i < len; i++)
      fprintf(log_file, "%02x ", (unsigned char)*(buf+i));

    fprintf(log_file, "\n");
}

void 
print_packet(struct pppoe_packet *p)
{
    int i;
    struct pppoe_tag *t = (struct pppoe_tag*)(p + 1);
    struct pppoe_tag tag; /* needed to avoid alignment problems */
    char *buf;
    time_t tm;
    
    if (opt_verbose == 0)
	return;
    
    time(&tm);
    
    fprintf(log_file, "Ethernet header:\n");
    fprintf(log_file, "h_dest: ");
#ifdef __linux__
    for (i = 0; i < 6; i++)
	fprintf(log_file, "%02x:", (unsigned)p->ethhdr.h_dest[i]);
#else 
    for (i = 0; i < 6; i++)
	fprintf(log_file, "%02x:", (unsigned)p->ethhdr.ether_dhost[i]);
#endif
    fprintf(log_file, "\nh_source: ");
#ifdef __linux__
    for (i = 0; i < 6; i++)
	fprintf(log_file, "%02x:", (unsigned)p->ethhdr.h_source[i]);
#else
    for (i = 0; i < 6; i++)
	fprintf(log_file, "%02x:", (unsigned)p->ethhdr.ether_shost[i]);
#endif

#ifdef __linux__
    fprintf(log_file, "\nh_proto: 0x%04x ", 
	    (unsigned)ntohs(p->ethhdr.h_proto));
#else
    fprintf(log_file, "\nh_proto: 0x%04x ", 
	    (unsigned)ntohs(p->ethhdr.ether_type));
#endif

#ifdef __linux__
    switch((unsigned)ntohs(p->ethhdr.h_proto))
#else
    switch((unsigned)ntohs(p->ethhdr.ether_type))
#endif
    {
    case ETH_P_PPPOE_DISC:
	fprintf(log_file, "(PPPOE Discovery)\n");
	break;
    case ETH_P_PPPOE_SESS:
	fprintf(log_file, "(PPPOE Session)\n");
	break;
    default:
	fprintf(log_file, "(Unknown)\n");
    }

    fprintf(log_file, "PPPoE header: \nver: 0x%01x type: 0x%01x code: 0x%02x "
	   "session: 0x%04x length: 0x%04x ", (unsigned)p->ver, 
	   (unsigned)p->type, (unsigned)p->code, (unsigned)p->session,
	   (unsigned)ntohs(p->length));

    switch(p->code)
    {
    case CODE_PADI: 
	fprintf(log_file, "(PADI)\n");
	break;
    case CODE_PADO:
	fprintf(log_file, "(PADO)\n");
	break;
    case CODE_PADR:
	fprintf(log_file, "(PADR)\n");
	break;
    case CODE_PADS:
	fprintf(log_file, "(PADS)\n");
	break;
    case CODE_PADT:
	fprintf(log_file, "(PADT)\n");
	break;
    default:
	fprintf(log_file, "(Unknown)\n");
    }

#ifdef __linux__
    if (ntohs(p->ethhdr.h_proto) != ETH_P_PPPOE_DISC)
#else
    if (ntohs(p->ethhdr.ether_type) != ETH_P_PPPOE_DISC)
#endif
    {
	print_hex((unsigned char *)(p+1), ntohs(p->length));
	return;
    }


    while (t < (struct pppoe_tag *)((char *)(p+1) + ntohs(p->length)))
    {
	/* no guarantee in PPPoE spec that t is aligned at all... */
	memcpy(&tag,t,sizeof(tag));
	fprintf(log_file, "PPPoE tag:\ntype: %04x length: %04x ", 
		ntohs(tag.type), ntohs(tag.length));
	switch(ntohs(tag.type))
	{
	case TAG_END_OF_LIST:
	    fprintf(log_file, "(End of list)\n");
	    break;
	case TAG_SERVICE_NAME:
	    fprintf(log_file, "(Service name)\n");
	    break;
	case TAG_AC_NAME:
	    fprintf(log_file, "(AC Name)\n");
	    break;
	case TAG_HOST_UNIQ:
	    fprintf(log_file, "(Host Uniq)\n");
	    break;
	case TAG_AC_COOKIE:
	    fprintf(log_file, "(AC Cookie)\n");
	    break;
	case TAG_VENDOR_SPECIFIC:
	    fprintf(log_file, "(Vendor Specific)\n");
	    break;
	case TAG_RELAY_SESSION_ID:
	    fprintf(log_file, "(Relay Session ID)\n");
	    break;
	case TAG_SERVICE_NAME_ERROR:
	    fprintf(log_file, "(Service Name Error)\n");
	    break;
	case TAG_AC_SYSTEM_ERROR:
	    fprintf(log_file, "(AC System Error)\n");
	    break;
	case TAG_GENERIC_ERROR:
	    fprintf(log_file, "(Generic Error)\n");
	    break;
	default:
	    fprintf(log_file, "(Unknown)\n");
	}
	if (ntohs(tag.length) > 0)
	    switch (ntohs(tag.type))
	    {
	    case TAG_SERVICE_NAME:
	    case TAG_AC_NAME:
	    case TAG_SERVICE_NAME_ERROR:
	    case TAG_AC_SYSTEM_ERROR:
	    case TAG_GENERIC_ERROR: /* ascii data */
		buf = malloc(ntohs(tag.length) + 1);
		memset(buf, 0, ntohs(tag.length)+1);
		strncpy(buf, (char *)(t+1), ntohs(tag.length));
		buf[ntohs(tag.length)] = '\0';
		fprintf(log_file, "data (UTF-8): %s\n", buf);
		free(buf);
		break;

	    case TAG_HOST_UNIQ:
	    case TAG_AC_COOKIE:
	    case TAG_RELAY_SESSION_ID:
		fprintf(log_file, "data (bin): ");
		for (i = 0; i < ntohs(tag.length); i++)
		    fprintf(log_file, "%02x", (unsigned)*((char *)(t+1) + i));
		fprintf(log_file, "\n");
		break;
		
	    default:
		fprintf(log_file, "unrecognized data\n");
	    }
	t = (struct pppoe_tag *)((char *)(t+1)+ntohs(tag.length));
    }
}
    
int 
open_interface(char *if_name, unsigned short type, char *hw_addr)
{
/* BSD stuff by mr */
#ifdef USE_BPF
  int fd;
  struct ifreq ifr;
  char bpf[16];
  int i, opt;
#ifdef SIMPLE_BPF
  /* a simple BPF program which just grabs the packets of the given type */
  /* by default use the clever BPF program - it works on my SPARC which
     has the same endian as network order.  If someone can confirm that
     the ordering also works on the opposite ending (e.g. ix86) I'll 
     remove the simple filter BPF program for good */
  struct bpf_insn filt[] = {
    BPF_STMT(BPF_LD+BPF_H+BPF_ABS, 12),
    BPF_JUMP(BPF_JMP+BPF_JEQ+BPF_K, 0 /* fill-in */, 0, 1), /* check type */
    BPF_STMT(BPF_RET+BPF_K, (u_int)-1),
    BPF_STMT(BPF_RET+BPF_K, 0)
  };
#else
  /* by default use the clever BPF program which filters out packets 
     originating from us in the kernel */
  /* note that we split the 6-byte ethernet address into a 4-byte word
     and 2-byte half-word to minimize the number of comparisons */
  struct bpf_insn filt[] = {
    BPF_STMT(BPF_LD+BPF_H+BPF_ABS, 12),
    BPF_JUMP(BPF_JMP+BPF_JEQ+BPF_K, 0 /* fill-in */, 0, 5), /* check type */
    /* check src address != our hw address */
    BPF_STMT(BPF_LD+BPF_W+BPF_ABS, 6),
    BPF_JUMP(BPF_JMP+BPF_JEQ+BPF_K, 0 /* fill-in */, 0, 2), /* 4 bytes */
    BPF_STMT(BPF_LD+BPF_H+BPF_ABS, 10),
    BPF_JUMP(BPF_JMP+BPF_JEQ+BPF_K, 0 /* fill-in */, 1, 0), /* 2 bytes */
    BPF_STMT(BPF_RET+BPF_K, (u_int)-1),
    BPF_STMT(BPF_RET+BPF_K, 0)
  };
#endif /* SIMPLE_BPF */
  struct bpf_program prog;
  
  /* hunt for an open bpf */
  for(i = 0; i < 10; i++) {  /* this max is arbitrary */
    sprintf(bpf,"/dev/bpf%d",i);
    if ((fd = open(bpf, O_RDWR)) >= 0)
      break;
  }
  if (fd < 0) {
    perror("pppoe: open(bpf)");
    return -1;
  }

   /* try to increase BPF size if possible */
   (void) ioctl(fd, BIOCSBLEN, &bpf_buf_size); /* try to set buffer size */
   if (ioctl(fd, BIOCGBLEN, &bpf_buf_size) < 0) { /* but find out for sure */
     perror("pppoe: bpf(BIOCGBLEN)");
     return -1;
   }
 

  /* attach to given interface */
  strncpy(ifr.ifr_name,if_name,sizeof(ifr.ifr_name));
  if (ioctl(fd, BIOCSETIF, &ifr) < 0) {
    perror("pppoe: bpf(BIOCSETIF)");
    return -1;
  }

  /* setup BPF */
  opt = 1;
  if (ioctl(fd, BIOCIMMEDIATE, &opt) < 0) {
    perror("pppoe: bpf(BIOCIMMEDIATE)");
    return -1;
  }
  if (ioctl(fd, BIOCGDLT, &opt) < 0) {
    perror("pppoe: bpf(BIOCGDLT)");
    return -1;
  }
  if (opt != DLT_EN10MB) {
    fprintf(stderr, "pppoe: interface %s is not Ethernet!\n", if_name);
    return -1;
  }

  /*************************************************************************
   *
   * WARNING - Really non-portable stuff follows.  This works on OpenBSD 2.5 
   * and may not work anywhere else.
   *
   * What's going on - there's no obvious user-level interface to determine
   * the MAC address of a network interface in BSD that I know of.  (If
   * anyone has an idea, please let me know.)  What happens here is that we
   * dig around in the kernel symbol list to find its list of interfaces,
   * walk through the list to find the interface we are interested in and
   * then we can (inobviously) get the ethernet info from that.
   * I don't like this solution, but it's the best I've got at this point.
   *
   *************************************************************************/

  {
    kvm_t *k;
    struct nlist n[2];
    struct ifnet_head ifhead;
    struct ifnet intf;
    unsigned long v;
    char ifn[IFNAMSIZ+1];
    struct arpcom arp;

    k = kvm_open(NULL,NULL,NULL,O_RDONLY,"pppoe");
    if (k == NULL) {
      fprintf(stderr, "pppoe: failed to open kvm\n");
      return -1;
    }
    n[0].n_name = "_ifnet";
    n[1].n_name = NULL;
    if (kvm_nlist(k,n) != 0) {
      fprintf(stderr, "pppoe: could not find interface list\n");
      kvm_close(k);
      return -1;
    }
    if (kvm_read(k,n[0].n_value,(void *)&ifhead,sizeof(ifhead)) != 
	sizeof(ifhead)) {
      fprintf(stderr, "pppoe: could not read ifnet_head structure\n");
      kvm_close(k);
      return -1;
    }
    v = (unsigned long)(ifhead.tqh_first);
    while(v != 0) {
      if (kvm_read(k,v,(void *)&intf,sizeof(intf)) != sizeof(intf)) {
	fprintf(stderr, "pppoe: could not read ifnet structure\n");
	kvm_close(k);
	return -1;
      }
      strncpy(ifn,intf.if_xname,IFNAMSIZ);
      ifn[IFNAMSIZ] = '\0';
      if (strcmp(ifn,if_name) == 0) 
	/* found our interface */
	break;
      else 
	/* walk the chain */
	v = (unsigned long)(intf.if_list.tqe_next);
    }
    if (v == 0) {
      fprintf(stderr, "pppoe: cannot find interface %s in kernel\n",if_name);
      kvm_close(k);
      return -1;
    }
    /* since we have the right interface, and we determined previously
       that it is an ethernet interface, reread from the same address into
       a "struct arpcom" structure (which begins with a struct ifnet).
       The ethernet address is located past the end of the ifnet structure */
    if (kvm_read(k,v,(void *)&arp,sizeof(arp)) != sizeof(arp)) {
      fprintf(stderr, "could not read arpcom structure\n");
      kvm_close(k);
      return -1;
    }
    /* whew! */
    /* save a copy of this for ourselves */
    memcpy(local_ether,arp.ac_enaddr,ETH_ALEN);
    if (hw_addr)
      memcpy(hw_addr,arp.ac_enaddr,ETH_ALEN); /* also copy if requested */
    kvm_close(k);
  }

  /* setup BPF filter */
  {
    union { unsigned int i; unsigned char b[4]; } x;
    union { unsigned short i; unsigned char b[2]; } y;

    filt[1].k = type; /* set type of packet we are looking for */
#ifndef SIMPLE_BPF
    /* now setup our source address so it gets filtered out */
    for(i = 0; i < 4; i++)
      x.b[i] = local_ether[i];
    for(i = 0; i < 2; i++)
      y.b[i] = local_ether[i+4];
    filt[3].k = x.i;
    filt[5].k = y.i;
#endif /* SIMPLE_BPF */
  }
  prog.bf_insns = filt;
  prog.bf_len = sizeof(filt)/sizeof(struct bpf_insn);
  if (ioctl(fd, BIOCSETF, &prog) < 0) {
    perror("pppoe: bpf(BIOCSETF)");
    return -1;
  }

  return fd;
#else /* do regular linux stuff */
  int optval = 1, rv;
  struct ifreq ifr;

  if ((rv = socket(PF_INET, SOCK_PACKET, htons(type))) < 0)
  {
      perror("pppoe: socket");
      return -1;
  }

  if (setsockopt(rv, SOL_SOCKET, SO_BROADCAST, &optval, sizeof(optval)) < 0)
  {
      perror("pppoe: setsockopt");
      return -1;
  }

  if (hw_addr != NULL) {
      strncpy(ifr.ifr_name, if_name, sizeof(ifr.ifr_name));
      
      if (ioctl(rv, SIOCGIFHWADDR, &ifr) < 0)
      {
	  perror("pppoe: ioctl(SIOCGIFHWADDR)");
	  return -1;
      }
      
      if (ifr.ifr_hwaddr.sa_family != ARPHRD_ETHER)
      {
	  fprintf(error_file, "pppoe: interface %s is not Ethernet!\n", if_name);
	  return -1;
      }
      
      memcpy(hw_addr, ifr.ifr_hwaddr.sa_data, sizeof(ifr.ifr_hwaddr.sa_data));
      
  }
  return rv;
#endif /* USE_BPF / linux */
}

int 
create_padi(struct pppoe_packet *packet, const char *src, const char *name)
{
    int size;

    if (packet == NULL)
	return 0;

    size = sizeof(struct pppoe_packet) + sizeof(struct pppoe_tag);
    if (name != NULL)
	size += strlen(name);

#ifdef __linux__
    memcpy(packet->ethhdr.h_dest, MAC_BCAST_ADDR, 6);
    memcpy(packet->ethhdr.h_source, src, 6);
    packet->ethhdr.h_proto = htons(ETH_P_PPPOE_DISC);
#else
    memcpy(packet->ethhdr.ether_dhost, MAC_BCAST_ADDR, 6);
    memcpy(packet->ethhdr.ether_shost, src, 6);
    packet->ethhdr.ether_type = htons(ETH_P_PPPOE_DISC);
#endif
    packet->ver = 1;
    packet->type = 1;
    packet->code = CODE_PADI;
    packet->session = 0;
    packet->length = htons(size - sizeof(struct pppoe_packet));
    
    /* fill out a blank service-name tag */
    (*(struct pppoe_tag *)(packet+1)).type = htons(TAG_SERVICE_NAME);
    (*(struct pppoe_tag *)(packet+1)).length = name ? htons(strlen(name)) : 0;
    if (name != NULL)
	memcpy((char *)(packet + 1) + sizeof(struct pppoe_tag), name,
	       strlen(name));

    return size;
}

int 
create_padr(struct pppoe_packet *packet, const char *src, const char *dst,
	    char *name)
{
    int size;

    if (packet == NULL)
	return 0;

    size = sizeof(struct pppoe_packet) + sizeof(struct pppoe_tag);
    if (name != NULL)
	size += strlen(name);

#ifdef __linux__
    memcpy(packet->ethhdr.h_dest, dst, 6);
    memcpy(packet->ethhdr.h_source, src, 6);
    packet->ethhdr.h_proto = htons(ETH_P_PPPOE_DISC);
#else
    memcpy(packet->ethhdr.ether_dhost, dst, 6);
    memcpy(packet->ethhdr.ether_shost, src, 6);
    packet->ethhdr.ether_type = htons(ETH_P_PPPOE_DISC);
#endif
    packet->ver = 1;
    packet->type = 1;
    packet->code = CODE_PADR;
    packet->session = 0;
    packet->length = htons(size - sizeof(struct pppoe_packet));
    
    /* fill out a blank service-name tag */
    (*(struct pppoe_tag *)(packet+1)).type = htons(TAG_SERVICE_NAME);
    (*(struct pppoe_tag *)(packet+1)).length = name ? htons(strlen(name)) : 0;
    if (name != NULL)
	memcpy((char *)(packet + 1) + sizeof(struct pppoe_tag), name,
	       strlen(name));
    memset(((char *)packet) + size, 0, 14);
    return size;
}

unsigned short fcstab[256] = {
    0x0000, 0x1189, 0x2312, 0x329b, 0x4624, 0x57ad, 0x6536, 0x74bf,
    0x8c48, 0x9dc1, 0xaf5a, 0xbed3, 0xca6c, 0xdbe5, 0xe97e, 0xf8f7,
    0x1081, 0x0108, 0x3393, 0x221a, 0x56a5, 0x472c, 0x75b7, 0x643e,
    0x9cc9, 0x8d40, 0xbfdb, 0xae52, 0xdaed, 0xcb64, 0xf9ff, 0xe876,
    0x2102, 0x308b, 0x0210, 0x1399, 0x6726, 0x76af, 0x4434, 0x55bd,
    0xad4a, 0xbcc3, 0x8e58, 0x9fd1, 0xeb6e, 0xfae7, 0xc87c, 0xd9f5,
    0x3183, 0x200a, 0x1291, 0x0318, 0x77a7, 0x662e, 0x54b5, 0x453c,
    0xbdcb, 0xac42, 0x9ed9, 0x8f50, 0xfbef, 0xea66, 0xd8fd, 0xc974,
    0x4204, 0x538d, 0x6116, 0x709f, 0x0420, 0x15a9, 0x2732, 0x36bb,
    0xce4c, 0xdfc5, 0xed5e, 0xfcd7, 0x8868, 0x99e1, 0xab7a, 0xbaf3,
    0x5285, 0x430c, 0x7197, 0x601e, 0x14a1, 0x0528, 0x37b3, 0x263a,
    0xdecd, 0xcf44, 0xfddf, 0xec56, 0x98e9, 0x8960, 0xbbfb, 0xaa72,
    0x6306, 0x728f, 0x4014, 0x519d, 0x2522, 0x34ab, 0x0630, 0x17b9,
    0xef4e, 0xfec7, 0xcc5c, 0xddd5, 0xa96a, 0xb8e3, 0x8a78, 0x9bf1,
    0x7387, 0x620e, 0x5095, 0x411c, 0x35a3, 0x242a, 0x16b1, 0x0738,
    0xffcf, 0xee46, 0xdcdd, 0xcd54, 0xb9eb, 0xa862, 0x9af9, 0x8b70,
    0x8408, 0x9581, 0xa71a, 0xb693, 0xc22c, 0xd3a5, 0xe13e, 0xf0b7,
    0x0840, 0x19c9, 0x2b52, 0x3adb, 0x4e64, 0x5fed, 0x6d76, 0x7cff,
    0x9489, 0x8500, 0xb79b, 0xa612, 0xd2ad, 0xc324, 0xf1bf, 0xe036,
    0x18c1, 0x0948, 0x3bd3, 0x2a5a, 0x5ee5, 0x4f6c, 0x7df7, 0x6c7e,
    0xa50a, 0xb483, 0x8618, 0x9791, 0xe32e, 0xf2a7, 0xc03c, 0xd1b5,
    0x2942, 0x38cb, 0x0a50, 0x1bd9, 0x6f66, 0x7eef, 0x4c74, 0x5dfd,
    0xb58b, 0xa402, 0x9699, 0x8710, 0xf3af, 0xe226, 0xd0bd, 0xc134,
    0x39c3, 0x284a, 0x1ad1, 0x0b58, 0x7fe7, 0x6e6e, 0x5cf5, 0x4d7c,
    0xc60c, 0xd785, 0xe51e, 0xf497, 0x8028, 0x91a1, 0xa33a, 0xb2b3,
    0x4a44, 0x5bcd, 0x6956, 0x78df, 0x0c60, 0x1de9, 0x2f72, 0x3efb,
    0xd68d, 0xc704, 0xf59f, 0xe416, 0x90a9, 0x8120, 0xb3bb, 0xa232,
    0x5ac5, 0x4b4c, 0x79d7, 0x685e, 0x1ce1, 0x0d68, 0x3ff3, 0x2e7a,
    0xe70e, 0xf687, 0xc41c, 0xd595, 0xa12a, 0xb0a3, 0x8238, 0x93b1,
    0x6b46, 0x7acf, 0x4854, 0x59dd, 0x2d62, 0x3ceb, 0x0e70, 0x1ff9,
    0xf78f, 0xe606, 0xd49d, 0xc514, 0xb1ab, 0xa022, 0x92b9, 0x8330,
    0x7bc7, 0x6a4e, 0x58d5, 0x495c, 0x3de3, 0x2c6a, 0x1ef1, 0x0f78
};

#define PPPINITFCS16    0xffff  /* Initial FCS value */
#define PPPGOODFCS16    0xf0b8  /* Good final FCS value */
/*
 * Calculate a new fcs given the current fcs and the new data.
 */
unsigned short pppfcs16(register unsigned short fcs, 
			register unsigned char * cp, 
			register int len)
{
/*    assert(sizeof (unsigned short) == 2);
    assert(((unsigned short) -1) > 0); */

    while (len--)
	fcs = (fcs >> 8) ^ fcstab[(fcs ^ *cp++) & 0xff];
    
    return (fcs);
}

#define FRAME_ESC 0x7d
#define FRAME_FLAG 0x7e
#define FRAME_ADDR 0xff
#define FRAME_CTL 0x03
#define FRAME_ENC 0x20

#define ADD_OUT(c) { *out++ = (c); n++; if (opt_verbose) fprintf(log_file, "%x ", (c)); }

void encode_ppp(int fd, unsigned char *buf, int len)
{
    static int first = 0;
    unsigned char out_buf[PACKETBUF];
    unsigned char *out = out_buf;
    unsigned char header[2], tail[2];
    int i,n;
    unsigned short fcs;
    time_t tm;

    header[0] = FRAME_ADDR;
    header[1] = FRAME_CTL;
    fcs = pppfcs16(PPPINITFCS16, header, 2);
    fcs = pppfcs16(fcs, buf, len) ^ 0xffff;
    tail[0] = fcs & 0x00ff;
    tail[1] = (fcs >> 8) & 0x00ff;

    if (opt_verbose)
    {
	time(&tm);
	fprintf(log_file, "%sWriting to pppd: \n", ctime(&tm));
    }

    n = 0;
    if (!first) {
	ADD_OUT(FRAME_FLAG);
	first = 1;
    }
    ADD_OUT(FRAME_ADDR); /* the header - which is constant */
    ADD_OUT(FRAME_ESC);
    ADD_OUT(FRAME_CTL ^ FRAME_ENC);

    for (i = 0; i < len; i++)
	if (buf[i] == FRAME_FLAG || buf[i] == FRAME_ESC || buf[i] < 0x20)
	{
	    ADD_OUT(FRAME_ESC);
	    ADD_OUT(buf[i] ^ FRAME_ENC);
	}
	else
	    ADD_OUT(buf[i]);
    
    for (i = 0; i < 2; i++) {
	if (tail[i] == FRAME_FLAG || tail[i] == FRAME_ESC || tail[i] < 0x20) {
	    ADD_OUT(FRAME_ESC);
	    ADD_OUT(tail[i] ^ FRAME_ENC);
	} else 
	    ADD_OUT(tail[i]);
    }
    ADD_OUT(FRAME_FLAG);

    write(fd, out_buf, n);

    if (opt_verbose)
	fprintf(log_file, "\n");
}

int 
create_sess(struct pppoe_packet *packet, const char *src, const char *dst, 
	    unsigned char *buf, int bufsize, int sess)
{
    int size;
    int i, o = 0;

    if (opt_fwd || !((buf[0] == FRAME_FLAG) || (buf[0] == FRAME_ADDR)))
    {
	if (opt_fwd_search) /* search for a valid packet */
	{
	    while (*buf++ != FRAME_FLAG && bufsize != 0)
		bufsize--;
	    if (bufsize == 0)
		return 0;
	}
	else
	{
	    fprintf(error_file, "create_sess: invalid data\n");
	    return 0;
	}
    }

    for (i = (buf[0] == FRAME_FLAG ? 4 : 3); i < bufsize - 1; i++) 
	if (buf[i] == FRAME_ESC)
	    buf[o++] = buf[++i] ^ FRAME_ENC;
	else
	    buf[o++] = buf[i];

    bufsize = o - 2; /* ignore fcs */

    if (packet == NULL)
	return 0;

    size = sizeof(struct pppoe_packet) + bufsize;

#ifdef __linux__
    memcpy(packet->ethhdr.h_dest, dst, 6);
    memcpy(packet->ethhdr.h_source, src, 6);
    packet->ethhdr.h_proto = htons(ETH_P_PPPOE_SESS);
#else
    memcpy(packet->ethhdr.ether_dhost, dst, 6);
    memcpy(packet->ethhdr.ether_shost, src, 6);
    packet->ethhdr.ether_type = htons(ETH_P_PPPOE_SESS);
#endif
    packet->ver = 1;
    packet->type = 1;
    packet->code = CODE_SESS;
    packet->session = sess;
    packet->length = htons(size - sizeof(struct pppoe_packet));
    
    /* fill out payload */
    memcpy(packet + 1, buf, bufsize);
    
    return size;
}

int 
send_packet(int sock, struct pppoe_packet *packet, int len, const char *ifn)
{
#ifdef USE_BPF
  int c;
  if ((c = write(sock,packet,len)) != len)
    perror("pppoe: write (send_packet)");
  return c;
#else /* regular linux stuff */
    struct sockaddr addr;
    int c;
    time_t tm;

    memset(&addr, 0, sizeof(addr));
    strcpy(addr.sa_data, ifn);

    if (opt_verbose == 1)
    {
	time(&tm);
	fprintf(log_file, "%sSending ", ctime(&tm));
	print_packet(packet);
	fputc('\n', log_file);
    }


    if ((c = sendto(sock, packet, len, 0, &addr, sizeof(addr))) < 0)
	perror("pppoe: sendto (send_packet)");
    
    return c;
#endif /* USE_BPF */
}

#ifdef USE_BPF
/* return:  -1 == error, 0 == okay, 1 == ignore this packet */
int read_bpf_packet(int fd, struct pppoe_packet *packet) {
    /* Nastiness - BPF may return multiple packets in one fell swoop */
    /* This makes select() difficult to use - you need to be ready to
       clear out packets as they arrive */
    static char *buf = NULL;
    static int lastdrop = 0;
    static int n = 0, off = 0;
    struct bpf_hdr *h;

    if (buf == NULL) {
	if ((buf = malloc(bpf_buf_size)) == NULL) {
	    perror("pppoe:malloc");
	    return -1;
	}
    }

    if (off < n) {
	/* read out of previously grabbed buffer */
	if (n-off < sizeof(struct bpf_hdr)) {
	    fprintf(stderr, "BPF: not enough left for header:  %d\n", n-off);
	    off = n = 0; /* force reread from BPF next time */
	    return 1; /* try again */
	}
	h = (struct bpf_hdr *)&(buf[off]);
	memcpy(packet,&(buf[off + h->bh_hdrlen]),h->bh_caplen);
	off += BPF_WORDALIGN(h->bh_hdrlen + h->bh_caplen);
	if (h->bh_caplen != h->bh_datalen) {
	    fprintf(stderr, "pppoe: truncated packet: %d -> %d\n",
		    h->bh_datalen, h->bh_caplen);
	    return 1; /* try again */
	}
    } else {
	struct bpf_stat s;
	if (ioctl(fd,BIOCGSTATS,&s)) {
	    perror("pppoe: BIOCGSTATS");
	} else {
	    if (s.bs_drop > lastdrop) {
		fprintf(stderr, "BPF: dropped %d packets\n", s.bs_drop - lastdrop);
		lastdrop = s.bs_drop;
	    }
	}
	if ((n = read(fd,buf,bpf_buf_size)) < 0) {
	    perror("pppoe: read (read_bpf_packet)");
	    return -1;
	}
	if (n == 0)
	    return 0; /* timeout on bpf - try again */
	h = (struct bpf_hdr *)(buf);
	memcpy(packet,&(buf[h->bh_hdrlen]),h->bh_caplen);
	off = BPF_WORDALIGN(h->bh_hdrlen + h->bh_caplen);
    }
    /* need to filter packets here - interface could be in promiscuous
       mode - we shouldn't see packets that we sent out thanks to BPF, but
       a quick double-check here is unlikely to seriously impact performance
       Once you know BPF is working, you can pop this out */
    if (memcmp(packet->ethhdr.ether_shost,local_ether,6) == 0) {
#ifdef SIMPLE_BPF
	return 1; /* ignore this packet */
#else
	/* with the bigger BPF program, we should never get here */
	fprintf(stderr, "BPF program is broken\n");
	exit(1);
#endif /* SIMPLE_BPF */
    }
    
    if (memcmp(packet->ethhdr.ether_dhost,MAC_BCAST_ADDR,6) == 0 ||
	memcmp(packet->ethhdr.ether_dhost,local_ether,6) == 0)
	return 0; /* I should look at this packet */
    else {
	print_packet(packet);
	return 1; /* ignore this packet */
    }
}

int is_bpf(int fd) {
    /* is this socket tied to bpf? */
    /* quick hack() - try a trivial bpf ioctl */
    struct bpf_version v;
    return (ioctl(fd,BIOCVERSION,&v) == 0);	
}
#endif /* USE_BPF */

int 
read_packet(int sock, struct pppoe_packet *packet, int *len) 
{
/*    struct sockaddr_in from; */
#if defined(__GNU_LIBRARY__) && __GNU_LIBRARY__ < 6
    int fromlen = PACKETBUF;
#else
    socklen_t fromlen = PACKETBUF;
#endif
    time_t tm;

    time(&tm);
    
    while(1) {
#ifdef USE_BPF
	{ 
	    int j;
	    if ((j = read_bpf_packet(sock, packet)) < 0)
		return -1; /* read_bpf_packet() will report error */
	    else if (j > 0)
		continue; /* read a packet,  but not what we wanted */
	}
#else
	if (recvfrom(sock, packet, PACKETBUF, 0, 
		     NULL /*(struct sockaddr *)&from*/, &fromlen) < 0) {
	    perror("pppoe: recv (read_packet)");
	    return -1;
	}
#endif /* USE_BPF */
	if (opt_verbose)
	{
	    fprintf(log_file, "Received packet at %s", ctime(&tm));
	    print_packet(packet);
	    fputc('\n', log_file);
	}

	return sock;
    }
}

void sigchild(int src) {
    clean_child = 1;
}

void cleanup_and_exit(int status) {
    close(disc_sock);
    close(sess_sock);
    close(1);
    if (pppd_listen > 0)
#ifdef __linux__
	kill(pppd_listen, SIGTERM);
#else
        kill(SIGTERM, pppd_listen);
#endif
    if (sess_listen > 0)
#ifdef __linux__
	kill(sess_listen, SIGTERM);
#else
	kill(SIGTERM, sess_listen);
#endif
    exit(status);
}

void sigint(int src)
{
    cleanup_and_exit(1);
}


void sess_handler(void) {
    /* pull packets of sess_sock and feed to pppd */
    struct pppoe_packet *packet = NULL;
    int pkt_size;

#ifdef BUGGY_AC
/* the following code deals with buggy AC software which sometimes sends
   duplicate packets */
#define DUP_COUNT 10
#define DUP_LENGTH 20
    unsigned char dup_check[DUP_COUNT][DUP_LENGTH];
    int i, ptr = 0;
#endif /* BUGGY_AC */

#ifdef BUGGY_AC
    memset(dup_check, 0, sizeof(dup_check));
#endif

    /* allocate packet once */
    packet = malloc(PACKETBUF);
    assert(packet != NULL);
    
    fprintf(error_file, "sess_handler %d\n", getpid());
    while(1) 
    {
	while(read_packet(sess_sock,packet,&pkt_size) != sess_sock)
	    ;
#ifdef __linux__
	if (memcmp(packet->ethhdr.h_source, dst_addr, sizeof(dst_addr)) != 0)
#else
	if (memcmp(packet->ethhdr.ether_shost, dst_addr, sizeof(dst_addr)) 
	    != 0)
#endif
	    continue; /* packet not from AC */
	if (packet->session != session)
	    continue; /* discard other sessions */
#ifdef __linux__
	if (packet->ethhdr.h_proto != htons(ETH_P_PPPOE_SESS)) 
	{
	    fprintf(log_file, "pppoe: invalid session proto %x detected\n",
		    ntohs(packet->ethhdr.h_proto));
	    continue;
	}
#else
	if (packet->ethhdr.ether_type != htons(ETH_P_PPPOE_SESS)) 
	{
	    fprintf(log_file, "pppoe: invalid session proto %x detected\n",
		    ntohs(packet->ethhdr.ether_type));
	    continue;
	}
#endif
	if (packet->code != CODE_SESS) {
	    fprintf(log_file, "pppoe: invalid session code %x\n", packet->code);
	    continue;
	}
#if BUGGY_AC
	/* we need to go through a list of recently-received packets to
	   make sure the AC hasn't sent us a duplicate */
	for (i = 0; i < DUP_COUNT; i++)
	    if (memcmp(packet, dup_check[i], sizeof(dup_check[0])) == 0)
		return; /* we've received a dup packet */
#define min(a,b) ((a) < (b) ? (a) : (b))
	memcpy(dup_check[ptr], packet, min(ntohs(packet->length), 
						 sizeof(dup_check[0])));
	ptr = ++ptr % DUP_COUNT;
#endif /* BUGGY_AC */
	encode_ppp(1, (unsigned char *)(packet+1), ntohs(packet->length));
    }
}

void pppd_handler(void) {
  /* take packets from pppd and feed them to sess_sock */
  struct pppoe_packet *packet = NULL;
  unsigned char buf[PACKETBUF];
  int len, pkt_size;
  time_t tm;

  fprintf(error_file, "pppd_handler %d\n", getpid());

  /* allocate packet once */
  packet = malloc(PACKETBUF);
  assert(packet != NULL);

  while(1) {
    if ((len = read(0, buf, sizeof(buf))) < 0) {
      perror("pppoe");
      exit(1);
    }
    if (len == 0)
      continue; 

    if (opt_verbose == 1) {
	time(&tm);
	fprintf(log_file, "\n%sInput of %d bytes:\n", ctime(&tm), len);
	print_hex(buf, len);
	fputc('\n', log_file);
    }
		
    if ((pkt_size = create_sess(packet, src_addr, dst_addr, buf, len, 
				session)) == 0) {
      fprintf(error_file, "pppoe: unable to create packet\n");
      continue;
    }
    
    if (send_packet(sess_sock, packet, pkt_size, if_name) < 0) {
      fprintf(error_file, "pppoe: unable to send PPPoE packet\n");
      exit(1);
    }
  }
}

	
int main(int argc, char **argv)
{
    struct pppoe_packet *packet = NULL;
    int pkt_size;

    int opt;

    /* initialize error_file here to avoid glibc2.1 issues */
     error_file = stderr;
    
    /* parse options */
    while ((opt = getopt(argc, argv, "I:L:VE:F:")) != -1)
	switch(opt)
	{
	case 'F': /* sets invalid forwarding */
	    if (*optarg == 'a') /* always forward */
		opt_fwd = 1;
	    else if (*optarg == 's') /* search for flag */
		opt_fwd_search = 1;
	    else
		fprintf(stderr, "Invalid forward option %c\n", *optarg);
	    break;

	case 'I': /* sets interface */
	    if (if_name != NULL)
		free(if_name);
	    if ((if_name=malloc(strlen(optarg+1))) == NULL)
	    {
		fprintf(stderr, "malloc\n");
		exit(1);
	    }
	    strcpy(if_name, optarg);
	    break;
	    
	case 'L': /* log file */
	    opt_verbose = 1;
	    if (log_file != NULL)
		fclose(log_file);
	    if ((log_file=fopen(optarg, "w")) == NULL)
	    {
		fprintf(stderr, "fopen\n");
		exit(1);
	    }
	    if (setvbuf(log_file, NULL, _IONBF, 0) != 0)
	    {
		fprintf(stderr, "setvbuf\n");
		exit(1);
	    }
	    break;
	case 'V': /* version */
	    printf("pppoe version %d.%d\n", VERSION_MAJOR, VERSION_MINOR);
	    exit(0);
	    break;
	case 'E': /* error file */
	    if ((error_file = fopen(optarg, "w")) == NULL)
	    {
		fprintf(stderr, "fopen\n");
		exit(1);
	    }
	    if (setvbuf(error_file, NULL, _IONBF, 0) != 0)
	    {
		fprintf(stderr, "setvbuf\n");
		exit(1);
	    }
	    break;
	default:
	    fprintf(stderr, "Unknown option %c\n", optopt);
	    exit(1);
	}

    if (if_name == 0)
	if_name = "eth0";
    /* allocate packet once */
    packet = malloc(PACKETBUF);
    assert(packet != NULL);
    
    /* create the raw socket we need */

    signal(SIGINT, sigint);
    signal(SIGTERM, sigint);

    if ((disc_sock = open_interface(if_name,ETH_P_PPPOE_DISC,src_addr)) < 0) 
    {
	fprintf(error_file, "pppoe: unable to create raw socket\n");
	return 1;
    }

    /* initiate connection */

    /* start the PPPoE session */
    if ((pkt_size = create_padi(packet, src_addr, NULL)) == 0) {
	fprintf(stderr, "pppoe: unable to create PADI packet\n");
	exit(1);
    }
    /* send the PADI packet */
    if (send_packet(disc_sock, packet, pkt_size, if_name) < 0) {
	fprintf(stderr, "pppoe: unable to send PADI packet\n");
	exit(1);
    }
    
    /* wait for PADO */
    while (read_packet(disc_sock, packet, &pkt_size) != disc_sock || 
	   (packet->code != CODE_PADO && packet->code != CODE_PADT)) {
	fprintf(log_file, "pppoe: unexpected packet %x\n", 
		packet->code);
	continue;
    }

#ifdef __linux__
    memcpy(dst_addr, packet->ethhdr.h_source, sizeof(dst_addr));
#else
    memcpy(dst_addr, packet->ethhdr.ether_shost, sizeof(dst_addr));
#endif
    
    /* send PADR */
    if ((pkt_size = create_padr(packet, src_addr, dst_addr, NULL)) == 0) {
	fprintf(stderr, "pppoe: unable to create PADR packet\n");
	exit(1);
    }
    if (send_packet(disc_sock, packet, pkt_size+14, if_name) < 0) {
	fprintf(stderr, "pppoe: unable to send PADR packet\n");
	exit(1);
    }
    
    /* wait for PADS */
#ifdef __linux__
    while (read_packet(disc_sock, packet, &pkt_size) != disc_sock ||
	   (memcmp(packet->ethhdr.h_source,
		   dst_addr, sizeof(dst_addr)) != 0)) 
#else
    while (read_packet(disc_sock, packet, &pkt_size) != disc_sock ||
	   (memcmp(packet->ethhdr.ether_shost, 
		   dst_addr, sizeof(dst_addr)) != 0)) 
#endif
    {
	if (packet->code != CODE_PADS && packet->code != CODE_PADT)
	    fprintf(log_file, "pppoe: unexpected packet %x\n", packet->code);
	continue;
    }

    if (packet->code == CODE_PADT) /* early termination */
	cleanup_and_exit(0);
    
    session = packet->session;
    if ((sess_sock = open_interface(if_name,ETH_P_PPPOE_SESS,NULL)) < 0) {
	fprintf(log_file, "pppoe: unable to create raw socket\n");
	cleanup_and_exit(1);
    }
    
    clean_child = 0;
    signal(SIGCHLD, sigchild);
    
    /* all sockets are open fork off handlers */
    if ((sess_listen = fork()) == 0) 
	sess_handler(); /* child */
    if (sess_listen < 0) {
	perror("pppoe: fork");
	cleanup_and_exit(1);
    }
    if ((pppd_listen = fork()) == 0)
	pppd_handler(); /* child */
    if (pppd_listen < 0) {
	perror("pppoe: fork");
	cleanup_and_exit(1);
    }
    
    
    /* wait for all children to die */
    /* this is not perfect - race conditions on dying children are still
       possible */
    while(1) {
	if (waitpid((pid_t)-1,NULL,WNOHANG) < 0 && errno == ECHILD)
	    break; /* all children dead */
	if (read_packet(disc_sock, packet, &pkt_size) == disc_sock) {
	    if (packet->code == CODE_PADT)
		cleanup_and_exit(1);
	}
	/* clean up any dead children */
	while (waitpid((pid_t)-1,NULL,WNOHANG) > 0)
	    ;
	
    }
    
    return 0;
}
