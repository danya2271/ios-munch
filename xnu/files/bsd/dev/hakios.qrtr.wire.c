#include <libkern/libkern.h>
#include <sys/errno.h>
#include <sys/hakios.qrtr.wire.h>
#include <sys/hakios.wire.h>

static bool
qrtr_type_valid(uint32_t type)
{
	return (type >= 1 && type <= 7) || type == 10 || type == 11;
}

static bool
qrtr_server_type(uint32_t type)
{
	return type == HAKIOS_QRTR_NEW_SERVER || type == HAKIOS_QRTR_DEL_SERVER ||
	       type == HAKIOS_QRTR_NEW_LOOKUP || type == HAKIOS_QRTR_DEL_LOOKUP;
}

static uint32_t
qrtr_get16(const uint8_t *bytes)
{
	return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8;
}

static int
qrtr_packet_valid(const struct hakios_qrtr_packet *packet)
{
	if (!qrtr_type_valid(packet->type)) {
		return ENOTSUP;
	}
	if (packet->payload == NULL || packet->length == 0 || packet->length > HAKIOS_QRTR_FRAME_MAX - 16) {
		return EMSGSIZE;
	}
	if (packet->type != HAKIOS_QRTR_DATA) {
		if (packet->length != 20 || hakios_wire_get32(packet->payload) != packet->type || packet->confirm_rx ||
		    (packet->destination_port != HAKIOS_QRTR_CONTROL_PORT && packet->type != HAKIOS_QRTR_RESUME_TX)) {
			return EPROTO;
		}
	}
	return 0;
}

int
hakios_qrtr_wire_decode(const void *frame, size_t size, struct hakios_qrtr_packet *packet)
{
	const uint8_t *bytes = frame;
	struct hakios_qrtr_packet decoded = {0};
	size_t header;
	int error;

	if (packet == NULL) {
		return EINVAL;
	}
	bzero(packet, sizeof(*packet));
	if (bytes == NULL || size < 16 || size > HAKIOS_QRTR_FRAME_MAX || (size & 3) != 0) {
		return EMSGSIZE;
	}
	if (bytes[0] == 1) {
		if (size < 32 || hakios_wire_get32(bytes) != 1) {
			return EPROTO;
		}
		header = 32;
		decoded.type = hakios_wire_get32(bytes + 4);
		decoded.source_node = hakios_wire_get32(bytes + 8);
		decoded.source_port = hakios_wire_get32(bytes + 12);
		decoded.confirm_rx = hakios_wire_get32(bytes + 16) != 0;
		decoded.length = hakios_wire_get32(bytes + 20);
		decoded.destination_node = hakios_wire_get32(bytes + 24);
		decoded.destination_port = hakios_wire_get32(bytes + 28);
	} else if (bytes[0] == 3) {
		if (bytes[3] != 0 || (bytes[2] & ~1u) != 0) {
			return ENOTSUP;
		}
		header = 16;
		decoded.type = bytes[1];
		decoded.confirm_rx = (bytes[2] & 1) != 0;
		decoded.length = hakios_wire_get32(bytes + 4);
		decoded.source_node = qrtr_get16(bytes + 8);
		decoded.source_port = qrtr_get16(bytes + 10);
		decoded.destination_node = qrtr_get16(bytes + 12);
		decoded.destination_port = qrtr_get16(bytes + 14);
		if (decoded.source_port == 0xfffe) {
			decoded.source_port = HAKIOS_QRTR_CONTROL_PORT;
		}
		if (decoded.destination_port == 0xfffe) {
			decoded.destination_port = HAKIOS_QRTR_CONTROL_PORT;
		}
	} else {
		return ENOTSUP;
	}
	if (decoded.length > size - header || ((decoded.length + 3) & ~(size_t)3) != size - header) {
		return EMSGSIZE;
	}
	if (decoded.destination_port == 0xffff) {
		decoded.destination_port = HAKIOS_QRTR_CONTROL_PORT;
	}
	decoded.payload = bytes + header;
	error = qrtr_packet_valid(&decoded);
	if (error == 0) {
		*packet = decoded;
	}
	return error;
}

int
hakios_qrtr_wire_encode(const struct hakios_qrtr_packet *packet, void *frame, size_t capacity, size_t *size)
{
	uint8_t *bytes = frame;
	int error;

	if (size == NULL) {
		return EINVAL;
	}
	*size = 0;
	if (packet == NULL || bytes == NULL) {
		return EINVAL;
	}
	struct hakios_qrtr_packet saved = *packet;
	error = qrtr_packet_valid(&saved);
	if (error != 0) {
		return error;
	}
	size_t padded = (saved.length + 3) & ~(size_t)3;
	if (padded > HAKIOS_QRTR_FRAME_MAX - 32 || capacity < padded + 32) {
		return EMSGSIZE;
	}
	bcopy(saved.payload, bytes + 32, saved.length);
	bzero(bytes + 32 + saved.length, padded - saved.length);
	hakios_wire_put32(bytes, 1);
	hakios_wire_put32(bytes + 4, saved.type);
	hakios_wire_put32(bytes + 8, saved.source_node);
	hakios_wire_put32(bytes + 12, saved.source_port);
	hakios_wire_put32(bytes + 16, saved.confirm_rx ? 1 : 0);
	hakios_wire_put32(bytes + 20, (uint32_t)saved.length);
	hakios_wire_put32(bytes + 24, saved.destination_node);
	hakios_wire_put32(bytes + 28, saved.destination_port);
	*size = padded + 32;
	return 0;
}

int
hakios_qrtr_control_decode(const struct hakios_qrtr_packet *packet, struct hakios_qrtr_control *control)
{
	if (control == NULL) {
		return EINVAL;
	}
	bzero(control, sizeof(*control));
	if (packet == NULL) {
		return EINVAL;
	}
	int error = qrtr_packet_valid(packet);
	if (error != 0 || packet->type == HAKIOS_QRTR_DATA) {
		return error != 0 ? error : EINVAL;
	}
	control->type = packet->type;
	if (qrtr_server_type(packet->type)) {
		control->service = hakios_wire_get32(packet->payload + 4);
		uint32_t instance = hakios_wire_get32(packet->payload + 8);
		control->version = (uint8_t)instance;
		control->instance = instance >> 8;
		control->node = hakios_wire_get32(packet->payload + 12);
		control->port = hakios_wire_get32(packet->payload + 16);
	} else if (packet->type == HAKIOS_QRTR_DEL_CLIENT || packet->type == HAKIOS_QRTR_RESUME_TX) {
		control->node = hakios_wire_get32(packet->payload + 4);
		control->port = hakios_wire_get32(packet->payload + 8);
	}
	return 0;
}

int
hakios_qrtr_control_encode(const struct hakios_qrtr_control *control,
    const struct hakios_qrtr_packet *route, void *frame, size_t capacity, size_t *size)
{
	uint8_t payload[20] = {0};
	if (size == NULL) {
		return EINVAL;
	}
	*size = 0;
	if (control == NULL || route == NULL || control->type == HAKIOS_QRTR_DATA ||
	    !qrtr_type_valid(control->type) || control->instance > 0xffffff) {
		return EINVAL;
	}
	hakios_wire_put32(payload, control->type);
	if (qrtr_server_type(control->type)) {
		hakios_wire_put32(payload + 4, control->service);
		hakios_wire_put32(payload + 8, control->instance << 8 | control->version);
		hakios_wire_put32(payload + 12, control->node);
		hakios_wire_put32(payload + 16, control->port);
	} else if (control->type == HAKIOS_QRTR_DEL_CLIENT || control->type == HAKIOS_QRTR_RESUME_TX) {
		hakios_wire_put32(payload + 4, control->node);
		hakios_wire_put32(payload + 8, control->port);
	}
	struct hakios_qrtr_packet packet = *route;
	packet.type = control->type;
	packet.confirm_rx = false;
	packet.payload = payload;
	packet.length = sizeof(payload);
	return hakios_qrtr_wire_encode(&packet, frame, capacity, size);
}

int
hakios_qrtr_wire_resume(const struct hakios_qrtr_packet *received, void *frame, size_t capacity, size_t *size)
{
	if (size == NULL) {
		return EINVAL;
	}
	*size = 0;
	if (received == NULL || received->type != HAKIOS_QRTR_DATA || !received->confirm_rx) {
		return EINVAL;
	}
	int error = qrtr_packet_valid(received);
	if (error != 0) {
		return error;
	}
	struct hakios_qrtr_packet route = {
		.source_node = received->destination_node, .source_port = received->destination_port,
		.destination_node = received->source_node, .destination_port = received->source_port,
	};
	struct hakios_qrtr_control control = {
		.type = HAKIOS_QRTR_RESUME_TX, .node = received->destination_node, .port = received->destination_port,
	};
	return hakios_qrtr_control_encode(&control, &route, frame, capacity, size);
}
