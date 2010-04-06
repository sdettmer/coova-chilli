/* 
 * Copyright (C) 2003, 2004, 2005, 2006 Mondru AB.
 * Copyright (C) 2007-2010 Coova Technologies, LLC. <support@coova.com>
 * 
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 * 
 */

#include "system.h"
#include "syserr.h"
#include "radius.h"
#include "radius_wispr.h"
#include "radius_chillispot.h"
#include "redir.h"
#include "md5.h"
#include "dhcp.h"
#include "dns.h"
#include "tun.h"
#include "chilli.h"
#include "options.h"
#include "ippool.h"
#include "lookup.h"

const uint32_t DHCP_OPTION_MAGIC = 0x63825363;

static int connections = 0;

char *dhcp_state2name(int authstate) {
  switch(authstate) {
  case DHCP_AUTH_NONE:   return "none";
  case DHCP_AUTH_DROP:   return "drop";
  case DHCP_AUTH_PASS:   return "pass";
  case DHCP_AUTH_DNAT:   return "dnat";
  case DHCP_AUTH_SPLASH: return "splash";
  default:               return "unknown";
  }
}

void dhcp_list(struct dhcp_t *this, bstring s, bstring pre, bstring post, int listfmt) {
  struct dhcp_conn_t *conn = this->firstusedconn;
  if (listfmt == LIST_JSON_FMT) {
    bcatcstr(s, "{ \"sessions\":[");
  }
  while (conn) {
    if (pre) bconcat(s, pre);
    dhcp_print(this, s, listfmt, conn);
    if (post) bconcat(s, post);
    conn = conn->next;
  }
  if (listfmt == LIST_JSON_FMT) {
    bcatcstr(s, "]}");
  }
}

void dhcp_entry_for_ip(struct dhcp_t *this, bstring s, struct in_addr *ip, int listfmt) {
  struct dhcp_conn_t *conn = this->firstusedconn;
  if (listfmt == LIST_JSON_FMT) {
    bcatcstr(s, "{ \"sessions\":[");
  }
  while (conn) {
    if (conn->hisip.s_addr == ip->s_addr){
      dhcp_print(this, s, listfmt, conn);
    }
    conn = conn->next;
  }
  if (listfmt == LIST_JSON_FMT) {
    bcatcstr(s, "]}");
  }
}

void dhcp_entry_for_mac(struct dhcp_t *this, bstring s, unsigned char * hwaddr, int listfmt) {
  struct dhcp_conn_t *conn;
  if (listfmt == LIST_JSON_FMT) {
    bcatcstr(s, "{ \"sessions\":[");
  }
  if (!dhcp_hashget(this, &conn, hwaddr)) {
    dhcp_print(this, s, listfmt, conn);
  }
  if (listfmt == LIST_JSON_FMT) {
    bcatcstr(s, "]}");
  }
}

void dhcp_print(struct dhcp_t *this, bstring s, int listfmt, struct dhcp_conn_t *conn) {
  struct app_conn_t *appconn = (struct app_conn_t *)conn->peer;
  bstring b = bfromcstr("");
  bstring tmp = bfromcstr("");

  if (conn && conn->inuse) {

    if (listfmt == LIST_JSON_FMT) {

      if (conn != this->firstusedconn)
	bcatcstr(b, ",");

      bcatcstr(b, "{");

      if (appconn) {
	bcatcstr(b, "\"nasPort\":");
	bassignformat(tmp, "%d", appconn->unit);
	bconcat(b, tmp);
	bcatcstr(b, ",\"clientState\":");
	bassignformat(tmp, "%d", appconn->s_state.authenticated);
	bconcat(b, tmp);
	bcatcstr(b, ",\"macAddress\":\"");
	bassignformat(tmp, "%.2X-%.2X-%.2X-%.2X-%.2X-%.2X",
		      conn->hismac[0], conn->hismac[1], conn->hismac[2],
		      conn->hismac[3], conn->hismac[4], conn->hismac[5]);
	bconcat(b, tmp);
	bcatcstr(b, "\",\"ipAddress\":\"");
	bcatcstr(b, inet_ntoa(conn->hisip));
	bcatcstr(b, "\"");
      }

    } else {

      bassignformat(b, "%.2X-%.2X-%.2X-%.2X-%.2X-%.2X %s %s",
		    conn->hismac[0], conn->hismac[1], conn->hismac[2],
		    conn->hismac[3], conn->hismac[4], conn->hismac[5],
		    inet_ntoa(conn->hisip), dhcp_state2name(conn->authstate));

    }
    
    if (listfmt && this->cb_getinfo)
      this->cb_getinfo(conn, b, listfmt);

    if (listfmt == LIST_JSON_FMT)
      bcatcstr(b, "}");
    else
      bcatcstr(b, "\n");

    bconcat(s, b);
  }

  bdestroy(b);
  bdestroy(tmp);
}

void dhcp_release_mac(struct dhcp_t *this, uint8_t *hwaddr, int term_cause) {
  struct dhcp_conn_t *conn;
  if (!dhcp_hashget(this, &conn, hwaddr)) {
    if (conn->authstate == DHCP_AUTH_DROP &&
	term_cause != RADIUS_TERMINATE_CAUSE_ADMIN_RESET) 
      return;
    dhcp_freeconn(conn, term_cause);
  }
}

void dhcp_block_mac(struct dhcp_t *this, uint8_t *hwaddr) {
  struct dhcp_conn_t *conn;
  if (!dhcp_hashget(this, &conn, hwaddr)) {
    struct app_conn_t *appconn = (struct app_conn_t *)conn->peer;
    conn->authstate = DHCP_AUTH_DROP;
    if (appconn) appconn->dnprot = DNPROT_NULL;
  }
}

static inline int dhcp_send(struct dhcp_t *this, struct _net_interface *netif, 
			    unsigned char *hismac, uint8_t *packet, size_t length) {
  if (_options.tcpwin)
    pkt_shape_tcpwin(packet, &length);

  if (_options.tcpmss)
    pkt_shape_tcpmss(packet, &length);

#if defined(__linux__)
  {

#ifndef USING_MMAP
    if (hismac) {
      netif->dest.sll_halen = PKT_ETH_ALEN;
      memcpy(netif->dest.sll_addr, hismac, PKT_ETH_ALEN);
    } else {
      netif->dest.sll_halen = 0;
      memset(netif->dest.sll_addr, 0, sizeof(netif->dest.sll_addr));
    }
#endif

    /*log_dbg("dhcp_send() len=%d", length);*/

    return net_write2(netif, packet, length, &netif->dest);
  }
#elif defined (__FreeBSD__) || defined (__APPLE__) || defined (__OpenBSD__) || defined (__NetBSD__)
  if (write(fd, packet, length) < 0) {
    log_err(errno, "write() failed");
    return -1;
  }
#endif
  return 0;
}


/**
 * dhcp_hash()
 * Generates a 32 bit hash based on a mac address
 **/
uint32_t dhcp_hash(uint8_t *hwaddr) {
  return lookup(hwaddr, PKT_ETH_ALEN, 0);
}


/**
 * dhcp_hashinit()
 * Initialises hash tables
 **/
int dhcp_hashinit(struct dhcp_t *this, int listsize) {
  /* Determine hashlog */
  for ((this)->hashlog = 0; 
       ((1 << (this)->hashlog) < listsize);
       (this)->hashlog++);
  
  /* Determine hashsize */
  (this)->hashsize = 1 << (this)->hashlog;
  (this)->hashmask = (this)->hashsize -1;
  
  /* Allocate hash table */
  if (!((this)->hash = calloc(sizeof(struct dhcp_conn_t), (this)->hashsize))){
    /* Failed to allocate memory for hash members */
    return -1;
  }
  return 0;
}


/**
 * dhcp_hashadd()
 * Adds a connection to the hash table
 **/
int dhcp_hashadd(struct dhcp_t *this, struct dhcp_conn_t *conn) {
  uint32_t hash;
  struct dhcp_conn_t *p;
  struct dhcp_conn_t *p_prev = NULL; 

  /* Insert into hash table */
  hash = dhcp_hash(conn->hismac) & this->hashmask;
  for (p = this->hash[hash]; p; p = p->nexthash)
    p_prev = p;
  if (!p_prev)
    this->hash[hash] = conn;
  else 
    p_prev->nexthash = conn;
  return 0; /* Always OK to insert */
}


/**
 * dhcp_hashdel()
 * Removes a connection from the hash table
 **/
int dhcp_hashdel(struct dhcp_t *this, struct dhcp_conn_t *conn) {
  uint32_t hash;
  struct dhcp_conn_t *p;
  struct dhcp_conn_t *p_prev = NULL; 

  /* Find in hash table */
  hash = dhcp_hash(conn->hismac) & this->hashmask;
  for (p = this->hash[hash]; p; p = p->nexthash) {
    if (p == conn) {
      break;
    }
    p_prev = p;
  }

  if (!p_prev)
    this->hash[hash] = p->nexthash;
  else
    p_prev->nexthash = p->nexthash;
  
  return 0;
}


#ifdef ENABLE_IEEE8021Q
void dhcp_checktag(struct dhcp_conn_t *conn, uint8_t *pack) {
  if (_options.ieee8021q && is_8021q(pack)) {
    uint16_t tag = get8021q(pack);
    if (tag != conn->tag8021q) {
      conn->tag8021q = tag;
      log_dbg("IEEE 802.1Q: %.2x:%.2x:%.2x:%.2x:%.2x:%.2x on VLAN %d", 
	      conn->hismac[0], conn->hismac[1], conn->hismac[2],
	      conn->hismac[3], conn->hismac[4], conn->hismac[5],
	      (int)(ntohs(tag) & 0xFF));
    }
    if (conn->peer) {
      ((struct app_conn_t *)conn->peer)->s_state.tag8021q = conn->tag8021q;
    }
  }
}
#endif


/**
 * dhcp_hashget()
 * Uses the hash tables to find a connection based on the mac address.
 * Returns -1 if not found.
 **/
int dhcp_hashget(struct dhcp_t *this, struct dhcp_conn_t **conn, uint8_t *hwaddr) {
  struct dhcp_conn_t *p;
  uint32_t hash;

  /* Find in hash table */
  hash = dhcp_hash(hwaddr) & this->hashmask;
  for (p = this->hash[hash]; p; p = p->nexthash) {
    if ((!memcmp(p->hismac, hwaddr, PKT_ETH_ALEN)) && (p->inuse)) {
      *conn = p;
      return 0;
    }
  }
  *conn = NULL;
  return -1; /* Address could not be found */
}

/**
 * dhcp_lnkconn()
 * Allocates/link a new connection from the pool. 
 * Returns -1 if unsuccessful.
 **/
int dhcp_lnkconn(struct dhcp_t *this, struct dhcp_conn_t **conn)
{
  if (!this->firstfreeconn) {

    if (connections == _options.max_clients) {
      log_err(0, "reached max connections!");
      return -1;
    }

    ++connections;

    if (!(*conn = calloc(1, sizeof(struct dhcp_conn_t)))) {
      log_err(0, "Out of memory!");
      return -1;
    }

  } else {

    *conn = this->firstfreeconn;

    /* Remove from link of free */
    if (this->firstfreeconn->next) {
      this->firstfreeconn->next->prev = NULL;
      this->firstfreeconn = this->firstfreeconn->next;
    }
    else { /* Took the last one */
      this->firstfreeconn = NULL; 
      this->lastfreeconn = NULL;
    }
    
    /* Initialise structures */
    memset(*conn, 0, sizeof(struct dhcp_conn_t));
  }

  /* Insert into link of used */
  if (this->firstusedconn) {
    this->firstusedconn->prev = *conn;
    (*conn)->next = this->firstusedconn;
  }
  else { /* First insert */
    this->lastusedconn = *conn;
  }
  
  this->firstusedconn = *conn;

  return 0; /* Success */
}

/**
 * dhcp_newconn()
 * Allocates a new connection from the pool. 
 * Returns -1 if unsuccessful.
 **/
int dhcp_newconn(struct dhcp_t *this, 
		 struct dhcp_conn_t **conn, 
		 uint8_t *hwaddr, uint8_t *pkt)
{
  if (_options.debug) 
    log_dbg("DHCP newconn: %.2x:%.2x:%.2x:%.2x:%.2x:%.2x", 
	    hwaddr[0], hwaddr[1], hwaddr[2],
	    hwaddr[3], hwaddr[4], hwaddr[5]);

  if (dhcp_lnkconn(dhcp, conn) != 0)
    return -1;

  (*conn)->inuse = 1;
  (*conn)->parent = this;
  (*conn)->mtu = this->mtu;
  (*conn)->noc2c = this->noc2c;

  /* Application specific initialisations */
  memcpy((*conn)->hismac, hwaddr, PKT_ETH_ALEN);
  /*memcpy((*conn)->ourmac, dhcp_nexthop(this), PKT_ETH_ALEN);*/

  (*conn)->lasttime = mainclock_now();
  
  dhcp_hashadd(this, *conn);

#ifdef ENABLE_IEEE8021Q
  if (pkt) 
    dhcp_checktag(*conn, pkt);
#endif
  
  /* Inform application that connection was created */
  if (this->cb_connect)
    this->cb_connect(*conn);
  
  return 0; /* Success */
}

uint8_t * dhcp_nexthop(struct dhcp_t *this) {
  if (_options.usetap && _options.has_nexthop) 
    return _options.nexthop;

  return this->rawif.hwaddr;
}


/**
 * dhcp_freeconn()
 * Returns a connection to the pool. 
 **/
int dhcp_freeconn(struct dhcp_conn_t *conn, int term_cause)
{
  /* TODO: Always returns success? */

  struct dhcp_t *this = conn->parent;

  /* Tell application that we disconnected */
  if (this->cb_disconnect)
    this->cb_disconnect(conn, term_cause);

  if (conn->is_reserved)
    return 0;

  log_dbg("DHCP freeconn: %.2x:%.2x:%.2x:%.2x:%.2x:%.2x", 
	  conn->hismac[0], conn->hismac[1], conn->hismac[2],
	  conn->hismac[3], conn->hismac[4], conn->hismac[5]);

#ifdef HAVE_NETFILTER_COOVA
  kmod_coova_release(conn);
#endif

  /* Application specific code */
  /* First remove from hash table */
  dhcp_hashdel(this, conn);

  /* Remove from link of used */
  if ((conn->next) && (conn->prev)) {
    conn->next->prev = conn->prev;
    conn->prev->next = conn->next;
  }
  else if (conn->next) { /* && prev == 0 */
    conn->next->prev = NULL;
    this->firstusedconn = conn->next;
  }
  else if (conn->prev) { /* && next == 0 */
    conn->prev->next = NULL;
    this->lastusedconn = conn->prev;
  }
  else { /* if ((next == 0) && (prev == 0)) */
    this->firstusedconn = NULL;
    this->lastusedconn = NULL;
  }

  /* Initialise structures */
  memset(conn, 0, sizeof(*conn));

  /* Insert into link of free */
  if (this->firstfreeconn) {
    this->firstfreeconn->prev = conn;
  }
  else { /* First insert */
    this->lastfreeconn = conn;
  }

  conn->next = this->firstfreeconn;
  this->firstfreeconn = conn;

  return 0;
}


/**
 * dhcp_checkconn()
 * Checks connections to see if the lease has expired
 **/
int dhcp_checkconn(struct dhcp_t *this) {
  struct dhcp_conn_t *conn = this->firstusedconn;

  while (conn) {
    /*
    if (_options.debug)
      log_dbg("dhcp_checkconn: %d %d", mainclock_diff(conn->lasttime), (int) this->lease);
    */
    if (!conn->is_reserved && mainclock_diff(conn->lasttime) > (int) this->lease) {
      log_dbg("DHCP timeout: Removing connection");
      dhcp_freeconn(conn, RADIUS_TERMINATE_CAUSE_LOST_CARRIER);
      return 0; /* Returning after first deletion */
    }
    conn = conn->next;
  }

  return 0;
}

/**
 * dhcp_new()
 * Allocates a new instance of the library
 **/

#ifdef HAVE_NETFILTER_QUEUE
static int nfqueue_cb_in(struct nfq_q_handle *qh, struct nfgenmsg *nfmsg,
			 struct nfq_data *nfa, void *cbdata) {
  struct nfqnl_msg_packet_hdr *ph;
  struct nfqnl_msg_packet_hw *hw;
  u_int32_t id;
  char *data;
  int ret;

  int result = NF_DROP;

  hw = nfq_get_packet_hw(nfa);
  ph = nfq_get_msg_packet_hdr(nfa);

  if (ph) {
    id = ntohl(ph->packet_id);
  }
  
  /*
  mark = nfq_get_nfmark(nfa);
  ifi = nfq_get_indev(nfa);
  ifi = nfq_get_outdev(nfa);
  */
  
  ret = nfq_get_payload(nfa, &data);

  if (hw && ret > 0) {
    struct pkt_iphdr_t  *pack_iph  = (struct pkt_ipphdr_t *)data;
    struct pkt_tcphdr_t *pack_tcph = (struct pkt_tcphdr_t *)(data + PKT_IP_HLEN);
    struct pkt_udphdr_t *pack_udph = (struct pkt_udphdr_t *)(data + PKT_IP_HLEN);
    struct dhcp_conn_t *conn;
    struct in_addr addr;

    if (!dhcp_hashget(dhcp, &conn, hw->hw_addr)) {
      struct app_conn_t *appconn = (struct app_conn_t *)conn->peer;
      if (appconn->s_state.authenticated) {
	if (chilli_acct_fromsub(appconn, (size_t) ret))
	  result = NF_DROP;
	else
	  result = NF_ACCEPT;
      }
    }

    if (_options.debug) {
      addr.s_addr = pack_iph->saddr;
      log_dbg("NFQUEUE: From %.2X-%.2X-%.2X-%.2X-%.2X-%.2X %s %s", 
	      hw->hw_addr[0],hw->hw_addr[1],hw->hw_addr[2],
	      hw->hw_addr[3],hw->hw_addr[4],hw->hw_addr[5],
	      inet_ntoa(addr), 
	      result == NF_ACCEPT ? "Accept" : "Drop");
      
      addr.s_addr = pack_iph->daddr;
      log_dbg("NFQUEUE: To %s %s %d %s", 
	      inet_ntoa(addr), 
	      pack_iph->protocol == PKT_IP_PROTO_UDP ? "UDP" : 
	      pack_iph->protocol == PKT_IP_PROTO_TCP ? "TCP" : "Other",
	      pack_iph->protocol == PKT_IP_PROTO_UDP ? ntohs(pack_udph->dst) : 
	      pack_iph->protocol == PKT_IP_PROTO_TCP ? ntohs(pack_tcph->dst) : 0,
	      result == NF_ACCEPT ? "Accept" : "Drop");
    }
  }
  
  return nfq_set_verdict(qh, id, result, 0, NULL);
}

extern struct ippool_t *ippool;

static int nfqueue_cb_out(struct nfq_q_handle *qh, struct nfgenmsg *nfmsg,
			 struct nfq_data *nfa, void *cbdata) {
  struct nfqnl_msg_packet_hdr *ph;
  u_int32_t id;
  char *data;
  int ret;

  int result = NF_DROP;

  ph = nfq_get_msg_packet_hdr(nfa);

  if (ph) {
    id = ntohl(ph->packet_id);
  }
  
  ret = nfq_get_payload(nfa, &data);

  if (ret > 0) {
    struct pkt_iphdr_t  *pack_iph  = (struct pkt_ipphdr_t *)data;
    struct pkt_tcphdr_t *pack_tcph = (struct pkt_tcphdr_t *)(data + PKT_IP_HLEN);
    struct pkt_udphdr_t *pack_udph = (struct pkt_udphdr_t *)(data + PKT_IP_HLEN);
    struct dhcp_conn_t *conn;
    struct in_addr addr;

    struct ippoolm_t *ipm;
    struct app_conn_t *appconn;

    result = NF_DROP;

    addr.s_addr = pack_iph->daddr;

    if (ippool_getip(ippool, &ipm, &addr)) {
      
      if (_options.debug) 
	log_dbg("dropping packet with unknown destination: %s", inet_ntoa(addr));
      
    }
    else {
      
      if ((appconn = (struct app_conn_t *)ipm->peer) == NULL ||
	  (appconn->dnlink) == NULL) {
	log_err(0, "No %s protocol defined for %s", appconn ? "dnlink" : "peer", inet_ntoa(addr));
      }
      else {
	
	if (appconn->s_state.authenticated == 1) {
	  if (chilli_acct_tosub(appconn, ret))
	    result = NF_DROP;
	  else
	    result = NF_ACCEPT;
	}
      }
    }

    if (_options.debug) {
      addr.s_addr = pack_iph->saddr;
      log_dbg("NFQUEUE OUT: From %s %s", 
	      inet_ntoa(addr), 
	      result == NF_ACCEPT ? "Accept" : "Drop");
      
      addr.s_addr = pack_iph->daddr;
      log_dbg("NFQUEUE OUT: To %s %s %d %s", 
	      inet_ntoa(addr), 
	      pack_iph->protocol == PKT_IP_PROTO_UDP ? "UDP" : 
	      pack_iph->protocol == PKT_IP_PROTO_TCP ? "TCP" : "Other",
	      pack_iph->protocol == PKT_IP_PROTO_UDP ? ntohs(pack_udph->dst) : 
	      pack_iph->protocol == PKT_IP_PROTO_TCP ? ntohs(pack_tcph->dst) : 0,
	      result == NF_ACCEPT ? "Accept" : "Drop");
    }
  }
  
  return nfq_set_verdict(qh, id, result, 0, NULL);
}
#endif

int
dhcp_new(struct dhcp_t **pdhcp, int numconn, char *interface,
	 int usemac, uint8_t *mac, int promisc, 
	 struct in_addr *listen, int lease, int allowdyn,
	 struct in_addr *uamlisten, uint16_t uamport, int useeapol,
	 int noc2c) {
  struct dhcp_t *dhcp;
  
  if (!(dhcp = *pdhcp = calloc(sizeof(struct dhcp_t), 1))) {
    log_err(0, "calloc() failed");
    return -1;
  }

  if (net_init(&dhcp->rawif, interface, ETH_P_ALL, promisc, usemac ? mac : 0) < 0) {
    free(dhcp);
    return -1; 
  }

#ifdef HAVE_NETFILTER_QUEUE
  if (getenv("NFQUEUE_IN") && getenv("NFQUEUE_OUT")) {
    int q1 = 0, q2 = 1;
    char *e;
    if (e = getenv("NFQUEUE_IN")) q1 = atoi(e);
    if (e = getenv("NFQUEUE_OUT")) q2 = atoi(e);
    if (net_open_nfqueue(&dhcp->qif_in, q1, nfqueue_cb_in) == -1) {
      return -1;
    }
    if (net_open_nfqueue(&dhcp->qif_out, q2, nfqueue_cb_out) == -1) {
      return -1;
    }
  }
#endif

#if defined (__FreeBSD__) || defined (__APPLE__) || defined (__OpenBSD__)
  { 
    int blen=0;
    if (ioctl(dhcp->rawif.fd, BIOCGBLEN, &blen) < 0) {
      log_err(errno,"ioctl() failed!");
    }
    dhcp->rbuf_max = blen;
    if (!(dhcp->rbuf = calloc(dhcp->rbuf_max, 1))) {
      /* TODO: Free malloc */
      log_err(errno, "malloc() failed");
    }
    dhcp->rbuf_offset = 0;
    dhcp->rbuf_len = 0;
  }
#endif
  
  if (_options.dhcpgwip.s_addr != 0) {
    struct sockaddr_in addr;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    int on = 1;
    
    if (fd > 0) {

      memset(&addr, 0, sizeof(addr));
      addr.sin_family = AF_INET;
      addr.sin_addr.s_addr = dhcp->uamlisten.s_addr;

      /*
       * ====[http://tools.ietf.org/id/draft-ietf-dhc-implementation-02.txt]====
       * 4.7.2 Relay Agent Port Usage
       *    Relay agents should use port 67 as the source port number.  Relay
       *    agents always listen on port 67, but port 68 has sometimes been used
       *    as the source port number probably because it was copied from the
       *    source port of the incoming packet.
       * 
       *    Cable modem vendors would like to install filters blocking outgoing
       *    packets with source port 67.
       * 
       *    RECOMMENDATIONS:
       *      O  Relay agents MUST use 67 as their source port number.
       *      O  Relay agents MUST NOT forward packets with non-zero giaddr
       *         unless the source port number on the packet is 67.
       */

      addr.sin_port = htons(67);
      
      if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) < 0) {
	log_err(errno, "Can't set reuse option");
      }
      
      if (bind(fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
	log_err(errno, "socket or bind failed for dhcp relay!");
	close(fd);
	fd = -1;
      }
    }
      
    if (fd > 0) {
      dhcp->relayfd = fd;
    } else {
      close(dhcp->rawif.fd);
      free(dhcp);
      return -1;
    }
  }

  if (dhcp_hashinit(dhcp, dhcp->numconn))
    return -1; /* Failed to allocate hash tables */

  /* Initialise various variables */
  dhcp->ourip.s_addr = listen->s_addr;
  dhcp->lease = lease;
  dhcp->allowdyn = allowdyn;
  dhcp->uamlisten.s_addr = uamlisten->s_addr;
  dhcp->uamport = uamport;
  dhcp->mtu = _options.mtu;
  dhcp->noc2c = noc2c;

  /* Initialise call back functions */
  dhcp->cb_data_ind = 0;
  dhcp->cb_request = 0;
  dhcp->cb_disconnect = 0;
  dhcp->cb_connect = 0;
  
  return 0;
}

int dhcp_reserve_ip(uint8_t *mac, struct in_addr *ip) {
  struct dhcp_conn_t *conn = 0;

  if (dhcp_hashget(dhcp, &conn, mac)) {
    if (dhcp_newconn(dhcp, &conn, mac, 0)) {
      log_err(0, "could not allocate connection");
      return -1;
    }
  }

  conn->is_reserved = 1;
  dhcp->cb_request(conn, ip, 0, 0);

  return 0;
}

int dhcp_reserve_str(char *b, size_t blen) {
  uint8_t mac[PKT_ETH_ALEN];
  unsigned int tmp[PKT_ETH_ALEN];
  struct in_addr ip;
  int state = 0;
  int newline;
  
  char *bp = b;
  char *t;
  int i;
  
  for (i=0; state >= 0 && i < blen; i++) {
    newline = 0;
    switch(b[i]) {
    case '\r': case '\n': case ',':
      {
	newline = 1;
      }
    case ' ': case '\t': case '=':
      {
	b[i]=0;
	switch (state) {
	case 0:
	  {
	    for (t=bp; *t; t++) 
	      if (!isxdigit(*t)) 
		*t = 0x20;
	    
	    if (sscanf (bp, "%2x %2x %2x %2x %2x %2x", 
			&tmp[0], &tmp[1], &tmp[2], 
			&tmp[3], &tmp[4], &tmp[5]) != 6) {	
	      log_err(0, "MAC conversion failed!");
	      state = -1;
	    } else {
	      mac[0] = (uint8_t) tmp[0];
	      mac[1] = (uint8_t) tmp[1];
	      mac[2] = (uint8_t) tmp[2];
	      mac[3] = (uint8_t) tmp[3];
	      mac[4] = (uint8_t) tmp[4];
	      mac[5] = (uint8_t) tmp[5];
	      state = 1;
	    }
	  }
	  break;
	case 1:
	  {
	    if (!inet_aton(bp, &ip)) {	
	      log_err(0, "Bad IP address!");
	      state = -1;
	    } else {
	      state = 2;
	    }
	  }
	  break;
	}
	
	if (newline || state == 2) {
	  dhcp_reserve_ip(mac, &ip);
	  state = 0;
	}
	
	while (i < blen && (b[i] == 0 || 
			    b[i] == '\r' || 
			    b[i] == '\n' || 
			    b[i] == ' ' || 
			    b[i] == '\t')) i++;
	bp = b + (size_t) i;
	i--;
      }
      break;
    }
  }

  return 0;
}

/**
 * dhcp_set()
 * Set dhcp parameters which can be altered at runtime.
 **/
int
dhcp_set(struct dhcp_t *dhcp, char *ethers, int debug) {
  dhcp->debug = debug;
  dhcp->anydns = _options.uamanydns;

  /* Copy list of uamserver IP addresses */
  if (dhcp->authip) free(dhcp->authip);
  dhcp->authiplen = _options.uamserverlen;

  if (!(dhcp->authip = calloc(sizeof(struct in_addr), _options.uamserverlen))) {
    log_err(0, "calloc() failed");
    dhcp->authip = 0;
    return -1;
  }
  
  memcpy(dhcp->authip, &_options.uamserver, sizeof(struct in_addr) * _options.uamserverlen);

  if (ethers && *ethers) {
    int fd = open(ethers, O_RDONLY);
    if (fd > 0) {
      struct stat buf;
      int r, blen;
      char *b;

      fstat(fd, &buf);
      blen = buf.st_size;

      if (blen > 0) {
	b = malloc(blen);
	if (b) {
	  r = read(fd, b, blen);
	  if (r == blen) {
	    dhcp_reserve_str(b, blen);
	  } else {
	    log_err(0, "bad ethers file %s", ethers);
	  }
	  free(b);
	}      
      }

      close(fd);
    } else {
      log_err(0, "could not open ethers file %s", ethers);
    }
  }

  return 0;
}

/**
 * dhcp_free()
 * Releases ressources allocated to the instance of the library
 **/
int dhcp_free(struct dhcp_t *dhcp) {
  if (dhcp->hash) free(dhcp->hash);
  if (dhcp->authip) free(dhcp->authip);
  dev_set_flags(dhcp->rawif.devname, dhcp->rawif.devflags);
  net_close(&dhcp->rawif);
  free(dhcp);
  return 0;
}

/**
 * dhcp_timeout()
 * Need to call this function at regular intervals to clean up old connections.
 **/
int
dhcp_timeout(struct dhcp_t *this)
{
  /*dhcp_validate(this);*/

  dhcp_checkconn(this);
  
  return 0;
}

/**
 * dhcp_timeleft()
 * Use this function to find out when to call dhcp_timeout()
 * If service is needed after the value given by tvp then tvp
 * is left unchanged.
 **/
struct timeval* dhcp_timeleft(struct dhcp_t *this, struct timeval *tvp) {
  return tvp;
}

int check_garden(pass_through *ptlist, int ptcnt, uint8_t *pack, int dst) {
  struct pkt_iphdr_t *iph = iphdr(pack);
  struct pkt_tcphdr_t *tcph = tcphdr(pack);
  struct pkt_udphdr_t *udph = udphdr(pack);
  pass_through *pt;
  int i;

  for (i = 0; i < ptcnt; i++) {
    pt = &ptlist[i];
    if (pt->proto == 0 || iph->protocol == pt->proto)
      if (pt->host.s_addr == 0 || 
	  pt->host.s_addr == ((dst ? iph->daddr : iph->saddr) & pt->mask.s_addr))
	if (pt->port == 0 || 
	    (iph->protocol == PKT_IP_PROTO_TCP && (dst ? tcph->dst : tcph->src) == htons(pt->port)) ||
	    (iph->protocol == PKT_IP_PROTO_UDP && (dst ? udph->dst : udph->src) == htons(pt->port)))
	  return 1;
  }

  return 0;
}

static
size_t tcprst(uint8_t *tcp_pack, uint8_t *orig_pack, char reverse) {

  size_t len = sizeofeth(orig_pack) + PKT_IP_HLEN + PKT_TCP_HLEN;

  struct pkt_iphdr_t  *orig_pack_iph  = iphdr(orig_pack);
  struct pkt_tcphdr_t *orig_pack_tcph = tcphdr(orig_pack);

  struct pkt_iphdr_t *tcp_pack_iph;
  struct pkt_tcphdr_t *tcp_pack_tcph;

  memcpy(tcp_pack, orig_pack, len); 

  tcp_pack_iph = iphdr(tcp_pack);
  tcp_pack_tcph = tcphdr(tcp_pack);
  
  if (reverse) {
    struct pkt_ethhdr_t *tcp_pack_ethh  = ethhdr(tcp_pack);
    struct pkt_ethhdr_t *orig_pack_ethh = ethhdr(orig_pack);

    /* eth */
    memcpy(tcp_pack_ethh->dst, orig_pack_ethh->src, PKT_ETH_ALEN); 
    memcpy(tcp_pack_ethh->src, orig_pack_ethh->dst, PKT_ETH_ALEN); 

    /* ip */
    tcp_pack_iph->saddr = orig_pack_iph->daddr;
    tcp_pack_iph->daddr = orig_pack_iph->saddr;
    
    /* tcp */
    tcp_pack_tcph->src = orig_pack_tcph->dst;
    tcp_pack_tcph->dst = orig_pack_tcph->src;
    tcp_pack_tcph->seq = htonl(ntohl(orig_pack_tcph->seq)+1);
  }

  tcp_pack_iph->tot_len = htons(PKT_IP_HLEN + PKT_TCP_HLEN);

  tcp_pack_tcph->flags = TCPHDR_FLAG_RST;
  tcp_pack_tcph->offres = 0x50;

  chksum(tcp_pack_iph);

  return len;
}


static
void tun_sendRESET(struct tun_t *tun, uint8_t *pack, struct app_conn_t *appconn) {
#if !defined(HAVE_NETFILTER_QUEUE) && !defined(HAVE_NETFILTER_COOVA)
  uint8_t tcp_pack[PKT_BUFFER];
  tun_encaps(tun, tcp_pack, tcprst(tcp_pack, pack, 1), appconn->s_params.routeidx);
#endif
}

static
void dhcp_sendRESET(struct dhcp_conn_t *conn, uint8_t *pack, char reverse) {
#if !defined(HAVE_NETFILTER_QUEUE) && !defined(HAVE_NETFILTER_COOVA)
  uint8_t tcp_pack[PKT_BUFFER];
  struct dhcp_t *this = conn->parent;
  dhcp_send(this, &this->rawif, conn->hismac, tcp_pack, tcprst(tcp_pack, pack, reverse));
#endif
}

static
int dhcp_nakDNS(struct dhcp_conn_t *conn, uint8_t *pack, size_t len) {
  struct dhcp_t *this = conn->parent;
  struct pkt_ethhdr_t *ethh = ethhdr(pack);
  struct pkt_iphdr_t *iph = iphdr(pack);
  struct pkt_udphdr_t *udph = udphdr(pack);

  uint8_t answer[PKT_BUFFER];

  struct pkt_ethhdr_t *answer_ethh;
  struct pkt_iphdr_t *answer_iph;
  struct pkt_udphdr_t *answer_udph;
  struct dns_packet_t *answer_dns;

  memcpy(answer, pack, len); 

  answer_ethh = ethhdr(answer);
  answer_iph  = iphdr(answer);
  answer_udph = udphdr(answer);
  answer_dns  = dnspkt(answer);

  /* DNS response, with no host error code */
  answer_dns->flags = htons(0x8583); 
  
  /* UDP */
  answer_udph->src = udph->dst;
  answer_udph->dst = udph->src;
  
  /* IP */
  answer_iph->check = 0; /* Calculate at end of packet */      
  memcpy(&answer_iph->daddr, &iph->saddr, PKT_IP_ALEN);
  memcpy(&answer_iph->saddr, &iph->daddr, PKT_IP_ALEN);
  
  /* Ethernet */
  memcpy(&answer_ethh->dst, &ethh->src, PKT_ETH_ALEN);
  memcpy(&answer_ethh->src, &ethh->dst, PKT_ETH_ALEN);
    
  /* checksums */
  chksum(answer_iph);
  
  dhcp_send(this, &this->rawif, conn->hismac, answer, len);

  return 0;
}

static 
int _filterDNSreq(struct dhcp_conn_t *conn, uint8_t *pack, size_t plen) {
  struct dns_packet_t *dnsp = dnspkt(pack);
  size_t len = plen - DHCP_DNS_HLEN - sizeofudp(pack);
  size_t olen = len;

  uint16_t id = ntohs(dnsp->id);
  uint16_t flags = ntohs(dnsp->flags);
  uint16_t qdcount = ntohs(dnsp->qdcount);
  uint16_t ancount = ntohs(dnsp->ancount);
  uint16_t nscount = ntohs(dnsp->nscount);
  uint16_t arcount = ntohs(dnsp->arcount);

  uint8_t *p_pkt = (uint8_t *)dnsp->records;
  char q[256];

  int d = _options.debug; /* XXX: debug */
  int i;

  if (d) log_dbg("DNS ID:    %d", id);
  if (d) log_dbg("DNS Flags: %d", flags);

  /* it was a response? shouldn't be */
  /*if (((flags & 0x8000) >> 15) == 1) return 0;*/

  memset(q,0,sizeof(q));

#undef  copyres
#define copyres(isq,n)			        \
  if (d) log_dbg(#n ": %d", n ## count);        \
  for (i=0; i < n ## count; i++)                \
    if (dns_copy_res(isq, &p_pkt, &len,         \
		     (uint8_t *)dnsp, olen, 	\
                     q, sizeof(q)))	        \
      return dhcp_nakDNS(conn,pack,plen)

  copyres(1,qd);
  copyres(0,an);
  copyres(0,ns);
  copyres(0,ar);

  if (d) log_dbg("left (should be zero): %d", len);

  return 1;
}

static
int _filterDNSresp(struct dhcp_conn_t *conn, uint8_t *pack, size_t plen) {
  struct dns_packet_t *dnsp = dnspkt(pack);
  size_t len = plen - DHCP_DNS_HLEN - sizeofudp(pack);
  size_t olen = len;

  uint16_t id = ntohs(dnsp->id);
  uint16_t flags = ntohs(dnsp->flags);
  uint16_t qdcount = ntohs(dnsp->qdcount);
  uint16_t ancount = ntohs(dnsp->ancount);
  uint16_t nscount = ntohs(dnsp->nscount);
  uint16_t arcount = ntohs(dnsp->arcount);

  uint8_t *p_pkt = (uint8_t *)dnsp->records;
  char q[256];

  int d = _options.debug; /* XXX: debug */
  int i;

  if (d) log_dbg("DNS ID:    %d", id);
  if (d) log_dbg("DNS Flags: %d", flags);

  /* it was a query? shouldn't be */
  if (((flags & 0x8000) >> 15) == 0) return 0;

  memset(q,0,sizeof(q));

#undef  copyres
#define copyres(isq,n)			        \
  if (d) log_dbg(#n ": %d", n ## count);        \
  for (i=0; i < n ## count; i++)                \
    dns_copy_res(isq, &p_pkt, &len,             \
		     (uint8_t *)dnsp, olen, 	\
                     q, sizeof(q))

  copyres(1,qd);
  copyres(0,an);
  copyres(0,ns);
  copyres(0,ar);

  if (d) log_dbg("left (should be zero): %d", len);

  /*
  dnsp->flags = htons(flags);
  dnsp->qdcount = htons(qdcount);
  dnsp->ancount = htons(ancount);
  dnsp->nscount = htons(nscount);
  dnsp->arcount = htons(arcount);
  */

  return 1;
}

static int
dhcp_uam_nat(struct dhcp_conn_t *conn,
	     struct pkt_ethhdr_t *ethh,
	     struct pkt_iphdr_t  *iph,
	     struct pkt_tcphdr_t *tcph,
	     struct in_addr *addr, 
	     uint16_t port) {
  int n;
  int pos = -1;
  
  for (n=0; n < DHCP_DNAT_MAX; n++) {
    if (conn->dnat[n].src_ip == iph->saddr && 
	conn->dnat[n].src_port == tcph->src) {
      pos = n;
      break;
    }
  }
  
  if (pos == -1) {
    if (_options.usetap) {
      memcpy(conn->dnat[conn->nextdnat].mac, ethh->dst, PKT_ETH_ALEN); 
    }
    conn->dnat[conn->nextdnat].dst_ip = iph->daddr; 
    conn->dnat[conn->nextdnat].src_ip = iph->saddr; 
    conn->dnat[conn->nextdnat].src_port = tcph->src;
    conn->dnat[conn->nextdnat].dst_port = tcph->dst;
    conn->nextdnat = (conn->nextdnat + 1) % DHCP_DNAT_MAX;
  }
  
  if (_options.usetap) {
    memcpy(ethh->dst, tuntap(tun).hwaddr, PKT_ETH_ALEN); 
  }
  
  iph->daddr = addr->s_addr;
  tcph->dst = htons(port);
  
  chksum(iph);
  
  return 0;
}


static int
dhcp_uam_unnat(struct dhcp_conn_t *conn,
	       struct pkt_ethhdr_t *ethh,
	       struct pkt_iphdr_t  *iph,
	       struct pkt_tcphdr_t *tcph) {
  int n;
  for (n=0; n < DHCP_DNAT_MAX; n++) {
    
    if (iph->daddr == conn->dnat[n].src_ip && 
	tcph->dst == conn->dnat[n].src_port) {
      
      if (_options.usetap) {
	memcpy(ethh->src, conn->dnat[n].mac, PKT_ETH_ALEN);
      }
      
      iph->saddr = conn->dnat[n].dst_ip;
      tcph->src = conn->dnat[n].dst_port;
      
      chksum(iph);
      
      return 0; 
    }
  }
  return 0; 
}


/**
 * dhcp_doDNAT()
 * Change destination address to authentication server.
 **/
int dhcp_doDNAT(struct dhcp_conn_t *conn, uint8_t *pack, size_t len, char do_reset) {
  struct dhcp_t *this = conn->parent;
  struct pkt_ethhdr_t *ethh = ethhdr(pack);
  struct pkt_iphdr_t  *iph  = iphdr(pack);
  struct pkt_tcphdr_t *tcph = tcphdr(pack);
  struct pkt_udphdr_t *udph = udphdr(pack);
  int i;
  /* Allow localhost through network... */
  if (iph->daddr == INADDR_LOOPBACK)
    return 0;

  /* Was it an ICMP request for us? */
  if (iph->protocol == PKT_IP_PROTO_ICMP)
    if (iph->daddr == conn->ourip.s_addr)
      return 0;

  /* Was it a DNS request? */
  if ((this->anydns ||
       iph->daddr == conn->dns1.s_addr ||
       iph->daddr == conn->dns2.s_addr) &&
      iph->protocol == PKT_IP_PROTO_UDP && 
      udph->dst == htons(DHCP_DNS)) {
    
    if (this->anydns && 
	iph->daddr != conn->dns1.s_addr && 
	iph->daddr != conn->dns2.s_addr) {
      conn->dnatdns = iph->daddr;
      iph->daddr = conn->dns1.s_addr;
      chksum(iph);
    }

    if (_options.dnsparanoia) {
      if (_filterDNSreq(conn, pack, len)) 
	return 0;
      else /* drop */
	return -1;
    } else { /* allow */
      return 0;
    }
  }

  /* Was it a request for authentication server? */
  for (i = 0; i<this->authiplen; i++) {
    if ((iph->daddr == this->authip[i].s_addr) /* &&
	(iph->protocol == PKT_IP_PROTO_TCP) &&
	((tcph->dst == htons(DHCP_HTTP)) ||
	(tcph->dst == htons(DHCP_HTTPS)))*/)
      return 0; /* Destination was authentication server */
  }

  /* Was it a request for local redirection server? */
  if ( ( iph->protocol == PKT_IP_PROTO_TCP )    &&
       ( iph->daddr == this->uamlisten.s_addr ) &&
       ( tcph->dst == htons(this->uamport) || 
	 ( _options.uamuiport && tcph->dst == htons(_options.uamuiport)) ) ) {

    return 0; /* Destination was local redir server */
  }

  /* shouldn't ever get here
  if ( (iph->protocol == PKT_IP_PROTO_TCP) &&
       (_options.uamalias.s_addr) &&
       (iph->daddr == _options.uamalias.s_addr) )
    return 0; 
  */

  /* Was it a request for a pass-through entry? */
  if (check_garden(_options.pass_throughs, _options.num_pass_throughs, pack, 1))
    return 0;

  /* Check uamdomain driven walled garden */
  if (check_garden(this->pass_throughs, this->num_pass_throughs, pack, 1))
    return 0;

#ifdef ENABLE_SESSGARDEN
  /* Check appconn session specific pass-throughs */
  if (conn->peer) {
    struct app_conn_t *appconn = (struct app_conn_t *)conn->peer;
    if (check_garden(appconn->s_params.pass_throughs, appconn->s_params.pass_through_count, pack, 1))
      return 0;
  }
#endif

  if (iph->protocol == PKT_IP_PROTO_TCP) {

    if (tcph->dst == htons(DHCP_HTTP)
#ifdef HAVE_SSL
	|| (_options.redirssl && tcph->dst == htons(DHCP_HTTPS))
#endif
	) {
      /* Was it a http request for another server? */
      /* We are changing dest IP and dest port to local UAM server */

      return dhcp_uam_nat(conn, ethh, iph, tcph, &this->uamlisten, this->uamport);

    } else if (do_reset) {
      /* otherwise, RESET and drop */
      log_dbg("Resetting connection on port %d->%d", ntohs(tcph->src), ntohs(tcph->dst));
      dhcp_sendRESET(conn, pack, 1);
    }
  }
  
  return -1; /* Something else */
}

static inline int dhcp_postauthDNAT(struct dhcp_conn_t *conn, uint8_t *pack, size_t len, int isreturn) {
  struct dhcp_t *this = conn->parent;
  struct pkt_ethhdr_t *ethh = ethhdr(pack);
  struct pkt_iphdr_t  *iph  = iphdr(pack);
  struct pkt_tcphdr_t *tcph = tcphdr(pack);

  if (isreturn) {
    /* We check here (we also do this in dhcp_dounDNAT()) for UAM */
    if ( ( iph->saddr == this->uamlisten.s_addr ) &&
	 ( iph->protocol == PKT_IP_PROTO_TCP )    &&
	 ( tcph->src == htons(dhcp->uamport) ||
	   ( _options.uamuiport && tcph->src == htons(_options.uamuiport))) ) {
      
      dhcp_uam_unnat(conn, ethh, iph, tcph);
    }
  }

  if (_options.postauth_proxyport > 0) {
    if (isreturn) {
      if ((iph->protocol == PKT_IP_PROTO_TCP) &&
	  (iph->saddr == _options.postauth_proxyip.s_addr) &&
	  (tcph->src == htons(_options.postauth_proxyport))) {

	return dhcp_uam_unnat(conn, ethh, iph, tcph);
      }
    }
    else {
      if ((iph->protocol == PKT_IP_PROTO_TCP) &&
	  (tcph->dst == htons(DHCP_HTTP) 
#ifdef HAVE_SSL
	   || (_options.redirssl && tcph->dst == htons(DHCP_HTTPS))
#endif
	   )) {

	int n;

	for (n = 0; n < this->authiplen; n++)
	  if ((iph->daddr == this->authip[n].s_addr))
	      return 0;

	log_dbg("rewriting packet for post-auth proxy %s:%d",
		inet_ntoa(_options.postauth_proxyip),
		_options.postauth_proxyport);
	
	return dhcp_uam_nat(conn, ethh, iph, tcph, &_options.postauth_proxyip, _options.postauth_proxyport);
      }
    }
  }

  return -1; /* Something else */
}

/**
 * dhcp_undoDNAT()
 * Change source address back to original server
 **/
static inline int dhcp_undoDNAT(struct dhcp_conn_t *conn, uint8_t *pack, size_t *plen, char do_reset) {
  struct dhcp_t *this = conn->parent;
  struct pkt_ethhdr_t *ethh = ethhdr(pack);
  struct pkt_iphdr_t  *iph  = iphdr(pack);
  struct pkt_tcphdr_t *tcph = tcphdr(pack);
  struct pkt_udphdr_t *udph = udphdr(pack);
  /*size_t len = *plen;*/
  int i;

  /* Allow localhost through network... */
  if (iph->saddr == INADDR_LOOPBACK)
    return 0;

  /* Was it a DNS reply? */
  if ((this->anydns ||
       iph->saddr == conn->dns1.s_addr ||
       iph->saddr == conn->dns2.s_addr) &&
      iph->protocol == PKT_IP_PROTO_UDP && 
      udph->src == htons(DHCP_DNS)) {
    
    if (this->anydns && 
	conn->dnatdns && 
	iph->saddr != conn->dns1.s_addr && 
	iph->saddr != conn->dns2.s_addr &&
	iph->saddr != conn->dnatdns) {
      iph->saddr = conn->dnatdns;
      chksum(iph);
    }

    if (_options.uamdomains && _options.uamdomains[0]) {
	if (_filterDNSresp(conn, pack, *plen)) 
	  return 0;
	else
	  return -1; /* drop */
    } else {   /* always let through dns when not filtering */
      return 0;
    }
  }

  if (iph->protocol == PKT_IP_PROTO_ICMP) {
    /* Was it an ICMP reply from us? */
    if (iph->saddr == conn->ourip.s_addr)
      return 0;
    /* Allow for MTU negotiation */
    if (_options.debug)
      log_dbg("Received ICMP");
#if(0)
    switch((unsigned char)pack->payload[0]) {
    case 0:  /* echo reply */
    case 3:  /* destination unreachable */
    case 5:  /* redirect */
    case 11: /* time excedded */
      switch((unsigned char)pack->payload[1]) {
      case 4: 
	log(LOG_NOTICE, "Fragmentation needed ICMP");
      }
      if (_options.debug)
	log_dbg("Forwarding ICMP to chilli client");
      return 0;
    }
#endif
    /* fail all else */
    return -1;
  }

  /* Was it a reply from redir server? */
  if ( (iph->saddr == this->uamlisten.s_addr) &&
       (iph->protocol == PKT_IP_PROTO_TCP) &&
       (tcph->src == htons(this->uamport) || 
	(_options.uamuiport && tcph->src == htons(_options.uamuiport))) ) {

    return dhcp_uam_unnat(conn, ethh, iph, tcph);
  }
  
  /* Was it a normal http or https reply from authentication server? */
  /* Was it a normal reply from authentication server? */
  for (i = 0; i<this->authiplen; i++) {
    if ((iph->saddr == this->authip[i].s_addr) /* &&
	(iph->protocol == PKT_IP_PROTO_TCP) &&
	((tcph->src == htons(DHCP_HTTP)) ||
	(tcph->src == htons(DHCP_HTTPS)))*/)
      return 0; /* Destination was authentication server */
  }
  
  /* Was it a reply for a pass-through entry? */
  if (check_garden(_options.pass_throughs, _options.num_pass_throughs, pack, 0))
    return 0;
  if (check_garden(this->pass_throughs, this->num_pass_throughs, pack, 0))
    return 0;

#ifdef ENABLE_SESSGARDEN
  /* Check appconn session specific pass-throughs */
  if (conn->peer) {
    struct app_conn_t *appconn = (struct app_conn_t *)conn->peer;
    if (check_garden(appconn->s_params.pass_throughs, appconn->s_params.pass_through_count, pack, 0))
      return 0;
  }
#endif

  if (do_reset && iph->protocol == PKT_IP_PROTO_TCP) {
    log_dbg("Resetting connection on port %d->%d", ntohs(tcph->src), ntohs(tcph->dst));
    dhcp_sendRESET(conn, pack, 0);
    if (conn->peer) {
      tun_sendRESET(tun, pack, (struct app_conn_t *)conn->peer);
    }
  }
  
  return -1; /* Something else */
}

static int dhcp_matchDNS(uint8_t *r, uint8_t *b, size_t blen, char *name) {
  int r_len = strlen((char *)r);
  int name_len = strlen(name);
  int domain_len = _options.domain ? strlen(_options.domain) : 0;
  
  if (r_len == name_len && !memcmp(r, name, name_len)) {
    return 1;
  }
  
  if (domain_len > 0 && 
      r_len == (name_len + domain_len + 1) &&
      !memcmp(r, name, name_len) &&
      !memcmp(r + name_len + 1, _options.domain, domain_len) &&
      r[name_len] == '.') {
    return 1;
  }
  
  return 0;
}

/**
 * dhcp_localDNS()
 * Check if it was request for known domain name.
 * In case it was a request for a known keyword then
 * redirect to the login/logout page
 **/
int dhcp_localDNS(struct dhcp_conn_t *conn, uint8_t *pack, size_t len) {

  struct dhcp_t *this = conn->parent;
  struct pkt_ethhdr_t *ethh = ethhdr(pack);
  struct pkt_iphdr_t  *iph  = iphdr(pack);
  struct pkt_udphdr_t *udph = udphdr(pack);
  struct dns_packet_t *dnsp = dnspkt(pack);

  size_t length;
  size_t udp_len;
  uint8_t query[256];
  size_t query_len = 0;
  int n;

  char *hostname = _options.uamhostname;
  char *aliasname = _options.uamaliasname;

  log_dbg("DNS ID:    %d", ntohs(dnsp->id));
  log_dbg("DNS flags: %d", ntohs(dnsp->flags));

  if ((ntohs(dnsp->flags)   == 0x0100) &&
      (ntohs(dnsp->qdcount) == 0x0001) &&
      (ntohs(dnsp->ancount) == 0x0000) &&
      (ntohs(dnsp->nscount) == 0x0000) &&
      (ntohs(dnsp->arcount) == 0x0000)) {

    uint8_t name[256];

    int match = 0;
    uint8_t reply[4];

    uint8_t *p = dnsp->records;
    size_t plen = len - DHCP_DNS_HLEN - sizeofudp(pack);
    size_t olen = plen;

    uint16_t type;
    uint16_t class;
    uint16_t us;
    
    if (dns_getname(&p, &plen, 0, 128, 0) || 
	plen < 4) {
      log_dbg("failed parsing DNS packet"); 
      return -1; 
    }
    
    memcpy(&us, p, sizeof(us));
    type = ntohs(us);
    p += 2;
    plen -= 2;
    
    memcpy(&us, p, sizeof(us));
    class = ntohs(us);
    p += 2;
    plen -= 2;
    
    log_dbg("LocalDNS: It was a dns record type: %d class: %d", type, class);

    memset(name, 0, sizeof(name));
    
    dns_fullname(name, sizeof(name), dnsp->records, (uint8_t *)dnsp, olen, 0);
    
    log_dbg("LocalDNS: Q: %s", name);
    
    switch (type) {
    case 1: break; 
    default: return 0;
    }
    
    match = dhcp_matchDNS(name, query, sizeof(query), "logout");
    
    if (match) {
      memcpy(reply, &_options.uamlogout.s_addr, 4);
    }
    
    if (!match && hostname) {
      match = dhcp_matchDNS(name, query, sizeof(query), hostname);

      if (match) {
	memcpy(reply, &_options.uamlisten.s_addr, 4);
      }
    }

    if (!match && aliasname) {
      match = dhcp_matchDNS(name, query, sizeof(query), aliasname);

      if (match) {
	memcpy(reply, &_options.uamalias.s_addr, 4);
      }
    }
    
    if (!match && _options.domaindnslocal && _options.domain) {
      int name_len = strlen((char *)name);
      int domain_len = strlen(_options.domain);
      
      if (name_len > (domain_len + 1)) {
	int off = name_len - domain_len;

	if (!memcmp(name + off, _options.domain, domain_len) &&
	    name[off - 1] == '.') {
	  
	  /*
	   * count (recent) dns requests vs responses to get an overall picture of on-line status.
	   */
	  
	  memcpy(reply, &_options.uamalias.s_addr, 4);
	  match = 1;
	}
      }
    }
    
    if (match) {
      
      uint8_t answer[PKT_BUFFER];
      
      struct pkt_ethhdr_t *answer_ethh;
      struct pkt_iphdr_t  *answer_iph;
      struct pkt_udphdr_t *answer_udph;
      struct dns_packet_t *answer_dns;

      p = dnsp->records;
      
      query_len = 0;
      
      log_dbg("It was a matching query!\n");
      
      do {
	if (query_len < 256)
	  query[query_len++] = *p;
      }
      while (*p++ != 0); /* TODO */
      
      for (n=0; n<4; n++) {
	if (query_len < 256)
	  query[query_len++] = *p++;
      }
      
      query[query_len++] = 0xc0;
      query[query_len++] = 0x0c;
      query[query_len++] = 0x00;
      query[query_len++] = 0x01;
      query[query_len++] = 0x00;
      query[query_len++] = 0x01;
      query[query_len++] = 0x00;
      query[query_len++] = 0x00;
      query[query_len++] = 0x01;
      query[query_len++] = 0x2c;
      query[query_len++] = 0x00;
      query[query_len++] = 0x04;
      memcpy(query + query_len, reply, 4);
      query_len += 4;
      
      memcpy(answer, pack, len); /* TODO */
      
      answer_ethh = ethhdr(answer);
      answer_iph = iphdr(answer);
      answer_udph = udphdr(answer);
      answer_dns = dnspkt(answer);
      
      /* DNS Header */
      answer_dns->id      = dnsp->id;
      answer_dns->flags   = htons(0x8000);
      answer_dns->qdcount = htons(0x0001);
      answer_dns->ancount = htons(0x0001);
      answer_dns->nscount = htons(0x0000);
      answer_dns->arcount = htons(0x0000);
      memcpy(answer_dns->records, query, query_len);
      
      /* UDP header */
      udp_len = query_len + DHCP_DNS_HLEN + PKT_UDP_HLEN;
      answer_udph->len = htons(udp_len);
      answer_udph->src = udph->dst;
      answer_udph->dst = udph->src;
      
      /* Ip header */
      answer_iph->version_ihl = PKT_IP_VER_HLEN;
      answer_iph->tos = 0;
      answer_iph->tot_len = htons(udp_len + PKT_IP_HLEN);
      answer_iph->id = 0;
      answer_iph->frag_off = 0;
      answer_iph->ttl = 0x10;
      answer_iph->protocol = 0x11;
      answer_iph->check = 0; /* Calculate at end of packet */      
      memcpy(&answer_iph->daddr, &iph->saddr, PKT_IP_ALEN);
      memcpy(&answer_iph->saddr, &iph->daddr, PKT_IP_ALEN);

      /* Ethernet header */
      memcpy(answer_ethh->dst, &ethh->src, PKT_ETH_ALEN);
      memcpy(answer_ethh->src, &ethh->dst, PKT_ETH_ALEN);

      /* Work out checksums */
      chksum(answer_iph);

      /* Calculate total length */
      length = udp_len + sizeofip(answer);
      
      return dhcp_send(this, &this->rawif, conn->hismac, answer, length);
    }
  }

  return 0; /* Something else */
}

/**
 * dhcp_getdefault()
 * Fill in a DHCP packet with most essential values
 **/
int
dhcp_getdefault(uint8_t *pack) {

  return 0;
}

/**
 * dhcp_create_pkt()
 * Create a new typed DHCP packet
 */
int
dhcp_create_pkt(uint8_t type, uint8_t *pack, uint8_t *req, struct dhcp_conn_t *conn) {
  struct dhcp_t *this = conn->parent;

  struct dhcp_packet_t *req_dhcp = dhcppkt(req);

  struct pkt_ethhdr_t *pack_ethh;
  struct pkt_iphdr_t *pack_iph;
  struct pkt_udphdr_t *pack_udph;
  struct dhcp_packet_t *pack_dhcp;

  int pos = 0;

  int is_req_dhcp = (req_dhcp->options[0] == 0x63 &&
		     req_dhcp->options[1] == 0x82 &&
		     req_dhcp->options[2] == 0x53 &&
		     req_dhcp->options[3] == 0x63);

  copy_ethproto(req, pack);

  pack_ethh = ethhdr(pack);
  pack_iph = iphdr(pack);
  pack_udph = udphdr(pack);
  pack_dhcp = dhcppkt(pack);

  pack_dhcp->op     = DHCP_BOOTREPLY;
  pack_dhcp->htype  = DHCP_HTYPE_ETH;
  pack_dhcp->hlen   = PKT_ETH_ALEN;

  /* IP header */
  pack_iph->version_ihl = PKT_IP_VER_HLEN;
  pack_iph->tos = 0;
  pack_iph->tot_len = 0; /* Calculate at end of packet */
  pack_iph->id = 0;
  pack_iph->frag_off = 0;
  pack_iph->ttl = 0x10;
  pack_iph->protocol = 0x11;
  pack_iph->check = 0; /* Calculate at end of packet */

  if (is_req_dhcp) {
    pack_dhcp->xid      = req_dhcp->xid;
    pack_dhcp->flags[0] = req_dhcp->flags[0];
    pack_dhcp->flags[1] = req_dhcp->flags[1];
    pack_dhcp->giaddr   = req_dhcp->giaddr;

    memcpy(&pack_dhcp->chaddr, &req_dhcp->chaddr, DHCP_CHADDR_LEN);
    memcpy(&pack_dhcp->sname, conn->dhcp_opts.sname, DHCP_SNAME_LEN);
    memcpy(&pack_dhcp->file, conn->dhcp_opts.file, DHCP_FILE_LEN);

    log_dbg("!!! dhcp server : %s !!!", pack_dhcp->sname);
  }

  switch(type) {
  case DHCPOFFER:
  case DHCPFORCERENEW:
    pack_dhcp->yiaddr = conn->hisip.s_addr;
    break;
  case DHCPACK:
    pack_dhcp->xid    = req_dhcp->xid;
    pack_dhcp->yiaddr = conn->hisip.s_addr;
    break;
  case DHCPNAK:
    break;
  }

  /* Ethernet Header */
  memcpy(pack_ethh->dst, conn->hismac, PKT_ETH_ALEN);
  memcpy(pack_ethh->src, dhcp_nexthop(this), PKT_ETH_ALEN);
  
  /* UDP and IP Headers */
  pack_udph->src = htons(DHCP_BOOTPS);
  pack_iph->saddr = conn->ourip.s_addr;

  /** http://www.faqs.org/rfcs/rfc1542.html
      BOOTREQUEST fields     BOOTREPLY values for UDP, IP, link-layer
   +-----------------------+-----------------------------------------+
   | 'ciaddr'  'giaddr'  B | UDP dest     IP destination   link dest |
   +-----------------------+-----------------------------------------+
   | non-zero     X      X | BOOTPC (68)  'ciaddr'         normal    |
   | 0.0.0.0   non-zero  X | BOOTPS (67)  'giaddr'         normal    |
   | 0.0.0.0   0.0.0.0   0 | BOOTPC (68)  'yiaddr'         'chaddr'  |
   | 0.0.0.0   0.0.0.0   1 | BOOTPC (68)  255.255.255.255  broadcast |
   +-----------------------+-----------------------------------------+

        B = BROADCAST flag

        X = Don't care

   normal = determine from the given IP destination using normal
            IP routing mechanisms and/or ARP as for any other
            normal datagram

   If the 'giaddr' field in a DHCP message from a client is non-zero,
   the server sends any return messages to the 'DHCP server' port on the
   BOOTP relay agent whose address appears in 'giaddr'. 

   If the 'giaddr' field is zero and the 'ciaddr' field is nonzero, then the
   server unicasts DHCPOFFER and DHCPACK messages to the address in
   'ciaddr'.  

   If 'giaddr' is zero and 'ciaddr' is zero, and the broadcast bit is set,
   then the server broadcasts DHCPOFFER and DHCPACK messages to
   0xffffffff. 

   If the broadcast bit is not set and 'giaddr' is zero and 'ciaddr' is
   zero, then the server unicasts DHCPOFFER and DHCPACK messages to the
   client's hardware address and 'yiaddr' address.  

   In all cases, when 'giaddr' is zero, the server broadcasts any DHCPNAK
   messages to 0xffffffff.

  **/

  if (is_req_dhcp) {
    if (req_dhcp->ciaddr) {
      pack_iph->daddr = req_dhcp->ciaddr; 
      pack_udph->dst = htons(DHCP_BOOTPC);
    } else if (req_dhcp->giaddr) {
      pack_iph->daddr = req_dhcp->giaddr; 
      pack_udph->dst = htons(DHCP_BOOTPS);
    } else if (type == DHCPNAK ||            /* Nak always to broadcast */
	       req_dhcp->flags[0] & 0x80 ||  /* Broadcast bit set */
	       _options.dhcp_broadcast) {    /* Optional always send to broadcast */
      pack_iph->daddr = ~0; 
      pack_udph->dst = htons(DHCP_BOOTPC);
      pack_dhcp->flags[0] = 0x80;
    } else {
      pack_iph->daddr = pack_dhcp->yiaddr; 
      pack_udph->dst = htons(DHCP_BOOTPC);
    }
  } else {
    struct pkt_iphdr_t *iph = iphdr(req);
    pack_iph->daddr = iph->saddr;
    pack_udph->dst = htons(DHCP_BOOTPC);
  }

  /* Magic cookie */
  pack_dhcp->options[pos++] = 0x63;
  pack_dhcp->options[pos++] = 0x82;
  pack_dhcp->options[pos++] = 0x53;
  pack_dhcp->options[pos++] = 0x63;

  pack_dhcp->options[pos++] = DHCP_OPTION_MESSAGE_TYPE;
  pack_dhcp->options[pos++] = 1;
  pack_dhcp->options[pos++] = type;

  memcpy(&pack_dhcp->options[pos], conn->dhcp_opts.options, DHCP_OPTIONS_LEN-pos);
  pos += conn->dhcp_opts.option_length;

  return pos;
}


/**
 * dhcp_gettag()
 * Search a DHCP packet for a particular tag.
 * Returns -1 if not found.
 **/
int dhcp_gettag(struct dhcp_packet_t *pack, size_t length,
		struct dhcp_tag_t **tag, uint8_t tagtype) {
  struct dhcp_tag_t *t;
  size_t offset = DHCP_MIN_LEN + DHCP_OPTION_MAGIC_LEN;

  /* if (length > DHCP_LEN) {
    log_warn(0,"Length of dhcp packet larger then %d: %d", DHCP_LEN, length);
    length = DHCP_LEN;
  } */
  
  while ((offset + 2) < length) {
    t = (struct dhcp_tag_t *)(((uint8_t *)pack) + offset);
    if (t->t == tagtype) {
      if ((offset + 2 + (size_t)(t->l)) > length)
	return -1; /* Tag length too long */
      *tag = t;
      return 0;
    }
    offset += 2 + t->l;
  }
  
  return -1; /* Not found  */
}


/**
 * dhcp_sendOFFER()
 * Send of a DHCP offer message to a peer.
 **/
int dhcp_sendOFFER(struct dhcp_conn_t *conn, uint8_t *pack, size_t len) {

  struct dhcp_t *this = conn->parent;

  uint8_t packet[PKT_BUFFER];

  struct pkt_iphdr_t *packet_iph;
  struct pkt_udphdr_t *packet_udph;
  struct dhcp_packet_t *packet_dhcp;

  uint16_t length = 576 + 4; /* Maximum length */
  uint16_t udp_len = 576 - 20; /* Maximum length */
  size_t pos = 0;

  /* Get packet default values */
  memset(packet, 0, sizeof(packet));
  pos = dhcp_create_pkt(DHCPOFFER, packet, pack, conn);

  packet_iph = iphdr(packet);
  packet_udph = udphdr(packet);
  packet_dhcp = dhcppkt(packet);
  
  /* DHCP Payload */

  packet_dhcp->options[pos++] = DHCP_OPTION_SUBNET_MASK;
  packet_dhcp->options[pos++] = 4;
  if (conn->noc2c)
    memset(&packet_dhcp->options[pos], 0xff, 4);
  else
    memcpy(&packet_dhcp->options[pos], &conn->hismask.s_addr, 4);
  pos += 4;

  if (conn->noc2c) {
    packet_dhcp->options[pos++] = DHCP_OPTION_STATIC_ROUTES;
    packet_dhcp->options[pos++] = 8;
    memcpy(&packet_dhcp->options[pos], &conn->ourip.s_addr, 4);
    pos += 4;
    memcpy(&packet_dhcp->options[pos], &conn->hisip.s_addr, 4);
    pos += 4;
  }

  packet_dhcp->options[pos++] = DHCP_OPTION_ROUTER_OPTION;
  packet_dhcp->options[pos++] = 4;
  memcpy(&packet_dhcp->options[pos], &conn->ourip.s_addr, 4);
  pos += 4;

  /* Insert DNS Servers if given */
  if (conn->dns1.s_addr && conn->dns2.s_addr) {
    packet_dhcp->options[pos++] = DHCP_OPTION_DNS;
    packet_dhcp->options[pos++] = 8;
    memcpy(&packet_dhcp->options[pos], &conn->dns1.s_addr, 4);
    pos += 4;
    memcpy(&packet_dhcp->options[pos], &conn->dns2.s_addr, 4);
    pos += 4;
  }
  else if (conn->dns1.s_addr) {
    packet_dhcp->options[pos++] = DHCP_OPTION_DNS;
    packet_dhcp->options[pos++] = 4;
    memcpy(&packet_dhcp->options[pos], &conn->dns1.s_addr, 4);
    pos += 4;
  }
  else if (conn->dns2.s_addr) {
    packet_dhcp->options[pos++] = DHCP_OPTION_DNS;
    packet_dhcp->options[pos++] = 4;
    memcpy(&packet_dhcp->options[pos], &conn->dns2.s_addr, 4);
    pos += 4;
  }

  /* Insert Domain Name if present */
  if (strlen(conn->domain)) {
    packet_dhcp->options[pos++] = DHCP_OPTION_DOMAIN_NAME;
    packet_dhcp->options[pos++] = strlen(conn->domain);
    memcpy(&packet_dhcp->options[pos], &conn->domain, strlen(conn->domain));
    pos += strlen(conn->domain);
  }

  packet_dhcp->options[pos++] = DHCP_OPTION_LEASE_TIME;
  packet_dhcp->options[pos++] = 4;
  packet_dhcp->options[pos++] = (this->lease >> 24) & 0xFF;
  packet_dhcp->options[pos++] = (this->lease >> 16) & 0xFF;
  packet_dhcp->options[pos++] = (this->lease >>  8) & 0xFF;
  packet_dhcp->options[pos++] = (this->lease >>  0) & 0xFF;

  /* Must be listening address */
  packet_dhcp->options[pos++] = DHCP_OPTION_SERVER_ID;
  packet_dhcp->options[pos++] = 4;
  memcpy(&packet_dhcp->options[pos], &conn->ourip.s_addr, 4);
  pos += 4;

  packet_dhcp->options[pos++] = DHCP_OPTION_END;

  /* UDP header */
  udp_len = pos + DHCP_MIN_LEN + PKT_UDP_HLEN;
  packet_udph->len = htons(udp_len);

  /* IP header */
  packet_iph->tot_len = htons(udp_len + PKT_IP_HLEN);

  /* Work out checksums */
  chksum(packet_iph);

  /* Calculate total length */
  length = udp_len + sizeofip(packet);

  return dhcp_send(this, &this->rawif, conn->hismac, packet, length);
}

/**
 * dhcp_sendACK()
 * Send of a DHCP acknowledge message to a peer.
 **/
int dhcp_sendACK(struct dhcp_conn_t *conn, uint8_t *pack, size_t len) {

  struct dhcp_t *this = conn->parent;

  uint8_t packet[PKT_BUFFER];

  struct pkt_iphdr_t *packet_iph;
  struct pkt_udphdr_t *packet_udph;
  struct dhcp_packet_t *packet_dhcp;

  uint16_t length = 576 + 4; /* Maximum length */
  uint16_t udp_len = 576 - 20; /* Maximum length */
  size_t pos = 0;

  /* Get packet default values */
  memset(packet, 0, sizeof(packet));
  pos = dhcp_create_pkt(DHCPACK, packet, pack, conn);

  packet_iph = iphdr(packet);
  packet_udph = udphdr(packet);
  packet_dhcp = dhcppkt(packet);
  
  /* DHCP Payload */
  packet_dhcp->options[pos++] = DHCP_OPTION_SUBNET_MASK;
  packet_dhcp->options[pos++] = 4;
  if (conn->noc2c)
    memset(&packet_dhcp->options[pos], 0xff, 4);
  else
    memcpy(&packet_dhcp->options[pos], &conn->hismask.s_addr, 4);
  pos += 4;

  if (conn->noc2c) {
    packet_dhcp->options[pos++] = DHCP_OPTION_STATIC_ROUTES;
    packet_dhcp->options[pos++] = 8;
    memcpy(&packet_dhcp->options[pos], &conn->ourip.s_addr, 4);
    pos += 4;
    memcpy(&packet_dhcp->options[pos], &conn->hisip.s_addr, 4);
    pos += 4;
  }

  packet_dhcp->options[pos++] = DHCP_OPTION_ROUTER_OPTION;
  packet_dhcp->options[pos++] = 4;
  memcpy(&packet_dhcp->options[pos], &conn->ourip.s_addr, 4);
  pos += 4;

  /* Insert DNS Servers if given */
  if (conn->dns1.s_addr && conn->dns2.s_addr) {
    packet_dhcp->options[pos++] = DHCP_OPTION_DNS;
    packet_dhcp->options[pos++] = 8;
    memcpy(&packet_dhcp->options[pos], &conn->dns1.s_addr, 4);
    pos += 4;
    memcpy(&packet_dhcp->options[pos], &conn->dns2.s_addr, 4);
    pos += 4;
  }
  else if (conn->dns1.s_addr) {
    packet_dhcp->options[pos++] = DHCP_OPTION_DNS;
    packet_dhcp->options[pos++] = 4;
    memcpy(&packet_dhcp->options[pos], &conn->dns1.s_addr, 4);
    pos += 4;
  }
  else if (conn->dns2.s_addr) {
    packet_dhcp->options[pos++] = DHCP_OPTION_DNS;
    packet_dhcp->options[pos++] = 4;
    memcpy(&packet_dhcp->options[pos], &conn->dns2.s_addr, 4);
    pos += 4;
  }

  /* Insert Domain Name if present */
  if (strlen(conn->domain)) {
    packet_dhcp->options[pos++] = DHCP_OPTION_DOMAIN_NAME;
    packet_dhcp->options[pos++] = strlen(conn->domain);
    memcpy(&packet_dhcp->options[pos], &conn->domain, strlen(conn->domain));
    pos += strlen(conn->domain);
  }

  packet_dhcp->options[pos++] = DHCP_OPTION_LEASE_TIME;
  packet_dhcp->options[pos++] = 4;
  packet_dhcp->options[pos++] = (this->lease >> 24) & 0xFF;
  packet_dhcp->options[pos++] = (this->lease >> 16) & 0xFF;
  packet_dhcp->options[pos++] = (this->lease >>  8) & 0xFF;
  packet_dhcp->options[pos++] = (this->lease >>  0) & 0xFF;

  packet_dhcp->options[pos++] = DHCP_OPTION_INTERFACE_MTU;
  packet_dhcp->options[pos++] = 2;
  packet_dhcp->options[pos++] = (conn->mtu >> 8) & 0xFF;
  packet_dhcp->options[pos++] = (conn->mtu >> 0) & 0xFF;

  /* Must be listening address */
  packet_dhcp->options[pos++] = DHCP_OPTION_SERVER_ID;
  packet_dhcp->options[pos++] = 4;
  memcpy(&packet_dhcp->options[pos], &conn->ourip.s_addr, 4);
  pos += 4;

  packet_dhcp->options[pos++] = DHCP_OPTION_END;

  /* UDP header */
  udp_len = pos + DHCP_MIN_LEN + PKT_UDP_HLEN;
  packet_udph->len = htons(udp_len);

  /* IP header */
  packet_iph->tot_len = htons(udp_len + PKT_IP_HLEN);

  /* Work out checksums */
  chksum(packet_iph);

  /* Calculate total length */
  length = udp_len + sizeofip(packet);

  return dhcp_send(this, &this->rawif, conn->hismac, packet, length);
}

int dhcp_sendTYPE(struct dhcp_conn_t *conn, uint8_t *pack, size_t len, int type) {

  struct dhcp_t *this = conn->parent;
  uint8_t packet[PKT_BUFFER];

  struct pkt_iphdr_t *packet_iph;
  struct pkt_udphdr_t *packet_udph;
  struct dhcp_packet_t *packet_dhcp;

  uint16_t length = 576 + 4; /* Maximum length */
  uint16_t udp_len = 576 - 20; /* Maximum length */
  size_t pos = 0;

  /* Get packet default values */
  memset(packet, 0, sizeof(packet));
  pos = dhcp_create_pkt(type, packet, pack, conn);

  packet_iph = iphdr(packet);
  packet_udph = udphdr(packet);
  packet_dhcp = dhcppkt(packet);

  /* DHCP Payload */

  /* Must be listening address */
  packet_dhcp->options[pos++] = DHCP_OPTION_SERVER_ID;
  packet_dhcp->options[pos++] = 4;
  memcpy(&packet_dhcp->options[pos], &conn->ourip.s_addr, 4);
  pos += 4;

  packet_dhcp->options[pos++] = DHCP_OPTION_END;

  /* UDP header */
  udp_len = pos + DHCP_MIN_LEN + PKT_UDP_HLEN;
  packet_udph->len = htons(udp_len);

  /* IP header */
  packet_iph->tot_len = htons(udp_len + PKT_IP_HLEN);

  /* Work out checksums */
  chksum(packet_iph);

  /* Calculate total length */
  length = udp_len + sizeofip(packet);

  return dhcp_send(this, &this->rawif, conn->hismac, packet, length);
}

/**
 * dhcp_sendNAK()
 * Send of a DHCP negative acknowledge message to a peer.
 * NAK messages are always sent to broadcast IP address (
 * except when using a DHCP relay server)
 **/
int dhcp_sendNAK(struct dhcp_conn_t *conn, uint8_t *pack, size_t len) {
  return dhcp_sendTYPE(conn, pack, len, DHCPNAK);
}

/* Requires DHCP-AUTH
int dhcp_sendRENEW(struct dhcp_conn_t *conn, uint8_t *pack, size_t len) {
  return dhcp_sendTYPE(conn, pack, len, DHCPFORCERENEW);
}
*/

/**
 *  dhcp_getreq()
 *  Process a received DHCP request and sends a response.
 **/
int dhcp_getreq(struct dhcp_t *this, uint8_t *pack, size_t len) {
  uint8_t mac[PKT_ETH_ALEN];
  struct dhcp_tag_t *message_type = 0;
  struct dhcp_tag_t *requested_ip = 0;
  struct dhcp_conn_t *conn;
  struct in_addr addr;

  struct pkt_ethhdr_t *pack_ethh = ethhdr(pack);
  struct pkt_udphdr_t *pack_udph = udphdr(pack);
  struct dhcp_packet_t *pack_dhcp = dhcppkt(pack);

  if (pack_udph->dst != htons(DHCP_BOOTPS)) 
    return 0; /* Not a DHCP packet */

  if (dhcp_gettag(dhcppkt(pack), ntohs(pack_udph->len)-PKT_UDP_HLEN, 
		  &message_type, DHCP_OPTION_MESSAGE_TYPE)) {
    return -1;
  }

  if (message_type->l != 1)
    return -1; /* Wrong length of message type */

  if (pack_dhcp->giaddr)
    memcpy(mac, pack_dhcp->chaddr, PKT_ETH_ALEN);
  else
    memcpy(mac, pack_ethh->src, PKT_ETH_ALEN);

  switch(message_type->v[0]) {

  case DHCPRELEASE:
    dhcp_release_mac(this, mac, RADIUS_TERMINATE_CAUSE_LOST_CARRIER);

  case DHCPDISCOVER:
  case DHCPREQUEST:
  case DHCPINFORM:
    break;

  default:
    return 0; /* Unsupported message type */
  }

  if (this->relayfd > 0) {
    /** Relay the DHCP request **/
    struct sockaddr_in addr;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = _options.dhcpgwip.s_addr;
    addr.sin_port = htons(_options.dhcpgwport);

    if (_options.dhcprelayip.s_addr)
      pack_dhcp->giaddr = _options.dhcprelayip.s_addr;
    else
      pack_dhcp->giaddr = _options.uamlisten.s_addr;

    /* if we can't send, lets do dhcp ourselves */
    if (sendto(this->relayfd, dhcppkt(pack), ntohs(pack_udph->len) - PKT_UDP_HLEN, 0, 
	       (struct sockaddr *)&addr, sizeof(addr)) < 0) {
      log_err(errno, "could not relay DHCP request!");
    }
    else {
      return 0;
    }
  }

  if (message_type->v[0] == DHCPRELEASE) {
    /* No Reply to client is sent */
    return 0;
  }

  /* Check to see if we know MAC address. If not allocate new conn */
  if (dhcp_hashget(this, &conn, mac)) {
    
    /* Do we allow dynamic allocation of IP addresses? */
    if (!this->allowdyn) /* TODO: Should be deleted! */
      return 0; 

    /* Allocate new connection */
    if (dhcp_newconn(this, &conn, mac, pack)) /* TODO: Delete! */
      return 0; /* Out of connections */
  }

  if (conn->authstate == DHCP_AUTH_DROP)
    return 0;

  /* Request an IP address */ 
  if (conn->authstate == DHCP_AUTH_NONE) {
    addr.s_addr = pack_dhcp->ciaddr;
    if (this->cb_request)
      if (this->cb_request(conn, &addr, pack, len)) {
	return 0; /* Ignore request if IP address was not allocated */
      }
  }
  
  conn->lasttime = mainclock_now();

  /* Discover message */
  /* If an IP address was assigned offer it to the client */
  /* Otherwise ignore the request */
  if (message_type->v[0] == DHCPDISCOVER) {
    if (conn->hisip.s_addr) 
      dhcp_sendOFFER(conn, pack, len);
    return 0;
  }
  
  /* Request message */
  if (message_type->v[0] == DHCPREQUEST) {
    
    if (!conn->hisip.s_addr) {
      if (this->debug) log_dbg("hisip not set");
      return dhcp_sendNAK(conn, pack, len);
    }

    if (!memcmp(&conn->hisip.s_addr, &pack_dhcp->ciaddr, 4)) {
      if (this->debug) log_dbg("hisip match ciaddr");
      return dhcp_sendACK(conn, pack, len);
    }

    if (!dhcp_gettag(dhcppkt(pack), ntohs(pack_udph->len)-PKT_UDP_HLEN, 
		     &requested_ip, DHCP_OPTION_REQUESTED_IP)) {
      if (!memcmp(&conn->hisip.s_addr, requested_ip->v, 4))
	return dhcp_sendACK(conn, pack, len);
    }

    if (this->debug) log_dbg("Sending NAK to client");
    return dhcp_sendNAK(conn, pack, len);
  }
  
  /* 
   *  Unsupported DHCP message: Ignore 
   */
  if (this->debug) log_dbg("Unsupported DHCP message ignored");
  return 0;
}


/**
 * dhcp_set_addrs()
 * Set various IP addresses of a connection.
 **/
int dhcp_set_addrs(struct dhcp_conn_t *conn, struct in_addr *hisip,
		   struct in_addr *hismask, struct in_addr *ourip,
		   struct in_addr *ourmask, struct in_addr *dns1,
		   struct in_addr *dns2, char *domain) {

  conn->hisip.s_addr = hisip->s_addr;
  conn->hismask.s_addr = hismask->s_addr;
  conn->ourip.s_addr = ourip->s_addr;
  conn->dns1.s_addr = dns1->s_addr;
  conn->dns2.s_addr = dns2->s_addr;

  if (domain) {
    strncpy(conn->domain, domain, DHCP_DOMAIN_LEN);
    conn->domain[DHCP_DOMAIN_LEN-1] = 0;
  }
  else {
    conn->domain[0] = 0;
  }

  if (_options.usetap) {
    /*
     *    USETAP ARP
     */
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd > 0) {
      struct arpreq req;

      memset(&req, 0, sizeof(req));

      /* SET_SA_FAMILY(req.arp_ha, AF_UNSPEC);*/
      SET_SA_FAMILY(req.arp_pa, AF_INET);
      ((struct sockaddr_in *) &req.arp_pa)->sin_addr.s_addr = conn->hisip.s_addr;
      req.arp_flags = ATF_PERM;

      memcpy(req.arp_ha.sa_data, conn->hismac, PKT_ETH_ALEN);

      log_dbg("ARP Entry: %s -> %.2x:%.2x:%.2x:%.2x:%.2x:%.2x", 
	      inet_ntoa(conn->hisip),
	      conn->hismac[0], conn->hismac[1], conn->hismac[2],
	      conn->hismac[3], conn->hismac[4], conn->hismac[5]);

      strncpy(req.arp_dev, tuntap(tun).devname, sizeof(req.arp_dev));

      if (ioctl(sockfd, SIOCSARP, &req) < 0) {
	perror("ioctrl()");
      }
      close(sockfd);
    }
  }

  if (_options.uamanyip && !_options.uamnatanyip &&
      (hisip->s_addr & ourmask->s_addr) != (ourip->s_addr & ourmask->s_addr)) {
    /**
     *  We have enabled ''uamanyip'' and the address we are setting does
     *  not fit in ourip's network. In this case, add a route entry. 
     */
    struct app_conn_t *appconn = (struct app_conn_t *)conn->peer;
    if (appconn) {
      struct ippoolm_t *ipm = (struct ippoolm_t*)appconn->uplink;
      if (ipm && ipm->in_use && ipm->is_static) {
	struct in_addr mask;
	int res;
	mask.s_addr = 0xffffffff;
	res = net_add_route(hisip, ourip, &mask);
	log_dbg("Adding route for %s %d", inet_ntoa(*hisip), res);
      }
    }
  }

  return 0;
}

static unsigned char const bmac[PKT_ETH_ALEN] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

int dhcp_receive_eapol(struct dhcp_t *this, uint8_t *pack);
int dhcp_receive_arp(struct dhcp_t *this, uint8_t *pack, size_t len);

int dhcp_receive_ip(struct dhcp_t *this, uint8_t *pack, size_t len) {
  struct pkt_ethhdr_t *pack_ethh = ethhdr(pack);
  struct pkt_iphdr_t  *pack_iph  = iphdr(pack);
  struct pkt_tcphdr_t *pack_tcph = tcphdr(pack);
  struct pkt_udphdr_t *pack_udph = udphdr(pack);
  struct dhcp_conn_t *conn;
  struct in_addr ourip;
  struct in_addr addr;

  /*
   *  Received a packet from the dhcpif
   */

  if (this->debug)
    log_dbg("DHCP packet received");
  
  /* 
   *  Check that the destination MAC address is our MAC or Broadcast 
   */
  if ((memcmp(pack_ethh->dst, dhcp_nexthop(this), PKT_ETH_ALEN)) && 
      (memcmp(pack_ethh->dst, bmac, PKT_ETH_ALEN))) {
    /*
      log_dbg("Not for our MAC or broadcast: %.2X-%.2X-%.2X-%.2X-%.2X-%.2X",
	    pack_ethh->dst[0], pack_ethh->dst[1], pack_ethh->dst[2], 
	    pack_ethh->dst[3], pack_ethh->dst[4], pack_ethh->dst[5]);
    */
    return 0;
  }
  
  ourip.s_addr = this->ourip.s_addr;
  
  /* 
   *  DHCP (BOOTPS) packets for broadcast or us specifically
   */
  if (((pack_iph->daddr == 0) ||
       (pack_iph->daddr == 0xffffffff) ||
       (pack_iph->daddr == ourip.s_addr)) &&
      ((pack_iph->version_ihl == PKT_IP_VER_HLEN) && 
       (pack_iph->protocol == PKT_IP_PROTO_UDP) &&
       (pack_udph->dst == htons(DHCP_BOOTPS)))) {
    log_dbg("dhcp/bootps request being processed");
    return dhcp_getreq(this, pack, len);
  }
  
  /* 
   *  Check to see if we know MAC address
   */
  if (!dhcp_hashget(this, &conn, pack_ethh->src)) {
    if (this->debug) log_dbg("Address found");
    ourip.s_addr = conn->ourip.s_addr;
  }
  else {
    struct in_addr reqaddr;
    
    memcpy(&reqaddr.s_addr, &pack_iph->saddr, PKT_IP_ALEN);
    
    log_dbg("Address not found (%s)", inet_ntoa(reqaddr)); 
    
    /* Do we allow dynamic allocation of IP addresses? */
    if (!this->allowdyn && !_options.uamanyip) {
      log_dbg("dropping packet; no dynamic ip and no anyip");
      return 0; 
    }
    
    /* Allocate new connection */
    if (dhcp_newconn(this, &conn, pack_ethh->src, pack)) {
      log_dbg("dropping packet; out of connections");
      return 0; /* Out of connections */
    }
  }

  /* Request an IP address 
  if (_options.uamanyip && 
      conn->authstate == DHCP_AUTH_NONE) {
    this->cb_request(conn, &pack_iph->saddr);
  } */
  
  /* Return if we do not know peer */
  if (!conn) {
    log_dbg("dropping packet; no peer");
    return 0;
  }

#ifdef ENABLE_IEEE8021Q
  dhcp_checktag(conn, pack);
#endif

  /* 
   *  Request an IP address 
   */
  if ((conn->authstate == DHCP_AUTH_NONE) && 
      (_options.uamanyip || 
       ((pack_iph->daddr != 0) && 
	(pack_iph->daddr != 0xffffffff)))) {
    addr.s_addr = pack_iph->saddr;
    if (this->cb_request)
      if (this->cb_request(conn, &addr, 0, 0)) {
	log_dbg("dropping packet; ip not known: %s", inet_ntoa(addr));
	return 0; /* Ignore request if IP address was not allocated */
      }
  }

  conn->lasttime = mainclock_now();

  if (pack_iph->saddr != conn->hisip.s_addr) {
    log_dbg("Received packet with spoofed source!");
    /*dhcp_sendRENEW(conn, pack, len);*/
    return 0;
  }

  if (pack_iph->protocol == PKT_IP_PROTO_UDP &&
      pack_udph->dst == htons(DHCP_DNS)) {
    if (dhcp_localDNS(conn, pack, len)) 
      return 0;
  }
  
  /* Was it a request for the auto-logout service? */
  if ((pack_iph->daddr == _options.uamlogout.s_addr) &&
      (pack_iph->protocol == PKT_IP_PROTO_TCP) &&
      (pack_tcph->dst == htons(DHCP_HTTP))) {
    if (conn->peer) {
      struct app_conn_t *appconn = (struct app_conn_t *)conn->peer;
      if (appconn->s_state.authenticated) {
	terminate_appconn(appconn, RADIUS_TERMINATE_CAUSE_USER_REQUEST);
	log_dbg("Dropping session due to request for auto-logout ip");
	appconn->uamexit = 1;
      }
    }
  }
  
  if (_options.uamalias.s_addr && 
      pack_iph->daddr == _options.uamalias.s_addr) {
    
    dhcp_uam_nat(conn, pack_ethh, pack_iph, pack_tcph, &this->uamlisten,
		 _options.uamuiport ? _options.uamuiport : this->uamport);

  }
  
  switch (conn->authstate) {
  case DHCP_AUTH_PASS:
#ifdef HAVE_NETFILTER_QUEUE
    if (this->qif_in.fd && this->qif_out.fd) {
      return 1;
    }
#endif
#ifdef HAVE_NETFILTER_COOVA
    if (_options.kname) {
      if (conn->peer) {
	struct app_conn_t *appconn = (struct app_conn_t *)conn->peer;
	appconn->s_state.last_sent_time =
	  appconn->s_state.last_time = mainclock_now();
      }
      return 1;
    }
#endif
    /* Check for post-auth proxy, otherwise pass packets unmodified */
    dhcp_postauthDNAT(conn, pack, len, 0);
    break; 

  case DHCP_AUTH_UNAUTH_TOS:
    /* Set TOS to specified value (unauthenticated) */
    pack_iph->tos = conn->unauth_cp;
    chksum(pack_iph);
    break;

  case DHCP_AUTH_AUTH_TOS:
    /* Set TOS to specified value (authenticated) */
    pack_iph->tos = conn->auth_cp;
    chksum(pack_iph);
    break;

  case DHCP_AUTH_SPLASH:
    dhcp_doDNAT(conn, pack, len, 0);
    break;

  case DHCP_AUTH_DNAT:
    /* Destination NAT if request to unknown web server */
    if (dhcp_doDNAT(conn, pack, len, 1)) {
      log_dbg("dropping packet; not nat'ed");
      return 0; /* drop */
    }
    break;

  case DHCP_AUTH_DROP: 
    log_dbg("dropping packet; auth-drop");
    return 0;
    
  default:
    log_dbg("dropping packet; unhandled auth state %d", conn->authstate);
    return 0;
  }

  /*done:*/

  if (_options.usetap) {
    memcpy(pack_ethh->dst, tuntap(tun).hwaddr, PKT_ETH_ALEN);
  }

  if ((conn->hisip.s_addr) && (this->cb_data_ind)) {
    this->cb_data_ind(conn, pack, len);
  } else {
    log_dbg("no hisip; packet-drop");
  }
  
  return 0;
}

static int dhcp_decaps_cb(void *ctx, void *packet, size_t length) {
  struct dhcp_t *this = (struct dhcp_t *)ctx;
  uint16_t prot;

#ifdef ENABLE_IEEE8021Q
  if (is_8021q(packet)) {
    struct pkt_ethhdr8021q_t *ethh = ethhdr8021q(packet);
    prot = ntohs(ethh->prot);
  } else 
#endif
  {
    struct pkt_ethhdr_t *ethh = ethhdr(packet);
    prot = ntohs(ethh->prot);
  }

  /*
  if (_options.debug) {
    struct pkt_ethhdr_t *ethh = ethhdr(packet);
    log_dbg("dhcp_decaps: src=%.2x:%.2x:%.2x:%.2x:%.2x:%.2x dst=%.2x:%.2x:%.2x:%.2x:%.2x:%.2x prot=%.4x len=%d",
	    ethh->src[0],ethh->src[1],ethh->src[2],ethh->src[3],ethh->src[4],ethh->src[5],
	    ethh->dst[0],ethh->dst[1],ethh->dst[2],ethh->dst[3],ethh->dst[4],ethh->dst[5],
	    prot, length);
  }
  */

  switch (prot) {
  case PKT_ETH_PROTO_EAPOL: return dhcp_receive_eapol(this, packet);
  case PKT_ETH_PROTO_ARP:   return dhcp_receive_arp(this, packet, length);
  case PKT_ETH_PROTO_IP:    return dhcp_receive_ip(this, packet, length);
  default: log_dbg("Layer2 PROT: 0x%.4x dropped", ntohs(prot)); 
  }
  return 0;
}

/**
 * Call this function when a new IP packet has arrived. This function
 * should be part of a select() loop in the application.
 **/
int dhcp_decaps(struct dhcp_t *this, int idx) {
  ssize_t length;

#ifdef HAVE_NETFILTER_QUEUE
  if (idx) {
    uint8_t buf[1600];
    net_interface *iface = 0;

    switch (idx) {
    case 1:
      iface = &this->qif_in;
      break;
    case 2:
      iface = &this->qif_out;
      break;
    }
    if (iface) {
      length = recv(iface->fd, buf, sizeof(buf), MSG_DONTWAIT);
      if (length > 0) {
	nfq_handle_packet(iface->h, buf, length);
      }
      return length;
    }
  }
#endif

  if ((length = net_read_dispatch(&this->rawif, dhcp_decaps_cb, this)) < 0) 
    return -1;

  return length;
}

static inline int dhcp_ethhdr(struct dhcp_conn_t *conn, uint8_t *packet, uint8_t *hismac, uint8_t *nexthop, uint16_t prot) {
#ifdef ENABLE_IEEE8021Q
  if (conn->tag8021q) {
    struct pkt_ethhdr8021q_t *pack_ethh = ethhdr8021q(packet);
    copy_mac6(pack_ethh->dst, hismac);
    copy_mac6(pack_ethh->src, nexthop);
    pack_ethh->prot = htons(prot);
    pack_ethh->tpid = htons(PKT_ETH_8021Q_TPID);
    pack_ethh->pcp_cfi_vid = conn->tag8021q;
  } else 
#endif
  {
    struct pkt_ethhdr_t *pack_ethh = ethhdr(packet);
    copy_mac6(pack_ethh->dst, hismac);
    copy_mac6(pack_ethh->src, nexthop);
    pack_ethh->prot = htons(prot);
  }
  return 0;
}

int dhcp_relay_decaps(struct dhcp_t *this, int idx) {
  struct dhcp_tag_t *message_type = 0;
  struct dhcp_conn_t *conn;
  struct dhcp_packet_t packet;
  struct sockaddr_in addr;
  socklen_t fromlen = sizeof(addr);
  ssize_t length;

  uint8_t fullpack[PKT_BUFFER];

  if ((length = recvfrom(this->relayfd, &packet, sizeof(packet), 0,
                         (struct sockaddr *) &addr, &fromlen)) <= 0) {
    log_err(errno, "recvfrom() failed");
    return -1;
  }

  log_dbg("DHCP relay response of length %d received", length);

  if (addr.sin_addr.s_addr != _options.dhcpgwip.s_addr) {
    log_err(0, "received DHCP response from host other than our gateway");
    return -1;
  }

  if (addr.sin_port != htons(_options.dhcpgwport)) {
    log_err(0, "received DHCP response from port other than our gateway");
    return -1;
  }

  if (dhcp_gettag(&packet, length, &message_type, DHCP_OPTION_MESSAGE_TYPE)) {
    log_err(0, "no message type");
    return -1;
  }
  
  if (message_type->l != 1) {
    log_err(0, "wrong message type length");
    return -1; /* Wrong length of message type */
  }
  
  if (dhcp_hashget(this, &conn, packet.chaddr)) {

    /* Allocate new connection */
    if (dhcp_newconn(this, &conn, packet.chaddr, 0)) {
      log_err(0, "out of connections");
      return 0; /* Out of connections */
    }

    this->cb_request(conn, (struct in_addr *)&packet.yiaddr, 0, 0);
  }

  packet.giaddr = 0;

  memset(&fullpack, 0, sizeof(fullpack));

  dhcp_ethhdr(conn, fullpack, conn->hismac, dhcp_nexthop(this), PKT_ETH_PROTO_IP);

  {
    struct pkt_iphdr_t *fullpack_iph = iphdr(fullpack);
    struct pkt_udphdr_t *fullpack_udph = udphdr(fullpack);
    
    fullpack_iph->version_ihl = PKT_IP_VER_HLEN;
    fullpack_iph->tot_len = htons(length + PKT_UDP_HLEN + PKT_IP_HLEN);
    fullpack_iph->ttl = 0x10;
    fullpack_iph->protocol = 0x11;
    
    fullpack_iph->saddr = conn->ourip.s_addr;
    fullpack_udph->src = htons(DHCP_BOOTPS);
    fullpack_udph->len = htons(length + PKT_UDP_HLEN);
    
    /*if (fullpack.dhcp.ciaddr) {
      fullpack_udph->daddr = req_dhcp->ciaddr; 
      fullpack_udph->dst = htons(DHCP_BOOTPC);
      } else if (req_dhcp->giaddr) {
      fullpack_iph->daddr = req_dhcp->giaddr; 
      fullpack_udph->dst = htons(DHCP_BOOTPS);
      } else */
    
    if (message_type->v[0] == DHCPNAK || packet.flags[0] & 0x80) {
      fullpack_iph->daddr = ~0; 
      fullpack_udph->dst = htons(DHCP_BOOTPC);
      /* fullpack.dhcp.flags[0] = 0x80;*/
    } if (packet.ciaddr) {
      fullpack_iph->daddr = packet.ciaddr; 
      fullpack_udph->dst = htons(DHCP_BOOTPC);
    } else {
      fullpack_iph->daddr = packet.yiaddr; 
      fullpack_udph->dst = htons(DHCP_BOOTPC);
    }
    
    memcpy(dhcppkt(fullpack), &packet, sizeof(packet));
  }

  { /* rewrite the server-id, otherwise will not get subsequent requests */
    struct dhcp_tag_t *tag = 0;
    if (!dhcp_gettag(dhcppkt(fullpack), length, &tag, DHCP_OPTION_SERVER_ID)) {
      memcpy(tag->v, &conn->ourip.s_addr, 4);
    }
  }

  chksum(iphdr(fullpack));

  return dhcp_send(this, &this->rawif, conn->hismac, fullpack, 
		   length + sizeofudp(fullpack));
}

/**
 * dhcp_data_req()
 * Call this function to send an IP packet to the peer.
 * Called from the tun_ind function. This method is passed either
 * an Ethernet frame or an IP packet. 
 **/
int dhcp_data_req(struct dhcp_conn_t *conn, uint8_t *pack, size_t len, int ethhdr) {
  struct dhcp_t *this = conn->parent;
  uint8_t packet[PKT_MAX_LEN];
  size_t length = len;

#ifdef ENABLE_IEEE8021Q
  uint16_t tag = conn->tag8021q;
#else
  uint16_t tag = 0;
#endif

  uint8_t * pkt = packet;

  if (ethhdr) { /* Ethernet frame */
    length += sizeofeth2(tag) - sizeofeth(pack);
    if (length > len) {
      log_dbg("adjusting %d -> %d", len, length);
      memcpy(packet + (length - len), pack, len);
    } else {
      pkt = pack;
    }
  } else { /* IP packet XXX: is there *any* way to avoid the memcpy? */
    size_t hdrlen = sizeofeth2(tag);
    memcpy(packet + hdrlen, pack, len);
    length += hdrlen;
    /*log_dbg("adding %d to IP frame", hdrlen);*/
  }

  dhcp_ethhdr(conn, pkt, conn->hismac, dhcp_nexthop(this), PKT_ETH_PROTO_IP);

  switch (conn->authstate) {

  case DHCP_AUTH_PASS:
  case DHCP_AUTH_AUTH_TOS:
    dhcp_postauthDNAT(conn, pkt, length, 1);
    break;

  case DHCP_AUTH_SPLASH:
  case DHCP_AUTH_UNAUTH_TOS:
    dhcp_undoDNAT(conn, pkt, &length, 0);
    break;

  case DHCP_AUTH_DNAT:
    /* undo destination NAT */
    if (dhcp_undoDNAT(conn, pkt, &length, 1)) { 
      log_dbg("dhcp_undoDNAT() returns true");
      return 0;
    }
    break;

  case DHCP_AUTH_DROP: 
  default: return 0;
  }

  return dhcp_send(this, &this->rawif, conn->hismac, pkt, length);
}


/**
 * dhcp_sendARP()
 * Send ARP message to peer
 **/
static int
dhcp_sendARP(struct dhcp_conn_t *conn, uint8_t *pack, size_t len) {

  uint8_t packet[PKT_BUFFER];
  struct dhcp_t *this = conn->parent;
  struct in_addr reqaddr;

  struct arp_packet_t *pack_arp = arppkt(pack);

  struct pkt_ethhdr_t *packet_ethh;
  struct arp_packet_t *packet_arp;

  /* Get local copy */
  memcpy(&reqaddr.s_addr, pack_arp->tpa, PKT_IP_ALEN);

  /* Check that request is within limits */

  /* Get packet default values */
  memset(packet, 0, sizeof(packet));
  copy_ethproto(pack, packet);

  packet_ethh = ethhdr(packet);
  packet_arp = arppkt(packet);
	 
  /* ARP Payload */
  packet_arp->hrd = htons(DHCP_HTYPE_ETH);
  packet_arp->pro = htons(PKT_ETH_PROTO_IP);
  packet_arp->hln = PKT_ETH_ALEN;
  packet_arp->pln = PKT_IP_ALEN;
  packet_arp->op  = htons(DHCP_ARP_REPLY);

  /* Source address */
  memcpy(packet_arp->spa, &reqaddr.s_addr, PKT_IP_ALEN);
  memcpy(packet_arp->sha, dhcp_nexthop(this), PKT_ETH_ALEN);

  /* Target address */
  memcpy(packet_arp->tha, &conn->hismac, PKT_ETH_ALEN);
  memcpy(packet_arp->tpa, &conn->hisip.s_addr, PKT_IP_ALEN);

  log_dbg("ARP: Replying to %s / %.2X-%.2X-%.2X-%.2X-%.2X-%.2X", 
	  inet_ntoa(conn->hisip),
	  conn->hismac[0], conn->hismac[1], conn->hismac[2],
	  conn->hismac[3], conn->hismac[4], conn->hismac[5]);
  
  /* Ethernet header */
  memcpy(packet_ethh->dst, conn->hismac, PKT_ETH_ALEN);
  memcpy(packet_ethh->src, dhcp_nexthop(this), PKT_ETH_ALEN);

  return dhcp_send(this, &this->rawif, conn->hismac, packet, sizeofarp(packet));
}

int dhcp_receive_arp(struct dhcp_t *this, uint8_t *pack, size_t len) {
  
  unsigned char const bmac[PKT_ETH_ALEN] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
  struct dhcp_conn_t *conn;
  struct in_addr reqaddr;
  struct in_addr taraddr;

  struct pkt_ethhdr_t *pack_ethh = ethhdr(pack);
  struct arp_packet_t *pack_arp = arppkt(pack);

  /* Check that this is ARP request */
  if (pack_arp->op != htons(DHCP_ARP_REQUEST)) {
    log_dbg("ARP: Received other ARP than request!");
    return 0;
  }

  /* Check that MAC address is our MAC or Broadcast */
  if ((memcmp(pack_ethh->dst, dhcp_nexthop(this), PKT_ETH_ALEN)) && 
      (memcmp(pack_ethh->dst, bmac, PKT_ETH_ALEN))) {
    log_dbg("ARP: Received ARP request for other destination!");
    return 0;
  }

  /* get sender IP address */
  memcpy(&reqaddr.s_addr, &pack_arp->spa, PKT_IP_ALEN);

  /* get target IP address */
  memcpy(&taraddr.s_addr, &pack_arp->tpa, PKT_IP_ALEN);

  /* Check to see if we know MAC address. */
  if (dhcp_hashget(this, &conn, pack_ethh->src)) {
    log_dbg("ARP: Address not found: %s", inet_ntoa(reqaddr));

    /* Do we allow dynamic allocation of IP addresses? */
    if (!this->allowdyn && !_options.uamanyip) {
      log_dbg("ARP: Unknown client and no dynip: %s", inet_ntoa(taraddr));
      return 0; 
    }
    
    /* Allocate new connection */
    if (dhcp_newconn(this, &conn, pack_ethh->src, pack)) {
      log_warn(0, "ARP: out of connections");
      return 0; /* Out of connections */
    }
  }

#ifdef ENABLE_IEEE8021Q
  dhcp_checktag(conn, pack);
#endif

  log_dbg("ARP: %.2X-%.2X-%.2X-%.2X-%.2X-%.2X asking about %s", 
	  conn->hismac[0], conn->hismac[1], conn->hismac[2],
	  conn->hismac[3], conn->hismac[4], conn->hismac[5],
	  inet_ntoa(taraddr));
  
  if (conn->authstate == DHCP_AUTH_DROP) {
    return 0;
  }
  
  /* if no sender ip, then client is checking their own ip */
  if (!reqaddr.s_addr) {
    /* XXX: lookup in ippool to see if we really do know who has this */
    /* XXX: it should also ack if *we* are that ip */
    log_dbg("ARP: Ignoring self-discovery: %s", inet_ntoa(taraddr));

    /* If a static ip address... */
    this->cb_request(conn, &taraddr, 0, 0);

    return 0;
  }

  if (!memcmp(&reqaddr.s_addr, &taraddr.s_addr, 4)) { 

    /* Request an IP address */
    if (_options.uamanyip /*or static ip*/ &&
	conn->authstate == DHCP_AUTH_NONE) {
      this->cb_request(conn, &reqaddr, 0, 0);
    } 

    log_dbg("ARP: Ignoring gratuitous arp %s", inet_ntoa(taraddr));
    return 0;
  }

  if (!conn->hisip.s_addr && !_options.uamanyip) {
    log_dbg("ARP: request did not come from known client");
    return 0; /* Only reply if he was allocated an address */
  }

  /* Is ARP request for clients own address: Ignore */
  if (conn->hisip.s_addr == taraddr.s_addr) {
    log_dbg("ARP: hisip equals target ip: %s", inet_ntoa(conn->hisip));
    return 0;
  }
  
  if (!_options.uamanyip) {
    /* If ARP request outside of mask: Ignore */
    if (reqaddr.s_addr &&
	(conn->hisip.s_addr & conn->hismask.s_addr) !=
	(reqaddr.s_addr & conn->hismask.s_addr)) {
      log_dbg("ARP: request not in our subnet");
      return 0;
    }
    
    if (memcmp(&conn->ourip.s_addr, &taraddr.s_addr, 4)) { /* if ourip differs from target ip */
      if (_options.debug) {
	log_dbg("ARP: Did not ask for router address: %s", inet_ntoa(conn->ourip));
	log_dbg("ARP: Asked for target: %s", inet_ntoa(taraddr));
      }
      return 0; /* Only reply if he asked for his router address */
    }
  }
  else if ((taraddr.s_addr != _options.dhcplisten.s_addr) &&
          ((taraddr.s_addr & _options.mask.s_addr) == _options.net.s_addr)) {
    /* when uamanyip is on we should ignore arp requests that ARE within our subnet except of course the ones for ourselves */
    log_dbg("ARP: Request for %s other than us within our subnet(uamanyip on), ignoring", inet_ntoa(taraddr));
    return 0;
  }

  conn->lasttime = mainclock_now();

  dhcp_sendARP(conn, pack, len);

  return 0;
}


/**
 * eapol_sendNAK()
 * Send of a EAPOL negative acknowledge message to a peer.
 * NAK messages are always sent to broadcast IP address (
 * except when using a EAPOL relay server)
 **/
int dhcp_senddot1x(struct dhcp_conn_t *conn,  uint8_t *pack, size_t len) {
  struct dhcp_t *this = conn->parent;
  return dhcp_send(this, &this->rawif, conn->hismac, pack, len);
}

/**
 * eapol_sendNAK()
 * Send of a EAPOL negative acknowledge message to a peer.
 * NAK messages are always sent to broadcast IP address (
 * except when using a EAPOL relay server)
 **/
int dhcp_sendEAP(struct dhcp_conn_t *conn, uint8_t *pack, size_t len) {

  struct dhcp_t *this = conn->parent;

  uint8_t packet[PKT_BUFFER];
  struct pkt_ethhdr_t *packet_ethh;
  struct pkt_dot1xhdr_t *packet_dot1x;

  copy_ethproto(pack, packet);

  packet_ethh = ethhdr(packet);
  packet_dot1x = dot1xhdr(packet);

  /* Ethernet header */
  memcpy(packet_ethh->dst, conn->hismac, PKT_ETH_ALEN);
  memcpy(packet_ethh->src, dhcp_nexthop(this), PKT_ETH_ALEN);

  packet_ethh->prot = htons(PKT_ETH_PROTO_EAPOL);
  
  /* 802.1x header */
  packet_dot1x->ver  = 1;
  packet_dot1x->type = 0; /* EAP */
  packet_dot1x->len =  htons((uint16_t)len);

  memcpy(eappkt(packet), pack, len);
  
  return dhcp_send(this, &this->rawif, conn->hismac, packet, (PKT_ETH_HLEN + 4 + len));
}

int dhcp_sendEAPreject(struct dhcp_conn_t *conn, uint8_t *pack, size_t len) {

  /*struct dhcp_t *this = conn->parent;*/

  struct eap_packet_t packet;

  if (pack) {
    dhcp_sendEAP(conn, pack, len);
  }
  else {
    memset(&packet, 0, sizeof(packet));
    packet.code      =  4;
    packet.id        =  1; /* TODO ??? */
    packet.length    =  htons(4);
  
    dhcp_sendEAP(conn, (uint8_t *)&packet, 4);
  }

  return 0;

}

int dhcp_receive_eapol(struct dhcp_t *this, uint8_t *pack) {
  struct dhcp_conn_t *conn = NULL;
  unsigned char const bmac[PKT_ETH_ALEN] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
  unsigned char const amac[PKT_ETH_ALEN] = {0x01, 0x80, 0xc2, 0x00, 0x00, 0x03};

  struct pkt_ethhdr_t *pack_ethh = ethhdr(pack);
  struct pkt_dot1xhdr_t *pack_dot1x = dot1xhdr(pack);

  /* Check to see if we know MAC address. */
  if (!dhcp_hashget(this, &conn, pack_ethh->src)) {
    if (this->debug) log_dbg("Address found");
  }
  else {
    if (this->debug) log_dbg("Address not found");
  }
  
  if (this->debug) 
    log_dbg("IEEE 802.1x Packet: %.2x, %.2x %d",
	    pack_dot1x->ver, pack_dot1x->type,
	    ntohs(pack_dot1x->len));
  
  /* Check that MAC address is our MAC, Broadcast or authentication MAC */
  if ((memcmp(pack_ethh->dst, this->rawif.hwaddr, PKT_ETH_ALEN)) && 
      (memcmp(pack_ethh->dst, bmac, PKT_ETH_ALEN)) && 
      (memcmp(pack_ethh->dst, amac, PKT_ETH_ALEN)))
    return 0;
  
  if (pack_dot1x->type == 1) { /* Start */
    uint8_t p[PKT_BUFFER];
    struct pkt_dot1xhdr_t *p_dot1x;
    struct eap_packet_t *p_eap;
    
    /* Allocate new connection */
    if (conn == NULL) {
      if (dhcp_newconn(this, &conn, pack_ethh->src, pack))
	return 0; /* Out of connections */
    }

    memset(&p, 0, sizeof(p));
    dhcp_ethhdr(conn, p, pack_ethh->src, dhcp_nexthop(this), PKT_ETH_PROTO_EAPOL);

    p_dot1x = dot1xhdr(p);
    p_eap = eappkt(p);

    /* Ethernet header */

    /* 802.1x header */
    p_dot1x->ver  = 1;
    p_dot1x->type = 0; /* EAP */
    p_dot1x->len =  htons(5);
    
    /* EAP Packet */
    p_eap->code      =  1;
    p_eap->id        =  1;
    p_eap->length    =  htons(5);
    p_eap->type      =  1; /* Identity */

    dhcp_senddot1x(conn, p, PKT_ETH_HLEN + 4 + 5);
    return 0;
  }
  else if (pack_dot1x->type == 0) { /* EAP */

    /* TODO: Currently we only support authentications starting with a
       client sending a EAPOL start message. Need to also support
       authenticator initiated communications. */
    if (!conn)
      return 0;

    conn->lasttime = mainclock_now();
    
    if (this->cb_eap_ind)
      this->cb_eap_ind(conn, (uint8_t *)eappkt(pack), ntohs(eappkt(pack)->length));

    return 0;
  }
  else { /* Check for logoff */
    return 0;
  }
}

/**
 * dhcp_eapol_ind()
 * Call this function when a new EAPOL packet has arrived. This function
 * should be part of a select() loop in the application.
 **/
int dhcp_eapol_ind(struct dhcp_t *this) {
  uint8_t packet[PKT_BUFFER];
  ssize_t length;
  
  if ((length = net_read(&this->rawif, packet, sizeof(packet))) < 0) 
    return -1;

  /*
  if (_options.debug) {
    struct pkt_ethhdr_t *ethh = ethhdr(packet);
    log_dbg("eapol_decaps: src=%.2x:%.2x:%.2x:%.2x:%.2x:%.2x dst=%.2x:%.2x:%.2x:%.2x:%.2x:%.2x prot=%.4x",
	    ethh->src[0],ethh->src[1],ethh->src[2],ethh->src[3],ethh->src[4],ethh->src[5],
	    ethh->dst[0],ethh->dst[1],ethh->dst[2],ethh->dst[3],ethh->dst[4],ethh->dst[5],
	    ntohs(ethh->prot));
  }
  */

  return dhcp_receive_eapol(this, packet);
}


/**
 * dhcp_set_cb_data_ind()
 * Set callback function which is called when packet has arrived
 **/
int dhcp_set_cb_data_ind(struct dhcp_t *this, 
  int (*cb_data_ind) (struct dhcp_conn_t *conn, uint8_t *pack, size_t len)) {
  this->cb_data_ind = cb_data_ind;
  return 0;
}

int dhcp_set_cb_eap_ind(struct dhcp_t *this, 
  int (*cb_eap_ind) (struct dhcp_conn_t *conn, uint8_t *pack, size_t len)) {
  this->cb_eap_ind = cb_eap_ind;
  return 0;
}


/**
 * dhcp_set_cb_data_ind()
 * Set callback function which is called when a dhcp request is received
 **/
int dhcp_set_cb_request(struct dhcp_t *this, 
  int (*cb_request) (struct dhcp_conn_t *conn, struct in_addr *addr, uint8_t *pack, size_t len)) {
  this->cb_request = cb_request;
  return 0;
}


/**
 * dhcp_set_cb_connect()
 * Set callback function which is called when a connection is created
 **/
int dhcp_set_cb_connect(struct dhcp_t *this, 
             int (*cb_connect) (struct dhcp_conn_t *conn)) {
  this->cb_connect = cb_connect;
  return 0;
}

/**
 * dhcp_set_cb_disconnect()
 * Set callback function which is called when a connection is deleted
 **/
int dhcp_set_cb_disconnect(struct dhcp_t *this, 
  int (*cb_disconnect) (struct dhcp_conn_t *conn, int term_cause)) {
  this->cb_disconnect = cb_disconnect;
  return 0;
}

int dhcp_set_cb_getinfo(struct dhcp_t *this, 
  int (*cb_getinfo) (struct dhcp_conn_t *conn, bstring b, int fmt)) {
  this->cb_getinfo = cb_getinfo;
  return 0;
}


#if defined (__FreeBSD__) || defined (__APPLE__) || defined (__OpenBSD__)

int dhcp_receive(struct dhcp_t *this) {
  ssize_t length = 0;
  size_t offset = 0;
  struct bpf_hdr *hdrp;
  struct pkt_ethhdr_t *ethhdr;
  
  if (this->rbuf_offset == this->rbuf_len) {
    length = net_read(&this->rawif, this->rbuf, this->rbuf_max);

    if (length <= 0)
      return length;

    this->rbuf_offset = 0;
    this->rbuf_len = length;
  }
  
  while (this->rbuf_offset != this->rbuf_len) {
    
    if (this->rbuf_len - this->rbuf_offset < sizeof(struct bpf_hdr)) {
      this->rbuf_offset = this->rbuf_len;
      continue;
    }
    
    hdrp = (struct bpf_hdr *) &this->rbuf[this->rbuf_offset];
    
    if (this->rbuf_offset + hdrp->bh_hdrlen + hdrp->bh_caplen > 
	this->rbuf_len) {
      this->rbuf_offset = this->rbuf_len;
      continue;
    }

    if (hdrp->bh_caplen != hdrp->bh_datalen) {
      this->rbuf_offset += hdrp->bh_hdrlen + hdrp->bh_caplen;
      continue;
    }

    ethhdr = (struct pkt_ethhdr_t *) 
      (this->rbuf + this->rbuf_offset + hdrp->bh_hdrlen);

    switch (ntohs(ethhdr->prot)) {
    case PKT_ETH_PROTO_IP:
      dhcp_receive_ip(this, (struct pkt_ippacket_t*) ethhdr, hdrp->bh_caplen);
      break;
    case PKT_ETH_PROTO_ARP:
      dhcp_receive_arp(this, (struct arp_fullpacket_t*) ethhdr, hdrp->bh_caplen);
      break;
    case PKT_ETH_PROTO_EAPOL:
      dhcp_receive_eapol(this, (struct dot1xpacket_t*) ethhdr);
      break;

    default:
      break;
    }
    this->rbuf_offset += hdrp->bh_hdrlen + hdrp->bh_caplen;
  };
  return (0);
}
#endif
