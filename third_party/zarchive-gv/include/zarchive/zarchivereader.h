// Xenia for Game Vault: ZArchive's reader (github.com/exzap/ZArchive, MIT No Attribution, LICENSE here) with
// one change: the archive is read through a function, which Xenia points at its own file access (Game Vault's
// shared file, a game on a network share, a USB drive). Changes Copyright (c) 2026 Skitzelles.
#pragma once

#include <cstdint>
#include <vector>
#include <string_view>
#include <unordered_map>
#include <mutex>

#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>

#include "../../../zarchive/include/zarchive/zarchivecommon.h"

using ZArchiveNodeHandle = uint32_t;

static inline ZArchiveNodeHandle ZARCHIVE_INVALID_NODE = 0xFFFFFFFF;

class ZArchiveReader
{
public:
	struct DirEntry
	{
		std::string_view name;
		bool isFile;
		bool isDirectory;
		uint64_t size; // only valid for directories
	};

	// reads the whole range at the offset or returns false
	using ReadFn = std::function<bool(uint64_t offset, void* buffer, uint32_t size)>;
	// Xenia for Game Vault: opens path for reading (read function and file size); false = open it with std::ifstream
	static inline std::function<bool(const std::filesystem::path& path, ReadFn& read, uint64_t& size)> openHook;

	static ZArchiveReader* OpenFromFile(const std::filesystem::path& path);
	static ZArchiveReader* OpenFromReader(ReadFn read, uint64_t fileSize);

	~ZArchiveReader();

	ZArchiveNodeHandle LookUp(std::string_view path, bool allowFile = true, bool allowDirectory = true);
	bool IsDirectory(ZArchiveNodeHandle nodeHandle) const;
	bool IsFile(ZArchiveNodeHandle nodeHandle) const;

	// directory operations
	uint32_t GetDirEntryCount(ZArchiveNodeHandle nodeHandle) const;
	bool GetDirEntry(ZArchiveNodeHandle nodeHandle, uint32_t index, DirEntry& dirEntry) const;

	// file operations
	uint64_t GetFileSize(ZArchiveNodeHandle nodeHandle);
	uint64_t ReadFromFile(ZArchiveNodeHandle nodeHandle, uint64_t offset, uint64_t length, void* buffer);

private:
	struct CacheBlock
	{
		uint8_t* data;
		uint64_t blockIndex;
		// linked-list for LRU
		CacheBlock* prev;
		CacheBlock* next;
	};

	std::mutex m_accessMutex;

	std::vector<uint8_t> m_cacheDataBuffer;
	std::vector<CacheBlock> m_cacheBlocks;
	CacheBlock* m_lruChainFirst;
	CacheBlock* m_lruChainLast;
	std::unordered_map<uint64_t, CacheBlock*> m_blockLookup;

	ZArchiveReader(ReadFn&& read, std::vector<_ZARCHIVE::CompressionOffsetRecord>&& offsetRecords, std::vector<uint8_t>&& nameTable, std::vector<_ZARCHIVE::FileDirectoryEntry>&& fileTree, uint64_t compressedDataOffset, uint64_t compressedDataSize);

	CacheBlock* GetCachedBlock(uint64_t blockIndex);
	CacheBlock* RecycleLRUBlock(uint64_t newBlockIndex);
	void MarkBlockAsMRU(CacheBlock* block);

	void RegisterBlock(CacheBlock* block, uint64_t blockIndex);
	void UnregisterBlock(CacheBlock* block);
	bool LoadBlock(CacheBlock* block);

	static std::string_view GetName(const std::vector<uint8_t>& nameTable, uint32_t nameOffset);

	ReadFn m_read;
	std::vector<_ZARCHIVE::CompressionOffsetRecord> m_offsetRecords;
	std::vector<uint8_t> m_nameTable;
	std::vector<_ZARCHIVE::FileDirectoryEntry> m_fileTree;
	uint64_t m_compressedDataOffset;
	uint64_t m_compressedDataSize;
	uint64_t m_blockCount;

	std::vector<uint8_t> m_blockDecompressionBuffer;
};