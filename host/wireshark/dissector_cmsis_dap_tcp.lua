-- SPDX-License-Identifier: GPL-2.0-or-later
--
-- Wireshark packet dissector for the cmsis_dap_tcp protocol.
-- Based on OpenOCD src/jtag/drivers/cmsis_dap_tcp.c (commit fcff4b7, 2025-09-01)
-- and CMSIS-DAP spec v2.1.2.
--
-- Brian Kuschak <bkuschak@gmail.com>
-- Co-authored by Claude (claude.ai).
--
-- Place this file into the Wireshark plugins directory:
--   Mac/Linux:   ~/.config/wireshark/plugins/
--   Windows:     %APPDATA%\Wireshark\plugins\

local cmsis_dap_tcp_proto = Proto("cmsis_dap_tcp", "CMSIS-DAP-TCP")

-- Header constants (matches cmsis_dap_tcp.c)
local DAP_PKT_HDR_SIGNATURE  = 0x00504144   -- "DAP\0" in LE
local HDR_PKT_TYPE_REQUEST   = 0x01
local HDR_PKT_TYPE_RESPONSE  = 0x02
local HEADER_SIZE            = 8

-- Enumerated packet types
local hdr_pkt_type_enum = {
    [HDR_PKT_TYPE_REQUEST]  = "Request",
    [HDR_PKT_TYPE_RESPONSE] = "Response",
}

-- Full DAP command ID table (from DAP.h)
local dap_id_enum = {
    [0x00] = "DAP Info",
    [0x01] = "DAP Host Status",
    [0x02] = "DAP Connect",
    [0x03] = "DAP Disconnect",
    [0x04] = "DAP Transfer Configure",
    [0x05] = "DAP Transfer",
    [0x06] = "DAP Transfer Block",
    [0x07] = "DAP Transfer Abort",
    [0x08] = "DAP Write Abort",
    [0x09] = "DAP Delay",
    [0x0A] = "DAP Reset Target",
    [0x10] = "DAP SWJ Pins",
    [0x11] = "DAP SWJ Clock",
    [0x12] = "DAP SWJ Sequence",
    [0x13] = "DAP SWD Configure",
    [0x14] = "DAP JTAG Sequence",
    [0x15] = "DAP JTAG Configure",
    [0x16] = "DAP JTAG IDCODE",
    [0x17] = "DAP SWO Transport",
    [0x18] = "DAP SWO Mode",
    [0x19] = "DAP SWO Baudrate",
    [0x1A] = "DAP SWO Control",
    [0x1B] = "DAP SWO Status",
    [0x1C] = "DAP SWO Data",
    [0x1D] = "DAP SWD Sequence",
    [0x1E] = "DAP SWO Extended Status",
    [0x1F] = "DAP UART Transport",
    [0x20] = "DAP UART Configure",
    [0x21] = "DAP UART Transfer",
    [0x22] = "DAP UART Control",
    [0x23] = "DAP UART Status",
    [0x7E] = "DAP Queue Commands",
    [0x7F] = "DAP Execute Commands",
    [0xFF] = "DAP Invalid",
}

-- DAP Info (0x00) Info ID values (from DAP.h)
local dap_info_id_enum = {
    [0x01] = "Vendor Name",
    [0x02] = "Product Name",
    [0x03] = "Serial Number",
    [0x04] = "DAP Protocol Version",
    [0x05] = "Target Device Vendor",
    [0x06] = "Target Device Name",
    [0x07] = "Target Board Vendor",
    [0x08] = "Target Board Name",
    [0x09] = "Product Firmware Version",
    [0xF0] = "Capabilities",
    [0xF1] = "Timestamp Clock (Hz)",
    [0xFB] = "UART Rx Buffer Size",
    [0xFC] = "UART Tx Buffer Size",
    [0xFD] = "SWO Trace Buffer Size",
    [0xFE] = "Max Packet Count",
    [0xFF] = "Max Packet Size",
}

-- DAP_HostStatus (0x01) type byte
local dap_hs_type_enum = {
    [0x00] = "Debugger Connected LED",
    [0x01] = "Target Running LED",
}

-- DAP_HostStatus (0x01) value byte (bit 0 only)
local dap_hs_value_enum = {
    [0x00] = "Off",
    [0x01] = "On",
}

-- DAP_Connect (0x02) port values (request and response share the same encoding,
-- except 0 means Autodetect in the request and Disabled/failed in the response)
local dap_port_req_enum = {
    [0x00] = "Autodetect",
    [0x01] = "SWD",
    [0x02] = "JTAG",
}
local dap_port_resp_enum = {
    [0x00] = "Disabled (connection failed)",
    [0x01] = "SWD",
    [0x02] = "JTAG",
}

-- DAP generic status (used in several command responses)
local dap_status_enum = {
    [0x00] = "OK",
    [0xFF] = "ERROR",
}

-- DAP_SWD_Configure (0x13) turnaround clock period.
-- Bits[1:0] encode (cycles - 1), so 0=1 cycle, 1=2 cycles, etc.
local dap_swdc_turnaround_enum = {
    [0] = "1 clock cycle",
    [1] = "2 clock cycles",
    [2] = "3 clock cycles",
    [3] = "4 clock cycles",
}

-- Transfer request bit 0: APnDP
local dap_apndp_enum = {
    [0] = "Debug Port (DP)",
    [1] = "Access Port (AP)",
}

-- Transfer request bit 1: RnW
local dap_rnw_enum = {
    [0] = "Write",
    [1] = "Read",
}

-- Transfer request bits [3:2]: register address within DP or AP.
-- These two bits select one of four word-aligned registers (offsets 0x0, 0x4, 0x8, 0xC).
-- Wireshark right-shifts masked values, so the valuestring keys are 0-3.
local dap_reg_addr_enum = {
    [0] = "0x00",
    [1] = "0x04",
    [2] = "0x08",
    [3] = "0x0C",
}

-- Register names for A[3:2] indexed by addr (0-3), split by APnDP.
local dp_reg_names = {
    [0] = "DPIDR/ABORT - ID / Abort",
    [1] = "CTRL/STAT - Control/Status",
    [2] = "SELECT - AP and Bank Select",
    [3] = "RDBUFF - Read Buffer",
}
local ap_reg_names = {
    [0] = "CSW - Control/Status Word",
    [1] = "TAR - Transfer Address Register",
    [2] = "(reserved)",
    [3] = "DRW - Data Read/Write",
}

local function transfer_reg_name(req_byte)
    local apndp = bit.band(req_byte, 0x01)
    local addr  = bit.rshift(bit.band(req_byte, 0x0C), 2)
    local names = (apndp == 0) and dp_reg_names or ap_reg_names
    return names[addr] or "?"
end

-- Transfer response bitmask values (single-bit cases; combined values shown via bool sub-fields)
local dap_transfer_resp_enum = {
    [0x01] = "OK",
    [0x02] = "WAIT",
    [0x04] = "FAULT",
    [0x08] = "ERROR",
    [0x10] = "MISMATCH",
}

-- ---------------------------------------------------------------------------
-- ProtoField declarations
-- ---------------------------------------------------------------------------

-- Header
local f_hdr_signature = ProtoField.uint32("cmsis_dap_tcp.hdr_signature", "Signature",      base.HEX)
local f_hdr_length    = ProtoField.uint16("cmsis_dap_tcp.hdr_length",    "Payload length", base.DEC)
local f_hdr_pkt_type  = ProtoField.uint8( "cmsis_dap_tcp.hdr_pkt_type",  "Packet type",    base.HEX, hdr_pkt_type_enum)
local f_hdr_reserved  = ProtoField.uint8( "cmsis_dap_tcp.hdr_reserved",  "Reserved",       base.HEX)

-- DAP command ID (first byte of every payload)
local f_dap_id = ProtoField.uint8("cmsis_dap_tcp.dap_id", "DAP ID", base.HEX, dap_id_enum)

-- DAP_Info (0x00) request
local f_info_req_id = ProtoField.uint8("cmsis_dap_tcp.info.req_id", "Info ID", base.HEX, dap_info_id_enum)

-- DAP_Info (0x00) response
local f_info_resp_length = ProtoField.uint8( "cmsis_dap_tcp.info.resp_length", "Info Length", base.DEC)
local f_info_resp_str    = ProtoField.string("cmsis_dap_tcp.info.resp_str",    "Info Data")
local f_info_resp_u8     = ProtoField.uint8( "cmsis_dap_tcp.info.resp_u8",     "Info Data",   base.DEC)
local f_info_resp_u16    = ProtoField.uint16("cmsis_dap_tcp.info.resp_u16",    "Info Data",   base.DEC)
local f_info_resp_u32    = ProtoField.uint32("cmsis_dap_tcp.info.resp_u32",    "Info Data",   base.DEC)

-- DAP_Info Capabilities byte 0 (Info ID 0xF0)
local f_info_caps0            = ProtoField.uint8("cmsis_dap_tcp.info.caps0",            "Capabilities (byte 0)", base.HEX)
local f_info_caps0_swd        = ProtoField.bool( "cmsis_dap_tcp.info.caps0.swd",        "SWD supported",                  8, nil, 0x01)
local f_info_caps0_jtag       = ProtoField.bool( "cmsis_dap_tcp.info.caps0.jtag",       "JTAG supported",                 8, nil, 0x02)
local f_info_caps0_swo_uart   = ProtoField.bool( "cmsis_dap_tcp.info.caps0.swo_uart",   "SWO UART supported",             8, nil, 0x04)
local f_info_caps0_swo_man    = ProtoField.bool( "cmsis_dap_tcp.info.caps0.swo_man",    "SWO Manchester supported",       8, nil, 0x08)
local f_info_caps0_atomic     = ProtoField.bool( "cmsis_dap_tcp.info.caps0.atomic",     "Atomic Commands supported",      8, nil, 0x10)
local f_info_caps0_timer      = ProtoField.bool( "cmsis_dap_tcp.info.caps0.timer",      "Test Domain Timer supported",    8, nil, 0x20)
local f_info_caps0_swo_stream = ProtoField.bool( "cmsis_dap_tcp.info.caps0.swo_stream", "SWO Streaming Trace supported",  8, nil, 0x40)
local f_info_caps0_uart       = ProtoField.bool( "cmsis_dap_tcp.info.caps0.uart",       "UART Port supported",            8, nil, 0x80)

-- DAP_Info Capabilities byte 1 (v2.1+)
local f_info_caps1         = ProtoField.uint8("cmsis_dap_tcp.info.caps1",         "Capabilities (byte 1)", base.HEX)
local f_info_caps1_usb_com = ProtoField.bool( "cmsis_dap_tcp.info.caps1.usb_com", "USB COM Port supported", 8, nil, 0x01)

-- DAP_HostStatus (0x01) request / response
local f_hs_type   = ProtoField.uint8("cmsis_dap_tcp.hs.type",   "Type",   base.HEX, dap_hs_type_enum)
local f_hs_value  = ProtoField.uint8("cmsis_dap_tcp.hs.value",  "Value",  base.HEX, dap_hs_value_enum)
local f_hs_status = ProtoField.uint8("cmsis_dap_tcp.hs.status", "Status", base.HEX, dap_status_enum)

-- DAP_Connect (0x02) request / response
local f_conn_req_port  = ProtoField.uint8("cmsis_dap_tcp.conn.req_port",  "Port", base.HEX, dap_port_req_enum)
local f_conn_resp_port = ProtoField.uint8("cmsis_dap_tcp.conn.resp_port", "Port", base.HEX, dap_port_resp_enum)

-- DAP_TransferConfigure (0x04) request / response
local f_tc_idle_cycles = ProtoField.uint8( "cmsis_dap_tcp.tc.idle_cycles", "Idle Cycles", base.DEC)
local f_tc_wait_retry  = ProtoField.uint16("cmsis_dap_tcp.tc.wait_retry",  "Wait Retry",  base.DEC)
local f_tc_match_retry = ProtoField.uint16("cmsis_dap_tcp.tc.match_retry", "Match Retry", base.DEC)
local f_tc_status      = ProtoField.uint8( "cmsis_dap_tcp.tc.status",      "Status",      base.HEX, dap_status_enum)

-- DAP_Transfer (0x05) request
local f_t_index         = ProtoField.uint8( "cmsis_dap_tcp.t.index",           "DAP Index",        base.DEC)
local f_t_count         = ProtoField.uint8( "cmsis_dap_tcp.t.count",           "Transfer Count",   base.DEC)
local f_t_req           = ProtoField.uint8( "cmsis_dap_tcp.t.req",             "Request",          base.HEX)
local f_t_req_apndp     = ProtoField.uint8( "cmsis_dap_tcp.t.req.apndp",       "APnDP",            base.DEC, dap_apndp_enum,    0x01)
local f_t_req_rnw       = ProtoField.uint8( "cmsis_dap_tcp.t.req.rnw",         "RnW",              base.DEC, dap_rnw_enum,      0x02)
local f_t_req_addr      = ProtoField.uint8( "cmsis_dap_tcp.t.req.addr",        "Register Address", base.HEX, dap_reg_addr_enum, 0x0C)
local f_t_req_matchval  = ProtoField.bool(  "cmsis_dap_tcp.t.req.match_value", "MATCH_VALUE",      8, nil, 0x10)
local f_t_req_matchmask = ProtoField.bool(  "cmsis_dap_tcp.t.req.match_mask",  "MATCH_MASK",       8, nil, 0x20)
local f_t_req_timestamp = ProtoField.bool(  "cmsis_dap_tcp.t.req.timestamp",   "TIMESTAMP",        8, nil, 0x80)
local f_t_write_data    = ProtoField.uint32("cmsis_dap_tcp.t.write_data",      "Write Data",       base.HEX)
local f_t_match_val_data= ProtoField.uint32("cmsis_dap_tcp.t.match_val",       "Match Value",      base.HEX)
local f_t_match_msk_data= ProtoField.uint32("cmsis_dap_tcp.t.match_mask",      "Match Mask",       base.HEX)

-- DAP_Transfer (0x05) response
local f_t_resp_count    = ProtoField.uint8( "cmsis_dap_tcp.t.resp_count",           "Transfer Count",    base.DEC)
local f_t_resp_status   = ProtoField.uint8( "cmsis_dap_tcp.t.resp_status",          "Transfer Response", base.HEX, dap_transfer_resp_enum)
local f_t_resp_ok       = ProtoField.bool(  "cmsis_dap_tcp.t.resp_status.ok",       "OK",       8, nil, 0x01)
local f_t_resp_wait     = ProtoField.bool(  "cmsis_dap_tcp.t.resp_status.wait",     "WAIT",     8, nil, 0x02)
local f_t_resp_fault    = ProtoField.bool(  "cmsis_dap_tcp.t.resp_status.fault",    "FAULT",    8, nil, 0x04)
local f_t_resp_error    = ProtoField.bool(  "cmsis_dap_tcp.t.resp_status.error",    "ERROR",    8, nil, 0x08)
local f_t_resp_mismatch = ProtoField.bool(  "cmsis_dap_tcp.t.resp_status.mismatch", "MISMATCH", 8, nil, 0x10)
local f_t_resp_data     = ProtoField.uint32("cmsis_dap_tcp.t.resp_data",            "Read Data", base.HEX)

-- DAP_TransferBlock (0x06) request
local f_tb_index      = ProtoField.uint8( "cmsis_dap_tcp.tb.index",       "DAP Index",        base.DEC)
local f_tb_count      = ProtoField.uint16("cmsis_dap_tcp.tb.count",       "Transfer Count",   base.DEC)
local f_tb_req        = ProtoField.uint8( "cmsis_dap_tcp.tb.req",         "Request",          base.HEX)
local f_tb_req_apndp  = ProtoField.uint8( "cmsis_dap_tcp.tb.req.apndp",   "APnDP",            base.DEC, dap_apndp_enum,    0x01)
local f_tb_req_rnw    = ProtoField.uint8( "cmsis_dap_tcp.tb.req.rnw",     "RnW",              base.DEC, dap_rnw_enum,      0x02)
local f_tb_req_addr   = ProtoField.uint8( "cmsis_dap_tcp.tb.req.addr",    "Register Address", base.HEX, dap_reg_addr_enum, 0x0C)
local f_tb_write_data = ProtoField.uint32("cmsis_dap_tcp.tb.write_data",  "Write Data",       base.HEX)

-- DAP_TransferBlock (0x06) response
local f_tb_resp_count    = ProtoField.uint16("cmsis_dap_tcp.tb.resp_count",           "Transfer Count",    base.DEC)
local f_tb_resp_status   = ProtoField.uint8( "cmsis_dap_tcp.tb.resp_status",          "Transfer Response", base.HEX, dap_transfer_resp_enum)
local f_tb_resp_ok       = ProtoField.bool(  "cmsis_dap_tcp.tb.resp_status.ok",       "OK",       8, nil, 0x01)
local f_tb_resp_wait     = ProtoField.bool(  "cmsis_dap_tcp.tb.resp_status.wait",     "WAIT",     8, nil, 0x02)
local f_tb_resp_fault    = ProtoField.bool(  "cmsis_dap_tcp.tb.resp_status.fault",    "FAULT",    8, nil, 0x04)
local f_tb_resp_error    = ProtoField.bool(  "cmsis_dap_tcp.tb.resp_status.error",    "ERROR",    8, nil, 0x08)
local f_tb_resp_mismatch = ProtoField.bool(  "cmsis_dap_tcp.tb.resp_status.mismatch", "MISMATCH", 8, nil, 0x10)
local f_tb_resp_data     = ProtoField.uint32("cmsis_dap_tcp.tb.resp_data",            "Read Data", base.HEX)

-- DAP_SWJ_Pins (0x10): pin byte shared across output, select, and input.
-- Bit positions (from DAP.h): SWCLK/TCK=0, SWDIO/TMS=1, TDI=2, TDO=3, nTRST=5, nRESET=7
local f_swjp_output   = ProtoField.uint8("cmsis_dap_tcp.swjp.output",   "Pin Output",  base.HEX)
local f_swjp_select   = ProtoField.uint8("cmsis_dap_tcp.swjp.select",   "Pin Select",  base.HEX)
local f_swjp_wait_us  = ProtoField.uint32("cmsis_dap_tcp.swjp.wait_us", "Wait (µs)",   base.DEC)
local f_swjp_input    = ProtoField.uint8("cmsis_dap_tcp.swjp.input",    "Pin Input",   base.HEX)
-- Shared pin bit sub-fields (reused under output, select, and input parent items)
local f_swjp_swclk    = ProtoField.bool("cmsis_dap_tcp.swjp.swclk",   "SWCLK/TCK",   8, nil, 0x01)
local f_swjp_swdio    = ProtoField.bool("cmsis_dap_tcp.swjp.swdio",   "SWDIO/TMS",   8, nil, 0x02)
local f_swjp_tdi      = ProtoField.bool("cmsis_dap_tcp.swjp.tdi",     "TDI",         8, nil, 0x04)
local f_swjp_tdo      = ProtoField.bool("cmsis_dap_tcp.swjp.tdo",     "TDO",         8, nil, 0x08)
local f_swjp_ntrst    = ProtoField.bool("cmsis_dap_tcp.swjp.ntrst",   "nTRST",       8, nil, 0x20)
local f_swjp_nreset   = ProtoField.bool("cmsis_dap_tcp.swjp.nreset",  "nRESET",      8, nil, 0x80)

-- DAP_SWJ_Clock (0x11)
local f_swjc_clock_hz = ProtoField.uint32("cmsis_dap_tcp.swjc.clock_hz", "Clock (Hz)", base.DEC)
local f_swjc_status   = ProtoField.uint8( "cmsis_dap_tcp.swjc.status",   "Status",     base.HEX, dap_status_enum)

-- DAP_SWD_Configure (0x13)
local f_swdc_config      = ProtoField.uint8("cmsis_dap_tcp.swdc.config",      "Configuration",        base.HEX)
local f_swdc_turnaround  = ProtoField.uint8("cmsis_dap_tcp.swdc.turnaround",  "Turnaround Clock Period", base.DEC,
                                             dap_swdc_turnaround_enum, 0x03)
local f_swdc_data_phase  = ProtoField.bool( "cmsis_dap_tcp.swdc.data_phase",  "Always Generate Data Phase", 8, nil, 0x04)
local f_swdc_status      = ProtoField.uint8("cmsis_dap_tcp.swdc.status",      "Status",               base.HEX, dap_status_enum)

-- DAP_SWJ_Sequence (0x12)
local f_swjs_bit_count = ProtoField.uint8("cmsis_dap_tcp.swjs.bit_count", "Bit Count (0=256)", base.DEC)
local f_swjs_data      = ProtoField.bytes("cmsis_dap_tcp.swjs.data",      "Sequence Data")
local f_swjs_status    = ProtoField.uint8("cmsis_dap_tcp.swjs.status",    "Status",            base.HEX, dap_status_enum)

-- DAP_SWD_Sequence (0x1D)
local f_swds_seq_count   = ProtoField.uint8("cmsis_dap_tcp.swds.seq_count",   "Sequence Count",  base.DEC)
local f_swds_seq_info    = ProtoField.uint8("cmsis_dap_tcp.swds.seq_info",    "Sequence Info",   base.HEX)
local f_swds_seq_clk     = ProtoField.uint8("cmsis_dap_tcp.swds.seq_clk",     "Clock Count (0=64)", base.DEC, nil, 0x3F)
local f_swds_seq_din     = ProtoField.bool( "cmsis_dap_tcp.swds.seq_din",     "SWDIO Capture (DIN)", 8, nil, 0x80)
local f_swds_seq_data    = ProtoField.bytes("cmsis_dap_tcp.swds.seq_data",    "Output Data")
local f_swds_status      = ProtoField.uint8("cmsis_dap_tcp.swds.status",      "Status",          base.HEX, dap_status_enum)
local f_swds_captured    = ProtoField.bytes("cmsis_dap_tcp.swds.captured",    "Captured Data")

cmsis_dap_tcp_proto.fields = {
    f_hdr_signature, f_hdr_length, f_hdr_pkt_type, f_hdr_reserved,
    f_dap_id,
    -- DAP_Info request/response
    f_info_req_id,
    f_info_resp_length, f_info_resp_str, f_info_resp_u8, f_info_resp_u16, f_info_resp_u32,
    f_info_caps0,
    f_info_caps0_swd, f_info_caps0_jtag, f_info_caps0_swo_uart, f_info_caps0_swo_man,
    f_info_caps0_atomic, f_info_caps0_timer, f_info_caps0_swo_stream, f_info_caps0_uart,
    f_info_caps1, f_info_caps1_usb_com,
    -- DAP_HostStatus request/response
    f_hs_type, f_hs_value, f_hs_status,
    -- DAP_Connect request/response
    f_conn_req_port, f_conn_resp_port,
    -- DAP_TransferConfigure request/response
    f_tc_idle_cycles, f_tc_wait_retry, f_tc_match_retry, f_tc_status,
    -- DAP_Transfer request
    f_t_index, f_t_count,
    f_t_req, f_t_req_apndp, f_t_req_rnw, f_t_req_addr,
    f_t_req_matchval, f_t_req_matchmask, f_t_req_timestamp,
    f_t_write_data, f_t_match_val_data, f_t_match_msk_data,
    -- DAP_Transfer response
    f_t_resp_count, f_t_resp_status,
    f_t_resp_ok, f_t_resp_wait, f_t_resp_fault, f_t_resp_error, f_t_resp_mismatch,
    f_t_resp_data,
    -- DAP_TransferBlock request
    f_tb_index, f_tb_count,
    f_tb_req, f_tb_req_apndp, f_tb_req_rnw, f_tb_req_addr,
    f_tb_write_data,
    -- DAP_TransferBlock response
    f_tb_resp_count, f_tb_resp_status,
    f_tb_resp_ok, f_tb_resp_wait, f_tb_resp_fault, f_tb_resp_error, f_tb_resp_mismatch,
    f_tb_resp_data,
    -- DAP_SWJ_Pins request/response
    f_swjp_output, f_swjp_select, f_swjp_wait_us, f_swjp_input,
    f_swjp_swclk, f_swjp_swdio, f_swjp_tdi, f_swjp_tdo, f_swjp_ntrst, f_swjp_nreset,
    -- DAP_SWJ_Sequence request/response
    f_swjs_bit_count, f_swjs_data, f_swjs_status,
    -- DAP_SWJ_Clock request/response
    f_swjc_clock_hz, f_swjc_status,
    -- DAP_SWD_Configure request/response
    f_swdc_config, f_swdc_turnaround, f_swdc_data_phase, f_swdc_status,
    -- DAP_SWD_Sequence request/response
    f_swds_seq_count, f_swds_seq_info, f_swds_seq_clk, f_swds_seq_din,
    f_swds_seq_data, f_swds_status, f_swds_captured,
}

-- ---------------------------------------------------------------------------
-- State: correlate DAP Info requests to their responses.
--
-- On the first sequential pass (pinfo.visited == false) we track the last
-- Info ID seen per TCP stream and stamp each response packet with the
-- resolved Info ID keyed by frame number.  On re-dissection (clicking a
-- packet), pinfo.visited == true so we go straight to the per-frame table
-- and avoid overwriting state with out-of-order lookups.
-- ---------------------------------------------------------------------------
local tcp_stream_field = Field.new("tcp.stream")
local pending_info_id  = {}     -- [stream_num] → last Info ID seen in a request
local resolved_info_id = {}     -- [frame_num]  → Info ID for this response packet

-- ---------------------------------------------------------------------------
-- Helpers
-- ---------------------------------------------------------------------------

-- Add the six named pin sub-fields to a parent tree item for one pin byte.
local function add_pin_fields(parent_item, buf, offset)
    parent_item:add(f_swjp_swclk,  buf(offset, 1))
    parent_item:add(f_swjp_swdio,  buf(offset, 1))
    parent_item:add(f_swjp_tdi,    buf(offset, 1))
    parent_item:add(f_swjp_tdo,    buf(offset, 1))
    parent_item:add(f_swjp_ntrst,  buf(offset, 1))
    parent_item:add(f_swjp_nreset, buf(offset, 1))
end

-- Add transfer request sub-fields under req_item.
-- has_match_bits: true for DAP_Transfer (0x05), false for DAP_TransferBlock (0x06).
local function add_transfer_request_fields(req_item, buf, offset, req_byte, has_match_bits)
    req_item:add(f_t_req_apndp, buf(offset, 1))
    req_item:add(f_t_req_rnw,   buf(offset, 1))
    local addr_item = req_item:add(f_t_req_addr, buf(offset, 1))
    addr_item:append_text(string.format("  (%s)", transfer_reg_name(req_byte)))
    if has_match_bits then
        req_item:add(f_t_req_matchval,  buf(offset, 1))
        req_item:add(f_t_req_matchmask, buf(offset, 1))
        req_item:add(f_t_req_timestamp, buf(offset, 1))
    end
end

local function add_tb_request_fields(req_item, buf, offset, req_byte, reg_name)
    req_item:add(f_tb_req_apndp, buf(offset, 1))
    req_item:add(f_tb_req_rnw,   buf(offset, 1))
    local addr_item = req_item:add(f_tb_req_addr, buf(offset, 1))
    addr_item:append_text(string.format("  (%s)", reg_name))
end

local function add_transfer_response_fields(status_item, buf, offset)
    status_item:add(f_t_resp_ok,       buf(offset, 1))
    status_item:add(f_t_resp_wait,     buf(offset, 1))
    status_item:add(f_t_resp_fault,    buf(offset, 1))
    status_item:add(f_t_resp_error,    buf(offset, 1))
    status_item:add(f_t_resp_mismatch, buf(offset, 1))
end

local function add_tb_response_fields(status_item, buf, offset)
    status_item:add(f_tb_resp_ok,       buf(offset, 1))
    status_item:add(f_tb_resp_wait,     buf(offset, 1))
    status_item:add(f_tb_resp_fault,    buf(offset, 1))
    status_item:add(f_tb_resp_error,    buf(offset, 1))
    status_item:add(f_tb_resp_mismatch, buf(offset, 1))
end

-- Decode a DAP_Info response payload given the Info ID from the matching request.
local function decode_info_response(payload_tree, payload_buf, info_id)
    local info_len = payload_buf(1, 1):uint()
    payload_tree:add(f_info_resp_length, payload_buf(1, 1))

    if info_len == 0 then
        return  -- info not available
    end

    local id_name = dap_info_id_enum[info_id] or string.format("Unknown (0x%02X)", info_id)

    if info_id == 0xF0 then
        -- Capabilities: 2 bytes of bitmask (always 2 per spec)
        local caps0_item = payload_tree:add(f_info_caps0, payload_buf(2, 1))
        caps0_item:add(f_info_caps0_swd,        payload_buf(2, 1))
        caps0_item:add(f_info_caps0_jtag,        payload_buf(2, 1))
        caps0_item:add(f_info_caps0_swo_uart,   payload_buf(2, 1))
        caps0_item:add(f_info_caps0_swo_man,    payload_buf(2, 1))
        caps0_item:add(f_info_caps0_atomic,     payload_buf(2, 1))
        caps0_item:add(f_info_caps0_timer,      payload_buf(2, 1))
        caps0_item:add(f_info_caps0_swo_stream, payload_buf(2, 1))
        caps0_item:add(f_info_caps0_uart,       payload_buf(2, 1))
        if info_len >= 2 then
            local caps1_item = payload_tree:add(f_info_caps1, payload_buf(3, 1))
            caps1_item:add(f_info_caps1_usb_com, payload_buf(3, 1))
        end
    elseif info_id == 0xF1 or info_id == 0xFB or info_id == 0xFC or info_id == 0xFD then
        -- uint32 LE: Timestamp Clock, UART Rx/Tx buffer sizes, SWO buffer size
        local item = payload_tree:add_le(f_info_resp_u32, payload_buf(2, 4))
        item:append_text(string.format("  (%s)", id_name))
    elseif info_id == 0xFF then
        -- uint16 LE: Max Packet Size
        local item = payload_tree:add_le(f_info_resp_u16, payload_buf(2, 2))
        item:append_text(string.format("  (%s)", id_name))
    elseif info_id == 0xFE then
        -- uint8: Max Packet Count
        local item = payload_tree:add(f_info_resp_u8, payload_buf(2, 1))
        item:append_text(string.format("  (%s)", id_name))
    else
        -- String (Info IDs 0x01-0x09): vendor/product/serial/version strings
        local item = payload_tree:add(f_info_resp_str, payload_buf(2, info_len))
        item:append_text(string.format("  (%s)", id_name))
    end
end

-- ---------------------------------------------------------------------------
-- Main dissector
-- ---------------------------------------------------------------------------
function cmsis_dap_tcp_proto.dissector(buffer, pinfo, tree)
    pinfo.cols.protocol = cmsis_dap_tcp_proto.name

    -- Resolve TCP stream number for DAP Info request/response correlation.
    local tcp_stream_fi = tcp_stream_field()
    local stream_num    = tcp_stream_fi and tcp_stream_fi.value or 0

    local total_len  = buffer:len()
    local buf_offset = 0
    local pkt_num    = 0
    local info_parts = {}

    -- Loop: a single TCP segment may contain multiple DAP packets.
    while buf_offset < total_len do
        local remaining = total_len - buf_offset

        -- Request more data if we don't even have a full header.
        if remaining < HEADER_SIZE then
            pinfo.desegment_offset = buf_offset
            pinfo.desegment_len    = HEADER_SIZE - remaining
            return
        end

        local hdr_buf      = buffer(buf_offset, HEADER_SIZE)
        local signature    = hdr_buf(0, 4):le_uint()
        local pkt_len      = hdr_buf(4, 2):le_uint()
        local hdr_pkt_type = hdr_buf(6, 1):uint()

        -- Request more data if we don't have the full payload yet.
        if remaining < HEADER_SIZE + pkt_len then
            pinfo.desegment_offset = buf_offset
            pinfo.desegment_len    = HEADER_SIZE + pkt_len - remaining
            return
        end

        pkt_num = pkt_num + 1
        local pkt_buf  = buffer(buf_offset, HEADER_SIZE + pkt_len)
        local pkt_tree = tree:add(cmsis_dap_tcp_proto, pkt_buf,
            string.format("CMSIS-DAP-TCP Packet #%d", pkt_num))

        -- Header subtree
        local hdr_tree = pkt_tree:add(cmsis_dap_tcp_proto, hdr_buf, "Header")
        local sig_item = hdr_tree:add_le(f_hdr_signature, hdr_buf(0, 4))
        if signature == DAP_PKT_HDR_SIGNATURE then
            sig_item:append_text(" [correct]")
        else
            sig_item:append_text(" [incorrect]")
            sig_item:add_expert_info(PI_MALFORMED, PI_ERROR,
                string.format("Invalid signature 0x%08X (expected 0x%08X)",
                    signature, DAP_PKT_HDR_SIGNATURE))
        end
        hdr_tree:add_le(f_hdr_length,  hdr_buf(4, 2))
        hdr_tree:add(f_hdr_pkt_type,   hdr_buf(6, 1))
        hdr_tree:add(f_hdr_reserved,   hdr_buf(7, 1))

        -- Payload subtree
        local payload_buf  = buffer(buf_offset + HEADER_SIZE, pkt_len)
        local dap_id       = payload_buf(0, 1):uint()
        local dap_name     = dap_id_enum[dap_id] or string.format("Unknown (0x%02X)", dap_id)
        local dir_label    = (hdr_pkt_type == HDR_PKT_TYPE_REQUEST) and "Request" or "Response"
        local payload_tree = pkt_tree:add(cmsis_dap_tcp_proto, payload_buf,
            string.format("CMSIS-DAP Payload (%s %s)", dap_name, dir_label))

        payload_tree:add(f_dap_id, payload_buf(0, 1))
        local dir_str = (hdr_pkt_type == HDR_PKT_TYPE_REQUEST) and "Req" or "Rsp"
        table.insert(info_parts, string.format("%s %s", dir_str, dap_name))

        -- ---------------------------------------------------------------
        -- Per-command decoding
        -- ---------------------------------------------------------------

        if dap_id == 0x00 then
            -- DAP_Info
            if hdr_pkt_type == HDR_PKT_TYPE_REQUEST then
                -- Request: [DAP_ID][info_id:1]
                local info_id = payload_buf(1, 1):uint()
                local id_item = payload_tree:add(f_info_req_id, payload_buf(1, 1))
                local id_name = dap_info_id_enum[info_id]
                if id_name then
                    id_item:append_text(string.format("  (%s)", id_name))
                end
                if not pinfo.visited then
                    pending_info_id[stream_num] = info_id
                end

            elseif hdr_pkt_type == HDR_PKT_TYPE_RESPONSE then
                -- Response: [DAP_ID][length:1][data:N]
                -- On first pass, stamp this frame with the pending Info ID so
                -- subsequent re-dissections (user clicking packets) are stable.
                if not pinfo.visited then
                    resolved_info_id[pinfo.number] = pending_info_id[stream_num]
                end
                decode_info_response(payload_tree, payload_buf, resolved_info_id[pinfo.number])
            end

        elseif dap_id == 0x01 then
            -- DAP_HostStatus
            if hdr_pkt_type == HDR_PKT_TYPE_REQUEST then
                -- Request: [DAP_ID][type:1][value:1]
                payload_tree:add(f_hs_type,  payload_buf(1, 1))
                -- Only bit 0 of the value byte is meaningful (on/off)
                local val_item = payload_tree:add(f_hs_value, payload_buf(2, 1))
                val_item:append_text(string.format("  (raw 0x%02X)", payload_buf(2, 1):uint()))
            elseif hdr_pkt_type == HDR_PKT_TYPE_RESPONSE then
                -- Response: [DAP_ID][status:1]
                payload_tree:add(f_hs_status, payload_buf(1, 1))
            end

        elseif dap_id == 0x02 then
            -- DAP_Connect
            if hdr_pkt_type == HDR_PKT_TYPE_REQUEST then
                -- Request: [DAP_ID][port:1]
                payload_tree:add(f_conn_req_port, payload_buf(1, 1))
            elseif hdr_pkt_type == HDR_PKT_TYPE_RESPONSE then
                -- Response: [DAP_ID][port:1]  (0 = connection failed)
                payload_tree:add(f_conn_resp_port, payload_buf(1, 1))
            end

        elseif dap_id == 0x04 then
            -- DAP_TransferConfigure
            if hdr_pkt_type == HDR_PKT_TYPE_REQUEST then
                -- Request: [DAP_ID][idle_cycles:1][wait_retry:2LE][match_retry:2LE]
                payload_tree:add(   f_tc_idle_cycles, payload_buf(1, 1))
                payload_tree:add_le(f_tc_wait_retry,  payload_buf(2, 2))
                payload_tree:add_le(f_tc_match_retry, payload_buf(4, 2))
            elseif hdr_pkt_type == HDR_PKT_TYPE_RESPONSE then
                -- Response: [DAP_ID][status:1]
                payload_tree:add(f_tc_status, payload_buf(1, 1))
            end

        elseif dap_id == 0x05 then
            -- DAP_Transfer
            if hdr_pkt_type == HDR_PKT_TYPE_REQUEST then
                payload_tree:add(f_t_index, payload_buf(1, 1))
                payload_tree:add(f_t_count, payload_buf(2, 1))
                local transfer_count = payload_buf(2, 1):uint()
                local off = 3

                for i = 1, transfer_count do
                    local req_byte     = payload_buf(off, 1):uint()
                    local is_write     = bit.band(req_byte, 0x02) == 0  -- RnW=0 → write
                    local is_matchval  = bit.band(req_byte, 0x10) ~= 0
                    local is_matchmask = bit.band(req_byte, 0x20) ~= 0
                    local has_word     = is_write or is_matchval
                    local entry_bytes  = has_word and 5 or 1
                    local dir          = is_write and "Write" or "Read"

                    local t_item = payload_tree:add(cmsis_dap_tcp_proto,
                        payload_buf(off, entry_bytes),
                        string.format("Transfer %d (%s %s)", i, dir,
                            transfer_reg_name(req_byte)))
                    local req_item = t_item:add(f_t_req, payload_buf(off, 1))
                    add_transfer_request_fields(req_item, payload_buf, off, req_byte, true)
                    off = off + 1

                    if is_write then
                        if is_matchmask then
                            t_item:add_le(f_t_match_msk_data, payload_buf(off, 4))
                        else
                            t_item:add_le(f_t_write_data, payload_buf(off, 4))
                        end
                        off = off + 4
                    elseif is_matchval then
                        t_item:add_le(f_t_match_val_data, payload_buf(off, 4))
                        off = off + 4
                    end
                end

            elseif hdr_pkt_type == HDR_PKT_TYPE_RESPONSE then
                -- Response: [DAP_ID][count:1][response:1][N×data:4]
                -- N derived from payload length: (pkt_len - 3) / 4
                payload_tree:add(f_t_resp_count, payload_buf(1, 1))
                local status_item = payload_tree:add(f_t_resp_status, payload_buf(2, 1))
                add_transfer_response_fields(status_item, payload_buf, 2)
                local num_words = math.floor((pkt_len - 3) / 4)
                for i = 0, num_words - 1 do
                    payload_tree:add_le(f_t_resp_data, payload_buf(3 + 4*i, 4))
                end
            end

        elseif dap_id == 0x06 then
            -- DAP_TransferBlock
            if hdr_pkt_type == HDR_PKT_TYPE_REQUEST then
                -- Request: [DAP_ID][index:1][count:2LE][request:1][N×data:4 if write]
                payload_tree:add(f_tb_index, payload_buf(1, 1))
                payload_tree:add_le(f_tb_count, payload_buf(2, 2))
                local req_byte = payload_buf(4, 1):uint()
                local reg_name = transfer_reg_name(req_byte)
                local req_item = payload_tree:add(f_tb_req, payload_buf(4, 1))
                add_tb_request_fields(req_item, payload_buf, 4, req_byte, reg_name)
                req_item:append_text(string.format("  (%s)", reg_name))
                local is_write = bit.band(req_byte, 0x02) == 0
                if is_write then
                    local transfer_count = payload_buf(2, 2):le_uint()
                    for i = 0, transfer_count - 1 do
                        payload_tree:add_le(f_tb_write_data, payload_buf(5 + 4*i, 4))
                    end
                end

            elseif hdr_pkt_type == HDR_PKT_TYPE_RESPONSE then
                -- Response: [DAP_ID][count:2LE][response:1][N×data:4 if read]
                -- N derived from payload length: (pkt_len - 4) / 4
                payload_tree:add_le(f_tb_resp_count, payload_buf(1, 2))
                local status_item = payload_tree:add(f_tb_resp_status, payload_buf(3, 1))
                add_tb_response_fields(status_item, payload_buf, 3)
                local num_words = math.floor((pkt_len - 4) / 4)
                for i = 0, num_words - 1 do
                    payload_tree:add_le(f_tb_resp_data, payload_buf(4 + 4*i, 4))
                end
            end

        elseif dap_id == 0x10 then
            -- DAP_SWJ_Pins
            if hdr_pkt_type == HDR_PKT_TYPE_REQUEST then
                -- Request: [DAP_ID][pin_output:1][pin_select:1][wait_us:4LE]
                local out_item = payload_tree:add(f_swjp_output, payload_buf(1, 1))
                add_pin_fields(out_item, payload_buf, 1)
                local sel_item = payload_tree:add(f_swjp_select, payload_buf(2, 1))
                add_pin_fields(sel_item, payload_buf, 2)
                payload_tree:add_le(f_swjp_wait_us, payload_buf(3, 4))
            elseif hdr_pkt_type == HDR_PKT_TYPE_RESPONSE then
                -- Response: [DAP_ID][pin_input:1]  (current state of all pins)
                local in_item = payload_tree:add(f_swjp_input, payload_buf(1, 1))
                add_pin_fields(in_item, payload_buf, 1)
            end

        elseif dap_id == 0x12 then
            -- DAP_SWJ_Sequence
            if hdr_pkt_type == HDR_PKT_TYPE_REQUEST then
                -- Request: [DAP_ID][bit_count:1][data: ceil(bit_count/8) bytes]
                -- bit_count=0 means 256 bits
                local bit_count  = payload_buf(1, 1):uint()
                local actual_bits = (bit_count == 0) and 256 or bit_count
                local nbytes     = math.floor((actual_bits + 7) / 8)
                local cnt_item   = payload_tree:add(f_swjs_bit_count, payload_buf(1, 1))
                cnt_item:append_text(string.format("  (%d bits)", actual_bits))
                payload_tree:add(f_swjs_data, payload_buf(2, nbytes))
            elseif hdr_pkt_type == HDR_PKT_TYPE_RESPONSE then
                -- Response: [DAP_ID][status:1]
                payload_tree:add(f_swjs_status, payload_buf(1, 1))
            end

        elseif dap_id == 0x11 then
            -- DAP_SWJ_Clock
            if hdr_pkt_type == HDR_PKT_TYPE_REQUEST then
                -- Request: [DAP_ID][clock_hz:4LE]
                payload_tree:add_le(f_swjc_clock_hz, payload_buf(1, 4))
            elseif hdr_pkt_type == HDR_PKT_TYPE_RESPONSE then
                -- Response: [DAP_ID][status:1]
                payload_tree:add(f_swjc_status, payload_buf(1, 1))
            end

        elseif dap_id == 0x13 then
            -- DAP_SWD_Configure
            if hdr_pkt_type == HDR_PKT_TYPE_REQUEST then
                -- Request: [DAP_ID][config:1]
                --   bits[1:0] = turnaround clock period (0=1 cycle … 3=4 cycles)
                --   bit 2     = always generate data phase
                local cfg_item = payload_tree:add(f_swdc_config, payload_buf(1, 1))
                cfg_item:add(f_swdc_turnaround, payload_buf(1, 1))
                cfg_item:add(f_swdc_data_phase, payload_buf(1, 1))
            elseif hdr_pkt_type == HDR_PKT_TYPE_RESPONSE then
                -- Response: [DAP_ID][status:1]
                payload_tree:add(f_swdc_status, payload_buf(1, 1))
            end

        elseif dap_id == 0x1D then
            -- DAP_SWD_Sequence
            if hdr_pkt_type == HDR_PKT_TYPE_REQUEST then
                -- Request: [DAP_ID][seq_count:1] then for each sequence:
                --   [seq_info:1]  bits[5:0]=clock count (0=64), bit7=DIN (capture SWDIO)
                --   if DIN=0: ceil(clock_count/8) bytes of output data
                local seq_count = payload_buf(1, 1):uint()
                payload_tree:add(f_swds_seq_count, payload_buf(1, 1))
                local off = 2
                for i = 1, seq_count do
                    local seq_info  = payload_buf(off, 1):uint()
                    local clk_count = bit.band(seq_info, 0x3F)
                    local din       = bit.band(seq_info, 0x80) ~= 0
                    local nbytes    = math.floor(((clk_count == 0 and 64 or clk_count) + 7) / 8)
                    local dir_tag   = din and "Capture" or "Output"
                    local actual_clk = (clk_count == 0) and 64 or clk_count

                    local seq_bytes = 1 + (din and 0 or nbytes)
                    local seq_item  = payload_tree:add(cmsis_dap_tcp_proto,
                        payload_buf(off, seq_bytes),
                        string.format("Sequence %d (%s, %d clocks)", i, dir_tag, actual_clk))
                    local info_item = seq_item:add(f_swds_seq_info, payload_buf(off, 1))
                    info_item:add(f_swds_seq_clk, payload_buf(off, 1))
                    info_item:add(f_swds_seq_din, payload_buf(off, 1))
                    off = off + 1
                    if not din then
                        seq_item:add(f_swds_seq_data, payload_buf(off, nbytes))
                        off = off + nbytes
                    end
                end

            elseif hdr_pkt_type == HDR_PKT_TYPE_RESPONSE then
                -- Response: [DAP_ID][status:1][captured_bytes...]
                -- Captured bytes belong to DIN=1 sequences; without request
                -- correlation we display them as a single raw blob.
                payload_tree:add(f_swds_status, payload_buf(1, 1))
                local captured_len = pkt_len - 2
                if captured_len > 0 then
                    payload_tree:add(f_swds_captured, payload_buf(2, captured_len))
                end
            end
        end

        buf_offset = buf_offset + HEADER_SIZE + pkt_len
    end

    -- Build the Info column from all packets in this TCP segment.
    if #info_parts > 0 then
        pinfo.cols.info = table.concat(info_parts, " | ")
    end
end

-- Register the protocol on port 4441
local tcp_port = DissectorTable.get("tcp.port")
tcp_port:add(4441, cmsis_dap_tcp_proto)
