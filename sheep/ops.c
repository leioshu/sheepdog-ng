/*
 * Copyright (C) 2011 Nippon Telegraph and Telephone Corporation.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version
 * 2 as published by the Free Software Foundation.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "sheep_priv.h"
#include "trace/trace.h"
#include "livepatch/livepatch.h"

enum sd_op_type {
	SD_OP_TYPE_CLUSTER = 1, /* Cluster operations */
	SD_OP_TYPE_LOCAL,       /* Local operations */
	SD_OP_TYPE_PEER,          /* IO operations */
	SD_OP_TYPE_GATEWAY,	/* Gateway operations */
	SD_OP_TYPE_NONE,	/* Non-queued operations */
};

struct sd_op_template {
	const char *name;
	enum sd_op_type type;

	/* process request even when cluster is not working */
	bool force;

	/*
	 * Indicates administrative operation to trace.
	 * If true is set, rx_main and tx_main log operations at info level.
	 */
	bool is_admin_op;

	/*
	 * process_work() will be called in a worker thread, and process_main()
	 * will be called in the main thread.
	 *
	 * If type is SD_OP_TYPE_CLUSTER, it is guaranteed that only one node
	 * processes a cluster operation at the same time.  We can use this for
	 * for example to implement distributed locking.  process_work()
	 * will be called on the local node, and process_main() will be called
	 * on every node.
	 *
	 * If type is SD_OP_TYPE_LOCAL, both process_work() and process_main()
	 * will be called on the local node.
	 *
	 * If type is SD_OP_TYPE_PEER, only process_work() will be called, and it
	 * will be called on the local node.
	 */
	int (*process_work)(struct request *req);
	int (*process_main)(const struct sd_req *req, struct sd_rsp *rsp,
			    void *data, const struct sd_node *sender);
};

static int stat_sheep(uint64_t *store_size, uint64_t *store_free)
{
	uint64_t used;

	if (sys->gateway_only) {
		*store_size = 0;
		*store_free = 0;
	} else {
		*store_size = md_get_size(&used);
		*store_free = *store_size - used;
	}
	return SD_RES_SUCCESS;
}

static int cluster_new_vdi(struct request *req)
{
	const struct sd_req *hdr = &req->rq;
	struct sd_rsp *rsp = &req->rp;
	uint32_t vid;
	int ret;
	struct timeval tv;

	gettimeofday(&tv, NULL);

	struct vdi_iocb iocb = {
		.name = req->data,
		.data_len = hdr->data_length,
		.size = hdr->vdi.vdi_size,
		.base_vid = hdr->vdi.base_vdi_id,
		.create_snapshot = !!hdr->vdi.snapid,
		.copy_policy = hdr->vdi.copy_policy,
		.store_policy = hdr->vdi.store_policy,
		.nr_copies = hdr->vdi.copies,
		.time = (uint64_t) tv.tv_sec << 32 | tv.tv_usec * 1000,
	};

	/* Client doesn't specify redundancy scheme (copy = 0) */
	if (!hdr->vdi.copies) {
		iocb.nr_copies = sys->cinfo.nr_copies;
		iocb.copy_policy = sys->cinfo.copy_policy;
	}

	if (iocb.copy_policy)
		iocb.nr_copies = ec_policy_to_dp(iocb.copy_policy, NULL, NULL);

	if (hdr->data_length != SD_MAX_VDI_LEN)
		return SD_RES_INVALID_PARMS;

	if (iocb.create_snapshot)
		ret = vdi_snapshot(&iocb, &vid);
	else
		ret = vdi_create(&iocb, &vid);

	rsp->vdi.vdi_id = vid;
	rsp->vdi.copies = iocb.nr_copies;

	return ret;
}

static int post_cluster_new_vdi(const struct sd_req *req, struct sd_rsp *rsp,
				void *data, const struct sd_node *sender)
{
	unsigned long nr = rsp->vdi.vdi_id;
	int ret = rsp->result;
	char *name = data;

	sd_info("name: %s, base_vdi_id: %x, new vdi_id: %x, sender: %s",
		name, req->vdi.base_vdi_id, rsp->vdi.vdi_id,
		node_to_str(sender));

	sd_debug("done %d %lx", ret, nr);
	if (ret == SD_RES_SUCCESS) {
		/*
		 * vdi state is a private state of this node that is never
		 * synced up with other nodes, so make sure you know of it
		 * before you implement any useful featurs that might need a
		 * syncd up states.
		 *
		 * QEMU client's online snapshot logic:
		 * qemu-img(or dog) snapshot -> tell connected sheep to make
		 *                              working as snapshot
		 * sheep   --> make the working vdi as snapshot
		 * QEMU VM --> get the SD_RES_READONLY while it is write to the
		 *             working vdi.
		 * QEMU VM --> reload new working vdi, switch to it.
		 *
		 * It only needs the connected sheep to return SD_RES_READONLY,
		 * so we can add a private state to the connected sheep and
		 * propagate it to other nodes via cluster notification. But
		 * note newly joining nodes won't share this state in order to
		 * avoid vdi states sync-up.
		 */
		vdi_mark_snapshot(req->vdi.base_vdi_id);
		atomic_set_bit(nr, sys->vdi_inuse);
	}

	return ret;
}

static int vdi_init_tag(const char **tag, const char *buf, uint32_t len)
{
	if (len == SD_MAX_VDI_LEN + SD_MAX_VDI_TAG_LEN)
		*tag = buf + SD_MAX_VDI_LEN;
	else if (len == SD_MAX_VDI_LEN)
		*tag = NULL;
	else
		return -1;

	return 0;
}

static int cluster_del_vdi(struct request *req)
{
	const struct sd_req *hdr = &req->rq;
	struct sd_rsp *rsp = &req->rp;
	uint32_t data_len = hdr->data_length;
	struct vdi_iocb iocb = {
		.name = req->data,
		.data_len = data_len,
		.snapid = hdr->vdi.snapid,
	};
	struct vdi_info info = {};
	int ret;

	if (vdi_init_tag(&iocb.tag, req->data, data_len) < 0)
		return SD_RES_INVALID_PARMS;

	ret = vdi_lookup(&iocb, &info);
	if (ret != SD_RES_SUCCESS)
		return ret;
	rsp->vdi.vdi_id = info.vid;

	return vdi_delete(info.vid, hdr->vdi.async_delete);
}

struct cache_deletion_work {
	uint32_t vid;
	struct work work;
};

static void cache_delete_work(struct work *work)
{
	struct cache_deletion_work *dw =
		container_of(work, struct cache_deletion_work, work);

	object_cache_delete(dw->vid);
}

static void cache_delete_done(struct work *work)
{
	struct cache_deletion_work *dw =
		container_of(work, struct cache_deletion_work, work);

	free(dw);
}

static int post_cluster_del_vdi(const struct sd_req *req, struct sd_rsp *rsp,
				void *data, const struct sd_node *sender)
{
	unsigned long vid = rsp->vdi.vdi_id;
	struct cache_deletion_work *dw;
	int ret = rsp->result;
	char *name = data;

	sd_info("name: %s, base_vdi_id: %x, new vdi_id: %x, sender: %s",
		name, req->vdi.base_vdi_id, rsp->vdi.vdi_id,
		node_to_str(sender));

	vdi_delete_state(vid);

	if (!sys->enable_object_cache)
		return ret;

	dw = xzalloc(sizeof(*dw));
	dw->vid = vid;
	dw->work.fn = cache_delete_work;
	dw->work.done = cache_delete_done;

	queue_work(sys->deletion_wqueue, &dw->work);

	return ret;
}

/*
 * Look up vid and copy number from vdi name
 *
 * This must be a cluster operation.  If QEMU reads the vdi object
 * while sheep snapshots the vdi, sheep can return SD_RES_NO_VDI.  To
 * avoid this problem, SD_OP_GET_INFO must be ordered with
 * SD_OP_NEW_VDI.
 */
static int cluster_get_vdi_info(struct request *req)
{
	const struct sd_req *hdr = &req->rq;
	struct sd_rsp *rsp = &req->rp;
	uint32_t data_len = hdr->data_length;
	int ret;
	struct vdi_info info = {};
	struct vdi_iocb iocb = {
		.name = req->data,
		.data_len = data_len,
		.snapid = hdr->vdi.snapid,
	};

	if (vdi_init_tag(&iocb.tag, req->data, data_len) < 0)
		return SD_RES_INVALID_PARMS;

	ret = vdi_lookup(&iocb, &info);
	if (ret != SD_RES_SUCCESS)
		return ret;

	rsp->vdi.vdi_id = info.vid;
	rsp->vdi.copies = get_vdi_copy_number(info.vid);

	return ret;
}

static int remove_epoch(uint32_t epoch)
{
	int ret;
	char path[PATH_MAX];

	sd_debug("remove epoch %"PRIu32, epoch);
	snprintf(path, sizeof(path), "%s%08u", epoch_path, epoch);
	ret = unlink(path);
	if (ret && errno != ENOENT) {
		sd_err("failed to remove %s: %m", path);
		return SD_RES_EIO;
	}

	return SD_RES_SUCCESS;
}

static int cluster_make_fs(const struct sd_req *req, struct sd_rsp *rsp,
			   void *data, const struct sd_node *sender)
{
	int i, ret = SD_RES_SUCCESS;
	uint32_t latest_epoch;
	struct store_driver *driver;
	char *store_name = data;

	driver = find_store_driver(data);
	if (!driver) {
		ret = SD_RES_NO_STORE;
		goto out;
	}

	pstrcpy((char *)sys->cinfo.store, sizeof(sys->cinfo.store),
		store_name);
	sd_store = driver;
	latest_epoch = get_latest_epoch();

	ret = sd_store->format();
	if (ret != SD_RES_SUCCESS)
		goto out;

	ret = sd_store->init();
	if (ret != SD_RES_SUCCESS)
		goto out;

	sys->cinfo.nr_copies = req->cluster.copies;
	sys->cinfo.copy_policy = req->cluster.copy_policy;
	sys->cinfo.flags = req->cluster.flags;
	if (!sys->cinfo.nr_copies)
		sys->cinfo.nr_copies = SD_DEFAULT_COPIES;
	sys->cinfo.ctime = req->cluster.ctime;
	set_cluster_config(&sys->cinfo);

	for (i = 1; i <= latest_epoch; i++)
		remove_epoch(i);

	memset(sys->vdi_inuse, 0, sizeof(sys->vdi_inuse));
	clean_vdi_state();
	objlist_cache_format();

	sys->cinfo.epoch = 0;

	ret = inc_and_log_epoch();
	if (ret) {
		ret = SD_RES_EIO;
		goto out;
	}

	sys->cinfo.status = SD_STATUS_OK;
out:
	return ret;
}

static int cluster_shutdown(const struct sd_req *req, struct sd_rsp *rsp,
			    void *data, const struct sd_node *sender)
{
	sys->cinfo.status = SD_STATUS_SHUTDOWN;
	if (!node_in_recovery()) {
		unregister_listening_fds();

		if (set_cluster_shutdown(true) != SD_RES_SUCCESS)
			/*
			 * It's okay we failed to set 'shutdown', just start
			 * recovery after restart blindly.
			 */
			sd_err("failed to set cluster as shutdown");
	}

	return SD_RES_SUCCESS;
}

static int cluster_get_vdi_attr(struct request *req)
{
	const struct sd_req *hdr = &req->rq;
	struct sd_rsp *rsp = &req->rp;
	uint32_t vid, attrid = 0;
	struct sheepdog_vdi_attr *vattr;
	struct vdi_iocb iocb = {};
	struct vdi_info info = {};
	int ret;

	vattr = req->data;
	iocb.name = vattr->name;
	iocb.tag = vattr->tag;
	iocb.snapid = hdr->vdi.snapid;
	ret = vdi_lookup(&iocb, &info);
	if (ret != SD_RES_SUCCESS)
		return ret;
	/*
	 * the current VDI id can change if we take a snapshot,
	 * so we use the hash value of the VDI name as the VDI id
	 */
	vid = sd_hash_vdi(vattr->name);
	ret = get_vdi_attr(req->data, hdr->data_length,
			   vid, &attrid, info.create_time,
			   !!(hdr->flags & SD_FLAG_CMD_CREAT),
			   !!(hdr->flags & SD_FLAG_CMD_EXCL),
			   !!(hdr->flags & SD_FLAG_CMD_DEL));

	rsp->vdi.vdi_id = vid;
	rsp->vdi.attr_id = attrid;
	rsp->vdi.copies = get_vdi_copy_number(vid);

	return ret;
}

static int local_release_vdi(struct request *req)
{
	uint32_t vid = req->rq.vdi.base_vdi_id;
	int ret;

	if (!sys->enable_object_cache)
		return SD_RES_SUCCESS;

	if (!vid) {
		sd_info("Some VDI failed to release the object cache. "
			"Probably you are running old QEMU.");
		return SD_RES_SUCCESS;
	}

	ret = object_cache_flush_vdi(vid);
	if (ret == SD_RES_SUCCESS)
		object_cache_delete(vid);

	return ret;
}

static int local_get_store_list(struct request *req)
{
	struct strbuf buf = STRBUF_INIT;
	struct store_driver *driver;

	list_for_each_entry(driver, &store_drivers, list) {
		strbuf_addf(&buf, "%s ", driver->name);
	}
	req->rp.data_length = strbuf_copyout(&buf, req->data, req->data_length);

	strbuf_release(&buf);
	return SD_RES_SUCCESS;
}

static int local_read_vdis(const struct sd_req *req, struct sd_rsp *rsp,
			   void *data, const struct sd_node *sender)
{
	return read_vdis(data, req->data_length, &rsp->data_length);
}

static int local_stat_sheep(struct request *req)
{
	struct sd_rsp *rsp = &req->rp;

	return stat_sheep(&rsp->node.store_size, &rsp->node.store_free);
}

static int local_stat_recovery(const struct sd_req *req, struct sd_rsp *rsp,
			       void *data, const struct sd_node *sender)
{
	get_recovery_state(data);
	rsp->data_length = sizeof(struct recovery_state);

	return SD_RES_SUCCESS;
}

static int local_stat_cluster(struct request *req)
{
	struct sd_rsp *rsp = &req->rp;
	struct epoch_log *elog;
	char *next_elog;
	int i, max_elogs;
	uint32_t epoch;
	uint32_t nodes_nr = req->rq.cluster.nodes_nr;

	if (req->vinfo == NULL) {
		sd_debug("cluster is not started up");
		goto out;
	}

	max_elogs = req->rq.data_length / (sizeof(*elog)
			+ nodes_nr * sizeof(struct sd_node));
	next_elog = (char *)req->data;
	epoch = get_latest_epoch();
	for (i = 0; i < max_elogs; i++) {
		int nr_nodes = 0, ret;

		if (epoch <= 0)
			break;

		elog = (struct epoch_log *)next_elog;
		memset(elog, 0, sizeof(*elog));

		/* some filed only need to store in first elog */
		if (i == 0) {
			elog->ctime = sys->cinfo.ctime;
			elog->nr_copies = sys->cinfo.nr_copies;
			elog->copy_policy = sys->cinfo.copy_policy;
			elog->flags = sys->cinfo.flags;
			pstrcpy(elog->drv_name, STORE_LEN,
				(char *)sys->cinfo.store);
		}

		elog->epoch = epoch;
		if (nodes_nr > 0) {
			ret = epoch_log_read_with_timestamp(
					epoch, elog->nodes,
					nodes_nr * sizeof(struct sd_node),
					&nr_nodes, (time_t *)&elog->time);
			if (ret == SD_RES_NO_TAG)
				ret = epoch_log_read_remote(
					epoch, elog->nodes,
					nodes_nr * sizeof(struct sd_node),
					&nr_nodes, (time_t *)&elog->time,
					req->vinfo);
			if (ret == SD_RES_BUFFER_SMALL)
				return ret;
			elog->nr_nodes = nr_nodes;
		} else
			elog->nr_nodes = 0;

		next_elog = (char *)elog->nodes
				+ nodes_nr * sizeof(struct sd_node);
		rsp->data_length += sizeof(*elog)
				+ nodes_nr * sizeof(struct sd_node);
		epoch--;
	}
out:
	switch (sys->cinfo.status) {
	case SD_STATUS_OK:
		return SD_RES_SUCCESS;
	case SD_STATUS_WAIT:
		if (sys->cinfo.ctime == 0)
			return SD_RES_WAIT_FOR_FORMAT;
		else
			return SD_RES_WAIT_FOR_JOIN;
	case SD_STATUS_SHUTDOWN:
		return SD_RES_SHUTDOWN;
	default:
		return SD_RES_SYSTEM_ERROR;
	}
}

static int local_get_obj_list(struct request *req)
{
	return get_obj_list(&req->rq, &req->rp, req->data);
}

static int local_get_epoch(struct request *req)
{
	uint32_t epoch = req->rq.obj.tgt_epoch;
	int nr_nodes = 0, nodes_len, ret;
	time_t timestamp;

	sd_debug("%d", epoch);

	ret =
		epoch_log_read_with_timestamp(epoch, req->data,
					req->rq.data_length - sizeof(timestamp),
					&nr_nodes, &timestamp);
	if (ret != SD_RES_SUCCESS)
		return ret;

	nodes_len = nr_nodes * sizeof(struct sd_node);
	memcpy((void *)((char *)req->data + nodes_len), &timestamp,
		sizeof(timestamp));
	req->rp.data_length = nodes_len + sizeof(time_t);
	return SD_RES_SUCCESS;
}

static int cluster_force_recover_work(struct request *req)
{
	struct vnode_info *old_vnode_info;
	uint32_t epoch = sys_epoch();

	/*
	 * We should manually recover the cluster when
	 * 1) the master is physically down (different epoch condition).
	 * 2) some nodes are physically down (same epoch condition).
	 * In both case, the nodes(s) stat is WAIT_FOR_JOIN.
	 */
	if (sys->cinfo.status != SD_STATUS_WAIT || req->vinfo == NULL)
		return SD_RES_FORCE_RECOVER;

	old_vnode_info = get_vnode_info_epoch(epoch, req->vinfo);
	if (!old_vnode_info) {
		sd_emerg("cannot get vnode info for epoch %d", epoch);
		put_vnode_info(old_vnode_info);
		return SD_RES_FORCE_RECOVER;
	}

	if (req->rq.data_length <
	    sizeof(struct sd_node) * old_vnode_info->nr_nodes) {
		sd_err("too small buffer size, %d", req->rq.data_length);
		return SD_RES_INVALID_PARMS;
	}

	req->rp.epoch = epoch;
	req->rp.data_length = sizeof(struct sd_node) * old_vnode_info->nr_nodes;
	nodes_to_buffer(&old_vnode_info->nroot, req->data);

	put_vnode_info(old_vnode_info);

	return SD_RES_SUCCESS;
}

static int cluster_force_recover_main(const struct sd_req *req,
				      struct sd_rsp *rsp,
				      void *data, const struct sd_node *sender)
{
	struct vnode_info *old_vnode_info, *vnode_info;
	int ret = SD_RES_SUCCESS;
	struct sd_node *nodes = data;
	size_t nr_nodes = rsp->data_length / sizeof(*nodes);
	struct rb_root nroot = RB_ROOT;

	if (rsp->epoch != sys->cinfo.epoch) {
		sd_err("epoch was incremented while cluster_force_recover");
		return SD_RES_FORCE_RECOVER;
	}

	ret = inc_and_log_epoch();
	if (ret) {
		sd_emerg("cannot update epoch log");
		goto err;
	}

	if (!is_cluster_formatted())
		/* initialize config file */
		set_cluster_config(&sys->cinfo);

	sys->cinfo.status = SD_STATUS_OK;

	for (int i = 0; i < nr_nodes; i++)
		rb_insert(&nroot, &nodes[i], rb, node_cmp);

	vnode_info = get_vnode_info();
	old_vnode_info = alloc_vnode_info(&nroot);
	start_recovery(vnode_info, old_vnode_info, true);
	put_vnode_info(vnode_info);
	put_vnode_info(old_vnode_info);
	return ret;
err:
	panic("failed in force recovery");
}

static int cluster_notify_vdi_add(const struct sd_req *req, struct sd_rsp *rsp,
				  void *data, const struct sd_node *sender)
{
	if (req->vdi_state.set_bitmap)
		atomic_set_bit(req->vdi_state.new_vid, sys->vdi_inuse);

	return SD_RES_SUCCESS;
}

static int cluster_delete_cache(const struct sd_req *req, struct sd_rsp *rsp,
				void *data, const struct sd_node *sender)
{
	uint32_t vid = oid_to_vid(req->obj.oid);

	if (sys->enable_object_cache)
		object_cache_delete(vid);

	return SD_RES_SUCCESS;
}

static int cluster_recovery_completion(const struct sd_req *req,
				       struct sd_rsp *rsp,
				       void *data, const struct sd_node *sender)
{
	static struct sd_node recovereds[SD_MAX_NODES], *node;
	static size_t nr_recovereds;
	static int latest_epoch;
	struct vnode_info *vnode_info;
	int i;
	uint32_t epoch = req->obj.tgt_epoch;

	node = (struct sd_node *)data;

	if (sys->cinfo.flags & SD_CLUSTER_FLAG_MANUAL) {
		struct vnode_info *cur_vinfo = get_vnode_info();
		struct sd_node *n = rb_search(&cur_vinfo->nroot, node, rb,
					      node_cmp);
		if (n) {
			struct sd_node *t;

			sd_debug("%s back", node_to_str(node));
			n->nid.status = NODE_STATUS_RUNNING;
			if (node_is_local(n))
				sys->this_node.nid.status = NODE_STATUS_RUNNING;

			/* FIXME: unify auto-recovery and manual cleanup */
			rb_for_each_entry(t, &cur_vinfo->nroot, rb) {
				sd_debug("%s, status %d", node_to_str(t),
					 t->nid.status);
				if (t->nid.status == NODE_STATUS_RECOVER)
					goto out;
			}
			sd_notice("live nodes are recovered, epoch %d", epoch);
			if (cur_vinfo->nr_zones >= ec_max_data_strip() &&
			    sd_store && sd_store->cleanup)
				sd_store->cleanup();
		} else {
			sd_err("can't find %s", node_to_str(node));
		}
out:
		for (i = 0; i < sys->cinfo.nr_nodes; i++) {
			if (!node_cmp(node, sys->cinfo.nodes + i))
				sys->cinfo.nodes[i].nid.status =
					NODE_STATUS_RUNNING;
		}
		put_vnode_info(cur_vinfo);
		return SD_RES_SUCCESS;
	}

	if (latest_epoch > epoch)
		return SD_RES_SUCCESS;

	if (latest_epoch < epoch) {
		sd_debug("new epoch %d", epoch);
		latest_epoch = epoch;
		nr_recovereds = 0;
	}

	recovereds[nr_recovereds++] = *node;
	xqsort(recovereds, nr_recovereds, node_cmp);

	sd_debug("%s is recovered at epoch %d", node_to_str(node), epoch);
	for (i = 0; i < nr_recovereds; i++)
		sd_debug("[%x] %s", i, node_to_str(recovereds + i));

	if (sys->cinfo.epoch != latest_epoch)
		return SD_RES_SUCCESS;

	vnode_info = get_vnode_info();

	if (vnode_info->nr_nodes == nr_recovereds) {
		for (i = 0; i < nr_recovereds; ++i) {
			if (!rb_search(&vnode_info->nroot, &recovereds[i],
				       rb, node_cmp))
				break;
		}
		if (i == nr_recovereds) {
			sd_notice("all nodes are recovered, epoch %d", epoch);
			/* sd_store can be NULL if this node is a gateway */
			if (vnode_info->nr_zones >= ec_max_data_strip() &&
			    sd_store && sd_store->cleanup)
				sd_store->cleanup();
		}
	}

	put_vnode_info(vnode_info);

	return SD_RES_SUCCESS;
}

static int cluster_alter_cluster_copy(const struct sd_req *req,
				      struct sd_rsp *rsp, void *data,
				      const struct sd_node *sender)
{
	if (req->cluster.copy_policy != 0)
		return SD_RES_INVALID_PARMS;

	sys->cinfo.nr_copies = req->cluster.copies;
	return set_cluster_config(&sys->cinfo);
}

static bool node_size_varied(void)
{
	uint64_t new, used, old = sys->this_node.space;
	double diff;

	if (sys->gateway_only)
		return false;

	new = md_get_size(&used);
	/* If !old, it is forced-out-gateway. Not supported by current node */
	if (!old) {
		if (new)
			return true;
		else
			return false;
	}

	diff = new > old ? (double)(new - old) : (double)(old - new);
	sd_debug("new %"PRIu64 ", old %"PRIu64", ratio %f", new, old,
		 diff / (double)old);
	if (diff / (double)old < 0.01)
		return false;

	sys->this_node.space = new;
	set_node_space(new);

	return true;
}

static int local_reconfig(struct request *req)
{
	if (sys->cinfo.flags & SD_CLUSTER_FLAG_MANUAL)
		return sys->cdrv->update_node(&sys->this_node);
	return SD_RES_SUCCESS;
}

static int cluster_reconfig(const struct sd_req *req, struct sd_rsp *rsp,
			    void *data, const struct sd_node *sender)
{
	if (node_size_varied())
		return sys->cdrv->update_node(&sys->this_node);

	return SD_RES_SUCCESS;
}

static int local_md_info(struct request *request)
{
	struct sd_rsp *rsp = &request->rp;

	sd_assert(request->rq.data_length == sizeof(struct sd_md_info));
	rsp->data_length = md_get_info((struct sd_md_info *)request->data);

	return rsp->data_length ? SD_RES_SUCCESS : SD_RES_UNKNOWN;
}

static int local_md_plug(const struct sd_req *req, struct sd_rsp *rsp,
			 void *data, const struct sd_node *sender)
{
	char *disks = (char *)data;

	return md_plug_disks(disks);
}

static int local_md_unplug(const struct sd_req *req, struct sd_rsp *rsp,
			   void *data, const struct sd_node *sender)
{
	char *disks = (char *)data;

	return md_unplug_disks(disks);
}

static int local_get_hash(struct request *request)
{
	struct sd_req *req = &request->rq;
	struct sd_rsp *rsp = &request->rp;

	if (!sd_store->get_hash)
		return SD_RES_NO_SUPPORT;

	return sd_store->get_hash(req->obj.oid, req->obj.tgt_epoch,
				  rsp->hash.digest);
}

static int local_get_cache_info(struct request *request)
{
	struct sd_rsp *rsp = &request->rp;

	sd_assert(request->rq.data_length == sizeof(struct object_cache_info));
	rsp->data_length = object_cache_get_info((struct object_cache_info *)
						 request->data);

	return SD_RES_SUCCESS;
}

static int local_cache_purge(struct request *req)
{
	const struct sd_req *hdr = &req->rq;
	uint32_t vid = oid_to_vid(req->rq.obj.oid);

	if (hdr->flags == SD_FLAG_CMD_WRITE) {
		object_cache_delete(vid);
		goto out;
	}
	object_cache_format();
out:
	return SD_RES_SUCCESS;
}

static int local_sd_stat(const struct sd_req *req, struct sd_rsp *rsp,
			 void *data, const struct sd_node *sender)
{
	memcpy(data, &sys->stat, sizeof(struct sd_stat));
	rsp->data_length = sizeof(struct sd_stat);
	return SD_RES_SUCCESS;
}

/* Return SD_RES_INVALID_PARMS to ask client not to send flush req again */
static int local_flush_vdi(struct request *req)
{
	int ret = SD_RES_INVALID_PARMS;

	if (sys->enable_object_cache) {
		uint32_t vid = oid_to_vid(req->rq.obj.oid);
		ret = object_cache_flush_vdi(vid);
	}

	return ret;
}

static int local_discard_obj(struct request *req)
{
	uint64_t oid = req->rq.obj.oid;
	uint32_t vid = oid_to_vid(oid), tmp_vid;
	int ret, idx = data_oid_to_idx(oid);
	struct sd_inode *inode = xmalloc(sizeof(struct sd_inode));

	sd_debug("%"PRIx64, oid);
	ret = sd_read_object(vid_to_vdi_oid(vid), (char *)inode,
			     sizeof(struct sd_inode), 0);
	if (ret != SD_RES_SUCCESS)
		goto out;

	tmp_vid = sd_inode_get_vid(inode, idx);
	/* if vid in idx is not exist, we don't need to remove it */
	if (tmp_vid) {
		sd_inode_set_vid(inode, idx, 0);
		ret = sd_inode_write_vid(inode, idx, vid, 0, 0, false, false);
		if (ret != SD_RES_SUCCESS)
			goto out;
		if (sd_remove_object(oid) != SD_RES_SUCCESS)
			sd_err("failed to remove %"PRIx64, oid);
	}
	/*
	 * Return success even if sd_remove_object fails because we have updated
	 * inode successfully.
	 */
out:
	free(inode);
	return ret;
}

static int local_flush_and_del(struct request *req)
{
	if (!sys->enable_object_cache)
		return SD_RES_SUCCESS;
	return object_cache_flush_and_del(req);
}

static int local_trace_enable(const struct sd_req *req, struct sd_rsp *rsp,
			      void *data, const struct sd_node *sender)
{
	return trace_enable(data);
}

static int local_trace_disable(const struct sd_req *req, struct sd_rsp *rsp,
			       void *data, const struct sd_node *sender)
{
	return trace_disable(data);
}

static int local_trace_status(const struct sd_req *req, struct sd_rsp *rsp,
			      void *data, const struct sd_node *sender)
{
	rsp->data_length = trace_status(data);

	return SD_RES_SUCCESS;
}

static int local_trace_read_buf(struct request *request)
{
	struct sd_req *req = &request->rq;
	struct sd_rsp *rsp = &request->rp;
	int ret;

	ret = trace_buffer_pop(request->data, req->data_length);
	if (ret == -1)
		return SD_RES_AGAIN;

	rsp->data_length = ret;
	sd_debug("%u", rsp->data_length);
	return SD_RES_SUCCESS;
}

static int local_livepatch_patch(const struct sd_req *req, struct sd_rsp *rsp,
                                 void *data, const struct sd_node *sender)
{
    return livepatch_patch(data);
}

static int local_livepatch_unpatch(const struct sd_req *req, struct sd_rsp *rsp,
                                   void *data, const struct sd_node *sender)
{
    return livepatch_unpatch(data);
}

static int local_livepatch_status(const struct sd_req *req, struct sd_rsp *rsp,
                                    void *data, const struct sd_node *sender)
{
    rsp->data_length = livepatch_status(data);

    return SD_RES_SUCCESS;
}

static int local_kill_node(const struct sd_req *req, struct sd_rsp *rsp,
			   void *data, const struct sd_node *sender)
{
	sys->cinfo.status = SD_STATUS_KILLED;
	unregister_listening_fds();

	return SD_RES_SUCCESS;
}

static int peer_remove_obj(struct request *req)
{
	uint64_t oid = req->rq.obj.oid;
	uint8_t ec_index = req->rq.obj.ec_index;

	objlist_cache_remove(oid);

	return sd_store->remove_object(oid, ec_index);
}

int peer_read_obj(struct request *req)
{
	struct sd_req *hdr = &req->rq;
	struct sd_rsp *rsp = &req->rp;
	int ret;
	uint32_t epoch = hdr->epoch;
	struct siocb iocb;

	if (sys->gateway_only)
		return SD_RES_NO_OBJ;

	memset(&iocb, 0, sizeof(iocb));
	iocb.epoch = epoch;
	iocb.buf = req->data;
	iocb.length = hdr->data_length;
	iocb.offset = hdr->obj.offset;
	iocb.ec_index = hdr->obj.ec_index;
	iocb.copy_policy = hdr->obj.copy_policy;
	ret = sd_store->read(hdr->obj.oid, &iocb);
	if (ret != SD_RES_SUCCESS)
		goto out;

	rsp->data_length = hdr->data_length;
out:
	return ret;
}

static int peer_write_obj(struct request *req)
{
	struct sd_req *hdr = &req->rq;
	struct siocb iocb = { };
	uint64_t oid = hdr->obj.oid;

	iocb.epoch = hdr->epoch;
	iocb.buf = req->data;
	iocb.length = hdr->data_length;
	iocb.offset = hdr->obj.offset;
	iocb.ec_index = hdr->obj.ec_index;
	iocb.copy_policy = hdr->obj.copy_policy;

	return sd_store->write(oid, &iocb);
}

static int peer_create_and_write_obj(struct request *req)
{
	struct sd_req *hdr = &req->rq;
	struct siocb iocb = { };

	iocb.epoch = hdr->epoch;
	iocb.buf = req->data;
	iocb.length = hdr->data_length;
	iocb.ec_index = hdr->obj.ec_index;
	iocb.copy_policy = hdr->obj.copy_policy;
	iocb.offset = hdr->obj.offset;

	return sd_store->create_and_write(hdr->obj.oid, &iocb);
}

static int local_get_loglevel(struct request *req)
{
	int32_t current_level;

	current_level = get_loglevel();
	memcpy(req->data, &current_level, sizeof(current_level));
	req->rp.data_length = sizeof(current_level);

	sd_info("returning log level: %u", current_level);

	return SD_RES_SUCCESS;
}

static int local_set_loglevel(struct request *req)
{
	int32_t new_level = 0;

	memcpy(&new_level, req->data, sizeof(int32_t));
	if (!(LOG_EMERG <= new_level && new_level <= LOG_DEBUG)) {
		sd_err("invalid log level: %d", new_level);
		return SD_RES_INVALID_PARMS;
	}

	set_loglevel(new_level);

	return SD_RES_SUCCESS;
}

static int local_oid_exist(struct request *req)
{
	uint64_t oid = req->rq.obj.oid;
	uint8_t ec_index = local_ec_index(req->vinfo, oid);

	if (sys->this_node.nr_vnodes == 0)
		return SD_RES_NO_OBJ;

	if (is_erasure_oid(oid) && ec_index == SD_MAX_COPIES)
		return SD_RES_NO_OBJ;

	if (sd_store->exist(oid, ec_index))
		return SD_RES_SUCCESS;
	return SD_RES_NO_OBJ;
}

static int local_oids_exist(const struct sd_req *req, struct sd_rsp *rsp,
			    void *data, const struct sd_node *sender)
{
	struct request *r = container_of(req, struct request, rq);
	uint64_t *oids = (uint64_t *) data;
	uint8_t ec_index;
	int i, j, n = req->data_length / sizeof(uint64_t);

	for (i = 0, j = 0; i < n; i++) {
		ec_index = local_ec_index(r->vinfo, oids[i]);
		if (is_erasure_oid(oids[i]) && ec_index == SD_MAX_COPIES)
			oids[j++] = oids[i];
		else if (!sd_store->exist(oids[i], ec_index))
			oids[j++] = oids[i];
	}

	if (j > 0) {
		rsp->data_length = sizeof(uint64_t) * j;
		return SD_RES_NO_OBJ;
	}

	return SD_RES_SUCCESS;
}

static int local_cluster_info(const struct sd_req *req, struct sd_rsp *rsp,
			      void *data, const struct sd_node *sender)
{
	memcpy(data, &sys->cinfo, sizeof(sys->cinfo));
	rsp->data_length = sizeof(sys->cinfo);
	return SD_RES_SUCCESS;
}

#ifdef HAVE_NFS

static int local_nfs_create(struct request *req)
{
	return nfs_create(req->data);
}

static int local_nfs_delete(struct request *req)
{
	return nfs_delete(req->data);
}

#else

static inline int local_nfs_create(struct request *req)
{
	return 0;
}

static inline int local_nfs_delete(struct request *req)
{
	return 0;
}

#endif

static int local_repair_replica(struct request *req)
{
	int ret;
	struct node_id nid;
	struct sd_req hdr;
	struct sd_rsp *rsp = (struct sd_rsp *)&hdr;
	struct siocb iocb = { 0 };
	uint64_t oid = req->rq.forw.oid;
	size_t rlen = get_store_objsize(oid);
	void *buf = xvalloc(rlen);

	sd_init_req(&hdr, SD_OP_READ_PEER);
	hdr.epoch = req->rq.epoch;
	hdr.data_length = rlen;
	hdr.obj.oid = oid;

	memcpy(nid.addr, req->rq.forw.addr, sizeof(nid.addr));
	nid.port = req->rq.forw.port;
	ret = sheep_exec_req(&nid, &hdr, buf);
	if (ret == SD_RES_SUCCESS) {
		sd_debug("read object %016"PRIx64" from %s successfully, "
				"try saving to local", oid,
				addr_to_str(nid.addr, nid.port));
		iocb.epoch = req->rq.epoch;
		iocb.length = rsp->data_length;
		iocb.offset = rsp->obj.offset;
		iocb.buf = buf;
		ret = sd_store->create_and_write(oid, &iocb);
		if (ret != SD_RES_SUCCESS)
			sd_err("failed to write object %016"PRIx64
					" to local", oid);
	} else {
		sd_err("failed to read object %016"PRIx64
				" from %s: %s", oid,
				addr_to_str(nid.addr, nid.port),
				sd_strerror(ret));
	}

	free(buf);
	return ret;
}

static int local_get_cluster_default(const struct sd_req *req,
				     struct sd_rsp *rsp,
				     void *data, const struct sd_node *sender)
{
	rsp->cluster_default.nr_copies = sys->cinfo.nr_copies;
	rsp->cluster_default.copy_policy = sys->cinfo.copy_policy;
	rsp->cluster_default.block_size_shift = SD_DEFAULT_BLOCK_SIZE_SHIFT;

	return SD_RES_SUCCESS;
}

static int null_get_nid(struct request *req)
{
	memcpy(req->data, &sys->this_node.nid, sizeof(struct node_id));
	req->rp.data_length = sizeof(struct node_id);
	return SD_RES_SUCCESS;
}

static struct sd_op_template sd_ops[] = {

	/* NULL operations */
	[SD_OP_GET_NID] = {
		.name = "NULL",
		.type = SD_OP_TYPE_NONE,
		.force = true,
		.process_work = null_get_nid,
	},

	/* cluster operations */
	[SD_OP_NEW_VDI] = {
		.name = "NEW_VDI",
		.type = SD_OP_TYPE_CLUSTER,
		.is_admin_op = true,
		.process_work = cluster_new_vdi,
		.process_main = post_cluster_new_vdi,
	},

	[SD_OP_DEL_VDI] = {
		.name = "DEL_VDI",
		.type = SD_OP_TYPE_CLUSTER,
		.is_admin_op = true,
		.process_work = cluster_del_vdi,
		.process_main = post_cluster_del_vdi,
	},

	[SD_OP_MAKE_FS] = {
		.name = "MAKE_FS",
		.type = SD_OP_TYPE_CLUSTER,
		.force = true,
		.is_admin_op = true,
		.process_main = cluster_make_fs,
	},

	[SD_OP_SHUTDOWN] = {
		.name = "SHUTDOWN",
		.type = SD_OP_TYPE_CLUSTER,
		.force = true,
		.is_admin_op = true,
		.process_main = cluster_shutdown,
	},

	[SD_OP_GET_VDI_ATTR] = {
		.name = "GET_VDI_ATTR",
		.type = SD_OP_TYPE_CLUSTER,
		.process_work = cluster_get_vdi_attr,
	},

	[SD_OP_FORCE_RECOVER] = {
		.name = "FORCE_RECOVER",
		.type = SD_OP_TYPE_CLUSTER,
		.force = true,
		.is_admin_op = true,
		.process_work = cluster_force_recover_work,
		.process_main = cluster_force_recover_main,
	},

	[SD_OP_NOTIFY_VDI_ADD] = {
		.name = "NOTIFY_VDI_ADD",
		.type = SD_OP_TYPE_CLUSTER,
		.force = true,
		.process_main = cluster_notify_vdi_add,
	},

	[SD_OP_DELETE_CACHE] = {
		.name = "DELETE_CACHE",
		.type = SD_OP_TYPE_CLUSTER,
		.process_main = cluster_delete_cache,
	},

	[SD_OP_COMPLETE_RECOVERY] = {
		.name = "COMPLETE_RECOVERY",
		.type = SD_OP_TYPE_CLUSTER,
		.force = true,
		.process_main = cluster_recovery_completion,
	},

	[SD_OP_GET_VDI_INFO] = {
		.name = "GET_VDI_INFO",
		.type = SD_OP_TYPE_CLUSTER,
		.process_work = cluster_get_vdi_info,
	},

	[SD_OP_LOCK_VDI] = {
		.name = "LOCK_VDI",
		.type = SD_OP_TYPE_CLUSTER,
		.process_work = cluster_get_vdi_info,
	},

	[SD_OP_RELEASE_VDI] = {
		.name = "RELEASE_VDI",
		.type = SD_OP_TYPE_CLUSTER,
		.process_work = local_release_vdi,
	},

	[SD_OP_REWEIGHT] = {
		.name = "REWEIGHT",
		.type = SD_OP_TYPE_CLUSTER,
		.is_admin_op = true,
		.process_work = local_reconfig,
		.process_main = cluster_reconfig,
	},

	[SD_OP_ALTER_CLUSTER_COPY] = {
		.name = "ALTER_CLUSTER_COPY",
		.type = SD_OP_TYPE_CLUSTER,
		.is_admin_op = true,
		.process_main = cluster_alter_cluster_copy,
	},

	/* local operations */

	[SD_OP_GET_STORE_LIST] = {
		.name = "GET_STORE_LIST",
		.type = SD_OP_TYPE_LOCAL,
		.force = true,
		.process_work = local_get_store_list,
	},

	[SD_OP_READ_VDIS] = {
		.name = "READ_VDIS",
		.type = SD_OP_TYPE_LOCAL,
		.force = true,
		.process_main = local_read_vdis,
	},

	[SD_OP_GET_NODE_LIST] = {
		.name = "GET_NODE_LIST",
		.type = SD_OP_TYPE_LOCAL,
		.force = true,
		.process_main = local_get_node_list,
	},

	[SD_OP_STAT_SHEEP] = {
		.name = "STAT_SHEEP",
		.type = SD_OP_TYPE_LOCAL,
		.process_work = local_stat_sheep,
	},

	[SD_OP_STAT_RECOVERY] = {
		.name = "STAT_RECOVERY",
		.type = SD_OP_TYPE_LOCAL,
		.process_main = local_stat_recovery,
	},

	[SD_OP_STAT_CLUSTER] = {
		.name = "STAT_CLUSTER",
		.type = SD_OP_TYPE_LOCAL,
		.force = true,
		.process_work = local_stat_cluster,
	},

	[SD_OP_GET_OBJ_LIST] = {
		.name = "GET_OBJ_LIST",
		.type = SD_OP_TYPE_LOCAL,
		.process_work = local_get_obj_list,
	},

	[SD_OP_GET_EPOCH] = {
		.name = "GET_EPOCH",
		.type = SD_OP_TYPE_LOCAL,
		.process_work = local_get_epoch,
	},

	[SD_OP_FLUSH_VDI] = {
		.name = "FLUSH_VDI",
		.type = SD_OP_TYPE_LOCAL,
		.process_work = local_flush_vdi,
	},

	[SD_OP_DISCARD_OBJ] = {
		.name = "DISCARD_OBJ",
		.type = SD_OP_TYPE_LOCAL,
		.process_work = local_discard_obj,
	},

	[SD_OP_FLUSH_DEL_CACHE] = {
		.name = "DEL_CACHE",
		.type = SD_OP_TYPE_LOCAL,
		.process_work = local_flush_and_del,
	},

	[SD_OP_TRACE_ENABLE] = {
		.name = "TRACE_ENABLE",
		.type = SD_OP_TYPE_LOCAL,
		.force = true,
		.process_main = local_trace_enable,
	},

	[SD_OP_TRACE_DISABLE] = {
		.name = "TRACE_DISABLE",
		.type = SD_OP_TYPE_LOCAL,
		.force = true,
		.process_main = local_trace_disable,
	},

	[SD_OP_TRACE_STATUS] = {
		.name = "TRACE_STATUS",
		.type = SD_OP_TYPE_LOCAL,
		.force = true,
		.process_main = local_trace_status,
	},

	[SD_OP_TRACE_READ_BUF] = {
		.name = "TRACE_READ_BUF",
		.type = SD_OP_TYPE_LOCAL,
		.force = true,
		.process_work = local_trace_read_buf,
	},

	[SD_OP_LIVEPATCH_PATCH] = {
		.name = "LIVEPATCH_PATCH",
		.type = SD_OP_TYPE_CLUSTER,
		.force = true,
		.process_main = local_livepatch_patch,
	},

	[SD_OP_LIVEPATCH_UNPATCH] = {
		.name = "LIVEPATCH_UNPATCH",
		.type = SD_OP_TYPE_CLUSTER,
		.force = true,
		.process_main = local_livepatch_unpatch,
	},

	[SD_OP_LIVEPATCH_STATUS] = {
		.name = "LIVEPATCH_STATUS",
		.type = SD_OP_TYPE_LOCAL,
		.force = true,
		.process_main = local_livepatch_status,
	},

	[SD_OP_KILL_NODE] = {
		.name = "KILL_NODE",
		.type = SD_OP_TYPE_LOCAL,
		.force = true,
		.is_admin_op = true,
		.process_main = local_kill_node,
	},

	[SD_OP_MD_INFO] = {
		.name = "MD_INFO",
		.type = SD_OP_TYPE_LOCAL,
		.process_work = local_md_info,
	},

	[SD_OP_MD_PLUG] = {
		.name = "MD_PLUG_DISKS",
		.type = SD_OP_TYPE_LOCAL,
		.is_admin_op = true,
		.process_main = local_md_plug,
	},

	[SD_OP_MD_UNPLUG] = {
		.name = "MD_UNPLUG_DISKS",
		.type = SD_OP_TYPE_LOCAL,
		.is_admin_op = true,
		.process_main = local_md_unplug,
	},

	[SD_OP_GET_HASH] = {
		.name = "GET_HASH",
		.type = SD_OP_TYPE_LOCAL,
		.process_work = local_get_hash,
	},

	[SD_OP_GET_CACHE_INFO] = {
		.name = "GET_CACHE_INFO",
		.type = SD_OP_TYPE_LOCAL,
		.process_work = local_get_cache_info,
	},

	[SD_OP_CACHE_PURGE] = {
		.name = "CACHE_PURGE",
		.type = SD_OP_TYPE_LOCAL,
		.process_work = local_cache_purge,
	},

	[SD_OP_STAT] = {
		.name = "STAT",
		.type = SD_OP_TYPE_LOCAL,
		.process_main = local_sd_stat,
	},

	[SD_OP_GET_LOGLEVEL] = {
		.name = "GET_LOGLEVEL",
		.type = SD_OP_TYPE_LOCAL,
		.force = true,
		.process_work = local_get_loglevel,
	},

	[SD_OP_SET_LOGLEVEL] = {
		.name = "SET_LOGLEVEL",
		.type = SD_OP_TYPE_LOCAL,
		.force = true,
		.process_work = local_set_loglevel,
	},

	[SD_OP_EXIST] =  {
		.name = "EXIST",
		.type = SD_OP_TYPE_LOCAL,
		.force = true,
		.process_work = local_oid_exist,
	},

	[SD_OP_OIDS_EXIST] =  {
		.name = "OIDS_EXIST",
		.type = SD_OP_TYPE_LOCAL,
		.force = true,
		.process_main = local_oids_exist,
	},

	[SD_OP_CLUSTER_INFO] = {
		.name = "CLUSTER INFO",
		.type = SD_OP_TYPE_LOCAL,
		.force = true,
		.process_main = local_cluster_info,
	},

#ifdef HAVE_NFS
	[SD_OP_NFS_CREATE] = {
		.name = "NFS_CREATE",
		.type = SD_OP_TYPE_LOCAL,
		.force = false,
		.process_work = local_nfs_create,
	},

	[SD_OP_NFS_DELETE] = {
		.name = "NFS_DELETE",
		.type = SD_OP_TYPE_LOCAL,
		.force = false,
		.process_work = local_nfs_delete,
	},
#endif

	[SD_OP_REPAIR_REPLICA] = {
		.name = "REPAIR_REPLICA",
		.type = SD_OP_TYPE_LOCAL,
		.process_work = local_repair_replica,
	},

	[SD_OP_GET_CLUSTER_DEFAULT] = {
		.name = "GET_CLUSTER_DEFAULT",
		.type = SD_OP_TYPE_LOCAL,
		.force = true,
		.process_main = local_get_cluster_default,
	},

	/* gateway I/O operations */
	[SD_OP_CREATE_AND_WRITE_OBJ] = {
		.name = "CREATE_AND_WRITE_OBJ",
		.type = SD_OP_TYPE_GATEWAY,
		.process_work = gateway_create_object,
	},

	[SD_OP_READ_OBJ] = {
		.name = "READ_OBJ",
		.type = SD_OP_TYPE_GATEWAY,
		.process_work = gateway_read_object,
	},

	[SD_OP_WRITE_OBJ] = {
		.name = "WRITE_OBJ",
		.type = SD_OP_TYPE_GATEWAY,
		.process_work = gateway_write_object,
	},

	[SD_OP_REMOVE_OBJ] = {
		.name = "REMOVE_OBJ",
		.type = SD_OP_TYPE_GATEWAY,
		.process_work = gateway_remove_object,
	},

	[SD_OP_UNREF_OBJ] = {
		.name = "UNREF_OBJ",
		.type = SD_OP_TYPE_GATEWAY,
		.process_work = gateway_unref_object,
	},

	/* peer I/O operations */
	[SD_OP_CREATE_AND_WRITE_PEER] = {
		.name = "CREATE_AND_WRITE_PEER",
		.type = SD_OP_TYPE_PEER,
		.process_work = peer_create_and_write_obj,
	},

	[SD_OP_READ_PEER] = {
		.name = "READ_PEER",
		.type = SD_OP_TYPE_PEER,
		.process_work = peer_read_obj,
	},

	[SD_OP_WRITE_PEER] = {
		.name = "WRITE_PEER",
		.type = SD_OP_TYPE_PEER,
		.process_work = peer_write_obj,
	},

	[SD_OP_REMOVE_PEER] = {
		.name = "REMOVE_PEER",
		.type = SD_OP_TYPE_PEER,
		.process_work = peer_remove_obj,
	},
};

const struct sd_op_template *get_sd_op(uint8_t opcode)
{
	if (sd_ops[opcode].type == 0)
		return NULL;

	return sd_ops + opcode;
}

const char *op_name(const struct sd_op_template *op)
{
	if (op == NULL)
		return "(invalid opcode)";

	return op->name;
}

bool is_null_op(const struct sd_op_template *op)
{
	return op != NULL && op->type == SD_OP_TYPE_NONE;
}

bool is_cluster_op(const struct sd_op_template *op)
{
	return op != NULL && op->type == SD_OP_TYPE_CLUSTER;
}

bool is_local_op(const struct sd_op_template *op)
{
	return op != NULL && op->type == SD_OP_TYPE_LOCAL;
}

bool is_peer_op(const struct sd_op_template *op)
{
	return op != NULL && op->type == SD_OP_TYPE_PEER;
}

bool is_gateway_op(const struct sd_op_template *op)
{
	return op != NULL && op->type == SD_OP_TYPE_GATEWAY;
}

bool is_force_op(const struct sd_op_template *op)
{
	return op != NULL && op->force;
}

bool is_logging_op(const struct sd_op_template *op)
{
	return op != NULL && op->is_admin_op;
}

bool has_process_work(const struct sd_op_template *op)
{
	return op != NULL && !!op->process_work;
}

bool has_process_main(const struct sd_op_template *op)
{
	return op != NULL && !!op->process_main;
}

void do_process_work(struct work *work)
{
	struct request *req = container_of(work, struct request, work);
	int ret = SD_RES_SUCCESS;

	sd_debug("%x, %" PRIx64", %"PRIu32, req->rq.opcode, req->rq.obj.oid,
		 req->rq.epoch);

	if (req->op->process_work)
		ret = req->op->process_work(req);

	if (ret != SD_RES_SUCCESS) {
		sd_debug("failed: %x, %" PRIx64" , %u, %s", req->rq.opcode,
			 req->rq.obj.oid, req->rq.epoch, sd_strerror(ret));
	}

	req->rp.result = ret;
}

int do_process_main(const struct sd_op_template *op, const struct sd_req *req,
		    struct sd_rsp *rsp, void *data,
		    const struct sd_node *sender)
{
	return op->process_main(req, rsp, data, sender);
}

int run_null_request(struct request *req)
{
	return req->op->process_work(req);
}

static int map_table[] = {
	[SD_OP_CREATE_AND_WRITE_OBJ] = SD_OP_CREATE_AND_WRITE_PEER,
	[SD_OP_READ_OBJ] = SD_OP_READ_PEER,
	[SD_OP_WRITE_OBJ] = SD_OP_WRITE_PEER,
	[SD_OP_REMOVE_OBJ] = SD_OP_REMOVE_PEER,
};

int gateway_to_peer_opcode(int opcode)
{
	sd_assert(opcode < ARRAY_SIZE(map_table));
	return map_table[opcode];
}
