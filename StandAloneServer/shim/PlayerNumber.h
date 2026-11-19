/* Shim for src/PlayerNumber.h (see shim/global.h): NetworkSyncManager.h only needs NUM_PLAYERS. */
#ifndef PlayerNumber_H
#define PlayerNumber_H

enum PlayerNumber
{
	PLAYER_1 = 0,
	PLAYER_2,
	NUM_PLAYERS,
	PLAYER_INVALID
};

#endif
