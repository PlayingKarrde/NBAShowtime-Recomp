-- Traces interrupt dispatch and IDE register traffic on the real BIOS/game in MAME.
-- Run via tools/mame/run_trace.sh. Writes $DUMP_DIR/irq_trace.txt.
--
--  * breakpoint on the BIOS interrupt dispatcher (0x8001204c): logs Cause (a0) and SR
--  * breakpoints on each interrupt-line handler: counts which ones run
--  * watchpoints on IDE status / bus-master status reads: logs the reading PC
local outdir = os.getenv("DUMP_DIR") or "."
local seconds = tonumber(os.getenv("TRACE_SECONDS") or "30")
local cpu = manager.machine.devices[":maincpu"]
local dbg = manager.machine.debugger
local log = {}

local function cmd(c) dbg:command(c) end

-- Debugger printf output lands in consolelog; actions keep running ("go").
cmd('bpset 8001204c,1,{printf "IRQ cause=%08X sr=%08X t=%d",a0,d@(a1+178),totalcycles; go}')
local handlers = {
  [0x80011d70] = "IP4", [0x80010774] = "IP2", [0x80011524] = "IP3", [0x80011fa4] = "IP6",
  [0x80012010] = "IP7", [0x80011dc8] = "IP5", [0x80010718] = "IP1", [0x800106bc] = "IP0",
}
for addr, name in pairs(handlers) do
  cmd(string.format('bpset %08x,1,{printf "HANDLER %s ra=%%08X",ra; go}', addr, name))
end
-- IDE status (task file +7), alternate status (+0xA), bus-master status (+0x22)
cmd('wpset ac000107,1,r,1,{printf "IDE status read pc=%08X",pc; go}')
cmd('wpset ac00010a,1,r,1,{printf "IDE altstatus read pc=%08X",pc; go}')
cmd('wpset ac000122,1,r,1,{printf "IDE bmstatus read pc=%08X",pc; go}')
cmd('wpset ac000107,1,w,1,{printf "IDE command %02X pc=%08X",wpdata,pc; go}')
cmd("go")

local seen = 0
emu.register_periodic(function()
  local n = #dbg.consolelog
  while seen < n do
    seen = seen + 1
    log[#log + 1] = dbg.consolelog[seen]
  end
  if manager.machine.time:as_double() >= seconds then
    local f = assert(io.open(outdir .. "/irq_trace.txt", "w"))
    for _, l in ipairs(log) do f:write(l, "\n") end
    f:close()
    print(string.format("[trace] %d lines -> %s/irq_trace.txt", #log, outdir))
    manager.machine:exit()
  end
end)
