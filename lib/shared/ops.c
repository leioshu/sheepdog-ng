/*
 * Copyright (C) 2015 China Mobile Inc.
 *
 * Wang Zhengyong <wangzhengyong@cmss.chinamobile.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version
 * 2 as published by the Free Software Foundation.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "sheepdog.h"
#include "internal.h"

static int vdi_rw_request(struct sheep_aiocb *aiocb)
{
	struct sd_request *request = aiocb->request;
	uint64_t offset = aiocb->offset;
	uint64_t total = aiocb->length;
	int start = offset % SD_DATA_OBJ_SIZE;
	uint32_t idx = offset / SD_DATA_OBJ_SIZE;
	int len = SD_DATA_OBJ_SIZE - start;
	struct sd_cluster *c = request->cluster;

	if (total < len)
		len = total;

	/*
	 * Make sure we don't free the aiocb before we are done with all
	 * requests.This additional reference is dropped at the end of this
	 * function.
	 */
	uatomic_inc(&aiocb->nr_requests);

	do {
		struct sheep_request *req;
		uint64_t oid = vid_to_data_oid(request->vdi->vid, idx),
			 cow_oid = 0;
		uint32_t vid = sheep_inode_get_vid(request, idx);

		/*
		 * For read, either read cow object or end the request.
		 * For write, copy-on-write cow object
		 */
		if (vid && vid != request->vdi->vid) {
			if (VDI_WRITE == request->opcode)
				cow_oid = vid_to_data_oid(vid, idx);
			else
				oid = vid_to_data_oid(vid, idx);
		}

		req = alloc_sheep_request(aiocb, oid, cow_oid, len, start);
		if (vid && !cow_oid)
			goto submit;

		switch (req->opcode) {
		case VDI_WRITE:
			/*
			 * Sheepdog can't handle concurrent creation on the same
			 * object. We send one create req first and then send
			 * write reqs in next.
			 */
			if (find_inflight_request_oid(c, oid)) {
				uint32_t tmp_vid;

				sd_write_lock(&c->blocking_lock);
				/*
				 * There are slim chance object was created
				 * before we grab blocking_lock
				 */
				tmp_vid = sheep_inode_get_vid(request, idx);
				if (tmp_vid && tmp_vid == request->vdi->vid) {
					sd_rw_unlock(&c->blocking_lock);
					goto submit;
				}
				list_add_tail(&req->list, &c->blocking_list);
				sd_rw_unlock(&c->blocking_lock);
				goto done;
			}
			req->opcode = VDI_CREATE;
			break;
		case VDI_READ:
			end_sheep_request(req);
			goto done;
		}
submit:
		submit_sheep_request(req);
done:
		idx++;
		total -= len;
		start = (start + len) % SD_DATA_OBJ_SIZE;
		len = total > SD_DATA_OBJ_SIZE ? SD_DATA_OBJ_SIZE : total;
	} while (total > 0);

	if (uatomic_sub_return(&aiocb->nr_requests, 1) <= 0)
		aiocb->aio_done_func(aiocb);

	return 0;
}

static int vdi_create_respond(struct sheep_request *req)
{
	int ret = 0;
	struct sd_vdi *vdi;
	struct sheep_request *new;
	uint32_t vid, idx;
	uint64_t oid;
	struct sd_cluster *c = req->aiocb->request->cluster;

	vdi = req->aiocb->request->vdi;

	/* We need to update inode for create */
	new = xmalloc(sizeof(*new));
	vid = vdi->vid;
	oid = vid_to_vdi_oid(vid);
	idx = data_oid_to_idx(req->oid);
	new->offset = SD_INODE_HEADER_SIZE + sizeof(vid) * idx;
	new->length = sizeof(vid);
	new->oid = oid;
	new->cow_oid = 0;
	new->aiocb = req->aiocb;
	new->buf = (char *)&vid;
	new->seq_num = uatomic_add_return(&c->seq_num, 1);
	new->opcode = VDI_WRITE;
	uatomic_inc(&req->aiocb->nr_requests);
	INIT_LIST_NODE(&new->list);

	/* Make sure no request is queued while we update inode */
	sd_write_lock(&vdi->lock);
	vdi->inode->data_vdi_id[idx] = vid;
	sd_rw_unlock(&vdi->lock);

	submit_sheep_request(new);
	submit_blocking_sheep_request(c, req->oid);

	end_sheep_request(req);
	return ret;
}

static struct sd_op_template sd_ops[] = {
	[VDI_READ] = {
		.name = "VDI WRITE",
		.request_process = vdi_rw_request,
		.respond_process = end_sheep_request,
	},
	[VDI_WRITE] = {
		.name = "VDI WRITE",
		.request_process = vdi_rw_request,
		.respond_process = end_sheep_request,
	},
	[VDI_CREATE] = {
		.name = "VDI CREATE",
		/* The request is submitted by vdi_rw_request */
		.respond_process = vdi_create_respond,
	},
};

const struct sd_op_template *get_sd_op(uint8_t opcode)
{
	return sd_ops + opcode;
}
