// Minimal Difficulty.h for StandAloneServer build.
// Only the enum is required so EndOfGame_PlayerData can declare a member of this type.
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

typedef Difficulty CourseDifficulty;
#define NUM_COURSE_DIFFICULTIES NUM_DIFFICULTIES

#endif
