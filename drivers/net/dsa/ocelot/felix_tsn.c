// SPDX-License-Identifier: (GPL-2.0 OR MIT)
/* Felix Switch TSN driver
 *
 * Copyright 2020 NXP
 */

#include <soc/mscc/ocelot_qsys.h>
#include <soc/mscc/ocelot_ana.h>
#include <soc/mscc/ocelot_dev.h>
#include <soc/mscc/ocelot_sys.h>
#include <soc/mscc/ocelot_ptp.h>
#include <soc/mscc/ocelot.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pcs-lynx.h>
#include <linux/io.h>
#include <net/dsa.h>
#include <net/tsn.h>
#include "felix_tsn.h"
#include "felix.h"

#define ETH_P_8021CB		0xF1C1
#define FELIX_QSYS_HSCH_NUM	72
/* MSCC TSN parameters limited */
#define FELIX_PSFP_SFID_NUM	176
#define FELIX_FRER_SSID_NUM	128
#define FELIX_STREAM_NUM	2048

struct felix_switch_capa {
	u8 num_tas_gcl;
	u32 tas_ct_min;
	u32 tas_ct_max;
	u32 tas_cte_max;
	u32 tas_it_max;
	u32 tas_it_min;
	u8 num_hsch;
	u8 num_psfp_sfid;
	u8 num_frer_ssid;
	u8 num_psfp_sgid;
	u16 psfp_fmi_max;
	u16 psfp_fmi_min;
	u8 num_sgi_gcl;
	u32 sgi_ct_min;
	u32 sgi_ct_max;
	u32 sgi_cte_max;
	u16 qos_pol_max;
	u8 pol_cbs_max;
	u8 pol_pbs_max;
	u8 frer_seq_len_min;
	u8 frer_seq_len_max;
	u8 frer_his_len_min;
	u8 frer_his_len_max;
	u8 qos_dscp_max;
	u8 qos_cos_max;
	u8 qos_dp_max;
};

struct stream_filter {
	struct list_head list;
	unsigned char mac[ETH_ALEN];
	int vid;
	u32 index;
	u8 handle;
};

static struct list_head streamtable;
static int hsch_bw[FELIX_QSYS_HSCH_NUM] = {0};

static const struct felix_switch_capa capa = {
	.num_tas_gcl	= 64,
	.tas_ct_min	= 100,
	.tas_ct_max	= 1000000000,
	.tas_cte_max	= 999999999,
	.tas_it_max	= 999999999,
	.tas_it_min	= 1000,
	.num_hsch	= 72,
	.num_psfp_sfid	= FELIX_PSFP_SFID_NUM,
	.num_psfp_sgid	= 184,
	.psfp_fmi_max	= 246,
	.psfp_fmi_min	= 63,
	.num_sgi_gcl	= 4,
	.sgi_ct_min	= 5000,
	.sgi_ct_max	= 1000000000,
	.sgi_cte_max	= 999999999,
	.qos_pol_max	= 383,
	/* Maximum allowed value of committed burst size(CBS) is 240 KB */
	.pol_cbs_max	= 60,
	/* Maximum allowed value of excess burst size(EBS) is 240 KB */
	.pol_pbs_max	= 60,
	.num_frer_ssid  = FELIX_FRER_SSID_NUM,
	.frer_seq_len_min = 1,
	.frer_seq_len_max = 28,
	.frer_his_len_min = 1,
	.frer_his_len_max = 32,
	.qos_dscp_max	= 63,
	.qos_cos_max	= FELIX_NUM_TC - 1,
	.qos_dp_max	= 1,
};

static u32 felix_tsn_get_cap(struct net_device *ndev)
{
	u32 cap = 0;

	cap = (TSN_CAP_QBV | TSN_CAP_QCI | TSN_CAP_QBU | TSN_CAP_CBS |
	       TSN_CAP_CB | TSN_CAP_TBS | TSN_CAP_CTH);

	return cap;
}

static void felix_get_basetime(struct ocelot *ocelot, ptptime_t basetime,
			       u64 cycle_time, struct timespec64 *ts_base)
{
	ptptime_t new_basetime;
	ptptime_t cur_time;

	ocelot_ptp_gettime64(&ocelot->ptp_info, ts_base);
	cur_time = ts_base->tv_sec * NSEC_PER_SEC + ts_base->tv_nsec;

	new_basetime = basetime;
	if (cur_time > basetime) {
		u64 nr_of_cycles = cur_time - basetime;

		do_div(nr_of_cycles, cycle_time);
		new_basetime += cycle_time * (nr_of_cycles + 1);
	}

	ts_base->tv_sec = new_basetime / NSEC_PER_SEC;
	ts_base->tv_nsec = new_basetime % NSEC_PER_SEC;
}

static int felix_tas_gcl_set(struct ocelot *ocelot, const u8 gcl_ix,
			     struct tsn_qbv_entry *control_list)
{
	if (gcl_ix >= capa.num_tas_gcl) {
		dev_err(ocelot->dev, "Invalid gcl ix %u\n", gcl_ix);
		return -EINVAL;
	}
	if (control_list->time_interval < capa.tas_it_min ||
	    control_list->time_interval > capa.tas_it_max) {
		dev_err(ocelot->dev, "Invalid time_interval %u\n",
			control_list->time_interval);

		return -EINVAL;
	}

	ocelot_write(ocelot,
		     QSYS_GCL_CFG_REG_1_GCL_ENTRY_NUM(gcl_ix) |
		     QSYS_GCL_CFG_REG_1_GATE_STATE(control_list->gate_state),
		     QSYS_GCL_CFG_REG_1);

	ocelot_write(ocelot,
		     control_list->time_interval,
		     QSYS_GCL_CFG_REG_2);

	return 0;
}

static u32 felix_tas_read_status(struct ocelot *ocelot)
{
	u32 val;

	val = ocelot_read(ocelot, QSYS_TAS_PARAM_CFG_CTRL);

	return val;
}

static int felix_qbv_set(struct net_device *ndev,
			 struct tsn_qbv_conf *shaper_config)
{
	struct tsn_qbv_basic *admin_basic = &shaper_config->admin;
	struct tsn_qbv_entry *control_list = admin_basic->control_list;
	struct timespec64 ts_base;
	struct ocelot *ocelot;
	struct dsa_port *dp;
	int i, ret, port;
	u32 val;

	dp = dsa_port_from_netdev(ndev);
	ocelot = dp->ds->priv;
	port = dp->index;

	if (admin_basic->control_list_length > capa.num_tas_gcl) {
		dev_err(ocelot->dev,
			"Invalid admin_control_list_length %u\n",
			admin_basic->control_list_length);
		return -EINVAL;
	}

	if ((admin_basic->cycle_time < capa.tas_ct_min ||
	     admin_basic->cycle_time > capa.tas_ct_max) &&
	    shaper_config->gate_enabled) {
		dev_err(ocelot->dev, "Invalid admin_cycle_time %u ns\n",
			admin_basic->cycle_time);
		return -EINVAL;
	}
	if (admin_basic->cycle_time_extension > capa.tas_cte_max) {
		dev_err(ocelot->dev,
			"Invalid admin_cycle_time_extension %u\n",
			admin_basic->cycle_time_extension);
		return -EINVAL;
	}

	felix_get_basetime(ocelot, admin_basic->base_time,
			   admin_basic->cycle_time, &ts_base);

	/* Select port */
	ocelot_rmw(ocelot,
		   QSYS_TAS_PARAM_CFG_CTRL_PORT_NUM(port),
		   QSYS_TAS_PARAM_CFG_CTRL_ALWAYS_GUARD_BAND_SCH_Q |
		   QSYS_TAS_PARAM_CFG_CTRL_PORT_NUM_M,
		   QSYS_TAS_PARAM_CFG_CTRL);

	val = ocelot_read(ocelot, QSYS_PARAM_STATUS_REG_8);
	if (val & QSYS_PARAM_STATUS_REG_8_CONFIG_PENDING) {
		ocelot_rmw_rix(ocelot, 0, QSYS_TAG_CONFIG_ENABLE,
			       QSYS_TAG_CONFIG, port);
	}

	if (!shaper_config->gate_enabled)
		admin_basic->gate_states = 0xff;

	ocelot_rmw_rix(ocelot,
		       (shaper_config->gate_enabled ? QSYS_TAG_CONFIG_ENABLE : 0) |
		       QSYS_TAG_CONFIG_INIT_GATE_STATE(admin_basic->gate_states) |
		       QSYS_TAG_CONFIG_SCH_TRAFFIC_QUEUES(0xff),
		       QSYS_TAG_CONFIG_ENABLE |
		       QSYS_TAG_CONFIG_INIT_GATE_STATE_M |
		       QSYS_TAG_CONFIG_SCH_TRAFFIC_QUEUES_M,
		       QSYS_TAG_CONFIG,
		       port);

	ocelot_write_rix(ocelot, shaper_config->maxsdu,
			 QSYS_PORT_MAX_SDU, port);

	if (shaper_config->gate_enabled) {
		ocelot_write(ocelot, ts_base.tv_nsec,
			     QSYS_PARAM_CFG_REG_1);

		ocelot_write(ocelot, lower_32_bits(ts_base.tv_sec),
			     QSYS_PARAM_CFG_REG_2);

		val = upper_32_bits(ts_base.tv_sec);
		ocelot_write(ocelot,
			     QSYS_PARAM_CFG_REG_3_BASE_TIME_SEC_MSB(val) |
			     QSYS_PARAM_CFG_REG_3_LIST_LENGTH(admin_basic->control_list_length),
			     QSYS_PARAM_CFG_REG_3);

		ocelot_write(ocelot, admin_basic->cycle_time,
			     QSYS_PARAM_CFG_REG_4);

		ocelot_write(ocelot, admin_basic->cycle_time_extension,
			     QSYS_PARAM_CFG_REG_5);

		for (i = 0; i < admin_basic->control_list_length; i++) {
			felix_tas_gcl_set(ocelot, i, control_list);
			control_list++;
		}

		/* Start configuration change */
		ocelot_rmw(ocelot,
			   QSYS_TAS_PARAM_CFG_CTRL_CONFIG_CHANGE,
			   QSYS_TAS_PARAM_CFG_CTRL_CONFIG_CHANGE,
			   QSYS_TAS_PARAM_CFG_CTRL);

		ret = readx_poll_timeout(felix_tas_read_status, ocelot, val,
					 !(QSYS_TAS_PARAM_CFG_CTRL_CONFIG_CHANGE
					 & val), 10, 100000);
		return ret;
	}

	return 0;
}

static int felix_qbv_get(struct net_device *ndev, struct tsn_qbv_conf *shaper_config)
{
	struct tsn_qbv_basic *admin = &shaper_config->admin;
	struct tsn_qbv_entry *list;
	u32 base_timel, base_timeh;
	struct ocelot *ocelot;
	struct dsa_port *dp;
	u32 val, reg;
	int i, port;

	dp = dsa_port_from_netdev(ndev);
	ocelot = dp->ds->priv;
	port = dp->index;

	ocelot_rmw(ocelot,
		   QSYS_TAS_PARAM_CFG_CTRL_PORT_NUM(port),
		   QSYS_TAS_PARAM_CFG_CTRL_PORT_NUM_M,
		   QSYS_TAS_PARAM_CFG_CTRL);

	val = ocelot_read_rix(ocelot, QSYS_TAG_CONFIG, port);
	shaper_config->gate_enabled = (val & QSYS_TAG_CONFIG_ENABLE);
	admin->gate_states = QSYS_TAG_CONFIG_INIT_GATE_STATE_X(val);

	base_timel = ocelot_read(ocelot, QSYS_PARAM_CFG_REG_1);
	base_timeh = ocelot_read(ocelot, QSYS_PARAM_CFG_REG_2);
	reg = ocelot_read(ocelot, QSYS_PARAM_CFG_REG_3);
	admin->base_time = base_timeh |
		(((u64)QSYS_PARAM_CFG_REG_3_BASE_TIME_SEC_MSB(reg)) << 32);

	admin->base_time = (admin->base_time * 1000000000) + base_timel;

	admin->control_list_length =
		QSYS_PARAM_CFG_REG_3_LIST_LENGTH_X(reg);

	admin->cycle_time = ocelot_read(ocelot, QSYS_PARAM_CFG_REG_4);
	admin->cycle_time_extension =
		ocelot_read(ocelot, QSYS_PARAM_CFG_REG_5);

	list = kmalloc_array(admin->control_list_length,
			     sizeof(struct tsn_qbv_entry), GFP_KERNEL);
	if (!list)
		return -ENOMEM;

	admin->control_list = list;

	for (i = 0; i < admin->control_list_length; i++) {
		ocelot_rmw(ocelot,
			   QSYS_GCL_CFG_REG_1_GCL_ENTRY_NUM(i),
			   QSYS_GCL_CFG_REG_1_GCL_ENTRY_NUM_M,
			   QSYS_GCL_CFG_REG_1);

		list->time_interval =
			ocelot_read(ocelot, QSYS_GCL_CFG_REG_2);

		reg = ocelot_read(ocelot, QSYS_GCL_CFG_REG_1);
		list->gate_state = QSYS_GCL_CFG_REG_1_GATE_STATE_X(reg);

		list++;
	}

	return 0;
}

static int felix_qbv_get_gatelist(struct ocelot *ocelot,
				  struct tsn_qbv_basic *oper)
{
	u32 base_timel;
	u32 base_timeh;
	u32 val;
	struct tsn_qbv_entry *glist;
	int i;

	base_timel = ocelot_read(ocelot, QSYS_PARAM_STATUS_REG_1);
	base_timeh = ocelot_read(ocelot, QSYS_PARAM_STATUS_REG_2);
	val = ocelot_read(ocelot, QSYS_PARAM_STATUS_REG_3);
	oper->base_time = base_timeh;
	oper->base_time +=
		((u64)QSYS_PARAM_STATUS_REG_3_BASE_TIME_SEC_MSB(val)) <<
		32;
	oper->base_time = (oper->base_time * 1000000000) + base_timel;

	oper->control_list_length =
		QSYS_PARAM_STATUS_REG_3_LIST_LENGTH_X(val);

	oper->cycle_time = ocelot_read(ocelot, QSYS_PARAM_STATUS_REG_4);
	oper->cycle_time_extension = ocelot_read(ocelot,
						 QSYS_PARAM_STATUS_REG_5);

	val = ocelot_read(ocelot, QSYS_PARAM_STATUS_REG_8);
	oper->gate_states = QSYS_PARAM_STATUS_REG_8_OPER_GATE_STATE_X(val);

	glist = kmalloc_array(oper->control_list_length,
			      sizeof(struct tsn_qbv_entry), GFP_KERNEL);
	if (!glist)
		return -ENOMEM;

	oper->control_list = glist;

	for (i = 0; i < oper->control_list_length; i++) {
		ocelot_rmw(ocelot,
			   QSYS_GCL_STATUS_REG_1_GCL_ENTRY_NUM(i),
			   QSYS_GCL_STATUS_REG_1_GCL_ENTRY_NUM_M,
			   QSYS_GCL_STATUS_REG_1);

		val = ocelot_read(ocelot, QSYS_GCL_STATUS_REG_2);
		glist->time_interval = val;
		val = ocelot_read(ocelot, QSYS_GCL_STATUS_REG_1);
		glist->gate_state =
			QSYS_GCL_STATUS_REG_1_GATE_STATE_X(val);

		glist++;
	}

	return 0;
}

static int felix_qbv_get_status(struct net_device *ndev,
				struct tsn_qbv_status *qbvstatus)
{
	struct tsn_qbv_basic *oper = &qbvstatus->oper;
	struct ocelot *ocelot;
	struct timespec64 ts;
	struct dsa_port *dp;
	ptptime_t cur_time;
	int port;
	u32 val;

	dp = dsa_port_from_netdev(ndev);
	ocelot = dp->ds->priv;
	port = dp->index;

	ocelot_rmw(ocelot,
		   QSYS_TAS_PARAM_CFG_CTRL_PORT_NUM(port),
		   QSYS_TAS_PARAM_CFG_CTRL_PORT_NUM_M,
		   QSYS_TAS_PARAM_CFG_CTRL);

	qbvstatus->supported_list_max = capa.num_tas_gcl;

	val = ocelot_read(ocelot, QSYS_PARAM_STATUS_REG_8);
	qbvstatus->config_pending =
		(val & QSYS_PARAM_STATUS_REG_8_CONFIG_PENDING) ? 1 : 0;

	qbvstatus->config_change_time =
		ocelot_read(ocelot, QSYS_PARAM_STATUS_REG_7);

	qbvstatus->config_change_time +=
		((u64)QSYS_PARAM_STATUS_REG_8_CFG_CHG_TIME_SEC_MSB(val)) <<
		32;

	qbvstatus->config_change_time =
		(qbvstatus->config_change_time * 1000000000) +
		ocelot_read(ocelot, QSYS_PARAM_STATUS_REG_6);

	qbvstatus->config_change_error =
		ocelot_read(ocelot, QSYS_PARAM_STATUS_REG_9);

	ocelot_ptp_gettime64(&ocelot->ptp_info, &ts);
	cur_time = ts.tv_sec * NSEC_PER_SEC + ts.tv_nsec;

	qbvstatus->current_time = cur_time;
	felix_qbv_get_gatelist(ocelot, oper);

	return 0;
}

static int felix_qbu_set(struct net_device *ndev, u8 preemptible)
{
	struct ocelot_port *ocelot_port;
	struct ocelot *ocelot;
	struct dsa_port *dp;
	int port;

	dp = dsa_port_from_netdev(ndev);
	ocelot = dp->ds->priv;
	port = dp->index;

	ocelot_port = ocelot->ports[port];

	ocelot_port_rmwl(ocelot_port,
			 DEV_MM_CONFIG_ENABLE_CONFIG_MM_RX_ENA |
			 DEV_MM_CONFIG_ENABLE_CONFIG_MM_TX_ENA,
			 DEV_MM_CONFIG_ENABLE_CONFIG_MM_RX_ENA |
			 DEV_MM_CONFIG_ENABLE_CONFIG_MM_TX_ENA,
			 DEV_MM_ENABLE_CONFIG);

	ocelot_rmw_rix(ocelot,
		       QSYS_PREEMPTION_CFG_P_QUEUES(preemptible),
		       QSYS_PREEMPTION_CFG_P_QUEUES_M,
		       QSYS_PREEMPTION_CFG,
		       port);

	return 0;
}

static int felix_qbu_get(struct net_device *ndev, struct tsn_preempt_status *c)
{
	struct ocelot_port *ocelot_port;
	struct ocelot *ocelot;
	struct dsa_port *dp;
	int port;
	u32 val;

	dp = dsa_port_from_netdev(ndev);
	ocelot = dp->ds->priv;
	port = dp->index;

	ocelot_port = ocelot->ports[port];

	val = ocelot_read_rix(ocelot, QSYS_PREEMPTION_CFG, port);

	c->admin_state = QSYS_PREEMPTION_CFG_P_QUEUES(val);
	c->hold_advance = QSYS_PREEMPTION_CFG_HOLD_ADVANCE_X(val);

	val = ocelot_port_readl(ocelot_port, DEV_MM_STATUS);
	c->preemption_active =
		DEV_MM_STATISTICS_MM_STATUS_PRMPT_ACTIVE_STATUS & val;

	return 0;
}

static int felix_stream_table_add(u32 index, const unsigned char mac[ETH_ALEN],
				  int vid, u8 handle)
{
	struct stream_filter *stream, *tmp;
	struct list_head *pos, *q;

	list_for_each_safe(pos, q, &streamtable) {
		tmp = list_entry(pos, struct stream_filter, list);
		if (tmp->index == index) {
			ether_addr_copy(tmp->mac, mac);
			tmp->vid = vid;
			tmp->handle = handle;
			return 0;
		}
		if (tmp->index > index)
			break;
	}
	stream = kzalloc(sizeof(*stream), GFP_KERNEL);
	if (!stream)
		return -ENOMEM;

	stream->index = index;
	ether_addr_copy(stream->mac, mac);
	stream->vid = vid;
	stream->handle = handle;
	list_add(&stream->list, pos->prev);

	return 0;
}

static void felix_stream_table_del(u32 index)
{
	struct stream_filter *tmp;
	struct list_head *pos, *q;

	list_for_each_safe(pos, q, &streamtable) {
		tmp = list_entry(pos, struct stream_filter, list);
		if (tmp->index == index) {
			list_del(pos);
			kfree(tmp);
			break;
		}
	}
}

static struct stream_filter *felix_stream_table_get(u32 index)
{
	struct stream_filter *tmp;

	list_for_each_entry(tmp, &streamtable, list)
		if (tmp->index == index)
			return tmp;

	return NULL;
}

static int felix_cb_streamid_set(struct net_device *ndev, u32 index, bool enable,
				 struct tsn_cb_streamid *streamid)
{
	struct ocelot_mact_entry entry;
	struct stream_filter *stream;
	u32 m_index, bucket, dst_idx;
	unsigned char mac[ETH_ALEN];
	int sfid, ssid, port;
	struct ocelot *ocelot;
	struct dsa_port *dp;
	u16 vid;
	int ret;

	dp = dsa_port_from_netdev(ndev);
	ocelot = dp->ds->priv;
	port = dp->index;

	if (index >= FELIX_STREAM_NUM) {
		dev_err(ocelot->dev, "Invalid index %u, maximum:%u\n",
			index, FELIX_STREAM_NUM - 1);
		return -EINVAL;
	}

	if (!enable) {
		stream = felix_stream_table_get(index);
		if (!stream)
			return -EINVAL;

		ocelot_mact_forget(ocelot, stream->mac, stream->vid);
		felix_stream_table_del(index);

		return 0;
	}

	if (streamid->type != 1) {
		dev_err(ocelot->dev, "Invalid stream type\n");
		return -EINVAL;
	}

	if (streamid->handle >= FELIX_PSFP_SFID_NUM) {
		dev_err(ocelot->dev,
			"Invalid stream handle %u, maximum:%u\n",
			streamid->handle, FELIX_PSFP_SFID_NUM - 1);
		return -EINVAL;
	}

	sfid = streamid->handle;
	ssid = (streamid->handle < FELIX_FRER_SSID_NUM ?
		streamid->handle : (FELIX_FRER_SSID_NUM - 1));

	u64_to_ether_addr(streamid->para.nid.dmac, mac);
	vid = streamid->para.nid.vid;

	ret = ocelot_mact_lookup(ocelot, mac, vid, &m_index, &bucket);
	if (ret && ret != -ENOENT)
		return ret;

	if (ret == -ENOENT) {
		/* The MAC table doesn't contain this entry, learn it as static
		 * and annotate it with a SSID and a SFID.
		 */
		ocelot_write(ocelot,
			     ANA_TABLES_STREAMDATA_SFID_VALID |
			     ANA_TABLES_STREAMDATA_SFID(sfid) |
			     ANA_TABLES_STREAMDATA_SSID_VALID |
			     ANA_TABLES_STREAMDATA_SSID(ssid),
			     ANA_TABLES_STREAMDATA);

		ocelot_mact_learn(ocelot, port, mac, vid, ENTRYTYPE_LOCKED);

		return felix_stream_table_add(index, mac, vid, streamid->handle);
	}

	/* The {DMAC, VLAN} pair was in the MAC table (either dynamically
	 * learned or installed statically by the CPU). Read back the entry,
	 * and re-write it while annotating it with a SSID and a SFID.
	 */

	/* This is done just to retrieve the @dst_idx */
	ret = ocelot_mact_read(ocelot, m_index, bucket, &dst_idx, &entry);
	if (ret)
		return ret;

	ocelot_write(ocelot,
		     ANA_TABLES_STREAMDATA_SFID_VALID |
		     ANA_TABLES_STREAMDATA_SFID(sfid) |
		     ANA_TABLES_STREAMDATA_SSID_VALID |
		     ANA_TABLES_STREAMDATA_SSID(ssid),
		     ANA_TABLES_STREAMDATA);

	/* Whatever the entry type was before,
	 * make sure that now it's static
	 */
	entry.type = ENTRYTYPE_LOCKED;

	ocelot_mact_write(ocelot, dst_idx, &entry, m_index, bucket);

	return felix_stream_table_add(index, mac, vid, streamid->handle);
}

static int felix_cb_streamid_get(struct net_device *ndev, u32 index,
				 struct tsn_cb_streamid *streamid)
{
	struct ocelot_mact_entry entry;
	struct stream_filter *stream;
	struct ocelot *ocelot;
	struct dsa_port *dp;
	u32 val, dst, fwdmask;
	u32 m_index, bucket;
	int ret;

	dp = dsa_port_from_netdev(ndev);
	ocelot = dp->ds->priv;

	if (index >= FELIX_STREAM_NUM) {
		dev_err(ocelot->dev,
			"Invalid stream handle %u, maximum:%u\n",
			index, FELIX_STREAM_NUM - 1);
		return -EINVAL;
	}

	stream = felix_stream_table_get(index);
	if (!stream)
		return -EINVAL;

	ret = ocelot_mact_lookup(ocelot, stream->mac, stream->vid,
				 &m_index, &bucket);
	if (ret)
		return ret;

	/* This is done just to retrieve the @dst */
	ret = ocelot_mact_read(ocelot, m_index, bucket, &dst, &entry);
	if (ret)
		return ret;

	streamid->type = 1;

	fwdmask = ocelot_read_rix(ocelot, ANA_PGID_PGID, dst);
	streamid->ofac_oport = ANA_PGID_PGID_PGID(fwdmask);

	streamid->para.nid.dmac = ether_addr_to_u64(entry.mac);
	streamid->para.nid.vid = entry.vid;

	val = ocelot_read(ocelot, ANA_TABLES_STREAMDATA);
	if (!(val & ANA_TABLES_STREAMDATA_SFID_VALID))
		return -EINVAL;

	streamid->handle = ANA_TABLES_STREAMDATA_SFID(val);

	return 0;
}

/* Modify all 802.1CB stream entries that are configured for splitting to use
 * the PGID_FRER multicast PGID. This is to avoid an issue with using a unicast
 * port PGID, where stream splitting stops working when that port goes down.
 */
static int felix_streamid_multi_forward_set(struct ocelot *ocelot,
					    const unsigned char mac[ETH_ALEN],
					    int vid, u8 fwdmask)
{
	int ret, m_index, bucket, dst_idx;
	struct ocelot_mact_entry entry;
	u32 val;

	ret = ocelot_mact_lookup(ocelot, mac, vid, &m_index, &bucket);
	if (ret)
		return ret;

	/* This is done just to retrieve the @fwdport */
	ret = ocelot_mact_read(ocelot, m_index, bucket, &dst_idx, &entry);
	if (ret)
		return ret;

	val = ocelot_read_rix(ocelot, ANA_PGID_PGID, dst_idx);

	fwdmask |= val;

	ocelot_mact_write(ocelot, PGID_FRER, &entry, m_index, bucket);
	ocelot_write_rix(ocelot, fwdmask, ANA_PGID_PGID, PGID_FRER);

	return 0;
}

static int felix_cb_streamid_counters_get(struct net_device *ndev, u32 index,
					  struct tsn_cb_streamid_counters *sc)
{
	return 0;
}

static int felix_qci_sfi_set(struct net_device *ndev, u32 index, bool enable,
			     struct tsn_qci_psfp_sfi_conf *sfi)
{
	int igr_prio = sfi->priority_spec;
	u16 sgid  = sfi->stream_gate_instance_id;
	int fmid = sfi->stream_filter.flow_meter_instance_id;
	u16 max_sdu_len = sfi->stream_filter.maximum_sdu_size;
	struct ocelot *ocelot;
	struct dsa_port *dp;
	int sfid = index;
	int i, port;
	u16 pol_idx;
	u32 val;

	dp = dsa_port_from_netdev(ndev);
	ocelot = dp->ds->priv;
	port = dp->index;

	if (fmid == -1)
		pol_idx = capa.psfp_fmi_max;
	else
		pol_idx = (u16)fmid;

	if (!enable) {
		val = ANA_TABLES_SFIDACCESS_SFID_TBL_CMD(SFIDACCESS_CMD_WRITE);
		ocelot_write(ocelot,
			     ANA_TABLES_SFIDTIDX_SFID_INDEX(sfid),
			     ANA_TABLES_SFIDTIDX);
		ocelot_write(ocelot, val, ANA_TABLES_SFIDACCESS);
		return 0;
	}

	/* Port default SFID set */
	if (sfi->stream_handle_spec < 0) {
		val = ANA_PORT_SFID_CFG_SFID_VALID |
			ANA_PORT_SFID_CFG_SFID(sfid);
		if (igr_prio < 0) {
			for (i = 0; i < FELIX_NUM_TC; i++)
				ocelot_write_ix(ocelot, val, ANA_PORT_SFID_CFG,
						port, i);
		} else {
			ocelot_write_ix(ocelot, val, ANA_PORT_SFID_CFG,
					port, igr_prio & 0x7);
		}
	} else if (index != sfi->stream_handle_spec) {
		dev_err(ocelot->dev, "Index must equal to streamHandle\n");
		return -EINVAL;
	}

	if (sgid >= capa.num_psfp_sgid) {
		dev_err(ocelot->dev, "Invalid sgid %u, maximum:%u\n",
			sgid, capa.num_psfp_sgid);
		return -EINVAL;
	}
	if (pol_idx > capa.psfp_fmi_max || pol_idx < capa.psfp_fmi_min) {
		dev_err(ocelot->dev, "Invalid pol_idx %u, range:%d~%d\n",
			pol_idx, capa.psfp_fmi_min, capa.psfp_fmi_max);
		return -EINVAL;
	}

	ocelot_write(ocelot, ANA_TABLES_SFIDTIDX_SGID_VALID |
		     ANA_TABLES_SFIDTIDX_SGID(sgid) |
		     ((fmid != -1) ? ANA_TABLES_SFIDTIDX_POL_ENA : 0) |
		     ANA_TABLES_SFIDTIDX_POL_IDX(pol_idx) |
		     ANA_TABLES_SFIDTIDX_SFID_INDEX(sfid),
		     ANA_TABLES_SFIDTIDX);

	ocelot_write(ocelot,
		     ((igr_prio >= 0) ?
		      ANA_TABLES_SFIDACCESS_IGR_PRIO_MATCH_ENA : 0) |
		     ANA_TABLES_SFIDACCESS_IGR_PRIO(igr_prio) |
		     ANA_TABLES_SFIDACCESS_MAX_SDU_LEN(max_sdu_len) |
		     ANA_TABLES_SFIDACCESS_SFID_TBL_CMD(SFIDACCESS_CMD_WRITE),
		     ANA_TABLES_SFIDACCESS);

	return 0;
}

static int felix_qci_sfi_get(struct net_device *ndev, u32 index,
			     struct tsn_qci_psfp_sfi_conf *sfi)
{
	u32 val, reg, fmeter_id, max_sdu;
	struct ocelot *ocelot;
	struct dsa_port *dp;
	u32 sfid = index;
	int enable = 1;
	int i, port;

	dp = dsa_port_from_netdev(ndev);
	ocelot = dp->ds->priv;
	port = dp->index;

	if (sfid >= capa.num_psfp_sfid) {
		dev_err(ocelot->dev, "Invalid index %u, maximum:%u\n",
			sfid, capa.num_psfp_sfid);
		return -EINVAL;
	}

	ocelot_rmw(ocelot,
		   ANA_TABLES_SFIDTIDX_SFID_INDEX(sfid),
		   ANA_TABLES_SFIDTIDX_SFID_INDEX_M,
		   ANA_TABLES_SFIDTIDX);

	ocelot_write(ocelot,
		     ANA_TABLES_SFIDACCESS_SFID_TBL_CMD(SFIDACCESS_CMD_READ),
		     ANA_TABLES_SFIDACCESS);

	val = ocelot_read(ocelot, ANA_TABLES_SFIDTIDX);
	if (!(val & ANA_TABLES_SFIDTIDX_SGID_VALID)) {
		enable = 0;
		return enable;
	}

	sfi->stream_gate_instance_id = ANA_TABLES_SFIDTIDX_SGID_X(val);
	fmeter_id = ANA_TABLES_SFIDTIDX_POL_IDX_X(val);
	sfi->stream_filter.flow_meter_instance_id = fmeter_id;

	reg = ocelot_read(ocelot, ANA_TABLES_SFIDACCESS);
	max_sdu = ANA_TABLES_SFIDACCESS_MAX_SDU_LEN_X(reg);
	sfi->stream_filter.maximum_sdu_size  = max_sdu;

	if (reg & ANA_TABLES_SFIDACCESS_IGR_PRIO_MATCH_ENA)
		sfi->priority_spec = ANA_TABLES_SFIDACCESS_IGR_PRIO_X(reg);
	else
		dev_err(ocelot->dev, "priority not enable\n");

	for (i = 0; i < FELIX_NUM_TC; i++) {
		val = ocelot_read_ix(ocelot, ANA_PORT_SFID_CFG, port, i);
		if ((val & ANA_PORT_SFID_CFG_SFID_VALID) &&
		    sfid == ANA_PORT_SFID_CFG_SFID(val)) {
			sfi->stream_handle_spec = -1;
			return enable;
		}
	}

	sfi->stream_handle_spec = sfid;
	return enable;
}

static int felix_qci_sfi_counters_get(struct net_device *ndev, u32 index,
				      struct tsn_qci_psfp_sfi_counters *sfi_cnt)
{
	u32 match, not_pass, not_pass_sdu, red;
	struct ocelot *ocelot;
	struct dsa_port *dp;
	u32 sfid = index;

	dp = dsa_port_from_netdev(ndev);
	ocelot = dp->ds->priv;

	if (sfid >= capa.num_psfp_sfid) {
		dev_err(ocelot->dev, "Invalid index %u, maximum:%u\n",
			sfid, capa.num_psfp_sfid);
		return -EINVAL;
	}

	ocelot_rmw(ocelot,
		   SYS_STAT_CFG_STAT_VIEW(sfid),
		   SYS_STAT_CFG_STAT_VIEW_M,
		   SYS_STAT_CFG);

	match = ocelot_read_gix(ocelot, SYS_CNT, 0x200);
	not_pass = ocelot_read_gix(ocelot, SYS_CNT, 0x201);
	not_pass_sdu = ocelot_read_gix(ocelot, SYS_CNT, 0x202);
	red = ocelot_read_gix(ocelot, SYS_CNT, 0x203);

	sfi_cnt->matching_frames_count = match;
	sfi_cnt->not_passing_frames_count = not_pass;
	sfi_cnt->not_passing_sdu_count = not_pass_sdu;
	sfi_cnt->red_frames_count  =  red;

	sfi_cnt->passing_frames_count = match - not_pass;
	sfi_cnt->passing_sdu_count = match - not_pass - not_pass_sdu;

	return 0;
}

static int felix_qci_max_cap_get(struct net_device *ndev,
				 struct tsn_qci_psfp_stream_param *stream_para)
{
	/* MaxStreamFilterInstances */
	stream_para->max_sf_instance = capa.num_psfp_sfid;
	/* MaxStreamGateInstances */
	stream_para->max_sg_instance = capa.num_psfp_sgid;
	/* MaxFlowMeterInstances */
	stream_para->max_fm_instance = capa.psfp_fmi_max -
				       capa.psfp_fmi_min + 1;
	/* SupportedListMax */
	stream_para->supported_list_max = capa.num_sgi_gcl;

	return 0;
}

static int felix_sgi_set_glist(struct ocelot *ocelot,
			       struct tsn_qci_psfp_gcl *gcl, uint32_t num)
{
	u32 time_sum = 0;
	int i;

	if (num > capa.num_sgi_gcl)
		return -EINVAL;

	for (i = 0; i < num; i++) {
		u32 val = ANA_SG_GCL_GS_CONFIG_IPS((gcl->ipv < 0) ?
						   0 : gcl->ipv + 8);
		val |= (gcl->gate_state ? ANA_SG_GCL_GS_CONFIG_GATE_STATE : 0);
		ocelot_write_rix(ocelot, val, ANA_SG_GCL_GS_CONFIG, i);

		time_sum += gcl->time_interval;
		ocelot_write_rix(ocelot, time_sum, ANA_SG_GCL_TI_CONFIG, i);

		gcl++;
	}

	return 0;
}

static u32 felix_sgi_read_status(struct ocelot *ocelot)
{
	u32 val;

	val = ocelot_read(ocelot, ANA_SG_ACCESS_CTRL);

	return val;
}

static int felix_qci_sgi_set(struct net_device *ndev, u32 index,
			     struct tsn_qci_psfp_sgi_conf *sgi_conf)
{
	struct tsn_qci_sg_control *admin_list = &sgi_conf->admin;
	u32 list_length = sgi_conf->admin.control_list_length;
	u32 cycle_time = sgi_conf->admin.cycle_time;
	u32 cycle_time_ex = sgi_conf->admin.cycle_time_extension;
	struct timespec64 ts_base;
	struct ocelot *ocelot;
	struct dsa_port *dp;
	u32 sgid = index;
	int ret;
	u32 val;

	dp = dsa_port_from_netdev(ndev);
	ocelot = dp->ds->priv;

	if (sgid >= capa.num_psfp_sgid) {
		dev_err(ocelot->dev, "Invalid sgid %u, maximum:%u\n",
			sgid, capa.num_psfp_sgid);
		return -EINVAL;
	}
	if ((cycle_time < capa.sgi_ct_min ||
	     cycle_time > capa.sgi_ct_max) &&
	     sgi_conf->gate_enabled) {
		dev_err(ocelot->dev, "Invalid cycle_time %u ns\n",
			cycle_time);
		return -EINVAL;
	}
	if (cycle_time_ex > capa.sgi_cte_max) {
		dev_err(ocelot->dev,
			"Invalid cycle_time_extension %u\n",
			cycle_time_ex);
		return -EINVAL;
	}
	if (list_length > capa.num_sgi_gcl) {
		dev_err(ocelot->dev,
			"Invalid sgi_gcl len %u, maximum:%u\n",
			list_length, capa.num_sgi_gcl);
		return -EINVAL;
	}

	/*configure SGID*/
	ocelot_rmw(ocelot,
		   ANA_SG_ACCESS_CTRL_SGID(sgid),
		   ANA_SG_ACCESS_CTRL_SGID_M,
		   ANA_SG_ACCESS_CTRL);

	/*Disable SG*/
	if (!sgi_conf->gate_enabled) {
		ocelot_rmw(ocelot,
			   ANA_SG_CONFIG_REG_3_INIT_GATE_STATE,
			   ANA_SG_CONFIG_REG_3_INIT_GATE_STATE |
			   ANA_SG_CONFIG_REG_3_GATE_ENABLE,
			   ANA_SG_CONFIG_REG_3);
		return 0;
	}

	felix_get_basetime(ocelot, sgi_conf->admin.base_time,
			   sgi_conf->admin.cycle_time, &ts_base);

	ocelot_write(ocelot, ts_base.tv_nsec, ANA_SG_CONFIG_REG_1);
	ocelot_write(ocelot, lower_32_bits(ts_base.tv_sec),
		     ANA_SG_CONFIG_REG_2);

	val = upper_32_bits(ts_base.tv_sec);
	ocelot_write(ocelot,
		     (sgi_conf->admin.init_ipv < 0 ?
		      0 : ANA_SG_CONFIG_REG_3_IPV_VALID) |
		     ANA_SG_CONFIG_REG_3_INIT_IPV(sgi_conf->admin.init_ipv) |
		     ANA_SG_CONFIG_REG_3_GATE_ENABLE |
		     ANA_SG_CONFIG_REG_3_LIST_LENGTH(list_length) |
		     (sgi_conf->admin.gate_states > 0 ?
		      ANA_SG_CONFIG_REG_3_INIT_GATE_STATE : 0) |
		     ANA_SG_CONFIG_REG_3_BASE_TIME_SEC_MSB(val),
		     ANA_SG_CONFIG_REG_3);

	ocelot_write(ocelot, cycle_time, ANA_SG_CONFIG_REG_4);
	ocelot_write(ocelot, cycle_time_ex, ANA_SG_CONFIG_REG_5);

	ret = felix_sgi_set_glist(ocelot, admin_list->gcl, list_length);
	if (ret < 0)
		return ret;

	/* Start configuration change */
	ocelot_rmw(ocelot,
		   ANA_SG_ACCESS_CTRL_CONFIG_CHANGE,
		   ANA_SG_ACCESS_CTRL_CONFIG_CHANGE,
		   ANA_SG_ACCESS_CTRL);

	ret = readx_poll_timeout(felix_sgi_read_status, ocelot, val,
				 (!(ANA_SG_ACCESS_CTRL_CONFIG_CHANGE & val)),
				 10, 100000);

	return ret;
}

static int felix_sgi_get_glist(struct ocelot *ocelot,
			       struct tsn_qci_psfp_gcl *gcl,
			       uint32_t num)
{
	int i;
	u16 val;
	u32 time = 0;
	u32 reg;

	if (num > capa.num_sgi_gcl)
		return -EINVAL;

	for (i = 0; i < num; i++) {
		val = ocelot_read_rix(ocelot, ANA_SG_GCL_GS_CONFIG, i);
		gcl->gate_state = (val & ANA_SG_GCL_GS_CONFIG_GATE_STATE);

		if (val & ANA_SG_GCL_GS_CONFIG_IPV_VALID)
			gcl->ipv = ANA_SG_GCL_GS_CONFIG_IPV(val);
		else
			gcl->ipv = -1;

		reg = ocelot_read_rix(ocelot, ANA_SG_GCL_TI_CONFIG, i);
		gcl->time_interval = (reg - time);
		time = reg;

		gcl++;
	}

	return 0;
}

static int felix_qci_sgi_get(struct net_device *ndev, u32 index,
			     struct tsn_qci_psfp_sgi_conf *sgi_conf)
{
	struct tsn_qci_sg_control *admin  = &sgi_conf->admin;
	struct tsn_qci_psfp_gcl *glist;
	u32 val, reg, list_num;
	struct ocelot *ocelot;
	struct dsa_port *dp;
	int ret;

	dp = dsa_port_from_netdev(ndev);
	ocelot = dp->ds->priv;

	if (index >= capa.num_psfp_sgid) {
		dev_err(ocelot->dev, "Invalid sgid %u, maximum:%u\n",
			index, capa.num_psfp_sgid);
		return -EINVAL;
	}

	ocelot_rmw(ocelot,
		   ANA_SG_ACCESS_CTRL_SGID(index),
		   ANA_SG_ACCESS_CTRL_SGID_M,
		   ANA_SG_ACCESS_CTRL);

	admin->cycle_time = ocelot_read(ocelot, ANA_SG_CONFIG_REG_4);
	admin->cycle_time_extension =
		ocelot_read(ocelot, ANA_SG_CONFIG_REG_5);

	val = ocelot_read(ocelot, ANA_SG_CONFIG_REG_2);
	admin->base_time = val;

	reg = ocelot_read(ocelot, ANA_SG_CONFIG_REG_1);
	val = ocelot_read(ocelot, ANA_SG_CONFIG_REG_3);

	admin->base_time +=
		ANA_SG_CONFIG_REG_3_BASE_TIME_SEC_MSB(val) << 32;

	admin->base_time = admin->base_time * 1000000000 + reg;

	if (val & ANA_SG_CONFIG_REG_3_IPV_VALID)
		admin->init_ipv = ANA_SG_CONFIG_REG_3_INIT_IPV_X(val);
	else
		admin->init_ipv = -1;

	if (val & ANA_SG_CONFIG_REG_3_GATE_ENABLE)
		sgi_conf->gate_enabled = 1;

	admin->control_list_length = ANA_SG_CONFIG_REG_3_LIST_LENGTH_X(val);

	list_num = admin->control_list_length;

	glist = kmalloc_array(list_num, sizeof(struct tsn_qci_psfp_gcl),
			      GFP_KERNEL);
	if (!glist)
		return -ENOMEM;

	admin->gcl = glist;

	ret = felix_sgi_get_glist(ocelot, glist, list_num);
	return ret;
}

static int felix_qci_sgi_status_get(struct net_device *ndev, u16 index,
				    struct tsn_psfp_sgi_status *sgi_status)
{
	struct ocelot *ocelot;
	struct dsa_port *dp;
	u32 val, reg;

	dp = dsa_port_from_netdev(ndev);
	ocelot = dp->ds->priv;

	if (index >= capa.num_psfp_sgid) {
		dev_err(ocelot->dev, "Invalid sgid %u, maximum:%u\n",
			index, capa.num_psfp_sgid);
		return -EINVAL;
	}

	ocelot_rmw(ocelot,
		   ANA_SG_ACCESS_CTRL_SGID(index),
		   ANA_SG_ACCESS_CTRL_SGID_M,
		   ANA_SG_ACCESS_CTRL);

	val = ocelot_read(ocelot, ANA_SG_STATUS_REG_2);
	sgi_status->config_change_time = val;

	reg = ocelot_read(ocelot, ANA_SG_STATUS_REG_1);
	val = ocelot_read(ocelot, ANA_SG_STATUS_REG_3);
	sgi_status->config_change_time +=
		ANA_SG_STATUS_REG_3_CFG_CHG_TIME_SEC_MSB(val) << 32;
	sgi_status->config_change_time =
		sgi_status->config_change_time * 1000000000 + reg;

	if (val & ANA_SG_STATUS_REG_3_CONFIG_PENDING)
		sgi_status->config_pending  = 1;
	else
		sgi_status->config_pending = 0;

	if (val & ANA_SG_STATUS_REG_3_GATE_STATE)
		sgi_status->oper.gate_states = 1;
	else
		sgi_status->oper.gate_states = 0;
	/*bit 3 encoding 0:IPV [0:2]is invalid . 1:IPV[0:2] is valid*/
	if (val & ANA_SG_STATUS_REG_3_IPV_VALID)
		sgi_status->oper.init_ipv = ANA_SG_STATUS_REG_3_IPV_X(val);
	else
		sgi_status->oper.init_ipv = -1;

	return 0;
}

static int felix_qci_fmi_set(struct net_device *ndev, u32 index,
			     bool enable, struct tsn_qci_psfp_fmi *fmi)
{
	u32 cir = 0, cbs = 0, pir = 0, pbs = 0;
	bool cir_discard = 0, pir_discard = 0;
	u32 pbs_max = 0, cbs_max = 0;
	struct ocelot *ocelot;
	struct dsa_port *dp;
	u32 cir_ena = 0;

	dp = dsa_port_from_netdev(ndev);
	ocelot = dp->ds->priv;

	if (index > capa.qos_pol_max) {
		dev_err(ocelot->dev, "Invalid pol_idx %u, maximum: %u\n",
			index, capa.qos_pol_max);
		return -EINVAL;
	}

	if (fmi->mark_red_enable && fmi->mark_red) {
		fmi->eir = 0;
		fmi->ebs = 0;
		fmi->cir = 0;
		fmi->cbs = 0;
	}

	pir = fmi->eir;
	pbs = fmi->ebs;

	if (!fmi->drop_on_yellow)
		cir_ena = 1;

	if (cir_ena) {
		cir = fmi->cir;
		cbs = fmi->cbs;
		if (cir == 0 && cbs == 0) {
			cir_discard = 1;
		} else {
			cir = DIV_ROUND_UP(cir, 100);
			cir *= 3;  /* Rate unit is 33 1/3 kbps */
			cbs = DIV_ROUND_UP(cbs, 4096);
			cbs = (cbs ? cbs : 1);
			cbs_max = capa.pol_cbs_max;
			if (fmi->cf)
				pir += fmi->cir;
		}
	}

	if (pir == 0 && pbs == 0) {
		pir_discard = 1;
	} else {
		pir = DIV_ROUND_UP(pir, 100);
		pir *= 3;  /* Rate unit is 33 1/3 kbps */
		pbs = DIV_ROUND_UP(pbs, 4096);
		pbs = (pbs ? pbs : 1);
		pbs_max = capa.pol_pbs_max;
	}
	pir = min_t(u32, GENMASK(15, 0), pir);
	cir = min_t(u32, GENMASK(15, 0), cir);
	pbs = min(pbs_max, pbs);
	cbs = min(cbs_max, cbs);

	ocelot_write_gix(ocelot, (ANA_POL_MODE_CFG_IPG_SIZE(20) |
			 ANA_POL_MODE_CFG_FRM_MODE(1) |
			 (fmi->cf ? ANA_POL_MODE_CFG_DLB_COUPLED : 0) |
			 (cir_ena ? ANA_POL_MODE_CFG_CIR_ENA : 0) |
			 ANA_POL_MODE_CFG_OVERSHOOT_ENA),
			 ANA_POL_MODE_CFG, index);

	ocelot_write_gix(ocelot, ANA_POL_PIR_CFG_PIR_RATE(pir) |
			 ANA_POL_PIR_CFG_PIR_BURST(pbs),
			 ANA_POL_PIR_CFG, index);

	ocelot_write_gix(ocelot,
			 (pir_discard ? GENMASK(22, 0) : 0),
			 ANA_POL_PIR_STATE, index);

	ocelot_write_gix(ocelot, ANA_POL_CIR_CFG_CIR_RATE(cir) |
			 ANA_POL_CIR_CFG_CIR_BURST(cbs),
			 ANA_POL_CIR_CFG, index);

	ocelot_write_gix(ocelot,
			 (cir_discard ? GENMASK(22, 0) : 0),
			 ANA_POL_CIR_STATE, index);

	return 0;
}

static int felix_qci_fmi_get(struct net_device *ndev, u32 index,
			     struct tsn_qci_psfp_fmi *fmi,
			     struct tsn_qci_psfp_fmi_counters *counters)
{
	struct ocelot *ocelot;
	struct dsa_port *dp;
	u32 val, reg;

	dp = dsa_port_from_netdev(ndev);
	ocelot = dp->ds->priv;

	if (index > capa.qos_pol_max) {
		dev_err(ocelot->dev, "Invalid pol_idx %u, maximum: %u\n",
			index, capa.qos_pol_max);
		return -EINVAL;
	}

	val = ocelot_read_gix(ocelot, ANA_POL_PIR_CFG, index);
	reg = ocelot_read_gix(ocelot, ANA_POL_CIR_CFG, index);

	fmi->eir = ANA_POL_PIR_CFG_PIR_RATE_X(val);
	fmi->eir = fmi->eir * 100 / 3;
	fmi->ebs = ANA_POL_PIR_CFG_PIR_BURST(val);
	fmi->ebs *= 4096;
	fmi->cir = ANA_POL_CIR_CFG_CIR_RATE_X(reg);
	fmi->cir = fmi->cir * 100 / 3;
	fmi->cbs = ANA_POL_CIR_CFG_CIR_BURST(reg);
	fmi->cbs *= 4096;
	if (!(fmi->eir | fmi->ebs | fmi->cir | fmi->cbs))
		fmi->mark_red = 1;
	else
		fmi->mark_red = 0;

	val = ocelot_read_gix(ocelot, ANA_POL_MODE_CFG, index);
	if (val & ANA_POL_MODE_CFG_DLB_COUPLED)
		fmi->cf = 1;
	else
		fmi->cf = 0;
	if (val & ANA_POL_MODE_CFG_CIR_ENA)
		fmi->drop_on_yellow = 0;
	else
		fmi->drop_on_yellow = 1;

	return 0;
}

static int felix_qos_shaper_conf_set(struct ocelot *ocelot, int idx,
				     u8 percent, int speed)
{
	u32 cbs = 0;
	u32 cir = 0;

	if (percent > 100) {
		dev_err(ocelot->dev, "percentage %d larger than 100\n",
			percent);
		return -EINVAL;
	}
	if (idx >= capa.num_hsch) {
		dev_err(ocelot->dev,
			"CIR_CFG: id %d is exceed num of HSCH instance\n",
			idx);
		return -EINVAL;
	}

	switch (speed) {
	case SPEED_10:
		cir = 10000;
		break;
	case SPEED_100:
		cir = 100000;
		break;
	case SPEED_1000:
		cir = 1000000;
		break;
	case SPEED_2500:
		cir = 2500000;
		break;
	}

	cir = cir * percent / 100;
	cir = DIV_ROUND_UP(cir, 100);  /* Rate unit is 100 kbps */
	cir = (cir ? cir : 1);              /* Avoid using zero rate */
	cbs = DIV_ROUND_UP(cbs, 4096); /* Burst unit is 4kB */
	cbs = (cbs ? cbs : 1);		/* Avoid using zero burst size */
	cir = min_t(u32, GENMASK(15, 0), cir);
	cbs = min_t(u32, GENMASK(6, 0), cbs);

	if (!percent) {
		cir = 0;
		cbs = 0;
	}

	ocelot_write_gix(ocelot,
			 QSYS_CIR_CFG_CIR_RATE(cir) |
			 QSYS_CIR_CFG_CIR_BURST(cbs),
			 QSYS_CIR_CFG,
			 idx);

	return 0;
}

static int felix_cbs_set(struct net_device *ndev, u8 tc, u8 bw)
{
	struct phylink_link_state state;
	struct phylink_pcs *pcs;
	struct ocelot *ocelot;
	struct felix *felix;
	struct dsa_port *dp;
	int port, speed;

	dp = dsa_port_from_netdev(ndev);
	ocelot = dp->ds->priv;
	port = dp->index;

	if (tc > capa.qos_cos_max) {
		dev_err(ocelot->dev, "Invalid tc: %u\n", tc);
		return -EINVAL;
	}

	felix = ocelot_to_felix(ocelot);
	pcs = &felix->pcs[port]->pcs;
	pcs->ops->pcs_get_state(pcs, &state);

	speed = state.speed;

	felix_qos_shaper_conf_set(ocelot, port * 8 + tc, bw, speed);

	ocelot_rmw_gix(ocelot,
		       QSYS_SE_CFG_SE_AVB_ENA,
		       QSYS_SE_CFG_SE_AVB_ENA,
		       QSYS_SE_CFG,
		       port * 8 + tc);

	hsch_bw[port * 8 + tc] = bw;

	return 0;
}

void felix_cbs_reset(struct ocelot *ocelot, int port, int speed)
{
	int i, idx;

	for (i = 0; i < FELIX_NUM_TC; i++) {
		idx = port * 8 + i;
		if (hsch_bw[idx] > 0)
			felix_qos_shaper_conf_set(ocelot, idx, hsch_bw[idx],
						  speed);
	}
}

static int felix_cbs_get(struct net_device *ndev, u8 tc)
{
	struct ocelot *ocelot;
	struct dsa_port *dp;
	int port;

	dp = dsa_port_from_netdev(ndev);
	ocelot = dp->ds->priv;
	port = dp->index;

	if (tc > capa.qos_cos_max) {
		dev_err(ocelot->dev, "Invalid tc: %u\n", tc);
		return -EINVAL;
	}

	return hsch_bw[port * 8 + tc];
}

static int felix_cut_thru_set(struct net_device *ndev, u8 cut_thru)
{
	struct ocelot *ocelot;
	struct dsa_port *dp;
	int port;

	dp = dsa_port_from_netdev(ndev);
	ocelot = dp->ds->priv;
	port = dp->index;

	ocelot_write_rix(ocelot, cut_thru, ANA_CUT_THRU_CFG, port);

	return 0;
}

static int felix_rtag_parse_enable(struct ocelot *ocelot, u8 port)
{
	ocelot_rmw_rix(ocelot,
		       ANA_PORT_MODE_REDTAG_PARSE_CFG,
		       ANA_PORT_MODE_REDTAG_PARSE_CFG,
		       ANA_PORT_MODE,
		       port);

	ocelot_write(ocelot, ETH_P_8021CB, SYS_SR_ETYPE_CFG);

	/* No harm, no foul: we are telling the switch to adjust maximum frame
	 * length for double-tagged VLANs lying that the EtherType for S-Tags
	 * is the one for 802.1CB. This is not an issue because with 802.1CB
	 * traffic, the switch will not parse more than 2 tags anyway, so
	 * either it doesn't support 802.1CB or the second VLAN tag.
	 */
	ocelot_port_writel(ocelot->ports[port],
			   DEV_MAC_TAGS_CFG_TAG_ID(ETH_P_8021CB) |
			   DEV_MAC_TAGS_CFG_VLAN_AWR_ENA |
			   DEV_MAC_TAGS_CFG_VLAN_DBL_AWR_ENA |
			   DEV_MAC_TAGS_CFG_VLAN_LEN_AWR_ENA,
			   DEV_MAC_TAGS_CFG);

	return 0;
}

static int felix_seq_gen_set(struct net_device *ndev, u32 index,
			     struct tsn_seq_gen_conf *sg_conf)
{
	u8 iport_mask = sg_conf->iport_mask;
	u8 split_mask = sg_conf->split_mask;
	u8 seq_len = sg_conf->seq_len;
	u32 seq_num = sg_conf->seq_num;
	struct stream_filter *tmp;
	struct ocelot *ocelot;
	struct dsa_port *dp;

	dp = dsa_port_from_netdev(ndev);
	ocelot = dp->ds->priv;

	if (index >= capa.num_frer_ssid) {
		dev_err(ocelot->dev, "Invalid SSID %u, maximum:%u\n",
			index, capa.num_frer_ssid - 1);
		return -EINVAL;
	}
	if (seq_len < capa.frer_seq_len_min ||
	    seq_len > capa.frer_seq_len_max) {
		dev_err(ocelot->dev,
			"Invalid seq_space_bits num %u,range:%d~%d\n",
			seq_len,
			capa.frer_seq_len_min,
			capa.frer_seq_len_max);
		return -EINVAL;
	}

	list_for_each_entry(tmp, &streamtable, list)
		if (tmp->handle == index)
			felix_streamid_multi_forward_set(ocelot, tmp->mac,
							 tmp->vid, split_mask);

	ocelot_write(ocelot,
		     ANA_TABLES_SEQ_MASK_SPLIT_MASK(split_mask) |
		     ANA_TABLES_SEQ_MASK_INPUT_PORT_MASK(iport_mask),
		     ANA_TABLES_SEQ_MASK);

	ocelot_write(ocelot,
		     ANA_TABLES_STREAMTIDX_S_INDEX(index) |
		     ANA_TABLES_STREAMTIDX_STREAM_SPLIT |
		     ANA_TABLES_STREAMTIDX_SEQ_SPACE_LOG2(seq_len),
		     ANA_TABLES_STREAMTIDX);

	ocelot_write(ocelot,
		     ANA_TABLES_STREAMACCESS_GEN_REC_SEQ_NUM(seq_num) |
		     ANA_TABLES_STREAMACCESS_SEQ_GEN_REC_ENA |
		     ANA_TABLES_STREAMACCESS_STREAM_TBL_CMD(SFIDACCESS_CMD_WRITE),
		     ANA_TABLES_STREAMACCESS);

	return 0;
}

static int felix_seq_rec_set(struct net_device *ndev, u32 index,
			     struct tsn_seq_rec_conf *sr_conf)
{
	u8 seq_len = sr_conf->seq_len;
	u8 hislen = sr_conf->his_len;
	struct ocelot *ocelot;
	struct dsa_port *dp;
	int i;

	dp = dsa_port_from_netdev(ndev);
	ocelot = dp->ds->priv;

	if (index >= capa.num_frer_ssid) {
		dev_err(ocelot->dev, "Invalid SSID %u, maximum:%u\n",
			index, capa.num_frer_ssid - 1);
		return -EINVAL;
	}
	if (seq_len < capa.frer_seq_len_min ||
	    seq_len > capa.frer_seq_len_max) {
		dev_err(ocelot->dev,
			"Invalid seq_space_bits num %u,range:%d~%d\n",
			seq_len,
			capa.frer_seq_len_min,
			capa.frer_seq_len_max);
		return -EINVAL;
	}
	if (hislen < capa.frer_his_len_min ||
	    hislen > capa.frer_his_len_max) {
		dev_err(ocelot->dev,
			"Invalid history_bits num %u,range:%d~%d\n",
			hislen,
			capa.frer_his_len_min,
			capa.frer_his_len_max);
		return -EINVAL;
	}

	for (i = 0; i < ocelot->num_phys_ports; i++)
		felix_rtag_parse_enable(ocelot, i);

	ocelot_write(ocelot,
		     ANA_TABLES_STREAMTIDX_S_INDEX(index) |
		     ANA_TABLES_STREAMTIDX_FORCE_SF_BEHAVIOUR |
		     ANA_TABLES_STREAMTIDX_SEQ_HISTORY_LEN(hislen) |
		     ANA_TABLES_STREAMTIDX_RESET_ON_ROGUE |
		     (sr_conf->rtag_pop_en ?
		      ANA_TABLES_STREAMTIDX_REDTAG_POP : 0) |
		     ANA_TABLES_STREAMTIDX_SEQ_SPACE_LOG2(seq_len),
		     ANA_TABLES_STREAMTIDX);

	ocelot_write(ocelot,
		     ANA_TABLES_STREAMACCESS_SEQ_GEN_REC_ENA |
		     ANA_TABLES_STREAMACCESS_GEN_REC_TYPE |
		     ANA_TABLES_STREAMACCESS_STREAM_TBL_CMD(SFIDACCESS_CMD_WRITE),
		     ANA_TABLES_STREAMACCESS);

	return 0;
}

static int felix_cb_get(struct net_device *ndev, u32 index,
			struct tsn_cb_status *c)
{
	struct ocelot *ocelot;
	struct dsa_port *dp;
	u32 val;

	dp = dsa_port_from_netdev(ndev);
	ocelot = dp->ds->priv;

	if (index >= capa.num_frer_ssid) {
		dev_err(ocelot->dev, "Invalid SSID %u, maximum:%u\n",
			index, capa.num_frer_ssid - 1);
		return -EINVAL;
	}

	ocelot_write(ocelot,
		     ANA_TABLES_STREAMTIDX_S_INDEX(index),
		     ANA_TABLES_STREAMTIDX);

	ocelot_write(ocelot,
		     ANA_TABLES_STREAMACCESS_STREAM_TBL_CMD(SFIDACCESS_CMD_READ),
		     ANA_TABLES_STREAMACCESS);

	val = ocelot_read(ocelot, ANA_TABLES_STREAMACCESS);
	c->gen_rec = (ANA_TABLES_STREAMACCESS_GEN_REC_TYPE & val) >> 2;
	c->seq_num = ANA_TABLES_STREAMACCESS_GEN_REC_SEQ_NUM_X(val);

	val = ocelot_read(ocelot, ANA_TABLES_STREAMTIDX);
	c->err = ANA_TABLES_STREAMTIDX_SEQ_GEN_ERR_STATUS_X(val);
	c->his_len = ANA_TABLES_STREAMTIDX_SEQ_HISTORY_LEN_X(val);
	c->seq_len = ANA_TABLES_STREAMTIDX_SEQ_SPACE_LOG2(val);

	val = ocelot_read(ocelot, ANA_TABLES_SEQ_MASK);
	c->split_mask = ANA_TABLES_SEQ_MASK_SPLIT_MASK_X(val);
	c->iport_mask = ANA_TABLES_SEQ_MASK_INPUT_PORT_MASK(val);

	c->seq_his = ocelot_read(ocelot, ANA_TABLES_SEQ_HISTORY);

	return 0;
}

static int felix_dscp_set(struct net_device *ndev, bool enable, const u8 dscp_ix,
			  struct tsn_qos_switch_dscp_conf *c)
{
	struct ocelot *ocelot;
	struct dsa_port *dp;
	u32 ri = dscp_ix;
	u32 val;
	int port;

	dp = dsa_port_from_netdev(ndev);
	ocelot = dp->ds->priv;
	port = dp->index;

	c->dscp = 0;
	c->trust = 1;
	c->remark = 0;

	if (dscp_ix > capa.qos_dscp_max) {
		dev_err(ocelot->dev, "Invalid dscp_ix %u\n", dscp_ix);
		return -EINVAL;
	}
	if (c->cos > capa.qos_cos_max) {
		dev_err(ocelot->dev, "Invalid cos %d\n", c->cos);
		return -EINVAL;
	}
	if (c->dpl > capa.qos_dp_max) {
		dev_err(ocelot->dev, "Invalid dpl %d\n", c->dpl);
		return -EINVAL;
	}

	ocelot_rmw_gix(ocelot,
		       (enable ? ANA_PORT_QOS_CFG_QOS_DSCP_ENA : 0) |
		       (c->dscp ? ANA_PORT_QOS_CFG_DSCP_TRANSLATE_ENA : 0),
		       ANA_PORT_QOS_CFG_QOS_DSCP_ENA |
		       ANA_PORT_QOS_CFG_DSCP_TRANSLATE_ENA,
		       ANA_PORT_QOS_CFG,
		       port);

	val = (c->dpl ? ANA_DSCP_CFG_DP_DSCP_VAL : 0) |
	       ANA_DSCP_CFG_QOS_DSCP_VAL(c->cos) |
	       ANA_DSCP_CFG_DSCP_TRANSLATE_VAL(c->dscp) |
	       (c->trust ? ANA_DSCP_CFG_DSCP_TRUST_ENA : 0) |
	       (c->remark ? ANA_DSCP_CFG_DSCP_REWR_ENA : 0);

	ocelot_write_rix(ocelot, val, ANA_DSCP_CFG, ri);

	return 0;
}

void felix_preempt_irq_clean(struct ocelot *ocelot)
{
	struct ocelot_port *ocelot_port;
	int port;
	u32 val;

	val = DEV_MM_STATISTICS_MM_STATUS_PRMPT_ACTIVE_STICKY;
	for (port = 0; port < ocelot->num_phys_ports; port++) {
		ocelot_port = ocelot->ports[port];
		ocelot_port_rmwl(ocelot_port, val, val, DEV_MM_STATUS);
	}
}

const struct tsn_ops switch_tsn_ops = {
	.get_capability                 = felix_tsn_get_cap,
	.qbv_set			= felix_qbv_set,
	.qbv_get			= felix_qbv_get,
	.qbv_get_status			= felix_qbv_get_status,
	.qbu_set			= felix_qbu_set,
	.qbu_get                        = felix_qbu_get,
	.cb_streamid_set		= felix_cb_streamid_set,
	.cb_streamid_get		= felix_cb_streamid_get,
	.cb_streamid_counters_get	= felix_cb_streamid_counters_get,
	.qci_sfi_set			= felix_qci_sfi_set,
	.qci_sfi_get			= felix_qci_sfi_get,
	.qci_sfi_counters_get		= felix_qci_sfi_counters_get,
	.qci_get_maxcap			= felix_qci_max_cap_get,
	.qci_sgi_set			= felix_qci_sgi_set,
	.qci_sgi_get			= felix_qci_sgi_get,
	.qci_sgi_status_get		= felix_qci_sgi_status_get,
	.qci_fmi_set			= felix_qci_fmi_set,
	.qci_fmi_get			= felix_qci_fmi_get,
	.cbs_set			= felix_cbs_set,
	.cbs_get			= felix_cbs_get,
	.ct_set				= felix_cut_thru_set,
	.cbgen_set			= felix_seq_gen_set,
	.cbrec_set			= felix_seq_rec_set,
	.cb_get				= felix_cb_get,
	.dscp_set			= felix_dscp_set,
};

int felix_tsn_enable(struct dsa_switch *ds)
{
	struct net_device *dev;
	struct dsa_port *dp;
	int port;

	for (port = 0; port < ds->num_ports; port++) {
		dp = dsa_to_port(ds, port);
		if (dp->type == DSA_PORT_TYPE_USER) {
			dev = dp->slave;
			tsn_port_register(dev, (struct tsn_ops *)&switch_tsn_ops,
					  GROUP_OFFSET_SWITCH);
		}
	}

	INIT_LIST_HEAD(&streamtable);

	return 0;
}
