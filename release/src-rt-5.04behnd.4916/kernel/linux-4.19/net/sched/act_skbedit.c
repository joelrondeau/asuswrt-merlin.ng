/*
 * Copyright (c) 2008, Intel Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, see <http://www.gnu.org/licenses/>.
 *
 * Author: Alexander Duyck <alexander.h.duyck@intel.com>
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/skbuff.h>
#include <linux/rtnetlink.h>
#include <net/netlink.h>
#include <net/pkt_sched.h>
#include <net/ip.h>
#include <net/ipv6.h>
#include <net/dsfield.h>

#include <linux/tc_act/tc_skbedit.h>
#include <net/tc_act/tc_skbedit.h>

#ifdef CONFIG_BCM_KF_ENHANCED_TC
#include <linux/netfilter/xt_FSMARK_sh.h>
#endif /* CONFIG_BCM_KF_ENHANCED_TC */

static unsigned int skbedit_net_id;
static struct tc_action_ops act_skbedit_ops;

static int tcf_skbedit_act(struct sk_buff *skb, const struct tc_action *a,
			   struct tcf_result *res)
{
	struct tcf_skbedit *d = to_skbedit(a);
	struct tcf_skbedit_params *params;
	int action;

	tcf_lastuse_update(&d->tcf_tm);
	bstats_cpu_update(this_cpu_ptr(d->common.cpu_bstats), skb);

	params = rcu_dereference_bh(d->params);
	action = READ_ONCE(d->tcf_action);

	if (params->flags & SKBEDIT_F_PRIORITY)
		skb->priority = params->priority;
	if (params->flags & SKBEDIT_F_INHERITDSFIELD) {
		int wlen = skb_network_offset(skb);

		switch (skb_protocol(skb, true)) {
		case htons(ETH_P_IP):
			wlen += sizeof(struct iphdr);
			if (!pskb_may_pull(skb, wlen))
				goto err;
			skb->priority = ipv4_get_dsfield(ip_hdr(skb)) >> 2;
			break;

		case htons(ETH_P_IPV6):
			wlen += sizeof(struct ipv6hdr);
			if (!pskb_may_pull(skb, wlen))
				goto err;
			skb->priority = ipv6_get_dsfield(ipv6_hdr(skb)) >> 2;
			break;
		}
	}
	if (params->flags & SKBEDIT_F_QUEUE_MAPPING &&
	    skb->dev->real_num_tx_queues > params->queue_mapping)
		skb_set_queue_mapping(skb, params->queue_mapping);
	if (params->flags & SKBEDIT_F_MARK) {
		skb->mark &= ~params->mask;
		skb->mark |= params->mark & params->mask;
	}
#ifdef CONFIG_BCM_KF_ENHANCED_TC
	if (params->flags & SKBEDIT_F_FSMARKID)
	    fsmark_update(skb, params->fsmark_id);
#endif /* CONFIG_BCM_KF_ENHANCED_TC */
	if (params->flags & SKBEDIT_F_PTYPE)
		skb->pkt_type = params->ptype;
	return action;

err:
	qstats_drop_inc(this_cpu_ptr(d->common.cpu_qstats));
	return TC_ACT_SHOT;
}

static const struct nla_policy skbedit_policy[TCA_SKBEDIT_MAX + 1] = {
	[TCA_SKBEDIT_PARMS]		= { .len = sizeof(struct tc_skbedit) },
	[TCA_SKBEDIT_PRIORITY]		= { .len = sizeof(u32) },
	[TCA_SKBEDIT_QUEUE_MAPPING]	= { .len = sizeof(u16) },
	[TCA_SKBEDIT_MARK]		= { .len = sizeof(u32) },
	[TCA_SKBEDIT_PTYPE]		= { .len = sizeof(u16) },
	[TCA_SKBEDIT_MASK]		= { .len = sizeof(u32) },
#ifdef CONFIG_BCM_KF_ENHANCED_TC
	[TCA_SKBEDIT_FSMARKID]	= { .len = sizeof(u32) },
#endif /* CONFIG_BCM_KF_ENHANCED_TC */
	[TCA_SKBEDIT_FLAGS]		= { .len = sizeof(u64) },
};

static int tcf_skbedit_init(struct net *net, struct nlattr *nla,
			    struct nlattr *est, struct tc_action **a,
			    int ovr, int bind, bool rtnl_held,
			    struct netlink_ext_ack *extack)
{
	struct tc_action_net *tn = net_generic(net, skbedit_net_id);
	struct tcf_skbedit_params *params_old, *params_new;
	struct nlattr *tb[TCA_SKBEDIT_MAX + 1];
	struct tc_skbedit *parm;
	struct tcf_skbedit *d;
	u32 flags = 0, *priority = NULL, *mark = NULL, *mask = NULL;
#ifdef CONFIG_BCM_KF_ENHANCED_TC
	u32 *fsmark_id = NULL;
#endif /* CONFIG_BCM_KF_ENHANCED_TC */
	u16 *queue_mapping = NULL, *ptype = NULL;
	bool exists = false;
	int ret = 0, err;
	u32 index;

	if (nla == NULL)
		return -EINVAL;

	err = nla_parse_nested(tb, TCA_SKBEDIT_MAX, nla, skbedit_policy, NULL);
	if (err < 0)
		return err;

	if (tb[TCA_SKBEDIT_PARMS] == NULL)
		return -EINVAL;

	if (tb[TCA_SKBEDIT_PRIORITY] != NULL) {
		flags |= SKBEDIT_F_PRIORITY;
		priority = nla_data(tb[TCA_SKBEDIT_PRIORITY]);
	}

	if (tb[TCA_SKBEDIT_QUEUE_MAPPING] != NULL) {
		flags |= SKBEDIT_F_QUEUE_MAPPING;
		queue_mapping = nla_data(tb[TCA_SKBEDIT_QUEUE_MAPPING]);
	}

	if (tb[TCA_SKBEDIT_PTYPE] != NULL) {
		ptype = nla_data(tb[TCA_SKBEDIT_PTYPE]);
		if (!skb_pkt_type_ok(*ptype))
			return -EINVAL;
		flags |= SKBEDIT_F_PTYPE;
	}

	if (tb[TCA_SKBEDIT_MARK] != NULL) {
		flags |= SKBEDIT_F_MARK;
		mark = nla_data(tb[TCA_SKBEDIT_MARK]);
	}

	if (tb[TCA_SKBEDIT_MASK] != NULL) {
		flags |= SKBEDIT_F_MASK;
		mask = nla_data(tb[TCA_SKBEDIT_MASK]);
	}

#ifdef CONFIG_BCM_KF_ENHANCED_TC
	if (tb[TCA_SKBEDIT_FSMARKID] != NULL) {
		flags |= SKBEDIT_F_FSMARKID;
		fsmark_id = nla_data(tb[TCA_SKBEDIT_FSMARKID]);
	}
#endif /* CONFIG_BCM_KF_ENHANCED_TC */

	if (tb[TCA_SKBEDIT_FLAGS] != NULL) {
		u64 *pure_flags = nla_data(tb[TCA_SKBEDIT_FLAGS]);

		if (*pure_flags & SKBEDIT_F_INHERITDSFIELD)
			flags |= SKBEDIT_F_INHERITDSFIELD;
	}

	parm = nla_data(tb[TCA_SKBEDIT_PARMS]);
	index = parm->index;
	err = tcf_idr_check_alloc(tn, &index, a, bind);
	if (err < 0)
		return err;
	exists = err;
	if (exists && bind)
		return 0;

	if (!flags) {
		if (exists)
			tcf_idr_release(*a, bind);
		else
			tcf_idr_cleanup(tn, index);
		return -EINVAL;
	}

	if (!exists) {
		ret = tcf_idr_create(tn, index, est, a,
				     &act_skbedit_ops, bind, true);
		if (ret) {
			tcf_idr_cleanup(tn, index);
			return ret;
		}

		d = to_skbedit(*a);
		ret = ACT_P_CREATED;
	} else {
		d = to_skbedit(*a);
		if (!ovr) {
			tcf_idr_release(*a, bind);
			return -EEXIST;
		}
	}

	ASSERT_RTNL();

	params_new = kzalloc(sizeof(*params_new), GFP_KERNEL);
	if (unlikely(!params_new)) {
		tcf_idr_release(*a, bind);
		return -ENOMEM;
	}

	params_new->flags = flags;
	if (flags & SKBEDIT_F_PRIORITY)
		params_new->priority = *priority;
	if (flags & SKBEDIT_F_QUEUE_MAPPING)
		params_new->queue_mapping = *queue_mapping;
	if (flags & SKBEDIT_F_MARK)
		params_new->mark = *mark;
#ifdef CONFIG_BCM_KF_ENHANCED_TC
	if (flags & SKBEDIT_F_FSMARKID)
		params_new->fsmark_id = *fsmark_id;
#endif /* CONFIG_BCM_KF_ENHANCED_TC */
	if (flags & SKBEDIT_F_PTYPE)
		params_new->ptype = *ptype;
	/* default behaviour is to use all the bits */
	params_new->mask = 0xffffffff;
	if (flags & SKBEDIT_F_MASK)
		params_new->mask = *mask;

	d->tcf_action = parm->action;
	params_old = rtnl_dereference(d->params);
	rcu_assign_pointer(d->params, params_new);
	if (params_old)
		kfree_rcu(params_old, rcu);

	if (ret == ACT_P_CREATED)
		tcf_idr_insert(tn, *a);
	return ret;
}

static int tcf_skbedit_dump(struct sk_buff *skb, struct tc_action *a,
			    int bind, int ref)
{
	unsigned char *b = skb_tail_pointer(skb);
	struct tcf_skbedit *d = to_skbedit(a);
	struct tcf_skbedit_params *params;
	struct tc_skbedit opt = {
		.index   = d->tcf_index,
		.refcnt  = refcount_read(&d->tcf_refcnt) - ref,
		.bindcnt = atomic_read(&d->tcf_bindcnt) - bind,
		.action  = d->tcf_action,
	};
	u64 pure_flags = 0;
	struct tcf_t t;

	params = rtnl_dereference(d->params);

	if (nla_put(skb, TCA_SKBEDIT_PARMS, sizeof(opt), &opt))
		goto nla_put_failure;
	if ((params->flags & SKBEDIT_F_PRIORITY) &&
	    nla_put_u32(skb, TCA_SKBEDIT_PRIORITY, params->priority))
		goto nla_put_failure;
	if ((params->flags & SKBEDIT_F_QUEUE_MAPPING) &&
	    nla_put_u16(skb, TCA_SKBEDIT_QUEUE_MAPPING, params->queue_mapping))
		goto nla_put_failure;
	if ((params->flags & SKBEDIT_F_MARK) &&
	    nla_put_u32(skb, TCA_SKBEDIT_MARK, params->mark))
		goto nla_put_failure;
#ifdef CONFIG_BCM_KF_ENHANCED_TC
	if ((params->flags & SKBEDIT_F_FSMARKID) &&
	    nla_put_u32(skb, TCA_SKBEDIT_FSMARKID, params->fsmark_id))
		goto nla_put_failure;
#endif /* CONFIG_BCM_KF_ENHANCED_TC */
	if ((params->flags & SKBEDIT_F_PTYPE) &&
	    nla_put_u16(skb, TCA_SKBEDIT_PTYPE, params->ptype))
		goto nla_put_failure;
	if ((params->flags & SKBEDIT_F_MASK) &&
	    nla_put_u32(skb, TCA_SKBEDIT_MASK, params->mask))
		goto nla_put_failure;
	if (params->flags & SKBEDIT_F_INHERITDSFIELD)
		pure_flags |= SKBEDIT_F_INHERITDSFIELD;
	if (pure_flags != 0 &&
	    nla_put(skb, TCA_SKBEDIT_FLAGS, sizeof(pure_flags), &pure_flags))
		goto nla_put_failure;

	tcf_tm_dump(&t, &d->tcf_tm);
	if (nla_put_64bit(skb, TCA_SKBEDIT_TM, sizeof(t), &t, TCA_SKBEDIT_PAD))
		goto nla_put_failure;
	return skb->len;

nla_put_failure:
	nlmsg_trim(skb, b);
	return -1;
}

static void tcf_skbedit_cleanup(struct tc_action *a)
{
	struct tcf_skbedit *d = to_skbedit(a);
	struct tcf_skbedit_params *params;

	params = rcu_dereference_protected(d->params, 1);
	if (params)
		kfree_rcu(params, rcu);
}

static int tcf_skbedit_walker(struct net *net, struct sk_buff *skb,
			      struct netlink_callback *cb, int type,
			      const struct tc_action_ops *ops,
			      struct netlink_ext_ack *extack)
{
	struct tc_action_net *tn = net_generic(net, skbedit_net_id);

	return tcf_generic_walker(tn, skb, cb, type, ops, extack);
}

static int tcf_skbedit_search(struct net *net, struct tc_action **a, u32 index,
			      struct netlink_ext_ack *extack)
{
	struct tc_action_net *tn = net_generic(net, skbedit_net_id);

	return tcf_idr_search(tn, a, index);
}

static struct tc_action_ops act_skbedit_ops = {
	.kind		=	"skbedit",
	.type		=	TCA_ACT_SKBEDIT,
	.owner		=	THIS_MODULE,
	.act		=	tcf_skbedit_act,
	.dump		=	tcf_skbedit_dump,
	.init		=	tcf_skbedit_init,
	.cleanup	=	tcf_skbedit_cleanup,
	.walk		=	tcf_skbedit_walker,
	.lookup		=	tcf_skbedit_search,
	.size		=	sizeof(struct tcf_skbedit),
};

static __net_init int skbedit_init_net(struct net *net)
{
	struct tc_action_net *tn = net_generic(net, skbedit_net_id);

	return tc_action_net_init(net, tn, &act_skbedit_ops);
}

static void __net_exit skbedit_exit_net(struct list_head *net_list)
{
	tc_action_net_exit(net_list, skbedit_net_id);
}

static struct pernet_operations skbedit_net_ops = {
	.init = skbedit_init_net,
	.exit_batch = skbedit_exit_net,
	.id   = &skbedit_net_id,
	.size = sizeof(struct tc_action_net),
};

MODULE_AUTHOR("Alexander Duyck, <alexander.h.duyck@intel.com>");
MODULE_DESCRIPTION("SKB Editing");
MODULE_LICENSE("GPL");

static int __init skbedit_init_module(void)
{
	return tcf_register_action(&act_skbedit_ops, &skbedit_net_ops);
}

static void __exit skbedit_cleanup_module(void)
{
	tcf_unregister_action(&act_skbedit_ops, &skbedit_net_ops);
}

module_init(skbedit_init_module);
module_exit(skbedit_cleanup_module);
