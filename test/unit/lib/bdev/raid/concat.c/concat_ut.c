/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk_internal/cunit.h"
#include "spdk/env.h"

#include "common/lib/ut_multithread.c"

#include "bdev/raid/concat.c"
#include "../common.c"

DEFINE_STUB(spdk_bdev_readv_blocks_with_md, int, (struct spdk_bdev_desc *desc,
		struct spdk_io_channel *ch,
		struct iovec *iov, int iovcnt, void *md,
		uint64_t offset_blocks, uint64_t num_blocks,
		spdk_bdev_io_completion_cb cb, void *cb_arg), 0);
DEFINE_STUB(spdk_bdev_writev_blocks_with_md, int, (struct spdk_bdev_desc *desc,
		struct spdk_io_channel *ch,
		struct iovec *iov, int iovcnt, void *md,
		uint64_t offset_blocks, uint64_t num_blocks,
		spdk_bdev_io_completion_cb cb, void *cb_arg), 0);
DEFINE_STUB(raid_bdev_remap_dix_reftag, int, (void *md_buf, uint64_t num_blocks,
		struct spdk_bdev *bdev, uint32_t remapped_offset), -1);

#define BLOCK_LEN (4096)

enum CONCAT_IO_TYPE {
	CONCAT_NONE = 0,
	CONCAT_WRITEV,
	CONCAT_READV,
	CONCAT_FLUSH,
	CONCAT_UNMAP,
};

#define MAX_RECORDS (10)
/*
 * Store the information of io requests sent to the underlying bdevs.
 * For a single null payload request to the concat bdev,
 * we may send multiple requests to the underling bdevs,
 * so we store the io request information to arrays.
 */
struct req_records {
	uint64_t offset_blocks[MAX_RECORDS];
	uint64_t num_blocks[MAX_RECORDS];
	enum CONCAT_IO_TYPE io_type[MAX_RECORDS];
	int count;
	void *md;
} g_req_records;

/*
 * g_succeed is true means the spdk_bdev_readv/writev/unmap/flush_blocks
 * functions will return 0.
 * g_succeed is false means the spdk_bdev_readv/writev/unmap/flush_blocks
 * functions will return -ENOMEM.
 * We always set it to false before an IO request, then the raid_bdev_queue_io_wait
 * function will re-submit the request, and the raid_bdev_queue_io_wait function will
 * set g_succeed to true, then the IO will succeed next time.
 */
bool g_succeed;

DEFINE_STUB_V(raid_bdev_module_list_add, (struct raid_bdev_module *raid_module));
DEFINE_STUB_V(spdk_bdev_free_io, (struct spdk_bdev_io *bdev_io));

int
spdk_bdev_readv_blocks_ext(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			   struct iovec *iov, int iovcnt, uint64_t offset_blocks, uint64_t num_blocks,
			   spdk_bdev_io_completion_cb cb, void *cb_arg, struct spdk_bdev_ext_io_opts *opts)
{
	if (g_succeed) {
		int i = g_req_records.count;

		g_req_records.offset_blocks[i] = offset_blocks;
		g_req_records.num_blocks[i] = num_blocks;
		g_req_records.io_type[i] = CONCAT_READV;
		g_req_records.count++;
		cb(NULL, true, cb_arg);
		g_req_records.md = opts->metadata;
		return 0;
	} else {
		return -ENOMEM;
	}
}

int
spdk_bdev_writev_blocks_ext(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			    struct iovec *iov, int iovcnt, uint64_t offset_blocks, uint64_t num_blocks,
			    spdk_bdev_io_completion_cb cb, void *cb_arg, struct spdk_bdev_ext_io_opts *opts)
{
	if (g_succeed) {
		int i = g_req_records.count;

		g_req_records.offset_blocks[i] = offset_blocks;
		g_req_records.num_blocks[i] = num_blocks;
		g_req_records.io_type[i] = CONCAT_WRITEV;
		g_req_records.count++;
		cb(NULL, true, cb_arg);
		g_req_records.md = opts->metadata;
		return 0;
	} else {
		return -ENOMEM;
	}
}

int
spdk_bdev_unmap_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		       uint64_t offset_blocks, uint64_t num_blocks,
		       spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	if (g_succeed) {
		int i = g_req_records.count;

		g_req_records.offset_blocks[i] = offset_blocks;
		g_req_records.num_blocks[i] = num_blocks;
		g_req_records.io_type[i] = CONCAT_UNMAP;
		g_req_records.count++;
		cb(NULL, true, cb_arg);
		return 0;
	} else {
		return -ENOMEM;
	}
}

int
spdk_bdev_flush_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		       uint64_t offset_blocks, uint64_t num_blocks,
		       spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	if (g_succeed) {
		int i = g_req_records.count;

		g_req_records.offset_blocks[i] = offset_blocks;
		g_req_records.num_blocks[i] = num_blocks;
		g_req_records.io_type[i] = CONCAT_FLUSH;
		g_req_records.count++;
		cb(NULL, true, cb_arg);
		return 0;
	} else {
		return -ENOMEM;
	}
}

void
raid_bdev_queue_io_wait(struct raid_bdev_io *raid_io, struct spdk_bdev *bdev,
			struct spdk_io_channel *ch, spdk_bdev_io_wait_cb cb_fn)
{
	g_succeed = true;
	cb_fn(raid_io);
}

void
raid_test_bdev_io_complete(struct raid_bdev_io *raid_io, enum spdk_bdev_io_status status)
{
	CU_ASSERT(status == SPDK_BDEV_IO_STATUS_SUCCESS);
}

static void
init_globals(void)
{
	int i;

	for (i = 0; i < MAX_RECORDS; i++) {
		g_req_records.offset_blocks[i] = 0;
		g_req_records.num_blocks[i] = 0;
		g_req_records.io_type[i] = CONCAT_NONE;
	}
	g_req_records.count = 0;
	g_succeed = false;
}

static int
test_setup(void)
{
	uint8_t num_base_bdevs_values[] = { 3, 4, 5 };
	uint64_t base_bdev_blockcnt_values[] = { 1, 1024, 1024 * 1024 };
	uint32_t base_bdev_blocklen_values[] = { 512, 4096 };
	uint32_t strip_size_kb_values[] = { 1, 4, 128 };
	uint8_t *num_base_bdevs;
	uint64_t *base_bdev_blockcnt;
	uint32_t *base_bdev_blocklen;
	uint32_t *strip_size_kb;
	uint64_t params_count;
	int rc;

	params_count = SPDK_COUNTOF(num_base_bdevs_values) *
		       SPDK_COUNTOF(base_bdev_blockcnt_values) *
		       SPDK_COUNTOF(base_bdev_blocklen_values) *
		       SPDK_COUNTOF(strip_size_kb_values);
	rc = raid_test_params_alloc(params_count);
	if (rc) {
		return rc;
	}

	ARRAY_FOR_EACH(num_base_bdevs_values, num_base_bdevs) {
		ARRAY_FOR_EACH(base_bdev_blockcnt_values, base_bdev_blockcnt) {
			ARRAY_FOR_EACH(base_bdev_blocklen_values, base_bdev_blocklen) {
				ARRAY_FOR_EACH(strip_size_kb_values, strip_size_kb) {
					struct raid_params params = {
						.num_base_bdevs = *num_base_bdevs,
						.base_bdev_blockcnt = *base_bdev_blockcnt,
						.base_bdev_blocklen = *base_bdev_blocklen,
						.strip_size = *strip_size_kb * 1024 / *base_bdev_blocklen,
					};
					if (params.strip_size == 0 ||
					    params.strip_size > params.base_bdev_blockcnt) {
						continue;
					}
					raid_test_params_add(&params);
				}
			}
		}
	}

	return 0;
}

static int
test_cleanup(void)
{
	raid_test_params_free();
	return 0;
}

static struct raid_bdev *
create_concat(struct raid_params *params)
{
	struct raid_bdev *raid_bdev = raid_test_create_raid_bdev(params, &g_concat_module);

	CU_ASSERT(concat_start(raid_bdev) == 0);
	return raid_bdev;
}

static void
delete_concat(struct raid_bdev *raid_bdev)
{
	concat_stop(raid_bdev);
	raid_test_delete_raid_bdev(raid_bdev);
}

static void
test_concat_start(void)
{
	struct raid_bdev *raid_bdev;
	struct raid_params *params;
	struct concat_block_range *block_range;
	uint64_t total_blockcnt;
	int i;

	RAID_PARAMS_FOR_EACH(params) {
		raid_bdev = create_concat(params);
		block_range = raid_bdev->module_private;
		total_blockcnt = 0;
		for (i = 0; i < params->num_base_bdevs; i++) {
			CU_ASSERT(block_range[i].start == total_blockcnt);
			CU_ASSERT(block_range[i].length == params->base_bdev_blockcnt);
			total_blockcnt += params->base_bdev_blockcnt;
		}
		delete_concat(raid_bdev);
	}
}

static void
raid_io_cleanup(struct raid_bdev_io *raid_io)
{
	if (raid_io->iovs) {
		free(raid_io->iovs->iov_base);
		free(raid_io->iovs);
	}

	free(raid_io);
}

static void
raid_io_initialize(struct raid_bdev_io *raid_io, struct raid_bdev_io_channel *raid_ch,
		   struct raid_bdev *raid_bdev, uint64_t lba, uint64_t blocks, int16_t iotype)
{
	struct iovec *iovs;
	int iovcnt;
	void *md_buf;

	if (iotype == SPDK_BDEV_IO_TYPE_UNMAP || iotype == SPDK_BDEV_IO_TYPE_FLUSH) {
		iovs = NULL;
		iovcnt = 0;
		md_buf = NULL;
	} else {
		iovcnt = 1;
		iovs = calloc(iovcnt, sizeof(struct iovec));
		SPDK_CU_ASSERT_FATAL(iovs != NULL);
		iovs->iov_len = raid_io->num_blocks * BLOCK_LEN;
		iovs->iov_base = calloc(1, iovs->iov_len);
		SPDK_CU_ASSERT_FATAL(iovs->iov_base != NULL);
		md_buf = (void *)0xAEDFEBAC;
	}

	raid_test_bdev_io_init(raid_io, raid_bdev, raid_ch, iotype, lba, blocks, iovs, iovcnt, md_buf);
}

static void
submit_and_verify_rw(enum CONCAT_IO_TYPE io_type, struct raid_params *params)
{
	struct raid_bdev *raid_bdev;
	struct raid_bdev_io *raid_io;
	struct raid_bdev_io_channel *raid_ch;
	uint64_t lba, blocks;
	int i;

	lba = 0;
	blocks = 1;
	for (i = 0; i < params->num_base_bdevs; i++) {
		init_globals();
		raid_bdev = create_concat(params);
		raid_io = calloc(1, sizeof(*raid_io));
		SPDK_CU_ASSERT_FATAL(raid_io != NULL);
		raid_ch = raid_test_create_io_channel(raid_bdev);

		switch (io_type) {
		case CONCAT_WRITEV:
			raid_io_initialize(raid_io, raid_ch, raid_bdev, lba, blocks, SPDK_BDEV_IO_TYPE_WRITE);
			concat_submit_rw_request(raid_io);
			break;
		case CONCAT_READV:
			raid_io_initialize(raid_io, raid_ch, raid_bdev, lba, blocks, SPDK_BDEV_IO_TYPE_READ);
			concat_submit_rw_request(raid_io);
			break;
		case CONCAT_UNMAP:
			raid_io_initialize(raid_io, raid_ch, raid_bdev, lba, blocks, SPDK_BDEV_IO_TYPE_UNMAP);
			concat_submit_null_payload_request(raid_io);
			break;
		case CONCAT_FLUSH:
			raid_io_initialize(raid_io, raid_ch, raid_bdev, lba, blocks, SPDK_BDEV_IO_TYPE_FLUSH);
			concat_submit_null_payload_request(raid_io);
			break;
		default:
			CU_ASSERT(false);
		}

		/*
		 * We submit request to the first lba of each underlying device,
		 * so the offset of the underling device should always be 0.
		 */
		CU_ASSERT(g_req_records.offset_blocks[0] == 0);
		CU_ASSERT(g_req_records.num_blocks[0] == blocks);
		CU_ASSERT(g_req_records.io_type[0] == io_type);
		CU_ASSERT(g_req_records.count == 1);
		CU_ASSERT(g_req_records.md == (void *)0xAEDFEBAC);
		raid_io_cleanup(raid_io);
		raid_test_destroy_io_channel(raid_ch);
		delete_concat(raid_bdev);
		lba += params->base_bdev_blockcnt;
	}
}

static void
test_concat_rw(void)
{
	struct raid_params *params;
	enum CONCAT_IO_TYPE io_type_list[] = {CONCAT_WRITEV, CONCAT_READV};
	enum CONCAT_IO_TYPE io_type;
	int i;

	RAID_PARAMS_FOR_EACH(params) {
		for (i = 0; i < 2; i ++) {
			io_type = io_type_list[i];
			submit_and_verify_rw(io_type, params);
		}
	}
}

static void
submit_and_verify_null_payload(enum CONCAT_IO_TYPE io_type, struct raid_params *params)
{
	struct raid_bdev *raid_bdev;
	struct raid_bdev_io *raid_io;
	struct raid_bdev_io_channel *raid_ch;
	uint64_t lba, blocks;

	/*
	 * In this unittest, all base bdevs have the same blockcnt.
	 * If the base_bdev_blockcnt > 1, the request will start from
	 * the second bdev, and across two bdevs.
	 * If the base_bdev_blockcnt == 1, the request will start from
	 * the third bdev. In this case, if there are only 3 bdevs,
	 * we can not set blocks to base_bdev_blockcnt + 1 because the request
	 * will be beyond the end of the last bdev, so we set the blocks to 1
	 */
	lba = params->base_bdev_blockcnt + 1;
	if (params->base_bdev_blockcnt == 1 && params->num_base_bdevs == 3) {
		blocks = 1;
	} else {
		blocks = params->base_bdev_blockcnt + 1;
	}
	init_globals();
	raid_bdev = create_concat(params);
	raid_io = calloc(1, sizeof(*raid_io));
	SPDK_CU_ASSERT_FATAL(raid_io != NULL);
	raid_ch = raid_test_create_io_channel(raid_bdev);

	switch (io_type) {
	case CONCAT_UNMAP:
		raid_io_initialize(raid_io, raid_ch, raid_bdev, lba, blocks, SPDK_BDEV_IO_TYPE_UNMAP);
		concat_submit_null_payload_request(raid_io);
		break;
	case CONCAT_FLUSH:
		raid_io_initialize(raid_io, raid_ch, raid_bdev, lba, blocks, SPDK_BDEV_IO_TYPE_FLUSH);
		concat_submit_null_payload_request(raid_io);
		break;
	default:
		CU_ASSERT(false);
	}

	if (params->base_bdev_blockcnt == 1) {
		if (params->num_base_bdevs == 3) {
			CU_ASSERT(g_req_records.count == 1);
			CU_ASSERT(g_req_records.offset_blocks[0] == 0);
			CU_ASSERT(g_req_records.num_blocks[0] == 1);
		} else {
			CU_ASSERT(g_req_records.count == 2);
			CU_ASSERT(g_req_records.offset_blocks[0] == 0);
			CU_ASSERT(g_req_records.num_blocks[0] == 1);
			CU_ASSERT(g_req_records.io_type[0] == io_type);
			CU_ASSERT(g_req_records.offset_blocks[1] == 0);
			CU_ASSERT(g_req_records.num_blocks[1] == 1);
			CU_ASSERT(g_req_records.io_type[1] == io_type);
		}
	} else {
		CU_ASSERT(g_req_records.count == 2);
		CU_ASSERT(g_req_records.offset_blocks[0] == 1);
		CU_ASSERT(g_req_records.num_blocks[0] == params->base_bdev_blockcnt - 1);
		CU_ASSERT(g_req_records.io_type[0] == io_type);
		CU_ASSERT(g_req_records.offset_blocks[1] == 0);
		CU_ASSERT(g_req_records.num_blocks[1] == 2);
		CU_ASSERT(g_req_records.io_type[1] == io_type);
	}
	raid_io_cleanup(raid_io);
	raid_test_destroy_io_channel(raid_ch);
	delete_concat(raid_bdev);
}

static void
test_concat_null_payload(void)
{
	struct raid_params *params;
	enum CONCAT_IO_TYPE io_type_list[] = {CONCAT_FLUSH, CONCAT_UNMAP};
	enum CONCAT_IO_TYPE io_type;
	int i;

	RAID_PARAMS_FOR_EACH(params) {
		for (i = 0; i < 2; i ++) {
			io_type = io_type_list[i];
			submit_and_verify_null_payload(io_type, params);
		}
	}
}

int
main(int argc, char **argv)
{
	CU_pSuite suite = NULL;
	unsigned int num_failures;

	CU_initialize_registry();

	suite = CU_add_suite("concat", test_setup, test_cleanup);
	CU_ADD_TEST(suite, test_concat_start);
	CU_ADD_TEST(suite, test_concat_rw);
	CU_ADD_TEST(suite, test_concat_null_payload);

	allocate_threads(1);
	set_thread(0);

	num_failures = spdk_ut_run_tests(argc, argv, NULL);
	CU_cleanup_registry();

	free_threads();

	return num_failures;
}
