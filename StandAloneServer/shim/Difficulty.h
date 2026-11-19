/* Shim for src/Difficulty.h (see shim/global.h): NetworkSyncManager.h declares a Difficulty member. */
#ifndef DIFFICULTY_H
#define DIFFICULTY_H

enum Difficulty
{
	DIFFICULTY_BEGINNER,
	DIFFICULTY_EASY,
	DIFFICULTY_MEDIUM,
	DIFFICULTY_HARD,
	DIFFICULTY_CHALLENGE,
	DIFFICULTY_EDIT,
	NUM_DIFFICULTIES,
	DIFFICULTY_INVALID
};

#endif
