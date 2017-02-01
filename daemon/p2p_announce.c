#include "daemon/broadcast.h"
#include "daemon/chaintopology.h"
#include "daemon/log.h"
#include "daemon/p2p_announce.h"
#include "daemon/packets.h"
#include "daemon/peer.h"
#include "daemon/routing.h"
#include "daemon/secrets.h"
#include "daemon/timeout.h"
#include "utils.h"

#include <ccan/tal/str/str.h>
#include <ccan/tal/tal.h>
#include <secp256k1.h>

void handle_channel_announcement(
	struct routing_state *rstate,
	const u8 *announce, size_t len)
{
	u8 *serialized;
	bool forward = false;
	secp256k1_ecdsa_signature node_signature_1;
	secp256k1_ecdsa_signature node_signature_2;
	struct channel_id channel_id;
	secp256k1_ecdsa_signature bitcoin_signature_1;
	secp256k1_ecdsa_signature bitcoin_signature_2;
	struct pubkey node_id_1;
	struct pubkey node_id_2;
	struct pubkey bitcoin_key_1;
	struct pubkey bitcoin_key_2;
	const tal_t *tmpctx = tal_tmpctx(rstate);
	u8 *features;

	serialized = tal_dup_arr(tmpctx, u8, announce, len, 0);
	if (!fromwire_channel_announcement(tmpctx, serialized, NULL,
					   &node_signature_1, &node_signature_2,
					   &bitcoin_signature_1,
					   &bitcoin_signature_2,
					   &channel_id,
					   &node_id_1, &node_id_2,
					   &bitcoin_key_1, &bitcoin_key_2,
					   &features)) {
		tal_free(tmpctx);
		return;
	}

	// FIXME: Check features!
	//FIXME(cdecker) Check signatures, when the spec is settled
	//FIXME(cdecker) Check chain topology for the anchor TX

	log_debug(rstate->base_log,
		  "Received channel_announcement for channel %d:%d:%d",
		  channel_id.blocknum,
		  channel_id.txnum,
		  channel_id.outnum
		);

	forward |= add_channel_direction(rstate, &node_id_1,
					 &node_id_2, 0, &channel_id,
					 serialized);
	forward |= add_channel_direction(rstate, &node_id_2,
					 &node_id_1, 1, &channel_id,
					 serialized);
	if (!forward){
		log_debug(rstate->base_log, "Not forwarding channel_announcement");
		tal_free(tmpctx);
		return;
	}

	u8 *tag = tal_arr(tmpctx, u8, 0);
	towire_channel_id(&tag, &channel_id);
	queue_broadcast(rstate->broadcasts, WIRE_CHANNEL_ANNOUNCEMENT,
			tag, serialized);

	tal_free(tmpctx);
}

void handle_channel_update(struct routing_state *rstate, const u8 *update, size_t len)
{
	u8 *serialized;
	struct node_connection *c;
	secp256k1_ecdsa_signature signature;
	struct channel_id channel_id;
	u32 timestamp;
	u16 flags;
	u16 expiry;
	u32 htlc_minimum_msat;
	u32 fee_base_msat;
	u32 fee_proportional_millionths;
	const tal_t *tmpctx = tal_tmpctx(rstate);

	serialized = tal_dup_arr(tmpctx, u8, update, len, 0);
	if (!fromwire_channel_update(serialized, NULL, &signature, &channel_id,
				     &timestamp, &flags, &expiry,
				     &htlc_minimum_msat, &fee_base_msat,
				     &fee_proportional_millionths)) {
		tal_free(tmpctx);
		return;
	}


	log_debug(rstate->base_log, "Received channel_update for channel %d:%d:%d(%d)",
		  channel_id.blocknum,
		  channel_id.txnum,
		  channel_id.outnum,
		  flags & 0x01
		);

	c = get_connection_by_cid(rstate, &channel_id, flags & 0x1);

	if (!c) {
		log_debug(rstate->base_log, "Ignoring update for unknown channel %d:%d:%d",
			  channel_id.blocknum,
			  channel_id.txnum,
			  channel_id.outnum
			);
		tal_free(tmpctx);
		return;
	} else if (c->last_timestamp >= timestamp) {
		log_debug(rstate->base_log, "Ignoring outdated update.");
		tal_free(tmpctx);
		return;
	}

	//FIXME(cdecker) Check signatures
	c->last_timestamp = timestamp;
	c->delay = expiry;
	c->htlc_minimum_msat = htlc_minimum_msat;
	c->base_fee = fee_base_msat;
	c->proportional_fee = fee_proportional_millionths;
	c->active = true;
	log_debug(rstate->base_log, "Channel %d:%d:%d(%d) was updated.",
		  channel_id.blocknum,
		  channel_id.txnum,
		  channel_id.outnum,
		  flags
		);

	u8 *tag = tal_arr(tmpctx, u8, 0);
	towire_channel_id(&tag, &channel_id);
	queue_broadcast(rstate->broadcasts,
			WIRE_CHANNEL_UPDATE,
			tag,
			serialized);

	tal_free(c->channel_update);
	c->channel_update = tal_steal(c, serialized);
	tal_free(tmpctx);
}

void handle_node_announcement(
	struct routing_state *rstate, const u8 *node_ann, size_t len)
{
	u8 *serialized;
	struct sha256_double hash;
	struct node *node;
	secp256k1_ecdsa_signature signature;
	u32 timestamp;
	struct pubkey node_id;
	u8 rgb_color[3];
	u8 alias[32];
	u8 *features, *addresses;
	const tal_t *tmpctx = tal_tmpctx(rstate);

	serialized = tal_dup_arr(tmpctx, u8, node_ann, len, 0);
	if (!fromwire_node_announcement(tmpctx, serialized, NULL,
					&signature, &timestamp,
					&node_id, rgb_color, alias, &features,
					&addresses)) {
		tal_free(tmpctx);
		return;
	}

	// FIXME: Check features!
	log_debug_struct(rstate->base_log,
			 "Received node_announcement for node %s",
			 struct pubkey, &node_id);

	sha256_double(&hash, serialized + 66, tal_count(serialized) - 66);
	if (!check_signed_hash(&hash, &signature, &node_id)) {
		log_debug(rstate->base_log,
			  "Ignoring node announcement, signature verification failed.");
		tal_free(tmpctx);
		return;
	}
	node = get_node(rstate, &node_id);

	if (!node) {
		log_debug(rstate->base_log,
			  "Node not found, was the node_announcement preceeded by at least channel_announcement?");
		tal_free(tmpctx);
		return;
	} else if (node->last_timestamp >= timestamp) {
		log_debug(rstate->base_log,
			  "Ignoring node announcement, it's outdated.");
		tal_free(tmpctx);
		return;
	}

	node->last_timestamp = timestamp;
	node->hostname = tal_free(node->hostname);
	if (!read_ip(node, addresses, &node->hostname, &node->port)) {
		/* FIXME: SHOULD fail connection here. */
		tal_free(serialized);
		return;
	}
	memcpy(node->rgb_color, rgb_color, 3);

	u8 *tag = tal_arr(tmpctx, u8, 0);
	towire_pubkey(&tag, &node_id);
	queue_broadcast(rstate->broadcasts,
			WIRE_NODE_ANNOUNCEMENT,
			tag,
			serialized);
	tal_free(node->node_announcement);
	node->node_announcement = tal_steal(node, serialized);
	tal_free(tmpctx);
}

static void broadcast_channel_update(struct lightningd_state *dstate, struct peer *peer)
{
	struct txlocator *loc;
	u8 *serialized;
	secp256k1_ecdsa_signature signature;
	struct channel_id channel_id;
	u32 timestamp = time_now().ts.tv_sec;
	const tal_t *tmpctx = tal_tmpctx(dstate);

	loc = locate_tx(tmpctx, dstate, &peer->anchor.txid);
	channel_id.blocknum = loc->blkheight;
	channel_id.txnum = loc->index;
	channel_id.outnum = peer->anchor.index;

	/* Avoid triggering memcheck */
	memset(&signature, 0, sizeof(signature));

	serialized = towire_channel_update(tmpctx, &signature, &channel_id,
					   timestamp,
					   pubkey_cmp(&dstate->id, peer->id) > 0,
					   dstate->config.min_htlc_expiry,
	//FIXME(cdecker) Make the minimum HTLC configurable
					   1,
					   dstate->config.fee_base,
					   dstate->config.fee_per_satoshi);
	privkey_sign(dstate, serialized + 66, tal_count(serialized) - 66,
		     &signature);
	serialized = towire_channel_update(tmpctx, &signature, &channel_id,
					   timestamp,
					   pubkey_cmp(&dstate->id, peer->id) > 0,
					   dstate->config.min_htlc_expiry,
					   1,
					   dstate->config.fee_base,
					   dstate->config.fee_per_satoshi);
	u8 *tag = tal_arr(tmpctx, u8, 0);
	towire_channel_id(&tag, &channel_id);
	queue_broadcast(dstate->rstate->broadcasts, WIRE_CHANNEL_UPDATE, tag, serialized);
	tal_free(tmpctx);
}

static void broadcast_node_announcement(struct lightningd_state *dstate)
{
	u8 *serialized;
	secp256k1_ecdsa_signature signature;
	static const u8 rgb_color[3];
	static const u8 alias[32];
	u32 timestamp = time_now().ts.tv_sec;
	const tal_t *tmpctx = tal_tmpctx(dstate);
	u8 *address;

	/* Are we listening for incoming connections at all? */
	if (!dstate->external_ip || !dstate->portnum) {
		tal_free(tmpctx);
		return;
	}

	/* Avoid triggering memcheck */
	memset(&signature, 0, sizeof(signature));

	address = write_ip(tmpctx, dstate->external_ip, dstate->portnum);
	serialized = towire_node_announcement(tmpctx, &signature,
					      timestamp,
					      &dstate->id, rgb_color, alias,
					      NULL,
					      address);
	privkey_sign(dstate, serialized + 66, tal_count(serialized) - 66,
		     &signature);
	serialized = towire_node_announcement(tmpctx, &signature,
					      timestamp,
					      &dstate->id, rgb_color, alias,
					      NULL,
					      address);
	u8 *tag = tal_arr(tmpctx, u8, 0);
	towire_pubkey(&tag, &dstate->id);
	queue_broadcast(dstate->rstate->broadcasts, WIRE_NODE_ANNOUNCEMENT, tag,
			serialized);
	tal_free(tmpctx);
}

static void broadcast_channel_announcement(struct lightningd_state *dstate, struct peer *peer)
{
	struct txlocator *loc;
	struct channel_id channel_id;
	secp256k1_ecdsa_signature node_signature[2];
	secp256k1_ecdsa_signature bitcoin_signature[2];
	const struct pubkey *node_id[2];
	const struct pubkey *bitcoin_key[2];
	secp256k1_ecdsa_signature *my_node_signature;
	secp256k1_ecdsa_signature *my_bitcoin_signature;
	u8 *serialized;
	const tal_t *tmpctx = tal_tmpctx(dstate);

	loc = locate_tx(tmpctx, dstate, &peer->anchor.txid);

	channel_id.blocknum = loc->blkheight;
	channel_id.txnum = loc->index;
	channel_id.outnum = peer->anchor.index;

	/* Set all sigs to zero */
	memset(node_signature, 0, sizeof(node_signature));
	memset(bitcoin_signature, 0, sizeof(bitcoin_signature));

	//FIXME(cdecker) Copy remote stored signatures into place
	if (pubkey_cmp(&dstate->id, peer->id) > 0) {
		node_id[0] = peer->id;
		node_id[1] = &dstate->id;
		bitcoin_key[0] = peer->id;
		bitcoin_key[1] = &dstate->id;
		my_node_signature = &node_signature[1];
		my_bitcoin_signature = &bitcoin_signature[1];
	} else {
		node_id[1] = peer->id;
		node_id[0] = &dstate->id;
		bitcoin_key[1] = peer->id;
		bitcoin_key[0] = &dstate->id;
		my_node_signature = &node_signature[0];
		my_bitcoin_signature = &bitcoin_signature[0];
	}

	/* Sign the node_id with the bitcoin_key, proves delegation */
	serialized = tal_arr(tmpctx, u8, 0);
	towire_pubkey(&serialized, &dstate->id);
	privkey_sign(dstate, serialized, tal_count(serialized), my_bitcoin_signature);

	/* BOLT #7:
	 *
	 * The creating node MUST compute the double-SHA256 hash `h` of the
	 * message, starting at offset 256, up to the end of the message.
	 */
	serialized = towire_channel_announcement(tmpctx, &node_signature[0],
						 &node_signature[1],
						 &bitcoin_signature[0],
						 &bitcoin_signature[1],
						 &channel_id,
						 node_id[0],
						 node_id[1],
						 bitcoin_key[0],
						 bitcoin_key[1],
						 NULL);
	privkey_sign(dstate, serialized + 256, tal_count(serialized) - 256, my_node_signature);

	serialized = towire_channel_announcement(tmpctx, &node_signature[0],
						 &node_signature[1],
						 &bitcoin_signature[0],
						 &bitcoin_signature[1],
						 &channel_id,
						 node_id[0],
						 node_id[1],
						 bitcoin_key[0],
						 bitcoin_key[1],
						 NULL);
	u8 *tag = tal_arr(tmpctx, u8, 0);
	towire_channel_id(&tag, &channel_id);
	queue_broadcast(dstate->rstate->broadcasts, WIRE_CHANNEL_ANNOUNCEMENT,
			tag, serialized);
	tal_free(tmpctx);
}

static void announce(struct lightningd_state *dstate)
{
	struct peer *p;
	int nchan = 0;

	new_reltimer(dstate, dstate, time_from_sec(5*60*60), announce, dstate);

	list_for_each(&dstate->peers, p, list) {
		if (state_is_normal(p->state)) {
			broadcast_channel_announcement(dstate, p);
			broadcast_channel_update(dstate, p);
			nchan += 1;
		}
	}

	/* No point in broadcasting our node if we don't have a channel */
	if (nchan > 0)
		broadcast_node_announcement(dstate);
}

void announce_channel(struct lightningd_state *dstate, struct peer *peer)
{
	broadcast_channel_announcement(dstate, peer);
	broadcast_channel_update(dstate, peer);
	broadcast_node_announcement(dstate);

}

static void process_broadcast_queue(struct lightningd_state *dstate)
{
	struct peer *p;
	struct queued_message *msg;
	new_reltimer(dstate, dstate, time_from_sec(30), process_broadcast_queue, dstate);
	list_for_each(&dstate->peers, p, list) {
		if (!state_is_normal(p->state))
			continue;
		msg = next_broadcast_message(dstate->rstate->broadcasts,
					     &p->broadcast_index);
		while (msg != NULL) {
			queue_pkt_nested(p, msg->type, msg->payload);
			msg = next_broadcast_message(dstate->rstate->broadcasts,
						     &p->broadcast_index);
		}
	}
}

void setup_p2p_announce(struct lightningd_state *dstate)
{
	new_reltimer(dstate, dstate, time_from_sec(5*60*60), announce, dstate);
	new_reltimer(dstate, dstate, time_from_sec(30), process_broadcast_queue, dstate);
}
