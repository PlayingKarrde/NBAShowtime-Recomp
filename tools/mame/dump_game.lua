-- Dumps NBA Showtime's main RAM from MAME.
-- Run with: mame nbashowt -debug -debugger none -autoboot_script tools/mame/dump_game.lua
-- 1) When the CPU first reaches 0x80100000 (GAME.EXE entry), saves RAM -> entry_ram.bin
-- 2) Also saves RAM snapshots at a few points in time as a fallback.
-- Output goes to the directory in env DUMP_DIR (default: current dir).

local outdir = os.getenv("DUMP_DIR") or "."
local cpu = manager.machine.devices[":maincpu"]
local space = cpu.spaces["program"]
-- MAME's program space is physical: kseg0 0x80000000 maps to physical 0.
local RAM_BASE, RAM_SIZE = 0x00000000, 0x800000 -- 8 MB main RAM

local function save_ram(name)
  local f = assert(io.open(outdir .. "/" .. name, "wb"))
  local chunk = 0x10000
  for a = 0, RAM_SIZE - 1, chunk do
    f:write(space:read_range(RAM_BASE + a, RAM_BASE + a + chunk - 1, 8))
  end
  f:close()
  print(string.format("[dump] wrote %s at %.2fs pc=%08x", name,
    manager.machine.time:as_double(), cpu.state["PC"].value & 0xffffffff))
end

local function save_regs(name)
  local f = assert(io.open(outdir .. "/" .. name, "w"))
  local keys = {}
  for k, _ in pairs(cpu.state) do keys[#keys + 1] = k end
  table.sort(keys)
  f:write("{\n")
  local dbg = manager.machine.debugger
  for n, k in ipairs(keys) do
    -- state_entry.value raises "integer value will be misrepresented" for 64-bit values
    -- with the top bit set (sign-extended kseg0 addresses), so fall back to the
    -- debugger's own printf and read the result from the console log.
    local ok, v = pcall(function() return cpu.state[k].value end)
    local hex
    if ok then
      hex = string.format("%016x", v)
    else
      dbg:command(string.format('printf "%%X",%s', k))
      local s = tostring(dbg.consolelog[#dbg.consolelog]):lower()
      hex = string.rep("0", 16 - #s) .. s
    end
    f:write(string.format('  "%s": "0x%s"%s\n', k, hex, n < #keys and "," or ""))
  end
  f:write("}\n")
  f:close()
  print("[dump] wrote " .. name)
end

if cpu.debug then
  -- Debugger is enabled: break at the game's entry point. The debugger saves RAM
  -- (logical kseg0 addresses) and stays stopped; the periodic callback below then
  -- records the CPU registers and resumes.
  local f = outdir .. "/entry_ram.bin"
  -- 0x80100000 is reached by several boot stages (loader, intermediate program,
  -- then the game). Fire only on the game crt0: lui t0,0x8036 (BSS @ 0x8035a470).
  cpu.debug:bpset(0x80100000, "d@80100000 == 3c088036", string.format("save %s,80000000,800000", f))
  local done = false
  emu.register_periodic(function()
    local dbg = manager.machine.debugger
    if done or dbg.execution_state ~= "stop" then return end
    if (cpu.state["PC"].value & 0xffffffff) == 0x80100000 then
      save_regs("entry_regs.json")
      -- VRC5074 (Nile 4) system controller registers 0x1FA00000-0x1FA001FF: interrupt
      -- routing etc. programmed by the BIOS before the game starts. (The UART at +0x300
      -- is skipped: reading its data/IIR registers has side effects.)
      local nf = assert(io.open(outdir .. "/entry_nile.bin", "wb"))
      for a = 0x1FA00000, 0x1FA001FC, 4 do
        nf:write(string.pack("<I4", space:read_u32(a) & 0xffffffff))
      end
      nf:close()
      print("[dump] wrote entry_nile.bin")
      -- SIO board registers (control, interrupt status, interrupt enable, ...): one byte each at
      -- 0x04000000 + 0x1000 * n, n = 0..7.
      local sf = assert(io.open(outdir .. "/entry_sio.bin", "wb"))
      for n = 0, 7 do sf:write(string.char(space:read_u8(0x04000000 + 0x1000 * n) & 0xff)) end
      sf:close()
      print("[dump] wrote entry_sio.bin")
      cpu.debug:bpclear()
      done = true
    end
    dbg.execution_state = "run"
  end)
else
  print("[dump] debugger not enabled; only timed snapshots will be taken")
end

local snapshots = { 20, 45, 75 }
local idx = 1
emu.register_frame_done(function()
  local t = manager.machine.time:as_double()
  if idx <= #snapshots and t >= snapshots[idx] then
    save_ram(string.format("ram_%03ds.bin", snapshots[idx]))
    idx = idx + 1
    if idx > #snapshots then manager.machine:exit() end
  end
end)
