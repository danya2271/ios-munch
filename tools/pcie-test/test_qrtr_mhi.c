#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/hakios.qmi.h>
#include <sys/hakios.qrtr.mhi.h>
#include <sys/hakios.qrtr.wire.h>
#include <sys/hakios.wire.h>

bool pcie_test_fail_allocation;
unsigned pcie_test_allocation_calls;
unsigned pcie_test_fail_allocation_at;

struct link_model {
	struct hakios_qrtr_mhi *router;
	struct hakios_qrtr_endpoint endpoint;
	struct hakios_qrtr_port *port;
	uint8_t last[8192];
	size_t last_size;
	unsigned transmitted;
	unsigned received;
	unsigned arrivals;
	unsigned departures;
	int tx_error;
	int quiesce_error;
	bool tx_after;
	bool stopped;
	bool self_close;
	bool block;
	atomic_bool callback_entered;
	atomic_bool release_callback;
	atomic_bool close_started;
	atomic_bool close_returned;
};

static int
link_transmit(void *owner, const void *frame, size_t size)
{
	struct link_model *model = owner;
	assert(size <= sizeof(model->last));
	if (model->tx_error == 0 || model->tx_after) {
		memcpy(model->last, frame, size);
		model->last_size = size;
		model->transmitted++;
	}
	return model->tx_error;
}

static int
link_quiesce(void *owner)
{
	struct link_model *model = owner;
	if (model->quiesce_error == 0) {
		model->stopped = true;
	}
	return model->quiesce_error;
}

static void
app_receive(void *context, uint32_t node, uint32_t port, const void *data, size_t length)
{
	struct link_model *model = context;
	assert(node == 7 && port == 42 && length == 3 && memcmp(data, "qmi", 3) == 0);
	model->received++;
	assert(hakios_qrtr_mhi_poll(model->router) == EDEADLK);
	if (model->self_close) {
		model->endpoint.operations->close(model->port);
	}
	if (model->block) {
		atomic_store(&model->callback_entered, true);
		while (!atomic_load(&model->release_callback)) {
			sched_yield();
		}
	}
}

static void
app_server(void *context, bool arrived, uint32_t service, uint32_t version, uint32_t instance, uint32_t node, uint32_t port)
{
	struct link_model *model = context;
	assert(service == 0x45 && version == 1 && instance == 1 && node == 7 && port == 42);
	if (arrived) {
		model->arrivals++;
	} else {
		model->departures++;
	}
	assert(hakios_qrtr_mhi_poll(model->router) == EDEADLK);
}

static void
link_init(struct link_model *model, bool open_port)
{
	memset(model, 0, sizeof(*model));
	atomic_init(&model->callback_entered, false);
	atomic_init(&model->release_callback, false);
	atomic_init(&model->close_started, false);
	atomic_init(&model->close_returned, false);
	struct hakios_qrtr_mhi_config config = {.local_node = 1, .local_port = 0x4000};
	struct hakios_qrtr_mhi_operations operations = {
		.version = HAKIOS_QRTR_MHI_VERSION, .transmit = link_transmit, .quiesce = link_quiesce,
	};
	assert(hakios_qrtr_mhi_create(&config, &operations, model, &model->router) == 0);
	assert(hakios_qrtr_mhi_endpoint(model->router, &model->endpoint) == 0);
	assert(hakios_qrtr_mhi_status(model->router) == EAGAIN);
	if (open_port) {
		model->port = model->endpoint.open(model->endpoint.owner, app_receive, app_server, model);
		assert(model->port != NULL);
		assert(model->endpoint.open(model->endpoint.owner, app_receive, app_server, model) == NULL);
	}
}

static int
peer_control(struct link_model *model, uint32_t type, uint32_t service, uint32_t node, uint32_t port)
{
	uint8_t frame[52];
	size_t size;
	struct hakios_qrtr_packet route = {
		.source_node = 7, .source_port = type == HAKIOS_QRTR_RESUME_TX ? 42 : HAKIOS_QRTR_CONTROL_PORT,
		.destination_node = 1, .destination_port = type == HAKIOS_QRTR_RESUME_TX ? 0x4000 : HAKIOS_QRTR_CONTROL_PORT,
	};
	struct hakios_qrtr_control control = {.type = type, .service = service, .version = 1, .instance = 1, .node = node, .port = port};
	assert(hakios_qrtr_control_encode(&control, &route, frame, sizeof(frame), &size) == 0);
	return hakios_qrtr_mhi_receive(model->router, frame, size);
}

static int
peer_data(struct link_model *model, const void *bytes, size_t length, bool confirm)
{
	uint8_t frame[8192];
	size_t size;
	struct hakios_qrtr_packet packet = {
		.type = HAKIOS_QRTR_DATA, .source_node = 7, .source_port = 42,
		.destination_node = 1, .destination_port = 0x4000, .payload = bytes, .length = length, .confirm_rx = confirm,
	};
	assert(hakios_qrtr_wire_encode(&packet, frame, sizeof(frame), &size) == 0);
	return hakios_qrtr_mhi_receive(model->router, frame, size);
}

static void
link_connect(struct link_model *model)
{
	assert(hakios_qrtr_mhi_start(model->router) == 0);
	assert(hakios_qrtr_mhi_start(model->router) == EALREADY);
	assert(peer_control(model, HAKIOS_QRTR_HELLO, 0, 0, 0) == 0);
	assert(hakios_qrtr_mhi_status(model->router) == 0);
	assert(peer_control(model, HAKIOS_QRTR_NEW_SERVER, 0x45, 7, 42) == 0);
}

static void
link_finish(struct link_model *model)
{
	if (model->port != NULL) {
		model->endpoint.operations->close(model->port);
	}
	model->tx_error = 0;
	int error = hakios_qrtr_mhi_poll(model->router);
	assert(error == 0 || error == EIO || error == ENOTCONN || error == EAGAIN);
	assert(hakios_qrtr_mhi_destroy(&model->router) == 0 && model->stopped);
}

static void
test_flow(void)
{
	struct link_model model;
	link_init(&model, true);
	assert(model.endpoint.operations->lookup(model.port, 0x45, 1, 2) == ENOTSUP);
	assert(model.endpoint.operations->lookup(model.port, 0x45, 1, 1) == 0);
	assert(model.endpoint.operations->send(model.port, 7, 42, "qmi", 3) == EAGAIN);
	link_connect(&model);
	assert(model.arrivals == 1 && model.departures == 0);
	assert(peer_control(&model, HAKIOS_QRTR_HELLO, 0, 0, 0) == 0 && model.transmitted == 2);
	assert(peer_control(&model, HAKIOS_QRTR_NEW_SERVER, 0x45, 7, 42) == 0 && model.arrivals == 1);
	for (unsigned round = 0; round < 3; round++) {
		for (unsigned index = 0; index < 10; index++) {
			assert(model.endpoint.operations->send(model.port, 7, 42, "qmi", 3) == 0);
			struct hakios_qrtr_packet packet;
			assert(hakios_qrtr_wire_decode(model.last, model.last_size, &packet) == 0);
			assert(packet.confirm_rx == (index == 4) && packet.source_port == 0x4000 && packet.destination_port == 42);
		}
		unsigned transmitted = model.transmitted;
		assert(model.endpoint.operations->send(model.port, 7, 42, "qmi", 3) == EAGAIN && model.transmitted == transmitted);
		assert(peer_control(&model, HAKIOS_QRTR_RESUME_TX, 0, 7, 42) == 0);
	}
	assert(model.endpoint.operations->send(model.port, 8, 42, "qmi", 3) == EHOSTUNREACH);
	assert(peer_data(&model, "qmi", 3, true) == 0 && model.received == 1);
	unsigned before = model.transmitted;
	model.tx_error = EAGAIN;
	assert(hakios_qrtr_mhi_poll(model.router) == EAGAIN && model.transmitted == before);
	assert(peer_data(&model, "qmi", 3, false) == EAGAIN && model.received == 1);
	model.tx_error = 0;
	assert(hakios_qrtr_mhi_poll(model.router) == 0);
	struct hakios_qrtr_packet packet;
	struct hakios_qrtr_control control;
	assert(hakios_qrtr_wire_decode(model.last, model.last_size, &packet) == 0);
	assert(hakios_qrtr_control_decode(&packet, &control) == 0);
	assert(control.type == HAKIOS_QRTR_RESUME_TX && control.node == 1 && control.port == 0x4000);
	assert(peer_data(&model, "qmi", 3, false) == 0 && model.received == 2);
	assert(peer_control(&model, HAKIOS_QRTR_DEL_SERVER, 0x45, 7, 42) == 0 && model.departures == 1);
	assert(model.endpoint.operations->send(model.port, 7, 42, "qmi", 3) == EHOSTUNREACH);
	assert(peer_control(&model, HAKIOS_QRTR_NEW_SERVER, 0x45, 7, 42) == 0 && model.arrivals == 2);
	assert(peer_control(&model, HAKIOS_QRTR_BYE, 0, 0, 0) == 0 && model.departures == 2);
	assert(hakios_qrtr_mhi_status(model.router) == ENOTCONN);
	link_finish(&model);
}

static void
test_late_lookup_and_errors(void)
{
	struct link_model model;
	link_init(&model, true);
	model.tx_error = ENOSPC;
	assert(hakios_qrtr_mhi_start(model.router) == ENOSPC && model.transmitted == 0);
	model.tx_error = 0;
	link_connect(&model);
	assert(model.arrivals == 0);
	assert(model.endpoint.operations->lookup(model.port, 0x45, 1, 1) == 0);
	assert(hakios_qrtr_mhi_poll(model.router) == 0 && model.arrivals == 1);
	assert(model.endpoint.operations->lookup(model.port, 0x45, 1, 1) == 0);
	assert(hakios_qrtr_mhi_poll(model.router) == 0 && model.arrivals == 1);
	assert(hakios_qrtr_mhi_destroy(&model.router) == EBUSY);
	model.tx_error = EAGAIN;
	assert(model.endpoint.operations->send(model.port, 7, 42, "qmi", 3) == EAGAIN);
	model.tx_error = 0;
	assert(model.endpoint.operations->send(model.port, 7, 42, "qmi", 3) == 0);
	assert(peer_control(&model, HAKIOS_QRTR_RESUME_TX, 0, 7, 42) == EPROTO);
	assert(hakios_qrtr_mhi_status(model.router) == EIO && model.departures == 1);
	link_finish(&model);
	for (unsigned variant = 0; variant < 4; variant++) {
		link_init(&model, true);
		link_connect(&model);
		model.tx_error = variant & 1 ? -ETIMEDOUT : ETIMEDOUT;
		model.tx_after = (variant & 2) != 0;
		int expected = model.tx_error < 0 ? EIO : model.tx_error;
		assert(model.endpoint.operations->send(model.port, 7, 42, "qmi", 3) == expected);
		assert(hakios_qrtr_mhi_status(model.router) == EIO);
		unsigned transmitted = model.transmitted;
		assert(model.endpoint.operations->send(model.port, 7, 42, "qmi", 3) == EIO && model.transmitted == transmitted);
		model.endpoint.operations->close(model.port);
		model.quiesce_error = -EBUSY;
		assert(hakios_qrtr_mhi_destroy(&model.router) == EIO && model.router != NULL);
		model.quiesce_error = 0;
		link_finish(&model);
	}
	link_init(&model, true);
	link_connect(&model);
	uint8_t bad[32] = {0};
	assert(hakios_qrtr_mhi_receive(model.router, bad, sizeof(bad)) != 0);
	assert(hakios_qrtr_mhi_status(model.router) == EIO);
	link_finish(&model);
	link_init(&model, true);
	link_connect(&model);
	assert(model.endpoint.operations->lookup(model.port, 0x45, 1, 1) == 0);
	assert(hakios_qrtr_mhi_poll(model.router) == 0 && model.arrivals == 1);
	assert(peer_data(&model, "qmi", 3, true) == 0);
	assert(hakios_qrtr_mhi_disconnect(model.router) == 0 && model.departures == 1);
	assert(hakios_qrtr_mhi_disconnect(model.router) == 0 && model.departures == 1);
	assert(hakios_qrtr_mhi_status(model.router) == ENOTCONN);
	assert(peer_data(&model, "qmi", 3, false) == ENOTCONN);
	link_finish(&model);
}

static void *
receive_thread(void *context)
{
	struct link_model *model = context;
	assert(peer_data(model, "qmi", 3, false) == 0);
	return NULL;
}

static void *
close_thread(void *context)
{
	struct link_model *model = context;
	atomic_store(&model->close_started, true);
	model->endpoint.operations->close(model->port);
	atomic_store(&model->close_returned, true);
	return NULL;
}

static void
test_close(void)
{
	struct link_model model;
	link_init(&model, true);
	link_connect(&model);
	model.self_close = true;
	assert(peer_data(&model, "qmi", 3, true) == 0 && model.received == 1);
	assert(hakios_qrtr_mhi_destroy(&model.router) == EBUSY);
	assert(hakios_qrtr_mhi_poll(model.router) == 0);
	assert(peer_data(&model, "qmi", 3, false) == ENOTCONN && model.received == 1);
	assert(model.endpoint.open(model.endpoint.owner, app_receive, app_server, &model) == NULL);
	link_finish(&model);
	link_init(&model, true);
	link_connect(&model);
	model.block = true;
	pthread_t receive, close;
	assert(pthread_create(&receive, NULL, receive_thread, &model) == 0);
	while (!atomic_load(&model.callback_entered)) {
		sched_yield();
	}
	assert(pthread_create(&close, NULL, close_thread, &model) == 0);
	while (!atomic_load(&model.close_started)) {
		sched_yield();
	}
	assert(!atomic_load(&model.close_returned));
	assert(hakios_qrtr_mhi_destroy(&model.router) == EBUSY);
	atomic_store(&model.release_callback, true);
	assert(pthread_join(receive, NULL) == 0 && pthread_join(close, NULL) == 0);
	assert(atomic_load(&model.close_returned));
	assert(peer_data(&model, "qmi", 3, false) == ENOTCONN && model.received == 1);
	link_finish(&model);
}

struct qmi_client {
	struct qmi_handle handle;
	unsigned arrivals;
	unsigned departures;
	struct sockaddr_qrtr server;
};

static int
qmi_new_server(struct qmi_handle *handle, struct qmi_service *service)
{
	struct qmi_client *client = (struct qmi_client *)handle;
	client->arrivals++;
	client->server = (struct sockaddr_qrtr) {.sq_family = AF_QIPCRTR, .sq_node = service->node, .sq_port = service->port};
	return 0;
}

static void
qmi_del_server(struct qmi_handle *handle, struct qmi_service *service)
{
	struct qmi_client *client = (struct qmi_client *)handle;
	assert(service->service == 0x45);
	client->departures++;
}

static void
test_qmi(void)
{
	struct link_model model;
	link_init(&model, false);
	struct qmi_client client = {0};
	struct qmi_ops callbacks = {.new_server = qmi_new_server, .del_server = qmi_del_server};
	assert(qmi_handle_init(&client.handle, 128, &callbacks, NULL) == -ENXIO);
	assert(hakios_qmi_handle_init_transport(&client.handle, 128, &callbacks, NULL, model.endpoint.operations) == -EINVAL);
	assert(hakios_qmi_handle_init_endpoint(&client.handle, 128, &callbacks, NULL, &model.endpoint) == 0);
	assert(qmi_add_lookup(&client.handle, 0x45, 1, 1) == 0);
	link_connect(&model);
	assert(client.arrivals == 1);
	const struct qmi_elem_info number_ei[] = {
		{.data_type = QMI_UNSIGNED_4_BYTE, .elem_len = 1, .elem_size = 4, .array_type = NO_ARRAY, .tlv_type = 0x10},
		{.data_type = QMI_EOTI},
	};
	uint32_t request = 17, response = 34, decoded = 0;
	struct qmi_txn transaction;
	assert(qmi_txn_init(&client.handle, &transaction, number_ei, &decoded) >= 0);
	assert(qmi_send_request(&client.handle, &client.server, &transaction, 0x20, 7, number_ei, &request) == 0);
	struct hakios_qrtr_packet packet;
	assert(hakios_qrtr_wire_decode(model.last, model.last_size, &packet) == 0);
	assert(packet.type == HAKIOS_QRTR_DATA && packet.length == 14 && packet.payload[0] == QMI_REQUEST);
	uint8_t reply[32];
	int length = hakios_qmi_encode(QMI_RESPONSE, 0x20, transaction.id, number_ei, &response, reply, sizeof(reply));
	assert(length == 14);
	assert(peer_data(&model, reply, length, true) == 0);
	assert(hakios_qmi_txn_wait_ms(&transaction, 50) >= 0 && decoded == 34);
	assert(hakios_qrtr_mhi_poll(model.router) == 0);
	assert(peer_control(&model, HAKIOS_QRTR_DEL_SERVER, 0x45, 7, 42) == 0 && client.departures == 1);
	qmi_handle_release(&client.handle);
	assert(client.handle.sock == NULL);
	link_finish(&model);
	assert(qmi_handle_init(&client.handle, 128, &callbacks, NULL) == -ENXIO);
}

static void
test_hello_retry_and_validation(void)
{
	struct link_model model;
	link_init(&model, false);
	assert(hakios_qrtr_mhi_disconnect(model.router) == 0);
	assert(hakios_qrtr_mhi_start(model.router) == ENOTCONN && model.transmitted == 0);
	link_finish(&model);
	link_init(&model, true);
	assert(hakios_qrtr_mhi_start(model.router) == 0);
	model.tx_error = EAGAIN;
	assert(peer_control(&model, HAKIOS_QRTR_HELLO, 0, 0, 0) == EAGAIN);
	assert(hakios_qrtr_mhi_status(model.router) == EAGAIN);
	model.tx_error = 0;
	assert(peer_control(&model, HAKIOS_QRTR_HELLO, 0, 0, 0) == 0);
	assert(hakios_qrtr_mhi_status(model.router) == 0);
	assert(peer_control(&model, HAKIOS_QRTR_NEW_SERVER, 0x45, 8, 42) == EPROTO);
	assert(hakios_qrtr_mhi_status(model.router) == EIO);
	link_finish(&model);
	struct hakios_qrtr_mhi_config config = {.local_node = 1, .local_port = 0x4000};
	struct hakios_qrtr_mhi_operations operations = {
		.version = HAKIOS_QRTR_MHI_VERSION, .transmit = link_transmit, .quiesce = link_quiesce,
	};
	pcie_test_fail_allocation = true;
	assert(hakios_qrtr_mhi_create(&config, &operations, &model, &model.router) == ENOMEM && model.router == NULL);
	pcie_test_fail_allocation = false;
	for (unsigned variant = 0; variant < 7; variant++) {
		struct hakios_qrtr_mhi_config bad_config = config;
		struct hakios_qrtr_mhi_operations bad_ops = operations;
		switch (variant) {
		case 0: bad_config.local_node = 0; break;
		case 1: bad_config.local_node = UINT32_MAX; break;
		case 2: bad_config.local_port = 0; break;
		case 3: bad_config.local_port = HAKIOS_QRTR_CONTROL_PORT; break;
		case 4: bad_ops.version++; break;
		case 5: bad_ops.transmit = NULL; break;
		case 6: bad_ops.quiesce = NULL; break;
		}
		assert(hakios_qrtr_mhi_create(&bad_config, &bad_ops, &model, &model.router) == EINVAL && model.router == NULL);
	}
}

int
main(void)
{
	test_flow();
	test_late_lookup_and_errors();
	test_close();
	test_qmi();
	test_hello_retry_and_validation();
	puts("qrtr-mhi-test: PASS (HELLO, WLFW lookup, flow control, deferred ACK, real QMI exchange, callback/close lifetime)");
	return 0;
}
