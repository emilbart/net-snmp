#include <config.h>

#include <stdio.h>
#include <sys/types.h>
#include <ctype.h>
#include <errno.h>

#if HAVE_STRING_H
#include <string.h>
#else
#include <strings.h>
#endif
#if HAVE_STDLIB_H
#include <stdlib.h>
#endif
#if HAVE_UNISTD_H
#include <unistd.h>
#endif
#if HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif
#if HAVE_NETINET_IN_H
#include <netinet/in.h>
#endif
#if HAVE_ARPA_INET_H
#include <arpa/inet.h>
#endif
#if HAVE_NETDB_H
#include <netdb.h>
#endif

#include "asn1.h"
#include "snmp.h"
#include "vacm.h"
#include "snmp_debug.h"
#include "default_store.h"
#include "read_config.h"
#include "snmp_transport.h"
#include "snmpUDPIPv6Domain.h"

const oid ucdSnmpUDPIPv6Domain[9] = { UCDAVIS_MIB, 251, 4 };
static snmp_tdomain udp6Domain;

/*  Return a string representing the address in data, or else the "far end"
    address if data is NULL.  */

char	       *snmp_udp6_fmtaddr	(snmp_transport *t,
					 void *data, int len)
{
  struct sockaddr_in6 *to = NULL;

  if (data != NULL && len == sizeof(struct sockaddr_in6)) {
    to = (struct sockaddr_in6 *)data;
  } else if (t != NULL && t->data != NULL) {
    to = (struct sockaddr_in6 *)t->data;
  }
  if (to == NULL) {
    return strdup("UDP/IPv6: unknown");
  } else {
    char tmp[INET6_ADDRSTRLEN + 8];
    
    sprintf(tmp, "%s/%d", inet_ntop(AF_INET6, (void *)&(to->sin6_addr),
				 tmp, INET6_ADDRSTRLEN), ntohs(to->sin6_port));
    return strdup(tmp);
  }
}



/*  You can write something into opaque that will subsequently get passed back 
    to your send function if you like.  For instance, you might want to
    remember where a PDU came from, so that you can send a reply there...  */

int		snmp_udp6_recv	(snmp_transport *t, void *buf, int size,
				 void **opaque, int *olength) 
{
  int rc = -1, fromlen = sizeof(struct sockaddr_in6);
  struct sockaddr *from;

  if (t != NULL && t->sock >= 0) {
    from = (struct sockaddr *)malloc(sizeof(struct sockaddr_in6));
    if (from == NULL) {
      *opaque  = NULL;
      *olength = 0;
      return -1;
    } else {
      memset(from, 0, fromlen);
    }
    
    rc = recvfrom(t->sock, buf, size, 0, from, &fromlen);

    if (rc >= 0) {
      char *string = snmp_udp6_fmtaddr(NULL, from, fromlen);
      DEBUGMSGTL(("snmp_udp6_recv", "recvfrom fd %d got %d bytes (from %s)\n",
		  t->sock, rc, string));
      free(string);
    } else {
      DEBUGMSGTL(("snmp_udp6_recv", "recvfrom fd %d FAILED (rc %d)\n",
		  t->sock, rc));
    }
    *opaque  = (void *)from;
    *olength = sizeof(struct sockaddr_in6);
  }
  return rc;
}



int		snmp_udp6_send	(snmp_transport *t, void *buf, int size,
				 void **opaque, int *olength)
{
  int rc = 0;
  struct sockaddr *to = NULL;

  if (opaque != NULL && *opaque != NULL &&
      *olength == sizeof(struct sockaddr_in6)) {
    to = (struct sockaddr *)(*opaque);
  } else if (t != NULL && t->data != NULL &&
	     t->data_length == sizeof(struct sockaddr_in6)) {
    to = (struct sockaddr *)(t->data);
  }

  if (to != NULL && t != NULL && t->sock >= 0) {
    char *string = NULL;
    string = snmp_udp6_fmtaddr(NULL, (void *)to, sizeof(struct sockaddr_in6));
    DEBUGMSGTL(("snmp_udp6_send", "%d bytes from %p to %s on fd %d\n",
		size, buf, string, t->sock));
    free(string);
    rc = sendto(t->sock, buf, size, 0, to, sizeof(struct sockaddr));
    return rc;
  } else {
    return -1;
  }
}



int		snmp_udp6_close	(snmp_transport *t)
{
  int rc = 0;
  if (t->sock >= 0) {
#ifndef HAVE_CLOSESOCKET
    rc = close(t->sock);
#else
    rc = closesocket(t->sock);
#endif
    t->sock = -1;
    return rc;
  } else {
    return -1;
  }
}



/*  Open a UDP/IPv6-based transport for SNMP.  Local is TRUE if addr is the
    local address to bind to (i.e. this is a server-type session); otherwise
    addr is the remote address to send things to.  */

snmp_transport		*snmp_udp6_transport	(struct sockaddr_in6 *addr,
						 int local)
{
  snmp_transport *t = NULL;
  int rc = 0, udpbuf = (1 << 17);
  char *string = NULL;

  if (addr == NULL || addr->sin_family != AF_INET6) {
    return NULL;
  }

  t = (snmp_transport *)malloc(sizeof(snmp_transport));
  if (t == NULL) {
    return NULL;
  }

  string = snmp_udp6_fmtaddr(NULL, (void *)addr, sizeof(struct sockaddr_in6));
  DEBUGMSGTL(("snmp_udp6", "open %s %s\n", local?"local":"remote", string));
  free(string);

  memset(t, 0, sizeof(snmp_transport));

  t->domain = snmpUDPDomain;
  t->domain_length = sizeof(snmpUDPDomain)/sizeof(snmpUDPDomain[0]);

  t->sock = socket(PF_INET, SOCK_DGRAM, 0);
  if (t->sock < 0) {
    snmp_transport_free(t);
    return NULL;
  }

#ifdef  SO_BSDCOMPAT
  /*  Patch for Linux.  Without this, UDP packets that fail get an ICMP
      response.  Linux turns the failed ICMP response into an error message
      and return value, unlike all other OS's.  */
  {
    int one=1;
    setsockopt(t->sock, SOL_SOCKET, SO_BSDCOMPAT, &one, sizeof(one));
  }
#endif/*SO_BSDCOMPAT*/

  /*  Try to set the send and receive buffers to a reasonably large value, so
      that we can send and receive big PDUs (defaults to 8192 bytes (!) on
      Solaris, for instance).  Don't worry too much about errors -- just
      plough on regardless.  */

#ifdef  SO_SNDBUF
  if (setsockopt(t->sock, SOL_SOCKET, SO_SNDBUF, &udpbuf, sizeof(int)) != 0) {
    DEBUGMSGTL(("snmp_udp", "couldn't set SO_SNDBUF to %d bytes: %s\n",
		udpbuf, strerror(errno)));
  }
#endif/*SO_SNDBUF*/

#ifdef  SO_RCVBUF
  if (setsockopt(t->sock, SOL_SOCKET, SO_RCVBUF, &udpbuf, sizeof(int)) != 0) {
    DEBUGMSGTL(("snmp_udp", "couldn't set SO_RCVBUF to %d bytes: %s\n",
		udpbuf, strerror(errno)));
  }
#endif/*SO_RCVBUF*/

  if (local) {
    /*  This session is inteneded as a server, so we must bind on to the given
	IP address, which may include an interface address, or could be
	INADDR_ANY, but certainly includes a port number.  */
    
    rc = bind(t->sock, (struct sockaddr *)addr, sizeof(struct sockaddr));
    if (rc != 0) {
      snmp_udp_close(t);
      snmp_transport_free(t);
      return NULL;
    }
    t->data = NULL;
    t->data_length = 0;
  } else {
    /*  This is a client session.  Save the address in the transport-specific
	data pointer for later use by snmp_udp_send.  */

    t->data = malloc(sizeof(struct sockaddr_in));
    if (t->data == NULL) {
      snmp_transport_free(t);
      return NULL;
    }
    memcpy(t->data, addr, sizeof(struct sockaddr_in));
    t->data_length = sizeof(struct sockaddr_in);
  }

  /*  16-bit length field, 8 byte UDP header, 40 byte IPv6 header.  */

  t->msgMaxSize  = 0xffff - 8 - 40;
  t->f_recv      = snmp_udp6_recv;
  t->f_send      = snmp_udp6_send;
  t->f_close     = snmp_udp6_close;
  t->f_accept    = NULL;
  t->f_fmtaddr   = snmp_udp6_fmtaddr;

  return t;
}



int			snmp_sockaddr_in6	(struct sockaddr_in6 *addr,
						 const char *inpeername,
						 int remote_port)
{
  char *cp = NULL, *peername = NULL;

  if (addr == NULL) {
    return 0;
  }

  DEBUGMSGTL(("snmp_sockaddr_in6", "addr %p, peername \"%s\"\n",
	      addr, inpeername?inpeername:"[NIL]"));

  memset(addr, 0, sizeof(struct sockaddr_in6));
  addr->sin6_family = AF_INET6;
  addr->sin6_addr   = in6addr_any;

  if (remote_port > 0) {
    addr->sin6_port = htons(remote_port);
  } else if (ds_get_int(DS_LIBRARY_ID, DS_LIB_DEFAULT_PORT) > 0) {
    addr->sin6_port = htons(ds_get_int(DS_LIBRARY_ID, DS_LIB_DEFAULT_PORT));
  } else {
    addr->sin6_port = htons(SNMP_PORT);
  }

  if (inpeername != NULL) {
    /*  Duplicate the peername because we might want to mank around with
	it.  */

    peername = strdup(inpeername);
    if (peername == NULL) {
      return 0;
    }

    /*  Try and extract an appended port number.  */
    cp = strchr(peername, ':');
    if (cp != NULL) {
      *cp = '\0';
      cp++;
      if (atoi(cp) != 0) {
	DEBUGMSGTL(("snmp_sockaddr_in6","port number suffix :%d\n", atoi(cp)));
	addr->sin6_port = htons(atoi(cp));
      }
    }

    for (cp = peername; *cp && isdigit((int)*cp); cp++);
    if (!*cp && atoi(peername) != 0) {
      /*  Okay, it looks like just a port number.  */
      DEBUGMSGTL(("snmp_sockaddr_in6","totally numeric: %d\n",atoi(peername)));
      addr->sin6_port = htons(atoi(peername));
    } else if (inet_pton(AF_INET6, peername, (void *)&(addr->sin6_addr))) {
      /*  It looks like an IPv6 address.  */
      DEBUGMSGTL(("snmp_sockaddr_in6", "IPv6 address\n"));
    } else {
      /*  Well, it must be a hostname then.  */
#ifdef  HAVE_GETHOSTBYNAME
      struct hostent *hp = gethostbyname(peername);
      if (hp == NULL) {
	DEBUGMSGTL(("snmp_sockaddr_in", "hostname (couldn't resolve)\n"));
	free(peername);
	return 0;
      } else {
	if (hp->h_addrtype != AF_INET) {
	  DEBUGMSGTL(("snmp_sockaddr_in", "hostname (not AF_INET!)\n"));
	  free(peername);
	  return 0;
	} else {
	  DEBUGMSGTL(("snmp_sockaddr_in", "hostname (resolved okay)\n"));
	  memcpy(&(addr->sin_addr), hp->h_addr, hp->h_length);
	}
      }
#else /*HAVE_GETHOSTBYNAME*/
      DEBUGMSGTL(("snmp_sockaddr_in", "hostname (no gethostbyname)\n"));
      free(peername);
      return 0;
#endif/*HAVE_GETHOSTBYNAME*/
    }
  } else {
    DEBUGMSGTL(("snmp_sockaddr_in", "NULL peername"));
    return 0;
  }
  DEBUGMSGTL(("snmp_sockaddr_in", "return { AF_INET, %s:%hu }\n",
	      inet_ntoa(addr->sin_addr), ntohs(addr->sin_port)));
  free(peername);
  return 1;
}



snmp_transport	*snmp_udp6_create	       (const char *string, int local)
{
  struct sockaddr_in6 addr;

  if (snmp_sockaddr_in6(&addr, string, 0)) {
    return snmp_udp6_transport(&addr, local);
  } else {
    return NULL;
  }
}


void		snmp_udp6_ctor			(void)
{
  udp6Domain.name        = ucdSnmpUDPIPv6Domain;
  udp6Domain.name_length = sizeof(ucdSnmpUDPIPv6Domain)/sizeof(oid);
  udp6Domain.f_create 	 = snmp_udp6_create;
  udp6Domain.prefix	 = calloc(5, sizeof(char *));
  udp6Domain.prefix[0] 	 = "udp6";
  udp6Domain.prefix[1] 	 = "ipv6";
  udp6Domain.prefix[2] 	 = "udpv6";
  udp6Domain.prefix[3] 	 = "udpipv6";

  snmp_tdomain_register(&udp6Domain);
}
