#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/hakios.ipcr.h>
#include <sys/hakios.qrtr.wire.h>
#include <sys/hakios.wire.h>

bool pcie_test_fail_allocation;
unsigned pcie_test_allocation_calls;
unsigned pcie_test_fail_allocation_at;

int test_pci_qmi_init(struct qmi_handle *qmi, size_t largest, const struct qmi_ops *operations);
int test_pci_qmi_send(struct qmi_handle *qmi, struct qmi_txn *transaction,
    const struct qmi_elem_info *elements, const void *data);

struct bridge_model {
	struct hakios_ipcr *bridge;
	struct hakios_ipcr_config config;
	struct hakios_ipcr_buffer buffers[2][4];
	_Alignas(16) uint8_t rings[2][1024];
	uint8_t device_rings[2][1024];
	uint8_t bytes[2][4][8192];
	uint8_t device_bytes[2][4][8192];
	uint32_t read[2];
	uint32_t write[2];
	struct hakios_qrtr_endpoint endpoint;
	struct hakios_qrtr_port *port;
	struct qmi_handle qmi;
	atomic_uint scheduled;
	unsigned deliveries;
	unsigned servers;
	unsigned departures;
	unsigned calls;
	unsigned fail_at;
	int failure;
	int quiesce_error;
	bool after;
	bool stopped;
	bool send_in_receive;
	bool block;
	atomic_bool entered;
	atomic_bool release;
};

static int
model_call(struct bridge_model *model)
{
	model->calls++;
	return model->calls == model->fail_at ? model->failure : 0;
}

static int
model_buffer(void *owner, uint8_t channel, uint32_t index, bool for_device, uint32_t length)
{
	struct bridge_model *model = owner;
	assert((channel == 20 || channel == 21) && index < 4 && length <= 8192 && !model->stopped);
	int error = model_call(model);
	if (error == 0 || model->after) {
		uint8_t *cpu = model->bytes[channel - 20][index];
		uint8_t *device = model->device_bytes[channel - 20][index];
		memcpy(for_device ? device : cpu, for_device ? cpu : device, length);
	}
	return error;
}

static int
model_ring(void *owner, uint8_t channel, uint32_t offset, uint32_t length)
{
	struct bridge_model *model = owner;
	assert((channel == 20 || channel == 21) && offset < 1024 && length == 16 && !model->stopped);
	int error = model_call(model);
	if (error == 0 || model->after) {
		memcpy(model->device_rings[channel - 20] + offset, model->rings[channel - 20] + offset, length);
	}
	return error;
}

static int
model_publish(void *owner, uint8_t channel, uint64_t next_wp)
{
	struct bridge_model *model = owner;
	assert((channel == 20 || channel == 21) && !model->stopped);
	uint32_t direction = channel - 20;
	uint64_t base = direction == 0 ? model->config.tx_ring.iova : model->config.rx_ring.iova;
	uint32_t next = (model->write[direction] + 1) % 64;
	assert(next_wp == base + next * 16);
	int error = model_call(model);
	if (error == 0 || model->after) {
		model->write[direction] = next;
	}
	return error;
}

static void
model_schedule(void *owner)
{
	struct bridge_model *model = owner;
	atomic_fetch_add(&model->scheduled, 1);
}

static int
model_quiesce(void *owner)
{
	struct bridge_model *model = owner;
	if (model->quiesce_error == 0) {
		model->stopped = true;
	}
	return model->quiesce_error;
}

static const struct hakios_ipcr_operations operations = {
	.version = HAKIOS_IPCR_VERSION, .sync_buffer = model_buffer, .sync_ring = model_ring,
	.publish = model_publish, .schedule = model_schedule, .quiesce = model_quiesce,
};

static void
app_receive(void *context, uint32_t node, uint32_t port, const void *data, size_t size)
{
	struct bridge_model *model = context;
	assert(node == 7 && port == 42 && size == 3 && memcmp(data, "qmi", 3) == 0);
	uint32_t received;
	assert(hakios_ipcr_work(model->bridge, 4, &received) == EBUSY);
	assert(hakios_ipcr_stop(model->bridge) == EBUSY);
	if (model->send_in_receive) {
		assert(model->endpoint.operations->send(model->port, 7, 42, data, size) == 0);
	}
	model->deliveries++;
	if (model->block) {
		atomic_store(&model->entered, true);
		while (!atomic_load(&model->release)) {
			sched_yield();
		}
	}
}

static void
app_server(void *context, bool arrived, uint32_t service, uint32_t version, uint32_t instance,
    uint32_t node, uint32_t port)
{
	struct bridge_model *model = context;
	assert(service == 0x45 && version == 1 && instance == 1 && node == 7 && port == 42);
	if (arrived) {
		model->servers++;
	} else {
		model->departures++;
	}
}

static struct bridge_model *
model_new(void)
{
	struct bridge_model *model = calloc(1, sizeof(*model));
	assert(model != NULL);
	atomic_init(&model->scheduled, 0);
	atomic_init(&model->entered, false);
	atomic_init(&model->release, false);
	for (uint32_t direction = 0; direction < 2; direction++) {
		for (uint32_t index = 0; index < 4; index++) {
			model->buffers[direction][index] = (struct hakios_ipcr_buffer) {
				.bytes = model->bytes[direction][index], .iova = 0x200000 + direction * 0x10000 + index * 8192,
			};
		}
	}
	model->config = (struct hakios_ipcr_config) {
		.epoch = 17, .tx_ring = {.elements = model->rings[0], .iova = 0x100000, .channel = 20},
		.rx_ring = {.elements = model->rings[1], .iova = 0x101000, .channel = 21},
		.tx_buffers = model->buffers[0], .rx_buffers = model->buffers[1], .tx_count = 4, .rx_count = 4,
		.router = {.local_node = 1, .local_port = 0x4000},
	};
	return model;
}

static void
model_create(struct bridge_model *model, bool open_port)
{
	assert(hakios_ipcr_create(&model->config, &operations, model, &model->bridge) == 0);
	assert(hakios_ipcr_endpoint(model->bridge, &model->endpoint) == 0);
	if (open_port) {
		model->port = model->endpoint.open(model->endpoint.owner, app_receive, app_server, model);
		assert(model->port != NULL);
	}
}

static void
model_free(struct bridge_model *model)
{
	if (model->port != NULL) {
		model->endpoint.operations->close(model->port);
	}
	if (model->qmi.sock != NULL) {
		qmi_handle_release(&model->qmi);
	}
	int error = hakios_ipcr_qmi_unbind(model->bridge, &model->qmi);
	assert(error == 0 || error == ENOENT);
	model->quiesce_error = 0;
	assert(hakios_ipcr_destroy(&model->bridge) == 0 && model->bridge == NULL);
	free(model);
}

static int
model_work(struct bridge_model *model, uint32_t *received)
{
	return hakios_ipcr_work(model->bridge, 4, received);
}

static int
model_complete(struct bridge_model *model, uint32_t direction, uint32_t count, uint32_t length)
{
	uint8_t event[16] = {0};
	uint32_t index = (model->read[direction] + count - 1) % 64;
	uint64_t base = direction == 0 ? model->config.tx_ring.iova : model->config.rx_ring.iova;
	hakios_wire_put64(event, base + index * 16);
	hakios_wire_put32(event + 8, 2u << 24 | length);
	hakios_wire_put32(event + 12, (direction + 20) << 24 | 0x22u << 16);
	assert(hakios_ipcr_complete(model->bridge, 16, event) == ESTALE);
	int error = hakios_ipcr_complete(model->bridge, 17, event);
	if (error == 0) {
		model->read[direction] = (index + 1) % 64;
	}
	return error;
}

static void
model_tx_drain(struct bridge_model *model)
{
	uint32_t count = (model->write[0] + 64 - model->read[0]) % 64;
	if (count != 0) {
		assert(model_complete(model, 0, count, 0) == 0);
	}
}

static void
model_receive(struct bridge_model *model, const void *frame, uint32_t size)
{
	assert(model->read[1] != model->write[1] && size <= 8192);
	uint64_t iova = hakios_wire_get64(model->device_rings[1] + model->read[1] * 16);
	uint32_t slot = (uint32_t)(iova - model->buffers[1][0].iova) / 8192;
	assert(slot < 4);
	memcpy(model->device_bytes[1][slot], frame, size);
	unsigned delivered = model->deliveries;
	assert(model_complete(model, 1, 1, size) == 0);
	assert(model->deliveries == delivered);
}

static void
peer_control(struct bridge_model *model, uint32_t type)
{
	uint8_t frame[52];
	size_t size;
	struct hakios_qrtr_packet route = {
		.source_node = 7, .source_port = HAKIOS_QRTR_CONTROL_PORT,
		.destination_node = 1, .destination_port = HAKIOS_QRTR_CONTROL_PORT,
	};
	struct hakios_qrtr_control control = {.type = type, .service = 0x45, .version = 1, .instance = 1, .node = 7, .port = 42};
	assert(hakios_qrtr_control_encode(&control, &route, frame, sizeof(frame), &size) == 0);
	model_receive(model, frame, (uint32_t)size);
}

static void
peer_data(struct bridge_model *model, const void *data, size_t length, bool confirm)
{
	uint8_t frame[8192];
	size_t size;
	struct hakios_qrtr_packet packet = {
		.type = HAKIOS_QRTR_DATA, .source_node = 7, .source_port = 42,
		.destination_node = 1, .destination_port = 0x4000, .payload = data, .length = length, .confirm_rx = confirm,
	};
	assert(hakios_qrtr_wire_encode(&packet, frame, sizeof(frame), &size) == 0);
	model_receive(model, frame, (uint32_t)size);
}

static void
model_connect(struct bridge_model *model)
{
	uint32_t received;
	assert(hakios_ipcr_start(model->bridge) == 0);
	assert(model->calls == 0 && atomic_load(&model->scheduled) != 0);
	assert(model_work(model, &received) == EAGAIN && received == 0);
	model_tx_drain(model);
	peer_control(model, HAKIOS_QRTR_HELLO);
	assert(model_work(model, &received) == 0 && received == 1);
	model_tx_drain(model);
	peer_control(model, HAKIOS_QRTR_NEW_SERVER);
	assert(model_work(model, &received) == 0 && received == 1);
}

static void
test_flow(void)
{
	struct bridge_model *model = model_new();
	model_create(model, true);
	model_connect(model);
	assert(model->servers == 0);
	unsigned scheduled = atomic_load(&model->scheduled);
	assert(model->endpoint.operations->lookup(model->port, 0x45, 1, 1) == 0);
	assert(atomic_load(&model->scheduled) > scheduled);
	assert(hakios_ipcr_pause(model->bridge) == EBUSY);
	uint32_t received;
	assert(model_work(model, &received) == 0 && model->servers == 1);
	uint8_t scratch[3] = {'q', 'm', 'i'};
	for (uint32_t index = 0; index < 4; index++) {
		assert(model->endpoint.operations->send(model->port, 7, 42, scratch, sizeof(scratch)) == 0);
	}
	memset(scratch, 0xff, sizeof(scratch));
	assert(memcmp(model->device_bytes[0][0] + 32, "qmi", 3) == 0);
	assert(model->endpoint.operations->send(model->port, 7, 42, "qmi", 3) == EAGAIN);
	assert(hakios_ipcr_pause(model->bridge) == EBUSY);
	peer_data(model, "qmi", 3, true);
	peer_data(model, "qmi", 3, false);
	assert(model_work(model, &received) == EAGAIN && received == 1 && model->deliveries == 1);
	struct hakios_ipcr_status status;
	assert(hakios_ipcr_status(model->bridge, &status) == 0 && status.rx_ready == 1 && status.tx_pending == 4);
	model_tx_drain(model);
	model->send_in_receive = true;
	assert(model_work(model, &received) == 0 && received == 1 && model->deliveries == 2);
	model->send_in_receive = false;
	model_tx_drain(model);
	assert(hakios_ipcr_pause(model->bridge) == 0);
	unsigned calls = model->calls;
	assert(model->endpoint.operations->send(model->port, 7, 42, "qmi", 3) == EAGAIN);
	assert(model_work(model, &received) == EAGAIN && calls == model->calls);
	peer_data(model, "qmi", 3, false);
	assert(hakios_ipcr_resume(model->bridge) == 0);
	assert(model_work(model, &received) == 0 && received == 1);
	for (uint32_t index = 0; index < 200; index++) {
		peer_data(model, "qmi", 3, false);
		assert(model_work(model, &received) == 0 && received == 1);
	}
	model->quiesce_error = EBUSY;
	assert(hakios_ipcr_stop(model->bridge) == EBUSY);
	assert(model->departures == 1);
	assert(hakios_ipcr_status(model->bridge, &status) == 0 && !status.quiesced && status.stopped);
	assert(hakios_ipcr_destroy(&model->bridge) == EBUSY && model->bridge != NULL);
	model_free(model);
}

static int
qmi_server(struct qmi_handle *qmi, struct qmi_service *service)
{
	struct bridge_model *model = (void *)((uint8_t *)qmi - offsetof(struct bridge_model, qmi));
	assert(service->service == 0x45 && service->version == 1 && service->instance == 1);
	model->servers++;
	return hakios_qmi_connect(qmi->sock, service->node, service->port);
}

static void
test_qmi(void)
{
	struct bridge_model *model = model_new();
	model_create(model, false);
	hakios_qmi_set_transport(model->endpoint.operations);
	assert(test_pci_qmi_init(&model->qmi, 128, NULL) == -ENXIO);
	assert(hakios_ipcr_qmi_bind(model->bridge, &model->qmi) == 0);
	assert(hakios_ipcr_qmi_bind(model->bridge, &model->qmi) == EBUSY);
	const struct qmi_ops qmi_operations = {.new_server = qmi_server};
	assert(test_pci_qmi_init(&model->qmi, 128, &qmi_operations) == 0);
	assert(test_pci_qmi_init(&model->qmi, 128, NULL) == -EBUSY);
	assert(hakios_ipcr_qmi_unbind(model->bridge, &model->qmi) == EBUSY);
	assert(hakios_ipcr_destroy(&model->bridge) == EBUSY);
	assert(qmi_add_lookup(&model->qmi, 0x45, 1, 1) == 0);
	model_connect(model);
	assert(model->servers == 1);
	const struct qmi_elem_info elements[] = {{.data_type = QMI_EOTI}};
	struct qmi_txn transaction;
	uint8_t data = 0, response[32];
	assert(qmi_txn_init(&model->qmi, &transaction, elements, &data) >= 0);
	assert(test_pci_qmi_send(&model->qmi, &transaction, elements, &data) == 0);
	struct hakios_qrtr_packet packet;
	uint32_t size = hakios_wire_get32(model->device_rings[0] + model->read[0] * 16 + 8);
	assert(hakios_qrtr_wire_decode(model->device_bytes[0][0], size, &packet) == 0);
	assert(packet.type == HAKIOS_QRTR_DATA && packet.length == 7 && packet.payload[0] == QMI_REQUEST);
	int length = hakios_qmi_encode(QMI_RESPONSE, 0x55, transaction.id, elements, &data, response, sizeof(response));
	assert(length == 7);
	peer_data(model, response, (size_t)length, false);
	uint32_t received;
	assert(model_work(model, &received) == 0 && received == 1);
	assert(hakios_qmi_txn_wait_ms(&transaction, 1) >= 0 && transaction.done);
	model_tx_drain(model);
	qmi_handle_release(&model->qmi);
	assert(hakios_ipcr_qmi_unbind(model->bridge, &model->qmi) == 0);
	assert(test_pci_qmi_init(&model->qmi, 128, NULL) == -ENXIO);
	hakios_qmi_set_transport(NULL);
	model_free(model);
}

static void *
worker_thread(void *owner)
{
	uint32_t received;
	assert(model_work(owner, &received) == 0 && received == 1);
	return NULL;
}

static void
test_concurrency(void)
{
	struct bridge_model *model = model_new();
	model_create(model, true);
	model_connect(model);
	peer_data(model, "qmi", 3, false);
	model->block = true;
	pthread_t worker;
	assert(pthread_create(&worker, NULL, worker_thread, model) == 0);
	while (!atomic_load(&model->entered)) {
		sched_yield();
	}
	assert(hakios_ipcr_pause(model->bridge) == EBUSY);
	assert(hakios_ipcr_stop(model->bridge) == EBUSY);
	assert(hakios_ipcr_destroy(&model->bridge) == EBUSY);
	assert(model->endpoint.operations->send(model->port, 7, 42, "qmi", 3) == 0);
	atomic_store(&model->release, true);
	assert(pthread_join(worker, NULL) == 0);
	model_free(model);
}

static void
test_failures(void)
{
	const int failures[] = {EIO, EAGAIN, ENOSPC, -EIO};
	for (uint32_t after = 0; after < 2; after++) {
		for (uint32_t failure = 0; failure < sizeof(failures) / sizeof(failures[0]); failure++) {
			for (uint32_t call = 1; call <= 15; call++) {
				struct bridge_model *model = model_new();
				model_create(model, false);
				model->after = after != 0;
				model->fail_at = call;
				model->failure = failures[failure];
				assert(hakios_ipcr_start(model->bridge) == 0);
				uint32_t received;
				assert(model_work(model, &received) == EIO && received == 0);
				struct hakios_ipcr_status status;
				assert(hakios_ipcr_status(model->bridge, &status) == 0 && status.fault == EIO && !status.quiesced);
				unsigned calls = model->calls;
				assert(model_work(model, &received) == EIO && calls == model->calls);
				model_free(model);
			}
		}
	}
	for (uint32_t allocation = 1; allocation <= 4; allocation++) {
		struct bridge_model *model = model_new();
		pcie_test_fail_allocation_at = pcie_test_allocation_calls + allocation;
		assert(hakios_ipcr_create(&model->config, &operations, model, &model->bridge) == ENOMEM && model->bridge == NULL);
		pcie_test_fail_allocation_at = 0;
		assert(model->calls == 0);
		free(model);
	}
	struct bridge_model *model = model_new();
	model->buffers[1][0].iova = model->buffers[0][0].iova;
	assert(hakios_ipcr_create(&model->config, &operations, model, &model->bridge) == EINVAL);
	free(model);
}

static void
test_completion_failures(void)
{
	const int failures[] = {EIO, EAGAIN, ENOSPC, -EIO};
	for (uint32_t after = 0; after < 2; after++) {
		for (uint32_t failure = 0; failure < sizeof(failures) / sizeof(failures[0]); failure++) {
			for (uint32_t call = 1; call <= 5; call++) {
				struct bridge_model *model = model_new();
				model_create(model, true);
				model_connect(model);
				model->after = after != 0;
				model->failure = failures[failure];
				uint32_t received;
				if (call == 1) {
					assert(model->endpoint.operations->send(model->port, 7, 42, "qmi", 3) == 0);
					model->fail_at = model->calls + 1;
					assert(model_complete(model, 0, 1, 0) == EIO);
				} else if (call == 2) {
					model->fail_at = model->calls + 1;
					assert(model_complete(model, 1, 1, 35) == EIO);
				} else {
					peer_data(model, "qmi", 3, false);
					model->fail_at = model->calls + call - 2;
					assert(model_work(model, &received) == EIO && received == 1 && model->deliveries == 1);
				}
				struct hakios_ipcr_status status;
				assert(hakios_ipcr_status(model->bridge, &status) == 0 && status.fault == EIO);
				unsigned callbacks = model->calls;
				assert(model_work(model, &received) == EIO && received == 0 && model->calls == callbacks);
				model_free(model);
			}
		}
	}
}

static void
test_prestart_shutdown(void)
{
	struct bridge_model *model = model_new();
	model_create(model, true);
	assert(model->endpoint.operations->lookup(model->port, 0x45, 1, 1) == 0);
	assert(hakios_ipcr_stop(model->bridge) == 0 && model->stopped);
	unsigned scheduled = atomic_load(&model->scheduled);
	model->endpoint.operations->close(model->port);
	assert(atomic_load(&model->scheduled) == scheduled);
	model_free(model);
}

int
main(void)
{
	test_flow();
	test_qmi();
	test_concurrency();
	test_failures();
	test_completion_failures();
	test_prestart_shutdown();
	puts("ipcr-test: PASS (160 callback failures; DMA copies, worker/backpressure, epochs, PM gate, PCI QMI, concurrency)");
	return 0;
}
