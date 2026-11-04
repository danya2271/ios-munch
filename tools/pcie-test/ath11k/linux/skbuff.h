#ifndef PCIE_TEST_SKBUFF_H
#define PCIE_TEST_SKBUFF_H
#include <stdatomic.h>
#include <linux/dma-mapping.h>
#define NET_SKB_PAD 64u
typedef atomic_uint refcount_t;
static inline void refcount_set(refcount_t *ref, unsigned value) { atomic_store(ref, value); }
static inline bool refcount_dec_and_test(refcount_t *ref) { return atomic_fetch_sub(ref, 1) == 1; }
struct sk_buff;
struct skb_shared_info { unsigned nr_frags; struct sk_buff *frag_list; };
struct sk_buff {
	struct sk_buff *prev, *next;
	struct skb_shared_info *shinfo;
	uint8_t *head, *data, *tail, *end;
	uint32_t truesize;
	refcount_t refcnt;
	void *m;
	void (*m_free_func)(void *);
	_Alignas(64) uint8_t cb[64];
};
static inline void skb_reserve(struct sk_buff *skb, size_t size) { skb->data += size; skb->tail += size; }
static inline struct sk_buff *skb_get(struct sk_buff *skb) { atomic_fetch_add(&skb->refcnt, 1); return skb; }
struct sk_buff *linuxkpi_dev_alloc_skb(size_t, gfp_t);
struct sk_buff *linuxkpi_alloc_skb(size_t, gfp_t);
void linuxkpi_kfree_skb(struct sk_buff *);
#endif
