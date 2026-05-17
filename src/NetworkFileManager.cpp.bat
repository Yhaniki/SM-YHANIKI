#include <boost/filesystem.hpp>
#include <windows.h>
#include <sys/stat.h>

#include "global.h"
#include "RageLog.h"
#include "NetworkFileManager.h"

// namespace fs = boost::filesystem;

unsigned long GetFileSizeInKB(FILE *file)
{
	unsigned long currentPosition = ftell(file); // Save the current file pointer position
	unsigned long fileSize = 0;

	fseek(file, 0L, SEEK_END);				// Move the file pointer to the end of the file
	fileSize = ftell(file);					// Get file size
	fseek(file, currentPosition, SEEK_SET); // Restore file pointer position

	return fileSize / 1024; // Return file size (KB)
}

bool MoveFolder(const std::string &sourcePath, const std::string &destinationPath)
{
	bool result = false;
	// Check if the source path exists
	if (!boost::filesystem::exists(sourcePath))
	{
		std::cerr << "Source path does not exist." << std::endl;
		result = false;
	}
	// else
	// {
	// 	// Move folders using the rename function of boost::filesystem
	// 	fs::rename(sourcePath, destinationPath);
	// 	std::cout << "Folder moved successfully." << std::endl;
	// 	result = true;
	// }
	return result;
}

bool PathExists(const std::string &path)
{
	struct stat buffer;
	return (stat(path.c_str(), &buffer) == 0);
}

std::string GetSongDirPath(std::string &songDir, std::string &additionalSongFolders, std::string &currentPath)
{
	std::string path = currentPath + "\\" + songDir;
	if (!PathExists(path))
	{
		path = additionalSongFolders + "\\" + songDir;
		if(!PathExists(path))
			path = "";
	}
	return path;
}

// std::string
void Transliterate(icu::UnicodeString source)
{
	// Create a Transliterator object
	UErrorCode status = U_ZERO_ERROR;
	icu::Transliterator *trans = icu::Transliterator::createInstance("Any-Latin; Latin-ASCII", UTRANS_FORWARD, status);

	if (U_SUCCESS(status))
	{
		// Translate
		trans->transliterate(source);

		// Output results
		// std::cout << source << std::endl;
		std::string utf8String;
		source.toUTF8String(utf8String);

		// Use UTF-8 string output
		std::cout << utf8String << std::endl;
		// Release resources
		delete trans;
	}
	else
	{
		std::cerr << "Transliterator creation failed with status: " << u_errorName(status) << std::endl;
	}

	//return 0;
}

bool CompressSongDir(std::string &songDir, std::string &additionalSongFolders, bool videoFilter)
{
	const int maxBufferSize = 512;
	// 1. Create a connection folder
	// 2. Put the song folder into the temporary folder
	// 3. If the song information or folder is not in English, rename it
	// 4. Filter unwanted files
	// 5. Put it into a zip file

	std::string currentPath;
	char buf[maxBufferSize];
	getcwd(buf, sizeof(buf));
	currentPath.assign(buf);

	// 1. Create a connection folder
	std::string connectDirPath = currentPath + "\\Songs\\connect\\temp";
	CreateDirectory(connectDirPath.c_str(), NULL);

	// 2. Put the song folder into the temporary folder
	std::string songDirPath = GetSongDirPath(songDir, additionalSongFolders, currentPath);
	// MoveFolder(songDirPath, connectDirPath);
	return true;
}
