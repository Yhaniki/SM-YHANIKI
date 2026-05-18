// Minimal PlayerNumber.h for StandAloneServer build.
// Only the enum is needed because NetworkSyncManager.h uses NUM_PLAYERS.
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
