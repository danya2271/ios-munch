#include <kern/kalloc.h>
#include <kern/locks.h>
#include <libkern/libkern.h>
#include <sys/errno.h>
#include <sys/hakios.pcie.kona.sid.h>

#define KONA_SID_TABLE_OFFSET 0x2000u
#define KONA_SID_TABLE_ENTRIES 256u
#define KONA_SID_REQUESTER_ID 0x0100u
#define KONA_SID_STREAM_BASE 0x1c00u
#define KONA_SID_STREAM_ID 0x1c01u
#define KONA_SID_CRC_POLYNOMIAL 0x07u

static LCK_GRP_DECLARE(kona_sid_lock_group, "hakios-pcie-kona-sid");

struct hakios_pcie_kona_sid {
	lck_mtx_t lock;
	struct hakios_pcie_kona_mmio *mmio;
	void *ownership;
	uint32_t original[KONA_SID_TABLE_ENTRIES];
	bool active;
};

static uint8_t
kona_sid_hash(uint16_t requester_id)
{
	uint8_t bytes[2] = {
		(uint8_t)(requester_id >> 8),
		(uint8_t)requester_id,
	};
	uint8_t crc = 0;

	for (unsigned byte = 0; byte < sizeof(bytes); byte++) {
		crc ^= bytes[byte];
		for (unsigned bit = 0; bit < 8; bit++) {
			crc = (crc & 0x80u) != 0 ?
			    (uint8_t)((crc << 1) ^ KONA_SID_CRC_POLYNOMIAL) :
			    (uint8_t)(crc << 1);
		}
	}
	return crc;
}

static int
kona_sid_read_table(struct hakios_pcie_kona_sid *sid, void *ownership, void *guard,
    uint32_t *values)
{
	for (uint32_t index = 0; index < KONA_SID_TABLE_ENTRIES; index++) {
		int error = hakios_pcie_kona_mmio_read(sid->mmio, ownership, guard,
		    HAKIOS_PCIE_KONA_PARF, KONA_SID_TABLE_OFFSET + index * 4, 4, &values[index]);

		if (error != 0) {
			return error;
		}
	}
	return 0;
}

static int
kona_sid_write_table(struct hakios_pcie_kona_sid *sid, void *ownership, void *guard,
    const uint32_t *values)
{
	for (uint32_t index = 0; index < KONA_SID_TABLE_ENTRIES; index++) {
		int error = hakios_pcie_kona_mmio_write(sid->mmio, ownership, guard,
		    HAKIOS_PCIE_KONA_PARF, KONA_SID_TABLE_OFFSET + index * 4, 4, values[index]);

		if (error != 0) {
			return error;
		}
	}
	return 0;
}

static int
kona_sid_verify_table(struct hakios_pcie_kona_sid *sid, void *ownership, void *guard,
    const uint32_t *expected)
{
	uint32_t observed[KONA_SID_TABLE_ENTRIES];
	int error = kona_sid_read_table(sid, ownership, guard, observed);

	if (error != 0) {
		return error;
	}
	return memcmp(observed, expected, sizeof(observed)) == 0 ? 0 : EIO;
}

int
hakios_pcie_kona_sid_create(struct hakios_pcie_kona_mmio *mmio,
    struct hakios_pcie_kona_sid **result)
{
	struct hakios_pcie_kona_sid *sid;

	if (result == NULL) {
		return EINVAL;
	}
	*result = NULL;
	if (mmio == NULL) {
		return EINVAL;
	}
	sid = kalloc_type(struct hakios_pcie_kona_sid, Z_WAITOK | Z_ZERO);
	if (sid == NULL) {
		return ENOMEM;
	}
	lck_mtx_init(&sid->lock, &kona_sid_lock_group, LCK_ATTR_NULL);
	sid->mmio = mmio;
	*result = sid;
	return 0;
}

int
hakios_pcie_kona_sid_apply(struct hakios_pcie_kona_sid *sid, void *ownership,
    void **cookie)
{
	void *guard = NULL;
	uint32_t replacement[KONA_SID_TABLE_ENTRIES] = { 0 };
	uint32_t value;
	uint8_t hash;
	int error, cleanup_error;

	if (cookie == NULL) {
		return EINVAL;
	}
	*cookie = NULL;
	if (sid == NULL || ownership == NULL) {
		return EINVAL;
	}
	lck_mtx_lock(&sid->lock);
	if (sid->active) {
		error = EBUSY;
		goto out;
	}
	error = hakios_pcie_kona_mmio_access_begin(sid->mmio, ownership, &guard);
	if (error != 0) {
		goto out;
	}
	error = kona_sid_read_table(sid, ownership, guard, sid->original);
	if (error != 0) {
		goto finish_guard;
	}
	hash = kona_sid_hash(KONA_SID_REQUESTER_ID);
	value = ((uint32_t)KONA_SID_REQUESTER_ID << 16) |
	    ((uint32_t)(KONA_SID_STREAM_ID - KONA_SID_STREAM_BASE) << 8);
	replacement[hash] = value;
	sid->ownership = ownership;
	sid->active = true;
	*cookie = sid;
	for (uint32_t index = 0; index < KONA_SID_TABLE_ENTRIES; index++) {
		error = hakios_pcie_kona_mmio_write(sid->mmio, ownership, guard,
		    HAKIOS_PCIE_KONA_PARF, KONA_SID_TABLE_OFFSET + index * 4, 4, 0);
		if (error != 0) {
			goto finish_guard;
		}
	}
	error = hakios_pcie_kona_mmio_write(sid->mmio, ownership, guard,
	    HAKIOS_PCIE_KONA_PARF, KONA_SID_TABLE_OFFSET + (uint32_t)hash * 4, 4, value);
	if (error == 0) {
		error = kona_sid_verify_table(sid, ownership, guard, replacement);
	}
finish_guard:
	cleanup_error = hakios_pcie_kona_mmio_access_end(sid->mmio, ownership, &guard);
	if (cleanup_error != 0) {
		error = cleanup_error;
	}
out:
	lck_mtx_unlock(&sid->lock);
	return error;
}

int
hakios_pcie_kona_sid_restore(struct hakios_pcie_kona_sid *sid, void *ownership,
    void **cookie)
{
	void *guard = NULL;
	int error, cleanup_error;

	if (sid == NULL || ownership == NULL || cookie == NULL || *cookie != sid) {
		return EINVAL;
	}
	lck_mtx_lock(&sid->lock);
	if (!sid->active || sid->ownership != ownership) {
		error = ESTALE;
		goto out;
	}
	error = hakios_pcie_kona_mmio_access_begin(sid->mmio, ownership, &guard);
	if (error != 0) {
		goto out;
	}
	error = kona_sid_write_table(sid, ownership, guard, sid->original);
	if (error == 0) {
		error = kona_sid_verify_table(sid, ownership, guard, sid->original);
	}
	cleanup_error = hakios_pcie_kona_mmio_access_end(sid->mmio, ownership, &guard);
	if (cleanup_error != 0) {
		error = cleanup_error;
	}
	if (error == 0) {
		sid->active = false;
		sid->ownership = NULL;
		bzero(sid->original, sizeof(sid->original));
		*cookie = NULL;
	}
out:
	lck_mtx_unlock(&sid->lock);
	return error;
}

int
hakios_pcie_kona_sid_destroy(struct hakios_pcie_kona_sid **sid)
{
	struct hakios_pcie_kona_sid *state;

	if (sid == NULL || *sid == NULL) {
		return EINVAL;
	}
	state = *sid;
	lck_mtx_lock(&state->lock);
	if (state->active) {
		lck_mtx_unlock(&state->lock);
		return EBUSY;
	}
	lck_mtx_unlock(&state->lock);
	lck_mtx_destroy(&state->lock, &kona_sid_lock_group);
	kfree_type(struct hakios_pcie_kona_sid, state);
	*sid = NULL;
	return 0;
}
