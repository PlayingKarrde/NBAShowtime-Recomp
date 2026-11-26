-- Records an instruction trace of the DCS2 sound DSP in MAME (debugger `trace`).
-- Env: DSP_TRACE_START / DSP_TRACE_STOP (emulated seconds), DUMP_DIR.
-- Output: $DUMP_DIR/dsp_trace.log  (one line per instruction: "PC: disassembly")
local outdir = os.getenv("DUMP_DIR") or "."
local start_at = tonumber(os.getenv("DSP_TRACE_START") or "8.0")
local stop_at = tonumber(os.getenv("DSP_TRACE_STOP") or "8.2")
local dbg = manager.machine.debugger
local state = 0
dbg:command("go")
emu.register_periodic(function()
  local t = manager.machine.time:as_double()
  if state == 0 and t >= start_at then
    dbg:command(string.format("trace %s/dsp_trace.log,dcs:dcs2", outdir))
    state = 1
    print(string.format("[dsp] tracing from %.4f", t))
  elseif state == 1 and t >= stop_at then
    dbg:command("trace off,dcs:dcs2")
    state = 2
    print(string.format("[dsp] stopped at %.4f", t))
    manager.machine:exit()
  end
end)
