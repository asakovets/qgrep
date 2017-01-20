#include "common.hpp"
#include "watch.hpp"

#include "project.hpp"
#include "fileutil.hpp"
#include "filestream.hpp"
#include "output.hpp"
#include "format.hpp"
#include "compression.hpp"
#include "constants.hpp"
#include "update.hpp"

#include <set>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <chrono>

#include <string.h>

struct WatchContext
{
	Output* output;

	std::thread updateThread;
	std::vector<std::thread> watchingThreads;

	std::set<std::string> changedFiles;
	std::string changedFilesLast;
	std::mutex changedFilesMutex;
	std::condition_variable changedFilesChanged;

	~WatchContext()
	{
		for (auto& t: watchingThreads)
			t.join();

		updateThread.join();
	}
};

static void fileChanged(WatchContext* context, ProjectGroup* group, const char* path, const char* file)
{
	if (isFileAcceptable(group, file))
	{
		std::string npath = normalizePath(path, file);

		std::unique_lock<std::mutex> lock(context->changedFilesMutex);

		context->changedFiles.insert(npath);
		context->changedFilesLast = npath;

		context->changedFilesChanged.notify_one();
	}
}

static void startWatchingRec(WatchContext* context, ProjectGroup* group)
{
	for (auto& path : group->paths)
	{
		context->output->print("Watching folder %s...\n", path.c_str());

		context->watchingThreads.emplace_back([=]
		{
			if (!watchDirectory(path.c_str(), [=](const char* file) { fileChanged(context, group, path.c_str(), file); }))
				context->output->error("Error watching folder %s\n", path.c_str());

			context->output->print("No longer watching folder %s\n", path.c_str());
		});
	}

	for (auto& child: group->groups)
		startWatchingRec(context, child.get());
}

static void processChunk(std::vector<FileInfo>& result, const char* data, size_t fileCount)
{
	const DataChunkFileHeader* files = reinterpret_cast<const DataChunkFileHeader*>(data);

	for (unsigned int i = 0; i < fileCount; ++i)
	{
		const DataChunkFileHeader& file = files[i];

		if (file.startLine == 0)
			result.push_back({ std::string(data + file.nameOffset, file.nameLength), file.timeStamp, file.fileSize });
	}
}

static bool getDataFileList(Output* output, const char* path, std::vector<FileInfo>& result)
{
	FileStream in(path, "rb");
	if (!in)
	{
		output->error("Error reading data file %s\n", path);
		return false;
	}

	DataFileHeader header;
	if (!read(in, header) || memcmp(header.magic, kDataFileHeaderMagic, strlen(kDataFileHeaderMagic)) != 0)
	{
		output->error("Error reading data file %s: file format is out of date, update the project to fix\n", path);
		return false;
	}

	DataChunkHeader chunk;

	while (read(in, chunk))
	{
		in.skip(chunk.extraSize);
		in.skip(chunk.indexSize);

		std::unique_ptr<char[]> data(new (std::nothrow) char[chunk.compressedSize + chunk.uncompressedSize]);

		if (!data || !read(in, data.get(), chunk.compressedSize))
		{
			output->error("Error reading data file %s: malformed chunk\n", path);
			return false;
		}

		char* uncompressed = data.get() + chunk.compressedSize;
		decompressPartial(uncompressed, chunk.uncompressedSize, data.get(), chunk.compressedSize, chunk.fileTableSize);
		processChunk(result, uncompressed, chunk.fileCount);
	}

	return true;
}

static std::vector<std::string> getChanges(const std::vector<FileInfo>& files, const std::vector<FileInfo>& packFiles)
{
	std::vector<std::string> result;

	size_t fileIt = 0;

	for (const FileInfo& pf: packFiles)
	{
		while (fileIt < files.size() && files[fileIt].path < pf.path)
		{
			result.push_back(files[fileIt].path);
			fileIt++;
		}

		if (files[fileIt].path == pf.path)
		{
			if (files[fileIt].timeStamp != pf.timeStamp || files[fileIt].fileSize != pf.fileSize)
				result.push_back(files[fileIt].path);

			fileIt++;
		}
	}

	while (fileIt < files.size())
	{
		result.push_back(files[fileIt].path);
		fileIt++;
	}

	return result;
}

static bool writeChanges(const char* path, const std::vector<std::string>& files)
{
	std::string targetPath = replaceExtension(path, ".qgc");

	if (files.empty())
		return removeFile(targetPath.c_str());

	std::string tempPath = targetPath + "_";

	{
		FileStream out(tempPath.c_str(), "wb");
		if (!out)
			return false;

		for (auto& f: files)
		{
			out.write(f.data(), f.size());
			out.write("\n", 1);
		}
	}

	return renameFile(tempPath.c_str(), targetPath.c_str());
}

static void printStatistics(Output* output, size_t fileCount, std::string last)
{
	if (last.size() > 40)
		last.replace(0, last.size() - 37, "...");

	output->print("%d files changed; last: %-40s\r", int(fileCount), last.c_str());
}

void watchProject(Output* output, const char* path)
{
	WatchContext context = { output };

    output->print("Watching %s:\n", path);

	std::unique_ptr<ProjectGroup> group = parseProject(output, path);
	if (!group)
		return;

	startWatchingRec(&context, group.get());

	output->print("Scanning project...\r");

	std::vector<FileInfo> files = getProjectGroupFiles(output, group.get());

	output->print("Reading data pack...\r");

	std::vector<FileInfo> packFiles;
	if (!getDataFileList(output, replaceExtension(path, ".qgd").c_str(), packFiles))
		return;

	removeFile(replaceExtension(path, ".qgc").c_str());

	std::vector<std::string> changedFiles = getChanges(files, packFiles);

	{
		std::unique_lock<std::mutex> lock(context.changedFilesMutex);

		context.changedFiles.insert(changedFiles.begin(), changedFiles.end());

		if (!changedFiles.empty())
			context.changedFilesLast = changedFiles.back();
	}

	if (changedFiles.size())
		output->print("%d files changed; listening for further changes\n", int(changedFiles.size()));
	else
		output->print("Listening for changes\n");

	// initial sync
	writeChanges(path, changedFiles);

	std::string changedFilesLast;

	for (;;)
	{
		bool updateNeeded = changedFiles.size() > size_t(kWatchUpdateThresholdFiles);
		bool updateNow = false;

		{
			std::unique_lock<std::mutex> lock(context.changedFilesMutex);

			if (updateNeeded)
			{
				if (context.changedFilesChanged.wait_for(lock, std::chrono::seconds(kWatchUpdateTimeout)) == std::cv_status::timeout)
				{
					updateNow = true;

					context.changedFiles.clear();
				}
			}
			else
			{
				context.changedFilesChanged.wait(lock, [&] { return context.changedFiles.size() != changedFiles.size(); });
			}

			changedFiles.assign(context.changedFiles.begin(), context.changedFiles.end());
			changedFilesLast = context.changedFilesLast;
		}

		if (updateNow)
		{
			// this removes the current changes file and updates the pack
			updateProject(output, path);
		}
		else
		{
			printStatistics(output, changedFiles.size(), changedFilesLast);

			if (!writeChanges(path, changedFiles))
			{
				output->error("Error saving changes to %s\n", replaceExtension(path, ".qgc").c_str());
				continue;
			}
		}
	}
}