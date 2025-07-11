/*
 * Copyright (c) 2022-2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/autoconf.h>
#include <zephyr/bluetooth/audio/audio.h>
#include <zephyr/bluetooth/audio/bap_lc3_preset.h>
#include <zephyr/bluetooth/audio/cap.h>
#include <zephyr/bluetooth/audio/bap.h>
#include <zephyr/bluetooth/audio/lc3.h>
#include <zephyr/bluetooth/audio/tbs.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/byteorder.h>
#include <zephyr/bluetooth/crypto.h>
#include <zephyr/bluetooth/gap.h>
#include <zephyr/bluetooth/iso.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/kernel.h>
#include <zephyr/net_buf.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>
#include <zephyr/sys/util_macro.h>
#include <zephyr/toolchain.h>

#include "bap_common.h"
#include "bap_stream_tx.h"
#include "bstests.h"
#include "common.h"

#if defined(CONFIG_BT_CAP_INITIATOR) && defined(CONFIG_BT_BAP_BROADCAST_SOURCE)
CREATE_FLAG(flag_source_started);

#define CAP_AC_MAX_STREAM       2
#define LOCATION                (BT_AUDIO_LOCATION_FRONT_LEFT | BT_AUDIO_LOCATION_FRONT_RIGHT)
#define CONTEXT                 (BT_AUDIO_CONTEXT_TYPE_MEDIA)
#define BROADCAST_STREAM_CNT    MIN(CAP_AC_MAX_STREAM, CONFIG_BT_BAP_BROADCAST_SRC_STREAM_COUNT)

struct cap_initiator_ac_param {
	char *name;
	size_t stream_cnt;
	size_t chan_cnt;
	const struct named_lc3_preset *named_preset;
};
static const struct named_lc3_preset *named_preset;

extern enum bst_result_t bst_result;
static struct audio_test_stream broadcast_source_streams[BROADCAST_STREAM_CNT];
static struct bt_cap_stream *broadcast_streams[ARRAY_SIZE(broadcast_source_streams)];
static struct bt_bap_lc3_preset broadcast_preset_16_2_1 =
	BT_BAP_LC3_BROADCAST_PRESET_16_2_1(LOCATION, CONTEXT);
static size_t stream_count;

static K_SEM_DEFINE(sem_broadcast_stream_started, 0U, ARRAY_SIZE(broadcast_streams));
static K_SEM_DEFINE(sem_broadcast_stream_stopped, 0U, ARRAY_SIZE(broadcast_streams));

static const struct named_lc3_preset lc3_broadcast_presets[] = {
	{"8_1_1", BT_BAP_LC3_BROADCAST_PRESET_8_1_1(LOCATION, CONTEXT)},
	{"8_2_1", BT_BAP_LC3_BROADCAST_PRESET_8_2_1(LOCATION, CONTEXT)},
	{"16_1_1", BT_BAP_LC3_BROADCAST_PRESET_16_1_1(LOCATION, CONTEXT)},
	{"16_2_1", BT_BAP_LC3_BROADCAST_PRESET_16_2_1(LOCATION, CONTEXT)},
	{"24_1_1", BT_BAP_LC3_BROADCAST_PRESET_24_1_1(LOCATION, CONTEXT)},
	{"24_2_1", BT_BAP_LC3_BROADCAST_PRESET_24_2_1(LOCATION, CONTEXT)},
	{"32_1_1", BT_BAP_LC3_BROADCAST_PRESET_32_1_1(LOCATION, CONTEXT)},
	{"32_2_1", BT_BAP_LC3_BROADCAST_PRESET_32_2_1(LOCATION, CONTEXT)},
	{"441_1_1", BT_BAP_LC3_BROADCAST_PRESET_441_1_1(LOCATION, CONTEXT)},
	{"441_2_1", BT_BAP_LC3_BROADCAST_PRESET_441_2_1(LOCATION, CONTEXT)},
	{"48_1_1", BT_BAP_LC3_BROADCAST_PRESET_48_1_1(LOCATION, CONTEXT)},
	{"48_2_1", BT_BAP_LC3_BROADCAST_PRESET_48_2_1(LOCATION, CONTEXT)},
	{"48_3_1", BT_BAP_LC3_BROADCAST_PRESET_48_3_1(LOCATION, CONTEXT)},
	{"48_4_1", BT_BAP_LC3_BROADCAST_PRESET_48_4_1(LOCATION, CONTEXT)},
	{"48_5_1", BT_BAP_LC3_BROADCAST_PRESET_48_5_1(LOCATION, CONTEXT)},
	{"48_6_1", BT_BAP_LC3_BROADCAST_PRESET_48_6_1(LOCATION, CONTEXT)},
	/* High-reliability presets */
	{"8_1_2", BT_BAP_LC3_BROADCAST_PRESET_8_1_2(LOCATION, CONTEXT)},
	{"8_2_2", BT_BAP_LC3_BROADCAST_PRESET_8_2_2(LOCATION, CONTEXT)},
	{"16_1_2", BT_BAP_LC3_BROADCAST_PRESET_16_1_2(LOCATION, CONTEXT)},
	{"16_2_2", BT_BAP_LC3_BROADCAST_PRESET_16_2_2(LOCATION, CONTEXT)},
	{"24_1_2", BT_BAP_LC3_BROADCAST_PRESET_24_1_2(LOCATION, CONTEXT)},
	{"24_2_2", BT_BAP_LC3_BROADCAST_PRESET_24_2_2(LOCATION, CONTEXT)},
	{"32_1_2", BT_BAP_LC3_BROADCAST_PRESET_32_1_2(LOCATION, CONTEXT)},
	{"32_2_2", BT_BAP_LC3_BROADCAST_PRESET_32_2_2(LOCATION, CONTEXT)},
	{"441_1_2", BT_BAP_LC3_BROADCAST_PRESET_441_1_2(LOCATION, CONTEXT)},
	{"441_2_2", BT_BAP_LC3_BROADCAST_PRESET_441_2_2(LOCATION, CONTEXT)},
	{"48_1_2", BT_BAP_LC3_BROADCAST_PRESET_48_1_2(LOCATION, CONTEXT)},
	{"48_2_2", BT_BAP_LC3_BROADCAST_PRESET_48_2_2(LOCATION, CONTEXT)},
	{"48_3_2", BT_BAP_LC3_BROADCAST_PRESET_48_3_2(LOCATION, CONTEXT)},
	{"48_4_2", BT_BAP_LC3_BROADCAST_PRESET_48_4_2(LOCATION, CONTEXT)},
	{"48_5_2", BT_BAP_LC3_BROADCAST_PRESET_48_5_2(LOCATION, CONTEXT)},
	{"48_6_2", BT_BAP_LC3_BROADCAST_PRESET_48_6_2(LOCATION, CONTEXT)},
};

static void broadcast_stream_started_cb(struct bt_bap_stream *stream)
{
	struct audio_test_stream *test_stream = audio_test_stream_from_bap_stream(stream);
	int err;

	test_stream->seq_num = 0U;
	test_stream->tx_cnt = 0U;

	printk("Stream %p started\n", stream);

	err = bap_stream_tx_register(stream);
	if (err != 0) {
		FAIL("Failed to register stream %p for TX: %d\n", stream, err);
		return;
	}

	k_sem_give(&sem_broadcast_stream_started);
}

static void broadcast_stream_stopped_cb(struct bt_bap_stream *stream, uint8_t reason)
{
	int err;

	printk("Stream %p stopped with reason 0x%02X\n", stream, reason);

	err = bap_stream_tx_unregister(stream);
	if (err != 0) {
		FAIL("Failed to unregister stream %p for TX: %d\n", stream, err);
		return;
	}

	k_sem_give(&sem_broadcast_stream_stopped);
}

static struct bt_bap_stream_ops broadcast_stream_ops = {
	.started = broadcast_stream_started_cb,
	.stopped = broadcast_stream_stopped_cb,
	.sent = bap_stream_tx_sent_cb,
};

static void broadcast_source_started_cb(struct bt_cap_broadcast_source *broadcast_source)
{
	printk("Broadcast source %p started\n", broadcast_source);
	SET_FLAG(flag_source_started);
}

static void broadcast_source_stopped_cb(struct bt_cap_broadcast_source *broadcast_source,
					uint8_t reason)
{
	printk("Broadcast source %p stopped with reason 0x%02X\n", broadcast_source, reason);
	UNSET_FLAG(flag_source_started);
}

static void init(void)
{
	static struct bt_cap_initiator_cb broadcast_cbs = {
		.broadcast_started = broadcast_source_started_cb,
		.broadcast_stopped = broadcast_source_stopped_cb,
	};
	int err;

	err = bt_enable(NULL);
	if (err != 0) {
		FAIL("Bluetooth enable failed (err %d)\n", err);
		return;
	}

	printk("Bluetooth initialized\n");
	bap_stream_tx_init();

	(void)memset(broadcast_source_streams, 0, sizeof(broadcast_source_streams));

	for (size_t i = 0; i < ARRAY_SIZE(broadcast_streams); i++) {
		broadcast_streams[i] =
			cap_stream_from_audio_test_stream(&broadcast_source_streams[i]);
		bt_cap_stream_ops_register(broadcast_streams[i], &broadcast_stream_ops);
	}

	if (IS_ENABLED(CONFIG_BT_TBS)) {
		const struct bt_tbs_register_param gtbs_param = {
			.provider_name = "Generic TBS",
			.uci = "un000",
			.uri_schemes_supported = "tel,skype",
			.gtbs = true,
			.authorization_required = false,
			.technology = BT_TBS_TECHNOLOGY_3G,
			.supported_features = CONFIG_BT_TBS_SUPPORTED_FEATURES,
		};

		err = bt_tbs_register_bearer(&gtbs_param);
		if (err < 0) {
			FAIL("Failed to register GTBS (err %d)\n", err);

			return;
		}

		printk("Registered GTBS\n");
	}

	err = bt_cap_initiator_register_cb(&broadcast_cbs);
	if (err != 0) {
		FAIL("Failed to register broadcast callbacks: %d\n", err);
		return;
	}
}

static void setup_extended_adv_data(struct bt_cap_broadcast_source *source,
				    struct bt_le_ext_adv *adv)
{
	/* Broadcast Audio Streaming Endpoint advertising data */
	NET_BUF_SIMPLE_DEFINE(ad_buf, BT_UUID_SIZE_16 + BT_AUDIO_BROADCAST_ID_SIZE);
	NET_BUF_SIMPLE_DEFINE(base_buf, 128);
	struct bt_data ext_ad;
	struct bt_data per_ad;
	uint32_t broadcast_id;
	int err;

	err = bt_rand(&broadcast_id, BT_AUDIO_BROADCAST_ID_SIZE);
	if (err) {
		FAIL("Unable to generate broadcast ID: %d\n", err);
		return;
	}

	/* Setup extended advertising data */
	net_buf_simple_add_le16(&ad_buf, BT_UUID_BROADCAST_AUDIO_VAL);
	net_buf_simple_add_le24(&ad_buf, broadcast_id);
	ext_ad.type = BT_DATA_SVC_DATA16;
	ext_ad.data_len = ad_buf.len;
	ext_ad.data = ad_buf.data;
	err = bt_le_ext_adv_set_data(adv, &ext_ad, 1, NULL, 0);
	if (err != 0) {
		FAIL("Failed to set extended advertising data: %d\n", err);
		return;
	}

	/* Setup periodic advertising data */
	err = bt_cap_initiator_broadcast_get_base(source, &base_buf);
	if (err != 0) {
		FAIL("Failed to get encoded BASE: %d\n", err);
		return;
	}

	per_ad.type = BT_DATA_SVC_DATA16;
	per_ad.data_len = base_buf.len;
	per_ad.data = base_buf.data;
	err = bt_le_per_adv_set_data(adv, &per_ad, 1);
	if (err != 0) {
		FAIL("Failed to set periodic advertising data: %d\n", err);
		return;
	}
}

static void stop_and_delete_extended_adv(struct bt_le_ext_adv *adv)
{
	int err;

	/* Stop extended advertising */
	err = bt_le_per_adv_stop(adv);
	if (err) {
		FAIL("Failed to stop periodic advertising: %d\n", err);
		return;
	}

	err = bt_le_ext_adv_stop(adv);
	if (err) {
		FAIL("Failed to stop extended advertising: %d\n", err);
		return;
	}

	err = bt_le_ext_adv_delete(adv);
	if (err) {
		FAIL("Failed to delete extended advertising: %d\n", err);
		return;
	}
}

static void test_broadcast_audio_create_inval(void)
{
	uint8_t bis_codec_data[] = {3, BT_AUDIO_CODEC_CFG_FREQ,
				    BT_BYTES_LIST_LE16(BT_AUDIO_CODEC_CFG_FREQ_16KHZ)};
	struct bt_cap_initiator_broadcast_stream_param
		stream_params[ARRAY_SIZE(broadcast_source_streams)];
	struct bt_cap_initiator_broadcast_subgroup_param subgroup_param;
	struct bt_cap_initiator_broadcast_create_param create_param;
	struct bt_cap_broadcast_source *broadcast_sources[CONFIG_BT_BAP_BROADCAST_SRC_COUNT + 1];
	struct bt_audio_codec_cfg invalid_codec = BT_AUDIO_CODEC_LC3_CONFIG(
		BT_AUDIO_CODEC_CFG_FREQ_16KHZ, BT_AUDIO_CODEC_CFG_DURATION_10,
		BT_AUDIO_LOCATION_FRONT_LEFT, 40U, 1, BT_AUDIO_CONTEXT_TYPE_MEDIA);
	int err;

	for (size_t i = 0U; i < ARRAY_SIZE(broadcast_streams); i++) {
		stream_params[i].stream =
			cap_stream_from_audio_test_stream(&broadcast_source_streams[i]);
		stream_params[i].data_len = ARRAY_SIZE(bis_codec_data);
		stream_params[i].data = bis_codec_data;
	}

	subgroup_param.stream_count = ARRAY_SIZE(broadcast_streams);
	subgroup_param.stream_params = stream_params;
	subgroup_param.codec_cfg = &broadcast_preset_16_2_1.codec_cfg;

	create_param.subgroup_count = 1U;
	create_param.subgroup_params = &subgroup_param;
	create_param.qos = &broadcast_preset_16_2_1.qos;
	create_param.packing = BT_ISO_PACKING_SEQUENTIAL;
	create_param.encryption = false;

	/* Test NULL parameters */
	err = bt_cap_initiator_broadcast_audio_create(NULL, &broadcast_sources[0]);
	if (err == 0) {
		FAIL("bt_cap_initiator_broadcast_audio_create with NULL param did not fail\n");
		return;
	}

	err = bt_cap_initiator_broadcast_audio_create(&create_param, NULL);
	if (err == 0) {
		FAIL("bt_cap_initiator_broadcast_audio_create with NULL broadcast source did not "
		     "fail\n");
		return;
	}

	/* Clear metadata so that it does not contain the mandatory stream context */
	memset(&invalid_codec.meta, 0, sizeof(invalid_codec.meta));
	subgroup_param.codec_cfg = &invalid_codec;
	err = bt_cap_initiator_broadcast_audio_create(&create_param, NULL);
	if (err == 0) {
		FAIL("bt_cap_initiator_broadcast_audio_create with invalid metadata did not "
		     "fail\n");
		return;
	}
	subgroup_param.codec_cfg = &broadcast_preset_16_2_1.codec_cfg;

	/* Test allocating too many sources */
	ARRAY_FOR_EACH(broadcast_sources, i) {
		err = bt_cap_initiator_broadcast_audio_create(&create_param, &broadcast_sources[i]);
		if (i < CONFIG_BT_BAP_BROADCAST_SRC_COUNT) {
			if (err != 0) {
				FAIL("[%zu]: bt_cap_initiator_broadcast_audio_create failed: %d\n",
				     i, err);
				return;
			}
		} else {
			if (err != -ENOMEM) {
				FAIL("bt_cap_initiator_broadcast_audio_create did not fail with "
				     "-ENOMEM when allocating too many sources: %d\n",
				     err);
				return;
			}
		}
	}

	/* Cleanup the created broadcast sources */
	ARRAY_FOR_EACH(broadcast_sources, i) {
		if (i < CONFIG_BT_BAP_BROADCAST_SRC_COUNT) {
			err = bt_cap_initiator_broadcast_audio_delete(broadcast_sources[i]);
			if (err != 0) {
				FAIL("[%zu]: bt_cap_initiator_broadcast_audio_delete failed: %d\n",
				     i, err);
				return;
			}
		}
	}

	/* Since we are just casting the CAP parameters to BAP parameters,
	 * we can rely on the BAP tests to verify the values
	 */
}

static void test_broadcast_audio_create(struct bt_cap_broadcast_source **broadcast_source)
{
	uint8_t bis_codec_data[] = {3, BT_AUDIO_CODEC_CFG_FREQ,
				    BT_BYTES_LIST_LE16(BT_AUDIO_CODEC_CFG_FREQ_16KHZ)};
	struct bt_cap_initiator_broadcast_stream_param
		stream_params[ARRAY_SIZE(broadcast_source_streams)];
	struct bt_cap_initiator_broadcast_subgroup_param subgroup_param;
	struct bt_cap_initiator_broadcast_create_param create_param;
	int err;

	for (size_t i = 0; i < ARRAY_SIZE(broadcast_streams); i++) {
		stream_params[i].stream =
			cap_stream_from_audio_test_stream(&broadcast_source_streams[i]);
		stream_params[i].data_len = ARRAY_SIZE(bis_codec_data);
		stream_params[i].data = bis_codec_data;
	}

	subgroup_param.stream_count = ARRAY_SIZE(broadcast_streams);
	subgroup_param.stream_params = stream_params;
	subgroup_param.codec_cfg = &broadcast_preset_16_2_1.codec_cfg;

	create_param.subgroup_count = 1U;
	create_param.subgroup_params = &subgroup_param;
	create_param.qos = &broadcast_preset_16_2_1.qos;
	create_param.packing = BT_ISO_PACKING_SEQUENTIAL;
	create_param.encryption = false;

	printk("Creating broadcast source with %zu broadcast_streams\n",
	       ARRAY_SIZE(broadcast_streams));

	err = bt_cap_initiator_broadcast_audio_create(&create_param, broadcast_source);
	if (err != 0) {
		FAIL("Unable to start broadcast source: %d\n", err);
		return;
	}

	for (size_t i = 0U; i < ARRAY_SIZE(broadcast_source_streams); i++) {
		struct audio_test_stream *test_stream = &broadcast_source_streams[i];

		test_stream->tx_sdu_size = create_param.qos->sdu;
	}

	printk("Broadcast source created with %zu broadcast_streams\n",
	       ARRAY_SIZE(broadcast_streams));

	stream_count = ARRAY_SIZE(broadcast_streams);
}

static void test_broadcast_audio_start_inval(struct bt_cap_broadcast_source *broadcast_source,
					     struct bt_le_ext_adv *adv)
{
	int err;

	/* Test NULL parameters */
	err = bt_cap_initiator_broadcast_audio_start(NULL, adv);
	if (err == 0) {
		FAIL("bt_cap_initiator_broadcast_audio_start with NULL broadcast source did not "
		     "fail\n");
		return;
	}

	err = bt_cap_initiator_broadcast_audio_start(broadcast_source, NULL);
	if (err == 0) {
		FAIL("bt_cap_initiator_broadcast_audio_start with NULL adv did not fail\n");
		return;
	}
}

static void test_broadcast_audio_start(struct bt_cap_broadcast_source *broadcast_source,
				       struct bt_le_ext_adv *adv)
{
	int err;

	err = bt_cap_initiator_broadcast_audio_start(broadcast_source, adv);
	if (err != 0) {
		FAIL("Unable to start broadcast source: %d\n", err);
		return;
	}

	printk("Broadcast source created with %zu broadcast_streams\n",
	       ARRAY_SIZE(broadcast_streams));
}

static void test_broadcast_audio_update_inval(struct bt_cap_broadcast_source *broadcast_source)
{
	const uint8_t new_metadata[] = {
		BT_AUDIO_CODEC_DATA(BT_AUDIO_METADATA_TYPE_STREAM_CONTEXT,
				    BT_BYTES_LIST_LE16(BT_AUDIO_CONTEXT_TYPE_MEDIA)),
		BT_AUDIO_CODEC_DATA(BT_AUDIO_METADATA_TYPE_PARENTAL_RATING,
				    BT_AUDIO_PARENTAL_RATING_AGE_ANY),
	};
	const uint8_t invalid_metadata[] = {
		BT_AUDIO_CODEC_DATA(BT_AUDIO_METADATA_TYPE_PARENTAL_RATING,
				    BT_AUDIO_PARENTAL_RATING_AGE_ANY),
	};
	int err;

	/* Test NULL parameters */
	err = bt_cap_initiator_broadcast_audio_update(NULL, new_metadata, ARRAY_SIZE(new_metadata));
	if (err == 0) {
		FAIL("bt_cap_initiator_broadcast_audio_update with NULL broadcast source did not "
		     "fail\n");
		return;
	}

	err = bt_cap_initiator_broadcast_audio_update(broadcast_source, NULL,
						      ARRAY_SIZE(new_metadata));
	if (err == 0) {
		FAIL("bt_cap_initiator_broadcast_audio_update with NULL metadata did not fail\n");
		return;
	}

	err = bt_cap_initiator_broadcast_audio_update(broadcast_source, new_metadata, 0);
	if (err == 0) {
		FAIL("bt_cap_initiator_broadcast_audio_update with 0 metadata count did not "
		     "fail\n");
		return;
	}

	/* Test with metadata without streaming context */
	err = bt_cap_initiator_broadcast_audio_update(broadcast_source, invalid_metadata,
						      ARRAY_SIZE(invalid_metadata));
	if (err == 0) {
		FAIL("bt_cap_initiator_broadcast_audio_update with invalid metadata did not "
		     "fail\n");
		return;
	}

	printk("Broadcast metadata updated\n");
}

static void test_broadcast_audio_update(struct bt_cap_broadcast_source *broadcast_source)
{
#if defined(CONFIG_BT_TBS)
	/* TODO: We do not have a way to get the CCID value of GTBS, but for now set to 0x00 as we
	 * know that it is the first content control service initialized
	 */
	const uint16_t gtbs_ccid = 0x00;
#endif /* CONFIG_BT_TBS */
	const uint8_t new_metadata[] = {
		BT_AUDIO_CODEC_DATA(BT_AUDIO_METADATA_TYPE_STREAM_CONTEXT,
				    BT_BYTES_LIST_LE16(BT_AUDIO_CONTEXT_TYPE_CONVERSATIONAL)),
#if defined(CONFIG_BT_TBS)
		BT_AUDIO_CODEC_DATA(BT_AUDIO_METADATA_TYPE_CCID_LIST, gtbs_ccid),
#endif /* CONFIG_BT_TBS */
	};
	int err;

	printk("Updating broadcast metadata\n");

	err = bt_cap_initiator_broadcast_audio_update(broadcast_source, new_metadata,
						      ARRAY_SIZE(new_metadata));
	if (err != 0) {
		FAIL("Failed to update broadcast source metadata: %d\n", err);
		return;
	}

	printk("Broadcast metadata updated\n");
}

static void test_broadcast_audio_stop_inval(void)
{
	int err;

	/* Test NULL parameters */
	err = bt_cap_initiator_broadcast_audio_stop(NULL);
	if (err == 0) {
		FAIL("bt_cap_initiator_broadcast_audio_stop with NULL broadcast source did not "
		     "fail\n");
		return;
	}
}

static void test_broadcast_audio_tx_sync(void)
{
	for (size_t i = 0U; i < stream_count; i++) {
		struct bt_cap_stream *cap_stream = broadcast_streams[i];
		struct bt_iso_tx_info info;
		int err;

		err = bt_cap_stream_get_tx_sync(cap_stream, &info);
		if (err != 0) {
			FAIL("Failed to get TX sync for stream[%zu]: %p: %d\n", i, cap_stream, err);
			return;
		}

		if (info.seq_num != 0) {
			printk("stream[%zu]: %p seq_num: %u\n", i, cap_stream, info.seq_num);
		} else {
			FAIL("stream[%zu]: %p seq_num was 0\n", i, cap_stream);
			return;
		}
	}
}

static void test_broadcast_audio_stop(struct bt_cap_broadcast_source *broadcast_source)
{
	int err;

	printk("Stopping broadcast source\n");

	err = bt_cap_initiator_broadcast_audio_stop(broadcast_source);
	if (err != 0) {
		FAIL("Failed to stop broadcast source: %d\n", err);
		return;
	}

	/* Wait for all to be stopped */
	printk("Waiting for broadcast_streams to be stopped\n");
	for (size_t i = 0U; i < stream_count; i++) {
		k_sem_take(&sem_broadcast_stream_stopped, K_FOREVER);
	}

	WAIT_FOR_UNSET_FLAG(flag_source_started);

	printk("Broadcast source stopped\n");

	/* Verify that it cannot be stopped twice */
	err = bt_cap_initiator_broadcast_audio_stop(broadcast_source);
	if (err == 0) {
		FAIL("bt_cap_initiator_broadcast_audio_stop with already-stopped broadcast source "
		     "did not fail\n");
		return;
	}
}

static void test_broadcast_audio_delete_inval(void)
{
	int err;

	/* Test NULL parameters */
	err = bt_cap_initiator_broadcast_audio_delete(NULL);
	if (err == 0) {
		FAIL("bt_cap_initiator_broadcast_audio_delete with NULL broadcast source did not "
		     "fail\n");
		return;
	}
}

static void test_broadcast_audio_delete(struct bt_cap_broadcast_source *broadcast_source)
{
	int err;

	printk("Deleting broadcast source\n");

	err = bt_cap_initiator_broadcast_audio_delete(broadcast_source);
	if (err != 0) {
		FAIL("Failed to stop broadcast source: %d\n", err);
		return;
	}

	printk("Broadcast source deleted\n");

	/* Verify that it cannot be deleted twice */
	err = bt_cap_initiator_broadcast_audio_delete(broadcast_source);
	if (err == 0) {
		FAIL("bt_cap_initiator_broadcast_audio_delete with already-deleted broadcast "
		     "source did not fail\n");
		return;
	}
}

static void test_main_cap_initiator_broadcast(void)
{
	struct bt_cap_broadcast_source *broadcast_source;
	struct bt_le_ext_adv *adv;

	(void)memset(broadcast_source_streams, 0, sizeof(broadcast_source_streams));

	init();

	setup_broadcast_adv(&adv);

	test_broadcast_audio_create(&broadcast_source);

	test_broadcast_audio_start(broadcast_source, adv);

	setup_extended_adv_data(broadcast_source, adv);

	start_broadcast_adv(adv);

	/* Wait for all to be started */
	printk("Waiting for broadcast_streams to be started\n");
	for (size_t i = 0U; i < stream_count; i++) {
		k_sem_take(&sem_broadcast_stream_started, K_FOREVER);
	}

	WAIT_FOR_FLAG(flag_source_started);

	/* Wait for other devices to have received the data they wanted */
	printk("Waiting for broadcast stop signal");
	backchannel_sync_wait_all();

	test_broadcast_audio_tx_sync();

	test_broadcast_audio_stop(broadcast_source);

	test_broadcast_audio_delete(broadcast_source);
	broadcast_source = NULL;

	stop_and_delete_extended_adv(adv);
	adv = NULL;

	PASS("CAP initiator broadcast passed\n");
}

static void test_main_cap_initiator_broadcast_inval(void)
{
	struct bt_cap_broadcast_source *broadcast_source;
	struct bt_le_ext_adv *adv;

	(void)memset(broadcast_source_streams, 0, sizeof(broadcast_source_streams));

	init();

	setup_broadcast_adv(&adv);

	test_broadcast_audio_create_inval();
	test_broadcast_audio_create(&broadcast_source);

	test_broadcast_audio_start_inval(broadcast_source, adv);
	test_broadcast_audio_start(broadcast_source, adv);

	setup_extended_adv_data(broadcast_source, adv);

	start_broadcast_adv(adv);

	/* Wait for all to be started */
	printk("Waiting for broadcast_streams to be started\n");
	for (size_t i = 0U; i < stream_count; i++) {
		k_sem_take(&sem_broadcast_stream_started, K_FOREVER);
	}

	WAIT_FOR_FLAG(flag_source_started);

	test_broadcast_audio_update_inval(broadcast_source);
	test_broadcast_audio_update(broadcast_source);

	/* Keeping running for a little while */
	k_sleep(K_SECONDS(5));

	test_broadcast_audio_stop_inval();
	test_broadcast_audio_stop(broadcast_source);

	test_broadcast_audio_delete_inval();
	test_broadcast_audio_delete(broadcast_source);
	broadcast_source = NULL;

	stop_and_delete_extended_adv(adv);
	adv = NULL;

	PASS("CAP initiator broadcast inval passed\n");
}

static void test_main_cap_initiator_broadcast_update(void)
{
	struct bt_cap_broadcast_source *broadcast_source;
	struct bt_le_ext_adv *adv;

	(void)memset(broadcast_source_streams, 0, sizeof(broadcast_source_streams));

	init();

	setup_broadcast_adv(&adv);

	test_broadcast_audio_create(&broadcast_source);

	test_broadcast_audio_start(broadcast_source, adv);

	setup_extended_adv_data(broadcast_source, adv);

	start_broadcast_adv(adv);

	/* Wait for all to be started */
	printk("Waiting for broadcast_streams to be started\n");
	for (size_t i = 0U; i < stream_count; i++) {
		k_sem_take(&sem_broadcast_stream_started, K_FOREVER);
	}

	WAIT_FOR_FLAG(flag_source_started);

	/* Wait for other devices to have received the data they wanted */
	backchannel_sync_wait_any();

	test_broadcast_audio_update(broadcast_source);

	/* Wait for other devices to have received the update */
	backchannel_sync_wait_any();

	test_broadcast_audio_tx_sync();

	test_broadcast_audio_stop(broadcast_source);

	test_broadcast_audio_delete(broadcast_source);
	broadcast_source = NULL;

	stop_and_delete_extended_adv(adv);
	adv = NULL;

	PASS("CAP initiator broadcast passed\n");
}

static int test_cap_initiator_ac(const struct cap_initiator_ac_param *param)
{
	struct bt_cap_initiator_broadcast_stream_param stream_params[CAP_AC_MAX_STREAM] = {0};
	uint8_t stereo_data[] = {
		BT_AUDIO_CODEC_DATA(BT_AUDIO_CODEC_CFG_CHAN_ALLOC,
				    BT_AUDIO_LOCATION_FRONT_RIGHT | BT_AUDIO_LOCATION_FRONT_LEFT)};
	uint8_t right_data[] = {BT_AUDIO_CODEC_DATA(BT_AUDIO_CODEC_CFG_CHAN_ALLOC,
						    BT_AUDIO_LOCATION_FRONT_RIGHT)};
	uint8_t left_data[] = {BT_AUDIO_CODEC_DATA(BT_AUDIO_CODEC_CFG_CHAN_ALLOC,
						   BT_AUDIO_LOCATION_FRONT_LEFT)};
	struct bt_cap_initiator_broadcast_subgroup_param subgroup_param = {0};
	struct bt_cap_initiator_broadcast_create_param create_param = {0};
	struct bt_cap_broadcast_source *broadcast_source;
	struct bt_audio_codec_cfg codec_cfg;
	struct bt_bap_qos_cfg qos;
	struct bt_le_ext_adv *adv;
	int err;

	for (size_t i = 0U; i < param->stream_cnt; i++) {
		stream_params[i].stream =
			cap_stream_from_audio_test_stream(&broadcast_source_streams[i]);

		if (param->stream_cnt == 1U) {
			stream_params[i].data_len = ARRAY_SIZE(stereo_data);
			stream_params[i].data = stereo_data;
		} else if (i == 0U) {
			stream_params[i].data_len = ARRAY_SIZE(left_data);
			stream_params[i].data = left_data;
		} else if (i == 1U) {
			stream_params[i].data_len = ARRAY_SIZE(right_data);
			stream_params[i].data = right_data;
		}
	}

	memcpy(&codec_cfg, &named_preset->preset.codec_cfg, sizeof(codec_cfg));
	memcpy(&qos, &named_preset->preset.qos, sizeof(qos));
	qos.sdu *= param->chan_cnt;

	subgroup_param.stream_count = param->stream_cnt;
	subgroup_param.stream_params = stream_params;
	subgroup_param.codec_cfg = &codec_cfg;
	create_param.subgroup_count = 1U;
	create_param.subgroup_params = &subgroup_param;
	create_param.qos = &qos;

	init();
	setup_broadcast_adv(&adv);

	err = bt_cap_initiator_broadcast_audio_create(&create_param, &broadcast_source);
	if (err != 0) {
		FAIL("Failed to create broadcast source: %d\n", err);
		return -ENOEXEC;
	}

	stream_count = param->stream_cnt;
	for (size_t i = 0U; i < stream_count; i++) {
		struct audio_test_stream *test_stream = &broadcast_source_streams[i];

		test_stream->tx_sdu_size = create_param.qos->sdu;
	}

	test_broadcast_audio_start(broadcast_source, adv);
	setup_extended_adv_data(broadcast_source, adv);
	start_broadcast_adv(adv);

	/* Wait for all to be started */
	printk("Waiting for broadcast_streams to be started\n");
	for (size_t i = 0U; i < stream_count; i++) {
		k_sem_take(&sem_broadcast_stream_started, K_FOREVER);
	}

	WAIT_FOR_FLAG(flag_source_started);

	/* Wait for other devices to have received what they wanted */
	backchannel_sync_wait_any();

	test_broadcast_audio_stop(broadcast_source);

	test_broadcast_audio_delete(broadcast_source);
	broadcast_source = NULL;

	stop_and_delete_extended_adv(adv);
	adv = NULL;

	PASS("CAP initiator broadcast passed\n");

	return 0;
}

static void test_cap_initiator_ac_12(void)
{
	const struct cap_initiator_ac_param param = {
		.name = "ac_12",
		.stream_cnt = 1U,
		.chan_cnt = 1U,
		.named_preset = named_preset,
	};

	test_cap_initiator_ac(&param);
}

#if BROADCAST_STREAM_CNT >= CAP_AC_MAX_STREAM
static void test_cap_initiator_ac_13(void)
{
	const struct cap_initiator_ac_param param = {
		.name = "ac_13",
		.stream_cnt = 2U,
		.chan_cnt = 1U,
		.named_preset = named_preset,
	};

	test_cap_initiator_ac(&param);
}
#endif /* BROADCAST_STREAM_CNT >= CAP_AC_MAX_STREAM */

static void test_cap_initiator_ac_14(void)
{
	const struct cap_initiator_ac_param param = {
		.name = "ac_14",
		.stream_cnt = 1U,
		.chan_cnt = 2U,
		.named_preset = named_preset,
	};

	test_cap_initiator_ac(&param);
}

static const struct named_lc3_preset *cap_get_named_preset(const char *preset_arg)
{
	for (size_t i = 0U; i < ARRAY_SIZE(lc3_broadcast_presets); i++) {
		if (strcmp(preset_arg, lc3_broadcast_presets[i].name) == 0) {
			return &lc3_broadcast_presets[i];
		}
	}

	return NULL;
}

static void test_args(int argc, char *argv[])
{
	for (size_t argn = 0; argn < argc; argn++) {
		const char *arg = argv[argn];

		if (strcmp(arg, "preset") == 0) {
			const char *preset_arg = argv[++argn];

			named_preset = cap_get_named_preset(preset_arg);
			if (named_preset == NULL) {
				FAIL("Failed to get preset from %s\n", preset_arg);
			}
		} else {
			FAIL("Invalid arg: %s\n", arg);
		}
	}
}

static const struct bst_test_instance test_cap_initiator_broadcast[] = {
	{
		.test_id = "cap_initiator_broadcast",
		.test_pre_init_f = test_init,
		.test_tick_f = test_tick,
		.test_main_f = test_main_cap_initiator_broadcast,
	},
	{
		.test_id = "cap_initiator_broadcast_inval",
		.test_pre_init_f = test_init,
		.test_tick_f = test_tick,
		.test_main_f = test_main_cap_initiator_broadcast_inval,
	},
	{
		.test_id = "cap_initiator_broadcast_update",
		.test_pre_init_f = test_init,
		.test_tick_f = test_tick,
		.test_main_f = test_main_cap_initiator_broadcast_update,
	},
	{
		.test_id = "cap_initiator_ac_12",
		.test_pre_init_f = test_init,
		.test_tick_f = test_tick,
		.test_main_f = test_cap_initiator_ac_12,
		.test_args_f = test_args,
	},
#if BROADCAST_STREAM_CNT >= CAP_AC_MAX_STREAM
	{
		.test_id = "cap_initiator_ac_13",
		.test_pre_init_f = test_init,
		.test_tick_f = test_tick,
		.test_main_f = test_cap_initiator_ac_13,
		.test_args_f = test_args,
	},
#endif /* BROADCAST_STREAM_CNT >= CAP_AC_MAX_STREAM */
	{
		.test_id = "cap_initiator_ac_14",
		.test_pre_init_f = test_init,
		.test_tick_f = test_tick,
		.test_main_f = test_cap_initiator_ac_14,
		.test_args_f = test_args,
	},
	BSTEST_END_MARKER,
};

struct bst_test_list *test_cap_initiator_broadcast_install(struct bst_test_list *tests)
{
	return bst_add_tests(tests, test_cap_initiator_broadcast);
}

#else /* !(defined(CONFIG_BT_CAP_INITIATOR) && defined(CONFIG_BT_BAP_BROADCAST_SOURCE)) */

struct bst_test_list *test_cap_initiator_broadcast_install(struct bst_test_list *tests)
{
	return tests;
}

#endif /* defined(CONFIG_BT_CAP_INITIATOR) && defined(CONFIG_BT_BAP_BROADCAST_SOURCE) */
