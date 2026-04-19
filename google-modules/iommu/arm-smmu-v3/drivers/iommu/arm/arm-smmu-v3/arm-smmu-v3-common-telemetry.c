// SPDX-License-Identifier: GPL-2.0

#define pr_fmt(fmt) "smmu-v3-telemetry: " fmt

#include <kvm/iommu.h>
#include "arm-smmu-v3.h"
#include "arm-smmu-v3-common-telemetry.h"
#include "pkvm/hyp-arm-smmu-v3-common-telemetry.h"

static struct kobject *arm_smmu_root_sysfs_kobj;
static struct kobject *arm_smmu_domains_sysfs_kobj;
static struct kobject *arm_smmu_devices_sysfs_kobj;

static DEFINE_MUTEX(arm_smmu_telemetry_mutex);

/* Give unique index number to each domain initializing its telemetry */
static unsigned int domain_index;

static struct hyp_shared_arm_smmu_telemetry *shared_telemetry;

/* Toggle 'enable' knob from kernel cmdline. */
static bool smmu_telemetry_enable;
module_param(smmu_telemetry_enable, bool, false);

/*
 * Global knob to control IOMMU telemetry. The static key provides the necessary atomicity for
 * branch patching, ensuring that performance critical paths safely transition to NOPs when
 * disabled.
 */
static DEFINE_STATIC_KEY_FALSE(iommu_telemetry_on);

/**
 * sysfs support for device telemetry
 */
static ssize_t device_id_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	struct arm_smmu_device_telemetry_common *asdevtc;

	asdevtc = container_of(kobj, struct arm_smmu_device_telemetry_common, device_kobj);
	return sysfs_emit(buf, "%u\n", asdevtc->data.kasdevt.device_id);
}

static ssize_t cmdq_exhausted_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	struct arm_smmu_device_telemetry_common *asdevtc;
	u32 device_id;
	u64 cnt = 0;

	asdevtc = container_of(kobj, struct arm_smmu_device_telemetry_common, device_kobj);
	switch (asdevtc->mode) {
	case NON_PKVM_MODE_DRIVER:
		return 0;
	case PKVM_MODE_DRIVER:
		device_id = asdevtc->data.kasdevt.device_id;
		cnt = shared_telemetry->hyp_dev_tel_arr[device_id].cmdq_tel.cmdq_full_cnt;
		break;
	default:
		break;
	}

	return sysfs_emit(buf, "%llu\n", cnt);
}

static ssize_t cmdq_sync_max_latency_us_show(struct kobject *kobj, struct kobj_attribute *attr,
					     char *buf)
{
	struct arm_smmu_device_telemetry_common *asdevtc;
	struct hyp_arm_smmu_device_telemetry *hasdevt;
	u32 device_id;
	u64 max_count;
	u64 max_latency = 0;

	asdevtc = container_of(kobj, struct arm_smmu_device_telemetry_common, device_kobj);
	switch (asdevtc->mode) {
	case NON_PKVM_MODE_DRIVER:
		return 0;
	case PKVM_MODE_DRIVER:
		device_id = asdevtc->data.kasdevt.device_id;
		hasdevt = &shared_telemetry->hyp_dev_tel_arr[device_id];
		max_count = hasdevt->cmdq_tel.sync_cmd_max_timer_tick;
		max_latency = div_u64(max_count * 1000000, shared_telemetry->arch_timer_rate);
		break;
	default:
		break;
	}

	return sysfs_emit(buf, "%llu\n", max_latency);
}

static ssize_t cmdq_sync_avg_latency_us_show(struct kobject *kobj, struct kobj_attribute *attr,
					     char *buf)
{
	struct arm_smmu_device_telemetry_common *asdevtc;
	struct hyp_arm_smmu_device_telemetry *hasdevt;
	u32 device_id;
	u64 total_timer_tick;
	u64 avg_timer_tick;
	u64 avg_latency = 0;

	asdevtc = container_of(kobj, struct arm_smmu_device_telemetry_common, device_kobj);
	switch (asdevtc->mode) {
	case NON_PKVM_MODE_DRIVER:
		return 0;
	case PKVM_MODE_DRIVER:
		device_id = asdevtc->data.kasdevt.device_id;
		hasdevt = &shared_telemetry->hyp_dev_tel_arr[device_id];
		total_timer_tick = hasdevt->cmdq_tel.sync_cmd_total_timer_tick;
		avg_timer_tick = div_u64(total_timer_tick, hasdevt->cmdq_tel.sync_cmd_cnt);
		avg_latency = div_u64(avg_timer_tick * 1000000, shared_telemetry->arch_timer_rate);
		break;
	default:
		break;
	}

	return sysfs_emit(buf, "%llu\n", avg_latency);
}

static ssize_t cmdq_sync_count_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	struct arm_smmu_device_telemetry_common *asdevtc;
	struct hyp_arm_smmu_device_telemetry *hasdevt;
	u32 device_id;
	u64 cnt = 0;

	asdevtc = container_of(kobj, struct arm_smmu_device_telemetry_common, device_kobj);
	switch (asdevtc->mode) {
	case NON_PKVM_MODE_DRIVER:
		return 0;
	case PKVM_MODE_DRIVER:
		device_id = asdevtc->data.kasdevt.device_id;
		hasdevt = &shared_telemetry->hyp_dev_tel_arr[device_id];
		cnt = hasdevt->cmdq_tel.sync_cmd_cnt;
		break;
	default:
		break;
	}

	return sysfs_emit(buf, "%llu\n", cnt);
}

static struct kobj_attribute device_id_attr = __ATTR_RO(device_id);
static struct kobj_attribute cmdq_exhausted_attr = __ATTR_RO(cmdq_exhausted);
static struct kobj_attribute cmdq_sync_max_latency_us_attr = __ATTR_RO(cmdq_sync_max_latency_us);
static struct kobj_attribute cmdq_sync_avg_latency_us_attr = __ATTR_RO(cmdq_sync_avg_latency_us);
static struct kobj_attribute cmdq_sync_count_attr = __ATTR_RO(cmdq_sync_count);

static struct attribute *device_attrs[] = {
	&device_id_attr.attr,
	&cmdq_exhausted_attr.attr,
	&cmdq_sync_max_latency_us_attr.attr,
	&cmdq_sync_avg_latency_us_attr.attr,
	&cmdq_sync_count_attr.attr,
	NULL
};
ATTRIBUTE_GROUPS(device);

static void device_release(struct kobject *kobj)
{
	struct arm_smmu_device_telemetry_common *asdevtc;

	asdevtc = container_of(kobj, struct arm_smmu_device_telemetry_common, device_kobj);
	kfree(asdevtc);
}

static const struct kobj_type device_ktype = {
	.sysfs_ops = &kobj_sysfs_ops,
	.default_groups = device_groups,
	.release = device_release,
};

/**
 * sysfs support smmu for domain telemetry
 */
static ssize_t map_sg_count_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	struct arm_smmu_domain_telemetry_common *asdtc;
	int offset = 0;
	unsigned long map_sg_count = 0;

	asdtc = container_of(kobj, struct arm_smmu_domain_telemetry_common, domain_kobj);
	switch (asdtc->mode) {
	case NON_PKVM_MODE_DRIVER:
		break;
	case PKVM_MODE_DRIVER:
		map_sg_count = atomic64_read(&asdtc->data.kasdt.map_sg_count);
		break;
	default:
		return -EINVAL;
	}

	offset += sysfs_emit_at(buf, offset, "%lu", map_sg_count);

	return offset;
}

static ssize_t avg_sg_list_len_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	struct arm_smmu_domain_telemetry_common *asdtc;
	int offset = 0;
	unsigned long map_sg_count = 0;
	unsigned long sg_len_total = 0;
	unsigned int avg = 0;

	asdtc = container_of(kobj, struct arm_smmu_domain_telemetry_common, domain_kobj);
	switch (asdtc->mode) {
	case NON_PKVM_MODE_DRIVER:
		break;
	case PKVM_MODE_DRIVER:
		/*
		 * The counters for total scatter-gather list length and count
		 * are updated asynchronously. A concurrent update could occur
		 * between the two atomic reads below, potentially leading to
		 * a slightly stale average being displayed. This trade-off is
		 * accepted to avoid the performance overhead of heavier locking
		 * in this performance-sensitive path.
		 */
		map_sg_count = atomic64_read(&asdtc->data.kasdt.map_sg_count);
		sg_len_total = atomic64_read(&asdtc->data.kasdt.sg_len_total);
		break;
	default:
		return -EINVAL;
	}

	if (map_sg_count)
		avg = div_u64(sg_len_total, map_sg_count);

	offset += sysfs_emit_at(buf, offset, "%u", avg);

	return offset;
}

static ssize_t domain_id_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	struct arm_smmu_domain_telemetry_common *asdtc;
	int offset = 0;

	asdtc = container_of(kobj, struct arm_smmu_domain_telemetry_common, domain_kobj);
	switch (asdtc->mode) {
	case NON_PKVM_MODE_DRIVER:
		break;
	case PKVM_MODE_DRIVER:
		offset += sysfs_emit_at(buf, offset, "%d", asdtc->data.kasdt.domain_id);
		break;
	default:
		return -EINVAL;
	}

	return offset;
}

static ssize_t mode_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	struct arm_smmu_domain_telemetry_common *asdtc;
	int offset = 0;

	asdtc = container_of(kobj, struct arm_smmu_domain_telemetry_common, domain_kobj);
	offset += sysfs_emit_at(buf, offset, "%s",
				asdtc->mode ? "pKVM mode" : "non-pKVM mode");

	return offset;
}

static ssize_t type_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	struct arm_smmu_domain_telemetry_common *asdtc;
	int offset = 0;

	asdtc = container_of(kobj, struct arm_smmu_domain_telemetry_common, domain_kobj);
	switch (asdtc->type) {
	case DOMAIN_TYPE_S1:
		offset += sysfs_emit_at(buf, offset, "%s", "S1");
		break;
	case DOMAIN_TYPE_S2:
		offset += sysfs_emit_at(buf, offset, "%s", "S2");
		break;
	default:
		return 0;
	}

	return offset;
}

static ssize_t iova_span_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	struct arm_smmu_domain_telemetry_common *asdtc;
	u64 iova_min;
	u64 iova_max;
	u64 iova_span = 0;

	asdtc = container_of(kobj, struct arm_smmu_domain_telemetry_common, domain_kobj);
	switch (asdtc->mode) {
	case NON_PKVM_MODE_DRIVER:
		break;
	case PKVM_MODE_DRIVER:
		iova_min = atomic64_read(&asdtc->data.kasdt.iova_min);
		iova_max = atomic64_read(&asdtc->data.kasdt.iova_max);

		if (iova_min != U64_MAX)
			iova_span = iova_max - iova_min + 1;
		break;
	default:
		return -EINVAL;
	}

	return sysfs_emit(buf, "%llu\n", iova_span);
}

static ssize_t smmu_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	struct arm_smmu_domain_telemetry_common *asdtc;
	struct kvm_arm_smmu_domain *domain;
	ssize_t offset;

	asdtc = container_of(kobj, struct arm_smmu_domain_telemetry_common, domain_kobj);
	switch (asdtc->mode) {
	case NON_PKVM_MODE_DRIVER:
		return sysfs_emit(buf, "none\n");
	case PKVM_MODE_DRIVER:
		mutex_lock(&asdtc->domain_lock);
		domain = asdtc->data.kasdt.domain;
		if (!domain || !domain->smmu) {
			mutex_unlock(&asdtc->domain_lock);
			return sysfs_emit(buf, "unattached\n");
		}

		offset = sysfs_emit(buf, "%s\n", dev_name(domain->smmu->dev));
		mutex_unlock(&asdtc->domain_lock);
		return offset;
	default:
		return -EINVAL;
	}
}

static ssize_t attached_devices_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	struct arm_smmu_domain_telemetry_common *asdtc;
	struct kvm_arm_smmu_domain *domain;
	struct kvm_arm_smmu_master *master;
	struct arm_smmu_device *smmu;
	unsigned long flags;
	int offset = 0;
	int i, j;

	asdtc = container_of(kobj, struct arm_smmu_domain_telemetry_common, domain_kobj);
	switch (asdtc->mode) {
	case NON_PKVM_MODE_DRIVER:
		return 0;
	case PKVM_MODE_DRIVER:
		mutex_lock(&asdtc->domain_lock);
		domain = asdtc->data.kasdt.domain;
		if (!domain) {
			mutex_unlock(&asdtc->domain_lock);
			return 0;
		}

		smmu = domain->smmu;
		if (!smmu) {
			mutex_unlock(&asdtc->domain_lock);
			return 0;
		}

		/*
		 * The list of masters can be modified concurrently. We must hold streams_mutex to
		 * safely access master->streams
		 */
		mutex_lock(&smmu->streams_mutex);

		/* The list of masters can be modified concurrently. */
		spin_lock_irqsave(&domain->masters_lock, flags);
		for (i = 0; i < MAX_MASTER_DEVICES_PER_DOMAIN; i++) {
			master = domain->masters[i];
			if (!master)
				continue;

			offset += sysfs_emit_at(buf, offset, "device: %s pasid: %u SIDs:",
						dev_name(master->dev), domain->pasid[i]);

			for (j = 0; j < master->num_streams; j++)
				offset += sysfs_emit_at(buf, offset, " 0x%x",
							master->streams[j].id);

			offset += sysfs_emit_at(buf, offset, "\n");
		}
		spin_unlock_irqrestore(&domain->masters_lock, flags);
		mutex_unlock(&smmu->streams_mutex);
		mutex_unlock(&asdtc->domain_lock);

		return offset;
	default:
		return -EINVAL;
	}
}

static ssize_t arm_smmu_domain_show_map_counter(struct kobject *kobj, char *buf, int idx)
{
	struct arm_smmu_domain_telemetry_common *asdtc;
	int domain_id;
	unsigned int map_counter = 0;

	asdtc = container_of(kobj, struct arm_smmu_domain_telemetry_common, domain_kobj);
	switch (asdtc->mode) {
	case NON_PKVM_MODE_DRIVER:
		break;
	case PKVM_MODE_DRIVER:
		domain_id = asdtc->data.kasdt.domain_id;
		if (!shared_telemetry)
			return 0;

		if (!(domain_id > 0 && domain_id < MAX_SMMU_DOMAIN))
			return 0;

		map_counter =
			shared_telemetry->hyp_dom_tel_arr[domain_id].map_counters.counters[idx];
		break;
	default:
		return -EINVAL;
	}

	return sysfs_emit(buf, "%u\n", map_counter);
}

#define DEFINE_DOMAIN_MAP_ATTR(size, idx)							\
	static ssize_t num_##size##_mapping_show(struct kobject *kobj,				\
						 struct kobj_attribute *attr, char *buf)	\
	{											\
		return arm_smmu_domain_show_map_counter(kobj, buf, idx);			\
	}											\
static struct kobj_attribute num_##size##_mapping_attr = __ATTR_RO(num_##size##_mapping)

DEFINE_DOMAIN_MAP_ATTR(4K, IDX_4K);
DEFINE_DOMAIN_MAP_ATTR(16K, IDX_16K);
DEFINE_DOMAIN_MAP_ATTR(64K, IDX_64K);
DEFINE_DOMAIN_MAP_ATTR(2M, IDX_2M);
DEFINE_DOMAIN_MAP_ATTR(32M, IDX_32M);
DEFINE_DOMAIN_MAP_ATTR(512M, IDX_512M);
DEFINE_DOMAIN_MAP_ATTR(1G, IDX_1G);
DEFINE_DOMAIN_MAP_ATTR(16G, IDX_16G);

#define DEFINE_DOMAIN_ALIGNMENT_ATTR(size)							\
static ssize_t unaligned_##size##_mappings_show(struct kobject *kobj,				\
						struct kobj_attribute *attr, char *buf)		\
{												\
	struct arm_smmu_domain_telemetry_common *asdtc;						\
												\
	asdtc = container_of(kobj, struct arm_smmu_domain_telemetry_common, domain_kobj);	\
	return sysfs_emit(buf, "%llu\n",							\
			  atomic64_read(&asdtc->alignment_stats.unaligned_##size##_mappings));	\
}												\
static struct kobj_attribute unaligned_##size##_mappings_attr =					\
							__ATTR_RO(unaligned_##size##_mappings)

DEFINE_DOMAIN_ALIGNMENT_ATTR(64k);
DEFINE_DOMAIN_ALIGNMENT_ATTR(2m);
DEFINE_DOMAIN_ALIGNMENT_ATTR(32m);
DEFINE_DOMAIN_ALIGNMENT_ATTR(512m);
DEFINE_DOMAIN_ALIGNMENT_ATTR(1g);

static struct kobj_attribute map_sg_count_attr = __ATTR_RO(map_sg_count);
static struct kobj_attribute avg_sg_list_len_attr = __ATTR_RO(avg_sg_list_len);
static struct kobj_attribute domain_id_attr = __ATTR_RO(domain_id);
static struct kobj_attribute mode_attr = __ATTR_RO(mode);
static struct kobj_attribute type_attr = __ATTR_RO(type);
static struct kobj_attribute smmu_attr = __ATTR_RO(smmu);
static struct kobj_attribute attached_devices_attr = __ATTR_RO(attached_devices);
static struct kobj_attribute iova_span_attr = __ATTR_RO(iova_span);

static struct attribute *domain_attrs[] = {
	&map_sg_count_attr.attr,
	&avg_sg_list_len_attr.attr,
	&domain_id_attr.attr,
	&mode_attr.attr,
	&type_attr.attr,
	&smmu_attr.attr,
	&attached_devices_attr.attr,
	&iova_span_attr.attr,
	&num_4K_mapping_attr.attr,
	&num_16K_mapping_attr.attr,
	&num_64K_mapping_attr.attr,
	&num_2M_mapping_attr.attr,
	&num_32M_mapping_attr.attr,
	&num_512M_mapping_attr.attr,
	&num_1G_mapping_attr.attr,
	&num_16G_mapping_attr.attr,
	&unaligned_64k_mappings_attr.attr,
	&unaligned_2m_mappings_attr.attr,
	&unaligned_32m_mappings_attr.attr,
	&unaligned_512m_mappings_attr.attr,
	&unaligned_1g_mappings_attr.attr,
	NULL
};
ATTRIBUTE_GROUPS(domain);

static void domain_release(struct kobject *kobj)
{
	struct arm_smmu_domain_telemetry_common *asdtc;

	asdtc = container_of(kobj, struct arm_smmu_domain_telemetry_common, domain_kobj);
	kfree(asdtc);
}

static const struct kobj_type domain_ktype = {
	.sysfs_ops = &kobj_sysfs_ops,
	.default_groups = domain_groups,
	.release = domain_release,
};

/**
 * sysfs support for s2 telemetry
 */
static ssize_t s2_atomic_pool_alloc_reqs_show(struct kobject *kobj,
					      struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%llu\n", shared_telemetry->hs2t.s2_atomic_pages.alloc_reqs);
}

static ssize_t s2_atomic_pool_free_reqs_show(struct kobject *kobj,
					     struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%llu\n", shared_telemetry->hs2t.s2_atomic_pages.free_reqs);
}

static ssize_t s2_atomic_pool_pages_in_use_show(struct kobject *kobj,
						struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%lld\n", shared_telemetry->hs2t.s2_atomic_pages.pages_in_use);
}

static ssize_t num_s2_tlb_invalidates_show(struct kobject *kobj,
					   struct kobj_attribute *attr, char *buf)
{
	if (!shared_telemetry)
		return 0;

	return sysfs_emit(buf, "%llu\n", shared_telemetry->hs2t.num_s2_tlb_invalidates);
}

static struct kobj_attribute s2_atomic_pool_alloc_reqs_attr = __ATTR_RO(s2_atomic_pool_alloc_reqs);
static struct kobj_attribute s2_atomic_pool_free_reqs_attr = __ATTR_RO(s2_atomic_pool_free_reqs);
static struct kobj_attribute s2_atomic_pool_pages_in_use_attr =
							__ATTR_RO(s2_atomic_pool_pages_in_use);
static struct kobj_attribute num_s2_tlb_invalidates_attr = __ATTR_RO(num_s2_tlb_invalidates);

#define DEFINE_S2_MAP_ATTR(size, idx)							\
static ssize_t num_s2_##size##_mapping_show(struct kobject *kobj,			\
					 struct kobj_attribute *attr, char *buf)	\
{											\
	if (!shared_telemetry)								\
		return 0;								\
											\
	return sysfs_emit(buf, "%u\n",							\
			  shared_telemetry->hs2t.s2_map_counters.counters[idx]);	\
}											\
static struct kobj_attribute num_s2_##size##_mapping_attr = __ATTR_RO(num_s2_##size##_mapping)

DEFINE_S2_MAP_ATTR(4K, IDX_4K);
DEFINE_S2_MAP_ATTR(16K, IDX_16K);
DEFINE_S2_MAP_ATTR(64K, IDX_64K);
DEFINE_S2_MAP_ATTR(2M, IDX_2M);
DEFINE_S2_MAP_ATTR(32M, IDX_32M);
DEFINE_S2_MAP_ATTR(512M, IDX_512M);
DEFINE_S2_MAP_ATTR(1G, IDX_1G);
DEFINE_S2_MAP_ATTR(16G, IDX_16G);

static struct attribute *stage2_idmap_attrs[] = {
	&s2_atomic_pool_alloc_reqs_attr.attr,
	&s2_atomic_pool_free_reqs_attr.attr,
	&s2_atomic_pool_pages_in_use_attr.attr,
	&num_s2_tlb_invalidates_attr.attr,
	&num_s2_4K_mapping_attr.attr,
	&num_s2_16K_mapping_attr.attr,
	&num_s2_64K_mapping_attr.attr,
	&num_s2_2M_mapping_attr.attr,
	&num_s2_32M_mapping_attr.attr,
	&num_s2_512M_mapping_attr.attr,
	&num_s2_1G_mapping_attr.attr,
	&num_s2_16G_mapping_attr.attr,
	NULL
};

static const struct attribute_group stage2_idmap_group = {
	.name = "stage2-idmap",
	.attrs = stage2_idmap_attrs,
};

/**
 * sysfs support for files at root level
 */
static ssize_t enable_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", static_branch_unlikely(&iommu_telemetry_on) ? 1 : 0);
}

static ssize_t cur_s1_pgtable_usage_show(struct kobject *kobj, struct kobj_attribute *attr,
					 char *buf)
{
	if (!shared_telemetry)
		return 0;

	return sysfs_emit(buf, "%d\n", shared_telemetry->cur_s1_pgtable_usage);
}

static ssize_t max_s1_pgtable_usage_show(struct kobject *kobj, struct kobj_attribute *attr,
					 char *buf)
{
	if (!shared_telemetry)
		return 0;

	return sysfs_emit(buf, "%d\n", shared_telemetry->max_s1_pgtable_usage);
}

static struct kobj_attribute enable_attr = __ATTR_RO(enable);
static struct kobj_attribute cur_s1_pgtable_usage_attr = __ATTR_RO(cur_s1_pgtable_usage);
static struct kobj_attribute max_s1_pgtable_usage_attr = __ATTR_RO(max_s1_pgtable_usage);

static const struct attribute *root_attrs[] = {
	&enable_attr.attr,
	&cur_s1_pgtable_usage_attr.attr,
	&max_s1_pgtable_usage_attr.attr,
	NULL
};

/**
 * Telemetry recording APIs
 */
void arm_smmu_dom_tlm_inc_map_sg_cnt(struct arm_smmu_domain_telemetry_common *asdtc)
{
	if (!static_branch_unlikely(&iommu_telemetry_on))
		return;

	switch (asdtc->mode) {
	case NON_PKVM_MODE_DRIVER:
		break;
	case PKVM_MODE_DRIVER:
		atomic64_inc(&asdtc->data.kasdt.map_sg_count);
		break;
	default:
		break;
	}
}

static void atomic64_max(atomic64_t *target, u64 value)
{
	u64 old_val, new_val;

	while (1) {
		old_val = atomic64_read(target);
		new_val = max(old_val, value);
		if (old_val == new_val)
			break;

		if (atomic64_cmpxchg(target, old_val, new_val) == old_val)
			break;
	}
}

static void atomic64_min(atomic64_t *target, u64 value)
{
	u64 old_val, new_val;

	while (1) {
		old_val = atomic64_read(target);
		new_val = min(old_val, value);
		if (old_val == new_val)
			break;

		if (atomic64_cmpxchg(target, old_val, new_val) == old_val)
			break;
	}
}

void arm_smmu_dom_tlm_rec_iova_range(struct arm_smmu_domain_telemetry_common *asdtc, u64 iova,
				     size_t size)
{
	if (!static_branch_unlikely(&iommu_telemetry_on))
		return;

	switch (asdtc->mode) {
	case NON_PKVM_MODE_DRIVER:
		break;
	case PKVM_MODE_DRIVER:
		atomic64_min(&asdtc->data.kasdt.iova_min, iova);
		atomic64_max(&asdtc->data.kasdt.iova_max, iova + size - 1);
		break;
	default:
		break;
	}
}

void arm_smmu_dom_tlm_rec_sg_len(struct arm_smmu_domain_telemetry_common *asdtc,
				 unsigned int sg_list_len)
{
	if (!static_branch_unlikely(&iommu_telemetry_on))
		return;

	switch (asdtc->mode) {
	case NON_PKVM_MODE_DRIVER:
		break;
	case PKVM_MODE_DRIVER:
		atomic64_add(sg_list_len, &asdtc->data.kasdt.sg_len_total);
		break;
	default:
		break;
	}
}

void arm_smmu_dom_tlm_inc_sg_segment_cnt(struct arm_smmu_domain_telemetry_common *asdtc)
{
	if (!static_branch_unlikely(&iommu_telemetry_on))
		return;

	switch (asdtc->mode) {
	case NON_PKVM_MODE_DRIVER:
		break;
	case PKVM_MODE_DRIVER:
		atomic_inc(&asdtc->data.kasdt.current_sg_count);
		break;
	default:
		break;
	}
}

void arm_smmu_dom_tlm_reset_sg_segment_cnt(struct arm_smmu_domain_telemetry_common *asdtc)
{
	if (!static_branch_unlikely(&iommu_telemetry_on))
		return;

	switch (asdtc->mode) {
	case NON_PKVM_MODE_DRIVER:
		break;
	case PKVM_MODE_DRIVER:
		atomic_set(&asdtc->data.kasdt.current_sg_count, 0);
		break;
	default:
		break;
	}
}

void arm_smmu_dom_tlm_commit_sg_list_len(struct arm_smmu_domain_telemetry_common *asdtc)
{
	int current_sg_count;

	if (!static_branch_unlikely(&iommu_telemetry_on))
		return;

	switch (asdtc->mode) {
	case NON_PKVM_MODE_DRIVER:
		break;
	case PKVM_MODE_DRIVER:
		current_sg_count = atomic_read(&asdtc->data.kasdt.current_sg_count);
		if (current_sg_count > 1) {
			atomic64_add(current_sg_count, &asdtc->data.kasdt.sg_len_total);
			atomic64_inc(&asdtc->data.kasdt.map_sg_count);
		}
		atomic_set(&asdtc->data.kasdt.current_sg_count, 0);
		break;
	default:
		break;
	}
}

void arm_smmu_dom_tlm_rec_domain_id(struct arm_smmu_domain_telemetry_common *asdtc,
				    pkvm_handle_t domain_id)
{
	switch (asdtc->mode) {
	case NON_PKVM_MODE_DRIVER:
		break;
	case PKVM_MODE_DRIVER:
		asdtc->data.kasdt.domain_id = domain_id;

		if (domain_id == KVM_IOMMU_DOMAIN_IDMAP_ID)
			asdtc->type = DOMAIN_TYPE_S2;
		else if (domain_id > KVM_IOMMU_DOMAIN_IDMAP_ID && domain_id < MAX_SMMU_DOMAIN)
			asdtc->type = DOMAIN_TYPE_S1;
		else
			asdtc->type = DOMAIN_TYPE_UNATTACHED;
		break;
	default:
		break;
	}
}

void arm_smmu_dev_tlm_rec_dev_id(struct arm_smmu_device_telemetry_common *asdevtc,
				 pkvm_handle_t device_id)
{
	switch (asdevtc->mode) {
	case NON_PKVM_MODE_DRIVER:
		break;
	case PKVM_MODE_DRIVER:
		asdevtc->data.kasdevt.device_id = device_id;
		asdevtc->data.kasdevt.hasdevt = &shared_telemetry->hyp_dev_tel_arr[device_id];
		break;
	default:
		break;
	}
}

void arm_smmu_dom_tlm_rec_iova_pa_alignment(struct arm_smmu_domain_telemetry_common *asdtc,
					    unsigned long iova, phys_addr_t paddr,
					    size_t size)
{
	u64 xor;
	int level;

	if (!static_branch_unlikely(&iommu_telemetry_on) || !asdtc)
		return;

	/* Fast path for the most common case (e.g., 4K pages) */
	if (size < SZ_64K)
		return;

	xor = iova ^ paddr;

	/* Determine the highest granularity level for the given size */
	if (size >= SZ_1G)
		level = 5;
	else if (size >= SZ_512M)
		level = 4;
	else if (size >= SZ_32M)
		level = 3;
	else if (size >= SZ_2M)
		level = 2;
	else /* size >= SZ_64K */
		level = 1;

	switch (level) {
	case 5:
		if (xor & (SZ_1G - 1))
			atomic64_inc(&asdtc->alignment_stats.unaligned_1g_mappings);
		fallthrough;
	case 4:
		if (xor & (SZ_512M - 1))
			atomic64_inc(&asdtc->alignment_stats.unaligned_512m_mappings);
		fallthrough;
	case 3:
		if (xor & (SZ_32M - 1))
			atomic64_inc(&asdtc->alignment_stats.unaligned_32m_mappings);
		fallthrough;
	case 2:
		if (xor & (SZ_2M - 1))
			atomic64_inc(&asdtc->alignment_stats.unaligned_2m_mappings);
		fallthrough;
	case 1:
		if (xor & (SZ_64K - 1))
			atomic64_inc(&asdtc->alignment_stats.unaligned_64k_mappings);
	}
}

/**
 * Telemetry initialization for root level, domain level or device level
 */
static void update_hyp_tel_enable(int set)
{
	if (!shared_telemetry)
		return;

	shared_telemetry->enabled = set;
}

/*
 * Note: Currently, there is no counter init function, and if SMMU driver is removed, below code
 *       would be having empty sysfs directories. But, this is not production scenario and at
 *       present, de-init code is not populated. A small leak is manageable.
 */
static void arm_smmu_telemetry_init(void)
{
	/* Bookkeeper for one time telemetry initialization */
	static int arm_smmu_telemetry_init_done;

	/*
	 * Telemetry init function needs to be performed once during boot up, but before any
	 * device or domain gets allocated by the SMMU driver.
	 */
	if (arm_smmu_telemetry_init_done)
		return;

	arm_smmu_root_sysfs_kobj = kobject_create_and_add("iommu", kernel_kobj);
	if (!arm_smmu_root_sysfs_kobj) {
		pr_err("Failed to create iommu root directory in sysfs\n");
		return;
	}

	if (sysfs_create_files(arm_smmu_root_sysfs_kobj, root_attrs)) {
		pr_err("Failed to create root files in sysfs\n");
		goto free_root;
	}

	arm_smmu_domains_sysfs_kobj = kobject_create_and_add("domains", arm_smmu_root_sysfs_kobj);
	if (!arm_smmu_domains_sysfs_kobj) {
		pr_err("Failed to create domains directory in sysfs\n");
		goto release_root_attrs;
	}

	arm_smmu_devices_sysfs_kobj = kobject_create_and_add("devices", arm_smmu_root_sysfs_kobj);
	if (!arm_smmu_devices_sysfs_kobj) {
		pr_err("Failed to create devices directory in sysfs\n");
		goto free_domains;
	}

	if (sysfs_create_group(arm_smmu_root_sysfs_kobj, &stage2_idmap_group)) {
		pr_err("Failed to create stage2-idmap group in sysfs\n");
		goto free_devices;
	}

	if (smmu_telemetry_enable) {
		static_branch_enable(&iommu_telemetry_on);
		update_hyp_tel_enable(1);
	}

	arm_smmu_telemetry_init_done = 1;

	return;

free_devices:
	kobject_put(arm_smmu_devices_sysfs_kobj);

free_domains:
	kobject_put(arm_smmu_domains_sysfs_kobj);

release_root_attrs:
	sysfs_remove_files(arm_smmu_root_sysfs_kobj, root_attrs);

free_root:
	kobject_put(arm_smmu_root_sysfs_kobj);
}

int arm_smmu_domain_telemetry_alloc(void *domain, int mode)
{
	struct arm_smmu_domain_telemetry_common *asdtc;
	struct kvm_arm_smmu_domain *kvm_arm_smmu_domain;
	int ret = 0;

	asdtc = kzalloc(sizeof(*asdtc), GFP_KERNEL);
	if (!asdtc)
		return -ENOMEM;

	mutex_init(&asdtc->domain_lock);

	mutex_lock(&arm_smmu_telemetry_mutex);

	arm_smmu_telemetry_init();

	if (kobject_init_and_add(&asdtc->domain_kobj, &domain_ktype,
				 arm_smmu_domains_sysfs_kobj, "%03u", domain_index++)) {
		pr_err("Failed to initialize smmu domain telemetry kobj\n");
		ret = -EIO;
		mutex_unlock(&arm_smmu_telemetry_mutex);
		goto free_asdtc;
	}
	mutex_unlock(&arm_smmu_telemetry_mutex);

	asdtc->mode = mode;
	switch (mode) {
	case NON_PKVM_MODE_DRIVER:
		//TODO: Add this support
		break;
	case PKVM_MODE_DRIVER:
		kvm_arm_smmu_domain = (struct kvm_arm_smmu_domain *)domain;
		kvm_arm_smmu_domain->telemetry = asdtc;
		asdtc->data.kasdt.domain = kvm_arm_smmu_domain;
		atomic64_set(&asdtc->data.kasdt.iova_min, U64_MAX);
		atomic64_set(&asdtc->data.kasdt.iova_max, 0);
		break;
	default:
		pr_err("Invalid mode %d during domain telmetry alloc\n", mode);
		ret = -EINVAL;
		goto release_kobj;
	}

	return ret;

free_asdtc:
	kfree(asdtc);
	return ret;

release_kobj:
	kobject_put(&asdtc->domain_kobj);
	return ret;
}

void arm_smmu_domain_telemetry_free(void *domain, int mode)
{
	struct arm_smmu_domain_telemetry_common *asdtc = NULL;
	struct kvm_arm_smmu_domain *kvm_arm_smmu_domain;

	switch (mode) {
	case NON_PKVM_MODE_DRIVER:
		//TODO: Add this support
		break;
	case PKVM_MODE_DRIVER:
		kvm_arm_smmu_domain = (struct kvm_arm_smmu_domain *)domain;
		asdtc = kvm_arm_smmu_domain->telemetry;
		kvm_arm_smmu_domain->telemetry = NULL;
		break;
	default:
		pr_err("Invalid mode %d during domain telmetry free\n", mode);
		return;
	}

	if (asdtc) {
		mutex_lock(&asdtc->domain_lock);
		asdtc->data.kasdt.domain = NULL;
		mutex_unlock(&asdtc->domain_lock);
		kobject_put(&asdtc->domain_kobj);
	}

	/* Note: We don't explicitly free asdtc right away. kobject framework would call
	 * kobj_type.release callback to let the owner of kobject know that now it's safe to free
	 * structure. This callback is implemented above and that does the freeing.
	 */
}

struct arm_smmu_device_telemetry_common
*arm_smmu_device_telemetry_alloc(struct arm_smmu_device *smmu_device, int mode)
{
	struct arm_smmu_device_telemetry_common *asdevtc;

	asdevtc = kzalloc(sizeof(*asdevtc), GFP_KERNEL);
	if (!asdevtc)
		return NULL;

	mutex_lock(&arm_smmu_telemetry_mutex);

	arm_smmu_telemetry_init();

	if (!arm_smmu_devices_sysfs_kobj) {
		mutex_unlock(&arm_smmu_telemetry_mutex);
		dev_err(smmu_device->dev, "Devices root folder not created\n");
		goto free_asdevtc;
	}

	if (kobject_init_and_add(&asdevtc->device_kobj, &device_ktype,
				 arm_smmu_devices_sysfs_kobj, "%s", dev_name(smmu_device->dev))) {
		mutex_unlock(&arm_smmu_telemetry_mutex);
		dev_err(smmu_device->dev,
			"Failed to initialize smmu device telemetry kobj for device.\n");
		goto free_asdevtc;
	}

	mutex_unlock(&arm_smmu_telemetry_mutex);

	asdevtc->mode = mode;
	switch (mode) {
	case NON_PKVM_MODE_DRIVER:
		break;
	case PKVM_MODE_DRIVER:
		asdevtc->data.kasdevt.dev = smmu_device->dev;
		break;
	default:
		dev_err(smmu_device->dev, "Invalid mode %d during device telemetry alloc.\n", mode);
		goto release_kobj;
	}

	return asdevtc;

free_asdevtc:
	kfree(asdevtc);
	return NULL;

release_kobj:
	kobject_put(&asdevtc->device_kobj);
	return NULL;
}

void arm_smmu_device_telemetry_free(struct arm_smmu_device *smmu_device, int mode)
{
	struct arm_smmu_device_telemetry_common *asdevtc = NULL;
	struct host_arm_smmu_device *host_smmu = smmu_to_host(smmu_device);

	switch (mode) {
	case NON_PKVM_MODE_DRIVER:
		break;
	case PKVM_MODE_DRIVER:
		asdevtc = host_smmu->telemetry;
		break;
	default:
		dev_err(smmu_device->dev, "Invalid mode %d during device telemetry free\n", mode);
		return;
	}

	if (asdevtc)
		kobject_put(&asdevtc->device_kobj);
}

void arm_smmu_set_shared_telemetry_ptr(struct hyp_shared_arm_smmu_telemetry *shared_tel)
{
	shared_telemetry = shared_tel;
}

