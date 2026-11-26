-- Logs every CPU access to a physical address range using Lua memory taps (no debugger needed).
-- Run via tools/mame/run_trace.sh. Environment:
--   TRACE_BASE (hex physical address, default 04800000 = I/O ASIC), TRACE_LEN (hex, default 20)
--   TRACE_SECONDS (emulated seconds, default 30), TRACE_MAX (max lines, default 300000), DUMP_DIR
-- Output: $DUMP_DIR/mmio_trace.txt, lines "t=<seconds> <R|W><bits> <addr> <value> pc=<pc>"
local outdir = os.getenv("DUMP_DIR") or "."
local seconds = tonumber(os.getenv("TRACE_SECONDS") or "30")
local base = tonumber(os.getenv("TRACE_BASE") or "04800000", 16)
local len = tonumber(os.getenv("TRACE_LEN") or "20", 16)
local maxlines = tonumber(os.getenv("TRACE_MAX") or "300000")
-- Optional: an address whose reads are not logged (e.g. a status register that is polled).
local skip_read = os.getenv("TRACE_SKIP_READ") and tonumber(os.getenv("TRACE_SKIP_READ"), 16) or nil
local cpu = manager.machine.devices[":maincpu"]
local space = cpu.spaces["program"]
local lines = {}

local function pc()
  local ok, v = pcall(function() return cpu.state["PC"].value end)
  return ok and (v & 0xffffffff) or 0
end

-- The R5000 bus is 64 bits wide: the tap gets an 8-byte aligned offset and a lane mask.
local function decode(offset, data, mask)
  local first, n = nil, 0
  for i = 0, 7 do
    if ((mask >> (8 * i)) & 0xff) ~= 0 then
      first = first or i
      n = n + 1
    end
  end
  if not first then return offset, 0, 0 end
  local bits = 8 * n
  local value = (data >> (8 * first)) & ((bits == 64) and -1 or ((1 << bits) - 1))
  return offset + first, bits, value
end

local function log(kind, offset, data, mask)
  if #lines >= maxlines then return end
  local addr, bits, value = decode(offset, data, mask)
  lines[#lines + 1] = string.format("t=%.6f %s%d %08X %08X pc=%08X", manager.machine.time:as_double(),
    kind, bits, addr, value & 0xffffffff, pc())
end

-- Install the taps once the game is running: the VRC5074 remaps its windows during BIOS boot,
-- which replaces the handlers (and any taps installed earlier).
local start_at = tonumber(os.getenv("TRACE_START") or "7")
local last = base + len - 1
local rtap, wtap
local function install()
  rtap = space:install_read_tap(base, last, "trace_r", function(offset, data, mask)
    if skip_read then
      local a = decode(offset, data, mask)
      if a == skip_read then return data end
    end
    log("R", offset, data, mask)
    return data
  end)
  wtap = space:install_write_tap(base, last, "trace_w", function(offset, data, mask)
    log("W", offset, data, mask)
  end)
end

emu.register_periodic(function()
  if not rtap and manager.machine.time:as_double() >= start_at then install() end
  if manager.machine.time:as_double() >= seconds then
    local f = assert(io.open(outdir .. "/mmio_trace.txt", "w"))
    for _, l in ipairs(lines) do f:write(l, "\n") end
    f:close()
    print(string.format("[trace] %d lines -> %s/mmio_trace.txt", #lines, outdir))
    rtap = nil
    wtap = nil
    manager.machine:exit()
  end
end)
