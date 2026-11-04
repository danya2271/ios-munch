#include <kern/kalloc.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <sys/errno.h>
#include <sys/hakios.qrtr.mhi.h>
#include <sys/hakios.qrtr.wire.h>

struct hakios_qrtr_port {
	struct hakios_qrtr_mhi *router;
	hakios_qrtr_receive_t receive;
	hakios_qrtr_server_t server;
	void *context;
	bool opened;
	bool closed;
	bool lookup;
	bool notified;
};

struct hakios_qrtr_mhi {
	lck_mtx_t lock;
	lck_mtx_t dispatch_lock;
	struct hakios_qrtr_mhi_config config;
	struct hakios_qrtr_mhi_operations operations;
	struct hakios_qrtr_port port;
	void *owner;
	thread_t callback_thread;
	uint8_t frame[HAKIOS_QRTR_FRAME_MAX];
	uint8_t ack[52];
	size_t ack_size;
	uint32_t peer;
	uint32_t server_port;
	uint32_t pending;
	bool started;
	bool hello;
	bool present;
	bool gone;
	bool faulted;
	bool del_pending;
};

static LCK_GRP_DECLARE(qrtr_mhi_group, "hakios-qrtr-mhi");

static int
router_status(struct hakios_qrtr_mhi *router)
{
	return router->faulted ? EIO : router->gone ? ENOTCONN : router->started && router->hello ? 0 : EAGAIN;
}

static int
router_transmit(struct hakios_qrtr_mhi *router, const void *frame, size_t size)
{
	int error = router->operations.transmit(router->owner, frame, size);
	if (error < 0) {
		error = EIO;
	}
	if (error != 0 && error != EAGAIN && error != ENOSPC) {
		router->faulted = true;
		router->present = false;
	}
	return error;
}

static int
router_control(struct hakios_qrtr_mhi *router, uint32_t type, uint32_t destination)
{
	struct hakios_qrtr_control control = {
		.type = type, .node = router->config.local_node, .port = router->config.local_port,
	};
	struct hakios_qrtr_packet route = {
		.source_node = router->config.local_node, .source_port = HAKIOS_QRTR_CONTROL_PORT,
		.destination_node = destination, .destination_port = HAKIOS_QRTR_CONTROL_PORT,
	};
	size_t size;
	int error = hakios_qrtr_control_encode(&control, &route, router->frame, sizeof(router->frame), &size);
	return error != 0 ? error : router_transmit(router, router->frame, size);
}

static struct hakios_qrtr_port *
router_open(void *owner, hakios_qrtr_receive_t receive, hakios_qrtr_server_t server, void *context)
{
	struct hakios_qrtr_mhi *router = owner;
	struct hakios_qrtr_port *port = NULL;
	if (receive == NULL || server == NULL || context == NULL) {
		return NULL;
	}
	lck_mtx_lock(&router->lock);
	if (!router->port.opened && !router->faulted && !router->gone) {
		router->port = (struct hakios_qrtr_port) {
			.router = router, .receive = receive, .server = server, .context = context, .opened = true,
		};
		port = &router->port;
	}
	lck_mtx_unlock(&router->lock);
	return port;
}

static int
router_send(struct hakios_qrtr_port *port, uint32_t node, uint32_t to_port, const void *data, size_t length)
{
	if (port == NULL || data == NULL || length == 0 || length > HAKIOS_QRTR_FRAME_MAX - 32) {
		return EMSGSIZE;
	}
	struct hakios_qrtr_mhi *router = port->router;
	lck_mtx_lock(&router->lock);
	int error = router_status(router);
	if (port->closed) {
		error = ENOTCONN;
	} else if (error == 0 && (!router->present || node != router->peer || to_port != router->server_port)) {
		error = EHOSTUNREACH;
	} else if (error == 0 && router->pending >= 10) {
		error = EAGAIN;
	}
	if (error == 0) {
		struct hakios_qrtr_packet packet = {
			.type = HAKIOS_QRTR_DATA, .source_node = router->config.local_node, .source_port = router->config.local_port,
			.destination_node = node, .destination_port = to_port, .confirm_rx = router->pending == 4,
			.payload = data, .length = length,
		};
		size_t size;
		error = hakios_qrtr_wire_encode(&packet, router->frame, sizeof(router->frame), &size);
		if (error == 0) {
			error = router_transmit(router, router->frame, size);
		}
		if (error == 0) {
			router->pending++;
		}
	}
	lck_mtx_unlock(&router->lock);
	return error;
}

static int
router_lookup(struct hakios_qrtr_port *port, uint32_t service, uint32_t version, uint32_t instance)
{
	if (port == NULL || service != 0x45 || version != 1 || instance != 1) {
		return ENOTSUP;
	}
	struct hakios_qrtr_mhi *router = port->router;
	lck_mtx_lock(&router->lock);
	int error = router->faulted ? EIO : port->closed || router->gone ? ENOTCONN : 0;
	if (error == 0) {
		port->lookup = true;
		if (router->operations.schedule != NULL) {
			router->operations.schedule(router->owner);
		}
	}
	lck_mtx_unlock(&router->lock);
	return error;
}

static void
router_close(struct hakios_qrtr_port *port)
{
	struct hakios_qrtr_mhi *router = port->router;
	lck_mtx_lock(&router->lock);
	bool in_callback = router->callback_thread == current_thread();
	lck_mtx_unlock(&router->lock);
	if (!in_callback) {
		lck_mtx_lock(&router->dispatch_lock);
	}
	lck_mtx_lock(&router->lock);
	if (!port->closed) {
		port->closed = true;
		router->del_pending = router->hello && !router->gone && !router->faulted;
		if (router->operations.schedule != NULL) {
			router->operations.schedule(router->owner);
		}
	}
	lck_mtx_unlock(&router->lock);
	if (!in_callback) {
		lck_mtx_unlock(&router->dispatch_lock);
	}
}

static const struct hakios_qrtr_ops router_operations = {
	.send = router_send, .lookup = router_lookup, .close = router_close,
};

int
hakios_qrtr_mhi_create(const struct hakios_qrtr_mhi_config *config,
    const struct hakios_qrtr_mhi_operations *operations, void *owner, struct hakios_qrtr_mhi **result)
{
	if (result == NULL) {
		return EINVAL;
	}
	*result = NULL;
	if (config == NULL || operations == NULL || owner == NULL || config->local_node == 0 ||
	    config->local_node >= HAKIOS_QRTR_CONTROL_PORT || config->local_port == 0 ||
	    config->local_port >= HAKIOS_QRTR_CONTROL_PORT || operations->version != HAKIOS_QRTR_MHI_VERSION ||
	    operations->transmit == NULL || operations->quiesce == NULL) {
		return EINVAL;
	}
	struct hakios_qrtr_mhi *router = kalloc_type(struct hakios_qrtr_mhi, Z_WAITOK | Z_ZERO);
	if (router == NULL) {
		return ENOMEM;
	}
	router->config = *config;
	router->operations = *operations;
	router->owner = owner;
	lck_mtx_init(&router->lock, &qrtr_mhi_group, LCK_ATTR_NULL);
	lck_mtx_init(&router->dispatch_lock, &qrtr_mhi_group, LCK_ATTR_NULL);
	*result = router;
	return 0;
}

int
hakios_qrtr_mhi_endpoint(struct hakios_qrtr_mhi *router, struct hakios_qrtr_endpoint *endpoint)
{
	if (endpoint == NULL) {
		return EINVAL;
	}
	bzero(endpoint, sizeof(*endpoint));
	if (router == NULL) {
		return EINVAL;
	}
	*endpoint = (struct hakios_qrtr_endpoint) {.operations = &router_operations, .owner = router, .open = router_open};
	return 0;
}

int
hakios_qrtr_mhi_start(struct hakios_qrtr_mhi *router)
{
	if (router == NULL) {
		return EINVAL;
	}
	lck_mtx_lock(&router->lock);
	int error = router->faulted ? EIO : router->gone ? ENOTCONN : router->started ? EALREADY : 0;
	if (error == 0) {
		error = router_control(router, HAKIOS_QRTR_HELLO, UINT32_MAX);
		if (error == 0) {
			router->started = true;
		}
	}
	lck_mtx_unlock(&router->lock);
	return error;
}

static int
router_enter(struct hakios_qrtr_mhi *router)
{
	lck_mtx_lock(&router->lock);
	bool in_callback = router->callback_thread == current_thread();
	lck_mtx_unlock(&router->lock);
	if (in_callback) {
		return EDEADLK;
	}
	lck_mtx_lock(&router->dispatch_lock);
	lck_mtx_lock(&router->lock);
	return 0;
}

static void
router_notify(struct hakios_qrtr_mhi *router)
{
	struct hakios_qrtr_port *port = &router->port;
	if (port->opened && !port->closed && port->lookup && port->notified != router->present) {
		port->notified = router->present;
		bool present = router->present;
		uint32_t node = router->peer, server = router->server_port;
		router->callback_thread = current_thread();
		lck_mtx_unlock(&router->lock);
		port->server(port->context, present, 0x45, 1, 1, node, server);
		lck_mtx_lock(&router->lock);
		router->callback_thread = THREAD_NULL;
	}
}

static int
router_control_receive(struct hakios_qrtr_mhi *router, const struct hakios_qrtr_packet *packet)
{
	struct hakios_qrtr_control control;
	int error = hakios_qrtr_control_decode(packet, &control);
	if (error != 0) {
		return error;
	}
	if (control.type != HAKIOS_QRTR_RESUME_TX && packet->source_port != HAKIOS_QRTR_CONTROL_PORT) {
		return EPROTO;
	}
	if (control.type == HAKIOS_QRTR_HELLO && !router->hello) {
		error = router_control(router, HAKIOS_QRTR_HELLO, packet->source_node);
		if (error == 0) {
			router->peer = packet->source_node;
			router->hello = true;
		}
		return error;
	}
	if (!router->hello || packet->source_node != router->peer) {
		return EPROTO;
	}
	switch (control.type) {
	case HAKIOS_QRTR_HELLO:
		return 0;
	case HAKIOS_QRTR_NEW_SERVER:
	case HAKIOS_QRTR_DEL_SERVER:
		if (control.node != router->peer || control.port == 0 || control.port >= HAKIOS_QRTR_CONTROL_PORT) {
			return EPROTO;
		}
		if (control.service != 0x45 || control.version != 1 || control.instance != 1) {
			return 0;
		}
		if (control.type == HAKIOS_QRTR_NEW_SERVER) {
			if (router->present && router->server_port != control.port) {
				return EPROTO;
			}
			router->server_port = control.port;
			router->present = true;
		} else if (control.port == router->server_port) {
			router->present = false;
			router->pending = 0;
		}
		return 0;
	case HAKIOS_QRTR_BYE:
		router->present = false;
		router->gone = true;
		return 0;
	case HAKIOS_QRTR_DEL_CLIENT:
		if (control.node != router->peer) {
			return EPROTO;
		}
		if (control.port == router->server_port) {
			router->present = false;
			router->pending = 0;
		}
		return 0;
	case HAKIOS_QRTR_RESUME_TX:
		if (!router->present || control.node != router->peer || control.port != router->server_port ||
		    packet->source_port != router->server_port || packet->destination_port != router->config.local_port ||
		    packet->destination_node != router->config.local_node ||
		    router->pending < 5) {
			return EPROTO;
		}
		router->pending = 0;
		return 0;
	default:
		return ENOTSUP;
	}
}

int
hakios_qrtr_mhi_receive(struct hakios_qrtr_mhi *router, const void *frame, size_t size)
{
	if (router == NULL) {
		return EINVAL;
	}
	int error = router_enter(router);
	if (error != 0) {
		return error;
	}
	struct hakios_qrtr_packet packet;
	if (router->faulted || router->gone || !router->started) {
		error = router_status(router);
		goto out;
	}
	if (router->ack_size != 0) {
		error = EAGAIN;
		goto out;
	}
	error = hakios_qrtr_wire_decode(frame, size, &packet);
	if (error != 0) {
		goto fault;
	}
	if (packet.source_node == 0 || packet.source_node >= HAKIOS_QRTR_CONTROL_PORT ||
	    packet.source_node == router->config.local_node ||
	    (packet.destination_node != router->config.local_node && packet.destination_node != UINT32_MAX)) {
		error = EPROTO;
		goto fault;
	}
	if (packet.type != HAKIOS_QRTR_DATA) {
		error = router_control_receive(router, &packet);
		if (error != 0 && error != EAGAIN && error != ENOSPC) {
			goto fault;
		}
		goto out;
	}
	if (!router->hello || !router->present || packet.source_node != router->peer || packet.source_port != router->server_port ||
	    packet.destination_node != router->config.local_node || packet.destination_port != router->config.local_port) {
		error = EPROTO;
		goto fault;
	}
	if (!router->port.opened || router->port.closed) {
		error = ENOTCONN;
		goto out;
	}
	if (packet.confirm_rx) {
		error = hakios_qrtr_wire_resume(&packet, router->ack, sizeof(router->ack), &router->ack_size);
		if (error != 0) {
			goto fault;
		}
	}
	router->callback_thread = current_thread();
	lck_mtx_unlock(&router->lock);
	router->port.receive(router->port.context, packet.source_node, packet.source_port, packet.payload, packet.length);
	lck_mtx_lock(&router->lock);
	router->callback_thread = THREAD_NULL;
	goto out;
fault:
	router->faulted = true;
	router->present = false;
out:
	router_notify(router);
	lck_mtx_unlock(&router->lock);
	lck_mtx_unlock(&router->dispatch_lock);
	return error;
}

int
hakios_qrtr_mhi_poll(struct hakios_qrtr_mhi *router)
{
	if (router == NULL) {
		return EINVAL;
	}
	int error = router_enter(router);
	if (error != 0) {
		return error;
	}
	error = router_status(router);
	if (error == 0 && router->ack_size != 0) {
		error = router_transmit(router, router->ack, router->ack_size);
		if (error == 0) {
			router->ack_size = 0;
		}
	}
	if (error == 0 && router->del_pending) {
		error = router_control(router, HAKIOS_QRTR_DEL_CLIENT, router->peer);
		if (error == 0) {
			router->del_pending = false;
		}
	}
	router_notify(router);
	lck_mtx_unlock(&router->lock);
	lck_mtx_unlock(&router->dispatch_lock);
	return error;
}

int
hakios_qrtr_mhi_disconnect(struct hakios_qrtr_mhi *router)
{
	if (router == NULL) {
		return EINVAL;
	}
	int error = router_enter(router);
	if (error != 0) {
		return error;
	}
	router->gone = true;
	router->present = false;
	router_notify(router);
	lck_mtx_unlock(&router->lock);
	lck_mtx_unlock(&router->dispatch_lock);
	return 0;
}

int
hakios_qrtr_mhi_status(struct hakios_qrtr_mhi *router)
{
	if (router == NULL) {
		return EINVAL;
	}
	lck_mtx_lock(&router->lock);
	int error = router_status(router);
	lck_mtx_unlock(&router->lock);
	return error;
}

int
hakios_qrtr_mhi_idle(struct hakios_qrtr_mhi *router)
{
	if (router == NULL) {
		return EINVAL;
	}
	lck_mtx_lock(&router->lock);
	int error = router_status(router);
	if (error == 0 && (router->ack_size != 0 || router->del_pending || router->callback_thread != THREAD_NULL ||
	    (router->port.lookup && !router->port.closed && router->present != router->port.notified))) {
		error = EBUSY;
	}
	lck_mtx_unlock(&router->lock);
	return error;
}

int
hakios_qrtr_mhi_destroy(struct hakios_qrtr_mhi **router)
{
	if (router == NULL || *router == NULL) {
		return EINVAL;
	}
	struct hakios_qrtr_mhi *state = *router;
	lck_mtx_lock(&state->lock);
	if ((state->port.opened && !state->port.closed) || state->callback_thread != THREAD_NULL ||
	    (!state->faulted && !state->gone && (state->del_pending || state->ack_size != 0))) {
		lck_mtx_unlock(&state->lock);
		return EBUSY;
	}
	int error = state->operations.quiesce(state->owner);
	if (error != 0) {
		state->faulted = true;
		lck_mtx_unlock(&state->lock);
		return error < 0 ? EIO : error;
	}
	lck_mtx_unlock(&state->lock);
	lck_mtx_destroy(&state->dispatch_lock, &qrtr_mhi_group);
	lck_mtx_destroy(&state->lock, &qrtr_mhi_group);
	kfree_type(struct hakios_qrtr_mhi, state);
	*router = NULL;
	return 0;
}
