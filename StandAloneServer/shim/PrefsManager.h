/* Shim for src/PrefsManager.h (see shim/global.h).  The server reads the judge
 * windows in CheckLowerJudge(); the values are the game's defaults
 * (PrefsManager::PrefsManager in src/PrefsManager.cpp). */
#ifndef PREFSMANAGER_H
#define PREFSMANAGER_H

class PrefsManager
{
public:
	float m_fJudgeWindowSecondsMarvelous;
	float m_fJudgeWindowSecondsPerfect;
	float m_fJudgeWindowSecondsGreat;
	float m_fJudgeWindowSecondsGood;
	float m_fJudgeWindowSecondsBoo;

	PrefsManager() :
		m_fJudgeWindowSecondsMarvelous( 0.0225f ),
		m_fJudgeWindowSecondsPerfect( 0.045f ),
		m_fJudgeWindowSecondsGreat( 0.090f ),
		m_fJudgeWindowSecondsGood( 0.135f ),
		m_fJudgeWindowSecondsBoo( 0.180f )
	{ }
};

extern PrefsManager *PREFSMAN;

#endif
