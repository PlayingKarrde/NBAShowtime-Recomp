// Versions of the interface between the engine (nbashowtime, shipped prebuilt) and the game
// library (nbagame.so / .dll / .dylib) that the launcher builds from the player's ROM.
#ifndef NBA_RECOMP_VERSION_H
#define NBA_RECOMP_VERSION_H

/* Bump when RecompContext, RecompHostApi or RecompGameExports change layout or meaning. */
#define RECOMP_GAME_ABI 1

/* Bump when the recompiler's output changes in a way the engine depends on: codegen fixes, new
   or changed instruction patches (Program::insn_override), new runtime hooks in generated code.
   The engine refuses a game library built with a different revision and asks the player to run
   the launcher again. Engine-only changes (renderer, audio, input, settings, the hardware models)
   don't need a bump: updating the executable is enough. */
#define RECOMP_CODEGEN_REVISION 1

#endif
