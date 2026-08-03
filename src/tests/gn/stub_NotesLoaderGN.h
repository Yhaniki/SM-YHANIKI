#ifndef GN_TEST_LOADER_H
#define GN_TEST_LOADER_H
#include "GNFile.h"
class GNLoader
{
public:
	static bool ReadGNFile( CString sPath, GNFile::StepFile &sfOut, GNFile::ContainerInfo &infoOut, CString &sErrOut );
	static int FrameTypeToCol( int iFrameType );
	static int ColToFrameType( int iCol );
};
#endif
