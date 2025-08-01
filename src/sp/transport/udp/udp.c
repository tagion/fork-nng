// Copyright 2025 Staysail Systems, Inc. <info@staysail.tech>
//
// This software is supplied under the terms of the MIT License, a
// copy of which should be located in the distribution where this
// file was obtained (LICENSE.txt).  A copy of the license may also be
// found online at https://opensource.org/licenses/MIT.
//
#include <stdio.h>

#include "core/aio.h"
#include "core/defs.h"
#include "core/idhash.h"
#include "core/message.h"
#include "core/nng_impl.h"
#include "core/options.h"
#include "core/pipe.h"
#include "core/platform.h"
#include "nng/nng.h"

#include <string.h>

// Experimental UDP transport.  Unicast only.
typedef struct udp_pipe udp_pipe;
typedef struct udp_ep   udp_ep;

// OP code, 8 bits
enum udp_opcode {
	OPCODE_DATA = 0,
	OPCODE_CREQ = 1,
	OPCODE_CACK = 2,
	OPCODE_DISC = 3,
	OPCODE_MESH = 4,
};

// Disconnect reason, must be 16 bits
typedef enum udp_disc_reason {
	DISC_CLOSED   = 0, // normal close
	DISC_TYPE     = 1, // bad SP type
	DISC_REFUSED  = 3, // refused by policy
	DISC_MSGSIZE  = 4, // message too large
	DISC_NEGO     = 5, // neogtiation failed
	DISC_INACTIVE = 6, // closed due to inactivity
	DISC_PROTO    = 7, // other protocol error
	DISC_NOBUF    = 8, // resources exhausted
} udp_disc_reason;

#ifndef NNG_UDP_TXQUEUE_LEN
#define NNG_UDP_TXQUEUE_LEN 32
#endif

#ifndef NNG_UDP_RXQUEUE_LEN
#define NNG_UDP_RXQUEUE_LEN 16
#endif

#ifndef NNG_UDP_RECVMAX
#define NNG_UDP_RECVMAX 65000 // largest permitted by spec
#endif

#ifndef NNG_UDP_COPYMAX // threshold for copying instead of loan up
#define NNG_UDP_COPYMAX 1024
#endif

#ifndef NNG_UDP_REFRESH
#define NNG_UDP_REFRESH (5 * NNI_SECOND)
#endif

#ifndef NNG_UDP_CONNRETRY
#define NNG_UDP_CONNRETRY (NNI_SECOND / 5)
#endif

#define UDP_EP_ROLE(ep) ((ep)->dialer ? "dialer  " : "listener")

// NB: Each of the following messages is exactly 20 bytes in size

typedef struct udp_sp_msg {
	uint8_t  us_ver;
	uint8_t  us_op_code;
	uint16_t us_type;
	uint16_t us_params[2];
} udp_sp_msg;

#define us_length us_params[0]  // for DATA requests
#define us_recvmax us_params[0] // for CREQ, CACK
#define us_refresh us_params[1] // for CREQ, CACK
#define us_reason us_params[0]  // for DISC

// Like a NIC driver, this is a "descriptor" for UDP TX packets.
// This allows us to create a circular ring of these to support
// queueing for TX gracefully.
typedef struct udp_txdesc {
	udp_sp_msg   header;  // UDP transport message headers
	nni_msg     *payload; // may be null, only for data messages
	nng_sockaddr sa;
	bool         submitted; // true if submitted
} udp_txdesc;

typedef struct udp_txring {
	udp_txdesc *descs;
	uint16_t    head;
	uint16_t    tail;
	uint16_t    count;
	uint16_t    size;
} udp_txring;

typedef enum {
	PIPE_CONN_INIT,  // pipe is created, but not yet matched to a peer
	PIPE_CONN_MATCH, // pipe matched to peer, but not added to SP socket
	PIPE_CONN_DONE,  // pipe is fully owned by the SP socket
} udp_pipe_state;

#define UDP_TXRING_SZ 128

// UDP pipe resend (CREQ) in msec (nng_duration)
#define UDP_PIPE_REFRESH(p) ((p)->refresh)

// UDP pipe timeout in msec (nng_duration)
#define UDP_PIPE_TIMEOUT(p) ((p)->refresh * 5)

struct udp_pipe {
	udp_ep        *ep;
	nni_pipe      *npipe;
	nng_sockaddr   peer_addr;
	uint16_t       peer;
	uint16_t       proto;
	uint64_t       id;
	uint32_t       self_id;
	uint32_t       peer_id;
	uint16_t       sndmax; // peer's max recv size
	uint16_t       rcvmax; // max recv size
	bool           closed;
	bool           dialer;
	nng_duration   refresh; // seconds, for the protocol
	nng_time       next_wake;
	nng_time       next_creq;
	nng_time       expire;
	nni_list_node  node;
	nni_lmq        rx_mq;
	nni_list       rx_aios;
	udp_pipe_state state;
};

struct udp_ep {
	nng_udp      *udp;
	nni_mtx       mtx;
	uint16_t      proto;
	uint16_t      peer;
	uint16_t      af; // address family
	bool          started;
	bool          closed;
	bool          stopped;
	bool          cooldown;
	nng_url      *url;
	const char   *host; // for dialers
	nni_aio      *useraio;
	nni_aio      *connaio;
	nni_aio       timeaio;
	nni_aio       resaio;
	bool          dialer;
	bool          tx_busy; // true if tx pending
	nni_listener *nlistener;
	nni_dialer   *ndialer;
	nni_msg      *rx_payload; // current receive message
	nng_sockaddr  rx_sa;      // addr for last message
	nni_aio       tx_aio;     // aio for TX handling
	nni_aio       rx_aio;     // aio for RX handling
	nni_id_map    pipes;      // pipes (indexed by id)
	nni_sockaddr  self_sa;    // our address
	nni_sockaddr  peer_sa;    // peer address, only for dialer;
	nni_sockaddr  mesh_sa;    // mesh source address (ours)
	nni_list      connaios;   // aios from accept waiting for a client peer
	nni_list      connpipes;  // pipes waiting to be connected
	nng_duration  refresh; // refresh interval for connections in seconds
	udp_sp_msg   *rx_msg;  // contains the received message header
	uint16_t      rcvmax;  // max payload, trimmed to uint16_t
	uint16_t      copymax;
	udp_txring    tx_ring;
	nni_time      next_wake;
	nni_aio_completions complq;
	nni_resolv_item     resolv;

	nni_stat_item st_rcv_max;
	nni_stat_item st_rcv_toobig;
	nni_stat_item st_rcv_nomatch;
	nni_stat_item st_rcv_copy;
	nni_stat_item st_rcv_nocopy;
	nni_stat_item st_rcv_nobuf;
	nni_stat_item st_snd_toobig;
	nni_stat_item st_snd_nobuf;
	nni_stat_item st_peer_inactive;
	nni_stat_item st_copy_max;
};

static void udp_ep_start(udp_ep *);
static void udp_resolv_cb(void *);
static void udp_rx_cb(void *);

static void udp_recv_data(
    udp_ep *ep, udp_sp_msg *msg, size_t len, const nng_sockaddr *sa);
static void udp_send_disc_full(
    udp_ep *ep, const nng_sockaddr *sa, udp_disc_reason reason);
static void udp_send_disc(udp_ep *ep, udp_pipe *p, udp_disc_reason reason);

static void    udp_ep_match(udp_ep *ep);
static nng_err udp_add_pipe(udp_ep *ep, udp_pipe *p);
static void    udp_remove_pipe(udp_pipe *p);

static void
udp_tran_init(void)
{
}

static void
udp_tran_fini(void)
{
}

static void
udp_pipe_close(void *arg)
{
	udp_pipe *p  = arg;
	udp_ep   *ep = p->ep;
	nni_aio  *aio;

	nni_mtx_lock(&ep->mtx);
	udp_remove_pipe(p);
	udp_send_disc(ep, p, DISC_CLOSED);
	while ((aio = nni_list_first(&p->rx_aios)) != NULL) {
		nni_aio_list_remove(aio);
		nni_aio_finish_error(aio, NNG_ECLOSED);
	}
	nni_mtx_unlock(&ep->mtx);
}

static void
udp_pipe_stop(void *arg)
{
	udp_pipe *p  = arg;
	udp_ep   *ep = p->ep;

	udp_pipe_close(arg);

	nni_mtx_lock(&ep->mtx);
	udp_remove_pipe(p);
	nni_mtx_unlock(&ep->mtx);
}

static int
udp_pipe_init(void *arg, nni_pipe *npipe)
{
	udp_pipe *p = arg;
	p->npipe    = npipe;
	nni_aio_list_init(&p->rx_aios);
	nni_lmq_init(&p->rx_mq, NNG_UDP_RXQUEUE_LEN);
	return (0);
}

static nng_err
udp_pipe_start(udp_pipe *p, udp_ep *ep, const nng_sockaddr *sa)
{
	nni_time now = nni_clock();
	p->ep        = ep;
	p->proto     = ep->proto;
	p->peer      = ep->peer;
	p->peer_addr = *sa;
	p->dialer    = ep->dialer;
	p->refresh   = p->dialer ? NNG_UDP_CONNRETRY : ep->refresh;
	p->rcvmax    = ep->rcvmax;
	p->id        = nng_sockaddr_hash(sa);
	p->expire = now + (p->dialer ? (5 * NNI_SECOND) : UDP_PIPE_TIMEOUT(p));

	return (udp_add_pipe(ep, p));
}

static void
udp_pipe_fini(void *arg)
{
	udp_pipe *p = arg;
	nng_msg  *m;

	// call with ep->mtx lock held
	while (!nni_lmq_empty(&p->rx_mq)) {
		nni_lmq_get(&p->rx_mq, &m);
		nni_msg_free(m);
	}
	nni_lmq_fini(&p->rx_mq);
	NNI_ASSERT(nni_list_empty(&p->rx_aios));
}

static udp_pipe *
udp_find_pipe(udp_ep *ep, const nng_sockaddr *peer_addr)
{
	uint64_t  id = nng_sockaddr_hash(peer_addr);
	udp_pipe *p;

	// we'll keep incrementing id until we conclusively match
	// or we get a NULL.  This is another level of rehashing, but
	// it keeps us from having to look up.
	for (;;) {
		if ((p = nni_id_get(&ep->pipes, id)) == NULL) {
			return (NULL);
		}
		if (nng_sockaddr_equal(&p->peer_addr, peer_addr)) {
			return (p);
		}
		id++;
		if (id == 0) {
			id = 1;
		}
	}
}

static void
udp_remove_pipe(udp_pipe *p)
{
	// ep locked
	udp_ep  *ep = p->ep;
	uint64_t id = p->id;
	if (id == 0) {
		return;
	}
	p->id = 0;
	for (;;) {
		udp_pipe *srch;
		if ((srch = nni_id_get(&ep->pipes, id)) == NULL) {
			break;
		}
		if (srch == p) {
			nni_id_remove(&ep->pipes, id);
			break;
		}
		id++;
		if (id == 0) {
			id = 1;
		}
	}
	if (p->state < PIPE_CONN_DONE) {
		nni_list_node_remove(&p->node);
		nni_pipe_rele(p->npipe);
	}
}

static nng_err
udp_add_pipe(udp_ep *ep, udp_pipe *p)
{
	// Id must be part of the hash
	uint64_t id = p->id;
	while (nni_id_get(&ep->pipes, id) != NULL) {
		id++;
		if (id == 0) {
			id = 1;
		}
	}
	return (nni_id_set(&ep->pipes, id, p));
}

static void
udp_pipe_schedule(udp_pipe *p)
{
	udp_ep *ep      = p->ep;
	bool    changed = false;
	if (p->expire < ep->next_wake) {
		ep->next_wake = p->expire;
		changed       = true;
	}
	if (p->next_wake < ep->next_wake) {
		ep->next_wake = p->next_wake;
		changed       = true;
	}
	if (changed) {
		nni_aio_abort(&ep->timeaio, NNG_EINTR);
	}
}

static void
udp_start_rx(udp_ep *ep)
{
	nni_iov iov;

	if (ep->closed) {
		return;
	}

	// We use this trick to collect the message header so that we can
	// do the entire message in a single iov, which avoids the need to
	// scatter/gather (which can be problematic for platforms that cannot
	// do scatter/gather due to missing recvmsg.)
	(void) nni_msg_insert(ep->rx_payload, NULL, sizeof(udp_sp_msg));
	iov.iov_buf = nni_msg_body(ep->rx_payload);
	iov.iov_len = nni_msg_len(ep->rx_payload);
	ep->rx_msg  = nni_msg_body(ep->rx_payload);
	nni_msg_trim(ep->rx_payload, sizeof(udp_sp_msg));

	nni_aio_set_input(&ep->rx_aio, 0, &ep->rx_sa);
	nni_aio_set_iov(&ep->rx_aio, 1, &iov);
	nng_udp_recv(ep->udp, &ep->rx_aio);
}

static void
udp_start_tx(udp_ep *ep)
{
	udp_txring *ring = &ep->tx_ring;
	udp_txdesc *desc;
	nng_msg    *msg;

	if ((!ring->count) || (!ep->started) || ep->tx_busy || ep->stopped) {
		return;
	}
	ep->tx_busy = true;

	// NB: This does not advance the tail yet.
	// The tail will be advanced when the operation is complete.
	desc = &ring->descs[ring->tail];
	nni_iov iov[3];
	int     niov = 0;

	NNI_ASSERT(desc->submitted);
	iov[0].iov_buf = &desc->header;
	iov[0].iov_len = sizeof(desc->header);
	niov++;

	if ((msg = desc->payload) != NULL) {
		if (nni_msg_header_len(msg) > 0) {
			iov[niov].iov_buf = nni_msg_header(msg);
			iov[niov].iov_len = nni_msg_header_len(msg);
			niov++;
		}
		if (nni_msg_len(msg) > 0) {
			iov[niov].iov_buf = nni_msg_body(msg);
			iov[niov].iov_len = nni_msg_len(msg);
			niov++;
		}
	}
	nni_aio_set_input(&ep->tx_aio, 0, &desc->sa);
	nni_aio_set_iov(&ep->tx_aio, niov, iov);
	// it should *never* take this long, but allow for ARP resolution
	nni_aio_set_timeout(&ep->tx_aio, NNI_SECOND * 10);
	nng_udp_send(ep->udp, &ep->tx_aio);
}

static void
udp_queue_tx(
    udp_ep *ep, const nng_sockaddr *sa, udp_sp_msg *msg, nni_msg *payload)
{
	udp_txring *ring = &ep->tx_ring;
	udp_txdesc *desc = &ring->descs[ring->head];

	if (ring->count == ring->size || !ep->started) {
		// ring is full
		nni_stat_inc(&ep->st_snd_nobuf, 1);
		if (payload != NULL) {
			nni_msg_free(payload);
		}
		return;
	}
#ifdef NNG_LITTLE_ENDIAN
	// This covers modern GCC, clang, Visual Studio.
	desc->header = *msg;
#else
	// Fix the endianness, so other routines don't have to.
	// We only have to do this for systems that are not known
	// (at compile time) to be little endian.
	desc->header.us_ver     = 0x1;
	desc->header.us_op_code = msg->us_op_code;
	NNI_PUT16LE(&desc->header.us_type, msg->us_type);
	NNI_PUT16LE(&desc->header.us_params[0], msg->us_params[0]);
	NNI_PUT16LE(&desc->header.us_params[1], msg->us_params[1]);
#endif

	desc->payload   = payload;
	desc->sa        = *sa;
	desc->submitted = true;
	ring->count++;
	ring->head++;
	if (ring->head == ring->size) {
		ring->head = 0;
	}
	udp_start_tx(ep);
}

static void
udp_finish_tx(udp_ep *ep)
{
	udp_txring *ring = &ep->tx_ring;
	udp_txdesc *desc;

	NNI_ASSERT(ring->count > 0);
	desc = &ring->descs[ring->tail];
	NNI_ASSERT(desc->submitted);
	if (desc->payload != NULL) {
		nni_msg_free(desc->payload);
		desc->payload = NULL;
	}
	desc->submitted = false;
	ring->tail++;
	ring->count--;
	if (ring->tail == ring->size) {
		ring->tail = 0;
	}
	ep->tx_busy = false;

	// possibly start another tx going
	udp_start_tx(ep);
}

static void
udp_send_disc(udp_ep *ep, udp_pipe *p, udp_disc_reason reason)
{
	nni_aio *aio;
	if (p->closed) {
		return;
	}
	p->closed = true;
	while ((aio = nni_list_first(&p->rx_aios)) != NULL) {
		nni_aio_list_remove(aio);
		nni_aio_finish_error(aio, NNG_ECLOSED);
	}
	udp_send_disc_full(ep, &p->peer_addr, reason);
	nni_pipe_close(p->npipe);
}

static void
udp_send_disc_full(udp_ep *ep, const nng_sockaddr *sa, udp_disc_reason reason)
{
	udp_sp_msg disc;

	disc.us_ver       = 0x1;
	disc.us_op_code   = OPCODE_DISC;
	disc.us_type      = ep->proto;
	disc.us_reason    = (uint16_t) reason;
	disc.us_params[1] = 0;
	udp_queue_tx(ep, sa, (void *) &disc, NULL);
}

static void
udp_send_creq(udp_ep *ep, udp_pipe *p)
{
	udp_sp_msg creq;
	creq.us_ver     = 0x1;
	creq.us_op_code = OPCODE_CREQ;
	creq.us_type    = p->proto;
	creq.us_recvmax = p->rcvmax;
	creq.us_refresh = (p->refresh + NNI_SECOND - 1) / NNI_SECOND;
	p->next_creq    = nni_clock() + UDP_PIPE_REFRESH(p);
	p->next_wake    = p->next_creq;

	udp_pipe_schedule(p);
	udp_queue_tx(ep, &p->peer_addr, (void *) &creq, NULL);
}

static void
udp_send_cack(udp_ep *ep, udp_pipe *p)
{
	udp_sp_msg cack;
	cack.us_ver     = 0x01;
	cack.us_op_code = OPCODE_CACK;
	cack.us_type    = p->proto;
	cack.us_recvmax = p->rcvmax;
	cack.us_refresh = (p->refresh + NNI_SECOND - 1) / NNI_SECOND;
	udp_queue_tx(ep, &p->peer_addr, (void *) &cack, NULL);
}

static void
udp_recv_disc(udp_ep *ep, udp_sp_msg *disc, const nng_sockaddr *sa)
{
	udp_pipe *p;
	nni_aio  *aio;
	char      buf[NNG_MAXADDRSTRLEN];

	nng_log_debug("NNG-UDP-DISC", "Received disconnect from %s reason %d",
	    nng_str_sockaddr(sa, buf, sizeof(buf)), disc->us_reason);

	p = udp_find_pipe(ep, sa);
	if (p != NULL) {
		p->closed = true;
		while ((aio = nni_list_first(&p->rx_aios)) != NULL) {
			nni_aio_list_remove(aio);
			nni_aio_finish_error(aio, NNG_ECLOSED);
		}
		nni_pipe_close(p->npipe);
	}
}

// Receive data for the pipe.  Returns true if we used
// the message, false otherwise.
static void
udp_recv_data(udp_ep *ep, udp_sp_msg *dreq, size_t len, const nng_sockaddr *sa)
{
	// NB: ep mtx is locked
	udp_pipe *p;
	nni_aio  *aio;
	nni_msg  *msg;
	nni_time  now;

	if ((p = udp_find_pipe(ep, sa)) == NULL) {
		nni_stat_inc(&ep->st_rcv_nomatch, 1);
		return;
	}

	now = nni_clock();

	// Make sure the message wasn't truncated, and that it fits within
	// our maximum agreed upon payload.
	if ((dreq->us_length > len) || (dreq->us_length > p->rcvmax)) {
		nni_stat_inc(&ep->st_rcv_toobig, 1);
		udp_send_disc(ep, p, DISC_MSGSIZE);
		return;
	}

	p->expire    = now + UDP_PIPE_TIMEOUT(p);
	p->next_wake = now + UDP_PIPE_REFRESH(p);

	udp_pipe_schedule(p);

	// trim the message down to its
	nni_msg_chop(
	    ep->rx_payload, nni_msg_len(ep->rx_payload) - dreq->us_length);

	// We have a choice to make.  Drop this message (easiest), or
	// drop the oldest.  We drop the oldest because generally we
	// find that applications prefer to have more recent data rather
	// than keeping stale data.
	if (nni_lmq_full(&p->rx_mq)) {
		nni_msg *old;
		(void) nni_lmq_get(&p->rx_mq, &old);
		nni_msg_free(old);
		nni_stat_inc(&ep->st_rcv_nobuf, 1);
	}

	// Short message, just alloc and copy
	if (len <= ep->copymax) {
		nni_stat_inc(&ep->st_rcv_copy, 1);
		if (nng_msg_alloc(&msg, len) != 0) {
			if (p->npipe != NULL) {
				nni_pipe_bump_error(p->npipe, NNG_ENOMEM);
			}
			return;
		}
		nni_msg_set_address(msg, sa);
		memcpy(nni_msg_body(msg), nni_msg_body(ep->rx_payload), len);
		nni_lmq_put(&p->rx_mq, msg);
		nni_msg_realloc(ep->rx_payload, ep->rcvmax);
	} else {
		nni_stat_inc(&ep->st_rcv_nocopy, 1);
		// Message size larger than copy break, do zero copy
		msg = ep->rx_payload;
		if (nng_msg_alloc(&ep->rx_payload, ep->rcvmax) != 0) {
			ep->rx_payload = msg; // make sure we put it back
			if (p->npipe != NULL) {
				nni_pipe_bump_error(p->npipe, NNG_ENOMEM);
			}
			return;
		}

		if (len > nng_msg_len(msg)) {
			// chop off any unfilled tail
			nng_msg_chop(msg, nng_msg_len(msg) - len);
		}
		nni_msg_set_address(msg, sa);
		nni_lmq_put(&p->rx_mq, msg);
	}

	while (((aio = nni_list_first(&p->rx_aios)) != NULL) &&
	    (!nni_lmq_empty(&p->rx_mq))) {
		nni_aio_list_remove(aio);
		nni_lmq_get(&p->rx_mq, &msg);
		nni_aio_set_msg(aio, msg);
		nni_aio_completions_add(
		    &ep->complq, aio, 0, nni_aio_count(aio));
	}
}

static void
udp_recv_creq(udp_ep *ep, udp_sp_msg *creq, nng_sockaddr *sa)
{
	udp_pipe *p;
	nni_time  now;

	now = nni_clock();

	if (ep->closed) {
		// endpoint is closing down, just drop it without further ado
		return;
	}

	if (ep->dialer) {
		// dialers do not accept CREQ requests
		udp_send_disc_full(ep, sa, DISC_REFUSED);
		return;
	}
	if ((p = udp_find_pipe(ep, sa))) {
		if (p->peer != creq->us_type) {
			udp_send_disc(ep, p, DISC_TYPE);
			return;
		}

		// so we know who it is from.. this is a refresh.
		if (creq->us_refresh == 0) {
			udp_send_disc(ep, p, DISC_NEGO);
			return;
		}
		if ((creq->us_refresh * NNI_SECOND) < p->refresh) {
			p->refresh = creq->us_refresh * NNI_SECOND;
		}
		p->next_wake = now + UDP_PIPE_REFRESH(p);
		p->expire    = now + UDP_PIPE_TIMEOUT(p);

		udp_pipe_schedule(p);
		udp_send_cack(ep, p);
		return;
	}

	// new pipe
	if (creq->us_refresh == 0) {
		udp_send_disc_full(ep, sa, DISC_NEGO);
		return;
	}

	if (nni_pipe_alloc_listener((void **) &p, ep->nlistener) != 0) {
		udp_send_disc_full(ep, sa, DISC_NOBUF);
		return;
	}

	if (udp_pipe_start(p, ep, sa) != NNG_OK) {
		udp_send_disc(ep, p, DISC_NOBUF);
		nni_pipe_close(p->npipe);
		return;
	}

	if ((creq->us_refresh * NNI_SECOND) < p->refresh) {
		p->refresh = (creq->us_refresh * NNI_SECOND);
	}
	p->peer      = creq->us_type;
	p->sndmax    = creq->us_recvmax;
	p->next_wake = now + UDP_PIPE_REFRESH(p);

	udp_pipe_schedule(p);
	p->state = PIPE_CONN_MATCH;
	nni_list_append(&ep->connpipes, p);
	udp_send_cack(ep, p);
	udp_ep_match(ep);
}

static void
udp_recv_cack(udp_ep *ep, udp_sp_msg *cack, const nng_sockaddr *sa)
{
	udp_pipe *p;
	nni_time  now;

	if ((p = udp_find_pipe(ep, sa)) && (!p->closed)) {
		if (p->peer != cack->us_type) {
			udp_send_disc(ep, p, DISC_TYPE);
			return;
		}

		// so we know who it is from.. this is a refresh.
		p->sndmax = cack->us_recvmax;
		p->peer   = cack->us_type;

		if (cack->us_refresh == 0) {
			udp_send_disc(ep, p, DISC_NEGO);
			return;
		}
		// Always reset this, as dialers may have started with an
		// unreasonably low value.
		p->refresh = ep->refresh;
		if ((cack->us_refresh * NNI_SECOND) < p->refresh) {
			p->refresh = cack->us_refresh * NNI_SECOND;
		}
		now          = nni_clock();
		p->next_wake = now + UDP_PIPE_REFRESH(p);
		p->expire    = now + UDP_PIPE_TIMEOUT(p);
		udp_pipe_schedule(p);

		if (p->state < PIPE_CONN_MATCH) {
			p->state = PIPE_CONN_MATCH;
			nni_list_append(&ep->connpipes, p);
			udp_ep_match(ep);
		}
		return;
	}
}

static void
udp_tx_cb(void *arg)
{
	udp_ep *ep = arg;

	nni_mtx_lock(&ep->mtx);
	udp_finish_tx(ep);
	nni_mtx_unlock(&ep->mtx);
}

// In the case of unicast UDP, we don't know
// whether the message arrived from a connected peer as part of a
// logical connection, or is a message related to connection management.
static void
udp_rx_cb(void *arg)
{
	udp_ep             *ep  = arg;
	nni_aio            *aio = &ep->rx_aio;
	int                 rv;
	size_t              n;
	udp_sp_msg         *hdr;
	nng_sockaddr       *sa;
	nni_aio_completions complq;

	// for a received packet we are either receiving it for a
	// connection we already have established, or for a new connection.
	// Dialers cannot receive connection requests (as a safety
	// precaution).

	nni_mtx_lock(&ep->mtx);
	if ((rv = nni_aio_result(aio)) != 0) {
		// something bad happened on RX... which is unexpected.
		// sleep a little bit and hope for recovery.
		switch (nni_aio_result(aio)) {
		case NNG_ECLOSED:
		case NNG_ECANCELED:
		case NNG_ESTOPPED:
			nni_mtx_unlock(&ep->mtx);
			return;
		case NNG_ETIMEDOUT:
		case NNG_EAGAIN:
		case NNG_EINTR:
			ep->cooldown = false;
			goto finish;
			break;
		default:
			ep->cooldown = true;
			nni_sleep_aio(5, aio);
			nni_mtx_unlock(&ep->mtx);
			return;
		}
	}
	if (ep->cooldown) {
		ep->cooldown = false;
		goto finish;
	}

	// Received message will be in the ep rx header.
	hdr = ep->rx_msg;
	sa  = &ep->rx_sa;
	n   = nng_aio_count(aio);

	if ((n >= sizeof(*hdr)) && (hdr->us_ver == 1)) {
		n -= sizeof(*hdr);

#ifndef NNG_LITTLE_ENDIAN
		// Fix the endianness, so other routines don't have to.
		// We only have to do this for systems that are not known
		// (at compile time) to be little endian.
		hdr->us_type      = NNI_GET16LE(&hdr->us_type);
		hdr->us_params[0] = NNI_GET16LE(&hdr->us_params[0]);
		hdr->us_params[1] = NNI_GET16LE(&hdr->us_params[1]);
#endif

		switch (hdr->us_op_code) {
		case OPCODE_DATA:
			udp_recv_data(ep, hdr, n, sa);
			break;
		case OPCODE_CREQ:
			udp_recv_creq(ep, hdr, sa);
			break;
		case OPCODE_CACK:
			udp_recv_cack(ep, hdr, sa);
			break;
		case OPCODE_DISC:
			udp_recv_disc(ep, hdr, sa);
			break;
		case OPCODE_MESH: // TODO:
		                  // udp_recv_mesh(ep, &hdr->mesh, sa);
		                  // break;
		default:
			udp_send_disc_full(ep, sa, DISC_PROTO);
			break;
		}
	}

finish:
	// start another receive
	udp_start_rx(ep);

	// grab the list of completions so we can finish them.
	complq = ep->complq;
	nni_aio_completions_init(&ep->complq);
	nni_mtx_unlock(&ep->mtx);

	// now run the completions -- synchronously
	nni_aio_completions_run(&complq);
}

static void
udp_pipe_send(void *arg, nni_aio *aio)
{
	udp_pipe  *p = arg;
	udp_ep    *ep;
	udp_sp_msg dreq;
	nng_msg   *msg;
	size_t     count = 0;

	msg = nni_aio_get_msg(aio);
	ep  = p->ep;

	if (msg != NULL) {
		count = nni_msg_len(msg) + nni_msg_header_len(msg);
	}

	nni_aio_reset(aio);
	nni_mtx_lock(&ep->mtx);
	if ((nni_msg_len(msg) + nni_msg_header_len(msg)) > p->sndmax) {
		nni_mtx_unlock(&ep->mtx);
		// rather failing this with an error, we just drop it on
		// the floor. this is on the sender, so there isn't a
		// compelling need to disconnect the pipe, since it we're
		// not being "ill-behaved" to our peer.
		nni_aio_finish(aio, 0, count);
		nni_stat_inc(&ep->st_snd_toobig, 1);
		nni_msg_free(msg);
		return;
	}

	dreq.us_ver     = 1;
	dreq.us_type    = ep->proto;
	dreq.us_op_code = OPCODE_DATA;
	dreq.us_length  = (uint16_t) count;

	// Just queue it, or fail it.
	udp_queue_tx(ep, &p->peer_addr, (void *) &dreq, msg);
	nni_mtx_unlock(&ep->mtx);

	nni_aio_finish(aio, 0, count);
}

static void
udp_pipe_recv_cancel(nni_aio *aio, void *arg, nng_err rv)
{
	udp_pipe *p  = arg;
	udp_ep   *ep = p->ep;

	nni_mtx_lock(&ep->mtx);
	if (!nni_aio_list_active(aio)) {
		nni_mtx_unlock(&ep->mtx);
		return;
	}
	nni_aio_list_remove(aio);
	nni_mtx_unlock(&ep->mtx);
	nni_aio_finish_error(aio, rv);
}

static void
udp_pipe_recv(void *arg, nni_aio *aio)
{
	udp_pipe *p  = arg;
	udp_ep   *ep = p->ep;

	nni_aio_reset(aio);
	nni_mtx_lock(&ep->mtx);
	if (p->closed) {
		nni_mtx_unlock(&ep->mtx);
		nni_aio_finish_error(aio, NNG_ECLOSED);
		return;
	}
	if (!nni_aio_start(aio, udp_pipe_recv_cancel, p)) {
		nni_mtx_unlock(&ep->mtx);
		return;
	}

	nni_list_append(&p->rx_aios, aio);
	nni_mtx_unlock(&ep->mtx);
}

static uint16_t
udp_pipe_peer(void *arg)
{
	udp_pipe *p = arg;

	return (p->peer);
}

static nng_err
udp_pipe_get_recvmax(void *arg, void *v, size_t *szp, nni_type t)
{
	udp_pipe *p  = arg;
	udp_ep   *ep = p->ep;
	nng_err   rv;
	nni_mtx_lock(&ep->mtx);
	rv = nni_copyout_size(p->rcvmax, v, szp, t);
	nni_mtx_unlock(&ep->mtx);
	return (rv);
}

static nng_err
udp_pipe_get_remaddr(void *arg, void *v, size_t *szp, nni_type t)
{
	udp_pipe *p  = arg;
	udp_ep   *ep = p->ep;
	nng_err   rv;
	nni_mtx_lock(&ep->mtx);
	rv = nni_copyout_sockaddr(&p->peer_addr, v, szp, t);
	nni_mtx_unlock(&ep->mtx);
	return (rv);
}

static nni_option udp_pipe_options[] = {
	{
	    .o_name = NNG_OPT_RECVMAXSZ,
	    .o_get  = udp_pipe_get_recvmax,
	},
	{
	    .o_name = NNG_OPT_REMADDR,
	    .o_get  = udp_pipe_get_remaddr,
	},
	{
	    .o_name = NULL,
	},
};

static nng_err
udp_pipe_getopt(
    void *arg, const char *name, void *buf, size_t *szp, nni_type t)
{
	udp_pipe *p = arg;
	int       rv;

	rv = nni_getopt(udp_pipe_options, name, p, buf, szp, t);
	return (rv);
}

static void
udp_ep_fini(void *arg)
{
	udp_ep *ep = arg;

	nni_aio_fini(&ep->timeaio);
	nni_aio_fini(&ep->resaio);
	nni_aio_fini(&ep->tx_aio);
	nni_aio_fini(&ep->rx_aio);

	if (ep->udp != NULL) {
		nng_udp_close(ep->udp);
	}

	for (int i = 0; i < ep->tx_ring.size; i++) {
		nni_msg_free(ep->tx_ring.descs[i].payload);
		ep->tx_ring.descs[i].payload = NULL;
	}
	nni_msg_free(ep->rx_payload); // safe even if msg is null
	nni_id_map_fini(&ep->pipes);
	NNI_FREE_STRUCTS(ep->tx_ring.descs, ep->tx_ring.size);
}

static void
udp_ep_close(void *arg)
{
	udp_ep   *ep = arg;
	udp_pipe *p;
	nni_aio  *aio;
	uint32_t  cursor;
	uint64_t  key;

	nni_mtx_lock(&ep->mtx);
	ep->closed = true;

	// leave tx open so we can send disconnects
	nni_aio_close(&ep->resaio);
	nni_aio_close(&ep->rx_aio);
	nni_aio_close(&ep->timeaio);

	// close all the underlying pipes, so the peer can see it.
	while (nni_id_visit(&ep->pipes, &key, (void **) &p, &cursor)) {
		nni_pipe_close(p->npipe);
	}
	while ((aio = nni_list_first(&ep->connaios)) != NULL) {
		nni_aio_list_remove(aio);
		nni_aio_finish_error(aio, NNG_ECONNABORTED);
	}
	nni_mtx_unlock(&ep->mtx);
}

static void
udp_ep_stop(void *arg)
{
	udp_ep *ep = arg;

	nni_aio_stop(&ep->resaio);
	nni_aio_stop(&ep->rx_aio);
	nni_aio_stop(&ep->timeaio);

	// We optionally linger a little bit (up to a half second)
	// so that the disconnect messages can get pushed out.  On
	// most systems this should only take a single millisecond.
	nni_time linger =
	    nni_clock() + NNI_SECOND / 2; // half second to drain, max
	nni_mtx_lock(&ep->mtx);
	while ((ep->tx_ring.count > 0) && (nni_clock() < linger)) {
		nni_mtx_unlock(&ep->mtx);
		nng_msleep(1);
		nni_mtx_lock(&ep->mtx);
	}
	if (ep->tx_ring.count > 0) {
		nng_log_warn("NNG-UDP-LINGER",
		    "Lingering timed out on endpoint close, peer "
		    "notifications dropped");
	}
	ep->stopped = true;
	nni_mtx_unlock(&ep->mtx);

	// finally close the tx channel
	nni_aio_stop(&ep->tx_aio);
}

// timer handler - sends out additional creqs as needed,
// reaps stale connections, and handles linger.
static void
udp_timer_cb(void *arg)
{
	udp_ep   *ep = arg;
	udp_pipe *p;
	int       rv;

	nni_mtx_lock(&ep->mtx);
	rv = nni_aio_result(&ep->timeaio);
	switch (rv) {
	case NNG_ECLOSED:
	case NNG_ECANCELED:
	case NNG_ESTOPPED:
		nni_mtx_unlock(&ep->mtx);
		return;
	default:
		if (ep->closed) {
			nni_mtx_unlock(&ep->mtx);
			return;
		}
		break;
	}

	uint32_t     cursor  = 0;
	nni_time     now     = nni_clock();
	nng_duration refresh = ep->refresh;

	ep->next_wake = NNI_TIME_NEVER;
	while (nni_id_visit(&ep->pipes, NULL, (void **) &p, &cursor)) {

		if (now > p->expire) {
			char     buf[128];
			nni_aio *aio;
			nng_log_info("NNG-UDP-INACTIVE",
			    "Pipe peer %s timed out due to inactivity",
			    nng_str_sockaddr(&p->peer_addr, buf, sizeof(buf)));

			// Possibly alert the dialer, so it can restart a
			// new attempt.
			if ((ep->dialer) && (p->peer_id == 0) &&
			    (aio = nni_list_first(&ep->connaios))) {
				nni_aio_list_remove(aio);
				nni_aio_finish_error(aio, NNG_ETIMEDOUT);
			}

			// If we're still on the connect list, then we need
			// take responsibility for cleaning this up.
			if (nni_list_node_active(&p->node)) {
				nni_pipe_close(p->npipe);
				continue;
			}

			// This will probably not be received by the peer,
			// since we aren't getting anything from them. But
			// having it on the wire may help debugging later.
			nni_stat_inc(&ep->st_peer_inactive, 1);
			udp_send_disc(ep, p, DISC_INACTIVE);
			continue;
		}

		if (p->dialer && now > p->next_creq) {
			udp_send_creq(ep, p);
		}
		if (p->next_wake < ep->next_wake) {
			ep->next_wake = p->next_wake;
		}
	}
	refresh = ep->next_wake == NNI_TIME_NEVER
	    ? NNG_DURATION_INFINITE
	    : (nng_duration) (ep->next_wake - now);
	nni_sleep_aio(refresh, &ep->timeaio);

	nni_mtx_unlock(&ep->mtx);
}

static int
udp_ep_init(
    udp_ep *ep, nng_url *url, nni_sock *sock, nni_dialer *d, nni_listener *l)
{
	int rv;

	nni_mtx_init(&ep->mtx);
	nni_id_map_init(&ep->pipes, 1, 0xFFFFFFFF, true);
	NNI_LIST_INIT(&ep->connpipes, udp_pipe, node);
	nni_aio_list_init(&ep->connaios);

	nni_aio_init(&ep->rx_aio, udp_rx_cb, ep);
	nni_aio_init(&ep->tx_aio, udp_tx_cb, ep);
	nni_aio_init(&ep->timeaio, udp_timer_cb, ep);
	nni_aio_init(&ep->resaio, udp_resolv_cb, ep);
	nni_aio_completions_init(&ep->complq);

	ep->tx_ring.descs =
	    NNI_ALLOC_STRUCTS(ep->tx_ring.descs, NNG_UDP_TXQUEUE_LEN);
	if (ep->tx_ring.descs == NULL) {
		NNI_FREE_STRUCT(ep);
		return (NNG_ENOMEM);
	}
	ep->tx_ring.size = NNG_UDP_TXQUEUE_LEN;

	if (strcmp(url->u_scheme, "udp") == 0) {
		ep->af = NNG_AF_UNSPEC;
	} else if (strcmp(url->u_scheme, "udp4") == 0) {
		ep->af = NNG_AF_INET;
	} else if (strcmp(url->u_scheme, "udp6") == 0) {
		ep->af = NNG_AF_INET6;
	} else {
		return (NNG_EADDRINVAL);
	}

	ep->self_sa.s_family = ep->af;
	ep->proto            = nni_sock_proto_id(sock);
	ep->peer             = nni_sock_peer_id(sock);
	ep->url              = url;
	ep->refresh          = NNG_UDP_REFRESH; // one minute by default
	ep->rcvmax           = NNG_UDP_RECVMAX;
	ep->copymax          = NNG_UDP_COPYMAX;
	if ((rv = nni_msg_alloc(&ep->rx_payload, ep->rcvmax) != 0)) {
		NNI_FREE_STRUCTS(ep->tx_ring.descs, NNG_UDP_TXQUEUE_LEN);
		return (rv);
	}

	NNI_STAT_LOCK(rcv_max_info, "rcv_max", "maximum receive size",
	    NNG_STAT_LEVEL, NNG_UNIT_BYTES);
	NNI_STAT_LOCK(copy_max_info, "copy_max",
	    "threshold to switch to loan-up", NNG_STAT_LEVEL, NNG_UNIT_BYTES);
	NNI_STAT_LOCK(rcv_nomatch_info, "rcv_nomatch",
	    "messages without a matching connection", NNG_STAT_COUNTER,
	    NNG_UNIT_MESSAGES);
	NNI_STAT_LOCK(rcv_toobig_info, "rcv_toobig",
	    "received messages rejected because too big", NNG_STAT_COUNTER,
	    NNG_UNIT_MESSAGES);
	NNI_STAT_LOCK(rcv_copy_info, "rcv_copy",
	    "received messages copied (small)", NNG_STAT_COUNTER,
	    NNG_UNIT_MESSAGES);
	NNI_STAT_LOCK(rcv_nocopy_info, "rcv_nocopy",
	    "received messages zero copy (large)", NNG_STAT_COUNTER,
	    NNG_UNIT_MESSAGES);
	NNI_STAT_LOCK(rcv_nobuf_info, "rcv_nobuf",
	    "received messages dropped no buffer", NNG_STAT_COUNTER,
	    NNG_UNIT_MESSAGES);
	NNI_STAT_LOCK(snd_toobig_info, "snd_toobig",
	    "sent messages rejected because too big", NNG_STAT_COUNTER,
	    NNG_UNIT_MESSAGES);
	NNI_STAT_LOCK(snd_nobuf_info, "snd_nobuf",
	    "sent messages dropped no buffer", NNG_STAT_COUNTER,
	    NNG_UNIT_MESSAGES);
	NNI_STAT_LOCK(peer_inactive_info, "peer_inactive",
	    "connections closed due to inactive peer", NNG_STAT_COUNTER,
	    NNG_UNIT_EVENTS);

	nni_stat_init_lock(&ep->st_rcv_max, &rcv_max_info, &ep->mtx);
	nni_stat_init_lock(&ep->st_copy_max, &copy_max_info, &ep->mtx);
	nni_stat_init_lock(&ep->st_rcv_copy, &rcv_copy_info, &ep->mtx);
	nni_stat_init_lock(&ep->st_rcv_nocopy, &rcv_nocopy_info, &ep->mtx);
	nni_stat_init_lock(&ep->st_rcv_toobig, &rcv_toobig_info, &ep->mtx);
	nni_stat_init_lock(&ep->st_rcv_nomatch, &rcv_nomatch_info, &ep->mtx);
	nni_stat_init_lock(&ep->st_rcv_nobuf, &rcv_nobuf_info, &ep->mtx);
	nni_stat_init_lock(&ep->st_snd_toobig, &snd_toobig_info, &ep->mtx);
	nni_stat_init_lock(&ep->st_snd_nobuf, &snd_nobuf_info, &ep->mtx);
	nni_stat_init_lock(
	    &ep->st_peer_inactive, &peer_inactive_info, &ep->mtx);

	if (l) {
		NNI_ASSERT(d == NULL);
		nni_listener_add_stat(l, &ep->st_rcv_max);
		nni_listener_add_stat(l, &ep->st_copy_max);
		nni_listener_add_stat(l, &ep->st_rcv_copy);
		nni_listener_add_stat(l, &ep->st_rcv_nocopy);
		nni_listener_add_stat(l, &ep->st_rcv_toobig);
		nni_listener_add_stat(l, &ep->st_rcv_nomatch);
		nni_listener_add_stat(l, &ep->st_rcv_nobuf);
		nni_listener_add_stat(l, &ep->st_snd_toobig);
		nni_listener_add_stat(l, &ep->st_snd_nobuf);
	}
	if (d) {
		NNI_ASSERT(l == NULL);
		nni_dialer_add_stat(d, &ep->st_rcv_max);
		nni_dialer_add_stat(d, &ep->st_copy_max);
		nni_dialer_add_stat(d, &ep->st_rcv_copy);
		nni_dialer_add_stat(d, &ep->st_rcv_nocopy);
		nni_dialer_add_stat(d, &ep->st_rcv_toobig);
		nni_dialer_add_stat(d, &ep->st_rcv_nomatch);
		nni_dialer_add_stat(d, &ep->st_rcv_nobuf);
		nni_dialer_add_stat(d, &ep->st_snd_toobig);
		nni_dialer_add_stat(d, &ep->st_snd_nobuf);
	}

	// schedule our timer callback - forever for now
	// adjusted automatically as we add pipes or other
	// actions which require earlier wakeup.
	nni_sleep_aio(NNG_DURATION_INFINITE, &ep->timeaio);

	return (0);
}

static int
udp_check_url(nng_url *url, bool listen)
{
	// Check for invalid URL components.
	if ((strlen(url->u_path) != 0) && (strcmp(url->u_path, "/") != 0)) {
		return (NNG_EADDRINVAL);
	}
	if ((url->u_fragment != NULL) || (url->u_userinfo != NULL) ||
	    (url->u_query != NULL)) {
		return (NNG_EADDRINVAL);
	}
	if (!listen) {
		if ((strlen(url->u_hostname) == 0) || (url->u_port == 0)) {
			return (NNG_EADDRINVAL);
		}
	}
	return (0);
}

static nng_err
udp_dialer_init(void *arg, nng_url *url, nni_dialer *ndialer)
{
	udp_ep   *ep = arg;
	nng_err   rv;
	nni_sock *sock = nni_dialer_sock(ndialer);

	ep->ndialer = ndialer;
	if ((rv = udp_ep_init(ep, url, sock, ndialer, NULL)) != NNG_OK) {
		return (rv);
	}

	if ((rv = udp_check_url(url, false)) != NNG_OK) {
		return (rv);
	}

	return (NNG_OK);
}

static nng_err
udp_listener_init(void *arg, nng_url *url, nni_listener *nlistener)
{
	udp_ep   *ep = arg;
	nng_err   rv;
	nni_sock *sock = nni_listener_sock(nlistener);

	ep->nlistener = nlistener;
	if ((rv = udp_ep_init(ep, url, sock, NULL, nlistener)) != NNG_OK) {
		return (rv);
	}
	// Check for invalid URL components.
	if (((rv = udp_check_url(url, true)) != 0) ||
	    ((rv = nni_url_to_address(&ep->self_sa, url)) != NNG_OK)) {
		return (rv);
	}

	return (0);
}

static void
udp_ep_cancel(nni_aio *aio, void *arg, nng_err rv)
{
	udp_ep *ep = arg;
	nni_mtx_lock(&ep->mtx);
	if (nni_aio_list_active(aio)) {
		nni_aio_list_remove(aio);
		nni_aio_finish_error(aio, rv);
		nni_aio_abort(&ep->resaio, rv);
	}
	nni_mtx_unlock(&ep->mtx);
}

static void
udp_resolv_cb(void *arg)
{
	udp_ep   *ep = arg;
	udp_pipe *p;
	nni_aio  *aio;
	int       rv;
	nni_mtx_lock(&ep->mtx);
	if ((aio = nni_list_first(&ep->connaios)) == NULL) {
		nni_mtx_unlock(&ep->mtx);
		return;
	}
	if (ep->closed) {
		nni_aio_list_remove(aio);
		nni_mtx_unlock(&ep->mtx);
		nni_aio_finish_error(aio, NNG_ECLOSED);
		return;
	}
	if ((rv = nni_aio_result(&ep->resaio)) != 0) {
		nni_aio_list_remove(aio);
		nni_mtx_unlock(&ep->mtx);
		nng_log_warn("NNG-UDP-RESOLV",
		    "Failed resolving IP address: %s", nng_strerror(rv));
		nni_aio_finish_error(aio, rv);
		return;
	}

	// Choose the right port to bind to. The family must match.
	if (ep->self_sa.s_family == NNG_AF_UNSPEC) {
		ep->self_sa.s_family = ep->peer_sa.s_family;
	}

	if (ep->udp == NULL) {
		if ((rv = nng_udp_open(&ep->udp, &ep->self_sa)) != NNG_OK) {
			nni_aio_list_remove(aio);
			nni_mtx_unlock(&ep->mtx);
			nni_aio_finish_error(aio, rv);
			return;
		}
	}

	// places a "hold" on the ep
	if ((rv = nni_pipe_alloc_dialer((void **) &p, ep->ndialer)) !=
	    NNG_OK) {
		nni_aio_list_remove(aio);
		nni_mtx_unlock(&ep->mtx);
		nni_aio_finish_error(aio, rv);
		return;
	}
	if ((rv = udp_pipe_start(p, ep, &ep->peer_sa)) != NNG_OK) {
		nni_aio_list_remove(aio);
		nni_pipe_close(p->npipe);
		nni_mtx_unlock(&ep->mtx);
		nni_aio_finish_error(aio, rv);
		return;
	}

	udp_pipe_schedule(p);
	udp_ep_start(ep);

	// Send out the connection request.  We don't complete
	// the user aio until we confirm a connection, so that
	// we can supply details like maximum receive message size
	// and the protocol the peer is using.
	udp_send_creq(ep, p);
	nni_mtx_unlock(&ep->mtx);
}

static void
udp_ep_connect(void *arg, nni_aio *aio)
{
	udp_ep *ep = arg;

	nni_mtx_lock(&ep->mtx);
	if (!nni_aio_start(aio, udp_ep_cancel, ep)) {
		nni_mtx_unlock(&ep->mtx);
		return;
	}
	if (ep->closed) {
		nni_mtx_unlock(&ep->mtx);
		nni_aio_finish_error(aio, NNG_ECLOSED);
		return;
	}
	if (ep->started) {
		nni_mtx_unlock(&ep->mtx);
		nni_aio_finish_error(aio, NNG_EBUSY);
		return;
	}
	NNI_ASSERT(nni_list_empty(&ep->connaios));
	ep->dialer = true;

	nni_list_append(&ep->connaios, aio);

	// lookup the IP address

	memset(&ep->resolv, 0, sizeof(ep->resolv));
	ep->resolv.ri_family  = ep->af;
	ep->resolv.ri_host    = ep->url->u_hostname;
	ep->resolv.ri_port    = ep->url->u_port;
	ep->resolv.ri_passive = false;
	ep->resolv.ri_sa      = &ep->peer_sa;
	nni_aio_set_timeout(&ep->resaio, NNI_SECOND * 5);
	nni_resolv(&ep->resolv, &ep->resaio);

	// wake up for retries
	nni_aio_abort(&ep->timeaio, NNG_EINTR);

	nni_mtx_unlock(&ep->mtx);
}

static nng_err
udp_ep_get_port(void *arg, void *buf, size_t *szp, nni_type t)
{
	udp_ep      *ep = arg;
	nng_sockaddr sa;
	int          port;
	uint8_t     *paddr;

	nni_mtx_lock(&ep->mtx);
	if (ep->udp != NULL) {
		(void) nng_udp_sockname(ep->udp, &sa);
	} else {
		sa = ep->self_sa;
	}
	switch (sa.s_family) {
	case NNG_AF_INET:
		paddr = (void *) &sa.s_in.sa_port;
		break;

	case NNG_AF_INET6:
		paddr = (void *) &sa.s_in6.sa_port;
		break;

	default:
		paddr = NULL;
		break;
	}
	nni_mtx_unlock(&ep->mtx);

	if (paddr == NULL) {
		return (NNG_ESTATE);
	}

	NNI_GET16(paddr, port);
	return (nni_copyout_int(port, buf, szp, t));
}

static nng_err
udp_ep_get_locaddr(void *arg, void *v, size_t *szp, nni_opt_type t)
{
	udp_ep      *ep = arg;
	nng_err      rv;
	nng_sockaddr sa;

	if (ep->udp != NULL) {
		(void) nng_udp_sockname(ep->udp, &sa);
	} else {
		sa = ep->self_sa;
	}

	rv = nni_copyout_sockaddr(&sa, v, szp, t);
	return (rv);
}

static nng_err
udp_ep_get_remaddr(void *arg, void *v, size_t *szp, nni_opt_type t)
{
	udp_ep      *ep = arg;
	nng_err      rv;
	nng_sockaddr sa;

	if (!ep->dialer) {
		return (NNG_ENOTSUP);
	}
	sa = ep->peer_sa;

	rv = nni_copyout_sockaddr(&sa, v, szp, t);
	return (rv);
}

static nng_err
udp_ep_get_recvmaxsz(void *arg, void *v, size_t *szp, nni_opt_type t)
{
	udp_ep *ep = arg;
	nng_err rv;

	nni_mtx_lock(&ep->mtx);
	rv = nni_copyout_size(ep->rcvmax, v, szp, t);
	nni_mtx_unlock(&ep->mtx);
	return (rv);
}

static nng_err
udp_ep_set_recvmaxsz(void *arg, const void *v, size_t sz, nni_opt_type t)
{
	udp_ep *ep = arg;
	size_t  val;
	nng_err rv;
	if ((rv = nni_copyin_size(&val, v, sz, 0, 65000, t)) == NNG_OK) {
		if ((val == 0) || (val > 65000)) {
			val = 65000;
		}
		nni_mtx_lock(&ep->mtx);
		if (ep->started) {
			nni_mtx_unlock(&ep->mtx);
			return (NNG_EBUSY);
		}
		ep->rcvmax = (uint16_t) val;
		nni_mtx_unlock(&ep->mtx);
		nni_stat_set_value(&ep->st_rcv_max, val);
	}
	return (rv);
}

static nng_err
udp_ep_get_copymax(void *arg, void *v, size_t *szp, nni_opt_type t)
{
	udp_ep *ep = arg;
	nng_err rv;

	nni_mtx_lock(&ep->mtx);
	rv = nni_copyout_size(ep->copymax, v, szp, t);
	nni_mtx_unlock(&ep->mtx);
	return (rv);
}

static nng_err
udp_ep_set_copymax(void *arg, const void *v, size_t sz, nni_opt_type t)
{
	udp_ep *ep = arg;
	size_t  val;
	nng_err rv;
	if ((rv = nni_copyin_size(&val, v, sz, 0, 65000, t)) == NNG_OK) {
		nni_mtx_lock(&ep->mtx);
		if (ep->started) {
			nni_mtx_unlock(&ep->mtx);
			return (NNG_EBUSY);
		}
		ep->copymax = (uint16_t) val;
		nni_mtx_unlock(&ep->mtx);
		nni_stat_set_value(&ep->st_copy_max, val);
	}
	return (rv);
}

// this just looks for pipes waiting for an aio, and aios waiting for
// a connection, and matches them together.
static void
udp_ep_match(udp_ep *ep)
{
	nng_aio  *aio = nni_list_first(&ep->connaios);
	udp_pipe *p   = nni_list_first(&ep->connpipes);

	if ((aio == NULL) || (p == NULL)) {
		return;
	}

	p->state = PIPE_CONN_DONE;
	nni_aio_list_remove(aio);
	nni_list_remove(&ep->connpipes, p);
	nni_aio_set_output(aio, 0, p->npipe);
	nni_aio_finish(aio, 0, 0);
}

static void
udp_ep_start(udp_ep *ep)
{
	ep->started = true;
	udp_start_rx(ep);
}

static nng_err
udp_ep_bind(void *arg, nng_url *url)
{
	udp_ep *ep = arg;
	nng_err rv;

	nni_mtx_lock(&ep->mtx);
	if (ep->started) {
		nni_mtx_unlock(&ep->mtx);
		return (NNG_EBUSY);
	}

	rv = nng_udp_open(&ep->udp, &ep->self_sa);
	if (rv != NNG_OK) {
		nni_mtx_unlock(&ep->mtx);
		return (rv);
	}
	nng_sockaddr sa;
	nng_udp_sockname(ep->udp, &sa);
	url->u_port = nng_sockaddr_port(&sa);
	udp_ep_start(ep);
	nni_mtx_unlock(&ep->mtx);

	return (rv);
}

static void
udp_ep_accept(void *arg, nni_aio *aio)
{
	udp_ep *ep = arg;

	nni_aio_reset(aio);
	nni_mtx_lock(&ep->mtx);
	if (ep->closed) {
		nni_mtx_unlock(&ep->mtx);
		nni_aio_finish_error(aio, NNG_ECLOSED);
		return;
	}
	if (!nni_aio_start(aio, udp_ep_cancel, ep)) {
		nni_mtx_unlock(&ep->mtx);
		return;
	}
	nni_aio_list_append(&ep->connaios, aio);
	udp_ep_match(ep);
	nni_mtx_unlock(&ep->mtx);
}

static size_t
udp_pipe_size(void)
{
	return (sizeof(udp_pipe));
}

static nni_sp_pipe_ops udp_pipe_ops = {
	.p_size   = udp_pipe_size,
	.p_init   = udp_pipe_init,
	.p_fini   = udp_pipe_fini,
	.p_stop   = udp_pipe_stop,
	.p_send   = udp_pipe_send,
	.p_recv   = udp_pipe_recv,
	.p_close  = udp_pipe_close,
	.p_peer   = udp_pipe_peer,
	.p_getopt = udp_pipe_getopt,
};

static const nni_option udp_ep_opts[] = {
	{
	    .o_name = NNG_OPT_RECVMAXSZ,
	    .o_get  = udp_ep_get_recvmaxsz,
	    .o_set  = udp_ep_set_recvmaxsz,
	},
	{
	    .o_name = NNG_OPT_UDP_COPY_MAX,
	    .o_get  = udp_ep_get_copymax,
	    .o_set  = udp_ep_set_copymax,
	},
	{
	    .o_name = NNG_OPT_LOCADDR,
	    .o_get  = udp_ep_get_locaddr,
	},
	{
	    .o_name = NNG_OPT_REMADDR,
	    .o_get  = udp_ep_get_remaddr,
	},
	{
	    .o_name = NNG_OPT_TCP_BOUND_PORT,
	    .o_get  = udp_ep_get_port,
	},
	// terminate list
	{
	    .o_name = NULL,
	},
};

static nng_err
udp_dialer_getopt(
    void *arg, const char *name, void *buf, size_t *szp, nni_type t)
{
	udp_ep *ep = arg;

	return (nni_getopt(udp_ep_opts, name, ep, buf, szp, t));
}

static nng_err
udp_dialer_setopt(
    void *arg, const char *name, const void *buf, size_t sz, nni_type t)
{
	udp_ep *ep = arg;

	return (nni_setopt(udp_ep_opts, name, ep, buf, sz, t));
}

static nng_err
udp_listener_getopt(
    void *arg, const char *name, void *buf, size_t *szp, nni_type t)
{
	udp_ep *ep = arg;

	return (nni_getopt(udp_ep_opts, name, ep, buf, szp, t));
}

static nng_err
udp_listener_setopt(
    void *arg, const char *name, const void *buf, size_t sz, nni_type t)
{
	udp_ep *ep = arg;

	return (nni_setopt(udp_ep_opts, name, ep, buf, sz, t));
}

static nni_sp_dialer_ops udp_dialer_ops = {
	.d_size    = sizeof(udp_ep),
	.d_init    = udp_dialer_init,
	.d_fini    = udp_ep_fini,
	.d_connect = udp_ep_connect,
	.d_close   = udp_ep_close,
	.d_stop    = udp_ep_stop,
	.d_getopt  = udp_dialer_getopt,
	.d_setopt  = udp_dialer_setopt,
};

static nni_sp_listener_ops udp_listener_ops = {
	.l_size   = sizeof(udp_ep),
	.l_init   = udp_listener_init,
	.l_fini   = udp_ep_fini,
	.l_bind   = udp_ep_bind,
	.l_accept = udp_ep_accept,
	.l_close  = udp_ep_close,
	.l_stop   = udp_ep_stop,
	.l_getopt = udp_listener_getopt,
	.l_setopt = udp_listener_setopt,
};

static nni_sp_tran udp_tran = {
	.tran_scheme   = "udp",
	.tran_dialer   = &udp_dialer_ops,
	.tran_listener = &udp_listener_ops,
	.tran_pipe     = &udp_pipe_ops,
	.tran_init     = udp_tran_init,
	.tran_fini     = udp_tran_fini,
};

static nni_sp_tran udp4_tran = {
	.tran_scheme   = "udp4",
	.tran_dialer   = &udp_dialer_ops,
	.tran_listener = &udp_listener_ops,
	.tran_pipe     = &udp_pipe_ops,
	.tran_init     = udp_tran_init,
	.tran_fini     = udp_tran_fini,
};

#ifdef NNG_ENABLE_IPV6
static nni_sp_tran udp6_tran = {
	.tran_scheme   = "udp6",
	.tran_dialer   = &udp_dialer_ops,
	.tran_listener = &udp_listener_ops,
	.tran_pipe     = &udp_pipe_ops,
	.tran_init     = udp_tran_init,
	.tran_fini     = udp_tran_fini,
};
#endif

void
nni_sp_udp_register(void)
{
	nni_sp_tran_register(&udp_tran);
	nni_sp_tran_register(&udp4_tran);
#ifdef NNG_ENABLE_IPV6
	nni_sp_tran_register(&udp6_tran);
#endif
}
