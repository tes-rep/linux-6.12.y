// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * SCSI Primary Commands (SPC) parsing and emulation.
 *
 * (c) Copyright 2002-2013 Datera, Inc.
 *
 * Nicholas A. Bellinger <nab@kernel.org>
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/unaligned.h>

#include <scsi/scsi_proto.h>
#include <scsi/scsi_common.h>
#include <scsi/scsi_tcq.h>

#include <target/target_core_base.h>
#include <target/target_core_backend.h>
#include <target/target_core_fabric.h>

#include "target_core_internal.h"
#include "target_core_alua.h"
#include "target_core_pr.h"
#include "target_core_ua.h"
#include "target_core_xcopy.h"

static void spc_fill_alua_data(struct se_lun *lun, unsigned char *buf)
{
	struct t10_alua_tg_pt_gp *tg_pt_gp;

	/*
	 * Set SCCS for MAINTENANCE_IN + REPORT_TARGET_PORT_GROUPS.
	 */
	buf[5]	= 0x80;

	/*
	 * Set TPGS field for explicit and/or implicit ALUA access type
	 * and opteration.
	 *
	 * See spc4r17 section 6.4.2 Table 135
	 */
	rcu_read_lock();
	tg_pt_gp = rcu_dereference(lun->lun_tg_pt_gp);
	if (tg_pt_gp)
		buf[5] |= tg_pt_gp->tg_pt_gp_alua_access_type;
	rcu_read_unlock();
}

static u16
spc_find_scsi_transport_vd(int proto_id)
{
	switch (proto_id) {
	case SCSI_PROTOCOL_FCP:
		return SCSI_VERSION_DESCRIPTOR_FCP4;
	case SCSI_PROTOCOL_ISCSI:
		return SCSI_VERSION_DESCRIPTOR_ISCSI;
	case SCSI_PROTOCOL_SAS:
		return SCSI_VERSION_DESCRIPTOR_SAS3;
	case SCSI_PROTOCOL_SBP:
		return SCSI_VERSION_DESCRIPTOR_SBP3;
	case SCSI_PROTOCOL_SRP:
		return SCSI_VERSION_DESCRIPTOR_SRP;
	default:
		pr_warn("Cannot find VERSION DESCRIPTOR value for unknown SCSI"
			" transport PROTOCOL IDENTIFIER %#x\n", proto_id);
		return 0;
	}
}

sense_reason_t
spc_emulate_inquiry_std(struct se_cmd *cmd, unsigned char *buf)
{
	struct se_lun *lun = cmd->se_lun;
	struct se_portal_group *tpg = lun->lun_tpg;
	struct se_device *dev = cmd->se_dev;
	struct se_session *sess = cmd->se_sess;

	/* Set RMB (removable media) for tape devices */
	if (dev->transport->get_device_type(dev) == TYPE_TAPE)
		buf[1] = 0x80;

	buf[2] = 0x06; /* SPC-4 */

	/*
	 * NORMACA and HISUP = 0, RESPONSE DATA FORMAT = 2
	 *
	 * SPC4 says:
	 *   A RESPONSE DATA FORMAT field set to 2h indicates that the
	 *   standard INQUIRY data is in the format defined in this
	 *   standard. Response data format values less than 2h are
	 *   obsolete. Response data format values greater than 2h are
	 *   reserved.
	 */
	buf[3] = 2;

	/*
	 * Enable SCCS and TPGS fields for Emulated ALUA
	 */
	spc_fill_alua_data(lun, buf);

	/*
	 * Set Third-Party Copy (3PC) bit to indicate support for EXTENDED_COPY
	 */
	if (dev->dev_attrib.emulate_3pc)
		buf[5] |= 0x8;
	/*
	 * Set Protection (PROTECT) bit when DIF has been enabled on the
	 * device, and the fabric supports VERIFY + PASS.  Also report
	 * PROTECT=1 if sess_prot_type has been configured to allow T10-PI
	 * to unprotected devices.
	 */
	if (sess->sup_prot_ops & (TARGET_PROT_DIN_PASS | TARGET_PROT_DOUT_PASS)) {
		if (dev->dev_attrib.pi_prot_type || cmd->se_sess->sess_prot_type)
			buf[5] |= 0x1;
	}

	/*
	 * Set MULTIP bit to indicate presence of multiple SCSI target ports
	 */
	if (dev->export_count > 1)
		buf[6] |= 0x10;

	buf[7] = 0x2; /* CmdQue=1 */

	/*
	 * ASCII data fields described as being left-aligned shall have any
	 * unused bytes at the end of the field (i.e., highest offset) and the
	 * unused bytes shall be filled with ASCII space characters (20h).
	 */
	memset(&buf[8], 0x20,
	       INQUIRY_VENDOR_LEN + INQUIRY_MODEL_LEN + INQUIRY_REVISION_LEN);
	memcpy(&buf[8], dev->t10_wwn.vendor,
	       strnlen(dev->t10_wwn.vendor, INQUIRY_VENDOR_LEN));
	memcpy(&buf[16], dev->t10_wwn.model,
	       strnlen(dev->t10_wwn.model, INQUIRY_MODEL_LEN));
	memcpy(&buf[32], dev->t10_wwn.revision,
	       strnlen(dev->t10_wwn.revision, INQUIRY_REVISION_LEN));

	/*
	 * Set the VERSION DESCRIPTOR fields
	 */
	put_unaligned_be16(SCSI_VERSION_DESCRIPTOR_SAM5, &buf[58]);
	put_unaligned_be16(spc_find_scsi_transport_vd(tpg->proto_id), &buf[60]);
	put_unaligned_be16(SCSI_VERSION_DESCRIPTOR_SPC4, &buf[62]);
	if (cmd->se_dev->transport->get_device_type(dev) == TYPE_DISK)
		put_unaligned_be16(SCSI_VERSION_DESCRIPTOR_SBC3, &buf[64]);

	buf[4] = 91; /* Set additional length to 91 */

	return 0;
}
EXPORT_SYMBOL(spc_emulate_inquiry_std);

/* unit serial number */
static sense_reason_t
spc_emulate_evpd_80(struct se_cmd *cmd, unsigned char *buf)
{
	struct se_device *dev = cmd->se_dev;
	u16 len;

	if (dev->dev_flags & DF_EMULATED_VPD_UNIT_SERIAL) {
		len = sprintf(&buf[4], "%s", dev->t10_wwn.unit_serial);
		len++; /* Extra Byte for NULL Terminator */
		buf[3] = len;
	}
	return 0;
}

/*
 * Generate NAA IEEE Registered Extended designator
 */
void spc_gen_naa_6h_vendor_specific(struct se_device *dev,
				    unsigned char *buf)
{
	unsigned char *p = &dev->t10_wwn.unit_serial[0];
	u32 company_id = dev->t10_wwn.company_id;
	int cnt, off = 0;
	bool next = true;

	/*
	 * Start NAA IEEE Registered Extended Identifier/Designator
	 */
	buf[off] = 0x6 << 4;

	/* IEEE COMPANY_ID */
	buf[off++] |= (company_id >> 20) & 0xf;
	buf[off++] = (company_id >> 12) & 0xff;
	buf[off++] = (company_id >> 4) & 0xff;
	buf[off] = (company_id & 0xf) << 4;

	/*
	 * Generate up to 36 bits of VENDOR SPECIFIC IDENTIFIER starting on
	 * byte 3 bit 3-0 for NAA IEEE Registered Extended DESIGNATOR field
	 * format, followed by 64 bits of VENDOR SPECIFIC IDENTIFIER EXTENSION
	 * to complete the payload.  These are based from VPD=0x80 PRODUCT SERIAL
	 * NUMBER set via vpd_unit_serial in target_core_configfs.c to ensure
	 * per device uniqeness.
	 */
	for (cnt = off + 13; *p && off < cnt; p++) {
		int val = hex_to_bin(*p);

		if (val < 0)
			continue;

		if (next) {
			next = false;
			buf[off++] |= val;
		} else {
			next = true;
			buf[off] = val << 4;
		}
	}
}

/*
 * Device identification VPD, for a complete list of
 * DESIGNATOR TYPEs see spc4r17 Table 459.
 */
sense_reason_t
spc_emulate_evpd_83(struct se_cmd *cmd, unsigned char *buf)
{
	struct se_device *dev = cmd->se_dev;
	struct se_lun *lun = cmd->se_lun;
	struct se_portal_group *tpg = NULL;
	struct t10_alua_lu_gp_member *lu_gp_mem;
	struct t10_alua_tg_pt_gp *tg_pt_gp;
	unsigned char *prod = &dev->t10_wwn.model[0];
	u32 off = 0;
	u16 len = 0, id_len;

	off = 4;

	/*
	 * NAA IEEE Registered Extended Assigned designator format, see
	 * spc4r17 section 7.7.3.6.5
	 *
	 * We depend upon a target_core_mod/ConfigFS provided
	 * /sys/kernel/config/target/core/$HBA/$DEV/wwn/vpd_unit_serial
	 * value in order to return the NAA id.
	 */
	if (!(dev->dev_flags & DF_EMULATED_VPD_UNIT_SERIAL))
		goto check_t10_vend_desc;

	/* CODE SET == Binary */
	buf[off++] = 0x1;

	/* Set ASSOCIATION == addressed logical unit: 0)b */
	buf[off] = 0x00;

	/* Identifier/Designator type == NAA identifier */
	buf[off++] |= 0x3;
	off++;

	/* Identifier/Designator length */
	buf[off++] = 0x10;

	/* NAA IEEE Registered Extended designator */
	spc_gen_naa_6h_vendor_specific(dev, &buf[off]);

	len = 20;
	off = (len + 4);

check_t10_vend_desc:
	/*
	 * T10 Vendor Identifier Page, see spc4r17 section 7.7.3.4
	 */
	id_len = 8; /* For Vendor field */

	if (dev->dev_flags & DF_EMULATED_VPD_UNIT_SERIAL)
		id_len += sprintf(&buf[off+12], "%s:%s", prod,
				&dev->t10_wwn.unit_serial[0]);
	buf[off] = 0x2; /* ASCII */
	buf[off+1] = 0x1; /* T10 Vendor ID */
	buf[off+2] = 0x0;
	/* left align Vendor ID and pad with spaces */
	memset(&buf[off+4], 0x20, INQUIRY_VENDOR_LEN);
	memcpy(&buf[off+4], dev->t10_wwn.vendor,
	       strnlen(dev->t10_wwn.vendor, INQUIRY_VENDOR_LEN));
	/* Extra Byte for NULL Terminator */
	id_len++;
	/* Identifier Length */
	buf[off+3] = id_len;
	/* Header size for Designation descriptor */
	len += (id_len + 4);
	off += (id_len + 4);

	if (1) {
		struct t10_alua_lu_gp *lu_gp;
		u32 padding, scsi_name_len, scsi_target_len;
		u16 lu_gp_id = 0;
		u16 tg_pt_gp_id = 0;
		u16 tpgt;

		tpg = lun->lun_tpg;
		/*
		 * Relative target port identifer, see spc4r17
		 * section 7.7.3.7
		 *
		 * Get the PROTOCOL IDENTIFIER as defined by spc4r17
		 * section 7.5.1 Table 362
		 */
		buf[off] = tpg->proto_id << 4;
		buf[off++] |= 0x1; /* CODE SET == Binary */
		buf[off] = 0x80; /* Set PIV=1 */
		/* Set ASSOCIATION == target port: 01b */
		buf[off] |= 0x10;
		/* DESIGNATOR TYPE == Relative target port identifer */
		buf[off++] |= 0x4;
		off++; /* Skip over Reserved */
		buf[off++] = 4; /* DESIGNATOR LENGTH */
		/* Skip over Obsolete field in RTPI payload
		 * in Table 472 */
		off += 2;
		put_unaligned_be16(lun->lun_tpg->tpg_rtpi, &buf[off]);
		off += 2;
		len += 8; /* Header size + Designation descriptor */
		/*
		 * Target port group identifier, see spc4r17
		 * section 7.7.3.8
		 *
		 * Get the PROTOCOL IDENTIFIER as defined by spc4r17
		 * section 7.5.1 Table 362
		 */
		rcu_read_lock();
		tg_pt_gp = rcu_dereference(lun->lun_tg_pt_gp);
		if (!tg_pt_gp) {
			rcu_read_unlock();
			goto check_lu_gp;
		}
		tg_pt_gp_id = tg_pt_gp->tg_pt_gp_id;
		rcu_read_unlock();

		buf[off] = tpg->proto_id << 4;
		buf[off++] |= 0x1; /* CODE SET == Binary */
		buf[off] = 0x80; /* Set PIV=1 */
		/* Set ASSOCIATION == target port: 01b */
		buf[off] |= 0x10;
		/* DESIGNATOR TYPE == Target port group identifier */
		buf[off++] |= 0x5;
		off++; /* Skip over Reserved */
		buf[off++] = 4; /* DESIGNATOR LENGTH */
		off += 2; /* Skip over Reserved Field */
		put_unaligned_be16(tg_pt_gp_id, &buf[off]);
		off += 2;
		len += 8; /* Header size + Designation descriptor */
		/*
		 * Logical Unit Group identifier, see spc4r17
		 * section 7.7.3.8
		 */
check_lu_gp:
		lu_gp_mem = dev->dev_alua_lu_gp_mem;
		if (!lu_gp_mem)
			goto check_scsi_name;

		spin_lock(&lu_gp_mem->lu_gp_mem_lock);
		lu_gp = lu_gp_mem->lu_gp;
		if (!lu_gp) {
			spin_unlock(&lu_gp_mem->lu_gp_mem_lock);
			goto check_scsi_name;
		}
		lu_gp_id = lu_gp->lu_gp_id;
		spin_unlock(&lu_gp_mem->lu_gp_mem_lock);

		buf[off++] |= 0x1; /* CODE SET == Binary */
		/* DESIGNATOR TYPE == Logical Unit Group identifier */
		buf[off++] |= 0x6;
		off++; /* Skip over Reserved */
		buf[off++] = 4; /* DESIGNATOR LENGTH */
		off += 2; /* Skip over Reserved Field */
		put_unaligned_be16(lu_gp_id, &buf[off]);
		off += 2;
		len += 8; /* Header size + Designation descriptor */
		/*
		 * SCSI name string designator, see spc4r17
		 * section 7.7.3.11
		 *
		 * Get the PROTOCOL IDENTIFIER as defined by spc4r17
		 * section 7.5.1 Table 362
		 */
check_scsi_name:
		buf[off] = tpg->proto_id << 4;
		buf[off++] |= 0x3; /* CODE SET == UTF-8 */
		buf[off] = 0x80; /* Set PIV=1 */
		/* Set ASSOCIATION == target port: 01b */
		buf[off] |= 0x10;
		/* DESIGNATOR TYPE == SCSI name string */
		buf[off++] |= 0x8;
		off += 2; /* Skip over Reserved and length */
		/*
		 * SCSI name string identifer containing, $FABRIC_MOD
		 * dependent information.  For LIO-Target and iSCSI
		 * Target Port, this means "<iSCSI name>,t,0x<TPGT> in
		 * UTF-8 encoding.
		 */
		tpgt = tpg->se_tpg_tfo->tpg_get_tag(tpg);
		scsi_name_len = sprintf(&buf[off], "%s,t,0x%04x",
					tpg->se_tpg_tfo->tpg_get_wwn(tpg), tpgt);
		scsi_name_len += 1 /* Include  NULL terminator */;
		/*
		 * The null-terminated, null-padded (see 4.4.2) SCSI
		 * NAME STRING field contains a UTF-8 format string.
		 * The number of bytes in the SCSI NAME STRING field
		 * (i.e., the value in the DESIGNATOR LENGTH field)
		 * shall be no larger than 256 and shall be a multiple
		 * of four.
		 */
		padding = ((-scsi_name_len) & 3);
		if (padding)
			scsi_name_len += padding;
		if (scsi_name_len > 256)
			scsi_name_len = 256;

		buf[off-1] = scsi_name_len;
		off += scsi_name_len;
		/* Header size + Designation descriptor */
		len += (scsi_name_len + 4);

		/*
		 * Target device designator
		 */
		buf[off] = tpg->proto_id << 4;
		buf[off++] |= 0x3; /* CODE SET == UTF-8 */
		buf[off] = 0x80; /* Set PIV=1 */
		/* Set ASSOCIATION == target device: 10b */
		buf[off] |= 0x20;
		/* DESIGNATOR TYPE == SCSI name string */
		buf[off++] |= 0x8;
		off += 2; /* Skip over Reserved and length */
		/*
		 * SCSI name string identifer containing, $FABRIC_MOD
		 * dependent information.  For LIO-Target and iSCSI
		 * Target Port, this means "<iSCSI name>" in
		 * UTF-8 encoding.
		 */
		scsi_target_len = sprintf(&buf[off], "%s",
					  tpg->se_tpg_tfo->tpg_get_wwn(tpg));
		scsi_target_len += 1 /* Include  NULL terminator */;
		/*
		 * The null-terminated, null-padded (see 4.4.2) SCSI
		 * NAME STRING field contains a UTF-8 format string.
		 * The number of bytes in the SCSI NAME STRING field
		 * (i.e., the value in the DESIGNATOR LENGTH field)
		 * shall be no larger than 256 and shall be a multiple
		 * of four.
		 */
		padding = ((-scsi_target_len) & 3);
		if (padding)
			scsi_target_len += padding;
		if (scsi_target_len > 256)
			scsi_target_len = 256;

		buf[off-1] = scsi_target_len;
		off += scsi_target_len;

		/* Header size + Designation descriptor */
		len += (scsi_target_len + 4);
	}
	put_unaligned_be16(len, &buf[2]); /* Page Length for VPD 0x83 */
	return 0;
}
EXPORT_SYMBOL(spc_emulate_evpd_83);

/* Extended INQUIRY Data VPD Page */
static sense_reason_t
spc_emulate_evpd_86(struct se_cmd *cmd, unsigned char *buf)
{
	struct se_device *dev = cmd->se_dev;
	struct se_session *sess = cmd->se_sess;

	buf[3] = 0x3c;
	/*
	 * Set GRD_CHK + REF_CHK for TYPE1 protection, or GRD_CHK
	 * only for TYPE3 protection.
	 */
	if (sess->sup_prot_ops & (TARGET_PROT_DIN_PASS | TARGET_PROT_DOUT_PASS)) {
		if (dev->dev_attrib.pi_prot_type == TARGET_DIF_TYPE1_PROT ||
		    cmd->se_sess->sess_prot_type == TARGET_DIF_TYPE1_PROT)
			buf[4] = 0x5;
		else if (dev->dev_attrib.pi_prot_type == TARGET_DIF_TYPE3_PROT ||
			 cmd->se_sess->sess_prot_type == TARGET_DIF_TYPE3_PROT)
			buf[4] = 0x4;
	}

	/* logical unit supports type 1 and type 3 protection */
	if ((dev->transport->get_device_type(dev) == TYPE_DISK) &&
	    (sess->sup_prot_ops & (TARGET_PROT_DIN_PASS | TARGET_PROT_DOUT_PASS)) &&
	    (dev->dev_attrib.pi_prot_type || cmd->se_sess->sess_prot_type)) {
		buf[4] |= (0x3 << 3);
	}

	/* Set HEADSUP, ORDSUP, SIMPSUP */
	buf[5] = 0x07;

	/* If WriteCache emulation is enabled, set V_SUP */
	if (target_check_wce(dev))
		buf[6] = 0x01;
	/* If an LBA map is present set R_SUP */
	spin_lock(&cmd->se_dev->t10_alua.lba_map_lock);
	if (!list_empty(&dev->t10_alua.lba_map_list))
		buf[8] = 0x10;
	spin_unlock(&cmd->se_dev->t10_alua.lba_map_lock);
	return 0;
}

/* Block Limits VPD page */
static sense_reason_t
spc_emulate_evpd_b0(struct se_cmd *cmd, unsigned char *buf)
{
	struct se_device *dev = cmd->se_dev;
	u32 mtl = 0;
	int have_tp = 0, opt, min;
	u32 io_max_blocks;

	/*
	 * Following spc3r22 section 6.5.3 Block Limits VPD page, when
	 * emulate_tpu=1 or emulate_tpws=1 we will be expect a
	 * different page length for Thin Provisioning.
	 */
	if (dev->dev_attrib.emulate_tpu || dev->dev_attrib.emulate_tpws)
		have_tp = 1;

	buf[0] = dev->transport->get_device_type(dev);
	buf[3] = have_tp ? 0x3c : 0x10;

	/* Set WSNZ to 1 */
	buf[4] = 0x01;
	/*
	 * Set MAXIMUM COMPARE AND WRITE LENGTH
	 */
	if (dev->dev_attrib.emulate_caw)
		buf[5] = 0x01;

	/*
	 * Set OPTIMAL TRANSFER LENGTH GRANULARITY
	 */
	if (dev->transport->get_io_min && (min = dev->transport->get_io_min(dev)))
		put_unaligned_be16(min / dev->dev_attrib.block_size, &buf[6]);
	else
		put_unaligned_be16(1, &buf[6]);

	/*
	 * Set MAXIMUM TRANSFER LENGTH
	 *
	 * XXX: Currently assumes single PAGE_SIZE per scatterlist for fabrics
	 * enforcing maximum HW scatter-gather-list entry limit
	 */
	if (cmd->se_tfo->max_data_sg_nents) {
		mtl = (cmd->se_tfo->max_data_sg_nents * PAGE_SIZE) /
		       dev->dev_attrib.block_size;
	}
	io_max_blocks = mult_frac(dev->dev_attrib.hw_max_sectors,
			dev->dev_attrib.hw_block_size,
			dev->dev_attrib.block_size);
	put_unaligned_be32(min_not_zero(mtl, io_max_blocks), &buf[8]);

	/*
	 * Set OPTIMAL TRANSFER LENGTH
	 */
	if (dev->transport->get_io_opt && (opt = dev->transport->get_io_opt(dev)))
		put_unaligned_be32(opt / dev->dev_attrib.block_size, &buf[12]);
	else
		put_unaligned_be32(dev->dev_attrib.optimal_sectors, &buf[12]);

	/*
	 * Exit now if we don't support TP.
	 */
	if (!have_tp)
		goto max_write_same;

	/*
	 * Set MAXIMUM UNMAP LBA COUNT
	 */
	put_unaligned_be32(dev->dev_attrib.max_unmap_lba_count, &buf[20]);

	/*
	 * Set MAXIMUM UNMAP BLOCK DESCRIPTOR COUNT
	 */
	put_unaligned_be32(dev->dev_attrib.max_unmap_block_desc_count,
			   &buf[24]);

	/*
	 * Set OPTIMAL UNMAP GRANULARITY
	 */
	put_unaligned_be32(dev->dev_attrib.unmap_granularity, &buf[28]);

	/*
	 * UNMAP GRANULARITY ALIGNMENT
	 */
	put_unaligned_be32(dev->dev_attrib.unmap_granularity_alignment,
			   &buf[32]);
	if (dev->dev_attrib.unmap_granularity_alignment != 0)
		buf[32] |= 0x80; /* Set the UGAVALID bit */

	/*
	 * MAXIMUM WRITE SAME LENGTH
	 */
max_write_same:
	put_unaligned_be64(dev->dev_attrib.max_write_same_len, &buf[36]);

	return 0;
}

/* Block Device Characteristics VPD page */
static sense_reason_t
spc_emulate_evpd_b1(struct se_cmd *cmd, unsigned char *buf)
{
	struct se_device *dev = cmd->se_dev;

	buf[0] = dev->transport->get_device_type(dev);
	buf[3] = 0x3c;
	buf[5] = dev->dev_attrib.is_nonrot ? 1 : 0;

	return 0;
}

/* Thin Provisioning VPD */
static sense_reason_t
spc_emulate_evpd_b2(struct se_cmd *cmd, unsigned char *buf)
{
	struct se_device *dev = cmd->se_dev;

	/*
	 * From spc3r22 section 6.5.4 Thin Provisioning VPD page:
	 *
	 * The PAGE LENGTH field is defined in SPC-4. If the DP bit is set to
	 * zero, then the page length shall be set to 0004h.  If the DP bit
	 * is set to one, then the page length shall be set to the value
	 * defined in table 162.
	 */
	buf[0] = dev->transport->get_device_type(dev);

	/*
	 * Set Hardcoded length mentioned above for DP=0
	 */
	put_unaligned_be16(0x0004, &buf[2]);

	/*
	 * The THRESHOLD EXPONENT field indicates the threshold set size in
	 * LBAs as a power of 2 (i.e., the threshold set size is equal to
	 * 2(threshold exponent)).
	 *
	 * Note that this is currently set to 0x00 as mkp says it will be
	 * changing again.  We can enable this once it has settled in T10
	 * and is actually used by Linux/SCSI ML code.
	 */
	buf[4] = 0x00;

	/*
	 * A TPU bit set to one indicates that the device server supports
	 * the UNMAP command (see 5.25). A TPU bit set to zero indicates
	 * that the device server does not support the UNMAP command.
	 */
	if (dev->dev_attrib.emulate_tpu != 0)
		buf[5] = 0x80;

	/*
	 * A TPWS bit set to one indicates that the device server supports
	 * the use of the WRITE SAME (16) command (see 5.42) to unmap LBAs.
	 * A TPWS bit set to zero indicates that the device server does not
	 * support the use of the WRITE SAME (16) command to unmap LBAs.
	 */
	if (dev->dev_attrib.emulate_tpws != 0)
		buf[5] |= 0x40 | 0x20;

	/*
	 * The unmap_zeroes_data set means that the underlying device supports
	 * REQ_OP_DISCARD and has the discard_zeroes_data bit set. This
	 * satisfies the SBC requirements for LBPRZ, meaning that a subsequent
	 * read will return zeroes after an UNMAP or WRITE SAME (16) to an LBA
	 * See sbc4r36 6.6.4.
	 */
	if (((dev->dev_attrib.emulate_tpu != 0) ||
	     (dev->dev_attrib.emulate_tpws != 0)) &&
	     (dev->dev_attrib.unmap_zeroes_data != 0))
		buf[5] |= 0x04;

	return 0;
}

/* Referrals VPD page */
static sense_reason_t
spc_emulate_evpd_b3(struct se_cmd *cmd, unsigned char *buf)
{
	struct se_device *dev = cmd->se_dev;

	buf[0] = dev->transport->get_device_type(dev);
	buf[3] = 0x0c;
	put_unaligned_be32(dev->t10_alua.lba_map_segment_size, &buf[8]);
	put_unaligned_be32(dev->t10_alua.lba_map_segment_multiplier, &buf[12]);

	return 0;
}

static sense_reason_t
spc_emulate_evpd_00(struct se_cmd *cmd, unsigned char *buf);

static struct {
	uint8_t		page;
	sense_reason_t	(*emulate)(struct se_cmd *, unsigned char *);
} evpd_handlers[] = {
	{ .page = 0x00, .emulate = spc_emulate_evpd_00 },
	{ .page = 0x80, .emulate = spc_emulate_evpd_80 },
	{ .page = 0x83, .emulate = spc_emulate_evpd_83 },
	{ .page = 0x86, .emulate = spc_emulate_evpd_86 },
	{ .page = 0xb0, .emulate = spc_emulate_evpd_b0 },
	{ .page = 0xb1, .emulate = spc_emulate_evpd_b1 },
	{ .page = 0xb2, .emulate = spc_emulate_evpd_b2 },
	{ .page = 0xb3, .emulate = spc_emulate_evpd_b3 },
};

/* supported vital product data pages */
static sense_reason_t
spc_emulate_evpd_00(struct se_cmd *cmd, unsigned char *buf)
{
	int p;

	/*
	 * Only report the INQUIRY EVPD=1 pages after a valid NAA
	 * Registered Extended LUN WWN has been set via ConfigFS
	 * during device creation/restart.
	 */
	if (cmd->se_dev->dev_flags & DF_EMULATED_VPD_UNIT_SERIAL) {
		buf[3] = ARRAY_SIZE(evpd_handlers);
		for (p = 0; p < ARRAY_SIZE(evpd_handlers); ++p)
			buf[p + 4] = evpd_handlers[p].page;
	}

	return 0;
}

static sense_reason_t
spc_emulate_inquiry(struct se_cmd *cmd)
{
	struct se_device *dev = cmd->se_dev;
	unsigned char *rbuf;
	unsigned char *cdb = cmd->t_task_cdb;
	unsigned char *buf;
	sense_reason_t ret;
	int p;
	int len = 0;

	buf = kzalloc(SE_INQUIRY_BUF, GFP_KERNEL);
	if (!buf) {
		pr_err("Unable to allocate response buffer for INQUIRY\n");
		return TCM_LOGICAL_UNIT_COMMUNICATION_FAILURE;
	}

	buf[0] = dev->transport->get_device_type(dev);

	if (!(cdb[1] & 0x1)) {
		if (cdb[2]) {
			pr_err("INQUIRY with EVPD==0 but PAGE CODE=%02x\n",
			       cdb[2]);
			ret = TCM_INVALID_CDB_FIELD;
			goto out;
		}

		ret = spc_emulate_inquiry_std(cmd, buf);
		len = buf[4] + 5;
		goto out;
	}

	for (p = 0; p < ARRAY_SIZE(evpd_handlers); ++p) {
		if (cdb[2] == evpd_handlers[p].page) {
			buf[1] = cdb[2];
			ret = evpd_handlers[p].emulate(cmd, buf);
			len = get_unaligned_be16(&buf[2]) + 4;
			goto out;
		}
	}

	pr_debug("Unknown VPD Code: 0x%02x\n", cdb[2]);
	ret = TCM_INVALID_CDB_FIELD;

out:
	rbuf = transport_kmap_data_sg(cmd);
	if (rbuf) {
		memcpy(rbuf, buf, min_t(u32, SE_INQUIRY_BUF, cmd->data_length));
		transport_kunmap_data_sg(cmd);
	}
	kfree(buf);

	if (!ret)
		target_complete_cmd_with_length(cmd, SAM_STAT_GOOD, len);
	return ret;
}

static int spc_modesense_rwrecovery(struct se_cmd *cmd, u8 pc, u8 *p)
{
	p[0] = 0x01;
	p[1] = 0x0a;

	/* No changeable values for now */
	if (pc == 1)
		goto out;

out:
	return 12;
}

static int spc_modesense_control(struct se_cmd *cmd, u8 pc, u8 *p)
{
	struct se_device *dev = cmd->se_dev;
	struct se_session *sess = cmd->se_sess;

	p[0] = 0x0a;
	p[1] = 0x0a;

	/* No changeable values for now */
	if (pc == 1)
		goto out;

	/* GLTSD: No implicit save of log parameters */
	p[2] = (1 << 1);
	if (target_sense_desc_format(dev))
		/* D_SENSE: Descriptor format sense data for 64bit sectors */
		p[2] |= (1 << 2);

	/*
	 * From spc4r23, 7.4.7 Control mode page
	 *
	 * The QUEUE ALGORITHM MODIFIER field (see table 368) specifies
	 * restrictions on the algorithm used for reordering commands
	 * having the SIMPLE task attribute (see SAM-4).
	 *
	 *                    Table 368 -- QUEUE ALGORITHM MODIFIER field
	 *                         Code      Description
	 *                          0h       Restricted reordering
	 *                          1h       Unrestricted reordering allowed
	 *                          2h to 7h    Reserved
	 *                          8h to Fh    Vendor specific
	 *
	 * A value of zero in the QUEUE ALGORITHM MODIFIER field specifies that
	 * the device server shall order the processing sequence of commands
	 * having the SIMPLE task attribute such that data integrity is maintained
	 * for that I_T nexus (i.e., if the transmission of new SCSI transport protocol
	 * requests is halted at any time, the final value of all data observable
	 * on the medium shall be the same as if all the commands had been processed
	 * with the ORDERED task attribute).
	 *
	 * A value of one in the QUEUE ALGORITHM MODIFIER field specifies that the
	 * device server may reorder the processing sequence of commands having the
	 * SIMPLE task attribute in any manner. Any data integrity exposures related to
	 * command sequence order shall be explicitly handled by the application client
	 * through the selection of appropriate ommands and task attributes.
	 */
	p[3] = (dev->dev_attrib.emulate_rest_reord == 1) ? 0x00 : 0x10;
	/*
	 * From spc4r17, section 7.4.6 Control mode Page
	 *
	 * Unit Attention interlocks control (UN_INTLCK_CTRL) to code 00b
	 *
	 * 00b: The logical unit shall clear any unit attention condition
	 * reported in the same I_T_L_Q nexus transaction as a CHECK CONDITION
	 * status and shall not establish a unit attention condition when a com-
	 * mand is completed with BUSY, TASK SET FULL, or RESERVATION CONFLICT
	 * status.
	 *
	 * 10b: The logical unit shall not clear any unit attention condition
	 * reported in the same I_T_L_Q nexus transaction as a CHECK CONDITION
	 * status and shall not establish a unit attention condition when
	 * a command is completed with BUSY, TASK SET FULL, or RESERVATION
	 * CONFLICT status.
	 *
	 * 11b a The logical unit shall not clear any unit attention condition
	 * reported in the same I_T_L_Q nexus transaction as a CHECK CONDITION
	 * status and shall establish a unit attention condition for the
	 * initiator port associated with the I_T nexus on which the BUSY,
	 * TASK SET FULL, or RESERVATION CONFLICT status is being returned.
	 * Depending on the status, the additional sense code shall be set to
	 * PREVIOUS BUSY STATUS, PREVIOUS TASK SET FULL STATUS, or PREVIOUS
	 * RESERVATION CONFLICT STATUS. Until it is cleared by a REQUEST SENSE
	 * command, a unit attention condition shall be established only once
	 * for a BUSY, TASK SET FULL, or RESERVATION CONFLICT status regardless
	 * to the number of commands completed with one of those status codes.
	 */
	switch (dev->dev_attrib.emulate_ua_intlck_ctrl) {
	case TARGET_UA_INTLCK_CTRL_ESTABLISH_UA:
		p[4] = 0x30;
		break;
	case TARGET_UA_INTLCK_CTRL_NO_CLEAR:
		p[4] = 0x20;
		break;
	default:	/* TARGET_UA_INTLCK_CTRL_CLEAR */
		p[4] = 0x00;
		break;
	}
	/*
	 * From spc4r17, section 7.4.6 Control mode Page
	 *
	 * Task Aborted Status (TAS) bit set to zero.
	 *
	 * A task aborted status (TAS) bit set to zero specifies that aborted
	 * tasks shall be terminated by the device server without any response
	 * to the application client. A TAS bit set to one specifies that tasks
	 * aborted by the actions of an I_T nexus other than the I_T nexus on
	 * which the command was received shall be completed with TASK ABORTED
	 * status (see SAM-4).
	 */
	p[5] = (dev->dev_attrib.emulate_tas) ? 0x40 : 0x00;
	/*
	 * From spc4r30, section 7.5.7 Control mode page
	 *
	 * Application Tag Owner (ATO) bit set to one.
	 *
	 * If the ATO bit is set to one the device server shall not modify the
	 * LOGICAL BLOCK APPLICATION TAG field and, depending on the protection
	 * type, shall not modify the contents of the LOGICAL BLOCK REFERENCE
	 * TAG field.
	 */
	if (sess->sup_prot_ops & (TARGET_PROT_DIN_PASS | TARGET_PROT_DOUT_PASS)) {
		if (dev->dev_attrib.pi_prot_type || sess->sess_prot_type)
			p[5] |= 0x80;
	}

	p[8] = 0xff;
	p[9] = 0xff;
	p[11] = 30;

out:
	return 12;
}

static int spc_modesense_caching(struct se_cmd *cmd, u8 pc, u8 *p)
{
	struct se_device *dev = cmd->se_dev;

	p[0] = 0x08;
	p[1] = 0x12;

	/* No changeable values for now */
	if (pc == 1)
		goto out;

	if (target_check_wce(dev))
		p[2] = 0x04; /* Write Cache Enable */
	p[12] = 0x20; /* Disabled Read Ahead */

out:
	return 20;
}

static int spc_modesense_informational_exceptions(struct se_cmd *cmd, u8 pc, unsigned char *p)
{
	p[0] = 0x1c;
	p[1] = 0x0a;

	/* No changeable values for now */
	if (pc == 1)
		goto out;

out:
	return 12;
}

static struct {
	uint8_t		page;
	uint8_t		subpage;
	int		(*emulate)(struct se_cmd *, u8, unsigned char *);
} modesense_handlers[] = {
	{ .page = 0x01, .subpage = 0x00, .emulate = spc_modesense_rwrecovery },
	{ .page = 0x08, .subpage = 0x00, .emulate = spc_modesense_caching },
	{ .page = 0x0a, .subpage = 0x00, .emulate = spc_modesense_control },
	{ .page = 0x1c, .subpage = 0x00, .emulate = spc_modesense_informational_exceptions },
};

static void spc_modesense_write_protect(unsigned char *buf, int type)
{
	/*
	 * I believe that the WP bit (bit 7) in the mode header is the same for
	 * all device types..
	 */
	switch (type) {
	case TYPE_DISK:
	case TYPE_TAPE:
	default:
		buf[0] |= 0x80; /* WP bit */
		break;
	}
}

static void spc_modesense_dpofua(unsigned char *buf, int type)
{
	switch (type) {
	case TYPE_DISK:
		buf[0] |= 0x10; /* DPOFUA bit */
		break;
	default:
		break;
	}
}

static int spc_modesense_blockdesc(unsigned char *buf, u64 blocks, u32 block_size)
{
	*buf++ = 8;
	put_unaligned_be32(min(blocks, 0xffffffffull), buf);
	buf += 4;
	put_unaligned_be32(block_size, buf);
	return 9;
}

static int spc_modesense_long_blockdesc(unsigned char *buf, u64 blocks, u32 block_size)
{
	if (blocks <= 0xffffffff)
		return spc_modesense_blockdesc(buf + 3, blocks, block_size) + 3;

	*buf++ = 1;		/* LONGLBA */
	buf += 2;
	*buf++ = 16;
	put_unaligned_be64(blocks, buf);
	buf += 12;
	put_unaligned_be32(block_size, buf);

	return 17;
}

static sense_reason_t spc_emulate_modesense(struct se_cmd *cmd)
{
	struct se_device *dev = cmd->se_dev;
	char *cdb = cmd->t_task_cdb;
	unsigned char buf[SE_MODE_PAGE_BUF], *rbuf;
	int type = dev->transport->get_device_type(dev);
	int ten = (cmd->t_task_cdb[0] == MODE_SENSE_10);
	bool dbd = !!(cdb[1] & 0x08);
	bool llba = ten ? !!(cdb[1] & 0x10) : false;
	u8 pc = cdb[2] >> 6;
	u8 page = cdb[2] & 0x3f;
	u8 subpage = cdb[3];
	int length = 0;
	int ret;
	int i;

	memset(buf, 0, SE_MODE_PAGE_BUF);

	/*
	 * Skip over MODE DATA LENGTH + MEDIUM TYPE fields to byte 3 for
	 * MODE_SENSE_10 and byte 2 for MODE_SENSE (6).
	 */
	length = ten ? 3 : 2;

	/* DEVICE-SPECIFIC PARAMETER */
	if (cmd->se_lun->lun_access_ro || target_lun_is_rdonly(cmd))
		spc_modesense_write_protect(&buf[length], type);

	/*
	 * SBC only allows us to enable FUA and DPO together.  Fortunately
	 * DPO is explicitly specified as a hint, so a noop is a perfectly
	 * valid implementation.
	 */
	if (target_check_fua(dev))
		spc_modesense_dpofua(&buf[length], type);

	++length;

	/* BLOCK DESCRIPTOR */

	/*
	 * For now we only include a block descriptor for disk (SBC)
	 * devices; other command sets use a slightly different format.
	 */
	if (!dbd && type == TYPE_DISK) {
		u64 blocks = dev->transport->get_blocks(dev);
		u32 block_size = dev->dev_attrib.block_size;

		if (ten) {
			if (llba) {
				length += spc_modesense_long_blockdesc(&buf[length],
								       blocks, block_size);
			} else {
				length += 3;
				length += spc_modesense_blockdesc(&buf[length],
								  blocks, block_size);
			}
		} else {
			length += spc_modesense_blockdesc(&buf[length], blocks,
							  block_size);
		}
	} else {
		if (ten)
			length += 4;
		else
			length += 1;
	}

	if (page == 0x3f) {
		if (subpage != 0x00 && subpage != 0xff) {
			pr_warn("MODE_SENSE: Invalid subpage code: 0x%02x\n", subpage);
			return TCM_INVALID_CDB_FIELD;
		}

		for (i = 0; i < ARRAY_SIZE(modesense_handlers); ++i) {
			/*
			 * Tricky way to say all subpage 00h for
			 * subpage==0, all subpages for subpage==0xff
			 * (and we just checked above that those are
			 * the only two possibilities).
			 */
			if ((modesense_handlers[i].subpage & ~subpage) == 0) {
				ret = modesense_handlers[i].emulate(cmd, pc, &buf[length]);
				if (!ten && length + ret >= 255)
					break;
				length += ret;
			}
		}

		goto set_length;
	}

	for (i = 0; i < ARRAY_SIZE(modesense_handlers); ++i)
		if (modesense_handlers[i].page == page &&
		    modesense_handlers[i].subpage == subpage) {
			length += modesense_handlers[i].emulate(cmd, pc, &buf[length]);
			goto set_length;
		}

	/*
	 * We don't intend to implement:
	 *  - obsolete page 03h "format parameters" (checked by Solaris)
	 */
	if (page != 0x03)
		pr_err("MODE SENSE: unimplemented page/subpage: 0x%02x/0x%02x\n",
		       page, subpage);

	return TCM_UNKNOWN_MODE_PAGE;

set_length:
	if (ten)
		put_unaligned_be16(length - 2, buf);
	else
		buf[0] = length - 1;

	rbuf = transport_kmap_data_sg(cmd);
	if (rbuf) {
		memcpy(rbuf, buf, min_t(u32, SE_MODE_PAGE_BUF, cmd->data_length));
		transport_kunmap_data_sg(cmd);
	}

	target_complete_cmd_with_length(cmd, SAM_STAT_GOOD, length);
	return 0;
}

static sense_reason_t spc_emulate_modeselect(struct se_cmd *cmd)
{
	char *cdb = cmd->t_task_cdb;
	bool ten = cdb[0] == MODE_SELECT_10;
	int off = ten ? 8 : 4;
	bool pf = !!(cdb[1] & 0x10);
	u8 page, subpage;
	unsigned char *buf;
	unsigned char tbuf[SE_MODE_PAGE_BUF];
	int length;
	sense_reason_t ret = 0;
	int i;

	if (!cmd->data_length) {
		target_complete_cmd(cmd, SAM_STAT_GOOD);
		return 0;
	}

	if (cmd->data_length < off + 2)
		return TCM_PARAMETER_LIST_LENGTH_ERROR;

	buf = transport_kmap_data_sg(cmd);
	if (!buf)
		return TCM_LOGICAL_UNIT_COMMUNICATION_FAILURE;

	if (!pf) {
		ret = TCM_INVALID_CDB_FIELD;
		goto out;
	}

	page = buf[off] & 0x3f;
	subpage = buf[off] & 0x40 ? buf[off + 1] : 0;

	for (i = 0; i < ARRAY_SIZE(modesense_handlers); ++i)
		if (modesense_handlers[i].page == page &&
		    modesense_handlers[i].subpage == subpage) {
			memset(tbuf, 0, SE_MODE_PAGE_BUF);
			length = modesense_handlers[i].emulate(cmd, 0, tbuf);
			goto check_contents;
		}

	ret = TCM_UNKNOWN_MODE_PAGE;
	goto out;

check_contents:
	if (cmd->data_length < off + length) {
		ret = TCM_PARAMETER_LIST_LENGTH_ERROR;
		goto out;
	}

	if (memcmp(buf + off, tbuf, length))
		ret = TCM_INVALID_PARAMETER_LIST;

out:
	transport_kunmap_data_sg(cmd);

	if (!ret)
		target_complete_cmd(cmd, SAM_STAT_GOOD);
	return ret;
}

static sense_reason_t spc_emulate_request_sense(struct se_cmd *cmd)
{
	unsigned char *cdb = cmd->t_task_cdb;
	unsigned char *rbuf;
	u8 ua_asc = 0, ua_ascq = 0;
	unsigned char buf[SE_SENSE_BUF];
	bool desc_format = target_sense_desc_format(cmd->se_dev);

	memset(buf, 0, SE_SENSE_BUF);

	if (cdb[1] & 0x01) {
		pr_err("REQUEST_SENSE description emulation not"
			" supported\n");
		return TCM_INVALID_CDB_FIELD;
	}

	rbuf = transport_kmap_data_sg(cmd);
	if (!rbuf)
		return TCM_LOGICAL_UNIT_COMMUNICATION_FAILURE;

	if (!core_scsi3_ua_clear_for_request_sense(cmd, &ua_asc, &ua_ascq))
		scsi_build_sense_buffer(desc_format, buf, UNIT_ATTENTION,
					ua_asc, ua_ascq);
	else
		scsi_build_sense_buffer(desc_format, buf, NO_SENSE, 0x0, 0x0);

	memcpy(rbuf, buf, min_t(u32, sizeof(buf), cmd->data_length));
	transport_kunmap_data_sg(cmd);

	target_complete_cmd(cmd, SAM_STAT_GOOD);
	return 0;
}

sense_reason_t spc_emulate_report_luns(struct se_cmd *cmd)
{
	struct se_dev_entry *deve;
	struct se_session *sess = cmd->se_sess;
	struct se_node_acl *nacl;
	struct scsi_lun slun;
	unsigned char *buf;
	u32 lun_count = 0, offset = 8;
	__be32 len;

	buf = transport_kmap_data_sg(cmd);
	if (cmd->data_length && !buf)
		return TCM_LOGICAL_UNIT_COMMUNICATION_FAILURE;

	/*
	 * If no struct se_session pointer is present, this struct se_cmd is
	 * coming via a target_core_mod PASSTHROUGH op, and not through
	 * a $FABRIC_MOD.  In that case, report LUN=0 only.
	 */
	if (!sess)
		goto done;

	nacl = sess->se_node_acl;

	rcu_read_lock();
	hlist_for_each_entry_rcu(deve, &nacl->lun_entry_hlist, link) {
		/*
		 * We determine the correct LUN LIST LENGTH even once we
		 * have reached the initial allocation length.
		 * See SPC2-R20 7.19.
		 */
		lun_count++;
		if (offset >= cmd->data_length)
			continue;

		int_to_scsilun(deve->mapped_lun, &slun);
		memcpy(buf + offset, &slun,
		       min(8u, cmd->data_length - offset));
		offset += 8;
	}
	rcu_read_unlock();

	/*
	 * See SPC3 r07, page 159.
	 */
done:
	/*
	 * If no LUNs are accessible, report virtual LUN 0.
	 */
	if (lun_count == 0) {
		int_to_scsilun(0, &slun);
		if (cmd->data_length > 8)
			memcpy(buf + offset, &slun,
			       min(8u, cmd->data_length - offset));
		lun_count = 1;
	}

	if (buf) {
		len = cpu_to_be32(lun_count * 8);
		memcpy(buf, &len, min_t(int, sizeof len, cmd->data_length));
		transport_kunmap_data_sg(cmd);
	}

	target_complete_cmd_with_length(cmd, SAM_STAT_GOOD, 8 + lun_count * 8);
	return 0;
}
EXPORT_SYMBOL(spc_emulate_report_luns);

static sense_reason_t
spc_emulate_testunitready(struct se_cmd *cmd)
{
	target_complete_cmd(cmd, SAM_STAT_GOOD);
	return 0;
}

static void set_dpofua_usage_bits(u8 *usage_bits, struct se_device *dev)
{
	if (!target_check_fua(dev))
		usage_bits[1] &= ~0x18;
	else
		usage_bits[1] |= 0x18;
}

static void set_dpofua_usage_bits32(u8 *usage_bits, struct se_device *dev)
{
	if (!target_check_fua(dev))
		usage_bits[10] &= ~0x18;
	else
		usage_bits[10] |= 0x18;
}

static struct target_opcode_descriptor tcm_opcode_read6 = {
	.support = SCSI_SUPPORT_FULL,
	.opcode = READ_6,
	.cdb_size = 6,
	.usage_bits = {READ_6, 0x1f, 0xff, 0xff,
		       0xff, SCSI_CONTROL_MASK},
};

static struct target_opcode_descriptor tcm_opcode_read10 = {
	.support = SCSI_SUPPORT_FULL,
	.opcode = READ_10,
	.cdb_size = 10,
	.usage_bits = {READ_10, 0xf8, 0xff, 0xff,
		       0xff, 0xff, SCSI_GROUP_NUMBER_MASK, 0xff,
		       0xff, SCSI_CONTROL_MASK},
	.update_usage_bits = set_dpofua_usage_bits,
};

static struct target_opcode_descriptor tcm_opcode_read12 = {
	.support = SCSI_SUPPORT_FULL,
	.opcode = READ_12,
	.cdb_size = 12,
	.usage_bits = {READ_12, 0xf8, 0xff, 0xff,
		       0xff, 0xff, 0xff, 0xff,
		       0xff, 0xff, SCSI_GROUP_NUMBER_MASK, SCSI_CONTROL_MASK},
	.update_usage_bits = set_dpofua_usage_bits,
};

static struct target_opcode_descriptor tcm_opcode_read16 = {
	.support = SCSI_SUPPORT_FULL,
	.opcode = READ_16,
	.cdb_size = 16,
	.usage_bits = {READ_16, 0xf8, 0xff, 0xff,
		       0xff, 0xff, 0xff, 0xff,
		       0xff, 0xff, 0xff, 0xff,
		       0xff, 0xff, SCSI_GROUP_NUMBER_MASK, SCSI_CONTROL_MASK},
	.update_usage_bits = set_dpofua_usage_bits,
};

static struct target_opcode_descriptor tcm_opcode_write6 = {
	.support = SCSI_SUPPORT_FULL,
	.opcode = WRITE_6,
	.cdb_size = 6,
	.usage_bits = {WRITE_6, 0x1f, 0xff, 0xff,
		       0xff, SCSI_CONTROL_MASK},
};

static struct target_opcode_descriptor tcm_opcode_write10 = {
	.support = SCSI_SUPPORT_FULL,
	.opcode = WRITE_10,
	.cdb_size = 10,
	.usage_bits = {WRITE_10, 0xf8, 0xff, 0xff,
		       0xff, 0xff, SCSI_GROUP_NUMBER_MASK, 0xff,
		       0xff, SCSI_CONTROL_MASK},
	.update_usage_bits = set_dpofua_usage_bits,
};

static struct target_opcode_descriptor tcm_opcode_write_verify10 = {
	.support = SCSI_SUPPORT_FULL,
	.opcode = WRITE_VERIFY,
	.cdb_size = 10,
	.usage_bits = {WRITE_VERIFY, 0xf0, 0xff, 0xff,
		       0xff, 0xff, SCSI_GROUP_NUMBER_MASK, 0xff,
		       0xff, SCSI_CONTROL_MASK},
	.update_usage_bits = set_dpofua_usage_bits,
};

static struct target_opcode_descriptor tcm_opcode_write12 = {
	.support = SCSI_SUPPORT_FULL,
	.opcode = WRITE_12,
	.cdb_size = 12,
	.usage_bits = {WRITE_12, 0xf8, 0xff, 0xff,
		       0xff, 0xff, 0xff, 0xff,
		       0xff, 0xff, SCSI_GROUP_NUMBER_MASK, SCSI_CONTROL_MASK},
	.update_usage_bits = set_dpofua_usage_bits,
};

static struct target_opcode_descriptor tcm_opcode_write16 = {
	.support = SCSI_SUPPORT_FULL,
	.opcode = WRITE_16,
	.cdb_size = 16,
	.usage_bits = {WRITE_16, 0xf8, 0xff, 0xff,
		       0xff, 0xff, 0xff, 0xff,
		       0xff, 0xff, 0xff, 0xff,
		       0xff, 0xff, SCSI_GROUP_NUMBER_MASK, SCSI_CONTROL_MASK},
	.update_usage_bits = set_dpofua_usage_bits,
};

static struct target_opcode_descriptor tcm_opcode_write_verify16 = {
	.support = SCSI_SUPPORT_FULL,
	.opcode = WRITE_VERIFY_16,
	.cdb_size = 16,
	.usage_bits = {WRITE_VERIFY_16, 0xf0, 0xff, 0xff,
		       0xff, 0xff, 0xff, 0xff,
		       0xff, 0xff, 0xff, 0xff,
		       0xff, 0xff, SCSI_GROUP_NUMBER_MASK, SCSI_CONTROL_MASK},
	.update_usage_bits = set_dpofua_usage_bits,
};

static bool tcm_is_ws_enabled(struct target_opcode_descriptor *descr,
			      struct se_cmd *cmd)
{
	struct exec_cmd_ops *ops = cmd->protocol_data;
	struct se_device *dev = cmd->se_dev;

	return (dev->dev_attrib.emulate_tpws && !!ops->execute_unmap) ||
	       !!ops->execute_write_same;
}

static struct target_opcode_descriptor tcm_opcode_write_same32 = {
	.support = SCSI_SUPPORT_FULL,
	.serv_action_valid = 1,
	.opcode = VARIABLE_LENGTH_CMD,
	.service_action = WRITE_SAME_32,
	.cdb_size = 32,
	.usage_bits = {VARIABLE_LENGTH_CMD, SCSI_CONTROL_MASK, 0x00, 0x00,
		       0x00, 0x00, SCSI_GROUP_NUMBER_MASK, 0x18,
		       0x00, WRITE_SAME_32, 0xe8, 0x00,
		       0xff, 0xff, 0xff, 0xff,
		       0xff, 0xff, 0xff, 0xff,
		       0x00, 0x00, 0x00, 0x00,
		       0x00, 0x00, 0x00, 0x00,
		       0xff, 0xff, 0xff, 0xff},
	.enabled = tcm_is_ws_enabled,
	.update_usage_bits = set_dpofua_usage_bits32,
};

static bool tcm_is_caw_enabled(struct target_opcode_descriptor *descr,
			       struct se_cmd *cmd)
{
	struct se_device *dev = cmd->se_dev;

	return dev->dev_attrib.emulate_caw;
}

static struct target_opcode_descriptor tcm_opcode_compare_write = {
	.support = SCSI_SUPPORT_FULL,
	.opcode = COMPARE_AND_WRITE,
	.cdb_size = 16,
	.usage_bits = {COMPARE_AND_WRITE, 0x18, 0xff, 0xff,
		       0xff, 0xff, 0xff, 0xff,
		       0xff, 0xff, 0x00, 0x00,
		       0x00, 0xff, SCSI_GROUP_NUMBER_MASK, SCSI_CONTROL_MASK},
	.enabled = tcm_is_caw_enabled,
	.update_usage_bits = set_dpofua_usage_bits,
};

static struct target_opcode_descriptor tcm_opcode_read_capacity = {
	.support = SCSI_SUPPORT_FULL,
	.opcode = READ_CAPACITY,
	.cdb_size = 10,
	.usage_bits = {READ_CAPACITY, 0x00, 0xff, 0xff,
		       0xff, 0xff, 0x00, 0x00,
		       0x01, SCSI_CONTROL_MASK},
};

static struct target_opcode_descriptor tcm_opcode_read_capacity16 = {
	.support = SCSI_SUPPORT_FULL,
	.serv_action_valid = 1,
	.opcode = SERVICE_ACTION_IN_16,
	.service_action = SAI_READ_CAPACITY_16,
	.cdb_size = 16,
	.usage_bits = {SERVICE_ACTION_IN_16, SAI_READ_CAPACITY_16, 0x00, 0x00,
		       0x00, 0x00, 0x00, 0x00,
		       0x00, 0x00, 0xff, 0xff,
		       0xff, 0xff, 0x00, SCSI_CONTROL_MASK},
};

static bool tcm_is_rep_ref_enabled(struct target_opcode_descriptor *descr,
				   struct se_cmd *cmd)
{
	struct se_device *dev = cmd->se_dev;

	spin_lock(&dev->t10_alua.lba_map_lock);
	if (list_empty(&dev->t10_alua.lba_map_list)) {
		spin_unlock(&dev->t10_alua.lba_map_lock);
		return false;
	}
	spin_unlock(&dev->t10_alua.lba_map_lock);
	return true;
}

static struct target_opcode_descriptor tcm_opcode_read_report_refferals = {
	.support = SCSI_SUPPORT_FULL,
	.serv_action_valid = 1,
	.opcode = SERVICE_ACTION_IN_16,
	.service_action = SAI_REPORT_REFERRALS,
	.cdb_size = 16,
	.usage_bits = {SERVICE_ACTION_IN_16, SAI_REPORT_REFERRALS, 0x00, 0x00,
		       0x00, 0x00, 0x00, 0x00,
		       0x00, 0x00, 0xff, 0xff,
		       0xff, 0xff, 0x00, SCSI_CONTROL_MASK},
	.enabled = tcm_is_rep_ref_enabled,
};

static struct target_opcode_descriptor tcm_opcode_sync_cache = {
	.support = SCSI_SUPPORT_FULL,
	.opcode = SYNCHRONIZE_CACHE,
	.cdb_size = 10,
	.usage_bits = {SYNCHRONIZE_CACHE, 0x02, 0xff, 0xff,
		       0xff, 0xff, SCSI_GROUP_NUMBER_MASK, 0xff,
		       0xff, SCSI_CONTROL_MASK},
};

static struct target_opcode_descriptor tcm_opcode_sync_cache16 = {
	.support = SCSI_SUPPORT_FULL,
	.opcode = SYNCHRONIZE_CACHE_16,
	.cdb_size = 16,
	.usage_bits = {SYNCHRONIZE_CACHE_16, 0x02, 0xff, 0xff,
		       0xff, 0xff, 0xff, 0xff,
		       0xff, 0xff, 0xff, 0xff,
		       0xff, 0xff, SCSI_GROUP_NUMBER_MASK, SCSI_CONTROL_MASK},
};

static bool tcm_is_unmap_enabled(struct target_opcode_descriptor *descr,
				 struct se_cmd *cmd)
{
	struct exec_cmd_ops *ops = cmd->protocol_data;
	struct se_device *dev = cmd->se_dev;

	return ops->execute_unmap && dev->dev_attrib.emulate_tpu;
}

static struct target_opcode_descriptor tcm_opcode_unmap = {
	.support = SCSI_SUPPORT_FULL,
	.opcode = UNMAP,
	.cdb_size = 10,
	.usage_bits = {UNMAP, 0x00, 0x00, 0x00,
		       0x00, 0x00, SCSI_GROUP_NUMBER_MASK, 0xff,
		       0xff, SCSI_CONTROL_MASK},
	.enabled = tcm_is_unmap_enabled,
};

static struct target_opcode_descriptor tcm_opcode_write_same = {
	.support = SCSI_SUPPORT_FULL,
	.opcode = WRITE_SAME,
	.cdb_size = 10,
	.usage_bits = {WRITE_SAME, 0xe8, 0xff, 0xff,
		       0xff, 0xff, SCSI_GROUP_NUMBER_MASK, 0xff,
		       0xff, SCSI_CONTROL_MASK},
	.enabled = tcm_is_ws_enabled,
};

static struct target_opcode_descriptor tcm_opcode_write_same16 = {
	.support = SCSI_SUPPORT_FULL,
	.opcode = WRITE_SAME_16,
	.cdb_size = 16,
	.usage_bits = {WRITE_SAME_16, 0xe8, 0xff, 0xff,
		       0xff, 0xff, 0xff, 0xff,
		       0xff, 0xff, 0xff, 0xff,
		       0xff, 0xff, SCSI_GROUP_NUMBER_MASK, SCSI_CONTROL_MASK},
	.enabled = tcm_is_ws_enabled,
};

static struct target_opcode_descriptor tcm_opcode_verify = {
	.support = SCSI_SUPPORT_FULL,
	.opcode = VERIFY,
	.cdb_size = 10,
	.usage_bits = {VERIFY, 0x00, 0xff, 0xff,
		       0xff, 0xff, SCSI_GROUP_NUMBER_MASK, 0xff,
		       0xff, SCSI_CONTROL_MASK},
};

static struct target_opcode_descriptor tcm_opcode_verify16 = {
	.support = SCSI_SUPPORT_FULL,
	.opcode = VERIFY_16,
	.cdb_size = 16,
	.usage_bits = {VERIFY_16, 0x00, 0xff, 0xff,
		       0xff, 0xff, 0xff, 0xff,
		       0xff, 0xff, 0xff, 0xff,
		       0xff, 0xff, SCSI_GROUP_NUMBER_MASK, SCSI_CONTROL_MASK},
};

static struct target_opcode_descriptor tcm_opcode_start_stop = {
	.support = SCSI_SUPPORT_FULL,
	.opcode = START_STOP,
	.cdb_size = 6,
	.usage_bits = {START_STOP, 0x01, 0x00, 0x00,
		       0x01, SCSI_CONTROL_MASK},
};

static struct target_opcode_descriptor tcm_opcode_mode_select = {
	.support = SCSI_SUPPORT_FULL,
	.opcode = MODE_SELECT,
	.cdb_size = 6,
	.usage_bits = {MODE_SELECT, 0x10, 0x00, 0x00,
		       0xff, SCSI_CONTROL_MASK},
};

static struct target_opcode_descriptor tcm_opcode_mode_select10 = {
	.support = SCSI_SUPPORT_FULL,
	.opcode = MODE_SELECT_10,
	.cdb_size = 10,
	.usage_bits = {MODE_SELECT_10, 0x10, 0x00, 0x00,
		       0x00, 0x00, 0x00, 0xff,
		       0xff, SCSI_CONTROL_MASK},
};

static struct target_opcode_descriptor tcm_opcode_mode_sense = {
	.support = SCSI_SUPPORT_FULL,
	.opcode = MODE_SENSE,
	.cdb_size = 6,
	.usage_bits = {MODE_SENSE, 0x08, 0xff, 0xff,
		       0xff, SCSI_CONTROL_MASK},
};

static struct target_opcode_descriptor tcm_opcode_mode_sense10 = {
	.support = SCSI_SUPPORT_FULL,
	.opcode = MODE_SENSE_10,
	.cdb_size = 10,
	.usage_bits = {MODE_SENSE_10, 0x18, 0xff, 0xff,
		       0x00, 0x00, 0x00, 0xff,
		       0xff, SCSI_CONTROL_MASK},
};

static struct target_opcode_descriptor tcm_opcode_pri_read_keys = {
	.support = SCSI_SUPPORT_FULL,
	.serv_action_valid = 1,
	.opcode = PERSISTENT_RESERVE_IN,
	.service_action = PRI_READ_KEYS,
	.cdb_size = 10,
	.usage_bits = {PERSISTENT_RESERVE_IN, PRI_READ_KEYS, 0x00, 0x00,
		       0x00, 0x00, 0x00, 0xff,
		       0xff, SCSI_CONTROL_MASK},
};

static struct target_opcode_descriptor tcm_opcode_pri_read_resrv = {
	.support = SCSI_SUPPORT_FULL,
	.serv_action_valid = 1,
	.opcode = PERSISTENT_RESERVE_IN,
	.service_action = PRI_READ_RESERVATION,
	.cdb_size = 10,
	.usage_bits = {PERSISTENT_RESERVE_IN, PRI_READ_RESERVATION, 0x00, 0x00,
		       0x00, 0x00, 0x00, 0xff,
		       0xff, SCSI_CONTROL_MASK},
};

static bool tcm_is_pr_enabled(struct target_opcode_descriptor *descr,
			      struct se_cmd *cmd)
{
	struct se_device *dev = cmd->se_dev;

	if (!dev->dev_attrib.emulate_pr)
		return false;

	if (!(dev->transport_flags & TRANSPORT_FLAG_PASSTHROUGH_PGR))
		return true;

	switch (descr->opcode) {
	case RESERVE:
	case RESERVE_10:
	case RELEASE:
	case RELEASE_10:
		/*
		 * The pr_ops which are used by the backend modules don't
		 * support these commands.
		 */
		return false;
	case PERSISTENT_RESERVE_OUT:
		switch (descr->service_action) {
		case PRO_REGISTER_AND_MOVE:
		case PRO_REPLACE_LOST_RESERVATION:
			/*
			 * The backend modules don't have access to ports and
			 * I_T nexuses so they can't handle these type of
			 * requests.
			 */
			return false;
		}
		break;
	case PERSISTENT_RESERVE_IN:
		if (descr->service_action == PRI_READ_FULL_STATUS)
			return false;
		break;
	}

	return true;
}

static struct target_opcode_descriptor tcm_opcode_pri_read_caps = {
	.support = SCSI_SUPPORT_FULL,
	.serv_action_valid = 1,
	.opcode = PERSISTENT_RESERVE_IN,
	.service_action = PRI_REPORT_CAPABILITIES,
	.cdb_size = 10,
	.usage_bits = {PERSISTENT_RESERVE_IN, PRI_REPORT_CAPABILITIES, 0x00, 0x00,
		       0x00, 0x00, 0x00, 0xff,
		       0xff, SCSI_CONTROL_MASK},
	.enabled = tcm_is_pr_enabled,
};

static struct target_opcode_descriptor tcm_opcode_pri_read_full_status = {
	.support = SCSI_SUPPORT_FULL,
	.serv_action_valid = 1,
	.opcode = PERSISTENT_RESERVE_IN,
	.service_action = PRI_READ_FULL_STATUS,
	.cdb_size = 10,
	.usage_bits = {PERSISTENT_RESERVE_IN, PRI_READ_FULL_STATUS, 0x00, 0x00,
		       0x00, 0x00, 0x00, 0xff,
		       0xff, SCSI_CONTROL_MASK},
	.enabled = tcm_is_pr_enabled,
};

static struct target_opcode_descriptor tcm_opcode_pro_register = {
	.support = SCSI_SUPPORT_FULL,
	.serv_action_valid = 1,
	.opcode = PERSISTENT_RESERVE_OUT,
	.service_action = PRO_REGISTER,
	.cdb_size = 10,
	.usage_bits = {PERSISTENT_RESERVE_OUT, PRO_REGISTER, 0xff, 0x00,
		       0x00, 0xff, 0xff, 0xff,
		       0xff, SCSI_CONTROL_MASK},
	.enabled = tcm_is_pr_enabled,
};

static struct target_opcode_descriptor tcm_opcode_pro_reserve = {
	.support = SCSI_SUPPORT_FULL,
	.serv_action_valid = 1,
	.opcode = PERSISTENT_RESERVE_OUT,
	.service_action = PRO_RESERVE,
	.cdb_size = 10,
	.usage_bits = {PERSISTENT_RESERVE_OUT, PRO_RESERVE, 0xff, 0x00,
		       0x00, 0xff, 0xff, 0xff,
		       0xff, SCSI_CONTROL_MASK},
	.enabled = tcm_is_pr_enabled,
};

static struct target_opcode_descriptor tcm_opcode_pro_release = {
	.support = SCSI_SUPPORT_FULL,
	.serv_action_valid = 1,
	.opcode = PERSISTENT_RESERVE_OUT,
	.service_action = PRO_RELEASE,
	.cdb_size = 10,
	.usage_bits = {PERSISTENT_RESERVE_OUT, PRO_RELEASE, 0xff, 0x00,
		       0x00, 0xff, 0xff, 0xff,
		       0xff, SCSI_CONTROL_MASK},
	.enabled = tcm_is_pr_enabled,
};

static struct target_opcode_descriptor tcm_opcode_pro_clear = {
	.support = SCSI_SUPPORT_FULL,
	.serv_action_valid = 1,
	.opcode = PERSISTENT_RESERVE_OUT,
	.service_action = PRO_CLEAR,
	.cdb_size = 10,
	.usage_bits = {PERSISTENT_RESERVE_OUT, PRO_CLEAR, 0xff, 0x00,
		       0x00, 0xff, 0xff, 0xff,
		       0xff, SCSI_CONTROL_MASK},
	.enabled = tcm_is_pr_enabled,
};

static struct target_opcode_descriptor tcm_opcode_pro_preempt = {
	.support = SCSI_SUPPORT_FULL,
	.serv_action_valid = 1,
	.opcode = PERSISTENT_RESERVE_OUT,
	.service_action = PRO_PREEMPT,
	.cdb_size = 10,
	.usage_bits = {PERSISTENT_RESERVE_OUT, PRO_PREEMPT, 0xff, 0x00,
		       0x00, 0xff, 0xff, 0xff,
		       0xff, SCSI_CONTROL_MASK},
	.enabled = tcm_is_pr_enabled,
};

static struct target_opcode_descriptor tcm_opcode_pro_preempt_abort = {
	.support = SCSI_SUPPORT_FULL,
	.serv_action_valid = 1,
	.opcode = PERSISTENT_RESERVE_OUT,
	.service_action = PRO_PREEMPT_AND_ABORT,
	.cdb_size = 10,
	.usage_bits = {PERSISTENT_RESERVE_OUT, PRO_PREEMPT_AND_ABORT, 0xff, 0x00,
		       0x00, 0xff, 0xff, 0xff,
		       0xff, SCSI_CONTROL_MASK},
	.enabled = tcm_is_pr_enabled,
};

static struct target_opcode_descriptor tcm_opcode_pro_reg_ign_exist = {
	.support = SCSI_SUPPORT_FULL,
	.serv_action_valid = 1,
	.opcode = PERSISTENT_RESERVE_OUT,
	.service_action = PRO_REGISTER_AND_IGNORE_EXISTING_KEY,
	.cdb_size = 10,
	.usage_bits = {
		PERSISTENT_RESERVE_OUT, PRO_REGISTER_AND_IGNORE_EXISTING_KEY,
		0xff, 0x00,
		0x00, 0xff, 0xff, 0xff,
		0xff, SCSI_CONTROL_MASK},
	.enabled = tcm_is_pr_enabled,
};

static struct target_opcode_descriptor tcm_opcode_pro_register_move = {
	.support = SCSI_SUPPORT_FULL,
	.serv_action_valid = 1,
	.opcode = PERSISTENT_RESERVE_OUT,
	.service_action = PRO_REGISTER_AND_MOVE,
	.cdb_size = 10,
	.usage_bits = {PERSISTENT_RESERVE_OUT, PRO_REGISTER_AND_MOVE, 0xff, 0x00,
		       0x00, 0xff, 0xff, 0xff,
		       0xff, SCSI_CONTROL_MASK},
	.enabled = tcm_is_pr_enabled,
};

static struct target_opcode_descriptor tcm_opcode_release = {
	.support = SCSI_SUPPORT_FULL,
	.opcode = RELEASE,
	.cdb_size = 6,
	.usage_bits = {RELEASE, 0x00, 0x00, 0x00,
		       0x00, SCSI_CONTROL_MASK},
	.enabled = tcm_is_pr_enabled,
};

static struct target_opcode_descriptor tcm_opcode_release10 = {
	.support = SCSI_SUPPORT_FULL,
	.opcode = RELEASE_10,
	.cdb_size = 10,
	.usage_bits = {RELEASE_10, 0x00, 0x00, 0x00,
		       0x00, 0x00, 0x00, 0xff,
		       0xff, SCSI_CONTROL_MASK},
	.enabled = tcm_is_pr_enabled,
};

static struct target_opcode_descriptor tcm_opcode_reserve = {
	.support = SCSI_SUPPORT_FULL,
	.opcode = RESERVE,
	.cdb_size = 6,
	.usage_bits = {RESERVE, 0x00, 0x00, 0x00,
		       0x00, SCSI_CONTROL_MASK},
	.enabled = tcm_is_pr_enabled,
};

static struct target_opcode_descriptor tcm_opcode_reserve10 = {
	.support = SCSI_SUPPORT_FULL,
	.opcode = RESERVE_10,
	.cdb_size = 10,
	.usage_bits = {RESERVE_10, 0x00, 0x00, 0x00,
		       0x00, 0x00, 0x00, 0xff,
		       0xff, SCSI_CONTROL_MASK},
	.enabled = tcm_is_pr_enabled,
};

static struct target_opcode_descriptor tcm_opcode_request_sense = {
	.support = SCSI_SUPPORT_FULL,
	.opcode = REQUEST_SENSE,
	.cdb_size = 6,
	.usage_bits = {REQUEST_SENSE, 0x00, 0x00, 0x00,
		       0xff, SCSI_CONTROL_MASK},
};

static struct target_opcode_descriptor tcm_opcode_inquiry = {
	.support = SCSI_SUPPORT_FULL,
	.opcode = INQUIRY,
	.cdb_size = 6,
	.usage_bits = {INQUIRY, 0x01, 0xff, 0xff,
		       0xff, SCSI_CONTROL_MASK},
};

static bool tcm_is_3pc_enabled(struct target_opcode_descriptor *descr,
			       struct se_cmd *cmd)
{
	struct se_device *dev = cmd->se_dev;

	return dev->dev_attrib.emulate_3pc;
}

static struct target_opcode_descriptor tcm_opcode_extended_copy_lid1 = {
	.support = SCSI_SUPPORT_FULL,
	.serv_action_valid = 1,
	.opcode = EXTENDED_COPY,
	.cdb_size = 16,
	.usage_bits = {EXTENDED_COPY, 0x00, 0x00, 0x00,
		       0x00, 0x00, 0x00, 0x00,
		       0x00, 0x00, 0xff, 0xff,
		       0xff, 0xff, 0x00, SCSI_CONTROL_MASK},
	.enabled = tcm_is_3pc_enabled,
};

static struct target_opcode_descriptor tcm_opcode_rcv_copy_res_op_params = {
	.support = SCSI_SUPPORT_FULL,
	.serv_action_valid = 1,
	.opcode = RECEIVE_COPY_RESULTS,
	.service_action = RCR_SA_OPERATING_PARAMETERS,
	.cdb_size = 16,
	.usage_bits = {RECEIVE_COPY_RESULTS, RCR_SA_OPERATING_PARAMETERS,
		       0x00, 0x00,
		       0x00, 0x00, 0x00, 0x00,
		       0x00, 0x00, 0xff, 0xff,
		       0xff, 0xff, 0x00, SCSI_CONTROL_MASK},
	.enabled = tcm_is_3pc_enabled,
};

static struct target_opcode_descriptor tcm_opcode_report_luns = {
	.support = SCSI_SUPPORT_FULL,
	.opcode = REPORT_LUNS,
	.cdb_size = 12,
	.usage_bits = {REPORT_LUNS, 0x00, 0xff, 0x00,
		       0x00, 0x00, 0xff, 0xff,
		       0xff, 0xff, 0x00, SCSI_CONTROL_MASK},
};

static struct target_opcode_descriptor tcm_opcode_test_unit_ready = {
	.support = SCSI_SUPPORT_FULL,
	.opcode = TEST_UNIT_READY,
	.cdb_size = 6,
	.usage_bits = {TEST_UNIT_READY, 0x00, 0x00, 0x00,
		       0x00, SCSI_CONTROL_MASK},
};

static struct target_opcode_descriptor tcm_opcode_report_target_pgs = {
	.support = SCSI_SUPPORT_FULL,
	.serv_action_valid = 1,
	.opcode = MAINTENANCE_IN,
	.service_action = MI_REPORT_TARGET_PGS,
	.cdb_size = 12,
	.usage_bits = {MAINTENANCE_IN, 0xE0 | MI_REPORT_TARGET_PGS, 0x00, 0x00,
		       0x00, 0x00, 0xff, 0xff,
		       0xff, 0xff, 0x00, SCSI_CONTROL_MASK},
};

static bool spc_rsoc_enabled(struct target_opcode_descriptor *descr,
			     struct se_cmd *cmd)
{
	struct se_device *dev = cmd->se_dev;

	return dev->dev_attrib.emulate_rsoc;
}

static struct target_opcode_descriptor tcm_opcode_report_supp_opcodes = {
	.support = SCSI_SUPPORT_FULL,
	.serv_action_valid = 1,
	.opcode = MAINTENANCE_IN,
	.service_action = MI_REPORT_SUPPORTED_OPERATION_CODES,
	.cdb_size = 12,
	.usage_bits = {MAINTENANCE_IN, MI_REPORT_SUPPORTED_OPERATION_CODES,
		       0x87, 0xff,
		       0xff, 0xff, 0xff, 0xff,
		       0xff, 0xff, 0x00, SCSI_CONTROL_MASK},
	.enabled = spc_rsoc_enabled,
};

static bool tcm_is_set_tpg_enabled(struct target_opcode_descriptor *descr,
				   struct se_cmd *cmd)
{
	struct t10_alua_tg_pt_gp *l_tg_pt_gp;
	struct se_lun *l_lun = cmd->se_lun;

	rcu_read_lock();
	l_tg_pt_gp = rcu_dereference(l_lun->lun_tg_pt_gp);
	if (!l_tg_pt_gp) {
		rcu_read_unlock();
		return false;
	}
	if (!(l_tg_pt_gp->tg_pt_gp_alua_access_type & TPGS_EXPLICIT_ALUA)) {
		rcu_read_unlock();
		return false;
	}
	rcu_read_unlock();

	return true;
}

static struct target_opcode_descriptor tcm_opcode_set_tpg = {
	.support = SCSI_SUPPORT_FULL,
	.serv_action_valid = 1,
	.opcode = MAINTENANCE_OUT,
	.service_action = MO_SET_TARGET_PGS,
	.cdb_size = 12,
	.usage_bits = {MAINTENANCE_OUT, MO_SET_TARGET_PGS, 0x00, 0x00,
		       0x00, 0x00, 0xff, 0xff,
		       0xff, 0xff, 0x00, SCSI_CONTROL_MASK},
	.enabled = tcm_is_set_tpg_enabled,
};

static struct target_opcode_descriptor *tcm_supported_opcodes[] = {
	&tcm_opcode_read6,
	&tcm_opcode_read10,
	&tcm_opcode_read12,
	&tcm_opcode_read16,
	&tcm_opcode_write6,
	&tcm_opcode_write10,
	&tcm_opcode_write_verify10,
	&tcm_opcode_write12,
	&tcm_opcode_write16,
	&tcm_opcode_write_verify16,
	&tcm_opcode_write_same32,
	&tcm_opcode_compare_write,
	&tcm_opcode_read_capacity,
	&tcm_opcode_read_capacity16,
	&tcm_opcode_read_report_refferals,
	&tcm_opcode_sync_cache,
	&tcm_opcode_sync_cache16,
	&tcm_opcode_unmap,
	&tcm_opcode_write_same,
	&tcm_opcode_write_same16,
	&tcm_opcode_verify,
	&tcm_opcode_verify16,
	&tcm_opcode_start_stop,
	&tcm_opcode_mode_select,
	&tcm_opcode_mode_select10,
	&tcm_opcode_mode_sense,
	&tcm_opcode_mode_sense10,
	&tcm_opcode_pri_read_keys,
	&tcm_opcode_pri_read_resrv,
	&tcm_opcode_pri_read_caps,
	&tcm_opcode_pri_read_full_status,
	&tcm_opcode_pro_register,
	&tcm_opcode_pro_reserve,
	&tcm_opcode_pro_release,
	&tcm_opcode_pro_clear,
	&tcm_opcode_pro_preempt,
	&tcm_opcode_pro_preempt_abort,
	&tcm_opcode_pro_reg_ign_exist,
	&tcm_opcode_pro_register_move,
	&tcm_opcode_release,
	&tcm_opcode_release10,
	&tcm_opcode_reserve,
	&tcm_opcode_reserve10,
	&tcm_opcode_request_sense,
	&tcm_opcode_inquiry,
	&tcm_opcode_extended_copy_lid1,
	&tcm_opcode_rcv_copy_res_op_params,
	&tcm_opcode_report_luns,
	&tcm_opcode_test_unit_ready,
	&tcm_opcode_report_target_pgs,
	&tcm_opcode_report_supp_opcodes,
	&tcm_opcode_set_tpg,
};

static int
spc_rsoc_encode_command_timeouts_descriptor(unsigned char *buf, u8 ctdp,
				struct target_opcode_descriptor *descr)
{
	if (!ctdp)
		return 0;

	put_unaligned_be16(0xa, buf);
	buf[3] = descr->specific_timeout;
	put_unaligned_be32(descr->nominal_timeout, &buf[4]);
	put_unaligned_be32(descr->recommended_timeout, &buf[8]);

	return 12;
}

static int
spc_rsoc_encode_command_descriptor(unsigned char *buf, u8 ctdp,
				   struct target_opcode_descriptor *descr)
{
	int td_size = 0;

	buf[0] = descr->opcode;

	put_unaligned_be16(descr->service_action, &buf[2]);

	buf[5] = (ctdp << 1) | descr->serv_action_valid;
	put_unaligned_be16(descr->cdb_size, &buf[6]);

	td_size = spc_rsoc_encode_command_timeouts_descriptor(&buf[8], ctdp,
							      descr);

	return 8 + td_size;
}

static int
spc_rsoc_encode_one_command_descriptor(unsigned char *buf, u8 ctdp,
				       struct target_opcode_descriptor *descr,
				       struct se_device *dev)
{
	int td_size = 0;

	if (!descr) {
		buf[1] = (ctdp << 7) | SCSI_SUPPORT_NOT_SUPPORTED;
		return 2;
	}

	buf[1] = (ctdp << 7) | SCSI_SUPPORT_FULL;
	put_unaligned_be16(descr->cdb_size, &buf[2]);
	memcpy(&buf[4], descr->usage_bits, descr->cdb_size);
	if (descr->update_usage_bits)
		descr->update_usage_bits(&buf[4], dev);

	td_size = spc_rsoc_encode_command_timeouts_descriptor(
			&buf[4 + descr->cdb_size], ctdp, descr);

	return 4 + descr->cdb_size + td_size;
}

static sense_reason_t
spc_rsoc_get_descr(struct se_cmd *cmd, struct target_opcode_descriptor **opcode)
{
	struct target_opcode_descriptor *descr;
	struct se_session *sess = cmd->se_sess;
	unsigned char *cdb = cmd->t_task_cdb;
	u8 opts = cdb[2] & 0x3;
	u8 requested_opcode;
	u16 requested_sa;
	int i;

	requested_opcode = cdb[3];
	requested_sa = ((u16)cdb[4]) << 8 | cdb[5];
	*opcode = NULL;

	if (opts > 3) {
		pr_debug("TARGET_CORE[%s]: Invalid REPORT SUPPORTED OPERATION CODES"
			" with unsupported REPORTING OPTIONS %#x for 0x%08llx from %s\n",
			cmd->se_tfo->fabric_name, opts,
			cmd->se_lun->unpacked_lun,
			sess->se_node_acl->initiatorname);
		return TCM_INVALID_CDB_FIELD;
	}

	for (i = 0; i < ARRAY_SIZE(tcm_supported_opcodes); i++) {
		descr = tcm_supported_opcodes[i];
		if (descr->opcode != requested_opcode)
			continue;

		switch (opts) {
		case 0x1:
			/*
			 * If the REQUESTED OPERATION CODE field specifies an
			 * operation code for which the device server implements
			 * service actions, then the device server shall
			 * terminate the command with CHECK CONDITION status,
			 * with the sense key set to ILLEGAL REQUEST, and the
			 * additional sense code set to INVALID FIELD IN CDB
			 */
			if (descr->serv_action_valid)
				return TCM_INVALID_CDB_FIELD;

			if (!descr->enabled || descr->enabled(descr, cmd)) {
				*opcode = descr;
				return TCM_NO_SENSE;
			}
			break;
		case 0x2:
			/*
			 * If the REQUESTED OPERATION CODE field specifies an
			 * operation code for which the device server does not
			 * implement service actions, then the device server
			 * shall terminate the command with CHECK CONDITION
			 * status, with the sense key set to ILLEGAL REQUEST,
			 * and the additional sense code set to INVALID FIELD IN CDB.
			 */
			if (descr->serv_action_valid &&
			    descr->service_action == requested_sa) {
				if (!descr->enabled || descr->enabled(descr,
								      cmd)) {
					*opcode = descr;
					return TCM_NO_SENSE;
				}
			} else if (!descr->serv_action_valid)
				return TCM_INVALID_CDB_FIELD;
			break;
		case 0x3:
			/*
			 * The command support data for the operation code and
			 * service action a specified in the REQUESTED OPERATION
			 * CODE field and REQUESTED SERVICE ACTION field shall
			 * be returned in the one_command parameter data format.
			 */
			if (descr->service_action == requested_sa)
				if (!descr->enabled || descr->enabled(descr,
								      cmd)) {
					*opcode = descr;
					return TCM_NO_SENSE;
				}
			break;
		}
	}

	return TCM_NO_SENSE;
}

static sense_reason_t
spc_emulate_report_supp_op_codes(struct se_cmd *cmd)
{
	int descr_num = ARRAY_SIZE(tcm_supported_opcodes);
	struct target_opcode_descriptor *descr = NULL;
	unsigned char *cdb = cmd->t_task_cdb;
	u8 rctd = (cdb[2] >> 7) & 0x1;
	unsigned char *buf = NULL;
	int response_length = 0;
	u8 opts = cdb[2] & 0x3;
	unsigned char *rbuf;
	sense_reason_t ret = 0;
	int i;

	if (!cmd->se_dev->dev_attrib.emulate_rsoc)
		return TCM_UNSUPPORTED_SCSI_OPCODE;

	rbuf = transport_kmap_data_sg(cmd);
	if (cmd->data_length && !rbuf) {
		ret = TCM_LOGICAL_UNIT_COMMUNICATION_FAILURE;
		goto out;
	}

	if (opts == 0)
		response_length = 4 + (8 + rctd * 12) * descr_num;
	else {
		ret = spc_rsoc_get_descr(cmd, &descr);
		if (ret)
			goto out;

		if (descr)
			response_length = 4 + descr->cdb_size + rctd * 12;
		else
			response_length = 2;
	}

	buf = kzalloc(response_length, GFP_KERNEL);
	if (!buf) {
		ret = TCM_LOGICAL_UNIT_COMMUNICATION_FAILURE;
		goto out;
	}
	response_length = 0;

	if (opts == 0) {
		response_length += 4;

		for (i = 0; i < ARRAY_SIZE(tcm_supported_opcodes); i++) {
			descr = tcm_supported_opcodes[i];
			if (descr->enabled && !descr->enabled(descr, cmd))
				continue;

			response_length += spc_rsoc_encode_command_descriptor(
					&buf[response_length], rctd, descr);
		}
		put_unaligned_be32(response_length - 4, buf);
	} else {
		response_length = spc_rsoc_encode_one_command_descriptor(
				&buf[response_length], rctd, descr,
				cmd->se_dev);
	}

	memcpy(rbuf, buf, min_t(u32, response_length, cmd->data_length));
out:
	kfree(buf);
	transport_kunmap_data_sg(cmd);

	if (!ret)
		target_complete_cmd_with_length(cmd, SAM_STAT_GOOD, response_length);
	return ret;
}

sense_reason_t
spc_parse_cdb(struct se_cmd *cmd, unsigned int *size)
{
	struct se_device *dev = cmd->se_dev;
	unsigned char *cdb = cmd->t_task_cdb;

	switch (cdb[0]) {
	case RESERVE:
	case RESERVE_10:
	case RELEASE:
	case RELEASE_10:
		if (!dev->dev_attrib.emulate_pr)
			return TCM_UNSUPPORTED_SCSI_OPCODE;

		if (dev->transport_flags & TRANSPORT_FLAG_PASSTHROUGH_PGR)
			return TCM_UNSUPPORTED_SCSI_OPCODE;
		break;
	case PERSISTENT_RESERVE_IN:
	case PERSISTENT_RESERVE_OUT:
		if (!dev->dev_attrib.emulate_pr)
			return TCM_UNSUPPORTED_SCSI_OPCODE;
		break;
	}

	switch (cdb[0]) {
	case MODE_SELECT:
		*size = cdb[4];
		cmd->execute_cmd = spc_emulate_modeselect;
		break;
	case MODE_SELECT_10:
		*size = get_unaligned_be16(&cdb[7]);
		cmd->execute_cmd = spc_emulate_modeselect;
		break;
	case MODE_SENSE:
		*size = cdb[4];
		cmd->execute_cmd = spc_emulate_modesense;
		break;
	case MODE_SENSE_10:
		*size = get_unaligned_be16(&cdb[7]);
		cmd->execute_cmd = spc_emulate_modesense;
		break;
	case LOG_SELECT:
	case LOG_SENSE:
		*size = get_unaligned_be16(&cdb[7]);
		break;
	case PERSISTENT_RESERVE_IN:
		*size = get_unaligned_be16(&cdb[7]);
		cmd->execute_cmd = target_scsi3_emulate_pr_in;
		break;
	case PERSISTENT_RESERVE_OUT:
		*size = get_unaligned_be32(&cdb[5]);
		cmd->execute_cmd = target_scsi3_emulate_pr_out;
		break;
	case RELEASE:
	case RELEASE_10:
		if (cdb[0] == RELEASE_10)
			*size = get_unaligned_be16(&cdb[7]);
		else
			*size = cmd->data_length;

		cmd->execute_cmd = target_scsi2_reservation_release;
		break;
	case RESERVE:
	case RESERVE_10:
		/*
		 * The SPC-2 RESERVE does not contain a size in the SCSI CDB.
		 * Assume the passthrough or $FABRIC_MOD will tell us about it.
		 */
		if (cdb[0] == RESERVE_10)
			*size = get_unaligned_be16(&cdb[7]);
		else
			*size = cmd->data_length;

		cmd->execute_cmd = target_scsi2_reservation_reserve;
		break;
	case REQUEST_SENSE:
		*size = cdb[4];
		cmd->execute_cmd = spc_emulate_request_sense;
		break;
	case INQUIRY:
		*size = get_unaligned_be16(&cdb[3]);

		/*
		 * Do implicit HEAD_OF_QUEUE processing for INQUIRY.
		 * See spc4r17 section 5.3
		 */
		cmd->sam_task_attr = TCM_HEAD_TAG;
		cmd->execute_cmd = spc_emulate_inquiry;
		break;
	case SECURITY_PROTOCOL_IN:
	case SECURITY_PROTOCOL_OUT:
		*size = get_unaligned_be32(&cdb[6]);
		break;
	case EXTENDED_COPY:
		*size = get_unaligned_be32(&cdb[10]);
		cmd->execute_cmd = target_do_xcopy;
		break;
	case RECEIVE_COPY_RESULTS:
		*size = get_unaligned_be32(&cdb[10]);
		cmd->execute_cmd = target_do_receive_copy_results;
		break;
	case READ_ATTRIBUTE:
	case WRITE_ATTRIBUTE:
		*size = get_unaligned_be32(&cdb[10]);
		break;
	case RECEIVE_DIAGNOSTIC:
	case SEND_DIAGNOSTIC:
		*size = get_unaligned_be16(&cdb[3]);
		break;
	case WRITE_BUFFER:
		*size = get_unaligned_be24(&cdb[6]);
		break;
	case REPORT_LUNS:
		cmd->execute_cmd = spc_emulate_report_luns;
		*size = get_unaligned_be32(&cdb[6]);
		/*
		 * Do implicit HEAD_OF_QUEUE processing for REPORT_LUNS
		 * See spc4r17 section 5.3
		 */
		cmd->sam_task_attr = TCM_HEAD_TAG;
		break;
	case TEST_UNIT_READY:
		cmd->execute_cmd = spc_emulate_testunitready;
		*size = 0;
		break;
	case MAINTENANCE_IN:
		if (dev->transport->get_device_type(dev) != TYPE_ROM) {
			/*
			 * MAINTENANCE_IN from SCC-2
			 * Check for emulated MI_REPORT_TARGET_PGS
			 */
			if ((cdb[1] & 0x1f) == MI_REPORT_TARGET_PGS) {
				cmd->execute_cmd =
					target_emulate_report_target_port_groups;
			}
			if ((cdb[1] & 0x1f) ==
			    MI_REPORT_SUPPORTED_OPERATION_CODES)
				cmd->execute_cmd =
					spc_emulate_report_supp_op_codes;
			*size = get_unaligned_be32(&cdb[6]);
		} else {
			/*
			 * GPCMD_SEND_KEY from multi media commands
			 */
			*size = get_unaligned_be16(&cdb[8]);
		}
		break;
	case MAINTENANCE_OUT:
		if (dev->transport->get_device_type(dev) != TYPE_ROM) {
			/*
			 * MAINTENANCE_OUT from SCC-2
			 * Check for emulated MO_SET_TARGET_PGS.
			 */
			if (cdb[1] == MO_SET_TARGET_PGS) {
				cmd->execute_cmd =
					target_emulate_set_target_port_groups;
			}
			*size = get_unaligned_be32(&cdb[6]);
		} else {
			/*
			 * GPCMD_SEND_KEY from multi media commands
			 */
			*size = get_unaligned_be16(&cdb[8]);
		}
		break;
	default:
		return TCM_UNSUPPORTED_SCSI_OPCODE;
	}

	return 0;
}
EXPORT_SYMBOL(spc_parse_cdb);
