// SPDX-License-Identifier: BSD-3-Clause
/* Copyright(c) 2019 Broadcom All rights reserved. */

#include <inttypes.h>
#include <stdbool.h>

#include <rte_bitmap.h>
#include <rte_byteorder.h>
#include <rte_malloc.h>
#include <rte_memory.h>
#if defined(RTE_ARCH_X86)
#include <tmmintrin.h>
#else
#error "bnxt vector pmd: unsupported target."
#endif

#include "bnxt.h"
#include "bnxt_cpr.h"
#include "bnxt_ring.h"
#include "bnxt_rxtx_vec_common.h"

#include "bnxt_txq.h"
#include "bnxt_txr.h"

/*
 * RX Ring handling
 */

static inline void
bnxt_rxq_rearm(struct bnxt_rx_queue *rxq, struct bnxt_rx_ring_info *rxr)
{
	struct rx_prod_pkt_bd *rxbds = &rxr->rx_desc_ring[rxq->rxrearm_start];
	struct rte_mbuf **rx_bufs = &rxr->rx_buf_ring[rxq->rxrearm_start];
	struct rte_mbuf *mb0, *mb1;
	int nb, i;

	const __m128i hdr_room = _mm_set_epi64x(RTE_PKTMBUF_HEADROOM, 0);
	const __m128i addrmask = _mm_set_epi64x(UINT64_MAX, 0);

	/*
	 * Number of mbufs to allocate must be a multiple of two. The
	 * allocation must not go past the end of the ring.
	 */
	nb = RTE_MIN(rxq->rxrearm_nb & ~0x1,
		     rxq->nb_rx_desc - rxq->rxrearm_start);

	/* Allocate new mbufs into the software ring */
	if (rte_mempool_get_bulk(rxq->mb_pool, (void *)rx_bufs, nb) < 0) {
		rte_eth_devices[rxq->port_id].data->rx_mbuf_alloc_failed += nb;

		return;
	}

	/* Initialize the mbufs in vector, process 2 mbufs in one loop */
	for (i = 0; i < nb; i += 2, rx_bufs += 2) {
		__m128i buf_addr0, buf_addr1;
		__m128i rxbd0, rxbd1;

		mb0 = rx_bufs[0];
		mb1 = rx_bufs[1];

		/* Load address fields from both mbufs */
		buf_addr0 = _mm_loadu_si128((__m128i *)&mb0->buf_addr);
		buf_addr1 = _mm_loadu_si128((__m128i *)&mb1->buf_addr);

		/* Load both rx descriptors (preserving some existing fields) */
		rxbd0 = _mm_loadu_si128((__m128i *)(rxbds + 0));
		rxbd1 = _mm_loadu_si128((__m128i *)(rxbds + 1));

		/* Add default offset to buffer address. */
		buf_addr0 = _mm_add_epi64(buf_addr0, hdr_room);
		buf_addr1 = _mm_add_epi64(buf_addr1, hdr_room);

		/* Clear all fields except address. */
		buf_addr0 =  _mm_and_si128(buf_addr0, addrmask);
		buf_addr1 =  _mm_and_si128(buf_addr1, addrmask);

		/* Clear address field in descriptor. */
		rxbd0 = _mm_andnot_si128(addrmask, rxbd0);
		rxbd1 = _mm_andnot_si128(addrmask, rxbd1);

		/* Set address field in descriptor. */
		rxbd0 = _mm_add_epi64(rxbd0, buf_addr0);
		rxbd1 = _mm_add_epi64(rxbd1, buf_addr1);

		/* Store descriptors to memory. */
		_mm_store_si128((__m128i *)(rxbds++), rxbd0);
		_mm_store_si128((__m128i *)(rxbds++), rxbd1);
	}

	rxq->rxrearm_start += nb;
	bnxt_db_write(&rxr->rx_db, rxq->rxrearm_start - 1);
	if (rxq->rxrearm_start >= rxq->nb_rx_desc)
		rxq->rxrearm_start = 0;

	rxq->rxrearm_nb -= nb;
}

static __m128i
bnxt_parse_pkt_type(__m128i mm_rxcmp, __m128i mm_rxcmp1)
{
	uint32_t flags_type, flags2;
	uint8_t index;

	flags_type = _mm_extract_epi16(mm_rxcmp, 0);
	flags2 = _mm_extract_epi32(mm_rxcmp1, 0);

	/*
	 * Index format:
	 *     bit 0: RX_PKT_CMPL_FLAGS2_T_IP_CS_CALC
	 *     bit 1: RX_CMPL_FLAGS2_IP_TYPE
	 *     bit 2: RX_PKT_CMPL_FLAGS2_META_FORMAT_VLAN
	 *     bits 3-6: RX_PKT_CMPL_FLAGS_ITYPE
	 */
	index = ((flags_type & RX_PKT_CMPL_FLAGS_ITYPE_MASK) >> 9) |
		((flags2 & (RX_PKT_CMPL_FLAGS2_META_FORMAT_VLAN |
			   RX_PKT_CMPL_FLAGS2_T_IP_CS_CALC)) >> 2) |
		((flags2 & RX_PKT_CMPL_FLAGS2_IP_TYPE) >> 7);

	return _mm_set_epi32(0, 0, 0, bnxt_ptype_table[index]);
}

static __m128i
bnxt_set_ol_flags(__m128i mm_rxcmp, __m128i mm_rxcmp1)
{
	uint16_t flags_type, errors, flags;
	uint32_t ol_flags;

	/* Extract rxcmp1->flags2. */
	flags = _mm_extract_epi32(mm_rxcmp1, 0) & 0x1F;
	/* Extract rxcmp->flags_type. */
	flags_type = _mm_extract_epi16(mm_rxcmp, 0);
	/* Extract rxcmp1->errors_v2. */
	errors = (_mm_extract_epi16(mm_rxcmp1, 4) >> 4) & flags & 0xF;

	ol_flags = bnxt_ol_flags_table[flags & ~errors];

	if (flags_type & RX_PKT_CMPL_FLAGS_RSS_VALID)
		ol_flags |= PKT_RX_RSS_HASH;

	if (errors)
		ol_flags |= bnxt_ol_flags_err_table[errors];

	return _mm_set_epi64x(ol_flags, 0);
}

uint16_t
bnxt_recv_pkts_vec(void *rx_queue, struct rte_mbuf **rx_pkts,
		   uint16_t nb_pkts)
{
	struct bnxt_rx_queue *rxq = rx_queue;
	struct bnxt_cp_ring_info *cpr = rxq->cp_ring;
	struct bnxt_rx_ring_info *rxr = rxq->rx_ring;
	uint32_t raw_cons = cpr->cp_raw_cons;
	uint32_t cons;
	int nb_rx_pkts = 0;
	struct rx_pkt_cmpl *rxcmp;
	const __m128i mbuf_init = _mm_set_epi64x(0, rxq->mbuf_initializer);
	const __m128i shuf_msk =
		_mm_set_epi8(15, 14, 13, 12,          /* rss */
			     0xFF, 0xFF,              /* vlan_tci (zeroes) */
			     3, 2,                    /* data_len */
			     0xFF, 0xFF, 3, 2,        /* pkt_len */
			     0xFF, 0xFF, 0xFF, 0xFF); /* pkt_type (zeroes) */
	int i;

	/* If Rx Q was stopped return */
	if (unlikely(!rxq->rx_started))
		return 0;

	if (rxq->rxrearm_nb >= rxq->rx_free_thresh)
		bnxt_rxq_rearm(rxq, rxr);

	/* Return no more than RTE_BNXT_MAX_RX_BURST per call. */
	nb_pkts = RTE_MIN(nb_pkts, RTE_BNXT_MAX_RX_BURST);

	/*
	 * Make nb_pkts an integer multiple of RTE_BNXT_DESCS_PER_LOOP.
	 * nb_pkts < RTE_BNXT_DESCS_PER_LOOP, just return no packet
	 */
	nb_pkts = RTE_ALIGN_FLOOR(nb_pkts, RTE_BNXT_DESCS_PER_LOOP);
	if (!nb_pkts)
		return 0;

	/* Handle RX burst request */
	for (i = 0; i < nb_pkts; i++) {
		struct rx_pkt_cmpl_hi *rxcmp1;
		struct rte_mbuf *mbuf;
		__m128i mm_rxcmp, mm_rxcmp1, pkt_mb, ptype, rearm;

		cons = RING_CMP(cpr->cp_ring_struct, raw_cons);

		rxcmp = (struct rx_pkt_cmpl *)&cpr->cp_desc_ring[cons];
		rxcmp1 = (struct rx_pkt_cmpl_hi *)&cpr->cp_desc_ring[cons + 1];

		if (!CMP_VALID(rxcmp1, raw_cons + 1, cpr->cp_ring_struct))
			break;

		mm_rxcmp = _mm_load_si128((__m128i *)rxcmp);
		mm_rxcmp1 = _mm_load_si128((__m128i *)rxcmp1);

		raw_cons += 2;
		cons = rxcmp->opaque;

		mbuf = rxr->rx_buf_ring[cons];
		rxr->rx_buf_ring[cons] = NULL;

		/* Set fields from mbuf initializer and ol_flags. */
		rearm = _mm_or_si128(mbuf_init,
				     bnxt_set_ol_flags(mm_rxcmp, mm_rxcmp1));
		_mm_store_si128((__m128i *)&mbuf->rearm_data, rearm);

		/* Set mbuf pkt_len, data_len, and rss_hash fields. */
		pkt_mb = _mm_shuffle_epi8(mm_rxcmp, shuf_msk);

		/* Set packet type. */
		ptype = bnxt_parse_pkt_type(mm_rxcmp, mm_rxcmp1);
		pkt_mb = _mm_blend_epi16(pkt_mb, ptype, 0x3);

		/*
		 * Shift vlan_tci from completion metadata field left six
		 * bytes and blend into mbuf->rx_descriptor_fields1 to set
		 * mbuf->vlan_tci.
		 */
		pkt_mb = _mm_blend_epi16(pkt_mb,
					 _mm_slli_si128(mm_rxcmp1, 6), 0x20);

		/* Store descriptor fields. */
		_mm_storeu_si128((void *)&mbuf->rx_descriptor_fields1, pkt_mb);

		rx_pkts[nb_rx_pkts++] = mbuf;
	}

	if (nb_rx_pkts) {
		rxr->rx_prod =
			RING_ADV(rxr->rx_ring_struct, rxr->rx_prod, nb_rx_pkts);

		rxq->rxrearm_nb += nb_rx_pkts;
		cpr->cp_raw_cons = raw_cons;
		cpr->valid =
			!!(cpr->cp_raw_cons & cpr->cp_ring_struct->ring_size);
		bnxt_db_cq(cpr);
	}

	return nb_rx_pkts;
}

static void
bnxt_tx_cmp_vec(struct bnxt_tx_queue *txq, int nr_pkts)
{
	struct bnxt_tx_ring_info *txr = txq->tx_ring;
	struct rte_mbuf **free = txq->free;
	uint16_t cons = txr->tx_cons;
	unsigned int blk = 0;

	while (nr_pkts--) {
		struct bnxt_sw_tx_bd *tx_buf;
		struct rte_mbuf *mbuf;

		tx_buf = &txr->tx_buf_ring[cons];
		cons = RING_NEXT(txr->tx_ring_struct, cons);
		mbuf = rte_pktmbuf_prefree_seg(tx_buf->mbuf);
		if (unlikely(mbuf == NULL))
			continue;
		tx_buf->mbuf = NULL;

		if (blk && mbuf->pool != free[0]->pool) {
			rte_mempool_put_bulk(free[0]->pool, (void **)free, blk);
			blk = 0;
		}
		free[blk++] = mbuf;
	}
	if (blk)
		rte_mempool_put_bulk(free[0]->pool, (void **)free, blk);

	txr->tx_cons = cons;
}

static void
bnxt_handle_tx_cp_vec(struct bnxt_tx_queue *txq)
{
	struct bnxt_cp_ring_info *cpr = txq->cp_ring;
	uint32_t raw_cons = cpr->cp_raw_cons;
	uint32_t cons;
	uint32_t nb_tx_pkts = 0;
	struct tx_cmpl *txcmp;
	struct cmpl_base *cp_desc_ring = cpr->cp_desc_ring;
	struct bnxt_ring *cp_ring_struct = cpr->cp_ring_struct;
	uint32_t ring_mask = cp_ring_struct->ring_mask;

	do {
		cons = RING_CMPL(ring_mask, raw_cons);
		txcmp = (struct tx_cmpl *)&cp_desc_ring[cons];

		if (!CMP_VALID(txcmp, raw_cons, cp_ring_struct))
			break;

		if (likely(CMP_TYPE(txcmp) == TX_CMPL_TYPE_TX_L2))
			nb_tx_pkts += txcmp->opaque;
		else
			RTE_LOG_DP(ERR, PMD,
				   "Unhandled CMP type %02x\n",
				   CMP_TYPE(txcmp));
		raw_cons = NEXT_RAW_CMP(raw_cons);
	} while (nb_tx_pkts < ring_mask);

	cpr->valid = !!(raw_cons & cp_ring_struct->ring_size);
	if (nb_tx_pkts) {
		bnxt_tx_cmp_vec(txq, nb_tx_pkts);
		cpr->cp_raw_cons = raw_cons;
		bnxt_db_cq(cpr);
	}
}

static uint16_t
bnxt_xmit_fixed_burst_vec(void *tx_queue, struct rte_mbuf **tx_pkts,
			  uint16_t nb_pkts)
{
	struct bnxt_tx_queue *txq = tx_queue;
	struct bnxt_tx_ring_info *txr = txq->tx_ring;
	uint16_t prod = txr->tx_prod;
	struct rte_mbuf *tx_mbuf;
	struct tx_bd_long *txbd = NULL;
	struct bnxt_sw_tx_bd *tx_buf;
	uint16_t to_send;

	nb_pkts = RTE_MIN(nb_pkts, bnxt_tx_avail(txq));

	if (unlikely(nb_pkts == 0))
		return 0;

	/* Handle TX burst request */
	to_send = nb_pkts;
	while (to_send) {
		tx_mbuf = *tx_pkts++;
		rte_prefetch0(tx_mbuf);

		tx_buf = &txr->tx_buf_ring[prod];
		tx_buf->mbuf = tx_mbuf;
		tx_buf->nr_bds = 1;

		txbd = &txr->tx_desc_ring[prod];
		txbd->address = tx_mbuf->buf_iova + tx_mbuf->data_off;
		txbd->len = tx_mbuf->data_len;
		txbd->flags_type = bnxt_xmit_flags_len(tx_mbuf->data_len,
						       TX_BD_FLAGS_NOCMPL);
		prod = RING_NEXT(txr->tx_ring_struct, prod);
		to_send--;
	}

	/* Request a completion for last packet in burst */
	if (txbd) {
		txbd->opaque = nb_pkts;
		txbd->flags_type &= ~TX_BD_LONG_FLAGS_NO_CMPL;
	}

	rte_compiler_barrier();
	bnxt_db_write(&txr->tx_db, prod);

	txr->tx_prod = prod;

	return nb_pkts;
}

uint16_t
bnxt_xmit_pkts_vec(void *tx_queue, struct rte_mbuf **tx_pkts,
		   uint16_t nb_pkts)
{
	int nb_sent = 0;
	struct bnxt_tx_queue *txq = tx_queue;

	/* Tx queue was stopped; wait for it to be restarted */
	if (unlikely(!txq->tx_started)) {
		PMD_DRV_LOG(DEBUG, "Tx q stopped;return\n");
		return 0;
	}

	/* Handle TX completions */
	if (bnxt_tx_bds_in_hw(txq) >= txq->tx_free_thresh)
		bnxt_handle_tx_cp_vec(txq);

	while (nb_pkts) {
		uint16_t ret, num;

		num = RTE_MIN(nb_pkts, RTE_BNXT_MAX_TX_BURST);
		ret = bnxt_xmit_fixed_burst_vec(tx_queue,
						&tx_pkts[nb_sent],
						num);
		nb_sent += ret;
		nb_pkts -= ret;
		if (ret < num)
			break;
	}

	return nb_sent;
}

int __rte_cold
bnxt_rxq_vec_setup(struct bnxt_rx_queue *rxq)
{
	return bnxt_rxq_vec_setup_common(rxq);
}
