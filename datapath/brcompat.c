/*
 * Copyright (c) 2009 Nicira Networks.
 * Distributed under the terms of the GNU GPL version 2.
 *
 * Significant portions of this file may be copied from parts of the Linux
 * kernel, by Linus Torvalds and others.
 */

#include <linux/kernel.h>
#include <asm/uaccess.h>
#include <linux/completion.h>
#include <linux/etherdevice.h>
#include <linux/if_bridge.h>
#include <linux/netdevice.h>
#include <linux/rtnetlink.h>
#include <net/genetlink.h>

#include "compat.h"
#include "openvswitch/brcompat-netlink.h"
#include "brc_procfs.h"
#include "datapath.h"

static struct genl_family brc_genl_family;
static struct genl_multicast_group brc_mc_group;

/* Time to wait for ovs-vswitchd to respond to a datapath action, in
 * jiffies. */
#define BRC_TIMEOUT (HZ * 5)

/* Mutex to serialize ovs-brcompatd callbacks.  (Some callbacks naturally hold
 * br_ioctl_mutex, others hold rtnl_lock, but we can't take the former
 * ourselves and we don't want to hold the latter over a potentially long
 * period of time.) */
static DEFINE_MUTEX(brc_serial);

/* Userspace communication. */
static DEFINE_SPINLOCK(brc_lock);    /* Ensure atomic access to these vars. */
static DECLARE_COMPLETION(brc_done); /* Userspace signaled operation done? */
static struct sk_buff *brc_reply;    /* Reply from userspace. */
static u32 brc_seq;		     /* Sequence number for current op. */

static struct sk_buff *brc_send_command(struct sk_buff *, struct nlattr **attrs);
static int brc_send_simple_command(struct sk_buff *);

static struct sk_buff *brc_make_request(int op, const char *bridge,
					const char *port)
{
	struct sk_buff *skb = genlmsg_new(NLMSG_GOODSIZE, GFP_KERNEL);
	if (!skb)
		goto error;

	genlmsg_put(skb, 0, 0, &brc_genl_family, 0, op);
	if (bridge)
		NLA_PUT_STRING(skb, BRC_GENL_A_DP_NAME, bridge);
	if (port)
		NLA_PUT_STRING(skb, BRC_GENL_A_PORT_NAME, port);
	return skb;

nla_put_failure:
	kfree_skb(skb);
error:
	return NULL;
}

static int brc_send_simple_command(struct sk_buff *request)
{
	struct nlattr *attrs[BRC_GENL_A_MAX + 1];
	struct sk_buff *reply;
	int error;

	reply = brc_send_command(request, attrs);
	if (IS_ERR(reply))
		return PTR_ERR(reply);

	error = nla_get_u32(attrs[BRC_GENL_A_ERR_CODE]);
	kfree_skb(reply);
	return -error;
}

static int brc_add_del_bridge(char __user *uname, int add)
{
	struct sk_buff *request;
	char name[IFNAMSIZ];

	if (copy_from_user(name, uname, IFNAMSIZ))
		return -EFAULT;

	name[IFNAMSIZ - 1] = 0;
	request = brc_make_request(add ? BRC_GENL_C_DP_ADD : BRC_GENL_C_DP_DEL,
				   name, NULL);
	if (!request)
		return -ENOMEM;

	return brc_send_simple_command(request);
}

static int brc_get_indices(int op, const char *br_name,
			   int __user *uindices, int n)
{
	struct nlattr *attrs[BRC_GENL_A_MAX + 1];
	struct sk_buff *request, *reply;
	int *indices;
	int ret;
	int len;

	if (n < 0)
		return -EINVAL;
	if (n >= 2048)
		return -ENOMEM;

	request = brc_make_request(op, br_name, NULL);
	if (!request)
		return -ENOMEM;

	reply = brc_send_command(request, attrs);
	ret = PTR_ERR(reply);
	if (IS_ERR(reply))
		goto exit;

	ret = -nla_get_u32(attrs[BRC_GENL_A_ERR_CODE]);
	if (ret < 0)
		goto exit_free_skb;

	ret = -EINVAL;
	if (!attrs[BRC_GENL_A_IFINDEXES])
		goto exit_free_skb;

	len = nla_len(attrs[BRC_GENL_A_IFINDEXES]);
	indices = nla_data(attrs[BRC_GENL_A_IFINDEXES]);
	if (len % sizeof(int))
		goto exit_free_skb;

	n = min_t(int, n, len / sizeof(int));
	ret = copy_to_user(uindices, indices, n * sizeof(int)) ? -EFAULT : n;

exit_free_skb:
	kfree_skb(reply);
exit:
	return ret;
}

/* Called with br_ioctl_mutex. */
static int brc_get_bridges(int __user *uindices, int n)
{
	return brc_get_indices(BRC_GENL_C_GET_BRIDGES, NULL, uindices, n);
}

/* Legacy deviceless bridge ioctl's.  Called with br_ioctl_mutex. */
static int old_deviceless(void __user *uarg)
{
	unsigned long args[3];

	if (copy_from_user(args, uarg, sizeof(args)))
		return -EFAULT;

	switch (args[0]) {
	case BRCTL_GET_BRIDGES:
		return brc_get_bridges((int __user *)args[1], args[2]);

	case BRCTL_ADD_BRIDGE:
		return brc_add_del_bridge((void __user *)args[1], 1);
	case BRCTL_DEL_BRIDGE:
		return brc_add_del_bridge((void __user *)args[1], 0);
	}

	return -EOPNOTSUPP;
}

/* Called with the br_ioctl_mutex. */
static int
#if LINUX_VERSION_CODE <= KERNEL_VERSION(2,6,23)
brc_ioctl_deviceless_stub(unsigned int cmd, void __user *uarg)
#else
brc_ioctl_deviceless_stub(struct net *net, unsigned int cmd, void __user *uarg)
#endif
{
	switch (cmd) {
	case SIOCGIFBR:
	case SIOCSIFBR:
		return old_deviceless(uarg);

	case SIOCBRADDBR:
		return brc_add_del_bridge(uarg, 1);
	case SIOCBRDELBR:
		return brc_add_del_bridge(uarg, 0);
	}

	return -EOPNOTSUPP;
}

static int brc_add_del_port(struct net_device *dev, int port_ifindex, int add)
{
	struct sk_buff *request;
	struct net_device *port;
	int err;

	port = __dev_get_by_index(&init_net, port_ifindex);
	if (!port)
		return -EINVAL;

	/* Save name of dev and port because there's a race between the
	 * rtnl_unlock() and the brc_send_simple_command(). */
	request = brc_make_request(add ? BRC_GENL_C_PORT_ADD : BRC_GENL_C_PORT_DEL,
				   dev->name, port->name);
	if (!request)
		return -ENOMEM;

	rtnl_unlock();
	err = brc_send_simple_command(request);
	rtnl_lock();

	return err;
}

static int brc_get_bridge_info(struct net_device *dev,
			       struct __bridge_info __user *ub)
{
	struct __bridge_info b;
	u64 id = 0;
	int i;

	memset(&b, 0, sizeof(struct __bridge_info));

	for (i=0; i<ETH_ALEN; i++)
		id |= (u64)dev->dev_addr[i] << (8*(ETH_ALEN-1 - i));
	b.bridge_id = cpu_to_be64(id);
	b.stp_enabled = 0;

	if (copy_to_user(ub, &b, sizeof(struct __bridge_info)))
		return -EFAULT;

	return 0;
}

static int brc_get_port_list(struct net_device *dev, int __user *uindices,
			     int num)
{
	int retval;

	rtnl_unlock();
	retval = brc_get_indices(BRC_GENL_C_GET_PORTS, dev->name,
				 uindices, num);
	rtnl_lock();

	return retval;
}

/*
 * Format up to a page worth of forwarding table entries
 * userbuf -- where to copy result
 * maxnum  -- maximum number of entries desired
 *            (limited to a page for sanity)
 * offset  -- number of records to skip
 */
static int brc_get_fdb_entries(struct net_device *dev, void __user *userbuf, 
			       unsigned long maxnum, unsigned long offset)
{
	struct nlattr *attrs[BRC_GENL_A_MAX + 1];
	struct sk_buff *request, *reply;
	int retval;
	int len;

	/* Clamp size to PAGE_SIZE, test maxnum to avoid overflow */
	if (maxnum > PAGE_SIZE/sizeof(struct __fdb_entry))
		maxnum = PAGE_SIZE/sizeof(struct __fdb_entry);

	request = brc_make_request(BRC_GENL_C_FDB_QUERY, dev->name, NULL);
	if (!request)
		return -ENOMEM;
	NLA_PUT_U64(request, BRC_GENL_A_FDB_COUNT, maxnum);
	NLA_PUT_U64(request, BRC_GENL_A_FDB_SKIP, offset);

	rtnl_unlock();
	reply = brc_send_command(request, attrs);
	retval = PTR_ERR(reply);
	if (IS_ERR(reply))
		goto exit;

	retval = -nla_get_u32(attrs[BRC_GENL_A_ERR_CODE]);
	if (retval < 0)
		goto exit_free_skb;

	retval = -EINVAL;
	if (!attrs[BRC_GENL_A_FDB_DATA])
		goto exit_free_skb;
	len = nla_len(attrs[BRC_GENL_A_FDB_DATA]);
	if (len % sizeof(struct __fdb_entry) ||
	    len / sizeof(struct __fdb_entry) > maxnum)
		goto exit_free_skb;

	retval = len / sizeof(struct __fdb_entry);
	if (copy_to_user(userbuf, nla_data(attrs[BRC_GENL_A_FDB_DATA]), len))
		retval = -EFAULT;

exit_free_skb:
	kfree_skb(reply);
exit:
	rtnl_lock();
	return retval;

nla_put_failure:
	kfree_skb(request);
	return -ENOMEM;
}

/* Legacy ioctl's through SIOCDEVPRIVATE.  Called with rtnl_lock. */
static int old_dev_ioctl(struct net_device *dev, struct ifreq *rq, int cmd)
{
	unsigned long args[4];

	if (copy_from_user(args, rq->ifr_data, sizeof(args)))
		return -EFAULT;

	switch (args[0]) {
	case BRCTL_ADD_IF:
		return brc_add_del_port(dev, args[1], 1);
	case BRCTL_DEL_IF:
		return brc_add_del_port(dev, args[1], 0);

	case BRCTL_GET_BRIDGE_INFO:
		return brc_get_bridge_info(dev, (struct __bridge_info __user *)args[1]);

	case BRCTL_GET_PORT_LIST:
		return brc_get_port_list(dev, (int __user *)args[1], args[2]);

	case BRCTL_GET_FDB_ENTRIES:
		return brc_get_fdb_entries(dev, (void __user *)args[1],
					   args[2], args[3]);
	}

	return -EOPNOTSUPP;
}

/* Called with the rtnl_lock. */
static int brc_dev_ioctl(struct net_device *dev, struct ifreq *rq, int cmd)
{
	int err;

	switch (cmd) {
		case SIOCDEVPRIVATE:
			err = old_dev_ioctl(dev, rq, cmd);
			break;

		case SIOCBRADDIF:
			return brc_add_del_port(dev, rq->ifr_ifindex, 1);
		case SIOCBRDELIF:
			return brc_add_del_port(dev, rq->ifr_ifindex, 0);

		default:
			err = -EOPNOTSUPP;
			break;
	}

	return err;
}


static struct genl_family brc_genl_family = {
	.id = GENL_ID_GENERATE,
	.hdrsize = 0,
	.name = BRC_GENL_FAMILY_NAME,
	.version = 1,
	.maxattr = BRC_GENL_A_MAX,
};

static int brc_genl_query(struct sk_buff *skb, struct genl_info *info)
{
	int err = -EINVAL;
	struct sk_buff *ans_skb;
	void *data;

	ans_skb = nlmsg_new(NLMSG_DEFAULT_SIZE, GFP_KERNEL);
	if (!ans_skb) 
		return -ENOMEM;

	data = genlmsg_put_reply(ans_skb, info, &brc_genl_family,
				 0, BRC_GENL_C_QUERY_MC);
	if (data == NULL) {
		err = -ENOMEM;
		goto err;
	}
	NLA_PUT_U32(ans_skb, BRC_GENL_A_MC_GROUP, brc_mc_group.id);

	genlmsg_end(ans_skb, data);
	return genlmsg_reply(ans_skb, info);

err:
nla_put_failure:
	kfree_skb(ans_skb);
	return err;
}

static struct genl_ops brc_genl_ops_query_dp = {
	.cmd = BRC_GENL_C_QUERY_MC,
	.flags = GENL_ADMIN_PERM, /* Requires CAP_NET_ADMIN privelege. */
	.policy = NULL,
	.doit = brc_genl_query,
	.dumpit = NULL
};

/* Attribute policy: what each attribute may contain.  */
static struct nla_policy brc_genl_policy[BRC_GENL_A_MAX + 1] = {
	[BRC_GENL_A_ERR_CODE] = { .type = NLA_U32 },

	[BRC_GENL_A_PROC_DIR] = { .type = NLA_NUL_STRING },
	[BRC_GENL_A_PROC_NAME] = { .type = NLA_NUL_STRING },
	[BRC_GENL_A_PROC_DATA] = { .type = NLA_NUL_STRING },

	[BRC_GENL_A_FDB_DATA] = { .type = NLA_UNSPEC },
};

static int brc_genl_dp_result(struct sk_buff *skb, struct genl_info *info)
{
	unsigned long int flags;
	int err;

	if (!info->attrs[BRC_GENL_A_ERR_CODE])
		return -EINVAL;

	skb = skb_clone(skb, GFP_KERNEL);
	if (!skb)
		return -ENOMEM;

	spin_lock_irqsave(&brc_lock, flags);
	if (brc_seq == info->snd_seq) {
		brc_seq++;

		if (brc_reply)
			kfree_skb(brc_reply);
		brc_reply = skb;

		complete(&brc_done);
		err = 0;
	} else {
		kfree_skb(skb);
		err = -ESTALE;
	}
	spin_unlock_irqrestore(&brc_lock, flags);

	return err;
}

static struct genl_ops brc_genl_ops_dp_result = {
	.cmd = BRC_GENL_C_DP_RESULT,
	.flags = GENL_ADMIN_PERM, /* Requires CAP_NET_ADMIN privelege. */
	.policy = brc_genl_policy,
	.doit = brc_genl_dp_result,
	.dumpit = NULL
};

static struct genl_ops brc_genl_ops_set_proc = {
	.cmd = BRC_GENL_C_SET_PROC,
	.flags = GENL_ADMIN_PERM, /* Requires CAP_NET_ADMIN privelege. */
	.policy = brc_genl_policy,
	.doit = brc_genl_set_proc,
	.dumpit = NULL
};

static struct sk_buff *brc_send_command(struct sk_buff *request,
					struct nlattr **attrs)
{
	unsigned long int flags;
	struct sk_buff *reply;
	int error;

	mutex_lock(&brc_serial);

	/* Increment sequence number first, so that we ignore any replies
	 * to stale requests. */
	spin_lock_irqsave(&brc_lock, flags);
	nlmsg_hdr(request)->nlmsg_seq = ++brc_seq;
	INIT_COMPLETION(brc_done);
	spin_unlock_irqrestore(&brc_lock, flags);

	nlmsg_end(request, nlmsg_hdr(request));

	/* Send message. */
	error = genlmsg_multicast(request, 0, brc_mc_group.id, GFP_KERNEL);
	if (error < 0)
		goto error;

	/* Wait for reply. */
	error = -ETIMEDOUT;
	if (!wait_for_completion_timeout(&brc_done, BRC_TIMEOUT)) {
		printk(KERN_WARNING "brcompat: timed out waiting for userspace\n");
		goto error;
    }

	/* Grab reply. */
	spin_lock_irqsave(&brc_lock, flags);
	reply = brc_reply;
	brc_reply = NULL;
	spin_unlock_irqrestore(&brc_lock, flags);

	mutex_unlock(&brc_serial);

	/* Re-parse message.  Can't fail, since it parsed correctly once
	 * already. */
	error = nlmsg_parse(nlmsg_hdr(reply), GENL_HDRLEN,
			    attrs, BRC_GENL_A_MAX, brc_genl_policy);
	WARN_ON(error);

	return reply;

error:
	mutex_unlock(&brc_serial);
	return ERR_PTR(error);
}

static int __init brc_init(void)
{
	int err;

	printk("Open vSwitch Bridge Compatibility, built "__DATE__" "__TIME__"\n");

	/* Set the bridge ioctl handler */
	brioctl_set(brc_ioctl_deviceless_stub);

	/* Set the openvswitch_mod device ioctl handler */
	dp_ioctl_hook = brc_dev_ioctl;

	/* Randomize the initial sequence number.  This is not a security
	 * feature; it only helps avoid crossed wires between userspace and
	 * the kernel when the module is unloaded and reloaded. */
	brc_seq = net_random();

	/* Register generic netlink family to communicate changes to
	 * userspace. */
	err = genl_register_family(&brc_genl_family);
	if (err)
		goto error;

	err = genl_register_ops(&brc_genl_family, &brc_genl_ops_query_dp);
	if (err != 0) 
		goto err_unregister;

	err = genl_register_ops(&brc_genl_family, &brc_genl_ops_dp_result);
	if (err != 0) 
		goto err_unregister;

	err = genl_register_ops(&brc_genl_family, &brc_genl_ops_set_proc);
	if (err != 0) 
		goto err_unregister;

	strcpy(brc_mc_group.name, "brcompat");
	err = genl_register_mc_group(&brc_genl_family, &brc_mc_group);
	if (err < 0)
		goto err_unregister;

	return 0;

err_unregister:
	genl_unregister_family(&brc_genl_family);
error:
	printk(KERN_EMERG "brcompat: failed to install!");
	return err;
}

static void brc_cleanup(void)
{
	/* Unregister ioctl hooks */
	dp_ioctl_hook = NULL;
	brioctl_set(NULL);

	genl_unregister_family(&brc_genl_family);
	brc_procfs_exit();
}

module_init(brc_init);
module_exit(brc_cleanup);

MODULE_DESCRIPTION("Open vSwitch bridge compatibility");
MODULE_AUTHOR("Nicira Networks");
MODULE_LICENSE("GPL");
