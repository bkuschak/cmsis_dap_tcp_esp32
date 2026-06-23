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
local f_hdr_signature = ProtoField.uint32("cmsis_dap_tcp.hdr_signature", "Signature",    base.HEX)
local f_hdr_length    = ProtoField.uint16("cmsis_dap_tcp.hdr_length",    "Payload length", base.DEC)
local f_hdr_pkt_type  = ProtoField.uint8( "cmsis_dap_tcp.hdr_pkt_type",  "Packet type",  base.HEX, hdr_pkt_type_enum)
local f_hdr_reserved  = ProtoField.uint8( "cmsis_dap_tcp.hdr_reserved",  "Reserved",     base.HEX)

-- DAP command ID (first byte of every payload)
local f_dap_id = ProtoField.uint8("cmsis_dap_tcp.dap_id", "DAP ID", base.HEX, dap_id_enum)

-- DAP_Transfer (0x05) request
local f_t_index          = ProtoField.uint8( "cmsis_dap_tcp.t.index",           "DAP Index",      base.DEC)
local f_t_count          = ProtoField.uint8( "cmsis_dap_tcp.t.count",           "Transfer Count", base.DEC)
local f_t_req            = ProtoField.uint8( "cmsis_dap_tcp.t.req",             "Request",        base.HEX)
local f_t_req_apndp      = ProtoField.uint8( "cmsis_dap_tcp.t.req.apndp",       "APnDP",          base.DEC, dap_apndp_enum,    0x01)
local f_t_req_rnw        = ProtoField.uint8( "cmsis_dap_tcp.t.req.rnw",         "RnW",            base.DEC, dap_rnw_enum,      0x02)
local f_t_req_addr       = ProtoField.uint8( "cmsis_dap_tcp.t.req.addr",        "Register Address", base.HEX, dap_reg_addr_enum, 0x0C)
local f_t_req_matchval   = ProtoField.bool(  "cmsis_dap_tcp.t.req.match_value", "MATCH_VALUE",    8, nil, 0x10)
local f_t_req_matchmask  = ProtoField.bool(  "cmsis_dap_tcp.t.req.match_mask",  "MATCH_MASK",     8, nil, 0x20)
local f_t_req_timestamp  = ProtoField.bool(  "cmsis_dap_tcp.t.req.timestamp",   "TIMESTAMP",      8, nil, 0x80)
local f_t_write_data     = ProtoField.uint32("cmsis_dap_tcp.t.write_data",      "Write Data",     base.HEX)
local f_t_match_val_data = ProtoField.uint32("cmsis_dap_tcp.t.match_val",       "Match Value",    base.HEX)
local f_t_match_msk_data = ProtoField.uint32("cmsis_dap_tcp.t.match_mask",      "Match Mask",     base.HEX)

-- DAP_Transfer (0x05) response
local f_t_resp_count     = ProtoField.uint8( "cmsis_dap_tcp.t.resp_count",           "Transfer Count",    base.DEC)
local f_t_resp_status    = ProtoField.uint8( "cmsis_dap_tcp.t.resp_status",          "Transfer Response", base.HEX, dap_transfer_resp_enum)
local f_t_resp_ok        = ProtoField.bool(  "cmsis_dap_tcp.t.resp_status.ok",       "OK",       8, nil, 0x01)
local f_t_resp_wait      = ProtoField.bool(  "cmsis_dap_tcp.t.resp_status.wait",     "WAIT",     8, nil, 0x02)
local f_t_resp_fault     = ProtoField.bool(  "cmsis_dap_tcp.t.resp_status.fault",    "FAULT",    8, nil, 0x04)
local f_t_resp_error     = ProtoField.bool(  "cmsis_dap_tcp.t.resp_status.error",    "ERROR",    8, nil, 0x08)
local f_t_resp_mismatch  = ProtoField.bool(  "cmsis_dap_tcp.t.resp_status.mismatch", "MISMATCH", 8, nil, 0x10)
local f_t_resp_data      = ProtoField.uint32("cmsis_dap_tcp.t.resp_data",            "Read Data", base.HEX)

-- DAP_TransferBlock (0x06) request
local f_tb_index      = ProtoField.uint8( "cmsis_dap_tcp.tb.index",       "DAP Index",      base.DEC)
local f_tb_count      = ProtoField.uint16("cmsis_dap_tcp.tb.count",       "Transfer Count", base.DEC)
local f_tb_req        = ProtoField.uint8( "cmsis_dap_tcp.tb.req",         "Request",        base.HEX)
local f_tb_req_apndp  = ProtoField.uint8( "cmsis_dap_tcp.tb.req.apndp",   "APnDP",          base.DEC, dap_apndp_enum,    0x01)
local f_tb_req_rnw    = ProtoField.uint8( "cmsis_dap_tcp.tb.req.rnw",     "RnW",            base.DEC, dap_rnw_enum,      0x02)
local f_tb_req_addr   = ProtoField.uint8( "cmsis_dap_tcp.tb.req.addr",    "Register Address", base.HEX, dap_reg_addr_enum, 0x0C)
local f_tb_write_data = ProtoField.uint32("cmsis_dap_tcp.tb.write_data",  "Write Data",     base.HEX)

-- DAP_TransferBlock (0x06) response
local f_tb_resp_count    = ProtoField.uint16("cmsis_dap_tcp.tb.resp_count",          "Transfer Count",    base.DEC)
local f_tb_resp_status   = ProtoField.uint8( "cmsis_dap_tcp.tb.resp_status",         "Transfer Response", base.HEX, dap_transfer_resp_enum)
local f_tb_resp_ok       = ProtoField.bool(  "cmsis_dap_tcp.tb.resp_status.ok",       "OK",       8, nil, 0x01)
local f_tb_resp_wait     = ProtoField.bool(  "cmsis_dap_tcp.tb.resp_status.wait",     "WAIT",     8, nil, 0x02)
local f_tb_resp_fault    = ProtoField.bool(  "cmsis_dap_tcp.tb.resp_status.fault",    "FAULT",    8, nil, 0x04)
local f_tb_resp_error    = ProtoField.bool(  "cmsis_dap_tcp.tb.resp_status.error",    "ERROR",    8, nil, 0x08)
local f_tb_resp_mismatch = ProtoField.bool(  "cmsis_dap_tcp.tb.resp_status.mismatch", "MISMATCH", 8, nil, 0x10)
local f_tb_resp_data     = ProtoField.uint32("cmsis_dap_tcp.tb.resp_data",            "Read Data", base.HEX)

cmsis_dap_tcp_proto.fields = {
    f_hdr_signature, f_hdr_length, f_hdr_pkt_type, f_hdr_reserved,
    f_dap_id,
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
}

-- ---------------------------------------------------------------------------
-- Helpers
-- ---------------------------------------------------------------------------

-- Add transfer request sub-fields under req_item.
-- req_byte is the raw uint8 value of the request byte.
-- has_match_bits: true for DAP_Transfer, false for DAP_TransferBlock.
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

-- ---------------------------------------------------------------------------
-- Main dissector
-- ---------------------------------------------------------------------------
function cmsis_dap_tcp_proto.dissector(buffer, pinfo, tree)
    pinfo.cols.protocol = cmsis_dap_tcp_proto.name

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
        local payload_buf = buffer(buf_offset + HEADER_SIZE, pkt_len)
        local dap_id      = payload_buf(0, 1):uint()
        local dap_name    = dap_id_enum[dap_id] or string.format("Unknown (0x%02X)", dap_id)
        local payload_tree = pkt_tree:add(cmsis_dap_tcp_proto, payload_buf,
            string.format("CMSIS-DAP Payload (%s)", dap_name))

        payload_tree:add(f_dap_id, payload_buf(0, 1))
        local dir_str  = (hdr_pkt_type == HDR_PKT_TYPE_REQUEST) and "Req" or "Rsp"
        table.insert(info_parts, string.format("%s %s", dir_str, dap_name))

        -- ---------------------------------------------------------------
        -- Per-command decoding
        -- ---------------------------------------------------------------

        if dap_id == 0x05 then
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
                local req_byte  = payload_buf(4, 1):uint()
                local reg_name  = transfer_reg_name(req_byte)
                local req_item  = payload_tree:add(f_tb_req, payload_buf(4, 1))
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
