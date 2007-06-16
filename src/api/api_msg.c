/*
 * Copyright (c) 2001-2004 Swedish Institute of Computer Science.
 * All rights reserved. 
 * 
 * Redistribution and use in source and binary forms, with or without modification, 
 * are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission. 
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED 
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF 
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT 
 * SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, 
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT 
 * OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS 
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN 
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING 
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY 
 * OF SUCH DAMAGE.
 *
 * This file is part of the lwIP TCP/IP stack.
 * 
 * Author: Adam Dunkels <adam@sics.se>
 *
 */

#include "lwip/opt.h"
#include "lwip/arch.h"
#include "lwip/api_msg.h"
#include "lwip/memp.h"
#include "lwip/sys.h"
#include "lwip/tcpip.h"
#include "lwip/igmp.h"

#if !NO_SYS

#if LWIP_RAW
/**
 * Receive callback function for RAW netconns.
 * Doesn't 'eat' the packet, only references it and sends it to
 * conn->recvmbox
 *
 * @see raw.h (struct raw_pcb.recv) for parameters and return value
 */
static u8_t
recv_raw(void *arg, struct raw_pcb *pcb, struct pbuf *p,
    struct ip_addr *addr)
{
  struct netbuf *buf;
  struct netconn *conn;

  conn = arg;
  if (!conn)
    return 0;

  if (conn->recvmbox != SYS_MBOX_NULL) {
    if (!(buf = memp_malloc(MEMP_NETBUF))) {
      return 0;
    }
    pbuf_ref(p);
    buf->p = p;
    buf->ptr = p;
    buf->addr = addr;
    buf->port = pcb->protocol;

    conn->recv_avail += p->tot_len;
    /* Register event with callback */
    if (conn->callback)
      (*conn->callback)(conn, NETCONN_EVT_RCVPLUS, p->tot_len);
    sys_mbox_post(conn->recvmbox, buf);
  }

  return 0; /* do not eat the packet */
}
#endif /* LWIP_RAW*/

#if LWIP_UDP
/**
 * Receive callback function for UDP netconns.
 * Posts the packet to conn->recvmbox or deletes it on memory error.
 *
 * @see udp.h (struct udp_pcb.recv) for parameters
 */
static void
recv_udp(void *arg, struct udp_pcb *pcb, struct pbuf *p,
   struct ip_addr *addr, u16_t port)
{
  struct netbuf *buf;
  struct netconn *conn;

  LWIP_UNUSED_ARG(pcb);

  conn = arg;

  if (conn == NULL) {
    pbuf_free(p);
    return;
  }

  if (conn->recvmbox != SYS_MBOX_NULL) {
    buf = memp_malloc(MEMP_NETBUF);
    if (buf == NULL) {
      pbuf_free(p);
      return;
    } else {
      buf->p = p;
      buf->ptr = p;
      buf->addr = addr;
      buf->port = port;
    }

    conn->recv_avail += p->tot_len;
    /* Register event with callback */
    if (conn->callback)
      (*conn->callback)(conn, NETCONN_EVT_RCVPLUS, p->tot_len);
    sys_mbox_post(conn->recvmbox, buf);
  }
}
#endif /* LWIP_UDP */

#if LWIP_TCP
/**
 * Receive callback function for TCP netconns.
 * Posts the packet to conn->recvmbox or deletes it on memory error.
 *
 * @see tcp.h (struct tcp_pcb.recv) for parameters and return value
 */
static err_t
recv_tcp(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
  struct netconn *conn;
  u16_t len;

  LWIP_UNUSED_ARG(pcb);

  conn = arg;

  if (conn == NULL) {
    pbuf_free(p);
    return ERR_VAL;
  }

  if (conn->recvmbox != SYS_MBOX_NULL) {
    conn->err = err;
    if (p != NULL) {
      len = p->tot_len;
      conn->recv_avail += len;
    } else {
      len = 0;
    }
    /* Register event with callback */
    if (conn->callback)
      (*conn->callback)(conn, NETCONN_EVT_RCVPLUS, len);
    sys_mbox_post(conn->recvmbox, p);
  }  
  return ERR_OK;
}

/**
 * Poll callback function for TCP netconns.
 * Wakes up an application thread that waits for a connection to close
 * or data to be sent. The application thread then takes the
 * appropriate action to go on.
 *
 * Signals the conn->sem.
 * netconn_close waits for conn->sem if closing failed.
 *
 * @see tcp.h (struct tcp_pcb.poll) for parameters and return value
 */
static err_t
poll_tcp(void *arg, struct tcp_pcb *pcb)
{
  struct netconn *conn;

  LWIP_UNUSED_ARG(pcb);

  conn = arg;

  if (conn != NULL &&
     (conn->state == NETCONN_WRITE || conn->state == NETCONN_CLOSE) &&
     conn->sem != SYS_SEM_NULL) {
    sys_sem_signal(conn->sem);
  }
  return ERR_OK;
}

/**
 * Sent callback function for TCP netconns.
 * Signals the conn->sem and calls conn->callback.
 * netconn_write waits for conn->sem if send buffer is low.
 *
 * @see tcp.h (struct tcp_pcb.sent) for parameters and return value
 */
static err_t
sent_tcp(void *arg, struct tcp_pcb *pcb, u16_t len)
{
  struct netconn *conn;

  LWIP_UNUSED_ARG(pcb);

  conn = arg;

  if (conn != NULL && conn->sem != SYS_SEM_NULL) {
    sys_sem_signal(conn->sem);
  }

  if (conn && conn->callback)
    if (tcp_sndbuf(conn->pcb.tcp) > TCP_SNDLOWAT)
      (*conn->callback)(conn, NETCONN_EVT_SENDPLUS, len);
  
  return ERR_OK;
}

/**
 * Error callback function for TCP netconns.
 * Signals conn->sem, posts to all conn mboxes and calls conn->callback.
 * The application thread has then to decide what to do.
 *
 * @see tcp.h (struct tcp_pcb.err) for parameters
 */
static void
err_tcp(void *arg, err_t err)
{
  struct netconn *conn;

  conn = arg;

  conn->pcb.tcp = NULL;

  conn->err = err;
  if (conn->recvmbox != SYS_MBOX_NULL) {
    /* Register event with callback */
    if (conn->callback)
      (*conn->callback)(conn, NETCONN_EVT_RCVPLUS, 0);
    sys_mbox_post(conn->recvmbox, NULL);
  }
  if (conn->mbox != SYS_MBOX_NULL && conn->state == NETCONN_CONNECT) {
    conn->state = NETCONN_NONE;
    sys_mbox_post(conn->mbox, NULL);
  }
  if (conn->acceptmbox != SYS_MBOX_NULL) {
     /* Register event with callback */
    if (conn->callback)
      (*conn->callback)(conn, NETCONN_EVT_RCVPLUS, 0);
    sys_mbox_post(conn->acceptmbox, NULL);
  }
  if (conn->sem != SYS_SEM_NULL) {
    sys_sem_signal(conn->sem);
  }
}

/**
 * Setup a tcp_pcb with the correct callback function pointers
 * and their arguments.
 *
 * @param conn the TCP netconn to setup
 */
static void
setup_tcp(struct netconn *conn)
{
  struct tcp_pcb *pcb;

  pcb = conn->pcb.tcp;
  tcp_arg(pcb, conn);
  tcp_recv(pcb, recv_tcp);
  tcp_sent(pcb, sent_tcp);
  tcp_poll(pcb, poll_tcp, 4);
  tcp_err(pcb, err_tcp);
}

/**
 * Accept callback function for TCP netconns.
 * Allocates a new netconn and posts that to conn->acceptmbox.
 *
 * @see tcp.h (struct tcp_pcb_listen.accept) for parameters and return value
 */
static err_t
accept_function(void *arg, struct tcp_pcb *newpcb, err_t err)
{
  struct netconn *newconn;
  struct netconn *conn;

#if API_MSG_DEBUG
#if TCP_DEBUG
  tcp_debug_print_state(newpcb->state);
#endif /* TCP_DEBUG */
#endif /* API_MSG_DEBUG */
  conn = (struct netconn *)arg;

  LWIP_ERROR("accept_function: invalid conn->acceptmbox",
             conn->acceptmbox != SYS_MBOX_NULL, return ERR_VAL;);

  newconn = memp_malloc(MEMP_NETCONN);
  if (newconn == NULL) {
    return ERR_MEM;
  }
  newconn->recvmbox = sys_mbox_new();
  if (newconn->recvmbox == SYS_MBOX_NULL) {
    memp_free(MEMP_NETCONN, newconn);
    return ERR_MEM;
  }
  newconn->mbox = sys_mbox_new();
  if (newconn->mbox == SYS_MBOX_NULL) {
    sys_mbox_free(newconn->recvmbox);
    memp_free(MEMP_NETCONN, newconn);
    return ERR_MEM;
  }
  newconn->sem = sys_sem_new(0);
  if (newconn->sem == SYS_SEM_NULL) {
    sys_mbox_free(newconn->mbox);
    sys_mbox_free(newconn->recvmbox);
    memp_free(MEMP_NETCONN, newconn);
    return ERR_MEM;
  }
  /* Allocations were OK, setup the PCB etc */
  newconn->type = NETCONN_TCP;
  newconn->pcb.tcp = newpcb;
  setup_tcp(newconn);
  newconn->acceptmbox = SYS_MBOX_NULL;
  newconn->err = err;
  /* Register event with callback */
  if (conn->callback) {
    (*conn->callback)(conn, NETCONN_EVT_RCVPLUS, 0);
  }
  /* We have to set the callback here even though
   * the new socket is unknown. Mark the socket as -1. */
  newconn->callback = conn->callback;
  newconn->socket = -1;
  newconn->recv_avail = 0;
#if LWIP_SO_RCVTIMEO
  newconn->recv_timeout = 0;
#endif /* LWIP_SO_RCVTIMEO */

  sys_mbox_post(conn->acceptmbox, newconn);
  return ERR_OK;
}
#endif /* LWIP_TCP */

/**
 * Create a new pcb of a specific type.
 * Called from do_newconn().
 *
 * @param msg the api_msg_msg describing the connection type
 * @return msg->conn->err, but the return value is currently ignored
 */
static err_t
pcb_new(struct api_msg_msg *msg)
{
   msg->conn->err = ERR_OK;

   /* Allocate a PCB for this connection */
   switch(NETCONNTYPE_GROUP(msg->conn->type)) {
#if LWIP_RAW
   case NETCONN_RAW:
     msg->conn->pcb.raw = raw_new(msg->msg.n.proto);
     if(msg->conn->pcb.raw == NULL) {
       msg->conn->err = ERR_MEM;
       break;
     }
     raw_recv(msg->conn->pcb.raw, recv_raw, msg->conn);
     break;
#endif /* LWIP_RAW */
#if LWIP_UDP
   case NETCONN_UDP:
     msg->conn->pcb.udp = udp_new();
     if(msg->conn->pcb.udp == NULL) {
       msg->conn->err = ERR_MEM;
       break;
     }
#if LWIP_UDPLITE
     if (msg->conn->type==NETCONN_UDPLITE)     udp_setflags(msg->conn->pcb.udp, UDP_FLAGS_UDPLITE);
#endif /* LWIP_UDPLITE */
     if (msg->conn->type==NETCONN_UDPNOCHKSUM) udp_setflags(msg->conn->pcb.udp, UDP_FLAGS_NOCHKSUM);
     udp_recv(msg->conn->pcb.udp, recv_udp, msg->conn);
     break;
#endif /* LWIP_UDP */
#if LWIP_TCP
   case NETCONN_TCP:
     msg->conn->pcb.tcp = tcp_new();
     if(msg->conn->pcb.tcp == NULL) {
       msg->conn->err = ERR_MEM;
       break;
     }
     setup_tcp(msg->conn);
     break;
#endif /* LWIP_TCP */
   default:
     /* Unsupported netconn type, e.g. protocol disabled */
     msg->conn->err = ERR_VAL;
     break;
   }

  return msg->conn->err;
}

/**
 * Create a new pcb of a specific type inside a netconn.
 * Called from netconn_new_with_proto_and_callback.
 *
 * @param msg the api_msg_msg describing the connection type
 */
void
do_newconn(struct api_msg_msg *msg)
{
   if(msg->conn->pcb.tcp == NULL) {
     pcb_new(msg);
   }
   /* Else? This "new" connection already has a PCB allocated. */
   /* Is this an error condition? Should it be deleted? */
   /* We currently just are happy and return. */

   TCPIP_APIMSG_ACK(msg);
}

/**
 * Delete the pcb inside a netconn.
 * Called from netconn_delete.
 *
 * @param msg the api_msg_msg pointing to the connection
 */
void
do_delconn(struct api_msg_msg *msg)
{
  if (msg->conn->pcb.tcp != NULL) {
    switch (NETCONNTYPE_GROUP(msg->conn->type)) {
#if LWIP_RAW
    case NETCONN_RAW:
      raw_remove(msg->conn->pcb.raw);
      break;
#endif /* LWIP_RAW */
#if LWIP_UDP
    case NETCONN_UDP:
      msg->conn->pcb.udp->recv_arg = NULL;
      udp_remove(msg->conn->pcb.udp);
      break;
#endif /* LWIP_UDP */
#if LWIP_TCP
    case NETCONN_TCP:
      if (msg->conn->pcb.tcp->state == LISTEN) {
        tcp_arg(msg->conn->pcb.tcp, NULL);
        tcp_accept(msg->conn->pcb.tcp, NULL);
        tcp_close(msg->conn->pcb.tcp);
      } else {
        tcp_arg(msg->conn->pcb.tcp, NULL);
        tcp_sent(msg->conn->pcb.tcp, NULL);
        tcp_recv(msg->conn->pcb.tcp, NULL);
        tcp_poll(msg->conn->pcb.tcp, NULL, 0);
        tcp_err(msg->conn->pcb.tcp, NULL);
        if (tcp_close(msg->conn->pcb.tcp) != ERR_OK) {
          tcp_abort(msg->conn->pcb.tcp);
        }
      }
      break;
#endif /* LWIP_TCP */
    default:
      break;
    }
  }
  /* Trigger select() in socket layer */
  if (msg->conn->callback) {
      (*msg->conn->callback)(msg->conn, NETCONN_EVT_RCVPLUS, 0);
      (*msg->conn->callback)(msg->conn, NETCONN_EVT_SENDPLUS, 0);
  }

  if (msg->conn->mbox != SYS_MBOX_NULL) {
    TCPIP_APIMSG_ACK(msg);
  }
}

/**
 * Bind a pcb contained in a netconn
 * Called from netconn_bind.
 *
 * @param msg the api_msg_msg pointing to the connection and containing
 *            the IP address and port to bind to
 */
void
do_bind(struct api_msg_msg *msg)
{
  if (msg->conn->err == ERR_OK) {
    if (msg->conn->pcb.tcp != NULL) {
      switch (NETCONNTYPE_GROUP(msg->conn->type)) {
#if LWIP_RAW
      case NETCONN_RAW:
        msg->conn->err = raw_bind(msg->conn->pcb.raw, msg->msg.bc.ipaddr);
        break;
#endif /* LWIP_RAW */
#if LWIP_UDP
      case NETCONN_UDP:
        msg->conn->err = udp_bind(msg->conn->pcb.udp, msg->msg.bc.ipaddr, msg->msg.bc.port);
        break;
#endif /* LWIP_UDP */
#if LWIP_TCP
      case NETCONN_TCP:
        msg->conn->err = tcp_bind(msg->conn->pcb.tcp, msg->msg.bc.ipaddr, msg->msg.bc.port);
        break;
#endif /* LWIP_TCP */
      default:
        break;
      }
    } else {
      /* msg->conn->pcb is NULL */
      msg->conn->err = ERR_VAL;
    }
  }
  TCPIP_APIMSG_ACK(msg);
}

#if LWIP_TCP
/**
 * TCP callback function if a connection (opened by tcp_connect/do_connect) has
 * been established (or reset by the remote host).
 *
 * @see tcp.h (struct tcp_pcb.connected) for parameters and return values
 */
static err_t
do_connected(void *arg, struct tcp_pcb *pcb, err_t err)
{
  struct netconn *conn;

  LWIP_UNUSED_ARG(pcb);

  conn = arg;

  if (conn == NULL) {
    return ERR_VAL;
  }

  conn->err = err;
  if (conn->type == NETCONN_TCP && err == ERR_OK) {
    setup_tcp(conn);
  }
  conn->state = NETCONN_NONE;
  sys_mbox_post(conn->mbox, NULL);
  return ERR_OK;
}
#endif /* LWIP_TCP */

/**
 * Connect a pcb contained inside a netconn
 * Called from netconn_connect.
 *
 * @param msg the api_msg_msg pointing to the connection and containing
 *            the IP address and port to connect to
 */
void
do_connect(struct api_msg_msg *msg)
{
  if (msg->conn->pcb.tcp == NULL) {
    sys_mbox_post(msg->conn->mbox, NULL);
    return;
  }

  switch (NETCONNTYPE_GROUP(msg->conn->type)) {
#if LWIP_RAW
  case NETCONN_RAW:
    msg->conn->err = raw_connect(msg->conn->pcb.raw, msg->msg.bc.ipaddr);
    sys_mbox_post(msg->conn->mbox, NULL);
    break;
#endif /* LWIP_RAW */
#if LWIP_UDP
  case NETCONN_UDP:
    msg->conn->err = udp_connect(msg->conn->pcb.udp, msg->msg.bc.ipaddr, msg->msg.bc.port);
    sys_mbox_post(msg->conn->mbox, NULL);
    break;
#endif /* LWIP_UDP */
#if LWIP_TCP
  case NETCONN_TCP:
    msg->conn->state = NETCONN_CONNECT;
    setup_tcp(msg->conn);
    msg->conn->err = tcp_connect(msg->conn->pcb.tcp, msg->msg.bc.ipaddr, msg->msg.bc.port,
    do_connected);
    break;
#endif /* LWIP_TCP */
  default:
    break;
  }
}

/**
 * Connect a pcb contained inside a netconn
 * Only used for UDP netconns.
 * Called from netconn_disconnect.
 *
 * @param msg the api_msg_msg pointing to the connection to disconnect
 */
void
do_disconnect(struct api_msg_msg *msg)
{
#if LWIP_UDP
  if (NETCONNTYPE_GROUP(msg->conn->type) == NETCONN_UDP) {
    udp_disconnect(msg->conn->pcb.udp);
  }
#endif /* LWIP_UDP */
  TCPIP_APIMSG_ACK(msg);
}

/**
 * Set a TCP pcb contained in a netconn into listen mode
 * Called from netconn_listen.
 *
 * @param msg the api_msg_msg pointing to the connection
 */
void
do_listen(struct api_msg_msg *msg)
{
#if LWIP_TCP
  if (msg->conn->err == ERR_OK) {
    if (msg->conn->pcb.tcp != NULL) {
      if (msg->conn->type == NETCONN_TCP) {
        msg->conn->pcb.tcp = tcp_listen(msg->conn->pcb.tcp);
        if (msg->conn->pcb.tcp == NULL) {
          msg->conn->err = ERR_MEM;
        } else {
          if (msg->conn->acceptmbox == SYS_MBOX_NULL) {
            if ((msg->conn->acceptmbox = sys_mbox_new()) == SYS_MBOX_NULL) {
              msg->conn->err = ERR_MEM;
            }
          }
          if (msg->conn->err == ERR_OK) {
            tcp_arg(msg->conn->pcb.tcp, msg->conn);
            tcp_accept(msg->conn->pcb.tcp, accept_function);
          }
        }
      }
    }
  }
#endif /* LWIP_TCP */
  TCPIP_APIMSG_ACK(msg);
}

/**
 * Send some data on a RAW or UDP pcb contained in a netconn
 * Called from netconn_send
 *
 * @param msg the api_msg_msg pointing to the connection
 */
void
do_send(struct api_msg_msg *msg)
{
  if (msg->conn->err == ERR_OK) {
    if (msg->conn->pcb.tcp != NULL) {
      switch (NETCONNTYPE_GROUP(msg->conn->type)) {
#if LWIP_RAW
      case NETCONN_RAW:
        if (msg->msg.b->addr == NULL) {
          msg->conn->err = raw_send(msg->conn->pcb.raw, msg->msg.b->p);
        } else {
          msg->conn->err = raw_sendto(msg->conn->pcb.raw, msg->msg.b->p, msg->msg.b->addr);
        }
        break;
#endif
#if LWIP_UDP
      case NETCONN_UDP:
        if (msg->msg.b->addr == NULL) {
          msg->conn->err = udp_send(msg->conn->pcb.udp, msg->msg.b->p);
        } else {
          msg->conn->err = udp_sendto(msg->conn->pcb.udp, msg->msg.b->p, msg->msg.b->addr, msg->msg.b->port);
        }
        break;
#endif /* LWIP_UDP */
      default:
        break;
      }
    }
  }
  TCPIP_APIMSG_ACK(msg);
}

/**
 * Recv some data from a RAW or UDP pcb contained in a netconn
 * Called from netconn_recv
 *
 * @param msg the api_msg_msg pointing to the connection
 */
void
do_recv(struct api_msg_msg *msg)
{
#if LWIP_TCP
  if (msg->conn->err == ERR_OK) {
    if (msg->conn->pcb.tcp != NULL) {
      if (msg->conn->type == NETCONN_TCP) {
        tcp_recved(msg->conn->pcb.tcp, msg->msg.r.len);
      }
    }
  }
#endif /* LWIP_TCP */
  TCPIP_APIMSG_ACK(msg);
}

/**
 * Send some data on a TCP pcb contained in a netconn
 * Called from netconn_write
 *
 * @param msg the api_msg_msg pointing to the connection
 */
void
do_write(struct api_msg_msg *msg)
{
#if LWIP_TCP
  err_t err;
#endif /* LWIP_TCP */
  if (msg->conn->err == ERR_OK) {
    if (msg->conn->pcb.tcp != NULL) {
      if (msg->conn->type == NETCONN_TCP) {
#if LWIP_TCP
        err = tcp_write(msg->conn->pcb.tcp, msg->msg.w.dataptr,
                        msg->msg.w.len, msg->msg.w.copy);
        /* This is the Nagle algorithm: inhibit the sending of new TCP
           segments when new outgoing data arrives from the user if any
           previously transmitted data on the connection remains
           unacknowledged. */
        if(err == ERR_OK && (msg->conn->pcb.tcp->unacked == NULL ||
          (msg->conn->pcb.tcp->flags & TF_NODELAY) || 
          (msg->conn->pcb.tcp->snd_queuelen) > 1)) {
            err = tcp_output(msg->conn->pcb.tcp);
        }
        msg->conn->err = err;
        if (msg->conn->callback)
          if (err == ERR_OK) {
            if (tcp_sndbuf(msg->conn->pcb.tcp) <= TCP_SNDLOWAT)
              (*msg->conn->callback)(msg->conn, NETCONN_EVT_SENDMINUS, msg->msg.w.len);
          }
#endif /* LWIP_TCP */
#if (LWIP_UDP || LWIP_RAW)
      } else {
        msg->conn->err = ERR_VAL;
#endif /* (LWIP_UDP || LWIP_RAW) */
      }
    }
  }
  TCPIP_APIMSG_ACK(msg);
}

/**
 * Close a TCP pcb contained in a netconn
 * Called from netconn_close
 *
 * @param msg the api_msg_msg pointing to the connection
 */
void
do_close(struct api_msg_msg *msg)
{
#if LWIP_TCP
  if (msg->conn->pcb.tcp != NULL) {
    if (msg->conn->type == NETCONN_TCP) {
      if (msg->conn->pcb.tcp->state == LISTEN) {
        msg->conn->err = tcp_close(msg->conn->pcb.tcp);
      } else if (msg->conn->pcb.tcp->state == CLOSE_WAIT) {
        msg->conn->err = tcp_output(msg->conn->pcb.tcp);
      }
    }
  }
#endif /* LWIP_TCP */
  TCPIP_APIMSG_ACK(msg);
}

#if LWIP_IGMP
/**
 * Join multicast groups for UDP netconns.
 * Called from netconn_join_leave_group
 *
 * @param msg the api_msg_msg pointing to the connection
 */
void
do_join_leave_group(struct api_msg_msg *msg)
{ 
  if (msg->conn->err == ERR_OK) {
    if (msg->conn->pcb.tcp != NULL) {
      if (NETCONNTYPE_GROUP(msg->conn->type) == NETCONN_UDP) {
#if LWIP_UDP
        if (msg->msg.jl.join_or_leave == NETCONN_JOIN) {
          msg->conn->err = igmp_joingroup ( netif_default, msg->msg.jl.multiaddr);
        } else {
          msg->conn->err = igmp_leavegroup( netif_default, msg->msg.jl.multiaddr);
        }
#endif /* LWIP_UDP */
#if (LWIP_TCP || LWIP_RAW)
      } else {
        msg->conn->err = ERR_VAL;
#endif /* (LWIP_TCP || LWIP_RAW) */
      }
    }
  }
  TCPIP_APIMSG_ACK(msg);
}
#endif /* LWIP_IGMP */

#endif /* !NO_SYS */
