#ifndef _SYS_HAKIOS_QRTR_WIRE_H_
#define _SYS_HAKIOS_QRTR_WIRE_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define HAKIOS_QRTR_FRAME_MAX 8192u
#define HAKIOS_QRTR_CONTROL_PORT 0xfffffffeu

enum hakios_qrtr_type {
	HAKIOS_QRTR_DATA = 1,
	HAKIOS_QRTR_HELLO = 2,
	HAKIOS_QRTR_BYE = 3,
	HAKIOS_QRTR_NEW_SERVER = 4,
	HAKIOS_QRTR_DEL_SERVER = 5,
	HAKIOS_QRTR_DEL_CLIENT = 6,
	HAKIOS_QRTR_RESUME_TX = 7,
	HAKIOS_QRTR_NEW_LOOKUP = 10,
	HAKIOS_QRTR_DEL_LOOKUP = 11,
};

struct hakios_qrtr_packet {
	uint32_t type;
	uint32_t source_node;
	uint32_t source_port;
	uint32_t destination_node;
	uint32_t destination_port;
	bool confirm_rx;
	const uint8_t *payload;
	size_t length;
};

struct hakios_qrtr_control {
	uint32_t type;
	uint32_t service;
	uint32_t instance;
	uint32_t node;
	uint32_t port;
	uint8_t version;
};

int hakios_qrtr_wire_decode(const void *frame, size_t size, struct hakios_qrtr_packet *packet);
int hakios_qrtr_wire_encode(const struct hakios_qrtr_packet *packet, void *frame, size_t capacity, size_t *size);
int hakios_qrtr_control_decode(const struct hakios_qrtr_packet *packet, struct hakios_qrtr_control *control);
int hakios_qrtr_control_encode(const struct hakios_qrtr_control *control,
    const struct hakios_qrtr_packet *route, void *frame, size_t capacity, size_t *size);
int hakios_qrtr_wire_resume(const struct hakios_qrtr_packet *received, void *frame, size_t capacity, size_t *size);

#endif
