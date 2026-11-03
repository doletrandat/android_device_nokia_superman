// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2011-2017, The Linux Foundation. */
enum slimbus_mode_enum_type_v01 {
	/* To force a 32 bit signed enum. Do not change or use*/
	SLIMBUS_MODE_ENUM_TYPE_MIN_ENUM_VAL_V01 = INT_MIN,
	SLIMBUS_MODE_SATELLITE_V01 = 1,
	SLIMBUS_MODE_MASTER_V01 = 2,
	SLIMBUS_MODE_ENUM_TYPE_MAX_ENUM_VAL_V01 = INT_MAX,
};

enum slimbus_pm_enum_type_v01 {
	/* To force a 32 bit signed enum. Do not change or use*/
	SLIMBUS_PM_ENUM_TYPE_MIN_ENUM_VAL_V01 = INT_MIN,
	SLIMBUS_PM_INACTIVE_V01 = 1,
	SLIMBUS_PM_ACTIVE_V01 = 2,
	SLIMBUS_PM_ENUM_TYPE_MAX_ENUM_VAL_V01 = INT_MAX,
};

enum slimbus_resp_enum_type_v01 {
	SLIMBUS_RESP_ENUM_TYPE_MIN_VAL_V01 = INT_MIN,
	SLIMBUS_RESP_SYNCHRONOUS_V01 = 1,
	SLIMBUS_RESP_ENUM_TYPE_MAX_VAL_V01 = INT_MAX,
};

struct slimbus_select_inst_req_msg_v01 {
	uint32_t instance;
	uint8_t mode_valid;
	enum slimbus_mode_enum_type_v01 mode;
};

struct slimbus_select_inst_resp_msg_v01 {
	struct qmi_response_type_v01 resp;
};

struct slimbus_power_req_msg_v01 {
	enum slimbus_pm_enum_type_v01 pm_req;
	uint8_t resp_type_valid;
	enum slimbus_resp_enum_type_v01 resp_type;
};

struct slimbus_power_resp_msg_v01 {
	struct qmi_response_type_v01 resp;
};

static const struct qmi_elem_info slimbus_select_inst_req_msg_v01_ei[] = {
	{
		.data_type  = QMI_UNSIGNED_4_BYTE,
		.elem_len   = 1,
		.elem_size  = sizeof(uint32_t),
		.array_type = NO_ARRAY,
		.tlv_type   = 0x01,
		.offset     = offsetof(struct slimbus_select_inst_req_msg_v01,
				       instance),
		.ei_array   = NULL,
	},
	{
		.data_type  = QMI_OPT_FLAG,
		.elem_len   = 1,
		.elem_size  = sizeof(uint8_t),
		.array_type = NO_ARRAY,
		.tlv_type   = 0x10,
		.offset     = offsetof(struct slimbus_select_inst_req_msg_v01,
				       mode_valid),
		.ei_array   = NULL,
	},
	{
		.data_type  = QMI_UNSIGNED_4_BYTE,
		.elem_len   = 1,
		.elem_size  = sizeof(enum slimbus_mode_enum_type_v01),
		.array_type = NO_ARRAY,
		.tlv_type   = 0x10,
		.offset     = offsetof(struct slimbus_select_inst_req_msg_v01,
				       mode),
		.ei_array   = NULL,
	},
	{
		.data_type  = QMI_EOTI,
		.elem_len   = 0,
		.elem_size  = 0,
		.array_type = NO_ARRAY,
		.tlv_type   = 0x00,
		.offset     = 0,
		.ei_array   = NULL,
	},
};

static const struct qmi_elem_info slimbus_select_inst_resp_msg_v01_ei[] = {
	{
		.data_type  = QMI_STRUCT,
		.elem_len   = 1,
		.elem_size  = sizeof(struct qmi_response_type_v01),
		.array_type = NO_ARRAY,
		.tlv_type   = 0x02,
		.offset     = offsetof(struct slimbus_select_inst_resp_msg_v01,
				       resp),
		.ei_array   = qmi_response_type_v01_ei,
	},
	{
		.data_type  = QMI_EOTI,
		.elem_len   = 0,
		.elem_size  = 0,
		.array_type = NO_ARRAY,
		.tlv_type   = 0x00,
		.offset     = 0,
		.ei_array   = NULL,
	},
};

static const struct qmi_elem_info slimbus_power_req_msg_v01_ei[] = {
	{
		.data_type  = QMI_UNSIGNED_4_BYTE,
		.elem_len   = 1,
		.elem_size  = sizeof(enum slimbus_pm_enum_type_v01),
		.array_type = NO_ARRAY,
		.tlv_type   = 0x01,
		.offset     = offsetof(struct slimbus_power_req_msg_v01,
				       pm_req),
		.ei_array   = NULL,
	},
	{
		.data_type  = QMI_OPT_FLAG,
		.elem_len   = 1,
		.elem_size  = sizeof(uint8_t),
		.array_type = NO_ARRAY,
		.tlv_type   = 0x10,
		.offset     = offsetof(struct slimbus_power_req_msg_v01,
				       resp_type_valid),
	},
	{
		.data_type  = QMI_SIGNED_4_BYTE_ENUM,
		.elem_len   = 1,
		.elem_size  = sizeof(enum slimbus_resp_enum_type_v01),
		.array_type = NO_ARRAY,
		.tlv_type   = 0x10,
		.offset     = offsetof(struct slimbus_power_req_msg_v01,
				       resp_type),
	},
	{
		.data_type  = QMI_EOTI,
		.elem_len   = 0,
		.elem_size  = 0,
		.array_type = NO_ARRAY,
		.tlv_type   = 0x00,
		.offset     = 0,
		.ei_array   = NULL,
	},
};

static const struct qmi_elem_info slimbus_power_resp_msg_v01_ei[] = {
	{
		.data_type  = QMI_STRUCT,
		.elem_len   = 1,
		.elem_size  = sizeof(struct qmi_response_type_v01),
		.array_type = NO_ARRAY,
		.tlv_type   = 0x02,
		.offset     = offsetof(struct slimbus_power_resp_msg_v01, resp),
		.ei_array   = qmi_response_type_v01_ei,
	},
	{
		.data_type  = QMI_EOTI,
		.elem_len   = 0,
		.elem_size  = 0,
		.array_type = NO_ARRAY,
		.tlv_type   = 0x00,
		.offset     = 0,
		.ei_array   = NULL,
	},
};

